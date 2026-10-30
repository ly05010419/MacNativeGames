// Run the actual serial-queue adapter with fake engines: no controller, actuator or app is opened.
#import "../haptics_apple.m"
#include <assert.h>

@interface TestPlayer : NSObject
@property BOOL loopEnabled, isMuted;
@property int starts, stops;
@property NSMutableArray<NSNumber *> *levels;
@end
@implementation TestPlayer
- (instancetype)init { if ((self = [super init])) _levels = [NSMutableArray new]; return self; }
- (BOOL)startAtTime:(NSTimeInterval)t error:(NSError **)e { (void)t; (void)e; self.starts++; return YES; }
- (BOOL)stopAtTime:(NSTimeInterval)t error:(NSError **)e { (void)t; (void)e; self.stops++; return YES; }
- (BOOL)sendParameters:(NSArray<CHHapticDynamicParameter *> *)p atTime:(NSTimeInterval)t error:(NSError **)e {
    (void)t; (void)e;
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
@end
@implementation TestEngine
- (BOOL)startAndReturnError:(NSError **)e { (void)e; self.starts++; return !self.fail; }
- (void)stopWithCompletionHandler:(CHHapticCompletionHandler)handler { self.stops++; if (handler) handler(nil); }
- (id<CHHapticAdvancedPatternPlayer>)createAdvancedPlayerWithPattern:(CHHapticPattern *)p error:(NSError **)e {
    (void)p; (void)e; self.testPlayer = [TestPlayer new]; return (id)self.testPlayer;
}
@end
static void drain(void) { dispatch_sync(queue, ^{}); }
static void send_pair(float left, float right) {
    dm_vibration_update u[] = {{0, 0, {left, 160, 0, 320}}, {0, 1, {right, 160, 0, 320}}};
    dm_haptics_send(u, 2); drain();
}
int main(void) { @autoreleasepool {
    queue = dispatch_queue_create("test.rumble", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(queue, &queue, &queue, NULL);
    dm_haptics_init(apply, 1); drain();
    TestEngine *left = [TestEngine new], *right = [TestEngine new];
    dispatch_sync(queue, ^{
        channels[0] = [NSMutableArray new]; add_engine(0, (id)left); add_engine(0, (id)right);
        dm_haptics_route(0, 1);
    }); drain();
    send_pair(0.2, 0.7);
    assert(left.testPlayer.levels.lastObject.floatValue == 0.2f && right.testPlayer.levels.lastObject.floatValue == 0.7f);
    send_pair(0, 0);
    assert(left.testPlayer.levels.lastObject.floatValue == 0 && right.testPlayer.levels.lastObject.floatValue == 0);
    assert(left.testPlayer.levels.count == 2);   // short pulse ends immediately, without a minimum duration
    send_pair(0.4, 0.5);
    usleep(1100000); drain();
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
    left.fail = NO; send_pair(0.8, 0.8);
    assert(left.testPlayer.levels.lastObject.floatValue == 0.8f);
    send_pair(0, 0); usleep(1100000); drain();
    assert(left.testPlayer.stops == 1);   // idle hardware eventually released
    dm_haptics_apple_shutdown();
    puts("Apple haptics: independent channels, pulse edges, sustained output, idle release, resets and stale work passed");
} }
