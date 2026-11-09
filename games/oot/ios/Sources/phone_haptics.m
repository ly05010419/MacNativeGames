/* The iPhone's own Taptic Engine as the game's HD rumble while the touch controls are used (2026-10-24).
   The game vibrates two devices (left and right), each a low and a high band; an iPhone has one actuator, so the two sides
   are mixed into one continuous Core Haptics event: the stronger side sets the intensity, its amplitude-weighted frequency
   sets the sharpness (the same 40..1250 Hz log scale as runtime/gamepad_feedback.m). This is a high-definition approximation,
   not the Switch's two independent linear-resonant motors: a hit on the left and a hit on the right feel the same.
   The phone engine is live only while no controller is attached and the app is active; a connected controller takes over
   (its own haptics are runtime/gamepad_feedback.m). TOTK_PHONE_HAPTICS=0 turns it off, TOTK_PHONE_HAPTICS_GAIN=<0..4>
   changes the strength. Values arrive from runtime/hid_feedback.c on the game's threads and only the latest ones are kept;
   a 10 ms timer on a private queue applies them, stops after 150 ms of silence, and treats a value older than 250 ms as
   silence so a paused game does not keep the phone buzzing. */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "phone_settings.h"

#if TARGET_OS_IOS

enum { TICK_MS = 10, STALE_MS = 250, HOLD_MS = 150, EVENT_S = 30, RENEW_MS = 25000 };

static dispatch_queue_t queue;
static dispatch_source_t timer;
static CHHapticEngine *engine;                    /* queue */
static id<CHHapticPatternPlayer> player;          /* queue */
static CHHapticPattern *pattern;                  /* queue */
static BOOL playing;                              /* queue */
static _Atomic int announced;
static float sentIntensity = -1, sentSharpness = 0.5f;
static uint64_t quietSince, playerSince, playingSince, playedNs, retryAt, lastLog;
static pthread_mutex_t wantLock = PTHREAD_MUTEX_INITIALIZER;
static float want[2][4] = {{0, 160, 0, 320}, {0, 160, 0, 320}};
static uint64_t wantAt[2];
static float holdVal[2][4];       /* the last non-trivial value, kept briefly so a pulse shorter than a tick is not lost */
static uint64_t holdUntil[2];
static float strongest;
static uint64_t valuesIn, valuesLogged;
static float gain = 1.5f;
static _Atomic int active;
static BOOL started, enabled;
static _Atomic int noHardwareLogged;

void runtime_phone_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high) {
    if (side > 1) return;
    pthread_mutex_lock(&wantLock);
    uint64_t now = runtime_now_ns();
    want[side][0] = amp_low; want[side][1] = freq_low; want[side][2] = amp_high; want[side][3] = freq_high;
    wantAt[side] = now; valuesIn++;
    float a = amp_low > amp_high ? amp_low : amp_high;
    if (a > strongest) strongest = a;
    if (a >= 0.005f) { memcpy(holdVal[side], want[side], sizeof holdVal[side]); holdUntil[side] = now + 30000000ull; }
    pthread_mutex_unlock(&wantLock);
}

static float intensity_of(const float v[4]) {
    float a = sqrtf(v[0] * v[0] + v[2] * v[2]);
    return a > 0 ? (a < 1 ? a : 1) : 0;
}
/* The Taptic Engine is one actuator, and at the game's lower amplitudes it is barely felt. A perceptual curve plus a
   small floor makes those values noticeable without flattening a strong hit; TOTK_PHONE_HAPTICS_GAIN scales them. */
