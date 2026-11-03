// Exact x87 FPU model (see x87.h and ARCHITECTURE.md, "x87: full 80-bit
// model").
//
// Arithmetic (add/sub/mul/div/sqrt, conversions, frndint) runs on Berkeley
// SoftFloat 3e, whose extF80 code implements the x87 precision control. The
// x87-specific parts SoftFloat does not know about are done here: operand
// classes (unsupported encodings, pseudo-denormals), the x87 NaN rules,
// exception priorities (IE/ZE suppress DE), the denormal-operand flag, C1
// round-up reporting, the register stack and tag word, and the special
// instructions (fprem, fscale, fxtract, constants).
//
// The transcendentals are computed in binary128 (SoftFloat f128) from our
// own argument reduction, series and tables, with the final operation in
// round-to-odd so the single rounding to 64 bits with the guest RC is
// correct to well below 1 ulp. They reproduce the hardware's documented
// quirks where those are observable: argument reduction with the x87's
// 66-bit pi, the |x| >= 2^63 range limit, results for tiny arguments, and
// the 66-bit pi used for the constant results of fpatan.
//
// Everything is portable C++: no host FPU, no long double, no intrinsics.
// Hardware behaviour was cross-checked against Bochs' documentation of it
// and against a real CPU (tests/x87_test.cpp); no code was taken from
// Bochs.

#include "runtime/x87.h"

#include <bit>
#include <climits>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "softfloat.h"
}

