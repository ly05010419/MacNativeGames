// kernel32: structured exception handling (RaiseException, RtlUnwind,
// UnhandledExceptionFilter).
//
// The SEH chain lives in guest memory exactly as on Windows (fs:[0] ->
// {next, handler} records on the guest stack). RaiseException builds an
// EXCEPTION_RECORD and CONTEXT in guest memory and calls each lifted
// handler in turn. When a handler (__except_handler3) decides to run an
// __except block it unwinds with RtlUnwind and then reaches a manifest
// `resume` site, which throws GuestResume to the owning lifted function.

#include <algorithm>
#include <cstring>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/seh64.h"
#include "runtime/lifted.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

constexpr uint32_t kExceptionNoncontinuable = 0x1;
constexpr uint32_t kExceptionUnwinding = 0x2;
constexpr uint32_t kExceptionExitUnwind = 0x4;
constexpr uint32_t kEnd = 0xFFFFFFFF;

// EXCEPTION_DISPOSITION
constexpr uint32_t kContinueExecution = 0, kContinueSearch = 1;

constexpr uint32_t kRecordSize = 0x50, kContextSize = 0x2CC;

void FillContext(uint32_t ctx, const Ctx& c, uint32_t eip, uint32_t esp) {
  std::memset(GPtr(ctx), 0, kContextSize);
  Wr32(ctx + 0x00, 0x0001003F);  // CONTEXT_FULL | FLOATING_POINT | DEBUG
  Wr32(ctx + 0x8C, 0);           // SegGs
  Wr32(ctx + 0x90, 0x3B);        // SegFs
  Wr32(ctx + 0x94, 0x23);
  Wr32(ctx + 0x98, 0x23);
  Wr32(ctx + 0x9C, c.edi);
  Wr32(ctx + 0xA0, c.esi);
  Wr32(ctx + 0xA4, c.ebx);
  Wr32(ctx + 0xA8, c.edx);
  Wr32(ctx + 0xAC, c.ecx);
  Wr32(ctx + 0xB0, c.eax);
  Wr32(ctx + 0xB4, c.ebp);
  Wr32(ctx + 0xB8, eip);
  Wr32(ctx + 0xBC, 0x1B);
  Wr32(ctx + 0xC0, c.eflags);
  Wr32(ctx + 0xC4, esp);
  Wr32(ctx + 0xC8, 0x23);
  // FloatSave (FNSAVE image) at +0x1C.
  x87::State copy = c.fpu;
  x87::Save(copy, GPtr(ctx + 0x1C));
}

// Walks the SEH chain for an exception. Returns true if a handler asked to
// continue execution, false if no handler took it. A handler that runs an
// __except block never returns here (GuestResume unwinds past us).
bool DispatchException(Ctx& c, uint32_t code, uint32_t flags, uint32_t nparams,
                       const uint32_t* params, uint32_t eip) {
  uint32_t teb = c.fs_base;
  uint32_t saved_esp = c.esp;
  // Scratch space for the records below the current stack.
  c.esp -= 0x800;
  uint32_t rec = c.esp + 0x10;
  uint32_t ctx = rec + kRecordSize;
  std::memset(GPtr(rec), 0, kRecordSize);
  Wr32(rec + 0x00, code);
  Wr32(rec + 0x04, flags);
  Wr32(rec + 0x0C, eip);
  nparams = std::min<uint32_t>(nparams, 15);
  Wr32(rec + 0x10, nparams);
  for (uint32_t i = 0; i < nparams; i++) Wr32(rec + 0x14 + 4 * i, params[i]);
  FillContext(ctx, c, eip, saved_esp + 4 + 16);

  RECOMP_TRACE("  exception %08X at %08X (params %u: %08X %08X), walking SEH chain\n",
            code, eip, nparams, nparams > 0 ? params[0] : 0,
            nparams > 1 ? params[1] : 0);
  for (uint32_t reg = Rd32(teb); reg != kEnd && reg; reg = Rd32(reg)) {
    if (reg < CurrentThread()->stack_lo || reg >= CurrentThread()->stack_hi)
      FatalF("corrupt SEH chain: record %08X outside the stack", reg);
    uint32_t handler = Rd32(reg + 4);
    uint32_t disp = CallGuest(c, handler, {rec, reg, ctx, 0});
    if (disp == kContinueExecution) {
      if (flags & kExceptionNoncontinuable)
        FatalF("handler %08X continued a noncontinuable exception %08X",
               handler, code);
      if (Rd32(ctx + 0xB8) != eip)
        FatalF("handler %08X changed EIP to %08X: not supported", handler,
               Rd32(ctx + 0xB8));
      // Resume after RaiseException with the (possibly edited) registers.
      c.edi = Rd32(ctx + 0x9C);
      c.esi = Rd32(ctx + 0xA0);
      c.ebx = Rd32(ctx + 0xA4);
      c.edx = Rd32(ctx + 0xA8);
      c.ecx = Rd32(ctx + 0xAC);
      c.ebp = Rd32(ctx + 0xB4);
      c.esp = saved_esp;
      return true;
    }
    if (disp != kContinueSearch)
      WarnF("SEH handler %08X returned disposition %u; continuing search\n",
            handler, disp);
  }
  c.esp = saved_esp;
  return false;
}

