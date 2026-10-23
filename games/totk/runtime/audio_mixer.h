/* Shared server-side audio renderer state (audio_renderer.c) and the mixer interface (audio_mixer.c). */
#pragma once
#include "audio_renderer.h"
#include "ipc.h"
#include <pthread.h>

/* A voice reaching the end of an end-of-stream buffer: plays on into the next queued one in the same frame (Breath of
   the Wild, 2b6fb60), or stops for the rest of the frame (Tears of the Kingdom, as before 2b6fb60: playing on left 1.1.2
   silent after the title). audio_renderer.c advance_voice and the mixer's walk follow it alike. */
#ifdef GAME_BOTW
#define AUDIO_EOS_PLAYS_ON 1
#else
#define AUDIO_EOS_PLAYS_ON 0
#endif

typedef struct {
    AudrenVoiceIn in;
    AudrenBiquadFloat float_biquads[2]; uint8_t use_float_biquads;
    AudrenWaveBuffer buffers[4]; uint8_t valid[4];
    uint8_t pending[4];            /* sent while the voice was not playing: valid once it plays (the DSP's IsSendToAudioProcessor) */
    uint8_t state;                 /* server play state (SV_*): the client's Start / Stop / Pause, plus Stopping until the next render frame */
    uint32_t flush, flush_from;    /* FlushWaveBuffers requested since the last render frame, from the client's current buffer */
    unsigned index; uint64_t offset; double fraction; int32_t loops_done;
    uint64_t played; uint32_t consumed, drops;
    struct { int16_t history[2]; int32_t buffer; uint64_t at; } adpcm;   /* DSP-ADPCM decode cursor (buffer slot, next sample) and history */
    float biquad_state[2][6][2];   /* filter, channel, transposed direct-form II delays */
} ServerVoice;

enum { SV_STOPPED, SV_STARTED, SV_PAUSED, SV_STOPPING };
typedef struct AudioRenderer {
    AudrenParameter param;
    pthread_mutex_t lock; pthread_t thread; int running;
    uint32_t state, revision, time_limit, event; int logged;
#ifdef GAME_BOTW
    float voice_drop; int voice_drop_set;   /* SetVoiceDropParameter (15.0.0+, Breath of the Wild 1.9.0): kept, never used */
#endif
    uint64_t behavior_flags, elapsed_frames;
    uint64_t stop_released, empty_played, flushed, logged_released;   /* wave buffers given back by Stop / without samples / by FlushWaveBuffers */
    ServerVoice *voices; AudrenPoolIn *pools; AudrenMixIn *mixes; AudrenSinkIn *sinks; AudrenEffectIn *effects; AudrenChannelIn *channels;
    void *mixer;
} AudioRenderer;

void audio_mixer_open(AudioRenderer *r);
void audio_mixer_set_running(AudioRenderer *r, int running);
void audio_mixer_close(AudioRenderer *r);
void audio_mixer_update_routing(AudioRenderer *r, const uint8_t *splitters, uint32_t splitter_bytes, const uint8_t *mixes, uint32_t mix_bytes, int mix_count);
void audio_mixer_update_effects(AudioRenderer *r);
void audio_mixer_effect_result(AudioRenderer *r, int id, void *output, unsigned size);
uint32_t audio_mixer_sink_written(AudioRenderer *r, int id);
void audio_mixer_render(AudioRenderer *r, double seconds);   /* called with r->lock held, after audio_voices_prepare, before voices advance */
void audio_voices_prepare(AudioRenderer *r);                 /* audio_renderer.c: each render frame's voice state changes (flush, stop, start) */
int audio_buffer_empty(const AudrenWaveBuffer *b);           /* no samples to decode: played as soon as it is reached */
void audio_device_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);
