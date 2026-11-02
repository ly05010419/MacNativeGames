// The Universal CRT (ucrtbase.dll, imported through its api-ms-win-crt-*
// API sets) for x64 guests: the parts the statically linked startup code
// and the game call. Its process-wide data (argv, the environment, the
// onexit tables' arrays, errno) lives in guest memory, where the guest
// reads it through the pointers these functions return.

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/ucrt.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

// The program name: up to the closing quote or the first blank, no escapes.
const char* SkipProgramName(const char* p) {
  if (*p == '"') {
    p++;
    while (*p && *p != '"') p++;
    if (*p) p++;
  } else {
    while (*p && *p != ' ' && *p != '\t') p++;
  }
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

// Guest copies of a list of strings: a null-terminated pointer array.
gaddr GuestStrings(const std::vector<std::string>& v) {
  gaddr arr = HeapAllocProcess(8 * (v.size() + 1), true);
  for (size_t i = 0; i < v.size(); i++) {
    gaddr s = HeapAllocProcess(v[i].size() + 1, false);
    PutStr(s, v[i]);
    Wr64(arr + 8 * i, s);
  }
  return arr;
}

// The CRT's process-wide variables, in guest memory (their addresses are
// what __p___argc and friends return).
struct Globals {
  gaddr argc, argv, environ, fmode, commode, app_type;
};
Globals& G() {
  static Globals g = [] {
    Globals r;
    gaddr b = HeapAllocProcess(0x40, true);
    r.argc = b;            // int
    r.argv = b + 0x08;     // char**
    r.environ = b + 0x10;  // char**
    r.fmode = b + 0x18;    // int
    r.commode = b + 0x1C;  // int
    r.app_type = b + 0x20;  // int
    return r;
  }();
  return g;
}

// _onexit_table_t: {first, last, end} function pointer array, which the
// guest's own startup code may also walk.
std::mutex g_onexit_mu;

void RegisterOnExit(gaddr table, gaddr fn) {
  std::lock_guard<std::mutex> l(g_onexit_mu);
  gaddr first = Rd64(table), last = Rd64(table + 8), end = Rd64(table + 16);
  if (last == end) {
    uint64_t n = (end - first) / 8, cap = n ? 2 * n : 32;
    gaddr grown = HeapAllocProcess(8 * cap, true);
    if (n) std::memcpy(GPtr(grown), GPtr(first), 8 * n);
    if (first) HeapFreeProcess(first);
    first = grown;
    last = grown + 8 * n;
    end = grown + 8 * cap;
  }
  Wr64(last, fn);
  Wr64(table, first);
  Wr64(table + 8, last + 8);
  Wr64(table + 16, end);
}

// Runs the table's functions last to first and empties it. A function may
// register more while this runs; those run too.
void ExecuteOnExit(Ctx& c, gaddr table) {
  for (;;) {
    gaddr fn;
    {
      std::lock_guard<std::mutex> l(g_onexit_mu);
      gaddr first = Rd64(table), last = Rd64(table + 8);
      if (!first || last == first) break;
      last -= 8;
      fn = Rd64(last);
      Wr64(table + 8, last);
    }
    if (fn) CallGuest(c, fn, {});
  }
  std::lock_guard<std::mutex> l(g_onexit_mu);
  if (gaddr first = Rd64(table)) HeapFreeProcess(first);
  for (int i = 0; i < 3; i++) Wr64(table + 8 * i, 0);
}

// The CRT's own table (_crt_atexit, atexit): run by exit and _cexit.
gaddr AtExitTable() {
  static gaddr t = HeapAllocProcess(24, true);
  return t;
}

[[noreturn]] void ExitNow(uint32_t code) {
  std::fflush(stdout);
  std::fprintf(stderr, "exit(%d)\n", int32_t(code));
  std::fflush(stderr);
  std::_Exit(int(code));
}

// The calling thread's errno, in guest memory.
gaddr ErrnoSlot() {
  static std::mutex mu;
  static std::unordered_map<uint32_t, gaddr> slots;
  std::lock_guard<std::mutex> l(mu);
  gaddr& s = slots[CurrentThread()->tid];
  if (!s) s = HeapAllocProcess(8, true);
  return s;
}

void SetErrno(uint32_t e) { Wr32(ErrnoSlot(), e); }

}  // namespace

