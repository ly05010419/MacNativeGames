/* The spatial audio route decision, the setting, the 5.1 fold and the device-paced mixing steps
   (games/totk/runtime/audio_route.h) without a device or game data. */
#include "../games/totk/runtime/audio_route.h"
#include <math.h>
#include <stdio.h>

static int failures;
#define EXPECT(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static AudioRoute route(int spatial, AudioDeviceInfo d) {
    AudioRouteInputs in = {.spatial = spatial, .system_spatial_ok = 1, .speaker_spatial_ok = 1};
    return audio_route_decide(&in, &d);
}

static void routes(void) {
    /* Devices as Core Audio reported them on an M5 MacBook Pro (macOS 27): AirPods Pro, the built-in speakers, a Studio Display. */
    AudioDeviceInfo airpods = {1, AUDIO_TRANSPORT_BLUETOOTH, 0, 2, 0};
    AudioDeviceInfo speakers = {1, AUDIO_TRANSPORT_BUILTIN, AUDIO_SOURCE_SPEAKERS, 2, 0};
    AudioDeviceInfo jack = {1, AUDIO_TRANSPORT_BUILTIN, AUDIO_SOURCE_HEADPHONES, 2, 0};
    AudioDeviceInfo studio = {1, AUDIO_FOURCC('u', 's', 'b', ' '), 0, 8, 1};
    AudioDeviceInfo usb_dac = {1, AUDIO_FOURCC('u', 's', 'b', ' '), 0, 2, 0};
    AudioDeviceInfo interface8 = {1, AUDIO_FOURCC('u', 's', 'b', ' '), 0, 8, 0};   /* 8 ch without C / LFE labels */
    AudioDeviceInfo airplay = {1, AUDIO_FOURCC('a', 'i', 'r', 'p'), 0, 2, 0};
    AudioDeviceInfo ble = {1, AUDIO_TRANSPORT_BLUETOOTH_LE, 0, 2, 0};
    AudioDeviceInfo none = {0};

    EXPECT(route(AUDIO_SPATIAL_AUTO, airpods) == AUDIO_ROUTE_SYSTEM_SPATIAL, "AirPods: system spatial");
    EXPECT(route(AUDIO_SPATIAL_AUTO, ble) == AUDIO_ROUTE_SYSTEM_SPATIAL, "Bluetooth LE: system spatial");
    EXPECT(route(AUDIO_SPATIAL_AUTO, jack) == AUDIO_ROUTE_SYSTEM_SPATIAL, "headphone jack: system spatial");
    EXPECT(route(AUDIO_SPATIAL_AUTO, speakers) == AUDIO_ROUTE_SPEAKER_SPATIAL, "built-in speakers: AUSpatialMixer");
    EXPECT(route(AUDIO_SPATIAL_AUTO, studio) == AUDIO_ROUTE_SURROUND, "Studio Display: 5.1 direct");
    EXPECT(route(AUDIO_SPATIAL_AUTO, usb_dac) == AUDIO_ROUTE_STEREO, "USB DAC: stereo fold");
    EXPECT(route(AUDIO_SPATIAL_AUTO, interface8) == AUDIO_ROUTE_STEREO, "8 ch without C/LFE: stereo fold");
    EXPECT(route(AUDIO_SPATIAL_AUTO, airplay) == AUDIO_ROUTE_STEREO, "AirPlay: stereo fold");
    EXPECT(route(AUDIO_SPATIAL_AUTO, none) == AUDIO_ROUTE_NONE, "no device");
    AudioDeviceInfo all[] = {airpods, speakers, jack, studio, usb_dac, airplay, none};
    for (unsigned i = 0; i < sizeof all / sizeof *all; i++)
        EXPECT(route(AUDIO_SPATIAL_OFF, all[i]) == AUDIO_ROUTE_LEGACY, "spatial off: legacy for device %u", i);

    /* TOTK_AUDIO_OFF wins over everything (the routing probe relies on it). */
    AudioRouteInputs off = {.spatial = AUDIO_SPATIAL_AUTO, .audio_off = 1, .system_spatial_ok = 1, .speaker_spatial_ok = 1};
    EXPECT(audio_route_decide(&off, &airpods) == AUDIO_ROUTE_NONE, "TOTK_AUDIO_OFF");
    /* Fallbacks when a renderer is unavailable, and the stereo VR tap. */
    AudioRouteInputs no_avsbar = {.spatial = AUDIO_SPATIAL_AUTO, .speaker_spatial_ok = 1};
    EXPECT(audio_route_decide(&no_avsbar, &airpods) == AUDIO_ROUTE_STEREO, "no AVSampleBufferAudioRenderer: stereo");
    AudioRouteInputs no_mixer = {.spatial = AUDIO_SPATIAL_AUTO, .system_spatial_ok = 1};
    EXPECT(audio_route_decide(&no_mixer, &speakers) == AUDIO_ROUTE_STEREO, "no AUSpatialMixer: stereo");
    AudioRouteInputs tap = {.spatial = AUDIO_SPATIAL_AUTO, .tap = 1, .system_spatial_ok = 1, .speaker_spatial_ok = 1};
    EXPECT(audio_route_decide(&tap, &airpods) == AUDIO_ROUTE_STEREO, "VR tap: stereo AudioUnit");
    EXPECT(audio_route_decide(&tap, &studio) == AUDIO_ROUTE_SURROUND, "VR tap on a 5.1 device: as before (5.1)");

    EXPECT(audio_route_device_channels(AUDIO_ROUTE_SURROUND) == 6 && audio_route_device_channels(AUDIO_ROUTE_SYSTEM_SPATIAL) == 6 &&
           audio_route_device_channels(AUDIO_ROUTE_SPEAKER_SPATIAL) == 6 && audio_route_device_channels(AUDIO_ROUTE_STEREO) == 2,
           "device channels per route");
    EXPECT(!strcmp(audio_route_name(AUDIO_ROUTE_SYSTEM_SPATIAL), "system-spatial"), "route names");
}

