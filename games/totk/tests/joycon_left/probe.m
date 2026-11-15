/* Real attach/report/detach functions, fake IOKit and controller objects. No physical device or game files are used. */
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <IOKit/hid/IOHIDManager.h>
#include <stdatomic.h>

@interface TestDevice : NSObject
@property(atomic) BOOL opened;
@property(nonatomic) IOHIDReportCallback callback;
@property(nonatomic, assign) void *callbackContext;
@property(nonatomic) IOHIDReportWithTimeStampCallback timestampCallback;
@property(nonatomic, assign) void *timestampContext;
@property(nonatomic) unsigned modeWrites;
@property(nonatomic, strong) NSNumber *vendorID, *productID;
@end
@implementation TestDevice
@end
@interface TestManager : NSObject
@property(nonatomic, strong) NSMutableDictionary *match;
@property(nonatomic, strong) NSArray *multiple;
@property(nonatomic, strong) TestDevice *device;
@end
@implementation TestManager
- (instancetype)init { if ((self = [super init])) _match = [NSMutableDictionary new]; return self; }
@end

static TestDevice *leftDevice, *rightDevice, *genericDevice;
static unsigned opens, closes;
static _Atomic unsigned neutralWrites, modeWrites, writesAfterClose;
static _Atomic unsigned switchInitWrites, switchWritesToGeneric;
static IOReturn leftOpenResult = kIOReturnSuccess;
static IOReturn modeWriteResult = kIOReturnSuccess;
static dispatch_semaphore_t modeWriteGate;
static dispatch_semaphore_t lateNeutralGate, lateNeutralStarted;
static IOHIDManagerRef test_manager(CFAllocatorRef allocator, IOOptionBits options) {
    (void)allocator; (void)options;
    return (IOHIDManagerRef)CFBridgingRetain([TestManager new]);
}
static void test_match(IOHIDManagerRef manager, CFDictionaryRef match) {
    TestManager *m = (__bridge TestManager *)manager;
    [m.match addEntriesFromDictionary:(__bridge NSDictionary *)match];
    m.device = [m.match[@kIOHIDProductIDKey] intValue] == 0x2006 ? leftDevice : rightDevice;
}
static void test_match_multiple(IOHIDManagerRef manager, CFArrayRef matches) {
    TestManager *m = (__bridge TestManager *)manager; m.multiple = (__bridge NSArray *)matches; m.device = genericDevice;
}
static CFSetRef test_devices(IOHIDManagerRef manager) {
    return (CFSetRef)CFBridgingRetain([NSSet setWithObject:((__bridge TestManager *)manager).device]);
}
static CFTypeRef test_property(IOHIDDeviceRef device, CFStringRef key) {
    TestDevice *d = (__bridge TestDevice *)device;
    if (CFEqual(key, CFSTR(kIOHIDVendorIDKey))) return (__bridge CFTypeRef)d.vendorID;
    if (CFEqual(key, CFSTR(kIOHIDProductIDKey))) return (__bridge CFTypeRef)d.productID;
    return NULL;
}
static IOReturn test_open(IOHIDDeviceRef device, IOOptionBits options) {
    (void)options;
    TestDevice *d = (__bridge TestDevice *)device;
    if (d == leftDevice && leftOpenResult != kIOReturnSuccess) {
        if (leftOpenResult == kIOReturnExclusiveAccess) d.opened = YES;   /* IOKit can enter the client set but report seized */
        return leftOpenResult;
    }
    d.opened = YES; opens++;
    return kIOReturnSuccess;
}
static IOReturn test_close(IOHIDDeviceRef device, IOOptionBits options) {
    (void)options;
    ((__bridge TestDevice *)device).opened = NO; closes++;
    return kIOReturnSuccess;
}
static void test_schedule(IOHIDDeviceRef device, CFRunLoopRef loop, CFStringRef mode) {
    (void)device; (void)loop; (void)mode;
}
static void test_callback(IOHIDDeviceRef device, uint8_t *buffer, CFIndex length, IOHIDReportCallback callback, void *ctx) {
    (void)buffer; (void)length;
    TestDevice *d = (__bridge TestDevice *)device; d.callback = callback; d.callbackContext = ctx;
}
static void test_timestamp_callback(IOHIDDeviceRef device, uint8_t *buffer, CFIndex length,
                                    IOHIDReportWithTimeStampCallback callback, void *ctx) {
    (void)buffer; (void)length;
    TestDevice *d = (__bridge TestDevice *)device; d.timestampCallback = callback; d.timestampContext = ctx;
}
static IOReturn test_write(IOHIDDeviceRef device, IOHIDReportType type, CFIndex reportId, const uint8_t *report, CFIndex length) {
    (void)type;
    if (!((__bridge TestDevice *)device).opened) atomic_fetch_add(&writesAfterClose, 1);
    if (device == (__bridge IOHIDDeviceRef)genericDevice && reportId == 0x01) atomic_fetch_add(&switchWritesToGeneric, 1);
    if (reportId == 0x01 && length >= 12 && (report[10] == 0x40 || report[10] == 0x10)) atomic_fetch_add(&switchInitWrites, 1);
    if (reportId == 0x10) {
        unsigned n = atomic_fetch_add(&neutralWrites, 1) + 1;
        if (lateNeutralGate && n == 3) {
            dispatch_semaphore_signal(lateNeutralStarted);
            dispatch_semaphore_wait(lateNeutralGate, DISPATCH_TIME_FOREVER);
        }
    }
    if (reportId == 0x01 && length >= 12 && report[10] == 0x03 && report[11] == 0x30) {
        atomic_fetch_add(&modeWrites, 1);
        TestDevice *d = (__bridge TestDevice *)device; d.modeWrites++;
        if (modeWriteGate) dispatch_semaphore_wait(modeWriteGate, DISPATCH_TIME_FOREVER);
        return modeWriteResult;
    }
    return kIOReturnSuccess;
}
#define IOHIDManagerCreate test_manager
#define IOHIDManagerSetDeviceMatching test_match
#define IOHIDManagerSetDeviceMatchingMultiple test_match_multiple
#define IOHIDManagerOpen(...) (runtime_fail("bulk manager open bypasses HID lifetime ownership"), kIOReturnError)
#define IOHIDManagerCopyDevices test_devices
#define IOHIDDeviceGetProperty test_property
#define IOHIDDeviceOpen test_open
#define IOHIDDeviceClose test_close
#define IOHIDDeviceScheduleWithRunLoop test_schedule
#define IOHIDDeviceUnscheduleFromRunLoop test_schedule
#define IOHIDDeviceRegisterInputReportCallback test_callback
#define IOHIDDeviceRegisterInputReportWithTimeStampCallback test_timestamp_callback
#define IOHIDDeviceSetReport test_write
#include "../../runtime/gamepad_feedback.m"
#include "../../runtime/switch_left_fallback.h"

