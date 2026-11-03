// SSE/SSE2 floating point and MXCSR (see sse.h).
//
// Lanes are raw bit patterns. NaN operands are handled here with the SSE
// rules (SDM vol. 1, "SSE and SSE2 response to NaN operands"): a single
// NaN operand is returned quieted; with two, the first source (the
// destination register) wins; an SNaN operand raises IE. Everything else
// goes to SoftFloat, whose 8086 specialization already matches x86 for
// non-NaN inputs: default NaN 0xFFC00000 / 0xFFF8000000000000, tininess
// after rounding, 0x80000000 for out-of-range conversions.

#include "runtime/sse.h"

extern "C" {
#include "softfloat.h"
}

#include "runtime/log.h"

namespace recomp::sse {
namespace {

// MXCSR fields.
constexpr uint32_t kIE = 0x01, kDE = 0x02, kZE = 0x04, kOE = 0x08, kUE = 0x10,
                   kPE = 0x20, kDAZ = 0x40, kFTZ = 0x8000;

// A lane format: float or double.
template <bool Dbl>
struct Fmt;
template <>
struct Fmt<false> {
  using U = uint32_t;
  static constexpr U kExp = 0x7F800000u, kFrac = 0x007FFFFFu, kQuiet = 0x00400000u;
};
template <>
struct Fmt<true> {
  using U = uint64_t;
  static constexpr U kExp = 0x7FF0000000000000ull, kFrac = 0x000FFFFFFFFFFFFFull,
                     kQuiet = 0x0008000000000000ull;
};

template <bool D>
bool IsNaN(typename Fmt<D>::U v) {
  return (v & Fmt<D>::kExp) == Fmt<D>::kExp && (v & Fmt<D>::kFrac);
}
template <bool D>
bool IsSNaN(typename Fmt<D>::U v) {
  return IsNaN<D>(v) && !(v & Fmt<D>::kQuiet);
}
template <bool D>
bool IsDenormal(typename Fmt<D>::U v) {
  return !(v & Fmt<D>::kExp) && (v & Fmt<D>::kFrac);
}
template <bool D>
typename Fmt<D>::U Quiet(typename Fmt<D>::U v) {
  return v | Fmt<D>::kQuiet;
}

uint8_t RoundingMode(uint32_t mxcsr) {
  static const uint8_t kModes[4] = {softfloat_round_near_even, softfloat_round_min,
                                    softfloat_round_max, softfloat_round_minMag};
  return kModes[(mxcsr >> 13) & 3];
}

uint32_t FromSf(uint_fast8_t f) {
  uint32_t r = 0;
  if (f & softfloat_flag_invalid) r |= kIE;
  if (f & softfloat_flag_infinite) r |= kZE;
  if (f & softfloat_flag_overflow) r |= kOE;
  if (f & softfloat_flag_underflow) r |= kUE;
  if (f & softfloat_flag_inexact) r |= kPE;
  return r;
}

// One instruction's MXCSR bookkeeping: rounding mode in, flags out.
struct Scope {
  Ctx& c;
  uint64_t va;
  uint32_t flags = 0;
  bool tiny = false;  // a result was tiny (matters only under FTZ)

