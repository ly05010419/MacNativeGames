// Controller motion from GameController (GCMotion) to the host's six-axis sensor (dm_motion_set, hle_hid.c). Shared by the Mac front end (front_mac.m) and the iOS gamepad
// (DmGamepad.m). A controller with a gyroscope (DualSense, DualShock 4, Switch Pro, Joy-Con) has its sensors switched on when it is given a player, and every motion update is
// passed on with the total acceleration (G, gravity direction as CoreMotion: (0, 0, -1) face up) and the rotation rate (rad/s); a controller without one does nothing.
// DM_MOTION=0 leaves the sensors off. GCMotion's acceleration / hasRotationRate / manual activation need macOS 11.3 / iOS 14.5, below both platforms' targets.
#pragma once
#import <GameController/GameController.h>
#include <stdlib.h>
#include "dm_input.h"

static inline void dm_motion_attach(GCController *c, int player) {
    const char *e = getenv("DM_MOTION"); if (e && !atoi(e)) return;
    GCMotion *mo = c.motion;
    if (!mo || !mo.hasRotationRate) return;
    if (mo.sensorsRequireManualActivation) mo.sensorsActive = YES;   // DualSense / DualShock 4: off until asked for
    __block int live = 0;
    mo.valueChangedHandler = ^(GCMotion *m) {
        float acc[3], rot[3];
        if (m.hasGravityAndUserAcceleration) {
            acc[0] = (float)(m.gravity.x + m.userAcceleration.x); acc[1] = (float)(m.gravity.y + m.userAcceleration.y); acc[2] = (float)(m.gravity.z + m.userAcceleration.z);
        } else { acc[0] = (float)m.acceleration.x; acc[1] = (float)m.acceleration.y; acc[2] = (float)m.acceleration.z; }
        rot[0] = (float)m.rotationRate.x; rot[1] = (float)m.rotationRate.y; rot[2] = (float)m.rotationRate.z;
        // Some macOS controller drivers advertise motion but send only zeroes (#296). Those are no source until data arrives; later zeroes can be a real free-fall sample.
        if (!live) {
            if (acc[0] == 0 && acc[1] == 0 && acc[2] == 0 && rot[0] == 0 && rot[1] == 0 && rot[2] == 0) return;
            live = 1;
        }
        dm_motion_set(player, acc, rot);
    };
}
// iOS turns a manual-activation controller's sensors off while the app is in the background (Control Center, the home screen) and leaves them off, so the gyro stayed dead until the
// controller reconnected (found by MacNativeGames on the iPad). Called when the app becomes active again: only controllers attached above (their handler is set)
static inline void dm_motion_rearm(GCController *c) {
    GCMotion *mo = c.motion;
    if (mo && mo.valueChangedHandler && mo.sensorsRequireManualActivation) mo.sensorsActive = YES;
}
static inline void dm_motion_detach(GCController *c, int player) {
    GCMotion *mo = c.motion;
    if (mo) { mo.valueChangedHandler = nil; if (mo.sensorsRequireManualActivation) mo.sensorsActive = NO; }
    dm_motion_clear(player);
}
