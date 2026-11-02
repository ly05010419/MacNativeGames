// kernel32: resources, time conversion/formatting, strings, and the odds
// and ends (console title, FormatMessage, CreateProcess); version.dll
// (version resources).

#include <cstring>
#include <ctime>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

using namespace win32;

// ---- resources ------------------------------------------------------------------
// Only the EXE's own .rsrc exists. Resource "handles" (HRSRC) are guest
// addresses of IMAGE_RESOURCE_DATA_ENTRY records; LoadResource returns the
// data pointer and LockResource is the identity, as on Win32.

uint32_t RsrcBase() {
  for (const auto& s : g_proc->img.sections)
    if (s.name == ".rsrc") return s.va;
  return 0;
}

bool IsOwnModule(uint32_t h) { return h == 0 || h == g_proc->img.image_base; }

// Name-or-id argument as a comparable key: "#123" for integer ids.
std::string ResKey(uint32_t v) {
  if (v < 0x10000) return "#" + std::to_string(v);
  std::string s = GStr(v);
  if (!s.empty() && s[0] == '#') return "#" + std::to_string(std::stoul(s.substr(1)));
  for (auto& ch : s) ch = char(std::toupper(uint8_t(ch)));
  return s;
}

// Key of a directory entry (names are UTF-16, upper-cased like Windows).
std::string EntryKey(uint32_t rsrc, uint32_t entry) {
  uint32_t id = Rd32(entry);
  if (!(id & 0x80000000)) return "#" + std::to_string(id & 0xFFFF);
  uint32_t str = rsrc + (id & 0x7FFFFFFF);
  uint16_t len = Rd16(str);
  std::string s;
  for (uint32_t i = 0; i < len; i++)
    s.push_back(char(std::toupper(uint8_t(Rd16(str + 2 + 2 * i)))));
  return s;
}

// Finds the subdirectory/data offset for `key` in directory `dir`; with an
// empty key, the first entry. Returns 0 if absent.
uint32_t FindEntry(uint32_t rsrc, uint32_t dir, const std::string& key) {
  uint32_t n = uint32_t(Rd16(dir + 12)) + Rd16(dir + 14);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t e = dir + 16 + 8 * i;
    if (key.empty() || EntryKey(rsrc, e) == key) return Rd32(e + 4);
  }
  return 0;
}

// The IMAGE_RESOURCE_DATA_ENTRY for type/name keys (an empty name: the
// first one) in language `lang` (0: the first one), or 0.
uint32_t FindResourceImpl(const std::string& type, const std::string& name, uint32_t lang) {
  uint32_t rsrc = RsrcBase();
  if (!rsrc) return 0;
  uint32_t t = FindEntry(rsrc, rsrc, type);
  if (!(t & 0x80000000)) return 0;
  uint32_t n = FindEntry(rsrc, rsrc + (t & 0x7FFFFFFF), name);
  if (!(n & 0x80000000)) return 0;
  uint32_t ldir = rsrc + (n & 0x7FFFFFFF);
  uint32_t l = lang ? FindEntry(rsrc, ldir, "#" + std::to_string(lang)) : 0;
  if (!l) l = FindEntry(rsrc, ldir, "");
  if (!l || (l & 0x80000000)) return 0;
  return rsrc + l;
}

HLE("kernel32.dll", FindResourceA, 3) {
  if (!IsOwnModule(Arg(c, 0))) return 0;
  uint32_t r = FindResourceImpl(ResKey(Arg(c, 2)), ResKey(Arg(c, 1)), 0);
  if (!r) SetLastError(kErrorResourceNameNotFound);
  return r;
}

HLE("kernel32.dll", FindResourceExA, 4) {
  if (!IsOwnModule(Arg(c, 0))) return 0;
  uint32_t r = FindResourceImpl(ResKey(Arg(c, 1)), ResKey(Arg(c, 2)), Arg(c, 3) & 0xFFFF);
  if (!r) SetLastError(kErrorResourceNameNotFound);
  return r;
}

HLE("kernel32.dll", LoadResource, 2) {
  uint32_t hrsrc = Arg(c, 1);
  return hrsrc ? g_proc->img.image_base + Rd32(hrsrc) : 0;
}

HLE("kernel32.dll", LockResource, 1) { return Arg(c, 0); }

// Enumeration callbacks get either an integer id or a guest string.
uint32_t KeyArg(const std::string& key, uint32_t* tmp) {
  if (key[0] == '#') return uint32_t(std::stoul(key.substr(1)));
  *tmp = HeapAllocProcess(uint32_t(key.size() + 1), false);
  PutStr(*tmp, key);
  return *tmp;
}

