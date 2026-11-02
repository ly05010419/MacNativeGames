#pragma once
// x64 unwind data (the .pdata RUNTIME_FUNCTIONs and their UNWIND_INFO) and
// the virtual unwinder over guest memory, as Windows' RtlVirtualUnwind
// works (ARCHITECTURE.md, "64-bit guests"). Shared by the lifter (prologue
// sizes, frame registers, language handlers) and the runtime (exception
// dispatch, RtlVirtualUnwind).

#include <cstdint>

#include "runtime/guest_memory.h"
#include "runtime/pe_image.h"

namespace recomp::unwind {

// UNWIND_INFO flags.
inline constexpr uint8_t kEHandler = 1, kUHandler = 2, kChainInfo = 4;

struct Info {
  uint8_t version = 0, flags = 0, prolog = 0, count = 0;
  uint8_t frame_reg = 0, frame_off = 0;  // frame register (0: none), offset / 16
  gaddr codes = 0;         // the UNWIND_CODE array
  gaddr handler = 0;       // language handler VA (EHANDLER/UHANDLER), else 0
  gaddr handler_data = 0;  // VA right after the handler's RVA
  gaddr chained = 0;       // CHAININFO: the parent RUNTIME_FUNCTION's VA
};
// Reads the UNWIND_INFO at unwind_va of the image at image_base.
Info Read(const GuestMemory& m, gaddr image_base, gaddr unwind_va);

// The RUNTIME_FUNCTION covering pc in img (its VA), or 0 for leaf code.
gaddr Lookup(const PeImage& img, gaddr pc);

// What the unwinder works on: CONTEXT's integer registers (rax..r15 by
// number), rip and the XMM registers.
struct Regs {
  uint64_t gpr[16];
  uint64_t rip;
  uint8_t xmm[16][16];
};

// One frame of RtlVirtualUnwind: for the function whose RUNTIME_FUNCTION is
// at fn_va (in the image at image_base) with r.rip inside it, undoes what
// its prologue has done so far and returns to its caller (r becomes the
// caller's state). Returns the frame's establisher frame. *handler gets its
// language handler if it has one of the `types` (kEHandler, kUHandler) and
// rip is past its prologue and not in an epilog, else 0; *data its
// handler data.
gaddr VirtualUnwind(const GuestMemory& m, gaddr image_base, gaddr fn_va, Regs& r,
                    uint8_t types, gaddr* handler, gaddr* data);

}  // namespace recomp::unwind