namespace recomp::x87 {

bool g_softfloat_only = false;

// Hot helpers of the arithmetic path are forced inline: as calls they pass
// F80s and small result structs through memory (store-forwarding stalls).
#define X87_INLINE [[gnu::always_inline]] inline

namespace {

// --- status / control word bits -------------------------------------

constexpr uint16_t kIE = 0x0001;
constexpr uint16_t kDE = 0x0002;
constexpr uint16_t kZE = 0x0004;
constexpr uint16_t kOE = 0x0008;
constexpr uint16_t kUE = 0x0010;
constexpr uint16_t kPE = 0x0020;
constexpr uint16_t kSF = 0x0040;
constexpr uint16_t kES = 0x0080;
constexpr uint16_t kC0 = 0x0100;
constexpr uint16_t kC1 = 0x0200;
constexpr uint16_t kC2 = 0x0400;
constexpr uint16_t kC3 = 0x4000;
constexpr uint16_t kBusy = 0x8000;
constexpr uint16_t kTopMask = 0x3800;
constexpr uint16_t kExceptionMask = 0x003F;
constexpr uint16_t kUnordered = kC3 | kC2 | kC0;

// --- 80-bit values ---------------------------------------------------

constexpr uint64_t kJ = 0x8000000000000000ull;      // explicit integer bit
constexpr uint64_t kQuiet = 0x4000000000000000ull;  // NaN quiet bit
constexpr uint64_t kHalf = 0x8000000000000000ull;
constexpr F80 kIndefinite = {0xC000000000000000ull, 0xFFFF};
constexpr F80 kOne = {kJ, 0x3FFF};

constexpr int ExpOf(F80 v) { return v.se & 0x7FFF; }
constexpr bool SignOf(F80 v) { return (v.se & 0x8000) != 0; }
constexpr F80 Make(bool sign, int exp, uint64_t sig) {
  return {sig, static_cast<uint16_t>((sign ? 0x8000 : 0) | exp)};
}
constexpr F80 Zero(bool sign) { return Make(sign, 0, 0); }
constexpr F80 Inf(bool sign) { return Make(sign, 0x7FFF, kJ); }

// Unnormals, pseudo-NaNs and pseudo-infinities: the 387 and later reject
// them as operands with IE. Pseudo-denormals (exp 0, J set) are accepted.
constexpr bool IsUnsupported(F80 v) {
  return ExpOf(v) != 0 && !(v.sig & kJ);
}
constexpr bool IsNaN(F80 v) {
  return ExpOf(v) == 0x7FFF && (v.sig & kJ) && (v.sig << 1) != 0;
}
constexpr bool IsSNaN(F80 v) { return IsNaN(v) && !(v.sig & kQuiet); }
constexpr bool IsInf(F80 v) { return ExpOf(v) == 0x7FFF && v.sig == kJ; }
constexpr bool IsZero(F80 v) { return ExpOf(v) == 0 && v.sig == 0; }
constexpr bool IsDenormal(F80 v) { return ExpOf(v) == 0 && v.sig != 0; }
constexpr F80 Quiet(F80 v) { return {v.sig | kQuiet, v.se}; }

// A pseudo-denormal has the value of the same significand with exponent 1;
// SoftFloat and the code below expect that encoding.
constexpr F80 Canonical(F80 v) {
  if (ExpOf(v) == 0 && (v.sig & kJ)) v.se |= 1;
  return v;
}

// Normalised (unbiased exponent, significand with J set) of a finite
// non-zero value, denormals included. value = sig / 2^63 * 2^exp.
struct Norm {
  int exp;
  uint64_t sig;
};
Norm Normalize(F80 v) {
  int e = ExpOf(v);
  uint64_t sig = v.sig;
  if (e == 0) {
    e = 1;
    int lz = std::countl_zero(sig);
    sig <<= lz;
    e -= lz;
  }
  return {e - 0x3FFF, sig};
}

F80 FromInt(int64_t v) {
  if (v == 0) return Zero(false);
  uint64_t m = v < 0 ? 0 - static_cast<uint64_t>(v) : static_cast<uint64_t>(v);
  int lz = std::countl_zero(m);
  return Make(v < 0, 0x3FFF + 63 - lz, m << lz);
}

// Exact m32/m64 -> extended conversions. SNaNs stay signalling so the NaN
// rules can tell them from QNaNs; denormal sources are normalised and
// reported through *denormal (DE is raised by the instruction).
F80 FromF32(uint32_t bits, bool* denormal) {
  bool sign = bits >> 31;
  int e = (bits >> 23) & 0xFF;
  uint64_t frac = bits & 0x7FFFFF;
  *denormal = false;
  if (e == 0xFF) return Make(sign, 0x7FFF, kJ | (frac << 40));
  if (e == 0) {
    if (!frac) return Zero(sign);
    *denormal = true;
    int lz = std::countl_zero(frac);
    return Make(sign, 0x3FFF - 126 - 23 + 63 - lz, frac << lz);
  }
  return Make(sign, e - 127 + 0x3FFF, kJ | (frac << 40));
}

F80 FromF64(uint64_t bits, bool* denormal) {
  bool sign = bits >> 63;
  int e = (bits >> 52) & 0x7FF;
  uint64_t frac = bits & 0xFFFFFFFFFFFFFull;
  *denormal = false;
  if (e == 0x7FF) return Make(sign, 0x7FFF, kJ | (frac << 11));
  if (e == 0) {
    if (!frac) return Zero(sign);
    *denormal = true;
    int lz = std::countl_zero(frac);
    return Make(sign, 0x3FFF - 1022 - 52 + 63 - lz, frac << lz);
  }
  return Make(sign, e - 1023 + 0x3FFF, kJ | (frac << 11));
}

// x87 NaN rules (Intel SDM vol. 1, table 4-7): an SNaN operand raises IE;
// a QNaN beats an SNaN; two NaNs of the same kind give the one with the
// larger significand; the result is always quiet.
F80 PropagateNaN(F80 a, F80 b, uint16_t* flags) {
  bool na = IsNaN(a), nb = IsNaN(b);
  bool sa = na && !(a.sig & kQuiet), sb = nb && !(b.sig & kQuiet);
  if (sa || sb) *flags |= kIE;
  if (!nb) return Quiet(a);
  if (!na) return Quiet(b);
  if (sa != sb) return Quiet(sa ? b : a);
  if (a.sig != b.sig) return Quiet(a.sig > b.sig ? a : b);
  return Quiet(SignOf(a) ? b : a);  // equal payloads: positive one
}

F80 PropagateNaN1(F80 a, uint16_t* flags) {
  if (!(a.sig & kQuiet)) *flags |= kIE;
  return Quiet(a);
}

// --- SoftFloat glue --------------------------------------------------

extFloat80_t ToSf(F80 v) {
  extFloat80_t r;
  r.signif = v.sig;
  r.signExp = v.se;
  return r;
}
F80 FromSf(extFloat80_t v) { return {v.signif, v.signExp}; }

int RcOf(uint32_t cw) { return (cw >> 10) & 3; }
int RcOf(const State& s) { return RcOf(s.cw); }

uint8_t SfMode(int rc) {
  static constexpr uint8_t kModes[4] = {
      softfloat_round_near_even, softfloat_round_min, softfloat_round_max,
      softfloat_round_minMag};
  return kModes[rc];
}

// PC 00 = 24 bits, 10 = 53, 11 = 64. The reserved 01 behaves as 64 bits
// on the CPUs we checked.
uint8_t SfPrecision(uint32_t cw) {
  switch ((cw >> 8) & 3) {
    case 0: return 32;
    case 2: return 64;
    default: return 80;
  }
}

void SfBegin(uint8_t mode, uint8_t precision) {
  softfloat_roundingMode = mode;
  extF80_roundingPrecision = precision;
  softfloat_exceptionFlags = 0;
}

uint16_t FromSfFlags(uint_fast8_t f) {
  uint16_t r = 0;
  if (f & softfloat_flag_invalid) r |= kIE;
  if (f & softfloat_flag_infinite) r |= kZE;
  if (f & softfloat_flag_overflow) r |= kOE;
  if (f & softfloat_flag_underflow) r |= kUE;
  if (f & softfloat_flag_inexact) r |= kPE;
  return r;
}

// C1 after an inexact result: set when the magnitude was rounded up. For
// the directed modes that follows from the sign; round-to-nearest needs
// the truncated result for comparison (computed lazily, it costs a second
// operation).
template <typename Truncated>
bool RoundedUp(int rc, bool sign, Truncated truncated_differs) {
  switch (rc) {
    case 0: return truncated_differs();
    case 1: return sign;   // toward -inf
    case 2: return !sign;  // toward +inf
    default: return false;
  }
}

// --- rounding of exact intermediate results --------------------------

struct Rounded {
  F80 v;
  uint16_t flags;  // PE, UE, OE
  bool up;         // magnitude rounded up (C1)
  bool tiny;       // below the normal range (after rounding)
};

X87_INLINE void ShiftRightJam128(uint64_t& hi, uint64_t& lo, int n) {
  if (n <= 0) return;
  if (n >= 128) {
    lo = (hi | lo) != 0;
    hi = 0;
  } else if (n >= 64) {
    uint64_t sticky = lo != 0;
    if (n > 64) sticky |= (hi << (128 - n)) != 0;
    lo = (n == 64 ? hi : hi >> (n - 64)) | sticky;
    hi = 0;
  } else {
    uint64_t sticky = (lo << (64 - n)) != 0;
    lo = (lo >> n) | (hi << (64 - n)) | sticky;
    hi >>= n;
  }
}

// keep = the top 64 - drop bits of hi:lo, rest = the rest as a fraction of
// 2^64 with a sticky bit. (Functions, not lambdas in RoundPack: compilers
// left the lambdas out of line.)
X87_INLINE void RpSplit(uint64_t h, uint64_t l, int drop, uint64_t* keep, uint64_t* rest) {
  if (drop == 0) {
    *keep = h;
    *rest = l;
  } else {
    *keep = h >> drop;
    *rest = (h << (64 - drop)) | (l != 0);
  }
}
// Whether rounding with RC increments the kept magnitude.
X87_INLINE bool RpInc(int rc, bool sign, uint64_t keep, uint64_t rest) {
  switch (rc) {
    case 0: return rest > kHalf || (rest == kHalf && (keep & 1));
    case 1: return sign && rest != 0;
    case 2: return !sign && rest != 0;
    default: return false;
  }
}

// Rounds (-1)^sign * (hi + lo/2^64) / 2^63 * 2^(exp - 0x3FFF), hi
// normalised (bit 63 set), to `prec` significand bits with RC `rc`, the
// way the x87 rounds into a register: full 15-bit exponent range, gradual
// underflow with tininess detected after rounding, masked overflow
// response. `exp` may lie outside the encodable range.
X87_INLINE Rounded RoundPack(bool sign, int32_t exp, uint64_t hi, uint64_t lo, int prec,
                  int rc) {
  Rounded out{};
  const int drop = 64 - prec;
  const uint64_t keep_max = prec == 64 ? ~0ull : (1ull << prec) - 1;

  bool denormal = false, tiny = false;
  if (exp < 1) {
    uint64_t keep, rest;
    RpSplit(hi, lo, drop, &keep, &rest);
    tiny = exp < 0 || !(keep == keep_max && RpInc(rc, sign, keep, rest));
    ShiftRightJam128(hi, lo, 1 - exp);
    exp = 1;
    denormal = true;
  }
  uint64_t keep, rest;
  RpSplit(hi, lo, drop, &keep, &rest);
  bool inc = RpInc(rc, sign, keep, rest);
  if (rest) out.flags |= kPE;
  if (tiny && rest) out.flags |= kUE;
  out.tiny = tiny;
  out.up = inc;
  if (inc) {
    if (keep == keep_max) {
      keep = 1ull << (prec - 1);
      ++exp;
    } else {
      ++keep;
    }
  }
  uint64_t sig = keep << drop;
  if (denormal) {
    out.v = Make(sign, (sig & kJ) ? 1 : 0, sig);
    return out;
  }
  if (exp >= 0x7FFF) {
    out.flags |= kOE | kPE;
    bool to_inf = rc == 0 || (rc == 2 && !sign) || (rc == 1 && sign);
    out.up = to_inf;
    out.v = to_inf ? Inf(sign) : Make(sign, 0x7FFE, keep_max << drop);
    return out;
  }
  out.v = Make(sign, exp, sig);
  return out;
}

// --- binary128 helpers for the transcendentals -------------------------

using Q = float128_t;

struct QBits {
  uint64_t lo, hi;
};
Q Ld(const QBits& b) {
  Q q;
  q.v[0] = b.lo;
  q.v[1] = b.hi;
  return q;
}

Q QAdd(Q a, Q b) { return f128_add(a, b); }
Q QSub(Q a, Q b) { return f128_sub(a, b); }
Q QMul(Q a, Q b) { return f128_mul(a, b); }
Q QDiv(Q a, Q b) { return f128_div(a, b); }
Q QNeg(Q a) {
  a.v[1] ^= 0x8000000000000000ull;
  return a;
}
Q QInt(int32_t v) { return i32_to_f128(v); }
Q QOne() { return Ld({0, 0x3FFF000000000000ull}); }
bool QIsZero(Q a) { return ((a.v[1] << 1) | a.v[0]) == 0; }
// Multiplication by 2^n: exact while the result stays normal (the fast
// path just moves the exponent), rounded like any multiply otherwise.
Q QScale(Q a, int n) {
  int e = static_cast<int>((a.v[1] >> 48) & 0x7FFF);
  if (QIsZero(a) || e == 0x7FFF) return a;
  if (e != 0 && e + n > 0 && e + n < 0x7FFF) {
    a.v[1] += static_cast<uint64_t>(static_cast<int64_t>(n)) << 48;
    return a;
  }
  while (n != 0) {
    int step = n > 16000 ? 16000 : n < -16000 ? -16000 : n;
    Q p = Ld({0, static_cast<uint64_t>(0x3FFF + step) << 48});
    a = f128_mul(a, p);
    n -= step;
  }
  return a;
}
// Exact extended -> binary128 (also for denormals).
Q QFrom(F80 v) { return extF80_to_f128(ToSf(Canonical(v))); }
// value = sig / 2^63 * 2^exp, exact within the normal binary128 range.
Q QFromSig(uint64_t sig, int exp) {
  return QScale(extF80_to_f128(ToSf(Make(false, 0x3FFF, sig))), exp);
}
// value = (hi:lo) * 2^scale for an integer of at most 113 significant
// bits; result must be a normal binary128.
Q QFromU128(uint64_t hi, uint64_t lo, int scale) {
  Q q{};
  if (!hi && !lo) return q;
  int msb = hi ? 127 - std::countl_zero(hi) : 63 - std::countl_zero(lo);
  // Shift so the leading one lands on bit 112.
  int sh = 112 - msb;
  if (sh > 0) {
    if (sh >= 64) {
      hi = lo << (sh - 64);
      lo = 0;
    } else {
      hi = (hi << sh) | (lo >> (64 - sh));
      lo <<= sh;
    }
  } else if (sh < 0) {
    ShiftRightJam128(hi, lo, -sh);  // callers never lose bits here
  }
  uint64_t e = static_cast<uint64_t>(0x3FFF + msb + scale);
  q.v[1] = (e << 48) | (hi & 0xFFFFFFFFFFFFull);
  q.v[0] = lo;
  return q;
}

int Clz128(uint64_t hi, uint64_t lo) {
  return hi ? std::countl_zero(hi) : 64 + std::countl_zero(lo);
}

// Shifts a non-zero 128-bit value left until bit 127 is set; returns the
// shift.
X87_INLINE int Normalize128(uint64_t* hi, uint64_t* lo) {
  int lz = Clz128(*hi, *lo);
  if (lz >= 64) {
    *hi = *lo << (lz - 64);
    *lo = 0;
  } else if (lz) {
    *hi = *hi << lz | *lo >> (64 - lz);
    *lo <<= lz;
  }
  return lz;
}

// Rounds a binary128 value times 2^extra to an extended register value
// with RC (precision control does not apply to these instructions).
Rounded RoundQ(Q q, int32_t extra, int rc) {
  bool sign = q.v[1] >> 63;
  int32_t exp = (q.v[1] >> 48) & 0x7FFF;
  uint64_t fh = q.v[1] & 0xFFFFFFFFFFFFull, fl = q.v[0];
  if (exp == 0x7FFF) return {Inf(sign), 0, false, false};
  if (exp == 0) {
    if (!(fh | fl)) return {Zero(sign), 0, false, false};
    exp = 1;
  } else {
    fh |= 1ull << 48;
  }
  uint64_t hi = (fh << 15) | (fl >> 49), lo = fl << 15;
  int lz = Normalize128(&hi, &lo);
  return RoundPack(sign, exp - lz + extra, hi, lo, 64, rc);
}

// Final operation of a transcendental: computed in round-to-odd, so the
// binary128 result keeps a sticky bit and the one rounding to 64 bits in
// RoundQ is correct.
template <typename F>
Q Odd(F f) {
  softfloat_roundingMode = softfloat_round_odd;
  Q r = f();
  softfloat_roundingMode = softfloat_round_near_even;
  return r;
}

// Constants (generated with 90-digit decimal arithmetic, rounded to
// nearest binary128).

// (-1)^k / (2k+1)!, k = 1..14
constexpr QBits kSinCoef[] = {
    {0x5555555555555555ull, 0xbffc555555555555ull},
    {0x1111111111111111ull, 0x3ff8111111111111ull},
    {0xa01a01a01a01a01aull, 0xbff2a01a01a01a01ull},
    {0x38faac1c88e50017ull, 0x3fec71de3a556c73ull},
    {0x38fe747e4b837dc7ull, 0xbfe5ae64567f544eull},
    {0x97ca38331d23af68ull, 0x3fde6124613a86d0ull},
    {0xf11d8656b0ee8cb0ull, 0xbfd6ae7f3e733b81ull},
    {0xa6b2605197771b00ull, 0x3fce952c77030ad4ull},
    {0x724ca1ec3b7b9675ull, 0xbfc62f49b4681415ull},
    {0x18bef146fcee6e45ull, 0x3fbd71b8ef6dcf57ull},
    {0x9d97b8704dd7f628ull, 0xbfb4761b41316381ull},
    {0x8d4e44a419776f11ull, 0x3fab3f3ccdd165faull},
    {0x320a9a18f15d4277ull, 0xbfa1d1ab1c2dcceaull},
    {0xd7abe30e7766f129ull, 0x3f98259f98b4358aull},
};
// (-1)^k / (2k)!, k = 1..14
constexpr QBits kCosCoef[] = {
    {0x0000000000000000ull, 0xbffe000000000000ull},
    {0x5555555555555555ull, 0x3ffa555555555555ull},
    {0x6c16c16c16c16c17ull, 0xbff56c16c16c16c1ull},
    {0xa01a01a01a01a01aull, 0x3fefa01a01a01a01ull},
    {0xc72ef016d3ea6679ull, 0xbfe927e4fb7789f5ull},
    {0x7b544da987acfe85ull, 0x3fe21eed8eff8d89ull},
    {0xd20badf145dfa3e5ull, 0xbfda93974a8c07c9ull},
    {0xf11d8656b0ee8cb0ull, 0x3fd2ae7f3e733b81ull},
    {0x77bb004886a2c2abull, 0xbfca6827863b97d9ull},
    {0x507a9cad2bf8f0bbull, 0x3fc1e542ba402022ull},
    {0x29450c90b7f338ecull, 0xbfb90ce396db7f85ull},
    {0x7cca4b4067ca9d8aull, 0x3faff2cf01972f57ull},
    {0x9a38f2050ba6b015ull, 0xbfa688e85fc6a4e5ull},
    {0xd373c5c51c354a8dull, 0x3f9d0a18a2635085ull},
};
// atan(k/16), k = 0..16
constexpr QBits kAtanTable[] = {
    {0x0000000000000000ull, 0x0000000000000000ull},
    {0x9c6d964f25b81c5cull, 0x3ffaff55bb72cfdeull},
    {0xdc65912f313e7d11ull, 0x3ffbfd5ba9aac2f6ull},
    {0x2268f6169f103939ull, 0x3ffc7b97b4bce5b0ull},
    {0xd62adb8f3debef44ull, 0x3ffcf5b75f92c80dull},
    {0xbcd38b576931a4f6ull, 0x3ffd362773707ebcull},
    {0x08e715464245b9fdull, 0x3ffd6f61941e4defull},
    {0xcb6c84f92bd2003dull, 0x3ffda64eec3cc23full},
    {0xf68adfc88bd97875ull, 0x3ffddac670561bb4ull},
    {0xfc5496d41396c34aull, 0x3ffe0657e94db30cull},
    {0x3f36b906bc2ccb88ull, 0x3ffe1e00babdefebull},
    {0xb440844df1c440a0ull, 0x3ffe345f01cce37bull},
    {0x12483350fe548afbull, 0x3ffe4978fa3269eeull},
    {0x810028e4bc5e7ca4ull, 0x3ffe5d58987169b1ull},
    {0x3ce7965b4aa42149ull, 0x3ffe700a7c578463ull},
    {0xcc8113bac588dd26ull, 0x3ffe819d0b7158a4ull},
    {0x8469898cc51701b8ull, 0x3ffe921fb54442d1ull},
};
// (-1)^k / (2k+1), k = 1..12
constexpr QBits kAtanCoef[] = {
    {0x5555555555555555ull, 0xbffd555555555555ull},
    {0x999999999999999aull, 0x3ffc999999999999ull},
    {0x2492492492492492ull, 0xbffc249249249249ull},
    {0xc71c71c71c71c71cull, 0x3ffbc71c71c71c71ull},
    {0x5d1745d1745d1746ull, 0xbffb745d1745d174ull},
    {0x3b13b13b13b13b14ull, 0x3ffb3b13b13b13b1ull},
    {0x1111111111111111ull, 0xbffb111111111111ull},
    {0xe1e1e1e1e1e1e1e2ull, 0x3ffae1e1e1e1e1e1ull},
    {0x86bca1af286bca1bull, 0xbffaaf286bca1af2ull},
    {0x8618618618618618ull, 0x3ffa861861861861ull},
    {0x42c8590b21642c86ull, 0xbffa642c8590b216ull},
    {0xae147ae147ae147bull, 0x3ffa47ae147ae147ull},
};
// 1 / (2k+1), k = 1..9
constexpr QBits kAtanhCoef[] = {
    {0x5555555555555555ull, 0x3ffd555555555555ull},
    {0x999999999999999aull, 0x3ffc999999999999ull},
    {0x2492492492492492ull, 0x3ffc249249249249ull},
    {0xc71c71c71c71c71cull, 0x3ffbc71c71c71c71ull},
    {0x5d1745d1745d1746ull, 0x3ffb745d1745d174ull},
    {0x3b13b13b13b13b14ull, 0x3ffb3b13b13b13b1ull},
    {0x1111111111111111ull, 0x3ffb111111111111ull},
    {0xe1e1e1e1e1e1e1e2ull, 0x3ffae1e1e1e1e1e1ull},
    {0x86bca1af286bca1bull, 0x3ffaaf286bca1af2ull},
};
// ln(k/64), k = 45..91
constexpr int kLnFirst = 45;
constexpr QBits kLnTable[] = {
    {0x41a64eadd7401786ull, 0xbffd68ac83e9c6a1ull},
    {0x7ce102c98eb2d0d4ull, 0xbffd522ae0738a3dull},
    {0x3b54b606bd5c4e75ull, 0xbffd3c2527733318ull},
    {0x2783beb7676c0aaaull, 0xbffd269621134db9ull},
    {0xbde338b41fc72de8ull, 0xbffd1178e8227e47ull},
    {0x97d99419be602863ull, 0xbffcf991c6cb3b37ull},
    {0xb5818a49091c6888ull, 0xbffcd1037f2655e7ull},
    {0x36f2bea77a5c80ddull, 0xbffca93ed3c8ad9eull},
    {0x1bb734c63d061fa6ull, 0xbffc823c16551a3cull},
    {0x1fb8292ecfc82063ull, 0xbffc5bf406b543dbull},
    {0x62fa8234b7289595ull, 0xbffc365fcb015901ull},
    {0xbde338b41fc72de8ull, 0xbffc1178e8227e47ull},
    {0x25007e9c5ccc0630ull, 0xbffbda727638446aull},
    {0x8ae1d5ea3eccd251ull, 0xbffb9335e5d59498ull},
    {0xc5da7d0b1e10b2f6ull, 0xbffb4d3115d207eaull},
    {0x688a3fd9bf503373ull, 0xbffb08598b59e3a0ull},
    {0x33517d2ecc144799ull, 0xbffa894aa149fb34ull},
    {0x470173c75d4d888aull, 0xbffa0415d89e7444ull},
    {0x749f23a105b9c7bcull, 0xbff9020565893584ull},
    {0x0000000000000000ull, 0x0000000000000000ull},
    {0x3cf9eda74d37abd5ull, 0x3ff8fc0a8b0fc03eull},
    {0x04cf8fc13c7bc8a8ull, 0x3ff9f829b0e78330ull},
    {0xc4634f2a1ee4a583ull, 0x3ffa77458f632dcfull},
    {0x6617cc9716eeb32full, 0x3ffaf0a30c01162aull},
    {0x092998376104d137ull, 0x3ffb341d7961bd1dull},
    {0xb9be499b9ed19b64ull, 0x3ffb6f0d28ae56b4ull},
    {0x3650bd22a9c3aa4cull, 0x3ffba926d3a4ad56ull},
    {0x5e9ea87ffe1fe9e1ull, 0x3ffbe27076e2af2eull},
    {0x96697717a40c2ae9ull, 0x3ffc0d77e7cd08e5ull},
    {0x34c05dc7101f6fa7ull, 0x3ffc29552f81ff52ull},
    {0xe67d3d950f87e1e1ull, 0x3ffc44d2b6ccb7d1ull},
    {0x3c873e20a072123cull, 0x3ffc5ff3070a793dull},
    {0x91be36b2d6a06080ull, 0x3ffc7ab890210d90ull},
    {0x47641307538b8967ull, 0x3ffc9525a9cf456bull},
    {0x2d8ce601937ccf5dull, 0x3ffcaf3c94e80bffull},
    {0x1ac25d81ef2ffb9aull, 0x3ffcc8ff7c79a9a2ull},
    {0x5e9ea87ffe1fe9e1ull, 0x3ffce27076e2af2eull},
    {0xa8d55466c3794d2dull, 0x3ffcfb9186d5e3e2ull},
    {0x35f73f7a018700c4ull, 0x3ffd0a324e27390eull},
    {0xe039cc7d57106e2eull, 0x3ffd1675cababa60ull},
    {0x5a242853da75d433ull, 0x3ffd22941fbcf796ull},
    {0x09c2cc91a85081b4ull, 0x3ffd2e8e2bae11d3ull},
    {0x9c72f35cd740d08eull, 0x3ffd3a64c556945eull},
    {0x27d0b7b37b33c734ull, 0x3ffd4618bc21c5ecull},
    {0xd09c93d60cfaaf19ull, 0x3ffd51aad872df82ull},
    {0xa508d8e0f71ff845ull, 0x3ffd5d1bdbf5809cull},
    {0xec442be1014e3869ull, 0x3ffd686c81e9b14aull},
};
// 2^(j/32), j = 0..31
constexpr QBits kExp2Table[] = {
    {0x0000000000000000ull, 0x3fff000000000000ull},
    {0x43ae7c548eb68ca4ull, 0x3fff059b0d315857ull},
    {0xf6298b92b71842aaull, 0x3fff0b5586cf9890ull},
    {0x0a4ebbf1aed9318dull, 0x3fff11301d0125b5ull},
    {0xadcdf7c8c50eb14aull, 0x3fff172b83c7d517ull},
    {0xa7805b8028990f08ull, 0x3fff1d4873168b9aull},
    {0x866c1fadb1c15cb6ull, 0x3fff2387a6e75623ull},
    {0x12c25d15f5a24aa4ull, 0x3fff29e9df51fdeeull},
    {0x52de8d5a46305c86ull, 0x3fff306fe0a31b71ull},
    {0xaa7145502f454798ull, 0x3fff371a7373aa9cull},
    {0x235b41223e13d774ull, 0x3fff3dea64c12342ull},
    {0xd03136f409df01a0ull, 0x3fff44e086061892ull},
    {0x71d4397afec42e21ull, 0x3fff4bfdad5362a2ull},
    {0x1df0a83c49d86a64ull, 0x3fff5342b569d4f8ull},
    {0x958c93015191eb34ull, 0x3fff5ab07dd48542ull},
    {0x4b1f0fa06fd2da43ull, 0x3fff6247eb03a558ull},
    {0xc908b2fb1366ea95ull, 0x3fff6a09e667f3bcull},
    {0x3dd2370f2ef0acd7ull, 0x3fff71f75e8ec5f7ull},
    {0x6d7d51023f6cda1full, 0x3fff7a11473eb018ull},
    {0x28acf88afab34a01ull, 0x3fff82589994cce1ull},
    {0xb5ba7c55a192c9bbull, 0x3fff8ace5422aa0dull},
    {0x4f4501c3f2540a23ull, 0x3fff93737b0cdc5eull},
    {0x01c7c46b071f2be6ull, 0x3fff9c49182a3f09ull},
    {0xc8b424491caf87bdull, 0x3fffa5503b23e255ull},
    {0xd5e8734d1773205aull, 0x3fffae89f995ad3aull},
    {0x6eaa7b081ab53c53ull, 0x3fffb7f76f2fb5e4ull},
    {0xc2220cb12a091ba6ull, 0x3fffc199bdd85529ull},
    {0x91503cbd1e949db7ull, 0x3fffcb720dcef906ull},
    {0x725da05aeb66e0ddull, 0x3fffd5818dcfba48ull},
    {0xeb968cac39ed291bull, 0x3fffdfc97337b9b5ull},
    {0x9858f73a18f5db30ull, 0x3fffea4afa2a490dull},
    {0x0674f84b762862bbull, 0x3ffff50765b6e454ull},
};
// 1 / k!, k = 2..14
constexpr QBits kExpCoef[] = {
    {0x0000000000000000ull, 0x3ffe000000000000ull},
    {0x5555555555555555ull, 0x3ffc555555555555ull},
    {0x5555555555555555ull, 0x3ffa555555555555ull},
    {0x1111111111111111ull, 0x3ff8111111111111ull},
    {0x6c16c16c16c16c17ull, 0x3ff56c16c16c16c1ull},
    {0xa01a01a01a01a01aull, 0x3ff2a01a01a01a01ull},
    {0xa01a01a01a01a01aull, 0x3fefa01a01a01a01ull},
    {0x38faac1c88e50017ull, 0x3fec71de3a556c73ull},
    {0xc72ef016d3ea6679ull, 0x3fe927e4fb7789f5ull},
    {0x38fe747e4b837dc7ull, 0x3fe5ae64567f544eull},
    {0x7b544da987acfe85ull, 0x3fe21eed8eff8d89ull},
    {0x97ca38331d23af68ull, 0x3fde6124613a86d0ull},
    {0xd20badf145dfa3e5ull, 0x3fda93974a8c07c9ull},
};
constexpr QBits kLn2 = {0xf35793c7673007e6ull, 0x3ffe62e42fefa39eull};
constexpr QBits kLog2E = {0xe1777d0ffda0d23aull, 0x3fff71547652b82full};

// The x87's internal pi: 66 bits, 0xC90FDAA22168C234C... (the next bits
// of the true pi, 0x4C6..., are dropped). The argument reduction of
// fsin/fcos/fsincos/fptan and the pi-valued results of fpatan use it,
// which is where the well-known large-argument error of fsin comes from.
// As an integer, pi/2 = kPi66 * 2^-65.
constexpr uint64_t kPi66Hi = 0x3;
constexpr uint64_t kPi66Lo = 0x243F6A8885A308D3ull;
// pi66 * 2^k as binary128 (exact: 66 bits).
Q QPi66(int k) { return QFromU128(kPi66Hi, kPi66Lo, -64 + k); }

// Horner evaluation of sum c[i] * z^i for the given table.
template <size_t N>
Q Poly(const QBits (&c)[N], Q z) {
  Q p = Ld(c[N - 1]);
  for (size_t i = N - 1; i-- > 0;) p = QAdd(QMul(p, z), Ld(c[i]));
  return p;
}

// --- state helpers ---------------------------------------------------

int Phys(const State& s, int i) { return (s.top + i) & 7; }
bool EmptyPhys(const State& s, int p) { return (s.empty >> p) & 1; }
bool Empty(const State& s, int i) { return EmptyPhys(s, Phys(s, i)); }
void SetEmpty(State& s, int p, bool empty) {
  s.empty = static_cast<uint8_t>((s.empty & ~(1u << p)) | (unsigned(empty) << p));
}
F80 Get(const State& s, int i) { return s.r[Phys(s, i)]; }
void Put(State& s, int i, F80 v) {
  int p = Phys(s, i);
  s.r[p] = v;
  SetEmpty(s, p, false);
}
void SetC1(State& s, bool on) {
  s.sw = static_cast<uint16_t>(on ? (s.sw | kC1) : (s.sw & ~kC1));
}
void SetCC(State& s, uint16_t cc) {
  s.sw = static_cast<uint16_t>((s.sw & ~(kC0 | kC1 | kC2 | kC3)) | cc);
}
void SetC320(State& s, uint16_t cc) {
  s.sw = static_cast<uint16_t>((s.sw & ~(kC0 | kC2 | kC3)) | cc);
}

// Accumulates exception flags. ES/B are derived in StatusWord().
void Raise(State& s, uint16_t flags) {
  flags &= kExceptionMask | kSF;
  s.sw |= flags;
  if (flags & ~s.cw & kExceptionMask) s.unmasked_fault = true;
}

// Every waiting x87 instruction first delivers a pending unmasked
// exception (one unmasked after the fact by fldcw/fldenv/frstor).
void Wait(State& s) {
  if (s.sw & ~s.cw & kExceptionMask) s.unmasked_fault = true;
}

// Stack faults: masked response is IE + SF, C1 = 1 for overflow and 0 for
// underflow, and the real indefinite as the result.
void Underflow(State& s) {
  Raise(s, kIE | kSF);
  SetC1(s, false);
}

bool PushSlotFull(const State& s) {
  return !EmptyPhys(s, (s.top + 7) & 7);
}

void PushRaw(State& s, F80 v) {
  s.top = (s.top + 7) & 7;
  Put(s, 0, v);
}

// Checks the push destination; on overflow delivers the masked response
// (push of the indefinite) and returns false.
bool CheckPush(State& s) {
  if (!PushSlotFull(s)) return true;
  Raise(s, kIE | kSF);
  SetC1(s, true);
  PushRaw(s, kIndefinite);
  return false;
}

// Writes an instruction result with its flags; C1 reports round-up.
struct Result {
  F80 v;
  uint16_t flags;
  bool up;
};
X87_INLINE void Commit(State& s, int i, const Result& r) {
  Raise(s, r.flags);
  SetC1(s, r.up);
  Put(s, i, r.v);
}

// --- arithmetic core -------------------------------------------------

enum class MathOp { Add, Sub, Mul, Div };

extFloat80_t SfApply(MathOp op, extFloat80_t a, extFloat80_t b) {
  switch (op) {
    case MathOp::Add: return extF80_add(a, b);
    case MathOp::Sub: return extF80_sub(a, b);
    case MathOp::Mul: return extF80_mul(a, b);
    case MathOp::Div: return extF80_div(a, b);
  }
  return a;
}

bool SameBits(F80 a, F80 b) { return a.sig == b.sig && a.se == b.se; }

int PrecBits(uint32_t cw) {
  switch ((cw >> 8) & 3) {
    case 0: return 24;
    case 2: return 53;
    default: return 64;
  }
}

constexpr bool IsNormal(F80 v) {
  return ExpOf(v) - 1u < 0x7FFEu && (v.sig & kJ);
}
constexpr bool IsNormalOrZero(F80 v) { return IsNormal(v) || IsZero(v); }

// The PC=24 and PC=53 cores are run::Fast24 and run::FastWide (x87.h),
// shared with lifted runs.
X87_INLINE run::FOp CoreOp(MathOp op) {
  switch (op) {
    case MathOp::Add: return run::FOp::Add;
    case MathOp::Sub: return run::FOp::Sub;
    case MathOp::Mul: return run::FOp::Mul;
    case MathOp::Div: return run::FOp::Div;
  }
  return run::FOp::Add;
}
X87_INLINE bool Fast24(int rc, MathOp op, F80 a, F80 b, Result* r) {
  run::RV rv;
  if (!run::Fast24(CoreOp(op), rc, run::ToV(a), run::ToV(b), &rv)) return false;
  *r = {run::ToF80(run::Val(rv)), uint16_t(run::Flags(rv)), run::Up(rv)};
  return true;
}
template <int kPrec>
X87_INLINE bool FastWide(int rc, MathOp op, F80 a, F80 b, Result* r) {
  run::RV rv;
  if (!run::FastWide<kPrec>(CoreOp(op), rc, run::ToV(a), run::ToV(b), &rv)) return false;
  *r = {run::ToF80(run::Val(rv)), uint16_t(run::Flags(rv)), run::Up(rv)};
  return true;
}

// a op b for two normal finite operands, in integers: the exact result
// (128 significand bits, the last one sticky) rounded by RoundPack. This
// is the common case; SoftFloat (below) covers the rest and is the
// reference the fast paths are tested against (tests/x87_fast_test.cpp).
X87_INLINE Result FastArith(uint32_t cw, MathOp op, F80 a, F80 b) {
  if ((cw & 0x0300) == 0) {  // PC=24
    Result r;
    if (Fast24(RcOf(cw), op, a, b, &r) || FastWide<24>(RcOf(cw), op, a, b, &r)) return r;
  } else if ((cw & 0x0300) == 0x0200) {  // PC=53
    Result r;
    if (FastWide<53>(RcOf(cw), op, a, b, &r)) return r;
  }
  const int rc = RcOf(cw), prec = PrecBits(cw);
  int32_t ea = ExpOf(a), eb = ExpOf(b);
  bool sa = SignOf(a), sb = SignOf(b);
  bool sign;
  int32_t exp;
  uint64_t hi, lo;
  if (IsZero(a) || IsZero(b)) {
    switch (op) {
      case MathOp::Mul:
      case MathOp::Div:  // b is not zero (checked by the caller)
        return {Zero(sa != sb), 0, false};
      default: {
        bool sbx = op == MathOp::Sub ? !sb : sb;
        if (IsZero(a) && IsZero(b)) return {Zero(sa == sbx ? sa : rc == 1), 0, false};
        // x + 0 is x rounded to the precision control.
        F80 x = IsZero(a) ? b : a;
        Rounded rr = RoundPack(IsZero(a) ? sbx : sa, ExpOf(x), x.sig, 0, prec, rc);
        return {rr.v, rr.flags, rr.up};
      }
    }
  }
  switch (op) {
    case MathOp::Mul: {
      unsigned __int128 p = static_cast<unsigned __int128>(a.sig) * b.sig;
      hi = uint64_t(p >> 64);
      lo = uint64_t(p);
      sign = sa != sb;
      exp = ea + eb - 0x3FFF + 1;
      if (!(hi & kJ)) {  // product in [2^126, 2^127)
        hi = hi << 1 | lo >> 63;
        lo <<= 1;
        --exp;
      }
      break;
    }
    case MathOp::Div: {
      // q = sig_a / sig_b * 2^64 (or 2^63), 128 quotient bits and a sticky
      // remainder.
      sign = sa != sb;
      unsigned __int128 n;
      if (a.sig >= b.sig) {
        n = static_cast<unsigned __int128>(a.sig) << 63;
        exp = ea - eb + 0x3FFF;
      } else {
        n = static_cast<unsigned __int128>(a.sig) << 64;
        exp = ea - eb + 0x3FFF - 1;
      }
      hi = uint64_t(n / b.sig);
      unsigned __int128 rem = n % b.sig;
      unsigned __int128 n2 = rem << 64;
      lo = uint64_t(n2 / b.sig);
      if (n2 % b.sig) lo |= 1;
      break;
    }
    default: {
      bool sub = (op == MathOp::Sub) != (sa != sb);  // magnitudes subtract
      bool sbx = op == MathOp::Sub ? !sb : sb;      // sign of the addend
      // x = the larger magnitude.
      bool swap = eb > ea || (eb == ea && b.sig > a.sig);
      uint64_t xs = swap ? b.sig : a.sig, ys = swap ? a.sig : b.sig;
      int32_t ex = swap ? eb : ea, ey = swap ? ea : eb;
      sign = swap ? sbx : sa;
      uint64_t yh = ys, yl = 0;
      ShiftRightJam128(yh, yl, ex - ey);
      exp = ex;
      if (!sub) {
        lo = yl;
        hi = xs + yh;
        if (hi < xs) {  // carry out: shift right one, keep the sticky
          lo = lo >> 1 | (hi << 63) | (lo & 1);
          hi = hi >> 1 | kJ;
          ++exp;
        }
      } else {
        lo = 0 - yl;
        hi = xs - yh - (yl != 0);
        if (!hi && !lo) {  // exact zero: +0, or -0 rounding down
          return {Zero(rc == 1), 0, false};
        }
        // Cancellation by more than one bit only happens when the
        // exponents differ by at most one, so no sticky bit was jammed.
        exp -= Normalize128(&hi, &lo);
      }
      break;
    }
  }
  Rounded rr = RoundPack(sign, exp, hi, lo, prec, rc);
  return {rr.v, rr.flags, rr.up};
}

// a op b with x87 operand checks. `mem_denormal` marks a memory operand
// that was a denormal before conversion (it raises DE like a register
// one).
X87_INLINE Result Arith(uint32_t cw, MathOp op, F80 a, F80 b, bool mem_denormal) {
  if (!mem_denormal && IsNormalOrZero(a) && IsNormalOrZero(b) &&
      !(op == MathOp::Div && IsZero(b)) && !g_softfloat_only)
    return FastArith(cw, op, a, b);
  Result r{};
  if (IsUnsupported(a) || IsUnsupported(b)) return {kIndefinite, kIE, false};
  if (IsNaN(a) || IsNaN(b)) {
    r.v = PropagateNaN(a, b, &r.flags);
    return r;
  }
  bool sa = SignOf(a), sb = SignOf(b);
  // Invalid operations and divide-by-zero are detected before the
  // operation and suppress the denormal flag.
  switch (op) {
    case MathOp::Add:
      if (IsInf(a) && IsInf(b) && sa != sb) return {kIndefinite, kIE, false};
      break;
    case MathOp::Sub:
      if (IsInf(a) && IsInf(b) && sa == sb) return {kIndefinite, kIE, false};
      break;
    case MathOp::Mul:
      if ((IsInf(a) && IsZero(b)) || (IsZero(a) && IsInf(b)))
        return {kIndefinite, kIE, false};
      break;
    case MathOp::Div:
      if ((IsZero(a) && IsZero(b)) || (IsInf(a) && IsInf(b)))
        return {kIndefinite, kIE, false};
      if (IsZero(b) && !IsInf(a)) return {Inf(sa != sb), kZE, false};
      break;
  }
  if (IsDenormal(a) || IsDenormal(b) || mem_denormal) r.flags |= kDE;

  int rc = RcOf(cw);
  uint8_t prec = SfPrecision(cw);
  extFloat80_t x = ToSf(Canonical(a)), y = ToSf(Canonical(b));
  SfBegin(SfMode(rc), prec);
  r.v = FromSf(SfApply(op, x, y));
  r.flags |= FromSfFlags(softfloat_exceptionFlags);
  if (r.flags & kPE) {
    r.up = RoundedUp(rc, SignOf(r.v), [&] {
      SfBegin(softfloat_round_minMag, prec);
      return !SameBits(r.v, FromSf(SfApply(op, x, y)));
    });
  }
  return r;
}

// Maps the instruction form onto the operation; dst is the register that
// receives the result.
X87_INLINE Result DoOp(uint32_t cw, Op op, F80 dst, F80 src, bool src_denormal) {
  switch (op) {
    case Op::Add: return Arith(cw, MathOp::Add, dst, src, src_denormal);
    case Op::Mul: return Arith(cw, MathOp::Mul, dst, src, src_denormal);
    case Op::Sub: return Arith(cw, MathOp::Sub, dst, src, src_denormal);
    case Op::Div: return Arith(cw, MathOp::Div, dst, src, src_denormal);
    case Op::SubR: return Arith(cw, MathOp::Sub, src, dst, src_denormal);
    case Op::DivR: return Arith(cw, MathOp::Div, src, dst, src_denormal);
  }
  return {};
}

void ArithSt0Mem(State& s, Op op, F80 src, bool src_denormal) {
  Wait(s);
  if (Empty(s, 0)) {
    Underflow(s);
    Put(s, 0, kIndefinite);
    return;
  }
  Commit(s, 0, DoOp(s.cw, op, Get(s, 0), src, src_denormal));
}

// --- compares --------------------------------------------------------

// -1, 0, 1 for ordered finite/infinite values (denormals included).
int CompareValues(F80 a, F80 b) {
  bool za = IsZero(a), zb = IsZero(b);
  if (za && zb) return 0;
  bool sa = SignOf(a), sb = SignOf(b);
  if (za) return sb ? 1 : -1;
  if (zb) return sa ? -1 : 1;
  if (sa != sb) return sa ? -1 : 1;
  F80 ca = Canonical(a), cb = Canonical(b);
  int ea = ExpOf(ca), eb = ExpOf(cb);
  // Denormals compare like exponent-1 values with J clear.
  if (ea == 0) ea = 1;
  if (eb == 0) eb = 1;
  int mag = 0;
  if (ea != eb) {
    mag = ea < eb ? -1 : 1;
  } else if (ca.sig != cb.sig) {
    mag = ca.sig < cb.sig ? -1 : 1;
  }
  return sa ? -mag : mag;
}

// C3/C2/C0 of a compare of a with b (FCOM semantics unless quiet).
uint16_t CompareCC(F80 a, F80 b, bool quiet, bool b_denormal,
                   uint16_t* flags) {
  if (IsUnsupported(a) || IsUnsupported(b)) {
    *flags |= kIE;
    return kUnordered;
  }
  if (IsNaN(a) || IsNaN(b)) {
    if (!quiet || IsSNaN(a) || IsSNaN(b)) *flags |= kIE;
    return kUnordered;
  }
  if (IsDenormal(a) || IsDenormal(b) || b_denormal) *flags |= kDE;
  int c = CompareValues(a, b);
  return c < 0 ? kC0 : c == 0 ? kC3 : 0;
}

void CompareWith(State& s, F80 b, bool quiet, bool b_denormal,
                 bool b_empty) {
  Wait(s);
  if (Empty(s, 0) || b_empty) {
    Underflow(s);
    SetC320(s, kUnordered);
    return;
  }
  uint16_t flags = 0;
  uint16_t cc = CompareCC(Get(s, 0), b, quiet, b_denormal, &flags);
  Raise(s, flags);
  SetCC(s, cc);  // C1 cleared
}

// The result of a store to memory: bits, flags, C1.
struct Stored {
  uint64_t bits;
  uint16_t flags;
  bool up;
};

// --- integer stores --------------------------------------------------

// FIST: `lo` is also the integer indefinite.
Stored StoreIntValue(uint32_t cw, F80 v, int64_t lo, int64_t hi) {
  const uint64_t indefinite = uint64_t(lo);
  if (IsUnsupported(v) || ExpOf(v) == 0x7FFF) return {indefinite, kIE, false};
  int rc = RcOf(cw);
  extFloat80_t x = ToSf(Canonical(v));
  SfBegin(SfMode(rc), 80);
  int64_t r = extF80_to_i64(x, SfMode(rc), true);
  uint16_t flags = FromSfFlags(softfloat_exceptionFlags);
  if ((flags & kIE) || r < lo || r > hi) return {indefinite, kIE, false};
  bool up = false;
  if (flags & kPE)
    up = RoundedUp(rc, SignOf(v), [&] { return r != extF80_to_i64_r_minMag(x, false); });
  return {uint64_t(r), flags, up};
}

int64_t StoreInt(State& s, int64_t lo, int64_t hi) {
  Wait(s);
  SetC1(s, false);
  if (Empty(s, 0)) {
    Underflow(s);
    return lo;
  }
  Stored r = StoreIntValue(s.cw, Get(s, 0), lo, hi);
  SetC1(s, r.up);
  Raise(s, r.flags);
  return int64_t(r.bits);
}

// --- transcendental cores ----------------------------------------------

// Argument reduction of fsin/fcos/fsincos/fptan exactly as the hardware
// does it: |x| = n * P + r with P = pi66/2, the remainder exact, then
// folded to |r| <= P/2. x = sig / 2^63 * 2^exp with |x| < 2^63.
struct Reduced {
  Q r;           // |r|
  bool r_neg;    // r < 0 after the fold
  unsigned n;    // quadrant, mod 4
};
Reduced ReduceTrig(uint64_t sig, int exp) {
  Reduced out{};
  if (exp < -1) {  // |x| < 1/2 < pi/4: no reduction
    out.r = QFromSig(sig, exp);
    return out;
  }
  // X = |x| * 2^65 = sig * 2^(exp + 2) is an integer; divide it by
  // P = pi/2 * 2^65 bit by bit. The remainder stays below 2^67.
  uint64_t rh = 0, rl = 0, q = 0;
  const int bits = 64 + exp + 2;
  for (int k = 0; k < bits; ++k) {
    uint64_t bit = k < 64 ? (sig >> (63 - k)) & 1 : 0;
    rh = (rh << 1) | (rl >> 63);
    rl = (rl << 1) | bit;
    q <<= 1;
    if (rh > kPi66Hi || (rh == kPi66Hi && rl >= kPi66Lo)) {
      rh -= kPi66Hi + (rl < kPi66Lo);
      rl -= kPi66Lo;
      q |= 1;
    }
  }
  // Fold: r > P/2 (P is odd, so 2r != P) becomes r - P.
  uint64_t dh = (rh << 1) | (rl >> 63), dl = rl << 1;
  if (dh > kPi66Hi || (dh == kPi66Hi && dl > kPi66Lo)) {
    uint64_t nh = kPi66Hi - rh - (kPi66Lo < rl);
    rl = kPi66Lo - rl;
    rh = nh;
    out.r_neg = true;
    ++q;
  }
  out.r = QFromU128(rh, rl, -65);
  out.n = static_cast<unsigned>(q & 3);
  return out;
}

// sin(r) = r + SinTail(r), cos(r) = 1 + CosTail(r) for 0 <= r <= pi/4.
Q SinTail(Q r, Q z) { return QMul(QMul(Poly(kSinCoef, z), z), r); }
Q CosTail(Q z) { return QMul(Poly(kCosCoef, z), z); }

enum class Trig { Sin, Cos };

// --- fixed-point fast path for sin/cos ----------------------------------
//
// The same series as SinTail/CosTail, evaluated in unsigned 128-bit fixed
// point (value = v / 2^127) instead of binary128. Every magnitude stays in
// [0, 1], so Horner's scheme with alternating signs never goes negative:
// t = a14; t = a(k) - z t; ...; sin r = r (1 - z t), cos r = 1 - z t.
// Its error is below 2^-118, the binary128 path's below 2^-108; when the
// result lies further than 2^-100 from every rounding boundary of the
// 64-bit significand both must round the same way, so the fast result is
// used. Otherwise (or for tiny r) the binary128 path decides.

using U128 = unsigned __int128;

// (a * b) >> 127 for a, b <= 2^127.
U128 MulFix(U128 a, U128 b) {
  uint64_t a0 = uint64_t(a), a1 = uint64_t(a >> 64);
  uint64_t b0 = uint64_t(b), b1 = uint64_t(b >> 64);
  U128 p00 = U128(a0) * b0, p01 = U128(a0) * b1, p10 = U128(a1) * b0,
       p11 = U128(a1) * b1;
  U128 mid = (p00 >> 64) + uint64_t(p01) + uint64_t(p10);
  U128 hi = p11 + (p01 >> 64) + (p10 >> 64) + (mid >> 64);
  return (hi << 1) | (uint64_t(mid) >> 63);
}

// |q| as fixed point (truncated), for |q| < 2.
U128 FixFromQ(Q q) {
  int e = int((q.v[1] >> 48) & 0x7FFF) - 0x3FFF;
  if (e < -127) return 0;
  U128 m = (U128(q.v[1] & 0xFFFFFFFFFFFFull) | (U128(1) << 48)) << 64 | q.v[0];
  int sh = e + 15;  // m has its leading one at bit 112
  return sh >= 0 ? m << sh : sh > -128 ? m >> -sh : 0;
}

struct FixCoefs {
  U128 sin[14], cos[14];
};
const FixCoefs& Coefs() {
  static const FixCoefs c = [] {
    FixCoefs f{};
    for (int k = 0; k < 14; ++k) {
      f.sin[k] = FixFromQ(Ld(kSinCoef[k]));
      f.cos[k] = FixFromQ(Ld(kCosCoef[k]));
    }
    return f;
  }();
  return c;
}

bool FastTrig(const Reduced& red, bool use_cos, bool sign, int rc, Rounded* out) {
  if (g_softfloat_only) return false;
  U128 r = FixFromQ(red.r);
  if (r < (U128(1) << 100)) return false;  // r < 2^-27: let binary128 decide
  U128 z = MulFix(r, r);
  const U128* a = use_cos ? Coefs().cos : Coefs().sin;
  U128 t = a[13];
  for (int k = 12; k >= 0; --k) t = a[k] - MulFix(z, t);
  U128 v = use_cos ? (U128(1) << 127) - MulFix(z, t) : r - MulFix(r, MulFix(z, t));
  // Normalise: bit 127 set; exp so that value = (hi + lo/2^64)/2^63 * 2^(exp-0x3FFF).
  int lz = Clz128(uint64_t(v >> 64), uint64_t(v));
  v <<= lz;
  uint64_t hi = uint64_t(v >> 64), lo = uint64_t(v);
  // 2^-100 in units of lo (2^(-127-lz)); boundaries are lo = 0 and 2^63.
  int mshift = 27 + lz;
  if (mshift >= 61) return false;
  uint64_t margin = 1ull << mshift;
  uint64_t d_half = lo > kHalf ? lo - kHalf : kHalf - lo;
  uint64_t d_edge = lo < kHalf ? lo : 0 - lo;
  if (d_half <= margin || d_edge <= margin) return false;
  *out = RoundPack(sign, 0x3FFF - lz, hi, lo, 64, rc);
  return true;
}

// One of +-sin(|x|) / +-cos(|x|) after reduction, rounded with the guest
// RC; `want` selects sin or cos of the original |x|.
Rounded TrigResult(const Reduced& red, Trig want, bool negate, int rc) {
  // sin(|x|) by quadrant: sin r, cos r, -sin r, -cos r (r signed);
  // cos(|x|): cos r, -sin r, -cos r, sin r.
  unsigned n = (red.n + (want == Trig::Cos ? 1 : 0)) & 3;
  bool use_cos = n & 1;
  bool neg = (n & 2) != 0;
  {
    Rounded fast;
    if (FastTrig(red, use_cos, neg ^ (use_cos ? false : red.r_neg) ^ negate, rc, &fast))
      return fast;
  }
  Q z = QMul(red.r, red.r);
  Q v;
  if (use_cos) {
    Q tail = CosTail(z);
    v = Odd([&] { return QAdd(QOne(), tail); });
  } else {
    Q tail = SinTail(red.r, z);
    v = Odd([&] { return QAdd(red.r, tail); });
    neg ^= red.r_neg;
  }
  if (neg ^ negate) v = QNeg(v);
  return RoundQ(v, 0, rc);
}

// atan(t) for 0 <= t <= 1 as head + tail (head from a table of atan(k/16)
// or 0), so callers can fold in pi terms before the final rounding.
void AtanParts(Q t, Q* head, Q* tail) {
  int k = static_cast<int>(f128_to_i32(QScale(t, 4), softfloat_round_near_even,
                                       false));
  if (QIsZero(t)) k = 0;
  Q u = t;
  if (k > 0) {
    Q c = QScale(QInt(k), -4);
    u = QDiv(QSub(t, c), QAdd(QOne(), QMul(t, c)));  // t - c is exact
  }
  Q u2 = QMul(u, u);
  *head = Ld(kAtanTable[k]);
  *tail = QMul(QMul(Poly(kAtanCoef, u2), u2), u);  // atan(u) = u + tail
  if (k > 0) {
    *tail = QAdd(u, *tail);
  } else {
    *head = u;  // caller adds head + tail; head carries u exactly
  }
}

// log2(m * 2^e) for an exact binary128 m in [sqrt(1/2), sqrt(2)).
Q Log2Core(Q m, int e) {
  int k = static_cast<int>(f128_to_i32(QScale(m, 6), softfloat_round_near_even,
                                       false));
  Q c = QScale(QInt(k), -6);
  Q u = QDiv(QSub(m, c), QAdd(m, c));  // m - c is exact
  Q u2 = QMul(u, u);
  // atanh(u) = u + u^3 (1/3 + u^2/5 + ...); ln(m) = ln(c) + 2 atanh(u)
  Q at = QAdd(u, QMul(QMul(Poly(kAtanhCoef, u2), u2), u));
  Q ln_m = QAdd(Ld(kLnTable[k - kLnFirst]), QScale(at, 1));
  return QAdd(QInt(e), QMul(ln_m, Ld(kLog2E)));
}

// Splits a finite positive binary128 into m in [sqrt(1/2), sqrt(2)) and e.
void SplitLog(Q w, Q* m, int* e) {
  int ew = static_cast<int>((w.v[1] >> 48) & 0x7FFF) - 0x3FFF;
  *m = QScale(w, -ew);
  // sqrt(2) = 1.6a09e667f3bcc908... as binary128 bits of the fraction.
  if (((*m).v[1] & 0xFFFFFFFFFFFFull) > 0x6a09e667f3bcull) {
    *m = QScale(*m, -1);
    ++ew;
  }
  *e = ew;
}

// Rounds the pi66-based constant results of fpatan.
Rounded PiConst(bool sign, int which, int rc) {
  // which: 0 = pi/4, 1 = pi/2, 2 = pi, 3 = 3pi/4
  if (which == 3)
    return RoundPack(sign, 0x4000, 0x96CBE3F9990E91A7ull,
                     0x9000000000000000ull, 64, rc);
  static constexpr int kExp[3] = {0x3FFE, 0x3FFF, 0x4000};
  return RoundPack(sign, kExp[which], 0xC90FDAA22168C234ull,
                   0xC000000000000000ull, 64, rc);
}

// The two-operand transcendentals (fpatan, fyl2x, fyl2xp1) read st0 and
// st1, write st1 and pop.
bool TwoOperandUnderflow(State& s) {
  if (!Empty(s, 0) && !Empty(s, 1)) return false;
  Underflow(s);
  Put(s, 1, kIndefinite);
  Pop(s);
  return true;
}

Result FromRounded(const Rounded& r, uint16_t extra) {
  return {r.v, static_cast<uint16_t>(r.flags | extra), r.up};
}

// Transcendental results: the hardware always reports them inexact, and
// so also reports underflow for any tiny result, exact or not.
Result Transcendental(const Rounded& r, uint16_t extra) {
  uint16_t f = static_cast<uint16_t>(r.flags | extra | kPE);
  if (r.tiny) f |= kUE;
  return {r.v, f, r.up};
}

// Rounds a value just below (dir < 0) or just above (dir > 0) the
// magnitude sig / 2^63 * 2^(exp - 0x3FFF): the result of sin, tan or cos
// for tiny arguments, where the true value differs from x (or 1) by far
// less than an ulp but still decides directed rounding.
Rounded Nudge(bool sign, int32_t exp, uint64_t sig, int dir, int rc) {
  if (dir > 0) return RoundPack(sign, exp, sig, 1, 64, rc);
  if (sig == kJ) return RoundPack(sign, exp - 1, ~0ull, ~0ull, 64, rc);
  return RoundPack(sign, exp, sig - 1, ~0ull, 64, rc);
}

// Forces PE without C1: the hardware reports these results as inexact
// even when they happen to be exact.
Result Inexact(F80 v, uint16_t extra) {
  return {v, static_cast<uint16_t>(kPE | extra), false};
}

}  // namespace

// --- state -----------------------------------------------------------

void Init(State& s) {
  s.cw = 0x037F;
  s.sw = 0;
  s.empty = 0xFF;
  s.top = 0;
  s.fip = s.fdp = 0;
  s.fop = 0;
}

uint16_t StatusWord(const State& s) { return run::StatusWordOf(s.sw, s.top, s.cw); }

uint16_t ControlWord(const State& s) { return static_cast<uint16_t>(s.cw); }

void SetControlWord(State& s, uint16_t cw) {
  Wait(s);
  // Bits 13-15 read back as 0 and bit 6 as 1; bit 12 (IC) is kept.
  s.cw = static_cast<uint16_t>((cw & 0x1F3F) | 0x0040);
}

void ClearExceptions(State& s) {
  s.sw &= static_cast<uint16_t>(~(kExceptionMask | kSF | kES | kBusy));
}

uint16_t TagWord(const State& s) {
  uint32_t tw = 0;
  for (int p = 0; p < 8; ++p)
    tw |= (EmptyPhys(s, p) ? 3u : run::Tag(run::ToV(s.r[p]))) << (2 * p);
  return static_cast<uint16_t>(tw);
}

// --- loads -----------------------------------------------------------

namespace {
// A value converted from m32/m64 as FLD loads it: an SNaN raises IE and
// becomes quiet, a denormal source raises DE.
F80 Converted(F80 v, bool denormal, uint16_t* flags) {
  if (IsSNaN(v)) {
    *flags = kIE;
    return Quiet(v);
  }
  *flags = denormal ? kDE : 0;
  return v;
}

void LoadConverted(State& s, F80 v, bool denormal) {
  Wait(s);
  SetC1(s, false);
  if (!CheckPush(s)) return;
  uint16_t flags;
  v = Converted(v, denormal, &flags);
  Raise(s, flags);
  PushRaw(s, v);
}

void LoadExact(State& s, F80 v) {
  Wait(s);
  SetC1(s, false);
  if (!CheckPush(s)) return;
  PushRaw(s, v);
}
}  // namespace

void LoadF32(State& s, uint32_t bits) {
  bool den;
  F80 v = FromF32(bits, &den);
  LoadConverted(s, v, den);
}

void LoadF64(State& s, uint64_t bits) {
  bool den;
  F80 v = FromF64(bits, &den);
  LoadConverted(s, v, den);
}

void LoadF80(State& s, F80 v) { LoadExact(s, v); }

void LoadSt(State& s, int i) {
  Wait(s);
  SetC1(s, false);
  if (!CheckPush(s)) {
    // An empty source is also reported (C1 = 0) when the stack overflows.
    if (Empty(s, i + 1)) SetC1(s, false);
    return;
  }
  if (Empty(s, i)) {
    Underflow(s);
    PushRaw(s, kIndefinite);
    return;
  }
  PushRaw(s, Get(s, i));
}

void LoadI16(State& s, int16_t v) { LoadExact(s, FromInt(v)); }
void LoadI32(State& s, int32_t v) { LoadExact(s, FromInt(v)); }
void LoadI64(State& s, int64_t v) { LoadExact(s, FromInt(v)); }

namespace {
F80 ConstOf(uint32_t cw, Const k) {
  // The hardware rounds its internal constants with RC as if from the
  // exact value (no PE, no C1). Truncated significand, exponent, and
  // whether round-to-nearest rounds up.
  struct C {
    uint64_t sig;
    uint16_t exp;
    bool rn_up;
  };
  static constexpr C kL2T = {0xd49a784bcd1b8afeull, 0x4000, false};
  static constexpr C kL2E = {0xb8aa3b295c17f0bbull, 0x3fff, true};
  static constexpr C kPi = {0xc90fdaa22168c234ull, 0x4000, true};
  static constexpr C kLG2 = {0x9a209a84fbcff798ull, 0x3ffd, true};
  static constexpr C kLN2 = {0xb17217f7d1cf79abull, 0x3ffe, true};
  F80 v;
  const C* c = nullptr;
  switch (k) {
    case Const::One: v = kOne; break;
    case Const::Zero: v = Zero(false); break;
    case Const::L2T: c = &kL2T; break;
    case Const::L2E: c = &kL2E; break;
    case Const::Pi: c = &kPi; break;
    case Const::LG2: c = &kLG2; break;
    case Const::LN2: c = &kLN2; break;
  }
  if (c) {
    int rc = RcOf(cw);
    bool up = rc == 2 || (rc == 0 && c->rn_up);  // all positive, inexact
    v = Make(false, c->exp, c->sig + (up ? 1 : 0));
  }
  return v;
}
}  // namespace

void LoadConst(State& s, Const k) { LoadExact(s, ConstOf(s.cw, k)); }

// --- stores ----------------------------------------------------------

// FST m32/m64 in integers: run::StoreCore (x87.h), shared with lifted
// runs. False sends the value through SoftFloat (overflow, underflow,
// specials).
X87_INLINE bool FastStore(uint32_t cw, F80 v, int mant, int bias, int emax, Stored* out) {
  if (g_softfloat_only) return false;
  run::SV sv;
  if (!run::StoreCore(cw, run::ToV(v), mant, bias, emax, &sv)) return false;
  *out = {sv.bits, uint16_t(run::Flags(sv)), run::Up(sv)};
  return true;
}

// FST m32 (single = true) / m64 of a register value.
Stored StoreFloat(uint32_t cw, F80 v, bool single) {
  if (IsUnsupported(v))
    return {single ? 0xFFC00000ull : 0xFFF8000000000000ull, kIE, false};
  Stored out;
  if (single ? FastStore(cw, v, 23, 127, 254, &out)
             : FastStore(cw, v, 52, 1023, 2046, &out))
    return out;
  int rc = RcOf(cw);
  extFloat80_t x = ToSf(Canonical(v));
  SfBegin(SfMode(rc), 80);
  out.bits = single ? extF80_to_f32(x).v : extF80_to_f64(x).v;
  out.flags = FromSfFlags(softfloat_exceptionFlags);
  out.up = false;
  if (out.flags & kPE) {
    out.up = RoundedUp(rc, SignOf(v), [&] {
      SfBegin(softfloat_round_minMag, 80);
      return (single ? extF80_to_f32(x).v : extF80_to_f64(x).v) != out.bits;
    });
  }
  return out;
}

uint32_t StoreF32(State& s) {
  Wait(s);
  SetC1(s, false);
  if (Empty(s, 0)) {
    Underflow(s);
    return 0xFFC00000u;
  }
  Stored r = StoreFloat(s.cw, Get(s, 0), true);
  SetC1(s, r.up);
  Raise(s, r.flags);
  return uint32_t(r.bits);
}

uint64_t StoreF64(State& s) {
  Wait(s);
  SetC1(s, false);
  if (Empty(s, 0)) {
    Underflow(s);
    return 0xFFF8000000000000ull;
  }
  Stored r = StoreFloat(s.cw, Get(s, 0), false);
  SetC1(s, r.up);
  Raise(s, r.flags);
  return r.bits;
}
F80 StoreF80(State& s) {
  Wait(s);
  SetC1(s, false);
  if (Empty(s, 0)) {
    Underflow(s);
    return kIndefinite;
  }
  return Get(s, 0);
}

void StoreSt(State& s, int i) {
  Wait(s);
  SetC1(s, false);
  if (Empty(s, 0)) {
    Underflow(s);
    Put(s, i, kIndefinite);
    return;
  }
  Put(s, i, Get(s, 0));
}

int16_t StoreI16(State& s) {
  return static_cast<int16_t>(StoreInt(s, INT16_MIN, INT16_MAX));
}
int32_t StoreI32(State& s) {
  return static_cast<int32_t>(StoreInt(s, INT32_MIN, INT32_MAX));
}
int64_t StoreI64(State& s) { return StoreInt(s, INT64_MIN, INT64_MAX); }

void Pop(State& s) {
  SetEmpty(s, Phys(s, 0), true);
  s.top = (s.top + 1) & 7;
}

// --- arithmetic ------------------------------------------------------

void ArithF32(State& s, Op op, uint32_t bits) {
  bool den;
  F80 v = FromF32(bits, &den);
  ArithSt0Mem(s, op, v, den);
}

void ArithF64(State& s, Op op, uint64_t bits) {
  bool den;
  F80 v = FromF64(bits, &den);
  ArithSt0Mem(s, op, v, den);
}

void ArithI16(State& s, Op op, int16_t v) {
  ArithSt0Mem(s, op, FromInt(v), false);
}

void ArithI32(State& s, Op op, int32_t v) {
  ArithSt0Mem(s, op, FromInt(v), false);
}

void ArithSt0Sti(State& s, Op op, int i) {
  Wait(s);
  if (Empty(s, 0) || Empty(s, i)) {
    Underflow(s);
    Put(s, 0, kIndefinite);
    return;
  }
  Commit(s, 0, DoOp(s.cw, op, Get(s, 0), Get(s, i), false));
}

void ArithStiSt0(State& s, Op op, int i) {
  Wait(s);
  if (Empty(s, 0) || Empty(s, i)) {
    Underflow(s);
    Put(s, i, kIndefinite);
    return;
  }
  Commit(s, i, DoOp(s.cw, op, Get(s, i), Get(s, 0), false));
}

// --- compares --------------------------------------------------------

void CompareSt(State& s, int i, bool quiet) {
  bool empty = Empty(s, i);
  CompareWith(s, Get(s, i), quiet, false, empty);
}

void CompareF32(State& s, uint32_t bits) {
  bool den;
  F80 v = FromF32(bits, &den);
  CompareWith(s, v, false, den, false);
}

void CompareF64(State& s, uint64_t bits) {
  bool den;
  F80 v = FromF64(bits, &den);
  CompareWith(s, v, false, den, false);
}

void CompareI16(State& s, int16_t v) {
  CompareWith(s, FromInt(v), false, false, false);
}

void CompareI32(State& s, int32_t v) {
  CompareWith(s, FromInt(v), false, false, false);
}

EFlagsResult CompareStEFlags(State& s, int i, bool quiet) {
  Wait(s);
  SetC1(s, false);
  uint16_t cc;
  if (Empty(s, 0) || Empty(s, i)) {
    Underflow(s);
    cc = kUnordered;
  } else {
    uint16_t flags = 0;
    cc = CompareCC(Get(s, 0), Get(s, i), quiet, false, &flags);
    Raise(s, flags);
  }
  return {(cc & kC3) != 0, (cc & kC2) != 0, (cc & kC0) != 0};
}

void Test(State& s) { CompareWith(s, Zero(false), false, false, false); }

void Examine(State& s) {
  Wait(s);
  F80 v = Get(s, 0);
  uint16_t cc;
  if (Empty(s, 0)) {
    cc = kC3 | kC0;
  } else if (IsUnsupported(v)) {
    cc = 0;
  } else if (IsNaN(v)) {
    cc = kC0;
  } else if (IsInf(v)) {
    cc = kC2 | kC0;
  } else if (IsZero(v)) {
    cc = kC3;
  } else if (IsDenormal(v)) {
    cc = kC3 | kC2;
  } else {
    cc = kC2;
  }
  // C1 is the sign bit of st0, whatever its tag.
  SetCC(s, cc | (SignOf(v) ? kC1 : 0));
}

// --- unary / misc ----------------------------------------------------

void Chs(State& s) {
  Wait(s);
  SetC1(s, false);
  if (Empty(s, 0)) {
    Underflow(s);
    Put(s, 0, kIndefinite);
    return;
  }
  F80 v = Get(s, 0);
  v.se ^= 0x8000;
  Put(s, 0, v);
}

void Abs(State& s) {
  Wait(s);
  SetC1(s, false);
  if (Empty(s, 0)) {
    Underflow(s);
    Put(s, 0, kIndefinite);
    return;
  }
  F80 v = Get(s, 0);
  v.se &= 0x7FFF;
  Put(s, 0, v);
}

namespace {
Result SqrtOf(uint32_t cw, F80 v) {
  Result r{};
  if (IsUnsupported(v)) {
    r = {kIndefinite, kIE, false};
  } else if (IsNaN(v)) {
    r.v = PropagateNaN1(v, &r.flags);
  } else if (IsZero(v)) {
    r.v = v;
  } else if (SignOf(v)) {
    r = {kIndefinite, kIE, false};
  } else if (IsInf(v)) {
    r.v = v;
  } else {
    if (IsDenormal(v)) r.flags |= kDE;
    int rc = RcOf(cw);
    uint8_t prec = SfPrecision(cw);
    extFloat80_t x = ToSf(Canonical(v));
    SfBegin(SfMode(rc), prec);
    r.v = FromSf(extF80_sqrt(x));
    r.flags |= FromSfFlags(softfloat_exceptionFlags);
    if (r.flags & kPE) {
      r.up = RoundedUp(rc, false, [&] {
        SfBegin(softfloat_round_minMag, prec);
        return !SameBits(r.v, FromSf(extF80_sqrt(x)));
      });
    }
  }
  return r;
}
}  // namespace

void Sqrt(State& s) {
  Wait(s);
  SetC1(s, false);
  if (Empty(s, 0)) {
    Underflow(s);
    Put(s, 0, kIndefinite);
    return;
  }
  Commit(s, 0, SqrtOf(s.cw, Get(s, 0)));
}

void RndInt(State& s) {
  Wait(s);
  SetC1(s, false);
  if (Empty(s, 0)) {
    Underflow(s);
    Put(s, 0, kIndefinite);
    return;
  }
  F80 v = Get(s, 0);
  Result r{};
  if (IsUnsupported(v)) {
    r = {kIndefinite, kIE, false};
  } else if (IsNaN(v)) {
    r.v = PropagateNaN1(v, &r.flags);
  } else if (IsZero(v) || IsInf(v)) {
    r.v = v;
  } else {
    if (IsDenormal(v)) r.flags |= kDE;
    int rc = RcOf(s);
    SfBegin(SfMode(rc), 80);
    r.v = FromSf(extF80_roundToInt(ToSf(Canonical(v)), SfMode(rc), true));
    r.flags |= FromSfFlags(softfloat_exceptionFlags);
    // An integer result of larger magnitude than an inexact operand was
    // rounded up.
    if (r.flags & kPE) {
      F80 a = Make(false, ExpOf(r.v), r.v.sig);
      F80 b = Make(false, ExpOf(v), v.sig);
      r.up = CompareValues(a, b) > 0;
    }
  }
  Commit(s, 0, r);
}

void Scale(State& s) {
  Wait(s);
  SetC1(s, false);
  if (Empty(s, 0) || Empty(s, 1)) {
    Underflow(s);
    Put(s, 0, kIndefinite);
    return;
  }
  F80 a = Get(s, 0), b = Get(s, 1);
  Result r{};
  bool sa = SignOf(a), sb = SignOf(b);
  if (IsUnsupported(a) || IsUnsupported(b)) {
    r = {kIndefinite, kIE, false};
  } else if (IsNaN(a) || IsNaN(b)) {
    r.v = PropagateNaN(a, b, &r.flags);
  } else if (IsInf(a)) {
    if (IsInf(b) && sb) {
      r = {kIndefinite, kIE, false};
    } else {
      if (IsDenormal(b)) r.flags |= kDE;
      r.v = a;
    }
  } else if (IsInf(b)) {
    if (IsZero(a)) {
      r = sb ? Result{a, 0, false} : Result{kIndefinite, kIE, false};
    } else {
      if (IsDenormal(a)) r.flags |= kDE;
      r.v = sb ? Zero(sa) : Inf(sa);
    }
  } else if (IsZero(a)) {
    if (IsDenormal(b)) r.flags |= kDE;
    r.v = a;
  } else {
    if (IsDenormal(a) || IsDenormal(b)) r.flags |= kDE;
    Norm na = Normalize(a);
    int32_t scale = 0;
    if (!IsZero(b)) {
      Norm nb = Normalize(b);
      if (nb.exp >= 16) {
        // Far outside any representable result: certain overflow or
        // underflow (a large step keeps it so after adding a's exponent).
        scale = sb ? -0x10000 : 0x10000;
      } else if (nb.exp >= 0) {
        scale = static_cast<int32_t>(nb.sig >> (63 - nb.exp));  // trunc
        if (sb) scale = -scale;
      }
    }
    Rounded rr = RoundPack(sa, na.exp + 0x3FFF + scale, na.sig, 0, 64,
                           RcOf(s));
    r = FromRounded(rr, r.flags);
  }
  Commit(s, 0, r);
}

void Xtract(State& s) {
  Wait(s);
  SetC1(s, false);
  if (Empty(s, 0) || PushSlotFull(s)) {
    if (Empty(s, 0)) {
      Underflow(s);
    } else {
      Raise(s, kIE | kSF);
      SetC1(s, true);
    }
    Put(s, 0, kIndefinite);
    PushRaw(s, kIndefinite);
    return;
  }
  F80 a = Get(s, 0);
  uint16_t flags = 0;
  F80 exponent, significand;
  if (IsUnsupported(a)) {
    flags = kIE;
    exponent = significand = kIndefinite;
  } else if (IsNaN(a)) {
    exponent = significand = PropagateNaN1(a, &flags);
  } else if (IsInf(a)) {
    exponent = Inf(false);
    significand = a;
  } else if (IsZero(a)) {
    flags = kZE;
    exponent = Inf(true);
    significand = a;
  } else {
    if (IsDenormal(a)) flags |= kDE;
    Norm n = Normalize(a);
    exponent = FromInt(n.exp);
    significand = Make(SignOf(a), 0x3FFF, n.sig);
  }
  Raise(s, flags);
  Put(s, 0, exponent);
  PushRaw(s, significand);
}

namespace {

// FPREM (ieee = false: quotient truncated) and FPREM1 (ieee = true:
// quotient rounded to nearest even). When the exponents differ by 64 or
// more only a partial remainder is produced (C2 = 1): the dividend is
// reduced by n = 32..63 bits, n = (D mod 32) + 32 like the hardware, so
// software loops until C2 = 0.
void PremImpl(State& s, bool ieee) {
  Wait(s);
  s.sw &= static_cast<uint16_t>(~(kC1 | kC2));
  if (Empty(s, 0) || Empty(s, 1)) {
    Underflow(s);
    Put(s, 0, kIndefinite);
    return;
  }
  F80 a = Get(s, 0), b = Get(s, 1);
  uint16_t flags = 0;
  F80 result;
  int status = 0;  // -1: exception (C0/C3 kept), 0: complete, 1: partial
  uint64_t q = 0;
  if (IsUnsupported(a) || IsUnsupported(b)) {
    flags = kIE;
    result = kIndefinite;
    status = -1;
  } else if (IsNaN(a) || IsNaN(b)) {
    result = PropagateNaN(a, b, &flags);
    status = -1;
  } else if (IsInf(a) || IsZero(b)) {
    flags = kIE;
    result = kIndefinite;
    status = -1;
  } else if (IsInf(b)) {
    if (IsDenormal(a)) flags |= kDE;
    result = Canonical(a);
  } else {
    if (IsDenormal(b)) flags |= kDE;
    if (IsZero(a)) {
      result = a;
    } else {
      if (IsDenormal(a)) flags |= kDE;
      Norm na = Normalize(a), nb = Normalize(b);
      bool sign = SignOf(a);
      int d = na.exp - nb.exp;
      uint64_t rem = 0;
      int rem_exp = 0;  // remainder value = rem / 2^63 * 2^rem_exp
      bool keep_a = false;
      if (d >= 64) {
        int n = (d & 31) | 32;
        rem = na.sig;
        if (rem >= nb.sig) rem -= nb.sig;
        for (int k = 0; k < n; ++k) {
          bool carry = rem >> 63;
          rem <<= 1;
          if (carry || rem >= nb.sig) rem -= nb.sig;
        }
        rem_exp = na.exp - n;
        status = 1;
      } else if (d < -1 || (d == -1 && !ieee)) {
        keep_a = true;
      } else if (d == -1) {
        // |a| in [|b|/4, |b|): the nearest quotient is 0 or 1.
        if (na.sig > nb.sig) {
          rem = nb.sig - (na.sig - nb.sig);  // (2b - a) in a's units
          rem_exp = na.exp;
          sign = !sign;
          q = 1;
        } else {
          keep_a = true;
        }
      } else {
        rem = na.sig;
        if (rem >= nb.sig) {
          rem -= nb.sig;
          q = 1;
        }
        for (int k = 0; k < d; ++k) {
          bool carry = rem >> 63;
          rem <<= 1;
          q <<= 1;
          if (carry || rem >= nb.sig) {
            rem -= nb.sig;
            q |= 1;
          }
        }
        if (ieee && (rem > nb.sig - rem ||
                     (rem == nb.sig - rem && (q & 1)))) {
          rem = nb.sig - rem;
          sign = !sign;
          ++q;
        }
        rem_exp = nb.exp;
      }
      if (keep_a) {
        result = Canonical(a);
      } else if (rem == 0) {
        result = Zero(SignOf(a));
      } else {
        int lz = std::countl_zero(rem);
        Rounded rr = RoundPack(sign, rem_exp + 0x3FFF - lz, rem << lz, 0, 64,
                               RcOf(s));
        result = rr.v;  // exact
      }
    }
  }
  Raise(s, flags);
  if (status == 1) {
    SetCC(s, kC2);
  } else if (status == 0) {
    SetCC(s, ((q & 4) ? kC0 : 0) | ((q & 2) ? kC3 : 0) | ((q & 1) ? kC1 : 0));
  }
  Put(s, 0, result);
}

}  // namespace

void Prem(State& s) { PremImpl(s, false); }
void Prem1(State& s) { PremImpl(s, true); }

namespace {

enum class TrigOp { Sin, Cos, SinCos, Tan };

void TrigOpImpl(State& s, TrigOp op) {
  Wait(s);
  s.sw &= static_cast<uint16_t>(~(kC1 | kC2));
  bool pushes = op == TrigOp::SinCos || op == TrigOp::Tan;
  if (Empty(s, 0) || (pushes && PushSlotFull(s))) {
    if (Empty(s, 0)) {
      Underflow(s);
    } else {
      Raise(s, kIE | kSF);
      SetC1(s, true);
    }
    Put(s, 0, kIndefinite);
    if (pushes) PushRaw(s, kIndefinite);
    return;
  }
  F80 x = Get(s, 0);
  int rc = RcOf(s);
  // Results: `first` replaces st0, `second` is pushed (fsincos: cos,
  // fptan: 1.0).
  auto finish = [&](F80 first, F80 second, uint16_t flags, bool up) {
    Raise(s, flags);
    SetC1(s, up);
    Put(s, 0, first);
    if (pushes) PushRaw(s, second);
  };
  if (IsUnsupported(x) || IsInf(x)) {
    finish(kIndefinite, kIndefinite, kIE, false);
    return;
  }
  if (IsNaN(x)) {
    uint16_t flags = 0;
    F80 q = PropagateNaN1(x, &flags);
    finish(q, q, flags, false);
    return;
  }
  bool sign = SignOf(x);
  if (IsZero(x)) {
    switch (op) {
      case TrigOp::Sin: finish(x, x, 0, false); break;
      case TrigOp::Cos: finish(kOne, kOne, 0, false); break;
      case TrigOp::SinCos: finish(x, kOne, 0, false); break;
      case TrigOp::Tan: finish(x, kOne, 0, false); break;
    }
    return;
  }
  uint16_t den = IsDenormal(x) ? kDE : 0;
  Norm n = Normalize(x);
  if (n.exp >= 63) {
    s.sw |= kC2;  // out of range: operand unchanged, nothing pushed
    return;
  }
  if (n.exp < -68) {
    // sin(x) = x - x^3/6, tan(x) = x + x^3/3, cos(x) = 1 - x^2/2: the
    // correction only decides the rounding direction.
    Rounded sn = Nudge(sign, n.exp + 0x3FFF, n.sig, -1, rc);
    Rounded tn = Nudge(sign, n.exp + 0x3FFF, n.sig, +1, rc);
    Rounded cs = Nudge(false, 0x3FFF, kJ, -1, rc);
    Result rs = Transcendental(sn, den), rt = Transcendental(tn, den);
    Result rcos = Transcendental(cs, den);
    switch (op) {
      case TrigOp::Sin: finish(rs.v, rs.v, rs.flags, rs.up); break;
      case TrigOp::Cos: finish(rcos.v, rcos.v, rcos.flags, rcos.up); break;
      case TrigOp::SinCos:  // C1 reports the sine (see below)
        finish(rs.v, rcos.v, rs.flags | rcos.flags, rs.up);
        break;
      case TrigOp::Tan: finish(rt.v, kOne, rt.flags, rt.up); break;
    }
    return;
  }
  SfBegin(softfloat_round_near_even, 80);
  Reduced red = ReduceTrig(n.sig, n.exp);
  switch (op) {
    case TrigOp::Sin: {
      Result r = Transcendental(TrigResult(red, Trig::Sin, sign, rc), den);
      finish(r.v, r.v, r.flags, r.up);
      break;
    }
    case TrigOp::Cos: {
      Result r = Transcendental(TrigResult(red, Trig::Cos, false, rc), den);
      finish(r.v, r.v, r.flags, r.up);
      break;
    }
    case TrigOp::SinCos: {
      Result rs = Transcendental(TrigResult(red, Trig::Sin, sign, rc), den);
      Result rcos =
          Transcendental(TrigResult(red, Trig::Cos, false, rc), den);
      // C1 reports the rounding of the sine (st1), as observed on a
      // Zen CPU; matching the cosine's instead disagrees in ~20% of cases.
      finish(rs.v, rcos.v, rs.flags | rcos.flags, rs.up);
      break;
    }
    case TrigOp::Tan: {
      Q z = QMul(red.r, red.r);
      Q sn = QAdd(red.r, SinTail(red.r, z));
      Q cs = QAdd(QOne(), CosTail(z));
      if (red.r_neg) sn = QNeg(sn);
      // tan(|x|) = sin r / cos r for even quadrants, -cos r / sin r odd.
      Q t = (red.n & 1) ? Odd([&] { return QDiv(QNeg(cs), sn); })
                        : Odd([&] { return QDiv(sn, cs); });
      if (sign) t = QNeg(t);
      Result r = Transcendental(RoundQ(t, 0, rc), den);
      finish(r.v, kOne, r.flags, r.up);
      break;
    }
  }
}

}  // namespace

void Sin(State& s) { TrigOpImpl(s, TrigOp::Sin); }
void Cos(State& s) { TrigOpImpl(s, TrigOp::Cos); }
void SinCos(State& s) { TrigOpImpl(s, TrigOp::SinCos); }
void Ptan(State& s) { TrigOpImpl(s, TrigOp::Tan); }

void Patan(State& s) {
  Wait(s);
  SetC1(s, false);
  if (TwoOperandUnderflow(s)) return;
  F80 x = Get(s, 0), y = Get(s, 1);
  int rc = RcOf(s);
  bool sx = SignOf(x), sy = SignOf(y);
  Result r{};
  if (IsUnsupported(x) || IsUnsupported(y)) {
    r = {kIndefinite, kIE, false};
  } else if (IsNaN(x) || IsNaN(y)) {
    r.v = PropagateNaN(x, y, &r.flags);
  } else if (IsInf(y)) {
    if (IsInf(x)) {
      r = FromRounded(PiConst(sy, sx ? 3 : 0, rc), 0);
    } else {
      r = FromRounded(PiConst(sy, 1, rc), IsDenormal(x) ? kDE : 0);
    }
  } else if (IsInf(x) || IsZero(y)) {
    // atan2(y, +-inf) and atan2(+-0, x): +-pi for negative x (-0
    // included), otherwise +-0, exactly.
    uint16_t den = IsDenormal(IsInf(x) ? y : x) ? kDE : 0;
    if (sx) {
      r = FromRounded(PiConst(sy, 2, rc), den);
    } else {
      r = {Zero(sy), den, false};
    }
  } else if (IsZero(x)) {
    r = FromRounded(PiConst(sy, 1, rc), IsDenormal(y) ? kDE : 0);
  } else {
    uint16_t den = (IsDenormal(x) || IsDenormal(y)) ? kDE : 0;
    Norm nx = Normalize(x), ny = Normalize(y);
    // t = smaller / larger magnitude, in (0, 1].
    bool swap = ny.exp > nx.exp || (ny.exp == nx.exp && ny.sig > nx.sig);
    Norm u = swap ? ny : nx, v = swap ? nx : ny;
    int dexp = v.exp - u.exp;
    SfBegin(softfloat_round_near_even, 80);
    Q result;
    int extra = 0;
    if (dexp < -16000) {
      // atan(t) = t far beyond 64 bits. Without a pi term the result is
      // t itself (scaled outside the binary128 range); with one, t only
      // decides the rounding direction, so any tiny stand-in will do.
      Q tq = QDiv(QFromSig(v.sig, 0), QFromSig(u.sig, 0));
      if (!swap && !sx) {
        // atan(t) is just below t.
        result = Odd([&] { return QSub(tq, QScale(tq, -200)); });
        if (sy) result = QNeg(result);
        extra = dexp;
      } else {
        Q tiny = QScale(QOne(), -16000);
        Q h = swap ? QScale(QPi66(0), -1) : QPi66(0);
        // swap: pi/2 - t (x > 0) or pi/2 + t (x < 0); else (x < 0) pi - t.
        bool add = swap && sx;
        result = Odd([&] { return add ? QAdd(h, tiny) : QSub(h, tiny); });
        if (sy) result = QNeg(result);
      }
    } else {
      Q t = QDiv(QFromSig(v.sig, dexp), QFromSig(u.sig, 0));
      Q head, tail;
      AtanParts(t, &head, &tail);  // atan(t) = head + tail
      Q pi2 = QScale(QPi66(0), -1), pi = QPi66(0);
      if (!swap && !sx) {
        result = Odd([&] { return QAdd(head, tail); });
      } else if (swap && !sx) {  // pi/2 - atan(t)
        Q h = QSub(pi2, head);
        result = Odd([&] { return QSub(h, tail); });
      } else if (!swap) {  // pi - atan(t)
        Q h = QSub(pi, head);
        result = Odd([&] { return QSub(h, tail); });
      } else {  // pi/2 + atan(t)
        Q h = QAdd(pi2, head);
        result = Odd([&] { return QAdd(h, tail); });
      }
      if (sy) result = QNeg(result);
    }
    r = Transcendental(RoundQ(result, extra, rc), den);
  }
  Commit(s, 1, r);
  Pop(s);
}

void F2xm1(State& s) {
  Wait(s);
  SetC1(s, false);
  if (Empty(s, 0)) {
    Underflow(s);
    Put(s, 0, kIndefinite);
    return;
  }
  F80 x = Get(s, 0);
  int rc = RcOf(s);
  Result r{};
  if (IsUnsupported(x)) {
    r = {kIndefinite, kIE, false};
  } else if (IsNaN(x)) {
    r.v = PropagateNaN1(x, &r.flags);
  } else if (IsInf(x)) {
    r.v = SignOf(x) ? Make(true, 0x3FFF, kJ) : x;  // -1 or +inf
  } else if (IsZero(x)) {
    r.v = x;
  } else {
    uint16_t den = IsDenormal(x) ? kDE : 0;
    Norm n = Normalize(x);
    if (n.exp >= 0) {
      // Outside the documented domain |x| <= 1 the hardware returns the
      // operand unchanged; +-1 come out exact (1 and -0.5). Always PE.
      if (n.exp == 0 && n.sig == kJ) {
        r = Inexact(SignOf(x) ? Make(true, 0x3FFE, kJ) : kOne, den);
      } else {
        r = Inexact(x, den);
      }
    } else {
      SfBegin(softfloat_round_near_even, 80);
      Q xq = QFrom(x);
      // 2^x = 2^(j/32) * 2^f with |f| <= 1/64; 2^f - 1 = expm1(f ln 2).
      int j = static_cast<int>(f128_to_i32(QScale(xq, 5),
                                           softfloat_round_near_even, false));
      Q f = QSub(xq, QScale(QInt(j), -5));  // exact
      Q g = QMul(f, Ld(kLn2));
      Q tail = QMul(QMul(g, g), Poly(kExpCoef, g));  // expm1(g) = g + tail
      Q result;
      if (j == 0) {
        result = Odd([&] { return QAdd(g, tail); });
      } else {
        int jj = j & 31, qexp = (j - jj) / 32;  // j = 32 qexp + jj
        Q t = QScale(Ld(kExp2Table[jj]), qexp);
        Q em1 = QAdd(g, tail);
        Q h = QSub(t, QOne());  // exact
        result = Odd([&] { return QAdd(h, QMul(t, em1)); });
      }
      r = Transcendental(RoundQ(result, 0, rc), den);
    }
  }
  Commit(s, 0, r);
}

void Yl2x(State& s) {
  Wait(s);
  SetC1(s, false);
  if (TwoOperandUnderflow(s)) return;
  F80 x = Get(s, 0), y = Get(s, 1);
  int rc = RcOf(s);
  bool sx = SignOf(x), sy = SignOf(y);
  Result r{};
  // Special operands, after Intel's FYL2X table: log2 of a negative
  // number, 0 * inf and inf * 0 are invalid; log2(0) is a zero-divide.
  auto x_below_one = [&] {  // 0 < x < 1
    return ExpOf(x) < 0x3FFF;
  };
  auto x_is_one = [&] { return ExpOf(x) == 0x3FFF && x.sig == kJ; };
  if (IsUnsupported(x) || IsUnsupported(y)) {
    r = {kIndefinite, kIE, false};
  } else if (IsNaN(x) || IsNaN(y)) {
    r.v = PropagateNaN(x, y, &r.flags);
  } else if (IsInf(x)) {
    if (sx) {
      r = {kIndefinite, kIE, false};
    } else if (IsZero(y)) {
      r = {kIndefinite, kIE, false};
    } else {
      r = {Inf(sy), static_cast<uint16_t>(IsDenormal(y) ? kDE : 0), false};
    }
  } else if (IsInf(y)) {
    if (sx && !IsZero(x)) {
      r = {kIndefinite, kIE, false};
    } else if (x_is_one()) {
      r = {kIndefinite, kIE, false};
    } else {
      uint16_t den = IsDenormal(x) ? kDE : 0;
      r = {Inf(x_below_one() ? !sy : sy), den, false};
    }
  } else if (IsZero(x)) {
    if (IsZero(y)) {
      r = {kIndefinite, kIE, false};
    } else {
      r = {Inf(!sy), kZE, false};
    }
  } else if (sx) {
    r = {kIndefinite, kIE, false};
  } else if (IsZero(y)) {
    uint16_t den = IsDenormal(x) ? kDE : 0;
    r = {Zero(x_below_one() ? !sy : sy), den, false};
  } else if (x_is_one()) {
    r = {Zero(sy), static_cast<uint16_t>(IsDenormal(y) ? kDE : 0), false};
  } else {
    uint16_t den = (IsDenormal(x) || IsDenormal(y)) ? kDE : 0;
    Norm nx = Normalize(x);
    SfBegin(softfloat_round_near_even, 80);
    Q m;
    int e;
    SplitLog(QFromSig(nx.sig, 0), &m, &e);
    Q z = Log2Core(m, e + nx.exp);
    // y's exponent is applied in the final rounding, so neither binary128
    // overflow nor its denormal range gets in the way.
    Norm ny = Normalize(y);
    Q ym = QFromSig(ny.sig, 0);
    if (sy) ym = QNeg(ym);
    Q result = Odd([&] { return QMul(ym, z); });
    r = Transcendental(RoundQ(result, ny.exp, rc), den);
  }
  Commit(s, 1, r);
  Pop(s);
}

void Yl2xp1(State& s) {
  Wait(s);
  SetC1(s, false);
  if (TwoOperandUnderflow(s)) return;
  F80 x = Get(s, 0), y = Get(s, 1);
  int rc = RcOf(s);
  bool sx = SignOf(x), sy = SignOf(y);
  Result r{};
  if (IsUnsupported(x) || IsUnsupported(y)) {
    r = {kIndefinite, kIE, false};
  } else if (IsNaN(x) || IsNaN(y)) {
    r.v = PropagateNaN(x, y, &r.flags);
  } else if (IsInf(x)) {
    if (sx || IsZero(y)) {
      r = {kIndefinite, kIE, false};
    } else {
      r = {Inf(sy), static_cast<uint16_t>(IsDenormal(y) ? kDE : 0), false};
    }
  } else if (IsInf(y)) {
    if (IsZero(x)) {
      r = {kIndefinite, kIE, false};
    } else {
      r = {Inf(sx != sy), static_cast<uint16_t>(IsDenormal(x) ? kDE : 0),
           false};
    }
  } else if (IsZero(x)) {
    r = {Zero(sx != sy), static_cast<uint16_t>(IsDenormal(y) ? kDE : 0),
         false};
  } else if (IsZero(y)) {
    r = {Zero(sx != sy), static_cast<uint16_t>(IsDenormal(x) ? kDE : 0),
         false};
  } else {
    uint16_t den = (IsDenormal(x) || IsDenormal(y)) ? kDE : 0;
    Norm nx = Normalize(x), ny = Normalize(y);
    if (sx && nx.exp >= 0) {
      // x <= -1 is outside the domain; the hardware returns x (with PE).
      r = Inexact(x, den);
    } else {
      SfBegin(softfloat_round_near_even, 80);
      Q result;
      int extra = 0;
      if (nx.exp < -70) {
        // log2(1+x) = x/ln2 * (1 - x/2 + ...): multiply the significands
        // and carry the exponents separately, since x/ln2 may lie below the
        // binary128 range while y * log2(1+x) does not.
        Q xm = QFromSig(nx.sig, 0), ym = QFromSig(ny.sig, 0);
        if (sx) xm = QNeg(xm);
        if (sy) ym = QNeg(ym);
        Q xq = QFrom(x);
        Q f = QSub(Ld(kLog2E), QMul(Ld(kLog2E), QScale(xq, -1)));
        Q p = QMul(xm, ym);
        result = Odd([&] { return QMul(p, f); });
        extra = nx.exp + ny.exp;
      } else {
        Q xq = QFrom(x), z;
        if (nx.exp < -6) {
          // log(1+x) = 2 atanh(x / (2 + x))
          Q u = QDiv(xq, QAdd(QScale(QOne(), 1), xq));
          Q u2 = QMul(u, u);
          Q at = QAdd(u, QMul(QMul(Poly(kAtanhCoef, u2), u2), u));
          z = QMul(QScale(at, 1), Ld(kLog2E));
        } else {
          Q m;
          int e;
          // 1 + x is exact for 2^-6 <= |x| < 1 (at most 71 bits).
          SplitLog(QAdd(QOne(), xq), &m, &e);
          z = Log2Core(m, e);
        }
        Q ym = QFromSig(ny.sig, 0);
        if (sy) ym = QNeg(ym);
        result = Odd([&] { return QMul(ym, z); });
        extra = ny.exp;
      }
      r = Transcendental(RoundQ(result, extra, rc), den);
    }
  }
  Commit(s, 1, r);
  Pop(s);
}

void Xch(State& s, int i) {
  Wait(s);
  SetC1(s, false);
  F80 a = Get(s, 0), b = Get(s, i);
  if (Empty(s, 0) || Empty(s, i)) {
    Underflow(s);
    if (Empty(s, 0)) a = kIndefinite;
    if (Empty(s, i)) b = kIndefinite;
  }
  Put(s, i, a);
  Put(s, 0, b);
}

void CopySt(State& s, int i) {
  Wait(s);
  if (Empty(s, 0) || Empty(s, i)) {
    Underflow(s);
    Put(s, 0, kIndefinite);
    return;
  }
  Put(s, 0, Get(s, i));
}

void Free(State& s, int i) {
  Wait(s);
  SetC1(s, false);
  SetEmpty(s, Phys(s, i), true);
}

void IncStp(State& s) {
  Wait(s);
  SetC1(s, false);
  s.top = (s.top + 1) & 7;
}

void DecStp(State& s) {
  Wait(s);
  SetC1(s, false);
  s.top = (s.top + 7) & 7;
}

// --- environment -----------------------------------------------------

namespace {

void Put16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
void Put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
uint16_t Get16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}
uint32_t Get32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// 32-bit protected-mode layout. The unused upper halves of the control,
// status and tag dwords read as 0xFFFF on hardware. CS/DS selectors are
// stored as 0, as on CPUs that deprecate them.
void WriteEnv(const State& s, uint8_t* out) {
  Put32(out + 0, 0xFFFF0000u | ControlWord(s));
  Put32(out + 4, 0xFFFF0000u | StatusWord(s));
  Put32(out + 8, 0xFFFF0000u | TagWord(s));
  Put32(out + 12, s.fip);
  Put32(out + 16, static_cast<uint32_t>(s.fop & 0x7FF) << 16);
  Put32(out + 20, s.fdp);
  Put32(out + 24, 0xFFFF0000u);
}

void ReadEnv(State& s, const uint8_t* in) {
  s.cw = static_cast<uint16_t>((Get16(in + 0) & 0x1F3F) | 0x0040);
  uint16_t sw = Get16(in + 4);
  s.top = (sw >> 11) & 7;
  s.sw = static_cast<uint16_t>(sw & ~(kTopMask | kES | kBusy));
  uint16_t tw = Get16(in + 8);
  s.fip = Get32(in + 12);
  s.fop = static_cast<uint16_t>((Get32(in + 16) >> 16) & 0x7FF);
  s.fdp = Get32(in + 20);
  // Only "empty" survives a load; the processor recomputes the class of
  // every other register from its contents (State::empty).
  s.empty = 0;
  for (int p = 0; p < 8; ++p) SetEmpty(s, p, ((tw >> (2 * p)) & 3) == 3);
}

}  // namespace

