// runtime/mouse_look.h: mouse movement as a right stick that decays (the yuzu-style "mouse panning" idea) and as a gyro.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "mouse_look.h"

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }
static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

typedef struct { int32_t rx, ry; double gyro[3], acc[3]; } Tick;
static Tick tick(MouseLook *m, const MouseLookSettings *s) { Tick t; mouse_look_tick(m, s, &t.rx, &t.ry, t.gyro, t.acc); return t; }

int main(void) {
    const MouseLookSettings s = mouse_look_settings(NULL, NULL, NULL, NULL, NULL);
    check(s.sensitivity == 50 && s.counterweight == 20 && s.decay == 18 && s.min_decay == 6 && !s.invert_y,
          "defaults: sensitivity 50, dead-zone counterweight 20, decay 18, minimum decay 6");
    MouseLook m = {0};
    Tick t = tick(&m, &s);
    check(t.rx == 0 && t.ry == 0 && t.gyro[0] == 0 && t.gyro[2] == 0 && near(t.acc[2], -1, 1e-9) && t.acc[0] == 0 && t.acc[1] == 0,
          "still: centred, no rotation, lying face up (z = -1 G)");

    // 10 points right at sensitivity 50: 0.5 of the stick's range, shown on the next tick, then decaying.
    mouse_look_move(&m, 10, 0, &s);
    t = tick(&m, &s);
    check(abs(t.rx - 16384) <= 1 && t.ry == 0, "10 points right: half the stick right");
    Tick u = tick(&m, &s);
    check(u.rx > 0 && u.rx < t.rx && u.rx > t.rx * 0.9, "no more movement: it decays (by 1 - 0.5^2 * 18%)");
    for (int i = 0; i < 200; i++) u = tick(&m, &s);
    check(u.rx == 0 && u.ry == 0, "and comes back to the centre (minimum decay 6% a tick)");

    // The counterweight lifts a tiny movement over the game's own dead zone; the length is held at 1.5, shown at most 1.
    mouse_look_move(&m, 0.5, 0, &s);
    t = tick(&m, &s);
    check(abs(t.rx - 6553) <= 2, "half a point: lifted to the counterweight, 20% of the range");
    MouseLook f = {0};
    mouse_look_move(&f, 0, 100, &s);
    t = tick(&f, &s);
    check(t.ry == -32767 && t.rx == 0, "100 points down: the stick fully down (AppKit's deltaY grows downwards)");
    check(near(hypot(f.x, f.y), 1.5 * (1 - 0.18), 1e-9), "the held length is cut to 1.5, then decays with the full 18%");
    MouseLook d = {0};
    mouse_look_move(&d, 100, -100, &s);
    t = tick(&d, &s);
    check(t.rx == t.ry && t.rx > 23000 && hypot(t.rx, t.ry) <= 32767.5, "a fast diagonal: up-right on the stick's circle");

    // Steady movement: equilibrium where the decay takes out what the mouse adds.
    MouseLook e = {0};
    for (int i = 0; i < 100; i++) { mouse_look_move(&e, 1, 0, &s); t = tick(&e, &s); }
    check(t.rx > 6553 && t.rx < 32767, "slow steady movement (100 pt/s): a steady partial turn");

    // Sensitivity and inverted Y.
    MouseLookSettings inv = mouse_look_settings("100", "1", NULL, NULL, NULL);
    check(inv.sensitivity == 100 && inv.invert_y == 1, "settings from the environment");
    MouseLook v = {0};
    mouse_look_move(&v, 0, 5, &inv);
    t = tick(&v, &inv);
    check(abs(t.ry - 16384) <= 1 && t.rx == 0, "sensitivity 100 doubles; inverted Y turns down into up");

    // Gyro: per tick, yaw = -dx and pitch = -dy (points * sensitivity / 1000 * 2.5 rev/s), capped at 2 rev/s.
    MouseLook g = {0};
    mouse_look_move(&g, 4, -2, &s);
    t = tick(&g, &s);
    check(near(t.gyro[2], -4 * 0.05 * 2.5 * 2 * M_PI, 1e-6) && near(t.gyro[0], 2 * 0.05 * 2.5 * 2 * M_PI, 1e-6) && t.gyro[1] == 0,
          "mouse right turns the controller clockwise seen from above (-z); mouse up tilts its top edge up (+x)");
    check(near(t.acc[1], -sin(t.gyro[0] * 0.01), 1e-9) && near(t.acc[2], -cos(t.gyro[0] * 0.01), 1e-9),
          "gravity follows the pitch, so the sensor fusion does not pull the aim back");
    t = tick(&g, &s);
    check(t.gyro[0] == 0 && t.gyro[2] == 0, "the gyro reads only the movement of its own tick");
    mouse_look_move(&g, 1000, 0, &s);
    t = tick(&g, &s);
    check(near(fabs(t.gyro[2]), 2 * 2 * M_PI, 1e-6), "a very fast turn: capped at 2 revolutions a second");
    MouseLook p = {0};
    for (int i = 0; i < 100; i++) { mouse_look_move(&p, 0, -1000, &s); tick(&p, &s); }
    check(near(p.pitch, 80 * M_PI / 180, 1e-9), "the pitch used for gravity stops at 80 degrees");

    // Junk, reset and out-of-range settings.
    MouseLook j = {0};
    mouse_look_move(&j, NAN, 3, &s); mouse_look_move(&j, INFINITY, 0, &s);
    t = tick(&j, &s);
    check(t.rx == 0 && t.ry == 0 && t.gyro[2] == 0, "non-finite movement is dropped");
    mouse_look_move(&j, 50, 50, &s); mouse_look_reset(&j);
    t = tick(&j, &s);
    check(t.rx == 0 && t.ry == 0 && t.gyro[0] == 0 && near(t.acc[2], -1, 1e-9), "reset: centred and lying flat again");
    MouseLookSettings bad = mouse_look_settings("0", "2", "101", "x", "-1");
    check(bad.sensitivity == 50 && bad.invert_y == 0 && bad.counterweight == 20 && bad.decay == 18 && bad.min_decay == 6,
          "out-of-range settings keep the defaults");

    printf("{\"status\":\"%s\",\"probe\":\"mouse-look\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
    return failures != 0;
}
