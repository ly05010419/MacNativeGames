// Indirect control flow and the fatal paths of lifted code.
//
// Dispatch() maps a guest address to its lifted function through a dense
// table over the .text of the EXE or of a lifted DLL, or to an HLE handler
// for thunk addresses. Anything else is fatal: there is no interpreter
// fallback (CLAUDE.md).

#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdarg>
#include <fcntl.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/lazy_mem.h"
#include "runtime/lifted.h"
#include "runtime/log.h"
#include "runtime/platform.h"
#include "runtime/runtime.h"

namespace recomp {

uint8_t* g_guest_base = nullptr;

namespace {

// One per image: the EXE's first, then the lifted DLLs'. Built before any
// guest code runs and never changed afterwards.
struct CodeRange {
  uint64_t lo = 0, size = 0;
  std::vector<GuestFn> table;  // by va - lo
};
CodeRange g_exe;
std::vector<CodeRange> g_dlls;
const LiftedFn* g_lifted = nullptr;
uint32_t g_lifted_count = 0;

CodeRange TextOf(const PeImage& img) {
  CodeRange r;
  for (const auto& s : img.sections)
    if (s.name == ".text") {
      r.lo = s.va;
      r.size = s.vsize;
    }
  r.table.assign(r.size, nullptr);
  for (uint32_t i = 0; i < g_lifted_count; i++) {
    const LiftedFn& f = g_lifted[i];
    if (f.va - r.lo < r.size) r.table[f.va - r.lo] = f.fn;
  }
  return r;
}

void DumpGuestState64(const Ctx& c) {
  Flags f;
  UnpackEFlags(f, c.eflags);
  std::fprintf(stderr,
               "  rax=%016llX rcx=%016llX rdx=%016llX rbx=%016llX\n"
               "  rsp=%016llX rbp=%016llX rsi=%016llX rdi=%016llX\n"
               "  r8 =%016llX r9 =%016llX r10=%016llX r11=%016llX\n"
               "  r12=%016llX r13=%016llX r14=%016llX r15=%016llX\n"
               "  flags: cf=%d zf=%d sf=%d of=%d df=%d  mxcsr=%04X\n",
               (unsigned long long)c.rax, (unsigned long long)c.rcx,
               (unsigned long long)c.rdx, (unsigned long long)c.rbx,
               (unsigned long long)c.rsp, (unsigned long long)c.rbp,
               (unsigned long long)c.rsi, (unsigned long long)c.rdi,
               (unsigned long long)c.r8, (unsigned long long)c.r9,
               (unsigned long long)c.r10, (unsigned long long)c.r11,
               (unsigned long long)c.r12, (unsigned long long)c.r13,
               (unsigned long long)c.r14, (unsigned long long)c.r15, f.cf, f.zf, f.sf,
               f.of, f.df, c.mxcsr);
  if (!g_proc) return;
  // x64 code keeps no frame chain: show the stack's qwords that point
  // into an image (return addresses, mostly).
  std::fprintf(stderr, "  stack (image addresses):\n");
  int shown = 0;
  for (uint64_t a = c.rsp; shown < 24 && a < c.rsp + 0x2000; a += 8) {
    if (!g_proc->mem.IsCommitted(a)) break;
    uint64_t v = Rd64(a);
    if (ImageAt(v)) {
      std::fprintf(stderr, "    [rsp+%04llX] %016llX\n", (unsigned long long)(a - c.rsp),
                   (unsigned long long)v);
      shown++;
    }
  }
}

void DumpGuestState(const Ctx& c) {
  if (g_proc && g_proc->x64) return DumpGuestState64(c);
  Flags f;
  UnpackEFlags(f, c.eflags);
  std::fprintf(stderr,
               "  eax=%08X ecx=%08X edx=%08X ebx=%08X\n"
               "  esp=%08X ebp=%08X esi=%08X edi=%08X\n"
               "  flags: cf=%d zf=%d sf=%d of=%d df=%d  fpu: cw=%04X sw=%04X\n",
               c.eax, c.ecx, c.edx, c.ebx, c.esp, c.ebp, c.esi, c.edi, f.cf,
               f.zf, f.sf, f.of, f.df, x87::ControlWord(c.fpu),
               x87::StatusWord(c.fpu));
  if (!g_proc) return;
  // Guest backtrace along the EBP chain (VC6 code mostly keeps frames).
  std::fprintf(stderr, "  guest backtrace (ebp chain):\n");
  uint32_t ebp = c.ebp;
  for (int i = 0; i < 24; i++) {
    if (!g_proc->mem.IsCommitted(ebp) || !g_proc->mem.IsCommitted(ebp + 4))
      break;
    uint32_t ret = Rd32(ebp + 4);
    std::fprintf(stderr, "    #%-2d ret %08X  (frame %08X)\n", i, ret, ebp);
    uint32_t next = Rd32(ebp);
    if (next <= ebp) break;
    ebp = next;
  }
  std::fprintf(stderr, "  stack top:");
  for (int i = 0; i < 8; i++)
    if (g_proc->mem.IsCommitted(c.esp + 4 * i))
      std::fprintf(stderr, " %08X", Rd32(c.esp + 4 * i));
  std::fprintf(stderr, "\n");
}

}  // namespace

namespace {

// g_fatal_report: FatalF's message with the guest state and backtraces,
// to stderr and, so it survives a filtered or closed terminal, to
// $XDG_DATA_HOME/<id>/crashes/crash-<time>-<pid>.txt.
[[noreturn]] void FatalReport(const char* msg) {
  auto report = [&] {
    std::fprintf(stderr, "FATAL: %s\n", msg);
    if (Thread* t = CurrentThread()) {
      std::fprintf(stderr, "  guest thread %04X\n", t->tid);
      DumpGuestState(t->ctx);
    }
    std::fprintf(stderr, "  host backtrace:\n");
    std::fflush(stderr);
    void* frames[64];
    int nf = backtrace(frames, 64);
    backtrace_symbols_fd(frames, nf, 2);
  };
  report();
  std::string path = ReportPath("crash");
  int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd >= 0) {
    std::fprintf(stderr, "  (report saved to %s)\n", path.c_str());
    std::fflush(stderr);
    dup2(fd, 2);
    report();
  }
  std::_Exit(3);
}

const bool kFatalReportSet = (g_fatal_report = FatalReport, true);

}  // namespace