  Scope(Ctx& cc, uint64_t v) : c(cc), va(v) {
    softfloat_roundingMode = RoundingMode(c.mxcsr);
  }
  // Denormal operand: DE, unless DAZ would have changed the operand.
  template <bool D>
  void Operand(typename Fmt<D>::U v) {
    if (!IsDenormal<D>(v)) return;
    if (c.mxcsr & kDAZ)
      FatalF("sse: DAZ with a denormal operand at %08llX (not modelled)",
             (unsigned long long)va);
    flags |= kDE;
  }
  void Begin() { softfloat_exceptionFlags = 0; }
  void End() {
    uint_fast8_t f = softfloat_exceptionFlags;
    if (f & softfloat_flag_underflow) tiny = true;
    flags |= FromSf(f);
  }
  // Commits the flags; an unmasked one would have raised #XM instead.
  void Finish() {
    if (tiny && (c.mxcsr & kFTZ))
      FatalF("sse: FTZ with a tiny result at %08llX (not modelled)",
             (unsigned long long)va);
    uint32_t unmasked = flags & ~(c.mxcsr >> 7) & 0x3F;
    if (unmasked)
      FatalF("sse: unmasked SIMD exception %02X at %08llX (#XM not modelled)",
             unmasked, (unsigned long long)va);
    c.mxcsr |= flags;
  }
};

template <bool D>
typename Fmt<D>::U ArithLane(Scope& sc, Op op, typename Fmt<D>::U a,
                             typename Fmt<D>::U b) {
  if (IsNaN<D>(a) || IsNaN<D>(b)) {
    if (IsSNaN<D>(a) || IsSNaN<D>(b)) sc.flags |= kIE;
    return IsNaN<D>(a) ? Quiet<D>(a) : Quiet<D>(b);
  }
  // Division by zero takes precedence over a denormal dividend (no DE).
  using U = typename Fmt<D>::U;
  constexpr U kSign = U(1) << (sizeof(U) * 8 - 1);
  if (!(op == kDiv && !(b & ~kSign))) sc.Operand<D>(a);
  sc.Operand<D>(b);
  sc.Begin();
  U r;
  if constexpr (D) {
    float64_t x{a}, y{b};
    switch (op) {
      case kAdd: r = f64_add(x, y).v; break;
      case kSub: r = f64_sub(x, y).v; break;
      case kMul: r = f64_mul(x, y).v; break;
      default: r = f64_div(x, y).v; break;
    }
  } else {
    float32_t x{a}, y{b};
    switch (op) {
      case kAdd: r = f32_add(x, y).v; break;
      case kSub: r = f32_sub(x, y).v; break;
      case kMul: r = f32_mul(x, y).v; break;
      default: r = f32_div(x, y).v; break;
    }
  }
  sc.End();
  return r;
}

template <bool D>
typename Fmt<D>::U SqrtLane(Scope& sc, typename Fmt<D>::U a) {
  if (IsNaN<D>(a)) {
    if (IsSNaN<D>(a)) sc.flags |= kIE;
    return Quiet<D>(a);
  }
  // A negative operand is invalid, which takes precedence over a denormal
  // one (no DE).
  using U = typename Fmt<D>::U;
  constexpr U kSign = U(1) << (sizeof(U) * 8 - 1);
  if (!(a & kSign) || !(a & ~kSign)) sc.Operand<D>(a);
  sc.Begin();
  U r;
  if constexpr (D)
    r = f64_sqrt(float64_t{a}).v;  // negative: default NaN, invalid
  else
    r = f32_sqrt(float32_t{a}).v;
  sc.End();
  return r;
}

template <bool D>
void SqrtT(Ctx& c, int lanes, Xmm& d, const Xmm& s, uint64_t va) {
  using U = typename Fmt<D>::U;
  Scope sc(c, va);
  Xmm r = d;
  for (int i = 0; i < lanes; i++) SetLane<U>(r, i, SqrtLane<D>(sc, Lane<U>(s, i)));
  sc.Finish();
  d = r;
}

template <bool D>
void ArithT(Ctx& c, Op op, int lanes, Xmm& d, const Xmm& s, uint64_t va) {
  using U = typename Fmt<D>::U;
  Scope sc(c, va);
  Xmm r = d;
  for (int i = 0; i < lanes; i++)
    SetLane<U>(r, i, ArithLane<D>(sc, op, Lane<U>(d, i), Lane<U>(s, i)));
  sc.Finish();
  d = r;
}

// Ordered comparison of two non-NaN values: -1, 0, 1. Zeros of either
// sign compare equal.
template <bool D>
int Order(typename Fmt<D>::U a, typename Fmt<D>::U b) {
  using U = typename Fmt<D>::U;
  constexpr U kSign = U(1) << (sizeof(U) * 8 - 1);
  if (!((a | b) & ~kSign)) return 0;
  bool na = a & kSign, nb = b & kSign;
  if (na != nb) return na ? -1 : 1;
  if (a == b) return 0;
  bool less = (a < b) != na;  // magnitude order flips for negatives
  return less ? -1 : 1;
}

template <bool D>
void CmpT(Ctx& c, int lanes, Xmm& d, const Xmm& s, uint8_t pred, uint64_t va) {
  using U = typename Fmt<D>::U;
  Scope sc(c, va);
  const bool signal_qnan = pred == 1 || pred == 2 || pred == 5 || pred == 6;
  Xmm r = d;
  for (int i = 0; i < lanes; i++) {
    U a = Lane<U>(d, i), b = Lane<U>(s, i);
    bool unord = IsNaN<D>(a) || IsNaN<D>(b);
    bool res;
    if (unord) {
      if (IsSNaN<D>(a) || IsSNaN<D>(b) || signal_qnan) sc.flags |= kIE;
      res = pred == 3 || pred == 4 || pred == 5 || pred == 6;
    } else {
      sc.Operand<D>(a);
      sc.Operand<D>(b);
      int o = Order<D>(a, b);
      switch (pred & 7) {
        case 0: res = o == 0; break;
        case 1: res = o < 0; break;
        case 2: res = o <= 0; break;
        case 3: res = false; break;
        case 4: res = o != 0; break;
        case 5: res = !(o < 0); break;
        case 6: res = !(o <= 0); break;
        default: res = true; break;
      }
    }
    SetLane<U>(r, i, res ? U(~U(0)) : U(0));
  }
  sc.Finish();
  d = r;
}

template <bool D>
int ComiT(Ctx& c, bool ordered, const Xmm& x, const Xmm& y, uint64_t va) {
  using U = typename Fmt<D>::U;
  Scope sc(c, va);
  U a = Lane<U>(x, 0), b = Lane<U>(y, 0);
  int r;
  if (IsNaN<D>(a) || IsNaN<D>(b)) {
    if (ordered || IsSNaN<D>(a) || IsSNaN<D>(b)) sc.flags |= kIE;
    r = 3;
  } else {
    sc.Operand<D>(a);
    sc.Operand<D>(b);
    int o = Order<D>(a, b);
    r = o > 0 ? 0 : o < 0 ? 1 : 2;
  }
  sc.Finish();
  return r;
}

template <bool D>
void MinMaxT(Ctx& c, bool max, int lanes, Xmm& d, const Xmm& s, uint64_t va) {
  using U = typename Fmt<D>::U;
  Scope sc(c, va);
  Xmm r = d;
  for (int i = 0; i < lanes; i++) {
    U a = Lane<U>(d, i), b = Lane<U>(s, i);
    U v = b;  // NaNs and equal values (zeros of either sign too) give s
    if (IsNaN<D>(a) || IsNaN<D>(b)) {
      sc.flags |= kIE;  // an ordered comparison: QNaNs are invalid too
    } else {
      sc.Operand<D>(a);
      sc.Operand<D>(b);
      int o = Order<D>(a, b);
      if (max ? o > 0 : o < 0) v = a;
    }
    SetLane<U>(r, i, v);
  }
  sc.Finish();
  d = r;
}

// One float <-> double conversion. NaNs keep their payload (truncated or
// extended) and are quieted, as SoftFloat's 8086 NaN conversion does.
template <bool FromDbl>
typename Fmt<!FromDbl>::U CvtLane(Scope& sc, typename Fmt<FromDbl>::U a) {
  if (!IsNaN<FromDbl>(a)) sc.Operand<FromDbl>(a);
  sc.Begin();
  typename Fmt<!FromDbl>::U r;
  if constexpr (FromDbl)
    r = f64_to_f32(float64_t{a}).v;
  else
    r = f32_to_f64(float32_t{a}).v;
  sc.End();
  return r;
}

}  // namespace

void CheckAligned(Ctx& c, uint64_t a, uint64_t va) {
  (void)c;
  if (a & 15)
    FatalF("sse: misaligned 16-byte operand %08llX at %08llX (#GP)",
           (unsigned long long)a, (unsigned long long)va);
}

void CheckAligned(Ctx& c, uint64_t a, uint64_t va, unsigned align) {
  (void)c;
  if (a & (align - 1))
    FatalF("sse: misaligned %u-byte operand %08llX at %08llX (#GP)", align,
           (unsigned long long)a, (unsigned long long)va);
}

void Arith(Ctx& c, Op op, bool dbl, int lanes, Xmm& d, const Xmm& s,
           uint64_t va) {
  if (dbl)
    ArithT<true>(c, op, lanes, d, s, va);
  else
    ArithT<false>(c, op, lanes, d, s, va);
}

void Cmp(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint8_t pred,
         uint64_t va) {
  if (dbl)
    CmpT<true>(c, lanes, d, s, pred, va);
  else
    CmpT<false>(c, lanes, d, s, pred, va);
}

int Comi(Ctx& c, bool dbl, bool ordered, const Xmm& a, const Xmm& b,
         uint64_t va) {
  return dbl ? ComiT<true>(c, ordered, a, b, va) : ComiT<false>(c, ordered, a, b, va);
}

void Sqrt(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint64_t va) {
  if (dbl)
    SqrtT<true>(c, lanes, d, s, va);
  else
    SqrtT<false>(c, lanes, d, s, va);
}

// For a positive normal x, the float nearest to y = 1/sqrt(x). A first
// guess from binary64 (sqrt, then a division: two roundings, then a
// third to binary32) is checked exactly and moved by an ulp if needed:
// r is correct iff y lies between the midpoints to its neighbours, i.e.
// lo^2 * x <= 1 <= hi^2 * x, evaluated in 128-bit integers. y is never a
// midpoint (it would be a dyadic rational with a 25-bit significand whose
// square's reciprocal is a float), so there are no ties.
uint32_t RsqrtLane(uint32_t x) {
  const uint32_t exp = (x >> 23) & 0xFF, frac = x & 0x7FFFFF;
  if (exp == 0xFF && frac) return x | 0x00400000u;     // NaN: quieted
  if (exp == 0) return (x & 0x80000000u) | 0x7F800000u;  // ±0, denormal: ±inf
  if (x & 0x80000000u) return 0xFFC00000u;               // negative: default NaN
  if (exp == 0xFF) return 0;                             // +inf: +0
  const uint_fast8_t mode = softfloat_roundingMode, flags = softfloat_exceptionFlags;
  softfloat_roundingMode = softfloat_round_near_even;
  float64_t one{0x3FF0000000000000ull};
  uint32_t r = f64_to_f32(f64_div(one, f64_sqrt(f32_to_f64(float32_t{x})))).v;
  softfloat_roundingMode = mode;
  softfloat_exceptionFlags = flags;
  const unsigned __int128 mx = frac | 0x800000u;
  const int ex = int(exp) - 150;  // x = mx * 2^ex
  for (;;) {
    const uint32_t rexp = (r >> 23) & 0xFF;
    const unsigned __int128 mr = (r & 0x7FFFFF) | 0x800000u;
    const int er = int(rexp) - 150;  // r = mr * 2^er
    // Midpoints in units of 2^(er-2): the one below is closer when r's
    // significand is a power of two (the next float down has half the ulp).
    const unsigned __int128 lo = 4 * mr - (mr == 0x800000u ? 1 : 2), hi = 4 * mr + 2;
    const int shift = -(2 * (er - 2) + ex);  // 1 = 2^shift in those units
    const unsigned __int128 one_scaled = static_cast<unsigned __int128>(1) << shift;
    if (lo * lo * mx > one_scaled)
      r--;
    else if (hi * hi * mx < one_scaled)
      r++;
    else
      return r;
  }
}

void Rsqrt(Xmm& d, const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 4; i++) SetLane<uint32_t>(r, i, RsqrtLane(Lane<uint32_t>(s, i)));
  d = r;
}

