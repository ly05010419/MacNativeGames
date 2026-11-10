/* The iPhone's own motion sensor as the game's six-axis device (2026-10-24; one-owner hand-over 2026-11-01).
   The touch controls give the player sticks and buttons but no gyro; CoreMotion's device motion supplies it, so bow and
   Ultrahand aiming can follow the phone while the left thumb walks and the right thumb turns the camera. Samples reach the
   game through runtime/motion_fusion.c, exactly like a controller's GCMotion samples.
   One gyro feeds the fusion at a time - two sources would be mixed (the user's request, 2026-10-24, needed now that the
   gamepad's own motion also reaches iOS through gamepad_feedback.m): a controller that reports its own motion aims by
   default; touching the screen hands the aiming to the phone; operating the controller's sticks or buttons gives it back.
   A controller without motion never takes the aiming away: the phone keeps aiming, exactly as with no controller at all.
   The hand-over silences the other source: the phone's handler drops its samples while the controller owns the sensor, and
   a manual-activation controller's GCMotion handler goes quiet when its sensors are turned off (controllers whose sensors
   cannot be turned off would still mix: not seen on iOS, both the DualSense and the Switch-style pads connected here are
   manual-activation). Re-applying the owner after the app returns matters: the runtime reactivates controller sensors on
   becoming active (PR #432's runtime_feedback_active), and the phone must take them back off if the player last chose
   touch.
   The device frame is rotated into the landscape screen's frame (device_motion_axes.h, probed host-side by
   tests/test_device_motion.py): GameController's x is right on the screen, y is up, z is out of the face, and acceleration
   is gravity plus user acceleration (face up reads z = -1 G).
   The phone is expected in landscape, held like a pad: turning it left and right (about the screen's y) is yaw, tilting
   it forward and back is pitch, and rolling it is roll. Samples come 60 a second. TOTK_PHONE_GYRO=0 (the Settings app
   switch) turns the phone sensor off; TOTK_GYRO=0 still removes the game's six-axis device entirely.
   NSMotionUsageDescription explains the sensor in Info.plist. The sensor runs only while the phone's gyro aims (the app
   active, and no motion controller or the screen touched since): while a controller's gyro aims it is stopped, and so is
   the 20 Hz watch of the controller's input, which runs only while the phone has taken the aiming from a motion
   controller. runtime_motion_phone(1) on each start re-aligns the frame to gravity. */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <CoreMotion/CoreMotion.h>
#import <GameController/GameController.h>
#include <TargetConditionals.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "phone_settings.h"
#include "device_motion_axes.h"
#import "device_motion.h"

#if TARGET_OS_IOS

_Static_assert(DEVICE_MOTION_UI_LANDSCAPE_RIGHT == UIInterfaceOrientationLandscapeRight &&
               DEVICE_MOTION_UI_LANDSCAPE_LEFT == UIInterfaceOrientationLandscapeLeft, "UIInterfaceOrientation");
static CMMotionManager *manager;
static NSOperationQueue *queue;
static BOOL started, enabled, sensing;   /* sensing: updates asked for and not stopped (main thread) */
static _Atomic int active;                                        /* the sensor runs (samples may be gated) */
static _Atomic int orientation = DEVICE_MOTION_UI_LANDSCAPE_RIGHT;   /* read on the sensor's queue */
static _Atomic uint64_t samples;
static _Atomic int sampleErrorLogged;
/* Which motion source owns the game's single six-axis sensor (see the file comment). Read on the sensor's queue. */
static _Atomic int controller_has_motion;                         /* a connected controller reports a gyro */
static _Atomic int phone_owns_gyro;                               /* touch took the aiming away from it */

