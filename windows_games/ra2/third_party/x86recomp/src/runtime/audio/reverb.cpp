#include "runtime/audio/reverb.h"

#include <algorithm>
#include <cmath>

namespace recomp::audio {

namespace {

constexpr double kPi = 3.14159265358979323846;
// Energy decays 60 dB in the decay time: e^(-kDecay t / T).
constexpr double kDecay = 6.0 * 2.302585092994046;

// What follows was measured on a Creative Audigy 2 ZS (Windows 98 SE,
// Miles 6.5e's EAX 2 and 3 providers; tools/eax_capture), as impulse
// responses of single property changes from GENERIC. Times are after
// the direct sound; levels are dB re a unity direct path with the room
// and the reflections or reverb level at 0 dB, and a tap's sign is its
// polarity relative to the direct sound. Tap levels are the peaks read
// off the responses; the card smears each tap over a few samples, so
// their energy is kReflectionsEnergyDb / kBurstEnergyDb more (fitted to
// the early and late levels). The card's tap times stay the same for
// every environment size (the size scales the EAX properties); only
// small rooms recirculate sooner (kFullSize).

// Reflections, first order: taps after the reflections delay, at fixed
// times or at multiples of a spacing x that grows with the reverb delay
// R: x = 0.974 ms + 0.0951 R (fitted to R = 1.5 to 100 ms).
struct FirstReflection {
  float fixed_ms, x;  // at fixed_ms + x * spacing
  float l_db, r_db;   // kNone: not on that channel
};
constexpr float kNone = -200.0f;
constexpr FirstReflection kFirst[] = {
    {0.0f, 0.0f, kNone, -18.0f},   {0.0f, 1.0f, -18.0f, -18.0f},  {2.67f, 0.0f, -21.1f, -21.1f},
    {0.0f, 1.657f, -18.0f, kNone}, {0.0f, 2.29f, -21.1f, -21.1f},
};
constexpr float kSpacingMs = 0.974f, kSpacingPerDelay = 0.0951f;
constexpr float kReflectionsEnergyDb = 2.45f;

// Reflections, later orders: a decaying sequence of taps at a + b x
// (fitted to R = 11, 50 and 100 ms), dB re -11.6 dB. They carry most of
// the reflections' energy, which ends about 80 ms (at R = 11 ms) after
// the reflections delay.
struct LaterReflection {
  float l_ms, r_ms, x, db, sign;
};
constexpr float kLaterDb = -11.6f;
constexpr LaterReflection kLater[] = {
    {5.31f, 5.31f, 2.93f, 0, 1},      {6.17f, 4.27f, 3.618f, 0, 1},
    {10.21f, 10.21f, 1.989f, -3, 1},  {9.02f, 9.02f, 4.29f, -3, 1},
    {10.6f, 10.6f, 4.87f, -8, -1},    {12.33f, 11.88f, 5.58f, -8, -1},
    {17.73f, 17.73f, 3.98f, -11, -1}, {18.06f, 18.06f, 6.29f, -11, -1},
    {15.91f, 15.91f, 6.80f, -16, 1},  {18.5f, 19.48f, 7.54f, -16, 1},
    {22.1f, 22.1f, 7.53f, -19, 1},    {22.7f, 22.7f, 8.0f, -24, -1},
};

// The late reverb starts after the reverb delay (counted from the
// reflections delay) with a burst of taps at a flat level, whatever the
// decay time, for 44 ms: at full diffusion eight clusters of four per
// channel (measured at a 1.49 s decay, HF ratio 0.83), at none only
// four. The feedback network takes over from there.
struct BurstTap {
  float ms, db, sign;
};
constexpr BurstTap kBurstL[] = {
    {1.312f, -26.4f, 1},  {1.750f, -26.5f, 1},   {2.229f, -23.3f, 1},   {3.500f, -23.4f, 1},
    {3.771f, -26.8f, 1},  {4.208f, -26.7f, -1},  {4.687f, -24.1f, 1},   {5.958f, -23.9f, -1},
    {6.937f, -28.3f, 1},  {7.375f, -28.0f, -1},  {7.854f, -25.1f, -1},  {9.125f, -25.2f, 1},
    {10.958f, -28.7f, 1}, {11.375f, -28.8f, 1},  {11.875f, -25.6f, -1}, {13.125f, -25.6f, -1},
    {16.042f, -26.0f, 1}, {16.479f, -26.1f, 1},  {16.958f, -22.9f, 1},  {18.229f, -23.0f, 1},
    {22.521f, -26.1f, 1}, {22.958f, -25.9f, -1}, {23.437f, -23.2f, 1},  {24.708f, -23.0f, -1},
    {30.792f, -25.6f, 1}, {31.229f, -25.5f, -1}, {31.708f, -22.6f, -1}, {32.979f, -22.6f, 1},
    {41.312f, -26.4f, 1}, {41.750f, -26.3f, 1},  {42.229f, -23.2f, -1}, {43.500f, -23.2f, -1},
};
constexpr BurstTap kBurstR[] = {
    {0.021f, -23.3f, 1},  {1.312f, -26.4f, 1},  {1.750f, -26.5f, 1},   {2.229f, -23.3f, 1},
    {2.479f, -24.1f, 1},  {3.771f, -27.0f, 1},  {4.208f, -26.7f, -1},  {4.687f, -24.0f, 1},
    {5.646f, -25.3f, 1},  {6.937f, -28.4f, 1},  {7.375f, -27.9f, -1},  {7.854f, -25.0f, -1},
    {9.646f, -25.8f, 1},  {10.958f, -28.8f, 1}, {11.375f, -28.9f, 1},  {11.875f, -25.5f, -1},
    {14.750f, -23.0f, 1}, {16.042f, -26.1f, 1}, {16.479f, -26.2f, 1},  {16.958f, -23.0f, 1},
    {21.229f, -23.2f, 1}, {22.521f, -26.2f, 1}, {22.958f, -25.8f, -1}, {23.437f, -23.2f, 1},
    {29.500f, -22.7f, 1}, {30.792f, -25.7f, 1}, {31.229f, -25.4f, -1}, {31.708f, -22.5f, -1},
    {40.021f, -23.3f, 1}, {41.312f, -26.5f, 1}, {41.750f, -26.4f, 1},  {42.229f, -23.2f, -1},
};
constexpr BurstTap kPrimaryL[] = {
    {1.750f, -17.3f, 1}, {4.687f, -14.9f, 1}, {9.125f, -16.2f, 1}, {10.937f, -19.8f, 1}};
constexpr BurstTap kPrimaryR[] = {
    {1.750f, -17.3f, 1}, {4.687f, -14.9f, 1}, {9.646f, -16.7f, 1}, {10.937f, -19.8f, 1}};
constexpr float kBurstEnergyDb = 4.45f;
constexpr double kBurstSeconds = 0.044;
// The burst is low-passed by the decay HF ratio: as much as the HF
// decays beyond the mid band in this time.
constexpr double kBurstHfSeconds = 0.08;

// The tail: energy density like an exponential at the decay time,
// kTailLevel x the burst's at kTailFrom after the reverb delay (as if
// from the burst's centre), present from the first pass through the
// network: about where the burst ends, 80 ms later without diffusion,
// sooner in rooms smaller than kFullSize. The total late energy (burst
// and tail) is the reverb level's whatever the decay time, as at the
// 1.49 s the burst was measured at (the burst's level from 0.3 to 10 s
// follows within 0.5 dB).
constexpr double kTailLevel = 0.43, kTailFrom = 0.03;
constexpr double kRefDecay = 1.49;
constexpr double kDiffusionDelay = 0.08;
constexpr double kFullSize = 3.0;
// Echo depth gives the echo comb up to this share of the tail's energy.
constexpr double kEchoShare = 0.5;
// Line lengths (ms): about 55 to 100 ms, so the tail starts where the
// burst ends.
constexpr double kLineMs[Reverb::kLines] = {53.1, 58.7, 64.3, 69.9, 76.1, 82.9, 91.3, 100.7};
// Input signs per line, and the outputs: both channels read every line,
// with a correlation of 0.7 between them (as the card's late reverb).
constexpr float kInSign[Reverb::kLines] = {1, 1, -1, 1, -1, -1, 1, -1};
constexpr float kOutCorrelation = 0.7f;
// The card's HF decay barely outlasts its mid decay, whatever the ratio.
constexpr double kMaxHfRatio = 1.0;
// A shorter LF decay reaches far up: the LF absorption's shelf turns over
// at kLfCorner x the LF reference (a longer one at the reference).
constexpr double kLfCorner = 3.0;
// The card's LF decay runs up to 12% longer than asked, the most at
// short decay times: x (1 + kLfExtra e^(-T / 3 s)).
constexpr double kLfExtra = 0.12;
// Modulation: the pitch deviation at full depth (not measured).
constexpr double kModDeviation = 0.01;
constexpr float kModCos[Reverb::kLines] = {1,  0.7071068f,  0, -0.7071068f,
                                           -1, -0.7071068f, 0, 0.7071068f};
constexpr float kModSin[Reverb::kLines] = {0, 0.7071068f,  1,  0.7071068f,
                                           0, -0.7071068f, -1, -0.7071068f};
constexpr double kSpeedOfSound = 343.3;

float Db(float db) { return std::pow(10.0f, db / 20.0f); }

// One-pole low-pass y = (1 - a) x + a y1 whose gain at w radians is t
// (t < 1), a in [0, 1).
float LowPassPole(double t, double w) {
  if (!(t < 1.0)) return 0.0f;
  t = std::max(t, 1e-4);
  double t2 = t * t, c = std::cos(w);
  double p = 1.0 - t2 * c, q = 1.0 - t2;
  return float((p - std::sqrt(std::max(p * p - q * q, 0.0))) / q);
}

// Low shelf x + (k - 1) lp(x), lp a one-pole low-pass with pole a: the k
// giving gain t at w radians.
float ShelfGain(double t, double a, double w) {
  // lp(e^jw) = (1 - a) / (1 - a e^-jw)
  double re_d = 1.0 - a * std::cos(w), im_d = a * std::sin(w);
  double den = re_d * re_d + im_d * im_d;
  double pr = (1.0 - a) * re_d / den, pi = -(1.0 - a) * im_d / den;
  double p2 = pr * pr + pi * pi;
  double disc = pr * pr - p2 * (1.0 - t * t);
  if (disc < 0) return float(std::max(1.0 - pr / p2, 0.0));  // nearest reachable
  double u = (-pr + std::sqrt(disc)) / p2;
  return float(std::max(1.0 + u, 0.0));
}

// One-pole high-pass y = (1 + a) / 2 (x - x1) + a y1 whose gain at w
// radians is t (t < 1).
float HighPassPole(double t, double w) {
  if (!(t < 1.0)) return 1.0f;
  auto gain2 = [&](double a) {
    double g = (1 + a) / 2;
    return g * g * 2 * (1 - std::cos(w)) / (1 - 2 * a * std::cos(w) + a * a);
  };
  double lo = -1.0, hi = 1.0;
  for (int k = 0; k < 60; k++) {
    double mid = (lo + hi) / 2;
    (gain2(mid) < t * t ? lo : hi) = mid;
  }
  return float((lo + hi) / 2);
}

// Stereo gains for an EAX pan vector: its x moves the energy across.
void PanGains(const float v[3], float* l, float* r) {
  float x = std::clamp(v[0], -1.0f, 1.0f);
  *l = std::sqrt(1.0f - x);
  *r = std::sqrt(1.0f + x);
}

}  // namespace

void Reverb::Open(int rate) {
  Close();
  rate_ = rate;
  // The longest delays: 0.3 s of reflections delay with 0.1 s of reverb
  // delay and the burst, or the later reflections at the longest
  // spacing; the echo time up to 0.25 s; line modulation up to 4 ms.
  auto pow2 = [](int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
  };
  int in_len = pow2(int(0.55 * rate));
  int line_max = int(std::lround(rate * (kLineMs[kLines - 1] + 6.0) / 1000.0));
  for (Bus& b : buses_) {
    b.in.assign(size_t(in_len), 0.0f);
    b.echo.assign(size_t(pow2(int(0.26 * rate))), 0.0f);
    for (auto& l : b.line) l.assign(size_t(pow2(line_max)), 0.0f);
    b.dirty = true;
    Clear(b);
  }
}

void Reverb::Close() {
  rate_ = 0;
  for (Bus& b : buses_) b = Bus{};
}

void Reverb::Set(int bus, const ReverbParams& p) {
  Bus& b = buses_[bus];
  b.params = p;
  b.dirty = true;
}

void Reverb::Clear(Bus& b) {
  std::fill(b.in.begin(), b.in.end(), 0.0f);
  std::fill(b.echo.begin(), b.echo.end(), 0.0f);
  for (auto& l : b.line) std::fill(l.begin(), l.end(), 0.0f);
  b.lf_x = b.lf_z = 0;
  b.burst_z[0] = b.burst_z[1] = b.feed_z = b.echo_z = 0;
  for (int i = 0; i < kLines; i++) b.hf_z[i] = b.lfs_z[i] = 0;
  b.quiet = true;
}

void Reverb::Apply(Bus& b) {
  b.dirty = false;
  const ReverbParams& p = b.params;
  Setup& s = b.s;
  const double fs = rate_;
  auto frames = [&](double ms) { return std::max(int(std::lround(ms * fs / 1000.0)), 0); };
  const double w_hf =
      2 * kPi * std::clamp(double(p.hf_reference), 1000.0, std::min(20000.0, 0.45 * fs)) / fs;
  const double w_lf = 2 * kPi * std::clamp(double(p.lf_reference), 20.0, 1000.0) / fs;

  float room = std::max(p.gain, 0.0f);
  s.lf_a = HighPassPole(std::clamp(double(p.gain_lf), 1e-5, 1.0), w_lf);

  // Reflections.
  double refl_ms = 1000.0 * std::clamp(double(p.reflections_delay), 0.0, 0.3);
  double rev_ms = 1000.0 * std::clamp(double(p.late_reverb_delay), 0.0, 0.1);
  double spacing = kSpacingMs + kSpacingPerDelay * rev_ms;
  float rl, rr;
  PanGains(p.reflections_pan, &rl, &rr);
  float rg = room * std::max(p.reflections_gain, 0.0f) * Db(kReflectionsEnergyDb);
  s.reflections.clear();
  if (rg > 0) {
    for (const FirstReflection& f : kFirst) {
      Tap t{frames(refl_ms + f.fixed_ms + f.x * spacing), 0, 0};
      if (f.l_db > kNone) t.l = rg * rl * Db(f.l_db);
      if (f.r_db > kNone) t.r = rg * rr * Db(f.r_db);
      s.reflections.push_back(t);
    }
    for (const LaterReflection& f : kLater) {
      float g = rg * f.sign * Db(kLaterDb + f.db);
      s.reflections.push_back({frames(refl_ms + f.l_ms + f.x * spacing), g * rl, 0});
      s.reflections.push_back({frames(refl_ms + f.r_ms + f.x * spacing), 0, g * rr});
    }
  }

  // Decay times per band.
  double decay = std::clamp(double(p.decay_time), 0.1, 20.0);
  double hf_ratio = std::clamp(double(p.decay_hf_ratio), 0.1, 2.0);
  if (p.decay_hf_limit && p.air_absorption_gain_hf < 1.0f && p.air_absorption_gain_hf > 0) {
    // Air absorption's HF decay time: 60 dB at its loss per metre.
    double db_per_s = -20.0 * std::log10(double(p.air_absorption_gain_hf)) * kSpeedOfSound;
    hf_ratio = std::min(hf_ratio, 60.0 / db_per_s / decay);
  }
  hf_ratio = std::clamp(hf_ratio, 0.1, kMaxHfRatio);
  double lf_ratio =
      std::clamp(double(p.decay_lf_ratio), 0.1, 2.0) * (1.0 + kLfExtra * std::exp(-decay / 3.0));

  // The burst at this diffusion, both tap sets merged.
  double diffusion = std::clamp(double(p.diffusion), 0.0, 1.0);
  float cluster = float(std::sqrt(diffusion)),
        primary = float(std::sqrt(1.0 - diffusion * diffusion));
  float ll, lr;
  PanGains(p.late_reverb_pan, &ll, &lr);
  std::vector<Tap> burst;
  auto add = [&](const BurstTap* taps, size_t n, float g, bool right) {
    if (g <= 0) return;
    for (size_t i = 0; i < n; i++) {
      int d = frames(taps[i].ms);
      float v = g * taps[i].sign * Db(taps[i].db);
      auto it =
          std::find_if(burst.begin(), burst.end(), [&](const Tap& t) { return t.delay == d; });
      if (it == burst.end()) {
        burst.push_back({d, 0, 0});
        it = burst.end() - 1;
      }
      (right ? it->r : it->l) += v;
    }
  };
  add(kBurstL, std::size(kBurstL), cluster, false);
  add(kBurstR, std::size(kBurstR), cluster, true);
  add(kPrimaryL, std::size(kPrimaryL), primary, false);
  add(kPrimaryR, std::size(kPrimaryR), primary, true);
  double e_burst = 0, e_ref = 0, e_left = 0;
  for (const Tap& t : burst) {
    e_burst += 0.5 * (double(t.l) * t.l + double(t.r) * t.r);
    e_left += double(t.l) * t.l;
  }
  for (const BurstTap& t : kBurstL) e_ref += 0.5 * Db(2 * t.db);
  for (const BurstTap& t : kBurstR) e_ref += 0.5 * Db(2 * t.db);

  // The tail starts with the first pass through the network: after the
  // shortest line, and later at lower diffusion (the card's first
  // recirculation comes 80 ms later without diffusion).
  double tail_delay = (1.0 - diffusion) * kDiffusionDelay;
  // Small rooms (under kFullSize) recirculate sooner: shorter lines.
  double line_scale = std::clamp(double(p.size) / kFullSize, 0.1, 1.0);
  for (int i = 0; i < kLines; i++)
    s.line_len[i] = std::max(int(std::lround(kLineMs[i] * line_scale * fs / 1000.0)), 2);
  // The tail starts where a full-size room's would, level and all, only
  // `early` sooner; its energy is the full-size one's.
  double t0 = kLineMs[0] / 1000.0, early = t0 * (1.0 - line_scale);
  auto late_energy = [&](double t, double start) {
    return 1.0 + kTailLevel * t / (kDecay * kBurstSeconds) * std::exp(-kDecay * start / t);
  };
  // The late level keeps the energy of the burst and tail measured at
  // kRefDecay.
  float late = room * std::max(p.late_reverb_gain, 0.0f) * Db(kBurstEnergyDb);
  double scale = late * std::sqrt(e_ref * late_energy(kRefDecay, t0) /
                                  (std::max(e_burst, 1e-12) * late_energy(decay, t0 + tail_delay)));
  s.burst.clear();
  s.feed.clear();
  for (Tap t : burst) {
    t.delay += frames(refl_ms + rev_ms);
    t.l *= float(scale);
    t.r *= float(scale);
    if (t.l != 0) s.feed.push_back({t.delay + frames(1000.0 * tail_delay), t.l, 0});
    t.l *= ll;
    t.r *= lr;
    s.burst.push_back(t);
  }
  double excess_db = 60.0 * kBurstHfSeconds * (1.0 / (hf_ratio * decay) - 1.0 / decay);
  s.burst_a = LowPassPole(std::pow(10.0, -excess_db / 20.0), w_hf);

  // The network, fed by the burst's left channel (energy e_left scale^2)
  // tail_delay later. With orthogonal mixing its output density is
  // sum(b^2) x input energy / total line length.
  const float lfs_a = float(std::exp(-w_lf * (lf_ratio < 1.0 ? kLfCorner : 1.0)));
  double total_len = 0;
  for (int i = 0; i < kLines; i++) total_len += s.line_len[i];
  double rho_burst = e_burst / (kBurstSeconds * fs);
  double sum_b2 = kTailLevel * rho_burst * total_len / std::max(e_left, 1e-12) *
                  std::exp(-kDecay * (kTailFrom + tail_delay - early) / decay);
  double echo_share = kEchoShare * std::clamp(double(p.echo_depth), 0.0, 1.0);
  s.tail_in = float(std::sqrt(sum_b2 * (1.0 - echo_share) / kLines));
  for (int i = 0; i < kLines; i++) {
    double len = s.line_len[i];
    double mid_db = -60.0 * len / (decay * fs);
    s.line_g[i] = float(std::pow(10.0, mid_db / 20.0));
    // Jot's absorption: a one-pole low-pass for the HF decay, a low shelf
    // for the LF decay.
    s.hf_a[i] = LowPassPole(std::pow(10.0, mid_db * (1.0 / hf_ratio - 1.0) / 20.0), w_hf);
    s.lfs_gain[i] = ShelfGain(std::pow(10.0, mid_db * (1.0 / lf_ratio - 1.0) / 20.0), lfs_a, w_lf);
  }
  s.lfs_a = lfs_a;
  double theta = diffusion * kPi / 4;
  s.rot_c = float(std::cos(theta));
  s.rot_s = float(std::sin(theta));
  s.late_l = ll;
  s.late_r = lr;

  // Echo: a comb at the echo time, decaying at the decay time, taking
  // part of the tail's energy.
  double echo_t = std::clamp(double(p.echo_time), 0.075, 0.25);
  s.echo_delay = std::max(frames(1000.0 * echo_t), 1);
  double ge = std::pow(10.0, -3.0 * echo_t / decay);
  s.echo_feedback = float(ge);
  s.echo_a = LowPassPole(std::pow(10.0, -3.0 * echo_t / decay * (1.0 / hf_ratio - 1.0)), w_hf);
  double e_tail = kTailLevel * e_burst / kBurstSeconds * decay / kDecay *
                  std::exp(-kDecay * (t0 + tail_delay) / decay);
  double e_comb = std::max(e_left, 1e-12) * ge * ge / (1.0 - ge * ge);
  s.echo_gain = float(std::sqrt(echo_share * e_tail / e_comb) * ge);

  // Modulation of the line lengths, at a fixed pitch deviation.
  double mod_t = std::clamp(double(p.modulation_time), 0.04, 4.0);
  double depth = std::clamp(double(p.modulation_depth), 0.0, 1.0);
  s.mod_depth = float(std::min(depth * kModDeviation * mod_t / (2 * kPi) * fs,
                               std::min(0.004 * fs, s.line_len[0] / 2.0)));
  s.mod_step = float(2 * kPi / (mod_t * fs));
}

void Reverb::RenderBus(Bus& b, const float* send, float* out, int frames) {
  Setup& s = b.s;
  const int in_mask = int(b.in.size()) - 1, echo_mask = int(b.echo.size()) - 1;
  const int line_mask = int(b.line[0].size()) - 1;
  const float out_a = std::sqrt((1.0f + kOutCorrelation) / 2.0f / kLines);
  const float out_b = std::sqrt((1.0f - kOutCorrelation) / 2.0f / kLines);
  float peak = 0;
  for (int n = 0; n < frames; n++) {
    float x = 0.5f * (send[2 * n] + send[2 * n + 1]);
    // Room LF: a high-pass on the input.
    if (s.lf_a < 1.0f) {
      float y = 0.5f * (1.0f + s.lf_a) * (x - b.lf_x) + s.lf_a * b.lf_z;
      b.lf_x = x;
      b.lf_z = y;
      x = y;
    }
    b.in[b.in_pos] = x;
    auto at = [&](int d) { return b.in[(b.in_pos - d) & in_mask]; };
    // The tapped parts: reflections, burst and the network's feed.
    auto taps = [&](const Setup& t, float* o) {
      for (const Tap& tap : t.reflections) {
        float v = at(tap.delay);
        o[0] += tap.l * v;
        o[1] += tap.r * v;
      }
      for (const Tap& tap : t.burst) {
        float v = at(tap.delay);
        o[2] += tap.l * v;
        o[3] += tap.r * v;
      }
      for (const Tap& tap : t.feed) o[4] += tap.l * at(tap.delay);
    };
    float o[5] = {};
    taps(s, o);
    if (b.fade) {
      // Crossfade from the previous parameters' taps over this chunk.
      float p[5] = {}, w = float(n + 1) / float(frames);
      taps(b.prev, p);
      for (int k = 0; k < 5; k++) o[k] = p[k] + w * (o[k] - p[k]);
    }
    float l = o[0], r = o[1], bl = o[2], br = o[3], fd = o[4];
    b.burst_z[0] += (1.0f - s.burst_a) * (bl - b.burst_z[0]);
    b.burst_z[1] += (1.0f - s.burst_a) * (br - b.burst_z[1]);
    bl = b.burst_z[0];
    br = b.burst_z[1];
    l += bl;
    r += br;
    b.feed_z += (1.0f - s.burst_a) * (fd - b.feed_z);
    fd = b.feed_z;

    // The network: read (modulated), absorb, mix, write back with the
    // burst added.
    float v[kLines];
    float ms = 0, mc = 0;
    if (s.mod_depth > 0) {
      ms = float(std::sin(b.mod_phase));
      mc = float(std::cos(b.mod_phase));
    }
    for (int i = 0; i < kLines; i++) {
      // Each line at its own phase, an eighth of a turn apart.
      float pos = float(s.line_len[i]) + s.mod_depth * (ms * kModCos[i] + mc * kModSin[i]);
      const std::vector<float>& ln = b.line[i];
      float y;
      if (s.mod_depth > 0) {
        // Cubic Lagrange interpolation, x0 at the nearer whole delay.
        int ip = int(pos);
        float f = pos - float(ip);
        const float xm = ln[(b.line_pos - ip + 1) & line_mask],
                    x0 = ln[(b.line_pos - ip) & line_mask];
        const float x1 = ln[(b.line_pos - ip - 1) & line_mask],
                    x2 = ln[(b.line_pos - ip - 2) & line_mask];
        y = x0 + f * (-xm / 3.0f - x0 / 2.0f + x1 - x2 / 6.0f +
                      f * ((xm + x1) / 2.0f - x0 + f * ((x2 - xm) / 6.0f + (x0 - x1) / 2.0f)));
      } else {
        y = ln[(b.line_pos - int(pos)) & line_mask];
      }
      y *= s.line_g[i];
      b.hf_z[i] += (1.0f - s.hf_a[i]) * (y - b.hf_z[i]);
      y = b.hf_z[i];
      b.lfs_z[i] += (1.0f - s.lfs_a) * (y - b.lfs_z[i]);
      v[i] = y + (s.lfs_gain[i] - 1.0f) * b.lfs_z[i];
    }
    float tl = 0, tr = 0;
    for (int i = 0; i < kLines; i++) {
      float c = (i & 1) ? out_b : -out_b;
      tl += (out_a + c) * v[i];
      tr += (out_a - c) * v[i];
    }
    // Butterfly rotations: identity at no diffusion, a normalised
    // Hadamard matrix at full.
    for (int span = 1; span < kLines; span <<= 1)
      for (int i = 0; i < kLines; i++)
        if (!(i & span)) {
          float p = v[i], q = v[i + span];
          v[i] = s.rot_c * p + s.rot_s * q;
          v[i + span] = s.rot_c * q - s.rot_s * p;
        }
    float feed = s.tail_in * fd;
    for (int i = 0; i < kLines; i++) b.line[i][b.line_pos] = v[i] + kInSign[i] * feed;

    // Echo comb, fed like the network.
    b.echo_z += (1.0f - s.echo_a) * (b.echo[(b.echo_pos - s.echo_delay) & echo_mask] - b.echo_z);
    float e = b.echo_z;
    b.echo[b.echo_pos] = fd + s.echo_feedback * e;
    float echo = s.echo_gain * e;

    l += s.late_l * (tl + echo);
    r += s.late_r * (tr + echo);
    out[2 * n] += l;
    out[2 * n + 1] += r;
    peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));

    b.in_pos = (b.in_pos + 1) & in_mask;
    b.echo_pos = (b.echo_pos + 1) & echo_mask;
    b.line_pos = (b.line_pos + 1) & line_mask;
    b.mod_phase += s.mod_step;
    if (b.mod_phase > 2 * kPi) b.mod_phase -= 2 * kPi;
  }
  b.fade = false;
  // Silence once the input has been quiet past every delay and the
  // network has died away (also keeps denormals out).
  if (b.idle > int64_t(b.in.size()) && peak < 1e-7f) Clear(b);
}

void Reverb::Render(const float* const* sends, float* out, int frames) {
  if (!rate_ || frames <= 0) return;
  for (int i = 0; i < kBuses; i++) {
    Bus& b = buses_[i];
    if (b.dirty) {
      if (!b.quiet) {
        std::swap(b.prev, b.s);
        b.fade = true;
      }
      Apply(b);
    }
    bool silent = std::all_of(sends[i], sends[i] + 2 * frames, [](float v) { return v == 0; });
    b.idle = silent ? b.idle + frames : 0;
    if (silent && b.quiet) continue;
    b.quiet = false;
    RenderBus(b, sends[i], out, frames);
  }
}

}  // namespace recomp::audio
