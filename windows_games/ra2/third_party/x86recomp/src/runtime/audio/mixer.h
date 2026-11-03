#pragma once
// Software mixer on an SDL3 audio stream: the host side of Miles' digital
// driver and of DirectSound (Voice::dsound). Voices are linear-interpolated, gain-ramped and summed into
// stereo float. Gains follow the mixer of the game's Miles generation
// (Voice::miles), and positional voices get the "Miles Fast 2D Positional
// Audio" treatment (distance attenuation and stereo panning), or with
// Voice::ds3d that of DirectSound3D hardware, which Miles' EAX providers
// use. Voices may also feed the EAX reverb bus, with an LF level and a
// low-pass on the direct path and the send (see Voice::direct_lf), and the
// Miles-compatible digital master reverb (Voice::wet, DigitalReverb).
//
// Locking: Voice fields are shared with the audio thread and may only be
// touched with Mixer::mu() held. The mixer never calls into guest code;
// it only raises Voice::eos for the owner to act on.

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "runtime/audio/digital_reverb.h"
#include "runtime/audio/miles65.h"
#include "runtime/audio/reverb.h"
#include "runtime/audio/source.h"
#include "runtime/audio/vec3.h"

struct SDL_AudioStream;

namespace recomp::audio {

struct Voice {
  enum class State { Idle, Playing, Paused, Done };

  // Whose mixer the gains follow: Miles 6.1c's (balance table; Mssfast.m3d
  // for positional voices) or Miles 6.5's (levels, miles65.h; msssoft.m3d
  // for positional voices).
  enum class Miles : uint8_t { k61, k65 };

  std::unique_ptr<Source> src;
  State state = State::Idle;
  Miles miles = Miles::k61;
  // 6.1c: linear amplitude. 6.5: AIL_set_sample_volume_pan's volume, or a
  // positional voice's AIL_set_3D_sample_volume.
  float volume = 1.0f;
  float pan = 0.5f;  // non-positional voices: 0 left, 0.5 centre, 1 right
  // 6.5: the sample's dry level (the driver's is the digital reverb's);
  // 6.1c voices take theirs in direct_lf/hf.
  float dry = 1.0f;
  float rate = 0;       // source frames per second; 0 = the source's own
  int loops = 1;        // total passes; 0 = forever
  int passes = 0;       // completed passes

  // Positional voices (Miles 3D samples). Coordinates are Miles' (left-
  // handed: x right, y up, z forward).
  bool positional = false;
  // Pan and distance as DirectSound3D hardware does them (measured on an
  // Audigy 2 ZS; Mixer::TargetGains) instead of Miles Fast 2D's.
  bool ds3d = false;
  // DirectSound's own buffers: `volume` is linear, `pan` DSBPAN (-10000 to
  // 10000: hundredths of a dB taken off the other channel); positional
  // ones use the DS3D model with the mixer's rolloff factor.
  bool dsound = false;
  // DS3DMODE_HEADRELATIVE: pos is in the listener's frame.
  bool head_relative = false;
  Vec3 pos;
  float min_dist = 1.0f, max_dist = 1e30f;

  // Linear levels at low frequencies and at 5 kHz (EAX's HF reference)
  // for the direct path and the send to each reverb bus: the LF level is
  // a gain, a lower 5 kHz level a two-pole low-pass (the ones EAX
  // hardware uses for obstruction, occlusion, exclusion, room HF and air
  // absorption; the send's is broader). Sends are not
  // panned or distance-attenuated; they scale with volume and the master.
  // A voice with flat direct levels and no sends takes the plain path.
  float direct_lf = 1.0f, direct_hf = 1.0f;
  float send_lf[Reverb::kBuses] = {}, send_hf[Reverb::kBuses] = {};
  // Send to the digital master reverb (the sample's wet level): mono,
  // scaled by the reverb's wet level, volume and the master as in Miles.
  float wet = 0.0f;

