#pragma once
// Exact model of the x87 FPU for lifted code (see ARCHITECTURE.md, "x87:
// full 80-bit model"). Portable C++ on top of Berkeley SoftFloat 3e; no
// long double or intrinsics. Host doubles only in the proven-exact run
// paths below ("host doubles"), in the host's default environment.
//
// Conventions:
// - "st(i)" is relative to TOP; physical register = (top + i) & 7.
// - Every operation honours the control word: PC (bits 8-9) rounds the
//   significand of add/sub/mul/div/sqrt results to 24/53/64 bits with the
//   full 15-bit exponent range kept; RC (bits 10-11) applies to every
//   rounding (arithmetic, stores to m32/m64/int, frndint).
// - Exceptions are assumed masked (Windows default 0x027F, game uses
//   masked modes). Flags are accumulated sticky in the status word. If an
//   exception occurs while its mask bit is clear, unmasked_fault latches
//   (checked by the tests); the masked response is still delivered.
// - Stack overflow/underflow follow the masked response: IE + SF set, C1
//   = 1 on overflow / 0 on underflow, result is the real indefinite.
// - Compare results go to C0/C2/C3 of the status word (for fnstsw+sahf);
//   the FCOMI family returns EFLAGS bits instead.

#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <limits>

// The host-double paths need IEEE binary64 evaluated as written: no
// excess precision, no fast-math (-ffast-math, -ffinite-math-only), and
// no contraction into FMA (code that runs them is built with
// RECOMP_X87_FP_FLAGS, CMakeLists.txt).
static_assert(std::numeric_limits<double>::is_iec559);
#if FLT_EVAL_METHOD != 0 || defined(__FAST_MATH__) || \
    (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__)
#error "x87.h needs strict IEEE double evaluation"
#endif

