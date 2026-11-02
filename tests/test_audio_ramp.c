/* Test the volume ramp and depop arithmetic of the Breath of the Wild mixer (games/totk/runtime/audio_ramp.h) with
   synthetic samples; no game data. Build with -DAUDIO_RAMP_STEP for the negative control: volume steps and stops are then
   not smoothed (the old mixer), and the test must fail. */
#include "../games/totk/runtime/audio_ramp.h"
#include <stdio.h>

enum { FRAME = 240 };   /* one 5 ms audio frame at 48 kHz */
#ifdef AUDIO_RAMP_STEP
#define GAIN(x, v0, v1, n) ramp_gain(x, v1, v1, n)
#define MIX(d, src, v0, v1, n) ramp_mix(d, src, v1, v1, n)
#define DECAY 0.0f
#else
#define GAIN(x, v0, v1, n) ramp_gain(x, v0, v1, n)
#define MIX(d, src, v0, v1, n) ramp_mix(d, src, v0, v1, n)
#define DECAY 0.962189f
#endif

static int failures;
static void expect(int ok, const char *what, double got, double limit) {
    if (!ok) { fprintf(stderr, "FAIL: %s (got %g, limit %g)\n", what, got, limit); failures++; }
}
static double max_step(const float *x, int n, float before) {
    double worst = 0, prev = before;
    for (int k = 0; k < n; k++) { double s = fabs(x[k] - prev); if (s > worst) worst = s; prev = x[k]; }
    return worst;
}

int main(void) {
    float voice[FRAME], out[FRAME];
    for (int k = 0; k < FRAME; k++) voice[k] = 0.8f;   /* a loud constant voice, as the music is */

    /* The music ducked under a menu: the voice volume goes from 1 to 0.25 between two frames. */
    float prev_sample = 0.8f, gained[FRAME];
    for (int k = 0; k < FRAME; k++) gained[k] = voice[k];
    GAIN(gained, 1.0f, 0.25f, FRAME);
    expect(max_step(gained, FRAME, prev_sample) <= 0.04, "no sample steps more than 0.04 when the volume ducks", max_step(gained, FRAME, prev_sample), 0.04);
    expect(fabsf(gained[FRAME - 1] - 0.8f * 0.25f) <= 0.8f * 0.75f / FRAME + 1e-4f, "the frame ends one step short of the new volume", gained[FRAME - 1], 0.2);
    expect(fabsf(gained[0] - 0.8f) < 1e-4f, "the frame starts at the last frame's volume", gained[0], 0.8);

    /* A channel mix volume goes to 0 and the mixed samples are added to a buffer that already holds a sound. */
    for (int k = 0; k < FRAME; k++) out[k] = 0.1f;
    float last = MIX(out, voice, 0.5f, 0.0f, FRAME);
    expect(max_step(out, FRAME, 0.1f + 0.8f * 0.5f) <= 0.04, "a mix volume that falls to 0 does not step", max_step(out, FRAME, 0.1f + 0.8f * 0.5f), 0.04);
    expect(fabsf(last - (out[FRAME - 1] - 0.1f)) < 1e-6f, "ramp_mix returns the last sample it added", last, out[FRAME - 1] - 0.1f);

    /* A stopped voice added 0.5 as its last sample: the next frames decay from it, not from 0. */
    for (int k = 0; k < FRAME; k++) out[k] = 0;
    float value = depop_decay(out, 0.5f, DECAY, FRAME);
    expect(max_step(out, FRAME, 0.5f) <= 0.04, "a stopped voice fades without a step", max_step(out, FRAME, 0.1f + 0.8f * 0.5f), 0.04);
    expect(fabsf(out[0] - 0.5f * 0.962189f) < 1e-5f, "the first depop sample is the last sample times the decay", out[0], 0.5 * 0.962189);
    expect(fabsf(value) < 1e-3f, "the depop value is nearly gone after one frame", value, 1e-3);

    if (failures) return 1;
    puts("audio ramp tests passed");
    return 0;
}
