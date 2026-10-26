/* iPhone / iPad: the device's own motion sensors as the Switch's six-axis sensor, so the bow (and everything else the game
   aims with motion) follows the phone, as on a Switch held in the hands. On with TOTK_GYRO=1 (Settings › Motion aiming),
   and only while no controller with motion sensors is connected: then runtime/gamepad_feedback.m feeds the controller's.

   CoreMotion gives the device frame (portrait: x to the right, y to the top edge, z out of the screen; gravity and
   rotation rate in G and rad/s, lying face up z = -1 G), the same conventions as GCMotion. The game is played in landscape,
   so the samples are turned into the screen's frame as the player sees it (x to the right, y up, z out of the screen),
   which is the controller frame runtime/motion_fusion.c expects. */
#import <CoreMotion/CoreMotion.h>
#import <GameController/GameController.h>
#import <UIKit/UIKit.h>
#include <stdatomic.h>
#include <stdio.h>
#include "runtime.h"

int runtime_gyro_enabled(void);   /* runtime/hid_feedback.c: TOTK_GYRO */

static CMMotionManager *g_motion;
static _Atomic long g_orientation = UIInterfaceOrientationLandscapeRight;
static _Atomic int g_pad_motion;   /* a connected controller has motion sensors */

void phone_motion_orientation(UIInterfaceOrientation o) {
    if (o == UIInterfaceOrientationLandscapeLeft || o == UIInterfaceOrientationLandscapeRight) atomic_store(&g_orientation, (long)o);
}
static void pad_check(void) {   /* main queue */
    int any = 0; for (GCController *c in GCController.controllers) if (c.motion) any = 1;
    atomic_store(&g_pad_motion, any);
}

void phone_motion_start(void) {   /* main queue, once the game starts */
    if (g_motion || !runtime_gyro_enabled()) return;
    g_motion = [CMMotionManager new];
    if (!g_motion.deviceMotionAvailable) { fprintf(stderr, "[gyro] this device has no motion sensors\n"); return; }
    pad_check();
    for (NSNotificationName n in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification])
        [NSNotificationCenter.defaultCenter addObserverForName:n object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) { (void)note; pad_check(); }];
    g_motion.deviceMotionUpdateInterval = 1.0 / 100;
    NSOperationQueue *queue = [NSOperationQueue new]; queue.maxConcurrentOperationCount = 1; queue.qualityOfService = NSQualityOfServiceUserInteractive;
    __block int owned = 0;   /* this source holds the sensor (runtime_motion_live) */
    [g_motion startDeviceMotionUpdatesToQueue:queue withHandler:^(CMDeviceMotion *m, NSError *error) {
        (void)error;
        if (!m) return;
        if (atomic_load(&g_pad_motion)) { owned = 0; return; }   /* the controller's sensor has it */
        if (!owned) { runtime_motion_live(1); owned = 1; }
        const double ax = m.gravity.x + m.userAcceleration.x, ay = m.gravity.y + m.userAcceleration.y, az = m.gravity.z + m.userAcceleration.z;
        const double rx = m.rotationRate.x, ry = m.rotationRate.y, rz = m.rotationRate.z;
        /* LandscapeRight: the device's top edge is on the player's left, so the screen's right is the device's -y and its
           up the device's x; LandscapeLeft the other way round. z is the same. */
        double acc[3], gyro[3];
        if (atomic_load(&g_orientation) == UIInterfaceOrientationLandscapeRight) {
            acc[0] = -ay; acc[1] = ax; acc[2] = az; gyro[0] = -ry; gyro[1] = rx; gyro[2] = rz;
        } else {
            acc[0] = ay; acc[1] = -ax; acc[2] = az; gyro[0] = ry; gyro[1] = -rx; gyro[2] = rz;
        }
        runtime_motion_feed(acc, gyro, runtime_now_ns());
    }];
    fprintf(stderr, "[gyro] the device's motion sensors aim (no controller with motion connected)\n");
}
