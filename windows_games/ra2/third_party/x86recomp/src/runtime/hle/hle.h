#pragma once
// High-level emulation of the Win32/DirectX API surface: host
// implementations of imported functions, bound to the import thunks.
//
// A handler reads its arguments from the guest stack (Arg(c, i)), returns
// the value for EAX, and the framework pops the return address plus nargs
// stack slots (stdcall). nargs = kCdecl pops only the return address.
// Imports without a handler are bound to a fatal stub that names them:
// there are no silent default stubs.
//
// x64 processes call the same handlers with the Win64 convention: Arg
// reads rcx, rdx, r8, r9 and then the stack, the return value goes to
// rax, nargs is unused (the caller pops). Handlers written for 32-bit
// guests keep guest pointers and structure fields in uint32_t, so only
// those declared with HLE64 (audited: pointers in gaddr, x64 structure
// layouts) can be called from x64 code; calling another is fatal and
// names it.

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

#include "runtime/ctx.h"
#include "runtime/runtime.h"

namespace recomp::hle {

inline constexpr int kCdecl = -1;

// >0 while an HLE handler runs on this thread (0 again inside CallGuest).
// A host fault then is a runtime bug, not a guest access violation.
extern thread_local int t_hle_depth;

// Guest range used for pseudo HMODULE values of the DLLs we provide.
inline constexpr uint32_t kModuleRegion = 0x70000000;
inline constexpr uint32_t kModuleRegionSize = 0x00200000;
inline constexpr gaddr kModuleRegion64 = 0xFFE000000;  // x64 (runtime.h)

using Fn = uint64_t (*)(Ctx& c);

// A handler, registered under (dll, export name), e.g. ("kernel32.dll",
// "Sleep") or ("mss32.dll", "_AIL_startup@0").
struct Def {
  int nargs;
  Fn fn;
  bool x64;  // audited for x64 guests (HLE64)
};

struct Registrar {
  Registrar(const char* dll, const char* name, int nargs, Fn fn, bool x64 = false);
};

// A data export of a DLL we provide (a C++ static member, a global the
// guest reads through its IAT slot): `size` bytes of guest memory, zero or
// set by `init`, allocated on first import. They live in the upper half of
// the thunk region (kDataExportOffset), which is writable.
inline constexpr uint32_t kDataExportOffset = kThunkRegionSize / 2;
struct DataExport {
  DataExport(const char* dll, const char* name, uint32_t size, void (*init)(gaddr) = nullptr);
};

// The full export table of a DLL we provide, for games that GetProcAddress
// every entry point up front and only call some (an engine's GL loader
// that resolves all of opengl32). GetProcAddress then returns a thunk for any listed name;
// calling one without a handler is fatal and names it, as for imports.
struct Exports {
  Exports(const char* dll, std::initializer_list<const char*> names);
};

// Argument i of the call into a handler. x64: rcx, rdx, r8, r9, then the
// stack above the return address and the 32-byte shadow space.
inline uint64_t Arg(const Ctx& c, int i) {
  if (!g_proc->x64) return Rd32(c.esp + 4 + 4 * i);
  switch (i) {
    case 0: return c.rcx;
    case 1: return c.rdx;
    case 2: return c.r8;
    case 3: return c.r9;
    default: return Rd64(c.rsp + 8 + 8 * uint64_t(i));
  }
}

// A handle as a return value: Win64 handles are pointer-sized, their values
// sign-extended 32-bit ones (the pseudo handles are -1 and -2). Handle
// arguments are read as their low 32 bits, as Windows does.
inline uint64_t HandleRet(uint32_t h) {
  return g_proc->x64 ? uint64_t(int64_t(int32_t(h))) : h;
}

// Win64 floating-point arguments and results: argument i of a float or
// double type is in xmm<i> (i < 4; later ones on the stack like integers),
// and the result goes to xmm0.
inline double ArgD(const Ctx& c, int i) {
  double v;
  if (i < 4) std::memcpy(&v, c.xmm[i].b, 8);
  else {
    uint64_t b = Arg(c, i);
    std::memcpy(&v, &b, 8);
  }
  return v;
}
inline float ArgF(const Ctx& c, int i) {
  float v;
  if (i < 4) std::memcpy(&v, c.xmm[i].b, 4);
  else {
    uint32_t b = uint32_t(Arg(c, i));
    std::memcpy(&v, &b, 4);
  }
  return v;
}
// Returns from a handler with a double (float) result in xmm0.
inline uint64_t RetD(Ctx& c, double v) {
  std::memcpy(c.xmm[0].b, &v, 8);
  return 0;
}
inline uint64_t RetF(Ctx& c, float v) {
  std::memcpy(c.xmm[0].b, &v, 4);
  return 0;
}

// IAT value for a static import dll!name of the EXE or a lifted DLL: its
// handler's thunk (PeImage::Load's binder).
gaddr ImportThunk(const std::string& dll, const std::string& name);

// "KERNEL32.dll", "C:\x\Foo" -> "kernel32.dll", "foo.dll".
std::string NormaliseDll(std::string dll);

// Called by Dispatch for any address in the thunk region.
void CallThunk(Ctx& c, gaddr thunk);

// Pseudo module handles for DLLs the game loads (LoadLibrary/
// GetModuleHandle). Returns 0 for DLLs we don't provide.
gaddr ModuleHandle(const std::string& dll);
// Thunk address for dll!name (GetProcAddress); 0 if not implemented.
gaddr ProcAddress(gaddr hmodule, const std::string& name);

// Thunk address for dll!name whether or not a handler exists (calling an
// unimplemented one is fatal and names it). Used for COM vtables.
gaddr ThunkFor(const std::string& dll, const std::string& name);

void SetLastError(uint32_t e);

// Writes s + NUL to a counted guest buffer the Win32 way: returns the
// length written without NUL, or the size needed (with NUL) if it does not
// fit.
uint32_t CopyOut(gaddr buf, uint32_t size, const std::string& s);
// The NLS convention (GetLocaleInfo, GetDateFormat): `cap` characters
// (WCHARs if `wide`), 0 asks for the size; returns the count with NUL, or
// 0 with ERROR_INSUFFICIENT_BUFFER.
uint32_t PutCounted(gaddr buf, uint32_t cap, const std::string& s, bool wide);

// Makes [va, va+len) readable before host code copies it, the way a DLL's
// read would on Windows: each uncommitted page raises a guest access
// violation, which the game's handlers may resolve (a game that
// demand-pages archive data commits file views this way). False if a page stays
// inaccessible.
bool EnsureReadable(Ctx& c, gaddr va, uint64_t len);

}  // namespace recomp::hle

#define RECOMP_HLE_CAT2(a, b) a##b
#define RECOMP_HLE_CAT(a, b) RECOMP_HLE_CAT2(a, b)

// HLE(dll, Name, nargs) { ... return eax; }  for C-identifier names.
#define HLE(dll, name, nargs) HLE_NAMED(dll, #name, name, nargs)
// HLE64(dll, Name, nargs): the same, audited for x64 guests too.
#define HLE64(dll, name, nargs) HLE_NAMED_X(dll, #name, name, nargs, true)

// HLE_NAMED(dll, "export@8", ident, nargs) for decorated export names.
#define HLE_NAMED(dll, export_name, ident, nargs) \
  HLE_NAMED_X(dll, export_name, ident, nargs, false)
#define HLE_NAMED_X(dll, export_name, ident, nargs, x64)                   \
  static uint64_t RECOMP_HLE_CAT(hle_, ident)(::recomp::Ctx & c);              \
  static ::recomp::hle::Registrar RECOMP_HLE_CAT(reg_, ident){                 \
      dll, export_name, nargs, &RECOMP_HLE_CAT(hle_, ident), x64};          \
  static uint64_t RECOMP_HLE_CAT(hle_, ident)([[maybe_unused]] ::recomp::Ctx & c)
