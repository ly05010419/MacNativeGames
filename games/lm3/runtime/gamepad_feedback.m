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
#include <stdatomic.h>
#include "runtime.h"

#if !TARGET_OS_OSX
void runtime_feedback_attach_player(GCController *c, unsigned player) { (void)c; (void)player; }
void runtime_feedback_attach(GCController *c) { runtime_feedback_attach_player(c, 0); }
void runtime_feedback_detach_player(GCController *c, unsigned player) { (void)c; (void)player; }
void runtime_feedback_detach(GCController *c) { runtime_feedback_detach_player(c, 0); }
void runtime_feedback_quiet(void) {}
int runtime_feedback_native_probe(void) { return 0; }
void runtime_rumble_set_player(unsigned player, unsigned side, float al, float fl, float ah, float fh) { (void)player; (void)side; (void)al; (void)fl; (void)ah; (void)fh; }
void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) { runtime_rumble_set_player(0, side, al, fl, ah, fh); }
void runtime_feedback_rumble_snapshot(unsigned player, unsigned side, float out[4]) { (void)player; (void)side; if(out){out[0]=out[2]=0;out[1]=160;out[3]=320;} }
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
static GCController *rumblePad[RUNTIME_GAMEPAD_PLAYERS]; /* rumbleQueue */
static GCController *feedbackPad[RUNTIME_GAMEPAD_PLAYERS]; /* main queue */
static _Atomic uint64_t feedbackGeneration[RUNTIME_GAMEPAD_PLAYERS]; /* main queue */
enum { RUMBLE_CHANNELS = RUNTIME_GAMEPAD_PLAYERS * 2 };
static CHHapticEngine *engines[RUMBLE_CHANNELS];
static id<CHHapticPatternPlayer> players[RUMBLE_CHANNELS];
static uint64_t engineGeneration[RUMBLE_CHANNELS];
static CHHapticPattern *pattern;
static BOOL playing[RUMBLE_CHANNELS];
static float sentIntensity[RUMBLE_CHANNELS], sentSharpness[RUMBLE_CHANNELS];
static uint64_t quietSince[RUMBLE_CHANNELS], retryAt[RUMBLE_CHANNELS], playedNs[RUMBLE_CHANNELS];
static uint64_t playingSince[RUMBLE_CHANNELS], playerSince[RUMBLE_CHANNELS];
static pthread_mutex_t wantLock = PTHREAD_MUTEX_INITIALIZER;
static float want[RUMBLE_CHANNELS][4] = {
    {0, 160, 0, 320}, {0, 160, 0, 320}, {0, 160, 0, 320}, {0, 160, 0, 320}
};
static uint64_t wantAt[RUMBLE_CHANNELS], wantCount, wantCountLogged;
static float strongest;
static float gain = 1.0f;

