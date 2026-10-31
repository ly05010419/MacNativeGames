/* Tilt mode: a controller without motion sensors (an Xbox pad) tilts and turns a virtual controller lying face up, for the
   motion-control puzzles (Breath of the Wild: Myahm Agana's maze board, Toto Sah's stairs). LB + LT (Switch L + ZL) pressed
   together turn it on and off (gyro_tilt_chord); runtime/gamepad.m ticks it and feeds the six-axis sensor.
   Every stick axis turns it at a speed, about an axis fixed to the player, never to the controller: left stick up tips the
   far edge down (about x), right tips the right edge down (about y), right stick right turns it clockwise seen from above
   (about the vertical, mouse mode's sign, runtime/mouse_look.h). Let go and it stays where it is; push again and it moves on
   from there. Speed is up to TILT_SPEED_DEG per second on a squared curve (half way: a quarter), for fine moves. Turning
   tilt mode on lays it flat. Each 10 ms tick hands the sensor fusion (runtime/motion_fusion.c) the rotation rate in the
   controller frame plus the gravity of the new pose (the fusion's frame: x right, y top edge, z out of the face; flat it
   reads z = -1 G). Pure functions, tested in tests/test_gyro_tilt.c. */
#ifndef GYRO_TILT_H
#define GYRO_TILT_H
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TILT_TICK_NS = 10000000, TILT_FIX_NS = 50000000, TILT_CHORD_NS = 150000000 };
#define TILT_RESET_NS 200000000ull   /* B: the pose holds this long, then turns back to flat (runtime/gamepad.m) */
#define TILT_HOME_DEG 720.0           /* ... at this speed (gyro_tilt_home): back within 0.45 s, before a quick re-entry */
#define TILT_SPEED_DEG 60.0   /* owner: 90 moved too far per push (2026-10-29); 60 the default (2026-10-30) */

/* The pose, controller -> reference (rows = reference axes in controller coordinates, as runtime/motion_fusion.c);
   {0} is flat. */
typedef struct { double q[3][3]; int set; } GyroTilt;

static inline void gyro_tilt_mul(const double a[3][3], const double b[3][3], double q[3][3]) {
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { q[i][j] = 0; for (int k = 0; k < 3; k++) q[i][j] += a[i][k] * b[k][j]; }
}

static inline void gyro_tilt_matrix(const GyroTilt *t, double q[3][3]) {
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) q[i][j] = t->set ? t->q[i][j] : i == j;
}