static void setting(void) {
    EXPECT(audio_spatial_parse("auto") == AUDIO_SPATIAL_AUTO && audio_spatial_parse("on") == AUDIO_SPATIAL_AUTO &&
           audio_spatial_parse("1") == AUDIO_SPATIAL_AUTO, "auto words");
    EXPECT(audio_spatial_parse("0") == AUDIO_SPATIAL_OFF && audio_spatial_parse("off") == AUDIO_SPATIAL_OFF, "off words");
    EXPECT(audio_spatial_parse(NULL) == AUDIO_SPATIAL_UNSET && audio_spatial_parse("") == AUDIO_SPATIAL_UNSET, "unset");
    EXPECT(audio_spatial_parse("yes please") == AUDIO_SPATIAL_UNKNOWN, "unknown word");
    EXPECT(audio_spatial_mode("0", "auto", AUDIO_SPATIAL_AUTO) == AUDIO_SPATIAL_OFF, "the environment wins");
    EXPECT(audio_spatial_mode(NULL, "off", AUDIO_SPATIAL_AUTO) == AUDIO_SPATIAL_OFF, "the file when no environment");
    EXPECT(audio_spatial_mode(NULL, NULL, AUDIO_SPATIAL_AUTO) == AUDIO_SPATIAL_AUTO, "the default (TotK)");
    EXPECT(audio_spatial_mode(NULL, NULL, AUDIO_SPATIAL_OFF) == AUDIO_SPATIAL_OFF, "the default (other games)");
    EXPECT(audio_spatial_mode("bogus", "off", AUDIO_SPATIAL_AUTO) == AUDIO_SPATIAL_OFF, "an unknown env word falls through");
    EXPECT(audio_spatial_mode(NULL, "bogus", AUDIO_SPATIAL_AUTO) == AUDIO_SPATIAL_AUTO, "an unknown file word falls through");
    EXPECT(audio_spatial_game_channels(NULL) == 6 && audio_spatial_game_channels("6") == 6 && audio_spatial_game_channels("2") == 2,
           "game channels with spatial on");
}

static void fold(void) {
    const float def[3] = {1.0f, 0.7071f, 0.7071f};
    float in[6] = {0.1f, 0.2f, 0.3f, 0.9f, 0.4f, 0.5f}, l, r;
    audio_fold_51(in, def, &l, &r);
    EXPECT(fabsf(l - (0.1f + 0.7071f * 0.3f + 0.7071f * 0.4f)) < 1e-6f, "fold left %g", l);
    EXPECT(fabsf(r - (0.2f + 0.7071f * 0.3f + 0.7071f * 0.5f)) < 1e-6f, "fold right %g", r);
    float loud[6] = {1, -1, 1, 1, 1, -1};
    audio_fold_51(loud, def, &l, &r);
    EXPECT(l == 1.0f && r == -1.0f, "fold clips (%g %g)", l, r);
    const float game[3] = {0.5f, 0.25f, 0.0f};
    audio_fold_51(in, game, &l, &r);
    EXPECT(fabsf(l - (0.05f + 0.075f)) < 1e-6f && fabsf(r - (0.1f + 0.075f)) < 1e-6f, "the game's coefficients (%g %g)", l, r);
    float lfe_only[6] = {0, 0, 0, 1, 0, 0};
    audio_fold_51(lfe_only, def, &l, &r);
    EXPECT(l == 0 && r == 0, "LFE dropped, as the mixer's stereo branch does");
}

