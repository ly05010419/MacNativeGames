// The UCRT's stdio for x64 guests: FILE streams on host files and the
// printf/scanf families. A FILE* is a guest pointer to a small opaque block
// (the UCRT's FILE is opaque too); the host stream behind it is kept here.
// Text mode is Windows': reads drop the \r of \r\n and stop at ^Z, writes
// turn \n into \r\n. Formatting follows the UCRT (Visual Studio 2015 and
// later), not the host's printf where they differ: %p is 16 uppercase hex
// digits, NaNs print as nan, nan(ind) or nan(snan).

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/hle/kernel32_state.h"
#include "runtime/hle/ucrt.h"
#include "runtime/hle/win32.h"
#include "runtime/log.h"
#include "runtime/runtime.h"

namespace recomp::hle {
namespace {

struct Stream {
  FILE* f = nullptr;
  bool text = false;
  bool owned = true;  // fclose closes it (not stdin/out/err)
};

std::recursive_mutex g_streams_mu;
std::map<gaddr, Stream> g_streams;  // by guest FILE*

gaddr NewStream(FILE* f, bool text, bool owned) {
  gaddr p = HeapAllocProcess(16, true);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  g_streams[p] = {f, text, owned};
  return p;
}

// The three standard streams (__acrt_iob_func(0..2)).
gaddr StdStream(uint32_t i) {
  static gaddr s[3] = {NewStream(stdin, true, false), NewStream(stdout, true, false),
                       NewStream(stderr, true, false)};
  return i < 3 ? s[i] : 0;
}

Stream* Find(gaddr fp) {
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  auto it = g_streams.find(fp);
  return it == g_streams.end() ? nullptr : &it->second;
}

Stream& Must(gaddr fp, const char* api) {
  Stream* s = Find(fp);
  if (!s) FatalF("%s on an unknown FILE* %08llX", api, (unsigned long long)fp);
  return *s;
}

int GetC(Stream& s) {
  int ch = std::fgetc(s.f);
  if (!s.text) return ch;
  if (ch == 0x1A) {  // ^Z ends a text stream
    std::ungetc(ch, s.f);
    return EOF;
  }
  if (ch == '\r') {
    int nx = std::fgetc(s.f);
    if (nx == '\n') return '\n';
    if (nx != EOF) std::ungetc(nx, s.f);
  }
  return ch;
}

size_t Write(Stream& s, const char* p, size_t n) {
  if (!s.text) return std::fwrite(p, 1, n, s.f);
  size_t done = 0;
  for (; done < n; done++) {
    // The host's own stdout and stderr keep plain newlines.
    if (p[done] == '\n' && s.owned && std::fputc('\r', s.f) == EOF) break;
    if (std::fputc(p[done], s.f) == EOF) break;
  }
  return done;
}

// ---- printf ----------------------------------------------------------------

// Successive variadic arguments: 8-byte slots of a guest va_list (floating
// point ones as doubles).
struct Args {
  gaddr p;
  uint64_t Next() {
    uint64_t v = Rd64(p);
    p += 8;
    return v;
  }
  double NextD() {
    uint64_t b = Next();
    double d;
    std::memcpy(&d, &b, 8);
    return d;
  }
};

std::string NanText(double v, bool upper) {
  uint64_t b;
  std::memcpy(&b, &v, 8);
  std::string s = (b >> 63) ? "-" : "";
  bool quiet = b & (1ull << 51);
  if (!quiet)
    s += "nan(snan)";
  else if ((b & 0x7FFFFFFFFFFFFull) == 0x8000000000000ull && (b >> 63))
    s += "nan(ind)";  // the default NaN (0/0): sign set, empty payload
  else
    s += "nan";
  if (upper)
    for (char& ch : s) ch = char(std::toupper(uint8_t(ch)));
  return s;
}

// Pads `body` to `width` (left-justified with '-'), as the host does.
std::string Pad(const std::string& body, int width, bool left) {
  if (int(body.size()) >= width) return body;
  return left ? body + std::string(width - body.size(), ' ')
              : std::string(width - body.size(), ' ') + body;
}

// The UCRT's printf (narrow functions: %s and %c are char, %S and %C
// wchar_t) over a guest format and va_list.
std::string Format(const std::string& fmt, Args a, bool three_digit_exponents) {
  std::string out;
  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%') {
      out.push_back(fmt[i]);
      continue;
    }
    size_t start = i++;
    std::string flags;
    while (i < fmt.size() && std::strchr("-+ #0", fmt[i])) flags.push_back(fmt[i++]);
    int width = -1, prec = -1;
    if (i < fmt.size() && fmt[i] == '*') {
      width = int32_t(a.Next());
      if (width < 0) {
        flags.push_back('-');
        width = -width;
      }
      i++;
    } else {
      while (i < fmt.size() && std::isdigit(uint8_t(fmt[i])))
        width = (width < 0 ? 0 : width) * 10 + (fmt[i++] - '0');
    }
    if (i < fmt.size() && fmt[i] == '.') {
      i++;
      prec = 0;
      if (i < fmt.size() && fmt[i] == '*') {
        prec = int32_t(a.Next());
        i++;
      } else {
        while (i < fmt.size() && std::isdigit(uint8_t(fmt[i]))) prec = prec * 10 + (fmt[i++] - '0');
      }
    }
    // Length: hh h l ll L j z t I I32 I64 w.
    int bits = 32;
    bool wide = false, narrow = false;
    for (;;) {
      if (i >= fmt.size()) break;
      char ch = fmt[i];
      if (ch == 'h') {
        bits = (i + 1 < fmt.size() && fmt[i + 1] == 'h') ? (i++, 8) : 16;
        narrow = true;
        i++;
      } else if (ch == 'l') {
        if (i + 1 < fmt.size() && fmt[i + 1] == 'l') {
          bits = 64;
          i += 2;
        } else {
          wide = true;  // long is 32-bit; %ls/%lc are wide
          i++;
        }
      } else if (ch == 'L' || ch == 'j') {
        bits = 64;
        i++;
      } else if (ch == 'z' || ch == 't') {
        bits = 64;
        i++;
      } else if (ch == 'w') {
        wide = true;
        i++;
      } else if (ch == 'I') {
        if (fmt.compare(i, 3, "I64") == 0) {
          bits = 64;
          i += 3;
        } else if (fmt.compare(i, 3, "I32") == 0) {
          bits = 32;
          i += 3;
        } else {
          bits = 64;  // size_t
          i++;
        }
      } else {
        break;
      }
    }
    if (i >= fmt.size()) {
      out += fmt.substr(start);
      break;
    }
    char conv = fmt[i];
    std::string spec = "%" + flags + (width >= 0 ? std::to_string(width) : "") +
                       (prec >= 0 ? "." + std::to_string(prec) : "");
    char buf[512];
    switch (conv) {
      case '%':
        out.push_back('%');
        break;
      case 'd': case 'i': {
        uint64_t v = a.Next();
        long long s = bits == 64 ? int64_t(v) : bits == 16 ? int16_t(v) : bits == 8 ? int8_t(v)
                                                                             : int32_t(v);
        std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), s);
        out += buf;
        break;
      }
      case 'u': case 'o': case 'x': case 'X': {
        uint64_t v = a.Next();
        unsigned long long u = bits == 64 ? v : bits == 16 ? uint16_t(v) : bits == 8 ? uint8_t(v)
                                                                           : uint32_t(v);
        std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), u);
        out += buf;
        break;
      }
      case 'p': {
        std::snprintf(buf, sizeof buf, "%016llX", (unsigned long long)a.Next());
        out += Pad(buf, width, flags.find('-') != std::string::npos);
        break;
      }
      case 'c': case 'C': {
        uint64_t v = a.Next();
        bool w = conv == 'C' ? !narrow : wide;
        char ch = w ? win32::Narrow(std::u16string(1, char16_t(v)))[0] : char(v);
        out += Pad(std::string(1, ch), width, flags.find('-') != std::string::npos);
        break;
      }
      case 's': case 'S': {
        gaddr p = a.Next();
        bool w = conv == 'S' ? !narrow : wide;
        std::string s = !p ? "(null)" : w ? win32::Narrow(GWStr(p)) : GStr(p);
        if (prec >= 0 && size_t(prec) < s.size()) s.resize(size_t(prec));
        out += Pad(s, width, flags.find('-') != std::string::npos);
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        double v = a.NextD();
        bool upper = std::isupper(uint8_t(conv));
        if (std::isnan(v) || std::isinf(v)) {
          std::string s = std::isnan(v) ? NanText(v, upper)
                                        : std::string(std::signbit(v) ? "-" : "") +
                                              (upper ? "INF" : "inf");
          if (!std::signbit(v) && flags.find('+') != std::string::npos) s = "+" + s;
          else if (!std::signbit(v) && flags.find(' ') != std::string::npos) s = " " + s;
          out += Pad(s, width, flags.find('-') != std::string::npos);
          break;
        }
        if (conv == 'a' || conv == 'A')
          FatalF("guest printf: %%%c is not supported yet", conv);
        std::snprintf(buf, sizeof buf, (spec + conv).c_str(), v);
        std::string s = buf;
        if (three_digit_exponents) {  // the legacy e+005: three exponent digits
          size_t e = s.find_first_of("eE");
          if (e != std::string::npos && s.size() - e == 4) s.insert(e + 2, "0");
        }
        out += s;
        break;
      }
      case 'n':
        FatalF("guest printf: %%n (disabled by default in the UCRT)");
      default:
        // An invalid conversion: the UCRT's invalid parameter handler.
        FatalF("guest printf: invalid conversion %%%c in \"%s\"", conv, fmt.c_str());
    }
  }
  return out;
}