void runtime_rumble_set_player(unsigned player, unsigned side, float al, float fl, float ah, float fh) {
    if (player >= RUNTIME_GAMEPAD_PLAYERS || side > 1) return;
    unsigned channel = player * 2 + side;
    pthread_mutex_lock(&wantLock);
    want[channel][0] = al; want[channel][1] = fl; want[channel][2] = ah; want[channel][3] = fh;
    wantAt[channel] = runtime_now_ns(); wantCount++;
    float a = al > ah ? al : ah; if (a > strongest) strongest = a;
    pthread_mutex_unlock(&wantLock);
}
void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) {
    runtime_rumble_set_player(0, side, al, fl, ah, fh);
}
void runtime_feedback_rumble_snapshot(unsigned player, unsigned side, float out[4]) {
    if (!out) return;
    out[0] = out[2] = 0; out[1] = 160; out[3] = 320;
    if (player >= RUNTIME_GAMEPAD_PLAYERS || side > 1) return;
    unsigned channel = player * 2 + side;
    pthread_mutex_lock(&wantLock);
    if (wantAt[channel]) memcpy(out, want[channel], sizeof want[channel]);
    pthread_mutex_unlock(&wantLock);
}
static void quiet_player(unsigned player) {
    if (player >= RUNTIME_GAMEPAD_PLAYERS) return;
    runtime_hid_feedback_clear_player(player);
    pthread_mutex_lock(&wantLock);
    for (unsigned side = 0; side < 2; side++) {
        unsigned channel = player * 2 + side;
        wantAt[channel] = 0;
        want[channel][0] = want[channel][2] = 0;
        want[channel][1] = 160; want[channel][3] = 320;
    }
    pthread_mutex_unlock(&wantLock);
}
void runtime_feedback_quiet(void) {
    for (unsigned player = 0; player < RUNTIME_GAMEPAD_PLAYERS; player++) quiet_player(player);
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
static void forget_side(int channel) { /* rumbleQueue; invalidate callbacks before releasing the old engine. */
    engineGeneration[channel]++;
    CHHapticEngine *old = engines[channel];
    old.resetHandler = ^{}; old.stoppedHandler = ^(CHHapticEngineStoppedReason reason) { (void)reason; };
    [players[channel] stopAtTime:CHHapticTimeImmediate error:nil];
    players[channel] = nil; engines[channel] = nil; playing[channel] = NO;
    [old stopWithCompletionHandler:nil];
    sentIntensity[channel] = sentSharpness[channel] = -1;
    quietSince[channel] = playerSince[channel] = playingSince[channel] = 0;
}
static void forget_if_current(int channel, uint64_t generation, CHHapticEngine *engine) {
    if (engineGeneration[channel] == generation && engines[channel] == engine) forget_side(channel);
}
static void rumble_failed(int channel, const char *what, NSError *err) {
    fprintf(stderr, "[rumble] player %d %s handle: %s failed: %s (trying again in 5 s)\n",
            channel / 2 + 1, channel & 1 ? "right" : "left", what,
            err ? err.localizedDescription.UTF8String : "no reason given");
    forget_side(channel); retryAt[channel] = runtime_now_ns() + 5000000000ull;
}
static BOOL ensure_engine(int channel) { /* rumbleQueue */
    if (engines[channel]) return YES;
    GCController *pad = rumblePad[channel / 2];
    if (!pad.haptics || runtime_now_ns() < retryAt[channel]) return NO;
    NSError *err = nil;
    if (!pattern) {
        CHHapticEventParameter *intensity = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f];
        CHHapticEventParameter *sharpness = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5f];
        CHHapticEvent *event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[intensity, sharpness]
                                                          relativeTime:0 duration:RUMBLE_EVENT_S];
        pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&err];
        if (!pattern) { rumble_failed(channel, "building the pattern", err); return NO; }
    }
    GCHapticsLocality where = channel & 1 ? GCHapticsLocalityRightHandle : GCHapticsLocalityLeftHandle;
    if (![pad.haptics.supportedLocalities containsObject:where]) where = GCHapticsLocalityDefault;
    CHHapticEngine *engine = [pad.haptics createEngineWithLocality:where];
    if (!engine) { rumble_failed(channel, "creating the haptic engine", nil); return NO; }
    uint64_t generation = ++engineGeneration[channel];
    __weak CHHapticEngine *weakEngine = engine;
    engine.resetHandler = ^{ dispatch_async(rumbleQueue, ^{
        forget_if_current(channel, generation, weakEngine);
    }); };
    engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) { (void)reason; dispatch_async(rumbleQueue, ^{
        forget_if_current(channel, generation, weakEngine);
    }); };
    if (![engine startAndReturnError:&err]) { rumble_failed(channel, "starting the haptic engine", err); return NO; }
    engines[channel] = engine;
    return YES;
}
static BOOL send_params(id<CHHapticPatternPlayer> player, int channel, float intensity, float sharpness) {
    NSError *err = nil;
    CHHapticDynamicParameter *i = [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:intensity relativeTime:0];
    CHHapticDynamicParameter *s = [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl value:sharpness - 0.5f relativeTime:0];
    if (![player sendParameters:@[i, s] atTime:CHHapticTimeImmediate error:&err]) { rumble_failed(channel, "updating the player", err); return NO; }
    sentIntensity[channel] = intensity; sentSharpness[channel] = sharpness;
    return YES;
}
static BOOL start_player(int channel, float intensity, float sharpness, uint64_t now) {
    NSError *err = nil;
    id<CHHapticPatternPlayer> player = [engines[channel] createPlayerWithPattern:pattern error:&err];
    if (!player) { rumble_failed(channel, "creating the player", err); return NO; }
    if (!send_params(player, channel, intensity, sharpness)) return NO;
    if (![player startAtTime:CHHapticTimeImmediate error:&err]) { rumble_failed(channel, "starting the player", err); return NO; }
    [players[channel] stopAtTime:CHHapticTimeImmediate error:nil];
    players[channel] = player; playerSince[channel] = now;
    if (!playing[channel]) { playing[channel] = YES; playingSince[channel] = now; }
    return YES;
}
static void rumble_tick(void) {
    uint64_t now = runtime_now_ns();
    for (int channel = 0; channel < RUMBLE_CHANNELS; channel++) {
        float value[4]; uint64_t at;
        pthread_mutex_lock(&wantLock); memcpy(value, want[channel], sizeof value); at = wantAt[channel]; pthread_mutex_unlock(&wantLock);
        float intensity = at && now - at < RUMBLE_STALE_MS * 1000000ull ? intensity_of(value) : 0;
        float sharpness = sharpness_of(value);
        if (intensity < 0.02f) {
            if (!playing[channel]) continue;
            if (sentIntensity[channel] != 0 && !send_params(players[channel], channel, 0, sharpness)) continue;
            if (!quietSince[channel]) quietSince[channel] = now;
            else if (now - quietSince[channel] > RUMBLE_HOLD_MS * 1000000ull) {
                [players[channel] stopAtTime:CHHapticTimeImmediate error:nil]; players[channel] = nil;
                playing[channel] = NO; playedNs[channel] += now - playingSince[channel];
            }
            continue;
        }
        quietSince[channel] = 0;
        if (!ensure_engine(channel)) continue;
        if (!playing[channel] || now - playerSince[channel] > RUMBLE_RENEW_MS * 1000000ull)
            start_player(channel, intensity, sharpness, now);
        else if (fabsf(intensity - sentIntensity[channel]) > 0.01f || fabsf(sharpness - sentSharpness[channel]) > 0.02f)
            send_params(players[channel], channel, intensity, sharpness);
    }
    static uint64_t lastLog; if (!lastLog) lastLog = now;
    if (now - lastLog >= 30000000000ull) {
        pthread_mutex_lock(&wantLock); uint64_t n = wantCount - wantCountLogged; float peak = strongest; wantCountLogged = wantCount; strongest = 0; pthread_mutex_unlock(&wantLock);
        for (int channel = 0; channel < RUMBLE_CHANNELS; channel++) if (playing[channel]) {
            playedNs[channel] += now - playingSince[channel]; playingSince[channel] = now;
        }
        if (n) fprintf(stderr, "[rumble] last 30 s: %llu values, strongest %.2f; P1 left/right %.1f/%.1f s, P2 left/right %.1f/%.1f s\n",
                       (unsigned long long)n, peak, playedNs[0] / 1e9, playedNs[1] / 1e9, playedNs[2] / 1e9, playedNs[3] / 1e9);
        memset(playedNs, 0, sizeof playedNs); lastLog = now;
    }
}
static void rumble_start(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *g = getenv("TOTK_RUMBLE_GAIN");
        if (g) { char *end; double v = strtod(g, &end); if (end == g || *end || !(v > 0) || v > 4) runtime_fail("TOTK_RUMBLE_GAIN must be a number in (0, 4]"); gain = (float)v; }
        for (int channel = 0; channel < RUMBLE_CHANNELS; channel++) sentIntensity[channel] = sentSharpness[channel] = -1;
        rumbleQueue = dispatch_queue_create("lm3.rumble", DISPATCH_QUEUE_SERIAL);
        rumbleTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, rumbleQueue);
        dispatch_source_set_timer(rumbleTimer, dispatch_time(DISPATCH_TIME_NOW, 0), RUMBLE_TICK_MS * NSEC_PER_MSEC, 2 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(rumbleTimer, ^{ @autoreleasepool { rumble_tick(); } });
        dispatch_resume(rumbleTimer);
    });
}