// Host faults (SIGSEGV/SIGBUS) in guest memory are first offered to the
// lazy regions (lazy_mem.h), whose pages fault by design, on any thread
// and from host code too. Others are guest access
// violations: they go to the guest SEH chain (games may demand-page data
// from an exception filter). Anything unhandled, or a fault outside
// the arena (a runtime bug), is reported with the host call chain (lifted
// functions are named f_<guest va>) and exits.
namespace hle {
bool RaiseAccessViolation(Ctx& c, bool write, uint32_t addr);
extern thread_local int t_hle_depth;
}

namespace {
// Faults can nest: an exception filter may itself touch a not-yet-paged
// page (an archive decompressor may). Windows dispatches a nested
// exception; so do we, up to a sanity limit.
thread_local int t_fault_depth = 0;
constexpr int kMaxFaultDepth = 8;

// Hosts whose unwinder cannot leave a signal frame (platform.h): a guest
// exception thrown while the fault is delivered (an __except unwind) is
// caught in the handler and rethrown from the faulting instruction.
thread_local std::exception_ptr t_fault_exception;

[[noreturn]] void RethrowFaultException() {
  std::exception_ptr e = std::move(t_fault_exception);
  t_fault_exception = nullptr;
  std::rethrow_exception(e);
}

// <ID>_WATCH_WRITE=<hex va>[,<hex len>[,<delay s>]]: a dev aid that finds the code
// writing a guest variable. Its host page is kept read-only; a write to
// the variable logs the host backtrace (lifted functions are named by
// their guest address) for the first few hits. The page is made writable
// at every fault and re-protected every few milliseconds, so writes to its
// other bytes go through (a few watched writes may be missed meanwhile).
std::atomic<uint32_t> g_watch_va{0}, g_watch_len{0}, g_watch_page{0}, g_watch_page_len{0};
std::atomic<int> g_watch_hits{0};
constexpr int kWatchMaxHits = 12;

// The faulting code's host frames: its pc and lr, then the frame-pointer
// chain (lifted code keeps frame records, RECOMP_FAULT_FRAME_FLAGS).
void LogFaultFrames(void* uc) {
#if defined(__APPLE__) && defined(__aarch64__)
  const auto& ss = static_cast<ucontext_t*>(uc)->uc_mcontext->__ss;
  uintptr_t frames[40];
  int n = 0;
  frames[n++] = uintptr_t(__darwin_arm_thread_state64_get_pc(ss));
  frames[n++] = uintptr_t(__darwin_arm_thread_state64_get_lr(ss));
  uintptr_t fp = uintptr_t(__darwin_arm_thread_state64_get_fp(ss));
  while (fp && n < 40 && (fp & 7) == 0) {
    const uintptr_t* rec = reinterpret_cast<const uintptr_t*>(fp);
    if (!rec[1]) break;
    frames[n++] = rec[1];
    if (rec[0] <= fp) break;
    fp = rec[0];
  }
  for (int i = 0; i < n; i++) {
    Dl_info info{};
    char buf[256];
    const int len = dladdr(reinterpret_cast<void*>(frames[i]), &info) && info.dli_sname
                        ? std::snprintf(buf, sizeof buf, "  %s\n", info.dli_sname)
                        : std::snprintf(buf, sizeof buf, "  %p\n", reinterpret_cast<void*>(frames[i]));
    (void)!write(2, buf, size_t(len));
  }
#else
  (void)uc;
  void* frames[48];
  backtrace_symbols_fd(frames, backtrace(frames, 48), 2);
#endif
}

bool WatchFault(uint32_t ga, void* uc) {
  const uint32_t page = g_watch_page.load();
  if (!page || ga - page >= g_watch_page_len.load()) return false;
  // The page is readable: whatever faults on it is a write.
  if (ga - g_watch_va.load() < g_watch_len.load() && g_watch_hits < kWatchMaxHits) {
    char buf[96];
    const int n = std::snprintf(buf, sizeof buf, "WATCH: write to %08X:\n", ga);
    (void)!write(2, buf, size_t(n));
    LogFaultFrames(uc);
    g_watch_hits++;
  }
  mprotect(g_proc->mem.ptr(page), g_watch_page_len.load(), PROT_READ | PROT_WRITE);
  return true;
}

void StartWriteWatch() {
  const char* e = Env("WATCH_WRITE");
  if (!e || !*e) return;
  char* end = nullptr;
  const uint32_t va = uint32_t(std::strtoul(e, &end, 16));
  const uint32_t len = end && *end == ',' ? uint32_t(std::strtoul(end + 1, &end, 16)) : 4;
  // A third field: seconds to wait before arming (heap objects exist
  // only once the game has made them).
  const int delay = end && *end == ',' ? std::atoi(end + 1) : 0;
  const uint32_t hp = uint32_t(g_proc->mem.host_page_size());
  g_watch_va = va;
  g_watch_len = len;
  g_watch_page_len = hp;
  std::thread([va, hp, delay] {
    std::this_thread::sleep_for(std::chrono::seconds(delay));
    g_watch_page = va & ~(hp - 1);
    if (mprotect(g_proc->mem.ptr(g_watch_page.load()), hp, PROT_READ) != 0)
      std::fprintf(stderr, "WATCH: mprotect failed (errno %d)\n", errno);
    else
      std::fprintf(stderr, "WATCH: armed on page %08X\n", g_watch_page.load());
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      if (g_watch_hits >= kWatchMaxHits) return;
      mprotect(g_proc->mem.ptr(g_watch_page.load()), g_watch_page_len.load(), PROT_READ);
    }
  }).detach();
  std::fprintf(stderr, "WATCH: writes to %08X..%08X\n", va, va + len);
}