void Cvtsi2sd(Xmm& d, int32_t v) { SetLane<uint64_t>(d, 0, i32_to_f64(v).v); }

uint32_t CvtToI32(Ctx& c, bool dbl, const Xmm& s, bool truncate, uint64_t va) {
  Scope sc(c, va);
  bool den = dbl ? IsDenormal<true>(Lane<uint64_t>(s, 0))
                 : IsDenormal<false>(Lane<uint32_t>(s, 0));
  if (den && (c.mxcsr & kDAZ))
    FatalF("sse: DAZ with a denormal operand at %08llX (not modelled)",
             (unsigned long long)va);
  uint8_t mode = truncate ? uint8_t(softfloat_round_minMag) : RoundingMode(c.mxcsr);
  sc.Begin();
  int32_t r = dbl ? f64_to_i32(float64_t{Lane<uint64_t>(s, 0)}, mode, true)
                  : f32_to_i32(float32_t{Lane<uint32_t>(s, 0)}, mode, true);
  sc.End();
  sc.Finish();
  return uint32_t(r);
}

uint64_t CvtToI64(Ctx& c, bool dbl, const Xmm& s, bool truncate, uint64_t va) {
  Scope sc(c, va);
  bool den = dbl ? IsDenormal<true>(Lane<uint64_t>(s, 0))
                 : IsDenormal<false>(Lane<uint32_t>(s, 0));
  if (den && (c.mxcsr & kDAZ))
    FatalF("sse: DAZ with a denormal operand at %08llX (not modelled)",
           (unsigned long long)va);
  uint8_t mode = truncate ? uint8_t(softfloat_round_minMag) : RoundingMode(c.mxcsr);
  sc.Begin();
  int64_t r = dbl ? f64_to_i64(float64_t{Lane<uint64_t>(s, 0)}, mode, true)
                  : f32_to_i64(float32_t{Lane<uint32_t>(s, 0)}, mode, true);
  sc.End();
  sc.Finish();
  return uint64_t(r);
}

