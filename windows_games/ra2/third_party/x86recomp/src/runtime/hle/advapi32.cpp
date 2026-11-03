// advapi32 (registry, user name) and the small ole32/oleaut32 surface the
// game uses.
//
// The registry is an in-memory tree persisted to
// $XDG_DATA_HOME/<id>/registry.txt. It is seeded with what the retail
// installer would have written (GameConfig::registry).

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <vector>

#include "runtime/hle/com.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using namespace win32;

constexpr uint32_t kHkeyClassesRoot = 0x80000000, kHkeyCurrentUser = 0x80000001,
                   kHkeyLocalMachine = 0x80000002, kHkeyUsers = 0x80000003;
constexpr uint32_t kRegSz = 1;

struct RegValue {
  uint32_t type;
  std::vector<uint8_t> data;
};

std::mutex g_reg_mu;
// Full key path (lower case, e.g. "hklm\software\...") -> values (lower
// case name -> value). Key existence = presence in the map.
std::map<std::string, std::map<std::string, RegValue>> g_reg;
std::map<uint32_t, std::string> g_open;  // HKEY -> key path
uint32_t g_next_hkey = 0x1000;
bool g_reg_loaded = false;

std::string RegFile() { return DataDir() + "/registry.txt"; }

RegValue Str(const std::string& s) {
  RegValue v{kRegSz, {}};
  v.data.assign(s.begin(), s.end());
  v.data.push_back(0);
  return v;
}

void Seed() {
  for (const auto& r : Game().registry) {
    std::string k = Lower(r.key);
    // Parent keys below the root exist too.
    for (size_t i = k.find('\\', k.find('\\') + 1); i != std::string::npos;
         i = k.find('\\', i + 1))
      g_reg[k.substr(0, i)];
    g_reg[k][Lower(r.name)] = Str(r.value);
  }
}

void Save() {
  MakeDirs(DataDir());
  std::ofstream f(RegFile());
  for (const auto& [key, vals] : g_reg) {
    f << "[" << key << "]\n";
    for (const auto& [name, v] : vals) {
      f << name << "\t" << v.type << "\t";
      for (uint8_t b : v.data) {
        char buf[3];
        std::snprintf(buf, sizeof buf, "%02x", b);
        f << buf;
      }
      f << "\n";
    }
  }
}

void LoadOnce() {
  if (g_reg_loaded) return;
  g_reg_loaded = true;
  std::ifstream f(RegFile());
  if (!f) {
    Seed();
    return;
  }
  std::string line, key;
  while (std::getline(f, line)) {
    if (line.empty()) continue;
    if (line[0] == '[') {
      key = line.substr(1, line.size() - 2);
      g_reg[key];
      continue;
    }
    std::istringstream in(line);
    std::string name, type, hex;
    std::getline(in, name, '\t');
    std::getline(in, type, '\t');
    std::getline(in, hex);
    RegValue v{uint32_t(std::stoul(type)), {}};
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
      v.data.push_back(uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
    g_reg[key][name] = v;
  }
}

std::string RootName(uint32_t h) {
  switch (h) {
    case kHkeyClassesRoot: return "hkcr";
    case kHkeyCurrentUser: return "hkcu";
    case kHkeyLocalMachine: return "hklm";
    case kHkeyUsers: return "hku";
    default: {
      auto it = g_open.find(h);
      return it == g_open.end() ? std::string() : it->second;
    }
  }
}

std::string Join(const std::string& base, const std::string& sub) {
  std::string s = Lower(sub);
  while (!s.empty() && s.back() == '\\') s.pop_back();
  while (!s.empty() && s.front() == '\\') s.erase(0, 1);
  return s.empty() ? base : base + "\\" + s;
}

HLE("advapi32.dll", RegOpenKeyExA, 5) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string base = RootName(Arg(c, 0));
  if (base.empty()) return kErrorInvalidHandle;
  std::string path = Join(base, GStr(Arg(c, 1)));
  if (!g_reg.count(path)) return kErrorFileNotFound;
  uint32_t h = g_next_hkey += 4;
  g_open[h] = path;
  Wr32(Arg(c, 4), h);
  return kErrorSuccess;
}

