// The Miles-compatible digital master reverb
// (runtime/audio/digital_reverb.h).
//
// Always: the native reverb runs a fixed set of scenarios (every room type
// and some free settings, mono and stereo, 22 and 44 kHz, impulses and
// noise, settings changed mid-tail, the tail timeout and waking up again)
// and its output must hash to the value below: a hash of our own output,
// recorded when the oracle run found it identical to the original's.
// Portable: this is what catches a host whose float arithmetic differs
// (contraction into FMA, flush-to-zero).
//
// DIGITAL_REVERB_ORACLE (built by recomp_add_digital_reverb_test from the
// user's installed mss32.dll, Miles 6.5e; nothing from it is in the
// source tree): the same scenarios also run through the original
// routines, lifted at build time with `recomp_lift --only` and executed on
// the x87 model at the Windows default control word (0x27F), and every
// fragment of output, the on/off state and the coefficients must match
// bit for bit. Addresses and driver-block offsets used here are documented
// in docs/miles.md ("Miles 6.5e digital reverb").
// The coefficients are also compared over a sweep of reverb times and
// dampings, for both of the CRT's pow paths (x87 and SSE2).

#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "runtime/audio/digital_reverb.h"

#ifdef DIGITAL_REVERB_ORACLE
#include <cstdarg>
#include <string>

#include "runtime/lifted.h"
#include "runtime/log.h"
#include "runtime/pe_image.h"
#endif

namespace {

using recomp::audio::DigitalReverb;

struct Setting {
  float time, predelay, damping;
};

struct Scenario {
  int rate;
  bool stereo;
  int fragment;  // frames
  Setting first, second;
};

std::vector<Scenario> Scenarios() {
  std::vector<Scenario> s;
  std::vector<Setting> settings;
  for (const auto& r : DigitalReverb::kRoomTable)
    settings.push_back({r.time, r.predelay, r.damping});
  settings.push_back({0.5f, 0.0f, 1.0f});
  settings.push_back({2.0f, 0.25f, 0.75f});
  settings.push_back({0.05f, 0.49f, 2.5f});
  settings.push_back({0.0f, 0.01f, 1.0f});
  for (int rate : {22050, 44100})
    for (bool stereo : {false, true})
      for (size_t i = 0; i < settings.size(); i++)
        s.push_back({rate, stereo, rate == 22050 ? 256 : 512, settings[i],
                     settings[(i + 7) % settings.size()]});
  return s;
}

// The summed sends of fragment `f`: an impulse, noise, silence, then a
// second burst after the reverb was woken again.
// The noise comes from raw mt19937 output, which the standard fixes (its
// distributions it does not): the top 16 bits of a draw, which is what
// libstdc++'s uniform_int_distribution<int>(-32768, 32767) returned when
// the hash below was recorded.
void Send(const Scenario& sc, int f, std::mt19937& g, std::vector<int32_t>* send) {
  for (int i = 0; i < sc.fragment; i++) {
    int32_t v = 0;
    if (f == 0 && i == 0) v = 32767 * 2048;
    if ((f >= 3 && f < 9) || (f >= 70 && f < 74))
      v = (int32_t(g() >> 16) - 32768) * 1024;
    (*send)[i] = v;
  }
}

constexpr int kFragments = 110;

uint64_t Fnv(uint64_t h, const void* p, size_t n) {
  auto* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
  return h;
}

// What the scenarios do, independently of who runs them.
struct Driver {
  virtual ~Driver() = default;
  virtual void Open(const Scenario& sc) = 0;
  virtual void SetLevels(float dry, float wet, uint32_t ms) = 0;
  virtual void SetReverb(const Setting& s, uint32_t ms) = 0;
  // One fragment; returns whether the reverb ran, `out` the build buffer
  // (cleared first, as Miles does).
  virtual bool Fragment(const std::vector<int32_t>& send, uint32_t ms,
                        std::vector<int32_t>* out) = 0;
  virtual DigitalReverb::Coefficients Coefficients() = 0;
};

struct Native : Driver {
  DigitalReverb r;
  Scenario sc{};
  void Open(const Scenario& s) override {
    sc = s;
    r.Open(s.rate, s.fragment);
  }
  void SetLevels(float dry, float wet, uint32_t ms) override { r.SetLevels(dry, wet, ms); }
  void SetReverb(const Setting& s, uint32_t ms) override {
    r.SetReverb(s.time, s.predelay, s.damping, ms);
  }
  bool Fragment(const std::vector<int32_t>& send, uint32_t ms,
                std::vector<int32_t>* out) override {
    std::fill(out->begin(), out->end(), 0);
    if (!r.Begin(ms)) return false;
    r.Render(send.data(), sc.fragment, out->data(), sc.stereo);
    return true;
  }
  DigitalReverb::Coefficients Coefficients() override { return r.coefficients(); }
};

// Runs every scenario on `d`; `check` sees each step's results.
template <typename Check>
uint64_t RunScenarios(Driver& d, Check check) {
  uint64_t h = 0xCBF29CE484222325ull;
  std::vector<int32_t> send, out;
  for (const Scenario& sc : Scenarios()) {
    std::mt19937 g(uint32_t(sc.rate) ^ uint32_t(sc.first.time * 1000));
    send.assign(size_t(sc.fragment), 0);
    out.assign(size_t(sc.fragment) * (sc.stereo ? 2 : 1), 0);
    d.Open(sc);
    d.SetLevels(1.0f, 0.5f, 0);
    d.SetReverb(sc.first, 0);
    check(sc, -1, false, out);
    for (int f = 0; f < kFragments; f++) {
      uint32_t ms = uint32_t(int64_t(f) * sc.fragment * 1000 / sc.rate);
      if (f == 40) d.SetReverb(sc.second, ms);  // mid-tail
      if (f == 69) d.SetLevels(1.0f, 0.75f, ms);  // wakes it after a timeout
      Send(sc, f, g, &send);
      bool on = d.Fragment(send, ms, &out);
      h = Fnv(h, &on, 1);
      h = Fnv(h, out.data(), out.size() * sizeof(int32_t));
      check(sc, f, on, out);
    }
  }
  return h;
}

// Recorded from the native reverb when it matched the lifted original.
constexpr uint64_t kGolden = 0x87CC5CE7162BDBD3ull;

}  // namespace

