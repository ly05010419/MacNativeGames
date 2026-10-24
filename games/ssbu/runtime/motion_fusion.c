/* Six-axis states from the Mac controller's motion samples (2026-10-17; fed by runtime/gamepad_feedback.m, read by the
   game through runtime/hid_feedback.c, only with TOTK_GYRO=1).
   Input per sample: acceleration in G including gravity and rotation rate in rad/s, in the controller's frame (GCMotion:
   x to the right, y toward the top edge, z out of the face; lying face up it reads z = -1 G). That is the Switch's
   six-axis frame, so by default nothing is remapped; TOTK_GYRO_AXES="-x,y,-z" style remaps (a reflection also flips the
   rotation's sense). Output per sample, like the Switch: acceleration in G, angular velocity in revolutions per second,
   angle = the integrated revolutions per axis, direction = the controller's three axes in a reference frame whose z is
   up. The orientation is integrated from the gyro and, while the controller is not being accelerated (|a| within 15 %
   of 1 G), pulled toward the measured gravity (Mahony, gain 0.5/s: about 2 s), as the Switch's sensor fusion does; yaw
   has no reference and drifts with the gyro's bias (well under a degree a minute on a DualSense at rest). The newest 64
   samples are kept; the game reads up to 16, newest first, and goes by their sampling numbers.
   Dead zone (2026-10-19): rotation slower than TOTK_GYRO_DEADZONE deg/s counts as none, and the dead zone is taken off
   faster rotation, so the rate never jumps at its edge. Default 0, except 1.2 while a controller's motion comes from its
   HID reports (runtime_motion_deadzone_default, gamepad_feedback.m): an 8BitDo Pro Controller lying on the desk read
   0.83 deg/s on average (bias 0.05, 0.73, 0.38 deg/s after its factory calibration), at most 1.07 deg/s, in 616 samples,
   enough for the aim to creep; a DualSense through GameController needs none.
   Mouse mode (Mac, key 7: runtime/gamepad.m, runtime/mouse_look.h): the mouse takes the sensor over. Its samples come in
   this frame already (runtime_motion_feed_mouse: no TOTK_GYRO_AXES remap, no dead zone), the controller's are dropped
   meanwhile, and with mouse mode off the controller has it back (from a fresh start). */
#include "runtime.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MOTION_RING = 64 };
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static RuntimeMotionState ring[MOTION_RING];
static uint64_t produced, last_ns, logged, log_at, reads_logged;
static double R[3][3];          /* controller -> reference; row i = reference axis i in controller coordinates */
static double angle[3];
static int restart = 1, live;
static int from[3] = {0, 1, 2};
static double sign[3] = {1, 1, 1}, det = 1;
static double deadzone = -1;   /* rad/s; -1 until read from TOTK_GYRO_DEADZONE */
static double deadzone_default;   /* deg/s, when TOTK_GYRO_DEADZONE is unset */
static int mouse_owns, controller_live;

