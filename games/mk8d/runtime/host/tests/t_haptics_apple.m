// Run the actual serial-queue adapter with fake engines: no controller, actuator or app is opened.
#import <Foundation/Foundation.h>
#include <assert.h>
#include <time.h>

static int test_clock_gettime(clockid_t clock, struct timespec *t);
static dispatch_time_t test_dispatch_time(dispatch_time_t when, int64_t delta);
static void test_dispatch_after(dispatch_time_t when, dispatch_queue_t target, dispatch_block_t block);
// dispatch_after need not submit its block before a sleep followed by dispatch_sync ends.
// Keep the real serial queue, but advance the adapter's clock and deliver its timers explicitly.
#define clock_gettime test_clock_gettime
#define dispatch_time test_dispatch_time
#define dispatch_after test_dispatch_after
#import "../haptics_apple.m"
#undef clock_gettime
#undef dispatch_time
#undef dispatch_after

@interface TestTimer : NSObject
@property dispatch_time_t deadline;
@property(copy) dispatch_block_t block;
@end
@implementation TestTimer
@end
static uint64_t testTime = 100 * NSEC_PER_SEC;
static NSMutableArray<TestTimer *> *timers;   // owned by the adapter's serial queue, like its engines
static int test_clock_gettime(clockid_t clock, struct timespec *t) {
    assert(clock == CLOCK_MONOTONIC);
    t->tv_sec = testTime / NSEC_PER_SEC; t->tv_nsec = testTime % NSEC_PER_SEC;
    return 0;
}
static dispatch_time_t test_dispatch_time(dispatch_time_t when, int64_t delta) {
    assert(when == DISPATCH_TIME_NOW && delta >= 0);
    return testTime + delta;
}
static void test_dispatch_after(dispatch_time_t when, dispatch_queue_t target, dispatch_block_t block) {
    assert(target == queue && dispatch_get_specific(&queue));
    TestTimer *timer = [TestTimer new]; timer.deadline = when; timer.block = block;
    [timers addObject:timer];
}

