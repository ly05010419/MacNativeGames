#pragma once
// SSE/SSE2 for lifted code. The XMM registers and MXCSR live in Ctx and
// are used in place. Floating-point lanes are computed with SoftFloat,
// with the SSE NaN rules applied here (the shared SoftFloat build uses the
// x87 ones), so results and MXCSR flags match the hardware bit for bit on
// any host. Integer and data-movement operations are plain portable C++.
//
// Not modelled, fatal if reached: unmasked SIMD exceptions (#XM), and FTZ
// or DAZ actually changing a result (a tiny result or a denormal operand
// while they are set).
//
// RSQRTPS has no architectural result, only an error bound (|rel. error|
// <= 1.5 * 2^-12), and CPUs differ: here it is the correctly rounded
// 1/sqrt(x), within the bound and the same on every host (and what a
// native replacement computes). tests/sse_rsqrt_test checks it
// exhaustively; the bit-for-bit oracle against the host skips it.
//
// Only the forms the supported games use are implemented; the lifter
// reports the rest as unimplemented.

#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "runtime/ctx.h"

namespace recomp::sse {

inline constexpr uint32_t kMxcsrDefault = 0x1F80;  // all masked, RN

template <typename T>
inline T Lane(const Xmm& x, int i) {
  T v;
  std::memcpy(&v, x.b + i * sizeof(T), sizeof(T));
  return v;
}
template <typename T>
inline void SetLane(Xmm& x, int i, T v) {
  std::memcpy(x.b + i * sizeof(T), &v, sizeof(T));
}

// `bytes` of guest memory at a, zero-extended to a register.
inline Xmm Ld(const uint8_t* B, uint64_t a, int bytes) {
  Xmm x{};
  std::memcpy(x.b, B + a, bytes);
  return x;
}
inline void St(uint8_t* B, uint64_t a, const Xmm& x, int bytes) {
  std::memcpy(B + a, x.b, bytes);
}
// Legacy-SSE m128 operands other than movups/movupd/movdqu must be 16-byte
// aligned (#GP otherwise): fatal, naming the instruction.
void CheckAligned(Ctx& c, uint64_t a, uint64_t va);
// The same for VEX moves that require `align` (16 or 32) bytes.
void CheckAligned(Ctx& c, uint64_t a, uint64_t va, unsigned align);

// ---- floating point ---------------------------------------------------
enum Op : uint8_t { kAdd, kSub, kMul, kDiv };

// d = d op s, lane-wise: `lanes` lanes of double (dbl) or float. Scalar
// forms pass lanes = 1 and keep the upper lanes of d.
void Arith(Ctx& c, Op op, bool dbl, int lanes, Xmm& d, const Xmm& s,
           uint64_t va);

// CMPPS/PD/SS/SD: each lane of d becomes all ones if `d pred s` holds,
// else zero. pred is the imm8 (0 EQ, 1 LT, 2 LE, 3 UNORD, 4 NEQ, 5 NLT,
// 6 NLE, 7 ORD).
void Cmp(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint8_t pred,
         uint64_t va);

// (U)COMISS/SD of the low lanes: 0 greater, 1 less, 2 equal, 3 unordered
// (the caller sets ZF/PF/CF). `ordered` = COMIS (invalid on QNaN too).
int Comi(Ctx& c, bool dbl, bool ordered, const Xmm& a, const Xmm& b,
         uint64_t va);

// SQRTSD/SQRTSS/SQRTPD/SQRTPS: d = sqrt(s), lane-wise; scalar forms keep
// the upper lanes of d.
void Sqrt(Ctx& c, bool dbl, int lanes, Xmm& d, const Xmm& s, uint64_t va);

// RSQRTPS: d = 1/sqrt(s) in all four float lanes (see above). No flags:
// ±0 and denormals give ±inf, negative values the default NaN, +inf +0,
// NaNs themselves quieted.
void Rsqrt(Xmm& d, const Xmm& s);
uint32_t RsqrtLane(uint32_t x);

// CVTSI2SD: the low lane of d = (double)v (always exact).
void Cvtsi2sd(Xmm& d, int32_t v);

// CVT(T)SS2SI / CVT(T)SD2SI of the low lane of s.
uint32_t CvtToI32(Ctx& c, bool dbl, const Xmm& s, bool truncate, uint64_t va);

// CVT(T)SS2SI / CVT(T)SD2SI to a 64-bit register.
uint64_t CvtToI64(Ctx& c, bool dbl, const Xmm& s, bool truncate, uint64_t va);

// MINSS/SD/PS/PD (max = false) and MAXSS/SD/PS/PD, lane-wise: d op s ? d : s.
// A NaN in either operand (invalid, quiet ones too) or equal values,
// zeros of either sign included, give s. Scalar forms keep d's upper lanes.
void MinMax(Ctx& c, bool max, bool dbl, int lanes, Xmm& d, const Xmm& s,
            uint64_t va);

// CVTSS2SD / CVTPS2PD (to_dbl, 1 or 2 lanes) and CVTSD2SS / CVTPD2PS.
// Scalar forms keep d's other lanes; CVTPD2PS zeroes its upper half.
void CvtFloat(Ctx& c, bool to_dbl, int lanes, Xmm& d, const Xmm& s, uint64_t va);

// CVTSI2SS / CVTSI2SD of a 32-bit or 64-bit (src64) integer into the low
// lane, rounded by MXCSR (only int64 -> double and int -> float can be
// inexact).
void CvtIntToF(Ctx& c, bool dbl, bool src64, uint64_t v, Xmm& d, uint64_t va);

// CVTTPS2DQ / CVTPS2DQ (truncate false) and CVTTPD2DQ / CVTPD2DQ (dbl,
// two lanes into the low half, the upper one zeroed): float lanes to
// int32, 0x80000000 for NaN and out of range (invalid).
void CvtToDq(Ctx& c, bool dbl, bool truncate, Xmm& d, const Xmm& s, uint64_t va);

// RSQRTSS: the low lane of d = 1/sqrt(the low lane of s), as RsqrtLane.
inline void RsqrtSs(Xmm& d, const Xmm& s) {
  SetLane<uint32_t>(d, 0, RsqrtLane(Lane<uint32_t>(s, 0)));
}

// CVTDQ2PS: four int32 lanes to float, rounded by MXCSR.
void Cvtdq2ps(Ctx& c, Xmm& d, const Xmm& s, uint64_t va);

// CVTDQ2PD: the two low int32 lanes of s as doubles (always exact).
inline Xmm Cvtdq2pd(const Xmm& s) {
  Xmm r;
  for (int i = 0; i < 2; i++) {
    double v = double(Lane<int32_t>(s, i));
    SetLane<double>(r, i, v);
  }
  return r;
}

// LDMXCSR: reserved bits set would #GP: fatal.
void Ldmxcsr(Ctx& c, uint32_t v, uint64_t va);

// ---- bitwise and integer ----------------------------------------------
inline void And(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 16; i++) d.b[i] &= s.b[i];
}
inline void AndNot(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 16; i++) d.b[i] = uint8_t(~d.b[i] & s.b[i]);
}
inline void Or(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 16; i++) d.b[i] |= s.b[i];
}
inline void Xor(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 16; i++) d.b[i] ^= s.b[i];
}

