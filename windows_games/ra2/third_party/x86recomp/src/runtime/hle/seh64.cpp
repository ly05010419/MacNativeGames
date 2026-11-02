// x64 structured exception handling (see seh64.h): RtlDispatchException's
// first pass, RtlUnwindEx's second, and the Rtl* unwind functions games
// import, on Windows' structures in guest memory.

#include "runtime/hle/seh64.h"

#include <cstring>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/lifted.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle::seh64 {
namespace {

constexpr int kRsp = 4;
constexpr uint32_t kRecordSize = 0x98, kDcSize = 0x50;

thread_local std::vector<Dispatching> t_dispatching;

// Guest scratch memory below the current stack pointer for the structures a
// handler call needs; c.rsp moves below it for the call and back after.
struct Scratch {
  Ctx& c;
  uint64_t saved;
  gaddr base;
  Scratch(Ctx& cc, uint64_t size) : c(cc), saved(cc.rsp) {
    base = (c.rsp - size - 0x100) & ~15ull;
    if (base < CurrentThread()->stack_lo + 0x10000)
      FatalF("guest stack exhausted dispatching an exception (rsp %08llX)",
             (unsigned long long)c.rsp);
    c.rsp = base - 0x40;
  }
  ~Scratch() { c.rsp = saved; }
};

bool OnStack(uint64_t a) {
  const Thread* t = CurrentThread();
  return a >= t->stack_lo && a < t->stack_hi;
}

// The function covering r.rip, or false at the end of what can be walked
// (a host boundary, a broken stack). Leaf code (no .pdata entry) returns
// to [rsp] at once.
bool FunctionAt(unwind::Regs& r, const PeImage** img, gaddr* fn) {
  for (int guard = 0; guard < 64; guard++) {
    *img = ImageAt(r.rip);
    if (!*img || !OnStack(r.gpr[kRsp])) return false;
    *fn = unwind::Lookup(**img, r.rip);
    if (*fn) return true;
    r.rip = Rd64(r.gpr[kRsp]);
    r.gpr[kRsp] += 8;
  }
  return false;
}

void WriteDc(gaddr dc, gaddr pc, const PeImage& img, gaddr fn, gaddr frame, gaddr target_ip,
             gaddr work, gaddr handler, gaddr data) {
  std::memset(GPtr(dc), 0, kDcSize);
  Wr64(dc + kDcControlPc, pc);
  Wr64(dc + kDcImageBase, img.image_base);
  Wr64(dc + kDcFunctionEntry, fn);
  Wr64(dc + kDcEstablisherFrame, frame);
  Wr64(dc + kDcTargetIp, target_ip);
  Wr64(dc + kDcContextRecord, work);
  Wr64(dc + kDcLanguageHandler, handler);
  Wr64(dc + kDcHandlerData, data);
}

}  // namespace

unwind::Regs AtReturn(const Ctx& c) {
  unwind::Regs r;
  const uint64_t* regs[16] = {&c.rax, &c.rcx, &c.rdx, &c.rbx, &c.rsp, &c.rbp, &c.rsi, &c.rdi,
                              &c.r8,  &c.r9,  &c.r10, &c.r11, &c.r12, &c.r13, &c.r14, &c.r15};
  for (int i = 0; i < 16; i++) r.gpr[i] = *regs[i];
  r.rip = Rd64(c.rsp);
  r.gpr[kRsp] = c.rsp + 8;
  for (int i = 0; i < 16; i++) std::memcpy(r.xmm[i], c.xmm[i].b, 16);
  return r;
}

void WriteContext(gaddr ctx, const unwind::Regs& r, const Ctx& c) {
  std::memset(GPtr(ctx), 0, kContextSize);
  Wr32(ctx + 0x30, 0x10001F);  // CONTEXT_AMD64 | CONTROL | INTEGER | SEGMENTS | FLOATING_POINT
  Wr32(ctx + kCtxMxcsr, c.mxcsr);
  Wr16(ctx + 0x38, 0x33);  // SegCs
  Wr16(ctx + 0x3A, 0x2B);  // SegDs
  Wr16(ctx + 0x3C, 0x2B);  // SegEs
  Wr16(ctx + 0x3E, 0x53);  // SegFs
  Wr16(ctx + 0x40, 0x2B);  // SegGs
  Wr16(ctx + 0x42, 0x2B);  // SegSs
  Wr32(ctx + 0x44, c.eflags);
  for (int i = 0; i < 16; i++) Wr64(ctx + kCtxRax + 8 * i, r.gpr[i]);
  Wr64(ctx + kCtxRip, r.rip);
  Wr16(ctx + 0x100, 0x027F);       // FltSave.ControlWord
  Wr32(ctx + 0x100 + 0x18, c.mxcsr);  // FltSave.MxCsr
  for (int i = 0; i < 16; i++) std::memcpy(GPtr(ctx + kCtxXmm0 + 16 * i), r.xmm[i], 16);
}