void MinMax(Ctx& c, bool max, bool dbl, int lanes, Xmm& d, const Xmm& s,
            uint64_t va) {
  if (dbl)
    MinMaxT<true>(c, max, lanes, d, s, va);
  else
    MinMaxT<false>(c, max, lanes, d, s, va);
}

void CvtFloat(Ctx& c, bool to_dbl, int lanes, Xmm& d, const Xmm& s, uint64_t va) {
  Scope sc(c, va);
  Xmm r = d;
  if (to_dbl) {
    for (int i = 0; i < lanes; i++)
      SetLane<uint64_t>(r, i, CvtLane<false>(sc, Lane<uint32_t>(s, i)));
  } else {
    for (int i = 0; i < lanes; i++)
      SetLane<uint32_t>(r, i, CvtLane<true>(sc, Lane<uint64_t>(s, i)));
    if (lanes == 2) SetLane<uint64_t>(r, 1, 0);  // CVTPD2PS zeroes the top
  }
  sc.Finish();
  d = r;
}

void CvtIntToF(Ctx& c, bool dbl, bool src64, uint64_t v, Xmm& d, uint64_t va) {
  Scope sc(c, va);
  sc.Begin();
  if (dbl)
    SetLane<uint64_t>(d, 0, src64 ? i64_to_f64(int64_t(v)).v : i32_to_f64(int32_t(v)).v);
  else
    SetLane<uint32_t>(d, 0, src64 ? i64_to_f32(int64_t(v)).v : i32_to_f32(int32_t(v)).v);
  sc.End();
  sc.Finish();
}

