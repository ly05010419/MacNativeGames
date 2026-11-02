#include "runtime/audio/miles65.h"

#include <bit>
#include <cmath>
#include <cstdint>

namespace recomp::audio::miles65 {

namespace {

// mss32.dll 6.5e (base 0x21100000) and msssoft.m3d (base 0x22400000).
constexpr double kVolumePower = std::bit_cast<double>(0x3FFAAAAAAAAAAAABull);  // 5/3: 0x2114BD50, 0x2240E340
constexpr double kPanPower = std::bit_cast<double>(0x3FD3333340000000ull);  // float 0.3: 0x2114BD48
constexpr double kRatioPower = std::bit_cast<double>(0x400AAAAAAAAAAAABull);  // 10/3: 0x2114BD60
constexpr double kReportPower = std::bit_cast<double>(0x3FE3333333333333ull);  // 0.6: 0x2114BD40
constexpr float kFloor = std::bit_cast<float>(0x38D1B717u);  // 1e-4: 0x2114BD1C, 0x2240E33C
constexpr double kGainScale = 2048.0;  // 0x2114BDF8
constexpr int kMaxGain = 0x800;
constexpr float kInvPi = std::bit_cast<float>(0x3EA2F983u);  // float 1/pi: 0x2240E338
constexpr double kBehind = 0.75;  // 0x2240E34C

// The x87 storing to a 64-bit integer (nearest even) of which Miles keeps
// the low half; NaN and out-of-range values give the integer indefinite,
// 0x8000000000000000, so 0.
int LowHalfOfRound(double x) {
  if (!(std::fabs(x) < 0x1p63)) return 0;
  return int32_t(uint32_t(uint64_t(std::llrint(x))));
}

// The CRT's pow of a negative base and a non-integer exponent is an x87
// invalid operation: the "real indefinite" QNaN, sign set. A host libm
// returns its FPU's default NaN instead, which on arm64 has the sign
// clear; NaN operands still propagate as they are.
double Pow(double x, double y) {
  double r = std::pow(x, y);
  if (r != r && x == x && y == y) return std::bit_cast<double>(0xFFF8000000000000ull);
  return r;
}

// acos(x) x float(1/pi), multiplied as the x87 does. The CRT's acos is
// atan2(sqrt((1 + x)(1 - x)), x) for |x| < 1, exactly 0 at 1 and NaN (a
// domain error) beyond 1; at -1 it is the x87's 64-bit pi, whose product
// with float(1/pi) rounds to the same double as atan2(0, -1)'s pi.
double AcosOverPi(double x) {
  if (!(std::fabs(x) <= 1.0)) return std::nan("");
  return std::atan2(std::sqrt((1.0 + x) * (1.0 - x)), x) * double(kInvPi);
}

}  // namespace

Levels FromVolumePan(float v, float p) {
  Levels l;
  l.volume = float(Pow(double(v), kVolumePower));
  l.pan = p;
  l.left = float(Pow(1.0 - double(p), kPanPower) * double(l.volume));
  l.right = float(Pow(double(p), kPanPower) * double(l.volume));
  return l;
}

Levels FromLevels(float left, float right) {
  Levels l;
  l.left = left;
  l.right = right;
  if (left > kFloor) {
    double q = Pow(double(right) / double(left), kRatioPower);
    l.pan = float(q / (1.0 + q));
    l.volume = l.pan < kFloor ? left
                              : float(Pow(double(l.pan), -kPanPower) * double(right));
  } else {
    l.pan = 0.0f;
    l.volume = right;
  }
  return l;
}

float ReportedVolume(const Levels& l) {
  return float(Pow(double(l.volume), kReportPower));
}

int DirectGain(float level, float master, float driver_dry, float dry) {
  double d = double(driver_dry) * double(dry);
  int g = LowHalfOfRound(double(level) * double(master) * d * kGainScale);
  return g > kMaxGain ? kMaxGain : g;
}

int WetGain(float volume, float master, float driver_wet, float wet) {
  int g = LowHalfOfRound(double(driver_wet) * double(wet) * double(volume) * double(master) *
                         kGainScale);
  return g > kMaxGain ? kMaxGain : g;
}

Listener MakeListener(const Vec3& pos, const Vec3& face, const Vec3& up) {
  Listener l;
  l.pos = pos;
  l.face = face;
  l.right = {float(double(face.z) * double(up.y) - double(face.y) * double(up.z)),
             float(double(face.x) * double(up.z) - double(face.z) * double(up.x)),
             float(double(face.y) * double(up.x) - double(face.x) * double(up.y))};
  return l;
}

Levels Fast2D(const Source& s, const Listener& l) {
  double dx = double(s.pos.x) - double(l.pos.x);
  double dy = double(s.pos.y) - double(l.pos.y);
  double dz = double(s.pos.z) - double(l.pos.z);
  double d = std::sqrt((dz * dz + dy * dy) + dx * dx);
  if (d > double(s.max_dist)) return FromVolumePan(0.0f, 0.5f);
  float dist = float(d);

  // Volume and distance falloff.
  float g = float(Pow(double(s.volume), kVolumePower));
  float m = s.min_dist < kFloor ? kFloor : s.min_dist;
  if (double(kFloor) + double(m) < double(dist))
    g = float(double(m) / ((double(dist) - double(m)) * double(l.rolloff) + double(m)) *
              double(g));

  // Direction from the listener, normalised.
  Vec3 n{float(dx), float(dy), float(dz)};
  double len = std::sqrt((double(n.x) * n.x + double(n.y) * n.y) + double(n.z) * n.z);
  if (len == 0) {
    n = {1, 0, 0};
  } else {
    double inv = 1.0 / len;
    n = {float(inv * n.x), float(inv * n.y), float(inv * n.z)};
  }
  float front = float((double(l.face.x) * n.x + double(l.face.y) * n.y) + double(l.face.z) * n.z);
  double pan = 0.5;
  if (!(dist <= kFloor))
    pan = AcosOverPi((double(l.right.x) * n.x + double(l.right.y) * n.y) +
                     double(l.right.z) * n.z);
  double v = g;
  if (front < 0) v *= kBehind;
  return FromLevels(float(pan * v), float((1.0 - pan) * v));
}

}  // namespace recomp::audio::miles65
