#pragma once
// EAX (Creative's environmental audio extensions, versions 1-3) as Miles'
// EAX providers take it: listener and source property sets in EAX units
// (millibels, seconds, ratios), the EAX 1.0-3.0 struct layouts games pass
// whole, and how they are rendered here: the listener becomes a Reverb
// bus's ReverbParams, a source becomes LF and 5 kHz levels on the direct
// path and the reverb send of its mixer voice.
//
// Where EAX leaves behaviour to the hardware, this follows measurements of
// a Creative Audigy 2 ZS under Windows 98 SE through Miles 6.5e's
// Msseax.m3d (tools/eax_capture; ARCHITECTURE.md lists what still differs).

#include <cstdint>
#include <string>

#include "runtime/audio/reverb.h"

namespace recomp::audio::eax {

// EAX environments (EAX_ENVIRONMENT_GENERIC .. _PSYCHOTIC); also Miles'
// room types.
inline constexpr uint32_t kEnvironments = 26;

// EAX 3.0 listener properties. EAX 2.0's are a subset (no LF, pans, echo,
// modulation or references; those keep their defaults).
struct Listener {
  uint32_t environment = 0;
  float environment_size = 7.5f, environment_diffusion = 1.0f;
  int32_t room = -1000, room_hf = -100, room_lf = 0;
  float decay_time = 1.49f, decay_hf_ratio = 0.83f, decay_lf_ratio = 1.0f;
  int32_t reflections = -2602;
  float reflections_delay = 0.007f;
  float reflections_pan[3] = {};
  int32_t reverb = 200;
  float reverb_delay = 0.011f;
  float reverb_pan[3] = {};
  float echo_time = 0.25f, echo_depth = 0.0f;
  float modulation_time = 0.25f, modulation_depth = 0.0f;
  float air_absorption_hf = -5.0f;  // mB per metre
  float hf_reference = 5000.0f, lf_reference = 250.0f;
  float room_rolloff_factor = 0.0f;
  uint32_t flags = 0x3F;  // the *SCALE flags and DECAYHFLIMIT (0x20)
  // EAX 1.0's effect volume and damping as the provider reports them.
  // Selecting an environment loads EAX 1.0's own table for these but
  // renders the EAX 2.0 preset; setting them changes the rendering (see
  // ListenerAttribute).
  float volume = 0.5f, damping = 0.5f;
};

// EAX_ENVIRONMENT_UNDEFINED: what EAX 3.0 reports once a single listener
// property has changed the selected environment.
inline constexpr uint32_t kEnvironmentUndefined = 26;

// The listener for an environment (EAX's presets; out-of-range values
// give GENERIC).
Listener Preset(uint32_t environment);

// `version`: the provider's EAX version, 1-3 (EAX 1.0's reverb is louder).
ReverbParams ToReverb(const Listener& l, int version);

// EAX_REVERBPROPERTIES (EAX 1.0: environment, volume, decay time,
// damping; 16 bytes) and EAXLISTENERPROPERTIES as EAX 2.0 (56 bytes) and
// EAX 3.0 (112 bytes) lay them out, read from / written to guest memory
// at `p`.
inline constexpr uint32_t kListener1Size = 16, kListener2Size = 56, kListener3Size = 112;
void ReadListener1(const uint8_t* p, Listener* l);
void WriteListener1(const Listener& l, uint8_t* p);
void ReadListener2(const uint8_t* p, Listener* l);
void WriteListener2(const Listener& l, uint8_t* p);
void ReadListener3(const uint8_t* p, Listener* l);
void WriteListener3(const Listener& l, uint8_t* p);

// EAX 2.0/3.0 source (buffer) properties, plus EAX 1.0's reverb mix.
struct Source {
  int32_t direct = 0, direct_hf = 0, room = 0, room_hf = 0;
  int32_t obstruction = 0;
  float obstruction_lf_ratio = 0.0f;
  int32_t occlusion = 0;
  float occlusion_lf_ratio = 0.25f, occlusion_room_ratio = 1.5f;
  float occlusion_direct_ratio = 1.0f;
  int32_t exclusion = 0;
  float exclusion_lf_ratio = 1.0f;
  int32_t outside_volume_hf = 0;
  float doppler_factor = 0.0f, rolloff_factor = 0.0f, room_rolloff_factor = 0.0f;
  float air_absorption_factor = 1.0f;
  uint32_t flags = 7;  // DIRECTHFAUTO 1, ROOMAUTO 2, ROOMHFAUTO 4
  float reverb_mix = -1.0f;  // EAX 1.0, 0..1; < 0: the default (full)
};

// EAX 2.0's occlusion room ratio default; EAX 3.0 raised it to 1.5. They
// mean the same: EAX 2.0's ratio counts on top of the direct path's
// occlusion, so EAX 2.0's r renders as EAX 3.0's r + 1.
inline constexpr float kOcclusionRoomRatio2 = 0.5f;

// EAXBUFFERPROPERTIES as EAX 2.0 (52 bytes) and EAX 3.0 (72 bytes).
inline constexpr uint32_t kSource2Size = 52, kSource3Size = 72;
void ReadSource2(const uint8_t* p, Source* s);
void WriteSource2(const Source& s, uint8_t* p);
void ReadSource3(const uint8_t* p, Source* s);
void WriteSource3(const Source& s, uint8_t* p);

// Msseax.m3d's attribute names for single listener and source properties
// (AIL_set_3D_provider_preference, AIL_3D_provider_attribute and their
// sample counterparts). `version` is the provider's EAX version, 1-3; each
// name exists for some versions only. Values are S32 mB levels, U32 flags
// or F32 (pans 3 x F32); "EAX environment selection" loads that preset,
// and setting a property has the side effects EAX gives it (the
// environment size rescales the size-dependent properties its flags
// name). Copies between the property and `value`; false if the version
// has no such name.
bool ListenerAttribute(Listener* l, const std::string& name, int version, void* value,
                       bool set);
bool SourceAttribute(Source* s, const std::string& name, int version, void* value,
                     bool set);

// Linear levels of a source's direct path and its send to the listener's
// reverb, below and at the HF reference (the mixer's low-pass takes the
// HF level there, audio::Voice::direct_lf).
struct Levels {
  float direct_lf = 1, direct_hf = 1, room_lf = 1, room_hf = 1;
};
// `distance` and `min_distance` in metres (air absorption, room rolloff);
// `version` the provider's EAX version.
Levels SourceLevels(const Source& s, const Listener& l, float distance,
                    float min_distance, int version);

float MbToGain(float mb);
int32_t GainToMb(float gain);

}  // namespace recomp::audio::eax
