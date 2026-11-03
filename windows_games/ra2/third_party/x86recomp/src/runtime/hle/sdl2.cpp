// SDL2.dll for x64 guests that link SDL2 themselves, on the host's SDL3.
// Guest-visible structures (SDL_Event, SDL_DisplayMode, SDL_Surface,
// SDL_SysWMinfo, SDL_RWops...) keep SDL2's x64 layouts; host objects
// (windows, game controllers, haptics) are handed to the guest as handles.
// SDL2 is middleware here like Miles or Bink: this is its API on SDL3, not
// a game's.

#include <SDL3/SDL.h>

#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/user32_window.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

constexpr const char* kDll = "sdl2.dll";

// A guest copy of a host string, kept per thread until the next call
// (SDL_GetError, SDL_GetHint: SDL2 returns pointers it owns).
gaddr ThreadString(const char* s, int slot) {
  static thread_local gaddr buf[4] = {};
  static thread_local size_t cap[4] = {};
  size_t n = std::strlen(s) + 1;
  if (n > cap[slot]) {
    if (buf[slot]) HeapFreeProcess(buf[slot]);
    cap[slot] = std::max<size_t>(n, 256);
    buf[slot] = HeapAllocProcess(cap[slot], false);
  }
  std::memcpy(GPtr(buf[slot]), s, n);
  return buf[slot];
}

// SDL2's SDL_bool and int results.
uint64_t Bool(bool b) { return b ? 1 : 0; }
uint64_t Status(bool ok) { return ok ? 0 : uint32_t(-1); }

}  // namespace

// ---- memory, strings ---------------------------------------------------------

HLE64(kDll, SDL_malloc, kCdecl) { return HeapAllocProcess(std::max<uint64_t>(Arg(c, 0), 1), false); }
HLE64(kDll, SDL_calloc, kCdecl) {
  uint64_t n = Arg(c, 0), size = Arg(c, 1);
  if (size && n > UINT64_MAX / size) return 0;
  return HeapAllocProcess(std::max<uint64_t>(n * size, 1), true);
}
HLE64(kDll, SDL_free, kCdecl) {
  if (gaddr p = Arg(c, 0)) HeapFreeProcess(p);
  return 0;
}
HLE64(kDll, SDL_wcslen, kCdecl) {  // wchar_t is 16-bit on Windows
  gaddr p = Arg(c, 0);
  uint64_t n = 0;
  while (Rd16(p + 2 * n)) n++;
  return n;
}

// ---- initialisation -------------------------------------------------------------
// SDL2's subsystem flags are SDL3's (SDL_INIT_TIMER, 0x1, is gone: timers
// need no init). Video goes through the runtime's own SDL setup.

HLE64(kDll, SDL_SetMainReady, kCdecl) { return 0; }

HLE64(kDll, SDL_Init, kCdecl) {
  uint32_t flags = uint32_t(Arg(c, 0)) & ~0x1u;
  if (flags & SDL_INIT_VIDEO) EnsureSdlVideo();
  return Status(SDL_InitSubSystem(flags));
}
HLE64(kDll, SDL_QuitSubSystem, kCdecl) {
  // Video stays up: the runtime's windows (and its presentation) need it.
  SDL_QuitSubSystem(uint32_t(Arg(c, 0)) & ~(0x1u | SDL_INIT_VIDEO | SDL_INIT_EVENTS));
  return 0;
}
HLE64(kDll, SDL_Quit, kCdecl) { return 0; }

HLE64(kDll, SDL_GetError, kCdecl) { return ThreadString(SDL_GetError(), 0); }

HLE64(kDll, SDL_SetHint, kCdecl) {
  return Bool(SDL_SetHint(GStr(Arg(c, 0)).c_str(), GStr(Arg(c, 1)).c_str()));
}
HLE64(kDll, SDL_GetHint, kCdecl) {
  const char* v = SDL_GetHint(GStr(Arg(c, 0)).c_str());
  return v ? ThreadString(v, 1) : 0;
}

// ---- time, CPU, paths -------------------------------------------------------------

HLE64(kDll, SDL_GetPerformanceCounter, kCdecl) { return SDL_GetPerformanceCounter(); }
HLE64(kDll, SDL_GetPerformanceFrequency, kCdecl) { return SDL_GetPerformanceFrequency(); }
HLE64(kDll, SDL_Delay, kCdecl) {
  GuestSyncPoint sync;
  SDL_Delay(uint32_t(Arg(c, 0)));
  return 0;
}
HLE64(kDll, SDL_GetCPUCount, kCdecl) { return GuestProcessorCount(); }

// The directory of the EXE as the guest sees it, with a trailing
// backslash; the caller frees it with SDL_free.
HLE64(kDll, SDL_GetBasePath, kCdecl) {
  std::string dir = GuestGameDir();
  if (dir.empty() || dir.back() != '\\') dir += '\\';
  gaddr p = HeapAllocProcess(dir.size() + 1, false);
  PutStr(p, dir);
  return p;
}

