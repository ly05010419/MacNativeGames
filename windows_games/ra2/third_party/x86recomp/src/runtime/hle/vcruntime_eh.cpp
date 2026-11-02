// VCRUNTIME140's exception handling for x64 guests: _CxxThrowException,
// __CxxFrameHandler4 (the compressed C++ EH metadata of MSVC 2019 and
// later), __C_specific_handler (__try/__except/__finally scope tables) and
// __current_exception. Dispatch and unwinding are runtime/hle/seh64.h's.
// The metadata's format follows MSVC's as Wine's handler4.c documents it
// (reference only).
//
// A C++ catch: when the first pass reaches a frame whose try block catches
// the thrown type, the frames below it are unwound (their handlers run the
// destructors), the catching frame's own objects down to the try block's
// level are destroyed, the exception object is copied to the catch
// parameter, the catch funclet runs (with the frame in rdx) and returns the
// continuation address, the exception object is destroyed, and the frame
// continues there. Not yet supported: rethrowing (throw;) or throwing out
// of a catch block, and SEH translators (_set_se_translator).

#include <cstring>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/seh64.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

constexpr uint32_t kCxxException = 0xE06D7363;  // 'msc' | 0xE0000000
constexpr uint64_t kCxxMagic = 0x19930520;

// ---- reading the metadata -----------------------------------------------------

struct Reader {
  gaddr p;
  uint8_t Byte() { return Rd8(p++); }
  int32_t Rva() {
    int32_t v = int32_t(Rd32(p));
    p += 4;
    return v;
  }
  // MSVC's compressed unsigned: 1-4 bytes, the low bits of the first one
  // giving the length (0, 01, 011, 0111), or 1111 + 4 bytes.
  uint32_t U() {
    uint8_t b = Rd8(p);
    uint32_t v;
    if (!(b & 1)) {
      v = b >> 1;
      p += 1;
    } else if ((b & 3) == 1) {
      v = (b >> 2) + (uint32_t(Rd8(p + 1)) << 6);
      p += 2;
    } else if ((b & 7) == 3) {
      v = (b >> 3) + (uint32_t(Rd8(p + 1)) << 5) + (uint32_t(Rd8(p + 2)) << 13);
      p += 3;
    } else if ((b & 15) == 7) {
      v = (b >> 4) + (uint32_t(Rd8(p + 1)) << 4) + (uint32_t(Rd8(p + 2)) << 12) +
          (uint32_t(Rd8(p + 3)) << 20);
      p += 4;
    } else {
      v = Rd32(p + 1);
      p += 5;
    }
    return v;
  }
};

// FuncInfo4 header flags.
constexpr uint8_t kIsCatch = 0x01, kIsSeparated = 0x02, kBbt = 0x04, kUnwindMap = 0x08,
                  kTryBlockMap = 0x10, kEhs = 0x20, kNoExcept = 0x40;

struct FuncInfo4 {
  uint8_t header = 0;
  uint32_t unwind_count = 0, try_count = 0, ip_count = 0, frame = 0;
  gaddr unwind_map = 0, try_map = 0, ip_map = 0;  // past their counts
};

// The descriptor of the function the dispatcher context is in (its
// separated IP-to-state map picked by the segment's start).
bool ReadFuncInfo(gaddr image, gaddr data, gaddr seg_begin, FuncInfo4* fi) {
  Reader r{image + uint32_t(Rd32(data))};
  fi->header = r.Byte();
  if (fi->header & kBbt) r.U();
  if (fi->header & kUnwindMap) {
    Reader m{image + uint32_t(r.Rva())};
    fi->unwind_count = m.U();
    fi->unwind_map = m.p;
  }
  if (fi->header & kTryBlockMap) {
    Reader m{image + uint32_t(r.Rva())};
    fi->try_count = m.U();
    fi->try_map = m.p;
  }
  gaddr ip_map = image + uint32_t(r.Rva());
  if (fi->header & kIsSeparated) {
    Reader m{ip_map};
    uint32_t n = m.U();
    bool found = false;
    for (uint32_t i = 0; i < n && !found; i++) {
      gaddr func = image + uint32_t(m.Rva());
      ip_map = image + uint32_t(m.Rva());
      found = func == seg_begin;
    }
    if (!found) return false;
  }
  Reader m{ip_map};
  fi->ip_count = m.U();
  fi->ip_map = m.p;
  if (fi->header & kIsCatch) fi->frame = r.U();
  return true;
}

// The EH state at pc: the IP-to-state entries' offsets accumulate from the
// segment's start.
int StateAt(const FuncInfo4& fi, gaddr seg_begin, gaddr pc) {
  Reader r{fi.ip_map};
  gaddr ip = seg_begin;
  int state = -1;
  for (uint32_t i = 0; i < fi.ip_count; i++) {
    ip += r.U();
    int s = int(r.U()) - 1;
    if (pc < ip) break;
    state = s;
  }
  return state;
}