gaddr GuestErrno() { return ErrnoSlot(); }

// ---- startup and termination ---------------------------------------------

// _initterm(first, last): every non-null function pointer in [first, last).
HLE64("ucrtbase.dll", _initterm, kCdecl) {
  for (gaddr p = Arg(c, 0), end = Arg(c, 1); p < end; p += 8)
    if (gaddr fn = Rd64(p)) CallGuest(c, fn, {});
  return 0;
}

// _initterm_e: the same with int-returning functions; the first nonzero
// result stops it and is returned.
HLE64("ucrtbase.dll", _initterm_e, kCdecl) {
  for (gaddr p = Arg(c, 0), end = Arg(c, 1); p < end; p += 8)
    if (gaddr fn = Rd64(p))
      if (int32_t r = int32_t(CallGuest(c, fn, {}))) return uint32_t(r);
  return 0;
}

HLE64("ucrtbase.dll", _set_app_type, kCdecl) {
  Wr32(G().app_type, uint32_t(Arg(c, 0)));
  return 0;
}

HLE64("ucrtbase.dll", _configure_narrow_argv, kCdecl) {
  std::vector<std::string> args = win32::SplitCommandLine(GStr(g_proc->cmdline_a));
  Wr32(G().argc, uint32_t(args.size()));
  Wr64(G().argv, GuestStrings(args));
  return 0;
}

HLE64("ucrtbase.dll", __p___argc, kCdecl) { return G().argc; }
HLE64("ucrtbase.dll", __p___argv, kCdecl) { return G().argv; }

HLE64("ucrtbase.dll", _initialize_narrow_environment, kCdecl) {
  Wr64(G().environ, GuestStrings(EnvironmentVars()));
  return 0;
}
HLE64("ucrtbase.dll", _get_initial_narrow_environment, kCdecl) {
  return Rd64(G().environ);
}

// The command line after the program name (WinMain's lpCmdLine).
HLE64("ucrtbase.dll", _get_narrow_winmain_command_line, kCdecl) {
  const char* base = g_proc->mem.ptr<const char>(g_proc->cmdline_a);
  return g_proc->cmdline_a + gaddr(SkipProgramName(base) - base);
}

HLE64("ucrtbase.dll", _set_fmode, kCdecl) {
  Wr32(G().fmode, uint32_t(Arg(c, 0)));
  return 0;
}
HLE64("ucrtbase.dll", __p__commode, kCdecl) { return G().commode; }

// The guest's floating-point environment needs no CRT setup.
HLE64("ucrtbase.dll", __setusermatherr, kCdecl) { return 0; }

HLE64("ucrtbase.dll", _initialize_onexit_table, kCdecl) {
  for (int i = 0; i < 3; i++) Wr64(Arg(c, 0) + 8 * i, 0);
  return 0;
}
HLE64("ucrtbase.dll", _register_onexit_function, kCdecl) {
  RegisterOnExit(Arg(c, 0), Arg(c, 1));
  return 0;
}
HLE64("ucrtbase.dll", _execute_onexit_table, kCdecl) {
  ExecuteOnExit(c, Arg(c, 0));
  return 0;
}
HLE64("ucrtbase.dll", _crt_atexit, kCdecl) {
  RegisterOnExit(AtExitTable(), Arg(c, 0));
  return 0;
}

// The thread_local destructor callback of the EXE: kept, never called
// (guest threads do not run TLS destructors here).
HLE64("ucrtbase.dll", _register_thread_local_exe_atexit_callback, kCdecl) { return 0; }

