// Miles 6.5's sample levels and Fast 2D (runtime/audio/miles65.h).
//
// Always: a fixed set of scenarios (listener poses, unit and not; sources
// at the listener, inside min_dist, on the axes, behind, past max_dist;
// volumes and pans including out-of-range ones) runs through the native
// code, and its levels and gains must hash to the value below: a hash of
// our own output, recorded when the oracle run found the levels identical
// to the original's. Portable: it catches a host whose float arithmetic
// or libm differs. Scenarios come from raw mt19937 output, which the
// standard fixes (its distributions it does not).
//
// FAST2D_ORACLE (built by recomp_add_fast2d_test from the user's installed
// mss32.dll 6.5e and miles/msssoft.m3d; nothing from them is in the
// source tree): the same scenarios also run through the original
// routines, lifted at build time with `recomp_lift --only` and executed on
// the x87 model at the Windows default control word (0x27F): msssoft's
// listener setters and per-sample update, which calls mss32's level
// setters (lifted too and called through the import), and mss32's pow for
// the read-back. Every level field must match bit for bit, for both of the
// CRTs' pow paths (x87 and SSE2). Addresses and offsets are documented in
// docs/miles.md ("Miles 6.5e sample levels and Fast 2D"). The channel gains are native only (three double
// multiplications and a rounding, described there).

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#include "runtime/audio/miles65.h"

#ifdef FAST2D_ORACLE
#include <cstdarg>
#include <map>
#include <string>

#include "runtime/lifted.h"
#include "runtime/log.h"
#include "runtime/pe_image.h"
#endif

