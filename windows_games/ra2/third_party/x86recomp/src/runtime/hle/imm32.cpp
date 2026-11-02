// imm32: the input method manager of a system without an IME installed.
// There is no input context (ImmGetContext returns NULL), so every call on
// one fails or reports nothing, as the documentation describes for NULL.

#include "runtime/hle/hle.h"

namespace recomp::hle {
namespace {

// (hWnd)
HLE("imm32.dll", ImmGetContext, 1) { return 0; }
// (hWnd, hIMC)
HLE("imm32.dll", ImmReleaseContext, 2) { return Arg(c, 1) ? 1 : 0; }
// The window's previous context, which there never is.
HLE("imm32.dll", ImmAssociateContext, 2) { return 0; }
// (hIMC, fOpen)
HLE("imm32.dll", ImmSetOpenStatus, 2) { return 0; }
// (hIMC)
HLE("imm32.dll", ImmGetOpenStatus, 1) { return 0; }
// (hIMC, dwAction, dwIndex, dwValue)
HLE("imm32.dll", ImmNotifyIME, 4) { return 0; }
// (hIMC, dwIndex, lpBuf, dwBufLen): bytes copied; IMM_ERROR_GENERAL (-2)
// without a context.
HLE("imm32.dll", ImmGetCompositionStringA, 4) { return 0xFFFFFFFE; }
HLE("imm32.dll", ImmGetCompositionStringW, 4) { return 0xFFFFFFFE; }
// (hIMC, dwIndex, lpCandList, dwBufLen): 0, no candidate list.
HLE("imm32.dll", ImmGetCandidateListA, 4) { return 0; }

}  // namespace
}  // namespace recomp::hle
