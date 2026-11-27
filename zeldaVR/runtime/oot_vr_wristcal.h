/* Ocarina of Time VR: the wrist calibration and the six wrist fine-tunes (vr.md row 31), the math only, without the runtime
   (tests/test_oot_vr_wristcal.c runs it). HookshotVR's idea (body/tracking.inc calibrate_wrists, body/math.inc local_euler;
   facts only, GPL-3): the player holds both hands ahead, palms facing each other and thumbs up, and presses A; each controller's
   turn from that reference is kept as three angles, and from then on the hand is the controller turned by them.

   Frames. A controller's grip pose (OpenXR, as the streamer sends it, in the head pose's space: y up, -z ahead at the recentre):
   -y along the open fingers, -z toward the thumb (through the closed fist, little finger to thumb), +x out of the back of the
   right hand and out of the palm of the left (to the right for both). The reference pose in those axes, in the room:
     x = the body's right (palms facing each other: the left palm and the back of the right hand point right),
     z = down (thumbs up: -z is up), y = z x x = the body's back (the fingers wrap forward, along -y);
   the body's right is the head's forward, levelled, crossed with up.

   The hand from a controller: hand = controller * E(pitch, yaw, roll) * Rx(tilt), quaternions xyzw, E in the controller's own
   frame: E = Rz(roll) Ry(yaw) Rx(pitch) (HookshotVR's local_euler order: pitch about x first, then yaw about y, then roll about z).
   Rx(tilt) is the older "hand tilt" option (degrees about the controller's side axis, as before), kept as it was.
   The calibration solves E = controller^-1 * reference * Rx(-tilt), so the hand is exactly the reference while the pose is held,
   and every later movement of the controller turns the hand the same way. Angles: pitch and roll -180..180, yaw -90..90. */
#pragma once
#include <math.h>
#include <string.h>

static inline int oot_wc_finite(const float *v, int n) { for (int i = 0; i < n; i++) if (!isfinite(v[i])) return 0; return 1; }
static inline void oot_wc_qmul(const float a[4], const float b[4], float o[4]) {
    const float r[4] = { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                         a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
    memcpy(o, r, sizeof r);
}
/* 0 when q is no rotation (zero or not finite) */
static inline int oot_wc_qnorm(float q[4]) {
    const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 1e-6f) || !isfinite(n)) return 0;
    for (int i = 0; i < 4; i++) q[i] /= n;
    return 1;
}
/* a turn of rad about axis 0 x, 1 y, 2 z */
static inline void oot_wc_axis_q(int axis, float rad, float q[4]) {
    q[0] = q[1] = q[2] = 0; q[axis] = sinf(rad / 2); q[3] = cosf(rad / 2);
}
/* E = Rz(roll) Ry(yaw) Rx(pitch), deg = { pitch, yaw, roll } */
static inline void oot_wc_euler_q(const float deg[3], float q[4]) {
    const float k = (float)M_PI / 180;
    float x[4], y[4], z[4], zy[4];
    oot_wc_axis_q(0, deg[0] * k, x); oot_wc_axis_q(1, deg[1] * k, y); oot_wc_axis_q(2, deg[2] * k, z);
    oot_wc_qmul(z, y, zy); oot_wc_qmul(zy, x, q);
}
/* the rotation's matrix, m[row][col]: column c is axis c turned */
static inline void oot_wc_q_matrix(const float q[4], float m[3][3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y - z * w);     m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);     m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);     m[2][1] = 2 * (y * z + x * w);     m[2][2] = 1 - 2 * (x * x + y * y);
}
/* m = Rz(roll) Ry(yaw) Rx(pitch): the three angles in degrees (yaw = +-90: pitch 0, the rest in roll) */
static inline void oot_wc_matrix_euler(const float m[3][3], float deg[3]) {
    const float k = 180 / (float)M_PI, s = fmaxf(-1, fminf(1, -m[2][0])), yaw = asinf(s), cy = cosf(yaw);
    float pitch, roll;
    if (fabsf(cy) > 1e-4f) { pitch = atan2f(m[2][1], m[2][2]); roll = atan2f(m[1][0], m[0][0]); }
    else { pitch = 0; roll = atan2f(-m[0][1], m[1][1]); }
    deg[0] = pitch * k; deg[1] = yaw * k; deg[2] = roll * k;
}
/* The hand's orientation from the controller's: controller * E(deg) * Rx(tilt). A controller orientation that is no rotation, or
   angles that are not finite: the controller's own, unchanged (1 when the hand is the calibrated one). */