// __stdio_common_v*printf options.
constexpr uint64_t kStandardSnprintf = 2, kThreeDigitExponents = 16;

// Writes s into the guest buffer the way the option set asks for; returns
// the function's result.
uint64_t PutFormatted(uint64_t options, gaddr buf, uint64_t count, const std::string& s) {
  const uint64_t n = s.size();
  if (!buf) return uint32_t(int32_t(n));
  if (options & kStandardSnprintf) {  // snprintf: truncate, always NUL
    if (count) {
      uint64_t k = std::min(n, count - 1);
      std::memcpy(GPtr(buf), s.data(), k);
      Wr8(buf + k, 0);
    }
    return uint32_t(int32_t(n));
  }
  // _vsnprintf: -1 if it does not fit; NUL only when there is room.
  uint64_t k = std::min(n, count);
  std::memcpy(GPtr(buf), s.data(), k);
  if (n < count) Wr8(buf + n, 0);
  return n <= count ? uint32_t(int32_t(n)) : uint32_t(-1);
}

}  // namespace

HLE64("ucrtbase.dll", __acrt_iob_func, kCdecl) { return StdStream(uint32_t(Arg(c, 0))); }

// __stdio_common_vsprintf(options, buffer, count, format, locale, va_list)
HLE64("ucrtbase.dll", __stdio_common_vsprintf, kCdecl) {
  uint64_t opt = Arg(c, 0);
  std::string s = Format(GStr(Arg(c, 3)), Args{Arg(c, 5)}, opt & kThreeDigitExponents);
  return PutFormatted(opt, Arg(c, 1), Arg(c, 2), s);
}

