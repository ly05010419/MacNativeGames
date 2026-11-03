// End-to-end oracle test for SSE/SSE2 lifting: every distinct SSE form in
// the game EXE (recomp_lift --sse-test) runs lifted (runtime/sse.h) and
// natively on this CPU from the same random general registers, flags, XMM
// registers, MXCSR (any rounding mode, random sticky flags; all exceptions
// masked, FTZ/DAZ off) and memory. XMM lanes and memory are drawn from
// float and double special values (zeros, infinities, QNaN/SNaN payloads,
// denormals, extremes, values near the int32 range) as well as random
// bits. Registers (except esp), flags, XMM registers, MXCSR and memory
// must match bit for bit. x86-64 hosts only; dev-time.

#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "lift_sse_forms.h"
#include "runtime/guest_memory.h"
#include "runtime/lifted.h"

namespace recomp {
uint8_t* g_guest_base = nullptr;
void Unimplemented(Ctx&, uint32_t va, const char* what) {
  std::printf("unimplemented %08X %s\n", va, what);
  std::abort();
}
void BadJump(Ctx&, uint32_t from, uint32_t to) {
  std::printf("bad jump %08X -> %08X\n", from, to);
  std::abort();
}
void FatalF(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::printf("\n");
  std::abort();
}
}  // namespace recomp

