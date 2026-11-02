// ANSI code pages (runtime/hle/codepage.h): Windows-1252's 0x80-0x9F
// block, and Big5 (code page 950) DBCS pairs from the Big5 standard's
// table (中 A4A4, 文 A4E5, 遊 B943, 戲 C0B8), lead bytes, undefined pairs
// and the default character.

#include <cstdio>
#include <string>

#include "runtime/hle/codepage.h"
#include "runtime/runtime.h"

namespace recomp {
// codepage.cpp reads the ANSI code page from the game's config.
const GameConfig& Game() {
  static GameConfig g = [] {
    GameConfig c;
    c.ansi_code_page = 950;
    return c;
  }();
  return g;
}
}  // namespace recomp

namespace {

using namespace recomp::win32;

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_failures++;
  }
}

std::u16string Dec(uint32_t cp, const std::string& s) {
  return MultiByteToUtf16(cp, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string Enc(uint32_t cp, const std::u16string& w, bool* used) {
  return Utf16ToMultiByte(cp, w.data(), w.size(), {}, used);
}

}  // namespace

int main() {
  bool used = false;
  // Windows-1252: 0x80 is the euro sign, 0x9F Y with diaeresis.
  Check(Dec(kCp1252, "\x80\x9F" "A") == u"€ŸA", "1252 decode");
  Check(Enc(kCp1252, u"€ŸA", &used) == "\x80\x9F" "A" && !used, "1252 encode");
  Check(Enc(kCp1252, u"中", &used) == "?" && used, "1252 default char");

  // The ANSI code page is the config's.
  Check(AnsiCodePage() == 950 && OemCodePage() == 950, "ACP/OEMCP 950");
  Check(ResolveCodePage(kCpAcp) == 950, "CP_ACP");
  if (!CodePageAvailable(950)) {
    std::printf("SKIP: the host's iconv has no CP950\n");
    return g_failures ? 1 : 0;
  }
  Check(MaxCharSize(950) == 2, "950 is DBCS");
  Check(IsLeadByte(950, 0xA4) && IsLeadByte(950, 0x81) && IsLeadByte(950, 0xFE), "lead bytes");
  Check(!IsLeadByte(950, 0x41) && !IsLeadByte(950, 0x80), "not lead bytes");
  uint8_t ranges[10] = {};
  Check(LeadByteRanges(950, ranges, 5) == 1 && ranges[0] == 0x81 && ranges[1] == 0xFE,
        "lead byte range 81-FE");

  const std::string big5 = "\xA4\xA4\xA4\xE5" "A" "\xB9\x43\xC0\xB8";
  Check(Dec(950, big5) == u"中文A遊戲", "950 decode");
  Check(Enc(950, u"中文A遊戲", &used) == big5 && !used, "950 encode");
  // A lead byte at the end of the input, and a character Big5 lacks.
  Check(Dec(950, "A\xA4") == u"A?", "950 trailing lead byte");
  Check(Enc(950, u"é", &used) == "?" && used, "950 default char");
  Check(Utf16ToMultiByte(950, u"é", 1, "\xA1\x48", &used) == "\xA1\x48",
        "950 given default char");

  if (g_failures) return 1;
  std::printf("codepage: all passed\n");
  return 0;
}