HLE64("kernel32.dll", RaiseException, 4) {
  if (g_proc->x64) {  // table-based (seh64.h)
    uint32_t code = uint32_t(Arg(c, 0)), flags = uint32_t(Arg(c, 1)), n = uint32_t(Arg(c, 2));
    gaddr args = Arg(c, 3);
    uint64_t params[15] = {};
    for (uint32_t i = 0; i < std::min<uint32_t>(n, 15); i++) params[i] = Rd64(args + 8 * i);
    seh64::Raise(c, code, flags, params, n, seh64::AtReturn(c));
    return c.rax;
  }
  uint32_t code = Arg(c, 0), flags = Arg(c, 1), n = Arg(c, 2), args = Arg(c, 3);
  uint32_t eip = Rd32(c.esp);  // return address: where Windows reports it
  uint32_t params[15] = {};
  for (uint32_t i = 0; i < std::min<uint32_t>(n, 15); i++) params[i] = Rd32(args + 4 * i);
  if (!DispatchException(c, code, flags & kExceptionNoncontinuable, n, params, eip))
    FatalF("unhandled guest exception %08X raised from %08X (params %u: %08X %08X)",
           code, eip, n, params[0], params[1]);
  return c.eax;
}

// RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue): calls the
// handlers of every frame above TargetFrame with the unwinding flag and
// pops them. The CRT always passes its own return address as TargetIp, so
// "continue at TargetIp" is a plain return here.
HLE("kernel32.dll", RtlUnwind, 4) {
  uint32_t target = Arg(c, 0), target_ip = Arg(c, 1), exc = Arg(c, 2),
           retval = Arg(c, 3);
  if (target_ip && target_ip != Rd32(c.esp))
    FatalF("RtlUnwind to %08X, not its return address %08X: unsupported",
           target_ip, Rd32(c.esp));
  uint32_t teb = c.fs_base;
  uint32_t saved_esp = c.esp;
  c.esp -= 0x800;
  uint32_t rec = exc;
  if (!rec) {
    rec = c.esp + 0x10;
    std::memset(GPtr(rec), 0, kRecordSize);
    Wr32(rec, 0xC0000027);  // STATUS_UNWIND
  }
  uint32_t ctx = c.esp + 0x10 + kRecordSize;
  FillContext(ctx, c, Rd32(saved_esp), saved_esp + 4 + 16);
  Wr32(rec + 4, Rd32(rec + 4) | kExceptionUnwinding |
                    (target ? 0 : kExceptionExitUnwind));
  for (uint32_t reg = Rd32(teb); reg != kEnd && reg && reg != target;
       reg = Rd32(teb)) {
    uint32_t handler = Rd32(reg + 4);
    CallGuest(c, handler, {rec, reg, ctx, 0});
    Wr32(teb, Rd32(reg));  // pop the frame
  }
  c.esp = saved_esp;
  return retval;
}

