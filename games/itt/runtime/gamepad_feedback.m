/* The Mac controller's haptics and motion sensors for the game's HD rumble and six-axis sensor (runtime/hid_feedback.c),
   2026-10-17. On by default (TOTK_RUMBLE=0 / TOTK_GYRO=0 turn them off), and only on macOS (iOS: stubs, never on).
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
void runtime_feedback_set_slot(GCController *c, int slot) { (void)c; (void)slot; }
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
enum { RUMBLE_TICK_MS = 10, RUMBLE_STALE_MS = 250, RUMBLE_HOLD_MS = 150, RUMBLE_EVENT_S = 30, RUMBLE_RENEW_MS = 25000, RUMBLE_SIDES = RUNTIME_MAX_GAMEPADS * 2 };
static dispatch_queue_t rumbleQueue;
static dispatch_source_t rumbleTimer;
static GCController *rumblePads[RUNTIME_MAX_GAMEPADS];       /* rumbleQueue */
static CHHapticEngine *engines[RUMBLE_SIDES];                /* rumbleQueue */
static id<CHHapticPatternPlayer> players[RUMBLE_SIDES];      /* rumbleQueue */
static CHHapticPattern *pattern;                    /* rumbleQueue: one continuous event, full intensity, middle sharpness */
static BOOL playing[RUMBLE_SIDES];
static float sentIntensity[RUMBLE_SIDES], sentSharpness[RUMBLE_SIDES];
static uint64_t quietSince[RUMBLE_SIDES], retryAt[RUMBLE_SIDES], playedNs[RUMBLE_SIDES], playingSince[RUMBLE_SIDES], playerSince[RUMBLE_SIDES];
static pthread_mutex_t wantLock = PTHREAD_MUTEX_INITIALIZER;
static float want[RUMBLE_SIDES][4];
static uint64_t wantAt[RUMBLE_SIDES], wantCount, wantCountLogged;
static float strongest;
static float gain = 1.0f;
static uint8_t hdRumbleSeq[RUNTIME_MAX_GAMEPADS];
static BOOL hdPlaying[RUNTIME_MAX_GAMEPADS];                 /* rumbleQueue */
static uint8_t hdSent[RUNTIME_MAX_GAMEPADS][8];              /* rumbleQueue */
static uint64_t hdSentAt[RUNTIME_MAX_GAMEPADS];              /* rumbleQueue */
static uint64_t hdQuietSince[RUNTIME_MAX_GAMEPADS];          /* rumbleQueue */
static void rumble_start(void);
extern void runtime_gamepad_hid_rumble_send(int slot, const uint8_t *out, size_t len);

static int encode_hd_amp(float a) {
    a *= gain;
    if (a < 0.01f) return 0;
    if (a > 1.0f) a = 1.0f;
    int v;
    if (a < 0.117f) v = (int)roundf((log2f(a * 1000.0f) * 32.0f - 0x60) / (5.0f - a * a) - 1.0f);
    else if (a < 0.23f) v = (int)roundf(log2f(a * 1000.0f) * 32.0f - 0x60 - 0x5c);
    else v = (int)roundf((log2f(a * 1000.0f) * 32.0f - 0x60) * 2.0f - 0xf6);
    if (v < 0) v = 0; else if (v > 0xc8) v = 0xc8;
    return v;
}