namespace recomp::x87 {

// 80-bit extended value, same layout as the x87 memory format and as
// SoftFloat's extFloat80M: 64-bit significand (explicit integer bit), then
// sign (bit 15) and biased exponent (bits 0-14).
struct F80 {
  uint64_t sig;
  uint16_t se;
};

struct State {
  F80 r[8];        // physical registers R0..R7
  // Control word, 16 bits in a field of its own: lifted runs store sw,
  // empty and top at their end and read cw at the start of the next, and
  // a load of cw widened over sw would miss store forwarding.
  uint32_t cw;
  uint16_t sw;     // status word, TOP field (bits 11-13) kept in `top`
  // Bit p set: physical register p is empty (tag 11). The other tags (00
  // valid, 01 zero, 10 special) always equal the class of the contents
  // (every write sets it; FLDENV/FRSTOR keep only "empty"), so
  // TagWord() derives them.
  uint8_t empty;
  uint8_t top;     // 0..7
  bool unmasked_fault;  // an unmasked exception was raised (see above)
  // Last instruction / operand pointers: zeroed by finit and only carried
  // through fnstenv/fldenv and fnsave/frstor (no game code inspects them).
  uint32_t fip, fdp;
  uint16_t fop;
};

// Test hook: true disables every integer fast path (arithmetic, stores,
// trig), leaving SoftFloat, their reference.
extern bool g_softfloat_only;

enum class Op : uint8_t { Add, Mul, Sub, SubR, Div, DivR };
// Sub: dst - src, SubR: src - dst, Div: dst / src, DivR: src / dst, where
// dst is the register being written (Intel manual operand order).

enum class Const : uint8_t { One, L2T, L2E, Pi, LG2, LN2, Zero };

// FCOMI/FUCOMI result as EFLAGS bits.
struct EFlagsResult {
  bool zf, pf, cf;
};

// --- state -----------------------------------------------------------
void Init(State& s);              // FNINIT: cw 0x037F, sw 0, all empty
uint16_t StatusWord(const State& s);  // FNSTSW: sw with TOP merged in
uint16_t ControlWord(const State& s);
void SetControlWord(State& s, uint16_t cw);  // FLDCW
void ClearExceptions(State& s);   // FNCLEX
uint16_t TagWord(const State& s);

// --- loads (push) ----------------------------------------------------
void LoadF32(State& s, uint32_t bits);   // FLD m32 (exact)
void LoadF64(State& s, uint64_t bits);   // FLD m64 (exact)
void LoadF80(State& s, F80 v);           // FLD m80
void LoadSt(State& s, int i);            // FLD st(i)
void LoadI16(State& s, int16_t v);       // FILD m16
void LoadI32(State& s, int32_t v);       // FILD m32
void LoadI64(State& s, int64_t v);       // FILD m64
void LoadConst(State& s, Const k);       // FLD1/FLDZ/FLDPI/... (RC-aware
                                         // like hardware for pi, l2e, ...)

// --- stores (st0 -> destination; caller pops for the P forms) --------
uint32_t StoreF32(State& s);   // FST m32: rounded with RC
uint64_t StoreF64(State& s);   // FST m64: rounded with RC
F80 StoreF80(State& s);        // FSTP m80 (exact)
void StoreSt(State& s, int i); // FST st(i)
int16_t StoreI16(State& s);    // FIST m16: RC, indefinite 0x8000 + IE
int32_t StoreI32(State& s);    // FIST m32: RC, indefinite 0x80000000
int64_t StoreI64(State& s);    // FISTP m64: RC, indefinite INT64_MIN
void Pop(State& s);            // mark st0 empty, ++TOP

// --- arithmetic ------------------------------------------------------
void ArithF32(State& s, Op op, uint32_t bits);  // st0 = st0 op m32
void ArithF64(State& s, Op op, uint64_t bits);  // st0 = st0 op m64
void ArithI16(State& s, Op op, int16_t v);      // FIADD m16 etc.
void ArithI32(State& s, Op op, int32_t v);      // FIADD m32 etc.
void ArithSt0Sti(State& s, Op op, int i);       // st0 = st0 op st(i)
void ArithStiSt0(State& s, Op op, int i);       // st(i) = st(i) op st0
                                                // (caller pops for ...P)

// --- compares (caller pops for FCOMP/FCOMPP/FUCOMPP) ------------------
// quiet = FUCOM semantics (only SNaN raises IE); otherwise FCOM.
void CompareSt(State& s, int i, bool quiet);
void CompareF32(State& s, uint32_t bits);
void CompareF64(State& s, uint64_t bits);
void CompareI16(State& s, int16_t v);   // FICOM m16
void CompareI32(State& s, int32_t v);   // FICOM m32
EFlagsResult CompareStEFlags(State& s, int i, bool quiet);  // FCOMI/FUCOMI
void Test(State& s);    // FTST
void Examine(State& s); // FXAM

// --- unary / misc ----------------------------------------------------
void Chs(State& s);
void Abs(State& s);
void Sqrt(State& s);     // PC-rounded
void RndInt(State& s);   // RC
void Scale(State& s);    // FSCALE
void Xtract(State& s);   // FXTRACT (pushes)
void Prem(State& s);     // FPREM (C0/C1/C3 quotient bits, C2 partial)
void Prem1(State& s);    // FPREM1
void Sin(State& s);      // FSIN   (C2=1 and unchanged if |x| >= 2^63)
void Cos(State& s);      // FCOS
void SinCos(State& s);   // FSINCOS (pushes)
void Ptan(State& s);     // FPTAN (pushes 1.0)
void Patan(State& s);    // FPATAN: st1 = atan2(st1, st0), pop
void F2xm1(State& s);    // st0 = 2^st0 - 1
void Yl2x(State& s);     // st1 = st1 * log2(st0), pop
void Yl2xp1(State& s);   // st1 = st1 * log2(st0 + 1), pop
void Xch(State& s, int i);      // FXCH st(i)
void CopySt(State& s, int i);   // st0 = st(i) (FCMOVcc when taken)
void Free(State& s, int i);     // FFREE st(i)
void IncStp(State& s);
void DecStp(State& s);

// --- value-level operations for lifted x87 runs ----------------------
//
// Pure forms of the operations above for lifted runs (ARCHITECTURE.md
// "x87 runs"); both share the same cores, so results are identical.
namespace run {

// Lifted runs are used when true (<ID>_X87_RUNS=0 disables them, for A/B;
// RunProcess reads it).
extern bool g_enabled;
// Runs use host doubles (below) when true (<ID>_X87_DOUBLE=0 disables
// them, for A/B; RunProcess reads it).
extern bool g_doubles;

// A register value in a run: F80 without padding (two full 64-bit fields),
// so it travels between calls in registers; passing F80 itself makes
// compilers assemble its padded half in memory (store-forwarding stalls).
struct V {
  uint64_t sig;
  uint64_t se;  // sign/exponent in bits 0-15, the rest zero
};
inline V ToV(F80 f) { return {f.sig, f.se}; }
inline F80 ToF80(V v) { return {v.sig, static_cast<uint16_t>(v.se)}; }

// A register result: value, exception flags and C1 (rounded up), packed
// so it comes back in two registers.
struct RV {
  uint64_t sig;
  uint64_t meta;  // bits 0-15 sign/exponent, 16-31 flags, 32 up
};
inline V Val(RV r) { return {r.sig, r.meta & 0xFFFF}; }
inline uint32_t Flags(RV r) { return static_cast<uint32_t>(r.meta >> 16) & 0xFFFF; }
inline bool Up(RV r) { return (r.meta >> 32) & 1; }

// A memory result: stored bits, flags, C1.
struct SV {
  uint64_t bits;
  uint64_t meta;  // bits 0-15 flags, 16 up
};
inline uint32_t Flags(SV r) { return static_cast<uint32_t>(r.meta) & 0xFFFF; }
inline bool Up(SV r) { return (r.meta >> 16) & 1; }

// dst op src (Intel operand order; dst is the register written).
RV Arith(uint32_t cw, Op op, V dst, V src);
RV ArithF32(uint32_t cw, Op op, V st0, uint32_t bits);
RV ArithF64(uint32_t cw, Op op, V st0, uint64_t bits);
RV ArithI16(uint32_t cw, Op op, V st0, int16_t v);
RV ArithI32(uint32_t cw, Op op, V st0, int32_t v);
RV Sqrt(uint32_t cw, V v);

// FLD m32/m64: the value pushed (SNaN quieted) and IE/DE.
RV LoadF32(uint32_t bits);
RV LoadF64(uint64_t bits);
V FromInt(int64_t v);                // FILD (exact)
V ConstValue(uint32_t cw, Const k);    // FLD1/FLDZ/FLDPI/...

SV StoreF32(uint32_t cw, V v);
SV StoreF64(uint32_t cw, V v);
SV StoreI16(uint32_t cw, V v);  // bits = the integer stored
SV StoreI32(uint32_t cw, V v);
SV StoreI64(uint32_t cw, V v);

// FCOM family: C3/C2/C0 (status word positions) | flags << 16.
uint32_t Compare(V a, V b, bool quiet);
uint32_t CompareF32(V a, uint32_t bits);
uint32_t CompareF64(V a, uint64_t bits);

// --- inline fast paths (the game's common case) ---
//
// D3D leaves the FPU at PC=24 (the game runs it with RC=chop), and m32
// loads and PC=24 results have 24-bit significands, so most run operations
// are add, sub, mul or div of two such values. Those are exact in 64-bit
// integers (a quotient with its remainder as the sticky bit); rounding to
// 24 bits with RC `rc` is done here as RoundPack does it. This core is
// also the model's own fast path (x87.cpp, Fast24), so lifted code and the
// State operations cannot disagree. Anything else (other modes, specials,
// results leaving the normal range, division by zero) returns false.
// Forced inline: compilers otherwise keep these out of line in the big
// lifted functions, which costs most of the gain.
enum class FOp { Add, Sub, Mul, Div };  // Sub: a - b, Div: a / b

// An operation with a zero operand, the other zero or a value the core
// accepts (so exact at its precision): true with the result, false for a
// division by zero. x + 0 is x; an exact zero sum is +0 except when
// rounding down (or when both addends are -0).
[[gnu::always_inline]] inline bool ZeroOp(FOp op, int rc, V a, V b, bool za, bool zb, RV* out) {
  uint64_t sa = a.se & 0x8000u, sb = b.se & 0x8000u;
  switch (op) {
    case FOp::Div:
      if (zb) return false;
      [[fallthrough]];
    case FOp::Mul:
      *out = {0, sa ^ sb};
      return true;
    default: {
      uint64_t sbx = op == FOp::Sub ? sb ^ 0x8000u : sb;
      if (za && zb)
        *out = {0, sa == sbx ? sa : rc == 1 ? 0x8000u : 0u};
      else if (za)
        *out = {b.sig, (b.se & 0x7FFFu) | sbx};
      else
        *out = {a.sig, a.se};
      return true;
    }
  }
}

[[gnu::always_inline]] inline bool Fast24(FOp op, int rc, V a, V b, RV* out) {
  constexpr uint64_t kLow40 = (1ull << 40) - 1, kJ = 1ull << 63;
  uint32_t ea = a.se & 0x7FFFu, eb = b.se & 0x7FFFu;
  if (((a.sig | b.sig) & kLow40) || !(a.sig & b.sig & kJ) || ea - 1 >= 0x7FFEu ||
      eb - 1 >= 0x7FFEu) {
    // A zero operand (the other zero or as above) is exact.
    bool za = !ea && !a.sig, zb = !eb && !b.sig;
    if ((za || ((a.sig & kJ) && !(a.sig & kLow40) && ea - 1 < 0x7FFEu)) &&
        (zb || ((b.sig & kJ) && !(b.sig & kLow40) && eb - 1 < 0x7FFEu)) && (za || zb))
      return ZeroOp(op, rc, a, b, za, zb, out);
    return false;
  }
  bool sa = (a.se & 0x8000u) != 0, sb = (b.se & 0x8000u) != 0, sign;
  uint64_t ma = a.sig >> 40, mb = b.sig >> 40, v, sticky = 0;  // [2^23, 2^24)
  int32_t exp;
  if (op == FOp::Mul) {
    v = ma * mb;  // value = v / 2^46 * 2^(ea + eb - 2 * 0x3FFF)
    sign = sa != sb;
    int lz = __builtin_clzll(v);
    v <<= lz;
    exp = int32_t(ea + eb) - 0x3FFF + 17 - lz;
  } else if (op == FOp::Div) {
    uint64_t n = ma << 40;  // quotient in (2^39, 2^41)
    v = n / mb;
    sticky = n % mb != 0;
    sign = sa != sb;
    int lz = __builtin_clzll(v);
    v <<= lz;
    exp = int32_t(ea) - int32_t(eb) + 0x3FFF + 23 - lz;
  } else {
    bool sbx = op == FOp::Sub ? !sb : sb;
    bool swap = eb > ea || (eb == ea && mb > ma);
    uint64_t mx = swap ? mb : ma, my = swap ? ma : mb;
    int32_t ex = int32_t(swap ? eb : ea), d = ex - int32_t(swap ? ea : eb);
    sign = swap ? sbx : sa;
    if (d > 39) return false;
    uint64_t x = mx << 39, y = (my << 39) >> d;  // exact: value = . / 2^62
    v = sa != sbx ? x - y : x + y;
    if (!v) {  // exact zero: -0 when rounding down, else +0
      *out = {0, rc == 1 ? 0x8000u : 0u};
      return true;
    }
    int lz = __builtin_clzll(v);
    v <<= lz;
    exp = ex + 1 - lz;
  }
  uint64_t keep = v >> 40, rest = (v & kLow40) | sticky;
  bool inc = rc == 0 ? rest > (1ull << 39) || (rest == (1ull << 39) && (keep & 1))
           : rc == 1 ? sign && rest
           : rc == 2 ? !sign && rest
                     : false;
  if (inc && ++keep == (1ull << 24)) {
    keep >>= 1;
    ++exp;
  }
  if (exp < 1 || exp > 0x7FFE) return false;
  *out = {keep << 40, uint64_t(sign ? 0x8000u : 0u) | uint64_t(exp) |
                          (rest ? uint64_t(0x20) << 16 : 0) | uint64_t(inc) << 32};
  return true;
}

[[gnu::always_inline]] inline bool Fast24Op(int rc, Op op, V dst, V src, RV* out) {
  switch (op) {
    case Op::Add: return Fast24(FOp::Add, rc, dst, src, out);
    case Op::Mul: return Fast24(FOp::Mul, rc, dst, src, out);
    case Op::Sub: return Fast24(FOp::Sub, rc, dst, src, out);
    case Op::SubR: return Fast24(FOp::Sub, rc, src, dst, out);
    case Op::Div: return Fast24(FOp::Div, rc, dst, src, out);
    case Op::DivR: return Fast24(FOp::Div, rc, src, dst, out);
  }
  return false;
}

// The other common mode is Windows' default, PC=53 with RC=nearest (games
// that never set the CW run all their float maths in it). m32/m64 loads
// and PC=53 results have at most 53 significand bits, so add, sub, mul and
// div of two such normal values are exact in 128-bit integers (the
// product of two 53-bit significands has 106 bits; an addend shifted by
// up to 64 bits loses none; a 64-bit quotient keeps its remainder as the
// sticky bit), and rounding to kPrec (53, or 24 for m64 operands under
// PC=24) bits with RC `rc` is done here as RoundPack does it. Shared with
// the model (x87.cpp, FastWide) like Fast24. Anything else (specials,
// exponents more than 64 apart, results leaving the normal range,
// division by zero, under PC=24 a zero added to a value it would round)
// returns false.
template <int kPrec>
[[gnu::always_inline]] inline bool FastWide(FOp op, int rc, V a, V b, RV* out) {
  constexpr uint64_t kLow11 = (1ull << 11) - 1, kJ = 1ull << 63;
  constexpr int kDrop = 64 - kPrec;
  uint32_t ea = a.se & 0x7FFFu, eb = b.se & 0x7FFFu;
  if (((a.sig | b.sig) & kLow11) || !(a.sig & b.sig & kJ) || ea - 1 >= 0x7FFEu ||
      eb - 1 >= 0x7FFEu) {
    // A zero operand (the other zero or as above) is exact, unless it is
    // added to a value with more than kPrec bits.
    bool za = !ea && !a.sig, zb = !eb && !b.sig;
    if ((za || ((a.sig & kJ) && !(a.sig & kLow11) && ea - 1 < 0x7FFEu)) &&
        (zb || ((b.sig & kJ) && !(b.sig & kLow11) && eb - 1 < 0x7FFEu)) && (za || zb)) {
      if (kPrec < 53 && (op == FOp::Add || op == FOp::Sub) &&
          ((a.sig | b.sig) & ((1ull << kDrop) - 1)))
        return false;
      return ZeroOp(op, rc, a, b, za, zb, out);
    }
    return false;
  }
  bool sa = (a.se & 0x8000u) != 0, sb = (b.se & 0x8000u) != 0, sign;
  uint64_t hi, lo;  // the exact result, normalised: value = hi:lo / 2^127 * 2^(exp - 0x3FFF)
  int32_t exp;
  if (op == FOp::Mul) {
    unsigned __int128 p = static_cast<unsigned __int128>(a.sig) * b.sig;
    hi = uint64_t(p >> 64);
    lo = uint64_t(p);
    sign = sa != sb;
    exp = int32_t(ea + eb) - 0x3FFF + 1;
    if (!(hi & kJ)) {  // product in [2^126, 2^127); its low 22 bits are zero
      hi = hi << 1 | lo >> 63;
      lo <<= 1;
      --exp;
    }
  } else if (op == FOp::Div) {
    // q = ma / mb * 2^63 (or 2^64 when ma < mb), in [2^63, 2^64); the
    // high half of the dividend is below mb, so the quotient fits.
    uint64_t ma = a.sig >> 11, mb = b.sig >> 11;
    bool shift = ma < mb;
    unsigned __int128 n = static_cast<unsigned __int128>(ma) << (63 + shift);
    hi = uint64_t(n / mb);
    lo = uint64_t(n) - hi * mb != 0;  // the remainder (below mb), as sticky
    sign = sa != sb;
    exp = int32_t(ea) - int32_t(eb) + 0x3FFF - shift;
  } else {
    bool sbx = op == FOp::Sub ? !sb : sb;
    bool swap = eb > ea || (eb == ea && b.sig > a.sig);
    uint64_t x = swap ? b.sig : a.sig, y = swap ? a.sig : b.sig;
    int32_t ex = int32_t(swap ? eb : ea), d = ex - int32_t(swap ? ea : eb);
    sign = swap ? sbx : sa;
    if (d > 64) return false;
    // y aligned below x: exact, and bit 0 of yl is clear (y's low 11 bits
    // are).
    uint64_t yh = d == 64 ? 0 : y >> d;
    uint64_t yl = d == 0 ? 0 : y << (64 - d);
    exp = ex;
    if (sa != sbx) {
      lo = 0 - yl;
      hi = x - yh - (yl != 0);
      // With d >= 1, hi >= 2^62; with d = 0, lo = 0. So hi = 0 only for an
      // exact zero: -0 when rounding down, else +0.
      if (!hi) {
        *out = {0, rc == 1 ? 0x8000u : 0u};
        return true;
      }
      int lz = __builtin_clzll(hi);
      if (lz) {
        hi = hi << lz | lo >> (64 - lz);
        lo <<= lz;
        exp -= lz;
      }
    } else {
      lo = yl;
      hi = x + yh;
      if (hi < x) {  // carry out: shift right one (the bit lost is zero)
        lo = lo >> 1 | hi << 63;
        hi = hi >> 1 | kJ;
        ++exp;
      }
    }
  }
  if (exp < 1) return false;
  // rest: the dropped bits as a fraction of 2^64; lo is sticky below it.
  uint64_t keep = hi >> kDrop, rest = hi << kPrec;
  bool inexact = rest | lo;
  bool inc = rc == 0 ? rest > kJ || (rest == kJ && (lo || (keep & 1)))
           : rc == 1 ? sign && inexact
           : rc == 2 ? !sign && inexact
                     : false;
  if (inc && ++keep == (1ull << kPrec)) {
    keep >>= 1;
    ++exp;
  }
  if (exp > 0x7FFE) return false;
  *out = {keep << kDrop, uint64_t(sign ? 0x8000u : 0u) | uint64_t(exp) |
                             (inexact ? uint64_t(0x20) << 16 : 0) | uint64_t(inc) << 32};
  return true;
}

template <int kPrec>
[[gnu::always_inline]] inline bool FastWideOp(int rc, Op op, V dst, V src, RV* out) {
  switch (op) {
    case Op::Add: return FastWide<kPrec>(FOp::Add, rc, dst, src, out);
    case Op::Mul: return FastWide<kPrec>(FOp::Mul, rc, dst, src, out);
    case Op::Sub: return FastWide<kPrec>(FOp::Sub, rc, dst, src, out);
    case Op::SubR: return FastWide<kPrec>(FOp::Sub, rc, src, dst, out);
    case Op::Div: return FastWide<kPrec>(FOp::Div, rc, dst, src, out);
    case Op::DivR: return FastWide<kPrec>(FOp::Div, rc, src, dst, out);
  }
  return false;
}

// The fast cores for control word cw: PC=24 (with kWide also for 53-bit
// operands, which m64 loads give) and PC=53.
template <bool kWide = false>
[[gnu::always_inline]] inline bool FastOp(uint32_t cw, Op op, V dst, V src, RV* out) {
  const int rc = int(cw >> 10) & 3;
  if ((cw & 0x0300) == 0)
    return Fast24Op(rc, op, dst, src, out) || (kWide && FastWideOp<24>(rc, op, dst, src, out));
  if ((cw & 0x0300) == 0x0200) return FastWideOp<53>(rc, op, dst, src, out);
  return false;
}

// Normal m32/m64 values as registers (exact, no flags).
[[gnu::always_inline]] inline V FromNormalF32(uint32_t bits) {
  return {(uint64_t(bits & 0x7FFFFF) | 0x800000) << 40,
          ((bits >> 31) << 15) | (((bits >> 23) & 0xFF) - 127 + 0x3FFF)};
}
[[gnu::always_inline]] inline V FromNormalF64(uint64_t bits) {
  return {((bits & 0xFFFFFFFFFFFFFull) | (1ull << 52)) << 11,
          ((bits >> 63) << 15) | (((bits >> 52) & 0x7FF) - 1023 + 0x3FFF)};
}
// Zero or normal (exponent field neither all zeros nor all ones).
[[gnu::always_inline]] inline bool OrdinaryF32(uint32_t bits) {
  uint32_t e = (bits >> 23) & 0xFF;
  return e - 1 < 254 || !(bits & 0x7FFFFFFF);
}
[[gnu::always_inline]] inline bool OrdinaryF64(uint64_t bits) {
  uint64_t e = (bits >> 52) & 0x7FF;
  return e - 1 < 2046 || !(bits << 1);
}
[[gnu::always_inline]] inline V FromOrdinaryF32(uint32_t bits) {
  return (bits & 0x7FFFFFFF) ? FromNormalF32(bits) : V{0, (bits >> 31) << 15};
}
[[gnu::always_inline]] inline V FromOrdinaryF64(uint64_t bits) {
  return (bits << 1) ? FromNormalF64(bits) : V{0, (bits >> 63) << 15};
}

// What lifted runs call: the fast paths inline, the rest out of line.
[[gnu::always_inline]] inline RV ArithFast(uint32_t cw, Op op, V dst, V src) {
  RV r;
  if (FastOp(cw, op, dst, src, &r)) return r;
  return Arith(cw, op, dst, src);
}
[[gnu::always_inline]] inline RV ArithF32Fast(uint32_t cw, Op op, V st0, uint32_t bits) {
  RV r;
  // A zero or normal m32 is exact and raises no DE.
  if (OrdinaryF32(bits) && FastOp(cw, op, st0, FromOrdinaryF32(bits), &r)) return r;
  return ArithF32(cw, op, st0, bits);
}
[[gnu::always_inline]] inline RV ArithF64Fast(uint32_t cw, Op op, V st0, uint64_t bits) {
  RV r;
  if (OrdinaryF64(bits) && FastOp<true>(cw, op, st0, FromOrdinaryF64(bits), &r)) return r;
  return ArithF64(cw, op, st0, bits);
}
[[gnu::always_inline]] inline RV LoadF32Fast(uint32_t bits) {
  if (OrdinaryF32(bits)) {
    V v = FromOrdinaryF32(bits);
    return {v.sig, v.se};
  }
  return LoadF32(bits);
}
[[gnu::always_inline]] inline RV LoadF64Fast(uint64_t bits) {
  if (OrdinaryF64(bits)) {
    V v = FromOrdinaryF64(bits);
    return {v.sig, v.se};
  }
  return LoadF64(bits);
}

// FST m32/m64 of zero or of a normal value that rounds to a normal one,
// in integers: `mant` stored fraction bits, `bias`/`emax` of the format.
// False for the rest (overflow, underflow, specials). Shared with the
// model (x87.cpp, FastStore).
[[gnu::always_inline]] inline bool StoreCore(uint32_t cw, V v, int mant, int bias, int emax,
                                             SV* out) {
  const uint64_t sign = (v.se >> 15) & 1;
  const int width = mant + 1 + (bias == 127 ? 8 : 11);
  int32_t e = int32_t(v.se & 0x7FFF);
  if (e == 0 && v.sig == 0) {
    *out = {sign << (width - 1), 0};
    return true;
  }
  e += bias - 0x3FFF;
  if (!(v.sig >> 63) || e < 1 || e > emax || (v.se & 0x7FFF) == 0x7FFF) return false;
  const int drop = 63 - mant;
  uint64_t keep = v.sig >> drop, rest = v.sig << (64 - drop);
  const int rc = int(cw >> 10) & 3;
  bool inc = rc == 0 ? rest > (1ull << 63) || (rest == (1ull << 63) && (keep & 1))
           : rc == 1 ? sign && rest
           : rc == 2 ? !sign && rest
                     : false;
  if (inc && ++keep == (2ull << mant)) {
    keep >>= 1;
    if (++e > emax) return false;  // overflow
  }
  *out = {sign << (width - 1) | uint64_t(e) << mant | (keep & ((1ull << mant) - 1)),
          (rest ? 0x20u : 0u) | uint64_t(rest && inc) << 16};
  return true;
}
[[gnu::always_inline]] inline SV StoreF32Fast(uint32_t cw, V v) {
  SV r;
  if (StoreCore(cw, v, 23, 127, 254, &r)) return r;
  return StoreF32(cw, v);
}
[[gnu::always_inline]] inline SV StoreF64Fast(uint32_t cw, V v) {
  SV r;
  if (StoreCore(cw, v, 52, 1023, 2046, &r)) return r;
  return StoreF64(cw, v);
}

// FCOM/FUCOM of two zero or normal values: C3/C2/C0 alone (no flags can
// arise: no NaN, denormal or unsupported operand).
[[gnu::always_inline]] inline bool Ordinary(V v) {
  uint32_t e = v.se & 0x7FFFu;
  return e == 0 ? v.sig == 0 : e != 0x7FFF && (v.sig >> 63);
}
[[gnu::always_inline]] inline uint32_t CompareOrdinary(V a, V b) {
  uint32_t ea = a.se & 0x7FFFu, eb = b.se & 0x7FFFu;
  // Negative non-zero values; zeros of either sign are plain 0.
  bool na = ea && (a.se & 0x8000u), nb = eb && (b.se & 0x8000u);
  if (na != nb) return na ? 0x0100u : 0;
  if (ea == eb && a.sig == b.sig) return 0x4000u;
  bool less = ea < eb || (ea == eb && a.sig < b.sig);  // in magnitude
  return less != na ? 0x0100u : 0;
}
[[gnu::always_inline]] inline uint32_t CompareFast(V a, V b, bool quiet) {
  if (Ordinary(a) && Ordinary(b)) return CompareOrdinary(a, b);
  return Compare(a, b, quiet);
}
[[gnu::always_inline]] inline uint32_t CompareF32Fast(V a, uint32_t bits) {
  if (Ordinary(a) && OrdinaryF32(bits)) return CompareOrdinary(a, FromOrdinaryF32(bits));
  return CompareF32(a, bits);
}
[[gnu::always_inline]] inline uint32_t CompareF64Fast(V a, uint64_t bits) {
  if (Ordinary(a) && OrdinaryF64(bits)) return CompareOrdinary(a, FromOrdinaryF64(bits));
  return CompareF64(a, bits);
}

// --- host doubles (ARCHITECTURE.md, "x87 runs on host doubles") ---
//
// Under the control word games run most in, 0x027F (PC=53, RC=nearest,
// exceptions masked), a run keeps its registers in host doubles. That is
// exact as long as every value is zero or has a magnitude in [2^-511,
// 2^512) ("in range"); the proof is in ARCHITECTURE.md. In short:
// - x87 add/sub/mul/div/sqrt at PC=53 round the exact result once to 53
//   bits; binary64 rounds it once to 53 bits too, and both agree while the
//   exact result is in binary64's normal range. A result in range comes
//   from an exact result in that range; one out of range is redone
//   exactly. Nonzero operands in range cannot give an exact result that
//   rounds to zero, so a zero result is an exact zero (+0 under RC=nearest
//   on both, -0 for -0 + -0).
// - PE and C1 (rounded up in magnitude) come from the exact rounding error
//   e = exact - result: TwoSum for add/sub, fma(a, b, -p) for mul, the
//   remainder fma(-q, b, a) for div, fma(-s, s, a) for sqrt. With
//   operands and result in range these are representable and computed
//   exactly (no overflow, no underflow, no denormal).
// Each function writes its outputs only on success; false leaves
// everything untouched for the exact path to redo the operation. No
// denormal is ever an operand, result or error term (NaN and infinity
// only as rejected results), so FTZ/DAZ cannot matter; only
// round-to-nearest and masked traps are needed, the host default (never
// changed; every thread that runs guest code checks it,
// platform::HostFpProblem).

// Whether runs under control word cw use host doubles: PC=53 with
// RC=nearest (Guard has checked the exception masks).
[[gnu::always_inline]] inline bool DoubleMode(uint32_t cw) {
  return g_doubles && (cw & 0x0F00u) == 0x0200u;
}

// Zero, or a magnitude in [2^-511, 2^512): biased exponent 512..1534.
[[gnu::always_inline]] inline bool InRange(uint64_t bits) {
  return ((bits >> 52) & 0x7FF) - 512 < 1023 || !(bits << 1);
}
[[gnu::always_inline]] inline bool InRange(double x) {
  return InRange(std::bit_cast<uint64_t>(x));
}

// A register as a double: false unless it is in range with at most 53
// significand bits.
[[gnu::always_inline]] inline bool ToD(V v, double* out) {
  const uint64_t e = v.se & 0x7FFFu, sign = (v.se >> 15) & 1;
  if (!e && !v.sig) {
    *out = std::bit_cast<double>(sign << 63);
    return true;
  }
  if ((v.sig & 0x7FF) || !(v.sig >> 63) || e - (0x3FFF - 511) >= 1023) return false;
  *out = std::bit_cast<double>(sign << 63 | (e - 0x3FFF + 1023) << 52 |
                               ((v.sig >> 11) & ((1ull << 52) - 1)));
  return true;
}
// A double in range as a register (exact).
[[gnu::always_inline]] inline V FromD(double x) {
  return FromOrdinaryF64(std::bit_cast<uint64_t>(x));
}

// Operands and loads: m32 (zero or normal: in range, raises nothing),
// m64 (in range), FILD m64 (at most 53 bits). False for the others.
[[gnu::always_inline]] inline bool OperandF32(uint32_t bits, double* out) {
  if (!OrdinaryF32(bits)) return false;
  *out = double(std::bit_cast<float>(bits));
  return true;
}
[[gnu::always_inline]] inline bool OperandF64(uint64_t bits, double* out) {
  if (!InRange(bits)) return false;
  *out = std::bit_cast<double>(bits);
  return true;
}
// FCOM m64: any zero or normal value (a compare needs no range).
[[gnu::always_inline]] inline bool CompareOperandF64(uint64_t bits, double* out) {
  if (!OrdinaryF64(bits)) return false;
  *out = std::bit_cast<double>(bits);
  return true;
}
[[gnu::always_inline]] inline bool OperandI64(int64_t v, double* out) {
  if (uint64_t(v) + (1ull << 53) > (1ull << 54)) return false;
  *out = double(v);
  return true;
}

// Result r with rounding error e (exact - r, or for a quotient the
// remainder, whose sign relative to the dividend `ref` tells the
// direction): PE, and C1 when the magnitude was rounded up.
[[gnu::always_inline]] inline void NoteD(double e, double ref, uint32_t* fl, bool* up) {
  const uint64_t eb = std::bit_cast<uint64_t>(e);
  const bool inexact = (eb << 1) != 0;
  *fl |= inexact ? 0x20u : 0u;
  *up = inexact && ((eb ^ std::bit_cast<uint64_t>(ref)) >> 63);
}

// dst op src (Intel operand order), as Arith at PC=53/RC=nearest.
[[gnu::always_inline]] inline bool ArithD(Op op, double dst, double src, double* out,
                                          uint32_t* fl, bool* up) {
  double a = dst, b = src, r, e, ref;
  if (op == Op::SubR || op == Op::DivR) {
    a = src;
    b = dst;
  }
  switch (op) {
    case Op::Mul:
      r = a * b;
      e = std::fma(a, b, -r);
      ref = r;
      break;
    case Op::Div:
    case Op::DivR:
      r = a / b;  // b = 0 gives inf or NaN: out of range
      e = std::fma(-r, b, a);
      ref = a;
      break;
    default: {  // TwoSum (Knuth): r + e = a + b exactly
      if (op != Op::Add) b = -b;
      r = a + b;
      const double bv = r - a;
      e = (a - (r - bv)) + (b - bv);
      ref = r;
      break;
    }
  }
  if (!InRange(r)) return false;
  *out = r;
  NoteD(e, ref, fl, up);
  return true;
}

// FSQRT: a negative operand gives NaN, out of range.
[[gnu::always_inline]] inline bool SqrtD(double a, double* out, uint32_t* fl, bool* up) {
  const double r = std::sqrt(a);
  if (!InRange(r)) return false;
  *out = r;
  NoteD(std::fma(-r, r, a), r, fl, up);
  return true;
}

// FST m32: (float)x when x is zero or in float's normal range and does
// not round to infinity (exact: one rounding with RC=nearest); otherwise
// the exact store (underflow, overflow).
[[gnu::always_inline]] inline SV StoreF32D(uint32_t cw, double x) {
  const float f = float(x);
  const uint32_t fb = std::bit_cast<uint32_t>(f);
  const double m = std::fabs(x);
  if ((m >= 0x1p-126 && ((fb >> 23) & 0xFF) != 0xFF) || m == 0) {
    const double back = double(f);
    return {fb, (back != x ? 0x20u : 0u) | uint64_t(std::fabs(back) > m) << 16};
  }
  return StoreF32Fast(cw, FromD(x));
}
// FST m64 of a double in range: exact, no flags.
[[gnu::always_inline]] inline uint64_t StoreF64D(double x) { return std::bit_cast<uint64_t>(x); }

// FCOM family on doubles (no NaN can occur): C3/C2/C0 as CompareOrdinary.
[[gnu::always_inline]] inline uint32_t CompareD(double a, double b) {
  return a < b ? 0x0100u : a == b ? 0x4000u : 0u;
}
[[gnu::always_inline]] inline double AbsD(double x) { return std::fabs(x); }

// Tag of a non-empty register: 00 valid, 01 zero, 10 special.
inline uint32_t Tag(V v) {
  uint32_t e = v.se & 0x7FFFu;
  if (e == 0) return v.sig ? 2 : 1;
  if (e == 0x7FFF || !(v.sig >> 63)) return 2;
  return 0;
}

// The run guard. `valid`/`empty` are entry-relative masks (bit k for
// st(k) at entry, slots below the top as k & 7): registers the run reads
// before writing, and slots it pushes into.
// An entry-relative register mask as physical registers (TOP t).
inline uint32_t Rotl8(uint32_t m, uint32_t t) { return ((m << t) | (m >> (8 - t))) & 0xFF; }
inline bool Guard(const State& s, uint32_t valid, uint32_t empty) {
  if (!g_enabled || (s.cw & 0x3F) != 0x3F) return false;
  uint32_t t = s.top, e = s.empty, m = Rotl8(empty, t);
  return (e & Rotl8(valid, t)) == 0 && (e & m) == m;
}

// The status word FNSTSW reports for sticky `sw` (C bits included), TOP
// `top`, under control word `cw` (as StatusWord()).
inline uint16_t StatusWordOf(uint32_t sw, uint32_t top, uint32_t cw) {
  uint32_t r = (sw & ~0xB880u) | (top << 11);  // drop TOP, ES, B
  if (sw & ~cw & 0x3F) r |= 0x8080;
  return static_cast<uint16_t>(r);
}

}  // namespace run

// --- environment (32-bit protected-mode formats) ---------------------
void StoreEnv(State& s, uint8_t out[28]);       // FNSTENV, then masks all
void LoadEnv(State& s, const uint8_t in[28]);   // FLDENV
void Save(State& s, uint8_t out[108]);          // FNSAVE, then FNINIT
void Restore(State& s, const uint8_t in[108]);  // FRSTOR

}  // namespace recomp::x87