namespace {

constexpr uint32_t kBuf = 0x00200000, kBufSize = 0x4000;
constexpr uint32_t kStackVa = 0x00300000;

struct Regs {
  uint64_t r[8];       // 0x00 eax ecx edx ebx (esp) ebp esi edi
  uint64_t flags;      // 0x40
  uint32_t mxcsr;      // 0x48
  uint32_t host_mxcsr; // 0x4C: the test's own, restored after the form
  uint8_t xmm[8][16];  // 0x50
};
static_assert(offsetof(Regs, xmm) == 0x50);

using NativeFn = void (*)(Regs*);

void Put(uint8_t*& p, std::initializer_list<uint8_t> b) {
  for (uint8_t x : b) *p++ = x;
}
void PutDisp32(uint8_t*& p, uint32_t d) {
  std::memcpy(p, &d, 4);
  p += 4;
}

void Stub(uint8_t* p, const SseForm& f) {
  Put(p, {0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});  // push
  Put(p, {0x49, 0x89, 0xFF});                                            // mov r15,rdi
  Put(p, {0x41, 0x0F, 0xAE, 0x9F}); PutDisp32(p, 0x4C);  // stmxcsr [r15+4C]
  Put(p, {0x41, 0x0F, 0xAE, 0x97}); PutDisp32(p, 0x48);  // ldmxcsr [r15+48]
  for (int i = 0; i < 8; i++) {                          // movdqu xmmi, [..]
    Put(p, {0xF3, 0x41, 0x0F, 0x6F, uint8_t(0x87 | (i << 3))});
    PutDisp32(p, 0x50 + 16 * i);
  }
  Put(p, {0x49, 0x8B, 0x47, 0x00, 0x49, 0x8B, 0x4F, 0x08, 0x49, 0x8B, 0x57, 0x10,
          0x49, 0x8B, 0x5F, 0x18, 0x49, 0x8B, 0x6F, 0x28, 0x49, 0x8B, 0x77, 0x30,
          0x49, 0x8B, 0x7F, 0x38});                      // load regs
  Put(p, {0x41, 0xFF, 0x77, 0x40, 0x9D});                // flags
  std::memcpy(p, f.bytes, f.len);
  p += f.len;
  Put(p, {0x9C, 0x41, 0x8F, 0x47, 0x40});                // flags out
  Put(p, {0x49, 0x89, 0x47, 0x00, 0x49, 0x89, 0x4F, 0x08, 0x49, 0x89, 0x57, 0x10,
          0x49, 0x89, 0x5F, 0x18, 0x49, 0x89, 0x6F, 0x28, 0x49, 0x89, 0x77, 0x30,
          0x49, 0x89, 0x7F, 0x38});                      // regs out
  for (int i = 0; i < 8; i++) {                          // movdqu [..], xmmi
    Put(p, {0xF3, 0x41, 0x0F, 0x7F, uint8_t(0x87 | (i << 3))});
    PutDisp32(p, 0x50 + 16 * i);
  }
  Put(p, {0x41, 0x0F, 0xAE, 0x9F}); PutDisp32(p, 0x48);  // stmxcsr [r15+48]
  Put(p, {0x41, 0x0F, 0xAE, 0x97}); PutDisp32(p, 0x4C);  // ldmxcsr [r15+4C]
  Put(p, {0xFC, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5B, 0xC3});
}

sigjmp_buf g_jmp;
volatile int g_phase = 0;  // 1 native, 2 lifted
const char* volatile g_form = "";
void OnFault(int sig) {
  if (g_phase == 1) siglongjmp(g_jmp, 1);
  std::printf("signal %d in the lifted run of %s\n", sig, g_form);
  std::_Exit(2);
}

uint32_t RandomGpr(std::mt19937_64& g) {
  switch (g() % 8) {
    case 0: return 0;
    case 1: return 0xFFFFFFFFu;
    case 2: return uint32_t(g() % 64);
    case 3: return 0x80000000u;
    default: return uint32_t(g());
  }
}

uint64_t SpecialDouble(std::mt19937_64& g) {
  const uint64_t frac = g() & 0x000FFFFFFFFFFFFFull, sign = (g() & 1) << 63;
  switch (g() % 16) {
    case 0: return sign;                                          // +-0
    case 1: return sign | 0x7FF0000000000000ull;                  // +-inf
    case 2: return sign | 0x7FF8000000000000ull | (frac >> 1);    // QNaN
    case 3: return sign | 0x7FF0000000000000ull | ((frac >> 1) | 1);  // SNaN
    case 4: return sign | frac;                                   // denormal
    case 5: return sign | 0x3FF0000000000000ull;                  // +-1
    case 6: return sign | 0x7FEFFFFFFFFFFFFFull;                  // max
    case 7: return sign | 0x0010000000000000ull | (frac & 0xFF);  // near min
    case 8: {                                                     // near int32 limits
      double v = double(int64_t(g() % 7) - 3) * 0.25 + (g() & 1 ? 2147483647.0 : -2147483648.0);
      uint64_t b;
      std::memcpy(&b, &v, 8);
      return b;
    }
    case 9: {                                                     // small ints/halves
      double v = double(int64_t(g() % 41) - 20) * 0.5;
      uint64_t b;
      std::memcpy(&b, &v, 8);
      return b;
    }
    default:                                                      // ordinary values
      return sign | (uint64_t(1023 - 60 + g() % 120) << 52) | frac;
  }
}

uint32_t SpecialFloat(std::mt19937_64& g) {
  const uint32_t frac = uint32_t(g()) & 0x007FFFFFu, sign = uint32_t(g() & 1) << 31;
  switch (g() % 16) {
    case 0: return sign;
    case 1: return sign | 0x7F800000u;
    case 2: return sign | 0x7FC00000u | (frac >> 1);
    case 3: return sign | 0x7F800000u | ((frac >> 1) | 1);
    case 4: return sign | frac;
    case 5: return sign | 0x3F800000u;
    case 6: return sign | 0x7F7FFFFFu;
    case 7: return sign | 0x00800000u | (frac & 0xFF);
    case 8: {
      float v = float(int(g() % 41) - 20) * 0.5f;
      uint32_t b;
      std::memcpy(&b, &v, 4);
      return b;
    }
    default:
      return sign | (uint32_t(127 - 30 + g() % 60) << 23) | frac;
  }
}

// 16 bytes of doubles, floats or raw bits.
void Fill16(std::mt19937_64& g, uint8_t* p) {
  switch (g() % 4) {
    case 0:
      for (int i = 0; i < 2; i++) {
        uint64_t v = SpecialDouble(g);
        std::memcpy(p + 8 * i, &v, 8);
      }
      break;
    case 1:
      for (int i = 0; i < 4; i++) {
        uint32_t v = SpecialFloat(g);
        std::memcpy(p + 4 * i, &v, 4);
      }
      break;
    default:
      for (int i = 0; i < 16; i++) p[i] = uint8_t(g());
      break;
  }
}

std::string Hex16(const uint8_t* b) {
  char s[40];
  for (int i = 0; i < 16; i++) std::snprintf(s + 2 * i, 3, "%02X", b[15 - i]);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  int trials = argc > 1 ? std::atoi(argv[1]) : 2000;
  const char* only = argc > 2 ? argv[2] : nullptr;
  recomp::GuestMemory mem;
  if (!mem.Reserve()) return 1;
  recomp::g_guest_base = mem.base();
  mem.Commit(kBuf, kBufSize, recomp::Prot::RW);
  mem.Commit(kStackVa - 0x1000, 0x2000, recomp::Prot::RW);
  auto* native_buf = static_cast<uint8_t*>(
      mmap(reinterpret_cast<void*>(uintptr_t(kBuf)), kBufSize, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0));
  if (native_buf != reinterpret_cast<uint8_t*>(uintptr_t(kBuf))) {
    std::printf("cannot map the native buffer at %08X\n", kBuf);
    return 1;
  }
  auto* page = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  struct sigaction sa{};
  sa.sa_handler = OnFault;
  sa.sa_flags = SA_NODEFER;
  sigaction(SIGFPE, &sa, nullptr);
  sigaction(SIGSEGV, &sa, nullptr);
  sigaction(SIGBUS, &sa, nullptr);
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  std::mt19937_64 g(778);
  std::vector<uint8_t> init(kBufSize);
  int failed_forms = 0;
  long checks = 0, skipped = 0;
  std::map<std::string, int> fails_by_mnemonic;
  for (int fi = 0; fi < kSseFormCount; fi++) {
    const SseForm& f = kSseForms[fi];
    std::string text = f.text;
    if (only && text.find(only) == std::string::npos) continue;
    Stub(page, f);
    auto native = reinterpret_cast<NativeFn>(page);
    const uint32_t defined = 0x8D5;  // cf pf af zf sf of
    int fails = 0;
    for (int t = 0; t < trials; t++) {
      Regs in{};
      for (int r = 0; r < 8; r++) in.r[r] = RandomGpr(g);
      in.r[4] = 0;
      if (f.base >= 0) in.r[f.base] = kBuf + 0x1000;
      in.flags = 0x2 | (g() & 0x8D5);
      in.mxcsr = 0x1F80 | uint32_t(g() % 4) << 13 | (g() % 4 == 0 ? uint32_t(g()) & 0x3F : 0);
      for (int i = 0; i < 8; i++) Fill16(g, in.xmm[i]);
      for (uint32_t o = 0; o < kBufSize; o += 16) Fill16(g, &init[o]);

      Regs hw = in;
      std::memcpy(native_buf, init.data(), kBufSize);
      g_form = f.text;
      g_phase = 1;
      if (sigsetjmp(g_jmp, 1)) {
        g_phase = 0;
        skipped++;
        continue;
      }
      native(&hw);
      g_phase = 2;

      std::memcpy(mem.ptr(kBuf), init.data(), kBufSize);
      recomp::Ctx c{};
      c.eax = uint32_t(in.r[0]); c.ecx = uint32_t(in.r[1]); c.edx = uint32_t(in.r[2]);
      c.ebx = uint32_t(in.r[3]); c.ebp = uint32_t(in.r[5]); c.esi = uint32_t(in.r[6]);
      c.edi = uint32_t(in.r[7]);
      c.esp = kStackVa;
      c.eflags = uint32_t(in.flags);
      std::memcpy(c.xmm, in.xmm, sizeof in.xmm);
      c.mxcsr = in.mxcsr;
      f.fn(c);
      checks++;

      uint32_t got[8] = {c.eax, c.ecx, c.edx, c.ebx, 0, c.ebp, c.esi, c.edi};
      static const char* names[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
      std::string why;
      for (int r = 0; r < 8; r++)
        if (r != 4 && got[r] != uint32_t(hw.r[r])) why += std::string(" ") + names[r];
      uint32_t lf = c.eflags;
      if ((lf ^ uint32_t(hw.flags)) & defined) why += " flags";
      for (int i = 0; i < 8; i++)
        if (std::memcmp(c.xmm[i].b, hw.xmm[i], 16)) why += " xmm" + std::to_string(i);
      if (c.mxcsr != hw.mxcsr) why += " mxcsr";
      if (std::memcmp(native_buf, mem.ptr(kBuf), kBufSize)) why += " mem";
      if (why.empty()) continue;
      if (++fails <= 3) {
        std::printf("FAIL %-40s (e.g. %08llX):%s\n", f.text,
                    (unsigned long long)f.example_va, why.c_str());
        std::printf("   in   mxcsr=%04X fl=%03X\n", in.mxcsr, uint32_t(in.flags) & 0x8D5);
        std::printf("   hw   mxcsr=%04X fl=%03X\n", hw.mxcsr, uint32_t(hw.flags) & 0x8D5);
        std::printf("   lift mxcsr=%04X fl=%03X\n", c.mxcsr, lf & 0x8D5);
        for (int i = 0; i < 8; i++)
          if (std::memcmp(c.xmm[i].b, hw.xmm[i], 16) || std::memcmp(in.xmm[i], hw.xmm[i], 16))
            std::printf("   xmm%d in %s hw %s lift %s\n", i, Hex16(in.xmm[i]).c_str(),
                        Hex16(hw.xmm[i]).c_str(), Hex16(c.xmm[i].b).c_str());
        if (f.base >= 0)
          std::printf("   mem  in %s\n", Hex16(&init[0x1000]).c_str());
      }
    }
    if (fails) {
      failed_forms++;
      fails_by_mnemonic[text.substr(0, text.find(' '))]++;
    }
  }
  std::printf("%d forms, %ld checks (%ld skipped: native faults), %d forms failed\n",
              kSseFormCount, checks, skipped, failed_forms);
  for (const auto& [m, n] : fails_by_mnemonic) std::printf("  %-10s %d forms\n", m.c_str(), n);
  return failed_forms ? 1 : 0;
}
