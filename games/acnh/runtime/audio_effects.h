/* Additional DSP effects. Wire layouts and algorithms follow Ryujinx.Audio
   (MIT, third_party/licenses/Ryujinx-Audio.txt). No device or guest-memory access. */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct {
    uint8_t input[6], output[6]; uint16_t channel_max, channels;
    uint32_t delay_max, delay, sample_rate, in_gain, feedback, out_gain, dry_gain, spread, lowpass;
    uint8_t status;
} AudrenDelayParam;
typedef struct {
    uint8_t input[6], output[6]; uint16_t channel_max, channels;
    int32_t sample_rate, early_mode, early_gain, pre_delay, late_mode, late_gain,
            decay, hf_decay, coloration, reverb_gain, out_gain, dry_gain;
    uint8_t status;
} AudrenReverbParam;
typedef struct {
    uint8_t input[6], output[6]; uint16_t channel_max, channels; int32_t sample_rate;
    float threshold, ratio; int32_t attack_time, release_time;
    float input_gain, attack, release, output_gain;
    uint8_t status, makeup, statistics, reset_statistics;
} AudrenCompressorParam;
typedef struct { uint8_t input[24], output[24]; float volume[24]; uint32_t count; } AudrenBufferMixParam;
typedef struct { uint8_t input[6], output[6]; int16_t numerator[3], denominator[2]; uint8_t channels, status; } AudrenBiquadEffectV1;
typedef struct { uint8_t input[6], output[6]; uint32_t padding; float numerator[3], denominator[2]; uint8_t channels, status; uint16_t reserved; } AudrenBiquadEffectV2;
_Static_assert(offsetof(AudrenDelayParam,status)==52,"delay wire layout");
_Static_assert(offsetof(AudrenReverbParam,status)==64,"reverb wire layout");
_Static_assert(offsetof(AudrenCompressorParam,status)==52,"compressor wire layout");
_Static_assert(sizeof(AudrenBufferMixParam)==148,"buffer-mix wire layout");
_Static_assert(sizeof(AudrenBiquadEffectV1)==24 && sizeof(AudrenBiquadEffectV2)==40,"biquad wire layouts");
typedef struct AudioEffectDsp AudioEffectDsp;
AudioEffectDsp *audio_effect_create(unsigned type, const void *param, unsigned revision);
void audio_effect_free(AudioEffectDsp *s);
void audio_effect_update(AudioEffectDsp *s, const void *param);
void audio_effect_process(AudioEffectDsp *s, float *const input[6], float *const output[6], int frames);
void audio_effect_statistics(AudioEffectDsp *s, void *output, unsigned bytes);