// Unwind map entries: {type and back-link, [action RVA, [object offset]]};
// the back-link is how far before this entry the next state to unwind
// starts (before the map: none).
struct UnwindEntry {
  uint32_t type;
  gaddr prev;  // 0: none
  gaddr action;
  uint32_t object;
};
UnwindEntry ReadUnwind(gaddr image, gaddr map_start, Reader& r) {
  gaddr start = r.p;
  uint32_t v = r.U();
  UnwindEntry e{v & 3, start - (v >> 2), 0, 0};
  if (e.prev < map_start) e.prev = 0;
  if (e.type) e.action = image + uint32_t(r.Rva());
  if (e.type == 1 || e.type == 2) e.object = r.U();
  return e;
}

// Destroys the frame's objects from state `from` down to (not including)
// `to`: destructors (type 1: the object at frame + offset; 2: the one it
// points to) and unwind funclets (3), each called (object, frame).
void LocalUnwind(Ctx& c, gaddr image, const FuncInfo4& fi, gaddr frame, int from, int to) {
  if (from <= to || from >= int(fi.unwind_count)) return;
  // The entries are in state order; the back-links only ever go back, so
  // the walk stops on reaching or passing state `to`'s entry.
  Reader r{fi.unwind_map};
  gaddr last = fi.unwind_map - 1;
  for (int i = 0; i < from; i++) {
    if (i == to) last = r.p;
    ReadUnwind(image, fi.unwind_map, r);
  }
  for (gaddr at = r.p; at && at > last;) {
    Reader e{at};
    UnwindEntry u = ReadUnwind(image, fi.unwind_map, e);
    if (u.action) {
      gaddr obj = frame + u.object;
      if (u.type == 2) obj = Rd64(obj);
      CallGuest(c, u.action, {obj, frame});
    }
    at = u.prev;
  }
}

// ---- types ----------------------------------------------------------------------
// ThrowInfo {attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray}
// and CatchableType {properties, pType, PMD {mdisp, pdisp, vdisp},
// sizeOrOffset, copyFunction}: RVAs from the throwing image's base.
// TypeDescriptor {vftable, spare, name}.

constexpr uint32_t kTiConst = 1, kTiVolatile = 2;
constexpr uint32_t kCtSimple = 1, kCtByRefOnly = 2, kCtVirtualBase = 4;
constexpr uint32_t kHtConst = 1, kHtVolatile = 2, kHtReference = 8;

struct Caught {
  gaddr type;  // the CatchableType
};

gaddr ThisPointer(gaddr ct, gaddr obj) {
  if (!obj) return 0;
  int32_t mdisp = int32_t(Rd32(ct + 8)), pdisp = int32_t(Rd32(ct + 12)),
          vdisp = int32_t(Rd32(ct + 16));
  if (pdisp >= 0) {
    gaddr vbtable = Rd64(obj + pdisp);
    obj += pdisp + int32_t(Rd32(vbtable + vdisp));
  }
  return obj + mdisp;
}

// The CatchableType of the thrown object that a catch of type `catch_td`
// (0: catch(...)) with `flags` takes, or 0.
gaddr FindCaught(gaddr throw_info, gaddr base, gaddr catch_td, uint32_t flags) {
  if (!catch_td) return ~0ull;  // catch(...): no copy
  uint32_t attr = Rd32(throw_info);
  if ((attr & kTiConst) && !(flags & kHtConst)) return 0;
  if ((attr & kTiVolatile) && !(flags & kHtVolatile)) return 0;
  gaddr arr = base + uint32_t(Rd32(throw_info + 12));
  int32_t n = int32_t(Rd32(arr));
  const std::string want = GStr(catch_td + 16);
  for (int32_t i = 0; i < n; i++) {
    gaddr ct = base + uint32_t(Rd32(arr + 4 + 4 * i));
    gaddr td = base + uint32_t(Rd32(ct + 4));
    if (td != catch_td && GStr(td + 16) != want) continue;
    if ((Rd32(ct) & kCtByRefOnly) && !(flags & kHtReference)) continue;
    return ct;
  }
  return 0;
}

void CopyException(Ctx& c, gaddr obj, gaddr dest, uint32_t flags, gaddr ct, gaddr base) {
  if (flags & kHtReference) {
    Wr64(dest, ThisPointer(ct, obj));
    return;
  }
  uint32_t props = Rd32(ct);
  uint32_t size = Rd32(ct + 20);
  int32_t copy = int32_t(Rd32(ct + 24));
  if (props & kCtSimple) {
    std::memmove(GPtr(dest), GPtr(obj), size);
    if (size == 8) Wr64(dest, ThisPointer(ct, Rd64(dest)));  // a pointer
    return;
  }
  gaddr src = ThisPointer(ct, obj);
  if (!copy) {
    std::memmove(GPtr(dest), GPtr(src), size);
  } else if (props & kCtVirtualBase) {
    CallGuest(c, base + uint32_t(copy), {dest, src, 1});
  } else {
    CallGuest(c, base + uint32_t(copy), {dest, src});
  }
}

