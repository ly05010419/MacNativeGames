// Exercise the shared GameController adapter with fake sensors; no physical controller is needed.
#import "../motion_apple.h"
#include <assert.h>
#include <stdio.h>

@interface TestMotion : NSObject
@property BOOL hasRotationRate, hasGravityAndUserAcceleration, sensorsRequireManualActivation, sensorsActive;
@property GCAcceleration acceleration, gravity, userAcceleration;
@property GCRotationRate rotationRate;
@property(copy) GCMotionValueChangedHandler valueChangedHandler;
@end
@implementation TestMotion
@end
@interface TestController : NSObject
@property TestMotion *motion;
@end
@implementation TestController
@end

static int samples, cleared = -1, player;
static float acc[3], rot[3];
void dm_motion_set(int p, const float a[3], const float w[3]) {
    samples++; player = p;
    for (int i = 0; i < 3; i++) { acc[i] = a[i]; rot[i] = w[i]; }
}
void dm_motion_clear(int p) { cleared = p; }

int main(void) { @autoreleasepool {
    TestController *c = [TestController new]; TestMotion *m = [TestMotion new]; c.motion = m;
    m.hasRotationRate = YES; m.sensorsRequireManualActivation = YES;
    setenv("DM_MOTION", "0", 1); dm_motion_attach((id)c, 2);
    assert(!m.sensorsActive && !m.valueChangedHandler);
    unsetenv("DM_MOTION"); dm_motion_attach((id)c, 2);
    assert(m.sensorsActive && m.valueChangedHandler);
    m.valueChangedHandler((id)m);
    assert(!samples);   // An advertised sensor with zero-only reports must not suppress the phone or the flat pose.
    m.hasGravityAndUserAcceleration = YES;
    m.gravity = (GCAcceleration){0, 0, -1}; m.userAcceleration = (GCAcceleration){0.25, 0, 0};
    m.rotationRate = (GCRotationRate){0, 1, 0}; m.valueChangedHandler((id)m);
    assert(samples == 1 && player == 2 && acc[0] == 0.25f && acc[2] == -1 && rot[1] == 1);
    m.hasGravityAndUserAcceleration = NO; m.rotationRate = (GCRotationRate){0};
    m.valueChangedHandler((id)m);
    assert(samples == 2 && acc[2] == 0);   // Once real data arrives, a zero acceleration can be free fall.
    dm_motion_detach((id)c, 2);
    assert(!m.sensorsActive && !m.valueChangedHandler && cleared == 2);
    puts("t_motion_apple: passed");
    return 0;
} }