/* Switch Pro HD rumble encoding: 4 bytes per actuator (left bytes 2..5, right bytes 6..9 of output report 0x10). */
static void encode_hd_rumble(float al, float fl, float ah, float fh, uint8_t out[4]) {
    if (al < 0.01f && ah < 0.01f) { out[0] = 0x00; out[1] = 0x01; out[2] = 0x40; out[3] = 0x40; return; }
    if (fl < 41.0f) fl = 41.0f; else if (fl > 626.0f) fl = 626.0f;
    if (fh < 82.0f) fh = 82.0f; else if (fh > 1252.0f) fh = 1252.0f;
    int hf_freq = (int)roundf(log2f(fh / 10.0f) * 32.0f) - 0x60;
    int lf_freq = (int)roundf(log2f(fl / 10.0f) * 32.0f) - 0x40;
    if (hf_freq < 0) hf_freq = 0; else if (hf_freq > 0x7f) hf_freq = 0x7f;
    if (lf_freq < 1) lf_freq = 1; else if (lf_freq > 0x7f) lf_freq = 0x7f;
    int hf_amp = encode_hd_amp(ah);
    int lf_amp = encode_hd_amp(al);
    uint16_t hf = (uint16_t)((hf_freq << 2) & 0x1fc) | (uint16_t)((hf_amp << 8) & 0xfe00);
    uint16_t lf_enc = (uint16_t)((lf_amp >> 1) + 0x40);
    uint16_t lf = (uint16_t)(lf_freq & 0x7f) | (uint16_t)((lf_amp & 1) ? 0x8000 : 0) | (uint16_t)((lf_enc & 0xff) << 8);
    out[0] = (uint8_t)(hf & 0xff);
    out[1] = (uint8_t)((hf >> 8) & 0xff);
    out[2] = (uint8_t)(lf & 0xff);
    out[3] = (uint8_t)((lf >> 8) & 0xff);
}

