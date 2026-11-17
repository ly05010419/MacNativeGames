/* Real native feedback callbacks, without Bluetooth hardware or a game. */
#include "../../runtime/gamepad_feedback.m"
#include <assert.h>

static int dsuOn, live, liveChanges, deadzoneChanges, feeds, clears, left, right, capture;
static double deadzone;
int runtime_dsu_motion_active(void) { return dsuOn; }
int runtime_gyro_enabled(void) { return 1; }
int runtime_rumble_enabled(void) { return 0; }
uint64_t runtime_now_ns(void) { return 1000000000ull; }
void runtime_motion_live(int on) { live = on; liveChanges++; }
void runtime_motion_deadzone_default(double value) { deadzone = value; deadzoneChanges++; }
void runtime_motion_feed(const double a[3], const double g[3], uint64_t t) { (void)a; (void)g; (void)t; feeds++; }
int runtime_motion_axes(const char *axes) { (void)axes; return 0; }
void runtime_gamepad_switch_left(void *c, int x, int y) { if (!c) clears++; else { assert(x == 2048 && y == 2048); left++; } }
void runtime_gamepad_switch_right(void *c, int x, int y) { if (!c) clears++; else { assert(x == 2048 && y == 2048); right++; } }
void runtime_gamepad_switch_capture(void *c, int down) { if (!c) clears++; else if (down) capture++; }
void runtime_gamepad_hid_battery(int side, int percent) { (void)side; (void)percent; }
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "%s\n", message); abort(); }

@interface DSUTestMotion : NSObject
@property GCAcceleration acceleration;
@property GCRotationRate rotationRate;
@property BOOL hasRotationRate, sensorsRequireManualActivation, sensorsActive;
@property(copy) void (^valueChangedHandler)(GCMotion *motion);
@end
@implementation DSUTestMotion
- (BOOL)hasGravityAndUserAcceleration { return NO; }
@end
@interface DSUTestPad : NSObject
@property DSUTestMotion *motion;
@end
@implementation DSUTestPad
- (NSString *)vendorName { return @"Native test controller"; }
- (NSString *)productCategory { return @"Switch Pro Controller"; }
- (GCExtendedGamepad *)extendedGamepad { return nil; }
@end

int main(void) { @autoreleasepool {
    DSUTestMotion *sensor = [DSUTestMotion new];
    sensor.acceleration = (GCAcceleration){0, 0, -1};
    sensor.rotationRate = (GCRotationRate){1, 0, 0};
    sensor.hasRotationRate = sensor.sensorsRequireManualActivation = YES;
    DSUTestPad *fake = [DSUTestPad new]; fake.motion = sensor;
    GCController *pad = (id)fake;
    runtime_feedback_attach(pad);
    assert(motionPad == pad && sensor.sensorsActive && sensor.valueChangedHandler && live);
    sensor.valueChangedHandler((id)sensor); assert(feeds == 1);

    /* An already-active Switch HID stream must survive DSU taking over fusion. */
    hidMotion = YES; sensor.valueChangedHandler = nil;
    dsuOn = 1; runtime_feedback_motion_source_changed();
    assert(hidMotion && motionPad == pad && sensor.sensorsActive && !clears && !live);
    runtime_motion_live(1);  /* the first DSU sample */
    int changes = liveChanges, zones = deadzoneChanges;
    uint8_t report[49] = {0}; report[4] = 0x20; report[7] = report[10] = 8; report[8] = report[11] = 128; report[13] = 1;
    hid_report(NULL, kIOReturnSuccess, NULL, kIOHIDReportTypeInput, 0x30, report, sizeof report);
    motion_sample((id)sensor);
    imuProfile = &hid_imu_profiles[0];
    hid_imu_report(NULL, kIOReturnSuccess, NULL, kIOHIDReportTypeInput, imuProfile->reportId, report, sizeof report, 1);
    imuProfile = NULL;
    assert(left == 1 && right == 1 && capture == 1 && !clears && feeds == 1);
    assert(live && liveChanges == changes && deadzoneChanges == zones);

    dsuOn = 0; runtime_feedback_motion_source_changed(); // DSU sensor timeout, even if its socket is still open
    assert(hidMotion && motionPad == pad && !clears && live && deadzone == 1.2);
    hid_report(NULL, kIOReturnSuccess, NULL, kIOHIDReportTypeInput, 0x30, report, sizeof report);
    assert(left == 2 && right == 2 && capture == 2 && feeds == 4);

    /* A native disconnect must release its inputs without stopping DSU motion. */
    dsuOn = 1; runtime_feedback_motion_source_changed(); runtime_motion_live(1);
    changes = liveChanges; zones = deadzoneChanges;
    runtime_feedback_detach(pad);
    assert(!motionPad && !hidMotion && clears == 3 && live);
    assert(liveChanges == changes && deadzoneChanges == zones);

    /* A pad first attached during DSU motion is ready when DSU sensors go stale. */
    runtime_feedback_attach(pad);
    assert(motionPad == pad && sensor.valueChangedHandler && sensor.sensorsActive);
    assert(liveChanges == changes && deadzoneChanges == zones);
    sensor.valueChangedHandler((id)sensor); assert(feeds == 4);
    dsuOn = 0; runtime_feedback_motion_source_changed();
    assert(live && deadzone == 0);
    sensor.valueChangedHandler((id)sensor); assert(feeds == 5);
    runtime_feedback_detach(pad); assert(!live);
    puts("Native HID input and sensor lifecycle survive DSU source switches");
    return 0;
} }