void FaultHandler(int sig, siginfo_t* si, void* uc) {
  char buf[256];
  uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
  uintptr_t base = reinterpret_cast<uintptr_t>(g_guest_base);
  // x64 guests: no guest exception dispatch yet (ARCHITECTURE.md, "64-bit
  // guests"), so their faults are reported below.
  const bool guest32 = a - base < (1ull << 32) && g_proc && !g_proc->x64;
  if (guest32 && lazy::HandleFault(uint32_t(a - base)))
    return;  // filled: retry the faulting instruction
  platform::FaultContext fc = platform::ReadFaultContext(uc);
  if (guest32 && WatchFault(uint32_t(a - base), uc)) return;
  if (guest32 && CurrentThread() &&
      t_fault_depth < kMaxFaultDepth && hle::t_hle_depth == 0) {
    ++t_fault_depth;
    bool cont;
    if constexpr (platform::kThrowThroughSignalFrames) {
      cont = hle::RaiseAccessViolation(CurrentThread()->ctx, fc.write,
                                       uint32_t(a - base));
    } else {
      try {
        cont = hle::RaiseAccessViolation(CurrentThread()->ctx, fc.write,
                                         uint32_t(a - base));
      } catch (...) {
        --t_fault_depth;
        t_fault_exception = std::current_exception();
        if (platform::RedirectToCall(uc, RethrowFaultException)) return;
        const char m[] = "FATAL: cannot resume after a guest exception "
                         "from a fault on this host\n";
        (void)!write(2, m, sizeof m - 1);
        _exit(4);
      }
    }
    --t_fault_depth;
    if (cont) return;  // retry the faulting instruction
  }
  int n;
  if (g_proc && a - base < g_proc->mem.size() * (g_proc->x64 ? 2 : 1))
    n = std::snprintf(buf, sizeof buf,
                      "FATAL: %s: guest memory fault at guest %08lX\n",
                      sig == SIGSEGV ? "SIGSEGV" : "SIGBUS",
                      static_cast<unsigned long>(a - base));
  else
    n = std::snprintf(buf, sizeof buf,
                      "FATAL: %s: host fault at %p (runtime bug)\n",
                      sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", si->si_addr);
  (void)!write(2, buf, size_t(n));
  char name[32] = "?";
  pthread_getname_np(pthread_self(), name, sizeof name);
  n = std::snprintf(buf, sizeof buf, "  thread '%s', pc %p, caller:\n", name,
                    reinterpret_cast<void*>(fc.pc));
  (void)!write(2, buf, size_t(n));
  // A call through a null pointer leaves pc 0: the caller (the return
  // address in [sp] or the link register) still shows where it was.
  if (fc.caller) {
    void* caller = reinterpret_cast<void*>(fc.caller);
    backtrace_symbols_fd(&caller, 1, 2);
  }
  void* frames[48];
  int nf = backtrace(frames, 48);
  backtrace_symbols_fd(frames, nf, 2);
  _exit(4);
}
}  // namespace