HLE64("ucrtbase.dll", exit, kCdecl) {
  uint32_t code = uint32_t(Arg(c, 0));
  ExecuteOnExit(c, AtExitTable());
  ExitNow(code);
}
HLE64("ucrtbase.dll", _exit, kCdecl) { ExitNow(uint32_t(Arg(c, 0))); }
HLE64("ucrtbase.dll", _cexit, kCdecl) {
  ExecuteOnExit(c, AtExitTable());
  return 0;
}
HLE64("ucrtbase.dll", _c_exit, kCdecl) { return 0; }

HLE64("ucrtbase.dll", terminate, kCdecl) { FatalF("guest called terminate()"); }
HLE64("ucrtbase.dll", _invalid_parameter_noinfo_noreturn, kCdecl) {
  FatalF("guest CRT: invalid parameter (_invalid_parameter_noinfo_noreturn)");
}

// The startup code's __except filters: no exception is handled there.
HLE64("ucrtbase.dll", _seh_filter_exe, kCdecl) { return 0; }  // CONTINUE_SEARCH
HLE64("ucrtbase.dll", _seh_filter_dll, kCdecl) { return 0; }

// errno: an int per guest thread.
HLE64("ucrtbase.dll", _errno, kCdecl) { return ErrnoSlot(); }

// ---- threads ----------------------------------------------------------------

// _beginthreadex(security, stack, start, arg, flags, &tid): a thread whose
// exit code is start's result; the handle is the caller's to close.
// Flags: CREATE_SUSPENDED, STACK_SIZE_PARAM_IS_A_RESERVATION.
HLE64("ucrtbase.dll", _beginthreadex, kCdecl) {
  uint32_t flags = uint32_t(Arg(c, 4));
  if (flags & ~0x10004u) FatalF("_beginthreadex: flags %X not supported", flags);
  uint32_t tid;
  uint32_t h = NewThread(uint32_t(Arg(c, 1)), Arg(c, 2), Arg(c, 3), flags & 4, &tid);
  if (Arg(c, 5)) Wr32(Arg(c, 5), tid);
  return HandleRet(h);
}

// ---- heap ----------------------------------------------------------------
// The UCRT allocates from the process heap since Visual Studio 2015.

namespace {
gaddr Malloc(uint64_t n) { return HeapAllocProcess(n ? n : 1, false); }
}  // namespace

HLE64("ucrtbase.dll", malloc, kCdecl) { return Malloc(Arg(c, 0)); }
HLE64("ucrtbase.dll", free, kCdecl) {
  if (gaddr p = Arg(c, 0)) HeapFreeProcess(p);
  return 0;
}
HLE64("ucrtbase.dll", calloc, kCdecl) {
  uint64_t n = Arg(c, 0), size = Arg(c, 1);
  if (size && n > UINT64_MAX / size) return 0;
  return HeapAllocProcess(n * size ? n * size : 1, true);
}
HLE64("ucrtbase.dll", realloc, kCdecl) {
  gaddr p = Arg(c, 0);
  uint64_t n = Arg(c, 1);
  if (!p) return Malloc(n);
  if (!n) {
    HeapFreeProcess(p);
    return 0;
  }
  return HeapReAllocProcess(p, n);
}
// _aligned_malloc: the block's own address is kept in the 8 bytes below the
// aligned pointer, as the UCRT does.
HLE64("ucrtbase.dll", _aligned_malloc, kCdecl) {
  uint64_t n = Arg(c, 0), align = std::max<uint64_t>(Arg(c, 1), 8);
  if (align & (align - 1)) return 0;
  gaddr block = Malloc(n + align + 8);
  if (!block) return 0;
  gaddr p = (block + 8 + align - 1) & ~(align - 1);
  Wr64(p - 8, block);
  return p;
}
HLE64("ucrtbase.dll", _aligned_free, kCdecl) {
  if (gaddr p = Arg(c, 0)) HeapFreeProcess(Rd64(p - 8));
  return 0;
}
// No new handler is ever installed (_set_new_handler is not provided).
HLE64("ucrtbase.dll", _callnewh, kCdecl) { return 0; }
HLE64("ucrtbase.dll", _set_new_mode, kCdecl) {
  static int mode = 0;
  int old = mode;
  mode = int(Arg(c, 0));
  return uint32_t(old);
}