// __stdio_common_vsprintf_s: a buffer too small is an invalid parameter.
HLE64("ucrtbase.dll", __stdio_common_vsprintf_s, kCdecl) {
  uint64_t opt = Arg(c, 0);
  gaddr buf = Arg(c, 1);
  uint64_t count = Arg(c, 2);
  std::string s = Format(GStr(Arg(c, 3)), Args{Arg(c, 5)}, opt & kThreeDigitExponents);
  if (!buf || s.size() + 1 > count)
    FatalF("guest sprintf_s: %zu characters into a buffer of %llu", s.size(),
           (unsigned long long)count);
  std::memcpy(GPtr(buf), s.c_str(), s.size() + 1);
  return uint32_t(s.size());
}

// __stdio_common_vsnprintf_s(options, buffer, size, max_count, format,
// locale, va_list): max_count _TRUNCATE ((size_t)-1) truncates to the
// buffer and returns -1 when it did.
HLE64("ucrtbase.dll", __stdio_common_vsnprintf_s, kCdecl) {
  uint64_t opt = Arg(c, 0);
  gaddr buf = Arg(c, 1);
  uint64_t size = Arg(c, 2), max = Arg(c, 3);
  std::string s = Format(GStr(Arg(c, 4)), Args{Arg(c, 6)}, opt & kThreeDigitExponents);
  if (!buf || !size) FatalF("guest vsnprintf_s: no buffer");
  uint64_t limit = std::min(max == ~0ull ? size - 1 : max, size - 1);
  bool truncated = s.size() > limit;
  if (truncated && max != ~0ull && max >= size)
    FatalF("guest vsnprintf_s: %zu characters into a buffer of %llu", s.size(),
           (unsigned long long)size);
  uint64_t k = std::min<uint64_t>(s.size(), limit);
  std::memcpy(GPtr(buf), s.data(), k);
  Wr8(buf + k, 0);
  return truncated ? uint32_t(-1) : uint32_t(k);
}

