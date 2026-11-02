// MSVCP140.dll (the MSVC C++ standard library) for x64 guests, as HLE:
// thread and synchronisation primitives here; locale and iostreams as the
// guest needs them. Objects the guest constructs keep MSVC's layouts (the
// guest's inline code reads their fields). Layouts follow microsoft/STL
// (reference only). The import names are those of the DLL's older ABI:
// _Thrd_sleep and _Cnd_timedwait take an absolute xtime {__time64_t sec;
// long nsec;}.

#include <sched.h>

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "msvcp140.dll";

// _Thrd_result.
constexpr uint32_t kSuccess = 0, kTimedout = 2, kBusy = 3, kError = 4;

// An absolute xtime (system clock).
std::chrono::system_clock::time_point XtimeAt(gaddr xt) {
  using namespace std::chrono;
  int64_t sec = int64_t(Rd64(xt));
  int32_t nsec = int32_t(Rd32(xt + 8));
  return system_clock::time_point(duration_cast<system_clock::duration>(
      seconds(sec) + nanoseconds(nsec)));
}

// Host state behind a guest object, keyed by its address (in-situ objects
// are guest memory; their contents stay MSVC's).
template <typename T>
T& StateAt(gaddr va) {
  static std::mutex mu;
  static std::map<gaddr, std::unique_ptr<T>> objs;
  std::lock_guard<std::mutex> l(mu);
  auto& o = objs[va];
  if (!o) o = std::make_unique<T>();
  return *o;
}

// ---- mutexes -----------------------------------------------------------------
// _Mtx_internal_imp_t (x64, 80 bytes): int _Type; the critical section
// storage (64 bytes, pointer-aligned, at +8); long _Thread_id (+0x48);
// int _Count (+0x4C). The guest's fields are kept up to date for code
// that reads them.

constexpr uint32_t kMtxRecursive = 0x100;

struct Mtx {
  std::mutex m;
  std::condition_variable cv;
  uint32_t owner = 0;  // guest thread id, 0: free
  int count = 0;
};

void Publish(gaddr mtx, const Mtx& s) {
  Wr32(mtx + 0x48, s.owner ? s.owner : 0xFFFFFFFF);
  Wr32(mtx + 0x4C, uint32_t(s.count));
}

// Takes the mutex `count` times (a condition variable's wait restores the
// count it released).
void Lock(gaddr mtx, int count = 1) {
  Mtx& s = StateAt<Mtx>(mtx);
  uint32_t tid = CurrentThread()->tid;
  std::unique_lock<std::mutex> l(s.m);
  if (s.owner == tid) {
    if (!(Rd32(mtx) & kMtxRecursive))
      FatalF("std::mutex at %08llX locked twice by its owner (resource deadlock)",
             (unsigned long long)mtx);
    s.count += count;
  } else {
    s.cv.wait(l, [&] { return s.owner == 0; });
    s.owner = tid;
    s.count = count;
  }
  Publish(mtx, s);
}

// Releases it entirely; returns the count it had.
int UnlockAll(gaddr mtx) {
  Mtx& s = StateAt<Mtx>(mtx);
  int n;
  {
    std::lock_guard<std::mutex> l(s.m);
    n = s.count;
    s.count = 0;
    s.owner = 0;
    Publish(mtx, s);
  }
  s.cv.notify_one();
  return n;
}

}  // namespace

HLE64(kDll, _Mtx_init_in_situ, kCdecl) {
  gaddr mtx = Arg(c, 0);
  std::memset(GPtr(mtx), 0, 80);
  Wr32(mtx, uint32_t(Arg(c, 1)));
  Wr32(mtx + 0x48, 0xFFFFFFFF);
  return 0;
}
HLE64(kDll, _Mtx_destroy_in_situ, kCdecl) { return 0; }

HLE64(kDll, _Mtx_lock, kCdecl) {
  GuestSyncPoint sync;
  Lock(Arg(c, 0));
  return kSuccess;
}

HLE64(kDll, _Mtx_trylock, kCdecl) {
  GuestSyncPoint sync;
  gaddr mtx = Arg(c, 0);
  Mtx& s = StateAt<Mtx>(mtx);
  uint32_t tid = CurrentThread()->tid;
  std::lock_guard<std::mutex> l(s.m);
  if (s.owner && !(s.owner == tid && (Rd32(mtx) & kMtxRecursive))) return kBusy;
  s.owner = tid;
  s.count++;
  Publish(mtx, s);
  return kSuccess;
}

