// The game's debug log. GAME.EXE calls a printf-style function (cdecl,
// 0x4068F0) for its diagnostics ("COPYPROTECTION - ...", "Init Secondary
// Mixfiles....."); the retail build compiled it to a bare `ret`. With
// RA2_DEBUGLOG=1 this replacement (config/game.manifest: replace) prints
// those lines to stderr, then returns as the original does.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "runtime/runtime.h"

namespace {

// printf over guest varargs: the format at `fmt`, arguments from `args`
// (32-bit stack slots; doubles take two).
std::string FormatGuest(recomp::gaddr fmt, recomp::gaddr args) {
  const std::string f = recomp::GStr(fmt);
  std::string out;
  for (size_t i = 0; i < f.size(); i++) {
    if (f[i] != '%') {
      out += f[i];
      continue;
    }
    size_t j = i + 1;
    while (j < f.size() && std::strchr("-+ #0", f[j])) j++;
    while (j < f.size() && (std::isdigit(uint8_t(f[j])) || f[j] == '.' || f[j] == '*')) j++;
    while (j < f.size() && std::strchr("hlLqjzt", f[j])) j++;
    if (j >= f.size()) break;
    std::string spec = f.substr(i, j - i + 1);
    const char conv = f[j];
    char buf[512];
    switch (conv) {
      case '%': out += '%'; break;
      case 's': {
        const recomp::gaddr p = recomp::Rd32(args);
        args += 4;
        std::snprintf(buf, sizeof buf, spec.c_str(), p ? recomp::GStr(p).c_str() : "(null)");
        out += buf;
        break;
      }
      case 'f': case 'g': case 'e': {
        const uint64_t bits = recomp::Rd64(args);
        args += 8;
        double d;
        std::memcpy(&d, &bits, 8);
        std::snprintf(buf, sizeof buf, spec.c_str(), d);
        out += buf;
        break;
      }
      default: {  // integers, characters, pointers: one 32-bit slot
        const uint32_t v = recomp::Rd32(args);
        args += 4;
        // The host's long is 64 bits: format the 32-bit value as int.
        std::string s;
        for (char ch : spec)
          if (ch != 'l' && ch != 'h') s += ch;
        if (conv == 'p') s.back() = 'X';
        std::snprintf(buf, sizeof buf, s.c_str(), v);
        out += buf;
        break;
      }
    }
    i = j;
  }
  return out;
}

// A guest UTF-16 string at p if it is a short printable one, as UTF-8.
bool GuestWide(recomp::gaddr p, std::string* out) {
  if (p < 0x10000 || !recomp::g_proc->mem.IsCommitted(p) ||
      !recomp::g_proc->mem.IsCommitted(p + 127))
    return false;
  std::string s;
  for (int i = 0; i < 64; i++) {
    const uint16_t ch = recomp::Rd16(p + 2 * i);
    if (!ch) break;
    if (ch < 0x20) return false;
    if (ch < 0x80) {
      s += char(ch);
    } else if (ch < 0x800) {
      s += char(0xC0 | (ch >> 6));
      s += char(0x80 | (ch & 0x3F));
    } else {
      s += char(0xE0 | (ch >> 12));
      s += char(0x80 | ((ch >> 6) & 0x3F));
      s += char(0x80 | (ch & 0x3F));
    }
  }
  *out = s;
  return !s.empty();
}

}  // namespace

namespace {

bool EnvOn(const char* name) {
  const char* e = std::getenv(name);
  return e && *e && *e != '0';
}

}  // namespace

