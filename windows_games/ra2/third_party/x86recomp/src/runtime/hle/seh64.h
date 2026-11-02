#pragma once
// x64 structured exception handling (ARCHITECTURE.md, "64-bit guests"):
// table-based, over the guest's .pdata/UNWIND_INFO and its stack. Exception
// dispatch calls the frames' language handlers (lifted code, or the HLE's
// __CxxFrameHandler4 / __C_specific_handler behind their import stubs) with
// Windows' structures in guest memory; an unwind ends by throwing
// GuestResume to the lifted function that owns the target frame.

#include <cstdint>

#include "runtime/ctx.h"
#include "runtime/guest_memory.h"
#include "runtime/unwind.h"

namespace recomp::hle::seh64 {

// EXCEPTION_RECORD flags.
inline constexpr uint32_t kNoncontinuable = 0x1, kUnwinding = 0x2, kExitUnwind = 0x4,
                          kTargetUnwind = 0x20;
// EXCEPTION_DISPOSITION.
inline constexpr uint32_t kContinueExecution = 0, kContinueSearch = 1;

// x64 CONTEXT (0x4D0 bytes) and DISPATCHER_CONTEXT field offsets.
inline constexpr uint32_t kContextSize = 0x4D0, kCtxRax = 0x78, kCtxRip = 0xF8,
                          kCtxMxcsr = 0x34, kCtxXmm0 = 0x1A0;
inline constexpr uint32_t kDcControlPc = 0x00, kDcImageBase = 0x08, kDcFunctionEntry = 0x10,
                          kDcEstablisherFrame = 0x18, kDcTargetIp = 0x20,
                          kDcContextRecord = 0x28, kDcLanguageHandler = 0x30,
                          kDcHandlerData = 0x38, kDcScopeIndex = 0x48;

// The caller's state at an HLE call: its registers (spilled in c), rip the
// return address, rsp past it.
unwind::Regs AtReturn(const Ctx& c);

void WriteContext(gaddr ctx, const unwind::Regs& r, const Ctx& c);
unwind::Regs ReadContext(gaddr ctx);

// RaiseException: dispatches the exception from `at`. Returns if a handler
// continued execution (c then holds the context it left); fatal if no
// handler takes it.
void Raise(Ctx& c, uint32_t code, uint32_t flags, const uint64_t* params, uint32_t n,
           const unwind::Regs& at);

// The exception being dispatched on this thread: its record and the
// context it was raised in (language handlers that unwind start there).
struct Dispatching {
  gaddr record, context;
  unwind::Regs at;
};
const Dispatching* Current();

// RtlUnwindEx's second pass: calls the unwind handlers of the frames from
// the raise point up to target_frame (excluded) with EXCEPTION_UNWINDING,
// and returns the state inside the target frame.
unwind::Regs UnwindBelow(Ctx& c, gaddr record, gaddr target_frame);

// Continues in the frame whose state is `s` at target_ip with rax = value
// (throws GuestResume to its lifted function).
[[noreturn]] void ContinueAt(Ctx& c, const unwind::Regs& s, gaddr target_frame,
                             gaddr target_ip, uint64_t value);

// RtlUnwindEx: UnwindBelow, then the target frame's own unwind handler
// with EXCEPTION_TARGET_UNWIND, then ContinueAt.
[[noreturn]] void UnwindTo(Ctx& c, gaddr record, gaddr target_frame, gaddr target_ip,
                           uint64_t value);

}  // namespace recomp::hle::seh64
