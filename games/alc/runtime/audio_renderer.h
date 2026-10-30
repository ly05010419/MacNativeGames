/* nn::audio renderer service (audren:u) wire formats. Layouts follow the public libnx audren.h
   description; per-revision section sizes are cross-checked against the request header at runtime. */
#pragma once
#include <stdint.h>

typedef struct {
    int32_t sample_rate, sample_count, mix_buffer_count, submix_count, voice_count, sink_count, effect_count,
            performance_frame_count;
    uint8_t voice_drop_enabled, unknown, rendering_device, execution_mode;
    int32_t splitter_count, splitter_destination_count;
    uint32_t external_context_size, revision;
} AudrenParameter;
_Static_assert(sizeof(AudrenParameter) == 0x34, "AudioRendererParameter");

typedef struct {
    uint32_t revision, behavior, memory_pools, voices, voice_resources, effects, mixes, sinks, performance,
             splitters, render_info, reserved[4], total;
} AudrenHeader;
_Static_assert(sizeof(AudrenHeader) == 0x40, "UpdateDataHeader");

typedef struct { uint64_t address, size; uint32_t state, pad[3]; } AudrenPoolIn;
typedef struct { uint32_t state, pad[3]; } AudrenPoolOut;
enum { POOL_INVALID, POOL_NEW, POOL_REQUEST_DETACH, POOL_DETACHED, POOL_REQUEST_ATTACH, POOL_ATTACHED, POOL_RELEASED };

typedef struct { uint32_t id; float mix[24]; uint8_t in_use, pad[11]; } AudrenChannelIn;
_Static_assert(sizeof(AudrenChannelIn) == 0x70, "voice channel resource");

typedef struct { uint8_t enable, pad; int16_t numerator[3], denominator[2]; } AudrenBiquad;
typedef struct { uint8_t enable,pad[3]; float numerator[3],denominator[2]; } AudrenBiquadFloat;
_Static_assert(sizeof(AudrenBiquadFloat)==0x18,"REV15 float biquad");
typedef struct {
    uint64_t address, size; int32_t start, end; uint8_t loop, end_of_stream, sent, pad;
    int32_t loop_count; uint64_t context_address, context_size; int32_t loop_start, loop_end;
} AudrenWaveBuffer;
_Static_assert(sizeof(AudrenWaveBuffer) == 0x38, "wave buffer");

typedef struct {
    uint32_t id, node_id; uint8_t is_new, in_use, play_state, sample_format;
    uint32_t sample_rate; int32_t priority, sort_order; uint32_t channel_count; float pitch, volume;
    AudrenBiquad biquads[2]; uint32_t wave_buffer_count; uint16_t wave_buffer_index, pad0; uint32_t pad1;
    uint64_t extra_address, extra_size; int32_t mix_id; uint32_t splitter_id;
    AudrenWaveBuffer wave_buffers[4]; int32_t channel_ids[6];
    uint8_t clear_voice_drop, flush_wave_buffer_count, pad2[2], flags, pad3, src_quality, pad4[17];
} AudrenVoiceIn;
_Static_assert(sizeof(AudrenVoiceIn) == 0x170, "voice in parameter");
/* REV15 enlarges only the two filter records (Q14 -> native float). The
 * surrounding fields retain their order; the tail moves by 24 bytes. */
typedef struct { uint8_t prefix[0x24]; AudrenBiquadFloat biquads[2]; uint8_t tail[0x134]; } AudrenVoiceInV2;
_Static_assert(sizeof(AudrenVoiceInV2)==0x188,"REV15 voice in parameter");
typedef struct { uint64_t played_samples; uint32_t wave_buffers_consumed, voice_drops; } AudrenVoiceOut;
enum { PLAY_STARTED, PLAY_STOPPED, PLAY_PAUSED };
enum { FMT_INVALID, FMT_PCM8, FMT_PCM16, FMT_PCM24, FMT_PCM32, FMT_PCMF32, FMT_ADPCM };

typedef struct {
    float volume; uint32_t sample_rate; int32_t buffer_count; uint8_t in_use, dirty, pad0[2];
    int32_t mix_id; uint32_t effect_count, node_id, pad1[2]; float mix[24][24]; int32_t dest_mix_id; uint32_t dest_splitter_id, pad2;
} AudrenMixIn;
_Static_assert(sizeof(AudrenMixIn) == 0x930, "mix in parameter");

typedef struct {
    uint8_t type, in_use, pad0[2]; uint32_t node_id; uint64_t pad1[3];
    char name[0x100]; uint32_t input_count; int8_t inputs[6]; uint8_t pad2, downmix_enabled; float downmix[4];
} AudrenSinkIn;
_Static_assert(sizeof(AudrenSinkIn) == 0x140, "sink in parameter");
typedef struct { uint32_t last_written_offset, pad0; uint64_t pad1[3]; } AudrenSinkOut;
enum { SINK_INVALID, SINK_DEVICE, SINK_CIRCULAR };

typedef struct { uint8_t type, is_new, enabled, pad0; int32_t mix_id; uint64_t buffer_address, buffer_size; int32_t order; uint32_t pad1; uint8_t specific[0xa0]; } AudrenEffectIn;
_Static_assert(sizeof(AudrenEffectIn) == 0xc0, "effect in parameter");

typedef struct {
    uint8_t input[6], output[6]; uint16_t channel_max, channels;
    int32_t sample_rate, lookahead_max, attack_time, release_time, lookahead;
    float attack, release, threshold, input_gain, output_gain;
    int32_t delay_min, delay_max; uint8_t status, statistics_enabled, statistics_reset, reserved;
} AudrenLimiterParam;
_Static_assert(sizeof(AudrenLimiterParam) == 68, "limiter effect wire layout");

typedef struct {
    uint8_t input[24], output[24]; uint32_t channels, sample_rate, capacity, channel_max;
    uint64_t send_info, send_storage, return_info, return_storage;
    uint32_t sample_size, total_samples, frame_samples;
} AudrenAuxParam;
_Static_assert(__builtin_offsetof(AudrenAuxParam, send_info) == 64, "auxiliary effect wire layout");

void runtime_audio_register(void);