static inline int oot_wc_apply(const float raw[4], const float deg[3], float tilt_deg, float out[4]) {
    float q[4] = { raw[0], raw[1], raw[2], raw[3] };
    if (!oot_wc_finite(q, 4) || !oot_wc_qnorm(q) || !oot_wc_finite(deg, 3) || !isfinite(tilt_deg)) { memcpy(out, raw, 4 * sizeof *out); return 0; }
    float e[4], t[4], qe[4];
    oot_wc_euler_q(deg, e); oot_wc_axis_q(0, tilt_deg * (float)M_PI / 180, t);
    oot_wc_qmul(q, e, qe); oot_wc_qmul(qe, t, out);
    oot_wc_qnorm(out);
    return 1;
}
/* The reference grip, its axes as the columns of want, from the head's orientation (head pose space). 0 when the head looks
   within about 17 degrees of straight up or down (no level forward to take the body's right from). */
static inline int oot_wc_reference(const float head_q[4], float want[3][3]) {
    float h[4] = { head_q[0], head_q[1], head_q[2], head_q[3] }, m[3][3];
    if (!oot_wc_finite(h, 4) || !oot_wc_qnorm(h)) return 0;
    oot_wc_q_matrix(h, m);
    float fx = -m[0][2], fz = -m[2][2];   /* the head's forward (its -z), levelled */
    const float n = sqrtf(fx * fx + fz * fz);
    if (n < 0.3f) return 0;
    fx /= n; fz /= n;
    const float x[3] = { -fz, 0, fx }, z[3] = { 0, -1, 0 };   /* right = forward x up; thumbs up */
    const float y[3] = { z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0] };   /* z x x: the back */
    for (int r = 0; r < 3; r++) { want[r][0] = x[r]; want[r][1] = y[r]; want[r][2] = z[r]; }
    return 1;
}
/* The three angles that make this controller (held in the reference pose) the reference hand, with the hand tilt as it is set.
   0, deg untouched, when the controller's or the head's orientation is no rotation or the head looks straight up or down. */
static inline int oot_wc_solve(const float raw[4], const float head_q[4], float tilt_deg, float deg[3]) {
    float q[4] = { raw[0], raw[1], raw[2], raw[3] }, want[3][3], r[3][3], t[3][3], wt[3][3], e[3][3], tq[4];
    if (!oot_wc_finite(q, 4) || !oot_wc_qnorm(q) || !isfinite(tilt_deg) || !oot_wc_reference(head_q, want)) return 0;
    oot_wc_q_matrix(q, r);
    oot_wc_axis_q(0, -tilt_deg * (float)M_PI / 180, tq); oot_wc_q_matrix(tq, t);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { wt[i][j] = 0; for (int k = 0; k < 3; k++) wt[i][j] += want[i][k] * t[k][j]; }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { e[i][j] = 0; for (int k = 0; k < 3; k++) e[i][j] += r[k][i] * wt[k][j]; }   /* R^T (W T) */
    float out[3];
    oot_wc_matrix_euler(e, out);
    if (!oot_wc_finite(out, 3)) return 0;
    memcpy(deg, out, sizeof out);
    return 1;
}
/* degrees as the options keep them: whole, -180..180 */
static inline int oot_wc_whole(float d) { const long v = lroundf(d); return v < -180 ? -180 : v > 180 ? 180 : (int)v; }