// ---- locale (only the "C" locale) ----------------------------------------

HLE64("ucrtbase.dll", _configthreadlocale, kCdecl) {
  // _ENABLE_PER_THREAD_LOCALE 1, _DISABLE_PER_THREAD_LOCALE 2; 0 queries,
  // -1 resets. Nothing differs between them with one locale.
  static std::mutex mu;
  static int mode = 2;
  std::lock_guard<std::mutex> l(mu);
  int old = mode, t = int(Arg(c, 0));
  if (t == 1 || t == 2) mode = t;
  if (t == -1) mode = 2;
  return uint32_t(old);
}

// localeconv: the "C" locale's struct lconv (x64 layout: ten char*, eight
// chars, six wchar_t*).
HLE64("ucrtbase.dll", localeconv, kCdecl) {
  static gaddr lc = [] {
    gaddr p = HeapAllocProcess(136, true);
    gaddr dot = HeapAllocProcess(4, true), empty = dot + 2;
    gaddr wdot = HeapAllocProcess(8, true), wempty = wdot + 4;
    PutStr(dot, ".");
    Wr16(wdot, '.');
    for (int i = 0; i < 10; i++) Wr64(p + 8 * i, i == 0 ? dot : empty);
    for (int i = 0; i < 8; i++) Wr8(p + 80 + i, CHAR_MAX);
    for (int i = 0; i < 6; i++) Wr64(p + 88 + 8 * i, i == 0 ? wdot : wempty);
    return p;
  }();
  return lc;
}

// ---- character classes and strings ("C" locale) --------------------------

namespace {
int CArg(const Ctx& c, int i) { return int32_t(Arg(c, i)); }
bool InC(int ch) { return ch >= 0 && ch < 128; }  // the classes are ASCII's
}  // namespace

