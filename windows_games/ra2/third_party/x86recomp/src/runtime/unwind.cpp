#include "runtime/unwind.h"

#include <algorithm>

namespace recomp::unwind {
namespace {

// UNWIND_CODE operations.
enum : uint8_t {
  kPushNonvol = 0,
  kAllocLarge = 1,
  kAllocSmall = 2,
  kSetFpreg = 3,
  kSaveNonvol = 4,
  kSaveNonvolFar = 5,
  kEpilog = 6,  // version 2 (version 1: SAVE_XMM, unused)
  kSpare = 7,
  kSaveXmm128 = 8,
  kSaveXmm128Far = 9,
  kPushMachframe = 10,
};

constexpr int kRsp = 4;

// Slots (2-byte units) an unwind code takes.
int Slots(uint8_t op, uint8_t info) {
  switch (op) {
    case kAllocLarge: return info ? 3 : 2;
    case kSaveNonvol: case kSaveXmm128: return 2;
    case kSaveNonvolFar: case kSaveXmm128Far: return 3;
    default: return 1;
  }
}

uint8_t Rd8(const GuestMemory& m, gaddr a) { return m.read<uint8_t>(a); }
uint16_t Rd16(const GuestMemory& m, gaddr a) { return m.read<uint16_t>(a); }
uint32_t Rd32(const GuestMemory& m, gaddr a) { return m.read<uint32_t>(a); }
uint64_t Rd64(const GuestMemory& m, gaddr a) { return m.read<uint64_t>(a); }

// Is r.rip in an epilog? x64 epilogs have one form: an optional
// `add rsp, imm` (or `lea rsp, [frame reg + disp]` with a frame register),
// pops of non-volatile registers, then `ret` or a tail `jmp`. If so, runs
// what remains of it on r and returns true.
bool UnwindEpilog(const GuestMemory& m, const Info& info, Regs& r) {
  gaddr p = r.rip;
  uint64_t rsp = r.gpr[kRsp];
  auto b = [&](int i) { return Rd8(m, p + i); };
  // add rsp, imm8 / imm32
  if ((b(0) & 0xF8) == 0x48 && b(1) == 0x83 && b(2) == 0xC4) {
    rsp += int8_t(b(3));
    p += 4;
  } else if ((b(0) & 0xF8) == 0x48 && b(1) == 0x81 && b(2) == 0xC4) {
    rsp += int32_t(Rd32(m, p + 3));
    p += 7;
  } else if (info.frame_reg && (b(0) & 0xF8) == 0x48 && b(1) == 0x8D &&
             ((b(2) >> 3) & 7) == kRsp && (b(2) & 7) == (info.frame_reg & 7) &&
             (b(0) & 1) == (info.frame_reg >> 3) && (b(2) >> 6) != 0 && (b(2) >> 6) != 3) {
    // lea rsp, [frame + disp8/disp32] (no SIB: the frame register is not rsp)
    int mod = b(2) >> 6;
    int32_t disp = mod == 1 ? int8_t(b(3)) : int32_t(Rd32(m, p + 3));
    rsp = r.gpr[info.frame_reg] + int64_t(disp);
    p += mod == 1 ? 4 : 7;
  }
  // pops, then the return or tail jump; check first, then apply.
  gaddr q = p;
  int pops = 0;
  for (;;) {
    uint8_t x = Rd8(m, q);
    if ((x & 0xF8) == 0x58) {
      q += 1;
    } else if ((x & 0xFE) == 0x40 && (Rd8(m, q + 1) & 0xF8) == 0x58) {
      q += 2;
    } else {
      break;
    }
    pops++;
  }
  uint8_t x = Rd8(m, q), y = Rd8(m, q + 1);
  bool end = x == 0xC3 || x == 0xC2 || x == 0xE9 || (x == 0xFF && y == 0x25) ||
             (x == 0x48 && y == 0xFF && Rd8(m, q + 2) == 0x25);
  if (!end) return false;
  // A jmp rel32 ends an epilog only as a tail call after the frame is gone:
  // with no stack adjustment and no pops before it, it is a body's jump.
  if (x == 0xE9 && p == r.rip && !pops) return false;
  for (gaddr s = p; s < q;) {
    uint8_t v = Rd8(m, s);
    int reg;
    if ((v & 0xFE) == 0x40) {
      reg = 8 + (Rd8(m, s + 1) & 7);
      s += 2;
    } else {
      reg = v & 7;
      s += 1;
    }
    r.gpr[reg] = Rd64(m, rsp);
    rsp += 8;
  }
  r.rip = Rd64(m, rsp);
  r.gpr[kRsp] = rsp + 8;
  return true;
}

}  // namespace

Info Read(const GuestMemory& m, gaddr image_base, gaddr unwind_va) {
  Info i;
  uint8_t b0 = Rd8(m, unwind_va), b3 = Rd8(m, unwind_va + 3);
  i.version = b0 & 7;
  i.flags = b0 >> 3;
  i.prolog = Rd8(m, unwind_va + 1);
  i.count = Rd8(m, unwind_va + 2);
  i.frame_reg = b3 & 15;
  i.frame_off = b3 >> 4;
  i.codes = unwind_va + 4;
  gaddr after = i.codes + 2 * ((i.count + 1u) & ~1u);
  if (i.flags & kChainInfo) {
    i.chained = after;
  } else if (i.flags & (kEHandler | kUHandler)) {
    i.handler = image_base + Rd32(m, after);
    i.handler_data = after + 4;
  }
  return i;
}

gaddr Lookup(const PeImage& img, gaddr pc) {
  const auto& f = img.functions;
  auto it = std::upper_bound(f.begin(), f.end(), pc,
                             [](gaddr v, const PeFunction& e) { return v < e.begin; });
  if (it == f.begin()) return 0;
  --it;
  return pc < it->end ? it->entry : 0;
}

gaddr VirtualUnwind(const GuestMemory& m, gaddr image_base, gaddr fn_va, Regs& r,
                    uint8_t types, gaddr* handler, gaddr* data) {
  *handler = *data = 0;
  // A RUNTIME_FUNCTION whose unwind RVA has bit 0 set is the short form of
  // a chain: it stands for the entry it points at.
  uint32_t uw = Rd32(m, fn_va + 8);
  while (uw & 1) {
    fn_va = image_base + (uw & ~1u);
    uw = Rd32(m, fn_va + 8);
  }
  Info info = Read(m, image_base, image_base + uw);
  const uint64_t offset = r.rip - (image_base + Rd32(m, fn_va));

  // The establisher frame: rsp, or the frame register once the prologue
  // has set it (always for a chained fragment).
  uint64_t frame = r.gpr[kRsp];
  if (info.frame_reg) {
    bool set = offset >= info.prolog || (info.flags & kChainInfo);
    for (int i = 0; !set && i < info.count;) {
      uint8_t op = Rd8(m, info.codes + 2 * i + 1) & 15, oi = Rd8(m, info.codes + 2 * i + 1) >> 4;
      if (op == kSetFpreg && offset >= Rd8(m, info.codes + 2 * i)) set = true;
      i += Slots(op, oi);
    }
    if (set) frame = r.gpr[info.frame_reg] - 16ull * info.frame_off;
  }

  if (offset >= info.prolog && UnwindEpilog(m, info, r)) return frame;

  const bool in_prolog = offset < info.prolog;
  bool machframe = false;
  uint64_t off = offset;  // chained parents: everything has run
  for (Info cur = info;;) {
    for (int i = 0; i < cur.count;) {
      gaddr code = cur.codes + 2 * i;
      uint8_t at = Rd8(m, code), op = Rd8(m, code + 1) & 15, oi = Rd8(m, code + 1) >> 4;
      int slots = Slots(op, oi);
      if (cur.version >= 2 && op == kEpilog) {
        i += 1;
        continue;
      }
      if (off < at) {  // not executed yet
        i += slots;
        continue;
      }
      uint64_t& rsp = r.gpr[kRsp];
      switch (op) {
        case kPushNonvol:
          r.gpr[oi] = Rd64(m, rsp);
          rsp += 8;
          break;
        case kAllocLarge:
          rsp += oi ? Rd32(m, code + 2) : uint64_t(Rd16(m, code + 2)) * 8;
          break;
        case kAllocSmall:
          rsp += 8ull * oi + 8;
          break;
        case kSetFpreg:
          rsp = r.gpr[cur.frame_reg] - 16ull * cur.frame_off;
          break;
        case kSaveNonvol:
          r.gpr[oi] = Rd64(m, frame + 8ull * Rd16(m, code + 2));
          break;
        case kSaveNonvolFar:
          r.gpr[oi] = Rd64(m, frame + Rd32(m, code + 2));
          break;
        case kSaveXmm128:
          for (int k = 0; k < 16; k++) r.xmm[oi][k] = Rd8(m, frame + 16ull * Rd16(m, code + 2) + k);
          break;
        case kSaveXmm128Far:
          for (int k = 0; k < 16; k++) r.xmm[oi][k] = Rd8(m, frame + Rd32(m, code + 2) + k);
          break;
        case kPushMachframe:
          if (oi) rsp += 8;  // the error code
          r.rip = Rd64(m, rsp);
          rsp = Rd64(m, rsp + 24);
          machframe = true;
          break;
        default:
          break;
      }
      i += slots;
    }
    if (!(cur.flags & kChainInfo)) {
      // The handler is the primary (unchained) entry's.
      if (!in_prolog && (cur.flags & types)) {
        *handler = cur.handler;
        *data = cur.handler_data;
      }
      break;
    }
    gaddr parent = cur.chained;
    uint32_t puw = Rd32(m, parent + 8);
    while (puw & 1) {
      parent = image_base + (puw & ~1u);
      puw = Rd32(m, parent + 8);
    }
    cur = Read(m, image_base, image_base + puw);
    off = ~0ull;
  }
  if (!machframe) {
    r.rip = Rd64(m, r.gpr[kRsp]);
    r.gpr[kRsp] += 8;
  }
  return frame;
}

}  // namespace recomp::unwind
