// sse::RsqrtLane (RSQRTPS) is the correctly rounded 1/sqrt(x) (see sse.h).
//
// - Every positive normal significand, at an even and an odd exponent
//   (1 <= x < 4: the rest follows from rsqrt(4x) = rsqrt(x) / 2): the
//   result is checked against the two midpoints around it with exact
//   rational arithmetic, written independently of the model's own check
//   (here: squares of the midpoints times x against 1, as 256-bit
//   products in two 128-bit halves).
// - The scaling identity at every exponent for a sample of significands.
// - The architectural error bound, |rel. error| <= 1.5 * 2^-12.
// - Special operands: ±0, denormals, negatives, ±inf, NaNs.

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "runtime/sse.h"

namespace recomp {
void FatalF(const char* fmt, ...) {  // runtime/sse.cpp's #XM
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::abort();
}
}  // namespace recomp

namespace {

using u128 = unsigned __int128;

int g_fail = 0;

void Fail(const char* what, uint32_t x, uint32_t got) {
  if (++g_fail <= 20) std::printf("FAIL %s: x=%08X got %08X\n", what, x, got);
}

float F(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// Compares a * 2^ea with b * 2^eb for a, b < 2^127: -1, 0, 1.
int Cmp(u128 a, int ea, u128 b, int eb) {
  while (ea > eb && !(a >> 126)) a <<= 1, ea--;
  while (eb > ea && !(b >> 126)) b <<= 1, eb--;
  if (ea != eb) return ea > eb ? 1 : -1;  // both normalised high: magnitude by exponent
  return a < b ? -1 : a > b ? 1 : 0;
}

// True if the positive normal float r is the float nearest to 1/sqrt(x):
// m_lo^2 * x < 1 < m_hi^2 * x for the midpoints m_lo, m_hi around r.
bool Nearest(uint32_t x, uint32_t r) {
  uint32_t rexp = r >> 23, rm = (r & 0x7FFFFF) | 0x800000;
  uint32_t xexp = x >> 23, xm = (x & 0x7FFFFF) | 0x800000;
  int er = int(rexp) - 150, ex = int(xexp) - 150;
  // Midpoints in units of 2^(er-1): 2*rm -/+ 1, except below a power of
  // two, where the neighbour has half the ulp: units of 2^(er-2), 4*rm - 1.
  u128 lo, hi;
  int elo, ehi;
  if (rm == 0x800000) {
    lo = 4 * u128(rm) - 1;
    elo = er - 2;
  } else {
    lo = 2 * u128(rm) - 1;
    elo = er - 1;
  }
  hi = 2 * u128(rm) + 1;
  ehi = er - 1;
  // lo^2 * xm * 2^(2 elo + ex) < 1 and hi^2 * xm * 2^(2 ehi + ex) > 1.
  bool below = Cmp(lo * lo * xm, 2 * elo + ex, 1, 0) < 0;
  bool above = Cmp(hi * hi * xm, 2 * ehi + ex, 1, 0) > 0;
  return below && above;
}

}  // namespace

int main() {
  using recomp::sse::RsqrtLane;

  // Exhaustive over 1 <= x < 4.
  uint64_t n = 0;
  double worst = 0;
  for (uint32_t e = 127; e <= 128; e++)
    for (uint32_t m = 0; m < 0x800000; m++) {
      uint32_t x = (e << 23) | m;
      uint32_t r = RsqrtLane(x);
      if ((r >> 23) == 0 || (r >> 23) >= 0xFF || !Nearest(x, r)) Fail("nearest", x, r);
      // The check itself rejects both neighbours.
      if (Nearest(x, r - 1) || Nearest(x, r + 1)) Fail("check rejects neighbours", x, r);
      double rel = std::fabs(double(F(r)) * std::sqrt(double(F(x))) - 1.0);
      if (rel > worst) worst = rel;
      n++;
    }
  if (worst > 1.5 / 4096) Fail("error bound", 0, 0);

  // Scaling: rsqrt(x * 4^k) = rsqrt(x) * 2^-k over the whole normal range.
  for (uint32_t m = 0; m < 0x800000; m += 4093)
    for (uint32_t par = 0; par < 2; par++) {
      uint32_t base = RsqrtLane(((127 + par) << 23) | m);
      for (int k = -63; k <= 63; k++) {
        int e = 127 + int(par) + 2 * k;
        if (e < 1 || e > 254) continue;
        uint32_t x = (uint32_t(e) << 23) | m;
        uint32_t want = base - uint32_t(k << 23);
        uint32_t got = RsqrtLane(x);
        if (got != want) Fail("scaling", x, got);
      }
    }

  // Special operands.
  struct {
    uint32_t x, want;
  } special[] = {
      {0x00000000, 0x7F800000}, {0x80000000, 0xFF800000},  // ±0 -> ±inf
      {0x00000001, 0x7F800000}, {0x807FFFFF, 0xFF800000},  // denormals as ±0
      {0x3F800000, 0x3F800000},                            // 1 -> 1
      {0x40800000, 0x3F000000},                            // 4 -> 0.5
      {0xBF800000, 0xFFC00000}, {0xFF800000, 0xFFC00000},  // negative -> default NaN
      {0x7F800000, 0x00000000},                            // +inf -> +0
      {0x7FC00001, 0x7FC00001}, {0x7F800001, 0x7FC00001},  // QNaN, SNaN quieted
      {0xFF800001, 0xFFC00001},
  };
  for (const auto& s : special) {
    uint32_t got = RsqrtLane(s.x);
    if (got != s.want) Fail("special", s.x, got);
  }

  std::printf("rsqrt: %llu significands, worst rel. error %.3g (bound %.3g), %d failures\n",
              (unsigned long long)n, worst, 1.5 / 4096, g_fail);
  return g_fail ? 1 : 0;
}