/* A simulated device pulling `period` frames every `period` frames of time while the renderer paces itself:
   the backlog must stay within [target - period, target] and the renderer must keep up (no underrun). */
static void pacing_sim(uint64_t frame, uint64_t period, uint64_t prefill) {
    const uint64_t ring = 16384, target = audio_pace_target(prefill, period, ring);
    AudioPace p = {0};
    uint64_t rd = 0, wr = 0, underruns = 0, renders = 0, max_backlog = 0;
    /* Prefill as the renderer would at the start: render until the target. */
    while (audio_pace_step(&p, rd, wr, frame, target, 0) == AUDIO_PACE_RENDER) { wr += frame; renders++; }
    for (uint64_t t = 0; t < 48000 * 10; t += period) {   /* ten seconds of device pulls */
        if (wr - rd < period) underruns++;
        rd += (wr - rd < period) ? wr - rd : period;
        int guard = 0;
        while (audio_pace_step(&p, rd, wr, frame, target, 0) == AUDIO_PACE_RENDER && guard++ < 64) { wr += frame; renders++; }
        if (wr - rd > max_backlog) max_backlog = wr - rd;
        EXPECT(wr - rd <= target, "frame %llu period %llu: backlog %llu over target %llu", (unsigned long long)frame,
               (unsigned long long)period, (unsigned long long)(wr - rd), (unsigned long long)target);
    }
    EXPECT(!underruns, "frame %llu period %llu: %llu underruns", (unsigned long long)frame, (unsigned long long)period, (unsigned long long)underruns);
    uint64_t expect = 48000ull * 10 / frame;
    EXPECT(renders + 2 >= expect && renders <= expect + target / frame + 2, "renders %llu for ~%llu frames of audio",
           (unsigned long long)renders, (unsigned long long)expect);
    EXPECT(max_backlog <= target, "max backlog");
}

static void pacing(void) {
    pacing_sim(240, 240, 960);    /* 5 ms device buffer (asked for), 20 ms prefill */
    pacing_sim(240, 512, 960);    /* the device kept 512 frames */
    pacing_sim(240, 480, 0);      /* the AVSampleBufferAudioRenderer feeder's 10 ms chunks, no prefill */
    pacing_sim(160, 160, 640);    /* a 32 kHz renderer: 5 ms frames of 160 */

    /* A stalled consumer: after AUDIO_PACE_STALL timeouts the renderer goes back to the wall clock, and returns to the
       device's pace as soon as the consumer moves. */
    AudioPace p = {0};
    uint64_t target = audio_pace_target(960, 240, 16384), rd = 100, wr = 100 + target;
    EXPECT(audio_pace_step(&p, rd, wr, 240, target, 0) == AUDIO_PACE_WAIT, "full ring: wait");
    for (int i = 0; i < AUDIO_PACE_STALL; i++) EXPECT(audio_pace_step(&p, rd, wr, 240, target, 1) == AUDIO_PACE_WAIT, "timeout %d: still waiting", i);
    EXPECT(audio_pace_step(&p, rd, wr, 240, target, 1) == AUDIO_PACE_CLOCK, "stalled: wall clock");
    EXPECT(audio_pace_step(&p, rd, wr + 240, 240, target, 0) == AUDIO_PACE_CLOCK, "still stalled while the consumer does not move");
    EXPECT(audio_pace_step(&p, rd + 480, wr + 240, 240, target, 0) == AUDIO_PACE_RENDER, "the consumer moved: device pace again");
    EXPECT(!p.stalled && !p.timeouts, "stall state cleared");

    EXPECT(audio_pace_timeout_ns(240, 240, 48000) == 10000000ull, "timeout 240+240 frames = 10 ms");
    EXPECT(audio_pace_timeout_ns(512, 240, 48000) == 15666666ull, "timeout 512+240 frames");
    EXPECT(audio_pace_timeout_ns(0, 240, 48000) == 10000000ull, "timeout at least two frames");
    EXPECT(audio_pace_target(960, 240, 16384) == 1200 && audio_pace_target(20000, 240, 16384) == 16384, "target");
    EXPECT(audio_buffer_frames(240, 15, 4096) == 240 && audio_buffer_frames(240, 256, 4096) == 256 && audio_buffer_frames(240, 15, 128) == 128,
           "buffer frames within the device's range");
}

int main(void) {
    routes(); setting(); fold(); pacing();
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    puts("audio route ok");
    return 0;
}
