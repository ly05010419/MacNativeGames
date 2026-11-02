#pragma once
// Win32 constants and small helpers shared by the HLE modules.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/hle/codepage.h"

namespace recomp::win32 {

// Error codes.
inline constexpr uint32_t kErrorSuccess = 0;
inline constexpr uint32_t kErrorFileNotFound = 2;
inline constexpr uint32_t kErrorPathNotFound = 3;
inline constexpr uint32_t kErrorAccessDenied = 5;
inline constexpr uint32_t kErrorInvalidHandle = 6;
inline constexpr uint32_t kErrorNotEnoughMemory = 8;
inline constexpr uint32_t kErrorNoMoreFiles = 18;
inline constexpr uint32_t kErrorFileExists = 80;
inline constexpr uint32_t kErrorInvalidParameter = 87;
inline constexpr uint32_t kErrorInsufficientBuffer = 122;
inline constexpr uint32_t kErrorModNotFound = 126;
inline constexpr uint32_t kErrorProcNotFound = 127;
inline constexpr uint32_t kErrorAlreadyExists = 183;
inline constexpr uint32_t kErrorEnvVarNotFound = 203;
inline constexpr uint32_t kErrorMoreData = 234;
inline constexpr uint32_t kErrorTooManyPosts = 298;
inline constexpr uint32_t kErrorInvalidAddress = 487;
inline constexpr uint32_t kErrorDllInitFailed = 1114;
inline constexpr uint32_t kErrorClassAlreadyExists = 1410;
inline constexpr uint32_t kErrorTimeout = 1460;
inline constexpr uint32_t kErrorCannotFindWndClass = 1407;
inline constexpr uint32_t kErrorResourceNameNotFound = 1814;
inline constexpr uint32_t kErrorBufferOverflow = 111;

// HRESULTs.
inline constexpr uint32_t kEInvalidArg = 0x80070057;

// Memory.
inline constexpr uint32_t kMemCommit = 0x1000;
inline constexpr uint32_t kMemReserve = 0x2000;
inline constexpr uint32_t kMemDecommit = 0x4000;
inline constexpr uint32_t kMemRelease = 0x8000;
inline constexpr uint32_t kMemFree = 0x10000;
inline constexpr uint32_t kMemPrivate = 0x20000;
inline constexpr uint32_t kMemTopDown = 0x100000;
inline constexpr uint32_t kMemImage = 0x1000000;
inline constexpr uint32_t kPageNoAccess = 0x01;
inline constexpr uint32_t kPageReadOnly = 0x02;
inline constexpr uint32_t kPageReadWrite = 0x04;
inline constexpr uint32_t kPageWriteCopy = 0x08;
inline constexpr uint32_t kPageExecute = 0x10;
inline constexpr uint32_t kPageExecuteRead = 0x20;
inline constexpr uint32_t kPageExecuteReadWrite = 0x40;
inline constexpr uint32_t kHeapZeroMemory = 0x08;
inline constexpr uint32_t kHeapReallocInPlaceOnly = 0x10;

inline constexpr uint32_t kCtCtype1 = 1;
inline constexpr uint32_t kLcmapLowercase = 0x100;
inline constexpr uint32_t kLcmapUppercase = 0x200;

inline std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char ch) { return std::tolower(ch); });
  return s;
}
inline std::string Upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char ch) { return std::toupper(ch); });
  return s;
}