void DestroyException(Ctx& c, gaddr rec) {
  gaddr obj = Rd64(rec + 0x20 + 8), ti = Rd64(rec + 0x20 + 16), base = Rd64(rec + 0x20 + 24);
  if (!obj || !ti) return;
  if (int32_t dtor = int32_t(Rd32(ti + 4))) CallGuest(c, base + uint32_t(dtor), {obj});
}

// __current_exception / __current_exception_context: per-thread slots the
// guest reads (std::current_exception, std::uncaught_exceptions).
gaddr CurrentSlots() {
  static thread_local gaddr slots = 0;
  if (!slots) slots = HeapAllocProcess(16, true);
  return slots;
}

}  // namespace

// _CxxThrowException(object, throw_info): RaiseException(0xE06D7363,
// noncontinuable, {magic, object, throw_info, the throw_info's image}).
HLE64("vcruntime140.dll", _CxxThrowException, kCdecl) {
  gaddr obj = Arg(c, 0), ti = Arg(c, 1);
  if (!obj && !ti) FatalF("guest C++: rethrow (throw;): not supported yet");
  const PeImage* img = ImageAt(ti);
  uint64_t params[4] = {kCxxMagic, obj, ti, img ? img->image_base : 0};
  seh64::Raise(c, kCxxException, seh64::kNoncontinuable, params, 4, seh64::AtReturn(c));
  FatalF("guest C++ exception continued");
}

HLE64("vcruntime140.dll", __current_exception, kCdecl) { return CurrentSlots(); }
HLE64("vcruntime140.dll", __current_exception_context, kCdecl) { return CurrentSlots() + 8; }

// __CxxFrameHandler4(record, frame, context, dispatcher_context)
HLE64("vcruntime140_1.dll", __CxxFrameHandler4, kCdecl) {
  gaddr rec = Arg(c, 0), frame = Arg(c, 1), dc = Arg(c, 3);
  const gaddr image = Rd64(dc + seh64::kDcImageBase);
  const gaddr seg_begin = image + Rd32(Rd64(dc + seh64::kDcFunctionEntry));
  const gaddr pc = Rd64(dc + seh64::kDcControlPc);
  const uint32_t code = Rd32(rec), flags = Rd32(rec + 4);
  FuncInfo4 fi;
  if (!ReadFuncInfo(image, Rd64(dc + seh64::kDcHandlerData), seg_begin, &fi))
    FatalF("__CxxFrameHandler4: no IP-to-state map for %08llX", (unsigned long long)seg_begin);
  // A catch funclet's objects are its parent's: its frame holds the
  // parent's establisher frame at fi.frame.
  const gaddr orig = (fi.header & kIsCatch) ? Rd64(frame + fi.frame) : frame;
  const int state = StateAt(fi, seg_begin, pc);

  if (flags & (seh64::kUnwinding | seh64::kExitUnwind)) {
    LocalUnwind(c, image, fi, orig, state, -1);
    return seh64::kContinueSearch;
  }
  if ((fi.header & kEhs) && code != kCxxException) return seh64::kContinueSearch;
  if (!fi.try_map) {
    if ((fi.header & kNoExcept) && !(fi.header & kIsCatch) && code == kCxxException)
      FatalF("guest C++: an exception left a noexcept function");
    return seh64::kContinueSearch;
  }

  const bool cxx = code == kCxxException && Rd32(rec + 0x18) >= 3 &&
                   Rd64(rec + 0x20) == kCxxMagic;
  const gaddr obj = cxx ? Rd64(rec + 0x28) : 0, ti = cxx ? Rd64(rec + 0x30) : 0;
  const gaddr exc_base = cxx && Rd32(rec + 0x18) >= 4 ? Rd64(rec + 0x38) : 0;

  Reader tr{fi.try_map};
  for (uint32_t i = 0; i < fi.try_count; i++) {
    int start = int(tr.U()), end = int(tr.U());
    tr.U();  // catch level
    int32_t cb = tr.Rva();
    if (state < start || state > end || !cb) continue;
    Reader cr{image + uint32_t(cb)};
    uint32_t n = cr.U();
    for (uint32_t j = 0; j < n; j++) {
      uint8_t h = cr.Byte();
      uint32_t cflags = (h & 0x01) ? cr.U() : 0;
      gaddr td = (h & 0x02) ? image + uint32_t(cr.Rva()) : 0;
      uint32_t offset = (h & 0x04) ? cr.U() : 0;
      gaddr handler = image + uint32_t(cr.Rva());
      gaddr ret[2] = {0, 0};
      int nret = (h & 0x30) == 0x10 ? 1 : (h & 0x30) == 0x20 ? 2 : 0;
      for (int k = 0; k < nret; k++)
        ret[k] = (h & 0x08) ? image + uint32_t(cr.Rva()) : seg_begin + cr.U();

      gaddr ct = 0;
      if (cxx) {
        ct = FindCaught(ti, exc_base, td, cflags);
        if (!ct) continue;
      } else if (td) {
        continue;  // only catch(...) takes a non-C++ exception
      }

      // Caught here. Unwind the frames below, then this frame down to the
      // try block, then run the catch block.
      RECOMP_TRACE("  C++ exception caught by the catch block at %08llX\n",
                   (unsigned long long)handler);
      unwind::Regs inside = seh64::UnwindBelow(c, rec, frame);
      LocalUnwind(c, image, fi, orig, state, start);
      if (ct && ct != ~0ull && td && Rd8(td + 16) && offset)
        CopyException(c, obj, orig + offset, cflags, ct, exc_base);
      Wr64(CurrentSlots(), rec);
      Wr64(CurrentSlots() + 8, seh64::Current()->context);
      // Funclets get (themselves, the parent frame, 0x100), as Windows
      // calls them.
      gaddr cont = CallGuest(c, handler, {handler, orig, 0x100});
      if (nret == 2) cont = ret[cont ? 1 : 0];
      else if (nret == 1) cont = ret[0];
      if (cxx) DestroyException(c, rec);
      Wr64(CurrentSlots(), 0);
      Wr64(CurrentSlots() + 8, 0);
      seh64::ContinueAt(c, inside, frame, cont, 0);
    }
  }
  if ((fi.header & kNoExcept) && !(fi.header & kIsCatch) && cxx)
    FatalF("guest C++: an exception left a noexcept function");
  return seh64::kContinueSearch;
}