@interface TestPlayer : NSObject
@property BOOL loopEnabled, isMuted;
@property int starts, stops;
@property NSMutableArray<NSNumber *> *levels;
@property BOOL failStart, failSend;
@end
@implementation TestPlayer
- (instancetype)init { if ((self = [super init])) _levels = [NSMutableArray new]; return self; }
- (BOOL)startAtTime:(NSTimeInterval)t error:(NSError **)e { (void)t; (void)e; self.starts++; return !self.failStart; }
- (BOOL)stopAtTime:(NSTimeInterval)t error:(NSError **)e { (void)t; (void)e; self.stops++; return YES; }
- (BOOL)sendParameters:(NSArray<CHHapticDynamicParameter *> *)p atTime:(NSTimeInterval)t error:(NSError **)e {
    (void)t; (void)e;
    if (self.failSend) return NO;
    for (CHHapticDynamicParameter *v in p) if ([v.parameterID isEqual:CHHapticDynamicParameterIDHapticIntensityControl]) [self.levels addObject:@(v.value)];
    return YES;
}
@end
@interface TestEngine : NSObject
@property BOOL playsHapticsOnly, autoShutdownEnabled;
@property(copy) void (^resetHandler)(void);
@property(copy) void (^stoppedHandler)(CHHapticEngineStoppedReason);
@property TestPlayer *testPlayer;
@property int starts, stops;
@property BOOL fail;
@property BOOL failPattern, failPlayer, failSend, handlersAtStart, resetDuringStart, noAdvanced, broken;
@property int plainPlayers;
@property(readonly) NSTimeInterval currentTime;
@property(copy) void (^startingReset)(void);
@end
@implementation TestEngine
- (NSTimeInterval)currentTime { return now(); }   // an engine's clock runs like the host's
- (BOOL)startAndReturnError:(NSError **)e {
    self.starts++; self.handlersAtStart = self.resetHandler && self.stoppedHandler;
    self.startingReset = self.resetHandler;
    if (self.resetDuringStart && self.resetHandler) self.resetHandler();
    if ((self.fail || self.broken) && e) *e = [NSError errorWithDomain:CoreHapticsErrorDomain code:CHHapticErrorCodeServerInitFailed userInfo:nil];
    return !self.fail && !self.broken;
}
- (void)stopWithCompletionHandler:(CHHapticCompletionHandler)handler { self.stops++; if (handler) handler(nil); }
- (id<CHHapticAdvancedPatternPlayer>)createAdvancedPlayerWithPattern:(CHHapticPattern *)p error:(NSError **)e {
    (void)p;
    if (self.failPattern || self.noAdvanced) {   // gamecontrollerd's answer when it offers no advanced players, after which the engine is unusable
        if (e) *e = [NSError errorWithDomain:NSCocoaErrorDomain code:NSXPCConnectionInterrupted userInfo:nil];
        self.broken = YES; return nil;
    }
    self.testPlayer = [TestPlayer new]; self.testPlayer.failStart = self.failPlayer; self.testPlayer.failSend = self.failSend;
    return (id)self.testPlayer;
}
- (id<CHHapticPatternPlayer>)createPlayerWithPattern:(CHHapticPattern *)p error:(NSError **)e {
    assert(p);
    if (self.failPattern || self.broken) {
        if (e) *e = [NSError errorWithDomain:CoreHapticsErrorDomain code:CHHapticErrorCodeServerInitFailed userInfo:nil];
        return nil;
    }
    self.plainPlayers++;
    self.testPlayer = [TestPlayer new]; self.testPlayer.failStart = self.failPlayer; self.testPlayer.failSend = self.failSend;
    return (id)self.testPlayer;
}
@end
@interface TestHaptics : NSObject
@property NSSet *supportedLocalities;
@property NSMutableArray<TestEngine *> *engines;
@property NSMutableArray<GCHapticsLocality> *requestedLocalities;
@property int attempts, failureStage;
@property BOOL noAdvanced;
@end
@implementation TestHaptics
- (instancetype)init {
    if ((self = [super init])) {
        _supportedLocalities = [NSSet setWithObject:GCHapticsLocalityDefault];
        _engines = [NSMutableArray new]; _requestedLocalities = [NSMutableArray new];
    }
    return self;
}
- (CHHapticEngine *)createEngineWithLocality:(GCHapticsLocality)locality {
    assert([self.supportedLocalities containsObject:locality]);
    self.attempts++;
    [self.requestedLocalities addObject:locality];
    if (self.attempts == 1 && self.failureStage == 1) return nil;
    TestEngine *e = [TestEngine new]; [self.engines addObject:e]; e.noAdvanced = self.noAdvanced;
    if (self.attempts == 1) {
        e.fail = self.failureStage == 2; e.failPattern = self.failureStage == 3;
        e.failPlayer = self.failureStage == 4; e.failSend = self.failureStage == 5;
        e.resetDuringStart = self.failureStage == 2;
    }
    return (id)e;
}
@end
@interface TestController : NSObject
@property TestHaptics *haptics;
@property NSString *vendorName, *productCategory;
@end
@implementation TestController
@end
static void drain(void) { dispatch_sync(queue, ^{}); dispatch_sync(queue, ^{}); }
static void advance(NSTimeInterval seconds) {
    uint64_t end = testTime + (uint64_t)llround(seconds * NSEC_PER_SEC);
    for (;;) {
        __block BOOL fired = NO;
        dispatch_sync(queue, ^{
            TestTimer *next = nil;
            for (TestTimer *timer in timers)
                if (timer.deadline <= end && (!next || timer.deadline < next.deadline)) next = timer;
            if (!next) { testTime = end; return; }
            testTime = next.deadline; [timers removeObject:next];
            next.block(); fired = YES;
        });
        drain();   // a callback may enqueue an epoch change or another output snapshot
        if (!fired) return;
    }
}
static void expire_retries(void) {
    dispatch_sync(queue, ^{ for (DmRumbleChannel *c in channels[0]) c.retryAfter = 0; });
}
static void send_pair(float left, float right) {
    dm_vibration_update u[] = {{0, 0, {left, 160, 0, 320}}, {0, 1, {right, 160, 0, 320}}};
    dm_haptics_send(u, 2); drain();
}
static int made;   // engines the test factories made
int main(void) { @autoreleasepool {
    queue = dispatch_queue_create("test.rumble", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(queue, &queue, &queue, NULL);
    timers = [NSMutableArray new];
    dm_haptics_init(apply, 1); drain();
    TestEngine *left = [TestEngine new], *right = [TestEngine new];
    dispatch_sync(queue, ^{
        channels[0] = [NSMutableArray new];
        add_engine(0, 0, @"test left", ^CHHapticEngine *(NSError **e) { (void)e; made++; return (id)left; });
        add_engine(0, 1, @"test right", ^CHHapticEngine *(NSError **e) { (void)e; made++; return (id)right; });
        dm_haptics_route(0, 1);
    }); drain();
    send_pair(0.2, 0.7);
    assert(left.testPlayer.levels.lastObject.floatValue == 0.2f && right.testPlayer.levels.lastObject.floatValue == 0.7f);
    send_pair(0, 0);
    assert(left.testPlayer.levels.lastObject.floatValue == 0 && right.testPlayer.levels.lastObject.floatValue == 0);
    assert(left.testPlayer.levels.count == 2);   // short pulse ends immediately, without a minimum duration
    send_pair(0.4, 0.5);
    advance(1.1);
    assert(left.testPlayer.levels.lastObject.floatValue == 0.4f && left.testPlayer.stops == 0);   // old idle timer cannot stop a new effect
    assert(left.starts == 1);   // reused player; no per-frame engine restart
    dm_haptic_state stale = {0, dm_haptics_epoch(0), {{1, 160, 0, 320}, {1, 160, 0, 320}}};
    suspend(DM_HAPTICS_INACTIVE, YES); suspend(DM_HAPTICS_INACTIVE, NO); drain();
    int starts = left.starts; apply(stale); drain(); assert(left.starts == starts);
    send_pair(0.3, 0.6); assert(left.starts == starts + 1);
    void (^oldReset)(void) = left.resetHandler;
    suspend(DM_HAPTICS_INACTIVE, YES); suspend(DM_HAPTICS_INACTIVE, NO); drain();
    send_pair(0.5, 0.5); oldReset(); drain();
    assert(dm_haptics_actual(0, 0).amplitude_low == 0.5f);   // delayed callback from a stopped engine is stale
    left.resetHandler(); drain(); drain();
    assert(dm_haptics_actual(0, 0).amplitude_low == 0 && right.testPlayer.levels.lastObject.floatValue == 0);
    left.fail = YES; send_pair(0.8, 0.8); drain();
    assert(dm_haptics_actual(0, 0).amplitude_low == 0);   // failed output is cleared, so a fresh identical send may retry
    left.fail = NO; expire_retries(); send_pair(0.8, 0.8);
    assert(left.testPlayer.levels.lastObject.floatValue == 0.8f);
    send_pair(0, 0); advance(0.99);
    assert(left.testPlayer.stops == 0);   // idle release must not run early
    advance(0.01);
    assert(left.testPlayer.stops == 1);   // idle hardware eventually released
    int engines = made; starts = left.starts; send_pair(0.7, 0.7);
    assert(made == engines && left.starts == starts + 1 && left.testPlayer.levels.lastObject.floatValue == 0.7f);   // the released engine is restarted, not made again
    // A permanently broken helper/engine is replaced, rather than retried for the rest of the session.
    // Exercise the real controller route (including a nil engine factory result), without hardware.
    for (int stage = 1; stage <= 5; stage++) {
        TestController *controller = [TestController new]; controller.vendorName = @"test controller";
        TestHaptics *h = [TestHaptics new]; h.failureStage = stage; controller.haptics = h;
        dm_haptics_apple_controller(0, (id)controller); drain();
        assert(dm_haptics_available() & 1);
        send_pair(0.6, 0); drain();
        if (stage == 3) {   // a refused advanced player is taken as a server without them: plain players from a new engine play at once
            assert(h.attempts == 2 && h.engines[1].plainPlayers == 1 && dm_haptics_actual(0, 0).amplitude_low == 0.6f);
            send_pair(0, 0); drain(); continue;
        }
        assert(h.attempts == 1 && dm_haptics_actual(0, 0).amplitude_low == 0);
        assert(stage == 1 || h.engines[0].handlersAtStart);   // callbacks cover startup itself
        void (^lateReset)(void) = h.engines.firstObject.startingReset;
        for (int i = 0; i < 20; i++) send_pair(0.6, 0);
        assert(h.attempts == 1);   // an ongoing effect must not hammer a failing XPC service
        expire_retries(); send_pair(0.6, 0); drain();
        assert(h.attempts == 2);
        TestEngine *recovered = h.engines.lastObject;
        assert(recovered.testPlayer.levels.lastObject.floatValue == 0.6f);
        assert(dm_haptics_actual(0, 0).amplitude_low == 0.6f);
        if (lateReset) { lateReset(); drain(); }
        assert(recovered.testPlayer.levels.lastObject.floatValue == 0.6f);   // old callbacks cannot silence the replacement
        send_pair(0, 0); drain();
        assert(recovered.testPlayer.levels.lastObject.floatValue == 0);
    }
    // A server without advanced players (game controllers on macOS 27): the first effect plays at once in plain pulses from a new engine, the pulses
    // follow each other while it lasts and take its level, and a stop never cuts one (stopAtTime): it silences them and stops the engine after the last
    TestController *plainc = [TestController new]; plainc.haptics = [TestHaptics new]; plainc.haptics.noAdvanced = YES;
    dm_haptics_apple_controller(0, (id)plainc); drain();
    send_pair(0.6, 0); drain();
    TestEngine *pe = plainc.haptics.engines.lastObject;
    assert(plainc.haptics.attempts == 2 && pe.plainPlayers == 1 && pe.testPlayer.levels.lastObject.floatValue == 0.6f);
    assert(dm_haptics_actual(0, 0).amplitude_low == 0.6f);
    send_pair(0.2, 0); drain();
    assert(pe.plainPlayers == 1 && pe.testPlayer.levels.lastObject.floatValue == 0.2f);
    advance(1.1);
    int pulses = pe.plainPlayers; assert(pulses >= 3);   // renewed while the effect lasts
    send_pair(0, 0); advance(1.7);
    assert(pe.plainPlayers == pulses && pe.testPlayer.levels.lastObject.floatValue == 0);   // silent: no new pulse
    assert(pe.stops == 1 && pe.testPlayer.stops == 0);   // the engine stopped after the last pulse, which was not cut
    send_pair(0.5, 0); drain();
    assert(plainc.haptics.attempts == 2 && pe.plainPlayers == pulses + 1 && pe.testPlayer.levels.lastObject.floatValue == 0.5f);
    send_pair(0, 0); drain();
    TestController *split = [TestController new]; split.haptics = [TestHaptics new];
    split.haptics.supportedLocalities = [NSSet setWithObjects:GCHapticsLocalityDefault, GCHapticsLocalityLeftHandle, GCHapticsLocalityRightHandle, nil];
    split.haptics.failureStage = 2;
    dm_haptics_apple_controller(0, (id)split); drain();
    send_pair(0.3, 0.4); drain();
    assert(split.haptics.attempts == 1);   // the old right-hand route must not replay the failed batch
    assert(channels[0].count == 1);
    expire_retries(); send_pair(0.3, 0.4); drain();
    assert(split.haptics.attempts == 2 && [split.haptics.requestedLocalities.lastObject isEqual:GCHapticsLocalityDefault]);
    assert(fabsf(split.haptics.engines.lastObject.testPlayer.levels.lastObject.floatValue - 0.5f) < 1e-6f);
    TestController *split2 = [TestController new]; split2.haptics = [TestHaptics new];
    split2.haptics.supportedLocalities = split.haptics.supportedLocalities;
    split2.haptics.failureStage = 3;
    dm_haptics_apple_controller(0, (id)split2); drain();
    send_pair(0.3, 0.4); drain();
    // A refused advanced player turns the handle route into one engine for both motors (the handles' engines override each other without them),
    // which plays the next send in plain pulses at the mixed level
    assert(split2.haptics.attempts == 1 && channels[0].count == 1 && channels[0].firstObject.simple);
    send_pair(0.3, 0.4); drain();
    NSArray *asked = split2.haptics.requestedLocalities;
    assert(split2.haptics.attempts == 2 && [asked[1] isEqual:GCHapticsLocalityDefault] && split2.haptics.engines[1].plainPlayers == 1);
    assert(fabsf(split2.haptics.engines[1].testPlayer.levels.lastObject.floatValue - 0.5f) < 1e-6f);
    send_pair(0, 0); drain();
    TestController *split3 = [TestController new]; split3.haptics = [TestHaptics new]; split3.haptics.noAdvanced = YES;
    split3.haptics.supportedLocalities = [NSSet setWithObjects:GCHapticsLocalityDefault, GCHapticsLocalityHandles, GCHapticsLocalityLeftHandle, GCHapticsLocalityRightHandle, nil];
    dm_haptics_apple_controller(0, (id)split3); drain();
    send_pair(0.3, 0.4); send_pair(0.3, 0.4); drain();
    assert([split3.haptics.requestedLocalities.lastObject isEqual:GCHapticsLocalityHandles]);   // the handles' own group when the controller lists it
    send_pair(0, 0); drain();
    dm_haptics_apple_controller(0, nil); drain();   // disconnect discards the controller factory and its old callbacks
    assert(!(dm_haptics_available() & 1));
    TestController *unsupported = [TestController new]; unsupported.haptics = [TestHaptics new];
    unsupported.haptics.supportedLocalities = [NSSet set];
    dm_haptics_apple_controller(0, (id)unsupported); drain();
    assert(!(dm_haptics_available() & 1) && unsupported.haptics.attempts == 0);
    // DM_RUMBLE_MIN: controller levels below it are silent, the rest stretched over the full range; the phone keeps its levels
    assert(controller_level(0) == 0 && controller_level(0.3f) == 0.3f && controller_level(1) == 1 && controller_level(NAN) == 0);
    rumble_min = 0.45f;
    assert(controller_level(0.4f) == 0 && controller_level(0.45f) == 0 && fabsf(controller_level(0.725f) - 0.5f) < 1e-6f && controller_level(1) == 1);
    TestController *cut = [TestController new]; cut.haptics = [TestHaptics new];
    dm_haptics_apple_controller(0, (id)cut); drain();
    send_pair(0.3, 0); drain();
    assert(cut.haptics.attempts == 0);   // silent: no engine is made
    send_pair(0.725, 0); drain();
    assert(fabsf(cut.haptics.engines.lastObject.testPlayer.levels.lastObject.floatValue - 0.5f) < 1e-6f);
    send_pair(0, 0); drain();
    rumble_min = 0;
    // A DualShock 4 goes through its effects report only while its HID device is there; without one it keeps Core Haptics
    TestController *ds = [TestController new]; ds.haptics = [TestHaptics new]; ds.productCategory = GCProductCategoryDualShock4;
    dm_haptics_apple_controller(0, (id)ds); drain();
    __block BOOL report = NO; dispatch_sync(queue, ^{ report = ds4[0].dev != NULL; });
    if (!report) {
        send_pair(0.6, 0); drain();
        assert(ds.haptics.attempts == 1 && ds.haptics.engines[0].testPlayer.levels.lastObject.floatValue == 0.6f);
        send_pair(0, 0); drain();
    }
    else assert(channels[0].count == 2 && [channels[0][0] isKindOfClass:DmDs4Channel.class] && ds.haptics.attempts == 0);
    dm_haptics_apple_controller(0, nil); drain();
    assert(ds4_crc((const uint8_t *)"123456789", 9) == 0xCBF43926u);   // the report's CRC32 (IEEE, reflected)
    dm_haptics_apple_shutdown();
    puts("Apple haptics: channels, pulses, idle release, startup handlers, engine replacement, backoff, stale callbacks, plain players, level cutoff and the DualShock 4 route passed");
} }
