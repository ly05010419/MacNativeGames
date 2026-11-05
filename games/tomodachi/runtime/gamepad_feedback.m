/* The Mac controller's haptics and motion sensors for the game's HD rumble and six-axis sensor (runtime/hid_feedback.c),
   2026-10-17. Only with TOTK_RUMBLE=1 / TOTK_GYRO=1, and only on macOS (iOS: stubs, never on).
   Rumble: each of the game's two vibration devices (left, right) is a Core Haptics engine on that handle of the
   controller (GCHapticsLocalityLeftHandle / RightHandle; a DualSense has both) playing a long continuous event. The
   game's value (two bands, each an amplitude 0..1 at a frequency) sets the event's intensity (TOTK_RUMBLE_GAIN, default 1,
   times the bands' combined amplitude) and sharpness (the bands' amplitude-weighted frequency, 40..1250 Hz on a log scale).
   The game sends values every few milliseconds from its own threads; those only store the latest one, and a timer on our
   own queue applies it 100 times a second. The player stops after 150 ms of silence, and the value counts as silence once
   the game has sent nothing for 250 ms (paused, frozen, window lost focus), so the controller never keeps buzzing alone.
   Motion: each GCMotion sample (a DualSense: 250 a second; acceleration in G with gravity, rotation rate in rad/s) goes
   to runtime/motion_fusion.c, which turns it into the Switch's six-axis states. The DualSense was checked lying on the
   desk 2026-10-17: acceleration (0.00, +0.15, -0.95) G, rotation under 0.004 rad/s, 751 samples in 3 s. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

#if !TARGET_OS_OSX
void runtime_feedback_attach(GCController *c) { (void)c; }
void runtime_feedback_detach(GCController *c) { (void)c; }
void runtime_feedback_quiet(void) {}
void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) { (void)side; (void)al; (void)fl; (void)ah; (void)fh; }
#else
#import <CoreHaptics/CoreHaptics.h>

/* ---- rumble ----
   Simple pattern players (CHHapticPatternPlayer) only: on macOS 26.5.2 (25F84) the advanced player (looping) never gets
   created on a game controller -- gamecontrollerd throws while decoding the pattern ("value for key 'NS.objects' was of
   unexpected class 'AVHapticEvent'"), the client gets NSXPCConnection error 4097 and the engine is dead (later calls
   -4810). Simple players and AHAP data play fine there (2026-10-17, DualSense, intensity 0). So each handle plays one
   30-second continuous event whose intensity and sharpness follow the game through dynamic parameters, and a fresh
   player takes over after 25 s. */
enum { RUMBLE_TICK_MS = 10, RUMBLE_STALE_MS = 250, RUMBLE_HOLD_MS = 150, RUMBLE_EVENT_S = 30, RUMBLE_RENEW_MS = 25000 };
static dispatch_queue_t rumbleQueue;
static dispatch_source_t rumbleTimer;
static GCController *rumblePad;                     /* rumbleQueue */
static CHHapticEngine *engines[2];                  /* rumbleQueue */
static id<CHHapticPatternPlayer> players[2];        /* rumbleQueue */
static CHHapticPattern *pattern;                    /* rumbleQueue: one continuous event, full intensity, middle sharpness */
static BOOL playing[2];
static float sentIntensity[2] = {-1, -1}, sentSharpness[2] = {-1, -1};
static uint64_t quietSince[2], retryAt[2], playedNs[2], playingSince[2], playerSince[2];
static pthread_mutex_t wantLock = PTHREAD_MUTEX_INITIALIZER;
static float want[2][4] = {{0, 160, 0, 320}, {0, 160, 0, 320}};
static uint64_t wantAt[2], wantCount, wantCountLogged;
static float strongest;
static float gain = 1.0f;