HLE64(kDll, _Mtx_unlock, kCdecl) {
  GuestSyncPoint sync;
  gaddr mtx = Arg(c, 0);
  Mtx& s = StateAt<Mtx>(mtx);
  bool freed;
  {
    std::lock_guard<std::mutex> l(s.m);
    if (s.owner != CurrentThread()->tid)
      FatalF("std::mutex at %08llX unlocked by a thread that does not own it",
             (unsigned long long)mtx);
    freed = --s.count == 0;
    if (freed) s.owner = 0;
    Publish(mtx, s);
  }
  if (freed) s.cv.notify_one();
  return kSuccess;
}

HLE64(kDll, _Mtx_current_owns, kCdecl) {
  Mtx& s = StateAt<Mtx>(Arg(c, 0));
  std::lock_guard<std::mutex> l(s.m);
  return s.count > 0 && s.owner == CurrentThread()->tid;
}

// ---- condition variables -------------------------------------------------------
// _Cnd_internal_imp_t: 72 bytes of storage, untouched here. A Wake right
// after the mutex is released still finds the waiter counted (the Cnd's
// own lock is held across the release), and waits may wake spuriously, as
// the standard allows.

namespace {
struct Cnd {
  std::mutex m;
  std::condition_variable cv;
  uint32_t waiting = 0, released = 0;
};

// Returns false on timeout.
bool CndWait(gaddr cnd, gaddr mtx, const std::chrono::system_clock::time_point* until) {
  Cnd& v = StateAt<Cnd>(cnd);
  int count;
  bool woken = true;
  {
    std::unique_lock<std::mutex> l(v.m);
    v.waiting++;
    count = UnlockAll(mtx);
    auto pred = [&] { return v.released > 0; };
    if (until)
      woken = v.cv.wait_until(l, *until, pred);
    else
      v.cv.wait(l, pred);
    v.waiting--;
    if (woken) v.released--;
  }
  Lock(mtx, count);
  return woken;
}
}  // namespace

HLE64(kDll, _Cnd_init_in_situ, kCdecl) {
  std::memset(GPtr(Arg(c, 0)), 0, 72);
  return 0;
}
HLE64(kDll, _Cnd_destroy_in_situ, kCdecl) { return 0; }

HLE64(kDll, _Cnd_wait, kCdecl) {
  GuestSyncPoint sync;
  CndWait(Arg(c, 0), Arg(c, 1), nullptr);
  return kSuccess;
}

HLE64(kDll, _Cnd_timedwait, kCdecl) {
  GuestSyncPoint sync;
  auto until = XtimeAt(Arg(c, 2));
  return CndWait(Arg(c, 0), Arg(c, 1), &until) ? kSuccess : kTimedout;
}

HLE64(kDll, _Cnd_signal, kCdecl) {
  GuestSyncPoint sync;
  Cnd& v = StateAt<Cnd>(Arg(c, 0));
  {
    std::lock_guard<std::mutex> l(v.m);
    if (v.released < v.waiting) v.released++;
  }
  v.cv.notify_one();
  return kSuccess;
}

HLE64(kDll, _Cnd_broadcast, kCdecl) {
  GuestSyncPoint sync;
  Cnd& v = StateAt<Cnd>(Arg(c, 0));
  {
    std::lock_guard<std::mutex> l(v.m);
    v.released = v.waiting;
  }
  v.cv.notify_all();
  return kSuccess;
}

// notify_all_at_thread_exit registrations: none are made
// (_Cnd_register_at_thread_exit is not provided), so nothing to broadcast.
HLE64(kDll, _Cnd_do_broadcast_at_thread_exit, kCdecl) { return 0; }

// ---- threads ---------------------------------------------------------------------