HLE("kernel32.dll", UnhandledExceptionFilter, 1) {
  uint32_t ptrs = Arg(c, 0);
  uint32_t rec = Rd32(ptrs);
  FatalF("UnhandledExceptionFilter: exception %08X at %08X", Rd32(rec),
         Rd32(rec + 0x0C));
}

// Host pages larger than the guest's 4 KiB (16 KiB on Apple Silicon) are
// accessible as a whole once any guest page in them is committed, so a
// game that demand-pages reserved memory from an exception filter (commit
// and fill one page per access violation) would never see the faults for
// the other guest pages sharing that host page: they read as zeros. After
// a handler has continued from a fault, the uncommitted pages of the same
// reservation in that host page are therefore faulted in as well (read
// faults at their first byte), as if the game had touched them next.
void CompleteHostPage(Ctx& c, uint32_t addr) {
  GuestMemory& mem = g_proc->mem;
  const uint32_t host = uint32_t(mem.host_page_size());
  if (host <= GuestMemory::kPageSize) return;
  GuestAllocator::Region region;
  if (!g_proc->vm.Find(addr, &region)) return;
  const uint32_t first = uint32_t(std::max<uint64_t>(addr & ~(host - 1), region.base));
  const uint64_t end = std::min<uint64_t>(uint64_t(addr & ~(host - 1)) + host,
                                          uint64_t(region.base) + region.size);
  for (uint64_t p = first; p < end; p += GuestMemory::kPageSize) {
    uint32_t va = uint32_t(p);
    if (mem.IsCommitted(va)) continue;
    uint32_t params[2] = {0, va};
    if (!DispatchException(c, 0xC0000005, 0, 2, params, 0) || !mem.IsCommitted(va)) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        WarnF("guest page %08X shares a host page with demand-paged %08X but was "
              "not paged in; it reads as zeros\n", va, addr);
      }
    }
  }
}

}  // namespace

// A host fault in guest memory, delivered as EXCEPTION_ACCESS_VIOLATION.
// Called from the SIGSEGV handler on the faulting thread (signal stack).
// Registers in `c` are those of the last spill (lifted code keeps them in
// locals), so handlers get an approximate CONTEXT; the game's handlers
// only use the exception record. The guest stack used for the handler
// calls starts well below the stale ESP so live frames are not clobbered.
bool RaiseAccessViolation(Ctx& c, bool write, uint32_t addr) {
  uint32_t saved_esp = c.esp;
  // The live area below the last spilled ESP is small: stack allocations
  // over 4 KiB go through _chkstk, which is a call (and spills). Nested
  // faults (a filter faulting) each step down by this gap again.
  constexpr uint32_t kGap = 0x8000;
  Thread* t = CurrentThread();
  if (c.esp < t->stack_lo + kGap + 0x4000)
    FatalF("guest stack exhausted delivering an access violation at %08X "
           "(esp %08X, stack %08X-%08X)", addr, c.esp, uint32_t(t->stack_lo), uint32_t(t->stack_hi));
  c.esp -= kGap;
  uint32_t params[2] = {write ? 1u : 0u, addr};
  bool cont = DispatchException(c, 0xC0000005, 0, 2, params, 0);
  if (cont) CompleteHostPage(c, addr);
  c.esp = saved_esp;
  return cont;
}

bool EnsureReadable(Ctx& c, gaddr va, uint64_t len) {
  if (!len) return true;
  GuestMemory& mem = g_proc->mem;
  gaddr end = va + len - 1;
  if (end < va) return false;
  for (gaddr p = va & ~0xFFFull;; p += 0x1000) {
    // A guest handler may commit the page and continue; anything else
    // (a second fault on the same page, no handler) is a failure.
    if (!mem.IsCommitted(p) &&
        (g_proc->x64 || !RaiseAccessViolation(c, false, uint32_t(std::max(p, va))) ||
         !mem.IsCommitted(p)))
      return false;
    if (p >= (end & ~0xFFFull)) break;
  }
  return true;
}

}  // namespace recomp::hle
