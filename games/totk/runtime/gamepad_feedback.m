/* The controller's haptics and motion sensors for the game's HD rumble and six-axis sensor (runtime/hid_feedback.c),
   2026-10-17. On by default (TOTK_RUMBLE=0 / TOTK_GYRO=0, or rumble.txt / gyro.txt saying off, turn them off), on macOS and
   iOS. The raw HID fallback for a controller whose GameController motion stays all zeros needs IOKit, so it is macOS only;
   the DualSense's trigger resistance reads the game's memory, so it is macOS only too.
   Rumble: each of the game's two vibration devices (left, right) is a Core Haptics engine on that handle of the
   controller (GCHapticsLocalityLeftHandle / RightHandle; a DualSense has both) playing a long continuous event. The
   game's value (two bands, each an amplitude 0..1 at a frequency) sets the event's intensity (TOTK_RUMBLE_GAIN, else
   rumble-gain.txt next to controls.txt, default 1, times the bands' combined amplitude) and sharpness (the bands' amplitude-weighted frequency, 40..1250 Hz on a log scale).
   The game sends values every few milliseconds from its own threads; those only store the latest one, and a timer on our
   own queue applies it 100 times a second. The player stops after 150 ms of silence, and the value counts as silence once
   the game has sent nothing for 250 ms (paused, frozen, window lost focus), so the controller never keeps buzzing alone.
   A Switch Pro Controller instead gets the game's HD rumble written to its HID reports (see rumble_tick_hid).
   Motion: each GCMotion sample (a DualSense: 250 a second; acceleration in G with gravity, rotation rate in rad/s) goes
   to runtime/motion_fusion.c, which turns it into the Switch's six-axis states. The DualSense was checked lying on the
   desk 2026-10-17: acceleration (0.00, +0.15, -0.95) G, rotation under 0.004 rad/s, 751 samples in 3 s. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "feedback_settings.h"

#import <CoreHaptics/CoreHaptics.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>   /* NSApp.active: R2 resists only while the app is in front */
#include <mach/mach_vm.h>
#include "bow_state.h"
#include "game_version.h"   /* Tears of the Kingdom 1.4.3's scene slot and actor name offsets, for the bow state */
#import <IOKit/hid/IOHIDManager.h>
#endif
#include "hid_imu.h"

/* ---- rumble ----
   Simple pattern players (CHHapticPatternPlayer) only: on macOS 26.5.2 (25F84) the advanced player (looping) never gets
   created on a game controller -- gamecontrollerd throws while decoding the pattern ("value for key 'NS.objects' was of
   unexpected class 'AVHapticEvent'"), the client gets NSXPCConnection error 4097 and the engine is dead (later calls
   -4810). Simple players and AHAP data play fine there (2026-10-17, DualSense, intensity 0). So each handle plays one
   30-second continuous event whose intensity and sharpness follow the game through dynamic parameters, and a fresh
   player takes over after 25 s. */
enum { RUMBLE_TICK_MS = 10, RUMBLE_SLOW_MS = 1000, RUMBLE_STALE_MS = 250, RUMBLE_HOLD_MS = 150, RUMBLE_EVENT_S = 30, RUMBLE_RENEW_MS = 25000 };
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
static _Atomic float gain = 1.0f;   /* the rumble queue reads it; the settings panel sets it */
/* Idle rumble ticks once a second instead of 100 times: a connected controller should not keep the CPU waking while
   the game sends nothing. A new value switches back to 100 Hz. */
static _Atomic int rumbleSlow;