// __stdio_common_vfprintf(options, stream, format, locale, va_list)
HLE64("ucrtbase.dll", __stdio_common_vfprintf, kCdecl) {
  uint64_t opt = Arg(c, 0);
  Stream& st = Must(Arg(c, 1), "fprintf");
  std::string s = Format(GStr(Arg(c, 2)), Args{Arg(c, 4)}, opt & kThreeDigitExponents);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return Write(st, s.data(), s.size()) == s.size() ? uint32_t(s.size()) : uint32_t(-1);
}

// ---- streams --------------------------------------------------------------------

namespace {
// fopen's mode: r w a, +, b/t; anything else is the UCRT's invalid
// parameter. The host file, or null with the guest's errno set.
FILE* OpenHost(const std::string& path, const std::string& mode, bool* text) {
  if (mode.empty() || !std::strchr("rwa", mode[0]))
    FatalF("guest fopen(\"%s\", \"%s\"): invalid mode", path.c_str(), mode.c_str());
  bool plus = mode.find('+') != std::string::npos;
  *text = mode.find('b') == std::string::npos;  // text unless "b" (_fmode)
  PathAccess acc = mode[0] == 'r' ? (plus ? PathAccess::kUpdate : PathAccess::kRead)
                                  : PathAccess::kWrite;
  if (mode[0] == 'a') acc = PathAccess::kUpdate;
  std::string host = HostPath(path, acc);
  std::string m(1, mode[0]);
  if (plus) m += '+';
  m += 'b';
  FILE* f = std::fopen(host.c_str(), m.c_str());
  RECOMP_TRACE("  fopen(%s, %s) -> %s%s\n", path.c_str(), mode.c_str(), host.c_str(),
               f ? "" : " (failed)");
  if (!f)
    Wr32(GuestErrno(), uint32_t(errno == ENOENT ? ENOENT : errno == EACCES ? EACCES : EINVAL));
  return f;
}

gaddr Open(const std::string& path, const std::string& mode) {
  bool text;
  FILE* f = OpenHost(path, mode, &text);
  return f ? NewStream(f, text, true) : 0;
}

// freopen: the stream is closed, then the same FILE* opens `path`; on
// failure it stays closed and the result is null.
gaddr Reopen(const std::string& path, const std::string& mode, gaddr fp) {
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  Stream& s = Must(fp, "freopen");
  if (s.owned) std::fclose(s.f);
  else std::fflush(s.f);
  bool text;
  FILE* f = OpenHost(path, mode, &text);
  if (!f) {
    g_streams.erase(fp);
    return 0;
  }
  s = {f, text, true};
  return fp;
}
}  // namespace

HLE64("ucrtbase.dll", fopen, kCdecl) { return Open(GStr(Arg(c, 0)), GStr(Arg(c, 1))); }
HLE64("ucrtbase.dll", _wfopen, kCdecl) {
  return Open(NarrowPath(GWStr(Arg(c, 0))), win32::Narrow(GWStr(Arg(c, 1))));
}
// _wfreopen(path, mode, stream); a null path (changing the mode of the open
// file) is not supported.
HLE64("ucrtbase.dll", _wfreopen, kCdecl) {
  if (!Arg(c, 0)) FatalF("_wfreopen with a null path (mode change) is not supported");
  return Reopen(NarrowPath(GWStr(Arg(c, 0))), win32::Narrow(GWStr(Arg(c, 1))), Arg(c, 2));
}
// fopen_s(&file, path, mode): 0 or the errno.
HLE64("ucrtbase.dll", fopen_s, kCdecl) {
  gaddr f = Open(GStr(Arg(c, 1)), GStr(Arg(c, 2)));
  Wr64(Arg(c, 0), f);
  return f ? 0 : Rd32(GuestErrno());
}

