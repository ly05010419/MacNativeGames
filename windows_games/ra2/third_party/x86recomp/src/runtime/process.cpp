// Process and thread bring-up: arena, image, PEB/TEB, TLS, stacks, and the
// host threads that run guest code.

#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <thread>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "runtime/gfx/device.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/lifted.h"
#include "runtime/log.h"
#include "runtime/platform.h"
#include "runtime/runtime.h"
#include "runtime/sse.h"
#include "runtime/watchdog.h"

namespace recomp {

void InitDispatch();
void InstallSignalStack();

Process* g_proc = nullptr;

namespace {

thread_local Thread* t_current = nullptr;
std::mutex g_threads_mu;
std::map<uint32_t, std::unique_ptr<Thread>> g_threads;  // by tid
gaddr g_next_teb = 0;  // the next TEB is below it (kTebTop, kTebTop64)

// Host stack for each guest thread. Guest call depth maps 1:1 onto host
// call depth, and lifted frames are larger than the originals, so be
// generous; the memory is only touched as needed.
constexpr size_t kHostStack = 256u << 20;

constexpr uint32_t kTlsArraySlots = 64;

}  // namespace

Thread* CurrentThread() { return t_current; }

uint32_t GuestProcessorCount() {
  unsigned n = std::thread::hardware_concurrency();
  return std::clamp<unsigned>(n, 2, 8);  // capped: 2002 code may assume few
}

const GameConfig& Game() { return g_proc->game; }

const char* Env(const char* name) {
  if (!g_proc) return nullptr;
  std::string var;
  for (char ch : g_proc->game.id) var += char(std::toupper(uint8_t(ch)));
  var += '_';
  var += name;
  return std::getenv(var.c_str());
}

std::string DataDir() {
  const std::string& id = g_proc->game.id;
  if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x)
    return std::string(x) + "/" + id;
  if (const char* h = std::getenv("HOME"); h && *h)
    return std::string(h) + platform::kUserDataDir + id;
  return "/tmp/" + id;
}