/* TOTK_GYRO_AXES: reference axis i = sign * controller axis from[i]. 0 if unset, -1 if malformed. */
int runtime_motion_axes(const char *spec) {
    int f[3], used[3] = {0}; double s[3], parity = 1; const char *p = spec;
    if (!spec) return 0;
    for (int i = 0; i < 3; i++) {
        s[i] = 1; if (*p == '-') { s[i] = -1; p++; } else if (*p == '+') p++;
        if (*p < 'x' || *p > 'z' || used[*p - 'x']) return -1;
        f[i] = *p - 'x'; used[f[i]] = 1; p++;
        if (i < 2 && *p++ != ',') return -1;
    }
    if (*p) return -1;
    for (int i = 0; i < 3; i++) for (int j = i + 1; j < 3; j++) if (f[i] > f[j]) parity = -parity;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 3; i++) { from[i] = f[i]; sign[i] = s[i]; }
    det = parity * s[0] * s[1] * s[2];
    pthread_mutex_unlock(&lock);
    return 1;
}
static double norm3(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static void cross3(const double a[3], const double b[3], double out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1]; out[1] = a[2] * b[0] - a[0] * b[2]; out[2] = a[0] * b[1] - a[1] * b[0];
}
static void orthonormalize(void) {   /* rows, from the up row (the one the gravity correction keeps right) */
    double n = norm3(R[2]); for (int j = 0; j < 3; j++) R[2][j] /= n;
    double d = R[0][0] * R[2][0] + R[0][1] * R[2][1] + R[0][2] * R[2][2];
    for (int j = 0; j < 3; j++) R[0][j] -= d * R[2][j];
    n = norm3(R[0]); for (int j = 0; j < 3; j++) R[0][j] /= n;
    cross3(R[2], R[0], R[1]);
}
/* A fresh orientation: z up against the measured gravity, x as close to the controller's x as that allows. */
static void align_to(const double acc[3]) {
    double n = norm3(acc), up[3] = {0, 0, 1}, x[3] = {1, 0, 0};
    if (n > 0.5) for (int j = 0; j < 3; j++) up[j] = -acc[j] / n;
    if (fabs(up[0]) > 0.9) { x[0] = 0; x[1] = 1; }
    memcpy(R[2], up, sizeof up); memcpy(R[0], x, sizeof x); orthonormalize();
}
static void integrate(const double gyro[3], const double acc[3], double dt) {
    double w[3] = {gyro[0], gyro[1], gyro[2]}, n = norm3(acc);
    if (fabs(n - 1) < 0.15) {   /* not being accelerated: turn the estimated down toward the measured one */
        const double kp = 0.5;
        double measured[3] = {acc[0] / n, acc[1] / n, acc[2] / n}, estimated[3] = {-R[2][0], -R[2][1], -R[2][2]}, e[3];
        cross3(measured, estimated, e);
        for (int j = 0; j < 3; j++) w[j] += kp * e[j];
    }
    double rate = norm3(w), theta = rate * dt;
    if (theta < 1e-12) return;
    double k[3] = {w[0] / rate, w[1] / rate, w[2] / rate}, s = sin(theta), c = 1 - cos(theta);
    double K[3][3] = {{0, -k[2], k[1]}, {k[2], 0, -k[0]}, {-k[1], k[0], 0}}, E[3][3], out[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        double kk = 0; for (int m = 0; m < 3; m++) kk += K[i][m] * K[m][j];
        E[i][j] = (i == j) + s * K[i][j] + c * kk;
    }
    static int negative = -1; if (negative < 0) { const char *e = getenv("TOTK_PROBE_NEGATIVE"); negative = e && atoi(e) == 20; }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {   /* body-frame rate: R * E (probe negative 20: E * R, a reference-frame rate) */
        double v = 0; for (int m = 0; m < 3; m++) v += negative ? E[i][m] * R[m][j] : R[i][m] * E[m][j];
        out[i][j] = v;
    }
    memcpy(R, out, sizeof R); orthonormalize();
}
static void feed_locked(const double acc[3], const double gyro[3], uint64_t now);
void runtime_motion_feed(const double raw_acc[3], const double raw_gyro[3], uint64_t now) {
    double acc[3], gyro[3];
    pthread_mutex_lock(&lock);
    for (int i = 0; i < 3; i++) { acc[i] = sign[i] * raw_acc[from[i]]; gyro[i] = det * sign[i] * raw_gyro[from[i]]; }
    if (deadzone < 0) {
        const char *e = getenv("TOTK_GYRO_DEADZONE"); double v = deadzone_default;
        if (e) { char *end; v = strtod(e, &end); if (end == e || *end || !(v >= 0) || v > 20) runtime_fail("TOTK_GYRO_DEADZONE must be a number of degrees a second in [0, 20]"); }
        deadzone = v * M_PI / 180;
        if (e) fprintf(stderr, "[gyro] dead zone %.2f deg/s (TOTK_GYRO_DEADZONE)\n", v);
    }
    double rate = norm3(gyro), keep = rate > deadzone ? (rate - deadzone) / rate : 0;
    for (int i = 0; i < 3; i++) gyro[i] *= keep;
    if (mouse_owns) { pthread_mutex_unlock(&lock); return; }
    feed_locked(acc, gyro, now);
    pthread_mutex_unlock(&lock);
}
/* One sample in the reference controller frame (lock held). */
static void feed_locked(const double acc[3], const double gyro[3], uint64_t now) {
    double dt = last_ns && now > last_ns ? (now - last_ns) / 1e9 : 0.004;
    if (dt > 0.05) dt = 0.004;   /* a gap (sensors were off, the app was in the background): no integration across it */
    last_ns = now; live = 1;
    if (restart) { align_to(acc); memset(angle, 0, sizeof angle); restart = 0; }
    else integrate(gyro, acc, dt);
    RuntimeMotionState *s = &ring[produced % MOTION_RING];
    *s = (RuntimeMotionState){.sampling = produced + 1, .delta_ns = (uint64_t)(dt * 1e9)};
    for (int i = 0; i < 3; i++) {
        double rps = gyro[i] / (2 * M_PI); angle[i] += rps * dt;
        s->acc[i] = (float)acc[i]; s->gyro[i] = (float)rps; s->angle[i] = (float)angle[i];
        for (int j = 0; j < 3; j++) s->dir[i * 3 + j] = (float)R[j][i];   /* direction i = controller axis i in the reference frame */
    }
    produced++;
    if (!log_at) log_at = now;
    if (now - log_at >= 30000000000ull) {
        uint64_t values, reads; runtime_hid_feedback_stats(&values, &reads);
        fprintf(stderr, "[gyro] last 30 s: %llu samples (%.0f a second), the game read the states %llu times; now acc (%+.2f %+.2f %+.2f) G, gyro (%+.3f %+.3f %+.3f) rev/s\n",
                (unsigned long long)(produced - logged), (produced - logged) / ((now - log_at) / 1e9), (unsigned long long)(reads - reads_logged),
                s->acc[0], s->acc[1], s->acc[2], s->gyro[0], s->gyro[1], s->gyro[2]);
        logged = produced; reads_logged = reads; log_at = now;
    }
}
void runtime_motion_feed_mouse(const double acc[3], const double gyro[3], uint64_t now) {
    pthread_mutex_lock(&lock);
    if (mouse_owns) feed_locked(acc, gyro, now);
    pthread_mutex_unlock(&lock);
}
int runtime_motion_read(RuntimeMotionState *out, int max) {
    if (max <= 0) return 0;
    int n = 0;
    pthread_mutex_lock(&lock);
    if (live && runtime_now_ns() - last_ns < 200000000ull) {   /* no sample for 200 ms: no motion */
        uint64_t have = produced < MOTION_RING ? produced : MOTION_RING;
        n = (int)(have < (uint64_t)max ? have : (uint64_t)max);
        for (int i = 0; i < n; i++) out[i] = ring[(produced - 1 - (uint64_t)i) % MOTION_RING];
    }
    pthread_mutex_unlock(&lock);
    return n;
}
void runtime_motion_start(void) { pthread_mutex_lock(&lock); restart = 1; pthread_mutex_unlock(&lock); }
/* The dead zone in deg/s when TOTK_GYRO_DEADZONE is unset: the HID reader sets 1.2, a GameController controller 0. */
void runtime_motion_deadzone_default(double deg) { pthread_mutex_lock(&lock); deadzone_default = deg; deadzone = -1; pthread_mutex_unlock(&lock); }
void runtime_motion_live(int on) {
    pthread_mutex_lock(&lock); controller_live = on; live = on || mouse_owns; if (on && !mouse_owns) restart = 1; pthread_mutex_unlock(&lock);
}
void runtime_motion_mouse(int on) {
    pthread_mutex_lock(&lock); mouse_owns = on; live = on || controller_live; restart = 1; last_ns = 0; pthread_mutex_unlock(&lock);
}