/* ---- motion: the math is in runtime/motion_fusion.c ---- */
static GCController *motionPad;  /* main queue; player 1 only */
static uint64_t motionGeneration; /* rejects callbacks and delayed HID commands from an old device */
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
            else if (!hidAsked && now - zeroSince > 1000000000ull) {
                hidAsked = YES; uint64_t generation = motionGeneration; GCController *expected = motionPad;
                dispatch_async(dispatch_get_main_queue(), ^{ if (motionGeneration == generation && motionPad == expected) hid_motion_start(); });
            }
            return;
        }
        motionSeen = YES;
    }
    runtime_motion_feed(acc, gyro, runtime_now_ns());
}

/* Serialize native samples with attach/detach. A queued sample from a replaced
   1P device must not restart or feed the shared 1P fusion context. */
static void install_motion_handler(GCController *controller) {
    uint64_t generation = motionGeneration;
    controller.motion.valueChangedHandler = ^(GCMotion *changed) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (motionGeneration == generation && motionPad == controller && changed == controller.motion)
                motion_sample(changed);
        });
    };
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
static uint8_t hidPacket;
static uint64_t hidLast;
static BOOL hidUserCalibration;
static double hidAccScale[3] = {8.0 / 32768, 8.0 / 32768, 8.0 / 32768}, hidGyroScale[3] = {2000.0 / 32768, 2000.0 / 32768, 2000.0 / 32768}, hidGyroOrigin[3];
static int16_t le16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static void hid_subcommand(uint8_t cmd, const uint8_t *arg, int n) {   /* main queue */
    if (!hidDevice) return;
    uint8_t out[49] = {0x01, hidPacket++ & 15, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, cmd};   /* neutral rumble */
    memcpy(out + 11, arg, n);
    IOReturn r = IOHIDDeviceSetReport(hidDevice, kIOHIDReportTypeOutput, 0x01, out, sizeof out);
    if (r != kIOReturnSuccess) fprintf(stderr, "[gyro] HID subcommand 0x%02x failed: 0x%x\n", cmd, r);
}
static void hid_calibration(const uint8_t *c, const char *which) {
    for (int i = 0; i < 3; i++) {
        int acc = le16(c + 6 + 2 * i) - le16(c + 2 * i), gyro = le16(c + 18 + 2 * i) - le16(c + 12 + 2 * i);
        if (acc <= 0 || gyro <= 0) { fprintf(stderr, "[gyro] %s HID calibration unusable: nominal scales kept\n", which); return; }
    }
    for (int i = 0; i < 3; i++) {
        hidAccScale[i] = 4.0 / (le16(c + 6 + 2 * i) - le16(c + 2 * i));
        hidGyroOrigin[i] = le16(c + 12 + 2 * i);
        hidGyroScale[i] = 936.0 / (le16(c + 18 + 2 * i) - hidGyroOrigin[i]);
    }
    fprintf(stderr, "[gyro] %s HID calibration: acc %.6f G, gyro %.4f deg/s per step\n", which, hidAccScale[0], hidGyroScale[0]);
}
static void hid_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t id, uint8_t *p, CFIndex len) {   /* main run loop */
    (void)type;
    if ((uint64_t)(uintptr_t)ctx != motionGeneration || sender != hidDevice ||
        result != kIOReturnSuccess || len < 49) return;
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
static void hid_motion_start(void) {   /* main queue */
    if (!motionPad || hidDevice) return;
    const char *name = (motionPad.vendorName ?: @"controller").UTF8String;
    /* The legacy HID path cannot match a raw Switch device to a GCController.
       Do not select an arbitrary sensor when two controllers are present. */
    unsigned physical = 0;
    for (GCController *controller in GCController.controllers) if (controller.extendedGamepad) physical++;
    NSString *identity = [[NSString stringWithFormat:@"%@ %@", motionPad.productCategory ?: @"", motionPad.vendorName ?: @""] lowercaseString];
    if (feedbackPad[1] || physical > 1 ||
        (![identity containsString:@"switch"] && ![identity containsString:@"pro controller"])) {
        fprintf(stderr, "[gyro] player 1 %s: raw HID fallback has no unambiguous device identity; native motion only\n", name);
        return;
    }
    hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(hidManager, (__bridge CFDictionaryRef)@{@kIOHIDVendorIDKey: @0x057e, @kIOHIDProductIDKey: @0x2009});
    NSSet *devices = (__bridge_transfer NSSet *)IOHIDManagerCopyDevices(hidManager);
    if (devices.count != 1) {
        fprintf(stderr, "[gyro] player 1 %s: %lu raw Switch Pro devices; ambiguous HID fallback disabled\n", name, (unsigned long)devices.count);
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
    IOHIDDeviceRegisterInputReportCallback(hidDevice, hidReport, sizeof hidReport, hid_report, (void *)(uintptr_t)motionGeneration);
    runtime_motion_deadzone_default(1.2); runtime_motion_live(1);
    fprintf(stderr, "[gyro] %s: GameController's motion is all zeros; reading the six-axis sensor from its HID reports\n", name);
    /* IMU on, then the calibrations; spaced out, as the controller drops subcommands sent back to back */
    static const uint8_t imuOn[1] = {0x01}, factory[5] = {0x20, 0x60, 0, 0, 24}, user[5] = {0x26, 0x80, 0, 0, 26};
    hid_subcommand(0x40, imuOn, sizeof imuOn);
    uint64_t generation = motionGeneration; IOHIDDeviceRef expected = hidDevice;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        if (motionGeneration == generation && hidDevice == expected) hid_subcommand(0x10, factory, sizeof factory);
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        if (motionGeneration == generation && hidDevice == expected) hid_subcommand(0x10, user, sizeof user);
    });
}
static void hid_motion_stop(void) {   /* main queue */
    if (!hidDevice) return;
    ++motionGeneration; /* Invalidate queued reports/calibration commands before releasing this device. */
    IOHIDDeviceRegisterInputReportCallback(hidDevice, hidReport, sizeof hidReport, NULL, NULL);
    IOHIDDeviceUnscheduleFromRunLoop(hidDevice, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeNone);
    CFRelease(hidDevice); hidDevice = NULL;
    CFRelease(hidManager); hidManager = NULL;
}

