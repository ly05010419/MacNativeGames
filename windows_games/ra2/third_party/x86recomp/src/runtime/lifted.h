#pragma once
// Everything generated (lifted) code needs: guest memory access, x86 flag
// semantics, and the runtime entry points it calls. Included by every file
// in generated/. Portable C++ only (see CLAUDE.md product constraints);
// __builtin_* used here are compiler builtins, not x86 intrinsics.

#include <cstdint>
#include <cstring>

#include "runtime/ctx.h"
#include "runtime/mmx.h"
#include "runtime/sse.h"
#include "runtime/x87.h"

namespace recomp {

extern uint8_t* g_guest_base;

using GuestFn = void (*)(Ctx&);

// Emitted by the lifter into generated/table.cpp.
struct LiftedFn {
  uint64_t va;
  GuestFn fn;
};
extern const LiftedFn kLiftedFunctions[];
extern const uint32_t kLiftedFunctionCount;
// Hands that table to the runtime (runtime/dispatch.cpp), which as a shared
// library cannot refer to it; called from runtime/lifted_table.cpp, which
// recomp_add_lifted compiles into the game's lifted code.
void RegisterLiftedFunctions(const LiftedFn* fns, uint32_t count);

// Thrown at manifest `resume` sites: the guest abandons the current call
// chain and continues at `target` in the frame whose EBP is `ebp` (SEH
// __except blocks, longjmp to a setjmp return point). Caught by the lifted
// function owning that frame, which continues there with Resume(). In x64
// guests `ebp` is the frame's establisher frame (ARCHITECTURE.md, "64-bit
// guests"), thrown by the unwinder at the end of an exception's unwind.
struct GuestResume {
  uint64_t target, ebp, from;
};

// Runtime services called from lifted code (runtime/dispatch.cpp).
// `va` is always the guest address of the instruction doing the call, for
// diagnostics. Code lifted from x64 images (RECOMP_LIFT_X64) sees the
// overloads with 64-bit addresses instead, so its literals never pick the
// 32-bit ones.
#ifndef RECOMP_LIFT_X64
void Dispatch(Ctx& c, uint32_t target, uint32_t va);
#endif
// Dispatches to r.target, and again for each later GuestResume into the
// same frame (r.ebp) thrown while that continuation runs.
void Resume(Ctx& c, GuestResume r);
#ifndef RECOMP_LIFT_X64
[[noreturn]] void Unimplemented(Ctx& c, uint32_t va, const char* what);
[[noreturn]] void BadJump(Ctx& c, uint32_t va, uint32_t target);
[[noreturn]] void Unreachable(Ctx& c, uint32_t va);
void Trap(Ctx& c, uint32_t va);                // int3 / int n
void RaiseDivideError(Ctx& c, uint32_t va);    // #DE from div/idiv
void Probe(Ctx& c, uint32_t va, const char* name);  // manifest probes
#else
void Dispatch(Ctx& c, uint64_t target, uint64_t va);
[[noreturn]] void Unimplemented(Ctx& c, uint64_t va, const char* what);
[[noreturn]] void BadJump(Ctx& c, uint64_t va, uint64_t target);
[[noreturn]] void Unreachable(Ctx& c, uint64_t va);
void Trap(Ctx& c, uint64_t va);
void RaiseDivideError(Ctx& c, uint64_t va);
void Probe(Ctx& c, uint64_t va, const char* name);
void Xgetbv(Ctx& c);                           // ecx in, edx:eax out
#endif
void Cpuid(Ctx& c);                            // eax/ecx in, eax..edx out
uint64_t Rdtsc(Ctx& c);

namespace lift {

// ---- memory -----------------------------------------------------------
template <typename T>
inline T Rd(const uint8_t* b, uint32_t a) {
  T v;
  std::memcpy(&v, b + a, sizeof(T));
  return v;
}
template <typename T>
inline void Wr(uint8_t* b, uint32_t a, T v) {
  std::memcpy(b + a, &v, sizeof(T));
}
// x64 guests: 64-bit addresses (ARCHITECTURE.md, "64-bit guests").
template <typename T>
inline T Rd(const uint8_t* b, uint64_t a) {
  T v;
  std::memcpy(&v, b + a, sizeof(T));
  return v;
}
template <typename T>
inline void Wr(uint8_t* b, uint64_t a, T v) {
  std::memcpy(b + a, &v, sizeof(T));
}
inline x87::F80 RdF80(const uint8_t* b, uint64_t a) {
  x87::F80 v;
  std::memcpy(&v.sig, b + a, 8);
  std::memcpy(&v.se, b + a + 8, 2);
  return v;
}
inline void WrF80(uint8_t* b, uint64_t a, x87::F80 v) {
  std::memcpy(b + a, &v.sig, 8);
  std::memcpy(b + a + 8, &v.se, 2);
}

// ---- widths -------------------------------------------------------------
// Operands of 1, 2 and 4 bytes are held in uint32_t, 8-byte ones (x64) in
// uint64_t: W<N>::T. The 8-byte forms of the helpers below are their own
// `if constexpr` branches, so the narrower ones compile exactly as before.
template <int N>
struct W;
template <>
struct W<1> {
  using T = uint32_t;
  static constexpr uint32_t kMask = 0xFFu, kSign = 0x80u, kBits = 8;
};
template <>
struct W<2> {
  using T = uint32_t;
  static constexpr uint32_t kMask = 0xFFFFu, kSign = 0x8000u, kBits = 16;
};
template <>
struct W<4> {
  using T = uint32_t;
  static constexpr uint32_t kMask = 0xFFFFFFFFu, kSign = 0x80000000u,
                            kBits = 32;
};
template <>
struct W<8> {
  using T = uint64_t;
  static constexpr uint64_t kMask = ~0ull, kSign = 1ull << 63, kBits = 64;
};
template <int N>
using U = typename W<N>::T;

template <int N>
inline auto Sext(U<N> v) {
  if constexpr (N == 1) return int32_t(static_cast<int8_t>(v));
  else if constexpr (N == 2) return int32_t(static_cast<int16_t>(v));
  else if constexpr (N == 4) return static_cast<int32_t>(v);
  else return static_cast<int64_t>(v);
}

[[gnu::always_inline]] inline bool Parity(uint64_t v) {
  return !__builtin_parity(uint32_t(v) & 0xFFu);
}

template <int N>
[[gnu::always_inline]] inline void SetSZP(Flags& f, U<N> r) {
  r &= W<N>::kMask;
  f.zf = r == 0;
  f.sf = (r & W<N>::kSign) != 0;
  f.pf = Parity(r);
}

// ---- arithmetic (operands are pre-masked to N bytes by the emitter) -----
template <int N>
[[gnu::always_inline]] inline U<N> Add(Flags& f, U<N> a, U<N> b) {
  if constexpr (N == 8) {
    uint64_t r = a + b;
    f.cf = r < a;
    f.of = ((a ^ r) & (b ^ r) & W<N>::kSign) != 0;
    f.af = ((a ^ b ^ r) & 0x10) != 0;
    SetSZP<N>(f, r);
    return r;
  } else {
    uint64_t full = uint64_t(a) + b;
    uint32_t r = uint32_t(full) & W<N>::kMask;
    f.cf = full > W<N>::kMask;
    f.of = ((a ^ r) & (b ^ r) & W<N>::kSign) != 0;
    f.af = ((a ^ b ^ r) & 0x10) != 0;
    SetSZP<N>(f, r);
    return r;
  }
}
template <int N>
[[gnu::always_inline]] inline U<N> Adc(Flags& f, U<N> a, U<N> b) {
  if constexpr (N == 8) {
    uint64_t cin = f.cf ? 1 : 0;
    uint64_t r = a + b + cin;
    f.cf = r < a || (cin && r == a);
    f.of = ((a ^ r) & (b ^ r) & W<N>::kSign) != 0;
    f.af = ((a ^ b ^ r) & 0x10) != 0;
    SetSZP<N>(f, r);
    return r;
  } else {
    uint64_t full = uint64_t(a) + b + (f.cf ? 1 : 0);
    uint32_t r = uint32_t(full) & W<N>::kMask;
    f.cf = full > W<N>::kMask;
    f.of = ((a ^ r) & (b ^ r) & W<N>::kSign) != 0;
    f.af = ((a ^ b ^ r) & 0x10) != 0;
    SetSZP<N>(f, r);
    return r;
  }
}
template <int N>
[[gnu::always_inline]] inline U<N> Sub(Flags& f, U<N> a, U<N> b) {
  U<N> r = (a - b) & W<N>::kMask;
  f.cf = a < b;
  f.of = ((a ^ b) & (a ^ r) & W<N>::kSign) != 0;
  f.af = ((a ^ b ^ r) & 0x10) != 0;
  SetSZP<N>(f, r);
  return r;
}
template <int N>
[[gnu::always_inline]] inline U<N> Sbb(Flags& f, U<N> a, U<N> b) {
  if constexpr (N == 8) {
    uint64_t cin = f.cf ? 1 : 0;
    uint64_t r = a - b - cin;
    f.cf = a < b || (cin && a == b);
    f.of = ((a ^ b) & (a ^ r) & W<N>::kSign) != 0;
    f.af = ((a ^ b ^ r) & 0x10) != 0;
    SetSZP<N>(f, r);
    return r;
  } else {
    uint32_t cin = f.cf ? 1 : 0;
    uint32_t r = (a - b - cin) & W<N>::kMask;
    f.cf = uint64_t(a) < uint64_t(b) + cin;
    f.of = ((a ^ b) & (a ^ r) & W<N>::kSign) != 0;
    f.af = ((a ^ b ^ r) & 0x10) != 0;
    SetSZP<N>(f, r);
    return r;
  }
}
template <int N>
[[gnu::always_inline]] inline U<N> Inc(Flags& f, U<N> a) {
  bool cf = f.cf;
  U<N> r = Add<N>(f, a, 1);
  f.cf = cf;
  return r;
}
template <int N>
[[gnu::always_inline]] inline U<N> Dec(Flags& f, U<N> a) {
  bool cf = f.cf;
  U<N> r = Sub<N>(f, a, 1);
  f.cf = cf;
  return r;
}
template <int N>
[[gnu::always_inline]] inline U<N> Neg(Flags& f, U<N> a) {
  return Sub<N>(f, 0, a);
}
template <int N>
[[gnu::always_inline]] inline U<N> Logic(Flags& f, U<N> r) {
  r &= W<N>::kMask;
  f.cf = f.of = f.af = false;
  SetSZP<N>(f, r);
  return r;
}

// ---- shifts and rotates (count is the raw CL/imm8 value) ----------------
// The CPU masks the count to 5 bits, 6 for 8-byte operands.
template <int N>
inline constexpr uint32_t kCountMask = N == 8 ? 63 : 31;

template <int N>
[[gnu::always_inline]] inline U<N> Shl(Flags& f, U<N> a, uint32_t cnt) {
  cnt &= kCountMask<N>;
  if (!cnt) return a;
  if constexpr (N == 8) {
    uint64_t r = a << cnt;
    f.cf = (a >> (64 - cnt)) & 1;
    f.of = ((r & W<N>::kSign) != 0) != f.cf;
    f.af = false;
    SetSZP<N>(f, r);
    return r;
  } else {
    uint64_t v = uint64_t(a & W<N>::kMask) << cnt;
    uint32_t r = uint32_t(v) & W<N>::kMask;
    f.cf = (v >> W<N>::kBits) & 1;
    f.of = ((r & W<N>::kSign) != 0) != f.cf;
    f.af = false;
    SetSZP<N>(f, r);
    return r;
  }
}
template <int N>
[[gnu::always_inline]] inline U<N> Shr(Flags& f, U<N> a, uint32_t cnt) {
  cnt &= kCountMask<N>;
  if (!cnt) return a;
  a &= W<N>::kMask;
  f.cf = cnt <= W<N>::kBits ? (a >> (cnt - 1)) & 1 : 0;
  U<N> r = cnt >= W<N>::kBits ? 0 : a >> cnt;
  f.of = (a & W<N>::kSign) != 0;
  f.af = false;
  SetSZP<N>(f, r);
  return r;
}
template <int N>
[[gnu::always_inline]] inline U<N> Sar(Flags& f, U<N> a, uint32_t cnt) {
  cnt &= kCountMask<N>;
  if (!cnt) return a;
  auto s = Sext<N>(a);
  f.cf = (s >> (cnt - 1)) & 1;
  U<N> r = U<N>(s >> cnt) & W<N>::kMask;
  f.of = false;
  f.af = false;
  SetSZP<N>(f, r);
  return r;
}
template <int N>
[[gnu::always_inline]] inline U<N> Rol(Flags& f, U<N> a, uint32_t cnt) {
  if (!(cnt & kCountMask<N>)) return a;
  uint32_t k = (cnt & kCountMask<N>) % W<N>::kBits;
  a &= W<N>::kMask;
  U<N> r = k ? ((a << k) | (a >> (W<N>::kBits - k))) & W<N>::kMask : a;
  f.cf = r & 1;
  f.of = ((r & W<N>::kSign) != 0) != f.cf;
  return r;
}
template <int N>
[[gnu::always_inline]] inline U<N> Ror(Flags& f, U<N> a, uint32_t cnt) {
  if (!(cnt & kCountMask<N>)) return a;
  uint32_t k = (cnt & kCountMask<N>) % W<N>::kBits;
  a &= W<N>::kMask;
  U<N> r = k ? ((a >> k) | (a << (W<N>::kBits - k))) & W<N>::kMask : a;
  f.cf = (r & W<N>::kSign) != 0;
  f.of = f.cf != ((r & (W<N>::kSign >> 1)) != 0);
  return r;
}
template <int N>
[[gnu::always_inline]] inline U<N> Rcl(Flags& f, U<N> a, uint32_t cnt) {
  uint32_t k = (cnt & kCountMask<N>) % (W<N>::kBits + 1);
  a &= W<N>::kMask;
  for (uint32_t i = 0; i < k; i++) {
    bool out = (a & W<N>::kSign) != 0;
    a = ((a << 1) | (f.cf ? 1 : 0)) & W<N>::kMask;
    f.cf = out;
  }
  if (cnt & kCountMask<N>) f.of = ((a & W<N>::kSign) != 0) != f.cf;
  return a;
}
template <int N>
[[gnu::always_inline]] inline U<N> Rcr(Flags& f, U<N> a, uint32_t cnt) {
  uint32_t k = (cnt & kCountMask<N>) % (W<N>::kBits + 1);
  a &= W<N>::kMask;
  if (cnt & kCountMask<N>) f.of = ((a & W<N>::kSign) != 0) != f.cf;
  for (uint32_t i = 0; i < k; i++) {
    bool out = a & 1;
    a = (a >> 1) | (f.cf ? W<N>::kSign : 0);
    f.cf = out;
  }
  return a;
}
template <int N>
[[gnu::always_inline]] inline U<N> Shld(Flags& f, U<N> dst, U<N> src,
                                        uint32_t cnt) {
  cnt &= kCountMask<N>;
  if (!cnt) return dst;
  if constexpr (N == 8) {
    uint64_t r = (dst << cnt) | (src >> (64 - cnt));
    f.cf = (dst >> (64 - cnt)) & 1;  // last bit shifted out
    f.of = ((r ^ dst) & W<N>::kSign) != 0;
    SetSZP<N>(f, r);
    return r;
  } else {
    uint64_t v = (uint64_t(dst & W<N>::kMask) << W<N>::kBits) |
                 (src & W<N>::kMask);
    f.cf = (v >> (2 * W<N>::kBits - cnt)) & 1;  // last bit shifted out
    v <<= cnt;
    uint32_t r = uint32_t(v >> W<N>::kBits) & W<N>::kMask;
    f.of = ((r ^ dst) & W<N>::kSign) != 0;
    SetSZP<N>(f, r);
    return r;
  }
}
template <int N>
[[gnu::always_inline]] inline U<N> Shrd(Flags& f, U<N> dst, U<N> src,
                                        uint32_t cnt) {
  cnt &= kCountMask<N>;
  if (!cnt) return dst;
  if constexpr (N == 8) {
    uint64_t r = (dst >> cnt) | (src << (64 - cnt));
    f.cf = (dst >> (cnt - 1)) & 1;
    f.of = ((r ^ dst) & W<N>::kSign) != 0;
    SetSZP<N>(f, r);
    return r;
  } else {
    uint64_t v = (uint64_t(src & W<N>::kMask) << W<N>::kBits) |
                 (dst & W<N>::kMask);
    f.cf = (v >> (cnt - 1)) & 1;
    uint32_t r = uint32_t(v >> cnt) & W<N>::kMask;
    f.of = ((r ^ dst) & W<N>::kSign) != 0;
    SetSZP<N>(f, r);
    return r;
  }
}

// ---- multiply / divide --------------------------------------------------
// 8-byte forms use the compilers' 128-bit integers (clang and GCC on
// x86-64 and arm64).
using u128 = unsigned __int128;
using i128 = __int128;

// Two- and three-operand IMUL: truncated product, CF=OF=overflow.
template <int N>
[[gnu::always_inline]] inline U<N> Imul2(Flags& f, U<N> a, U<N> b) {
  if constexpr (N == 8) {
    i128 p = i128(int64_t(a)) * int64_t(b);
    uint64_t r = uint64_t(p);
    f.cf = f.of = p != i128(int64_t(r));
    SetSZP<N>(f, r);
    return r;
  } else {
    int64_t p = int64_t(Sext<N>(a)) * Sext<N>(b);
    uint32_t r = uint32_t(p) & W<N>::kMask;
    f.cf = f.of = p != Sext<N>(r);
    SetSZP<N>(f, r);
    return r;
  }
}
// One-operand MUL/IMUL. lo/hi are AL:AH, AX:DX, EAX:EDX or RAX:RDX values.
template <int N>
[[gnu::always_inline]] inline void Mul1(Flags& f, U<N> a, U<N> b, U<N>& lo,
                                        U<N>& hi) {
  if constexpr (N == 8) {
    u128 p = u128(a) * b;
    lo = uint64_t(p);
    hi = uint64_t(p >> 64);
  } else {
    uint64_t p = uint64_t(a & W<N>::kMask) * (b & W<N>::kMask);
    lo = uint32_t(p) & W<N>::kMask;
    hi = uint32_t(p >> W<N>::kBits) & W<N>::kMask;
  }
  f.cf = f.of = hi != 0;
  SetSZP<N>(f, lo);
}
template <int N>
[[gnu::always_inline]] inline void Imul1(Flags& f, U<N> a, U<N> b, U<N>& lo,
                                         U<N>& hi) {
  if constexpr (N == 8) {
    i128 p = i128(int64_t(a)) * int64_t(b);
    lo = uint64_t(p);
    hi = uint64_t(u128(p) >> 64);
    f.cf = f.of = p != i128(int64_t(lo));
  } else {
    int64_t p = int64_t(Sext<N>(a)) * Sext<N>(b);
    lo = uint32_t(p) & W<N>::kMask;
    hi = uint32_t(uint64_t(p) >> W<N>::kBits) & W<N>::kMask;
    f.cf = f.of = p != Sext<N>(lo);
  }
  SetSZP<N>(f, lo);
}
// Returns false on #DE (divide by zero or quotient overflow).
template <int N>
inline bool Div(U<N>& lo, U<N>& hi, U<N> d) {
  if constexpr (N == 8) {
    if (!d) return false;
    u128 n = (u128(hi) << 64) | lo;
    u128 q = n / d;
    if (q >> 64) return false;
    lo = uint64_t(q);
    hi = uint64_t(n % d);
    return true;
  } else {
    d &= W<N>::kMask;
    if (!d) return false;
    uint64_t n = (uint64_t(hi & W<N>::kMask) << W<N>::kBits) |
                 (lo & W<N>::kMask);
    uint64_t q = n / d;
    if (q > W<N>::kMask) return false;
    lo = uint32_t(q);
    hi = uint32_t(n % d);
    return true;
  }
}
template <int N>
inline bool Idiv(U<N>& lo, U<N>& hi, U<N> d) {
  if constexpr (N == 8) {
    int64_t dv = int64_t(d);
    if (!dv) return false;
    i128 n = i128((u128(hi) << 64) | lo);
    if (n == i128(u128(1) << 127) && dv == -1) return false;
    i128 q = n / dv;
    if (q > INT64_MAX || q < INT64_MIN) return false;
    lo = uint64_t(q);
    hi = uint64_t(n % dv);
    return true;
  } else {
    int64_t dv = Sext<N>(d);
    if (!dv) return false;
    int64_t n = int64_t((uint64_t(hi & W<N>::kMask) << W<N>::kBits) |
                        (lo & W<N>::kMask));
    if constexpr (N < 4) {
      constexpr int s = 64 - 2 * W<N>::kBits;  // sign-extend the dividend
      n = int64_t(uint64_t(n) << s) >> s;
    }
    if (n == INT64_MIN && dv == -1) return false;
    int64_t q = n / dv;
    int64_t lim = int64_t(1) << (W<N>::kBits - 1);
    if (q >= lim || q < -lim) return false;
    lo = uint32_t(q) & W<N>::kMask;
    hi = uint32_t(n % dv) & W<N>::kMask;
    return true;
  }
}

// ---- bit ops --------------------------------------------------------------
template <int N>
[[gnu::always_inline]] inline U<N> Bsf(Flags& f, U<N> dst, U<N> src) {
  src &= W<N>::kMask;
  f.zf = src == 0;
  if constexpr (N == 8)
    return src ? uint64_t(__builtin_ctzll(src)) : dst;
  else
    return src ? uint32_t(__builtin_ctz(src)) : dst;
}
template <int N>
[[gnu::always_inline]] inline U<N> Bsr(Flags& f, U<N> dst, U<N> src) {
  src &= W<N>::kMask;
  f.zf = src == 0;
  if constexpr (N == 8)
    return src ? uint64_t(63 - __builtin_clzll(src)) : dst;
  else
    return src ? uint32_t(31 - __builtin_clz(src)) : dst;
}

// LZCNT / TZCNT (ABM, BMI1): CF = zero source, ZF = zero result; the other
// flags are undefined (left alone).
template <int N>
[[gnu::always_inline]] inline U<N> Lzcnt(Flags& f, U<N> src) {
  src &= W<N>::kMask;
  U<N> r;
  if constexpr (N == 8)
    r = src ? U<N>(__builtin_clzll(src)) : 64;
  else
    r = src ? U<N>(__builtin_clz(src) - (32 - W<N>::kBits)) : W<N>::kBits;
  f.cf = src == 0;
  f.zf = r == 0;
  return r;
}
template <int N>
[[gnu::always_inline]] inline U<N> Tzcnt(Flags& f, U<N> src) {
  src &= W<N>::kMask;
  U<N> r;
  if constexpr (N == 8)
    r = src ? U<N>(__builtin_ctzll(src)) : 64;
  else
    r = src ? U<N>(__builtin_ctz(src)) : W<N>::kBits;
  f.cf = src == 0;
  f.zf = r == 0;
  return r;
}

// ---- partial registers --------------------------------------------------
inline uint32_t SetLo8(uint32_t r, uint32_t v) {
  return (r & ~0xFFu) | (v & 0xFFu);
}
inline uint32_t SetHi8(uint32_t r, uint32_t v) {
  return (r & ~0xFF00u) | ((v & 0xFFu) << 8);
}
inline uint32_t SetLo16(uint32_t r, uint32_t v) {
  return (r & ~0xFFFFu) | (v & 0xFFFFu);
}
// x64 registers (the emitter passes both arguments as uint64_t).
inline uint64_t SetLo8(uint64_t r, uint64_t v) {
  return (r & ~0xFFull) | (v & 0xFFu);
}
inline uint64_t SetHi8(uint64_t r, uint64_t v) {
  return (r & ~0xFF00ull) | ((v & 0xFFu) << 8);
}
inline uint64_t SetLo16(uint64_t r, uint64_t v) {
  return (r & ~0xFFFFull) | (v & 0xFFFFu);
}

// ---- flags as bytes (sahf/lahf) -----------------------------------------
[[gnu::always_inline]] inline void Sahf(Flags& f, uint32_t ah) {
  f.cf = ah & 0x01;
  f.pf = ah & 0x04;
  f.af = ah & 0x10;
  f.zf = ah & 0x40;
  f.sf = ah & 0x80;
}
[[gnu::always_inline]] inline uint32_t Lahf(const Flags& f) {
  return uint32_t(PackEFlags(f) & 0xD5u) | 0x02u;
}

// ---- atomics for lock-prefixed instructions -----------------------------
template <typename T>
inline T* AtomicPtr(uint8_t* b, uint64_t a) {
  return reinterpret_cast<T*>(b + a);
}

}  // namespace lift
}  // namespace recomp