void runtime_rumble_set(unsigned side, float al, float fl, float ah, float fh) {
    if (side > 1) return;
    pthread_mutex_lock(&wantLock);
    want[side][0] = al; want[side][1] = fl; want[side][2] = ah; want[side][3] = fh; wantAt[side] = runtime_now_ns(); wantCount++;
    float a = al > ah ? al : ah; if (a > strongest) strongest = a;
    pthread_mutex_unlock(&wantLock);
    if (a > 0.02f && rumbleQueue && atomic_load_explicit(&rumbleSlow, memory_order_relaxed)) {
        atomic_store_explicit(&rumbleSlow, 0, memory_order_relaxed);
        dispatch_async(rumbleQueue, ^{
            dispatch_source_set_timer(rumbleTimer, dispatch_time(DISPATCH_TIME_NOW, 0), RUMBLE_TICK_MS * NSEC_PER_MSEC, 2 * NSEC_PER_MSEC);
        });
    }
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
/* The two HID rumble paths and the HID motion below are macOS only (IOKit); iOS gets the Core Haptics and GameController
   paths above, as Apple's GameController API is the only controller HID access there. */
#if TARGET_OS_OSX
/* ---- rumble written straight to a Switch Pro Controller (2026-10-19) ----
   The 8BitDo controller in Switch mode (057e:2009) gets GCDeviceHaptics with both handles, Core Haptics plays without an
   error (a BOTW session: amplitude 0.20, "played left 0.5 s / right 0.5 s"), yet the controller never vibrates. So a
   controller GameController files as a Switch Pro Controller gets the game's values as they are -- they already are
   Switch HD rumble, two bands per side -- in output report 0x10 on its HID device (shared with the motion below), after
   subcommand 0x48 turns vibration on, as SDL does. Encoding from SDL / dekuNukem's notes: frequency code
   round(log2(f / 10) * 32), high band (code - 0x60) * 4 over 9 bits, low band code - 0x40 over 7 bits; amplitude index
   0..100 (2^(i / 32) / 8.7 above 0.23, 2^(i / 16) / 17 above 0.12, about 0.00836 * 2^(i / 4) below), high band i * 2, low
   band 0x40 + i / 2 with the odd bit in 0x80 of the frequency byte. At most one report every 30 ms (SDL: faster can switch
   the controller off), carrying the strongest amplitude since the last one. TOTK_RUMBLE_HID=0 keeps Core Haptics. */
enum { RUMBLE_HID_MS = 30 };
static IOHIDDeviceRef rumbleHid;    /* rumbleQueue: retained while the HID path is on */
static float hidHeld[2][2];         /* rumbleQueue: strongest low / high amplitude since the last report */
static uint64_t hidSentAt;
static int hidQuietSent;
static void hid_write(IOHIDDeviceRef d, uint8_t id, const uint8_t *body, int n);
static int freq_code(float f, int lo, int hi) {
    float c = roundf(log2f(f / 10.f) * 32.f);
    return !(c >= lo) ? lo : c > hi ? hi : (int)c;
}
static int amp_code(float a) {
    if (!(a > 0)) return 0;
    float c = a >= 0.23f ? log2f(a * 8.7f) * 32.f : a >= 0.12f ? log2f(a * 17.f) * 16.f : log2f(a / 0.00836f) * 4.f;
    c = roundf(c);
    return c < 1 ? 1 : c > 100 ? 100 : (int)c;
}
static void hd_rumble(uint8_t out[4], float al, float fl, float ah, float fh) {
    int la = amp_code(al), ha = amp_code(ah);
    if (!la && !ha) { out[0] = 0x00; out[1] = 0x01; out[2] = 0x40; out[3] = 0x40; return; }   /* neutral */
    int hf = (freq_code(fh, 0x60, 0xDF) - 0x60) * 4, lf = freq_code(fl, 0x40, 0xBF) - 0x40, lamp = (la & 1 ? 0x8000 : 0) | (0x40 + la / 2);
    out[0] = hf & 0xFF; out[1] = (uint8_t)(ha * 2 | (hf >> 8 & 1)); out[2] = (uint8_t)(lf | (lamp >> 8 & 0x80)); out[3] = lamp & 0xFF;
}
static void rumble_tick_hid(uint64_t now) {   /* rumbleQueue, every 10 ms */
    float v[2][4]; uint64_t at[2];
    pthread_mutex_lock(&wantLock); memcpy(v, want, sizeof v); at[0] = wantAt[0]; at[1] = wantAt[1]; pthread_mutex_unlock(&wantLock);
    for (int side = 0; side < 2; side++) {
        BOOL live = at[side] && now - at[side] < RUMBLE_STALE_MS * 1000000ull;
        for (int b = 0; b < 2; b++) {
            float a = live ? fminf(v[side][2 * b] * gain, 1.f) : 0;
            if (a > hidHeld[side][b]) hidHeld[side][b] = a;
        }
        if (amp_code(hidHeld[side][0]) || amp_code(hidHeld[side][1])) playedNs[side] += RUMBLE_TICK_MS * 1000000ull;
    }
    if (now - hidSentAt < RUMBLE_HID_MS * 1000000ull) return;
    uint8_t body[8]; BOOL quiet = YES;
    for (int side = 0; side < 2; side++) {
        hd_rumble(body + 4 * side, hidHeld[side][0], v[side][1], hidHeld[side][1], v[side][3]);
        if (amp_code(hidHeld[side][0]) || amp_code(hidHeld[side][1])) quiet = NO;
        hidHeld[side][0] = hidHeld[side][1] = 0;
    }
    if (quiet && hidQuietSent >= 3) return;   /* neutral a few times, then nothing until the game rumbles again */
    hidQuietSent = quiet ? hidQuietSent + 1 : 0; hidSentAt = now;
    hid_write(rumbleHid, 0x10, body, sizeof body);
}
/* ---- rumble written straight to a controller's HID device, from a table of profiles ----
   Some controllers macOS files as generic HID pads: GameController gives them no haptics (the log says "has no haptics"),
   so the Core Haptics path never plays. For those the game's rumble is written to the HID device as one output report
   with a motor byte for the low-frequency (left) and one for the high-frequency (right) motor. A profile says which
   device (vendor and product id), which report (id, length) and where the two motor bytes sit in it (offsets; 0 is the
   report id). The built-in table has the controllers checked on real hardware; a player can add one in
   rumble-profiles.txt next to controls.txt (TOTK_RUMBLE_PROFILES names another file): one profile per line,
   `vendor product report_id length low_offset high_offset` (decimal or 0x hex, `#` starts a comment), for example the
   built-in line `0x2dc8 0x6012 0x05 5 1 2 1` (an optional seventh number, 1, says the high motor is the weak one, so the low motor carries the stronger side). Many reports can be found in SDL's HIDAPI drivers (src/joystick/hidapi).
   Built in:
   - 2dc8:6012 8BitDo Ultimate 2 Wireless, firmware 1.03 and later (34-byte input reports), output report 0x05 =
     {0x05, low, high, 0, 0} as SDL_hidapi_8bitdo.c sends it; a probe over Bluetooth LE felt both motors (2026-10-21):
     the low motor is strong and the high one weak; every strength from 50/255 was felt, so the floor is for the game's
     faint values; one report gives a short kick, so a held value is resent every 50 ms.
   The game's left value drives the low motor and its right value the high one, each by the same intensity as the Core
   Haptics path. A report goes out when a motor value changes (at most every 20 ms) and every 50 ms while one is on (the
   8BitDo stops a lone report after a moment); silence is written as zeros three times, also at exit. TOTK_RUMBLE_HID=0 keeps it off. The game's own pulses are faint and short
   (a play session: strongest amplitude 0.10-0.15, a few tenths of a second in 30 s) and motors need a push to start
   turning, so a non-zero value is raised to at least TOTK_RUMBLE_HID_FLOOR (0..255, default 110) and held for
   TOTK_RUMBLE_HID_HOLD_MS (default 120) after it ends. */
typedef struct { uint16_t vendor, product; uint8_t reportId, length, lowAt, highAt, lowTakesMax; } HidRumbleProfile;
enum { HID_PROFILES_MAX = 16, HID_MIN_MS = 20, HID_KEEP_MS = 50, HID_QUIET_REPEATS = 3 };
static HidRumbleProfile hidProfiles[HID_PROFILES_MAX] = { {0x2dc8, 0x6012, 0x05, 5, 1, 2, 1} };
static int hidProfileCount = 1;
static IOHIDManagerRef hidRumbleManager;   /* main queue */
static IOHIDDeviceRef hidRumbleDevice;     /* set on rumbleQueue, opened on the main queue */
static HidRumbleProfile hidRumbleProfile;  /* the profile of hidRumbleDevice */
static GCController *padRumblePad;         /* main queue */
static uint8_t padSent[2], padHeld[2];
static uint64_t padSentAt, padHeldUntil[2];
static int hidFloor = 110, hidHoldMs = 120, padQuietSent = HID_QUIET_REPEATS;
static BOOL hidRumbleFailed;
static void hid_profiles_load(void) {   /* main queue, once */
    const char *path = getenv("TOTK_RUMBLE_PROFILES"), *controls = getenv("TOTK_CONTROLS");
    NSString *file = path && *path ? @(path) : controls && *controls ? [[@(controls) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"rumble-profiles.txt"] : nil;
    NSString *text = file ? [NSString stringWithContentsOfFile:file encoding:NSUTF8StringEncoding error:nil] : nil;
    if (!text) return;
    int lineNo = 0;
    for (NSString *raw in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet]) {
        lineNo++;
        NSRange hash = [raw rangeOfString:@"#"];
        NSString *line = [(hash.location == NSNotFound ? raw : [raw substringToIndex:hash.location]) stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (!line.length) continue;
        int v, p, rid, len, lo, hi, mx = 0; char extra; int got = sscanf(line.UTF8String, "%i %i %i %i %i %i %i %c", &v, &p, &rid, &len, &lo, &hi, &mx, &extra);
        BOOL ok = (got == 6 || got == 7) && mx >= 0 && mx <= 1
                  && v > 0 && v <= 0xffff && p >= 0 && p <= 0xffff && rid >= 1 && rid <= 255 && len >= 3 && len <= 64
                  && lo >= 1 && lo < len && hi >= 1 && hi < len && lo != hi;
        if (!ok) { fprintf(stderr, "[rumble] %s line %d ignored (want: vendor product report_id length low_offset high_offset [1 if the high motor is weak])\n", file.UTF8String, lineNo); continue; }
        if (hidProfileCount >= HID_PROFILES_MAX) { fprintf(stderr, "[rumble] %s: at most %d profiles\n", file.UTF8String, HID_PROFILES_MAX); break; }
        hidProfiles[hidProfileCount++] = (HidRumbleProfile){(uint16_t)v, (uint16_t)p, (uint8_t)rid, (uint8_t)len, (uint8_t)lo, (uint8_t)hi, (uint8_t)mx};
        fprintf(stderr, "[rumble] profile %04x:%04x report 0x%02x, %d bytes, motors at %d and %d%s (from %s)\n", v, p, rid, len, lo, hi, mx ? ", low motor follows the stronger side" : "", file.UTF8String);
    }
}
static void hid_rumble_write(IOHIDDeviceRef d, const HidRumbleProfile *pr, uint8_t lo, uint8_t hi) {
    uint8_t out[64] = {0};
    out[0] = pr->reportId; out[pr->lowAt] = lo; out[pr->highAt] = hi;
    IOReturn r = IOHIDDeviceSetReport(d, kIOHIDReportTypeOutput, pr->reportId, out, pr->length);
    if (r != kIOReturnSuccess && !hidRumbleFailed) { hidRumbleFailed = YES; fprintf(stderr, "[rumble] HID output report 0x%02x failed: 0x%x (not logged again)\n", pr->reportId, r); }
}
static void hid_rumble_tick(uint64_t now) {   /* rumbleQueue, every 10 ms */
    uint8_t m[2];
    for (int side = 0; side < 2; side++) {
        float v[4]; uint64_t at;
        pthread_mutex_lock(&wantLock); memcpy(v, want[side], sizeof v); at = wantAt[side]; pthread_mutex_unlock(&wantLock);
        float a = at && now - at < RUMBLE_STALE_MS * 1000000ull ? intensity_of(v) : 0;
        if (a >= 0.02f) {
            int level = hidFloor + (int)(a * (255 - hidFloor) + 0.5f);
            padHeld[side] = (uint8_t)(level > 255 ? 255 : level);
            padHeldUntil[side] = now + (uint64_t)hidHoldMs * 1000000ull;
        }
        m[side] = now < padHeldUntil[side] ? padHeld[side] : 0;
        if (m[side]) playedNs[side] += RUMBLE_TICK_MS * 1000000ull;
    }
    if (hidRumbleProfile.lowTakesMax && m[1] > m[0]) m[0] = m[1];   /* the high-frequency motor is the weak one: the low one carries the stronger side */
    if (!m[0] && !m[1]) {   /* a stop that gets lost keeps the motor turning, so zeros go out a few times */
        if (padQuietSent >= HID_QUIET_REPEATS || now - padSentAt < HID_MIN_MS * 1000000ull) return;
        hid_rumble_write(hidRumbleDevice, &hidRumbleProfile, 0, 0);
        padQuietSent++; padSent[0] = padSent[1] = 0; padSentAt = now;
        return;
    }
    padQuietSent = 0;
    BOOL same = m[0] == padSent[0] && m[1] == padSent[1];
    if (now - padSentAt < (same ? HID_KEEP_MS : HID_MIN_MS) * 1000000ull) return;   /* the controller stops a lone report after a moment */
    hid_rumble_write(hidRumbleDevice, &hidRumbleProfile, m[0], m[1]);
    padSent[0] = m[0]; padSent[1] = m[1]; padSentAt = now;
}
static void hid_rumble_quiet_at_exit(void) {   /* the controller keeps the last motor values it got */
    if (hidRumbleDevice) hid_rumble_write(hidRumbleDevice, &hidRumbleProfile, 0, 0);
}
static void hid_rumble_attach(GCController *c) {   /* main queue: c has no haptics */
    const char *off = getenv("TOTK_RUMBLE_HID");
    if (hidRumbleDevice || (off && !strcmp(off, "0"))) return;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        hid_profiles_load();
        const char *fl = getenv("TOTK_RUMBLE_HID_FLOOR"), *ho = getenv("TOTK_RUMBLE_HID_HOLD_MS");
        if (fl) hidFloor = atoi(fl) < 0 ? 0 : atoi(fl) > 255 ? 255 : atoi(fl);
        if (ho) hidHoldMs = atoi(ho) < 0 ? 0 : atoi(ho) > 500 ? 500 : atoi(ho);
        atexit(hid_rumble_quiet_at_exit);
    });
    NSMutableArray *matching = [NSMutableArray array];
    for (int i = 0; i < hidProfileCount; i++) [matching addObject:@{@kIOHIDVendorIDKey: @(hidProfiles[i].vendor), @kIOHIDProductIDKey: @(hidProfiles[i].product)}];
    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatchingMultiple(m, (__bridge CFArrayRef)matching);
    IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    NSSet *devices = (__bridge_transfer NSSet *)IOHIDManagerCopyDevices(m);
    IOHIDDeviceRef d = (__bridge IOHIDDeviceRef)devices.anyObject;
    if (!d) { CFRelease(m); return; }
    int vendor = 0, product = 0;
    CFTypeRef cv = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDVendorIDKey)), cp = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDProductIDKey));
    if (cv) CFNumberGetValue(cv, kCFNumberIntType, &vendor);
    if (cp) CFNumberGetValue(cp, kCFNumberIntType, &product);
    const HidRumbleProfile *found = NULL;
    for (int i = hidProfileCount - 1; i >= 0 && !found; i--) if (hidProfiles[i].vendor == vendor && hidProfiles[i].product == product) found = &hidProfiles[i];   /* the player's lines win */
    if (!found) { CFRelease(m); return; }
    IOReturn r = IOHIDDeviceOpen(d, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) { fprintf(stderr, "[rumble] HID device %04x:%04x found but did not open (0x%x): no rumble from it\n", vendor, product, r); CFRelease(m); return; }
    hidRumbleManager = m; padRumblePad = c; hidRumbleFailed = NO;
    HidRumbleProfile pr = *found;
    IOHIDDeviceRef held = (IOHIDDeviceRef)CFRetain(d);
    dispatch_async(rumbleQueue, ^{ padSent[0] = padSent[1] = 0; padSentAt = 0; padQuietSent = HID_QUIET_REPEATS; hidRumbleProfile = pr; hidRumbleDevice = held; });
    fprintf(stderr, "[rumble] %s: no haptics, so rumble is written to its HID reports (%04x:%04x, report 0x%02x)\n", (c.vendorName ?: @"controller").UTF8String, vendor, product, pr.reportId);
}
static void hid_rumble_detach(GCController *c) {   /* main queue */
    if (padRumblePad != c) return;
    padRumblePad = nil;
    IOHIDManagerRef m = hidRumbleManager; hidRumbleManager = NULL;
    dispatch_async(rumbleQueue, ^{
        IOHIDDeviceRef d = hidRumbleDevice; hidRumbleDevice = NULL;
        if (d) { hid_rumble_write(d, &hidRumbleProfile, 0, 0); CFRelease(d); }
        if (m) CFRelease(m);
    });
}
static BOOL hid_rumble_active(void) { return rumbleHid || hidRumbleDevice; }   /* a HID path, not Core Haptics */
#else
static void hid_rumble_attach(GCController *c) { (void)c; }
static void hid_rumble_detach(GCController *c) { (void)c; }
static BOOL hid_rumble_active(void) { return NO; }
#endif
static void rumble_tick(void) {   /* rumbleQueue, every 10 ms */
    uint64_t now = runtime_now_ns();
    for (int side = 0; side < 2 && !hid_rumble_active(); side++) {
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
#if TARGET_OS_OSX
    if (rumbleHid) rumble_tick_hid(now);           /* a Switch Pro Controller (PR #920) */
    if (hidRumbleDevice) hid_rumble_tick(now);     /* a HID pad from the profile table (PR #149) */
#endif
    BOOL busy = playing[0] || playing[1];
    for (int side = 0; side < 2; side++) {
        pthread_mutex_lock(&wantLock); uint64_t at = wantAt[side]; pthread_mutex_unlock(&wantLock);
        if (at && now - at < RUMBLE_STALE_MS * 1000000ull) busy = YES;
    }
    int slow = atomic_load_explicit(&rumbleSlow, memory_order_relaxed);
    if (busy && slow) {
        atomic_store_explicit(&rumbleSlow, 0, memory_order_relaxed);
        dispatch_source_set_timer(rumbleTimer, dispatch_time(DISPATCH_TIME_NOW, 0), RUMBLE_TICK_MS * NSEC_PER_MSEC, 2 * NSEC_PER_MSEC);
    } else if (!busy && !slow) {
        atomic_store_explicit(&rumbleSlow, 1, memory_order_relaxed);
        dispatch_source_set_timer(rumbleTimer, dispatch_time(DISPATCH_TIME_NOW, RUMBLE_SLOW_MS * NSEC_PER_MSEC),
                                  RUMBLE_SLOW_MS * NSEC_PER_MSEC, 100 * NSEC_PER_MSEC);
    }
    static uint64_t lastLog; if (!lastLog) lastLog = now;
    if (now - lastLog >= 30000000000ull) {
        pthread_mutex_lock(&wantLock); uint64_t n = wantCount - wantCountLogged; float peak = strongest; wantCountLogged = wantCount; strongest = 0; pthread_mutex_unlock(&wantLock);
        for (int side = 0; side < 2; side++) if (playing[side]) { playedNs[side] += now - playingSince[side]; playingSince[side] = now; }
        if (n) fprintf(stderr, "[rumble] last 30 s: %llu values from the game, strongest amplitude %.2f, played left %.1f s / right %.1f s%s\n",
                       (unsigned long long)n, peak, playedNs[0] / 1e9, playedNs[1] / 1e9, hid_rumble_active() ? " (HID)" : rumblePad.haptics ? "" : " (no controller with haptics)");
        playedNs[0] = playedNs[1] = 0; lastLog = now;
    }
}
/* The strength, once: TOTK_RUMBLE_GAIN, else rumble-gain.txt, else 1. */
static void gain_load(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *g = getenv("TOTK_RUMBLE_GAIN"); char word[32] = {0}; float saved;
        if (g) { char *end; double v = strtod(g, &end); if (end == g || *end || !(v > 0) || v > 4) runtime_fail("TOTK_RUMBLE_GAIN must be a number in (0, 4]"); gain = (float)v; }
        else if (runtime_setting_word("rumble-gain.txt", word, sizeof word)) {
            if (feedback_gain_word(word, &saved)) { gain = saved; fprintf(stderr, "[rumble] strength %g (rumble-gain.txt)\n", saved); }
            else fprintf(stderr, "[rumble] rumble-gain.txt: not a number in (0, 4]; strength 1\n");
        }
    });
}
/* The settings panel's strength row (key 1, runtime/settings_panel.h): applied at once and saved in rumble-gain.txt;
   TOTK_RUMBLE_GAIN, when set, decides and the row shows it greyed out. */