void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) {
    if (side > 1) return;
    pthread_mutex_lock(&wantLock);
    want[side][0] = al; want[side][1] = fl; want[side][2] = ah; want[side][3] = fh; wantAt[side] = runtime_now_ns(); wantCount++;
    float a = al > ah ? al : ah; if (a > strongest) strongest = a;
    pthread_mutex_unlock(&wantLock);
}
void runtime_feedback_quiet(void) {
    pthread_mutex_lock(&wantLock); wantAt[0] = wantAt[1] = 0; pthread_mutex_unlock(&wantLock);
}
static float intensity_of(const float v[4]) {
    float a = sqrtf(v[0] * v[0] + v[2] * v[2]) * gain;
    return a > 0 ? (a < 1 ? a : 1) : 0;
}
static float sharpness_of(const float v[4]) {
    float w = v[0] + v[2]; if (w <= 1e-4f) return 0.5f;
    float f = (v[0] * v[1] + v[2] * v[3]) / w; f = f < 40 ? 40 : f > 1250 ? 1250 : f;
    return log2f(f / 40.f) / log2f(1250.f / 40.f);
}
static void forget_side(int side) {   /* rumbleQueue: the engine stopped or was reset; build it again on the next value */
    players[side] = nil; engines[side] = nil; playing[side] = NO; sentIntensity[side] = sentSharpness[side] = -1;
}
static void rumble_failed(int side, const char *what, NSError *err) {
    fprintf(stderr, "[rumble] %s handle: %s failed: %s (trying again in 5 s)\n", side ? "right" : "left", what,
            err ? err.localizedDescription.UTF8String : "no reason given");
    forget_side(side); retryAt[side] = runtime_now_ns() + 5000000000ull;
}
static BOOL ensure_engine(int side) {   /* rumbleQueue */
    if (engines[side]) return YES;
    if (!rumblePad.haptics || runtime_now_ns() < retryAt[side]) return NO;
    NSError *err = nil;
    if (!pattern) {
        CHHapticEventParameter *intensity = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
        CHHapticEventParameter *sharpness = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f];
        CHHapticEvent *event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[intensity, sharpness]
                                                          relativeTime:0 duration:RUMBLE_EVENT_S];
        pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&err];
        if (!pattern) { rumble_failed(side, "building the pattern", err); return NO; }
    }
    GCHapticsLocality where = side ? GCHapticsLocalityRightHandle : GCHapticsLocalityLeftHandle;
    if (![rumblePad.haptics.supportedLocalities containsObject:where]) where = GCHapticsLocalityDefault;   /* a pad with one motor */
    CHHapticEngine *e = [rumblePad.haptics createEngineWithLocality:where];
    if (!e) { rumble_failed(side, "creating the haptic engine", nil); return NO; }
    e.resetHandler = ^{ dispatch_async(rumbleQueue, ^{ forget_side(side); }); };
    e.stoppedHandler = ^(CHHapticEngineStoppedReason reason) { (void)reason; dispatch_async(rumbleQueue, ^{ forget_side(side); }); };
    if (![e startAndReturnError:&err]) { rumble_failed(side, "starting the haptic engine", err); return NO; }
    engines[side] = e;
    return YES;
}
static BOOL send_params(id<CHHapticPatternPlayer> p, int side, float intensity, float sharpness) {   /* rumbleQueue */
    NSError *err = nil;
    CHHapticDynamicParameter *i = [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:intensity relativeTime:0];
    CHHapticDynamicParameter *s = [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl value:sharpness - 0.5f relativeTime:0];
    if (![p sendParameters:@[i, s] atTime:CHHapticTimeImmediate error:&err]) { rumble_failed(side, "updating the player", err); return NO; }
    sentIntensity[side] = intensity; sentSharpness[side] = sharpness;
    return YES;
}
/* A new player already at the value (set before it starts, so it never starts at full strength); replaces the old one. */
static BOOL start_player(int side, float intensity, float sharpness, uint64_t now) {   /* rumbleQueue */
    NSError *err = nil;
    id<CHHapticPatternPlayer> p = [engines[side] createPlayerWithPattern:pattern error:&err];
    if (!p) { rumble_failed(side, "creating the player", err); return NO; }
    if (!send_params(p, side, intensity, sharpness)) return NO;
    if (![p startAtTime:CHHapticTimeImmediate error:&err]) { rumble_failed(side, "starting the player", err); return NO; }
    if (players[side]) [players[side] stopAtTime:CHHapticTimeImmediate error:nil];
    players[side] = p; playerSince[side] = now;
    if (!playing[side]) { playing[side] = YES; playingSince[side] = now; }
    return YES;
}
static void rumble_tick(void) {   /* rumbleQueue, every 10 ms */
    uint64_t now = runtime_now_ns();
    for (int side = 0; side < 2; side++) {
        float v[4]; uint64_t at;
        pthread_mutex_lock(&wantLock); memcpy(v, want[side], sizeof v); at = wantAt[side]; pthread_mutex_unlock(&wantLock);
        float intensity = at && now - at < RUMBLE_STALE_MS * 1000000ull ? intensity_of(v) : 0, sharpness = sharpness_of(v);
        if (intensity < 0.02f) {
            if (!playing[side]) continue;
            if (sentIntensity[side] != 0 && !send_params(players[side], side, 0, sharpness)) continue;
            if (!quietSince[side]) quietSince[side] = now;
            else if (now - quietSince[side] > RUMBLE_HOLD_MS * 1000000ull) {
                [players[side] stopAtTime:CHHapticTimeImmediate error:nil]; players[side] = nil;
                playing[side] = NO; playedNs[side] += now - playingSince[side];
            }
            continue;
        }
        quietSince[side] = 0;
        if (!ensure_engine(side)) continue;
        if (!playing[side] || now - playerSince[side] > RUMBLE_RENEW_MS * 1000000ull) start_player(side, intensity, sharpness, now);
        else if (fabsf(intensity - sentIntensity[side]) > 0.01f || fabsf(sharpness - sentSharpness[side]) > 0.02f) send_params(players[side], side, intensity, sharpness);
    }
    static uint64_t lastLog; if (!lastLog) lastLog = now;
    if (now - lastLog >= 30000000000ull) {
        pthread_mutex_lock(&wantLock); uint64_t n = wantCount - wantCountLogged; float peak = strongest; wantCountLogged = wantCount; strongest = 0; pthread_mutex_unlock(&wantLock);
        for (int side = 0; side < 2; side++) if (playing[side]) { playedNs[side] += now - playingSince[side]; playingSince[side] = now; }
        if (n) fprintf(stderr, "[rumble] last 30 s: %llu values from the game, strongest amplitude %.2f, played left %.1f s / right %.1f s%s\n",
                       (unsigned long long)n, peak, playedNs[0] / 1e9, playedNs[1] / 1e9, rumblePad.haptics ? "" : " (no controller with haptics)");
        playedNs[0] = playedNs[1] = 0; lastLog = now;
    }
}
static void rumble_start(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *g = getenv("TOTK_RUMBLE_GAIN");
        if (g) { char *end; double v = strtod(g, &end); if (end == g || *end || !(v > 0) || v > 4) runtime_fail("TOTK_RUMBLE_GAIN must be a number in (0, 4]"); gain = (float)v; }
        rumbleQueue = dispatch_queue_create("totk.rumble", DISPATCH_QUEUE_SERIAL);
        rumbleTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, rumbleQueue);   /* kept for the life of the process */
        dispatch_source_set_timer(rumbleTimer, dispatch_time(DISPATCH_TIME_NOW, 0), RUMBLE_TICK_MS * NSEC_PER_MSEC, 2 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(rumbleTimer, ^{ @autoreleasepool { rumble_tick(); } });
        dispatch_resume(rumbleTimer);
    });
}

