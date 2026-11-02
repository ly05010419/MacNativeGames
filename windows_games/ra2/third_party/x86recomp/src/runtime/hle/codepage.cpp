#include "runtime/hle/codepage.h"

#include <iconv.h>

#include <cerrno>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "runtime/log.h"
#include "runtime/runtime.h"
#include "runtime/hle/win32.h"

namespace recomp::win32 {
namespace {

constexpr char16_t kUndefined = u'?';

// A code page's mapping: single bytes, DBCS lead bytes and the characters
// of the pairs they start, and the reverse.
struct Table {
  bool dbcs = false;
  char16_t single[256] = {};
  bool single_ok[256] = {};
  bool lead[256] = {};
  std::vector<char16_t> pair;  // lead << 8 | trail; 0 = undefined
  std::unordered_map<char16_t, uint16_t> to_mb;  // byte, or lead << 8 | trail
};

// One character of `cp` through iconv: its UTF-16 unit, or why not.
enum class Decode { Ok, Incomplete, Invalid };
Decode IconvChar(iconv_t cd, const uint8_t* in, size_t n, char16_t* out) {
  iconv(cd, nullptr, nullptr, nullptr, nullptr);
  char* ip = reinterpret_cast<char*>(const_cast<uint8_t*>(in));
  uint8_t buf[8];
  char* op = reinterpret_cast<char*>(buf);
  size_t il = n, ol = sizeof buf;
  if (iconv(cd, &ip, &il, &op, &ol) == size_t(-1))
    return errno == EINVAL ? Decode::Incomplete : Decode::Invalid;
  if (il != 0 || sizeof buf - ol != 2) return Decode::Invalid;  // not one BMP unit
  *out = char16_t(buf[0] | buf[1] << 8);
  return Decode::Ok;
}

std::unique_ptr<Table> BuildCp1252() {
  auto t = std::make_unique<Table>();
  for (int b = 0; b < 256; b++) {
    t->single[b] = Cp1252ToUnicode(uint8_t(b));
    t->single_ok[b] = true;
    t->to_mb.emplace(t->single[b], uint16_t(b));
  }
  return t;
}

// The lead bytes Windows' DBCS code pages declare (GetCPInfo), which can
// be wider than a host table's: hosts may leave the user-defined rows
// (e.g. Big5's 0x81-0xA0) out, and a pair there is still one character.
struct LeadRange {
  uint32_t cp;
  uint8_t lo, hi;
};
constexpr LeadRange kWindowsLeads[] = {
    {932, 0x81, 0x9F}, {932, 0xE0, 0xFC},  // Shift JIS
    {936, 0x81, 0xFE},                     // GBK
    {949, 0x81, 0xFE},                     // Unified Hangul Code
    {950, 0x81, 0xFE},                     // Big5
};

std::unique_ptr<Table> BuildIconv(uint32_t cp) {
  const std::string name = "CP" + std::to_string(cp);
  iconv_t cd = iconv_open("UTF-16LE", name.c_str());
  if (cd == iconv_t(-1)) return nullptr;
  auto t = std::make_unique<Table>();
  for (int b = 0; b < 256; b++) {
    const uint8_t in = uint8_t(b);
    char16_t ch = 0;
    switch (IconvChar(cd, &in, 1, &ch)) {
      case Decode::Ok:
        t->single[b] = ch;
        t->single_ok[b] = true;
        break;
      case Decode::Incomplete:
        t->lead[b] = t->dbcs = true;
        break;
      case Decode::Invalid:
        t->single[b] = kUndefined;
        break;
    }
  }
  for (const LeadRange& r : kWindowsLeads) {
    if (r.cp != cp) continue;
    for (int b = r.lo; b <= r.hi; b++) {
      t->lead[b] = t->dbcs = true;
      t->single[b] = kUndefined;
      t->single_ok[b] = false;
    }
  }
  if (t->dbcs) {
    t->pair.assign(0x10000, 0);
    for (int l = 0; l < 256; l++) {
      if (!t->lead[l]) continue;
      for (int r = 1; r < 256; r++) {
        const uint8_t in[2] = {uint8_t(l), uint8_t(r)};
        char16_t ch = 0;
        if (IconvChar(cd, in, 2, &ch) == Decode::Ok) t->pair[l << 8 | r] = ch;
      }
    }
  }
  iconv_close(cd);
  // Reverse: single bytes first, then pairs; the first byte sequence for
  // a character wins.
  for (int b = 0; b < 256; b++)
    if (t->single_ok[b]) t->to_mb.emplace(t->single[b], uint16_t(b));
  for (size_t i = 0; i < t->pair.size(); i++)
    if (t->pair[i]) t->to_mb.emplace(t->pair[i], uint16_t(i));
  return t;
}

// The table for a resolved code page; nullptr if the host cannot convert
// it. Windows-1252 (and 437, which has always shared its table here)
// need no iconv.
const Table* TableFor(uint32_t cp) {
  static std::mutex mu;
  static std::map<uint32_t, std::unique_ptr<Table>> tables;
  static std::map<uint32_t, bool> missing;
  std::lock_guard<std::mutex> l(mu);
  if (cp == kCp437) cp = kCp1252;
  auto it = tables.find(cp);
  if (it != tables.end()) return it->second.get();
  if (missing.count(cp)) return nullptr;
  std::unique_ptr<Table> t = cp == kCp1252 ? BuildCp1252() : BuildIconv(cp);
  if (!t) {
    missing[cp] = true;
    return nullptr;
  }
  return (tables[cp] = std::move(t)).get();
}

// The table to convert with: the code page's, or Windows-1252's for one
// the host lacks (what every code page was converted as before).
const Table& Usable(uint32_t cp) {
  cp = ResolveCodePage(cp);
  if (const Table* t = TableFor(cp)) return *t;
  static bool warned = false;
  if (!warned) {
    warned = true;
    WarnF("codepage: code page %u not available on the host; converting as 1252\n", cp);
  }
  return *TableFor(kCp1252);
}

}  // namespace

uint32_t AnsiCodePage() {
  const uint32_t cp = Game().ansi_code_page;
  return cp ? cp : kCp1252;
}

uint32_t OemCodePage() {
  const uint32_t acp = AnsiCodePage();
  return acp == kCp1252 ? kCp437 : acp;
}

uint32_t ResolveCodePage(uint32_t cp) {
  switch (cp) {
    case kCpAcp: case kCpThreadAcp: case kCpMacCp: return AnsiCodePage();
    case kCpOemCp: return OemCodePage();
    default: return cp;
  }
}

bool CodePageAvailable(uint32_t cp) {
  cp = ResolveCodePage(cp);
  return cp == kCpUtf8 || TableFor(cp) != nullptr;
}

uint32_t MaxCharSize(uint32_t cp) { return Usable(cp).dbcs ? 2 : 1; }

bool IsLeadByte(uint32_t cp, uint8_t b) { return Usable(cp).lead[b]; }

size_t LeadByteRanges(uint32_t cp, uint8_t* out, size_t max_pairs) {
  const Table& t = Usable(cp);
  size_t n = 0;
  for (int b = 0; b < 256 && n < max_pairs;) {
    if (!t.lead[b]) {
      b++;
      continue;
    }
    int e = b;
    while (e + 1 < 256 && t.lead[e + 1]) e++;
    out[2 * n] = uint8_t(b);
    out[2 * n + 1] = uint8_t(e);
    n++;
    b = e + 1;
  }
  return n;
}

std::u16string MultiByteToUtf16(uint32_t cp, const uint8_t* s, size_t n) {
  const Table& t = Usable(cp);
  std::u16string w;
  w.reserve(n);
  for (size_t i = 0; i < n; i++) {
    const uint8_t b = s[i];
    if (!t.lead[b]) {
      w.push_back(t.single[b]);
    } else if (i + 1 < n && s[i + 1]) {
      const char16_t ch = t.pair[b << 8 | s[i + 1]];
      w.push_back(ch ? ch : kUndefined);
      i++;
    } else {
      w.push_back(kUndefined);  // a lead byte at the end
    }
  }
  return w;
}

std::string AnsiToUtf8(const std::string& s) {
  const std::u16string w = Widen(s);
  bool bad;
  return Utf16ToUtf8(w.data(), w.size(), &bad);
}

std::string Utf16ToMultiByte(uint32_t cp, const char16_t* w, size_t n,
                             const std::string& default_char, bool* used_default) {
  const Table& t = Usable(cp);
  const std::string dflt = default_char.empty() ? std::string(1, '?') : default_char;
  std::string s;
  s.reserve(n);
  if (used_default) *used_default = false;
  for (size_t i = 0; i < n; i++) {
    auto it = t.to_mb.find(w[i]);
    if (it == t.to_mb.end()) {
      s += dflt;
      if (used_default) *used_default = true;
    } else if (it->second > 0xFF) {
      s.push_back(char(it->second >> 8));
      s.push_back(char(it->second & 0xFF));
    } else {
      s.push_back(char(it->second));
    }
  }
  return s;
}

}  // namespace recomp::win32
