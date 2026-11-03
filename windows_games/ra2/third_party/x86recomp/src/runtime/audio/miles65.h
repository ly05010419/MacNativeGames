#pragma once
// Miles Sound System 6.5's sample levels: how its mss32 turns a sample's
// volume and pan into left and right levels and those into mixer gains,
// and how its "Miles Fast 2D Positional Audio" provider (msssoft.m3d)
// turns a 3D sample's position into the levels. Written from the
// behavioural description in docs/miles.md ("Miles 6.5e sample levels
// and Fast 2D", with the addresses it was observed at) so that the gains come out the same. Miles 6.1c works
// differently (balance table; Mixer::TargetGains).
//
// In short: volume v and pan p give levels v^(5/3) (1 - p)^0.3 and
// v^(5/3) p^0.3; Fast 2D gives left = pan x v', right = (1 - pan) x v'
// with pan = acos(right axis . direction) / pi and v' = volume^(5/3)
// x min_dist / distance, x 0.75 behind the listener. A channel's gain is
// level x master x dry levels, out of 2048.
//
// Arithmetic as in the original: each step rounds to double (the x87 at
// Windows' default 53-bit precision), values Miles keeps as floats are
// rounded to float at the same points. pow and atan2 come from the host;
// tests/fast2d_test.cpp compares with the original.

#include "runtime/audio/vec3.h"

namespace recomp::audio::miles65 {

// A sample's level fields.
struct Levels {
  float left = 0, right = 0;  // linear channel levels
  // Derived from the levels: AIL_sample_volume_pan reports them (the
  // volume through ReportedVolume), and the digital reverb send scales
  // by the volume.
  float volume = 0, pan = 0.5f;
};

// AIL_set_sample_volume_pan(v, p), no clamping (every sample starts at
// 1, 0.5).
Levels FromVolumePan(float v, float p);
// AIL_set_sample_volume_levels(left, right).
Levels FromLevels(float left, float right);
// The volume AIL_sample_volume_pan reports.
float ReportedVolume(const Levels& l);

// A channel's gain out of 2048: level x master x driver dry x sample dry,
// capped at 2048.
int DirectGain(float level, float master, float driver_dry, float dry);
// The digital reverb send's scale out of 2048: driver wet x sample wet x
// volume x master, capped at 2048.
int WetGain(float volume, float master, float driver_wet, float wet);

// Fast 2D's listener: face as given (not normalised) and its right axis,
// up x face.
struct Listener {
  Vec3 pos, face{0, 0, 1}, right{1, 0, 0};
  float rolloff = 1.0f;  // the provider's rolloff factor
};
Listener MakeListener(const Vec3& pos, const Vec3& face, const Vec3& up);

// A 3D sample as Fast 2D sees it (volume 1, distances 200 and 1 when its
// data is set).
struct Source {
  Vec3 pos;
  float volume = 1.0f;
  float min_dist = 1.0f, max_dist = 200.0f;
};
// The levels Fast 2D gives the sample's 2D voice. Past max_dist the
// sample is muted (the provider's "Mute at maximum", on by default). Not
// modelled: the original leaves a stopped sample's levels alone there,
// so one resumed out of range keeps its old levels until it next moves.
Levels Fast2D(const Source& s, const Listener& l);

}  // namespace recomp::audio::miles65