static GCController *first_motion_controller(void) {
    for (GCController *c in GCController.controllers) if (c.motion && c.motion.hasRotationRate) return c;
    return nil;
}
static void set_controller_sensors(BOOL on) {   /* main thread */
    for (GCController *c in GCController.controllers)
        if (c.motion && c.motion.hasRotationRate && c.motion.sensorsRequireManualActivation) c.motion.sensorsActive = on;
}
static void follow_owner(void);
static void claim_phone_gyro(void) {   /* main thread */
    if (!atomic_exchange(&phone_owns_gyro, 1)) fprintf(stderr, "[gyro] the screen was touched: the phone's gyro aims now\n");
    set_controller_sensors(NO);
    follow_owner();
}
static void claim_pad_gyro(void) {   /* main thread */
    if (atomic_exchange(&phone_owns_gyro, 0)) fprintf(stderr, "[gyro] the gamepad is used: its own gyro aims again\n");
    set_controller_sensors(YES);
    follow_owner();   /* dropping the phone's flag drops its last samples at once */
}
static void recheck_controllers(void) {   /* main thread */
    BOOL any = first_motion_controller() != nil;
    if (atomic_exchange(&controller_has_motion, any) == any) return;
    atomic_store(&phone_owns_gyro, 0);   /* a (dis)connecting controller starts with the controller's aim, as before */
    if (any) fprintf(stderr, "[gyro] a controller brings its own motion: the phone's gyro yields to it\n");
    else fprintf(stderr, "[gyro] no controller with motion: the phone's gyro is aiming again\n");
    follow_owner();
}
void totk_device_motion_note_touch(void) {   /* the touch layer saw a finger (touch_pad.m); main thread */
    if (enabled && atomic_load(&active) && atomic_load(&controller_has_motion) && !atomic_load(&phone_owns_gyro))
        claim_phone_gyro();
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
static dispatch_source_t pad_watch;   /* main thread; exists only while the phone has the aiming from a motion controller */
static void watch_gamepad(BOOL on) {   /* main thread */
    if (!on) { if (pad_watch) { dispatch_source_cancel(pad_watch); pad_watch = nil; } return; }
    if (pad_watch) return;
    pad_watch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(pad_watch, DISPATCH_TIME_NOW, NSEC_PER_SEC / 20, NSEC_PER_SEC / 100);
    dispatch_source_set_event_handler(pad_watch, ^{
        if (!atomic_load(&controller_has_motion) || !atomic_load(&phone_owns_gyro)) return;
        for (GCController *c in GCController.controllers)
            if (c.motion && c.motion.hasRotationRate && gamepad_used(c.extendedGamepad)) { claim_pad_gyro(); return; }
    });
    dispatch_resume(pad_watch);
}

static UIInterfaceOrientation current_orientation(void) {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class]) continue;
        UIWindowScene *window = (UIWindowScene *)scene;
        if (window.activationState != UISceneActivationStateForegroundActive) continue;
        UIInterfaceOrientation o = window.effectiveGeometry.interfaceOrientation;
        if (o == UIInterfaceOrientationLandscapeLeft || o == UIInterfaceOrientationLandscapeRight ||
            o == UIInterfaceOrientationPortrait || o == UIInterfaceOrientationPortraitUpsideDown) return o;
    }
    return (UIInterfaceOrientation)atomic_load(&orientation);
}

static BOOL start_updates(void) {
    if (!manager.isDeviceMotionAvailable) {
        fprintf(stderr, "[phone-gyro] this device has no device motion: no phone gyro\n");
        return NO;
    }
    if (sensing) return YES;
    manager.deviceMotionUpdateInterval = 1.0 / 60.0;
    [manager startDeviceMotionUpdatesUsingReferenceFrame:CMAttitudeReferenceFrameXArbitraryZVertical toQueue:queue
                                            withHandler:^(CMDeviceMotion *motion, NSError *error) {
        if (error) {
            if (!atomic_exchange(&sampleErrorLogged, 1))
                fprintf(stderr, "[phone-gyro] device motion stopped: %s\n", error.localizedDescription.UTF8String);
            return;
        }
        if (!motion) return;
        if (atomic_load(&controller_has_motion) && !atomic_load(&phone_owns_gyro))
            return;   /* the controller's gyro took the aiming between this sample and the sensor's stop */
        const double deviceAcc[3] = {motion.gravity.x + motion.userAcceleration.x,
                                     motion.gravity.y + motion.userAcceleration.y,
                                     motion.gravity.z + motion.userAcceleration.z};
        const double deviceGyro[3] = {motion.rotationRate.x, motion.rotationRate.y, motion.rotationRate.z};
        double acc[3], gyro[3]; const int o = atomic_load(&orientation);
        device_motion_to_landscape(o, deviceAcc, acc);
        device_motion_to_landscape(o, deviceGyro, gyro);
        uint64_t n = atomic_fetch_add(&samples, 1);
        if (n == 0)
            fprintf(stderr, "[phone-gyro] first sample: acc %+.2f %+.2f %+.2f G, gyro %+.1f %+.1f %+.1f deg/s (%s)\n",
                    acc[0], acc[1], acc[2], gyro[0] * 180 / M_PI, gyro[1] * 180 / M_PI, gyro[2] * 180 / M_PI,
                    o == DEVICE_MOTION_UI_LANDSCAPE_LEFT ? "landscape left" : "landscape right");
        runtime_motion_feed(acc, gyro, runtime_now_ns());
    }];
    /* isDeviceMotionActive can still be false right after the call; the handler above is already scheduled. */
    sensing = YES;
    return YES;
}