void CvtToDq(Ctx& c, bool dbl, bool truncate, Xmm& d, const Xmm& s, uint64_t va) {
  Scope sc(c, va);
  const uint8_t mode = truncate ? uint8_t(softfloat_round_minMag) : RoundingMode(c.mxcsr);
  Xmm r{};
  sc.Begin();
  for (int i = 0; i < (dbl ? 2 : 4); i++) {
    bool den = dbl ? IsDenormal<true>(Lane<uint64_t>(s, i)) : IsDenormal<false>(Lane<uint32_t>(s, i));
    if (den && (c.mxcsr & kDAZ))
      FatalF("sse: DAZ with a denormal operand at %08llX (not modelled)",
             (unsigned long long)va);
    int32_t v = dbl ? f64_to_i32(float64_t{Lane<uint64_t>(s, i)}, mode, true)
                    : f32_to_i32(float32_t{Lane<uint32_t>(s, i)}, mode, true);
    SetLane<int32_t>(r, i, v);
  }
  sc.End();
  sc.Finish();
  d = r;
}

void Cvtdq2ps(Ctx& c, Xmm& d, const Xmm& s, uint64_t va) {
  Scope sc(c, va);
  Xmm r;
  sc.Begin();
  for (int i = 0; i < 4; i++) SetLane<uint32_t>(r, i, i32_to_f32(Lane<int32_t>(s, i)).v);
  sc.End();
  sc.Finish();
  d = r;
}

void Ldmxcsr(Ctx& c, uint32_t v, uint64_t va) {
  if (v & 0xFFFF0000u)
    FatalF("sse: ldmxcsr %08X sets reserved bits at %08llX (#GP)", v,
           (unsigned long long)va);
  c.mxcsr = v;
}

}  // namespace recomp::sse