/* The body-frame rate (rad/s) that turns pose `from` into pose `to` over dt. */
static inline void gyro_tilt_rate(const double from[3][3], const double to[3][3], double dt, double gyro[3]) {
    double e[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { e[i][j] = 0; for (int k = 0; k < 3; k++) e[i][j] += from[k][i] * to[k][j]; }
    double c = (e[0][0] + e[1][1] + e[2][2] - 1) / 2, angle = acos(c > 1 ? 1 : c < -1 ? -1 : c), s = sin(angle);
    double axis[3] = {e[2][1] - e[1][2], e[0][2] - e[2][0], e[1][0] - e[0][1]};
    for (int i = 0; i < 3; i++) gyro[i] = angle < 1e-9 ? 0 : axis[i] / (2 * s) * angle / dt;
}

/* Turn *t at w (rad/s, the reference frame) over dt; the rate and gravity as gyro_tilt_tick says. */
static inline void gyro_tilt_turn(GyroTilt *t, const double w[3], double dt, const double (*from)[3], double gyro[3], double acc[3]) {
    double old[3][3], now[3][3];
    gyro_tilt_matrix(t, old);
    double a = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * dt, k[3] = {0, 0, 0}, kk[3][3], r[3][3];
    if (a > 0) for (int i = 0; i < 3; i++) k[i] = w[i] * dt / a;
    double K[3][3] = {{0, -k[2], k[1]}, {k[2], 0, -k[0]}, {-k[1], k[0], 0}};
    gyro_tilt_mul(K, K, kk);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[i][j] = (i == j) + sin(a) * K[i][j] + (1 - cos(a)) * kk[i][j];
    gyro_tilt_mul(r, old, now);   /* on the left: about the reference's (the player's) axes */
    for (int i = 0; i < 3; i++) {   /* keep it a rotation: Gram-Schmidt on the columns */
        for (int j = 0; j < i; j++) { double d = 0; for (int m = 0; m < 3; m++) d += now[m][i] * now[m][j]; for (int m = 0; m < 3; m++) now[m][i] -= d * now[m][j]; }
        double n = sqrt(now[0][i] * now[0][i] + now[1][i] * now[1][i] + now[2][i] * now[2][i]); for (int m = 0; m < 3; m++) now[m][i] /= n;
    }
    memcpy(t->q, now, sizeof now); t->set = 1;
    gyro_tilt_rate(old, now, dt, gyro);   /* the move itself, over the last tick's length (the best guess of the next hold) */
    if (from) {   /* plus what the sensor is off by, spread over the fusion's longest hold, so a run of late ticks cannot overshoot */
        double fix[3]; gyro_tilt_rate(from, old, TILT_FIX_NS / 1e9, fix);
        for (int i = 0; i < 3; i++) gyro[i] += fix[i];
    }
    for (int j = 0; j < 3; j++) acc[j] = -now[2][j];
}

/* One tick: turn *t by the sticks (-32767..32767 per axis, y up) over dt about the player's axes, and give the new pose's
   gravity in G and the body-frame rate: the move over dt plus the turn from `from` to the old pose over TILT_FIX_NS.
   `from` is the pose the sensor shows now (runtime/motion_fusion.c holds each rate until the next sample and drops what a
   late tick misses, so steering from what it shows keeps late ticks from leaving it off); NULL: the old pose. */
static inline void gyro_tilt_tick_at(GyroTilt *t, int32_t lx, int32_t ly, int32_t rx, double speed_deg, double dt, const double (*from)[3], double gyro[3], double acc[3]) {
    const double speed = speed_deg * M_PI / 180;
    double x = lx / 32767.0, y = ly / 32767.0, z = rx / 32767.0;
    double w[3] = {-speed * y * fabs(y), speed * x * fabs(x), -speed * z * fabs(z)};   /* reference frame, rad/s; squared, sign kept */
    gyro_tilt_turn(t, w, dt, from, gyro, acc);
}
static inline void gyro_tilt_tick(GyroTilt *t, int32_t lx, int32_t ly, int32_t rx, double dt, const double (*from)[3], double gyro[3], double acc[3]) {
    gyro_tilt_tick_at(t, lx, ly, rx, TILT_SPEED_DEG, dt, from, gyro, acc);
}

/* tilt.txt next to controls.txt: the settings panel's tilt rows (runtime/settings_panel.h) write it, and runtime/gamepad.m
   reads it once at the first use. One setting a line, '#' starts a comment:
     speed = 60     top speed in degrees a second: 60 (default, TILT_SPEED_DEG), 90, 180 or 360
     invert = off   the stick axis reversed: off (default), all (all three), or one of lx, ly, rx (left stick x, left
                    stick y, right stick x)
   Returns the number of lines it could not read; those keep the default. */
enum { TILT_INVERT_LX = 1, TILT_INVERT_LY = 2, TILT_INVERT_RX = 4, TILT_INVERT_ALL = 7 };   /* GyroTiltConfig.invert */
typedef struct { int speed, invert; } GyroTiltConfig;
static inline int gyro_tilt_speed_ok(int speed) { return speed == 60 || speed == 90 || speed == 180 || speed == 360; }
static inline const char *gyro_tilt_invert_name(int invert) {   /* NULL: not a choice */
    switch (invert) { case 0: return "off"; case TILT_INVERT_LX: return "lx"; case TILT_INVERT_LY: return "ly"; case TILT_INVERT_RX: return "rx"; case TILT_INVERT_ALL: return "all"; }
    return NULL;
}
static inline int gyro_tilt_invert_ok(int invert) { return gyro_tilt_invert_name(invert) != NULL; }
static inline int gyro_tilt_config(const char *text, GyroTiltConfig *c) {
    *c = (GyroTiltConfig){(int)TILT_SPEED_DEG, 0};
    int bad = 0;
    for (const char *p = text; p && *p;) {
        char line[200]; size_t n = strcspn(p, "\n"); if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, p, n); line[n] = 0; p += strcspn(p, "\n"); if (*p) p++;
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char key[32], value[32] = "", extra[2] = "";
        int got = sscanf(line, " %31[a-z] = %31[a-z0-9] %1s", key, value, extra);
        if (got < 1) { for (char *q = line; *q; q++) if (*q != ' ' && *q != '\t' && *q != '\r') { bad++; break; } continue; }
        if (got != 2) bad++;
        else if (!strcmp(key, "speed") && gyro_tilt_speed_ok(atoi(value)) && strlen(value) == (atoi(value) >= 100 ? 3u : 2u)) c->speed = atoi(value);
        else if (!strcmp(key, "invert")) {
            int found = -1;
            for (int i = 0; i < 8; i++) if (gyro_tilt_invert_name(i) && !strcmp(value, gyro_tilt_invert_name(i))) found = i;
            if (found >= 0) c->invert = found; else bad++;
        } else bad++;
    }
    return bad;
}
/* The same two lines, as the settings panel saves them. */
static inline void gyro_tilt_config_text(const GyroTiltConfig *c, char *text, size_t size) {
    snprintf(text, size, "speed = %d\ninvert = %s\n", c->speed, gyro_tilt_invert_ok(c->invert) ? gyro_tilt_invert_name(c->invert) : "off");
}