void MakeDirs(const std::string& path) {
  for (size_t i = 1; i <= path.size(); i++)
    if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

std::string ReportPath(const char* kind) {
  std::string k = kind;  // crash -> crashes, hang -> hangs
  std::string dir = DataDir() + "/" + k + (k.ends_with("sh") ? "es" : "s");
  MakeDirs(dir);
  char name[64];
  std::snprintf(name, sizeof name, "/%s-%ld-%d.txt", kind, long(std::time(nullptr)),
                int(getpid()));
  return dir + name;
}

std::string GStr(gaddr va) {
  if (!va) return {};
  return std::string(g_proc->mem.ptr<const char>(va));
}

std::u16string GWStr(gaddr va) {
  std::u16string s;
  if (!va) return s;
  for (;; va += 2) {
    char16_t ch = Rd16(va);
    if (!ch) break;
    s.push_back(ch);
  }
  return s;
}

void PutStr(gaddr va, const std::string& s) {
  std::memcpy(GPtr(va), s.c_str(), s.size() + 1);
}

void PutWStr(gaddr va, const std::string& s) {
  for (size_t i = 0; i <= s.size(); i++) Wr16(va + 2 * i, uint8_t(s.c_str()[i]));
}

uint64_t CallGuest(Ctx& c, gaddr va, std::initializer_list<uint64_t> args) {
  // Guest code runs here: faults are guest access violations again.
  struct DepthGuard {
    int saved = hle::t_hle_depth;
    DepthGuard() { hle::t_hle_depth = 0; }
    ~DepthGuard() { hle::t_hle_depth = saved; }
  } guard;
  if (g_proc->x64) {
    // Win64: the callee finds its stack arguments above the return
    // address and 32 bytes of shadow space (homes for rcx..r9), and rsp
    // is 16-byte aligned before the call.
    const uint64_t saved = c.rsp;
    const uint64_t n = std::max<uint64_t>(args.size(), 4);
    uint64_t sp = ((c.rsp - 8 * n) & ~15ull) - 8;
    uint64_t* regs[4] = {&c.rcx, &c.rdx, &c.r8, &c.r9};
    size_t i = 0;
    for (uint64_t a : args) {
      if (i < 4) *regs[i] = a;
      Wr64(sp + 8 + 8 * i, a);
      i++;
    }
    Wr64(sp, kReturnToHost64);
    c.rsp = sp;
    Dispatch(c, va, kReturnToHost64);
    c.rsp = saved;
    return c.rax;
  }
  uint32_t saved = c.esp;
  for (auto it = std::rbegin(args); it != std::rend(args); ++it) {
    c.esp -= 4;
    Wr32(c.esp, uint32_t(*it));
  }
  c.esp -= 4;
  Wr32(c.esp, kReturnToHost);
  Dispatch(c, uint32_t(va), kReturnToHost);
  c.esp = saved;  // stdcall callees popped their args, cdecl didn't
  return c.eax;
}

// The thread's TEB and TLS array, in the layout of the process's mode.
static void InitTeb(Thread* t) {
  Process& p = *g_proc;
  const gaddr teb = t->teb;
  // Static TLS of the image (__declspec(thread)): a per-thread copy of the
  // template, found through the TEB's ThreadLocalStoragePointer
  // [tls_index].
  const uint32_t ptr = p.x64 ? 8 : 4;
  gaddr tls_array = HeapAllocProcess(kTlsArraySlots * ptr, true);
  if (p.x64) {  // NT_TIB64 and the x64 TEB
    Wr64(teb + 0x08, t->stack_hi);  // StackBase
    Wr64(teb + 0x10, t->stack_lo);  // StackLimit
    Wr64(teb + 0x30, teb);          // Self
    Wr64(teb + 0x40, kGuestPid);    // ClientId
    Wr64(teb + 0x48, t->tid);
    Wr64(teb + 0x58, tls_array);    // ThreadLocalStoragePointer
    Wr64(teb + 0x60, kPebVa64);
  } else {
    Wr32(teb + 0x00, 0xFFFFFFFF);  // SEH chain: empty
    Wr32(teb + 0x04, uint32_t(t->stack_hi));
    Wr32(teb + 0x08, uint32_t(t->stack_lo));
    Wr32(teb + 0x18, uint32_t(teb));  // Self
    Wr32(teb + 0x20, kGuestPid);
    Wr32(teb + 0x24, t->tid);
    Wr32(teb + 0x2C, uint32_t(tls_array));
    Wr32(teb + 0x30, kPebVa);
  }
  auto block = [&](const PeImage& img, int index) {
    const PeTls& tls = img.tls;
    uint64_t raw = tls.raw_end - tls.raw_start;
    gaddr b = HeapAllocProcess(raw + tls.zero_fill + 16, true);
    std::memcpy(GPtr(b), GPtr(tls.raw_start), raw);
    WrPtr(tls_array + ptr * uint32_t(index), b);
  };
  if (p.img.has_tls) block(p.img, 0);
  // Lifted DLLs with static TLS (x64 guests, as on Vista and later): every
  // thread has their blocks, loaded or not, since all are mapped at start.
  for (const Module& m : p.modules)
    if (m.tls_index >= 0) block(m.img, m.tls_index);
}

// Static TLS callbacks are not run (no supported module has any).
static void CheckNoTlsCallbacks(const PeImage& img, const std::string& name) {
  if (img.has_tls && img.tls.callbacks && RdPtr(img.tls.callbacks))
    FatalF("%s has TLS callbacks: not supported", name.c_str());
}

Thread* CreateGuestThread(uint64_t stack_size) {
  auto t = std::make_unique<Thread>();
  Process& p = *g_proc;

  stack_size = std::max<uint64_t>(stack_size, 0x100000);
  gaddr stack = p.vm.Reserve(0, stack_size);
  if (!stack) FatalF("cannot reserve a %llu byte guest stack", (unsigned long long)stack_size);
  p.vm.Commit(stack, stack_size, Prot::RW);
  t->stack_lo = stack;
  t->stack_hi = stack + stack_size;

  const uint32_t stride = p.x64 ? kTebStride64 : kTebStride;
  {
    std::lock_guard<std::mutex> l(g_threads_mu);
    if (!g_next_teb) g_next_teb = p.x64 ? kTebTop64 : kTebTop;
    g_next_teb -= stride;
    if (p.x64 && g_next_teb < kTebLo64) FatalF("too many guest threads");
    t->teb = g_next_teb;
  }
  t->tid = p.next_tid.fetch_add(4);
  p.mem.Commit(t->teb, stride, Prot::RW);
  InitTeb(t.get());

  Ctx& c = t->ctx;
  std::memset(&c, 0, sizeof c);
  c.eflags = 0x202;
  if (p.x64) {
    c.rsp = t->stack_hi - 16;
    c.gs_base = t->teb;
  } else {
    c.esp = uint32_t(t->stack_hi - 16);
    c.fs_base = uint32_t(t->teb);
  }
  c.thread = t.get();
  x87::Init(c.fpu);
  x87::SetControlWord(c.fpu, 0x027F);  // Windows default for new threads
  c.mxcsr = sse::kMxcsrDefault;

  Thread* raw_t = t.get();
  std::lock_guard<std::mutex> l(g_threads_mu);
  g_threads[raw_t->tid] = std::move(t);
  return raw_t;
}

// The x87 model's host-double paths need the default floating-point
// environment on every thread that runs guest code. Nothing here changes
// it; this catches a host library that does.
static void CheckHostFp() {
  if (const char* why = platform::HostFpProblem())
    FatalF("host floating-point environment: %s", why);
}

// Runs guest function `entry` (with no argument, as the process entry
// point, or with `param`) on the calling host thread as guest thread t.
// Returns its exit code.
static uint32_t RunGuestThread(Thread* t, gaddr entry, const uint64_t* param,
                               bool main_thread = false) {
  // Thread start is a full barrier on Windows. pthread_create already
  // makes the creator's writes visible here; the fence states it for
  // weakly ordered hosts like every other guest sync point
  // (kernel32_state.h). The exit side fences in OnGuestThreadFinished.
  GuestSyncFence();
  t_current = t;
  CheckHostFp();
  InstallSignalStack();
  RegisterGuestHostThread("guest", t->tid);
  uint32_t r;
  try {
    if (main_thread && g_proc->game.before_entry) g_proc->game.before_entry(t->ctx);
    r = uint32_t(param ? CallGuest(t->ctx, entry, {*param}) : CallGuest(t->ctx, entry, {}));
  } catch (const GuestThreadExit& e) {
    r = e.code;
  }
  OnGuestThreadFinished(t->tid, r);
  return r;
}

// Starts fn(arg) on a new detached host thread with the big guest stack.
static void SpawnHostThread(void* (*fn)(void*), void* arg) {
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, kHostStack);
  pthread_t th;
  if (pthread_create(&th, &attr, fn, arg) != 0) FatalF("pthread_create failed");
  pthread_attr_destroy(&attr);
  pthread_detach(th);
}