// __C_specific_handler(record, frame, context, dispatcher_context): the
// scope table {count; {begin, end, handler, target}...} (RVAs). An entry
// with a target is __try/__except (handler: the filter, or 1 for
// EXCEPTION_EXECUTE_HANDLER); without one, __try/__finally (handler: the
// termination handler, run while unwinding).
HLE64("vcruntime140.dll", __C_specific_handler, kCdecl) {
  gaddr rec = Arg(c, 0), frame = Arg(c, 1), ctx = Arg(c, 2), dc = Arg(c, 3);
  const gaddr image = Rd64(dc + seh64::kDcImageBase);
  const gaddr pc = Rd64(dc + seh64::kDcControlPc);
  const gaddr table = Rd64(dc + seh64::kDcHandlerData);
  const uint32_t count = Rd32(table), flags = Rd32(rec + 4);
  auto field = [&](uint32_t i, int k) { return Rd32(table + 4 + 16 * i + 4 * k); };
  auto in = [&](uint32_t i, gaddr a) {
    return a >= image + field(i, 0) && a < image + field(i, 1);
  };
  if (!(flags & (seh64::kUnwinding | seh64::kExitUnwind))) {
    for (uint32_t i = Rd32(dc + seh64::kDcScopeIndex); i < count; i++) {
      if (!in(i, pc) || !field(i, 3)) continue;
      int32_t filter = 1;  // EXCEPTION_EXECUTE_HANDLER
      if (field(i, 2) != 1) {
        gaddr ptrs = (c.rsp - 0x40) & ~15ull;  // EXCEPTION_POINTERS {record, context}
        Wr64(ptrs, rec);
        Wr64(ptrs + 8, ctx);
        uint64_t saved = c.rsp;
        c.rsp = ptrs - 0x20;
        filter = int32_t(CallGuest(c, image + field(i, 2), {ptrs, frame}));
        c.rsp = saved;
      }
      if (filter < 0) return seh64::kContinueExecution;
      if (filter > 0) seh64::UnwindTo(c, rec, frame, image + field(i, 3), Rd32(rec));
    }
    return seh64::kContinueSearch;
  }
  const gaddr target = Rd64(dc + seh64::kDcTargetIp);
  for (uint32_t i = Rd32(dc + seh64::kDcScopeIndex); i < count; i++) {
    if (!in(i, pc)) continue;
    // The target unwind stops at the __except whose block it goes to.
    if ((flags & seh64::kTargetUnwind) && field(i, 3) && image + field(i, 3) == target) break;
    if (field(i, 3)) continue;
    Wr32(dc + seh64::kDcScopeIndex, i + 1);
    CallGuest(c, image + field(i, 2), {1, frame});  // abnormal termination
  }
  return seh64::kContinueSearch;
}

}  // namespace recomp::hle