void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) {
    if (side >= RUMBLE_SIDES) return;
    if (runtime_rumble_enabled()) rumble_start();
    pthread_mutex_lock(&wantLock);
    want[side][0] = al; want[side][1] = fl; want[side][2] = ah; want[side][3] = fh; wantAt[side] = runtime_now_ns(); wantCount++;
    float a = al > ah ? al : ah; if (a > strongest) strongest = a;
    pthread_mutex_unlock(&wantLock);
}
void runtime_feedback_quiet(void) {
    pthread_mutex_lock(&wantLock); memset(wantAt, 0, sizeof wantAt); pthread_mutex_unlock(&wantLock);
    if (rumbleQueue) {
        dispatch_async(rumbleQueue, ^{
            for (int p = 0; p < RUNTIME_MAX_GAMEPADS; p++) {
                if (hdPlaying[p]) {
                    uint8_t pkt[10] = {0x10, hdRumbleSeq[p]++ & 0x0f, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
                    runtime_gamepad_hid_rumble_send(p, pkt, sizeof pkt);
                    hdPlaying[p] = NO; hdQuietSince[p] = 0; hdSentAt[p] = 0;
                    memset(hdSent[p], 0, sizeof hdSent[p]);
                }
            }
        });
    }
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
    fprintf(stderr, "[rumble] pad %d %s handle: %s failed: %s (trying again in 5 s)\n", side / 2, (side & 1) ? "right" : "left", what,
            err ? err.localizedDescription.UTF8String : "no reason given");
    forget_side(side); retryAt[side] = runtime_now_ns() + 5000000000ull;
}
static BOOL ensure_engine(int side) {   /* rumbleQueue */
    if (engines[side]) return YES;
    GCController *pad = rumblePads[side / 2];
    if (!pad || !pad.haptics || runtime_now_ns() < retryAt[side]) return NO;
    NSError *err = nil;
    if (!pattern) {
        CHHapticEventParameter *intensity = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
        CHHapticEventParameter *sharpness = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f];
        CHHapticEvent *event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[intensity, sharpness]
                                                          relativeTime:0 duration:RUMBLE_EVENT_S];
        pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&err];
        if (!pattern) { rumble_failed(side, "building the pattern", err); return NO; }
    }
    GCHapticsLocality where = (side & 1) ? GCHapticsLocalityRightHandle : GCHapticsLocalityLeftHandle;
    if (![pad.haptics.supportedLocalities containsObject:where]) where = GCHapticsLocalityDefault;   /* a pad with one motor */
    CHHapticEngine *e = [pad.haptics createEngineWithLocality:where];
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
    for (int pad = 0; pad < RUNTIME_MAX_GAMEPADS; pad++) {
        float l[4], r[4]; uint64_t atL, atR;
        pthread_mutex_lock(&wantLock);
        memcpy(l, want[pad * 2], sizeof l); atL = wantAt[pad * 2];
        memcpy(r, want[pad * 2 + 1], sizeof r); atR = wantAt[pad * 2 + 1];
        pthread_mutex_unlock(&wantLock);
        if (!atL || now - atL >= RUMBLE_STALE_MS * 1000000ull) { l[0] = 0; l[1] = 160; l[2] = 0; l[3] = 320; }
        if (!atR || now - atR >= RUMBLE_STALE_MS * 1000000ull) { r[0] = 0; r[1] = 160; r[2] = 0; r[3] = 320; }
        BOOL active = (l[0] >= 0.01f || l[2] >= 0.01f || r[0] >= 0.01f || r[2] >= 0.01f);
        uint8_t enc[8];
        encode_hd_rumble(l[0], l[1], l[2], l[3], enc);
        encode_hd_rumble(r[0], r[1], r[2], r[3], enc + 4);
        if (!active) {
            if (!hdPlaying[pad]) continue;
            if (memcmp(enc, hdSent[pad], sizeof enc) != 0) {
                uint8_t pkt[10] = {0x10, hdRumbleSeq[pad]++ & 0x0f};
                memcpy(pkt + 2, enc, sizeof enc);
                runtime_gamepad_hid_rumble_send(pad, pkt, sizeof pkt);
                memcpy(hdSent[pad], enc, sizeof enc);
                hdSentAt[pad] = now;
            }
            if (!hdQuietSince[pad]) hdQuietSince[pad] = now;
            else if (now - hdQuietSince[pad] > RUMBLE_HOLD_MS * 1000000ull) {
                uint8_t pkt[10] = {0x10, hdRumbleSeq[pad]++ & 0x0f, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
                runtime_gamepad_hid_rumble_send(pad, pkt, sizeof pkt);
                hdPlaying[pad] = NO; hdQuietSince[pad] = 0; hdSentAt[pad] = 0;
                memset(hdSent[pad], 0, sizeof hdSent[pad]);
            }
            continue;
        }
        hdQuietSince[pad] = 0;
        if (!hdPlaying[pad] || memcmp(enc, hdSent[pad], sizeof enc) != 0 || now - hdSentAt[pad] >= 120000000ull) {
            uint8_t pkt[10] = {0x10, hdRumbleSeq[pad]++ & 0x0f};
            memcpy(pkt + 2, enc, sizeof enc);
            runtime_gamepad_hid_rumble_send(pad, pkt, sizeof pkt);
            memcpy(hdSent[pad], enc, sizeof enc);
            hdSentAt[pad] = now;
            hdPlaying[pad] = YES;
        }
    }
    for (int side = 0; side < RUMBLE_SIDES; side++) {
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
        for (int side = 0; side < RUMBLE_SIDES; side++) if (playing[side]) { playedNs[side] += now - playingSince[side]; playingSince[side] = now; }
        if (n) fprintf(stderr, "[rumble] last 30 s: %llu values from the game, strongest amplitude %.2f, p1 %.1f/%.1f s, p2 %.1f/%.1f s\n",
                       (unsigned long long)n, peak, playedNs[0] / 1e9, playedNs[1] / 1e9, playedNs[2] / 1e9, playedNs[3] / 1e9);
        memset(playedNs, 0, sizeof playedNs); lastLog = now;
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
static void hid_motion_start(void);
static void hid_motion_stop(void);
static uint64_t zeroSince;          /* the controller's handler queue */
static BOOL motionSeen, hidAsked;   /* the controller's handler queue; reset on attach */
static void motion_sample(GCMotion *m) {   /* the controller's handler queue */
    GCAcceleration a = m.acceleration;
    if (m.hasGravityAndUserAcceleration) { a.x = m.gravity.x + m.userAcceleration.x; a.y = m.gravity.y + m.userAcceleration.y; a.z = m.gravity.z + m.userAcceleration.z; }
    const double acc[3] = {a.x, a.y, a.z}, gyro[3] = {m.rotationRate.x, m.rotationRate.y, m.rotationRate.z};
    if (!motionSeen) {   /* a real sensor never reads exactly zero on all six axes; see hid_motion_start */
        if (!acc[0] && !acc[1] && !acc[2] && !gyro[0] && !gyro[1] && !gyro[2]) {
            uint64_t now = runtime_now_ns();
            if (!zeroSince) zeroSince = now;
            else if (!hidAsked && now - zeroSince > 1000000000ull) { hidAsked = YES; dispatch_async(dispatch_get_main_queue(), ^{ hid_motion_start(); }); }
            return;
        }
        motionSeen = YES;
    }
    runtime_motion_feed(acc, gyro, runtime_now_ns());
}

/* ---- motion straight from a Switch Pro Controller's HID reports (2026-10-19) ----
   An 8BitDo controller in Switch mode (057e:2009 "Pro Controller" over Bluetooth) gets a GCMotion with hasRotationRate,
   and GameController delivers its samples (33 a second) -- but every one is exactly zero, moved or not, while the same
   controller's raw 0x30 reports carry live six-axis data (gyro up to 550 deg/s, about 1 G at rest) and its SPI factory
   calibration at 0x6020 is valid. So once GameController's motion has been all zeros for a second, it is read from the
   HID reports instead, as SDL does for emulators. Input report 0x30 holds three samples, oldest first, int16 little
   endian from byte 13: acceleration x, y, z, then rotation x, y, z, on the raw axes x toward the top edge, y to the
   left, z out of the face, acceleration as the push against gravity (lying face up z = +1 G). Scales from the SPI
   calibration (the user calibration at 0x8026 when it has its 0xB2A1 magic, else the factory one at 0x6020), as SDL:
   G = raw * 4 / (sensitivity - origin), deg/s = (raw - origin) * 936 / (sensitivity - origin); until it arrives the
   sensor's nominal ranges, 8 G and 2000 deg/s over 32768. */
#import <IOKit/hid/IOHIDManager.h>
static IOHIDManagerRef hidManager;   /* main queue */
static IOHIDDeviceRef hidDevice;     /* main queue */
static uint8_t hidReport[64];
static uint64_t hidLast;
static BOOL hidUserCalibration;
static double hidAccScale[3] = {8.0 / 32768, 8.0 / 32768, 8.0 / 32768}, hidGyroScale[3] = {2000.0 / 32768, 2000.0 / 32768, 2000.0 / 32768}, hidGyroOrigin[3];
static int16_t le16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static void hid_calibration(const uint8_t *c, const char *which) {
    /* Origins are rest offsets: a few dozen steps on a Nintendo controller. An 8BitDo reports -2048, -2048, 2048 (acceleration)
       and -2048, -2048, 2047 (gyro) there, placeholders that are no rest value (at rest it reads 0, 0, 1 G): taking them
       as origins adds about 110 deg/s of constant rotation and makes the scales 12 % off. Such an origin counts as 0. */
    int accOrigin[3], gyroOrigin[3];
    for (int i = 0; i < 3; i++) {
        accOrigin[i] = le16(c + 2 * i); gyroOrigin[i] = le16(c + 12 + 2 * i);
        if (abs(accOrigin[i]) >= 1000) accOrigin[i] = 0;
        if (abs(gyroOrigin[i]) >= 1000) gyroOrigin[i] = 0;
        if (le16(c + 6 + 2 * i) - accOrigin[i] <= 0 || le16(c + 18 + 2 * i) - gyroOrigin[i] <= 0) { fprintf(stderr, "[gyro] %s HID calibration unusable: nominal scales kept\n", which); return; }
    }
    for (int i = 0; i < 3; i++) {
        hidAccScale[i] = 4.0 / (le16(c + 6 + 2 * i) - accOrigin[i]);
        hidGyroOrigin[i] = gyroOrigin[i];
        hidGyroScale[i] = 936.0 / (le16(c + 18 + 2 * i) - gyroOrigin[i]);
    }
    fprintf(stderr, "[gyro] %s HID calibration: acc %.6f G, gyro %.4f deg/s per step, gyro origin %d %d %d\n", which, hidAccScale[0], hidGyroScale[0], gyroOrigin[0], gyroOrigin[1], gyroOrigin[2]);
}
/* The third-party Pro reader (runtime/gamepad.m) already holds the 057e:2009 device for its buttons, so it feeds the
   six-axis samples from the same 0x30 report instead of this file opening the device a second time. Three samples, oldest
   first, int16 little endian from byte 13: acceleration then rotation, on the raw axes. Nominal 8 G / 2000 deg/s. */
static uint64_t proMotionLast[RUNTIME_MAX_GAMEPADS]; /* main run loop, same slot as buttons */
void runtime_gamepad_hid_motion_reset(unsigned slot) {
    if (slot >= RUNTIME_MAX_GAMEPADS) return;
    proMotionLast[slot] = 0;
    runtime_motion_live_index(slot, 0);
}
void runtime_gamepad_hid_motion(unsigned slot, const uint8_t *p, int len) {
    if (slot >= RUNTIME_MAX_GAMEPADS || len < 49 || p[0] != 0x30 || !runtime_gyro_enabled()) return;
    if (!proMotionLast[slot]) { runtime_motion_deadzone_default_index(slot, 1.2); runtime_motion_live_index(slot, 1); }
    uint64_t now = runtime_now_ns(), step = proMotionLast[slot] && now - proMotionLast[slot] < 50000000ull ? (now - proMotionLast[slot]) / 3 : 5000000ull;
    proMotionLast[slot] = now;
    for (int k = 0; k < 3; k++) {
        const uint8_t *s = p + 13 + 12 * k;
        double raw[6];
        for (int i = 0; i < 6; i++) raw[i] = le16(s + 2 * i);
        for (int i = 0; i < 3; i++) { raw[i] *= 8.0 / 32768; raw[3 + i] *= (2000.0 / 32768) * (M_PI / 180); }
        const double acc[3] = {raw[1], -raw[0], -raw[2]}, gyro[3] = {-raw[4], raw[3], raw[5]};
        runtime_motion_feed_index(slot, acc, gyro, now - (uint64_t)(2 - k) * step);
    }
}

static void hid_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t id, uint8_t *p, CFIndex len) {   /* main run loop */
    (void)ctx; (void)sender; (void)type;
    if (result != kIOReturnSuccess || len < 49) return;
    if (id == 0x21 && (p[13] & 0x80) && p[14] == 0x10) {   /* an SPI read's reply: address at 15, size at 19, data at 20 */
        uint32_t at = p[15] | p[16] << 8 | p[17] << 16 | (uint32_t)p[18] << 24;
        if (at == 0x8026 && p[19] >= 26 && p[20] == 0xB2 && p[21] == 0xA1) { hidUserCalibration = YES; hid_calibration(p + 22, "user"); }
        else if (at == 0x6020 && p[19] >= 24 && !hidUserCalibration) hid_calibration(p + 20, "factory");
        return;
    }
    if (id != 0x30) return;
    uint64_t now = runtime_now_ns(), step = hidLast && now - hidLast < 50000000ull ? (now - hidLast) / 3 : 5000000ull;
    hidLast = now;
    for (int k = 0; k < 3; k++) {
        const uint8_t *s = p + 13 + 12 * k;
        double raw[6];
        for (int i = 0; i < 6; i++) raw[i] = le16(s + 2 * i);
        for (int i = 0; i < 3; i++) { raw[i] *= hidAccScale[i]; raw[3 + i] = (raw[3 + i] - hidGyroOrigin[i]) * hidGyroScale[i] * (M_PI / 180); }
        /* to GCMotion's frame (x right, y top edge, z out of the face; acceleration as gravity: face up z = -1 G) */
        const double acc[3] = {raw[1], -raw[0], -raw[2]}, gyro[3] = {-raw[4], raw[3], raw[5]};
        runtime_motion_feed(acc, gyro, now - (uint64_t)(2 - k) * step);
    }
}
/* Declared in runtime/gamepad.m: the third-party Pro type (key 6) reads a clone's sticks from its HID reports. */
extern int runtime_gamepad_type(int *detected);
static void hid_motion_start(void) {   /* main queue */
    if (!motionPad || hidDevice || runtime_gamepad_type(NULL) == 4) return;   /* 4: GAMEPAD_TYPE_THIRDPARTY_PRO */
    const char *name = (motionPad.vendorName ?: @"controller").UTF8String;
    hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(hidManager, (__bridge CFDictionaryRef)@{@kIOHIDVendorIDKey: @0x057e, @kIOHIDProductIDKey: @0x2009});
    NSSet *devices = (__bridge_transfer NSSet *)IOHIDManagerCopyDevices(hidManager);
    NSUInteger pads = 0;
    for (GCController *controller in GCController.controllers) if (controller.extendedGamepad) ++pads;
    /* No public association from this fallback HID handle to motionPad. With
       multiple controllers, choosing anyObject could feed another player's gyro. */
    if (devices.count != 1 || pads != 1) {
        fprintf(stderr, "[gyro] raw HID fallback identity ambiguous; retaining GameController motion only\n");
        CFRelease(hidManager); hidManager = NULL; return;
    }
    IOHIDDeviceRef d = (__bridge IOHIDDeviceRef)devices.anyObject;
    IOReturn r = d ? IOHIDDeviceOpen(d, kIOHIDOptionsTypeNone) : kIOReturnNotFound;
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "[gyro] %s: GameController's motion is all zeros and no Switch Pro HID device could be opened (0x%x): no motion from it\n", name, r);
        CFRelease(hidManager); hidManager = NULL; return;
    }
    hidDevice = (IOHIDDeviceRef)CFRetain(d); hidLast = 0; hidUserCalibration = NO;
    motionPad.motion.valueChangedHandler = nil;   /* sensors stay active: GameController keeps the IMU reporting */
    IOHIDDeviceScheduleWithRunLoop(hidDevice, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDDeviceRegisterInputReportCallback(hidDevice, hidReport, sizeof hidReport, hid_report, NULL);
    runtime_motion_deadzone_default(1.2); runtime_motion_live(1);
    fprintf(stderr, "[gyro] %s: GameController's motion is all zeros; reading the six-axis sensor from its HID reports\n", name);
    /* Do not send subcommand 0x40 (enable IMU) or SPI reads. This path only runs when GameController's motion is all
       zeros, which is an 8BitDo (or clone) reporting as 057e:2009, not an official Pro Controller. Those pads already
       stream 0x30 reports with live six-axis data, and an output report switches them out of the standard HID mode
       GameController reads: buttons and sticks then die while the gyro keeps working. Nominal 8 G / 2000 deg/s scales
       apply until a calibration reply arrives on its own. */
}
static void hid_motion_stop(void) {   /* main queue */
    if (!hidDevice) return;
    IOHIDDeviceRegisterInputReportCallback(hidDevice, hidReport, sizeof hidReport, NULL, NULL);
    IOHIDDeviceUnscheduleFromRunLoop(hidDevice, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeNone);
    CFRelease(hidDevice); hidDevice = NULL;
    CFRelease(hidManager); hidManager = NULL;
}

