// End-to-end oracle test for x87 lifting: every distinct x87 instruction
// form in the game EXE (lifted by `recomp_lift --x87-test`) is run as
// lifted C++ on the x87 model and natively on this machine's FPU, from the
// same random state (control word, stack, memory operand). Results must
// match bit for bit: control/status/tag words, all registers, memory, and
// for fcomi/fnstsw the integer side effects. x86-64 hosts only; dev-time.
//
// Transcendentals (fsin, fptan, ...) are known to differ from hardware in
// the last bit for some inputs; for them a 1-ulp register difference is
// counted separately and does not fail the test (nor does C1, which
// reports the rounding direction of that last bit).

#include <sys/mman.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "lift_x87_forms.h"
#include "runtime/guest_memory.h"
#include "runtime/lifted.h"
#include "runtime/x87.h"

namespace recomp {
uint8_t* g_guest_base = nullptr;
// Referenced by lifted code that never runs here.
void Unimplemented(Ctx&, uint32_t va, const char* what) {
  std::printf("unimplemented %08X %s\n", va, what);
  std::abort();
}
void BadJump(Ctx&, uint32_t from, uint32_t to) {
  std::printf("bad jump %08X -> %08X\n", from, to);
  std::abort();
}
}  // namespace recomp

namespace {

constexpr uint32_t kMemVa = 0x00200000;    // operand at [eax]
constexpr uint32_t kStackVa = 0x00300000;  // guest stack (ret pops 4)

using recomp::x87::F80;
using NativeFn = void (*)(void* mem, const void* in, void* out, uint64_t* flags,
                          uint64_t* rax);

// mov r9,rcx; mov r10,r8; mov r11,rdx; mov rax,rdi; frstor [rsi];
// <insn>; pushfq; pop rcx; mov [r9],rcx; mov [r10],rax; fnsave [r11]; ret
NativeFn MakeNative(uint8_t* page, const X87Form& f) {
  static const uint8_t pre[] = {0x49, 0x89, 0xC9, 0x4D, 0x89, 0xC2, 0x49, 0x89,
                                0xD3, 0x48, 0x89, 0xF8, 0xDD, 0x26};
  static const uint8_t post[] = {0x9C, 0x59, 0x49, 0x89, 0x09, 0x49, 0x89,
                                 0x02, 0x41, 0xDD, 0x33, 0xC3};
  uint8_t* p = page;
  std::memcpy(p, pre, sizeof pre);
  p += sizeof pre;
  std::memcpy(p, f.bytes, f.len);
  p += f.len;
  std::memcpy(p, post, sizeof post);
  return reinterpret_cast<NativeFn>(page);
}

F80 RandomValue(std::mt19937_64& g) {
  F80 v{};
  uint16_t sign = (g() & 1) ? 0x8000 : 0;
  switch (g() % 16) {
    case 0: return {0, sign};                                   // zero
    case 1: return {0x8000000000000000ull, uint16_t(sign | 0x7FFF)};  // inf
    case 2: return {0xC000000000000000ull | (g() >> 2), 0x7FFF};    // qnan
    case 3: return {g() >> 1, sign};                            // denormal
    case 4: {                                                   // small int
      int64_t n = int64_t(g() % 2001) - 1000;
      if (!n) return {0, sign};
      uint64_t a = uint64_t(n < 0 ? -n : n);
      int sh = __builtin_clzll(a);
      return {a << sh, uint16_t((n < 0 ? 0x8000 : 0) | (0x3FFF + 63 - sh))};
    }
    case 5:  // float-precision significand
      return {(g() | 0x8000000000000000ull) & 0xFFFFFF0000000000ull,
              uint16_t(sign | (0x3FFF + int(g() % 41) - 20))};
    case 6:  // double-precision significand
      return {(g() | 0x8000000000000000ull) & 0xFFFFFFFFFFFFF800ull,
              uint16_t(sign | (0x3FFF + int(g() % 81) - 40))};
    case 7:  // huge / tiny exponents
      return {g() | 0x8000000000000000ull,
              uint16_t(sign | ((g() & 1) ? 0x7F00 + g() % 0xFE : 1 + g() % 0x100))};
    default:
      v.sig = g() | 0x8000000000000000ull;
      v.se = uint16_t(sign | (0x3FFF + int(g() % 61) - 30));
      return v;
  }
}

// Tag of a non-empty register as FNSAVE writes it.
int Tag(const F80& v) {
  uint16_t e = v.se & 0x7FFF;
  if (e == 0) return v.sig == 0 ? 1 : 2;
  if (e == 0x7FFF || !(v.sig >> 63)) return 2;
  return 0;
}

const uint16_t kCws[] = {0x027F, 0x037F, 0x007F, 0x0C7F, 0x047F, 0x087F,
                         0x0E7F, 0x067F, 0x0A7F, 0x0F7F, 0x017F};

void RandomState(std::mt19937_64& g, uint8_t img[108]) {
  std::memset(img, 0, 108);
  uint16_t cw = kCws[g() % (sizeof kCws / sizeof *kCws)];
  int top = int(g() % 8);
  int depth = int(g() % 10);
  if (depth > 8) depth = 8 - int(g() % 3);  // bias towards some free slots
  uint16_t sw = uint16_t(top << 11) | uint16_t(g() & 0x4700);  // C3,C2,C1,C0
  uint16_t tw = 0xFFFF;
  // Under 0x027F, often registers a double holds exactly, so runs take
  // their host-double form (x87.h) and not only its exits.
  bool doubles = cw == 0x027F && (g() & 1);
  for (int i = 0; i < 8; i++) {  // i = ST(i)
    F80 v = RandomValue(g);
    if (doubles && (v.sig >> 63) && (v.se & 0x7FFF) != 0x7FFF && g() % 8) {
      v.sig &= ~0x7FFull;
      int e = int(v.se & 0x7FFF) - 0x3FFF;
      if (e < -300 || e > 300) v.se = uint16_t((v.se & 0x8000) | (0x3FFF + int(g() % 81) - 40));
    }
    std::memcpy(img + 28 + i * 10, &v.sig, 8);
    std::memcpy(img + 28 + i * 10 + 8, &v.se, 2);
    int phys = (top + i) & 7;
    if (i < depth) {
      tw = uint16_t(tw & ~(3 << (2 * phys))) | uint16_t(Tag(v) << (2 * phys));
    }
  }
  std::memcpy(img + 0, &cw, 2);
  std::memcpy(img + 4, &sw, 2);
  std::memcpy(img + 8, &tw, 2);
}

void RandomMem(std::mt19937_64& g, int bytes, uint8_t* m) {
  for (int i = 0; i < 16; i++) m[i] = uint8_t(g());
  if (bytes == 4 && (g() & 1)) {
    float f = float(std::ldexp(double(int64_t(g() % 2000001) - 1000000), int(g() % 30) - 15));
    std::memcpy(m, &f, 4);
  } else if (bytes == 8 && (g() & 1)) {
    double d = std::ldexp(double(int64_t(g() % 2000001) - 1000000), int(g() % 60) - 30);
    std::memcpy(m, &d, 8);
  } else if (bytes == 10 && (g() & 1)) {
    F80 v = RandomValue(g);
    std::memcpy(m, &v.sig, 8);
    std::memcpy(m + 8, &v.se, 2);
  } else if (bytes == 2 && (g() & 1)) {
    // Mostly sane control words for fldcw: masked exceptions.
    uint16_t cw = kCws[g() % (sizeof kCws / sizeof *kCws)];
    std::memcpy(m, &cw, 2);
  }
  if (bytes == 2 && (g() % 4)) {  // fldcw with unmasked exceptions would trap
    uint16_t v;
    std::memcpy(&v, m, 2);
    v |= 0x3F;
    std::memcpy(m, &v, 2);
  }
}

bool IsTranscendental(const std::string& t) {
  for (const char* n : {"fsin", "fcos", "fptan", "fpatan", "f2xm1", "fyl2x"})
    if (t.rfind(n, 0) == 0) return true;
  return false;
}

// Distance in units of the last place between two finite F80s with the
// same sign and exponent (else huge).
uint64_t Ulps(const uint8_t* a, const uint8_t* b) {
  uint64_t sa, sb;
  uint16_t ea, eb;
  std::memcpy(&sa, a, 8);
  std::memcpy(&sb, b, 8);
  std::memcpy(&ea, a + 8, 2);
  std::memcpy(&eb, b + 8, 2);
  if ((ea ^ eb) & 0x8000) return ~0ull;  // different signs
  // (exponent, significand) is monotonic for normal values, so a result
  // one ulp below a power of two counts as 1 ulp from it.
  auto key = [](uint16_t e, uint64_t s) {
    return (static_cast<unsigned __int128>(e & 0x7FFF) << 63) | (s & ~(1ull << 63));
  };
  if (!(sa >> 63) || !(sb >> 63)) {  // denormals/zeros: same exponent only
    if (ea != eb) return ~0ull;
    return sa > sb ? sa - sb : sb - sa;
  }
  unsigned __int128 ka = key(ea, sa), kb = key(eb, sb);
  unsigned __int128 d = ka > kb ? ka - kb : kb - ka;
  return d > ~0ull ? ~0ull : static_cast<uint64_t>(d);
}

}  // namespace