/* ---- controllers ---- */
void runtime_feedback_attach_player(GCController *controller, unsigned player) { /* main queue */
    if (!controller || player >= RUNTIME_GAMEPAD_PLAYERS || feedbackPad[player] == controller) return;
    if (feedbackPad[player]) runtime_feedback_detach_player(feedbackPad[player], player);
    feedbackPad[player] = controller;
    uint64_t generation = atomic_fetch_add(&feedbackGeneration[player], 1) + 1;
    quiet_player(player);
    const char *name = (controller.vendorName ?: @"controller").UTF8String;
    if (runtime_rumble_enabled()) {
        rumble_start();
        dispatch_async(rumbleQueue, ^{
            if (atomic_load(&feedbackGeneration[player]) != generation) return;
            /* Queue order plus the device identity protects a replacement from
               an old disconnect/reset callback. Start each assignment silent. */
            for (unsigned side = 0; side < 2; side++) { int channel = (int)(player * 2 + side); forget_side(channel); retryAt[channel] = 0; }
            quiet_player(player); rumblePad[player] = controller;
        });
        if (controller.haptics) {
            NSMutableArray *where = [NSMutableArray array]; for (NSString *locality in controller.haptics.supportedLocalities) [where addObject:locality];
            fprintf(stderr, "[rumble] player %u %s: haptics (%s)\n", player + 1, name, [where componentsJoinedByString:@", "].UTF8String);
        } else fprintf(stderr, "[rumble] player %u %s has no haptics\n", player + 1, name);
    }
    if (player) {
        /* A second pad must never take over the single 1P motion context. */
        if (hidDevice) {
            hid_motion_stop(); runtime_motion_live(0);
            if (motionPad) { zeroSince = 0; motionSeen = hidAsked = NO; runtime_motion_deadzone_default(0); install_motion_handler(motionPad); }
            fprintf(stderr, "[gyro] second controller attached: ambiguous raw HID fallback stopped; player 1 native motion retained\n");
        }
        return;
    }
    if (!runtime_gyro_enabled()) return;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *axes = getenv("TOTK_GYRO_AXES"); int result = runtime_motion_axes(axes);
        if (result < 0) runtime_fail("TOTK_GYRO_AXES must name x, y and z once each, e.g. \"-x,y,-z\"");
        if (result > 0) fprintf(stderr, "[gyro] axes remapped by TOTK_GYRO_AXES=%s\n", axes);
    });
    GCMotion *motion = controller.motion;
    motionGeneration++; motionPad = controller;
    hid_motion_stop(); runtime_motion_live(0);
    zeroSince = 0; motionSeen = hidAsked = NO; runtime_motion_deadzone_default(0);
    if (!motion || !motion.hasRotationRate) { fprintf(stderr, "[gyro] player 1 %s has no gyro\n", name); return; }
    if (motion.sensorsRequireManualActivation) motion.sensorsActive = YES;
    install_motion_handler(controller); runtime_motion_live(1);
    fprintf(stderr, "[gyro] player 1 %s: native motion sensors on (gyro%s)\n", name, motion.hasGravityAndUserAcceleration ? ", gravity" : ", acceleration");
}
void runtime_feedback_detach_player(GCController *controller, unsigned player) { /* main queue */
    if (player >= RUNTIME_GAMEPAD_PLAYERS || feedbackPad[player] != controller) return;
    feedbackPad[player] = nil; uint64_t generation = atomic_fetch_add(&feedbackGeneration[player], 1) + 1;
    quiet_player(player);
    if (rumbleQueue) dispatch_async(rumbleQueue, ^{
        if (atomic_load(&feedbackGeneration[player]) != generation) return;
        for (unsigned side = 0; side < 2; side++) forget_side((int)(player * 2 + side));
        rumblePad[player] = nil;
    });
    if (!player && motionPad == controller) {
        motionGeneration++; controller.motion.valueChangedHandler = nil;
        hid_motion_stop(); motionPad = nil; runtime_motion_live(0);
    } else if (player && motionPad) {
        /* Allow the legacy single-controller fallback to retry once the second
           device is gone; a new ambiguity is rechecked before opening HID. */
        zeroSince = 0; motionSeen = hidAsked = NO;
    }
}
/* Isolated production routing checks use developer-created snapshots only.
   They never enumerate/attach a real controller and have no haptic devices. */