/* ---- controllers ---- */
void runtime_feedback_attach(GCController *c) {   /* main queue */
    const char *name = (c.vendorName ?: @"controller").UTF8String;
    if (runtime_rumble_enabled()) {
        rumble_start();
        if (c.haptics) {
            dispatch_async(rumbleQueue, ^{
                for (int p = 0; p < RUNTIME_MAX_GAMEPADS; p++) if (rumblePads[p] == c) return;
                for (int p = 0; p < RUNTIME_MAX_GAMEPADS; p++) {
                    if (!rumblePads[p]) {
                        for (int s = p * 2; s < p * 2 + 2; s++) {
                            if (players[s]) [players[s] stopAtTime:CHHapticTimeImmediate error:nil];
                            forget_side(s); retryAt[s] = 0;
                        }
                        rumblePads[p] = c;
                        break;
                    }
                }
            });
            NSMutableArray *where = [NSMutableArray array]; for (NSString *l in c.haptics.supportedLocalities) [where addObject:l];
            fprintf(stderr, "[rumble] %s: haptics (%s)\n", name, [where componentsJoinedByString:@", "].UTF8String);
        } else fprintf(stderr, "[rumble] %s has no haptics: no rumble from it\n", name);
    }
    if (runtime_gyro_enabled()) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *axes = getenv("TOTK_GYRO_AXES"); int r = runtime_motion_axes(axes);
            if (r < 0) runtime_fail("TOTK_GYRO_AXES must name x, y and z once each, e.g. \"-x,y,-z\"");
            if (r > 0) fprintf(stderr, "[gyro] axes remapped by TOTK_GYRO_AXES=%s\n", axes);
        });
        if (motionPad) return;   /* the first controller keeps the motion sensor; a second one must not take it over */
        GCMotion *m = c.motion;
        if (!m || !m.hasRotationRate) { fprintf(stderr, "[gyro] %s has no gyro: no motion from it\n", name); return; }
        if (m.sensorsRequireManualActivation) m.sensorsActive = YES;
        hid_motion_stop(); zeroSince = 0; motionSeen = hidAsked = NO; runtime_motion_deadzone_default(0);
        m.valueChangedHandler = ^(GCMotion *changed) { motion_sample(changed); };
        motionPad = c; runtime_motion_live(1);
        fprintf(stderr, "[gyro] %s: motion sensors on (gyro%s)\n", name, m.hasGravityAndUserAcceleration ? ", gravity" : ", acceleration");
    }
}
void runtime_feedback_set_slot(GCController *c, int slot) {   /* main queue */
    if (!c || slot < 0 || slot >= RUNTIME_MAX_GAMEPADS || !runtime_rumble_enabled() || !c.haptics) return;
    rumble_start();
    dispatch_async(rumbleQueue, ^{
        if (rumblePads[slot] == c) return;
        for (int p = 0; p < RUNTIME_MAX_GAMEPADS; p++) {
            if (rumblePads[p] == c) {
                for (int s = p * 2; s < p * 2 + 2; s++) {
                    if (players[s]) [players[s] stopAtTime:CHHapticTimeImmediate error:nil];
                    forget_side(s);
                }
                rumblePads[p] = nil;
            }
        }
        for (int s = slot * 2; s < slot * 2 + 2; s++) {
            if (players[s]) [players[s] stopAtTime:CHHapticTimeImmediate error:nil];
            forget_side(s); retryAt[s] = 0;
        }
        rumblePads[slot] = c;
    });
}
void runtime_feedback_detach(GCController *c) {   /* main queue */
    if (runtime_rumble_enabled() && rumbleQueue)
        dispatch_async(rumbleQueue, ^{
            for (int p = 0; p < RUNTIME_MAX_GAMEPADS; p++) {
                if (rumblePads[p] == c) {
                    for (int s = p * 2; s < p * 2 + 2; s++) {
                        if (players[s]) [players[s] stopAtTime:CHHapticTimeImmediate error:nil];
                        forget_side(s);
                    }
                    rumblePads[p] = nil;
                }
            }
        });
    if (motionPad == c) {
        c.motion.valueChangedHandler = nil; hid_motion_stop(); motionPad = nil; runtime_motion_live(0);
    }
}
#endif
