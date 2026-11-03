// Checks the x87 model's integer fast paths (add/sub/mul/div of normal
// operands, fst m32/m64, fsin/fcos/fsincos) against the SoftFloat paths
// they bypass (x87::g_softfloat_only), then the inline forms lifted runs
// call (x87::run::*Fast: arithmetic with register, m32 and m64 operands,
// m32/m64 loads and stores, compares) against the out-of-line run
// functions on the SoftFloat path, and last the host-double forms
// (x87::run::*D, control word 0x027F) the same way.
// Portable, so it also runs on arm64. Every case runs one
// instruction on identical states both ways and compares the registers,
// the status word (flags, C1) and the tag word bit for bit. The SoftFloat
// path itself is checked against the hardware by x87_test.
//
// Usage: x87_fast_test [cases] [seed]

#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "runtime/platform.h"
#include "runtime/x87.h"

namespace {

using recomp::x87::F80;
using recomp::x87::Op;
using recomp::x87::State;

std::mt19937_64 g_rng;

uint64_t Rand() { return g_rng(); }

// A normal significand: random, or with only the top 24/53 bits set
// (values that came from m32/m64), or close to a rounding tie.
uint64_t Sig() {
  uint64_t s = Rand() | 0x8000000000000000ull;
  switch (Rand() % 8) {
    case 0: return s & ~((1ull << 40) - 1);           // float-exact
    case 1: return s & ~((1ull << 11) - 1);           // double-exact
    case 2: return (s & ~((1ull << 40) - 1)) | (1ull << 39);  // PC24 tie
    case 3: return (s & ~((1ull << 11) - 1)) | (1ull << 10);  // PC53 tie
    case 4: return 0x8000000000000000ull;             // power of two
    case 5: return ~0ull;                             // all ones
    case 6: return s | ((1ull << 40) - 1);            // just below a step
    default: return s;
  }
}

// Trig operands: mostly everyday angles, some needing reduction, some
// close to multiples of pi/2.
F80 Angle() {
  F80 v{Sig(), uint16_t(0x3FFF - 30 + int(Rand() % 52))};
  if (Rand() % 4 == 0) v.se = uint16_t(0x3FFF - 2 + int(Rand() % 6));
  if (Rand() % 8 == 0) {  // k * pi/2 nudged: 0xC90FDAA22168C235 * 2^-63 = pi/2 * ...
    static const uint64_t kHalfPi = 0xC90FDAA22168C235ull;
    int k = 1 + int(Rand() % 16);
    unsigned __int128 p = (unsigned __int128)kHalfPi * k;
    int sh = 0;
    while (p >> (64 + sh)) ++sh;
    v = {uint64_t(p >> sh) ^ (Rand() % 1024), uint16_t(0x3FFF + sh)};
  }
  if (Rand() & 1) v.se |= 0x8000;
  return v;
}

int Exp(int near) {
  switch (Rand() % 7) {
    case 0: return 1 + int(Rand() % 0x7FFE);                  // anywhere
    case 1: return 1 + int(Rand() % 80);                      // near underflow
    case 2: return 0x7FFE - int(Rand() % 80);                 // near overflow
    case 3: return 0x3FFF - 200 + int(Rand() % 400);          // everyday
    case 4: {  // the m32/m64 range edges (stores)
      static const int kEdges[] = {0x3FFF + 127, 0x3FFF - 126, 0x3FFF + 1023,
                                   0x3FFF - 1022};
      return kEdges[Rand() % 4] + int(Rand() % 5) - 2;
    }
    default: {
      int e = near + int(Rand() % 141) - 70;                  // close to the other
      return e < 1 ? 1 : e > 0x7FFE ? 0x7FFE : e;
    }
  }
}

F80 Value(int near) {
  F80 v{Sig(), uint16_t(Exp(near))};
  if (Rand() % 12 == 0) v = {0, 0};  // zero
  if (Rand() & 1) v.se |= 0x8000;
  return v;
}

struct Snap {
  F80 r[8];
  uint16_t sw, tw;
};

// k < 6: arithmetic; 6: fst m32; 7: fst m64 (result in r[0].sig);
// 8: fsin, 9: fcos, 10: fsincos.
Snap Run(bool softfloat_only, uint16_t cw, F80 a, F80 b, int k, bool st0_first) {
  static const Op kOps[] = {Op::Add, Op::Mul, Op::Sub, Op::SubR, Op::Div, Op::DivR};
  recomp::x87::g_softfloat_only = softfloat_only;
  State s;
  recomp::x87::Init(s);
  recomp::x87::SetControlWord(s, cw);
  recomp::x87::LoadF80(s, b);
  recomp::x87::LoadF80(s, a);  // st0 = a, st1 = b
  uint64_t stored = 0;
  if (k == 6)
    stored = recomp::x87::StoreF32(s);
  else if (k == 7)
    stored = recomp::x87::StoreF64(s);
  else if (k == 8)
    recomp::x87::Sin(s);
  else if (k == 9)
    recomp::x87::Cos(s);
  else if (k == 10)
    recomp::x87::SinCos(s);
  else if (st0_first)
    recomp::x87::ArithSt0Sti(s, kOps[k], 1);
  else
    recomp::x87::ArithStiSt0(s, kOps[k], 1);
  Snap r{};
  for (int i = 0; i < 8; i++) r.r[i] = s.r[i];
  r.r[0].sig ^= stored;  // empty slot after two pushes; carries the store
  r.sw = recomp::x87::StatusWord(s);
  r.tw = recomp::x87::TagWord(s);
  return r;
}

bool Same(const Snap& x, const Snap& y) {
  if (x.sw != y.sw || x.tw != y.tw) return false;
  for (int i = 0; i < 8; i++)
    if (x.r[i].sig != y.r[i].sig || x.r[i].se != y.r[i].se) return false;
  return true;
}

// ---- inline run forms ----------------------------------------------------

namespace run = recomp::x87::run;

// A register value: mostly what Value() gives, with 24/53-bit significands
// often, and every special class now and then.
run::V RunValue(int near) {
  F80 v = Value(near);
  switch (Rand() % 24) {
    case 0: v = {0x8000000000000000ull, 0x7FFF}; break;                // inf
    case 1: v = {0xC000000000000000ull | (Rand() >> 2), 0x7FFF}; break; // QNaN
    case 2: v = {0x8000000000000001ull | (Rand() >> 2 & ~(1ull << 62)), 0x7FFF}; break;  // SNaN
    case 3: v = {Rand() >> 1, 0}; break;                                // denormal
    case 4: v = {Rand() | 0x8000000000000000ull, 0}; break;            // pseudo-denormal
    case 5: v = {Rand() >> 1, uint16_t(1 + Rand() % 0x7FFE)}; break;   // unsupported
    case 6: case 7: case 8: v.sig &= ~((1ull << 40) - 1); break;
    case 9: case 10: case 11: v.sig &= ~((1ull << 11) - 1); break;
    case 12: case 13: case 14:  // sparse 53-bit: exact results, ties, carries
      v.sig = 0x8000000000000000ull | (Rand() & Rand() & Rand() & ~((1ull << 11) - 1));
      break;
    case 15: case 16:  // leading ones, then sparse: sums that carry out
      v.sig = (~0ull << (11 + Rand() % 53)) | (Rand() & Rand() & Rand() & ~((1ull << 11) - 1));
      break;
    default: break;
  }
  if (Rand() & 1) v.se ^= 0x8000;
  return run::ToV(v);
}

// m32/m64 operands: normal (often near the other operand), zero,
// denormal, inf, NaN, raw bits.
uint32_t RandF32(int near) {
  uint32_t sign = uint32_t(Rand() & 1) << 31;
  switch (Rand() % 12) {
    case 0: return sign;
    case 1: return sign | uint32_t(Rand() & 0x7FFFFF);
    case 2: return sign | 0x7F800000u;
    case 3: return sign | 0x7F800000u | uint32_t(1 + Rand() % 0x7FFFFF);
    case 4: return uint32_t(Rand());
    default: {
      int e = near - 0x3FFF + 127 + int(Rand() % 61) - 30;
      if (Rand() % 4 == 0) e = 1 + int(Rand() % 254);
      e = e < 1 ? 1 : e > 254 ? 254 : e;
      return sign | uint32_t(e) << 23 | uint32_t(Rand() & 0x7FFFFF);
    }
  }
}
uint64_t RandF64(int near) {
  uint64_t sign = (Rand() & 1) << 63;
  switch (Rand() % 12) {
    case 0: return sign;
    case 1: return sign | (Rand() & 0xFFFFFFFFFFFFFull);
    case 2: return sign | 0x7FF0000000000000ull;
    case 3: return sign | 0x7FF0000000000000ull | (1 + Rand() % 0xFFFFFFFFFFFFFull);
    case 4: return Rand();
    default: {
      int e = near - 0x3FFF + 1023 + int(Rand() % 141) - 70;
      if (Rand() % 4 == 0) e = 1 + int(Rand() % 2046);
      e = e < 1 ? 1 : e > 2046 ? 2046 : e;
      uint64_t f = Rand() & 0xFFFFFFFFFFFFFull;
      if (Rand() % 3 == 0) f &= ~((1ull << 29) - 1);  // float-exact
      return sign | uint64_t(e) << 52 | f;
    }
  }
}

bool SameRV(run::RV x, run::RV y) { return x.sig == y.sig && x.meta == y.meta; }
bool SameSV(run::SV x, run::SV y) { return x.bits == y.bits && x.meta == y.meta; }

// One random case of every inline form; the count of mismatches.
long RunForms(long n) {
  static const Op kOps[] = {Op::Add, Op::Mul, Op::Sub, Op::SubR, Op::Div, Op::DivR};
  static const uint16_t kPc[] = {0x0000, 0x0200, 0x0300};
  run::V a = RunValue(0x3FFF);
  run::V b = RunValue(int(a.se & 0x7FFF));
  if (Rand() % 4 == 0) {  // close to a: cancellation, equal compares
    b = a;
    b.se = (b.se & 0x8000) | ((a.se + Rand() % 3 - 1) & 0x7FFF);
    b.sig ^= (Rand() % 4) << ((Rand() & 1) ? 11 : 40);
    if (Rand() & 1) b.se ^= 0x8000;
  }
  int near = int(a.se & 0x7FFF);
  uint32_t m32 = RandF32(near);
  uint64_t m64 = RandF64(near);
  uint16_t cw = uint16_t(0x007F | kPc[Rand() % 3] | (Rand() % 4) << 10);
  if (Rand() % 2) cw = Rand() & 1 ? 0x027F : uint16_t(0x007F | (Rand() % 4) << 10);
  Op op = kOps[Rand() % 6];
  long fails = 0;
  auto report = [&](const char* what, uint64_t f1, uint64_t f2, uint64_t s1, uint64_t s2) {
    if (++fails, n < 20)
      std::printf("FAIL %s cw=%04X a=%04X:%016" PRIX64 " b=%04X:%016" PRIX64 " m32=%08X"
                  " m64=%016" PRIX64 "\n  fast %016" PRIX64 " %016" PRIX64
                  "\n  soft %016" PRIX64 " %016" PRIX64 "\n",
                  what, cw, uint32_t(a.se), a.sig, uint32_t(b.se), b.sig, m32, m64, f1, f2, s1,
                  s2);
  };
  auto arith = [&](const char* what, run::RV fast, auto&& ref) {
    recomp::x87::g_softfloat_only = true;
    run::RV soft = ref();
    recomp::x87::g_softfloat_only = false;
    if (!SameRV(fast, soft)) report(what, fast.sig, fast.meta, soft.sig, soft.meta);
  };
  auto store = [&](const char* what, run::SV fast, auto&& ref) {
    recomp::x87::g_softfloat_only = true;
    run::SV soft = ref();
    recomp::x87::g_softfloat_only = false;
    if (!SameSV(fast, soft)) report(what, fast.bits, fast.meta, soft.bits, soft.meta);
  };
  auto cmp = [&](const char* what, uint32_t fast, uint32_t soft) {
    if (fast != soft) report(what, fast, 0, soft, 0);
  };
  arith("ArithFast", run::ArithFast(cw, op, a, b), [&] { return run::Arith(cw, op, a, b); });
  arith("ArithF32Fast", run::ArithF32Fast(cw, op, a, m32),
        [&] { return run::ArithF32(cw, op, a, m32); });
  arith("ArithF64Fast", run::ArithF64Fast(cw, op, a, m64),
        [&] { return run::ArithF64(cw, op, a, m64); });
  arith("LoadF32Fast", run::LoadF32Fast(m32), [&] { return run::LoadF32(m32); });
  arith("LoadF64Fast", run::LoadF64Fast(m64), [&] { return run::LoadF64(m64); });
  store("StoreF32Fast", run::StoreF32Fast(cw, a), [&] { return run::StoreF32(cw, a); });
  store("StoreF64Fast", run::StoreF64Fast(cw, a), [&] { return run::StoreF64(cw, a); });
  bool quiet = Rand() & 1;
  cmp("CompareFast", run::CompareFast(a, b, quiet), run::Compare(a, b, quiet));
  cmp("CompareF32Fast", run::CompareF32Fast(a, m32), run::CompareF32(a, m32));
  cmp("CompareF64Fast", run::CompareF64Fast(a, m64), run::CompareF64(a, m64));
  return fails;
}

// ---- host doubles ---------------------------------------------------------
//
// Every *D function against the SoftFloat path at 0x027F. A *D function
// must succeed exactly when its result can be held (zero, or 53 bits with
// a magnitude in [2^-511, 2^512): InRangeV, written independently of
// x87.h), and then give the same value, flags and C1; on failure it must
// leave its outputs alone.

constexpr uint32_t kCw = 0x027F;

bool InRangeV(run::V v) {
  int e = int(v.se & 0x7FFF);
  if (!e && !v.sig) return true;
  return (v.sig >> 63) && !(v.sig & 0x7FF) && e - 0x3FFF >= -511 && e - 0x3FFF <= 511;
}
bool SameV(run::V x, run::V y) { return x.sig == y.sig && x.se == y.se; }
double D(uint64_t bits) { return std::bit_cast<double>(bits); }
uint64_t Bits(double x) { return std::bit_cast<uint64_t>(x); }

// A 52-bit fraction: random, sparse (exact results and ties), power of
// two, all ones, leading ones then sparse, or 24 bits (float-exact).
uint64_t Frac() {
  const uint64_t m = (1ull << 52) - 1;
  switch (Rand() % 8) {
    case 0: return Rand() & Rand() & Rand() & m;
    case 1: return 0;
    case 2: return m;
    case 3: return ((~0ull << (Rand() % 52)) | (Rand() & Rand() & Rand())) & m;
    case 4: return Rand() & m & ~((1ull << 29) - 1);
    case 5: return (Rand() & m & ~((1ull << 29) - 1)) | (1ull << 28);  // 24-bit tie
    default: return Rand() & m;
  }
}
// An unbiased exponent in range ([-511, 511]): at the edges, anywhere,
// everyday, or near `near`.
int ExpIn(int near) {
  int e;
  switch (Rand() % 6) {
    case 0: e = -511 + int(Rand() % 4); break;
    case 1: e = 511 - int(Rand() % 4); break;
    case 2: e = -511 + int(Rand() % 1023); break;
    case 3: e = int(Rand() % 61) - 30; break;
    default: e = near + int(Rand() % 121) - 60; break;
  }
  return e < -511 ? -511 : e > 511 ? 511 : e;
}
double Make(int e, uint64_t frac, bool neg) {
  return D(uint64_t(neg) << 63 | uint64_t(e + 1023) << 52 | frac);
}
double RandIn(int near) {
  if (Rand() % 12 == 0) return (Rand() & 1) ? -0.0 : 0.0;
  return Make(ExpIn(near), Frac(), Rand() & 1);
}
int ExpOf(double x) { return int((Bits(x) >> 52) & 0x7FF) - 1023; }
// A second operand: like the first, or built for cancellation and exact
// zeros, add/sub ties (52-55 binades below), or products and quotients at
// the range edges (just inside, at, or just outside).
double RandOther(double a) {
  int ea = Bits(a) << 1 ? ExpOf(a) : 0;
  bool neg = Rand() & 1;
  auto clamp = [](int e) { return e < -511 ? -511 : e > 511 ? 511 : e; };
  switch (Rand() % 10) {
    case 0: {
      double b = Rand() & 1 ? a : -a;
      if (Rand() & 1 && (Bits(b) << 1)) b = D(Bits(b) ^ (Rand() % 4));
      return b;
    }
    case 1: return Make(clamp(ea - 52 - int(Rand() % 4)), Frac(), neg);
    case 2: return Make(clamp(511 - ea + int(Rand() % 5) - 2), Frac(), neg);    // a*b ~ 2^512
    case 3: return Make(clamp(-511 - ea + int(Rand() % 5) - 2), Frac(), neg);   // a*b ~ 2^-511
    case 4: return Make(clamp(ea - 511 + int(Rand() % 5) - 2), Frac(), neg);    // a/b ~ 2^511
    case 5: return Make(clamp(ea + 511 + int(Rand() % 5) - 2), Frac(), neg);    // a/b ~ 2^-511
    default: return RandIn(ea);
  }
}

// Any m32/m64 bits: in range, at float's and double's edges, out of
// range, zero, denormal, inf, NaN, raw.
uint32_t AnyF32() {
  uint32_t sign = uint32_t(Rand() & 1) << 31;
  switch (Rand() % 6) {
    case 0: return sign | uint32_t(Rand() % 3) << 23 | uint32_t(Rand() & 0x7FFFFF);
    case 1: return sign | uint32_t(253 + Rand() % 3) << 23 | uint32_t(Rand() & 0x7FFFFF);
    default: return RandF32(0x3FFF);
  }
}
uint64_t AnyF64() {
  if (Rand() % 3) return RandF64(0x3FFF + int(Rand() % 1101) - 550);
  int e = Rand() & 1 ? 511 : -511;
  return Bits(Make(e, Frac(), Rand() & 1)) + (Rand() & 1 ? 1ull << 52 : 0) -
         (Rand() & 1 ? 1ull << 52 : 0);
}

// A double result: true for success with value/flags/C1, compared with the
// reference RV; failure only when the reference cannot be held.
template <class F>
long CheckD(const char* what, run::RV ref, F&& fn, double a, double b) {
  const double kSentinel = 12345.0;
  double out = kSentinel;
  uint32_t fl = 0x40;  // untouched bits must stay
  bool up = true;
  bool ok = fn(&out, &fl, &up);
  bool want = InRangeV(run::Val(ref));
  bool good = ok == want &&
              (ok ? SameV(run::FromD(out), run::Val(ref)) &&
                        fl == (0x40 | run::Flags(ref)) && up == run::Up(ref)
                  : Bits(out) == Bits(kSentinel) && fl == 0x40 && up);
  if (good) return 0;
  std::printf("FAIL %s a=%a b=%a ok=%d out=%a fl=%X up=%d\n  soft %04X:%016" PRIX64
              " fl=%X up=%d\n",
              what, a, b, ok, out, fl, up, unsigned(run::Val(ref).se), run::Val(ref).sig,
              run::Flags(ref), run::Up(ref));
  return 1;
}

long DoubleForms() {
  static const Op kOps[] = {Op::Add, Op::Mul, Op::Sub, Op::SubR, Op::Div, Op::DivR};
  static const char* kOpNames[] = {"ArithD add", "ArithD mul", "ArithD sub",
                                   "ArithD subr", "ArithD div", "ArithD divr"};
  long fails = 0;
  recomp::x87::g_softfloat_only = true;
  double a = RandIn(int(Rand() % 61) - 30), b = RandOther(a);
  int k = int(Rand() % 6);
  run::RV ref = run::Arith(kCw, kOps[k], run::FromD(a), run::FromD(b));
  fails += CheckD(kOpNames[k], ref, [&](double* o, uint32_t* f, bool* u) {
    return run::ArithD(kOps[k], a, b, o, f, u);
  }, a, b);
  // sqrt: in range, and negative operands (NaN, IE: never held).
  ref = run::Sqrt(kCw, run::FromD(a));
  fails += CheckD("SqrtD", ref, [&](double* o, uint32_t* f, bool* u) {
    return run::SqrtD(a, o, f, u);
  }, a, 0);
  // m32/m64 operands: held exactly when the load is flag-free and in range.
  uint32_t m32 = AnyF32();
  uint64_t m64 = AnyF64();
  auto load = [&](const char* what, run::RV r, bool ok, double v, uint64_t raw) {
    bool want = run::Flags(r) == 0 && InRangeV(run::Val(r));
    if (ok == want && (!ok || SameV(run::FromD(v), run::Val(r)))) return 0;
    std::printf("FAIL %s %016" PRIX64 " ok=%d\n", what, raw, ok);
    return 1;
  };
  double v = 7.0;
  fails += load("OperandF32", run::LoadF32(m32), run::OperandF32(m32, &v), v, m32);
  v = 7.0;
  fails += load("OperandF64", run::LoadF64(m64), run::OperandF64(m64, &v), v, m64);
  // As a run does it: two m64 loads and an operation, from any bits.
  uint64_t n64 = Rand() % 64 ? AnyF64() : Bits(-D(m64));
  double x0, x1;
  if (Rand() % 64 == 0) {
    // (2^53 - 1) * 2^-1075: binary64 rounds it to 2^-1022 (its subnormal
    // quantum), the x87 at PC=53 holds it exactly. Normal operands and a
    // result >= 2^-1022 are not enough.
    m64 = Bits(6361 * 0x1p-600);
    n64 = Bits(1416003655831 * 0x1p-475);
    k = 1;
  }
  if (run::OperandF64(m64, &x0) && run::OperandF64(n64, &x1)) {
    run::RV r = run::ArithF64(kCw, kOps[k], run::Val(run::LoadF64(m64)), n64);
    fails += CheckD(kOpNames[k], r, [&](double* o, uint32_t* f, bool* u) {
      return run::ArithD(kOps[k], x0, x1, o, f, u);
    }, x0, x1);
  }
  // FCOM m64 takes any zero or normal value.
  v = 7.0;
  run::RV l64 = run::LoadF64(m64);
  bool ok = run::CompareOperandF64(m64, &v);
  if (ok != (run::Flags(l64) == 0 && run::Ordinary(run::Val(l64)) &&
             (run::Val(l64).se & 0x7FFF) != 0x7FFF) ||
      (ok && Bits(v) != m64) || (!ok && v != 7.0)) {
    std::printf("FAIL CompareOperandF64 %016" PRIX64 " ok=%d\n", m64, ok);
    ++fails;
  }
  // FILD m64: held when |v| <= 2^53 (larger ones held exactly by a double
  // go to the exact path too).
  int64_t iv = int64_t(Rand()) >> (Rand() % 64);
  if (Rand() % 4 == 0) iv = (Rand() & 1 ? 1 : -1) * ((1ll << 53) + int64_t(Rand() % 5) - 2);
  v = 7.0;
  ok = run::OperandI64(iv, &v);
  run::V fi = run::FromInt(iv);
  bool small = iv >= -(1ll << 53) && iv <= (1ll << 53);
  if (ok != small || (ok && !SameV(run::FromD(v), fi)) || (!ok && v != 7.0)) {
    std::printf("FAIL OperandI64 %" PRId64 " ok=%d\n", iv, ok);
    ++fails;
  }
  // Registers to doubles and back: any value, held exactly when in range.
  run::V rv = Rand() & 1 ? run::FromD(RandIn(0)) : RunValue(0x3FFF + int(Rand() % 1101) - 550);
  v = 7.0;
  ok = run::ToD(rv, &v);
  if (ok != InRangeV(rv) || (ok && !SameV(run::FromD(v), rv)) || (!ok && v != 7.0)) {
    std::printf("FAIL ToD %04X:%016" PRIX64 " ok=%d\n", unsigned(rv.se), rv.sig, ok);
    ++fails;
  }
  // Stores of a held value: m32 near float's range edges and ties, m64.
  double x = a;
  switch (Rand() % 4) {
    case 0: x = Make(-126 - int(Rand() % 3) + int(Rand() % 2), Frac(), Rand() & 1); break;
    case 1: x = Make(127 + int(Rand() % 2), Frac() | (Rand() & 1 ? (1ull << 52) - 1 : 0), Rand() & 1); break;
    case 2: x = Make(int(Rand() % 253) - 126, Frac(), Rand() & 1); break;
    default: break;
  }
  run::SV s32 = run::StoreF32D(kCw, x), r32 = run::StoreF32(kCw, run::FromD(x));
  if (!SameSV(s32, r32)) {
    std::printf("FAIL StoreF32D %a: %08" PRIX64 " %" PRIX64 " soft %08" PRIX64 " %" PRIX64 "\n",
                x, s32.bits, s32.meta, r32.bits, r32.meta);
    ++fails;
  }
  run::SV r64 = run::StoreF64(kCw, run::FromD(x));
  if (run::StoreF64D(x) != r64.bits || r64.meta) {
    std::printf("FAIL StoreF64D %a\n", x);
    ++fails;
  }
  // Compares, fchs and fabs.
  uint32_t c = run::Compare(run::FromD(a), run::FromD(b), Rand() & 1);
  if (run::CompareD(a, b) != c) {
    std::printf("FAIL CompareD %a %a: %X soft %X\n", a, b, run::CompareD(a, b), c);
    ++fails;
  }
  run::V na = run::FromD(a), aa = na;
  na.se ^= 0x8000;
  aa.se &= 0x7FFF;
  if (!SameV(run::FromD(-a), na) || !SameV(run::FromD(run::AbsD(a)), aa)) {
    std::printf("FAIL fchs/fabs %a\n", a);
    ++fails;
  }
  recomp::x87::g_softfloat_only = false;
  return fails;
}

}  // namespace

