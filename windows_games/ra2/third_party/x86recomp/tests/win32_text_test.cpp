// UTF-8 <-> UTF-16 (runtime/hle/win32.h): the Unicode standard's example
// of U+FFFD substitution by maximal subparts, the other ill-formed classes
// (overlong, surrogate, beyond U+10FFFF, truncated), unpaired surrogates,
// and a round trip of every scalar value.

#include <cstdio>
#include <string>
#include <vector>

#include "runtime/hle/win32.h"

namespace {

using recomp::win32::Utf16ToUtf8;
using recomp::win32::Utf8ToUtf16;

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
  }
}

std::u16string Dec(const std::vector<uint8_t>& b, bool* bad) {
  return Utf8ToUtf16(b.data(), b.size(), bad);
}

}  // namespace

int main() {
  bool bad;
  // The Unicode standard, chapter 3, "U+FFFD Substitution of Maximal
  // Subparts" (table 3-8).
  Check(Dec({0x61, 0xF1, 0x80, 0x80, 0xE1, 0x80, 0xC2, 0x62, 0x80, 0x63, 0x80, 0xBF, 0x64},
            &bad) == u"a���b�c��d" &&
            bad,
        "table 3-8 example");
  Check(Dec({0xC0, 0xAF}, &bad) == u"��" && bad, "overlong C0 AF");
  Check(Dec({0xE0, 0x80, 0xAF}, &bad) == u"���" && bad, "overlong E0 80 AF");
  Check(Dec({0xED, 0xA0, 0x80}, &bad) == u"���" && bad, "encoded surrogate");
  Check(Dec({0xF4, 0x90, 0x80, 0x80}, &bad) == u"����" && bad,
        "beyond U+10FFFF");
  Check(Dec({0xF0, 0x9F, 0x98}, &bad) == u"�" && bad, "truncated at the end");
  Check(Dec({0xF0, 0x9F, 0x98, 0x80, 0x00}, &bad) == std::u16string(u"\U0001F600") + u'\0' &&
            !bad,
        "supplementary + NUL");

  std::u16string lone = u"a";
  lone.push_back(0xD800);
  lone.push_back(u'b');
  lone.push_back(0xDC00);
  Check(Utf16ToUtf8(lone.data(), lone.size(), &bad) == "a\xEF\xBF\xBD" "b\xEF\xBF\xBD" && bad,
        "unpaired surrogates");

  for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
    if (cp >= 0xD800 && cp < 0xE000) continue;
    std::u16string w;
    if (cp >= 0x10000) {
      w.push_back(char16_t(0xD800 + ((cp - 0x10000) >> 10)));
      w.push_back(char16_t(0xDC00 + ((cp - 0x10000) & 0x3FF)));
    } else {
      w.push_back(char16_t(cp));
    }
    bool b1, b2;
    std::string u = Utf16ToUtf8(w.data(), w.size(), &b1);
    size_t want = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (b1 || u.size() != want ||
        Utf8ToUtf16(reinterpret_cast<const uint8_t*>(u.data()), u.size(), &b2) != w || b2) {
      std::printf("FAIL: round trip U+%04X\n", cp);
      g_failures++;
      break;
    }
  }

  if (g_failures) return 1;
  std::printf("win32_text: ok\n");
  return 0;
}