HLE64("ucrtbase.dll", iscntrl, kCdecl) { return InC(CArg(c, 0)) && std::iscntrl(CArg(c, 0)); }
HLE64("ucrtbase.dll", isdigit, kCdecl) { return InC(CArg(c, 0)) && std::isdigit(CArg(c, 0)); }
HLE64("ucrtbase.dll", isspace, kCdecl) { return InC(CArg(c, 0)) && std::isspace(CArg(c, 0)); }
HLE64("ucrtbase.dll", isupper, kCdecl) { return InC(CArg(c, 0)) && std::isupper(CArg(c, 0)); }
HLE64("ucrtbase.dll", isxdigit, kCdecl) { return InC(CArg(c, 0)) && std::isxdigit(CArg(c, 0)); }
HLE64("ucrtbase.dll", tolower, kCdecl) {
  int ch = CArg(c, 0);
  return uint32_t(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
}
HLE64("ucrtbase.dll", toupper, kCdecl) {
  int ch = CArg(c, 0);
  return uint32_t(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
}

namespace {
const char* S(gaddr p) { return g_proc->mem.ptr<const char>(p); }
int Sign(int v) { return v < 0 ? -1 : v > 0 ? 1 : 0; }
int Ci(int ch) { return ch >= 'A' && ch <= 'Z' ? ch + 32 : ch; }  // _stricmp folds to lower
int StrICmp(const char* a, const char* b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    int x = Ci(uint8_t(a[i])), y = Ci(uint8_t(b[i]));
    if (x != y) return x - y;
    if (!x) break;
  }
  return 0;
}
}  // namespace

HLE64("ucrtbase.dll", strcmp, kCdecl) {
  return uint32_t(Sign(std::strcmp(S(Arg(c, 0)), S(Arg(c, 1)))));
}
HLE64("ucrtbase.dll", strncmp, kCdecl) {
  return uint32_t(Sign(std::strncmp(S(Arg(c, 0)), S(Arg(c, 1)), Arg(c, 2))));
}
HLE64("ucrtbase.dll", _stricmp, kCdecl) {
  return uint32_t(Sign(StrICmp(S(Arg(c, 0)), S(Arg(c, 1)), SIZE_MAX)));
}
HLE64("ucrtbase.dll", _strnicmp, kCdecl) {
  return uint32_t(Sign(StrICmp(S(Arg(c, 0)), S(Arg(c, 1)), Arg(c, 2))));
}
HLE64("ucrtbase.dll", strcspn, kCdecl) { return std::strcspn(S(Arg(c, 0)), S(Arg(c, 1))); }
HLE64("ucrtbase.dll", strncpy, kCdecl) {
  gaddr d = Arg(c, 0), src = Arg(c, 1);
  uint64_t n = Arg(c, 2), i = 0;
  for (; i < n && S(src)[i]; i++) Wr8(d + i, uint8_t(S(src)[i]));
  for (; i < n; i++) Wr8(d + i, 0);
  return d;
}
// strtok: the position is per thread, as in the UCRT.
HLE64("ucrtbase.dll", strtok, kCdecl) {
  static thread_local gaddr next = 0;
  gaddr s = Arg(c, 0) ? Arg(c, 0) : next;
  const char* delim = S(Arg(c, 1));
  if (!s) return 0;
  s += std::strspn(S(s), delim);
  if (!*S(s)) {
    next = 0;
    return 0;
  }
  gaddr e = s + std::strcspn(S(s), delim);
  if (*S(e)) {
    Wr8(e, 0);
    next = e + 1;
  } else {
    next = 0;
  }
  return s;
}
// wcscat_s: 0, or EINVAL / ERANGE (and an empty destination) as the UCRT.
HLE64("ucrtbase.dll", wcscat_s, kCdecl) {
  gaddr d = Arg(c, 0), src = Arg(c, 2);
  uint64_t size = Arg(c, 1);
  if (!d || !size) return EINVAL;
  if (!src) {
    Wr16(d, 0);
    return EINVAL;
  }
  uint64_t n = 0;
  while (n < size && Rd16(d + 2 * n)) n++;
  if (n == size) {
    Wr16(d, 0);
    return EINVAL;
  }
  for (uint64_t i = 0;; i++) {
    if (n + i >= size) {
      Wr16(d, 0);
      return ERANGE;
    }
    uint16_t ch = Rd16(src + 2 * i);
    Wr16(d + 2 * (n + i), ch);
    if (!ch) return 0;
  }
}

// ---- numeric conversion ("C" locale) -------------------------------------

namespace {

// strto*: the host's (the same grammar since Visual Studio 2015, hex floats,
// inf and nan included) on the guest string; *end is a guest pointer.
template <typename T, typename F>
T StrTo(gaddr s, gaddr end, F f) {
  const char* p = S(s);
  char* e = nullptr;
  errno = 0;
  T v = f(p, &e);
  if (errno == ERANGE) SetErrno(ERANGE);
  if (end) Wr64(end, s + gaddr(e - p));
  return v;
}
}  // namespace

HLE64("ucrtbase.dll", strtod, kCdecl) {
  return RetD(c, StrTo<double>(Arg(c, 0), Arg(c, 1),
                               [](const char* p, char** e) { return std::strtod(p, e); }));
}
HLE64("ucrtbase.dll", strtol, kCdecl) {
  int base = int(Arg(c, 2));
  return uint32_t(int32_t(StrTo<long>(Arg(c, 0), Arg(c, 1), [&](const char* p, char** e) {
    long long v = std::strtoll(p, e, base);
    if (v > INT32_MAX || v < INT32_MIN) {  // long is 32-bit in Win64
      errno = ERANGE;
      return long(v > 0 ? INT32_MAX : INT32_MIN);
    }
    return long(v);
  })));
}
HLE64("ucrtbase.dll", strtoul, kCdecl) {
  int base = int(Arg(c, 2));
  return uint32_t(StrTo<unsigned long>(Arg(c, 0), Arg(c, 1), [&](const char* p, char** e) {
    const char* q = p;
    while (std::isspace(uint8_t(*q))) q++;
    bool neg = *q == '-';
    unsigned long long v = std::strtoull(p, e, base);
    // 32-bit unsigned long: out of range unless the magnitude fits.
    unsigned long long mag = neg ? 0 - v : v;
    if (errno == ERANGE || mag > UINT32_MAX) {
      errno = ERANGE;
      return (unsigned long)UINT32_MAX;
    }
    return (unsigned long)(uint32_t(v));
  }));
}
HLE64("ucrtbase.dll", strtoull, kCdecl) {
  int base = int(Arg(c, 2));
  return StrTo<unsigned long long>(Arg(c, 0), Arg(c, 1), [&](const char* p, char** e) {
    return std::strtoull(p, e, base);
  });
}
HLE64("ucrtbase.dll", atof, kCdecl) { return RetD(c, std::strtod(S(Arg(c, 0)), nullptr)); }
HLE64("ucrtbase.dll", atoi, kCdecl) {
  long long v = std::strtoll(S(Arg(c, 0)), nullptr, 10);
  return uint32_t(int32_t(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v));
}
HLE64("ucrtbase.dll", atol, kCdecl) {
  long long v = std::strtoll(S(Arg(c, 0)), nullptr, 10);
  return uint32_t(int32_t(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v));
}
HLE64("ucrtbase.dll", atoll, kCdecl) { return uint64_t(std::strtoll(S(Arg(c, 0)), nullptr, 10)); }

// mbstowcs in the "C" locale: each byte is the wchar_t of the same value.
HLE64("ucrtbase.dll", mbstowcs, kCdecl) {
  gaddr d = Arg(c, 0);
  const char* src = S(Arg(c, 1));
  uint64_t n = Arg(c, 2), len = std::strlen(src);
  if (!d) return len;
  uint64_t i = 0;
  for (; i < n && i < len; i++) Wr16(d + 2 * i, uint8_t(src[i]));
  if (i < n) Wr16(d + 2 * i, 0);
  return i;
}

// ---- utility ---------------------------------------------------------------

// rand: the UCRT's per-thread linear congruential generator (seed 1).
HLE64("ucrtbase.dll", rand, kCdecl) {
  static thread_local uint32_t seed = 1;
  seed = seed * 214013u + 2531011u;
  return (seed >> 16) & 0x7FFF;
}

// qsort(base, n, width, compare): the MSVC CRT's algorithm, so elements
// that compare equal end up in the order they do on Windows: quicksort on
// the median of the first, middle and last elements, an explicit stack of
// the larger partitions, and a selection sort (largest to the end) for
// partitions of up to 8 elements.
HLE64("ucrtbase.dll", qsort, kCdecl) {
  gaddr base = Arg(c, 0), cmp = Arg(c, 3);
  uint64_t n = Arg(c, 1), w = Arg(c, 2);
  if (n < 2 || !w) return 0;
  auto compare = [&](gaddr a, gaddr b) { return int32_t(CallGuest(c, cmp, {a, b})); };
  auto swap = [&](gaddr a, gaddr b) {
    if (a == b) return;
    uint8_t* pa = GPtr(a);
    uint8_t* pb = GPtr(b);
    for (uint64_t i = 0; i < w; i++) std::swap(pa[i], pb[i]);
  };
  auto shortsort = [&](gaddr lo, gaddr hi) {
    for (; hi > lo; hi -= w) {
      gaddr max = lo;
      for (gaddr p = lo + w; p <= hi; p += w)
        if (compare(p, max) > 0) max = p;
      swap(max, hi);
    }
  };
  constexpr uint64_t kCutoff = 8;
  std::vector<std::pair<gaddr, gaddr>> stack;
  gaddr lo = base, hi = base + w * (n - 1);
  for (;;) {
    uint64_t size = (hi - lo) / w + 1;
    if (size <= kCutoff) {
      shortsort(lo, hi);
    } else {
      gaddr mid = lo + (size / 2) * w;
      if (compare(lo, mid) > 0) swap(lo, mid);
      if (compare(lo, hi) > 0) swap(lo, hi);
      if (compare(mid, hi) > 0) swap(mid, hi);
      gaddr loguy = lo, higuy = hi;
      for (;;) {
        if (mid > loguy) {
          do loguy += w;
          while (loguy < mid && compare(loguy, mid) <= 0);
        }
        if (mid <= loguy) {
          do loguy += w;
          while (loguy <= hi && compare(loguy, mid) <= 0);
        }
        do higuy -= w;
        while (higuy > mid && compare(higuy, mid) > 0);
        if (higuy < loguy) break;
        swap(loguy, higuy);
        if (mid == higuy) mid = loguy;  // the pivot moved
      }
      higuy += w;
      if (mid < higuy) {
        do higuy -= w;
        while (higuy > mid && compare(higuy, mid) == 0);
      }
      if (mid >= higuy) {
        do higuy -= w;
        while (higuy > lo && compare(higuy, mid) == 0);
      }
      // [lo, higuy] and [loguy, hi] remain: the smaller one next, the
      // larger one stacked.
      if (higuy - lo >= hi - loguy) {
        if (lo < higuy) stack.push_back({lo, higuy});
        if (loguy < hi) {
          lo = loguy;
          continue;
        }
      } else {
        if (loguy < hi) stack.push_back({loguy, hi});
        if (lo < higuy) {
          hi = higuy;
          continue;
        }
      }
    }
    if (stack.empty()) return 0;
    std::tie(lo, hi) = stack.back();
    stack.pop_back();
  }
}

// ---- environment -----------------------------------------------------------

HLE64("ucrtbase.dll", getenv, kCdecl) {
  std::string name = S(Arg(c, 0));
  gaddr env = Rd64(G().environ);
  if (!env) {
    Wr64(G().environ, GuestStrings(EnvironmentVars()));
    env = Rd64(G().environ);
  }
  for (gaddr e; (e = Rd64(env)); env += 8) {
    const char* v = S(e);
    if (StrICmp(v, name.c_str(), name.size()) == 0 && v[name.size()] == '=')
      return e + name.size() + 1;
  }
  return 0;
}

// ---- elementary functions ------------------------------------------------
// Correctly rounded (CORE-MATH, cmake/coremath.cmake), so the same on every
// host; the UCRT's own are not correctly rounded and may differ from these
// in the last bit. Domain and range errors set the guest's errno, as the
// UCRT does. Computed in the host's round-to-nearest mode.

extern "C" {
float cr_acosf(float), cr_asinf(float), cr_atanf(float), cr_atan2f(float, float),
    cr_cosf(float), cr_logf(float), cr_log2f(float), cr_log10f(float),
    cr_powf(float, float), cr_sinf(float), cr_tanf(float);
double cr_atan(double), cr_cos(double), cr_exp(double), cr_log(double),
    cr_pow(double, double), cr_sin(double);
}

namespace {
template <typename T, typename F>
T WithErrno(F f) {
  errno = 0;
  T v = f();
  if (errno == EDOM || errno == ERANGE) SetErrno(uint32_t(errno));
  return v;
}
}  // namespace

#define RECOMP_CR_F1(name)                                                  \
  HLE64("ucrtbase.dll", name, kCdecl) {                                     \
    return RetF(c, WithErrno<float>([&] { return cr_##name(ArgF(c, 0)); })); \
  }
#define RECOMP_CR_D1(name)                                                   \
  HLE64("ucrtbase.dll", name, kCdecl) {                                      \
    return RetD(c, WithErrno<double>([&] { return cr_##name(ArgD(c, 0)); })); \
  }
RECOMP_CR_F1(acosf)
RECOMP_CR_F1(asinf)
RECOMP_CR_F1(atanf)
RECOMP_CR_F1(cosf)
RECOMP_CR_F1(logf)
RECOMP_CR_F1(log2f)
RECOMP_CR_F1(log10f)
RECOMP_CR_F1(sinf)
RECOMP_CR_F1(tanf)
RECOMP_CR_D1(atan)
RECOMP_CR_D1(cos)
RECOMP_CR_D1(exp)
RECOMP_CR_D1(log)
RECOMP_CR_D1(sin)
#undef RECOMP_CR_F1
#undef RECOMP_CR_D1
HLE64("ucrtbase.dll", atan2f, kCdecl) {
  return RetF(c, WithErrno<float>([&] { return cr_atan2f(ArgF(c, 0), ArgF(c, 1)); }));
}
HLE64("ucrtbase.dll", powf, kCdecl) {
  return RetF(c, WithErrno<float>([&] { return cr_powf(ArgF(c, 0), ArgF(c, 1)); }));
}
HLE64("ucrtbase.dll", pow, kCdecl) {
  return RetD(c, WithErrno<double>([&] { return cr_pow(ArgD(c, 0), ArgD(c, 1)); }));
}

// ---- math with exact results (the same on every IEEE host) ---------------

HLE64("ucrtbase.dll", sqrt, kCdecl) { return RetD(c, std::sqrt(ArgD(c, 0))); }
HLE64("ucrtbase.dll", sqrtf, kCdecl) { return RetF(c, std::sqrt(ArgF(c, 0))); }
HLE64("ucrtbase.dll", ceil, kCdecl) { return RetD(c, std::ceil(ArgD(c, 0))); }
HLE64("ucrtbase.dll", ceilf, kCdecl) { return RetF(c, std::ceil(ArgF(c, 0))); }
HLE64("ucrtbase.dll", floor, kCdecl) { return RetD(c, std::floor(ArgD(c, 0))); }
HLE64("ucrtbase.dll", floorf, kCdecl) { return RetF(c, std::floor(ArgF(c, 0))); }
HLE64("ucrtbase.dll", truncf, kCdecl) { return RetF(c, std::trunc(ArgF(c, 0))); }
HLE64("ucrtbase.dll", roundf, kCdecl) { return RetF(c, std::round(ArgF(c, 0))); }
HLE64("ucrtbase.dll", fmod, kCdecl) { return RetD(c, std::fmod(ArgD(c, 0), ArgD(c, 1))); }
HLE64("ucrtbase.dll", fmodf, kCdecl) { return RetF(c, std::fmod(ArgF(c, 0), ArgF(c, 1))); }
HLE64("ucrtbase.dll", ldexp, kCdecl) {
  return RetD(c, std::ldexp(ArgD(c, 0), int32_t(Arg(c, 1))));
}
HLE64("ucrtbase.dll", modf, kCdecl) {
  double ip;
  double f = std::modf(ArgD(c, 0), &ip);
  uint64_t b;
  std::memcpy(&b, &ip, 8);
  Wr64(Arg(c, 1), b);
  return RetD(c, f);
}
HLE64("ucrtbase.dll", nextafterf, kCdecl) {
  return RetF(c, std::nextafter(ArgF(c, 0), ArgF(c, 1)));
}
// _dclass/_fdclass/_ldclass: FP_INFINITE 1, FP_NAN 2, FP_NORMAL -1,
// FP_SUBNORMAL -2, FP_ZERO 0 (MSVC's values). long double is double.
namespace {
uint64_t Class(double v) {
  switch (std::fpclassify(v)) {
    case FP_INFINITE: return 1;
    case FP_NAN: return 2;
    case FP_NORMAL: return uint32_t(-1);
    case FP_SUBNORMAL: return uint32_t(-2);
    default: return 0;
  }
}
}  // namespace
HLE64("ucrtbase.dll", _dclass, kCdecl) { return Class(ArgD(c, 0)); }
HLE64("ucrtbase.dll", _ldclass, kCdecl) { return Class(ArgD(c, 0)); }
HLE64("ucrtbase.dll", _fdclass, kCdecl) { return Class(ArgF(c, 0)); }
HLE64("ucrtbase.dll", _dsign, kCdecl) { return std::signbit(ArgD(c, 0)) ? 0x8000 : 0; }

}  // namespace recomp::hle