namespace recomp::gen {

void f_00432350_lifted(Ctx& c);

// The game's text drawing routine (cdecl, 7 arguments: the font first).
// RA2_TEXTLOG=1 logs its arguments, with any UTF-16 string among them.
void f_00432350(Ctx& c) {
  static const bool on = EnvOn("RA2_TEXTLOG");
  if (on) {
    std::string line;
    char buf[64];
    for (int i = 1; i <= 7; i++) {
      const uint32_t a = Rd32(c.esp + 4 * i);
      std::snprintf(buf, sizeof buf, " %08X", a);
      line += buf;
      std::string w;
      if (GuestWide(a, &w)) line += " \"" + w + "\"";
    }
    std::fprintf(stderr, "[ra2 text]%s\n", line.c_str());
  }
  f_00432350_lifted(c);
}

void f_00431CC0_lifted(Ctx& c);

// The font's glyph-string renderer, which every text printer calls
// (thiscall, 5 arguments). RA2_TEXTLOG=1 logs its caller and arguments.
void f_00431CC0(Ctx& c) {
  static const bool on = EnvOn("RA2_TEXTLOG");
  if (on) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "[ra2 glyphs] from %08X ecx %08X", Rd32(c.esp), c.ecx);
    std::string line = buf;
    for (int i = 1; i <= 5; i++) {
      const uint32_t a = Rd32(c.esp + 4 * i);
      std::snprintf(buf, sizeof buf, " %08X", a);
      line += buf;
      std::string w;
      if (GuestWide(a, &w)) line += " \"" + w + "\"";
    }
    std::fprintf(stderr, "%s\n", line.c_str());
  }
  f_00431CC0_lifted(c);
}

void f_004318E0_lifted(Ctx& c);

// The other glyph renderer (0x432300 and 0x432490 print through it).
void f_004318E0(Ctx& c) {
  static const bool on = EnvOn("RA2_TEXTLOG");
  if (on) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "[ra2 glyphs2] from %08X ecx %08X", Rd32(c.esp), c.ecx);
    std::string line = buf;
    for (int i = 1; i <= 6; i++) {
      const uint32_t a = Rd32(c.esp + 4 * i);
      std::snprintf(buf, sizeof buf, " %08X", a);
      line += buf;
      std::string w;
      if (GuestWide(a, &w)) line += " \"" + w + "\"";
    }
    std::fprintf(stderr, "%s\n", line.c_str());
  }
  f_004318E0_lifted(c);
}

void f_00402720_lifted(Ctx& c);

// The audio engine's sample start (thiscall). RA2_AUDIOLOG=1 logs the
// sample's state bits (+0xA4) before and the result.
void f_00402720(Ctx& c) {
  static const bool on = EnvOn("RA2_AUDIOLOG");
  const uint32_t self = c.ecx, caller = Rd32(c.esp);
  const uint32_t before = on ? Rd32(self + 0xA4) : 0;
  f_00402720_lifted(c);
  if (on)
    std::fprintf(stderr, "[ra2 audio] start %08X from %08X: state %X -> %X, result %08X\n", self,
                 caller, before, Rd32(self + 0xA4), c.eax);
}

void f_00407A90_lifted(Ctx& c);

// The audio engine's file-stream start. RA2_AUDIOLOG=1 logs the file
// name, the stream's suspend counts (+0x88, +0x80) and flags (+0x0C).
void f_00407A90(Ctx& c) {
  static const bool on = EnvOn("RA2_AUDIOLOG");
  const uint32_t self = c.ecx, name = c.edx;
  if (on)
    std::fprintf(stderr, "[ra2 audio] stream %08X \"%s\": +88 %u +80 %u flags %X\n", self,
                 name ? GStr(name).c_str() : "", Rd32(self + 0x88), Rd32(self + 0x80),
                 Rd32(self + 0x0C));
  f_00407A90_lifted(c);
  if (on)
    std::fprintf(stderr, "[ra2 audio] stream %08X -> %08X flags %X\n", self, c.eax,
                 Rd32(self + 0x0C));
}

void f_004068F0_lifted(Ctx& c);

void f_004068F0(Ctx& c) {
  static const bool on = [] {
    const char* e = std::getenv("RA2_DEBUGLOG");
    return e && *e && *e != '0';
  }();
  if (on) {
    std::string line = FormatGuest(Rd32(c.esp + 4), c.esp + 8);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    std::fprintf(stderr, "[ra2] %s\n", line.c_str());
  }
  f_004068F0_lifted(c);
}

}  // namespace recomp::gen