HLE64(kDll, _Thrd_id, kCdecl) { return CurrentThread()->tid; }
HLE64(kDll, _Thrd_hardware_concurrency, kCdecl) { return GuestProcessorCount(); }
HLE64(kDll, _Thrd_yield, kCdecl) {
  GuestSyncPoint sync;
  sched_yield();
  return 0;
}
// _Thrd_join(_Thrd_t, int* result) and _Thrd_detach(_Thrd_t): the 16-byte
// _Thrd_t {HANDLE _Hnd; _Thrd_id_t _Id;} comes by reference (Win64 passes
// larger aggregates as a pointer to a copy). Both close the handle.
HLE64(kDll, _Thrd_join, kCdecl) {
  uint32_t h = uint32_t(Rd64(Arg(c, 0))), code;
  if (!JoinThread(h, &code)) return kError;
  CloseHandleValue(h);
  if (Arg(c, 1)) Wr32(Arg(c, 1), code);
  return kSuccess;
}
HLE64(kDll, _Thrd_detach, kCdecl) {
  return CloseHandleValue(uint32_t(Rd64(Arg(c, 0)))) ? kSuccess : kError;
}
// Sleeps until the absolute xtime.
HLE64(kDll, _Thrd_sleep, kCdecl) {
  GuestSyncPoint sync;
  std::this_thread::sleep_until(XtimeAt(Arg(c, 0)));
  return 0;
}

// ---- _Lockit: the library's global locks, one recursive lock per kind ---------
// _Lockit {int _Locktype;}.

namespace {
std::recursive_mutex& GlobalLock(uint32_t kind) {
  static std::recursive_mutex locks[8];
  return locks[kind & 7];
}
}  // namespace

HLE_NAMED_X(kDll, "??0_Lockit@std@@QEAA@H@Z", LockitCtor, kCdecl, true) {
  GuestSyncPoint sync;
  uint32_t kind = uint32_t(Arg(c, 1));
  Wr32(Arg(c, 0), kind);
  GlobalLock(kind).lock();
  return Arg(c, 0);
}
HLE_NAMED_X(kDll, "??1_Lockit@std@@QEAA@XZ", LockitDtor, kCdecl, true) {
  GuestSyncPoint sync;
  GlobalLock(Rd32(Arg(c, 0))).unlock();
  return 0;
}

// ---- clocks and randomness -------------------------------------------------------

HLE64(kDll, _Query_perf_counter, kCdecl) { return PerfCounter(); }
HLE64(kDll, _Query_perf_frequency, kCdecl) { return 10000000; }
// 100 ns ticks since 1970 (the system clock).
HLE64(kDll, _Xtime_get_ticks, kCdecl) {
  using namespace std::chrono;
  return uint64_t(duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() /
                  100);
}
HLE_NAMED_X(kDll, "?_Random_device@std@@YAIXZ", RandomDevice, kCdecl, true) {
  static std::random_device rd;
  static std::mutex mu;
  std::lock_guard<std::mutex> l(mu);
  return uint32_t(rd());
}

// ---- errors ----------------------------------------------------------------------
// These throw std:: exceptions; until guest C++ exceptions are raised from
// the HLE (ARCHITECTURE.md, "64-bit guests"), they stop with the message.

HLE_NAMED_X(kDll, "?_Xbad_alloc@std@@YAXXZ", XbadAlloc, kCdecl, true) {
  FatalF("guest C++: std::bad_alloc");
}
HLE_NAMED_X(kDll, "?_Xbad_function_call@std@@YAXXZ", XbadFunctionCall, kCdecl, true) {
  FatalF("guest C++: std::bad_function_call");
}
HLE_NAMED_X(kDll, "?_Xinvalid_argument@std@@YAXPEBD@Z", XinvalidArgument, kCdecl, true) {
  FatalF("guest C++: std::invalid_argument(%s)", GStr(Arg(c, 0)).c_str());
}
HLE_NAMED_X(kDll, "?_Xlength_error@std@@YAXPEBD@Z", XlengthError, kCdecl, true) {
  FatalF("guest C++: std::length_error(%s)", GStr(Arg(c, 0)).c_str());
}
HLE_NAMED_X(kDll, "?_Xout_of_range@std@@YAXPEBD@Z", XoutOfRange, kCdecl, true) {
  FatalF("guest C++: std::out_of_range(%s)", GStr(Arg(c, 0)).c_str());
}
HLE_NAMED_X(kDll, "?_Throw_C_error@std@@YAXH@Z", ThrowCError, kCdecl, true) {
  FatalF("guest C++: std::system_error from C error %d", int32_t(Arg(c, 0)));
}
HLE_NAMED_X(kDll, "?_Throw_Cpp_error@std@@YAXH@Z", ThrowCppError, kCdecl, true) {
  FatalF("guest C++: std::system_error (code %d)", int32_t(Arg(c, 0)));
}
// No guest exception is ever in flight yet.
HLE_NAMED_X(kDll, "?uncaught_exception@std@@YA_NXZ", UncaughtException, kCdecl, true) {
  return 0;
}

}  // namespace recomp::hle