HLE("advapi32.dll", RegCreateKeyExA, 9) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string base = RootName(Arg(c, 0));
  if (base.empty()) return kErrorInvalidHandle;
  std::string path = Join(base, GStr(Arg(c, 1)));
  bool existed = g_reg.count(path);
  // Create the key and all its parents.
  for (size_t i = 0; i <= path.size(); i++)
    if (i == path.size() || path[i] == '\\') g_reg[path.substr(0, i)];
  uint32_t h = g_next_hkey += 4;
  g_open[h] = path;
  Wr32(Arg(c, 7), h);
  if (Arg(c, 8)) Wr32(Arg(c, 8), existed ? 2 : 1);  // REG_OPENED / CREATED
  if (!existed) Save();
  return kErrorSuccess;
}

HLE("advapi32.dll", RegCloseKey, 1) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  g_open.erase(Arg(c, 0));
  return kErrorSuccess;
}

HLE("advapi32.dll", RegQueryValueExA, 6) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string key = RootName(Arg(c, 0));
  uint32_t type_out = Arg(c, 3), data = Arg(c, 4), size_ptr = Arg(c, 5);
  auto k = g_reg.find(key);
  if (k == g_reg.end()) return kErrorInvalidHandle;
  auto v = k->second.find(Lower(GStr(Arg(c, 1))));
  if (v == k->second.end()) {
    RECOMP_TRACE("  RegQueryValueExA(%s, %s): not found\n", key.c_str(),
              GStr(Arg(c, 1)).c_str());
    return kErrorFileNotFound;
  }
  if (type_out) Wr32(type_out, v->second.type);
  uint32_t n = uint32_t(v->second.data.size());
  if (!size_ptr) return data ? kErrorInvalidHandle : kErrorSuccess;
  uint32_t cap = Rd32(size_ptr);
  Wr32(size_ptr, n);
  if (!data) return kErrorSuccess;
  if (cap < n) return kErrorMoreData;
  std::memcpy(GPtr(data), v->second.data.data(), n);
  return kErrorSuccess;
}

HLE("advapi32.dll", RegSetValueExA, 6) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string key = RootName(Arg(c, 0));
  if (!g_reg.count(key)) return kErrorInvalidHandle;
  RegValue v{uint32_t(Arg(c, 3)), {}};
  uint8_t* p = GPtr(Arg(c, 4));
  v.data.assign(p, p + Arg(c, 5));
  g_reg[key][Lower(GStr(Arg(c, 1)))] = v;
  Save();
  return kErrorSuccess;
}

// Direct subkeys of `path` (names as stored: the tree keeps them in lower
// case), in sorted order.
std::vector<std::string> SubKeys(const std::string& path) {
  std::vector<std::string> out;
  const std::string prefix = path + "\\";
  for (auto it = g_reg.lower_bound(prefix); it != g_reg.end(); ++it) {
    if (it->first.compare(0, prefix.size(), prefix) != 0) break;
    std::string rest = it->first.substr(prefix.size());
    if (!rest.empty() && rest.find('\\') == std::string::npos) out.push_back(rest);
  }
  return out;
}

constexpr uint32_t kErrorNoMoreItems = 259;

// (hKey, lpSubKey): a key with subkeys is not deleted (ERROR_ACCESS_DENIED).
HLE("advapi32.dll", RegDeleteKeyA, 2) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string base = RootName(Arg(c, 0));
  if (base.empty()) return kErrorInvalidHandle;
  std::string path = Join(base, GStr(Arg(c, 1)));
  if (path == base || !g_reg.count(path)) return kErrorFileNotFound;
  if (!SubKeys(path).empty()) return kErrorAccessDenied;
  g_reg.erase(path);
  Save();
  return kErrorSuccess;
}

