#pragma once
// Windows code pages for the ANSI ("A") APIs. The ANSI code page is
// GameConfig::ansi_code_page (Windows-1252 unless a game needs the one of
// its home locale, e.g. a DBCS page for CJK text). Windows-1252 and
// CP_UTF8 are converted here; other code pages through the host's iconv,
// whose tables are read once per code page.

#include <cstddef>
#include <cstdint>
#include <string>

namespace recomp::win32 {

inline constexpr uint32_t kCpAcp = 0, kCpOemCp = 1, kCpMacCp = 2, kCpThreadAcp = 3;
inline constexpr uint32_t kCp1252 = 1252, kCp437 = 437, kCpUtf8 = 65001;

uint32_t AnsiCodePage();
// 437 with Windows-1252, else the ANSI code page (as on CJK Windows).
uint32_t OemCodePage();
// CP_ACP, CP_OEMCP, CP_THREAD_ACP and CP_MACCP to the code page they name.
uint32_t ResolveCodePage(uint32_t cp);
// Whether the code page can be converted (IsValidCodePage).
bool CodePageAvailable(uint32_t cp);

// 1 for single-byte code pages, 2 for DBCS ones.
uint32_t MaxCharSize(uint32_t cp);
bool IsLeadByte(uint32_t cp, uint8_t b);
// The DBCS lead-byte ranges as CPINFO's LeadByte lists them: pairs of
// first and last byte, at most `max_pairs`; returns the pair count.
size_t LeadByteRanges(uint32_t cp, uint8_t* out, size_t max_pairs);

// Not for CP_UTF8. Undefined bytes and pairs become '?'.
std::u16string MultiByteToUtf16(uint32_t cp, const uint8_t* s, size_t n);
// Not for CP_UTF8. Characters the code page lacks become `default_char`
// (one or two bytes; '?' if empty), and set *used_default.
std::string Utf16ToMultiByte(uint32_t cp, const char16_t* w, size_t n,
                             const std::string& default_char, bool* used_default);

}  // namespace recomp::win32