/* ---- motion: the math is in runtime/motion_fusion.c ---- */
static GCController *motionPad;  /* main queue */
static void motion_sample(GCMotion *m) {   /* the controller's handler queue */
    GCAcceleration a = m.acceleration;
    if (m.hasGravityAndUserAcceleration) { a.x = m.gravity.x + m.userAcceleration.x; a.y = m.gravity.y + m.userAcceleration.y; a.z = m.gravity.z + m.userAcceleration.z; }
    const double acc[3] = {a.x, a.y, a.z}, gyro[3] = {m.rotationRate.x, m.rotationRate.y, m.rotationRate.z};
    runtime_motion_feed(acc, gyro, runtime_now_ns());
}

/* ---- controllers ---- */
void runtime_feedback_attach(GCController *c) {   /* main queue */
    const char *name = (c.vendorName ?: @"controller").UTF8String;
    if (runtime_rumble_enabled()) {
        rumble_start();
        if (c.haptics) {
            dispatch_async(rumbleQueue, ^{ for (int s = 0; s < 2; s++) { if (players[s]) [players[s] stopAtTime:CHHapticTimeImmediate error:nil]; forget_side(s); retryAt[s] = 0; } rumblePad = c; });
            NSMutableArray *where = [NSMutableArray array]; for (NSString *l in c.haptics.supportedLocalities) [where addObject:l];
            fprintf(stderr, "[rumble] %s: haptics (%s)\n", name, [where componentsJoinedByString:@", "].UTF8String);
        } else fprintf(stderr, "[rumble] %s has no haptics: no rumble from it\n", name);
        if (c.haptics && getenv("TOTK_RUMBLE_TEST")) {
            /* Diagnostic: a fixed 2 s pattern, so the haptics path can be checked without waiting for a game event. */
            fprintf(stderr, "[rumble] self-test: short pattern in 1 s (left 0.8/160+0.2/320, right swapped; the 250 ms stale policy ends it early)\n");
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1000ull * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                runtime_rumble_set(0, 0.8f, 160, 0.2f, 320);
                runtime_rumble_set(1, 0.2f, 160, 0.8f, 320);
            });
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3000ull * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
                runtime_rumble_set(0, 0, 160, 0, 320); runtime_rumble_set(1, 0, 160, 0, 320);
                fprintf(stderr, "[rumble] self-test: pattern ended\n");
            });
        }
    } else if (c.haptics) {
        fprintf(stderr, "[rumble] %s supports haptics; TOTK_RUMBLE=1 enables HD rumble\n", name);
    }
    if (runtime_gyro_enabled()) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *axes = getenv("TOTK_GYRO_AXES"); int r = runtime_motion_axes(axes);
            if (r < 0) runtime_fail("TOTK_GYRO_AXES must name x, y and z once each, e.g. \"-x,y,-z\"");
            if (r > 0) fprintf(stderr, "[gyro] axes remapped by TOTK_GYRO_AXES=%s\n", axes);
        });
        GCMotion *m = c.motion;
        if (!m || !m.hasRotationRate) { fprintf(stderr, "[gyro] %s has no gyro: no motion from it\n", name); return; }
        if (m.sensorsRequireManualActivation) m.sensorsActive = YES;
        m.valueChangedHandler = ^(GCMotion *changed) { motion_sample(changed); };
        motionPad = c; runtime_motion_live(1);
        fprintf(stderr, "[gyro] %s: motion sensors on (gyro%s)\n", name, m.hasGravityAndUserAcceleration ? ", gravity" : ", acceleration");
    } else if (c.motion) {
        fprintf(stderr, "[gyro] %s has motion sensors; TOTK_GYRO=1 enables gyro\n", name);
    }
}
void runtime_feedback_detach(GCController *c) {   /* main queue */
    if (runtime_rumble_enabled() && rumbleQueue)
        dispatch_async(rumbleQueue, ^{ if (rumblePad == c) { for (int s = 0; s < 2; s++) forget_side(s); rumblePad = nil; } });
    if (motionPad == c) {
        c.motion.valueChangedHandler = nil; motionPad = nil; runtime_motion_live(0);
    }
}
#endif
