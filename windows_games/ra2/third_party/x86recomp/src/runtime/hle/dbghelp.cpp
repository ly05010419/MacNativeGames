// dbghelp: UnDecorateSymbolName, for games that bind script calls by the
// demangled names of their own EXE's exports, plus the symbol-engine
// entry points a crash reporter resolves (a game's dbghelp loader may
// insist on all of them and abort otherwise).
//
// Such a game undecorates every exported C++ symbol with flags 0x800
// (UNDNAME_32_BIT_DECODE: the full declaration) and parses the text, so
// the output must match MSVC's undname formatting. LLVM's Microsoft
// demangler produces the same declarations with a few cosmetic
// differences that are normalised here:
//   - argument/template lists: "a, b"  -> "a,b"
//   - nested template closers: ">>"    -> "> >"
//   - function pointer types:  "(__cdecl *)" -> "(__cdecl*)"

#include <llvm/Demangle/Demangle.h>

#include <cctype>
#include <cstdlib>
#include <string>

#include "runtime/hle/hle.h"
#include "runtime/log.h"

namespace recomp::hle {
namespace {

std::string UndnameMsvc(const std::string& mangled, uint32_t flags) {
  int flags_llvm = llvm::MSDF_None;
  if (flags & 0x0002) flags_llvm |= llvm::MSDF_NoCallingConvention;  // NO_MS_KEYWORDS
  if (flags & 0x0004) flags_llvm |= llvm::MSDF_NoReturnType;  // NO_FUNCTION_RETURNS
  if (flags & 0x0080) flags_llvm |= llvm::MSDF_NoAccessSpecifier;
  if (flags & 0x0200) flags_llvm |= llvm::MSDF_NoMemberType;
  bool name_only = flags & 0x1000;  // UNDNAME_NAME_ONLY
  if (name_only)
    flags_llvm |= llvm::MSDF_NoAccessSpecifier | llvm::MSDF_NoCallingConvention |
                  llvm::MSDF_NoReturnType | llvm::MSDF_NoMemberType |
                  llvm::MSDF_NoVariableType;
  int status = 0;
  char* out = llvm::microsoftDemangle(mangled, nullptr, &status,
                                      llvm::MSDemangleFlags(flags_llvm));
  if (!out || status != 0) {
    std::free(out);
    return {};
  }
  std::string s = out;
  std::free(out);
  if (name_only) {
    size_t paren = s.find('(');
    if (paren != std::string::npos) s.resize(paren);
  }

  std::string r;
  r.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    char ch = s[i];
    if (ch == ',' && i + 1 < s.size() && s[i + 1] == ' ') {
      r += ',';
      i++;
      continue;
    }
    if (ch == '>' && !r.empty() && r.back() == '>') {
      // "operator>>" / "operator>>=" are names, not template closers.
      bool op = r.size() >= 9 && r.compare(r.size() - 9, 9, "operator>") == 0;
      if (!op) r += ' ';
    }
    auto after_callconv = [&]() {
      size_t k = r.find_last_of(" (");
      std::string last = r.substr(k == std::string::npos ? 0 : k + 1);
      return last == "__cdecl" || last == "__stdcall" || last == "__thiscall" ||
             last == "__fastcall" || last == "__pascal";
    };
    // "__cdecl *)" -> "__cdecl*)": calling convention bound to a pointer.
    if (ch == ' ' && i + 1 < s.size() && s[i + 1] == '*' && after_callconv())
      continue;
    // undname always separates a pointer/reference from its type: "X *",
    // and stacked indirections from each other: "X * *", "X * &".
    if ((ch == '*' || ch == '&') && !r.empty() &&
        (std::isalnum(uint8_t(r.back())) || r.back() == '_' || r.back() == '>' ||
         r.back() == '*') &&
        !after_callconv())
      r += ' ';
    r += ch;
  }
  // A top-level const on a returned pointer is not printed:
  // "Goid_ const *const __thiscall" -> "Goid_ const * __thiscall".
  for (const char* cc : {"__cdecl", "__stdcall", "__thiscall", "__fastcall"}) {
    std::string from = std::string("*const ") + cc, to = std::string("* ") + cc;
    size_t at = r.find(from);
    if (at != std::string::npos) r.replace(at, from.size(), to);
  }
  // Member function qualifiers follow the parameter list without a space
  // and with a trailing one: "f(void)const ".
  for (const char* q : {" const volatile", " const", " volatile"}) {
    std::string tail = std::string(")") + q;
    if (r.size() >= tail.size() && r.compare(r.size() - tail.size(), tail.size(), tail) == 0) {
      r.resize(r.size() - tail.size());
      r += std::string(")") + (q + 1) + " ";
      break;
    }
  }
  return r;
}

// UnDecorateSymbolName(name, out, maxlen, flags) -> length or 0.
HLE("dbghelp.dll", UnDecorateSymbolName, 4) {
  std::string mangled = GStr(Arg(c, 0));
  uint32_t out = Arg(c, 1), cap = Arg(c, 2), flags = Arg(c, 3);
  std::string s = UndnameMsvc(mangled, flags);
  if (s.empty()) s = mangled;  // undname echoes what it cannot decode
  if (!cap) return 0;
  s = s.substr(0, cap - 1);
  PutStr(out, s);
  return uint32_t(s.size());
}

// The symbol engine is only used to annotate crash reports.
HLE("dbghelp.dll", SymInitialize, 3) { return 1; }
HLE("dbghelp.dll", SymCleanup, 1) { return 1; }
HLE("dbghelp.dll", SymGetOptions, 0) { return 0; }
HLE("dbghelp.dll", SymSetOptions, 1) { return Arg(c, 0); }
HLE("dbghelp.dll", SymLoadModule, 6) { return g_proc->img.image_base; }
HLE("dbghelp.dll", SymGetModuleBase, 2) {
  uint32_t a = Arg(c, 1);
  const PeImage& img = g_proc->img;
  return a >= img.image_base && a < img.image_base + img.image_size ? img.image_base : 0;
}
HLE("dbghelp.dll", SymFunctionTableAccess, 2) { return 0; }
HLE("dbghelp.dll", StackWalk, 9) { return 0; }  // no frames: report ends
HLE("dbghelp.dll", SymGetSymFromAddr, 4) { return 0; }
HLE("dbghelp.dll", SymGetLineFromAddr, 4) { return 0; }
HLE("dbghelp.dll", SymGetModuleInfo, 3) { return 0; }
HLE("dbghelp.dll", MiniDumpWriteDump, 7) {
  WarnF("MiniDumpWriteDump: not supported\n");
  return 0;
}

}  // namespace
}  // namespace recomp::hle
