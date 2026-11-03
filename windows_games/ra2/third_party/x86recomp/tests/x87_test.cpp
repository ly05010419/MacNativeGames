// Oracle test for the x87 model (src/runtime/x87.cpp). Dev-only, x86-64
// hosts only: every case loads the same full FPU image (frstor) into the
// host x87 and into the model, runs one instruction on both and compares
// the fnsave images (control, status and tag words, all eight registers)
// plus any memory result.
//
// Everything except the transcendentals must agree bit for bit. For
// fsin/fcos/fsincos/fptan/fpatan/f2xm1/fyl2x/fyl2xp1 the results may differ
// by at most 1 ulp (the hardware is not correctly rounded and the model
// is); C1 may then differ too. The maximum ulp error per instruction is
// reported.
//
// Usage: x87_test [seed] [scale]   (scale multiplies the random counts)

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "runtime/x87.h"

#if !defined(__x86_64__)
#error "x87_test needs an x86-64 host"
#endif

namespace {

using recomp::x87::Const;
using recomp::x87::F80;
using recomp::x87::Op;
using recomp::x87::State;

// --- FPU images ------------------------------------------------------

struct Image {
  uint8_t b[108];
};

uint16_t Rd16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}
void Wr16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
F80 RdF80(const uint8_t* p) {
  F80 v;
  std::memcpy(&v.sig, p, 8);
  v.se = Rd16(p + 8);
  return v;
}
void WrF80(uint8_t* p, F80 v) {
  std::memcpy(p, &v.sig, 8);
  Wr16(p + 8, v.se);
}

// Image accessors (st(i) order, as fnsave stores the registers).
F80 St(const Image& im, int i) { return RdF80(im.b + 28 + 10 * i); }
uint16_t Cw(const Image& im) { return Rd16(im.b); }
uint16_t Sw(const Image& im) { return Rd16(im.b + 4); }
uint16_t Tw(const Image& im) { return Rd16(im.b + 8); }
int Top(const Image& im) { return (Sw(im) >> 11) & 7; }
// Tag of st(i).
int TagSt(const Image& im, int i) {
  return (Tw(im) >> (2 * ((Top(im) + i) & 7))) & 3;
}

// --- host side -------------------------------------------------------

using HostFn = void (*)(uint8_t* img, uint8_t* mem);

#define HOST(name, insn)                                              \
  void name(uint8_t* img, uint8_t* mem) {                             \
    __asm__ volatile("frstor (%0)\n\t" insn "\n\tfnsave (%0)"         \
                     :                                                \
                     : "r"(img), "r"(mem)                             \
                     : "memory", "cc");                               \
  }

// Register forms are written as bytes so that GNU as' historical swap of
// fsub/fsubr (and fdiv/fdivr) in AT&T syntax cannot confuse anything.
#define B2(a, b) ".byte " #a ", " #b

HOST(H_fld_m32, "flds (%1)")
HOST(H_fld_m64, "fldl (%1)")
HOST(H_fld_m80, "fldt (%1)")
HOST(H_fild_m16, "filds (%1)")
HOST(H_fild_m32, "fildl (%1)")
HOST(H_fild_m64, "fildll (%1)")
HOST(H_fld_st1, B2(0xD9, 0xC1))
HOST(H_fld_st7, B2(0xD9, 0xC7))
HOST(H_fld1, "fld1")
HOST(H_fldl2t, "fldl2t")
HOST(H_fldl2e, "fldl2e")
HOST(H_fldpi, "fldpi")
HOST(H_fldlg2, "fldlg2")
HOST(H_fldln2, "fldln2")
HOST(H_fldz, "fldz")

HOST(H_fst_m32, "fsts (%1)")
HOST(H_fstp_m32, "fstps (%1)")
HOST(H_fst_m64, "fstl (%1)")
HOST(H_fstp_m64, "fstpl (%1)")
HOST(H_fstp_m80, "fstpt (%1)")
HOST(H_fst_st3, B2(0xDD, 0xD3))
HOST(H_fstp_st1, B2(0xDD, 0xD9))
HOST(H_fist_m16, "fists (%1)")
HOST(H_fistp_m16, "fistps (%1)")
HOST(H_fist_m32, "fistl (%1)")
HOST(H_fistp_m32, "fistpl (%1)")
HOST(H_fistp_m64, "fistpll (%1)")