// PCMPEQB/W/D: lanes of type T equal -> all ones.
template <typename T>
inline void CmpEq(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, Lane<T>(d, i) == Lane<T>(s, i) ? T(~T(0)) : T(0));
}
// PADDB/W/D/Q (wrapping).
template <typename T>
inline void Add(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, T(Lane<T>(d, i) + Lane<T>(s, i)));
}
// PSUBB/W/D/Q (wrapping).
template <typename T>
inline void Sub(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, T(Lane<T>(d, i) - Lane<T>(s, i)));
}
// PCMPGTB/W/D (signed lanes of type T).
template <typename T>
inline void CmpGt(Xmm& d, const Xmm& s) {
  using U = std::make_unsigned_t<T>;
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<U>(d, i, Lane<T>(d, i) > Lane<T>(s, i) ? U(~U(0)) : U(0));
}
// PMAXUB/UW/UD, PMAXSB/SW/SD, PMINxx: lane-wise, by T's order.
template <typename T>
inline void Max(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T a = Lane<T>(d, i), b = Lane<T>(s, i);
    SetLane<T>(d, i, a > b ? a : b);
  }
}
template <typename T>
inline void Min(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T a = Lane<T>(d, i), b = Lane<T>(s, i);
    SetLane<T>(d, i, a < b ? a : b);
  }
}
// PABSB/W/D (SSSE3): the minimum value stays itself (as unsigned, its
// absolute value).
template <typename T>
inline void Abs(Xmm& d, const Xmm& s) {
  using U = std::make_unsigned_t<T>;
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T v = Lane<T>(s, i);
    SetLane<U>(d, i, v < 0 ? U(-U(v)) : U(v));
  }
}
// PSLLW/D/Q, PSRLW/D/Q, PSRAW/D: every lane of type T by `count` (the
// immediate, or the low quadword of the register/memory operand). Counts
// past the lane width clear the lane (logical) or fill it with its sign.
enum ShiftKind : uint8_t { kShl, kShr, kSar };
template <typename T>
inline void Shift(Xmm& d, uint64_t count, ShiftKind k) {
  using S = std::make_signed_t<T>;
  constexpr uint64_t kBits = sizeof(T) * 8;
  for (int i = 0; i < int(16 / sizeof(T)); i++) {
    T v = Lane<T>(d, i);
    if (k == kSar)
      v = T(S(v) >> (count >= kBits ? kBits - 1 : count));
    else if (count >= kBits)
      v = 0;
    else
      v = k == kShl ? T(v << count) : T(v >> count);
    SetLane<T>(d, i, v);
  }
}
// PUNPCKLBW/WD/DQ/QDQ (high: PUNPCKH..): the low (high) halves of d and s
// interleaved, d's lane first.
template <typename T>
inline void Unpck(Xmm& d, const Xmm& s, bool high) {
  constexpr int n = int(16 / sizeof(T));
  Xmm r;
  int b = high ? n / 2 : 0;
  for (int i = 0; i < n / 2; i++) {
    SetLane<T>(r, 2 * i, Lane<T>(d, b + i));
    SetLane<T>(r, 2 * i + 1, Lane<T>(s, b + i));
  }
  d = r;
}
// PSHUFLW / PSHUFHW: the four words of the low (high) quadword of s
// picked by imm; the other quadword copied.
inline void ShufW(Xmm& d, const Xmm& s, uint8_t imm, bool high) {
  Xmm r = s;
  int b = high ? 4 : 0;
  for (int i = 0; i < 4; i++)
    SetLane<uint16_t>(r, b + i, Lane<uint16_t>(s, b + ((imm >> (2 * i)) & 3)));
  d = r;
}
// PMULUDQ / PMULDQ (SSE4.1): the even 32-bit lanes multiplied into
// 64-bit products.
inline void MulUDq(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 2; i++)
    SetLane<uint64_t>(d, i, uint64_t(Lane<uint32_t>(d, 2 * i)) * Lane<uint32_t>(s, 2 * i));
}
inline void MulDq(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 2; i++)
    SetLane<int64_t>(d, i, int64_t(Lane<int32_t>(d, 2 * i)) * Lane<int32_t>(s, 2 * i));
}
// PMULLW / PMULLD (SSE4.1): the low half of each lane's product.
template <typename T>
inline void MulLo(Xmm& d, const Xmm& s) {
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    SetLane<T>(d, i, T(uint64_t(Lane<T>(d, i)) * Lane<T>(s, i)));
}
// PACKSSDW / PACKSSWB / PACKUSWB / PACKUSDW: d's lanes then s's,
// narrowed with saturation (signed From to To).
template <typename From, typename To>
inline void Pack(Xmm& d, const Xmm& s) {
  constexpr int n = int(16 / sizeof(From));
  auto sat = [](From v) {
    constexpr From lo = From(std::numeric_limits<To>::min()),
                   hi = From(std::numeric_limits<To>::max());
    return To(v < lo ? lo : v > hi ? hi : v);
  };
  Xmm r;
  for (int i = 0; i < n; i++) SetLane<To>(r, i, sat(Lane<From>(d, i)));
  for (int i = 0; i < n; i++) SetLane<To>(r, n + i, sat(Lane<From>(s, i)));
  d = r;
}
// PMOVSX/PMOVZX (SSE4.1): the low lanes of s widened from From to To.
template <typename From, typename To>
inline void Extend(Xmm& d, const Xmm& s) {
  Xmm r;
  for (int i = 0; i < int(16 / sizeof(To)); i++) SetLane<To>(r, i, To(Lane<From>(s, i)));
  d = r;
}
// PALIGNR (SSSE3): bytes n.. of the 32-byte d:s (d high), zero past it.
inline void Alignr(Xmm& d, const Xmm& s, uint8_t n) {
  uint8_t t[32];
  std::memcpy(t, s.b, 16);
  std::memcpy(t + 16, d.b, 16);
  Xmm r;
  for (int i = 0; i < 16; i++) r.b[i] = n + i < 32 ? t[n + i] : 0;
  d = r;
}
// MOVMSKPS / MOVMSKPD: the sign bits of the float (double) lanes.
inline uint32_t MovMskPs(const Xmm& s) {
  uint32_t m = 0;
  for (int i = 0; i < 4; i++) m |= (Lane<uint32_t>(s, i) >> 31) << i;
  return m;
}
inline uint32_t MovMskPd(const Xmm& s) {
  return uint32_t(Lane<uint64_t>(s, 0) >> 63) | uint32_t(Lane<uint64_t>(s, 1) >> 63) << 1;
}
// BLENDVPS/PD (SSE4.1): lanes of s where the mask lane's sign bit is set.
template <typename T>
inline void BlendV(Xmm& d, const Xmm& s, const Xmm& mask) {
  constexpr int kTop = int(sizeof(T) * 8 - 1);
  for (int i = 0; i < int(16 / sizeof(T)); i++)
    if (Lane<T>(mask, i) >> kTop) SetLane<T>(d, i, Lane<T>(s, i));
}

