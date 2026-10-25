/* The iPhone's / iPad's own motion sensors as the Switch controller's six-axis sensor.

   The game asks for the six-axis sensor itself (hid StartSixAxisSensor -> runtime_motion_start) and runtime/motion_fusion.c
   turns samples into controller states; on the Mac gamepad_feedback.m feeds them from the connected controller, but on iOS
   nothing ever did, so the game always saw a controller lying still. Here CMMotionManager device motion (100 a second:
   gravity + user acceleration in G, rotation rate in rad/s - the same frame and units as the Switch/GCMotion six-axis) is
   rotated from the phone's portrait frame into the landscape frame the game uses (device_motion_axes.h) and fed with
   runtime_motion_feed. runtime_gyro_enabled() (TOTK_GYRO=0, the Settings app switch) turns this off; TOTK_GYRO_AXES still
   remaps for testing. Updates stop while the app is inactive and runtime_motion_live(1) on resume re-aligns the frame to
   gravity. No smoothing and no dead zone of our own: the game has its own motion options. The sensitivity is the
   player's (the touch editor's gyro slider, Documents/save/motion.json): it scales the rotation rate only, so the aim
   speed follows it and the gravity correction (acceleration) keeps its true value.

   One gyro feeds the fusion at a time - two sources would be mixed by runtime/motion_fusion.c (the user's request
   2026-10-24, needed once the gamepad's own motion also reaches iOS): a controller with motion aims by default;
   touching the screen hands the aiming to the phone; operating the controller's sticks or buttons gives it back.
   The hand-over silences the other source: the phone's handler drops its samples while the controller owns the sensor,
   and a manual-activation controller's GCMotion handler goes quiet when its sensors are turned off (controllers whose
   sensors cannot be turned off would still mix: not seen on iOS, both the DualSense and the Switch-style pads
   connected here are manual-activation). Re-applying the owner after the app returns matters: the runtime reactivates
   controller sensors on becoming active (PR #432's runtime_feedback_active), and the phone must take them back off if
   the player last chose touch. */
#import <CoreMotion/CoreMotion.h>
#import <GameController/GameController.h>
#import <UIKit/UIKit.h>
#include <stdatomic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "device_motion_axes.h"
#import "device_motion.h"

_Static_assert(DEVICE_MOTION_UI_LANDSCAPE_RIGHT == UIInterfaceOrientationLandscapeRight && DEVICE_MOTION_UI_LANDSCAPE_LEFT == UIInterfaceOrientationLandscapeLeft, "UIInterfaceOrientation");
static CMMotionManager *manager;
static NSOperationQueue *queue;
static _Atomic int orientation = DEVICE_MOTION_UI_LANDSCAPE_RIGHT;
static _Atomic double gain = 1;   /* the player's sensitivity (device_motion.h), read by the sensor thread */
static BOOL wanted, running;   /* main thread */
/* Which motion source owns the game's single six-axis sensor (see the file comment). Read on the sensor's queue. */
static _Atomic int controller_has_motion;   /* a connected controller reports a gyro */
static _Atomic int phone_owns_gyro;         /* touch took the aiming away from it */
static GCController *first_motion_controller(void) {
    for (GCController *c in GCController.controllers) if (c.motion && c.motion.hasRotationRate) return c;
    return nil;
}
static void set_controller_sensors(BOOL on) {   /* main thread */
    for (GCController *c in GCController.controllers) if (c.motion && c.motion.hasRotationRate && c.motion.sensorsRequireManualActivation) c.motion.sensorsActive = on;
}
static void claim_phone_gyro(void) {   /* main thread */
    if (!atomic_exchange(&phone_owns_gyro, 1)) fprintf(stderr, "[gyro] the screen was touched: the phone's gyro aims now\n");
    set_controller_sensors(NO);
}
static void claim_pad_gyro(void) {   /* main thread */
    if (atomic_exchange(&phone_owns_gyro, 0)) fprintf(stderr, "[gyro] the gamepad is used: its own gyro aims again\n");
    set_controller_sensors(YES);
}
static void recheck_controllers(void) {   /* main thread */
    GCController *c = first_motion_controller();
    BOOL any = c != nil;
    if (atomic_exchange(&controller_has_motion, any) != any) {
        if (any) fprintf(stderr, "[gyro] a controller brings its own motion: the phone's gyro yields to it\n");
        else fprintf(stderr, "[gyro] no controller with motion: the phone's gyro is aiming again\n");
        atomic_store(&phone_owns_gyro, 0);   /* a (dis)connecting controller starts with the controller's aim, as before */
    }
}
void totk_device_motion_note_touch(void) {   /* the touch layer saw a finger (touch_pad.m); main thread */
    if (wanted && running && atomic_load(&controller_has_motion) && !atomic_load(&phone_owns_gyro)) claim_phone_gyro();
}
/* The player picked the gamepad back up: a stick, the dpad, a trigger or a button moved. runtime/gamepad.m owns the
   extendedGamepad's valueChangedHandler and sets it again on a reconnect, so nothing here wraps or replaces a handler:
   while the phone owns the gyro a 20 Hz timer on the main queue reads the controller's state. The thresholds are plain:
   a resting controller never reaches them. */