unwind::Regs ReadContext(gaddr ctx) {
  unwind::Regs r;
  for (int i = 0; i < 16; i++) r.gpr[i] = Rd64(ctx + kCtxRax + 8 * i);
  r.rip = Rd64(ctx + kCtxRip);
  for (int i = 0; i < 16; i++) std::memcpy(r.xmm[i], GPtr(ctx + kCtxXmm0 + 16 * i), 16);
  return r;
}

const Dispatching* Current() { return t_dispatching.empty() ? nullptr : &t_dispatching.back(); }

void Raise(Ctx& c, uint32_t code, uint32_t flags, const uint64_t* params, uint32_t n,
           const unwind::Regs& at) {
  Scratch s(c, 0x1000);
  const gaddr rec = s.base, ctx = s.base + 0x100, work = s.base + 0x600, dc = s.base + 0xB00;
  std::memset(GPtr(rec), 0, kRecordSize);
  Wr32(rec + 0x00, code);
  Wr32(rec + 0x04, flags & kNoncontinuable);
  Wr64(rec + 0x10, at.rip);
  n = std::min<uint32_t>(n, 15);
  Wr32(rec + 0x18, n);
  for (uint32_t i = 0; i < n; i++) Wr64(rec + 0x20 + 8 * i, params[i]);
  WriteContext(ctx, at, c);
  RECOMP_TRACE("  x64 exception %08X at %08llX, dispatching\n", code,
               (unsigned long long)at.rip);

  t_dispatching.push_back({rec, ctx, at});
  struct Pop {
    ~Pop() { t_dispatching.pop_back(); }
  } pop;
  unwind::Regs r = at;
  const PeImage* img;
  gaddr fn;
  while (FunctionAt(r, &img, &fn)) {
    const gaddr pc = r.rip;
    gaddr h, data;
    gaddr frame = unwind::VirtualUnwind(g_proc->mem, img->image_base, fn, r, unwind::kEHandler,
                                        &h, &data);
    if (!h) continue;
    WriteContext(work, r, c);
    WriteDc(dc, pc, *img, fn, frame, 0, work, h, data);
    uint32_t d = uint32_t(CallGuest(c, h, {rec, frame, ctx, dc}));
    if (d == kContinueExecution) {
      if (flags & kNoncontinuable)
        FatalF("a handler continued the noncontinuable exception %08X", code);
      unwind::Regs back = ReadContext(ctx);
      if (back.rip != at.rip || back.gpr[kRsp] != at.gpr[kRsp])
        FatalF("a handler continued exception %08X elsewhere (rip %08llX): not supported",
               code, (unsigned long long)back.rip);
      uint64_t* regs[16] = {&c.rax, &c.rcx, &c.rdx, &c.rbx, &c.rsp, &c.rbp, &c.rsi, &c.rdi,
                            &c.r8,  &c.r9,  &c.r10, &c.r11, &c.r12, &c.r13, &c.r14, &c.r15};
      for (int i = 0; i < 16; i++)
        if (i != kRsp) *regs[i] = back.gpr[i];
      return;
    }
    if (d != kContinueSearch)
      FatalF("x64 exception %08X: handler %08llX returned disposition %u", code,
             (unsigned long long)h, d);
  }
  FatalF("unhandled guest exception %08X raised at %08llX (params %u: %llX %llX)", code,
         (unsigned long long)at.rip, n, n > 0 ? (unsigned long long)params[0] : 0ull,
         n > 1 ? (unsigned long long)params[1] : 0ull);
}

