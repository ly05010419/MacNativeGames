// Core Haptics adapts the game's bands; it cannot reproduce the Switch's exact waveforms.
// All engines and players belong to one serial queue. Guest threads enqueue ordered snapshots only.
#import "haptics_apple.h"
#import <CoreHaptics/CoreHaptics.h>
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#import <AVFAudio/AVFAudio.h>
#else
#import <AppKit/AppKit.h>
#endif
#include <math.h>
#include <pthread.h>

@interface DmRumbleChannel : NSObject
@property(nonatomic) CHHapticEngine *engine;
@property(nonatomic) id<CHHapticAdvancedPatternPlayer> pattern;
@property(nonatomic) int player;
@property(nonatomic) uint64_t generation;
@property(nonatomic) uint64_t silenceGeneration;
- (void)apply:(dm_haptic_level)level;
- (void)stop;
@end

static dispatch_queue_t queue;
static NSMutableArray<DmRumbleChannel *> *channels[DM_PLAYERS];
static GCController *controllers[DM_PLAYERS];
#if TARGET_OS_IPHONE
static BOOL phoneSupported;
#endif
static float gain = 1;
static pthread_mutex_t routeLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned errors;
static void report(NSError *e) {
    if (++errors <= 8 || errors % 64 == 0) fprintf(stderr, "[RED] haptics: %s\n", e.localizedDescription.UTF8String ?: "engine unavailable");
}

@implementation DmRumbleChannel
- (void)stop {
    self.generation++; self.silenceGeneration++;
    self.engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) { (void)reason; }; self.engine.resetHandler = ^{};
    [self.pattern stopAtTime:CHHapticTimeImmediate error:nil]; self.pattern = nil;
    [self.engine stopWithCompletionHandler:nil];
}
- (void)apply:(dm_haptic_level)level {
    uint64_t silence = ++self.silenceGeneration;
    NSError *error = nil;
    if (!self.pattern) {
        if (level.intensity == 0) return;
        if (![self.engine startAndReturnError:&error]) { report(error); dm_haptics_clear(self.player); return; }
        __weak DmRumbleChannel *weak = self;
        uint64_t generation = ++self.generation;
        void (^reset)(void) = ^{
            dispatch_async(queue, ^{
                DmRumbleChannel *c = weak; if (!c || c.generation != generation) return;
                c.pattern = nil;
                dm_haptics_clear(c.player);   // queued effects from before an interruption cannot restart
            });
        };
        self.engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) { (void)reason; reset(); };
        self.engine.resetHandler = reset;
        CHHapticEvent *event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous parameters:@[
            [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1],
            [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.5]
        ] relativeTime:0 duration:30];
        CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error];
        self.pattern = pattern ? [self.engine createAdvancedPlayerWithPattern:pattern error:&error] : nil;
        if (!self.pattern) { report(error); dm_haptics_clear(self.player); return; }
        self.pattern.loopEnabled = YES;
        self.pattern.isMuted = YES;
        if (![self.pattern startAtTime:CHHapticTimeImmediate error:&error]) { report(error); self.pattern = nil; dm_haptics_clear(self.player); return; }
    }
    // Controls live outside the loop: the loop boundary never replays an initial intensity.
    NSArray *parameters = @[
        [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:level.intensity relativeTime:0],
        [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticSharpnessControl value:level.sharpness - 0.5f relativeTime:0]
    ];
    if (![self.pattern sendParameters:parameters atTime:CHHapticTimeImmediate error:&error]) {
        report(error); [self stop]; dm_haptics_clear(self.player); return;
    }
    self.pattern.isMuted = NO;
    if (level.intensity == 0) {
        // Reuse a warm player for nearby pulses, then release idle hardware. This never times out an active effect.
        __weak DmRumbleChannel *weak = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), queue, ^{
            DmRumbleChannel *c = weak; if (c && c.silenceGeneration == silence) [c stop];
        });
    }
}
@end