/* One tick of the way back to flat after B: turn *t toward level, yaw 0, at up to TILT_HOME_DEG per second, about the
   axis that gets there the shortest way, with a real rate like any move. The game sums the rate into a pose of its own
   (an owner run, 2026-10-29: a jump to flat with no rate left its pose tilted, and the next entry turned about sloped
   axes), so it comes back level with ours. 1 once flat. */
static inline int gyro_tilt_home(GyroTilt *t, double dt, const double (*from)[3], double gyro[3], double acc[3]) {
    double q[3][3]; gyro_tilt_matrix(t, q);
    double c = (q[0][0] + q[1][1] + q[2][2] - 1) / 2, a = acos(c > 1 ? 1 : c < -1 ? -1 : c), k[3] = {q[2][1] - q[1][2], q[0][2] - q[2][0], q[1][0] - q[0][1]};
    double n = sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]);
    if (a > M_PI - 1e-3 || n < 1e-9) {   /* upside down (or flat): the axis is the longest column of (q + I) / 2 */
        int best = 0; double bn = -1;
        for (int j = 0; j < 3; j++) { double m = 0; for (int i = 0; i < 3; i++) m += (q[i][j] + (i == j)) * (q[i][j] + (i == j)); if (m > bn) { bn = m; best = j; } }
        for (int i = 0; i < 3; i++) k[i] = q[i][best] + (i == best);
        n = sqrt(bn);
    }
    double step = TILT_HOME_DEG * M_PI / 180 * dt, w[3] = {0, 0, 0};
    if (a > 1e-9 && n > 1e-12) for (int i = 0; i < 3; i++) w[i] = -k[i] / n * (a < step ? a : step) / dt;
    gyro_tilt_turn(t, w, dt, from, gyro, acc);
    if (a > step) return 0;
    *t = (GyroTilt){0};   /* exactly flat, as a fresh start */
    return 1;
}

/* TOTK_GYRO_EMU: whether LB + LT may turn tilt mode on. Never without the game's six-axis device (TOTK_GYRO / gyro.txt);
   "0" never, "1" always, else (unset) only while no controller feeds real motion (a DualSense or Switch Pro keeps its gyro). */
static inline int gyro_tilt_allowed(const char *env, int six_axis, int motion_live) {
    if (!six_axis || (env && !strcmp(env, "0"))) return 0;
    return (env && !strcmp(env, "1")) || !motion_live;
}

/* LB + LT pressed together (physical buttons 6 and 8, controller_map.h; Switch L + ZL; owner, 2026-10-30: LT + RT came
   together in play, and a D-pad cannot press left and right at once): 1 on the press that completes the pair when the two presses came within TILT_CHORD_NS of each other.
   A held pair fires once; both must be let go (one is enough) and pressed again. */
typedef struct { uint64_t at[2]; int down; } GyroTiltChord;
static inline int gyro_tilt_chord(GyroTiltChord *c, uint64_t physical, uint64_t edges, uint64_t now) {
    const uint64_t pair = 1ull << 6 | 1ull << 8;
    for (int i = 0; i < 2; i++) if (edges >> (6 + 2 * i) & 1) c->at[i] = now;
    int both = (physical & pair) == pair, fire = 0;
    if (both && !c->down && (edges & pair)) fire = (c->at[0] > c->at[1] ? c->at[0] - c->at[1] : c->at[1] - c->at[0]) <= TILT_CHORD_NS;
    c->down = both;
    return fire;
}
#endif
