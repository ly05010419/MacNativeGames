/* Test tilt mode (games/totk/runtime/gyro_tilt.h): the virtual controller's tilt, turn and reported rates, the LB + LT
   chord and when tilt mode is allowed. */
#define _DEFAULT_SOURCE   /* M_PI */
#include "../games/totk/runtime/gyro_tilt.h"
#include <stdio.h>
#include <string.h>

static int failures;
static void expect(int ok, const char *what) { if (!ok) { fprintf(stderr, "FAIL: %s\n", what); failures++; } }
static int near(double a, double b, double eps) { return fabs(a - b) < eps; }

/* The pose the reported rate turns old into over dt (Rodrigues), against the pose gyro_tilt_matrix gives. */
static double rate_error(const GyroTilt *before, const GyroTilt *after, const double w[3], double dt) {
    double old[3][3], now[3][3], r[3][3], q[3][3], err = 0;
    gyro_tilt_matrix(before, old); gyro_tilt_matrix(after, now);
    double a = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * dt, k[3] = {0, 0, 0};
    if (a > 0) for (int i = 0; i < 3; i++) k[i] = w[i] * dt / a;
    double K[3][3] = {{0, -k[2], k[1]}, {k[2], 0, -k[0]}, {-k[1], k[0], 0}}, KK[3][3];
    gyro_tilt_mul(K, K, KK);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[i][j] = (i == j) + sin(a) * K[i][j] + (1 - cos(a)) * KK[i][j];
    gyro_tilt_mul(old, r, q);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) err = fmax(err, fabs(q[i][j] - now[i][j]));
    return err;
}

/* n ticks of 10 ms; returns the worst rate error and leaves the last gyro and acc. */
static double run(GyroTilt *t, int n, int32_t lx, int32_t ly, int32_t rx, double gyro[3], double acc[3]) {
    double worst = 0;
    for (int i = 0; i < n; i++) {
        GyroTilt before = *t;
        gyro_tilt_tick(t, lx, ly, rx, 0.01, NULL, gyro, acc);
        worst = fmax(worst, rate_error(&before, t, gyro, 0.01));
    }
    return worst;
}

/* runtime/motion_fusion.c as tilt mode sees it: each sample's rate is held until the next sample, at most 50 ms after it,
   and integrated in the body frame. hold_ms[i] is the gap after tick i (a late timer tick: a long gap). Returns the angle
   in degrees between the pose shown at the end and the virtual controller's. */
static double fusion_error(int closed, const int *hold_ms, int ticks, int32_t ly_until, int ly_ticks) {
    GyroTilt t = {0};
    double R[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, g[3], a[3];
    for (int i = 0; i < ticks; i++) {
        double dt = i ? hold_ms[i - 1] / 1000.0 : 0.01;
        gyro_tilt_tick(&t, 0, i < ly_ticks ? ly_until : 0, i < ly_ticks ? 20000 : 0, dt, closed ? (const double (*)[3])R : NULL, g, a);
        double held = fmin(hold_ms[i], 50) / 1000.0, w = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]) * held, k[3] = {0, 0, 0}, E[3][3], KK[3][3], out[3][3];
        if (w > 0) for (int j = 0; j < 3; j++) k[j] = g[j] * held / w;
        double K[3][3] = {{0, -k[2], k[1]}, {k[2], 0, -k[0]}, {-k[1], k[0], 0}};
        gyro_tilt_mul(K, K, KK);
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) E[r][c] = (r == c) + sin(w) * K[r][c] + (1 - cos(w)) * KK[r][c];
        gyro_tilt_mul(R, E, out); memcpy(R, out, sizeof R);
    }
    double q[3][3], e = 0; gyro_tilt_matrix(&t, q);
    for (int k = 0; k < 3; k++) e += R[k][0] * q[k][0] + R[k][1] * q[k][1] + R[k][2] * q[k][2];
    return acos(fmax(-1, fmin(1, (e - 1) / 2))) * 180 / M_PI;
}

/* The height (reference z) of the controller axis that pointed at reference direction d in pose `before`, in pose `after`. */
static double height_of(const GyroTilt *before, const GyroTilt *after, const double d[3]) {
    double b[3][3], q[3][3], v[3] = {0, 0, 0}, z = 0; gyro_tilt_matrix(before, b); gyro_tilt_matrix(after, q);
    for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) v[i] += b[k][i] * d[k];   /* b^T d */
    for (int i = 0; i < 3; i++) z += q[2][i] * v[i];
    return z;
}

