/* Exercise the real rumble queue and motion sample buffer without physical hardware. */
#import <IOKit/hid/IOHIDManager.h>
#define IOHIDManagerCopyDevices(manager) ((CFSetRef)NULL)   /* a Joy-Con whose raw HID device is unavailable */
#include "../../runtime/gamepad_feedback.m"
#undef IOHIDManagerCopyDevices
#include <assert.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
uint64_t runtime_now_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW); }
int runtime_rumble_enabled(void) { return 1; }
int runtime_gyro_enabled(void) { return 1; }
void runtime_hid_feedback_stats(uint64_t *values, uint64_t *reads) { *values = *reads = 0; }

@interface TestMotion : NSObject
@property (nonatomic) BOOL hasRotationRate, sensorsActive;
@property (nonatomic) GCRotationRate rotationRate;
@property (nonatomic, copy) GCMotionValueChangedHandler valueChangedHandler;
@end
@implementation TestMotion
- (BOOL)sensorsRequireManualActivation { return YES; }
- (BOOL)hasGravityAndUserAcceleration { return NO; }
- (GCAcceleration)acceleration { return (GCAcceleration){0, 0, -1}; }
@end

@interface TestPad : NSObject
@property (nonatomic) BOOL hasRumble;
@property (nonatomic, strong) TestMotion *motion;
@property (nonatomic, copy) NSString *productCategory;
@end
@implementation TestPad
- (NSString *)vendorName { return @"test controller"; }
- (id)haptics { return self.hasRumble ? self : nil; }
- (NSSet *)supportedLocalities { return [NSSet setWithObject:GCHapticsLocalityDefault]; }
@end

static void expect_output(GCController *expected) {
    dispatch_sync(rumbleQueue, ^{ assert(rumblePad == expected); });
}

static void test_motion(void) {
    TestPad *a = [TestPad new], *b = [TestPad new], *accelOnly = [TestPad new];
    a.motion = [TestMotion new]; b.motion = [TestMotion new]; accelOnly.motion = [TestMotion new];
    a.motion.hasRotationRate = b.motion.hasRotationRate = YES;
    a.motion.rotationRate = (GCRotationRate){2 * M_PI, 0, 0};
    b.motion.rotationRate = (GCRotationRate){-2 * M_PI, 0, 0};
    accelOnly.hasRumble = YES;
    GCController *first = (GCController *)a, *second = (GCController *)b;
    TestPad *joy = [TestPad new]; joy.productCategory = @"Joy-Con";
    runtime_feedback_attach((GCController *)joy);
    assert(!motionPad);   /* a failed HID open must leave motion available to the next pad */
    runtime_feedback_attach((GCController *)accelOnly);
    expect_output((GCController *)accelOnly);
    assert(!accelOnly.motion.valueChangedHandler && !motionPad);
    runtime_feedback_attach(first);
    GCMotionValueChangedHandler firstHandler = a.motion.valueChangedHandler;
    assert(firstHandler && a.motion.sensorsActive);
    runtime_feedback_attach(second);
    assert(a.motion.valueChangedHandler == firstHandler && !b.motion.valueChangedHandler && !b.motion.sensorsActive);
    expect_output((GCController *)accelOnly);  /* rumble and motion may have different owners */

    RuntimeMotionState states[4];
    firstHandler((GCMotion *)a.motion);
    assert(runtime_motion_read(states, 4) == 1 && states[0].gyro[0] == 1.f);
    uint64_t last = states[0].sampling;
    runtime_feedback_detach(first);
    assert(!a.motion.sensorsActive && !a.motion.valueChangedHandler);
    runtime_feedback_attach(second); runtime_feedback_attach(first);
    assert(!a.motion.valueChangedHandler && b.motion.sensorsActive && b.motion.valueChangedHandler);
    firstHandler((GCMotion *)a.motion);   /* a queued event from the previous owner */
    assert(runtime_motion_read(states, 4) == 0);
    b.motion.valueChangedHandler((GCMotion *)b.motion);
    assert(runtime_motion_read(states, 4) == 1 && states[0].gyro[0] == -1.f && states[0].sampling > last);
    runtime_feedback_detach(first);
    assert(b.motion.valueChangedHandler && b.motion.sensorsActive);
    runtime_feedback_detach(second); runtime_feedback_detach((GCController *)accelOnly);
    assert(runtime_motion_read(states, 4) == 0);
    puts("pass: the first capable motion source survives later connections and hands off a fresh sample stream");
}

int main(int argc, char **argv) { @autoreleasepool {
    setenv("TOTK_RUMBLE_HID", "0", 1);
    setenv("TOTK_RUMBLE_GAIN", "1", 1);
    setenv("TOTK_GYRO_AXES", "x,y,z", 1);
    setenv("TOTK_GYRO_DEADZONE", "0", 1);
    setenv("TOTK_GYRO_CLOCK", "0", 1);
    if (argc == 2 && !strcmp(argv[1], "--motion")) { test_motion(); return 0; }
    TestPad *a = [TestPad new], *b = [TestPad new];
    a.hasRumble = b.hasRumble = YES;
    GCController *first = (GCController *)a, *second = (GCController *)b;
    GCController *noMotor = (GCController *)[TestPad new];

    runtime_feedback_attach(noMotor); expect_output(nil);
    runtime_feedback_attach(first);   expect_output(first);
    runtime_feedback_attach(second);  expect_output(first);
    runtime_feedback_detach(second);  expect_output(first);
    runtime_feedback_attach(second);  expect_output(first);

    /* On a removal the router offers the remaining eligible pads again, in connection order. */
    runtime_feedback_detach(first);   expect_output(nil);
    runtime_feedback_attach(noMotor);
    runtime_feedback_attach(second);  expect_output(second);
    runtime_feedback_attach(first);   expect_output(second);
    runtime_feedback_detach(noMotor); expect_output(second);

    /* Two Players removes an ineligible solo owner before offering Mario's seat. */
    runtime_feedback_detach(second);
    runtime_feedback_attach(first);   expect_output(first);
    runtime_feedback_detach(first);   expect_output(nil);
    puts("pass: the first rumble-capable controller keeps Mario's vibration until it leaves");
    return 0;
}}
