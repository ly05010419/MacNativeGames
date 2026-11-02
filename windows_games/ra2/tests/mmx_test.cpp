// runtime/mmx.h against hand-computed values (Intel SDM Vol. 1 §9.5, Vol. 2
// per mnemonic). Every expected value below is worked out by hand in the
// comment above it, never taken from the implementation.
//
//   clang++ -std=c++20 -I third_party/x86recomp/src tests/mmx_test.cpp -o build/mmx_test && build/mmx_test

#include <cinttypes>
#include <cstdio>

#include "runtime/mmx.h"

namespace {

int g_failures = 0;

void Check(const char* what, uint64_t got, uint64_t want) {
  if (got != want) {
    std::printf("FAIL %s: got %016" PRIX64 " want %016" PRIX64 "\n", what, got, want);
    g_failures++;
  }
}

void Lanes() {
  using namespace recomp::mmx;
  // PSRLW 1: words FFFF,0001,7FFF,8000 -> 7FFF,0000,3FFF,4000.
  Check("psrlw 1", Psrlw(0x80007FFF0001FFFFull, 1), 0x40003FFF00007FFFull);
  // PSRLW 16: count > 15 clears every word.
  Check("psrlw 16", Psrlw(0x80007FFF0001FFFFull, 16), 0);
  // PSRLW with a 64-bit count whose low bits are small but high bits set.
  Check("psrlw big", Psrlw(0xFFFFull, 0x100000001ull), 0);
  // PSRAD 4: 80000000 -> F8000000 (sign fill), 7FFFFFFF -> 07FFFFFF.
  Check("psrad 4", Psrad(0x800000007FFFFFFFull, 4), 0xF800000007FFFFFFull);
  // PSRAD 40: count > 31 fills with the sign: FFFFFFFF, 00000000.
  Check("psrad 40", Psrad(0x800000007FFFFFFFull, 40), 0xFFFFFFFF00000000ull);
  // PUNPCKLBW: d low bytes 11 22 33 44, s low bytes AA BB CC DD ->
  // 11 AA 22 BB 33 CC 44 DD (byte 0 first).
  Check("punpcklbw", Punpcklbw(0x9999999944332211ull, 0x77777777DDCCBBAAull),
        0xDD44CC33BB22AA11ull);
  // PUNPCKHWD: d words 1111 2222 3333 4444, s AAAA BBBB CCCC DDDD ->
  // 3333 CCCC 4444 DDDD (word 0 first).
  Check("punpckhwd", Punpckhwd(0x4444333322221111ull, 0xDDDDCCCCBBBBAAAAull),
        0xDDDD4444CCCC3333ull);
  // PACKUSWB: d words 0100(256) 00FF FFFF(-1) 0080 -> FF FF 00 80;
  // s words 8000(-32768) 7FFF 0001 0000 -> 00 FF 01 00.
  Check("packuswb", Packuswb(0x0080FFFF00FF0100ull, 0x000000017FFF8000ull),
        0x0001FF008000FFFFull);
  // PACKSSDW: d dwords FFFF7FFF(-32769) 00008000(32768) -> 8000 7FFF;
  // s dwords 00000005 FFFFFFFF(-1) -> 0005 FFFF.
  Check("packssdw", Packssdw(0x00008000FFFF7FFFull, 0xFFFFFFFF00000005ull),
        0xFFFF00057FFF8000ull);
  // Word products: 7FFF*7FFF = 3FFF0001, 8000*8000 = 40000000,
  // FFFF*0002 = -2 = FFFFFFFE, 0100*0100 = 00010000.
  Check("pmulhw", Pmulhw(0x0100FFFF80007FFFull, 0x0100000280007FFFull), 0x0001FFFF40003FFFull);
  Check("pmullw", Pmullw(0x0100FFFF80007FFFull, 0x0100000280007FFFull), 0x0000FFFE00000001ull);
  // PADDUSB: 80+90 = 110 -> FF, FF+01 = 100 -> FF, 10+20 = 30.
  Check("paddusb", Paddusb(0x10FF80ull, 0x200190ull), 0x30FFFFull);
}

void State() {
  using namespace recomp;
  x87::State s{};
  s.top = 5;
  s.empty = 0xFF;
  s.r[3].se = 0x3FFF;
  mmx::Enter(s);
  // Enter: TOP = 0 and all eight registers valid (SDM Vol. 1 §9.5.1).
  Check("enter top", s.top, 0);
  Check("enter empty", s.empty, 0);
  mmx::Set(s, 3, 0x0123456789ABCDEFull);
  // An MMX write sets bits 79-64 of the register to all ones.
  Check("set sig", s.r[3].sig, 0x0123456789ABCDEFull);
  Check("set se", s.r[3].se, 0xFFFF);
  Check("get", mmx::Get(s, 3), 0x0123456789ABCDEFull);
  mmx::Emms(s);
  // EMMS: all eight tags empty (11).
  Check("emms", s.empty, 0xFF);
}

}  // namespace

int main() {
  Lanes();
  State();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("mmx_test: all passed\n");
  return 0;
}