int runtime_rumble_gain_locked(void) { return getenv("TOTK_RUMBLE_GAIN") != NULL; }
float runtime_rumble_gain(void) {
    const char *g = getenv("TOTK_RUMBLE_GAIN");
    if (g) { char *end; double v = strtod(g, &end); return end != g && !*end && v > 0 && v <= 4 ? (float)v : 1; }   /* shown only: a bad value stops the game when rumble starts, as before */
    gain_load(); return gain;
}
void runtime_rumble_gain_set(float g) {
    if (runtime_rumble_gain_locked() || !(g > 0) || g > 4) return;
    gain_load(); gain = g;
    char word[32]; snprintf(word, sizeof word, "%g", g);
    runtime_setting_save("rumble-gain.txt", word, "rumble strength");
    fprintf(stderr, "[rumble] strength %g (settings)\n", g);
}
static void rumble_start(void) {
    gain_load();
    static dispatch_once_t once;
    dispatch_once(&once, ^{
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

/* ---- motion straight from a Switch Pro Controller's HID reports (2026-10-19; macOS only) ----
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
#if TARGET_OS_OSX
static IOHIDManagerRef hidManager;   /* main queue */
static IOHIDDeviceRef hidDevice;     /* main queue: open while motion or rumble uses it */
static BOOL hidMotion, hidRumble;    /* main queue: which of them does (motion: a Switch pad's IMU, or hid_imu_start's pad) */
static const HidImuProfile *imuProfile;   /* main run loop: the pad hid_imu_start reads (PR #309), else NULL */
static GCController *hidRumblePad;   /* main queue */
static dispatch_queue_t hidOut;      /* every output report, in order; the packet counter is its own */
static uint8_t hidPacket;            /* hidOut */
static uint8_t hidReport[128];   /* room for the DS4's 78-byte report, the largest */
static BOOL hidJoyConRight; /* local: paired original Joy-Con uses the right IMU */
static uint64_t hidLast;
static BOOL hidUserCalibration;
/* The IMU-on subcommand can be lost (a controller drops subcommands sent close together, and a Joy-Con a Switch has
   used can keep its IMU off), and then every 0x30 report carries six zero axes -- which a working sensor never reads.
   While that lasts it is sent again, every 2 s, at most HID_IMU_RETRIES times; the first nonzero sample ends it. */
enum { HID_IMU_RETRIES = 5 };
static uint64_t hidZeroSince, hidImuTriedAt;   /* main run loop; reset by hid_motion_start */
static int hidImuRetries;
static BOOL hidImuLive;
static double hidAccScale[3] = {8.0 / 32768, 8.0 / 32768, 8.0 / 32768}, hidGyroScale[3] = {2000.0 / 32768, 2000.0 / 32768, 2000.0 / 32768}, hidGyroOrigin[3];
static int16_t le16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
/* Output report `id`: the packet counter, then `body` (0x01: rumble for both sides, subcommand, its argument; 0x10: rumble). */
static void hid_write(IOHIDDeviceRef d, uint8_t id, const uint8_t *body, int n) {   /* any queue */
    if (!d) return;
    struct { uint8_t b[49]; } out = {{id}};
    memcpy(out.b + 2, body, n);
    CFRetain(d);
    dispatch_async(hidOut, ^{
        uint8_t report[49]; memcpy(report, out.b, sizeof report);
        report[1] = hidPacket++ & 15;
        IOReturn r = IOHIDDeviceSetReport(d, kIOHIDReportTypeOutput, id, report, sizeof report);
        static BOOL rumbleFailed;
        if (r != kIOReturnSuccess && (id != 0x10 || !rumbleFailed)) {
            if (id == 0x10) rumbleFailed = YES;
            fprintf(stderr, "[hid] output report 0x%02x%s failed: 0x%x\n", id, id == 0x10 ? " (rumble; not logged again)" : "", r);
        }
        CFRelease(d);
    });
}
static void hid_subcommand(uint8_t cmd, const uint8_t *arg, int n) {   /* main queue */
    uint8_t body[47] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40, cmd};   /* neutral rumble */
    memcpy(body + 9, arg, n);
    hid_write(hidDevice, 0x01, body, 9 + n);
}
static void hid_quiet_at_exit(void) {   /* the controller keeps the last rumble it got: leave it neutral */
    if (!hidDevice || !hidRumble) return;
    uint8_t out[49] = {0x10, 0, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
    IOHIDDeviceSetReport(hidDevice, kIOHIDReportTypeOutput, 0x10, out, sizeof out);
}
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
static void hid_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t id, uint8_t *p, CFIndex len) {   /* main run loop */
    (void)ctx; (void)sender; (void)type;
    if (result != kIOReturnSuccess || len < 49 || !hidMotion) return;
    if (id == 0x21 && (p[13] & 0x80) && p[14] == 0x10) {   /* an SPI read's reply: address at 15, size at 19, data at 20 */
        uint32_t at = p[15] | p[16] << 8 | p[17] << 16 | (uint32_t)p[18] << 24;
        if (at == 0x8026 && p[19] >= 26 && p[20] == 0xB2 && p[21] == 0xA1) { hidUserCalibration = YES; hid_calibration(p + 22, "user"); }
        else if (at == 0x6020 && p[19] >= 24 && !hidUserCalibration) hid_calibration(p + 20, "factory");
        return;
    }
    if (id != 0x30) return;
    if (!hidJoyConRight) runtime_gamepad_switch_left((__bridge void *)motionPad,
        p[6] | (p[7] & 15) << 8, (p[7] >> 4) | p[8] << 4);
    runtime_gamepad_switch_right((__bridge void *)motionPad,
        p[9] | (p[10] & 15) << 8, (p[10] >> 4) | p[11] << 4);
    uint64_t now = runtime_now_ns(), step = hidLast && now - hidLast < 50000000ull ? (now - hidLast) / 3 : 5000000ull;
    hidLast = now;
    if (!hidImuLive) {
        BOOL zero = YES;
        for (int i = 13; i < 13 + 36 && zero; i++) zero = !p[i];
        if (!zero) {
            hidImuLive = YES;
            if (hidImuRetries) fprintf(stderr, "[gyro] six-axis sensor on after %d resend%s of the IMU-on subcommand\n", hidImuRetries, hidImuRetries == 1 ? "" : "s");
        } else if (!hidZeroSince) hidZeroSince = now;
        else if (hidImuRetries < HID_IMU_RETRIES && now - hidZeroSince > 1000000000ull && now - hidImuTriedAt > 2000000000ull) {
            hidImuTriedAt = now; ++hidImuRetries;
            static const uint8_t imuOn[1] = {0x01};
            hid_subcommand(0x40, imuOn, sizeof imuOn);
            fprintf(stderr, "[gyro] six-axis sensor reads all zeros: IMU-on subcommand sent again (%d of %d)%s\n", hidImuRetries, HID_IMU_RETRIES,
                    hidImuRetries == HID_IMU_RETRIES ? "; not sent again after this one" : "");
        }
    }
    for (int k = 0; k < 3; k++) {
        const uint8_t *s = p + 13 + 12 * k;
        double raw[6];
        for (int i = 0; i < 6; i++) raw[i] = le16(s + 2 * i);
        for (int i = 0; i < 3; i++) { raw[i] *= hidAccScale[i]; raw[3 + i] = (raw[3 + i] - hidGyroOrigin[i]) * hidGyroScale[i] * (M_PI / 180); }
        /* to GCMotion's frame (x right, y top edge, z out of the face; acceleration as gravity: face up z = -1 G) */
        /* Right Joy-Con reverses raw Y/Z relative to Pro (SDL Switch sensor mapping). */
        const double side = hidJoyConRight ? -1 : 1;
        const double acc[3] = {side * raw[1], -raw[0], -side * raw[2]}, gyro[3] = {-side * raw[4], raw[3], side * raw[5]};
        runtime_motion_feed(acc, gyro, now - (uint64_t)(2 - k) * step);
    }
}
/* product: 0x2009 a Switch Pro Controller (motion or rumble), 0x2007 the right Joy-Con of a paired set (motion, PR #889) */
static IOReturn hid_open(int product) {   /* main queue */
    if (hidDevice) return kIOReturnSuccess;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ hidOut = dispatch_queue_create("totk.hid-out", DISPATCH_QUEUE_SERIAL); atexit(hid_quiet_at_exit); });
    hidManager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatching(hidManager, (__bridge CFDictionaryRef)@{@kIOHIDVendorIDKey: @0x057e, @kIOHIDProductIDKey: @(product)});
    NSSet *devices = (__bridge_transfer NSSet *)IOHIDManagerCopyDevices(hidManager);
    IOHIDDeviceRef d = (__bridge IOHIDDeviceRef)devices.anyObject;
    IOReturn r = d ? IOHIDDeviceOpen(d, kIOHIDOptionsTypeNone) : kIOReturnNotFound;
    if (r != kIOReturnSuccess) { CFRelease(hidManager); hidManager = NULL; return r; }
    hidDevice = (IOHIDDeviceRef)CFRetain(d);
    IOHIDDeviceScheduleWithRunLoop(hidDevice, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDDeviceRegisterInputReportCallback(hidDevice, hidReport, sizeof hidReport, hid_report, NULL);
    return kIOReturnSuccess;
}
static void hid_close_now(void) {   /* main queue */
    if (!hidDevice || hidMotion || hidRumble) return;
    IOHIDDeviceRegisterInputReportCallback(hidDevice, hidReport, sizeof hidReport, NULL, NULL);
    IOHIDDeviceRegisterInputReportWithTimeStampCallback(hidDevice, hidReport, sizeof hidReport, NULL, NULL);   /* hid_imu_start's (PR #309) */
    IOHIDDeviceUnscheduleFromRunLoop(hidDevice, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    IOHIDDeviceClose(hidDevice, kIOHIDOptionsTypeNone);
    CFRelease(hidDevice); hidDevice = NULL;
    CFRelease(hidManager); hidManager = NULL; imuProfile = NULL;
}
/* Close the device once nothing uses it, but only after the reports already queued have been written: the neutral rumble
   goes rumbleQueue, then hidOut, and a device closed before it would keep its last rumble. */
static void hid_close_unused(void) {   /* main queue */
    if (!hidDevice || hidMotion || hidRumble) return;
    dispatch_async(rumbleQueue ?: hidOut, ^{ dispatch_async(hidOut, ^{ dispatch_async(dispatch_get_main_queue(), ^{ hid_close_now(); }); }); });
}
static void hid_motion_start(void) {   /* main queue */
    if (!motionPad || hidMotion) return;
    const char *name = (motionPad.vendorName ?: @"controller").UTF8String;
    hidJoyConRight = [motionPad.productCategory containsString:@"Joy-Con"];
    if (hidDevice && !hidRumble) hid_close_now();   /* a device still closing (hid_close_unused) may be another pad's */
    IOReturn r = hid_open(hidJoyConRight ? 0x2007 : 0x2009);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "[gyro] %s: no usable GameController motion and no matching Switch HID device could be opened (0x%x): no motion from it\n", name, r);
        return;
    }
    hidMotion = YES; hidLast = 0; hidUserCalibration = NO;
    hidZeroSince = hidImuTriedAt = 0; hidImuRetries = 0; hidImuLive = NO;
    motionPad.motion.valueChangedHandler = nil;   /* sensors stay active: GameController keeps the IMU reporting */
    runtime_motion_deadzone_default(1.2); runtime_motion_live(1);
    fprintf(stderr, "[gyro] %s: reading the six-axis sensor from its HID reports (right Joy-Con for a paired set)\n", name);
    /* IMU on, then the calibrations; spaced out, as the controller drops subcommands sent back to back */
    static const uint8_t imuOn[1] = {0x01}, factory[5] = {0x20, 0x60, 0, 0, 24}, user[5] = {0x26, 0x80, 0, 0, 26};
    hid_subcommand(0x40, imuOn, sizeof imuOn);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ if (hidMotion) hid_subcommand(0x10, factory, sizeof factory); });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ if (hidMotion) hid_subcommand(0x10, user, sizeof user); });
    if (hidJoyConRight) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 750 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ const uint8_t mode = 0x30; if (hidMotion) hid_subcommand(0x03, &mode, 1); });
}
static void hid_motion_stop(void) {   /* main queue */
    if (!hidMotion) return;
    runtime_gamepad_switch_left(NULL, -1, -1);
    runtime_gamepad_switch_right(NULL, -1, -1);
    hidMotion = NO; hid_close_unused(); runtime_motion_deadzone_default(0);
}
static BOOL hid_rumble_start(GCController *c) {   /* main queue, rumbleQueue made */
    const char *name = (c.vendorName ?: @"controller").UTF8String;
    IOReturn r = hid_open(0x2009);
    if (r != kIOReturnSuccess) { fprintf(stderr, "[rumble] %s: no Switch Pro HID device could be opened (0x%x): Core Haptics kept\n", name, r); return NO; }
    static const uint8_t on[1] = {0x01};
    hid_subcommand(0x48, on, sizeof on);
    hidRumble = YES; hidRumblePad = c;
    IOHIDDeviceRef d = (IOHIDDeviceRef)CFRetain(hidDevice);
    dispatch_async(rumbleQueue, ^{ if (rumbleHid) CFRelease(rumbleHid); rumbleHid = d; hidSentAt = 0; hidQuietSent = 0; memset(hidHeld, 0, sizeof hidHeld); });
    fprintf(stderr, "[rumble] %s: HD rumble written to its HID reports\n", name);
    return YES;
}
static void hid_rumble_stop(void) {   /* main queue */
    if (!hidRumble) return;
    hidRumble = NO; hidRumblePad = nil;
    dispatch_async(rumbleQueue, ^{   /* after the timer's last report: leave the controller neutral */
        static const uint8_t neutral[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
        if (rumbleHid) { hid_write(rumbleHid, 0x10, neutral, sizeof neutral); CFRelease(rumbleHid); } rumbleHid = NULL;
    });
    hid_close_unused();
}
#else
static void hid_motion_start(void) {
    static int logged;
    if (!logged) { logged = 1; fprintf(stderr, "[gyro] GameController reports no motion for this controller; the raw HID fallback is macOS only\n"); }
}
static void hid_motion_stop(void) {}
#endif

/* The trigger resistance follows the bow, which is read from the game's memory: macOS only (iOS: stubs, never on). */
#if TARGET_OS_OSX
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include "bow_state.h"
#include "game_version.h"   /* Tears of the Kingdom 1.4.3's scene slot and actor name offsets, for the bow state */
/* ---- DualSense adaptive triggers ----
   R2 (ZR: draw the bow) resists like a bowstring, more the further it is pressed, but only while Link is drawing the
   bow; at any other time both triggers are slack, and L2 never resists. The game sends a controller nothing for this, so
   a thread reads it from the game's memory every 10 ms (runtime/bow_state.h: Breath of the Wild 1.9.0 and Tears of the
   Kingdom 1.4.3; other versions leave the triggers alone) and R2 changes through GameController only when the state does.
   It follows the draw within a frame; over Bluetooth the controller takes another 30-45 ms, so the very start of the pull
   is slack. On by default: dualsense-triggers.txt next to controls.txt saying "off" turns it off (TOTK_DUALSENSE_TRIGGERS
   =on / off overrides the file; an agent's test session, TOTK_SLOT_OWNER claude / codex, leaves it off unless set).
   R2 resists only while bow_resists (bow_state.h) says so, and lets go the moment one part does not: the draw ends, a menu
   pauses the world (Tears of the Kingdom: BOW_PAUSED; Breath of the Wild's timer drops to 0), the state cannot be read (a
   load, the title screen), the settings panel or the on-screen keyboard takes the controller (until its buttons are
   released), or the app leaves the front (it lets go then, as macOS would, and sends the state again when it is back).
   Both triggers go back to off when the controller goes away (another DualSense still connected takes the bow), when the
   settings turn them off, or when the game ends. TOTK_TRIGGER_LOG=1 logs every change of the triggers' mode and status,
   and why R2 holds or lets go (diagnostics). */
enum { BOW_POLL_MS = 10 };
static GCDualSenseGamepad *triggerPad;   /* main queue */
static int bowState = BOW_UNKNOWN;       /* main queue: the bow thread's last reading */
static BOOL gameInput = YES, appActive;  /* main queue: the game gets the controllers; the app is in front */
static BOOL r2Resists;                   /* main queue: what R2 was last set to */
static int triggerChoice = -1;   /* main queue: the settings panel's pick, or the file's word once read; -1 not yet */
static int triggers_wanted(void) {
    const char *e = getenv("TOTK_DUALSENSE_TRIGGERS"), *owner = getenv("TOTK_SLOT_OWNER");
    if (e && *e) {
        if (!strcmp(e, "on") || !strcmp(e, "1")) return 1;
        if (!strcmp(e, "off") || !strcmp(e, "0")) return 0;
        runtime_fail("TOTK_DUALSENSE_TRIGGERS must be on or off");
    }
    if (owner && (!strcmp(owner, "claude") || !strcmp(owner, "codex"))) return 0;
    if (triggerChoice >= 0) return triggerChoice;
    char word[16] = {0}, *w = word;
    if (!runtime_setting_word("dualsense-triggers.txt", word, sizeof word)) return triggerChoice = 1;
    while (*w && strchr(" \t\n\v\f\r", *w)) w++;   /* the blanks around the word go, as for every one-word file */
    for (size_t n = strlen(w); n && strchr(" \t\n\v\f\r", w[n - 1]); n--) w[n - 1] = 0;
    if (!strcmp(w, "off")) return triggerChoice = 0;
    if (strcmp(w, "on")) fprintf(stderr, "[triggers] dualsense-triggers.txt: not on or off; on\n");
    return triggerChoice = 1;
}
static int bow_state_supported(void) {
#if defined(GAME_BOTW190) || defined(TOTK_GAME_V143)
    return 1;
#else
    return 0;
#endif
}
#if defined(GAME_BOTW190) || defined(TOTK_GAME_V143)
static int bow_read(uint64_t address, void *out, size_t size) {   /* fails instead of faulting on a bad address */
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), address, size, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == size;
}
#endif
static int bow_state_now(void) {   /* the bow thread only */
#if defined(GAME_BOTW190)
    uint64_t main_base = (uintptr_t)runtime_module_base("main");
    return main_base ? bow_state_botw190(main_base, bow_read) : BOW_UNKNOWN;
#elif defined(TOTK_GAME_V143)
    static BowTotkCache player;
    uint64_t main_base = (uintptr_t)runtime_module_base("main");
    return main_base ? bow_state_totk143(main_base, GAME_ADDR(0x4663f08), GAME_ACTOR_NAME, bow_read, &player) : BOW_UNKNOWN;
#else
    return BOW_UNKNOWN;
#endif
}
/* main queue: R2 as bow_resists says; sent when that changes, or always (force: a new pad, the app active again, whose
   effects macOS dropped) */
