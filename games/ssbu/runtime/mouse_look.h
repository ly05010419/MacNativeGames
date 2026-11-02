/* Mouse movement as the right stick and as the controller's gyro (Mac mouse mode, key 7: runtime/gamepad.m).
   The idea is yuzu's "mouse panning" (written anew here, no code taken): every movement adds to a held stick deflection;
   a tick every 10 ms (MOUSE_LOOK_TICK_NS) shows the deflection to the game and then lets it decay, so the camera keeps
   turning while the mouse moves and settles smoothly when it stops, whatever the mouse's report rate and the game's
   frame rate. The decay grows with the deflection (1 - length^2 * decay%), but is at least min_decay% a tick; the held
   length is cut to 1.5 (it may run past the stick's edge, which makes a steady turn easier to hold) and shown at most 1.
   A movement that leaves a deflection shorter than counterweight% is lifted to it, past the game's own stick dead zone.
   Units: one point of mouse movement (AppKit's deltaX / deltaY; deltaY grows downwards) moves the stick by
   sensitivity / 1000 of its range: at the default 50, 20 points is a full deflection.
   The same movement also turns a controller lying face up (x right, y top edge, z out of the face: runtime/
   motion_fusion.c's frame): mouse right turns it clockwise seen from above (-z), mouse up tilts its top edge up (+x);
   per tick, sensitivity / 1000 * 2.5 revolutions a second per point, at most 2 revolutions a second. The gravity it
   reports follows the pitch (stopped at 80 degrees), so the sensor fusion keeps the aim where the mouse put it.
   Plain C, no clock and no lock: the caller ticks it (gamepad.m, one timer while mouse mode is on) and serializes the
   calls; tests/mouse_look/probe.c covers it. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

enum { MOUSE_LOOK_TICK_NS = 10000000 };
#define MOUSE_LOOK_MAX_LENGTH 1.5
#define MOUSE_LOOK_GYRO_GAIN 2.5        /* revolutions a second, per unit of stick movement in one tick */
#define MOUSE_LOOK_MAX_REV_S 2.0
#define MOUSE_LOOK_MAX_PITCH (80 * M_PI / 180)

typedef struct { int sensitivity, invert_y, counterweight, decay, min_decay; } MouseLookSettings;
typedef struct { double x, y; double pitch_move, yaw_move; double pitch; } MouseLook;   /* stick in units of its range */

/* From the environment's text (NULL or out of range: the default). sensitivity 1-100 (50), invert_y 0/1 (0),
   counterweight / decay / min_decay 0-100 (20, 18, 6). */
static inline int mouse_look_setting(const char *text, int lo, int hi, int fallback) {
    if (!text || !*text) return fallback;
    char *end; long v = strtol(text, &end, 10);
    return *end || v < lo || v > hi ? fallback : (int)v;
}
static inline MouseLookSettings mouse_look_settings(const char *sensitivity, const char *invert_y, const char *counterweight,
                                                     const char *decay, const char *min_decay) {
    return (MouseLookSettings){
        .sensitivity = mouse_look_setting(sensitivity, 1, 100, 50), .invert_y = mouse_look_setting(invert_y, 0, 1, 0),
        .counterweight = mouse_look_setting(counterweight, 0, 100, 20), .decay = mouse_look_setting(decay, 0, 100, 18),
        .min_decay = mouse_look_setting(min_decay, 0, 100, 6)};
}

static inline void mouse_look_reset(MouseLook *m) { *m = (MouseLook){0}; }

static inline void mouse_look_move(MouseLook *m, double dx, double dy, const MouseLookSettings *s) {
    if (!isfinite(dx) || !isfinite(dy)) return;
    const double kx = s->sensitivity / 1000.0, ky = s->invert_y ? -kx : kx;
    m->x += dx * kx; m->y += dy * ky;
    m->yaw_move -= dx * kx; m->pitch_move -= dy * ky;
    const double length = hypot(m->x, m->y), least = s->counterweight / 100.0;
    if (length > 0 && length < least) { m->x *= least / length; m->y *= least / length; }
}

/* One tick: the stick to show (rx, ry in -32767..32767, up positive), the gyro in rad/s and the gravity in G for this
   tick (a controller lying face up reads z = -1 G); then the held deflection decays. */
static inline void mouse_look_tick(MouseLook *m, const MouseLookSettings *s, int32_t *rx, int32_t *ry, double gyro[3], double acc[3]) {
    double length = hypot(m->x, m->y);
    if (length > MOUSE_LOOK_MAX_LENGTH) { m->x *= MOUSE_LOOK_MAX_LENGTH / length; m->y *= MOUSE_LOOK_MAX_LENGTH / length; length = MOUSE_LOOK_MAX_LENGTH; }
    double ox = m->x, oy = -m->y, shown = hypot(ox, oy);
    if (shown > 1) { ox /= shown; oy /= shown; }
    *rx = (int32_t)lround(ox * 32767); *ry = (int32_t)lround(oy * 32767);
    const double c = length < 1 ? length : 1, decay = 1 - c * c * s->decay / 100.0, most = 1 - s->min_decay / 100.0;
    const double keep = decay < most ? decay : most;
    m->x *= keep > 0 ? keep : 0; m->y *= keep > 0 ? keep : 0;
    if (fabs(m->x) < 1e-6 && fabs(m->y) < 1e-6) m->x = m->y = 0;

    double pitch = m->pitch_move * MOUSE_LOOK_GYRO_GAIN, yaw = m->yaw_move * MOUSE_LOOK_GYRO_GAIN, rate = hypot(pitch, yaw);
    if (rate > MOUSE_LOOK_MAX_REV_S) { pitch *= MOUSE_LOOK_MAX_REV_S / rate; yaw *= MOUSE_LOOK_MAX_REV_S / rate; }
    m->pitch_move = m->yaw_move = 0;
    gyro[0] = pitch * 2 * M_PI; gyro[1] = 0; gyro[2] = yaw * 2 * M_PI;
    m->pitch += gyro[0] * (MOUSE_LOOK_TICK_NS / 1e9);
    if (m->pitch > MOUSE_LOOK_MAX_PITCH) m->pitch = MOUSE_LOOK_MAX_PITCH;
    if (m->pitch < -MOUSE_LOOK_MAX_PITCH) m->pitch = -MOUSE_LOOK_MAX_PITCH;
    acc[0] = 0; acc[1] = -sin(m->pitch); acc[2] = -cos(m->pitch);
}