namespace {

namespace m65 = recomp::audio::miles65;
using recomp::audio::Vec3;

uint32_t Bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

// Uniform in [lo, hi) from one raw mt19937 draw.
float Uniform(std::mt19937& g, float lo, float hi) {
  return lo + (hi - lo) * float(g() >> 8) * 0x1p-24f;
}

Vec3 Direction(std::mt19937& g) {
  for (;;) {
    Vec3 v{Uniform(g, -1, 1), Uniform(g, -1, 1), Uniform(g, -1, 1)};
    float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (l > 0.1f && l <= 1) return {v.x / l, v.y / l, v.z / l};
  }
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

struct Pose {
  Vec3 pos, face, up;
};

// Listener poses: the default, axis-aligned turns, random orthonormal
// ones, and ones with unnormalised or skewed vectors.
Pose MakePose(std::mt19937& g, int i) {
  switch (i % 6) {
    case 0: return {{0, 0, 0}, {0, 0, 1}, {0, 1, 0}};
    case 1: return {{Uniform(g, -50, 50), 0, Uniform(g, -50, 50)}, {1, 0, 0}, {0, 1, 0}};
    case 2: return {{0, 0, 0}, {0, 0, 2.5f}, {0, 0.5f, 0}};
    case 3: {
      Vec3 f = Direction(g), u = Direction(g);
      return {{Uniform(g, -9, 9), Uniform(g, -9, 9), Uniform(g, -9, 9)}, f, u};
    }
    default: {
      Vec3 f = Direction(g);
      Vec3 r = Cross(Direction(g), f);
      float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
      r = {r.x / l, r.y / l, r.z / l};
      Vec3 u = Cross(f, r);
      return {{Uniform(g, -500, 500), Uniform(g, -20, 20), Uniform(g, -500, 500)}, f, u};
    }
  }
}

struct Case {
  Pose pose;
  m65::Source src;
};

constexpr int kCases = 60000;

Case MakeCase(std::mt19937& g, int i) {
  Case c;
  c.pose = MakePose(g, i / 7);
  const Pose& p = c.pose;
  m65::Source& s = c.src;
  s.min_dist = (i % 11 == 0) ? 0.0f : (i % 13 == 0 ? 5e-5f : Uniform(g, 0.5f, 40));
  s.max_dist = (i % 17 == 0) ? 1e30f : s.min_dist + Uniform(g, 1, 400);
  s.volume = (i % 9 == 0) ? 1.0f : (i % 23 == 0 ? Uniform(g, 1, 1.5f) : Uniform(g, 0, 1));
  // Mostly in range; some past max_dist.
  float d = Uniform(g, 0, 1) < 0.2f ? Uniform(g, 0, 2)
                                    : Uniform(g, 0, 1.15f * std::min(s.max_dist, 1000.0f));
  Vec3 dir = Direction(g);
  // Exactly on the listener's axes (dot products of +-1).
  Vec3 right = Cross(p.up, p.face);
  switch (i % 10) {
    case 1: dir = right; break;
    case 2: dir = {-right.x, -right.y, -right.z}; break;
    case 3: dir = p.face; break;
    case 4: dir = {-p.face.x, -p.face.y, -p.face.z}; break;
    default: break;
  }
  if (i % 29 == 0) d = 0;
  if (i % 31 == 0) d = s.max_dist;
  s.pos = {p.pos.x + dir.x * d, p.pos.y + dir.y * d, p.pos.z + dir.z * d};
  return c;
}

struct VolPan {
  float v, p;
};

VolPan MakeVolPan(std::mt19937& g, int i) {
  static const float kEdges[] = {0.0f, 0.5f, 1.0f, 1e-5f, 0.25f, 0.75f};
  VolPan x{Uniform(g, 0, 1), Uniform(g, 0, 1)};
  if (i % 5 == 0) x.p = kEdges[(i / 5) % 6];
  if (i % 7 == 0) x.v = kEdges[(i / 7) % 6];
  if (i % 19 == 0) x.p = Uniform(g, -0.2f, 1.2f);
  if (i % 37 == 0) x.v = Uniform(g, -0.5f, 1.5f);
  return x;
}

// AIL_set_sample_volume_levels pairs: from Fast 2D-like levels to
// arbitrary ones around the 1e-4 floor.
VolPan MakeLevels(std::mt19937& g, int i) {
  VolPan x{Uniform(g, 0, 1), Uniform(g, 0, 1)};
  if (i % 3 == 0) x.v *= 1e-3f;
  if (i % 4 == 0) x.p *= 1e-3f;
  if (i % 41 == 0) x.v = 1e-4f;
  if (i % 43 == 0) x.p = 0;
  if (i % 47 == 0) x.v = 0;
  if (i % 53 == 0) x.v = x.p;
  return x;
}

uint64_t Fnv(uint64_t h, uint32_t v) {
  for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 0x100000001B3ull;
  return h;
}

uint64_t HashLevels(uint64_t h, const m65::Levels& l) {
  h = Fnv(h, Bits(l.left));
  h = Fnv(h, Bits(l.right));
  h = Fnv(h, Bits(l.volume));
  h = Fnv(h, Bits(l.pan));
  h = Fnv(h, uint32_t(m65::DirectGain(l.left, 1.0f, 1.0f, 1.0f)));
  h = Fnv(h, uint32_t(m65::DirectGain(l.right, 0.8f, 0.7f, 0.9f)));
  h = Fnv(h, uint32_t(m65::WetGain(l.volume, 1.0f, 0.5f, 0.3f)));
  return h;
}

// What the scenarios do, independently of who runs them.
struct Driver {
  virtual ~Driver() = default;
  virtual m65::Levels Fast2D(const Case& c) = 0;
  virtual m65::Levels VolumePan(float v, float p) = 0;
  virtual m65::Levels Levels(float l, float r) = 0;
  virtual float Reported(const m65::Levels& l) = 0;
};

struct Native : Driver {
  m65::Levels Fast2D(const Case& c) override {
    return m65::Fast2D(c.src, m65::MakeListener(c.pose.pos, c.pose.face, c.pose.up));
  }
  m65::Levels VolumePan(float v, float p) override { return m65::FromVolumePan(v, p); }
  m65::Levels Levels(float l, float r) override { return m65::FromLevels(l, r); }
  float Reported(const m65::Levels& l) override { return m65::ReportedVolume(l); }
};

// Runs every scenario on `d`; `check` sees each result with a label (and
// whether it is a read-back, whose volume is in `left`).
template <typename Check>
uint64_t RunScenarios(Driver& d, Check check) {
  uint64_t h = 0xCBF29CE484222325ull;
  std::mt19937 g(65);
  char what[160];
  for (int i = 0; i < kCases; i++) {
    Case c = MakeCase(g, i);
    m65::Levels l = d.Fast2D(c);
    h = HashLevels(h, l);
    std::snprintf(what, sizeof what, "fast2d %d: src (%.9g %.9g %.9g) vol %.9g dist %.9g..%.9g",
                  i, c.src.pos.x, c.src.pos.y, c.src.pos.z, c.src.volume, c.src.min_dist,
                  c.src.max_dist);
    check(what, false, l);
  }
  for (int i = 0; i < kCases; i++) {
    VolPan x = MakeVolPan(g, i);
    m65::Levels l = d.VolumePan(x.v, x.p);
    float rep = d.Reported(l);
    h = HashLevels(h, l);
    h = Fnv(h, Bits(rep));
    std::snprintf(what, sizeof what, "volume_pan %d: %.9g %.9g", i, x.v, x.p);
    check(what, false, l);
    check(what, true, m65::Levels{rep, 0, 0, 0});
  }
  for (int i = 0; i < kCases; i++) {
    VolPan x = MakeLevels(g, i);
    m65::Levels l = d.Levels(x.v, x.p);
    h = HashLevels(h, l);
    std::snprintf(what, sizeof what, "levels %d: %.9g %.9g", i, x.v, x.p);
    check(what, false, l);
  }
  return h;
}

// Recorded from the native code when it matched the lifted original.
constexpr uint64_t kGolden = 0x4B371E2353FE3FB5ull;

}  // namespace

#ifdef FAST2D_ORACLE

namespace {

using recomp::Ctx;

// mss32.dll 6.5e (base 0x21100000).
constexpr uint32_t kSetLevels = 0x2112A810;     // ESI = sample; (left, right)
constexpr uint32_t kSetVolumePan = 0x2112A930;  // ESI = sample; (volume, pan)
constexpr uint32_t kMssPow = 0x21136D30;        // CRT pow: st1 ^ st0
constexpr uint32_t kMssSse2 = 0x2115AD20;       // its SSE2 flag
constexpr uint32_t kMssMatherr = 0x2113AB34;    // its default _matherr: returns 0
constexpr uint32_t kReportPower = 0x2114BD40;   // the read-back's exponent
// msssoft.m3d (base 0x22400000).
constexpr uint32_t kUpdate = 0x22401000;       // ESI = 3D sample
constexpr uint32_t kSetPosition = 0x22401870;  // (object, x, y, z)
constexpr uint32_t kSetOrientation = 0x224019B0;  // (object, face, up)
constexpr uint32_t kSamples = 0x22413224;      // samples in use
constexpr uint32_t kSoftSse2 = 0x22417228;     // its CRT's SSE2 flag

constexpr uint32_t kThunks = 0x7F000000;
constexpr uint32_t kHeap = 0x30000000;
constexpr uint32_t kStackTop = 0x40100000;
constexpr uint32_t kTeb = 0x40200000;
// Guest blocks: the listener object, the 3D sample, the 2D sample.
constexpr uint32_t kListener = kHeap, kSample3D = kHeap + 0x100, kSample2D = kHeap + 0x1000;

recomp::GuestMemory g_mem;
std::map<uint32_t, std::string> g_imports;  // thunk -> name
uint32_t g_status = 4;                      // AIL_sample_status: playing

uint32_t Rd(uint32_t va) { return g_mem.read<uint32_t>(va); }
void Wr(uint32_t va, uint32_t v) { g_mem.write<uint32_t>(va, v); }
float RdF(uint32_t va) {
  uint32_t u = Rd(va);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

}  // namespace

namespace recomp {
uint8_t* g_guest_base = nullptr;

namespace gen {
void f_2112A810(Ctx& c);
void f_2112A930(Ctx& c);
void f_21136D30(Ctx& c);
void f_22401000(Ctx& c);
void f_22401870(Ctx& c);
void f_224019B0(Ctx& c);
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

// mss32's level setters on sample block `s` (ESI), as msssoft's calls
// into its imports reach them.
void CallWithEsi(Ctx& c, void (*fn)(Ctx&), uint32_t s, uint32_t a, uint32_t b) {
  uint32_t esi = c.esi;
  c.esi = s;
  Call(c, fn, {a, b});
  c.esi = esi;
}

}  // namespace

namespace recomp {

void Dispatch(Ctx& c, uint32_t target, uint32_t va) {
  auto it = g_imports.find(target);
  const std::string name = it == g_imports.end() ? "" : it->second;
  uint32_t a1 = Rd(c.esp + 4), a2 = Rd(c.esp + 8), a3 = Rd(c.esp + 12);
  if (target == kMssMatherr) {  // cdecl, reached on pow's domain errors
    c.eax = 0;
    c.esp += 4;
  } else if (name == "_AIL_set_sample_volume_levels@12") {
    CallWithEsi(c, gen::f_2112A810, a1, a2, a3);
    c.esp += 16;
  } else if (name == "_AIL_set_sample_volume_pan@12") {
    CallWithEsi(c, gen::f_2112A930, a1, a2, a3);
    c.esp += 16;
  } else if (name == "_AIL_sample_status@4") {
    c.eax = g_status;
    c.esp += 8;
  } else if (name == "_AIL_set_sample_playback_rate@8") {
    c.esp += 12;
  } else if (name == "_AIL_set_sample_reverb_levels@12") {
    c.esp += 16;
  } else {
    std::printf("unexpected call to %08X (%s) from %08X\n", target, name.c_str(), va);
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

}  // namespace recomp

namespace {

Ctx NewCtx() {
  Ctx c{};
  recomp::x87::Init(c.fpu);
  recomp::x87::SetControlWord(c.fpu, 0x027F);  // Windows' default
  c.mxcsr = 0x1F80;
  c.esp = kStackTop;
  c.fs_base = kTeb;
  return c;
}

uint32_t B(float f) { return Bits(f); }

// The originals: msssoft's update on a 3D sample whose 2D sample is the
// mss32 block kSample2D; mss32's setters on that block directly.
struct Lifted : Driver {
  Ctx c = NewCtx();
  m65::Levels Fields() {
    return {RdF(kSample2D + 0x6C), RdF(kSample2D + 0x70), RdF(kSample2D + 0x64),
            RdF(kSample2D + 0x68)};
  }
  m65::Levels Fast2D(const Case& k) override {
    const Pose& p = k.pose;
    Wr(kSamples, 0);  // no sample in the provider's table: setters update none
    Wr(kListener, 1);  // object type: the listener
    Call(c, recomp::gen::f_22401870, {kListener, B(p.pos.x), B(p.pos.y), B(p.pos.z)});
    Call(c, recomp::gen::f_224019B0, {kListener, B(p.face.x), B(p.face.y), B(p.face.z),
                                      B(p.up.x), B(p.up.y), B(p.up.z)});
    std::memset(g_mem.ptr(kSample3D), 0, 0x68);
    const m65::Source& s = k.src;
    Wr(kSample3D + 0x08, B(s.volume));
    Wr(kSample3D + 0x0C, 22050);  // playback rate
    Wr(kSample3D + 0x10, B(s.pos.x));
    Wr(kSample3D + 0x14, B(s.pos.y));
    Wr(kSample3D + 0x18, B(s.pos.z));
    Wr(kSample3D + 0x44, B(s.max_dist));
    Wr(kSample3D + 0x48, B(s.min_dist));
    Wr(kSample3D + 0x4C, 1);  // has data
    Wr(kSample3D + 0x54, kSample2D);
    Wr(kSample3D + 0x5C, B(1.0f));  // no exclusion
    Wr(kSample3D + 0x60, B(1.0f));  // no obstruction
    // A stale 2D sample, so a missing update shows.
    std::memset(g_mem.ptr(kSample2D), 0xCD, 0x80);
    c.esi = kSample3D;
    Call(c, recomp::gen::f_22401000, {});
    return Fields();
  }
  m65::Levels VolumePan(float v, float p) override {
    std::memset(g_mem.ptr(kSample2D), 0xCD, 0x80);
    CallWithEsi(c, recomp::gen::f_2112A930, kSample2D, B(v), B(p));
    return Fields();
  }
  m65::Levels Levels(float l, float r) override {
    std::memset(g_mem.ptr(kSample2D), 0xCD, 0x80);
    CallWithEsi(c, recomp::gen::f_2112A810, kSample2D, B(l), B(r));
    return Fields();
  }
  // AIL_sample_volume_pan's read-back: pow(volume, 0.6) to a float.
  float Reported(const m65::Levels& l) override {
    namespace x87 = recomp::x87;
    x87::LoadF32(c.fpu, Bits(l.volume));
    x87::LoadF64(c.fpu, g_mem.read<uint64_t>(kReportPower));
    Call(c, recomp::gen::f_21136D30, {});
    uint32_t u = x87::StoreF32(c.fpu);
    x87::Pop(c.fpu);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
};

bool Same(float a, float b) { return (std::isnan(a) && std::isnan(b)) || Bits(a) == Bits(b); }

bool Load(const char* path, uint32_t* next_thunk) {
  recomp::PeImage img;
  std::string err;
  auto bind = [&](const std::string&, const std::string& name) {
    uint32_t t = *next_thunk;
    *next_thunk += 0x10;
    g_imports[t] = name;
    return t;
  };
  if (!img.Load(path, g_mem, &err, 0, bind)) {
    std::printf("%s: %s\n", path, err.c_str());
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: fast2d_oracle_test <mss32.dll (6.5e)> <msssoft.m3d>\n");
    return 2;
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // messages before an abort
  if (!g_mem.Reserve()) return 1;
  recomp::g_guest_base = g_mem.base();
  uint32_t thunk = kThunks;
  if (!Load(argv[1], &thunk) || !Load(argv[2], &thunk)) return 1;
  g_mem.Commit(kHeap, 0x10000, recomp::Prot::RW);
  g_mem.Commit(kStackTop - 0x10000, 0x10000, recomp::Prot::RW);
  g_mem.Commit(kTeb, 0x1000, recomp::Prot::RW);

  int failures = 0;
  uint64_t h = 0;
  for (bool sse2 : {false, true}) {
    // The fresh images have the CRTs' SSE2 flags clear: pow takes its x87
    // path unless set here.
    Wr(kMssSse2, sse2);
    Wr(kSoftSse2, sse2);
    Native native;
    Lifted lifted;
    struct Lockstep : Driver {
      Native* n;
      Lifted* l;
      m65::Levels last[2];  // the original's last result, and its read-back
      m65::Levels Fast2D(const Case& c) override {
        last[0] = l->Fast2D(c);
        return n->Fast2D(c);
      }
      m65::Levels VolumePan(float v, float p) override {
        last[0] = l->VolumePan(v, p);
        return n->VolumePan(v, p);
      }
      m65::Levels Levels(float a, float b) override {
        last[0] = l->Levels(a, b);
        return n->Levels(a, b);
      }
      float Reported(const m65::Levels& x) override {
        last[1] = {l->Reported(last[0]), 0, 0, 0};
        return n->Reported(x);
      }
    } step;
    step.n = &native;
    step.l = &lifted;
    int bad = 0, checks = 0;
    // What the Fast 2D scenarios reached in the original.
    int muted = 0, centred = 0, side = 0, nan = 0;
    h = RunScenarios(step, [&](const char* what, bool readback, const m65::Levels& got) {
      const m65::Levels& want = step.last[readback ? 1 : 0];
      checks++;
      if (std::strncmp(what, "fast2d", 6) == 0) {
        muted += want.left == 0 && want.right == 0 && want.pan == 0.5f;
        centred += want.left == want.right && want.left > 0;
        side += (want.left == 0) != (want.right == 0);
        nan += std::isnan(want.left);
      }
      static const char* names[4] = {"left", "right", "volume", "pan"};
      const float* a = &got.left;
      const float* b = &want.left;
      for (int i = 0; i < 4; i++)
        if (!Same(a[i], b[i])) {
          if (bad++ < 10)
            std::printf("%s pow, %s%s: %s native %.9g (%08X) original %.9g (%08X)\n",
                        sse2 ? "SSE2" : "x87", what, readback ? " (read-back)" : "",
                        names[i], a[i], Bits(a[i]), b[i], Bits(b[i]));
          break;
        }
    });
    std::printf("%s pow: %d of %d results differ from the original (Fast 2D: %d muted, "
                "%d centred, %d on one channel, %d NaN)\n",
                sse2 ? "SSE2" : "x87", bad, checks, muted, centred, side, nan);
    failures += bad;
  }
  if (h != kGolden) std::printf("note: kGolden is %016" PRIX64 "\n", kGolden);
  std::printf("hash %016" PRIX64 "\n%s\n", h, failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}

#else

int main() {
  Native native;
  uint64_t h = RunScenarios(native, [](const char*, bool, const m65::Levels&) {});
  std::printf("hash %016" PRIX64 ", expected %016" PRIX64 "\n", h, kGolden);
  return h == kGolden ? 0 : 1;
}

#endif