#ifdef DIGITAL_REVERB_ORACLE

namespace {

using recomp::Ctx;

// Miles 6.5e (mss32.dll 6.5e, image base 0x21100000).
constexpr uint32_t kSetLevels = 0x2112AA10;  // ESI = driver; (dry, wet)
constexpr uint32_t kOpen = 0x2112AA70;       // ESI = driver
constexpr uint32_t kCoefficients = 0x2112ACE0;  // ESI = driver + 0x2C0; (time, damping)
constexpr uint32_t kRender = 0x2112B040;     // ESI = driver
constexpr uint32_t kSetReverb = 0x2112B0F0;  // EAX = driver; (time, predelay, damping)
constexpr uint32_t kBegin = 0x2112BCC0;      // EDI = driver
constexpr uint32_t kAllocHook = 0x21155830;  // AIL_mem_alloc_lock's allocator
constexpr uint32_t kMaxPredelayPref = 0x21156618;  // DIG preference 2
constexpr uint32_t kSse2Available = 0x2115AD20;    // CRT: pow takes its SSE2 path

// Host stand-ins the original calls.
constexpr uint32_t kHostAlloc = 0x7E000000, kHostClear = 0x7E000010;
constexpr uint32_t kHeap = 0x30000000, kHeapSize = 0x01000000;
constexpr uint32_t kStackTop = 0x40100000;
constexpr uint32_t kTeb = 0x40200000;

recomp::GuestMemory g_mem;
uint32_t g_heap_next = kHeap;
uint32_t g_ms = 0;
uint32_t g_time_get_time = 0;  // its import thunk

uint32_t Rd(uint32_t va) { return g_mem.read<uint32_t>(va); }
void Wr(uint32_t va, uint32_t v) { g_mem.write<uint32_t>(va, v); }
uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
float Float(uint32_t u) {
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

uint32_t Alloc(uint32_t n) {
  uint32_t p = g_heap_next;
  g_heap_next += (n + 15) & ~15u;
  if (g_heap_next > kHeap + kHeapSize) {
    std::printf("guest heap exhausted\n");
    std::abort();
  }
  std::memset(g_mem.ptr(p), 0xCD, n);  // Miles must clear what it uses
  return p;
}

}  // namespace

namespace recomp {
uint8_t* g_guest_base = nullptr;

void Dispatch(Ctx& c, uint32_t target, uint32_t va) {
  if (target == g_time_get_time) {
    c.eax = g_ms;
    c.esp += 4;
  } else if (target == kHostAlloc) {  // stdcall (bytes)
    c.eax = Alloc(Rd(c.esp + 4));
    c.esp += 8;
  } else if (target == kHostClear) {  // stdcall (dest, bytes, mmx)
    std::memset(g_mem.ptr(Rd(c.esp + 4)), 0, Rd(c.esp + 8));
    c.esp += 16;
  } else {
    std::printf("unexpected call to %08X from %08X\n", target, va);
    std::abort();
  }
}
void Unimplemented(Ctx&, uint32_t va, const char* what) {
  std::printf("unimplemented %08X %s\n", va, what);
  std::abort();
}
void BadJump(Ctx&, uint32_t from, uint32_t to) {
  std::printf("bad jump %08X -> %08X\n", from, to);
  std::abort();
}
void Unreachable(Ctx&, uint32_t va) {
  std::printf("unreachable %08X\n", va);
  std::abort();
}
void Trap(Ctx&, uint32_t va) {
  std::printf("trap %08X\n", va);
  std::abort();
}
void RaiseDivideError(Ctx&, uint32_t va) {
  std::printf("divide error %08X\n", va);
  std::abort();
}
void Cpuid(Ctx&) { std::abort(); }
uint64_t Rdtsc(Ctx&) { std::abort(); }
void Probe(Ctx&, uint32_t, const char*) {}
void FatalF(const char* fmt, ...) {  // runtime/sse.cpp's #XM
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::abort();
}

namespace gen {
void f_2112AA10(Ctx& c);
void f_2112AA70(Ctx& c);
void f_2112ACE0(Ctx& c);
void f_2112B040(Ctx& c);
void f_2112B0F0(Ctx& c);
void f_2112BCC0(Ctx& c);
}  // namespace gen
}  // namespace recomp

namespace {

// Calls a lifted routine with `args` pushed (last first) and a return
// address; restores ESP afterwards (cdecl and stdcall alike).
void Call(Ctx& c, void (*fn)(Ctx&), std::initializer_list<uint32_t> args) {
  uint32_t saved = c.esp;
  for (auto it = std::rbegin(args); it != std::rend(args); ++it) {
    c.esp -= 4;
    Wr(c.esp, *it);
  }
  c.esp -= 4;
  Wr(c.esp, 0xDEAD0000);
  fn(c);
  c.esp = saved;
}

Ctx NewCtx() {
  Ctx c{};
  recomp::x87::Init(c.fpu);
  recomp::x87::SetControlWord(c.fpu, 0x027F);  // Windows' default
  c.mxcsr = 0x1F80;
  c.esp = kStackTop;
  c.fs_base = kTeb;
  return c;
}

// The original, on a zeroed driver block as AIL_open_digital_driver
// leaves it (rate, channels, flags, build buffer, levels 1).
struct Lifted : Driver {
  Ctx c = NewCtx();
  uint32_t drv = 0;
  Scenario sc{};
  void Open(const Scenario& s) override {
    sc = s;
    g_heap_next = kHeap;
    drv = Alloc(0x370);
    std::memset(g_mem.ptr(drv), 0, 0x370);
    int ch = s.stereo ? 2 : 1;
    Wr(drv + 0x14, uint32_t(s.rate));
    Wr(drv + 0x18, s.stereo ? 3 : 1);  // 16-bit, stereo
    Wr(drv + 0x20, uint32_t(ch));
    Wr(drv + 0x3C, uint32_t(s.fragment * ch * 4));
    Wr(drv + 0x40, Alloc(uint32_t(s.fragment * ch * 4)));
    Wr(drv + 0x10, Bits(1.0f));
    Wr(drv + 0x104, Bits(1.0f));
    Wr(drv + 0x108, Bits(1.0f));
    Wr(drv + 0x128, kHostClear);
    c = NewCtx();
    c.esi = drv;
    Call(c, recomp::gen::f_2112AA70, {});
    if (c.eax != 1) {
      std::printf("lifted open failed\n");
      std::abort();
    }
  }
  void SetLevels(float dry, float wet, uint32_t ms) override {
    g_ms = ms;
    c.esi = drv;
    Call(c, recomp::gen::f_2112AA10, {Bits(dry), Bits(wet)});
  }
  void SetReverb(const Setting& s, uint32_t ms) override {
    g_ms = ms;
    c.eax = drv;
    Call(c, recomp::gen::f_2112B0F0, {Bits(s.time), Bits(s.predelay), Bits(s.damping)});
  }
  bool Fragment(const std::vector<int32_t>& send, uint32_t ms,
                std::vector<int32_t>* out) override {
    g_ms = ms;
    c.edi = drv;
    Call(c, recomp::gen::f_2112BCC0, {});
    bool on = Rd(drv + 0x348) != 0;
    if (on) {
      uint32_t slot = Rd(drv + 0x33C) + Rd(drv + 0x360);
      for (int i = 0; i < sc.fragment; i++)
        Wr(slot + 4 * i, Rd(slot + 4 * i) + uint32_t(send[size_t(i)]));
      c.esi = drv;
      Call(c, recomp::gen::f_2112B040, {});
    }
    std::memcpy(out->data(), g_mem.ptr(Rd(drv + 0x40)), out->size() * 4);
    return on;
  }
  DigitalReverb::Coefficients Coefficients() override {
    DigitalReverb::Coefficients k;
    uint32_t p = drv + 0x2C0;
    for (int i = 0; i < 4; i++) k.comb[i] = Float(Rd(p + 0x58 + 4 * i));
    for (int i = 0; i < 2; i++) k.allpass[i] = Float(Rd(p + 0x68 + 4 * i));
    k.a0 = Float(Rd(p + 0x70));
    k.b1 = Float(Rd(p + 0x74));
    k.b2 = Float(Rd(p + 0x78));
    return k;
  }
};

bool Same(float a, float b) {
  return (std::isnan(a) && std::isnan(b)) || Bits(a) == Bits(b);
}

int CompareCoefficients(const DigitalReverb::Coefficients& n,
                        const DigitalReverb::Coefficients& l, const char* what) {
  const float* a = &n.comb[0];
  const float* b = &l.comb[0];
  static const char* names[9] = {"comb0", "comb1", "comb2", "comb3", "allpass0",
                                 "allpass1", "a0", "b1", "b2"};
  int bad = 0;
  for (int i = 0; i < 9; i++)
    if (!Same(a[i], b[i])) {
      if (bad++ < 1)
        std::printf("%s: %s native %.9g (%08X) original %.9g (%08X)\n", what, names[i],
                    a[i], Bits(a[i]), b[i], Bits(b[i]));
    }
  return bad ? 1 : 0;
}

// Coefficients alone over many times and dampings.
int SweepCoefficients(bool sse2) {
  Wr(kSse2Available, sse2 ? 1 : 0);
  Ctx c = NewCtx();
  uint32_t block = Alloc(0x80);
  DigitalReverb r;
  r.Open(22050, 256);
  r.SetLevels(1.0f, 0.0f, 0);  // no wake: the read position is not used
  std::mt19937 g(sse2 ? 2 : 1);
  std::uniform_real_distribution<float> lt(std::log(0.01f), std::log(20.0f));
  std::uniform_real_distribution<float> damp(0.0f, 4.0f);
  int bad = 0, n = 0;
  for (int i = 0; i < 20000; i++) {
    float time = std::exp(lt(g)), damping = damp(g);
    if (i < DigitalReverb::kRooms) {
      time = DigitalReverb::kRoomTable[i].time;
      damping = DigitalReverb::kRoomTable[i].damping;
    }
    c.esi = block;
    Call(c, recomp::gen::f_2112ACE0, {Bits(time), Bits(damping)});
    DigitalReverb::Coefficients l;
    for (int j = 0; j < 4; j++) l.comb[j] = Float(Rd(block + 0x58 + 4 * j));
    for (int j = 0; j < 2; j++) l.allpass[j] = Float(Rd(block + 0x68 + 4 * j));
    l.a0 = Float(Rd(block + 0x70));
    l.b1 = Float(Rd(block + 0x74));
    l.b2 = Float(Rd(block + 0x78));
    r.SetReverb(time, 0.0f, damping, 0);
    char what[64];
    std::snprintf(what, sizeof what, "time %.9g damping %.9g", time, damping);
    bad += CompareCoefficients(r.coefficients(), l, what);
    n++;
  }
  std::printf("coefficients (%s pow): %d of %d settings differ\n", sse2 ? "SSE2" : "x87",
              bad, n);
  return bad;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: digital_reverb_oracle_test <mss32.dll (6.5e)>\n");
    return 2;
  }
  if (!g_mem.Reserve()) return 1;
  recomp::g_guest_base = g_mem.base();
  recomp::PeImage img;
  std::string err;
  if (!img.Load(argv[1], g_mem, &err)) {
    std::printf("%s: %s\n", argv[1], err.c_str());
    return 1;
  }
  for (const auto& imp : img.imports)
    if (imp.name == "timeGetTime") g_time_get_time = imp.thunk_va;
  g_mem.Commit(kHeap, kHeapSize, recomp::Prot::RW);
  g_mem.Commit(kStackTop - 0x10000, 0x10000, recomp::Prot::RW);
  g_mem.Commit(kTeb, 0x1000, recomp::Prot::RW);
  Wr(kAllocHook, kHostAlloc);
  Wr(kMaxPredelayPref, 500);
  // The fresh image has the CRT's SSE2 flag clear: pow takes its x87
  // path unless a sweep sets it.
  Wr(kSse2Available, 0);

