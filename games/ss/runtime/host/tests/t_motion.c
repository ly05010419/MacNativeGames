// Controller motion fusion (motion_fusion.h): axis maps, units, orientation from the gyro, tilt correction toward gravity, and the SixAxisSensorState fields. No host, no controller
#include "../motion_fusion.h"
#include <stdio.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

static void run(motion_state *s, const motion_map *m, float ax, float ay, float az, float wx, float wy, float wz, float secs, float k) {
    float a[3] = {ax, ay, az}, w[3] = {wx, wy, wz};
    int n = (int)(secs * 250 + 0.5f);
    for (int i = 0; i < n; i++) motion_update(s, m, a, w, 1 / 250.f, k);
}

int main(void) {
    motion_map m;
    CHECK(motion_map_parse(&m, NULL) && m.src[0] == 0 && m.det == 1, "default map is the identity");
    CHECK(motion_map_parse(&m, "x,-z,y") && m.src[1] == 2 && m.sign[1] == -1 && m.det == 1, "x,-z,y is a proper rotation");
    CHECK(motion_map_parse(&m, "y,x,z") && m.det == -1, "a swap is a mirror");
    CHECK(motion_map_parse(&m, "-x, y, z") && m.det == -1, "one sign flip is a mirror");
    CHECK(!motion_map_parse(&m, "x,x,z") && m.det == 1, "a repeated axis is refused (identity)");
    CHECK(!motion_map_parse(&m, "x,y") , "two entries are refused");

    // flat and still: identity orientation, acc (0,0,-1), no rotation
    motion_map id; motion_map_parse(&id, NULL);
    motion_state s; motion_reset(&s);
    run(&s, &id, 0, 0, -1, 0, 0, 0, 1, 0.5f);
    float acc[3], gyro[3], ang[3], dir[9];
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(acc[2], -1, 1e-5f) && near(dir[0], 1, 1e-4f) && near(dir[4], 1, 1e-4f) && near(dir[8], 1, 1e-4f), "flat: identity direction");

    // a quarter turn of yaw over one second (pi/2 rad/s): gyro 0.25 rev/s, angle 0.25 rev, the controller's x axis now points along world y (as the old shake's convention)
    motion_reset(&s);
    run(&s, &id, 0, 0, -1, 0, 0, (float)M_PI / 2, 1, 0.5f);
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(gyro[2], 0.25f, 1e-4f), "gyro in revolutions / s");
    CHECK(near(ang[2], 0.25f, 0.01f), "angle integrates to a quarter revolution");
    CHECK(near(dir[0], 0, 0.02f) && near(dir[1], 1, 0.02f), "x axis turned to world y");

    // pitch: the top raised 30 degrees about x, with gravity measured to match: stays there (correction agrees with the gyro)
    motion_reset(&s);
    float th = 30 * (float)M_PI / 180;
    run(&s, &id, 0, 0, -1, 0, 0, 0, 0.1f, 0.5f);
    run(&s, &id, 0, 0, -1, th, 0, 0, 1, 0);          // turn by the gyro alone (no correction while the accelerometer lags)
    run(&s, &id, 0, -sinf(th), -cosf(th), 0, 0, 0, 2, 0.5f);   // then still, gravity seen tilted in the body
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(dir[5], sinf(th), 0.02f) && near(dir[4], cosf(th), 0.02f), "y axis raised 30 degrees");

    // tilt drift: the gyro says nothing while gravity shows a 20-degree roll: the correction pulls the orientation there
    motion_reset(&s);
    run(&s, &id, 0, 0, -1, 0, 0, 0, 0.1f, 0.5f);
    float r = 20 * (float)M_PI / 180;
    run(&s, &id, sinf(r), 0, -cosf(r), 0, 0, 0, 20, 0.5f);
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(-dir[2], sinf(r), 0.03f), "roll pulled toward gravity");

    // a source that starts tilted is primed level with its gravity
    motion_reset(&s);
    run(&s, &id, sinf(r), 0, -cosf(r), 0, 0, 0, 0.01f, 0.5f);
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(-dir[2], sinf(r), 0.02f), "primed from the first sample's gravity");

    // A phone held upright starts with gravity on -y, parallel to the old heading reference.
    // All six axis-aligned poses must map their measured gravity to world down on the first sample.
    for (int axis = 0; axis < 3; axis++) for (int sign = -1; sign <= 1; sign += 2) {
        float a[3] = {0}, w[3] = {0}; a[axis] = (float)sign;
        motion_reset(&s); motion_update(&s, &id, a, w, 0, 0.5f);
        CHECK(near(s.R[2][axis] * sign, -1, 1e-5f), "first sample: gravity points down in every starting pose");
    }

    // a frame map: Apple reports rotation about its y where the Switch has z (map "x,-z,y": switch y = -apple z, switch z = apple y)
    motion_map fm; motion_map_parse(&fm, "x,-z,y");
    motion_reset(&s);
    run(&s, &fm, 0, -1, 0, 0, (float)M_PI / 2, 0, 1, 0.5f);   // Apple: gravity along -y, rotating about +y
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(acc[2], -1, 1e-4f) && near(gyro[2], 0.25f, 1e-4f), "mapped: gravity on -z, yaw on z");
    CHECK(near(dir[1], 1, 0.02f), "mapped yaw turns x to world y");

    // the specific-force convention: +1 G up at rest becomes gravity down with DM_MOTION_ACC_SIGN=-1
    motion_map fs; motion_map_parse(&fs, NULL); fs.acc_sign = -1;
    motion_reset(&s);
    run(&s, &fs, 0, 0, 1, 0, 0, 0, 0.2f, 0.5f);
    motion_fill(&s, acc, gyro, ang, dir);
    CHECK(near(acc[2], -1, 1e-5f) && near(dir[8], 1, 1e-4f), "acc sign flip");

    // orthonormal after a long noisy spin
    motion_reset(&s);
    for (int i = 0; i < 25000; i++) { float a[3] = {0, 0, -1}, w[3] = {0.7f * sinf(i * 0.01f), 1.3f, -0.4f}; motion_update(&s, &id, a, w, 1 / 250.f, 0.5f); }
    float d01 = s.R[0][0] * s.R[0][1] + s.R[1][0] * s.R[1][1] + s.R[2][0] * s.R[2][1], n0 = s.R[0][0] * s.R[0][0] + s.R[1][0] * s.R[1][0] + s.R[2][0] * s.R[2][0];
    CHECK(near(d01, 0, 1e-4f) && near(n0, 1, 1e-4f), "orientation stays orthonormal");

    // the phone's own sensors: held upright in landscape, screen toward the player, gravity is toward the bottom of the picture (Switch -y) whichever way it is turned
    float up3[3] = {-1, 0, 0}, up4[3] = {1, 0, 0}, flat[3] = {0, 0, -1}, o[3];
    motion_phone_to_switch(3, up3, o); CHECK(near(o[0], 0, 1e-6f) && near(o[1], -1, 1e-6f) && near(o[2], 0, 1e-6f), "landscape right upright: gravity to the picture's bottom");
    motion_phone_to_switch(4, up4, o); CHECK(near(o[0], 0, 1e-6f) && near(o[1], -1, 1e-6f), "landscape left upright: gravity to the picture's bottom");
    motion_phone_to_switch(3, flat, o); CHECK(near(o[2], -1, 1e-6f), "flat face up stays face up");
    // turning the phone (landscape right) about its own long side, which is the picture's x axis: rotation about phone -y becomes rotation about Switch +x
    float rl[3] = {0, -1, 0}; motion_phone_to_switch(3, rl, o); CHECK(near(o[0], 1, 1e-6f) && near(o[1], 0, 1e-6f), "rotation axes follow the picture");

    printf("t_motion: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
