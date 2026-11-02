// Oracle test for the integer semantics in src/runtime/lifted.h: every
// helper is run against this machine's CPU (inline asm, x86-64 hosts only;
// dev-time test, never shipped) with the same operands and input flags.
// Results must match bit for bit, and so must every flag the Intel manual
// defines for that instruction; flags it leaves undefined are masked.

#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <string>

#include "runtime/lifted.h"

using recomp::Flags;
namespace L = recomp::lift;

namespace {

constexpr uint32_t CF = 1u << 0, PF = 1u << 2, AF = 1u << 4, ZF = 1u << 6,
                   SF = 1u << 7, OF = 1u << 11;
constexpr uint32_t kAll = CF | PF | AF | ZF | SF | OF;

uint32_t Pack(const Flags& f) {
  return (f.cf ? CF : 0) | (f.pf ? PF : 0) | (f.af ? AF : 0) |
         (f.zf ? ZF : 0) | (f.sf ? SF : 0) | (f.of ? OF : 0);
}
Flags Unpack(uint32_t v) {
  Flags f{};
  f.cf = v & CF; f.pf = v & PF; f.af = v & AF;
  f.zf = v & ZF; f.sf = v & SF; f.of = v & OF;
  return f;
}

int g_fail = 0;
long g_checks = 0;
std::map<std::string, int> g_fail_by_op;

void Check(const char* op, int n, uint32_t a, uint32_t b, uint32_t fin,
           uint32_t want_r, uint32_t want_f, uint32_t got_r, uint32_t got_f,
           uint32_t mask) {
  g_checks++;
  if (want_r == got_r && (want_f & mask) == (got_f & mask)) return;
  g_fail_by_op[std::string(op) + "/" + std::to_string(n)]++;
  if (++g_fail <= 40)
    std::printf("FAIL %s/%d a=%08X b=%08X fin=%03X: hw r=%08X f=%03X, "
                "lift r=%08X f=%03X (mask %03X)\n",
                op, n, a, b, fin, want_r, want_f & mask, got_r, got_f & mask,
                mask);
}

// ---- native execution ------------------------------------------------------
// Each macro runs `insn` with the flags register preloaded from `fin` and
// returns the result and the resulting flags.

#define NATIVE2(name, insn32, insn16, insn8)                                 \
  uint32_t name(int n, uint32_t a, uint32_t b, uint64_t fin, uint64_t* fo) { \
    uint64_t r = a, fl;                                                      \
    if (n == 4)                                                              \
      asm volatile("push %[fi]\n popfq\n " insn32 " %k[b], %k[r]\n"          \
                   " pushfq\n pop %[fl]"                                     \
                   : [r] "+r"(r), [fl] "=&r"(fl)                             \
                   : [b] "r"(uint64_t(b)), [fi] "r"(fin) : "cc");            \
    else if (n == 2)                                                         \
      asm volatile("push %[fi]\n popfq\n " insn16 " %w[b], %w[r]\n"          \
                   " pushfq\n pop %[fl]"                                     \
                   : [r] "+r"(r), [fl] "=&r"(fl)                             \
                   : [b] "r"(uint64_t(b)), [fi] "r"(fin) : "cc");            \
    else                                                                     \
      asm volatile("push %[fi]\n popfq\n " insn8 " %b[b], %b[r]\n"           \
                   " pushfq\n pop %[fl]"                                     \
                   : [r] "+q"(r), [fl] "=&r"(fl)                             \
                   : [b] "q"(uint64_t(b)), [fi] "r"(fin) : "cc");            \
    *fo = fl;                                                                \
    return uint32_t(r) & (n == 4 ? 0xFFFFFFFFu : n == 2 ? 0xFFFFu : 0xFFu);  \
  }

NATIVE2(NAdd, "addl", "addw", "addb")
NATIVE2(NAdc, "adcl", "adcw", "adcb")
NATIVE2(NSub, "subl", "subw", "subb")
NATIVE2(NSbb, "sbbl", "sbbw", "sbbb")
NATIVE2(NAnd, "andl", "andw", "andb")
NATIVE2(NOr, "orl", "orw", "orb")
NATIVE2(NXor, "xorl", "xorw", "xorb")

#define NATIVE1(name, insn32, insn16, insn8)                                 \
  uint32_t name(int n, uint32_t a, uint64_t fin, uint64_t* fo) {             \
    uint64_t r = a, fl;                                                      \
    if (n == 4)                                                              \
      asm volatile("push %[fi]\n popfq\n " insn32 " %k[r]\n pushfq\n"        \
                   " pop %[fl]" : [r] "+r"(r), [fl] "=&r"(fl)                \
                   : [fi] "r"(fin) : "cc");                                  \
    else if (n == 2)                                                         \
      asm volatile("push %[fi]\n popfq\n " insn16 " %w[r]\n pushfq\n"        \
                   " pop %[fl]" : [r] "+r"(r), [fl] "=&r"(fl)                \
                   : [fi] "r"(fin) : "cc");                                  \
    else                                                                     \
      asm volatile("push %[fi]\n popfq\n " insn8 " %b[r]\n pushfq\n"         \
                   " pop %[fl]" : [r] "+q"(r), [fl] "=&r"(fl)                \
                   : [fi] "r"(fin) : "cc");                                  \
    *fo = fl;                                                                \
    return uint32_t(r) & (n == 4 ? 0xFFFFFFFFu : n == 2 ? 0xFFFFu : 0xFFu);  \
  }

NATIVE1(NInc, "incl", "incw", "incb")
NATIVE1(NDec, "decl", "decw", "decb")
NATIVE1(NNeg, "negl", "negw", "negb")

// Shifts/rotates by CL.
#define NATIVESH(name, insn32, insn16, insn8)                                \
  uint32_t name(int n, uint32_t a, uint32_t cnt, uint64_t fin, uint64_t* fo) { \
    uint64_t r = a, fl;                                                      \
    uint64_t cl = cnt;                                                       \
    if (n == 4)                                                              \
      asm volatile("push %[fi]\n popfq\n " insn32 " %%cl, %k[r]\n pushfq\n"  \
                   " pop %[fl]" : [r] "+r"(r), [fl] "=&r"(fl)                \
                   : "c"(cl), [fi] "r"(fin) : "cc");                         \
    else if (n == 2)                                                         \
      asm volatile("push %[fi]\n popfq\n " insn16 " %%cl, %w[r]\n pushfq\n"  \
                   " pop %[fl]" : [r] "+r"(r), [fl] "=&r"(fl)                \
                   : "c"(cl), [fi] "r"(fin) : "cc");                         \
    else                                                                     \
      asm volatile("push %[fi]\n popfq\n " insn8 " %%cl, %b[r]\n pushfq\n"   \
                   " pop %[fl]" : [r] "+q"(r), [fl] "=&r"(fl)                \
                   : "c"(cl), [fi] "r"(fin) : "cc");                         \
    *fo = fl;                                                                \
    return uint32_t(r) & (n == 4 ? 0xFFFFFFFFu : n == 2 ? 0xFFFFu : 0xFFu);  \
  }

NATIVESH(NShl, "shll", "shlw", "shlb")
NATIVESH(NShr, "shrl", "shrw", "shrb")
NATIVESH(NSar, "sarl", "sarw", "sarb")
NATIVESH(NRol, "roll", "rolw", "rolb")
NATIVESH(NRor, "rorl", "rorw", "rorb")
NATIVESH(NRcl, "rcll", "rclw", "rclb")
NATIVESH(NRcr, "rcrl", "rcrw", "rcrb")

uint32_t NShld(int n, uint32_t a, uint32_t b, uint32_t cnt, uint64_t fin,
               uint64_t* fo) {
  uint64_t r = a, fl, cl = cnt;
  if (n == 4)
    asm volatile("push %[fi]\n popfq\n shldl %%cl, %k[b], %k[r]\n pushfq\n pop %[fl]"
                 : [r] "+r"(r), [fl] "=&r"(fl) : [b] "r"(uint64_t(b)), "c"(cl), [fi] "r"(fin) : "cc");
  else
    asm volatile("push %[fi]\n popfq\n shldw %%cl, %w[b], %w[r]\n pushfq\n pop %[fl]"
                 : [r] "+r"(r), [fl] "=&r"(fl) : [b] "r"(uint64_t(b)), "c"(cl), [fi] "r"(fin) : "cc");
  *fo = fl;
  return uint32_t(r) & (n == 4 ? 0xFFFFFFFFu : 0xFFFFu);
}
uint32_t NShrd(int n, uint32_t a, uint32_t b, uint32_t cnt, uint64_t fin,
               uint64_t* fo) {
  uint64_t r = a, fl, cl = cnt;
  if (n == 4)
    asm volatile("push %[fi]\n popfq\n shrdl %%cl, %k[b], %k[r]\n pushfq\n pop %[fl]"
                 : [r] "+r"(r), [fl] "=&r"(fl) : [b] "r"(uint64_t(b)), "c"(cl), [fi] "r"(fin) : "cc");
  else
    asm volatile("push %[fi]\n popfq\n shrdw %%cl, %w[b], %w[r]\n pushfq\n pop %[fl]"
                 : [r] "+r"(r), [fl] "=&r"(fl) : [b] "r"(uint64_t(b)), "c"(cl), [fi] "r"(fin) : "cc");
  *fo = fl;
  return uint32_t(r) & (n == 4 ? 0xFFFFFFFFu : 0xFFFFu);
}

uint32_t NImul2(int n, uint32_t a, uint32_t b, uint64_t* fo) {
  uint64_t r = a, fl;
  if (n == 4)
    asm volatile("imull %k[b], %k[r]\n pushfq\n pop %[fl]"
                 : [r] "+r"(r), [fl] "=&r"(fl) : [b] "r"(uint64_t(b)) : "cc");
  else
    asm volatile("imulw %w[b], %w[r]\n pushfq\n pop %[fl]"
                 : [r] "+r"(r), [fl] "=&r"(fl) : [b] "r"(uint64_t(b)) : "cc");
  *fo = fl;
  return uint32_t(r) & (n == 4 ? 0xFFFFFFFFu : 0xFFFFu);
}

// One-operand MUL/IMUL, 32-bit: edx:eax = eax * b.
void NMul1(bool sign, uint32_t a, uint32_t b, uint32_t* lo, uint32_t* hi,
           uint64_t* fo) {
  uint32_t eax = a, edx;
  uint64_t fl;
  if (sign)
    asm volatile("imull %[b]\n pushfq\n pop %[fl]"
                 : "+a"(eax), "=d"(edx), [fl] "=&r"(fl) : [b] "r"(b) : "cc");
  else
    asm volatile("mull %[b]\n pushfq\n pop %[fl]"
                 : "+a"(eax), "=d"(edx), [fl] "=&r"(fl) : [b] "r"(b) : "cc");
  *lo = eax;
  *hi = edx;
  *fo = fl;
}

// ---- drivers ---------------------------------------------------------------

std::mt19937_64 rng(12345);

uint32_t Interesting(int n) {
  static const uint32_t k[] = {0, 1, 2, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF,
                               0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF,
                               0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x0F,
                               0x10, 0xF0, 0x55555555, 0xAAAAAAAA};
  uint32_t mask = n == 4 ? 0xFFFFFFFFu : n == 2 ? 0xFFFFu : 0xFFu;
  if (rng() % 3 == 0) return k[rng() % (sizeof k / sizeof *k)] & mask;
  return uint32_t(rng()) & mask;
}

template <int N>
void TestWidth() {
  for (int it = 0; it < 200000; it++) {
    uint32_t a = Interesting(N), b = Interesting(N);
    uint64_t fin = 0x202 | (rng() & kAll);  // random input flags
    uint64_t fo;
    Flags f;
    uint32_t r;

#define BIN(label, native, call, mask)                                   \
    f = Unpack(uint32_t(fin));                                           \
    r = call;                                                            \
    Check(label, N, a, b, uint32_t(fin), native(N, a, b, fin, &fo),      \
          uint32_t(fo), r, Pack(f), mask);

    BIN("add", NAdd, L::Add<N>(f, a, b), kAll)
    BIN("adc", NAdc, L::Adc<N>(f, a, b), kAll)
    BIN("sub", NSub, L::Sub<N>(f, a, b), kAll)
    BIN("sbb", NSbb, L::Sbb<N>(f, a, b), kAll)
    BIN("and", NAnd, L::Logic<N>(f, a & b), kAll & ~AF)
    BIN("or", NOr, L::Logic<N>(f, a | b), kAll & ~AF)
    BIN("xor", NXor, L::Logic<N>(f, a ^ b), kAll & ~AF)

#define UN(label, native, call)                                          \
    f = Unpack(uint32_t(fin));                                           \
    r = call;                                                            \
    Check(label, N, a, 0, uint32_t(fin), native(N, a, fin, &fo),         \
          uint32_t(fo), r, Pack(f), kAll);

    UN("inc", NInc, L::Inc<N>(f, a))
    UN("dec", NDec, L::Dec<N>(f, a))
    UN("neg", NNeg, L::Neg<N>(f, a))

    // Shifts: count 0..33; flags undefined per Intel: AF always; OF unless
    // count == 1; for counts >= width, CF too (shl/shr).
    uint32_t cnt = uint32_t(rng() % 34);
    uint32_t mc = cnt & 31;
    uint32_t smask = mc == 0 ? kAll : (kAll & ~AF & ~(mc == 1 ? 0u : OF));
    if (mc >= uint32_t(N * 8)) smask &= ~CF;
#define SH(label, native, call, mask)                                    \
    f = Unpack(uint32_t(fin));                                           \
    r = call;                                                            \
    Check(label, N, a, cnt, uint32_t(fin), native(N, a, cnt, fin, &fo),  \
          uint32_t(fo), r, Pack(f), mask);
    SH("shl", NShl, L::Shl<N>(f, a, cnt), smask)
    SH("shr", NShr, L::Shr<N>(f, a, cnt), smask)
    SH("sar", NSar, L::Sar<N>(f, a, cnt), mc ? (kAll & ~AF & ~(mc == 1 ? 0u : OF)) : kAll)
    // Rotates touch only CF and OF; OF defined for count 1 only.
    uint32_t rmask = mc <= 1 ? kAll : kAll & ~OF;
    SH("rol", NRol, L::Rol<N>(f, a, cnt), rmask)
    SH("ror", NRor, L::Ror<N>(f, a, cnt), rmask)
    SH("rcl", NRcl, L::Rcl<N>(f, a, cnt), rmask)
    SH("rcr", NRcr, L::Rcr<N>(f, a, cnt), rmask)

    if constexpr (N >= 2) {
      // SHLD/SHRD: count masked to 5 bits; for 16-bit, counts > 16 are
      // undefined, skip them. AF undefined; OF only for count 1.
      if (N == 4 || mc <= 16) {
        uint32_t dmask = mc == 0 ? kAll : (kAll & ~AF & ~(mc == 1 ? 0u : OF));
        f = Unpack(uint32_t(fin));
        r = L::Shld<N>(f, a, b, cnt);
        Check("shld", N, a, b, uint32_t(fin), NShld(N, a, b, cnt, fin, &fo),
              uint32_t(fo), r, Pack(f), dmask);
        f = Unpack(uint32_t(fin));
        r = L::Shrd<N>(f, a, b, cnt);
        Check("shrd", N, a, b, uint32_t(fin), NShrd(N, a, b, cnt, fin, &fo),
              uint32_t(fo), r, Pack(f), dmask);
      }
      // IMUL r, r/m: only CF/OF defined.
      f = Unpack(uint32_t(fin));
      r = L::Imul2<N>(f, a, b);
      Check("imul2", N, a, b, 0, NImul2(N, a, b, &fo), uint32_t(fo), r,
            Pack(f), CF | OF);
    }
    if constexpr (N == 4) {
      for (bool sign : {false, true}) {
        uint32_t lo, hi, glo, ghi;
        NMul1(sign, a, b, &lo, &hi, &fo);
        f = Unpack(uint32_t(fin));
        if (sign) L::Imul1<4>(f, a, b, glo, ghi);
        else L::Mul1<4>(f, a, b, glo, ghi);
        Check(sign ? "imul1.lo" : "mul1.lo", 4, a, b, 0, lo, uint32_t(fo), glo,
              Pack(f), CF | OF);
        Check(sign ? "imul1.hi" : "mul1.hi", 4, a, b, 0, hi, 0, ghi, 0, 0);
      }
    }
  }
}

// Division: compare quotient/remainder against host arithmetic, including
// the #DE cases.
void TestDiv() {
  for (int it = 0; it < 200000; it++) {
    uint32_t lo = uint32_t(rng()), hi = uint32_t(rng()) % 4 ? 0 : uint32_t(rng());
    uint32_t d = Interesting(4);
    {
      uint32_t l = lo, h = hi;
      bool ok = L::Div<4>(l, h, d);
      uint64_t n = (uint64_t(hi) << 32) | lo;
      bool want_ok = d && n / d <= 0xFFFFFFFFull;
      g_checks++;
      if (ok != want_ok || (ok && (l != uint32_t(n / d) || h != uint32_t(n % d)))) {
        if (++g_fail <= 40)
          std::printf("FAIL div %08X:%08X / %08X\n", hi, lo, d);
      }
    }
    {
      uint32_t h0 = uint32_t(int32_t(lo) >> 31);
      if (rng() % 4 == 0) h0 = uint32_t(rng());
      uint32_t l = lo, h = h0;
      bool ok = L::Idiv<4>(l, h, d);
      int64_t n = int64_t((uint64_t(h0) << 32) | lo);
      int64_t dv = int32_t(d);
      bool want_ok = dv != 0 && !(n == INT64_MIN && dv == -1);
      int64_t q = want_ok ? n / dv : 0;
      if (want_ok && (q > INT32_MAX || q < INT32_MIN)) want_ok = false;
      g_checks++;
      if (ok != want_ok ||
          (ok && (l != uint32_t(int32_t(q)) || h != uint32_t(int32_t(n % dv))))) {
        if (++g_fail <= 40)
          std::printf("FAIL idiv %08X:%08X / %08X\n", h0, lo, d);
      }
    }
  }
}

}  // namespace

int main() {
  TestWidth<1>();
  TestWidth<2>();
  TestWidth<4>();
  TestDiv();
  for (const auto& [op, n] : g_fail_by_op)
    std::printf("  %-10s %d failures\n", op.c_str(), n);
  std::printf("lift_semantics_test: %ld checks, %d failures\n", g_checks,
              g_fail);
  return g_fail ? 1 : 0;
}