struct ThreadStart {
  Thread* t;
  gaddr entry;
  uint64_t param;
};

static void* GuestThreadMain(void* arg) {
  std::unique_ptr<ThreadStart> s(static_cast<ThreadStart*>(arg));
  RunGuestThread(s->t, s->entry, &s->param);
  return nullptr;
}

void StartHostThread(Thread* t, gaddr entry, uint64_t param) {
  SpawnHostThread(GuestThreadMain, new ThreadStart{t, entry, param});
}

// A host-driven thread that may call into guest code (Miles' service
// thread delivering callbacks): bound to guest thread t, with the same big
// host stack and signal stack as other guest threads. Outside CallGuest it
// counts as HLE code, so a host fault there is a runtime bug.
struct ServiceStart {
  Thread* t;
  std::function<void()> fn;
};

static void* ServiceThreadMain(void* arg) {
  std::unique_ptr<ServiceStart> s(static_cast<ServiceStart*>(arg));
  t_current = s->t;
  CheckHostFp();
  InstallSignalStack();
  RegisterGuestHostThread("service", s->t->tid);
  hle::t_hle_depth = 1;
  s->fn();
  return nullptr;
}

void StartServiceThread(Thread* t, std::function<void()> fn) {
  SpawnHostThread(ServiceThreadMain, new ServiceStart{t, std::move(fn)});
}