// PMAXSW.
inline void MaxS16(Xmm& d, const Xmm& s) {
  for (int i = 0; i < 8; i++) {
    int16_t a = Lane<int16_t>(d, i), b = Lane<int16_t>(s, i);
    SetLane<int16_t>(d, i, a > b ? a : b);
  }
}
// PMOVMSKB: the sign bit of every byte.
inline uint32_t MovMskB(const Xmm& s) {
  uint32_t m = 0;
  for (int i = 0; i < 16; i++) m |= uint32_t(s.b[i] >> 7) << i;
  return m;
}
// PSLLQ/PSRLQ: each quadword shifted by `count` (>= 64 clears it). The
// register/memory form passes the low quadword of its operand.
inline void ShlQ(Xmm& d, uint64_t count) {
  for (int i = 0; i < 2; i++)
    SetLane<uint64_t>(d, i, count > 63 ? 0 : Lane<uint64_t>(d, i) << count);
}
inline void ShrQ(Xmm& d, uint64_t count) {
  for (int i = 0; i < 2; i++)
    SetLane<uint64_t>(d, i, count > 63 ? 0 : Lane<uint64_t>(d, i) >> count);
}
// PSRLDQ / PSLLDQ: the whole register shifted by `n` bytes (more than 15
// clears it).
inline void ShrDq(Xmm& d, uint8_t n) {
  Xmm r{};
  for (int i = 0; i + n < 16; i++) r.b[i] = d.b[i + n];
  d = r;
}
inline void ShlDq(Xmm& d, uint8_t n) {
  Xmm r{};
  for (int i = n; i < 16; i++) r.b[i] = d.b[i - n];
  d = r;
}
// PSHUFD.
inline void ShufD(Xmm& d, const Xmm& s, uint8_t imm) {
  Xmm r;
  for (int i = 0; i < 4; i++)
    SetLane<uint32_t>(r, i, Lane<uint32_t>(s, (imm >> (2 * i)) & 3));
  d = r;
}
// SHUFPS: low two lanes from d, high two from s.
inline void ShufPs(Xmm& d, const Xmm& s, uint8_t imm) {
  Xmm r;
  SetLane<uint32_t>(r, 0, Lane<uint32_t>(d, imm & 3));
  SetLane<uint32_t>(r, 1, Lane<uint32_t>(d, (imm >> 2) & 3));
  SetLane<uint32_t>(r, 2, Lane<uint32_t>(s, (imm >> 4) & 3));
  SetLane<uint32_t>(r, 3, Lane<uint32_t>(s, (imm >> 6) & 3));
  d = r;
}
// UNPCKLPD: high quadword of d = low quadword of s.
inline void UnpckLPd(Xmm& d, const Xmm& s) {
  SetLane<uint64_t>(d, 1, Lane<uint64_t>(s, 0));
}
// UNPCKHPD: low quadword of d = its high one, high quadword = s's high one.
inline void UnpckHPd(Xmm& d, const Xmm& s) {
  SetLane<uint64_t>(d, 0, Lane<uint64_t>(d, 1));
  SetLane<uint64_t>(d, 1, Lane<uint64_t>(s, 1));
}
// UNPCKLPS / UNPCKHPS: the low / high two lanes of d and s interleaved.
inline void UnpckPs(Xmm& d, const Xmm& s, bool high) {
  Xmm r;
  int b = high ? 2 : 0;
  SetLane<uint32_t>(r, 0, Lane<uint32_t>(d, b));
  SetLane<uint32_t>(r, 1, Lane<uint32_t>(s, b));
  SetLane<uint32_t>(r, 2, Lane<uint32_t>(d, b + 1));
  SetLane<uint32_t>(r, 3, Lane<uint32_t>(s, b + 1));
  d = r;
}
// MOVLHPS: high quadword of d = low quadword of s. MOVHLPS: low quadword
// of d = high quadword of s.
inline void MovLHPs(Xmm& d, const Xmm& s) {
  SetLane<uint64_t>(d, 1, Lane<uint64_t>(s, 0));
}
inline void MovHLPs(Xmm& d, const Xmm& s) {
  SetLane<uint64_t>(d, 0, Lane<uint64_t>(s, 1));
}
// SHUFPD: low quadword from d, high from s, picked by imm bits 0 and 1.
inline void ShufPd(Xmm& d, const Xmm& s, uint8_t imm) {
  Xmm r;
  SetLane<uint64_t>(r, 0, Lane<uint64_t>(d, imm & 1));
  SetLane<uint64_t>(r, 1, Lane<uint64_t>(s, (imm >> 1) & 1));
  d = r;
}

}  // namespace recomp::sse