unwind::Regs UnwindBelow(Ctx& c, gaddr record, gaddr target_frame) {
  const Dispatching* d = Current();
  if (!d) FatalF("unwind with no exception being dispatched");
  Wr32(record + 0x04, Rd32(record + 0x04) | kUnwinding);
  Scratch s(c, 0x800);
  const gaddr work = s.base, dc = s.base + 0x500;
  unwind::Regs r = d->at;
  const PeImage* img;
  gaddr fn;
  while (FunctionAt(r, &img, &fn)) {
    const unwind::Regs inside = r;
    gaddr h, data;
    gaddr frame = unwind::VirtualUnwind(g_proc->mem, img->image_base, fn, r, unwind::kUHandler,
                                        &h, &data);
    if (frame == target_frame) return inside;
    if (frame > target_frame) break;
    if (h) {
      WriteContext(work, r, c);
      WriteDc(dc, inside.rip, *img, fn, frame, 0, work, h, data);
      CallGuest(c, h, {record, frame, d->context, dc});
    }
  }
  FatalF("unwind: target frame %08llX not found on the stack", (unsigned long long)target_frame);
}

void ContinueAt(Ctx& c, const unwind::Regs& s, gaddr target_frame, gaddr target_ip,
                uint64_t value) {
  uint64_t* regs[16] = {&c.rax, &c.rcx, &c.rdx, &c.rbx, &c.rsp, &c.rbp, &c.rsi, &c.rdi,
                        &c.r8,  &c.r9,  &c.r10, &c.r11, &c.r12, &c.r13, &c.r14, &c.r15};
  for (int i = 0; i < 16; i++) *regs[i] = s.gpr[i];
  for (int i = 0; i < 16; i++) std::memcpy(c.xmm[i].b, s.xmm[i], 16);
  c.rax = value;
  RECOMP_TRACE("  x64 unwind: continuing at %08llX (frame %08llX)\n",
               (unsigned long long)target_ip, (unsigned long long)target_frame);
  throw GuestResume{target_ip, target_frame, s.rip};
}

void UnwindTo(Ctx& c, gaddr record, gaddr target_frame, gaddr target_ip, uint64_t value) {
  unwind::Regs inside = UnwindBelow(c, record, target_frame);
  // The target frame's own unwind handler (its __finally blocks between the
  // exception and the target).
  unwind::Regs r = inside;
  const PeImage* img;
  gaddr fn;
  if (FunctionAt(r, &img, &fn)) {
    gaddr h, data;
    gaddr frame = unwind::VirtualUnwind(g_proc->mem, img->image_base, fn, r, unwind::kUHandler,
                                        &h, &data);
    if (h) {
      Scratch s(c, 0x800);
      const gaddr work = s.base, dc = s.base + 0x500;
      Wr32(record + 0x04, Rd32(record + 0x04) | kTargetUnwind);
      WriteContext(work, r, c);
      WriteDc(dc, inside.rip, *img, fn, frame, target_ip, work, h, data);
      CallGuest(c, h, {record, frame, Current()->context, dc});
    }
  }
  ContinueAt(c, inside, target_frame, target_ip, value);
}

// ---- the Rtl* unwind functions ------------------------------------------------

// RtlCaptureContext(ctx): the caller's state at the call.
HLE64("kernel32.dll", RtlCaptureContext, 1) {
  WriteContext(Arg(c, 0), AtReturn(c), c);
  return 0;
}

// RtlLookupFunctionEntry(pc, &image_base, history)
HLE64("kernel32.dll", RtlLookupFunctionEntry, 3) {
  const PeImage* img = ImageAt(Arg(c, 0));
  gaddr fn = img ? unwind::Lookup(*img, Arg(c, 0)) : 0;
  if (fn && Arg(c, 1)) Wr64(Arg(c, 1), img->image_base);
  return fn;
}

// RtlVirtualUnwind(type, image_base, pc, fn, ctx, &handler_data,
// &establisher, context_pointers): one frame on the guest CONTEXT.
HLE64("kernel32.dll", RtlVirtualUnwind, 8) {
  uint8_t type = uint8_t(Arg(c, 0));
  gaddr image = Arg(c, 1), fn = Arg(c, 3), ctx = Arg(c, 4);
  unwind::Regs r = ReadContext(ctx);
  r.rip = Arg(c, 2);
  gaddr h, data;
  gaddr frame = unwind::VirtualUnwind(g_proc->mem, image, fn, r, type, &h, &data);
  for (int i = 0; i < 16; i++) Wr64(ctx + kCtxRax + 8 * i, r.gpr[i]);
  Wr64(ctx + kCtxRip, r.rip);
  for (int i = 0; i < 16; i++) std::memcpy(GPtr(ctx + kCtxXmm0 + 16 * i), r.xmm[i], 16);
  if (Arg(c, 5)) Wr64(Arg(c, 5), data);
  if (Arg(c, 6)) Wr64(Arg(c, 6), frame);
  return h;
}

}  // namespace recomp::hle::seh64