#define ARITH_HOST(OPN, m32, m64, i16, i32, d8, dc, de)              \
  HOST(H_##OPN##_m32, m32 " (%1)")                                   \
  HOST(H_##OPN##_m64, m64 " (%1)")                                   \
  HOST(H_##OPN##_i16, i16 " (%1)")                                   \
  HOST(H_##OPN##_i32, i32 " (%1)")                                   \
  HOST(H_##OPN##_st0_st3, B2(0xD8, d8 + 3))                          \
  HOST(H_##OPN##_st0_st1, B2(0xD8, d8 + 1))                          \
  HOST(H_##OPN##_st3_st0, B2(0xDC, dc + 3))                          \
  HOST(H_##OPN##_p_st1, B2(0xDE, de + 1))

// Intel encodings: D8 /r st0 op st(i); DC/DE st(i) op st0 (DC E8+i is
// FSUB st(i),st0 = st(i) - st0, DC E0+i is FSUBR).
ARITH_HOST(add, "fadds", "faddl", "fiadds", "fiaddl", 0xC0, 0xC0, 0xC0)
ARITH_HOST(mul, "fmuls", "fmull", "fimuls", "fimull", 0xC8, 0xC8, 0xC8)
ARITH_HOST(sub, "fsubs", "fsubl", "fisubs", "fisubl", 0xE0, 0xE8, 0xE8)
ARITH_HOST(subr, "fsubrs", "fsubrl", "fisubrs", "fisubrl", 0xE8, 0xE0, 0xE0)
ARITH_HOST(div, "fdivs", "fdivl", "fidivs", "fidivl", 0xF0, 0xF8, 0xF8)
ARITH_HOST(divr, "fdivrs", "fdivrl", "fidivrs", "fidivrl", 0xF8, 0xF0, 0xF0)

HOST(H_fcom_st3, B2(0xD8, 0xD3))
HOST(H_fcomp_st1, B2(0xD8, 0xD9))
HOST(H_fcompp, B2(0xDE, 0xD9))
HOST(H_fucom_st3, B2(0xDD, 0xE3))
HOST(H_fucomp_st1, B2(0xDD, 0xE9))
HOST(H_fucompp, B2(0xDA, 0xE9))
HOST(H_fcom_m32, "fcoms (%1)")
HOST(H_fcomp_m64, "fcompl (%1)")
HOST(H_ficom_m16, "ficoms (%1)")
HOST(H_ficomp_m32, "ficompl (%1)")
HOST(H_fcomi_st3,
     B2(0xDB, 0xF3) "\n\tsetz (%1)\n\tsetp 1(%1)\n\tsetc 2(%1)")
HOST(H_fucomip_st1,
     B2(0xDF, 0xE9) "\n\tsetz (%1)\n\tsetp 1(%1)\n\tsetc 2(%1)")
HOST(H_ftst, "ftst")
HOST(H_fxam, "fxam")

HOST(H_fchs, "fchs")
HOST(H_fabs, "fabs")
HOST(H_fsqrt, "fsqrt")
HOST(H_frndint, "frndint")
HOST(H_fscale, "fscale")
HOST(H_fxtract, "fxtract")
HOST(H_fprem, "fprem")
HOST(H_fprem1, "fprem1")
HOST(H_fsin, "fsin")
HOST(H_fcos, "fcos")
HOST(H_fsincos, "fsincos")
HOST(H_fptan, "fptan")
HOST(H_fpatan, "fpatan")
HOST(H_f2xm1, "f2xm1")
HOST(H_fyl2x, "fyl2x")
HOST(H_fyl2xp1, "fyl2xp1")
HOST(H_fxch_st3, B2(0xD9, 0xCB))
HOST(H_fcmove_st2, "cmp %0, %0\n\t" B2(0xDA, 0xCA))
HOST(H_ffree_st2, B2(0xDD, 0xC2))
HOST(H_fincstp, "fincstp")
HOST(H_fdecstp, "fdecstp")
HOST(H_fninit, "fninit")
HOST(H_fnclex, "fnclex")
HOST(H_fldcw, "fldcw (%1)")
HOST(H_fnstenv, "fnstenv (%1)")
HOST(H_fldenv, "fldenv (%1)")

// --- model side ------------------------------------------------------

using ModelFn = void (*)(State& s, uint8_t* mem);

uint32_t M32(const uint8_t* m) {
  uint32_t v;
  std::memcpy(&v, m, 4);
  return v;
}
uint64_t M64(const uint8_t* m) {
  uint64_t v;
  std::memcpy(&v, m, 8);
  return v;
}
template <typename T>
void Out(uint8_t* m, T v) {
  std::memcpy(m, &v, sizeof v);
}

#define MODEL(name, body) \
  void name(State& s, uint8_t* m) { (void)m; body; }

namespace x = recomp::x87;

MODEL(M_fld_m32, x::LoadF32(s, M32(m)))
MODEL(M_fld_m64, x::LoadF64(s, M64(m)))
MODEL(M_fld_m80, x::LoadF80(s, RdF80(m)))
MODEL(M_fild_m16, x::LoadI16(s, static_cast<int16_t>(Rd16(m))))
MODEL(M_fild_m32, x::LoadI32(s, static_cast<int32_t>(M32(m))))
MODEL(M_fild_m64, x::LoadI64(s, static_cast<int64_t>(M64(m))))
MODEL(M_fld_st1, x::LoadSt(s, 1))
MODEL(M_fld_st7, x::LoadSt(s, 7))
MODEL(M_fld1, x::LoadConst(s, Const::One))
MODEL(M_fldl2t, x::LoadConst(s, Const::L2T))
MODEL(M_fldl2e, x::LoadConst(s, Const::L2E))
MODEL(M_fldpi, x::LoadConst(s, Const::Pi))
MODEL(M_fldlg2, x::LoadConst(s, Const::LG2))
MODEL(M_fldln2, x::LoadConst(s, Const::LN2))
MODEL(M_fldz, x::LoadConst(s, Const::Zero))

MODEL(M_fst_m32, Out(m, x::StoreF32(s)))
MODEL(M_fstp_m32, Out(m, x::StoreF32(s)); x::Pop(s))
MODEL(M_fst_m64, Out(m, x::StoreF64(s)))
MODEL(M_fstp_m64, Out(m, x::StoreF64(s)); x::Pop(s))
MODEL(M_fstp_m80, WrF80(m, x::StoreF80(s)); x::Pop(s))
MODEL(M_fst_st3, x::StoreSt(s, 3))
MODEL(M_fstp_st1, x::StoreSt(s, 1); x::Pop(s))
MODEL(M_fist_m16, Out(m, x::StoreI16(s)))
MODEL(M_fistp_m16, Out(m, x::StoreI16(s)); x::Pop(s))
MODEL(M_fist_m32, Out(m, x::StoreI32(s)))
MODEL(M_fistp_m32, Out(m, x::StoreI32(s)); x::Pop(s))
MODEL(M_fistp_m64, Out(m, x::StoreI64(s)); x::Pop(s))

#define ARITH_MODEL(OPN, OP)                                           \
  MODEL(M_##OPN##_m32, x::ArithF32(s, OP, M32(m)))                     \
  MODEL(M_##OPN##_m64, x::ArithF64(s, OP, M64(m)))                     \
  MODEL(M_##OPN##_i16, x::ArithI16(s, OP, static_cast<int16_t>(Rd16(m)))) \
  MODEL(M_##OPN##_i32, x::ArithI32(s, OP, static_cast<int32_t>(M32(m)))) \
  MODEL(M_##OPN##_st0_st3, x::ArithSt0Sti(s, OP, 3))                   \
  MODEL(M_##OPN##_st0_st1, x::ArithSt0Sti(s, OP, 1))                   \
  MODEL(M_##OPN##_st3_st0, x::ArithStiSt0(s, OP, 3))                   \
  MODEL(M_##OPN##_p_st1, x::ArithStiSt0(s, OP, 1); x::Pop(s))

ARITH_MODEL(add, Op::Add)
ARITH_MODEL(mul, Op::Mul)
ARITH_MODEL(sub, Op::Sub)
ARITH_MODEL(subr, Op::SubR)
ARITH_MODEL(div, Op::Div)
ARITH_MODEL(divr, Op::DivR)

void FlagsOut(uint8_t* m, x::EFlagsResult r) {
  m[0] = r.zf;
  m[1] = r.pf;
  m[2] = r.cf;
}

MODEL(M_fcom_st3, x::CompareSt(s, 3, false))
MODEL(M_fcomp_st1, x::CompareSt(s, 1, false); x::Pop(s))
MODEL(M_fcompp, x::CompareSt(s, 1, false); x::Pop(s); x::Pop(s))
MODEL(M_fucom_st3, x::CompareSt(s, 3, true))
MODEL(M_fucomp_st1, x::CompareSt(s, 1, true); x::Pop(s))
MODEL(M_fucompp, x::CompareSt(s, 1, true); x::Pop(s); x::Pop(s))
MODEL(M_fcom_m32, x::CompareF32(s, M32(m)))
MODEL(M_fcomp_m64, x::CompareF64(s, M64(m)); x::Pop(s))
MODEL(M_ficom_m16, x::CompareI16(s, static_cast<int16_t>(Rd16(m))))
MODEL(M_ficomp_m32, x::CompareI32(s, static_cast<int32_t>(M32(m)));
      x::Pop(s))
MODEL(M_fcomi_st3, FlagsOut(m, x::CompareStEFlags(s, 3, false)))
MODEL(M_fucomip_st1, FlagsOut(m, x::CompareStEFlags(s, 1, true)); x::Pop(s))
MODEL(M_ftst, x::Test(s))
MODEL(M_fxam, x::Examine(s))

MODEL(M_fchs, x::Chs(s))
MODEL(M_fabs, x::Abs(s))
MODEL(M_fsqrt, x::Sqrt(s))
MODEL(M_frndint, x::RndInt(s))
MODEL(M_fscale, x::Scale(s))
MODEL(M_fxtract, x::Xtract(s))
MODEL(M_fprem, x::Prem(s))
MODEL(M_fprem1, x::Prem1(s))
MODEL(M_fsin, x::Sin(s))
MODEL(M_fcos, x::Cos(s))
MODEL(M_fsincos, x::SinCos(s))
MODEL(M_fptan, x::Ptan(s))
MODEL(M_fpatan, x::Patan(s))
MODEL(M_f2xm1, x::F2xm1(s))
MODEL(M_fyl2x, x::Yl2x(s))
MODEL(M_fyl2xp1, x::Yl2xp1(s))
MODEL(M_fxch_st3, x::Xch(s, 3))
MODEL(M_fcmove_st2, x::CopySt(s, 2))
MODEL(M_ffree_st2, x::Free(s, 2))
MODEL(M_fincstp, x::IncStp(s))
MODEL(M_fdecstp, x::DecStp(s))
MODEL(M_fninit, x::Init(s))
MODEL(M_fnclex, x::ClearExceptions(s))
MODEL(M_fldcw, x::SetControlWord(s, Rd16(m)))
MODEL(M_fnstenv, x::StoreEnv(s, m))
MODEL(M_fldenv, x::LoadEnv(s, m))

// --- random values ---------------------------------------------------

struct Rng {
  std::mt19937_64 g;
  explicit Rng(uint64_t seed) : g(seed) {}
  uint64_t U64() { return g(); }
  uint32_t Below(uint32_t n) { return static_cast<uint32_t>(g() % n); }
  bool Chance(uint32_t num, uint32_t den) { return Below(den) < num; }
  int Range(int lo, int hi) { return lo + static_cast<int>(Below(hi - lo + 1)); }
};

constexpr uint64_t kJ = 0x8000000000000000ull;

F80 Mk(bool sign, int exp, uint64_t sig) {
  return {sig, static_cast<uint16_t>((sign ? 0x8000 : 0) | exp)};
}

F80 FromDouble(double d) {
  uint64_t b;
  std::memcpy(&b, &d, 8);
  bool sign = b >> 63;
  int e = (b >> 52) & 0x7FF;
  uint64_t f = b & 0xFFFFFFFFFFFFFull;
  if (e == 0 && f == 0) return Mk(sign, 0, 0);
  return Mk(sign, e - 1023 + 0x3FFF, kJ | (f << 11));
}

// Interesting 80-bit values. `allow_odd` adds unsupported encodings and
// pseudo-denormals.
F80 RandValue(Rng& r, bool allow_odd = true) {
  bool sign = r.Chance(1, 2);
  uint64_t sig = r.U64();
  switch (r.Below(24)) {
    case 0: return Mk(sign, 0, 0);
    case 1: {
      uint64_t d = (sig & ~kJ) >> r.Below(63);
      return Mk(sign, 0, d ? d : 1);
    }
    case 2:
      if (allow_odd) return Mk(sign, 0, sig | kJ);
      return Mk(sign, 1, sig | kJ);
    case 3: return Mk(sign, 0x7FFF, kJ);
    case 4: return Mk(sign, 0x7FFF, sig | kJ | (1ull << 62));  // QNaN
    case 5: {
      uint64_t p = sig & 0x3FFFFFFFFFFFFFFFull;
      return Mk(sign, 0x7FFF, kJ | (p ? p : 1));  // SNaN
    }
    case 6: return Mk(true, 0x7FFF, 0xC000000000000000ull);
    case 7:
      if (allow_odd) {
        int e = r.Chance(1, 4) ? 0x7FFF : r.Range(1, 0x7FFE);
        return Mk(sign, e, sig & ~kJ);
      }
      return Mk(sign, 0x3FFF, sig | kJ);
    case 8: return Mk(sign, r.Range(1, 70), sig | kJ);
    case 9: return Mk(sign, r.Range(0x7FF0, 0x7FFE), sig | kJ);
    case 10: {
      int64_t k = static_cast<int64_t>(r.Below(2000)) - 1000;
      return FromDouble(k / (r.Chance(1, 2) ? 2.0 : 4.0));
    }
    case 11:
      return Mk(sign, 0x3FFF + r.Range(-30, 30),
                (sig | kJ) & 0xFFFFFF0000000000ull);
    case 12:
      return Mk(sign, 0x3FFF + r.Range(-300, 300),
                (sig | kJ) & 0xFFFFFFFFFFFFF800ull);
    case 13: {
      uint64_t s = kJ | (1ull << r.Below(63)) | (1ull << r.Below(63));
      return Mk(sign, 0x3FFF + r.Range(-80, 80), s);
    }
    case 14: return Mk(sign, r.Range(1, 0x7FFE), sig | kJ);
    case 15: {  // around the PC=24/53 rounding points
      uint64_t s = sig | kJ;
      int pos = r.Chance(1, 2) ? 40 : 11;
      s &= ~((1ull << pos) - 1);
      s |= (1ull << (pos - 1)) * r.Below(2);
      if (r.Chance(1, 2)) s |= 1;
      return Mk(sign, 0x3FFF + r.Range(-20, 20), s);
    }
    case 16: return Mk(sign, r.Range(0x3FFF - 16382 + 1, 0x3FFF - 16300),
                       sig | kJ);  // tiny normals
    default: return Mk(sign, 0x3FFF + r.Range(-70, 70), sig | kJ);
  }
}

// Values for integer stores and frndint: near integer-range edges and
// rounding ties.
F80 RandIntish(Rng& r) {
  bool sign = r.Chance(1, 2);
  switch (r.Below(5)) {
    case 0: {
      static const int kBits[] = {15, 16, 31, 32, 63, 64};
      int b = kBits[r.Below(6)];
      // 2^b plus/minus a little, with a fraction
      uint64_t s = kJ;
      if (r.Chance(1, 2)) {
        s = ~0ull << r.Below(4);  // just below 2^b
        return Mk(sign, 0x3FFF + b - 1, s);
      }
      s |= r.U64() >> (r.Range(40, 63));
      return Mk(sign, 0x3FFF + b, s);
    }
    case 1: {  // n + 1/2 ties
      uint64_t n = r.U64() >> r.Range(1, 62);
      double d = static_cast<double>(n % 100000) + 0.5;
      F80 v = FromDouble(sign ? -d : d);
      return v;
    }
    case 2: {  // random value with a fraction, magnitude up to 2^k
      int k = r.Range(0, 66);
      return Mk(sign, 0x3FFF + k, r.U64() | kJ);
    }
    case 3: return Mk(sign, 0x3FFF + r.Range(-5, 0), r.U64() | kJ);
    default: return RandValue(r);
  }
}

uint32_t RandF32(Rng& r) {
  uint32_t v = static_cast<uint32_t>(r.U64());
  switch (r.Below(8)) {
    case 0: return v & 0x807FFFFF;  // zero / denormal
    case 1: return (v & 0x80000000) | 0x7F800000 | (r.Chance(1, 2) ? 0 : v & 0x7FFFFF);
    case 2: return (v & 0x807FFFFF) | ((r.Range(100, 154)) << 23);
    case 3: return (v & 0x80000000) | (r.Chance(1, 2) ? 0x00000001 : 0x7F7FFFFF);
    default: return v;
  }
}

uint64_t RandF64(Rng& r) {
  uint64_t v = r.U64();
  switch (r.Below(8)) {
    case 0: return v & 0x800FFFFFFFFFFFFFull;
    case 1: return (v & kJ) | 0x7FF0000000000000ull |
                   (r.Chance(1, 2) ? 0 : v & 0xFFFFFFFFFFFFFull);
    case 2: return (v & 0x800FFFFFFFFFFFFFull) |
                   (static_cast<uint64_t>(r.Range(900, 1150)) << 52);
    default: return v;
  }
}

uint16_t RandCw(Rng& r) {
  static const int kPc[] = {0, 2, 3, 3, 1};
  int pc = kPc[r.Below(5)];
  int rc = r.Below(4);
  uint16_t cw = static_cast<uint16_t>(0x007F | (pc << 8) | (rc << 10));
  if (r.Chance(1, 8)) cw |= 0x1000;  // IC (ignored, but stored)
  return cw;
}

// A random full image: masked control word, sticky flags without ES/B,
// random C0-C3, random TOP and tags.
Image RandImage(Rng& r, int empty_pct, bool allow_odd = true) {
  Image im{};
  Wr16(im.b, RandCw(r));
  Wr16(im.b + 2, 0xFFFF);
  uint16_t sw = static_cast<uint16_t>(r.U64() & 0x477F);  // C0-C3, flags, SF
  int top = r.Below(8);
  sw |= static_cast<uint16_t>(top << 11);
  Wr16(im.b + 4, sw);
  Wr16(im.b + 6, 0xFFFF);
  uint16_t tw = 0;
  for (int p = 0; p < 8; ++p) {
    int tag = r.Below(100) < static_cast<uint32_t>(empty_pct) ? 3 : 0;
    tw |= static_cast<uint16_t>(tag << (2 * p));
  }
  Wr16(im.b + 8, tw);
  Wr16(im.b + 10, 0xFFFF);
  for (int i = 0; i < 8; ++i) WrF80(im.b + 28 + 10 * i, RandValue(r, allow_odd));
  return im;
}

void SetSt(Image& im, int i, F80 v) { WrF80(im.b + 28 + 10 * i, v); }
void SetTagSt(Image& im, int i, int tag) {
  int p = (Top(im) + i) & 7;
  uint16_t tw = Tw(im);
  tw = static_cast<uint16_t>((tw & ~(3 << (2 * p))) | (tag << (2 * p)));
  Wr16(im.b + 8, tw);
}
void SetRcPc(Image& im, int rc, int pc) {
  uint16_t cw = Cw(im);
  cw = static_cast<uint16_t>((cw & ~0x0F00) | (pc << 8) | (rc << 10));
  Wr16(im.b, cw);
}

// --- comparison ------------------------------------------------------

// Distance in ulps (of the smaller exponent) between two finite values of
// the same sign; large for anything else.
uint64_t UlpDiff(F80 a, F80 b) {
  if (a.sig == b.sig && a.se == b.se) return 0;
  if ((a.se ^ b.se) & 0x8000) {
    bool za = !(a.se & 0x7FFF) && !a.sig, zb = !(b.se & 0x7FFF) && !b.sig;
    if (za && zb) return 0;
    return ~0ull;
  }
  int ea = a.se & 0x7FFF, eb = b.se & 0x7FFF;
  if (ea == 0x7FFF || eb == 0x7FFF) return ~0ull;
  if (ea == 0) ea = 1;
  if (eb == 0) eb = 1;
  if (ea > eb) {
    std::swap(a, b);
    std::swap(ea, eb);
  }
  if (eb - ea > 1) return ~0ull;
  if (eb == ea) return a.sig > b.sig ? a.sig - b.sig : b.sig - a.sig;
  // b has the larger exponent: b = 2 * b.sig in a's ulps.
  unsigned __int128 vb = static_cast<unsigned __int128>(b.sig) * 2;
  unsigned __int128 va = a.sig;
  unsigned __int128 d = vb > va ? vb - va : va - vb;
  return d > 0xFFFFFFFFull ? ~0ull : static_cast<uint64_t>(d);
}

std::string Hex(F80 v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04x:%016" PRIx64, v.se, v.sig);
  return buf;
}

enum class Kind { Exact, Transcendental };

struct OpDef {
  const char* name;
  HostFn host;
  ModelFn model;
  int in_bytes;   // random memory input
  int out_bytes;  // memory output compared
  Kind kind;
  int profile;    // value profile for st0/st1
  // Status bits not compared: documented differences between Intel and
  // other x87 implementations (see main()).
  uint16_t sw_ignore = 0;
};

enum Profile {
  kAny = 0,
  kIntish,
  kTrig,
  kAtan,
  kExp,
  kLog,
  kLogp1,
  kScale,
  kRem,
  kEnv,
  kCw,
  kSqrt,
};

struct Stats {
  uint64_t cases = 0, fails = 0;
  uint64_t max_ulp = 0, ulp1 = 0, c1_diff = 0;
};

int g_print_budget = 0;  // per instruction, reset in main()

void Dump(const char* tag, const Image& im) {
  std::printf("    %s cw=%04x sw=%04x tw=%04x st0=%s st1=%s st2=%s st3=%s\n",
              tag, Cw(im), Sw(im), Tw(im), Hex(St(im, 0)).c_str(),
              Hex(St(im, 1)).c_str(), Hex(St(im, 2)).c_str(),
              Hex(St(im, 3)).c_str());
}

void DumpMem(const char* tag, const uint8_t* m, int n) {
  std::printf("    %s mem:", tag);
  for (int i = n - 1; i >= 0; --i) std::printf("%02x", m[i]);
  std::printf("\n");
}

// Runs one case, counting and reporting disagreements in `st`.
void RunCase(const OpDef& op, const Image& in, const uint8_t* mem_in,
             Stats& st) {
  alignas(16) Image hi = in, mo = in;
  alignas(16) uint8_t hm[32], mm[32];
  std::memcpy(hm, mem_in, 32);
  std::memcpy(mm, mem_in, 32);

  op.host(hi.b, hm);

  State s{};
  recomp::x87::Restore(s, in.b);
  op.model(s, mm);
  recomp::x87::Save(s, mo.b);
  if (op.sw_ignore) {
    Wr16(hi.b + 4, Sw(hi) & ~op.sw_ignore);
    Wr16(mo.b + 4, Sw(mo) & ~op.sw_ignore);
  }

  ++st.cases;
  bool ok = true;
  // Bytes 12..27 are the instruction/operand pointers: not modelled.
  if (std::memcmp(hi.b, mo.b, 12) != 0) ok = false;
  if (op.out_bytes && std::memcmp(hm, mm, op.out_bytes) != 0) ok = false;
  uint64_t worst = 0;
  bool regs_same = std::memcmp(hi.b + 28, mo.b + 28, 80) == 0;
  if (!regs_same) {
    if (op.kind == Kind::Exact) {
      ok = false;
    } else {
      for (int i = 0; i < 8; ++i) {
        uint64_t d = UlpDiff(St(hi, i), St(mo, i));
        if (d > worst) worst = d;
      }
    }
  }
  if (op.kind == Kind::Transcendental) {
    if (worst > st.max_ulp) st.max_ulp = worst;
    if (worst == 1) ++st.ulp1;
    bool words_same = std::memcmp(hi.b, mo.b, 12) == 0;
    uint16_t c1 = 0x0200;
    bool same_but_c1 =
        Rd16(hi.b) == Rd16(mo.b) && Tw(hi) == Tw(mo) &&
        ((Sw(hi) ^ Sw(mo)) & ~c1) == 0;
    if (!words_same && same_but_c1) ++st.c1_diff;
    ok = (words_same || same_but_c1) && worst <= 1 &&
         (!op.out_bytes || std::memcmp(hm, mm, op.out_bytes) == 0);
  }
  if (!ok) {
    ++st.fails;
    if (g_print_budget > 0) {
      --g_print_budget;
      std::printf("  MISMATCH %s\n", op.name);
      Dump("in   ", in);
      if (op.in_bytes) DumpMem("in   ", mem_in, op.in_bytes);
      Dump("host ", hi);
      Dump("model", mo);
      if (op.out_bytes) {
        DumpMem("host ", hm, op.out_bytes);
        DumpMem("model", mm, op.out_bytes);
      }
    }
  }
}

// --- per-op value profiles -------------------------------------------

void ApplyProfile(Rng& r, int profile, Image& im) {
  switch (profile) {
    case kIntish:
      SetSt(im, 0, RandIntish(r));
      break;
    case kSqrt:
      if (r.Chance(3, 4)) {
        F80 v = RandValue(r);
        v.se &= 0x7FFF;
        SetSt(im, 0, v);
      }
      break;
    case kTrig:
      if (r.Chance(7, 8)) {
        // Mostly in range, including near multiples of pi/2 and huge.
        bool sign = r.Chance(1, 2);
        int e;
        switch (r.Below(6)) {
          case 0: e = r.Range(-80, -60); break;
          case 1: e = r.Range(60, 66); break;
          case 2: e = r.Range(0, 62); break;
          default: e = r.Range(-6, 4); break;
        }
        uint64_t sig = r.U64() | kJ;
        if (r.Chance(1, 8)) {
          // close to k * pi/2
          static const uint64_t kPiSig = 0xC90FDAA22168C235ull;
          sig = kPiSig ^ (r.U64() >> r.Range(40, 63));
          sig |= kJ;
          e = r.Range(0, 20);
        }
        SetSt(im, 0, Mk(sign, 0x3FFF + e, sig));
      }
      break;
    case kAtan:
      if (r.Chance(7, 8)) {
        for (int i = 0; i < 2; ++i) {
          int e = r.Chance(1, 8) ? r.Range(-16000, 16000) : r.Range(-70, 70);
          SetSt(im, i, Mk(r.Chance(1, 2), 0x3FFF + e, r.U64() | kJ));
        }
        if (r.Chance(1, 8)) SetSt(im, 1, St(im, 0));  // |y| == |x|
      }
      break;
    case kExp:
      if (r.Chance(7, 8)) {
        int e = r.Chance(1, 4) ? r.Range(-90, -1) : r.Range(-8, -1);
        SetSt(im, 0, Mk(r.Chance(1, 2), 0x3FFF + e, r.U64() | kJ));
      }
      break;
    case kLog:
      if (r.Chance(7, 8)) {
        int e = r.Chance(1, 4) ? r.Range(-16380, 16380) : r.Range(-4, 4);
        uint64_t sig = r.U64() | kJ;
        if (r.Chance(1, 6)) {  // near 1
          e = r.Chance(1, 2) ? 0 : -1;
          sig = e == 0 ? kJ | (r.U64() >> r.Range(1, 63))
                       : ~0ull ^ (r.U64() >> r.Range(1, 63));
        }
        if (r.Chance(1, 16)) sig = kJ;  // power of two
        SetSt(im, 0, Mk(false, 0x3FFF + e, sig));
        int ey = r.Chance(1, 8) ? r.Range(-16000, 16000) : r.Range(-10, 10);
        SetSt(im, 1, Mk(r.Chance(1, 2), 0x3FFF + ey, r.U64() | kJ));
      }
      break;
    case kLogp1:
      if (r.Chance(7, 8)) {
        int e = r.Chance(1, 4) ? r.Range(-200, -2) : r.Range(-12, -2);
        SetSt(im, 0, Mk(r.Chance(1, 2), 0x3FFF + e, r.U64() | kJ));
        int ey = r.Chance(1, 8) ? r.Range(-16000, 16000) : r.Range(-10, 10);
        SetSt(im, 1, Mk(r.Chance(1, 2), 0x3FFF + ey, r.U64() | kJ));
      }
      break;
    case kScale:
      if (r.Chance(3, 4)) {
        F80 sc;
        switch (r.Below(4)) {
          case 0: sc = FromDouble(static_cast<double>(r.Range(-70, 70))); break;
          case 1: sc = FromDouble(r.Range(-40000, 40000) / 2.0); break;
          case 2: sc = Mk(r.Chance(1, 2), 0x3FFF + r.Range(-3, 20), r.U64() | kJ); break;
          default: sc = RandValue(r); break;
        }
        SetSt(im, 1, sc);
      }
      break;
    case kRem:
      if (r.Chance(3, 4)) {
        int ea = r.Range(-100, 100);
        int eb = r.Chance(1, 3) ? ea - r.Range(0, 200) : ea - r.Range(-3, 70);
        if (r.Chance(1, 8)) eb = ea - r.Range(60, 20000);
        auto clamp = [](int e) { return e < -16382 ? -16382 : e > 16383 ? 16383 : e; };
        SetSt(im, 0, Mk(r.Chance(1, 2), 0x3FFF + clamp(ea), r.U64() | kJ));
        SetSt(im, 1, Mk(r.Chance(1, 2), 0x3FFF + clamp(eb), r.U64() | kJ));
      }
      break;
    default:
      break;
  }
}

void RandMem(Rng& r, const OpDef& op, uint8_t* mem) {
  for (int i = 0; i < 32; ++i) mem[i] = static_cast<uint8_t>(r.U64());
  switch (op.in_bytes) {
    case 2: {
      int16_t v = static_cast<int16_t>(r.Chance(1, 2) ? r.Range(-40, 40)
                                                    : static_cast<int>(r.U64()));
      Wr16(mem, static_cast<uint16_t>(v));
      break;
    }
    case 4:
      if (op.profile == kCw) {
        Wr16(mem, static_cast<uint16_t>(r.U64() | 0x3F));  // keep masked
      } else if (std::strstr(op.name, "fi") == op.name) {
        int32_t v = r.Chance(1, 2) ? r.Range(-1000, 1000)
                                   : static_cast<int32_t>(r.U64());
        std::memcpy(mem, &v, 4);
      } else {
        uint32_t v = RandF32(r);
        std::memcpy(mem, &v, 4);
      }
      break;
    case 8:
      if (std::strstr(op.name, "fild") == op.name) {
        int64_t v = static_cast<int64_t>(r.U64()) >> r.Below(64);
        std::memcpy(mem, &v, 8);
      } else {
        uint64_t v = RandF64(r);
        std::memcpy(mem, &v, 8);
      }
      break;
    case 10:
      WrF80(mem, RandValue(r));
      break;
    case 28: {  // fldenv: a random environment with a masked control word
      Image t = RandImage(r, 30);
      std::memcpy(mem, t.b, 28);
      break;
    }
    default:
      break;
  }
}

// --- special-value sweeps -----------------------------------------------

std::vector<F80> SpecialValues() {
  std::vector<F80> v;
  for (int sign = 0; sign < 2; ++sign) {
    bool s = sign;
    v.push_back(Mk(s, 0, 0));
    v.push_back(Mk(s, 0, 1));
    v.push_back(Mk(s, 0, 0x7FFFFFFFFFFFFFFFull));
    v.push_back(Mk(s, 0, kJ));                    // pseudo-denormal
    v.push_back(Mk(s, 1, kJ));                    // min normal
    v.push_back(Mk(s, 0x3FFF, kJ));               // 1
    v.push_back(Mk(s, 0x3FFF, 0xC000000000000000ull));  // 1.5
    v.push_back(Mk(s, 0x4000, kJ));               // 2
    v.push_back(Mk(s, 0x3FFE, 0xFFFFFFFFFFFFFFFFull));  // 1 - 2^-64
    v.push_back(Mk(s, 0x3FFF, kJ | 1));           // 1 + 2^-63
    v.push_back(Mk(s, 0x7FFE, ~0ull));            // max
    v.push_back(Mk(s, 0x7FFF, kJ));               // inf
    v.push_back(Mk(s, 0x7FFF, 0xC000000000000001ull));  // QNaN
    v.push_back(Mk(s, 0x7FFF, 0x8000000000000001ull));  // SNaN
    v.push_back(Mk(s, 0x7FFF, 0xA000000000000000ull));  // SNaN
    v.push_back(Mk(s, 0x3FFF, 0x4000000000000000ull));  // unnormal
    v.push_back(Mk(s, 0x7FFF, 0));                       // pseudo-inf
    v.push_back(Mk(s, 0x7FFF, 0x4000000000000000ull));  // pseudo-NaN
    v.push_back(Mk(s, 0x400D, 0xFFFF000000000000ull));  // 65535 / 2^15 area
    v.push_back(Mk(s, 0x401D, 0xFFFFFFFF00000000ull));  // 2^31 - 1
    v.push_back(Mk(s, 0x403E, kJ));               // 2^63
    v.push_back(Mk(s, 0x3FFE, kJ));               // 0.5
    v.push_back(Mk(s, 0x4000, 0xC90FDAA22168C235ull));  // pi
    v.push_back(Mk(s, 0x3C00, 0xFFFFFFFFFFFFFFFFull));
  }
  v.push_back(Mk(true, 0x7FFF, 0xC000000000000000ull));  // indefinite
  v.push_back(Mk(false, 0x7FFF, 0xC000000000000000ull));
  return v;
}

// Memory operands for the sweep: the b-th special value of the operand's
// type.
void SpecialMem(const OpDef& op, size_t b, F80 v80, uint8_t* mem) {
  static const uint32_t kF32[] = {
      0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x007FFFFF,
      0x00800000, 0x3F800000, 0xBF800000, 0x3FC00000, 0x7F7FFFFF,
      0xFF7FFFFF, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000,
      0x7F800001, 0x7FA00000, 0xFFBFFFFF, 0x7FC00001, 0x4B000000,
      0x5F000000, 0xCF000000, 0x3EAAAAAB, 0x40490FDB};
  static const uint64_t kF64[] = {
      0x0000000000000000ull, 0x8000000000000000ull, 0x0000000000000001ull,
      0x800FFFFFFFFFFFFFull, 0x0010000000000000ull, 0x3FF0000000000000ull,
      0xBFF8000000000000ull, 0x7FEFFFFFFFFFFFFFull, 0x7FF0000000000000ull,
      0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000000ull,
      0x7FF0000000000001ull, 0x7FF4000000000000ull, 0x7FF8000000000001ull,
      0x43E0000000000000ull, 0xC1E0000000000000ull, 0x3FD5555555555555ull,
      0x400921FB54442D18ull, 0x0008000000000000ull};
  static const int64_t kInt[] = {0,      1,          -1,         2,
                                 -2,     32767,      -32768,     65535,
                                 100,    INT32_MAX,  INT32_MIN,  INT64_MAX,
                                 INT64_MIN, 1ll << 53, -(1ll << 31) - 1};
  bool is_int = std::strstr(op.name, "fi") == op.name;
  switch (op.in_bytes) {
    case 10: WrF80(mem, v80); break;
    case 8:
      if (is_int) {
        std::memcpy(mem, &kInt[b % std::size(kInt)], 8);
      } else {
        std::memcpy(mem, &kF64[b % std::size(kF64)], 8);
      }
      break;
    case 4:
      if (op.profile == kCw) break;
      if (is_int) {
        int32_t v = static_cast<int32_t>(kInt[b % std::size(kInt)]);
        std::memcpy(mem, &v, 4);
      } else {
        std::memcpy(mem, &kF32[b % std::size(kF32)], 4);
      }
      break;
    case 2: {
      int16_t v = static_cast<int16_t>(kInt[b % std::size(kInt)]);
      std::memcpy(mem, &v, 2);
      break;
    }
    default: break;
  }
}

// Model-only checks for what the host cannot show without trapping:
// unmasked exceptions and the no-wait/wait distinction.
int UnmaskedChecks() {
  int fails = 0;
  auto expect = [&](bool c, const char* what) {
    if (!c) {
      std::printf("  UNMASKED CHECK FAILED: %s\n", what);
      ++fails;
    }
  };
  State s{};
  x::Init(s);
  x::SetControlWord(s, 0x037E);  // IE unmasked
  x::LoadConst(s, Const::One);
  expect(!s.unmasked_fault, "no fault before an exception");
  x::LoadConst(s, Const::Zero);
  x::Sqrt(s);  // sqrt(0) is fine
  expect(!s.unmasked_fault, "sqrt(0) does not fault");
  x::Chs(s);
  x::LoadConst(s, Const::One);
  x::Chs(s);
  x::Sqrt(s);  // sqrt(-1): IE
  expect(s.unmasked_fault, "sqrt(-1) faults with IE unmasked");
  expect((x::StatusWord(s) & 0x8081) == 0x8081, "IE, ES and B set");
  s.unmasked_fault = false;
  x::ClearExceptions(s);
  expect((x::StatusWord(s) & 0x80FF) == 0, "fnclex clears flags, ES, B");

  // A masked exception unmasked later faults at the next waiting
  // instruction, not at fldcw itself.
  x::Init(s);
  x::LoadConst(s, Const::Zero);
  x::LoadConst(s, Const::One);
  x::ArithSt0Sti(s, Op::Div, 1);  // 1/0: ZE, masked
  expect(!s.unmasked_fault, "masked ZE does not fault");
  x::SetControlWord(s, 0x037B);  // unmask ZE
  expect((x::StatusWord(s) & 0x8080) == 0x8080, "pending ZE shows ES/B");
  expect(!s.unmasked_fault, "fldcw itself does not fault");
  uint16_t sw = x::StatusWord(s);  // fnstsw is no-wait
  (void)sw;
  expect(!s.unmasked_fault, "fnstsw does not fault");
  x::LoadConst(s, Const::One);
  expect(s.unmasked_fault, "next waiting instruction faults");

  // Underflow unmasked; precision masked.
  x::Init(s);
  x::SetControlWord(s, 0x036F);
  uint8_t tiny[10] = {1, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00};  // 2^-16382 * 2^-63
  x::LoadF80(s, RdF80(tiny));
  x::LoadF80(s, RdF80(tiny));
  x::ArithSt0Sti(s, Op::Mul, 1);
  expect(s.unmasked_fault, "unmasked UE faults");

  // Stack overflow with IE masked never faults; unmasked it does.
  x::Init(s);
  s.unmasked_fault = false;  // the runtime's latch; fninit leaves it
  for (int i = 0; i < 9; ++i) x::LoadConst(s, Const::One);
  expect(!s.unmasked_fault && (x::StatusWord(s) & 0x0241) == 0x0241,
         "masked stack overflow: IE, SF, C1");
  x::Init(s);
  x::SetControlWord(s, 0x037E);
  for (int i = 0; i < 9; ++i) x::LoadConst(s, Const::One);
  expect(s.unmasked_fault, "unmasked stack overflow faults");

  // fnstenv masks everything afterwards; fnsave reinitialises.
  x::Init(s);
  x::SetControlWord(s, 0x0240);
  uint8_t env[28];
  x::StoreEnv(s, env);
  expect(Rd16(env) == 0x0240 && x::ControlWord(s) == 0x027F,
         "fnstenv stores the old cw and masks all");
  uint8_t img[108];
  x::LoadConst(s, Const::Pi);
  x::Save(s, img);
  expect(x::ControlWord(s) == 0x037F && x::TagWord(s) == 0xFFFF &&
             x::StatusWord(s) == 0,
         "fnsave reinitialises");
  x::Restore(s, img);
  expect(x::TagWord(s) != 0xFFFF && x::ControlWord(s) == 0x027F,
         "frstor brings the state back");
  return fails;
}

}  // namespace

int main(int argc, char** argv) {
  uint64_t seed = argc > 1 ? std::strtoull(argv[1], nullptr, 0) : 12345;
  double scale = argc > 2 ? std::atof(argv[2]) : 1.0;
  Rng r(seed);

  const Kind E = Kind::Exact, T = Kind::Transcendental;
  // name, host, model, in, out, kind, profile
  std::vector<OpDef> ops = {
      {"fld m32", H_fld_m32, M_fld_m32, 4, 0, E, kAny},
      {"fld m64", H_fld_m64, M_fld_m64, 8, 0, E, kAny},
      {"fld m80", H_fld_m80, M_fld_m80, 10, 0, E, kAny},
      {"fild m16", H_fild_m16, M_fild_m16, 2, 0, E, kAny},
      {"fild m32", H_fild_m32, M_fild_m32, 4, 0, E, kAny},
      {"fild m64", H_fild_m64, M_fild_m64, 8, 0, E, kAny},
      {"fld st1", H_fld_st1, M_fld_st1, 0, 0, E, kAny},
      {"fld st7", H_fld_st7, M_fld_st7, 0, 0, E, kAny},
      {"fld1", H_fld1, M_fld1, 0, 0, E, kAny},
      {"fldl2t", H_fldl2t, M_fldl2t, 0, 0, E, kAny},
      {"fldl2e", H_fldl2e, M_fldl2e, 0, 0, E, kAny},
      {"fldpi", H_fldpi, M_fldpi, 0, 0, E, kAny},
      {"fldlg2", H_fldlg2, M_fldlg2, 0, 0, E, kAny},
      {"fldln2", H_fldln2, M_fldln2, 0, 0, E, kAny},
      {"fldz", H_fldz, M_fldz, 0, 0, E, kAny},
      {"fst m32", H_fst_m32, M_fst_m32, 0, 4, E, kAny},
      {"fstp m32", H_fstp_m32, M_fstp_m32, 0, 4, E, kAny},
      {"fst m64", H_fst_m64, M_fst_m64, 0, 8, E, kAny},
      {"fstp m64", H_fstp_m64, M_fstp_m64, 0, 8, E, kAny},
      {"fstp m80", H_fstp_m80, M_fstp_m80, 0, 10, E, kAny},
      {"fst st3", H_fst_st3, M_fst_st3, 0, 0, E, kAny},
      {"fstp st1", H_fstp_st1, M_fstp_st1, 0, 0, E, kAny},
      {"fist m16", H_fist_m16, M_fist_m16, 0, 2, E, kIntish},
      {"fistp m16", H_fistp_m16, M_fistp_m16, 0, 2, E, kIntish},
      {"fist m32", H_fist_m32, M_fist_m32, 0, 4, E, kIntish},
      {"fistp m32", H_fistp_m32, M_fistp_m32, 0, 4, E, kIntish},
      {"fistp m64", H_fistp_m64, M_fistp_m64, 0, 8, E, kIntish},
#define ARITH_OPS(OPN)                                                      \
  {"f" #OPN " m32", H_##OPN##_m32, M_##OPN##_m32, 4, 0, E, kAny},          \
  {"f" #OPN " m64", H_##OPN##_m64, M_##OPN##_m64, 8, 0, E, kAny},          \
  {"fi" #OPN " m16", H_##OPN##_i16, M_##OPN##_i16, 2, 0, E, kAny},         \
  {"fi" #OPN " m32", H_##OPN##_i32, M_##OPN##_i32, 4, 0, E, kAny},         \
  {"f" #OPN " st0,st3", H_##OPN##_st0_st3, M_##OPN##_st0_st3, 0, 0, E, kAny}, \
  {"f" #OPN " st0,st1", H_##OPN##_st0_st1, M_##OPN##_st0_st1, 0, 0, E, kAny}, \
  {"f" #OPN " st3,st0", H_##OPN##_st3_st0, M_##OPN##_st3_st0, 0, 0, E, kAny}, \
  {"f" #OPN "p st1,st0", H_##OPN##_p_st1, M_##OPN##_p_st1, 0, 0, E, kAny}
      ARITH_OPS(add), ARITH_OPS(mul), ARITH_OPS(sub), ARITH_OPS(subr),
      ARITH_OPS(div), ARITH_OPS(divr),
      {"fcom st3", H_fcom_st3, M_fcom_st3, 0, 0, E, kAny},
      {"fcomp st1", H_fcomp_st1, M_fcomp_st1, 0, 0, E, kAny},
      {"fcompp", H_fcompp, M_fcompp, 0, 0, E, kAny},
      {"fucom st3", H_fucom_st3, M_fucom_st3, 0, 0, E, kAny},
      {"fucomp st1", H_fucomp_st1, M_fucomp_st1, 0, 0, E, kAny},
      {"fucompp", H_fucompp, M_fucompp, 0, 0, E, kAny},
      {"fcom m32", H_fcom_m32, M_fcom_m32, 4, 0, E, kAny},
      {"fcomp m64", H_fcomp_m64, M_fcomp_m64, 8, 0, E, kAny},
      {"ficom m16", H_ficom_m16, M_ficom_m16, 2, 0, E, kAny},
      {"ficomp m32", H_ficomp_m32, M_ficomp_m32, 4, 0, E, kAny},
      // Intel documents FCOMI/FUCOMI as clearing C1; AMD CPUs leave it
      // unchanged. The model follows Intel.
      {"fcomi st3", H_fcomi_st3, M_fcomi_st3, 0, 3, E, kAny, 0x0200},
      {"fucomip st1", H_fucomip_st1, M_fucomip_st1, 0, 3, E, kAny, 0x0200},
      {"ftst", H_ftst, M_ftst, 0, 0, E, kAny},
      {"fxam", H_fxam, M_fxam, 0, 0, E, kAny},
      {"fchs", H_fchs, M_fchs, 0, 0, E, kAny},
      {"fabs", H_fabs, M_fabs, 0, 0, E, kAny},
      {"fsqrt", H_fsqrt, M_fsqrt, 0, 0, E, kSqrt},
      {"frndint", H_frndint, M_frndint, 0, 0, E, kIntish},
      {"fscale", H_fscale, M_fscale, 0, 0, E, kScale},
      {"fxtract", H_fxtract, M_fxtract, 0, 0, E, kAny},
      {"fprem", H_fprem, M_fprem, 0, 0, E, kRem},
      {"fprem1", H_fprem1, M_fprem1, 0, 0, E, kRem},
      {"fxch st3", H_fxch_st3, M_fxch_st3, 0, 0, E, kAny},
      {"fcmove st2", H_fcmove_st2, M_fcmove_st2, 0, 0, E, kAny},
      {"ffree st2", H_ffree_st2, M_ffree_st2, 0, 0, E, kAny},
      {"fincstp", H_fincstp, M_fincstp, 0, 0, E, kAny},
      {"fdecstp", H_fdecstp, M_fdecstp, 0, 0, E, kAny},
      {"fninit", H_fninit, M_fninit, 0, 0, E, kAny},
      {"fnclex", H_fnclex, M_fnclex, 0, 0, E, kAny},
      {"fldcw", H_fldcw, M_fldcw, 4, 0, E, kCw},
      {"fnstenv", H_fnstenv, M_fnstenv, 0, 12, E, kAny},
      {"fldenv", H_fldenv, M_fldenv, 28, 0, E, kEnv},
      {"fsin", H_fsin, M_fsin, 0, 0, T, kTrig},
      {"fcos", H_fcos, M_fcos, 0, 0, T, kTrig},
      {"fsincos", H_fsincos, M_fsincos, 0, 0, T, kTrig},
      {"fptan", H_fptan, M_fptan, 0, 0, T, kTrig},
      {"fpatan", H_fpatan, M_fpatan, 0, 0, T, kAtan},
      {"f2xm1", H_f2xm1, M_f2xm1, 0, 0, T, kExp},
      {"fyl2x", H_fyl2x, M_fyl2x, 0, 0, T, kLog},
      {"fyl2xp1", H_fyl2xp1, M_fyl2xp1, 0, 0, T, kLogp1},
  };

  const std::vector<F80> specials = SpecialValues();
  uint64_t total_cases = 0, total_fails = 0;
  bool failed = false;

  const char* only = std::getenv("X87_TEST_ONLY");
  for (const OpDef& op : ops) {
    if (only && std::strcmp(only, op.name) != 0) continue;
    g_print_budget = 4;
    Stats st;
    alignas(16) uint8_t mem[32];
    // 1. Special operands in st0 (and st1/st3) under every RC x PC.
    for (size_t a = 0; a < specials.size(); ++a) {
      for (size_t b = 0; b < specials.size(); b += (op.in_bytes ? 3 : 1)) {
        for (int rc = 0; rc < 4; ++rc) {
          for (int pc : {0, 2, 3}) {
            if ((rc + pc + a + b) % 3 != 0 && op.kind == Kind::Transcendental)
              continue;  // the slow ones get a third of the sweep
            Image im = RandImage(r, 0);
            SetRcPc(im, rc, pc);
            SetSt(im, 0, specials[a]);
            SetSt(im, 1, specials[b]);
            SetSt(im, 3, specials[b]);
            SetTagSt(im, 7, 3);  // room for a push
            RandMem(r, op, mem);
            SpecialMem(op, b, specials[b], mem);
            RunCase(op, im, mem, st);
          }
        }
      }
    }
    // 2. Random states (with empty registers for stack faults).
    int n = op.kind == Kind::Transcendental ? 60000 : 30000;
    if (std::strstr(op.name, "m32") || std::strstr(op.name, "m64")) n = 50000;
    n = static_cast<int>(n * scale);
    for (int k = 0; k < n; ++k) {
      Image im = RandImage(r, k % 4 == 0 ? 25 : 2);
      if (k % 4 != 0) SetTagSt(im, 7, 3);
      ApplyProfile(r, op.profile, im);
      RandMem(r, op, mem);
      RunCase(op, im, mem, st);
    }
    total_cases += st.cases;
    total_fails += st.fails;
    if (st.fails) failed = true;
    if (op.kind == Kind::Transcendental) {
      std::printf("%-14s %7" PRIu64 " cases %6" PRIu64
                  " fail  max %" PRIu64 " ulp  (%" PRIu64
                  " off by 1 ulp, %" PRIu64 " C1-only diffs)\n",
                  op.name, st.cases, st.fails, st.max_ulp == ~0ull ? 999999 : st.max_ulp,
                  st.ulp1, st.c1_diff);
    } else {
      std::printf("%-14s %7" PRIu64 " cases %6" PRIu64 " fail\n", op.name,
                  st.cases, st.fails);
    }
  }
  if (!only) {
    int uf = UnmaskedChecks();
    std::printf("unmasked-exception checks: %d failures\n", uf);
    if (uf) failed = true;
  }
  std::printf("total: %" PRIu64 " cases, %" PRIu64 " failures\n", total_cases,
              total_fails);
  return failed ? 1 : 0;
}