static void triggers_r2(BOOL force) {
    if (!triggerPad) return;
    BOOL resists = bow_resists(bowState, gameInput, appActive);
    if (resists == r2Resists && !force) return;
    if (resists) [triggerPad.rightTrigger setModeSlopeFeedbackWithStartPosition:0.05f endPosition:0.95f startStrength:0.2f endStrength:0.8f];
    else [triggerPad.rightTrigger setModeOff];
    if (resists != r2Resists && getenv("TOTK_TRIGGER_LOG"))
        fprintf(stderr, "[triggers] R2 %s\n", resists ? "bowstring" : !appActive ? "slack: the app is not in front" :
                !gameInput ? "slack: the settings panel or keyboard has the controller" : bowState == BOW_PAUSED ? "slack: the game is paused" :
                bowState == BOW_UNKNOWN ? "slack: no bow state" : "slack: the bow is not drawn");
    r2Resists = resists;
}
static void triggers_send(void) {   /* main queue: both triggers, at attach and when the app is active again */
    if (!triggerPad) return;
    [triggerPad.leftTrigger setModeOff];
    triggers_r2(YES);
}
static void triggers_off(void) {
    if (!triggerPad) return;
    [triggerPad.leftTrigger setModeOff]; [triggerPad.rightTrigger setModeOff]; triggerPad = nil; r2Resists = NO;
}
/* The game ends: both triggers off, and a moment for the report to reach the controller. At exit, and when the window
   closes (frontend.m): its _exit skips atexit, and a save can keep the game running a while after it. */