static void apply(dm_haptic_state s) {
    dispatch_async(queue, ^{ @autoreleasepool {
        if (s.epoch != dm_haptics_epoch(s.player)) return;
        NSArray<DmRumbleChannel *> *out = channels[s.player];
        for (NSUInteger i = 0; i < out.count; i++) {
            dm_haptic_level level = out.count == 2 ? dm_haptics_level(&s.motor[i], 1, gain) : dm_haptics_level(s.motor, 2, gain);
            [out[i] apply:level];
        }
    } });
}
static void add_engine(int p, CHHapticEngine *engine) {
    if (!engine) return;
    engine.playsHapticsOnly = YES; engine.autoShutdownEnabled = NO;
    DmRumbleChannel *c = [DmRumbleChannel new]; c.engine = engine; c.player = p;
    [channels[p] addObject:c];
}
static void configure(int p, GCController *controller) {
    for (DmRumbleChannel *c in channels[p]) [c stop];
    channels[p] = [NSMutableArray new];
    if (controller) {
        GCDeviceHaptics *h = controller.haptics;
        NSSet *localities = h.supportedLocalities;
        if ([localities containsObject:GCHapticsLocalityLeftHandle] && [localities containsObject:GCHapticsLocalityRightHandle]) {
            CHHapticEngine *left = [h createEngineWithLocality:GCHapticsLocalityLeftHandle];
            CHHapticEngine *right = [h createEngineWithLocality:GCHapticsLocalityRightHandle];
            if (left && right) { add_engine(p, left); add_engine(p, right); }
        }
        if (!channels[p].count) add_engine(p, [h createEngineWithLocality:GCHapticsLocalityDefault]);
    }
#if TARGET_OS_IPHONE
    else if (!p && phoneSupported) {
        NSError *error = nil;
        CHHapticEngine *engine = [[CHHapticEngine alloc] initWithAudioSession:AVAudioSession.sharedInstance error:&error];
        if (!engine) report(error);
        add_engine(p, engine);
    }
#endif
    dm_haptics_route(p, channels[p].count != 0);
}
void dm_haptics_apple_controller(int p, GCController *controller) {
    if (!queue || p < 0 || p >= DM_PLAYERS) return;
    pthread_mutex_lock(&routeLock);
    if (controllers[p] != controller) {
        controllers[p] = controller;
        dm_haptics_route(p, 0);   // invalidate queued work before changing hardware
        dispatch_sync(queue, ^{ configure(p, controller); });
    }
    pthread_mutex_unlock(&routeLock);
}
static void suspend(unsigned reason, BOOL on) {
    dm_haptics_suspend(reason, on);
    void (^stop)(void) = ^{
        for (int p = 0; p < DM_PLAYERS; p++) for (DmRumbleChannel *c in channels[p]) [c stop];
    };
    if (on) { if (dispatch_get_specific(&queue)) stop(); else dispatch_sync(queue, stop); }
}
void dm_haptics_apple_shutdown(void) {
    if (queue) suspend(DM_HAPTICS_INACTIVE, YES);
}
void dm_haptics_apple_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("dm.rumble", DISPATCH_QUEUE_SERIAL);
        dispatch_queue_set_specific(queue, &queue, &queue, NULL);
        const char *e = getenv("DM_RUMBLE_GAIN");
        if (e) { float v = strtof(e, NULL); if (isfinite(v)) gain = fmaxf(0, fminf(3, v)); }
        e = getenv("DM_RUMBLE");
        dm_haptics_init(apply, !e || atoi(e) != 0);
#if TARGET_OS_IPHONE
        phoneSupported = CHHapticEngine.capabilitiesForHardware.supportsHaptics;
#endif
        dispatch_sync(queue, ^{ configure(0, nil); });
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
#if TARGET_OS_IPHONE
        [nc addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; suspend(DM_HAPTICS_INACTIVE, YES); }];
        [nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; suspend(DM_HAPTICS_INACTIVE, NO); }];
        [nc addObserverForName:UIApplicationWillTerminateNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_haptics_apple_shutdown(); }];
        [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            suspend(DM_HAPTICS_INTERRUPTED, [n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue] == AVAudioSessionInterruptionTypeBegan);
        }];
        [nc addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            (void)n; dm_haptics_clear(0);
        }];
#else
        [nc addObserverForName:NSApplicationWillResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; suspend(DM_HAPTICS_INACTIVE, YES); }];
        [nc addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; suspend(DM_HAPTICS_INACTIVE, NO); }];
        [nc addObserverForName:NSApplicationWillTerminateNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_haptics_apple_shutdown(); }];
#endif
        atexit(dm_haptics_apple_shutdown);
    });
}