// SDL_iconv_string(to, from, text, bytes): the conversions between the
// encodings Windows games use (UTF-8, UTF-16LE/UCS-2, ASCII); the result
// is SDL_malloc'd and NUL-terminated (four zero bytes, as SDL2's).
HLE64(kDll, SDL_iconv_string, kCdecl) {
  std::string to = win32::Lower(GStr(Arg(c, 0))), from = win32::Lower(GStr(Arg(c, 1)));
  gaddr in = Arg(c, 2);
  uint64_t n = Arg(c, 3);
  const char* hto = to == "ucs-2" || to == "ucs-2-internal" || to == "wchar_t" ? "UTF-16LE"
                                                                               : to.c_str();
  const char* hfrom = from == "ucs-2" || from == "ucs-2-internal" || from == "wchar_t"
                          ? "UTF-16LE"
                          : from.c_str();
  char* r = SDL_iconv_string(hto, hfrom, reinterpret_cast<const char*>(GPtr(in)), size_t(n));
  if (!r) return 0;
  // The host result ends with up to four zero bytes; find the content's end
  // by the target's unit size.
  size_t unit = std::strstr(hto, "16") ? 2 : std::strstr(hto, "32") ? 4 : 1;
  size_t len = 0;
  for (;; len += unit) {
    bool zero = true;
    for (size_t k = 0; k < unit; k++) zero = zero && !r[len + k];
    if (zero) break;
  }
  gaddr out = HeapAllocProcess(len + 4, true);
  std::memcpy(GPtr(out), r, len);
  SDL_free(r);
  return out;
}

// ---- SDL_RWops --------------------------------------------------------------------
// A guest SDL_RWops (SDL2's x64 layout: size, seek, read, write, close,
// type) stands for a host SDL_IOStream. The guest only hands them to SDL
// functions, which use the host stream; its function pointers are thunks
// named "SDL_RWops::<op>" without handlers, so a direct call is fatal and
// says which.

namespace {

constexpr uint64_t kRwopsSize = 0x58;
std::mutex g_rw_mu;
std::map<gaddr, SDL_IOStream*> g_rw;

uint64_t NewRw(SDL_IOStream* io) {
  if (!io) return 0;
  static const char* const kOps[] = {"size", "seek", "read", "write", "close"};
  gaddr rw = HeapAllocProcess(kRwopsSize, true);
  for (int i = 0; i < 5; i++) Wr64(rw + 8 * i, ThunkFor(kDll, std::string("SDL_RWops::") + kOps[i]));
  std::lock_guard<std::mutex> l(g_rw_mu);
  g_rw[rw] = io;
  return rw;
}

// The host stream for a guest SDL_RWops passed to an SDL function; with
// freesrc the function closes it, so the guest block goes too.
SDL_IOStream* UseRw(gaddr rw, bool freesrc, const char* api) {
  std::lock_guard<std::mutex> l(g_rw_mu);
  auto it = g_rw.find(rw);
  if (it == g_rw.end()) FatalF("%s: unknown SDL_RWops %08llX", api, (unsigned long long)rw);
  SDL_IOStream* io = it->second;
  if (freesrc) {
    g_rw.erase(it);
    HeapFreeProcess(rw);
  }
  return io;
}

}  // namespace

// SDL_RWFromFile(file, mode): file is UTF-8 (a guest path).
HLE64(kDll, SDL_RWFromFile, kCdecl) {
  std::string file = GStr(Arg(c, 0)), mode = GStr(Arg(c, 1));
  bool bad;
  std::u16string w = win32::Utf8ToUtf16(reinterpret_cast<const uint8_t*>(file.data()), file.size(), &bad);
  if (bad) FatalF("SDL_RWFromFile: path is not UTF-8");
  if (mode.empty() || !std::strchr("rwa", mode[0])) FatalF("SDL_RWFromFile: mode \"%s\"", mode.c_str());
  bool plus = mode.find('+') != std::string::npos;
  PathAccess acc = mode[0] == 'r' ? (plus ? PathAccess::kUpdate : PathAccess::kRead)
                   : mode[0] == 'a' ? PathAccess::kUpdate
                                    : PathAccess::kWrite;
  std::string host = HostPath(NarrowPath(w), acc);
  RECOMP_TRACE("  SDL_RWFromFile(%s, %s) -> %s\n", file.c_str(), mode.c_str(), host.c_str());
  return NewRw(SDL_IOFromFile(host.c_str(), mode.c_str()));
}

// SDL_RWFromConstMem(mem, size): the host stream reads guest memory in
// place.
HLE64(kDll, SDL_RWFromConstMem, kCdecl) {
  return NewRw(SDL_IOFromConstMem(GPtr(Arg(c, 0)), size_t(uint32_t(Arg(c, 1)))));
}

// SDL_LoadBMP_RW(rw, freesrc). A null rw (a failed SDL_RWFromFile passed
// on) is SDL2's invalid-parameter error; images need guest SDL_Surfaces,
// which are not provided yet.
HLE64(kDll, SDL_LoadBMP_RW, kCdecl) {
  if (!Arg(c, 0)) {
    SDL_SetError("Parameter 'src' is invalid");
    return 0;
  }
  FatalF("SDL_LoadBMP_RW: guest SDL_Surface not implemented");
}

// SDL_GameControllerAddMappingsFromRW(rw, freerw): the mapping database
// format is unchanged in SDL3. The number added, or -1.
HLE64(kDll, SDL_GameControllerAddMappingsFromRW, kCdecl) {
  bool free = uint32_t(Arg(c, 1)) != 0;
  SDL_IOStream* io = UseRw(Arg(c, 0), free, "SDL_GameControllerAddMappingsFromRW");
  return uint64_t(int64_t(SDL_AddGamepadMappingsFromIO(io, free)));
}

}  // namespace recomp::hle