HLE64("ucrtbase.dll", fclose, kCdecl) {
  gaddr fp = Arg(c, 0);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  auto it = g_streams.find(fp);
  if (it == g_streams.end()) return uint32_t(EOF);
  int r = it->second.owned ? std::fclose(it->second.f) : std::fflush(it->second.f);
  if (it->second.owned) {
    g_streams.erase(it);
    HeapFreeProcess(fp);
  }
  return uint32_t(r);
}

HLE64("ucrtbase.dll", fread, kCdecl) {
  gaddr buf = Arg(c, 0);
  uint64_t size = Arg(c, 1), n = Arg(c, 2);
  Stream& s = Must(Arg(c, 3), "fread");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (!size || !n) return 0;
  if (!s.text) return std::fread(GPtr(buf), size_t(size), size_t(n), s.f);
  uint64_t total = size * n, got = 0;
  for (; got < total; got++) {
    int ch = GetC(s);
    if (ch == EOF) break;
    Wr8(buf + got, uint8_t(ch));
  }
  return got / size;
}

HLE64("ucrtbase.dll", fwrite, kCdecl) {
  gaddr buf = Arg(c, 0);
  uint64_t size = Arg(c, 1), n = Arg(c, 2);
  Stream& s = Must(Arg(c, 3), "fwrite");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (!size || !n) return 0;
  return Write(s, g_proc->mem.ptr<const char>(buf), size * n) / size;
}

HLE64("ucrtbase.dll", fgets, kCdecl) {
  gaddr buf = Arg(c, 0);
  int n = int32_t(Arg(c, 1));
  Stream& s = Must(Arg(c, 2), "fgets");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (n <= 0) return 0;
  int i = 0;
  while (i < n - 1) {
    int ch = GetC(s);
    if (ch == EOF) break;
    Wr8(buf + i++, uint8_t(ch));
    if (ch == '\n') break;
  }
  if (!i) return 0;
  Wr8(buf + i, 0);
  return buf;
}

HLE64("ucrtbase.dll", fputs, kCdecl) {
  std::string str = GStr(Arg(c, 0));
  Stream& s = Must(Arg(c, 1), "fputs");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return Write(s, str.data(), str.size()) == str.size() ? 0 : uint32_t(EOF);
}

HLE64("ucrtbase.dll", fputc, kCdecl) {
  char ch = char(Arg(c, 0));
  Stream& s = Must(Arg(c, 1), "fputc");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return Write(s, &ch, 1) == 1 ? uint8_t(ch) : uint32_t(EOF);
}

HLE64("ucrtbase.dll", fflush, kCdecl) {
  gaddr fp = Arg(c, 0);
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  if (!fp) return uint32_t(std::fflush(nullptr));
  Stream& s = Must(fp, "fflush");
  return uint32_t(std::fflush(s.f));
}

HLE64("ucrtbase.dll", ferror, kCdecl) { return std::ferror(Must(Arg(c, 0), "ferror").f) != 0; }

HLE64("ucrtbase.dll", fseek, kCdecl) {
  Stream& s = Must(Arg(c, 0), "fseek");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return uint32_t(std::fseek(s.f, long(int32_t(Arg(c, 1))), int(Arg(c, 2))));
}
HLE64("ucrtbase.dll", _fseeki64, kCdecl) {
  Stream& s = Must(Arg(c, 0), "_fseeki64");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  return uint32_t(fseeko(s.f, off_t(int64_t(Arg(c, 1))), int(Arg(c, 2))));
}
HLE64("ucrtbase.dll", ftell, kCdecl) {
  Stream& s = Must(Arg(c, 0), "ftell");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  long p = std::ftell(s.f);
  return uint32_t(int32_t(p > INT32_MAX ? -1 : p));  // long is 32-bit
}
HLE64("ucrtbase.dll", rewind, kCdecl) {
  Stream& s = Must(Arg(c, 0), "rewind");
  std::lock_guard<std::recursive_mutex> l(g_streams_mu);
  std::rewind(s.f);
  return 0;
}

}  // namespace recomp::hle
