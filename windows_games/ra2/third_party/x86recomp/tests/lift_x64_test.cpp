// End-to-end oracle test for x64 lifting: every distinct integer form
// (recomp_lift --int-test on an x64 image) or SSE/AVX form (--sse-test,
// built with LIFT_X64_SSE; needs an AVX host) runs lifted and natively on
// this CPU from the same random state: all 16 general registers, flags,
// the 16 XMM registers (SSE: the whole YMM registers), MXCSR (SSE: any
// rounding mode, random sticky flags, all exceptions masked) and memory. Everything except rsp must match bit for bit, flags
// the instruction leaves undefined excepted. x86-64 hosts only; dev-time.
//
// The native stub keeps the Regs pointer in a RIP-relative slot of its own
// page, so the instruction under test may use any register but rsp. The
// memory operand is a buffer mapped at the same low address natively and
// in the guest arena, so register values (pointers included) are identical
// in both runs.

#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>

#include <cmath>
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

#ifdef LIFT_X64_SSE
#include "lift_sse_forms.h"
using Form = SseForm;
#define kForms kSseForms
#define kFormCount kSseFormCount
#else
#include "lift_int_forms.h"
using Form = IntForm;
#define kForms kIntForms
#define kFormCount kIntFormCount
#endif
#define RECOMP_LIFT_X64 1
#include "runtime/guest_memory.h"
#include "runtime/lifted.h"