static BOOL gamepad_used(GCExtendedGamepad *p) {
    if (!p) return NO;
    if (fabs(p.leftThumbstick.xAxis.value) > .25 || fabs(p.leftThumbstick.yAxis.value) > .25 ||
        fabs(p.rightThumbstick.xAxis.value) > .25 || fabs(p.rightThumbstick.yAxis.value) > .25 ||
        fabs(p.dpad.xAxis.value) > .5 || fabs(p.dpad.yAxis.value) > .5) return YES;
    GCControllerButtonInput *buttons[] = {p.buttonA, p.buttonB, p.buttonX, p.buttonY, p.leftShoulder, p.rightShoulder, p.leftTrigger, p.rightTrigger};
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) if (buttons[i].value > .5) return YES;
    return NO;
}
static dispatch_source_t pad_watch;
static void watch_gamepad(void) {   /* main thread */
    if (pad_watch) return;
    pad_watch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(pad_watch, DISPATCH_TIME_NOW, NSEC_PER_SEC / 20, NSEC_PER_SEC / 100);
    dispatch_source_set_event_handler(pad_watch, ^{
        if (!atomic_load(&controller_has_motion) || !atomic_load(&phone_owns_gyro)) return;
        for (GCController *c in GCController.controllers)
            if (c.motion && c.motion.hasRotationRate && gamepad_used(c.extendedGamepad)) { claim_pad_gyro(); return; }
    });
    dispatch_resume(pad_watch);
    fprintf(stderr, "[gyro] watching the gamepad's input to hand its gyro back\n");
}

/* Documents/save/motion.json, next to the saves and the touch layout: {"sensitivity": 100} in percent, 10-200, default
   100. Anything else reads as the default; it is read at the game's start and whenever the editor changes it. */
static NSString *motion_path(void) {
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    return [[docs stringByAppendingPathComponent:@"save"] stringByAppendingPathComponent:@"motion.json"];
}
double totk_device_motion_sensitivity(void) {
    NSData *raw = [NSData dataWithContentsOfFile:motion_path()];
    NSDictionary *d = raw ? [NSJSONSerialization JSONObjectWithData:raw options:0 error:NULL] : nil;
    id v = [d isKindOfClass:NSDictionary.class] ? d[@"sensitivity"] : nil;
    double p = [v isKindOfClass:NSNumber.class] ? [v doubleValue] : 100;
    return (p >= 10 && p <= 200 ? p : 100) / 100.0;
}
void totk_device_motion_set_sensitivity(double percent) {
    double p = MIN(MAX(percent, 10.), 200.);
    NSString *path = motion_path();
    [NSFileManager.defaultManager createDirectoryAtPath:[path stringByDeletingLastPathComponent] withIntermediateDirectories:YES attributes:nil error:nil];
    NSData *json = [NSJSONSerialization dataWithJSONObject:@{@"sensitivity": @(round(p))} options:0 error:NULL];
    if (json) [json writeToFile:path atomically:YES];
    atomic_store(&gain, p / 100.0);
}

void totk_device_motion_set_orientation(UIInterfaceOrientation o) {
    if (o == UIInterfaceOrientationLandscapeLeft || o == UIInterfaceOrientationLandscapeRight) atomic_store(&orientation, (int)o);
}
static void motion_start(void) {
    if (running) return;
    if (!manager.deviceMotionAvailable) { fprintf(stderr, "[gyro] no motion sensors on this device\n"); return; }
    runtime_motion_deadzone_default(0);
    [manager startDeviceMotionUpdatesToQueue:queue withHandler:^(CMDeviceMotion *m, NSError *error) {
        if (!m || error) return;
        if (atomic_load(&controller_has_motion) && !atomic_load(&phone_owns_gyro)) return;   /* the controller's gyro aims: two sources would mix */
        const double acc[3] = {m.gravity.x + m.userAcceleration.x, m.gravity.y + m.userAcceleration.y, m.gravity.z + m.userAcceleration.z};
        const double rot[3] = {m.rotationRate.x * atomic_load(&gain), m.rotationRate.y * atomic_load(&gain), m.rotationRate.z * atomic_load(&gain)};
        double a[3], g[3]; const int o = atomic_load(&orientation);
        device_motion_to_landscape(o, acc, a); device_motion_to_landscape(o, rot, g);
        runtime_motion_feed(a, g, runtime_now_ns());
    }];
    running = YES; runtime_motion_live(1);
    recheck_controllers();
    if (atomic_load(&phone_owns_gyro)) set_controller_sensors(NO);   /* the runtime re-enabled the sensors on returning: the player's choice still stands */
    fprintf(stderr, "[gyro] device motion on (%.0f a second)\n", 1 / manager.deviceMotionUpdateInterval);
}
static void motion_stop(void) {
    if (!running) return;
    [manager stopDeviceMotionUpdates]; running = NO;
    if (!atomic_load(&controller_has_motion)) runtime_motion_live(0);   /* a controller that brings motion keeps the sensor live */
    fprintf(stderr, "[gyro] device motion off\n");
}
void totk_device_motion_begin(void) {
    if (wanted) return;
    if (!runtime_gyro_enabled()) { fprintf(stderr, "[gyro] off (TOTK_GYRO=0)\n"); return; }
    const char *axes = getenv("TOTK_GYRO_AXES"); int r = runtime_motion_axes(axes);
    if (r < 0) runtime_fail("TOTK_GYRO_AXES must name x, y and z once each, e.g. \"-x,y,-z\"");
    if (r > 0) fprintf(stderr, "[gyro] axes remapped by TOTK_GYRO_AXES=%s\n", axes);
    manager = [CMMotionManager new]; manager.deviceMotionUpdateInterval = 1.0 / 100;
    queue = [NSOperationQueue new]; queue.maxConcurrentOperationCount = 1; queue.qualityOfService = NSQualityOfServiceUserInteractive;
    wanted = YES; atomic_store(&gain, totk_device_motion_sensitivity());
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        (void)n; recheck_controllers();
    }];
    [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; recheck_controllers(); }];
    watch_gamepad();
    motion_start();
}
void totk_device_motion_pause(BOOL paused) {
    if (!wanted) return;
    if (paused) motion_stop(); else motion_start();
}