void runtime_triggers_release(void) { if (triggerPad) { triggers_off(); usleep(50000); } }
/* The bow thread: every 10 ms the bow's state and whether the game gets the controllers; the main queue hears of a change.
   A line the first time the state can be read (the world is up), and every 30 s one with the draws that ended, if any;
   TOTK_TRIGGER_LOG=1 also logs each change of the state. */
static void *bow_watch(void *arg) {
    (void)arg;
    pthread_setname_np("bow-state");
    static const char *names[] = {"unknown", "not drawn", "drawn", "paused"};
    BOOL found = NO, log = getenv("TOTK_TRIGGER_LOG") != NULL;
    int last = BOW_UNKNOWN, lastInput = 1;
    unsigned draws = 0; uint64_t since = 0, longest = 0, logAt = runtime_now_ns();
    for (;;) {
        int state = bow_state_now(), input = runtime_gamepad_game_input();
        uint64_t t = runtime_now_ns();
        if (state != BOW_UNKNOWN && !found) { found = YES; fprintf(stderr, "[triggers] bow state found: R2 follows the bow\n"); }
        if (state != last || input != lastInput) {
            if ((state == BOW_DRAWN) != (last == BOW_DRAWN)) {
                if (state == BOW_DRAWN) since = t;
                else { draws++; if (t - since > longest) longest = t - since; }
            }
            if (log && state != last) fprintf(stderr, "[triggers] bow %s\n", names[state + 1]);
            if (log && input != lastInput) fprintf(stderr, "[triggers] the game %s the controllers\n", input ? "has" : "does not have");
            last = state; lastInput = input;
            dispatch_async(dispatch_get_main_queue(), ^{ bowState = state; gameInput = input; triggers_r2(NO); });
        }
        if (t - logAt >= 30000000000ull) {
            if (draws) fprintf(stderr, "[triggers] last 30 s: bow drawn %u times, longest %.1f s\n", draws, longest / 1e9);
            draws = 0; longest = 0; logAt = t;
        }
        usleep(BOW_POLL_MS * 1000);
    }
    return NULL;
}
/* TOTK_TRIGGER_LOG=1: a line whenever a trigger's mode or status changes or its arm moves a tenth of its travel (status:
   0/1 feedback no load / load applied, 7-9 slope feedback ready / applying load / finished, -1 unknown). */