namespace recomp {
uint8_t* g_guest_base = nullptr;
void Unimplemented(Ctx&, uint64_t va, const char* what) {
  std::printf("unimplemented %08llX %s\n", (unsigned long long)va, what);
  std::abort();
}
void BadJump(Ctx&, uint64_t from, uint64_t to) {
  std::printf("bad jump %08llX -> %08llX\n", (unsigned long long)from,
              (unsigned long long)to);
  std::abort();
}
// Trials that divide-fault natively are skipped before the lifted run.
void RaiseDivideError(Ctx&, uint64_t va) {
  std::printf("lifted divide error at %08llX where the CPU had none\n",
              (unsigned long long)va);
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
constexpr uint64_t kStackVa = 0x00300000;

struct Regs {
  uint64_t r[16];       // 0x00 rax rcx rdx rbx (rsp) rbp rsi rdi r8..r15
  uint64_t flags;       // 0x80
  uint32_t mxcsr;       // 0x88
  uint32_t host_mxcsr;  // 0x8C: the test's own, restored after the form
  uint8_t xmm[16][16];  // 0x90
  uint8_t ymm_hi[16][16];  // 0x190: bits 255:128 (SSE test)
};
static_assert(offsetof(Regs, xmm) == 0x90 && offsetof(Regs, ymm_hi) == 0x190);

using NativeFn = void (*)(Regs*);

constexpr int kPtrSlot = 0xF00, kRaxSlot = 0xF08;  // in the stub's page

struct Asm {
  uint8_t* page;
  uint8_t* p;
  void Put(std::initializer_list<uint8_t> b) {
    for (uint8_t x : b) *p++ = x;
  }
  void D32(uint32_t d) {
    std::memcpy(p, &d, 4);
    p += 4;
  }
  // disp32 of a RIP-relative operand to `slot`, the instruction ending
  // `after` bytes after the displacement.
  void Rip(int slot, int after = 0) { D32(uint32_t(page + slot - (p + 4 + after))); }
};

void Stub(uint8_t* page, const Form& f) {
  Asm a{page, page};
  a.Put({0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57});  // push
  a.Put({0x48, 0x89, 0x3D}); a.Rip(kPtrSlot);              // mov [ptr], rdi
  a.Put({0x48, 0x89, 0xF8});                               // mov rax, rdi
  a.Put({0x0F, 0xAE, 0x98}); a.D32(0x8C);                  // stmxcsr [rax+8C]
  a.Put({0x0F, 0xAE, 0x90}); a.D32(0x88);                  // ldmxcsr [rax+88]
  for (int i = 0; i < 16; i++) {                           // movdqu xmmi, [rax+..]
    a.Put({0xF3});
    if (i >= 8) a.Put({0x44});
    a.Put({0x0F, 0x6F, uint8_t(0x80 | ((i & 7) << 3))});
    a.D32(0x90 + 16 * i);
  }
#ifdef LIFT_X64_SSE
  for (int i = 0; i < 16; i++) {  // vinsertf128 ymmi, ymmi, [rax+..], 1
    a.Put({0xC4, uint8_t(i < 8 ? 0xE3 : 0x63), uint8_t(((~i & 15) << 3) | 0x05), 0x18,
           uint8_t(0x80 | ((i & 7) << 3))});
    a.D32(0x190 + 16 * i);
    a.Put({0x01});
  }
#endif
  a.Put({0xFF, 0xB0}); a.D32(0x80);                        // push [rax+80]
  a.Put({0x9D});                                           // popfq
  for (int i = 1; i < 16; i++) {                           // mov ri, [rax+8i]
    if (i == 4) continue;
    a.Put({uint8_t(i >= 8 ? 0x4C : 0x48), 0x8B, uint8_t(0x40 | ((i & 7) << 3)),
           uint8_t(8 * i)});
  }
  a.Put({0x48, 0x8B, 0x00});                               // mov rax, [rax]
  std::memcpy(a.p, f.bytes, f.len);
  a.p += f.len;
  a.Put({0x48, 0x89, 0x05}); a.Rip(kRaxSlot);              // mov [scratch], rax
  a.Put({0x9C});                                           // pushfq
  a.Put({0x48, 0x8B, 0x05}); a.Rip(kPtrSlot);              // mov rax, [ptr]
  a.Put({0x8F, 0x80}); a.D32(0x80);                        // pop [rax+80]
  for (int i = 1; i < 16; i++) {                           // mov [rax+8i], ri
    if (i == 4) continue;
    a.Put({uint8_t(i >= 8 ? 0x4C : 0x48), 0x89, uint8_t(0x40 | ((i & 7) << 3)),
           uint8_t(8 * i)});
  }
  a.Put({0x48, 0x8B, 0x0D}); a.Rip(kRaxSlot);              // mov rcx, [scratch]
  a.Put({0x48, 0x89, 0x08});                               // mov [rax], rcx
  for (int i = 0; i < 16; i++) {                           // movdqu [rax+..], xmmi
    a.Put({0xF3});
    if (i >= 8) a.Put({0x44});
    a.Put({0x0F, 0x7F, uint8_t(0x80 | ((i & 7) << 3))});
    a.D32(0x90 + 16 * i);
  }
#ifdef LIFT_X64_SSE
  for (int i = 0; i < 16; i++) {  // vextractf128 [rax+..], ymmi, 1
    a.Put({0xC4, uint8_t(i < 8 ? 0xE3 : 0x63), 0x7D, 0x19, uint8_t(0x80 | ((i & 7) << 3))});
    a.D32(0x190 + 16 * i);
    a.Put({0x01});
  }
  a.Put({0xC5, 0xF8, 0x77});  // vzeroupper
#endif
  a.Put({0x0F, 0xAE, 0x98}); a.D32(0x88);                  // stmxcsr [rax+88]
  a.Put({0x0F, 0xAE, 0x90}); a.D32(0x8C);                  // ldmxcsr [rax+8C]
  a.Put({0xFC, 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5B, 0xC3});
}

sigjmp_buf g_jmp;
volatile int g_phase = 0;  // 1 native, 2 lifted
const char* volatile g_form = "";
void OnFault(int sig) {
  if (g_phase == 1) siglongjmp(g_jmp, 1);
  std::printf("signal %d in the lifted run of %s\n", sig, g_form);
  std::_Exit(2);
}

uint64_t RandomValue(std::mt19937_64& g) {
  switch (g() % 14) {
    case 0: return 0;
    case 1: return ~0ull;
    case 2: return 0x8000000000000000ull >> (g() % 8 * 8);
    case 3: return 0x7FFFFFFFFFFFFFFFull;
    case 4: return g() % 64;
    case 5: return uint64_t(int64_t(int8_t(g())));
    case 6: return 0xFFFFFFFFFFFFFF00ull | (g() & 0xFF);
    case 7: return 0xFFFFFFFFu;                      // 32-bit patterns, zero-
    case 8: return 0x80000000u;                      // and sign-extended
    case 9: return uint64_t(int64_t(int32_t(g())));
    case 10: return uint32_t(g());
    default: return g();
  }
}

#ifdef LIFT_X64_SSE
uint64_t SpecialDouble(std::mt19937_64& g) {
  const uint64_t frac = g() & 0x000FFFFFFFFFFFFFull, sign = (g() & 1) << 63;
  switch (g() % 17) {
    case 0: return sign;                                              // +-0
    case 1: return sign | 0x7FF0000000000000ull;                      // +-inf
    case 2: return sign | 0x7FF8000000000000ull | (frac >> 1);        // QNaN
    case 3: return sign | 0x7FF0000000000000ull | ((frac >> 1) | 1);  // SNaN
    case 4: return sign | frac;                                       // denormal
    case 5: return sign | 0x3FF0000000000000ull;                      // +-1
    case 6: return sign | 0x7FEFFFFFFFFFFFFFull;                      // max
    case 7: return sign | 0x0010000000000000ull | (frac & 0xFF);      // near min
    case 8: {  // near the int32 and int64 limits
      double lim = g() & 1 ? (g() & 1 ? 2147483647.0 : -2147483648.0)
                           : (g() & 1 ? 9223372036854775808.0 : -9223372036854775808.0);
      double v = double(int64_t(g() % 7) - 3) * (std::abs(lim) > 1e10 ? 1024.0 : 0.25) + lim;
      uint64_t b;
      std::memcpy(&b, &v, 8);
      return b;
    }
    case 9: {  // small ints/halves
      double v = double(int64_t(g() % 41) - 20) * 0.5;
      uint64_t b;
      std::memcpy(&b, &v, 8);
      return b;
    }
    case 10: {  // float-range edges, for conversions to float
      static const uint64_t k[] = {0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull,
                                   0x3810000000000000ull, 0x380FFFFFF0000000ull,
                                   0x36A0000000000000ull, 0x3690000000000000ull};
      return sign | k[g() % 6] | (frac & 0x3);
    }
    default:  // ordinary values
      return sign | (uint64_t(1023 - 60 + g() % 120) << 52) | frac;
  }
}

uint32_t SpecialFloat(std::mt19937_64& g) {
  const uint32_t frac = uint32_t(g()) & 0x007FFFFFu, sign = uint32_t(g() & 1) << 31;
  switch (g() % 17) {
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
    case 9: {  // near the int32 and int64 limits
      static const uint32_t k[] = {0x4F000000u, 0x4EFFFFFFu, 0x5F000000u, 0x5EFFFFFFu};
      return sign | k[g() % 4];
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
#endif

std::string Hex16(const uint8_t* b) {
  char s[40];
  for (int i = 0; i < 16; i++) std::snprintf(s + 2 * i, 3, "%02X", b[15 - i]);
  return s;
}

const char* const kNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

}  // namespace

int main(int argc, char** argv) {
#ifdef LIFT_X64_SSE
  const bool sse = true;
#else
  const bool sse = false;
#endif
  int trials = argc > 1 ? std::atoi(argv[1]) : sse ? 2000 : 300;
  const char* only = argc > 2 ? argv[2] : nullptr;
  recomp::GuestMemory mem;
  if (!mem.Reserve(36)) return 1;
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

  std::mt19937_64 g(sse ? 779 : 780);
  std::vector<uint8_t> init(kBufSize);
  int failed_forms = 0;
  long checks = 0, skipped = 0;
  std::map<std::string, int> fails_by_mnemonic;
  std::map<std::string, long> skips_by_mnemonic;
  for (int fi = 0; fi < kFormCount; fi++) {
    const Form& f = kForms[fi];
    std::string text = f.text;
    if (only && text.find(only) == std::string::npos) continue;
    Stub(page, f);
    auto native = reinterpret_cast<NativeFn>(page);
    std::string mn = text.substr(0, text.find(' '));
    bool is_string = mn.find("movs") == 0 || mn.find("stos") == 0 ||
                     mn.find("lods") == 0 || mn.find("scas") == 0 ||
                     mn.find("cmps") == 0 || mn == "rep" || mn == "repe" || mn == "repne";
    is_string = is_string && mn != "movsx" && mn != "movsxd" && mn != "movss" &&
                mn != "movsd" && text.find("xmm") == std::string::npos;
    bool is_bt = mn.rfind("bt", 0) == 0 && f.base >= 0;
    uint32_t defined = (0x1u | 0x4u | 0x10u | 0x40u | 0x80u | 0x400u | 0x800u) & ~f.undef;
    int fails = 0;
    for (int t = 0; t < trials; t++) {
      Regs in{};
      for (int r = 0; r < 16; r++) in.r[r] = RandomValue(g);
      in.r[4] = 0;
      if (is_bt)
        for (int r = 0; r < 16; r++) in.r[r] &= 0x7F;
      if (f.base >= 0) in.r[f.base] = kBuf + 0x1000;
      if (is_string) {
        in.r[6] = kBuf + 0x1000;  // rsi
        in.r[7] = kBuf + 0x2000;  // rdi
        in.r[1] = g() % 9;        // rcx
      }
      in.flags = 0x2 | (g() & 0x8D5) | ((g() % 4 == 0) ? 0x400 : 0);
      in.flags &= is_string ? ~0ull : ~0x400ull;  // DF only matters there
      in.mxcsr = 0x1F80;
#ifdef LIFT_X64_SSE
      in.mxcsr |= uint32_t(g() % 4) << 13 | (g() % 4 == 0 ? uint32_t(g()) & 0x3F : 0);
      for (int i = 0; i < 16; i++) Fill16(g, in.xmm[i]);
      for (int i = 0; i < 16; i++) Fill16(g, in.ymm_hi[i]);
      for (uint32_t o = 0; o < kBufSize; o += 16) Fill16(g, &init[o]);
#else
      for (int i = 0; i < 16; i++)
        for (int k = 0; k < 16; k++) in.xmm[i][k] = uint8_t(g());
      for (auto& b : init) b = uint8_t(g());
#endif

      Regs hw = in;
      std::memcpy(native_buf, init.data(), kBufSize);
      g_form = f.text;
      g_phase = 1;
      if (sigsetjmp(g_jmp, 1)) {
        asm volatile("cld");
        g_phase = 0;
        skipped++;
        skips_by_mnemonic[mn]++;
        continue;  // natively faulted (#DE, or an address off the buffer)
      }
      native(&hw);
      g_phase = 2;

      std::memcpy(mem.ptr(kBuf), init.data(), kBufSize);
      recomp::Ctx c{};
      uint64_t* cr[16] = {&c.rax, &c.rcx, &c.rdx, &c.rbx, &c.rsp, &c.rbp, &c.rsi, &c.rdi,
                          &c.r8,  &c.r9,  &c.r10, &c.r11, &c.r12, &c.r13, &c.r14, &c.r15};
      for (int r = 0; r < 16; r++) *cr[r] = in.r[r];
      c.rsp = kStackVa;
      c.eflags = uint32_t(in.flags);
      std::memcpy(c.xmm, in.xmm, sizeof in.xmm);
#ifdef LIFT_X64_SSE
      std::memcpy(c.ymm_hi, in.ymm_hi, sizeof in.ymm_hi);
#endif
      c.mxcsr = in.mxcsr;
      f.fn(c);
      checks++;

      std::string why;
      for (int r = 0; r < 16; r++)
        if (r != 4 && *cr[r] != hw.r[r]) why += std::string(" ") + kNames[r];
      uint32_t lf = c.eflags;
      if ((lf ^ uint32_t(hw.flags)) & defined) {
        char b[48];
        std::snprintf(b, sizeof b, " flags(%03X vs %03X)", uint32_t(hw.flags) & defined,
                      lf & defined);
        why += b;
      }
      for (int i = 0; i < 16; i++)
        if (std::memcmp(c.xmm[i].b, hw.xmm[i], 16)) why += " xmm" + std::to_string(i);
#ifdef LIFT_X64_SSE
      for (int i = 0; i < 16; i++)
        if (std::memcmp(c.ymm_hi[i].b, hw.ymm_hi[i], 16)) why += " ymm_hi" + std::to_string(i);
#endif
      if (c.mxcsr != hw.mxcsr) why += " mxcsr";
      if (std::memcmp(native_buf, mem.ptr(kBuf), kBufSize)) why += " mem";
      if (why.empty()) continue;
      if (++fails <= 2) {
        std::printf("FAIL %-40s (e.g. %08llX, base %d):%s\n", f.text,
                    (unsigned long long)f.example_va, f.base, why.c_str());
        for (int r = 0; r < 16; r++)
          if (r != 4 && (*cr[r] != hw.r[r] || in.r[r] != hw.r[r]))
            std::printf("   %-3s in %016llX hw %016llX lift %016llX\n", kNames[r],
                        (unsigned long long)in.r[r], (unsigned long long)hw.r[r],
                        (unsigned long long)*cr[r]);
        std::printf("   fl in %03X hw %03X lift %03X; mxcsr in %04X hw %04X lift %04X\n",
                    uint32_t(in.flags) & 0xDD5, uint32_t(hw.flags) & 0xDD5, lf & 0xDD5,
                    in.mxcsr, hw.mxcsr, c.mxcsr);
        for (int i = 0; i < 16; i++)
          if (std::memcmp(c.xmm[i].b, hw.xmm[i], 16) || std::memcmp(in.xmm[i], hw.xmm[i], 16))
            std::printf("   xmm%-2d in %s hw %s lift %s\n", i, Hex16(in.xmm[i]).c_str(),
                        Hex16(hw.xmm[i]).c_str(), Hex16(c.xmm[i].b).c_str());
#ifdef LIFT_X64_SSE
        for (int i = 0; i < 16; i++)
          if (std::memcmp(c.ymm_hi[i].b, hw.ymm_hi[i], 16) ||
              std::memcmp(in.ymm_hi[i], hw.ymm_hi[i], 16))
            std::printf("   ymm%-2d hi in %s hw %s lift %s\n", i, Hex16(in.ymm_hi[i]).c_str(),
                        Hex16(hw.ymm_hi[i]).c_str(), Hex16(c.ymm_hi[i].b).c_str());
#endif
        if (f.base >= 0) std::printf("   mem in %s\n", Hex16(&init[0x1000]).c_str());
      }
    }
    if (fails) {
      failed_forms++;
      fails_by_mnemonic[mn]++;
    }
  }
  std::printf("%d forms, %ld checks (%ld skipped: native faults), %d forms failed\n",
              kFormCount, checks, skipped, failed_forms);
  for (const auto& [m, n] : fails_by_mnemonic) std::printf("  %-10s %d forms\n", m.c_str(), n);
  if (!skips_by_mnemonic.empty()) {
    std::printf("native faults (skipped trials) by mnemonic:\n");
    for (const auto& [m, n] : skips_by_mnemonic) std::printf("  %-10s %ld\n", m.c_str(), n);
  }
  return failed_forms ? 1 : 0;
}
