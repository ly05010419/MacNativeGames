/* The iPhone's own motion sensor as the game's six-axis device (2026-10-24).
   The touch controls give the player sticks and buttons but no gyro; CoreMotion's device motion supplies it, so bow and
   Ultrahand aiming can follow the phone while the left thumb walks and the right thumb turns the camera. Samples reach the
   game through runtime/motion_fusion.c, exactly like a controller's GCMotion samples, and the phone source is used only
   while no controller is attached at all (touch play); a controller brings its own buttons and sticks (and, where supported, its gyro).
   The device frame is rotated into the landscape screen's frame: GameController's x is right on the screen, y is up, z is
   out of the face, and acceleration is gravity plus user acceleration (face up reads z = -1 G).
   The phone is expected in landscape, held like a pad: turning it left and right (about the screen's y) is yaw, tilting
   it forward and back is pitch, and rolling it is roll. TOTK_PHONE_GYRO=0 turns the phone sensor off; TOTK_GYRO=0 still
   removes the game's six-axis device entirely. NSMotionUsageDescription explains the sensor in Info.plist. */
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

#if TARGET_OS_IOS

static CMMotionManager *manager;
static NSOperationQueue *queue;
static BOOL started, enabled;
static _Atomic int active;
static UIInterfaceOrientation orientation = UIInterfaceOrientationLandscapeRight;
static _Atomic uint64_t samples;
static _Atomic int sampleErrorLogged;

/* Device axes (portrait: x right, y up, z out of the screen) -> the landscape picture's axes. Both landscape cases are
   proper rotations, so the gyro's right-hand sense survives. */
static void to_screen(const double in[3], double out[3]) {
    switch (orientation) {
    case UIInterfaceOrientationLandscapeRight:   /* home button on the right */
        out[0] = -in[1]; out[1] = in[0]; out[2] = in[2]; break;
    case UIInterfaceOrientationLandscapeLeft:    /* home button on the left */
        out[0] = in[1]; out[1] = -in[0]; out[2] = in[2]; break;
    case UIInterfaceOrientationPortraitUpsideDown:
        out[0] = -in[0]; out[1] = -in[1]; out[2] = in[2]; break;
    default:                                     /* portrait, if it ever runs there */
        out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; break;
    }
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
    return orientation;
}

/* With a controller in hand the phone is usually resting somewhere, so the phone sensor is for touch play only; a
   controller with motion brings its own gyro. */
static BOOL controller_connected(void) {
    for (GCController *controller in GCController.controllers)
        if (controller.extendedGamepad) return YES;
    return NO;
}

static BOOL start_updates(void) {
    if (!manager.isDeviceMotionAvailable) {
        fprintf(stderr, "[phone-gyro] this device has no device motion: no phone gyro\n");
        return NO;
    }
    if (manager.isDeviceMotionActive) return YES;
    manager.deviceMotionUpdateInterval = 1.0 / 60.0;
    [manager startDeviceMotionUpdatesUsingReferenceFrame:CMAttitudeReferenceFrameXArbitraryZVertical toQueue:queue
                                            withHandler:^(CMDeviceMotion *motion, NSError *error) {
        if (error) {
            if (!atomic_exchange(&sampleErrorLogged, 1))
                fprintf(stderr, "[phone-gyro] device motion stopped: %s\n", error.localizedDescription.UTF8String);
            return;
        }
        if (!motion) return;
        const double deviceAcc[3] = {motion.gravity.x + motion.userAcceleration.x,
                                     motion.gravity.y + motion.userAcceleration.y,
                                     motion.gravity.z + motion.userAcceleration.z};
        const double deviceGyro[3] = {motion.rotationRate.x, motion.rotationRate.y, motion.rotationRate.z};
        double acc[3], gyro[3];
        to_screen(deviceAcc, acc);
        to_screen(deviceGyro, gyro);
        uint64_t n = atomic_fetch_add(&samples, 1);
        if (n == 0)
            fprintf(stderr, "[phone-gyro] first sample: acc %+.2f %+.2f %+.2f G, gyro %+.1f %+.1f %+.1f deg/s (%s)\n",
                    acc[0], acc[1], acc[2], gyro[0] * 180 / M_PI, gyro[1] * 180 / M_PI, gyro[2] * 180 / M_PI,
                    orientation == UIInterfaceOrientationLandscapeLeft ? "landscape left" :
                    orientation == UIInterfaceOrientationLandscapeRight ? "landscape right" : "portrait");
        runtime_motion_feed(acc, gyro, runtime_now_ns());
    }];
    /* isDeviceMotionActive can still be false right after the call; the handler above is already scheduled. */
    return YES;
}

static void stop_updates(void) {
    if (manager.isDeviceMotionActive) [manager stopDeviceMotionUpdates];
}

static void phone_start(void) {
    if (atomic_load(&active)) return;
    orientation = current_orientation();
    atomic_store(&samples, 0); atomic_store(&sampleErrorLogged, 0);
    runtime_motion_phone(1);
    runtime_motion_deadzone_default(0.6);   /* a phone at rest is quiet, but not as quiet as a DualSense */
    runtime_motion_start();
    if (!start_updates()) { runtime_motion_phone(0); return; }
    atomic_store(&active, 1);
    fprintf(stderr, "[phone-gyro] on: the iPhone's motion sensor is the six-axis device\n");
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (atomic_load(&active))
            fprintf(stderr, "[phone-gyro] %llu samples in 3 s\n", (unsigned long long)atomic_load(&samples));
    });
}

static void phone_stop(const char *why) {
    if (!atomic_load(&active)) return;
    stop_updates();
    atomic_store(&active, 0);
    runtime_motion_phone(0);
    fprintf(stderr, "[phone-gyro] off: %s\n", why);
}

static void update_active(void) {
    if (!enabled) return;
    BOOL pad = controller_connected();
    BOOL foreground = UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
    BOOL want = foreground && !pad;
    if (want && !atomic_load(&active)) phone_start();
    else if (!want && atomic_load(&active)) phone_stop(pad ? "a controller is connected" : "the app is not active");
}

static void orientation_changed(void) {
    UIInterfaceOrientation o = current_orientation();
    if (o == orientation) return;
    BOOL was = atomic_load(&active);
    if (was) phone_stop("the interface orientation changed");
    orientation = o;
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
    manager = [[CMMotionManager alloc] init];
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserverForName:GCControllerDidConnectNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; update_active(); }];
    [center addObserverForName:GCControllerDidDisconnectNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; update_active(); }];
    [center addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; update_active(); }];
    [center addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; update_active(); }];
    [center addObserverForName:UIDeviceOrientationDidChangeNotification object:nil queue:NSOperationQueue.mainQueue
                    usingBlock:^(NSNotification *note) { (void)note; orientation_changed(); }];
    update_active();
    /* A controller that was already connected when the game started may not be in GCController.controllers yet. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ update_active(); });
}

#else

void totk_phone_motion_start(void) {}

#endif