HLE("kernel32.dll", EnumResourceTypesA, 3) {
  uint32_t rsrc = RsrcBase(), fn = Arg(c, 1), param = Arg(c, 2);
  if (!rsrc || !IsOwnModule(Arg(c, 0))) return 0;
  uint32_t n = uint32_t(Rd16(rsrc + 12)) + Rd16(rsrc + 14);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t tmp = 0;
    uint32_t key = KeyArg(EntryKey(rsrc, rsrc + 16 + 8 * i), &tmp);
    bool go = CallGuest(c, fn, {Arg(c, 0), key, param});
    if (tmp) HeapFreeProcess(tmp);
    if (!go) break;
  }
  return 1;
}

HLE("kernel32.dll", EnumResourceNamesA, 4) {
  uint32_t rsrc = RsrcBase(), type = Arg(c, 1), fn = Arg(c, 2), param = Arg(c, 3);
  if (!rsrc || !IsOwnModule(Arg(c, 0))) return 0;
  uint32_t t = FindEntry(rsrc, rsrc, ResKey(type));
  if (!(t & 0x80000000)) return 0;
  uint32_t dir = rsrc + (t & 0x7FFFFFFF);
  uint32_t n = uint32_t(Rd16(dir + 12)) + Rd16(dir + 14);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t tmp = 0;
    uint32_t key = KeyArg(EntryKey(rsrc, dir + 16 + 8 * i), &tmp);
    bool go = CallGuest(c, fn, {Arg(c, 0), type, key, param});
    if (tmp) HeapFreeProcess(tmp);
    if (!go) break;
  }
  return 1;
}

HLE("kernel32.dll", EnumResourceLanguagesA, 5) {
  uint32_t rsrc = RsrcBase(), type = Arg(c, 1), name = Arg(c, 2), fn = Arg(c, 3),
           param = Arg(c, 4);
  if (!rsrc || !IsOwnModule(Arg(c, 0))) return 0;
  uint32_t t = FindEntry(rsrc, rsrc, ResKey(type));
  if (!(t & 0x80000000)) return 0;
  uint32_t nd = FindEntry(rsrc, rsrc + (t & 0x7FFFFFFF), ResKey(name));
  if (!(nd & 0x80000000)) return 0;
  uint32_t dir = rsrc + (nd & 0x7FFFFFFF);
  uint32_t n = uint32_t(Rd16(dir + 12)) + Rd16(dir + 14);
  for (uint32_t i = 0; i < n; i++) {
    uint32_t lang = Rd32(dir + 16 + 8 * i) & 0xFFFF;
    if (!CallGuest(c, fn, {Arg(c, 0), type, name, lang, param})) break;
  }
  return 1;
}

// ---- time --------------------------------------------------------------------------

// SYSTEMTIME at st as a broken-down time (milliseconds dropped).
std::tm ReadSystemTime(uint32_t st) {
  std::tm t{};
  t.tm_year = Rd16(st) - 1900;
  t.tm_mon = Rd16(st + 2) - 1;
  t.tm_wday = Rd16(st + 4);
  t.tm_mday = Rd16(st + 6);
  t.tm_hour = Rd16(st + 8);
  t.tm_min = Rd16(st + 10);
  t.tm_sec = Rd16(st + 12);
  return t;
}

HLE("kernel32.dll", CompareFileTime, 2) {
  uint64_t a = Rd64(Arg(c, 0)), b = Rd64(Arg(c, 1));
  return a < b ? 0xFFFFFFFF : a > b ? 1 : 0;
}

HLE64("kernel32.dll", FileTimeToLocalFileTime, 2) {
  time_t now = time(nullptr);
  std::tm lt{};
  localtime_r(&now, &lt);
  int64_t off = int64_t(lt.tm_gmtoff) * 10000000;
  Wr64(Arg(c, 1), uint64_t(int64_t(Rd64(Arg(c, 0))) + off));
  return 1;
}

