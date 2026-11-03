#include "runtime/hle/hle.h"

#include <cstdlib>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <deque>
#include <vector>

#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/watchdog.h"

namespace recomp::hle {

thread_local int t_hle_depth = 0;

namespace {

std::map<std::pair<std::string, std::string>, Def>& Registry() {
  static std::map<std::pair<std::string, std::string>, Def> r;
  return r;
}

// Real export tables of provided DLLs whose games fetch every entry point
// up front (see Exports in hle.h).
std::map<std::string, std::set<std::string>>& DeclaredExports() {
  static std::map<std::string, std::set<std::string>> e;
  return e;
}

struct Slot {
  std::string dll, name;
  const Def* def;  // nullptr: unimplemented
  // DirectDraw/Direct3D 7 entry points run under one process-wide lock,
  // like DirectDraw with DDSCL_MULTITHREADED (which the game requests):
  // its worker thread uploads textures while the main thread renders,
  // and neither our ddraw state nor the d3d9 device is reentrant.
  bool graphics;
};

std::mutex g_slots_mu;  // serialises AddSlot
std::deque<Slot> g_slots;  // under g_slots_mu; stable refs

// CallThunk's view of g_slots, read without a lock (it runs on every
// import call): index = (thunk - kThunkBase) / kThunkStride. AddSlot
// stores each pointer with release once its Slot is complete.
constexpr size_t kMaxSlots = kDataExportOffset / kThunkStride;
std::atomic<const Slot*> g_slot_table[kMaxSlots];

// Pseudo HMODULEs for DLLs we provide. Real modules are guest pointers to
// a PE header; nothing in the game dereferences these, so a recognisable
// value outside the arena's used ranges is enough.
const std::vector<std::string>& KnownDlls() {
  static const std::vector<std::string> dlls = {
      "kernel32.dll", "user32.dll",  "gdi32.dll",    "advapi32.dll",
      "ole32.dll",    "version.dll", "winmm.dll",    "wsock32.dll",
      "ddraw.dll",    "mss32.dll",   "binkw32.dll",  "dbghelp.dll",
      "imm32.dll",    "shfolder.dll", "ntdll.dll",   "opengl32.dll",
      "comctl32.dll", "oleaut32.dll", "dsound.dll"};
  return dlls;
}

// The thunk region of this process's mode.
gaddr ThunkBase() { return g_proc->x64 ? kThunkBase64 : kThunkBase; }
gaddr ModuleRegion() { return g_proc->x64 ? kModuleRegion64 : kModuleRegion; }

gaddr AddSlot(const std::string& dll, const std::string& name) {
  std::lock_guard<std::mutex> l(g_slots_mu);
  for (size_t i = 0; i < g_slots.size(); i++)
    if (g_slots[i].dll == dll && g_slots[i].name == name)
      return ThunkBase() + uint32_t(i) * kThunkStride;
  if (g_slots.size() >= kMaxSlots) FatalF("thunk region exhausted");
  gaddr thunk = ThunkBase() + uint32_t(g_slots.size()) * kThunkStride;
  auto it = Registry().find({dll, name});
  g_slots.push_back({dll, name, it == Registry().end() ? nullptr : &it->second,
                     dll == "ddraw.dll"});
  g_slot_table[g_slots.size() - 1].store(&g_slots.back(),
                                         std::memory_order_release);
  return thunk;
}

struct DataDef {
  uint32_t size;
  void (*init)(gaddr);
};
std::map<std::pair<std::string, std::string>, DataDef>& DataRegistry() {
  static std::map<std::pair<std::string, std::string>, DataDef> r;
  return r;
}

// The guest address of a data export (allocated and initialised on first
// use), or 0 if dll!name is not one.
gaddr DataAddress(const std::string& dll, const std::string& name) {
  static std::mutex mu;
  static std::map<std::pair<std::string, std::string>, gaddr> placed;
  static gaddr next = 0;
  auto it = DataRegistry().find({dll, name});
  if (it == DataRegistry().end()) return 0;
  std::lock_guard<std::mutex> l(mu);
  gaddr& a = placed[{dll, name}];
  if (a) return a;
  if (!next) next = ThunkBase() + kDataExportOffset;
  a = next;
  next = (next + it->second.size + 15) & ~15ull;
  if (next > ThunkBase() + kThunkRegionSize - 0x1000) FatalF("data export region exhausted");
  if (it->second.init) it->second.init(a);
  return a;
}

}  // namespace

Registrar::Registrar(const char* dll, const char* name, int nargs, Fn fn, bool x64) {
  Registry()[{dll, name}] = Def{nargs, fn, x64};
}

DataExport::DataExport(const char* dll, const char* name, uint32_t size, void (*init)(gaddr)) {
  DataRegistry()[{dll, name}] = DataDef{size, init};
}

Exports::Exports(const char* dll, std::initializer_list<const char*> names) {
  auto& s = DeclaredExports()[dll];
  for (const char* n : names) s.insert(n);
}

std::string NormaliseDll(std::string dll) {
  dll = win32::Lower(dll);
  size_t slash = dll.find_last_of("\\/");
  if (slash != std::string::npos) dll = dll.substr(slash + 1);
  if (dll.find('.') == std::string::npos) dll += ".dll";
  // The UCRT's API sets (api-ms-win-crt-runtime-l1-1-0.dll, ...) are all
  // ucrtbase.dll, as Windows' API set map resolves them.
  if (dll.rfind("api-ms-win-crt-", 0) == 0) return "ucrtbase.dll";
  return dll;
}

gaddr ImportThunk(const std::string& dll, const std::string& name) {
  std::string n = NormaliseDll(dll);
  if (gaddr data = DataAddress(n, name)) return data;
  if (!Registry().count({n, name}))
    RECOMP_TRACE("hle: import %s!%s has no handler\n", n.c_str(), name.c_str());
  return AddSlot(n, name);
}

void CallThunk(Ctx& c, gaddr thunk) {
  const bool x64 = g_proc->x64;
  const gaddr off = thunk - ThunkBase();
  size_t idx = off / kThunkStride;
  const Slot* s = nullptr;
  if (off % kThunkStride == 0 && idx < kMaxSlots)
    s = g_slot_table[idx].load(std::memory_order_acquire);
  const unsigned long long ret = x64 ? Rd64(c.rsp) : Rd32(c.esp);
  if (!s)
    FatalF("call to unknown thunk %08llX (return address %08llX)",
           (unsigned long long)thunk, ret);
  if (!s->def)
    FatalF("unimplemented import %s!%s called (return address %08llX)",
           s->dll.c_str(), s->name.c_str(), ret);
  if (x64 && !s->def->x64)
    FatalF("import %s!%s called from x64 code: its handler is not audited for "
           "x64 guests (HLE64) (return address %08llX)",
           s->dll.c_str(), s->name.c_str(), ret);
  const Def& d = *s->def;
  // <ID>_TRACE_FILTER=mss32 (substring of "dll!name") narrows the trace.
  static const char* filter = Env("TRACE_FILTER");
  const bool trace = g_trace && (!filter || (s->dll + "!" + s->name).find(filter) !=
                                                std::string::npos);
  if (trace) {
    std::string args;
    for (int i = 0; i < std::max(d.nargs, 0) && i < 8; i++) {
      char buf[24];
      std::snprintf(buf, sizeof buf, i ? ", %llX" : "%llX", (unsigned long long)Arg(c, i));
      args += buf;
    }
    TraceF("[%04X] %s!%s(%s) from %08llX\n", CurrentThread()->tid,
           s->dll.c_str(), s->name.c_str(), args.c_str(), ret);
  }
  // For hang dumps: which import this thread is inside (innermost). A
  // guest exception's unwind (GuestResume) may leave through the handler,
  // so this is undone on the way out either way.
  struct Inside {
    const char* saved_name = t_wd_hle_name;
    uint64_t saved_ret = t_wd_hle_ret;
    Inside(const char* name, uint64_t ret) {
      t_wd_hle_name = name;
      t_wd_hle_ret = ret;
      ++t_hle_depth;
    }
    ~Inside() {
      --t_hle_depth;
      t_wd_hle_name = saved_name;
      t_wd_hle_ret = saved_ret;
    }
  };
  uint64_t r;
  {
    Inside inside(s->name.c_str(), ret);
    static std::recursive_mutex graphics_mu;
    std::unique_lock<std::recursive_mutex> gl;
    if (s->graphics) gl = std::unique_lock<std::recursive_mutex>(graphics_mu);
    r = d.fn(c);
  }
  if (trace) TraceF("[%04X]   -> %llX\n", CurrentThread()->tid, (unsigned long long)r);
  if (x64) {
    c.rax = r;
    c.rsp += 8;  // the caller pops its own arguments
  } else {
    c.eax = uint32_t(r);
    c.esp += 4 + (d.nargs > 0 ? 4 * uint32_t(d.nargs) : 0);
  }
}

gaddr ModuleHandle(const std::string& dll) {
  std::string n = NormaliseDll(dll);
  const auto& k = KnownDlls();
  auto it = std::find(k.begin(), k.end(), n);
  if (it == k.end()) return 0;
  return ModuleRegion() + uint32_t(it - k.begin()) * 0x10000;
}

gaddr ProcAddress(gaddr hmodule, const std::string& name) {
  const auto& k = KnownDlls();
  gaddr idx = (hmodule - ModuleRegion()) / 0x10000;
  if (hmodule < ModuleRegion() || idx >= k.size()) return 0;
  if (gaddr data = DataAddress(k[idx], name)) return data;
  const std::string& dll = k[idx];
  if (!Registry().count({dll, name})) {
    auto ex = DeclaredExports().find(dll);
    if (ex != DeclaredExports().end() && ex->second.count(name))
      return AddSlot(dll, name);  // a real export; fatal if ever called
    WarnF("GetProcAddress(%s, %s): not implemented, returning NULL\n",
          dll.c_str(), name.c_str());
    return 0;
  }
  return AddSlot(dll, name);
}

gaddr ThunkFor(const std::string& dll, const std::string& name) {
  return AddSlot(dll, name);
}

// TEB LastErrorValue: +0x34 (32-bit), +0x68 (x64).
void SetLastError(uint32_t e) {
  Wr32(CurrentThread()->teb + (g_proc->x64 ? 0x68 : 0x34), e);
}

uint32_t CopyOut(gaddr buf, uint32_t size, const std::string& s) {
  if (s.size() + 1 > size) return uint32_t(s.size() + 1);
  PutStr(buf, s);
  return uint32_t(s.size());
}

uint32_t PutCounted(gaddr buf, uint32_t cap, const std::string& s, bool wide) {
  if (!cap) return uint32_t(s.size() + 1);
  if (s.size() + 1 > cap) {
    SetLastError(win32::kErrorInsufficientBuffer);
    return 0;
  }
  if (wide)
    PutWStr(buf, s);
  else
    PutStr(buf, s);
  return uint32_t(s.size() + 1);
}

}  // namespace recomp::hle