  int failures = 0;
  Native native;
  Lifted lifted;
  // Lockstep: each step of the scenarios runs on the original first.
  struct Lockstep : Driver {
    Native* n;
    Lifted* l;
    std::vector<int32_t> lout;
    bool lon = false;
    void Open(const Scenario& s) override {
      l->Open(s);
      n->Open(s);
    }
    void SetLevels(float dry, float wet, uint32_t ms) override {
      l->SetLevels(dry, wet, ms);
      n->SetLevels(dry, wet, ms);
    }
    void SetReverb(const Setting& s, uint32_t ms) override {
      l->SetReverb(s, ms);
      n->SetReverb(s, ms);
    }
    bool Fragment(const std::vector<int32_t>& send, uint32_t ms,
                  std::vector<int32_t>* out) override {
      lout.resize(out->size());
      lon = l->Fragment(send, ms, &lout);
      return n->Fragment(send, ms, out);
    }
    DigitalReverb::Coefficients Coefficients() override { return n->Coefficients(); }
  } step;
  step.n = &native;
  step.l = &lifted;
  int scenario_failures = 0, scenarios = 0;
  long fragments_on = 0, nonzero = 0;
  bool scenario_bad = false;
  uint64_t h = RunScenarios(step, [&](const Scenario& sc, int f, bool on,
                                      const std::vector<int32_t>& out) {
    if (f < 0) {
      if (scenario_bad) scenario_failures++;
      scenario_bad = false;
      scenarios++;
      char what[96];
      std::snprintf(what, sizeof what, "%d Hz %s time %g", sc.rate,
                    sc.stereo ? "stereo" : "mono", double(sc.first.time));
      if (CompareCoefficients(native.Coefficients(), lifted.Coefficients(), what))
        scenario_bad = true;
      return;
    }
    if (scenario_bad) return;
    fragments_on += on;
    for (int32_t v : out) nonzero += v != 0;
    if (on != step.lon) {
      std::printf("%d Hz %s time %g fragment %d: on %d, original %d\n", sc.rate,
                  sc.stereo ? "stereo" : "mono", double(sc.first.time), f, on, step.lon);
      scenario_bad = true;
      return;
    }
    for (size_t i = 0; i < out.size(); i++)
      if (out[i] != step.lout[i]) {
        std::printf("%d Hz %s time %g fragment %d sample %zu: %d, original %d\n", sc.rate,
                    sc.stereo ? "stereo" : "mono", double(sc.first.time), f, i, out[i],
                    step.lout[i]);
        scenario_bad = true;
        return;
      }
  });
  if (scenario_bad) scenario_failures++;
  std::printf("scenarios: %d of %d differ from the original (reverb on in %ld "
              "fragments, %ld non-zero samples); hash %016" PRIX64 "\n",
              scenario_failures, scenarios, fragments_on, nonzero, h);
  failures += scenario_failures;
  failures += SweepCoefficients(false);
  failures += SweepCoefficients(true);
  if (h != kGolden) std::printf("note: kGolden is %016" PRIX64 "\n", kGolden);
  std::printf("%s\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}

#else

int main() {
  Native native;
  uint64_t h = RunScenarios(native, [](const Scenario&, int, bool,
                                       const std::vector<int32_t>&) {});
  std::printf("hash %016" PRIX64 ", expected %016" PRIX64 "\n", h, kGolden);
  return h == kGolden ? 0 : 1;
}

#endif
