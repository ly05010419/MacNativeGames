/* The volume ramp and depop arithmetic of audio_mixer.c (Breath of the Wild, GAME_BOTW), free of the renderer so that
   tests/test_audio_ramp.c can run it without game data. As Ryujinx.Audio does it (CommandGenerator.GenerateVoice). */
#ifndef AUDIO_RAMP_H
#define AUDIO_RAMP_H
#include <math.h>

/* Each sample of x[0..n) scaled by a gain that moves in n equal steps from v0 (the last frame's value) to v1. */
static inline void ramp_gain(float *x, float v0, float v1, int n) {
    const float step = (v1 - v0) / (float)n; float vol = v0;
    for (int k = 0; k < n; k++) { x[k] *= vol; vol += step; }
}
/* src[k] * the gain ramp from v0 to v1 added to d[k]; returns the last sample added (kept for the depop). */
static inline float ramp_mix(float *d, const float *src, float v0, float v1, int n) {
    const float step = (v1 - v0) / (float)n; float vol = v0, last = 0;
    for (int k = 0; k < n; k++) { last = src[k] * vol; d[k] += last; vol += step; }
    return last;
}
/* A stopped voice's last sample decays by decay per sample (0.962189 at 48 kHz) over the n samples of d; returns the value left. */
static inline float depop_decay(float *d, float value, float decay, int n) {
    for (int k = 0; k < n; k++) { value *= decay; d[k] += value; }
    return value;
}
#endif