static void stop_updates(void) {
    if (sensing) [manager stopDeviceMotionUpdates];
    sensing = NO;
}

/* The phone's sensor and the input watch follow the owner of the aiming (main thread, while the app is active). */
static void follow_owner(void) {
    if (!atomic_load(&active)) { watch_gamepad(NO); return; }
    BOOL pad = atomic_load(&controller_has_motion) && !atomic_load(&phone_owns_gyro);
    watch_gamepad(atomic_load(&controller_has_motion) && atomic_load(&phone_owns_gyro));
    if (pad) { stop_updates(); runtime_motion_phone(0); return; }
    if (!sensing && start_updates()) runtime_motion_phone(1);   /* re-aligned to gravity */
}

static void phone_start(void) {
    if (atomic_load(&active)) return;
    atomic_store(&orientation, (int)current_orientation());
    atomic_store(&samples, 0); atomic_store(&sampleErrorLogged, 0);
    runtime_motion_phone(1);
    runtime_motion_deadzone_default(0.6);   /* a phone at rest is quiet, but not as quiet as a DualSense */
    runtime_motion_start();
    if (!manager.isDeviceMotionAvailable) { fprintf(stderr, "[phone-gyro] this device has no device motion: no phone gyro\n"); runtime_motion_phone(0); return; }
    atomic_store(&active, 1);
    recheck_controllers();   /* a motion controller that appeared while the app was away takes the aim back */
    if (atomic_load(&phone_owns_gyro)) set_controller_sensors(NO);   /* the runtime re-enabled the sensors on returning: the player's choice still stands */
    follow_owner();   /* the sensor runs only while the phone aims */
    fprintf(stderr, "[phone-gyro] on: the iPhone's motion sensor feeds the six-axis device (%.0f a second)\n",
            1 / manager.deviceMotionUpdateInterval);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (atomic_load(&active))
            fprintf(stderr, "[phone-gyro] %llu samples in 3 s\n", (unsigned long long)atomic_load(&samples));
    });
}

static void phone_stop(const char *why) {
    if (!atomic_load(&active)) return;
    stop_updates();
    atomic_store(&active, 0);
    watch_gamepad(NO);
    runtime_motion_phone(0);
    fprintf(stderr, "[phone-gyro] off: %s\n", why);
}

static void update_active(void) {
    if (!enabled) return;
    BOOL foreground = UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
    /* A controller no longer stops the sensor: which source feeds is decided per sample (see the file comment); only
       the app leaving the foreground stops the sensor itself. */
    if (foreground && !atomic_load(&active)) phone_start();
    else if (!foreground && atomic_load(&active)) phone_stop("the app is not active");
}

static void orientation_changed(void) {
    int o = (int)current_orientation();
    if (o == atomic_load(&orientation)) return;
    BOOL was = atomic_load(&active);
    if (was) phone_stop("the interface orientation changed");
    atomic_store(&orientation, o);
    if (was) phone_start();
}

void totk_phone_motion_start(void) {
    if (started) return;
    started = YES;
    enabled = totk_phone_setting_enabled(@"TOTK_PHONE_GYRO") && runtime_gyro_enabled();
    if (!enabled) {
        fprintf(stderr, "[phone-gyro] off (phone setting or gyro disabled)\n");
        return;
    }
    queue = [[NSOperationQueue alloc] init];
    queue.name = @"totk.phone-motion";
    queue.maxConcurrentOperationCount = 1;
    queue.qualityOfService = NSQualityOfServiceUserInteractive;
    manager = [[CMMotionManager alloc] init];
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; recheck_controllers(); }];
    [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; recheck_controllers(); }];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; update_active(); }];
    [center addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; update_active(); }];
    [center addObserverForName:UIDeviceOrientationDidChangeNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; orientation_changed(); }];
    update_active();
    /* A controller that was already connected when the game started may not be in GCController.controllers yet. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        recheck_controllers();
        update_active();
    });
}

#else

void totk_phone_motion_start(void) {}
void totk_device_motion_note_touch(void) {}

#endif