static int feedback_native_probe_impl(void) {
    const char *owner = getenv("TOTK_SLOT_OWNER"), *mode = getenv("LM3_INPUT_PROBE");
    if (!owner || strcmp(owner, "codex") || !mode || strcmp(mode, "1")) runtime_fail("native feedback probe requires isolated input probe");
    if (![NSThread isMainThread]) runtime_fail("native feedback probe must run on main thread");
    for (unsigned p = 0; p < RUNTIME_GAMEPAD_PLAYERS; p++) if (feedbackPad[p]) runtime_fail("native feedback probe cannot replace live devices");
    GCController *a = [GCController controllerWithExtendedGamepad];
    GCController *b = [GCController controllerWithExtendedGamepad];
    GCController *replacement = [GCController controllerWithExtendedGamepad];
    if (!a.isSnapshot || !b.isSnapshot || !replacement.isSnapshot || a.haptics || b.haptics || replacement.haptics)
        runtime_fail("native feedback probe requires non-haptic snapshots");
    __block int checked = 0;
    runtime_feedback_attach_player(a, 0); runtime_feedback_attach_player(b, 1);
    dispatch_sync(rumbleQueue, ^{
        if (rumblePad[0] != a || rumblePad[1] != b) runtime_fail("native feedback probe player assignment"); checked += 2;
    });
    runtime_rumble_set_player(0, 0, 0.2f, 100, 0.1f, 200);
    runtime_rumble_set_player(1, 0, 0.6f, 300, 0.4f, 400);
    runtime_feedback_detach_player(a, 0);
    dispatch_sync(rumbleQueue, ^{
        if (rumblePad[0] || rumblePad[1] != b) runtime_fail("native feedback probe disconnect shifted player 2"); checked += 2;
    });
    float value[4]; runtime_feedback_rumble_snapshot(0, 0, value);
    if (value[0] || value[2]) runtime_fail("native feedback probe disconnect retained vibration"); checked++;
    runtime_feedback_rumble_snapshot(1, 0, value);
    if (value[0] != 0.6f || value[2] != 0.4f) runtime_fail("native feedback probe disconnect muted other player"); checked++;
    runtime_feedback_attach_player(replacement, 0);
    runtime_feedback_detach_player(a, 0); /* Late disconnect for the old object. */
    dispatch_sync(rumbleQueue, ^{
        if (rumblePad[0] != replacement || rumblePad[1] != b) runtime_fail("native feedback probe stale disconnect cleared replacement"); checked += 2;
        uint64_t old = engineGeneration[0];
        forget_side(0); uint64_t current = engineGeneration[0];
        forget_if_current(0, old, nil); /* Same gate as a queued engine reset. */
        if (engineGeneration[0] != current) runtime_fail("native feedback probe stale engine callback cleared replacement"); checked++;
    });
    /* Coalesce assignment and disconnect before the queue processes either.
       A skipped attach must not leave the older device as the rumble target. */
    runtime_feedback_attach_player(a, 0); runtime_feedback_detach_player(a, 0);
    dispatch_sync(rumbleQueue, ^{
        if (rumblePad[0] || rumblePad[1] != b) runtime_fail("native feedback probe queued replacement retained old target"); checked += 2;
    });
    runtime_feedback_detach_player(b, 1);
    dispatch_sync(rumbleQueue, ^{
        if (rumblePad[0] || rumblePad[1]) runtime_fail("native feedback probe final devices retained"); checked += 2;
    });
    return checked;
}
int runtime_feedback_native_probe(void) {
    @autoreleasepool { return feedback_native_probe_impl(); }
}
void runtime_feedback_attach(GCController *controller) { runtime_feedback_attach_player(controller, 0); }
void runtime_feedback_detach(GCController *controller) { runtime_feedback_detach_player(controller, 0); }
#endif