// Each host thread running guest code needs its own signal stack: SEH
// handlers for guest faults run lifted code on it, so it is large.
void InstallSignalStack() {
  constexpr size_t kSize = 16u << 20;
  void* p = mmap(nullptr, kSize, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  stack_t ss{};
  ss.ss_sp = p;
  ss.ss_size = kSize;
  sigaltstack(&ss, nullptr);
}

namespace {
void InstallFaultHandler() {
  InstallSignalStack();
  struct sigaction sa{};
  sa.sa_sigaction = FaultHandler;
  // SA_NODEFER: a fault inside a guest exception filter must be
  // deliverable while we are still in the handler for the first one.
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
}
}  // namespace

void RegisterLiftedFunctions(const LiftedFn* fns, uint32_t count) {
  g_lifted = fns;
  g_lifted_count = count;
}

void InitDispatch() {
  if (!g_lifted)
    FatalF("no lifted code registered (recomp_add_lifted compiles "
           "runtime/lifted_table.cpp into it)");
  InstallFaultHandler();
  StartWriteWatch();
  g_exe = TextOf(g_proc->img);
  for (const Module& m : g_proc->modules) {
    CodeRange& r = g_dlls.emplace_back(TextOf(m.img));
    // A DLL lifted at another base, or not at all, has no functions here.
    if (m.img.entry && !r.table[m.img.entry - r.lo])
      FatalF("%s at %08llX: its entry point %08llX is not lifted (lift it with "
             "recomp_lift --module, at the same --base)",
             m.name.c_str(), (unsigned long long)m.img.image_base,
             (unsigned long long)m.img.entry);
  }
}

void Dispatch(Ctx& c, uint64_t target, uint64_t va) {
  const CodeRange* r = &g_exe;
  if (target - r->lo >= r->size) {
    if (target - kThunkBase64 < kThunkRegionSize) {
      hle::CallThunk(c, target);
      return;
    }
    r = nullptr;
    for (const CodeRange& d : g_dlls)
      if (target - d.lo < d.size) r = &d;
    if (!r)
      FatalF("indirect branch to %08llX from %08llX: not code", (unsigned long long)target,
             (unsigned long long)va);
  }
  if (GuestFn fn = r->table[target - r->lo]) {
    fn(c);
    return;
  }
  FatalF("indirect branch to unlifted address %08llX from %08llX "
         "(add it to the manifest as a function entry)",
         (unsigned long long)target, (unsigned long long)va);
}

void Dispatch(Ctx& c, uint32_t target, uint32_t va) {
  const CodeRange* r = &g_exe;
  if (target - r->lo >= r->size) {
    if (target - kThunkBase < kThunkRegionSize) {
      hle::CallThunk(c, target);
      return;
    }
    r = nullptr;
    for (const CodeRange& d : g_dlls)
      if (target - d.lo < d.size) r = &d;
    if (!r) FatalF("indirect branch to %08X from %08X: not code", target, va);
  }
  if (GuestFn fn = r->table[target - r->lo]) {
    fn(c);
    return;
  }
  FatalF("indirect branch to unlifted address %08X from %08X "
         "(add it to the manifest as a function entry)", target, va);
}

// The continuation runs in the owning function's catch handler, where a
// second throw would leave that function's try block: catch it here.
void Resume(Ctx& c, GuestResume r) {
  for (;;) {
    try {
      if (g_proc->x64)
        Dispatch(c, uint64_t(r.target), uint64_t(r.from));
      else
        Dispatch(c, uint32_t(r.target), uint32_t(r.from));
      return;
    } catch (const GuestResume& again) {
      if (again.ebp != r.ebp) throw;
      r = again;
    }
  }
}

// x64 lifted code's fatal paths (64-bit VAs).
void Unimplemented(Ctx& c, uint64_t va, const char* what) {
  (void)c;
  FatalF("unimplemented instruction at %08llX: %s", (unsigned long long)va, what);
}
void BadJump(Ctx& c, uint64_t va, uint64_t target) {
  (void)c;
  FatalF("branch at %08llX to %08llX outside lifted code", (unsigned long long)va,
         (unsigned long long)target);
}
void Unreachable(Ctx& c, uint64_t va) {
  (void)c;
  FatalF("returned from a no-return call at %08llX", (unsigned long long)va);
}
void Trap(Ctx& c, uint64_t va) {
  (void)c;
  FatalF("breakpoint/trap instruction at %08llX", (unsigned long long)va);
}
void RaiseDivideError(Ctx& c, uint64_t va) {
  (void)c;
  FatalF("integer divide error (#DE) at %08llX", (unsigned long long)va);
}

void Unimplemented(Ctx& c, uint32_t va, const char* what) {
  (void)c;
  FatalF("unimplemented instruction at %08X: %s", va, what);
}

void BadJump(Ctx& c, uint32_t va, uint32_t target) {
  (void)c;
  FatalF("branch at %08X to %08X outside lifted code", va, target);
}

void Unreachable(Ctx& c, uint32_t va) {
  (void)c;
  FatalF("returned from a no-return call at %08X", va);
}

void Trap(Ctx& c, uint32_t va) {
  (void)c;
  // int3 in shipped code is an assert/DebugBreak. EXCEPTION_BREAKPOINT
  // is not raised through SEH (no game path has needed it); stop loudly.
  FatalF("breakpoint/trap instruction at %08X", va);
}

void RaiseDivideError(Ctx& c, uint32_t va) {
  (void)c;
  FatalF("integer divide error (#DE) at %08X", va);
}

// The x64 guest CPU (ARCHITECTURE.md, "64-bit guests"): AMD family 16h
// model 0 (the Jaguar class), with only what the lifter implements
// claimed: the x86-64 baseline up to SSE2, LAHF/SAHF in long mode and
// LZCNT. SSE3 and later, CMPXCHG16B, POPCNT, XSAVE and AVX are not (yet)
// claimed: the lifter has the forms the supported games contain, not
// those whole instruction sets.
static void Cpuid64(Ctx& c) {
  const uint32_t leaf = c.eax;
  uint32_t a = 0, b = 0, cx = 0, d = 0;
  switch (leaf) {
    case 0:
      a = 1;
      b = 0x68747541;  // "AuthenticAMD"
      d = 0x69746E65;
      cx = 0x444D4163;
      break;
    case 1:
      a = 0x00700F01;  // family 16h (0Fh + 07h), model 0, stepping 1
      b = (GuestProcessorCount() << 16) | (8u << 8);  // CLFLUSH: 64 bytes
      cx = 0;
      d = 0x0783F9FF;  // fpu tsc cx8 cmov mmx fxsr sse sse2 (as 32-bit)
      break;
    case 0x80000000:
      a = 0x80000001;
      b = 0x68747541;
      d = 0x69746E65;
      cx = 0x444D4163;
      break;
    case 0x80000001:
      a = 0x00700F01;
      cx = (1u << 0) | (1u << 5);   // LahfSahf, ABM (LZCNT)
      d = (1u << 11) | (1u << 20) | (1u << 29);  // SYSCALL, NX, long mode
      break;
    default:
      break;
  }
  c.rax = a;
  c.rbx = b;
  c.rcx = cx;
  c.rdx = d;
}

// XGETBV: XCR0 = x87 | SSE state (AVX state once AVX is claimed). Only
// reached by code that ignores the OSXSAVE bit, which is clear.
void Xgetbv(Ctx& c) {
  if (uint32_t(c.rcx) != 0) FatalF("xgetbv of XCR%u", uint32_t(c.rcx));
  c.rax = 0x3;
  c.rdx = 0;
}

void Cpuid(Ctx& c) {
  if (g_proc && g_proc->x64) return Cpuid64(c);
  // A fixed CPU, never the host's: lifted code behaves the same on every
  // machine (Apple Silicon has no CPUID), and nothing beyond SSE2 is
  // claimed. SSE and SSE2 are reported because every PC these games still
  // run on has them, so their SSE paths are the ones that matter
  // (runtime/sse.h). MMX comes with any SSE CPU; no supported game has
  // MMX code, and the lifter would report it as unimplemented. Family 6
  // model 13 (Pentium M) is a real SSE2 part without SSE3.
  switch (c.eax) {
    case 0:
      c.eax = 2;
      c.ebx = 0x756E6547;  // "GenuineIntel"
      c.edx = 0x49656E69;
      c.ecx = 0x6C65746E;
      break;
    case 1:
      c.eax = 0x000006D8;  // family 6, model 13, stepping 8
      c.ebx = 0;
      c.ecx = 0;
      c.edx = 0x0783F9FF;  // fpu tsc cx8 cmov mmx fxsr sse sse2
      break;
    default:
      c.eax = c.ebx = c.ecx = c.edx = 0;
      break;
  }
}

// A 3 GHz TSC. Games time it against QueryPerformanceCounter to measure
// the CPU and pick detail settings from tables of the era's machines, so
// it must not read slower than the fastest of those.
uint64_t Rdtsc(Ctx& c) {
  (void)c;
  using namespace std::chrono;
  return 3 * uint64_t(
      duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
          .count());
}

void Probe(Ctx& c, uint64_t va, const char* name) {
  FatalF("probe %s at %08llX: probes are not supported in x64 guests yet", name,
         (unsigned long long)va);
  (void)c;
}

void Probe(Ctx& c, uint32_t va, const char* name) {
  std::string line;
  char buf[96];
  std::snprintf(buf, sizeof buf, "probe: %s (%08X) ecx %08X args", name, va, c.ecx);
  line = buf;
  for (int i = 1; i <= 4; i++) {
    uint32_t a = Rd32(c.esp + 4 * i);
    std::snprintf(buf, sizeof buf, " %08X", a);
    line += buf;
    // Show arguments that point at a short printable string, directly or
    // through one pointer (the game's string class holds a char*).
    auto str = [&](uint32_t v, std::string* out) {
      if (v < 0x10000 || !g_proc->mem.IsCommitted(v) || !g_proc->mem.IsCommitted(v + 63))
        return false;
      const char* p = g_proc->mem.ptr<const char>(v);
      int n = 0;
      while (n < 64 && p[n] >= 0x20 && p[n] < 0x7F) n++;
      if (!(n == 64 || p[n] == 0) || (n < 3 && n != 0)) return false;
      *out = std::string(p, n);
      return n >= 3;
    };
    std::string sv;
    if (str(a, &sv)) {
      line += "=\"" + sv + "\"";
    } else if (a >= 0x10000 && g_proc->mem.IsCommitted(a) && g_proc->mem.IsCommitted(a + 3)) {
      uint32_t q = Rd32(a);
      if (q == 0) line += "->null";
      else if (str(q, &sv)) line += "->\"" + sv + "\"";
      else if (q >= 0x10000 && g_proc->mem.IsCommitted(q) && !GPtr(q)[0]) line += "->\"\"";
    }
  }
  std::fprintf(stderr, "%s\n", line.c_str());
}

}  // namespace recomp