// (hKey, lpValueName)
HLE("advapi32.dll", RegDeleteValueA, 2) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  auto k = g_reg.find(RootName(Arg(c, 0)));
  if (k == g_reg.end()) return kErrorInvalidHandle;
  if (!k->second.erase(Lower(Arg(c, 1) ? GStr(Arg(c, 1)) : std::string())))
    return kErrorFileNotFound;
  Save();
  return kErrorSuccess;
}

// (hKey, dwIndex, lpName, lpcchName, lpReserved, lpClass, lpcchClass,
// lpftLastWriteTime). *lpcchName is the buffer size with the NUL on entry
// and the name's length without it on success. Keys have no class and no
// write time here (both reported empty / zero).
HLE("advapi32.dll", RegEnumKeyExA, 8) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  std::string key = RootName(Arg(c, 0));
  if (key.empty() || !g_reg.count(key)) return kErrorInvalidHandle;
  std::vector<std::string> subs = SubKeys(key);
  uint32_t index = Arg(c, 1), name = Arg(c, 2), pcch = Arg(c, 3);
  if (index >= subs.size()) return kErrorNoMoreItems;
  const std::string& n = subs[index];
  if (!pcch || Rd32(pcch) <= n.size()) return kErrorMoreData;
  PutStr(name, n);
  Wr32(pcch, uint32_t(n.size()));
  if (Arg(c, 5) && Arg(c, 6) && Rd32(Arg(c, 6))) Wr8(Arg(c, 5), 0);
  if (Arg(c, 6)) Wr32(Arg(c, 6), 0);
  if (Arg(c, 7)) {
    Wr32(Arg(c, 7), 0);
    Wr32(Arg(c, 7) + 4, 0);
  }
  return kErrorSuccess;
}

// (hKey, lpClass, lpcchClass, lpReserved, lpcSubKeys, lpcbMaxSubKeyLen,
// lpcbMaxClassLen, lpcValues, lpcbMaxValueNameLen, lpcbMaxValueLen,
// lpcbSecurityDescriptor, lpftLastWriteTime); every out pointer optional.
HLE("advapi32.dll", RegQueryInfoKeyA, 12) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  auto k = g_reg.find(RootName(Arg(c, 0)));
  if (k == g_reg.end()) return kErrorInvalidHandle;
  std::vector<std::string> subs = SubKeys(k->first);
  uint32_t max_sub = 0, max_name = 0, max_value = 0;
  for (const auto& s : subs) max_sub = std::max(max_sub, uint32_t(s.size()));
  for (const auto& [n, v] : k->second) {
    max_name = std::max(max_name, uint32_t(n.size()));
    max_value = std::max(max_value, uint32_t(v.data.size()));
  }
  auto put = [&](int i, uint32_t v) {
    if (Arg(c, i)) Wr32(Arg(c, i), v);
  };
  if (Arg(c, 1) && Arg(c, 2) && Rd32(Arg(c, 2))) Wr8(Arg(c, 1), 0);
  put(2, 0);
  put(4, uint32_t(subs.size()));
  put(5, max_sub);
  put(6, 0);
  put(7, uint32_t(k->second.size()));
  put(8, max_name);
  put(9, max_value);
  put(10, 0);
  if (Arg(c, 11)) {
    Wr32(Arg(c, 11), 0);
    Wr32(Arg(c, 11) + 4, 0);
  }
  return kErrorSuccess;
}

HLE("advapi32.dll", GetUserNameA, 2) {
  std::string n = "Player";
  uint32_t buf = Arg(c, 0), psize = Arg(c, 1);
  if (Rd32(psize) < n.size() + 1) {
    Wr32(psize, uint32_t(n.size() + 1));
    SetLastError(kErrorInsufficientBuffer);
    return 0;
  }
  PutStr(buf, n);
  Wr32(psize, uint32_t(n.size() + 1));
  return 1;
}

}  // namespace