// Register/flag caching. Lifted functions keep guest registers in locals
// (which nothing can alias) and write them back before anything that may
// observe or change them. Files lifted from x64 images define
// RECOMP_LIFT_X64 before including this header.
#ifndef RECOMP_LIFT_X64
#define LIFT_ENTER                                                       \
  uint8_t* const B = ::recomp::g_guest_base;                                \
  uint32_t eax = c.eax, ecx = c.ecx, edx = c.edx, ebx = c.ebx,           \
           esp = c.esp, ebp = c.ebp, esi = c.esi, edi = c.edi;           \
  ::recomp::Flags F;                                                        \
  ::recomp::UnpackEFlags(F, c.eflags);                                   \
  const uint32_t FS = c.fs_base;                                         \
  (void)FS;
#else
#define LIFT_ENTER                                                       \
  uint8_t* const B = ::recomp::g_guest_base;                                \
  uint64_t rax = c.rax, rcx = c.rcx, rdx = c.rdx, rbx = c.rbx,           \
           rsp = c.rsp, rbp = c.rbp, rsi = c.rsi, rdi = c.rdi;           \
  uint64_t r8 = c.r8, r9 = c.r9, r10 = c.r10, r11 = c.r11, r12 = c.r12,  \
           r13 = c.r13, r14 = c.r14, r15 = c.r15;                        \
  ::recomp::Flags F;                                                        \
  ::recomp::UnpackEFlags(F, c.eflags);                                   \
  const uint64_t GS = c.gs_base;                                         \
  (void)GS;
