#pragma once
// A Miles-compatible digital master reverb: what Miles Sound System 6.5
// games get from AIL_set_digital_master_reverb, _room_type and
// _reverb_levels and the per-sample/stream wet levels (Miles 6.1 and
// earlier have none). Written from a behavioural description of mss32.dll
// 6.5e's reverb (docs/miles.md, "Miles 6.5e digital reverb", with the
// addresses it was observed at) so that it produces the same output.
//
// Wet sends (driver wet x sample wet x volume x master, mono, not panned)
// collect in a ring of 32-bit samples long enough for the maximum
// predelay. Each mixer fragment, the reverb takes one fragment from the
// ring `predelay` behind where the sends go in, and adds its output to
// the stereo 32-bit mix (16-bit samples x gains out of 2048, so 2^26 is
// full scale). It is a Schroeder reverb: a damping low-pass feeding four
// parallel feedback combs, then two allpasses, the second one tapped
// twice for left and right. It runs while sends keep arriving and for
// time + predelay + 100 ms after the last one, then freezes with its
// state intact until something wakes it.
//
// Matching the output bit for bit needs the same arithmetic: each step
// rounds to double (the x87 at Windows' default 53-bit precision, which
// the original runs at), and values the original keeps as floats are
// rounded to float at the same points. tan and pow come from the host;
// their results are rounded to float and agreed with the original in
// every case tests/digital_reverb_test.cpp compares.

#include <cstdint>
#include <vector>

namespace recomp::audio {

class DigitalReverb {
 public:
  // The room types (EAX environment numbers 0-25).
  struct Room {
    float wet, time, damping, predelay;
  };
  static constexpr int kRooms = 26;
  static const Room kRoomTable[kRooms];

  // The 32-bit mix's full scale for a float sample of 1.0.
  static constexpr float kScale = 67108864.0f;  // 32768 * 2048

  // Driver open: levels dry 1 and wet 1, reverb time 0 (off), all state
  // zero; delay lines for `rate` Hz and a ring holding `max_predelay_ms`
  // rounded up to whole fragments of `fragment` frames.
  void Open(int rate, int fragment, int max_predelay_ms = 500);

  // AIL_set_digital_master_reverb / _reverb_levels / _room_type. `now_ms`
  // is the millisecond clock the tail timeout runs on.
  void SetReverb(float time, float predelay, float damping, uint32_t now_ms);
  void SetLevels(float dry, float wet, uint32_t now_ms);
  void SetRoomType(int room, uint32_t now_ms);
  float time() const { return time_; }
  float predelay() const { return predelay_; }
  float damping() const { return damping_; }
  float dry() const { return dry_; }
  float wet() const { return wet_; }

  // A sample with wet level `sample_wet` starts, resumes, is given new
  // data, or gets new levels while playing: wakes the reverb if the
  // sample, the driver and the reverb time are all non-zero.
  void Wake(float sample_wet, uint32_t now_ms);

  // One mixer fragment: Begin (true: the reverb runs this fragment and
  // takes sends), KeepAlive for each sample mixed with a non-zero send
  // scale, then Render with the summed sends (`frames` mono samples at
  // kScale), which adds the reverb to `out` (`frames` frames, interleaved
  // when stereo).
  bool Begin(uint32_t now_ms);
  void KeepAlive(uint32_t now_ms);
  void Render(const int32_t* send, int frames, int32_t* out, bool stereo);

  // Gains and filter coefficients as floats (for tests).
  struct Coefficients {
    float comb[4], allpass[2];
    float a0, b1, b2;  // y = a0 (x + 2x1 + x2) - b1 y1 - b2 y2
  };
  const Coefficients& coefficients() const { return k_; }

 private:
  // A delay line of floats; `at()` is the oldest sample, replaced as the
  // line advances.
  struct Line {
    std::vector<float> s;
    size_t i = 0;
    float& at() { return s[i]; }
    void Advance() {
      if (++i == s.size()) i = 0;
    }
  };

  void Design(float time, float damping);
  void Arm(uint32_t now_ms);
  void Run(const int32_t* in, int n, int32_t*& out, bool stereo);

  int rate_ = 0;
  float time_ = 0, predelay_ = 0, damping_ = 0;
  float dry_ = 1, wet_ = 1;
  int32_t tail_ms_ = 0;  // time + predelay
  bool active_ = false;
  uint32_t timeout_ = 0;
  std::vector<int32_t> ring_;
  uint32_t write_ = 0, read_ = 0;
  Line comb_[4], allpass_[2];
  // Low-pass history: a0 x of the last two inputs, last two outputs
  // (+1e-10), as floats.
  float ax1_ = 0, ax2_ = 0, y1_ = 0, y2_ = 0;
  Coefficients k_{};
};

}  // namespace recomp::audio