int main(int argc, char** argv) {
  int trials = argc > 1 ? std::atoi(argv[1]) : 20000;
  recomp::GuestMemory mem;
  if (!mem.Reserve()) return 1;
  recomp::g_guest_base = mem.base();
  mem.Commit(kMemVa, 0x1000, recomp::Prot::RW);
  mem.Commit(kStackVa - 0x1000, 0x2000, recomp::Prot::RW);
  auto* page = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (page == MAP_FAILED) return 1;

  std::mt19937_64 g(12345);
  int failed_forms = 0;
  long checks = 0, approx = 0;
  for (int fi = 0; fi < kX87FormCount; fi++) {
    const X87Form& f = kX87Forms[fi];
    std::string text = f.text;
    NativeFn native = MakeNative(page, f);
    bool transc = IsTranscendental(text);
    bool flags = text.find("comi") != std::string::npos;
    bool ax = text.find("fnstsw ax") != std::string::npos;
    int fails = 0;
    for (int t = 0; t < trials; t++) {
      uint8_t in[108], hw[108], lf[108], m0[16];
      RandomState(g, in);
      RandomMem(g, f.mem_bytes, m0);
      // Native.
      alignas(16) uint8_t hm[32] = {};
      std::memcpy(hm, m0, 16);
      uint64_t hflags = 0, hrax = 0;
      native(hm, in, hw, &hflags, &hrax);
      // Lifted.
      std::memcpy(mem.ptr(kMemVa), m0, 16);
      std::memset(mem.ptr(kMemVa) + 16, 0, 16);
      recomp::Ctx c{};
      recomp::x87::Restore(c.fpu, in);
      c.eax = kMemVa;
      c.esp = kStackVa;
      f.fn(c);
      recomp::x87::Save(c.fpu, lf);
      checks++;
      // Compare: CW, SW, TW, registers; memory; flags/ax where relevant.
      std::string why;
      if (std::memcmp(hw, lf, 2)) why += " cw";
      {
        // C1 reports the rounding direction; for transcendentals it moves
        // with the last-bit difference.
        uint16_t hs, ls;
        std::memcpy(&hs, hw + 4, 2);
        std::memcpy(&ls, lf + 4, 2);
        if (transc) hs &= ~0x0200, ls &= ~0x0200;
        if (hs != ls) why += " sw";
      }
      if (std::memcmp(hw + 8, lf + 8, 2)) why += " tw";
      for (int r = 0; r < 8; r++) {
        if (std::memcmp(hw + 28 + r * 10, lf + 28 + r * 10, 10) == 0) continue;
        if (transc && Ulps(hw + 28 + r * 10, lf + 28 + r * 10) <= 1) continue;
        why += " st" + std::to_string(r);
      }
      if (std::memcmp(hm, mem.ptr(kMemVa), 32)) why += " mem";
      if (flags) {
        uint32_t hf = uint32_t(hflags) & 0x45;  // CF PF ZF
        uint32_t lfl = c.eflags & 0x45;
        if (hf != lfl) why += " eflags";
      }
      if (ax && uint16_t(hrax) != uint16_t(c.eax)) why += " ax";
      if (why.empty()) {
        if (transc && std::memcmp(hw + 28, lf + 28, 80)) approx++;
        continue;
      }
      if (++fails <= 3) {
        std::printf("FAIL %-28s (form %d, e.g. %08X):%s\n", text.c_str(), fi,
                    f.example_va, why.c_str());
        auto dump = [](const char* who, const uint8_t* img) {
          uint16_t cw, sw, tw;
          std::memcpy(&cw, img, 2);
          std::memcpy(&sw, img + 4, 2);
          std::memcpy(&tw, img + 8, 2);
          std::printf("   %s cw=%04X sw=%04X tw=%04X", who, cw, sw, tw);
          for (int r = 0; r < 3; r++) {
            uint64_t s;
            uint16_t e;
            std::memcpy(&s, img + 28 + r * 10, 8);
            std::memcpy(&e, img + 28 + r * 10 + 8, 2);
            std::printf(" st%d=%04X:%016llX", r, e, (unsigned long long)s);
          }
          std::printf("\n");
        };
        dump("in  ", in);
        dump("hw  ", hw);
        dump("lift", lf);
        if (f.mem_bytes) {
          std::printf("   mem in ");
          for (int i = 0; i < f.mem_bytes; i++) std::printf("%02X", m0[i]);
          std::printf(" hw ");
          for (int i = 0; i < f.mem_bytes; i++) std::printf("%02X", hm[i]);
          std::printf(" lift ");
          for (int i = 0; i < f.mem_bytes; i++) std::printf("%02X", mem.ptr(kMemVa)[i]);
          std::printf("\n");
        }
      }
    }
    if (fails) {
      failed_forms++;
      std::printf("  -> %s: %d/%d trials differ\n", text.c_str(), fails, trials);
    }
  }
  std::printf("%d forms, %ld checks, %d forms failed; %ld transcendental "
              "results within 1 ulp\n",
              kX87FormCount, checks, failed_forms, approx);
  return failed_forms ? 1 : 0;
}