static void triggers_watch(void) {   /* main queue, every 250 ms */
    static long last[2][3] = {{-9, -9, -9}, {-9, -9, -9}};
    GCDualSenseGamepad *pad = triggerPad;
    if (!pad) return;
    GCDualSenseAdaptiveTrigger *t[2] = {pad.leftTrigger, pad.rightTrigger};
    for (int i = 0; i < 2; i++) {
        long now[3] = {(long)t[i].mode, (long)t[i].status, lroundf(t[i].armPosition * 10)};
        if (!memcmp(now, last[i], sizeof now)) continue;
        memcpy(last[i], now, sizeof now);
        fprintf(stderr, "[triggers] %s mode %ld status %ld arm %.2f pressed %.2f\n", i ? "R2" : "L2", now[0], now[1], t[i].armPosition, t[i].value);
    }
}
static void triggers_apply(GCController *c, const char *name) {   /* main queue */
    if (![c.extendedGamepad isKindOfClass:GCDualSenseGamepad.class] || !triggers_wanted()) return;
    if (!bow_state_supported()) { fprintf(stderr, "[triggers] %s: no bow state for this game version; triggers left alone\n", name); return; }
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        atexit(runtime_triggers_release);
        appActive = NSApp.active;   /* from here the two notifications keep it */
        [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification *n) { (void)n; appActive = YES; triggers_send(); }];
        [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationWillResignActiveNotification object:nil queue:NSOperationQueue.mainQueue
                                                    usingBlock:^(NSNotification *n) { (void)n; appActive = NO; triggers_r2(YES); }];
        if (getenv("TOTK_TRIGGER_LOG")) {
            static dispatch_source_t watch;
            watch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
            dispatch_source_set_timer(watch, dispatch_time(DISPATCH_TIME_NOW, 0), 250 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
            dispatch_source_set_event_handler(watch, ^{ triggers_watch(); });
            dispatch_resume(watch);
        }
        pthread_t thread; pthread_attr_t attr;
        pthread_attr_init(&attr); pthread_attr_set_qos_class_np(&attr, QOS_CLASS_UTILITY, 0);
        int r = pthread_create(&thread, &attr, bow_watch, NULL);
        pthread_attr_destroy(&attr);
        if (r) fprintf(stderr, "[triggers] the bow thread could not start (%d): R2 stays slack\n", r);
        else pthread_detach(thread);
    });
    if (triggerPad && triggerPad != c.extendedGamepad) { [triggerPad.leftTrigger setModeOff]; [triggerPad.rightTrigger setModeOff]; }   /* a newer pad takes the bow; the old one goes slack */
    triggerPad = (GCDualSenseGamepad *)c.extendedGamepad;
    triggers_send();
    fprintf(stderr, "[triggers] %s: R2 is a bowstring while the bow is drawn (slope 0.2 -> 0.8 over 5-95 %%), L2 slack\n", name);
}
/* The DualSense to take the bow when none has it (the settings turned it on, or the one that had it went away): the pad in
   use, else any connected one; never `gone`, which is disconnecting. */