// FileTimeToDosDateTime(ft, &date, &time): the FILETIME's fields as
// they are (no time zone conversion), DOS-packed; years 1980-2107 only.
HLE64("kernel32.dll", FileTimeToDosDateTime, 3) {
  uint64_t ft = Rd64(Arg(c, 0));
  time_t secs = time_t(int64_t(ft - kFileTimeUnixEpoch) / 10000000);
  std::tm t{};
  if (ft < kFileTimeUnixEpoch || !gmtime_r(&secs, &t) || t.tm_year < 80 || t.tm_year > 207) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  Wr16(Arg(c, 1), uint16_t((t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday));
  Wr16(Arg(c, 2), uint16_t(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2));
  return 1;
}

HLE("kernel32.dll", FileTimeToSystemTime, 2) {
  uint64_t ft = Rd64(Arg(c, 0));
  if (ft < kFileTimeUnixEpoch) return 0;
  time_t secs = time_t((ft - kFileTimeUnixEpoch) / 10000000);
  std::tm t{};
  gmtime_r(&secs, &t);
  WriteSystemTime(Arg(c, 1), t, int((ft / 10000) % 1000));
  return 1;
}

HLE("kernel32.dll", SystemTimeToFileTime, 2) {
  uint32_t st = Arg(c, 0);
  std::tm t = ReadSystemTime(st);
  time_t secs = timegm(&t);
  Wr64(Arg(c, 1),
       uint64_t(secs) * 10000000 + uint64_t(Rd16(st + 14)) * 10000 + kFileTimeUnixEpoch);
  return 1;
}

// The SYSTEMTIME a Get*Format call formats; null means now (local).
std::tm TmFromSystemTime(uint32_t st) {
  if (st) return ReadSystemTime(st);
  std::tm t{};
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  return t;
}

// en-US defaults: M/d/yyyy and h:mm:ss tt. Explicit pictures are not
// translated (the game passes NULL for the format).
HLE("kernel32.dll", GetDateFormatA, 6) {
  std::tm t = TmFromSystemTime(Arg(c, 2));
  char out[64];
  if (Arg(c, 1) & 2)  // DATE_LONGDATE
    std::strftime(out, sizeof out, "%A, %B %d, %Y", &t);
  else
    std::snprintf(out, sizeof out, "%d/%d/%d", t.tm_mon + 1, t.tm_mday, t.tm_year + 1900);
  if (Arg(c, 3)) WarnF("GetDateFormatA: custom picture '%s' ignored\n", GStr(Arg(c, 3)).c_str());
  return PutCounted(Arg(c, 4), Arg(c, 5), out, false);
}

HLE("kernel32.dll", GetTimeFormatA, 6) {
  std::tm t = TmFromSystemTime(Arg(c, 2));
  char out[64];
  int h = t.tm_hour % 12;
  if (Arg(c, 1) & 2)  // TIME_NOSECONDS
    std::snprintf(out, sizeof out, "%d:%02d %s", h ? h : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  else
    std::snprintf(out, sizeof out, "%d:%02d:%02d %s", h ? h : 12, t.tm_min, t.tm_sec,
                  t.tm_hour < 12 ? "AM" : "PM");
  if (Arg(c, 3)) WarnF("GetTimeFormatA: custom picture '%s' ignored\n", GStr(Arg(c, 3)).c_str());
  return PutCounted(Arg(c, 4), Arg(c, 5), out, false);
}

// ---- strings -------------------------------------------------------------------------

HLE64("kernel32.dll", lstrlenA, 1) {
  return Arg(c, 0) ? std::strlen(g_proc->mem.ptr<const char>(Arg(c, 0))) : 0;
}

HLE64("kernel32.dll", lstrcpyA, 2) {
  PutStr(Arg(c, 0), GStr(Arg(c, 1)));
  return Arg(c, 0);
}

HLE("kernel32.dll", lstrcatA, 2) {
  PutStr(Arg(c, 0), GStr(Arg(c, 0)) + GStr(Arg(c, 1)));
  return Arg(c, 0);
}

HLE("kernel32.dll", lstrlenW, 1) { return Arg(c, 0) ? GWStr(Arg(c, 0)).size() : 0; }

// CompareStringA/W(Locale, dwCmpFlags, s1, cch1, s2, cch2): CSTR_LESS_THAN
// (1), CSTR_EQUAL (2) or CSTR_GREATER_THAN (3); 0 on bad arguments. A count
// of -1 means NUL-terminated. Ordinal order of the characters, with
// NORM_IGNORECASE (1) folding ASCII letters; the other flags (kana, width,
// symbols) do not change ASCII text.
template <typename Str>
uint64_t CompareStrings(uint32_t flags, const Str& a, const Str& b) {
  const bool fold = flags & 1;
  auto ch = [fold](uint32_t x) { return fold && x >= 'a' && x <= 'z' ? x - 32 : x; };
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; i++) {
    const uint32_t x = ch(uint32_t(a[i])), y = ch(uint32_t(b[i]));
    if (x != y) return x < y ? 1 : 3;
  }
  return a.size() == b.size() ? 2 : a.size() < b.size() ? 1 : 3;
}

HLE("kernel32.dll", CompareStringA, 6) {
  if (!Arg(c, 2) || !Arg(c, 4)) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  auto get = [](gaddr p, int32_t n) {
    std::string s = GStr(p);
    if (n >= 0) s = std::string(g_proc->mem.ptr<const char>(p), size_t(n));
    // Comparison stops at a NUL even inside an explicit count.
    return s.substr(0, s.find('\0'));
  };
  std::string a = get(Arg(c, 2), int32_t(Arg(c, 3))), b = get(Arg(c, 4), int32_t(Arg(c, 5)));
  std::vector<uint8_t> ua(a.begin(), a.end()), ub(b.begin(), b.end());
  return CompareStrings(uint32_t(Arg(c, 1)), ua, ub);
}

HLE("kernel32.dll", CompareStringW, 6) {
  if (!Arg(c, 2) || !Arg(c, 4)) {
    SetLastError(kErrorInvalidParameter);
    return 0;
  }
  auto get = [](gaddr p, int32_t n) {
    std::u16string s = GWStr(p);
    if (n >= 0) {
      s.clear();
      for (int32_t i = 0; i < n; i++) s.push_back(char16_t(Rd16(p + 2 * i)));
    }
    return s.substr(0, s.find(u'\0'));
  };
  return CompareStrings(uint32_t(Arg(c, 1)), get(Arg(c, 2), int32_t(Arg(c, 3))),
                        get(Arg(c, 4), int32_t(Arg(c, 5))));
}

// (uAction, lpMessageText): ends the process with the message.
HLE("kernel32.dll", FatalAppExitA, 2) {
  FatalF("FatalAppExitA: %s", Arg(c, 1) ? GStr(Arg(c, 1)).c_str() : "");
}

HLE("kernel32.dll", IsDBCSLeadByte, 1) { return IsLeadByte(kCpAcp, uint8_t(Arg(c, 0))) ? 1 : 0; }
HLE("kernel32.dll", IsDBCSLeadByteEx, 2) {
  return IsLeadByte(Arg(c, 0), uint8_t(Arg(c, 1))) ? 1 : 0;
}

HLE("kernel32.dll", VerLanguageNameA, 3) {
  std::string n = (Arg(c, 0) & 0xFFFF) == 0x0409 ? "English (United States)"
                                                 : "Language Neutral";
  uint32_t buf = Arg(c, 1), cap = Arg(c, 2);
  if (!cap) return uint32_t(n.size());
  n = n.substr(0, cap - 1);
  PutStr(buf, n);
  return uint32_t(n.size());
}

HLE("kernel32.dll", FormatMessageA, 7) {
  uint32_t flags = Arg(c, 0), id = Arg(c, 2), buf = Arg(c, 4), cap = Arg(c, 5);
  if (!(flags & 0x1000)) {  // only FORMAT_MESSAGE_FROM_SYSTEM
    WarnF("FormatMessageA(flags %X): only FROM_SYSTEM is supported\n", flags);
    return 0;
  }
  std::string s = "Error " + std::to_string(id) + ".\r\n";
  if (flags & 0x100) {  // FORMAT_MESSAGE_ALLOCATE_BUFFER
    uint32_t p = HeapAllocProcess(uint32_t(s.size() + 1), false);
    PutStr(p, s);
    Wr32(buf, p);
    return uint32_t(s.size());
  }
  if (s.size() + 1 > cap) return 0;
  PutStr(buf, s);
  return uint32_t(s.size());
}

// ---- console, processes, debugging ------------------------------------------------------

HLE("kernel32.dll", GetConsoleTitleA, 2) {
  SetLastError(kErrorInvalidHandle);  // no console attached
  return 0;
}
HLE("kernel32.dll", SetConsoleTitleA, 1) { return 1; }

HLE("kernel32.dll", CreateProcessA, 10) {
  WarnF("CreateProcessA(%s, %s): not supported\n", GStr(Arg(c, 0)).c_str(),
        GStr(Arg(c, 1)).c_str());
  SetLastError(kErrorFileNotFound);
  return 0;
}

HLE("kernel32.dll", FlushInstructionCache, 3) { return 1; }

HLE64("kernel32.dll", DebugBreak, 0) {
  FatalF("DebugBreak() called from %08llX", (unsigned long long)RdPtr(c.rsp));
}

// ---- version.dll ------------------------------------------------------------
// Only the game's own EXE has version info we can serve; it comes straight
// from the mapped .rsrc section.

// Finds the first RT_VERSION resource in the mapped image. Returns its
// guest VA and size.
bool FindVersionResource(uint32_t* va, uint32_t* size) {
  uint32_t e = FindResourceImpl("#16", "", 0);  // RT_VERSION, first name/lang
  if (!e) return false;
  *va = g_proc->img.image_base + Rd32(e);
  *size = Rd32(e + 4);
  return true;
}

bool IsOwnExe(const std::string& path) {
  std::string l = Lower(path), exe = Lower(Game().exe_name);
  size_t slash = l.find_last_of("\\/");
  return l.substr(slash == std::string::npos ? 0 : slash + 1) == exe;
}

HLE("version.dll", GetFileVersionInfoSizeA, 2) {
  uint32_t va, size;
  if (Arg(c, 1)) Wr32(Arg(c, 1), 0);
  if (!IsOwnExe(GStr(Arg(c, 0))) || !FindVersionResource(&va, &size)) {
    SetLastError(kErrorFileNotFound);
    return 0;
  }
  // Room after the resource for ANSI copies handed out by VerQueryValueA.
  return size * 3;
}

HLE("version.dll", GetFileVersionInfoA, 4) {
  uint32_t va, size;
  uint32_t len = Arg(c, 2), buf = Arg(c, 3);
  if (!IsOwnExe(GStr(Arg(c, 0))) || !FindVersionResource(&va, &size)) {
    SetLastError(kErrorFileNotFound);
    return 0;
  }
  std::memcpy(GPtr(buf), GPtr(va), std::min(len, size));
  // Remember where the scratch area starts (stored after the resource).
  if (len >= size + 8) Wr32(buf + size, size + 8);
  return 1;
}

// Walks a VS_VERSIONINFO block tree (UTF-16 keys) along a backslash path.
bool VersionLookup(uint32_t block, const std::vector<std::string>& path,
                   size_t depth, uint32_t* value, uint32_t* vlen, bool* text) {
  uint16_t length = Rd16(block), value_len = Rd16(block + 2), type = Rd16(block + 4);
  std::u16string key = GWStr(block + 6);
  uint32_t p = block + 6 + uint32_t(key.size() + 1) * 2;
  p = (p + 3) & ~3u;
  if (depth == path.size()) {
    *value = p;
    *vlen = value_len;
    *text = type == 1;
    return true;
  }
  uint32_t child = (p + (type == 1 ? value_len * 2u : value_len) + 3) & ~3u;
  uint32_t end = block + length;
  while (child < end) {
    uint16_t clen = Rd16(child);
    if (!clen) break;
    if (Lower(Narrow(GWStr(child + 6))) == Lower(path[depth]))
      return VersionLookup(child, path, depth + 1, value, vlen, text);
    child = (child + clen + 3) & ~3u;
  }
  return false;
}

HLE("version.dll", VerQueryValueA, 4) {
  uint32_t buf = Arg(c, 0), out_ptr = Arg(c, 2), out_len = Arg(c, 3);
  std::string sub = GStr(Arg(c, 1));
  std::vector<std::string> path;
  std::string cur;
  for (char ch : sub) {
    if (ch == '\\') {
      if (!cur.empty()) path.push_back(cur);
      cur.clear();
    } else {
      cur += ch;
    }
  }
  if (!cur.empty()) path.push_back(cur);
  uint32_t value, vlen;
  bool text;
  if (!VersionLookup(buf, path, 0, &value, &vlen, &text)) return 0;
  if (text) {
    // Hand out an ANSI copy from the scratch area after the resource.
    uint32_t size = Rd16(buf);
    uint32_t scratch_off = Rd32(buf + size);
    std::string s = Narrow(GWStr(value));
    uint32_t dst = buf + scratch_off;
    PutStr(dst, s);
    Wr32(buf + size, scratch_off + uint32_t(s.size()) + 1);
    Wr32(out_ptr, dst);
    Wr32(out_len, uint32_t(s.size() + 1));
  } else {
    Wr32(out_ptr, value);
    Wr32(out_len, vlen);
  }
  return 1;
}

}  // namespace
}  // namespace recomp::hle
