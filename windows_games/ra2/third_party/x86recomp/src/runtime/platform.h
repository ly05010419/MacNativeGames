#pragma once
// Host platform specifics (Linux, macOS) that the runtime cannot express
// in plain POSIX: signal contexts, stack switching, stat's time fields,
// precise sleeps.
// Everything here is behind one interface so the rest of the runtime has
// no #ifdef __linux__ / __APPLE__.

#include <sys/stat.h>

#include <cfenv>
#include <cstddef>
#include <cstdint>
#include <ctime>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace recomp::platform {

// ---- files
inline timespec StatAccessTime(const struct stat& st) {
#if defined(__APPLE__)
  return st.st_atimespec;
#else
  return st.st_atim;
#endif
}
inline timespec StatModifyTime(const struct stat& st) {
#if defined(__APPLE__)
  return st.st_mtimespec;
#else
  return st.st_mtim;
#endif
}

// Per-user data under $HOME (without XDG_DATA_HOME), with a trailing '/'.
#if defined(__APPLE__)
inline constexpr const char* kUserDataDir = "/Library/Application Support/";
#else
inline constexpr const char* kUserDataDir = "/.local/share/";
#endif

// ---- windows
// Whether the host shows one window per display (iOS): top-level windows
// after the first get no host window of their own.
#if defined(__APPLE__) && TARGET_OS_IOS
inline constexpr bool kSingleHostWindow = true;
#else
inline constexpr bool kSingleHostWindow = false;
#endif

// ---- floating point
// Why this thread's floating-point environment is not the default one the
// x87 model's host-double paths rely on (round to nearest, no flush-to-
// zero or denormals-are-zero, no traps), or nullptr. Checks the control
// register (MXCSR on x86-64, FPCR on arm64) and the behaviour itself.
inline const char* HostFpProblem() {
  if (std::fegetround() != FE_TONEAREST) return "rounding mode is not to nearest";
  std::fenv_t env;
  if (std::fegetenv(&env)) return "fegetenv failed";
#if defined(__x86_64__)
  // FTZ (15), RC (13-14), the exception masks (7-12, all set), DAZ (6).
  if ((env.__mxcsr & 0xFFC0u) != 0x1F80u) return "MXCSR is not the default";
#elif defined(__aarch64__)
  // FZ (24), RMode (22-23), the trap enables IDE (15) and IXE..IOE
  // (8-12), and FEAT_AFP's NEP, AH and FIZ (0-2).
  if (env.__fpcr & 0x01C09F07u) return "FPCR is not the default";
#endif
  volatile double one = 1.0, tiny = 0x1p-1022, denormal = 0x1p-1060;
  if (one + 0x1p-53 != 1.0 || one + 0x1.8p-53 == 1.0 || -one - 0x1.8p-53 == -1.0)
    return "rounding is not to nearest even";
  if (tiny / 4 == 0) return "denormal results are flushed to zero";
  if (denormal * 0x1p100 == 0) return "denormal operands are treated as zero";
  return nullptr;
}

// ---- time
// The host's monotonic clock in nanoseconds (not a guest tick counter).
uint64_t MonotonicNs();
// Sleeps until MonotonicNs() reaches t, without spinning. On an idle
// host it wakes within some tens of microseconds of t (Linux: absolute
// clock_nanosleep with the thread's timer slack at 1 us; macOS:
// mach_wait_until with the thread real-time while it waits).
void SleepUntilNs(uint64_t t);

// ---- stacks
// Runs fn() on [stack, stack + size) on the calling thread and returns
// when it does (the process main thread must stay the UI thread, but the
// guest needs a bigger stack than the OS gives it).
void RunOnStack(void* stack, size_t size, void (*fn)());

// ---- faults (SIGSEGV/SIGBUS handlers; `uc` is the handler's ucontext)
struct FaultContext {
  bool write_known = false;  // whether `write` comes from the CPU
  bool write = false;
  uintptr_t pc = 0;
  // Return address of the faulting function's caller as far as the
  // registers show it: [sp] on x86-64 at a call through null, the link
  // register on arm64. 0 when unknown.
  uintptr_t caller = 0;
};
FaultContext ReadFaultContext(const void* uc);

// Whether a C++ exception thrown inside a signal handler unwinds through
// the signal frame into the interrupted code. glibc's unwinder steps
// through the kernel's signal trampoline; Darwin's does not.
#if defined(__APPLE__)
inline constexpr bool kThrowThroughSignalFrames = false;
#else
inline constexpr bool kThrowThroughSignalFrames = true;
#endif

// Where kThrowThroughSignalFrames is false: makes the interrupted thread
// resume, when the handler returns, in fn() as if the faulting instruction
// had called it. An exception fn() throws then unwinds through the
// faulting function like one thrown by a call made there. The faulting
// function must have a frame record (lifted code is built with
// -mno-omit-leaf-frame-pointer on such hosts: the link register is
// overwritten). False if this host has no implementation.
bool RedirectToCall(void* uc, void (*fn)());

}  // namespace recomp::platform