#endif

// Tail calls between lifted functions (all void(Ctx&)) are guaranteed
// where the compiler supports it.
#if defined(__clang__)
#define LIFT_MUSTTAIL [[clang::musttail]]
#elif defined(__GNUC__) && __GNUC__ >= 15
#define LIFT_MUSTTAIL [[gnu::musttail]]
#else
#define LIFT_MUSTTAIL
#endif

#ifndef RECOMP_LIFT_X64
#define LIFT_SPILL                                                       \
  c.eax = eax; c.ecx = ecx; c.edx = edx; c.ebx = ebx;                    \
  c.esp = esp; c.ebp = ebp; c.esi = esi; c.edi = edi;                    \
  c.eflags = ::recomp::PackEFlags(F);

#define LIFT_RELOAD                                                      \
  eax = c.eax; ecx = c.ecx; edx = c.edx; ebx = c.ebx;                    \
  esp = c.esp; ebp = c.ebp; esi = c.esi; edi = c.edi;                    \
  ::recomp::UnpackEFlags(F, c.eflags);
#else
#define LIFT_SPILL                                                       \
  c.rax = rax; c.rcx = rcx; c.rdx = rdx; c.rbx = rbx;                    \
  c.rsp = rsp; c.rbp = rbp; c.rsi = rsi; c.rdi = rdi;                    \
  c.r8 = r8; c.r9 = r9; c.r10 = r10; c.r11 = r11;                        \
  c.r12 = r12; c.r13 = r13; c.r14 = r14; c.r15 = r15;                    \
  c.eflags = ::recomp::PackEFlags(F);

#define LIFT_RELOAD                                                      \
  rax = c.rax; rcx = c.rcx; rdx = c.rdx; rbx = c.rbx;                    \
  rsp = c.rsp; rbp = c.rbp; rsi = c.rsi; rdi = c.rdi;                    \
  r8 = c.r8; r9 = c.r9; r10 = c.r10; r11 = c.r11;                        \
  r12 = c.r12; r13 = c.r13; r14 = c.r14; r15 = c.r15;                    \
  ::recomp::UnpackEFlags(F, c.eflags);
#endif
