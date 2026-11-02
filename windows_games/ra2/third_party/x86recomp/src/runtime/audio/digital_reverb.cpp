#include "runtime/audio/digital_reverb.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// The rounding below is part of the behaviour: a fused multiply-add would
// round once where the original rounds twice.
#pragma STDC FP_CONTRACT OFF

namespace recomp::audio {

namespace {

// Constants from mss32.dll 6.5e (addresses in its image at 0x21100000),
// needed for identical output.
// Comb delays in seconds (0x21142238..0x21142244).
constexpr float kCombDelay[4] = {0.0297f, 0.0371f, 0.0411f, 0.0437f};
// Allpass delays in seconds (0x21142248, 0x2114224C) and the decay times
// their fixed gains are computed for (0x21142250, 0x21142254).
constexpr float kAllpassDelay[2] = {0.005f, 0.0017f};
constexpr float kAllpassDecay[2] = {0.0968535f, 0.032924f};
// Gains are this ^ (delay / decay time): -60 dB after the decay time. It
// is the float 0.001 widened to double (0x2114BD38).
constexpr double kDecayFloor = double(0.001f);
// The damping low-pass cutoff is damping * pi/8 in tan's argument
// (0x2114BD34): damping/8 of the rate.
constexpr float kPiOver8 = 0.392699093f;
// Added to the low-pass's stored output, against denormals (0x2114BD30).
constexpr float kLowPassBias = 1e-10f;
// How much of the first allpass's output the second one takes for the
// left and right channels (0x2114BD2C, 0x2114BD28).
constexpr float kLeftTap = 0.25f, kRightTap = 0.15f;
// Levels and times at or below this count as off (0x2114BD1C).
constexpr float kOff = 1e-4f;
// Tail milliseconds per second (0x2114BE08) and the margin after it.
constexpr float kMsPerSecond = 1000.0f;
constexpr uint32_t kTailMargin = 100;

// Rounds to the nearest integer as a 64-bit value and keeps its low 32
// bits; out of range or NaN gives the x86 integer indefinite (low half 0).
int32_t ToInt(double v) {
  if (!(std::fabs(v) < 9223372036854775808.0)) return 0;
  return int32_t(uint32_t(uint64_t(std::llrint(v))));
}

}  // namespace

// mss32.dll 6.5e 0x21155838: the room types' {wet, time, damping,
// predelay}, used by AIL_set_digital_master_room_type.
const DigitalReverb::Room DigitalReverb::kRoomTable[kRooms] = {
    {0.0f, 1.493f, 0.5f, 0.011f},      // 0 generic
    {0.25f, 0.1f, 0.0f, 0.002f},       // 1 padded cell
    {0.417f, 0.4f, 0.666f, 0.003f},    // 2 room
    {0.75f, 1.499f, 0.166f, 0.011f},   // 3 bathroom
    {0.208f, 0.478f, 0.0f, 0.004f},    // 4 living room
    {0.3f, 2.309f, 0.68f, 0.017f},     // 5 stone room
    {0.403f, 4.279f, 0.5f, 0.03f},     // 6 auditorium
    {0.5f, 3.961f, 0.5f, 0.029f},      // 7 concert hall
    {0.5f, 2.886f, 1.304f, 0.022f},    // 8 cave
    {0.361f, 7.284f, 0.332f, 0.03f},   // 9 arena
    {0.5f, 10.0f, 0.3f, 0.03f},        // 10 hangar
    {0.153f, 0.259f, 0.55f, 0.03f},    // 11 carpeted hallway
    {0.361f, 1.493f, 0.5f, 0.011f},    // 12 hallway
    {0.444f, 2.697f, 0.638f, 0.02f},   // 13 stone corridor
    {0.25f, 1.752f, 0.776f, 0.011f},   // 14 alley
    {0.111f, 3.145f, 0.472f, 0.088f},  // 15 forest
    {0.111f, 2.767f, 0.224f, 0.011f},  // 16 city
    {0.2f, 0.8f, 0.2f, 0.3f},          // 17 mountains
    {0.55f, 3.0f, 0.55f, 0.043f},      // 18 quarry
    {0.12f, 0.8f, 0.18f, 0.1f},        // 19 plain
    {0.208f, 1.652f, 1.5f, 0.012f},    // 20 parking lot
    {0.652f, 2.886f, 0.25f, 0.021f},   // 21 sewer pipe
    {1.0f, 1.499f, 0.1f, 0.011f},      // 22 underwater
    {0.875f, 8.392f, 1.388f, 0.011f},  // 23 drugged
    {0.139f, 10.0f, 1.5f, 0.011f},     // 24 dizzy
    {0.486f, 7.563f, 4.0f, 0.011f},    // 25 psychotic
};

void DigitalReverb::Open(int rate, int fragment, int max_predelay_ms) {
  *this = DigitalReverb();
  rate_ = rate;
  const double r = rate;
  // The predelay ring, in bytes as the original sizes it (the product is
  // taken negated, as it is there), rounded up to whole fragments.
  int64_t bytes = -int64_t(ToInt(double(max_predelay_ms) * double(0.001f) * r * -4.0));
  int64_t frag = int64_t(fragment) * 4;
  ring_.assign(size_t((bytes + frag - 1) / frag * frag / 4), 0);
  for (int i = 0; i < 4; i++) comb_[i].s.assign(size_t(ToInt(double(kCombDelay[i]) * r)), 0.0f);
  for (int i = 0; i < 2; i++)
    allpass_[i].s.assign(size_t(ToInt(double(kAllpassDelay[i]) * r)), 0.0f);
}

void DigitalReverb::Design(float time, float damping) {
  const float per_second = float(1.0 / double(time));
  for (int i = 0; i < 4; i++)
    k_.comb[i] = float(std::pow(kDecayFloor, double(per_second) * double(kCombDelay[i])));
  for (int i = 0; i < 2; i++)
    k_.allpass[i] =
        float(std::pow(kDecayFloor, double(kAllpassDelay[i]) / double(kAllpassDecay[i])));

  // Bilinear-transform Butterworth: a0 = 1 / ((sqrt2 + c) c + 1),
  // b1 = 2 (1 - c^2) a0, b2 = ((1 - sqrt2 c) + c^2) a0. Damping 0 makes
  // c infinite and the coefficients NaN, as in the original.
  const double c = 1.0 / std::tan(double(damping) * double(kPiOver8));
  const double sqrt2 = std::sqrt(2.0);
  const double c2 = c * c;
  const double a0 = 1.0 / ((sqrt2 + c) * c + 1.0);
  const double b1 = (1.0 - c2) * a0;
  k_.a0 = float(a0);
  k_.b1 = float(b1 + b1);
  k_.b2 = float(((1.0 - sqrt2 * c) + c2) * a0);
}

void DigitalReverb::Arm(uint32_t now_ms) {
  timeout_ = now_ms + uint32_t(tail_ms_) + kTailMargin;
  active_ = true;
}

void DigitalReverb::SetReverb(float time, float predelay, float damping, uint32_t now_ms) {
  time_ = time;
  predelay_ = predelay;
  damping_ = damping;
  tail_ms_ = ToInt((double(time) + double(predelay)) * double(kMsPerSecond));
  if (wet_ > kOff && time > kOff) {
    Arm(now_ms);
    // The read position trails the write position by the predelay. The
    // original reads outside its ring past the maximum predelay; here it
    // is clamped to the ring.
    const uint32_t size = uint32_t(ring_.size());
    const uint32_t lag = std::min(uint32_t(ToInt(double(rate_) * double(predelay))), size);
    read_ = write_ < lag ? size - lag + write_ : write_ - lag;
  }
  Design(time_, damping_);
}

void DigitalReverb::SetLevels(float dry, float wet, uint32_t now_ms) {
  dry_ = dry;
  wet_ = wet;
  if (wet > kOff && time_ > kOff) Arm(now_ms);
}

void DigitalReverb::SetRoomType(int room, uint32_t now_ms) {
  const Room& r = kRoomTable[room];
  SetReverb(r.time, r.predelay, r.damping, now_ms);
  SetLevels(1.0f, r.wet, now_ms);
}

void DigitalReverb::Wake(float sample_wet, uint32_t now_ms) {
  if (sample_wet > kOff && wet_ > kOff && time_ > kOff) Arm(now_ms);
}

bool DigitalReverb::Begin(uint32_t now_ms) {
  if (active_ && timeout_ < now_ms) active_ = false;
  return active_;
}

void DigitalReverb::KeepAlive(uint32_t now_ms) {
  const uint32_t tail = uint32_t(tail_ms_) + kTailMargin;
  if (timeout_ - now_ms < tail) timeout_ = now_ms + tail;
}

void DigitalReverb::Render(const int32_t* send, int frames, int32_t* out, bool stereo) {
  const uint32_t size = uint32_t(ring_.size());
  const uint32_t n = uint32_t(frames);
  // The sends go in at the write position...
  for (uint32_t done = 0; done < n;) {
    uint32_t k = std::min(n - done, size - write_);
    std::memcpy(&ring_[write_], send + done, k * sizeof(int32_t));
    done += k;
    write_ = (write_ + k) % size;
  }
  // ...and the reverb reads from the predelayed position.
  for (uint32_t done = 0; done < n;) {
    uint32_t k = std::min(n - done, size - read_);
    Run(&ring_[read_], int(k), out, stereo);
    done += k;
    read_ = (read_ + k) % size;
  }
}

void DigitalReverb::Run(const int32_t* in, int n, int32_t*& out, bool stereo) {
  const Coefficients& k = k_;
  for (int s = 0; s < n; s++) {
    // Damping low-pass.
    const double ax = double(in[s]) * double(k.a0);
    double y = double(ax1_) + double(ax1_);
    y = y + double(ax2_);
    y = y + ax;
    y = y - double(y1_) * double(k.b1);
    y = y - double(y2_) * double(k.b2);
    ax2_ = ax1_;
    ax1_ = float(ax);
    y2_ = y1_;
    y1_ = float(double(kLowPassBias) + y);

    // Combs: the four outputs are summed, then each line takes its
    // feedback plus the new input.
    double sum = double(comb_[1].at()) + double(comb_[0].at());
    sum = sum + double(comb_[3].at());
    sum = sum + double(comb_[2].at());
    const float combs = float(sum);
    for (int i = 0; i < 4; i++) {
      float& c = comb_[i].at();
      c = float(double(k.comb[i]) * double(c) + y);
      comb_[i].Advance();
    }

    // First allpass.
    const double g = k.allpass[0];
    const double a = allpass_[0].at();
    const double v = g * a + double(combs);
    allpass_[0].at() = float(v);
    allpass_[0].Advance();
    const float first = float(a - g * v);

    // Second allpass, fed differently for each channel; its line keeps
    // the left channel's input.
    const double h = k.allpass[1];
    const double b = allpass_[1].at();
    const float fed_back = float(h * b);
    const double left_in = double(first) * double(kLeftTap) + double(fed_back);
    allpass_[1].at() = float(left_in);
    allpass_[1].Advance();
    out[0] = int32_t(uint32_t(out[0]) + uint32_t(ToInt(b - left_in * h)));
    if (stereo) {
      const double right_in = double(first) * double(kRightTap) + double(fed_back);
      out[1] = int32_t(uint32_t(out[1]) + uint32_t(ToInt(b - right_in * h)));
    }
    out += stereo ? 2 : 1;
  }
}

}  // namespace recomp::audio