int main(void) {
    double g[3], a[3], deg = M_PI / 180, far[3] = {0, 1, 0}, right[3] = {1, 0, 0};
    GyroTilt t = {0}, start;
    run(&t, 10, 0, 0, 0, g, a);
    expect(g[0] == 0 && g[1] == 0 && g[2] == 0 && near(a[2], -1, 1e-12) && near(a[0], 0, 1e-12), "at rest: flat, no rate, z = -1 G");

    /* Left stick up for a second: the far edge goes down at TILT_SPEED_DEG; let go: it stays; up again: on from there. */
    start = t;
    expect(run(&t, 100, 0, 32767, 0, g, a) < 1e-9, "tilt: the rate turns the old pose into the new one");
    expect(near(height_of(&start, &t, far), -sin(TILT_SPEED_DEG * deg), 1e-9), "a second of stick up: the far edge 60 degrees down");
    GyroTilt held = t;
    run(&t, 50, 0, 0, 0, g, a);
    expect(g[0] == 0 && g[1] == 0 && g[2] == 0 && !memcmp(&held, &t, sizeof t), "let go: it stays where it is, no rate");
    run(&t, 200, 0, 32767, 0, g, a);
    expect(near(a[2], 1, 1e-9), "up again for 2 s: on past its edge to face down (180 degrees)");

    /* Every axis is fixed to the player: turned a quarter, a half, or tilted, stick up still tips the far edge down and
       stick right the right edge, and the right stick turns about the vertical. */
    for (int k = 1; k <= 2; k++) {
        t = (GyroTilt){0};
        run(&t, 150 * k, 0, 0, 32767, g, a);   /* turned 90 / 180 degrees clockwise */
        start = t;
        run(&t, 50, 0, 32767, 0, g, a);   /* 30 degrees (0.5 s at 60 deg/s) */
        expect(height_of(&start, &t, far) < -0.49, k == 1 ? "a quarter turn, then stick up: the far edge goes down" : "a half turn, then stick up: the far edge goes down");
        start = t;
        run(&t, 67, 32767, 0, 0, g, a);
        expect(height_of(&start, &t, right) < -0.49, "tilted and turned, then stick right: the right edge goes down");
    }
    t = (GyroTilt){0};
    run(&t, 100, 0, 32767, 0, g, a);   /* tilted 60 degrees */
    double before_acc[3] = {a[0], a[1], a[2]}, q[3][3], world_rate[3] = {0, 0, 0};
    expect(run(&t, 100, 0, 0, 32767, g, a) < 1e-9, "turning while tilted: the rate matches the pose");
    expect(near(a[0], before_acc[0], 1e-9) && near(a[1], before_acc[1], 1e-9) && near(a[2], before_acc[2], 1e-9), "turning while tilted: gravity in the controller stays (a turntable)");
    gyro_tilt_matrix(&t, q);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) world_rate[i] += q[i][j] * g[j];
    expect(near(world_rate[0], 0, 1e-6) && near(world_rate[1], 0, 1e-6) && near(world_rate[2], -TILT_SPEED_DEG * deg, 1e-6), "turning: about the vertical, clockwise at full speed");

    /* The squared curve: half stick, a quarter of the speed; the sign kept. */
    t = (GyroTilt){0}; start = t;
    run(&t, 100, -16384, 0, 0, g, a);
    expect(near(height_of(&start, &t, right), sin(TILT_SPEED_DEG * deg * (16384 / 32767.0) * (16384 / 32767.0)), 1e-9), "half stick left for 1 s: the right edge 15 degrees up");

    /* All three at once, for a while: the rate always matches the pose, and the pose stays a rotation. */
    t = (GyroTilt){0};
    expect(run(&t, 500, 20000, -12000, -32767, g, a) < 1e-9, "all three at once: the rate matches the pose");
    gyro_tilt_matrix(&t, q);
    double det = q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) + q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
    expect(near(det, 1, 1e-12) && near(a[0] * a[0] + a[1] * a[1] + a[2] * a[2], 1, 1e-12), "the pose stays a rotation, gravity 1 G");

    /* LB + LT (physical 6, 8): together within 150 ms, in either order; once per press. */
    enum { LB = 1 << 6, LT = 1 << 8 }; const uint64_t ms = 1000000;
    GyroTiltChord c = {0};
    expect(!gyro_tilt_chord(&c, LB, LB, 1000 * ms) && gyro_tilt_chord(&c, LB | LT, LT, 1100 * ms), "LB then LT 100 ms later toggles");
    expect(!gyro_tilt_chord(&c, LB | LT, 0, 1200 * ms) && !gyro_tilt_chord(&c, LB | LT, 0, 3000 * ms), "a held pair toggles once");
    expect(!gyro_tilt_chord(&c, LB, 0, 3100 * ms) && !gyro_tilt_chord(&c, LB | LT, LT, 3200 * ms), "LB still held from the pair, LT let go and pressed again: no toggle");
    c = (GyroTiltChord){0};
    expect(!gyro_tilt_chord(&c, LT, LT, 5000 * ms) && gyro_tilt_chord(&c, LB | LT, LB, 5150 * ms), "LT then LB at exactly 150 ms toggles");
    c = (GyroTiltChord){0};
    expect(!gyro_tilt_chord(&c, LB, LB, 7000 * ms) && !gyro_tilt_chord(&c, LB | LT, LT, 7151 * ms), "151 ms apart: no toggle");
    c = (GyroTiltChord){0};
    expect(!gyro_tilt_chord(&c, LB, LB, 9000 * ms) && !gyro_tilt_chord(&c, LB | LT, LT, 11000 * ms), "left held, right 2 s later: no toggle");
    expect(!gyro_tilt_chord(&c, 0, 0, 12000 * ms) && !gyro_tilt_chord(&c, LB, LB, 13000 * ms) && gyro_tilt_chord(&c, LB | LT, LT, 13050 * ms),
           "after both are let go, a new pair toggles");
    c = (GyroTiltChord){0};
    expect(gyro_tilt_chord(&c, LB | LT, LB | LT, 15000 * ms), "both in one report toggle");
    { GyroTiltChord t = {0}; expect(!gyro_tilt_chord(&t, 1 << 8 | 1 << 9, 1 << 8 | 1 << 9, 16000 * ms), "LT + RT no longer toggles (owner, 2026-10-30)"); }
    { GyroTiltChord t = {0}; expect(!gyro_tilt_chord(&t, 1 << 12 | 1 << 14, 1 << 12 | 1 << 14, 17000 * ms), "D-pad left + right does not toggle"); }

    /* When the pair may turn it on. */
    expect(gyro_tilt_allowed(NULL, 1, 0) && !gyro_tilt_allowed(NULL, 1, 1), "unset: only without a controller gyro");
    expect(gyro_tilt_allowed("1", 1, 1) && !gyro_tilt_allowed("0", 1, 0), "TOTK_GYRO_EMU=1 always, =0 never");
    expect(!gyro_tilt_allowed("1", 0, 0) && !gyro_tilt_allowed(NULL, 0, 0), "never without the game's six-axis device");
    expect(gyro_tilt_allowed("", 1, 0) && !gyro_tilt_allowed("yes", 1, 1), "another value counts as unset");

    /* A late timer tick (90 ms, the fusion holds 50 ms of it) while tilting and turning, then the stick let go: steering
       from the pose the sensor shows ends level and on the controller's angle; the open-loop rate would stay off (control). */
    int hold[200]; for (int i = 0; i < 200; i++) hold[i] = 10;
    hold[20] = 90; hold[35] = 3; hold[36] = 17;
    expect(fusion_error(1, hold, 200, 32767, 50) < 0.1, "a late tick: the sensor ends on the controller's pose");
    expect(fusion_error(0, hold, 200, 32767, 50) > 1, "negative control: the open-loop rate stays off after a late tick");
    /* A long run of late ticks (30 ms each, 20 ms on the first): the correction settles, it does not swing wider. */
    for (int i = 0; i < 200; i++) hold[i] = i < 100 ? 10 : 30;
    hold[100] = 20; hold[60] = 90;
    expect(fusion_error(1, hold, 200, 32767, 150) < 0.1, "late ticks in a row while turning: the sensor still ends on the pose");
    for (int i = 0; i < 200; i++) hold[i] = 10;
    expect(fusion_error(1, hold, 200, 32767, 50) < 1e-4 && fusion_error(0, hold, 200, 32767, 50) < 1e-4, "even ticks: both end on the pose");

    if (failures) return 1;
    /* B: back to flat with a real rate, the shortest way, at up to TILT_HOME_DEG a second; then exactly flat. Tilted,
       turned and tilted, and upside down (the axis-angle edge at 180 degrees). */
    for (int c = 0; c < 3; c++) {
        t = (GyroTilt){0};
        if (c == 0) run(&t, 100, 0, 32767, 0, g, a);   /* tilted 60 degrees */
        if (c == 1) { run(&t, 100, 0, 32767, 0, g, a); run(&t, 150, 0, 0, 32767, g, a); run(&t, 80, -20000, 0, 0, g, a); }
        if (c == 2) run(&t, 300, 0, 32767, 0, g, a);   /* 180 degrees: face down */
        double q0[3][3]; gyro_tilt_matrix(&t, q0);
        double cc = (q0[0][0] + q0[1][1] + q0[2][2] - 1) / 2, angle = acos(cc > 1 ? 1 : cc < -1 ? -1 : cc) * 180 / M_PI, worst = 0;
        int ticks = 0, done = 0;
        while (!done && ticks < 1000) { GyroTilt before = t; done = gyro_tilt_home(&t, 0.01, NULL, g, a); worst = fmax(worst, rate_error(&before, &t, g, 0.01)); ticks++; }
        expect(done && ticks <= (int)ceil(angle / (TILT_HOME_DEG * 0.01)) + 1, "B: back to flat in time at TILT_HOME_DEG");
        expect(worst < 1e-6, "B: every step's rate matches the move (the game's own pose follows)");
        expect(!t.set && near(a[2], -1, 1e-6), "B: ends exactly flat, z = -1 G");
    }

    /* tilt.txt: one of three speeds, one axis or all reversed; a bad line keeps the default and is counted. */
    {
        GyroTiltConfig c;
        expect(gyro_tilt_config("", &c) == 0 && c.speed == 60 && !c.invert, "tilt.txt: empty: the defaults");
        expect(gyro_tilt_config("# faster, all reversed\nspeed = 90\ninvert = all\n", &c) == 0 && c.speed == 90 && c.invert == TILT_INVERT_ALL, "tilt.txt: speed 90, all reversed");
        expect(gyro_tilt_config("speed = 180\n", &c) == 0 && c.speed == 180 && gyro_tilt_config("speed = 360\n", &c) == 0 && c.speed == 360, "tilt.txt: speed 180 and 360");
        expect(gyro_tilt_config("invert = ly\n", &c) == 0 && c.invert == TILT_INVERT_LY && gyro_tilt_config("invert = rx\n", &c) == 0 && c.invert == TILT_INVERT_RX, "tilt.txt: one axis reversed");
        expect(gyro_tilt_config("speed=60\r\ninvert=off\r\n", &c) == 0 && c.speed == 60 && !c.invert, "tilt.txt: no spaces, CRLF");
        expect(gyro_tilt_config("speed = 30\nspeed = 45\nspeed = 090\ninvert = on\ninvert = lx ly\ncolour = red\n", &c) == 6 && c.speed == 60 && !c.invert,
               "tilt.txt: other speeds, on, two axes, an unknown key: six bad lines, defaults kept");
        char text[64]; GyroTiltConfig w = {60, TILT_INVERT_LX}, r;
        gyro_tilt_config_text(&w, text, sizeof text);
        expect(gyro_tilt_config(text, &r) == 0 && r.speed == 60 && r.invert == TILT_INVERT_LX, "tilt.txt: what the panel saves reads back the same");
        GyroTilt a1 = {0}, a2 = {0}; double g2[3], acc2[3];
        for (int i = 0; i < 100; i++) { gyro_tilt_tick_at(&a1, 32767, 0, 0, 90, 0.01, NULL, g, a); gyro_tilt_tick(&a2, 32767, 0, 0, 0.01, NULL, g2, acc2); }
        GyroTilt flat = {0};
        expect(near(height_of(&flat, &a1, right), -sin(90 * deg), 1e-9) && near(height_of(&flat, &a2, right), -sin(TILT_SPEED_DEG * deg), 1e-9), "tilt speed 90: a second of full stick turns 90 degrees");
    }

    if (failures) { fprintf(stderr, "gyro tilt: %d failure(s)\n", failures); return 1; }
    puts("gyro tilt: ok");
    return 0;
}