static void triggers_pick(GCController *gone) {   /* main queue */
    if (triggerPad) return;
    GCController *current = GCController.current, *pick = current != gone && [current.extendedGamepad isKindOfClass:GCDualSenseGamepad.class] ? current : nil;
    for (GCController *c in GCController.controllers) if (!pick && c != gone && [c.extendedGamepad isKindOfClass:GCDualSenseGamepad.class]) pick = c;
    if (pick) triggers_apply(pick, (pick.vendorName ?: @"controller").UTF8String);
}

/* The settings panel (key 1, runtime/settings_panel.h; main queue). TOTK_DUALSENSE_TRIGGERS or an agent's test session decide
   first: the row then shows that value, greyed out. A pick is saved in dualsense-triggers.txt and acts at once: on puts the
   connected DualSense's R2 on the bow (as at its attach), off lets both triggers go. */
int runtime_triggers_available(void) { return bow_state_supported(); }
int runtime_triggers_locked(void) {
    const char *e = getenv("TOTK_DUALSENSE_TRIGGERS"), *owner = getenv("TOTK_SLOT_OWNER");
    return (e && *e) || (owner && (!strcmp(owner, "claude") || !strcmp(owner, "codex")));
}
int runtime_triggers_choice(void) {
    const char *e = getenv("TOTK_DUALSENSE_TRIGGERS");
    if (e && *e) return !strcmp(e, "on") || !strcmp(e, "1");   /* shown only: a bad value stops the game at a DualSense's attach, as before */
    return triggers_wanted();
}
void runtime_triggers_choose(int on) {
    if (runtime_triggers_locked() || !bow_state_supported()) return;
    triggerChoice = !!on;
    runtime_setting_save("dualsense-triggers.txt", on ? "on" : "off", "DualSense trigger setting");
    if (!on) {   /* every DualSense's triggers go slack, also one that an earlier pad took the bow from */
        for (GCController *c in GCController.controllers)
            if ([c.extendedGamepad isKindOfClass:GCDualSenseGamepad.class]) { GCDualSenseGamepad *p = (GCDualSenseGamepad *)c.extendedGamepad; [p.leftTrigger setModeOff]; [p.rightTrigger setModeOff]; }
        triggerPad = nil; r2Resists = NO; fprintf(stderr, "[triggers] off (settings): both triggers slack\n"); return;
    }
    triggers_pick(nil);
}
#else
int runtime_triggers_available(void) { return 0; }
int runtime_triggers_locked(void) { return 1; }
int runtime_triggers_choice(void) { return 0; }
void runtime_triggers_choose(int on) { (void)on; }
void runtime_triggers_release(void) {}
#endif