// Windows-1252 <-> UTF-16 (BMP). 0x80-0x9F differ from Latin-1.
inline uint16_t Cp1252ToUnicode(uint8_t b) {
  static const uint16_t k80[32] = {
      0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
      0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
      0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
      0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
  return b >= 0x80 && b < 0xA0 ? k80[b - 0x80] : b;
}

inline uint8_t UnicodeToCp1252(uint16_t u, bool* ok) {
  *ok = true;
  if (u < 0x80 || (u >= 0xA0 && u < 0x100)) return uint8_t(u);
  for (int b = 0x80; b < 0xA0; b++)
    if (Cp1252ToUnicode(uint8_t(b)) == u) return uint8_t(b);
  *ok = false;
  return '?';
}

// ANSI (the ANSI code page, codepage.h) <-> UTF-16.
inline std::u16string Widen(const std::string& s) {
  return MultiByteToUtf16(kCpAcp, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

inline std::string Narrow(const std::u16string& w) {
  return Utf16ToMultiByte(kCpAcp, w.data(), w.size(), {}, nullptr);
}

// A command line split into arguments as the MSVC CRT and
// CommandLineToArgvW do. The program name runs to the closing quote or the
// first blank, with no escapes. In the other arguments 2n backslashes
// before a quote are n backslashes and the quote toggles quoting, 2n+1 are
// n and a literal quote, "" inside quotes is a quote, and other
// backslashes are literal.
template <typename S>
std::vector<S> SplitCommandLine(const S& cmd) {
  using Ch = typename S::value_type;
  std::vector<S> args;
  size_t i = 0, n = cmd.size();
  auto blank = [&](size_t k) { return k < n && (cmd[k] == Ch(' ') || cmd[k] == Ch('\t')); };
  S prog;
  if (i < n && cmd[i] == Ch('"')) {
    for (i++; i < n && cmd[i] != Ch('"'); i++) prog.push_back(cmd[i]);
    if (i < n) i++;
  } else {
    for (; i < n && !blank(i); i++) prog.push_back(cmd[i]);
  }
  args.push_back(prog);
  while (blank(i)) i++;
  while (i < n) {
    S a;
    bool quoted = false;
    while (i < n && (quoted || !blank(i))) {
      if (cmd[i] == Ch('\\')) {
        size_t k = 0;
        while (i < n && cmd[i] == Ch('\\')) i++, k++;
        if (i < n && cmd[i] == Ch('"')) {
          a.append(k / 2, Ch('\\'));
          if (k % 2) a.push_back(cmd[i++]);
        } else {
          a.append(k, Ch('\\'));
        }
      } else if (cmd[i] == Ch('"')) {
        if (quoted && i + 1 < n && cmd[i + 1] == Ch('"')) {
          a.push_back(Ch('"'));
          i += 2;
        } else {
          quoted = !quoted;
          i++;
        }
      } else {
        a.push_back(cmd[i++]);
      }
    }
    args.push_back(a);
    while (blank(i)) i++;
  }
  return args;
}

// ANSI text as UTF-8, for host APIs that take UTF-8 (SDL window titles,
// message boxes). macOS rejects titles that are not valid UTF-8.
std::string AnsiToUtf8(const std::string& s);

// UTF-8 -> UTF-16. Ill-formed input becomes U+FFFD, one per maximal
// subpart (the Unicode-recommended practice); *bad tells whether any was.
inline std::u16string Utf8ToUtf16(const uint8_t* s, size_t n, bool* bad) {
  std::u16string w;
  *bad = false;
  for (size_t i = 0; i < n;) {
    uint8_t b = s[i];
    if (b < 0x80) {
      w.push_back(b);
      i++;
      continue;
    }
    // The sequence length and the range of the second byte (Unicode table
    // 3-7); later bytes are 80-BF.
    int len = 0;
    uint8_t lo = 0x80, hi = 0xBF;
    if (b >= 0xC2 && b <= 0xDF) len = 2;
    else if (b == 0xE0) len = 3, lo = 0xA0;
    else if (b == 0xED) len = 3, hi = 0x9F;
    else if (b >= 0xE1 && b <= 0xEF) len = 3;
    else if (b == 0xF0) len = 4, lo = 0x90;
    else if (b >= 0xF1 && b <= 0xF3) len = 4;
    else if (b == 0xF4) len = 4, hi = 0x8F;
    uint32_t cp = len == 2 ? b & 0x1F : len == 3 ? b & 0x0F : b & 0x07;
    size_t k = 1;
    for (; len && k < size_t(len); k++) {
      if (i + k >= n) break;
      uint8_t x = s[i + k];
      if (x < (k == 1 ? lo : 0x80) || x > (k == 1 ? hi : 0xBF)) break;
      cp = cp << 6 | (x & 0x3F);
    }
    if (!len || k < size_t(len)) {
      w.push_back(0xFFFD);
      *bad = true;
      i += k;
      continue;
    }
    if (cp >= 0x10000) {
      w.push_back(char16_t(0xD800 + ((cp - 0x10000) >> 10)));
      w.push_back(char16_t(0xDC00 + ((cp - 0x10000) & 0x3FF)));
    } else {
      w.push_back(char16_t(cp));
    }
    i += len;
  }
  return w;
}

// UTF-16 -> UTF-8. An unpaired surrogate becomes U+FFFD; *bad tells
// whether there was one.
inline std::string Utf16ToUtf8(const char16_t* w, size_t n, bool* bad) {
  std::string s;
  *bad = false;
  for (size_t i = 0; i < n; i++) {
    uint32_t cp = w[i];
    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (w[++i] - 0xDC00);
    } else if (cp >= 0xD800 && cp < 0xE000) {
      cp = 0xFFFD;
      *bad = true;
    }
    if (cp < 0x80) {
      s.push_back(char(cp));
    } else if (cp < 0x800) {
      s.push_back(char(0xC0 | (cp >> 6)));
      s.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      s.push_back(char(0xE0 | (cp >> 12)));
      s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(char(0x80 | (cp & 0x3F)));
    } else {
      s.push_back(char(0xF0 | (cp >> 18)));
      s.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      s.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back(char(0x80 | (cp & 0x3F)));
    }
  }
  return s;
}

// CT_CTYPE1 classification for Latin-1 + the CP1252 extras.
inline uint16_t CharType1(uint16_t u) {
  enum { kUpper = 1, kLower = 2, kDigit = 4, kSpace = 8, kPunct = 0x10,
         kCntrl = 0x20, kBlank = 0x40, kXDigit = 0x80, kAlpha = 0x100 };
  uint16_t t = 0;
  if (u < 0x20 || (u >= 0x7F && u < 0xA0)) t |= kCntrl;
  if (u == ' ' || u == '\t' || u == 0xA0) t |= kBlank;
  if (u == ' ' || (u >= 0x09 && u <= 0x0D) || u == 0xA0) t |= kSpace;
  if (u >= '0' && u <= '9') t |= kDigit | kXDigit;
  if ((u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F')) t |= kXDigit;
  bool upper = (u >= 'A' && u <= 'Z') || (u >= 0xC0 && u <= 0xDE && u != 0xD7) ||
               u == 0x0160 || u == 0x0152 || u == 0x017D || u == 0x0178;
  bool lower = (u >= 'a' && u <= 'z') || (u >= 0xDF && u <= 0xFF && u != 0xF7) ||
               u == 0x0161 || u == 0x0153 || u == 0x017E || u == 0x0192;
  if (upper) t |= kUpper | kAlpha;
  if (lower) t |= kLower | kAlpha;
  if (!t && u > 0x20 && u < 0x7F) t |= kPunct;
  if (!t && u >= 0xA1) t |= kPunct;
  return t;
}

inline uint16_t MapCase(uint16_t u, uint32_t flags) {
  if (flags & kLcmapUppercase) {
    if ((u >= 'a' && u <= 'z') || (u >= 0xE0 && u <= 0xFE && u != 0xF7))
      return u - 0x20;
    if (u == 0x0161 || u == 0x0153 || u == 0x017E) return u - 1;
    if (u == 0xFF) return 0x0178;
  } else if (flags & kLcmapLowercase) {
    if ((u >= 'A' && u <= 'Z') || (u >= 0xC0 && u <= 0xDE && u != 0xD7))
      return u + 0x20;
    if (u == 0x0160 || u == 0x0152 || u == 0x017D) return u + 1;
    if (u == 0x0178) return 0xFF;
  }
  return u;
}

}  // namespace recomp::win32