int main(int argc, char** argv) {
  long cases = argc > 1 ? std::atol(argv[1]) : 2000000;
  g_rng.seed(argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 1);
  static const char* kNames[] = {"add", "mul", "sub", "subr", "div", "divr",
                                 "fst m32", "fst m64", "fsin", "fcos", "fsincos"};
  static const uint16_t kPc[] = {0x0000, 0x0200, 0x0300};
  long fails = 0;
  for (long n = 0; n < cases; n++) {
    F80 a = Value(0x3FFF);
    F80 b = Value(a.se & 0x7FFF);
    if (Rand() % 16 == 0) b = {a.sig ^ (Rand() % 4), a.se};  // cancellation
    uint16_t cw = uint16_t(0x007F | kPc[Rand() % 3] | (Rand() % 4) << 10);
    if (Rand() % 4 == 0) {  // games' common modes: PC=24 (any RC), 24-bit operands,
      // or PC=53 with RC=nearest, 53-bit operands
      bool pc53 = Rand() & 1;
      cw = pc53 ? 0x027F : uint16_t(0x007F | (Rand() % 4) << 10);
      const uint64_t low = pc53 ? (1ull << 11) - 1 : (1ull << 40) - 1;
      a.sig = (a.sig | 0x8000000000000000ull) & ~low;
      b.sig = (b.sig | 0x8000000000000000ull) & ~low;
      if (!(a.se & 0x7FFF)) a.se |= 0x3FFF;
      if (!(b.se & 0x7FFF)) b.se |= 0x3FFF;
    }
    int k = int(Rand() % 11);
    if (k >= 8) a = Angle();
    bool st0_first = Rand() & 1;
    Snap fast = Run(false, cw, a, b, k, st0_first);
    Snap ref = Run(true, cw, a, b, k, st0_first);
    if (Same(fast, ref)) continue;
    if (++fails <= 20) {
      int dst = st0_first ? 0 : 1;  // physical reg after the two pushes
      int p = k == 6 || k == 7 ? 0 : k >= 8 ? 6 : (6 + dst) & 7;
      std::printf("FAIL %s%s cw=%04X a=%04X:%016" PRIX64 " b=%04X:%016" PRIX64
                  "\n  fast %04X:%016" PRIX64 " sw=%04X\n  soft %04X:%016" PRIX64
                  " sw=%04X\n",
                  kNames[k], st0_first ? "" : " (st1 dst)", cw, a.se, a.sig, b.se,
                  b.sig, fast.r[p].se, fast.r[p].sig, fast.sw, ref.r[p].se,
                  ref.r[p].sig, ref.sw);
    }
  }
  std::printf("%ld cases, %ld mismatches\n", cases, fails);
  long run_fails = 0;
  for (long n = 0; n < cases; n++) run_fails += RunForms(run_fails);
  std::printf("%ld cases of the inline run forms, %ld mismatches\n", cases, run_fails);
  // The host-double forms rely on the default environment.
  if (const char* why = recomp::platform::HostFpProblem()) {
    std::printf("FAIL host floating-point environment: %s\n", why);
    return 1;
  }
  long dbl_fails = 0;
  for (long n = 0; n < cases && dbl_fails < 20; n++) dbl_fails += DoubleForms();
  std::printf("%ld cases of the host-double forms, %ld mismatches\n", cases, dbl_fails);
  for (uint16_t cw : {0x027F, 0x067F, 0x0A7F, 0x0E7F, 0x007F, 0x037F, 0x127F})
    if (run::DoubleMode(cw) != (cw == 0x027F || cw == 0x127F)) {
      std::printf("FAIL DoubleMode %04X\n", cw);
      ++dbl_fails;
    }
  return fails || run_fails || dbl_fails ? 1 : 0;
}