  // Set by the mixer when the last pass ends (state becomes Done). The
  // owner clears it after delivering its end-of-sample callback.
  bool eos = false;

  // Repositions playback to source frame f (also rewinds loop counting).
  void Seek(int64_t f);
  // Source frame playing now.
  int64_t Position() const { return pos_; }

  // ---- mixer-private state ----
  int64_t pos_ = 0;   // absolute source frame of a_
  double frac_ = 0;   // 0..1 between a_ and b_
  bool primed_ = false;
  bool at_end_ = false;  // b_ is past the end of this pass
  float a_[2] = {}, b_[2] = {};
  std::vector<float> buf_;  // decoded frames not yet consumed
  size_t buf_head_ = 0;     // in frames
  float cur_l_ = 0, cur_r_ = 0;
  bool gains_valid_ = false;
  float cur_dlf_ = 1;
  float cur_slf_[Reverb::kBuses] = {};
  // Low-pass coefficients (a1, a2) and per-channel state (y[n-1], y[n-2])
  // of the direct path and each send.
  float cur_da_[2] = {}, dy_[2][2] = {};
  float cur_sa_[Reverb::kBuses][2] = {}, sy_[Reverb::kBuses][2][2] = {};
};

class Mixer {
 public:
  // Opens the default playback device at `rate` Hz stereo. False if no
  // device (the game then runs without sound, as on a soundless PC).
  bool Open(int rate);
  // No device: the caller pulls the mix with Mix() (offline rendering).
  void OpenOffline(int rate);
  void Close();

  std::mutex& mu() { return mu_; }
  // Output rate; 0 until opened (no device: never).
  int rate() const { return rate_; }
  // With mu() held:
  void Add(Voice* v);
  void Remove(Voice* v);
  void set_master(float g) { master_ = g; }
  void SetReverb(int bus, const ReverbParams& p) { reverb_.Set(bus, p); }
  void SetListener(const Vec3& pos, const Vec3& face, const Vec3& up);
  // DirectSound3D's rolloff factor for DS3D voices: distance attenuation
  // is (min_dist / d)^rolloff (1: 6 dB per doubling; 0: none).
  void set_rolloff(float r) { rolloff_ = r; }
  DigitalReverb& digital_reverb() { return digital_; }
  // The millisecond clock of the reverb's tail: the time mixed so far.
  uint32_t now_ms() const { return rate_ ? uint32_t(mixed_ * 1000 / uint64_t(rate_)) : 0; }

  // Called by the audio thread.
  void Mix(float* out, int frames);

 private:
  void Init(int rate);
  void MixVoice(Voice& v, float* out, int frames, bool reverb);
  // Channel gains of `v` now, and the volume its digital reverb send
  // scales by.
  void TargetGains(const Voice& v, float* l, float* r, float* send_volume) const;
  // The low-pass from `table` taking a path from level `lf` to `hf` at
  // 5 kHz.
  void LowPass(const std::vector<std::array<float, 2>>& table, float lf, float hf,
               float* a) const;

  std::mutex mu_;
  SDL_AudioStream* stream_ = nullptr;
  int rate_ = 0;
  float master_ = 1.0f;
  // LowPass coefficients (a1, a2) per 10 mB of HF cut, 0 to -10000.
  std::vector<std::array<float, 2>> low_pass_;
  // The same for reverb sends (a broader low-pass).
  std::vector<std::array<float, 2>> send_low_pass_;
  Reverb reverb_;
  std::vector<float> sends_[Reverb::kBuses];
  DigitalReverb digital_;
  std::vector<float> wet_;  // mono sends to digital_
  std::vector<int32_t> wet_int_, reverb_out_;
  uint64_t mixed_ = 0;  // frames
  Vec3 lpos_, lface_{0, 0, 1}, lup_{0, 1, 0};  // face and up normalised
  float rolloff_ = 1.0f;
  miles65::Listener listener65_;
  std::vector<Voice*> voices_;
};

Mixer& GetMixer();

}  // namespace recomp::audio