static float shaped(float raw) {
    if (raw < 0.004f) return 0;
    raw *= gain; if (raw > 1) raw = 1;
    return 0.12f + 0.88f * powf(raw, 0.55f);
}
static float sharpness_of(const float v[4]) {
    float w = v[0] + v[2]; if (w <= 1e-4f) return 0.5f;
    float f = (v[0] * v[1] + v[2] * v[3]) / w; f = f < 40 ? 40 : f > 1250 ? 1250 : f;
    return log2f(f / 40.f) / log2f(1250.f / 40.f);
}
static void stop_all(void) {   /* queue */
    if (player) [player stopAtTime:CHHapticTimeImmediate error:nil];
    player = nil; playing = NO; sentIntensity = -1;
    if (engine) { [engine stopWithCompletionHandler:nil]; engine = nil; }
}
static void forget_engine(void) {   /* queue: the engine stopped or was reset; build it again on the next value */
    player = nil; playing = NO; sentIntensity = -1; engine = nil;
    retryAt = runtime_now_ns() + 3000000000ull;
}
static void failed(const char *what, NSError *err) {   /* queue */
    fprintf(stderr, "[phone-haptics] %s failed: %s (trying again in 3 s)\n", what,
            err ? err.localizedDescription.UTF8String : "no reason given");
    forget_engine();
}
static BOOL ensure_engine(void) {   /* queue */
    if (engine) return YES;
    if (runtime_now_ns() < retryAt) return NO;
    if (!CHHapticEngine.capabilitiesForHardware.supportsHaptics) {
        if (!atomic_exchange(&noHardwareLogged, 1)) fprintf(stderr, "[phone-haptics] this iPhone has no haptic engine\n");
        retryAt = runtime_now_ns() + 60000000000ull;
        return NO;
    }
    NSError *err = nil;
    CHHapticEngine *e = [[CHHapticEngine alloc] initAndReturnError:&err];
    if (!e) { failed("creating the haptic engine", err); return NO; }
    e.playsHapticsOnly = YES;   /* never touch the game's audio session */
    e.resetHandler = ^{ dispatch_async(queue, ^{ forget_engine(); }); };
    e.stoppedHandler = ^(CHHapticEngineStoppedReason reason) { (void)reason; dispatch_async(queue, ^{ forget_engine(); }); };
    if (![e startAndReturnError:&err]) { failed("starting the haptic engine", err); return NO; }
    engine = e;
    if (!pattern) {
        CHHapticEventParameter *intensity = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
        CHHapticEventParameter *sharpness = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f];
        CHHapticEvent *event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[intensity, sharpness]
                                                          relativeTime:0 duration:EVENT_S];
        pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&err];
        if (!pattern) { failed("building the pattern", err); return NO; }
    }
    return YES;
}
static BOOL send_params(float intensity, float sharpness) {   /* queue */
    NSError *err = nil;
    CHHapticDynamicParameter *i = [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:intensity relativeTime:0];
    CHHapticDynamicParameter *s = [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl value:sharpness - 0.5f relativeTime:0];
    if (![player sendParameters:@[i, s] atTime:CHHapticTimeImmediate error:&err]) { failed("updating the player", err); return NO; }
    sentIntensity = intensity; sentSharpness = sharpness;
    return YES;
}
static BOOL start_player(float intensity, float sharpness, uint64_t now) {   /* queue */
    NSError *err = nil;
    id<CHHapticPatternPlayer> p = [engine createPlayerWithPattern:pattern error:&err];
    if (!p) { failed("creating the player", err); return NO; }
    player = p;
    if (!send_params(intensity, sharpness)) return NO;
    if (![p startAtTime:CHHapticTimeImmediate error:&err]) { failed("starting the player", err); return NO; }
    if (!playing) {
        playing = YES; playingSince = now;
        if (!atomic_exchange(&announced, 1)) fprintf(stderr, "[phone-haptics] playing (left and right mixed)\n");
    }
    playerSince = now;
    return YES;
}
static void tick(void) {   /* queue, every 10 ms */
    uint64_t now = runtime_now_ns();
    if (!atomic_load(&active)) return;
    float v0[4], v1[4]; uint64_t at0, at1, hold0, hold1;
    pthread_mutex_lock(&wantLock);
    memcpy(v0, want[0], sizeof v0); memcpy(v1, want[1], sizeof v1); at0 = wantAt[0]; at1 = wantAt[1];
    hold0 = holdUntil[0]; hold1 = holdUntil[1];
    if (hold0 && now < hold0) memcpy(v0, holdVal[0], sizeof v0);
    if (hold1 && now < hold1) memcpy(v1, holdVal[1], sizeof v1);
    pthread_mutex_unlock(&wantLock);
    BOOL fresh0 = (hold0 && now < hold0) || (at0 && now - at0 < STALE_MS * 1000000ull);
    BOOL fresh1 = (hold1 && now < hold1) || (at1 && now - at1 < STALE_MS * 1000000ull);
    float i0 = fresh0 ? intensity_of(v0) : 0, i1 = fresh1 ? intensity_of(v1) : 0;
    const float *loud = i0 >= i1 ? v0 : v1;
    float raw = sqrtf(i0 * i0 + i1 * i1); if (raw > 1) raw = 1;
    float sharpness = sharpness_of(loud), intensity = shaped(raw);
    if (raw < 0.004f) {
        if (!playing) return;
        if (sentIntensity != 0 && !send_params(0, sharpness)) return;
        if (!quietSince) quietSince = now;
        else if (now - quietSince > HOLD_MS * 1000000ull) {
            [player stopAtTime:CHHapticTimeImmediate error:nil]; player = nil;
            playing = NO; playedNs += now - playingSince;
        }
        return;
    }
    quietSince = 0;
    if (!ensure_engine()) return;
    if (!playing || now - playerSince > RENEW_MS * 1000000ull) start_player(intensity, sharpness, now);
    else if (fabsf(intensity - sentIntensity) > 0.01f || fabsf(sharpness - sentSharpness) > 0.02f) send_params(intensity, sharpness);
    if (!lastLog) lastLog = now;
    if (now - lastLog >= 30000000000ull) {
        pthread_mutex_lock(&wantLock); uint64_t n = valuesIn - valuesLogged; float peak = strongest; valuesLogged = valuesIn; strongest = 0; pthread_mutex_unlock(&wantLock);
        if (n) fprintf(stderr, "[phone-haptics] last 30 s: %llu values from the game, strongest amplitude %.2f, played %.1f s\n",
                       (unsigned long long)n, peak, playedNs / 1e9);
        playedNs = 0; lastLog = now;
    }
}