// A REG_SZ value for host code (com: InprocServer32). `path` is a full key
// path with the root abbreviated ("HKCR\CLSID\{...}\InprocServer32"); an
// empty `name` is the key's default value.
bool RegistryString(const std::string& path, const std::string& name, std::string* out) {
  std::lock_guard<std::mutex> l(g_reg_mu);
  LoadOnce();
  auto k = g_reg.find(Lower(path));
  if (k == g_reg.end()) return false;
  auto v = k->second.find(Lower(name));
  if (v == k->second.end() || v->second.type != kRegSz) return false;
  out->assign(reinterpret_cast<const char*>(v->second.data.data()), v->second.data.size());
  while (!out->empty() && out->back() == '\0') out->pop_back();
  return true;
}

namespace {

// ---- ole32 ------------------------------------------------------------------

HLE("ole32.dll", CoInitialize, 1) { return 0; }  // S_OK
HLE("ole32.dll", CoInitializeEx, 2) { return 0; }
HLE("ole32.dll", CoUninitialize, 0) { return 0; }

// The COM task allocator: the process heap (a NULL free is a no-op).
HLE64("ole32.dll", CoTaskMemAlloc, 1) { return HeapAllocProcess(Arg(c, 0), false); }
HLE64("ole32.dll", CoTaskMemFree, 1) {
  if (Arg(c, 0)) HeapFreeProcess(Arg(c, 0));
  return 0;
}

// (pProxy, dwAuthnSvc, dwAuthzSvc, pServerPrincName, dwAuthnLevel,
// dwImpLevel, pAuthInfo, dwCapabilities): the objects are in-process, so
// there is no proxy security to set.
HLE("ole32.dll", CoSetProxyBlanket, 8) { return 0; }

HLE("ole32.dll", CoCreateGuid, 1) {
  static std::mt19937_64 rng{std::random_device{}()};
  uint32_t g = Arg(c, 0);
  for (int i = 0; i < 4; i++) Wr32(g + 4 * i, uint32_t(rng()));
  Wr8(g + 7, uint8_t((g_proc->mem.read<uint8_t>(g + 7) & 0x0F) | 0x40));  // v4
  return 0;
}

HLE("ole32.dll", StringFromGUID2, 3) {
  uint32_t g = Arg(c, 0), out = Arg(c, 1), cap = Arg(c, 2);
  char buf[64];
  const uint8_t* b = GPtr(g + 8);
  std::snprintf(buf, sizeof buf,
                "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", Rd32(g),
                Rd16(g + 4), Rd16(g + 6), b[0], b[1], b[2], b[3], b[4], b[5],
                b[6], b[7]);
  size_t n = std::strlen(buf) + 1;
  if (cap < n) return 0;
  PutWStr(out, buf);
  return uint32_t(n);
}

HLE("ole32.dll", IIDFromString, 2) {
  std::string s = Narrow(GWStr(Arg(c, 0)));
  unsigned int d1, d2, d3, b[8];
  if (std::sscanf(s.c_str(), "{%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1,
                  &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6],
                  &b[7]) != 11)
    return kEInvalidArg;
  uint32_t g = Arg(c, 1);
  Wr32(g, d1);
  Wr16(g + 4, uint16_t(d2));
  Wr16(g + 6, uint16_t(d3));
  for (int i = 0; i < 8; i++) Wr8(g + 8 + i, uint8_t(b[i]));
  return 0;
}

// ---- oleaut32 (imported by ordinal) -----------------------------------------

HLE_NAMED("oleaut32.dll", "#2", SysAllocString, 1) {
  return Arg(c, 0) ? com::AllocBstr(GWStr(Arg(c, 0))) : 0;
}

HLE_NAMED("oleaut32.dll", "#6", SysFreeString, 1) {
  com::FreeBstr(Arg(c, 0));
  return 0;
}

}  // namespace
}  // namespace recomp::hle