static _Atomic uint64_t now = 1000000000ULL;
static int gyroEnabled;
static unsigned feeds, clears, rightFeeds, rightClears;
static unsigned motionFeeds;
static void *leftSource, *rightSource;
static int leftX, leftY;
static SwitchLeftFallback raw;
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "FAIL: %s\n", s); exit(1); }
uint64_t runtime_now_ns(void) { return now; }
int runtime_rumble_enabled(void) { return 0; }
int runtime_gyro_enabled(void) { return gyroEnabled; }
int runtime_gamepad_game_input(void) { return 1; }
int runtime_setting_word(const char *n, char *w, size_t s) { (void)n; (void)w; (void)s; return 0; }
void runtime_setting_save(const char *n, const char *w, const char *s) { (void)n; (void)w; (void)s; }
void runtime_motion_feed(const double a[3], const double g[3], uint64_t t) { (void)a; (void)g; (void)t; motionFeeds++; }
int runtime_motion_axes(const char *s) { (void)s; return 0; }
void runtime_motion_deadzone_default(double d) { (void)d; }
void runtime_motion_live(int on) { (void)on; }
void runtime_gamepad_hid_battery(int left, int percent) { (void)left; (void)percent; }
/* Record the feedback boundary and apply the same source reset/feed as gamepad.m. */
void runtime_gamepad_switch_left(void *source, int x, int y) {
    if (leftSource != source) raw = (SwitchLeftFallback){0};
    leftSource = source; leftX = x; leftY = y;
    switch_left_feed(&raw, x, y, now);
    if (source) feeds++; else clears++;
}
void runtime_gamepad_switch_right(void *source, int x, int y) {
    (void)x; (void)y; rightSource = source;
    if (source) rightFeeds++; else rightClears++;
}
void runtime_gamepad_switch_capture(void *source, int pressed) { (void)source; (void)pressed; }