static BOOL controller_connected(void) {
    for (GCController *controller in GCController.controllers)
        if (controller.extendedGamepad) return YES;
    return NO;
}
static void update_active(void) {
    if (!enabled) return;
    BOOL pad = controller_connected();
    BOOL foreground = UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
    BOOL wantOn = foreground && !pad;
    BOOL was = atomic_exchange(&active, wantOn ? 1 : 0);
    if (wantOn && !was) fprintf(stderr, "[phone-haptics] on: the iPhone's Taptic Engine follows the game's rumble\n");
    else if (!wantOn && was) {
        fprintf(stderr, "[phone-haptics] off: %s\n", pad ? "a controller is connected" : "the app is not active");
        dispatch_async(queue, ^{ stop_all(); });
    }
}

void totk_phone_haptics_start(void) {
    if (started) return;
    started = YES;
    enabled = totk_phone_setting_enabled(@"TOTK_PHONE_HAPTICS") && runtime_rumble_enabled();
    if (!enabled) {
        fprintf(stderr, "[phone-haptics] off (phone setting or rumble disabled)\n");
        return;
    }
    const char *g = getenv("TOTK_PHONE_HAPTICS_GAIN");
    if (g) { char *end; double v = strtod(g, &end); if (end != g && !*end && v > 0 && v <= 4) gain = (float)v; }
    queue = dispatch_queue_create("totk.phone-haptics", DISPATCH_QUEUE_SERIAL);
    timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 0), TICK_MS * NSEC_PER_MSEC, 2 * NSEC_PER_MSEC);
    dispatch_source_set_event_handler(timer, ^{ @autoreleasepool { tick(); } });
    dispatch_resume(timer);
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; update_active(); }];
    [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; update_active(); }];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; update_active(); }];
    [center addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; update_active(); }];
    update_active();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ update_active(); });
}

#else

void totk_phone_haptics_start(void) {}
void runtime_phone_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high) {
    (void)side; (void)amp_low; (void)freq_low; (void)amp_high; (void)freq_high;
}

#endif