void StoreEnv(State& s, uint8_t out[28]) {
  WriteEnv(s, out);
  s.cw |= kExceptionMask;
}

void LoadEnv(State& s, const uint8_t in[28]) {
  Wait(s);
  ReadEnv(s, in);
}

void Save(State& s, uint8_t out[108]) {
  WriteEnv(s, out);
  for (int i = 0; i < 8; ++i) {
    F80 v = Get(s, i);
    std::memcpy(out + 28 + 10 * i, &v.sig, 8);
    Put16(out + 28 + 10 * i + 8, v.se);
  }
  Init(s);
}

void Restore(State& s, const uint8_t in[108]) {
  Wait(s);
  uint16_t sw = Get16(in + 4);
  int top = (sw >> 11) & 7;
  for (int i = 0; i < 8; ++i) {
    F80 v;
    std::memcpy(&v.sig, in + 28 + 10 * i, 8);
    v.se = Get16(in + 28 + 10 * i + 8);
    s.r[(top + i) & 7] = v;
  }
  ReadEnv(s, in);
}

// --- value-level operations for lifted runs ---------------------------

namespace run {

bool g_enabled = true;
bool g_doubles = true;

namespace {
RV Pack(const Result& r) {
  return {r.v.sig, uint64_t(r.v.se) | uint64_t(r.flags) << 16 | uint64_t(r.up) << 32};
}
SV Pack(const Stored& r) { return {r.bits, uint64_t(r.flags) | uint64_t(r.up) << 16}; }
RV Loaded(F80 v, bool denormal) {
  uint16_t flags;
  v = Converted(v, denormal, &flags);
  return {v.sig, uint64_t(v.se) | uint64_t(flags) << 16};
}
uint32_t Compared(F80 a, F80 b, bool quiet, bool b_denormal) {
  uint16_t flags = 0;
  uint16_t cc = CompareCC(a, b, quiet, b_denormal, &flags);
  return cc | uint32_t(flags & (kExceptionMask | kSF)) << 16;
}
}  // namespace

// The inline *Fast forms in x87.h have already tried the fast cores.
RV Arith(uint32_t cw, Op op, V dv, V sv) {
  return Pack(DoOp(cw, op, ToF80(dv), ToF80(sv), false));
}
RV ArithF32(uint32_t cw, Op op, V sv, uint32_t bits) {
  bool den;
  F80 v = FromF32(bits, &den);
  return Pack(DoOp(cw, op, ToF80(sv), v, den));
}
RV ArithF64(uint32_t cw, Op op, V sv, uint64_t bits) {
  bool den;
  F80 v = FromF64(bits, &den);
  return Pack(DoOp(cw, op, ToF80(sv), v, den));
}
RV ArithI16(uint32_t cw, Op op, V sv, int16_t v) {
  return Pack(DoOp(cw, op, ToF80(sv), ::recomp::x87::FromInt(v), false));
}
RV ArithI32(uint32_t cw, Op op, V sv, int32_t v) {
  return Pack(DoOp(cw, op, ToF80(sv), ::recomp::x87::FromInt(v), false));
}
RV Sqrt(uint32_t cw, V v) { return Pack(SqrtOf(cw, ToF80(v))); }

RV LoadF32(uint32_t bits) {
  bool den;
  F80 v = ::recomp::x87::FromF32(bits, &den);
  return Loaded(v, den);
}
RV LoadF64(uint64_t bits) {
  bool den;
  F80 v = ::recomp::x87::FromF64(bits, &den);
  return Loaded(v, den);
}
V FromInt(int64_t v) { return ToV(::recomp::x87::FromInt(v)); }
V ConstValue(uint32_t cw, Const k) { return ToV(ConstOf(cw, k)); }

SV StoreF32(uint32_t cw, V v) { return Pack(StoreFloat(cw, ToF80(v), true)); }
SV StoreF64(uint32_t cw, V v) { return Pack(StoreFloat(cw, ToF80(v), false)); }
SV StoreI16(uint32_t cw, V v) { return Pack(StoreIntValue(cw, ToF80(v), INT16_MIN, INT16_MAX)); }
SV StoreI32(uint32_t cw, V v) { return Pack(StoreIntValue(cw, ToF80(v), INT32_MIN, INT32_MAX)); }
SV StoreI64(uint32_t cw, V v) { return Pack(StoreIntValue(cw, ToF80(v), INT64_MIN, INT64_MAX)); }

uint32_t Compare(V a, V b, bool quiet) { return Compared(ToF80(a), ToF80(b), quiet, false); }
uint32_t CompareF32(V av, uint32_t bits) {
  F80 a = ToF80(av);
  bool den;
  F80 v = ::recomp::x87::FromF32(bits, &den);
  return Compared(a, v, false, den);
}
uint32_t CompareF64(V av, uint64_t bits) {
  F80 a = ToF80(av);
  bool den;
  F80 v = ::recomp::x87::FromF64(bits, &den);
  return Compared(a, v, false, den);
}

}  // namespace run

}  // namespace recomp::x87