/* ---- motion from the input reports of a pad GameController gives none (2026-10-23): runtime/hid_imu.h ---- */
#if TARGET_OS_OSX
static uint64_t imuLast;   /* main run loop: the time of the last sample */
static void hid_imu_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t id, uint8_t *p, CFIndex len, uint64_t stamp) {   /* main run loop */
    (void)ctx; (void)sender; (void)type;
    const HidImuProfile *pr = imuProfile;
    if (result != kIOReturnSuccess || !pr || id != pr->reportId) return;
    if (len < pr->length) {
        static BOOL said;
        if (!said) { said = YES; fprintf(stderr, "[gyro] %04x:%04x sends %ld-byte reports and the sensor needs %d (old firmware?): no motion from it\n", pr->vendor, pr->product, (long)len, pr->length); }
        return;
    }
    double acc[3], gyro[3];
    if (!hid_imu_decode(pr, id, p, len, acc, gyro)) return;
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    runtime_motion_feed(acc, gyro, hid_imu_time((uint64_t)((__uint128_t)stamp * tb.numer / tb.denom), runtime_now_ns(), &imuLast));   /* the report's own receive time: see hid_imu.h */
}
static BOOL hid_imu_start(GCController *c) {   /* main queue: c has no GCMotion, or a DualShock 4 whose GCMotion yields nothing */
    if (![c.productCategory isEqualToString:GCProductCategoryHID] && ![c.productCategory isEqualToString:@"DualShock 4"]) return NO;   /* not an Xbox pad that merely sits beside one of these */
    NSMutableArray *matching = [NSMutableArray array];
    for (int i = 0; i < HID_IMU_PROFILES; i++)
        [matching addObject:@{@kIOHIDVendorIDKey: @(hid_imu_profiles[i].vendor), @kIOHIDProductIDKey: @(hid_imu_profiles[i].product)}];
    IOHIDManagerRef m = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    IOHIDManagerSetDeviceMatchingMultiple(m, (__bridge CFArrayRef)matching);
    IOHIDManagerOpen(m, kIOHIDOptionsTypeNone);
    NSSet *devices = (__bridge_transfer NSSet *)IOHIDManagerCopyDevices(m);
    IOHIDDeviceRef d = (__bridge IOHIDDeviceRef)devices.anyObject;
    if (!d) { CFRelease(m); return NO; }
    int vendor = 0, product = 0;
    CFTypeRef cv = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDVendorIDKey)), cp = IOHIDDeviceGetProperty(d, CFSTR(kIOHIDProductIDKey));
    if (cv) CFNumberGetValue(cv, kCFNumberIntType, &vendor);
    if (cp) CFNumberGetValue(cp, kCFNumberIntType, &product);
    const HidImuProfile *found = hid_imu_find(vendor, product);
    const char *name = (c.vendorName ?: @"controller").UTF8String;
    IOReturn r = found ? IOHIDDeviceOpen(d, kIOHIDOptionsTypeNone) : kIOReturnNotFound;
    if (r != kIOReturnSuccess) {
        if (found) fprintf(stderr, "[gyro] %s: HID device %04x:%04x found but did not open (0x%x): no motion from it\n", name, vendor, product, r);
        CFRelease(m); return NO;
    }
    hid_motion_stop(); hid_close_now();   /* the shared device (PR #920): close it now unless rumble uses it */
    if (hidDevice) { fprintf(stderr, "[gyro] %s: the Switch HID device is in use for rumble: no motion from %04x:%04x\n", name, vendor, product); IOHIDDeviceClose(d, kIOHIDOptionsTypeNone); CFRelease(m); return NO; }
    hidManager = m; hidDevice = (IOHIDDeviceRef)CFRetain(d); imuProfile = found; hidMotion = YES;
    IOHIDDeviceScheduleWithRunLoop(hidDevice, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    imuLast = 0;
    IOHIDDeviceRegisterInputReportWithTimeStampCallback(hidDevice, hidReport, sizeof hidReport, hid_imu_report, NULL);
    motionPad = c; runtime_motion_deadzone_default(1.2); runtime_motion_live(1);
    fprintf(stderr, "[gyro] %s: no motion from GameController; reading the six-axis sensor from its HID reports (%04x:%04x)\n", name, vendor, product);
    return YES;
}
#else
static BOOL hid_imu_start(GCController *c) {   /* the raw HID fallback is macOS only (IOKit) */
    (void)c;
    return NO;
}
#endif

/* ---- controllers ---- */
void runtime_feedback_attach(GCController *c) {   /* main queue */
    const char *name = (c.vendorName ?: @"controller").UTF8String;
    if (runtime_rumble_enabled()) {
        rumble_start();
        BOOL tookHid = NO;
#if TARGET_OS_OSX
        hid_rumble_stop();
        const char *viaHid = getenv("TOTK_RUMBLE_HID");
        tookHid = [c.productCategory isEqualToString:@"Switch Pro Controller"] && !(viaHid && !strcmp(viaHid, "0")) && hid_rumble_start(c);
        if (tookHid)
            dispatch_async(rumbleQueue, ^{ for (int s = 0; s < 2; s++) { if (players[s]) [players[s] stopAtTime:CHHapticTimeImmediate error:nil]; forget_side(s); } rumblePad = nil; });
#endif
        if (tookHid) {
            /* nothing further: hid_rumble_start writes the game's rumble to the pad itself */
        } else if (c.haptics) {
            dispatch_async(rumbleQueue, ^{ for (int s = 0; s < 2; s++) { if (players[s]) [players[s] stopAtTime:CHHapticTimeImmediate error:nil]; forget_side(s); retryAt[s] = 0; } rumblePad = c; });
            NSMutableArray *where = [NSMutableArray array]; for (NSString *l in c.haptics.supportedLocalities) [where addObject:l];
            fprintf(stderr, "[rumble] %s: haptics (%s)\n", name, [where componentsJoinedByString:@", "].UTF8String);
        } else {
            fprintf(stderr, "[rumble] %s has no haptics\n", name);
            hid_rumble_attach(c);
        }
    }
#if TARGET_OS_OSX
    triggers_apply(c, name);
#endif
    if (runtime_gyro_enabled()) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *axes = getenv("TOTK_GYRO_AXES"); int r = runtime_motion_axes(axes);
            if (r < 0) runtime_fail("TOTK_GYRO_AXES must name x, y and z once each, e.g. \"-x,y,-z\"");
            if (r > 0) fprintf(stderr, "[gyro] axes remapped by TOTK_GYRO_AXES=%s\n", axes);
        });
        GCMotion *m = c.motion;
        if ([c.productCategory isEqualToString:@"DualShock 4"]) {   /* its GCMotion yields 0 samples: read its HID IMU instead */
            GCController *before = motionPad;
            if (hid_imu_start(c)) {   /* the v1 Bluetooth profile; v2 or USB has no matching profile and keeps GCMotion below */
                if (before && before != c) {   /* one motion feed only, as below (hid_imu_start has made c the feed) */
                    before.motion.valueChangedHandler = nil;
                    if (before.motion.sensorsRequireManualActivation) before.motion.sensorsActive = NO;
                }
                return;
            }
        } else if (!m || !m.hasRotationRate) {
            if ([c.productCategory containsString:@"Joy-Con"]) {   /* PR #889: a paired Joy-Con's right HID sensor */
                hid_motion_stop(); zeroSince = 0; motionSeen = hidAsked = NO;
                motionPad = c; hid_motion_start();
            } else if (!hid_imu_start(c)) fprintf(stderr, "[gyro] %s has no gyro: no motion from it\n", name);   /* PR #309: IMU in its HID reports (8BitDo Ultimate 2) */
            return;
        }
        if (motionPad && motionPad != c) {   /* one motion feed only: two controllers would be mixed into one fusion */
            motionPad.motion.valueChangedHandler = nil;
            /* A controller that needed manual activation keeps its sensors powered until told otherwise. */
            if (motionPad.motion.sensorsRequireManualActivation) motionPad.motion.sensorsActive = NO;
            runtime_motion_live(0);
        }
        if (m.sensorsRequireManualActivation) m.sensorsActive = YES;
        hid_motion_stop(); zeroSince = 0; motionSeen = hidAsked = NO; runtime_motion_deadzone_default(0);
        m.valueChangedHandler = ^(GCMotion *changed) { motion_sample(changed); };
        motionPad = c; runtime_motion_live(1);
        fprintf(stderr, "[gyro] %s: motion sensors on (gyro%s)\n", name, m.hasGravityAndUserAcceleration ? ", gravity" : ", acceleration");
    }
}
void runtime_feedback_detach(GCController *c) {   /* main queue */
    hid_rumble_detach(c);
    if (runtime_rumble_enabled() && rumbleQueue)
        dispatch_async(rumbleQueue, ^{ if (rumblePad == c) { for (int s = 0; s < 2; s++) forget_side(s); rumblePad = nil; } });
#if TARGET_OS_OSX
    if (triggerPad && c.extendedGamepad == triggerPad) { triggers_off(); triggers_pick(c); }   /* e.g. USB unplugged, Bluetooth still on */
    if (hidRumblePad == c) hid_rumble_stop();
#endif
    if (motionPad == c) {
        c.motion.valueChangedHandler = nil; hid_motion_stop(); motionPad = nil; runtime_motion_live(0);
    }
}
/* iOS deactivates a manual-activation controller's sensors while the app is in the background (Settings, Control
   Center); without this, gyro aiming stays dead until the controller reconnects. */
void runtime_feedback_active(void) {   /* main queue */
    if (motionPad && motionPad.motion.sensorsRequireManualActivation) {
        motionPad.motion.sensorsActive = YES;
        fprintf(stderr, "[gyro] motion sensors reactivated after the app returned\n");
    }
}