// The guest main thread runs on the process main thread (SDL, and Cocoa on
// macOS, require UI calls there) but on a big private stack.
namespace {
Thread* g_main_thread = nullptr;
gaddr g_main_entry = 0;
uint32_t g_main_exit = 0;

void GuestMainTrampoline() {
  g_main_exit = RunGuestThread(g_main_thread, g_main_entry, nullptr, true);
}
}  // namespace

static uint32_t RunOnMainThread(Thread* t, gaddr entry) {
  void* stack = mmap(nullptr, kHostStack, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (stack == MAP_FAILED) FatalF("cannot allocate the main guest stack");
  g_main_thread = t;
  g_main_entry = entry;
  platform::RunOnStack(stack, kHostStack, GuestMainTrampoline);
  munmap(stack, kHostStack);
  return g_main_exit;
}

const PeImage* ImageAt(gaddr va) {
  const Process& p = *g_proc;
  if (va - p.img.image_base < p.img.image_size) return &p.img;
  for (const Module& m : p.modules)
    if (va - m.img.image_base < m.img.image_size) return &m.img;
  return nullptr;
}

void MapModule(Module& m) {
  Process& p = *g_proc;
  if (m.img.image_size) p.mem.Decommit(m.img.image_base, m.img.image_size);
  std::string err;
  if (!m.img.Load(m.host_path, p.mem, &err, m.base, hle::ImportThunk))
    FatalF("lifted DLL %s: %s", m.name.c_str(), err.c_str());
  // Windows XP did not set up static TLS for LoadLibrary'd DLLs either,
  // so 32-bit DLLs of that era have none. x64 ones get a slot of their own
  // after the EXE's (slot 0), kept across remaps.
  if (m.img.has_tls) {
    if (!p.x64)
      FatalF("lifted DLL %s has static TLS (__declspec(thread)): not supported",
             m.name.c_str());
    CheckNoTlsCallbacks(m.img, m.name);
    if (m.tls_index < 0) {
      int next = 1;
      for (const Module& o : p.modules) next = std::max(next, o.tls_index + 1);
      m.tls_index = next;
    }
    Wr32(m.img.tls.index_va, uint32_t(m.tls_index));
  }
  m.stale = false;
}

int RunProcess(const GameConfig& game, const std::string& exe_path,
               const std::string& game_dir, const std::string& game_args) {
  g_proc = new Process;
  Process& p = *g_proc;
  p.game = game;
  p.game_dir = game_dir;
  // How frames are shown: before a window sizes itself for a display mode.
  const GameConfig::SidePanel& sp = game.side_panel;
  gfx::SetSidePanel({sp.width, sp.scale, sp.active, sp.bar_height, sp.bar_length, sp.bar_scale});
  gfx::SetCropBottom(game.crop_bottom, game.crop_active);
  // <ID>_TRACE=1: every import call is logged (hle.cpp, CallThunk).
  const char* trace = Env("TRACE");
  g_trace = trace && trace[0] && trace[0] != '0';
  const char* runs = Env("X87_RUNS");
  x87::run::g_enabled = !(runs && runs[0] == '0');
  const char* doubles = Env("X87_DOUBLE");
  x87::run::g_doubles = !(doubles && doubles[0] == '0');
  p.game_args = game_args;
  StartWatchdog();
  p.x64 = PeImage::IsX64File(exe_path);
  if (!p.mem.Reserve(p.x64 ? kArenaBits64 : 32)) FatalF("cannot reserve the guest arena");
  g_guest_base = p.mem.base();
  // Calls to import thunks go to HLE handlers (Dispatch); nothing runs
  // there, but the IAT values point at committed memory as on Windows.
  const gaddr thunks = p.x64 ? kThunkBase64 : kThunkBase;
  if (!p.mem.Commit(thunks, kThunkRegionSize, Prot::R))
    FatalF("cannot commit the thunk region");
  // Its upper half holds the HLE's data exports, which are variables.
  p.mem.Protect(thunks + hle::kDataExportOffset, kThunkRegionSize - hle::kDataExportOffset,
                Prot::RW);
  std::string err;
  if (!p.img.Load(exe_path, p.mem, &err, 0, hle::ImportThunk))
    FatalF("%s", err.c_str());
  for (const auto& d : game.lifted_dlls) {
    Module& m = p.modules.emplace_back();
    m.name = hle::NormaliseDll(d.name);
    m.host_path = HostPath(GuestGameDir() + "\\" + d.name);
    m.base = d.base;
    MapModule(m);
  }

  if (p.x64) {
    // Everything above 4 GiB, below the TEBs (runtime.h).
    p.vm.Init(&p.mem, kUserLo64, kTebLo64);
  } else {
    p.vm.Init(&p.mem, 0x00010000, kThunkBase);
    p.vm.MarkReserved(hle::kModuleRegion, hle::kModuleRegionSize);
  }
  p.vm.MarkReserved(p.img.image_base, p.img.image_size);
  for (const Module& m : p.modules) p.vm.MarkReserved(m.img.image_base, m.img.image_size);

  // PEB: only the fields Win32 code commonly peeks at. x64 guests see
  // Windows 10 (22H2), 32-bit ones Windows XP.
  if (p.x64) {
    p.mem.Commit(kPebVa64, 0x1000, Prot::RW);
    Wr64(kPebVa64 + 0x10, p.img.image_base);
    Wr32(kPebVa64 + 0xB8, GuestProcessorCount());  // NumberOfProcessors
    Wr32(kPebVa64 + 0x118, 10);     // OSMajorVersion
    Wr32(kPebVa64 + 0x11C, 0);      // OSMinorVersion
    Wr16(kPebVa64 + 0x120, 19045);  // OSBuildNumber
    Wr32(kPebVa64 + 0x124, 2);      // OSPlatformId: VER_PLATFORM_WIN32_NT
  } else {
    p.mem.Commit(kPebVa, 0x1000, Prot::RW);
    Wr32(kPebVa + 0x08, uint32_t(p.img.image_base));
    Wr32(kPebVa + 0x64, GuestProcessorCount());  // NumberOfProcessors
    Wr32(kPebVa + 0xA4, 5);      // OSMajorVersion (Windows XP)
    Wr32(kPebVa + 0xA8, 1);      // OSMinorVersion
    Wr16(kPebVa + 0xAC, 2600);   // OSBuildNumber
    Wr32(kPebVa + 0xB0, 2);      // OSPlatformId: VER_PLATFORM_WIN32_NT
  }

  // The image's TLS index: slot 0 (lifted DLLs have theirs, MapModule).
  if (p.img.has_tls) {
    CheckNoTlsCallbacks(p.img, game.exe_name);
    Wr32(p.img.tls.index_va, 0);
  }

  InitDispatch();
  InitKernel32State();

  Thread* main = CreateGuestThread(p.img.stack_reserve);
  return int(RunOnMainThread(main, p.img.entry));
}

}  // namespace recomp