@interface TestMotion : NSObject
@property(nonatomic) BOOL hasRotationRate, hasGravityAndUserAcceleration, sensorsRequireManualActivation, sensorsActive;
@property(nonatomic, copy) void (^valueChangedHandler)(GCMotion *);
@end
@implementation TestMotion
@end
@interface TestController : NSObject
@property(nonatomic, copy) NSString *productCategory, *vendorName;
@property(nonatomic, strong) TestMotion *motion;
@property(nonatomic, readonly) GCExtendedGamepad *extendedGamepad;
@end
@implementation TestController
- (GCExtendedGamepad *)extendedGamepad { return nil; }
@end

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); return 1; } } while (0)
static GCController *controller(NSString *category, BOOL nativeMotion) {
    TestController *c = [TestController new]; c.productCategory = category; c.vendorName = category;
    if (nativeMotion) { c.motion = [TestMotion new]; c.motion.hasRotationRate = YES; }
    return (GCController *)c;
}
static void stick_bytes(uint8_t *p, int x, int y) { p[0] = x & 255; p[1] = (x >> 8) | ((y & 15) << 4); p[2] = y >> 4; }
static void simple_bytes(uint8_t *p, unsigned x, unsigned y) {
    memset(p, 0, 12); p[0] = 0x3f; p[3] = 8;
    p[4] = x & 255; p[5] = x >> 8; p[6] = y & 255; p[7] = y >> 8; p[9] = p[11] = 0x80;
}
static void left_report(uint32_t id, CFIndex length, IOReturn result) {
    uint8_t report[49] = {0}; report[0] = (uint8_t)id; report[2] = 0x80; stick_bytes(report + 6, 2048, 0);
    if (id == 0x3f) simple_bytes(report, 32768, 32768);
    hid_left_report(leftDevice.callbackContext, result, (__bridge void *)leftDevice, kIOHIDReportTypeInput, id, report, length);
}
static void left_simple_down(void) {
    uint8_t report[12]; simple_bytes(report, 32768, 0xff00);
    hid_left_report(leftDevice.callbackContext, kIOReturnSuccess, (__bridge void *)leftDevice, kIOHIDReportTypeInput, 0x3f, report, sizeof report);
}
static void mode_report(BOOL right, uint32_t id) {
    if (!right) { left_report(id, id == 0x3f ? 12 : 49, kIOReturnSuccess); return; }
    uint8_t report[49] = {0}; report[0] = (uint8_t)id; report[13] = 1;
    hid_report(rightDevice.callbackContext, kIOReturnSuccess, (__bridge void *)rightDevice, kIOHIDReportTypeInput, id, report, id == 0x3f ? 12 : 49);
}
static void drain_queues(void) {
    /* Generic rumble retirement hands its owned device back to main before posting the usual producer/output
       barriers. Drain that main -> producer -> output -> main chain as well, without relying on fixed sleeps. */
    for (int round = 0; round < 3; round++) {
        if (rumbleQueue) dispatch_sync(rumbleQueue, ^{});
        if (hidOut) {
            dispatch_sync(hidOut, ^{});
            dispatch_sync(hidOut, ^{});   /* without rumbleQueue the outer task queues one more task on hidOut */
        }
        __block BOOL done = NO;
        dispatch_async(dispatch_get_main_queue(), ^{ done = YES; });
        while (!done) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
        if (hidOut) dispatch_sync(hidOut, ^{});
    }
}
static void activate_left(void) {
    float x, y;
    left_report(0x30, 49, kIOReturnSuccess);
    switch_left_get(&raw, 0, 0, now, &x, &y);
    now += 200000000ULL;
    left_report(0x30, 49, kIOReturnSuccess);
    switch_left_get(&raw, 0, 0, now, &x, &y);
}
static void run_mode_timer(void) {
    __block BOOL done = NO;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ done = YES; });
    while (!done) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
}
static void drain_mode_timer(void) {
    run_mode_timer();
    drain_queues();
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    leftDevice = [TestDevice new]; rightDevice = [TestDevice new];
    genericDevice = [TestDevice new]; genericDevice.vendorID = @0x2dc8; genericDevice.productID = @0x6012;
    NSString *category = @"Nintendo Switch Joy-Con (L/R)";
    GCController *pair = controller(category, NO), *other = controller(@"DualSense", YES);
    const char *test = argv[1];
    BOOL testRight = !strncmp(test, "right-", 6);
    if (testRight) test += 6;
    if (!strcmp(test, "gyro-off") || !strcmp(test, "native-motion")) {
        gyroEnabled = !strcmp(test, "native-motion");
        pair = controller(category, gyroEnabled);
        runtime_feedback_attach(pair);
        CHECK(leftDevice.opened && !hidMotion);
        left_report(0x21, 49, kIOReturnSuccess);
        left_report(0x30, 8, kIOReturnSuccess);
        left_report(0x30, 49, kIOReturnError);
        CHECK(feeds == 0);
        left_report(0x30, 49, kIOReturnSuccess);
        CHECK(feeds == 1 && leftSource == (__bridge void *)pair && leftX == 2048 && leftY == 0);
        activate_left(); CHECK(raw.active);
        CHECK(hidDevice == (__bridge IOHIDDeviceRef)rightDevice && hidProduct == 0x2007 && !hidMotion);
        mode_report(YES, 0x30); CHECK(!motionFeeds);   /* right report-mode setup does not enable gyro */
    } else if (!strcmp(test, "motion-stop")) {
        gyroEnabled = 1; runtime_feedback_attach(pair); CHECK(hidMotion);
        activate_left(); CHECK(raw.active);
        unsigned before = clears;
        hid_motion_stop();
        CHECK(clears == before && raw.active && leftDevice.opened && rightClears);
        left_report(0x30, 49, kIOReturnSuccess);
        CHECK(leftSource == (__bridge void *)pair && raw.active);
    } else if (!strcmp(test, "motion-switch")) {
        gyroEnabled = 1; runtime_feedback_attach(pair); CHECK(hidMotion);
        activate_left(); CHECK(raw.active);
        unsigned before = clears;
        runtime_feedback_attach(other); CHECK(motionPad == other && !hidMotion);
        CHECK(clears == before && raw.active);
        left_report(0x30, 49, kIOReturnSuccess);
        CHECK(leftSource == (__bridge void *)pair);
        runtime_feedback_detach(other);
        CHECK(clears == before && leftDevice.opened);
    } else if (!strcmp(test, "unrelated-detach")) {
        gyroEnabled = 1; runtime_feedback_attach(pair);
        activate_left(); CHECK(raw.active);
        unsigned before = clears;
        runtime_feedback_detach(other); drain_queues();
        CHECK(clears == before && raw.active && leftDevice.opened && !closes);
    } else if (!strcmp(test, "reconnect")) {
        runtime_feedback_attach(pair);
        /* Seed active input so this case can independently test detach/reattach even before gyro-off is fixed. */
        runtime_gamepad_switch_left((__bridge void *)pair, 2048, 0); raw.active = 1;
        unsigned before = clears;
        runtime_feedback_detach(pair);
        CHECK(clears == before + 1 && !leftSource && !raw.valid);
        left_report(0x30, 49, kIOReturnSuccess); CHECK(!leftSource);
        GCController *replacement = controller(category, NO);
        runtime_feedback_attach(replacement); drain_queues();
        CHECK(leftDevice.opened && rightDevice.opened && opens == 4 && closes == 0);   /* rebind even reused physical objects */
        left_report(0x30, 49, kIOReturnSuccess);
        CHECK(leftSource == (__bridge void *)replacement && !raw.active);
        runtime_feedback_detach(pair); drain_queues(); CHECK(leftDevice.opened);
        runtime_feedback_detach(replacement); drain_queues();
        CHECK(!leftDevice.opened && !rightDevice.opened && !leftSource && closes == 2);
        runtime_feedback_attach(pair);
        CHECK(opens == 6 && leftDevice.opened && rightDevice.opened);
        left_report(0x30, 49, kIOReturnSuccess); CHECK(leftSource == (__bridge void *)pair);
    } else if (!strcmp(test, "rumble-order") || !strcmp(test, "rumble-stop")) {
        runtime_feedback_attach(pair);
        rumbleQueue = dispatch_queue_create("joycon-test.rumble", DISPATCH_QUEUE_SERIAL);
        CHECK(hid_rumble_start(pair)); drain_queues();
        if (!strcmp(test, "rumble-stop")) {
            activate_left(); CHECK(raw.active);
            unsigned before = clears;
            hid_rumble_stop(); drain_queues();
            CHECK(leftDevice.opened && rightDevice.opened && raw.active && clears == before);
            left_report(0x30, 49, kIOReturnSuccess); CHECK(leftSource == (__bridge void *)pair);
            CHECK(neutralWrites == 2 && writesAfterClose == 0 && closes == 0);
        } else {
            dispatch_semaphore_t releaseOutput = dispatch_semaphore_create(0);
            dispatch_async(hidOut, ^{ dispatch_semaphore_wait(releaseOutput, DISPATCH_TIME_FOREVER); });
            runtime_feedback_detach(pair);
            CHECK(leftDevice.opened && rightDevice.opened);   /* neither may close ahead of the neutral reports */
            dispatch_semaphore_signal(releaseOutput); drain_queues();
            CHECK(neutralWrites == 2 && writesAfterClose == 0 && closes == 2);
            CHECK(!leftDevice.opened && !rightDevice.opened);
        }
    } else if (!strcmp(test, "late-open")) {
        leftOpenResult = kIOReturnNotOpen;
        runtime_feedback_attach(pair); CHECK(!leftDevice.opened);
        leftOpenResult = kIOReturnSuccess;
        CHECK(hid_left_open() == kIOReturnSuccess);   /* another consumer later succeeds; no autonomous retry claimed */
        left_report(0x30, 49, kIOReturnSuccess);
        CHECK(leftSource == (__bridge void *)pair && feeds == 1);
        runtime_feedback_detach(pair); drain_queues(); CHECK(!leftDevice.opened);
    } else if (!strcmp(test, "pro-controller") || !strcmp(test, "pro-alongside")) {
        BOOL alongside = !strcmp(test, "pro-alongside");
        GCController *pro = controller(@"Switch Pro Controller", NO);
        motionPad = pro; hid_motion_start(); CHECK(hidMotion && !hidJoyConRight);
        if (alongside) {
            runtime_feedback_attach(pair); activate_left(); CHECK(raw.active);
            CHECK(motionPad == pro && hidMotion && !hidJoyConRight);   /* left attach must not change the IMU's type */
        }
        uint8_t report[49] = {0x30}; stick_bytes(report + 6, 0, 2048); stick_bytes(report + 9, 4095, 2048); report[13] = 1;
        unsigned before = clears;
        hid_report(rightDevice.callbackContext, kIOReturnSuccess, (__bridge void *)rightDevice, kIOHIDReportTypeInput, 0x30, report, sizeof report);
        CHECK(rightFeeds == 1 && rightSource == (__bridge void *)pro);
        CHECK(leftSource == (__bridge void *)(alongside ? pair : pro));
        hid_motion_stop();
        CHECK(clears == before + (alongside ? 0 : 1));
    } else if (!strcmp(test, "simple-mode")) {
        runtime_feedback_attach(pair);
        mode_report(testRight, 0x3f);   /* captured after a real Bluetooth reconnect: simple mode */
        CHECK(feeds == (testRight ? 0u : 1u) && !modeWrites);
        drain_mode_timer();
        CHECK(modeWrites == 1 && !hidMotion && !raw.active);   /* neutral simple input must not activate fallback */
        CHECK((testRight ? rightDevice : leftDevice).modeWrites == 1);
        mode_report(testRight, 0x30);   /* the device accepts the mode request; button mapping stays in GameController */
        if (!testRight) CHECK(feeds == 2 && leftSource == (__bridge void *)pair);
        CHECK(!motionFeeds);
    } else if (!strcmp(test, "mode-retry")) {
        runtime_feedback_attach(pair);
        for (unsigned attempt = 1; attempt <= 5; attempt++) {
            for (int i = 0; i < 10; i++) mode_report(testRight, 0x3f);
            drain_mode_timer();
            CHECK(modeWrites == attempt && !raw.active);
            now += 2000000000ULL;
        }
        mode_report(testRight, 0x3f); drain_mode_timer();
        CHECK(modeWrites == 5 && !writesAfterClose);   /* bounded, no command per input report */
    } else if (!strcmp(test, "mode-cancel")) {
        runtime_feedback_attach(pair);
        mode_report(testRight, 0x3f);
        runtime_feedback_detach(pair); drain_queues();
        CHECK(!leftDevice.opened);
        leftDevice = [TestDevice new]; rightDevice = [TestDevice new];   /* new physical devices, not just a new GC owner */
        GCController *replacement = controller(category, NO);
        runtime_feedback_attach(replacement);
        mode_report(testRight, 0x3f); drain_mode_timer();
        CHECK(modeWrites == 1 && !writesAfterClose && leftDevice.opened && rightDevice.opened);
        CHECK((testRight ? rightDevice : leftDevice).modeWrites == 1);
        mode_report(testRight, 0x30);
        if (!testRight) CHECK(leftSource == (__bridge void *)replacement);
    } else if (!strcmp(test, "mode-already-full")) {
        runtime_feedback_attach(pair);
        mode_report(testRight, 0x3f);
        mode_report(testRight, 0x30); drain_mode_timer();
        CHECK(modeWrites == 0 && feeds == (testRight ? 0u : 2u));   /* do not reconfigure an already-working stream */
    } else if (!strcmp(test, "both-simple-modes")) {
        runtime_feedback_attach(pair);
        mode_report(NO, 0x3f); mode_report(YES, 0x3f); drain_mode_timer();
        CHECK(leftDevice.modeWrites == 1 && rightDevice.modeWrites == 1);
        mode_report(NO, 0x30); mode_report(YES, 0x30);
        CHECK(feeds == 2 && !motionFeeds);
        now += 2000000000ULL;
        mode_report(YES, 0x3f); drain_mode_timer();
        CHECK(leftDevice.modeWrites == 1 && rightDevice.modeWrites == 2);   /* independent state per side */
    } else if (!strcmp(test, "joycon-before-pro")) {
        runtime_feedback_attach(pair);
        GCController *pro = controller(@"Switch Pro Controller", NO);
        motionPad = pro; hid_motion_start();
        CHECK(!hidMotion && hidProduct == 0x2007);   /* never reinterpret an input-owned right Joy-Con as a Pro IMU */
        mode_report(YES, 0x3f); drain_mode_timer(); CHECK(rightDevice.modeWrites == 1);
    } else if (!strcmp(test, "slow-mode-write") || !strcmp(test, "both-slow-mode-writes")) {
        BOOL both = !strcmp(test, "both-slow-mode-writes");
        runtime_feedback_attach(pair);
        modeWriteGate = dispatch_semaphore_create(0);
        mode_report(NO, 0x3f);
        if (both) mode_report(YES, 0x3f);
        run_mode_timer();
        for (int i = 0; i < 5; i++) {
            now += 2000000000ULL;
            mode_report(NO, 0x3f);
            if (both) mode_report(YES, 0x3f);
            run_mode_timer();
        }
        CHECK(hidJoyConModes[0].tries == 1 && hidJoyConModes[0].pending);
        if (both) CHECK(hidJoyConModes[1].tries == 1 && hidJoyConModes[1].pending);
        dispatch_semaphore_signal(modeWriteGate);
        if (both) dispatch_semaphore_signal(modeWriteGate);
        drain_queues();
        CHECK(modeWrites == (both ? 2u : 1u) && !hidJoyConModes[0].pending);
    } else if (!strcmp(test, "mode-write-timeout")) {
        runtime_feedback_attach(pair);
        modeWriteResult = kIOReturnTimeout;
        mode_report(NO, 0x3f); drain_mode_timer();
        CHECK(modeWrites == 1 && !hidJoyConModes[0].pending);
        now += 1900000000ULL;
        mode_report(NO, 0x3f); drain_mode_timer(); CHECK(modeWrites == 1);
        now += 100000000ULL;
        modeWriteResult = kIOReturnSuccess;
        mode_report(NO, 0x3f); drain_mode_timer(); CHECK(modeWrites == 2);
        unsigned before = feeds;
        mode_report(NO, 0x30); CHECK(feeds == before + 1);
    } else if (!strcmp(test, "simple-stick-blocked-mode")) {
        runtime_feedback_attach(pair);
        modeWriteGate = dispatch_semaphore_create(0);
        left_simple_down();
        float x, y;
        switch_left_get(&raw, 0, 0, now, &x, &y);
        run_mode_timer();   /* mode SetReport is blocked, but input callbacks continue */
        now += 208000000ULL;
        left_simple_down();
        CHECK(leftSource == (__bridge void *)pair && raw.valid && leftY == 16);
        CHECK(switch_left_get(&raw, 0, 0, now, &x, &y) && y < -0.99f);
        CHECK(hidJoyConModes[0].pending && !hidMotion);
        dispatch_semaphore_signal(modeWriteGate); drain_queues();
        left_report(0x30, 49, kIOReturnSuccess);
        CHECK(raw.active && switch_left_get(&raw, 0, 0, now, &x, &y) && y == -1);
    } else if (!strcmp(test, "rapid-physical-reconnect-pending-output") || !strcmp(test, "rumble-reconnect-pending-output")) {
        BOOL rumble = !strcmp(test, "rumble-reconnect-pending-output");
        runtime_feedback_attach(pair);
        if (rumble) {
            rumbleQueue = dispatch_queue_create("joycon-test.reconnect-rumble", DISPATCH_QUEUE_SERIAL);
            CHECK(hid_rumble_start(pair)); drain_queues();
        }
        TestDevice *oldLeft = leftDevice, *oldRight = rightDevice;
        dispatch_semaphore_t gate = dispatch_semaphore_create(0), started = dispatch_semaphore_create(0);
        dispatch_async(hidOut, ^{ dispatch_semaphore_signal(started); dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER); });
        dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
        runtime_feedback_detach(pair);
        leftDevice = [TestDevice new]; rightDevice = [TestDevice new];   /* new physical HID objects before old output drains */
        GCController *replacement = controller(category, NO);
        runtime_feedback_attach(replacement);
        CHECK(oldLeft.opened && oldRight.opened);   /* preserve the old devices until queued writes finish */
        CHECK(leftDevice.opened && rightDevice.opened);
        CHECK(hidLeft == (__bridge IOHIDDeviceRef)leftDevice && hidDevice == (__bridge IOHIDDeviceRef)rightDevice);
        dispatch_semaphore_signal(gate); drain_queues();
        CHECK(!oldLeft.opened && !oldRight.opened && leftDevice.opened && rightDevice.opened);
        CHECK(closes == 2 && !writesAfterClose && neutralWrites == (rumble ? 2u : 0u));
        left_report(0x30, 49, kIOReturnSuccess); CHECK(leftSource == (__bridge void *)replacement);
    } else if (!strcmp(test, "owner-replaced-before-detach")) {
        gyroEnabled = 1; runtime_feedback_attach(pair);
        rumbleQueue = dispatch_queue_create("joycon-test.owner-rumble", DISPATCH_QUEUE_SERIAL);
        CHECK(hid_rumble_start(pair)); drain_queues();
        TestDevice *oldLeft = leftDevice, *oldRight = rightDevice;
        dispatch_semaphore_t gate = dispatch_semaphore_create(0), started = dispatch_semaphore_create(0);
        dispatch_async(hidOut, ^{ dispatch_semaphore_signal(started); dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER); });
        dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
        leftDevice = [TestDevice new]; rightDevice = [TestDevice new];
        GCController *replacement = controller(category, NO);
        runtime_feedback_attach(replacement);   /* new connect notification arrives before the old disconnect */
        CHECK(leftDevice.opened && rightDevice.opened && motionPad == replacement && hidMotion);
        runtime_feedback_detach(pair);
        CHECK(leftDevice.opened && rightDevice.opened && motionPad == replacement);
        dispatch_semaphore_signal(gate); drain_queues();
        CHECK(!oldLeft.opened && !oldRight.opened && leftDevice.opened && rightDevice.opened);
        CHECK(neutralWrites == 2 && !writesAfterClose);
        left_report(0x30, 49, kIOReturnSuccess); CHECK(leftSource == (__bridge void *)replacement);
    } else if (!strcmp(test, "retired-report-callback")) {
        runtime_feedback_attach(pair);
        TestDevice *oldLeft = leftDevice, *oldRight = rightDevice;
        IOHIDReportCallback oldLeftCallback = oldLeft.callback, oldRightCallback = oldRight.callback;
        void *oldLeftContext = oldLeft.callbackContext, *oldRightContext = oldRight.callbackContext;
        runtime_feedback_detach(pair); drain_queues();
        leftDevice = [TestDevice new]; rightDevice = [TestDevice new];
        GCController *replacement = controller(category, NO);
        runtime_feedback_attach(replacement);
        left_report(0x30, 49, kIOReturnSuccess);
        unsigned before = feeds;
        uint8_t report[49] = {0x30}; stick_bytes(report + 6, 4095, 4095);
        oldLeftCallback(oldLeftContext, kIOReturnSuccess, (__bridge void *)oldLeft, kIOHIDReportTypeInput, 0x30, report, sizeof report);
        CHECK(feeds == before && leftX == 2048 && leftY == 0 && leftSource == (__bridge void *)replacement);
        report[0] = 0x3f;
        oldRightCallback(oldRightContext, kIOReturnSuccess, (__bridge void *)oldRight, kIOHIDReportTypeInput, 0x3f, report, 12);
        CHECK(!hidJoyConModes[1].simple && !hidJoyConModes[1].pending);
    } else if (!strcmp(test, "recovery-budget-after-success")) {
        runtime_feedback_attach(pair);
        for (unsigned recovery = 1; recovery <= 6; recovery++) {
            now += 2000000000ULL;
            mode_report(YES, 0x3f); drain_mode_timer();
            CHECK(rightDevice.modeWrites == recovery);
            mode_report(YES, 0x30);
        }
        for (unsigned failure = 1; failure <= 6; failure++) {
            now += 2000000000ULL;
            mode_report(YES, 0x3f); drain_mode_timer();
            CHECK(rightDevice.modeWrites == 6 + (failure < 5 ? failure : 5));   /* consecutive failures remain bounded */
        }
    } else if (!strcmp(test, "recovery-while-write-pending")) {
        runtime_feedback_attach(pair);
        modeWriteGate = dispatch_semaphore_create(0);
        mode_report(YES, 0x3f); run_mode_timer();
        CHECK(hidJoyConModes[1].pending);
        mode_report(YES, 0x30);
        CHECK(hidJoyConModes[1].tries == 0 && hidJoyConModes[1].pending);
        now += 2000000000ULL;
        mode_report(YES, 0x3f); run_mode_timer(); CHECK(hidJoyConModes[1].pending);
        dispatch_semaphore_signal(modeWriteGate); drain_queues();
        CHECK(modeWrites == 1 && !hidJoyConModes[1].pending);
        modeWriteGate = nil;
        now += 2000000000ULL;
        mode_report(YES, 0x3f); drain_mode_timer();
        CHECK(modeWrites == 2 && hidJoyConModes[1].tries == 1);
    } else if (!strcmp(test, "overlapping-retirements")) {
        runtime_feedback_attach(pair);
        rumbleQueue = dispatch_queue_create("joycon-test.overlap", DISPATCH_QUEUE_SERIAL);
        CHECK(hid_rumble_start(pair)); drain_queues();
        dispatch_semaphore_t gate = dispatch_semaphore_create(0), started = dispatch_semaphore_create(0);
        dispatch_async(hidOut, ^{ dispatch_semaphore_signal(started); dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER); });
        dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
        runtime_feedback_detach(pair); dispatch_sync(rumbleQueue, ^{});
        GCController *replacement = controller(category, NO);
        runtime_feedback_attach(replacement); CHECK(hid_rumble_start(replacement)); dispatch_sync(rumbleQueue, ^{});
        runtime_feedback_detach(replacement); dispatch_sync(rumbleQueue, ^{});
        lateNeutralGate = dispatch_semaphore_create(0); lateNeutralStarted = dispatch_semaphore_create(0);
        dispatch_semaphore_signal(gate);
        CHECK(dispatch_semaphore_wait(lateNeutralStarted, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)) == 0);
        run_mode_timer();   /* A's cleanup runs while B's neutral report is still held */
        BOOL alive = leftDevice.opened && rightDevice.opened;
        dispatch_semaphore_signal(lateNeutralGate); drain_queues();
        CHECK(alive && !writesAfterClose && closes == 2 && neutralWrites == 4);
    } else if (!strcmp(test, "many-reused-retirements")) {
        runtime_feedback_attach(pair);
        rumbleQueue = dispatch_queue_create("joycon-test.many-retire", DISPATCH_QUEUE_SERIAL);
        CHECK(hid_rumble_start(pair)); drain_queues();
        dispatch_semaphore_t gate = dispatch_semaphore_create(0), started = dispatch_semaphore_create(0);
        dispatch_async(hidOut, ^{ dispatch_semaphore_signal(started); dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER); });
        dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
        GCController *current = pair;
        for (int i = 0; i < 8; i++) {
            runtime_feedback_detach(current); dispatch_sync(rumbleQueue, ^{});
            current = controller(category, NO); runtime_feedback_attach(current);
            CHECK(hid_rumble_start(current)); dispatch_sync(rumbleQueue, ^{});
        }
        runtime_feedback_detach(current); dispatch_sync(rumbleQueue, ^{});
        dispatch_semaphore_signal(gate); drain_queues();
        CHECK(closes == 2 && neutralWrites == 18 && !writesAfterClose);
        CHECK(!leftDevice.opened && !rightDevice.opened);
        runtime_feedback_attach(pair); CHECK(leftDevice.opened && rightDevice.opened);
        runtime_feedback_detach(pair); drain_queues(); CHECK(closes == 4);
    } else if (!strcmp(test, "generic-shared-lifetime")) {
        unsetenv("TOTK_CONTROLS"); unsetenv("TOTK_RUMBLE_PROFILES"); setenv("TOTK_RUMBLE_HID", "1", 1);
        GCController *generic = controller(GCProductCategoryHID, NO);
        rumbleQueue = dispatch_queue_create("joycon-test.generic", DISPATCH_QUEUE_SERIAL);
        hid_rumble_attach(generic); drain_queues(); CHECK(hidRumbleDevice == (__bridge IOHIDDeviceRef)genericDevice);
        CHECK(hid_imu_start(generic));
        hid_rumble_detach(generic); drain_queues();
        CHECK(genericDevice.opened && hidMotion && !closes);
        runtime_feedback_detach(generic); drain_queues();
        CHECK(!genericDevice.opened && closes == 1 && !writesAfterClose);
    } else if (!strcmp(test, "generic-imu-regroup")) {
        GCController *first = controller(GCProductCategoryHID, NO), *second = controller(GCProductCategoryHID, NO);
        CHECK(hid_imu_start(first)); CHECK(hid_imu_start(second)); drain_queues();
        CHECK(genericDevice.opened && !closes && motionPad == second);
        runtime_feedback_detach(first); CHECK(genericDevice.opened);
        runtime_feedback_detach(second); drain_queues(); CHECK(closes == 1 && !writesAfterClose);
    } else if (!strcmp(test, "generic-imu-rejected")) {
        runtime_feedback_attach(pair);
        CHECK(!hid_imu_start(controller(GCProductCategoryHID, NO))); drain_queues();
        CHECK(!genericDevice.opened && leftDevice.opened && rightDevice.opened && closes == 1);
        left_report(0x30, 49, kIOReturnSuccess); CHECK(leftSource == (__bridge void *)pair);
        runtime_feedback_detach(pair); drain_queues(); CHECK(closes == 3);
    } else if (!strcmp(test, "generic-pending-attach")) {
        unsetenv("TOTK_CONTROLS"); unsetenv("TOTK_RUMBLE_PROFILES"); setenv("TOTK_RUMBLE_HID", "1", 1);
        GCController *first = controller(GCProductCategoryHID, NO), *second = controller(GCProductCategoryHID, NO);
        rumbleQueue = dispatch_queue_create("joycon-test.pending-attach", DISPATCH_QUEUE_SERIAL);
        dispatch_semaphore_t gate = dispatch_semaphore_create(0), started = dispatch_semaphore_create(0);
        dispatch_async(rumbleQueue, ^{ dispatch_semaphore_signal(started); dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER); });
        dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
        hid_rumble_attach(first); hid_rumble_attach(second); CHECK(opens == 1);
        dispatch_semaphore_signal(gate); drain_queues(); CHECK(padRumblePad == first);
        dispatch_async(rumbleQueue, ^{ dispatch_semaphore_signal(started); dispatch_semaphore_wait(gate, DISPATCH_TIME_FOREVER); });
        dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
        hid_rumble_detach(first); hid_rumble_attach(second);
        dispatch_semaphore_signal(gate); drain_queues();
        CHECK(padRumblePad == second && genericDevice.opened && opens == 2 && !closes);
        hid_rumble_detach(second); drain_queues(); CHECK(closes == 1 && !writesAfterClose);
    } else if (!strcmp(test, "switch-rumble-independent-binding")) {
        runtime_feedback_attach(pair);
        rumbleQueue = dispatch_queue_create("joycon-test.rumble-lease", DISPATCH_QUEUE_SERIAL);
        CHECK(hid_rumble_start(pair)); drain_queues();
        IOHIDDeviceRef left = hidLeft, right = hidDevice;
        hidLeft = NULL; hidDevice = NULL; hidProduct = 0;
        hid_joycon_mode_reset(1); hid_joycon_mode_reset(0);
        hid_retire_device(left, hidLeftReport, sizeof hidLeftReport, NO);
        hid_retire_device(right, hidReport, sizeof hidReport, YES);
        drain_queues();   /* rumble is a live consumer even without an input/motion binding */
        CHECK(leftDevice.opened && rightDevice.opened && !closes);
        hid_rumble_stop(); drain_queues();
        CHECK(closes == 2 && neutralWrites == 2 && !writesAfterClose);
    } else if (!strcmp(test, "switch-rumble-restart")) {
        GCController *pro = controller(@"Switch Pro Controller", NO);
        rumbleQueue = dispatch_queue_create("joycon-test.rumble-restart", DISPATCH_QUEUE_SERIAL);
        CHECK(hid_rumble_start(pro)); CHECK(hid_rumble_start(pro)); drain_queues();
        CHECK(rightDevice.opened && !closes);
        hid_rumble_stop(); drain_queues();
        CHECK(closes == 1 && neutralWrites == 2 && !writesAfterClose);
    } else if (!strcmp(test, "generic-stale-sensor")) {
        GCController *first = controller(GCProductCategoryHID, NO), *second = controller(GCProductCategoryHID, NO);
        CHECK(hid_imu_start(first));
        TestDevice *old = genericDevice;
        IOHIDReportWithTimeStampCallback callback = old.timestampCallback;
        void *context = old.timestampContext;
        genericDevice = [TestDevice new]; genericDevice.vendorID = @0x2dc8; genericDevice.productID = @0x6012;
        CHECK(hid_imu_start(second)); drain_queues();
        CHECK(!old.opened && genericDevice.opened);
        uint8_t report[34] = {0x01}; unsigned before = motionFeeds;
        callback(context, kIOReturnSuccess, (__bridge void *)old, kIOHIDReportTypeInput, 0x01, report, sizeof report, 1);
        CHECK(motionFeeds == before);
        genericDevice.timestampCallback(genericDevice.timestampContext, kIOReturnSuccess, (__bridge void *)genericDevice,
            kIOHIDReportTypeInput, 0x01, report, sizeof report, 1);
        CHECK(motionFeeds == before + 1);
    } else if (!strcmp(test, "managed-open-failure")) {
        leftOpenResult = kIOReturnNotOpen;
        runtime_feedback_attach(pair);
        for (int i = 0; i < 20; i++) {
            CHECK(hid_left_open() == kIOReturnNotOpen);
            CHECK(!hid_managed_find((__bridge IOHIDDeviceRef)leftDevice) && !leftDevice.opened && !closes);
        }
        leftOpenResult = kIOReturnSuccess; CHECK(hid_left_open() == kIOReturnSuccess);
        left_report(0x30, 49, kIOReturnSuccess); CHECK(leftSource == (__bridge void *)pair);
    } else if (!strcmp(test, "managed-exclusive-open")) {
        leftOpenResult = kIOReturnExclusiveAccess;
        runtime_feedback_attach(pair);
        CHECK(!leftDevice.opened && closes == 1 && !hid_managed_find((__bridge IOHIDDeviceRef)leftDevice));
        leftOpenResult = kIOReturnSuccess; CHECK(hid_left_open() == kIOReturnSuccess);
        unsigned before = closes;
        leftOpenResult = kIOReturnExclusiveAccess;
        CHECK(hid_open_binding((__bridge IOHIDDeviceRef)leftDevice, test_manager(kCFAllocatorDefault, 0)) == kIOReturnExclusiveAccess);
        CHECK(leftDevice.opened && closes == before && hid_managed_find((__bridge IOHIDDeviceRef)leftDevice)->lifetime.bindings == 1);
        leftOpenResult = kIOReturnSuccess;
    } else if (!strcmp(test, "motion-init-owner-changed")) {
        motionPad = controller(@"Switch Pro Controller", NO); hid_motion_start(); CHECK(hidMotion);
        CHECK(hid_imu_start(controller(GCProductCategoryHID, NO)));
        for (int i = 0; i < 4; i++) drain_mode_timer();   /* all previous Switch setup deadlines have passed */
        CHECK(!switchWritesToGeneric && genericDevice.opened && hidMotion && hidProduct == 0);
    } else if (!strcmp(test, "motion-init-restarted")) {
        motionPad = controller(@"Switch Pro Controller", NO); hid_motion_start(); CHECK(hidMotion);
        hid_motion_stop(); hid_motion_start(); CHECK(hidMotion);
        for (int i = 0; i < 4; i++) drain_mode_timer();
        CHECK(switchInitWrites == 3 && !writesAfterClose);   /* only the current IMU-on/factory/user setup */
    } else return 2;
    /* End every scenario through the real release paths, not by dropping flags at process exit. */
    if (padRumblePad) hid_rumble_detach(padRumblePad);
    if (hidRumble) hid_rumble_stop();
    if (motionPad) runtime_feedback_detach(motionPad);
    if (hidJoyConPad) runtime_feedback_detach(hidJoyConPad);
    hid_close_unused(); hid_left_close_unused();
    drain_queues();
    CHECK(!hidManagedDevices && !hidMotion && !hidRumble && !writesAfterClose);
    CHECK(!leftDevice.opened && !rightDevice.opened && !genericDevice.opened);
    printf("PASS: %s\n", argv[1]);
    return 0;
}}
