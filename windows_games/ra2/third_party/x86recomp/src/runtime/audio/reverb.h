#pragma once
// Environmental reverb for the software mixer: the EAX listener
// environment as a Creative Audigy 2 ZS renders it, rebuilt from its
// measured impulse responses (tools/eax_capture, docs/eax.md) with
// textbook parts: tapped delay lines for the reflections and the start
// of the late reverb, and a feedback delay network for the rest (Jot
// and Chaigne 1991: per-line absorption filters set from the decay
// times; Schroeder and Moorer's echo comb).
//
// Bus 0 is the open EAX provider's listener environment (Miles 6.5's
// digital master reverb is DigitalReverb). The mixer hands each bus's
// send signal over and adds the wet result to its own mix; all calls
// come from the audio thread with the mixer's lock held.

#include <cstdint>
#include <vector>

namespace recomp::audio {

// One environment, in linear gains and seconds (EAX's mB converted).
// Pans are EAX vectors (x right): their x component moves the
// reflections or the late reverb towards that side.
struct ReverbParams {
  float size = 7.5f;     // environment size, m
  float gain = 0.32f;    // room: the whole reverb
  float gain_lf = 1.0f;  // room LF: the reverb's input at lf_reference
  float decay_time = 1.49f, decay_hf_ratio = 0.83f, decay_lf_ratio = 1.0f;
  float reflections_gain = 0.05f, reflections_delay = 0.007f;
  float reflections_pan[3] = {};
  float late_reverb_gain = 1.26f, late_reverb_delay = 0.011f;
  float late_reverb_pan[3] = {};
  float diffusion = 1.0f;
  float echo_time = 0.25f, echo_depth = 0.0f;
  float modulation_time = 0.25f, modulation_depth = 0.0f;
  float air_absorption_gain_hf = 0.994f;  // per metre; for decay_hf_limit
  float hf_reference = 5000.0f, lf_reference = 250.0f;
  bool decay_hf_limit = true;
};

class Reverb {
 public:
  static constexpr int kBuses = 1;
  static constexpr int kLines = 8;  // feedback delay network size

  void Open(int rate);
  void Close();

  // Takes effect at the next Render.
  void Set(int bus, const ReverbParams& p);

  // sends[b]: `frames` interleaved stereo frames fed to bus b. Adds the
  // wet output of all buses to `out` (interleaved stereo).
  void Render(const float* const* sends, float* out, int frames);

 private:
  // A tap off the input delay line: its delay in frames and its gain on
  // each output channel.
  struct Tap {
    int delay;
    float l, r;
  };
  // The fixed part of a bus's response for its current parameters.
  struct Setup {
    std::vector<Tap> reflections, burst, feed;  // feed: into the network
    float lf_a = 1;                             // input high-pass pole (1: none)
    float burst_a = 0;                          // burst low-pass pole
    int line_len[kLines] = {};                  // frames
    float line_g[kLines] = {};                  // per-pass mid gain
    float hf_a[kLines] = {};                    // per-pass HF low-pass pole
    float lfs_a = 0, lfs_gain[kLines] = {};     // per-pass LF shelf
    float rot_c = 1, rot_s = 0;                 // mixing rotation (diffusion)
    float tail_in = 0, echo_gain = 0, echo_feedback = 0, echo_a = 0;
    float late_l = 1, late_r = 1;  // late reverb pan
    int echo_delay = 1;
    float mod_depth = 0, mod_step = 0;  // frames, radians per frame
  };
  struct Bus {
    ReverbParams params;
    Setup s, prev;  // prev: crossfaded from after a change
    bool dirty = true, fade = false;
    std::vector<float> in;    // input delay line (mono)
    std::vector<float> echo;  // echo comb
    std::vector<float> line[kLines];
    int in_pos = 0, echo_pos = 0, line_pos = 0;
    float lf_x = 0, lf_z = 0, burst_z[2] = {}, feed_z = 0, echo_z = 0, hf_z[kLines] = {},
          lfs_z[kLines] = {};
    double mod_phase = 0;
    int64_t idle = 0;  // frames since the input was last non-zero
    bool quiet = true;
  };
  void Apply(Bus& b);
  void Clear(Bus& b);
  void RenderBus(Bus& b, const float* send, float* out, int frames);

  int rate_ = 0;
  Bus buses_[kBuses];
};

}  // namespace recomp::audio
