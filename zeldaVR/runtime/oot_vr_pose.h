/* The first-person arm's skinning matrices for Ocarina of Time, without the runtime (tests/test_oot_vr_pose.c runs it). Input:
   the arm's joints in the model's rest pose and where the shoulder and the controller are. Output: one 3x4 matrix per part of
   the arm, each taking a rest-pose point to where it goes, which is exactly what a smooth skinning palette entry is
   (world x inverse rest). Built from the rest pose, so what the game's animation does to the arm does not show through.
   Our own solver (oot_vr_ik.h); the twist shares and the palm offset follow OcarinaVR's description (ideas only, GPL-3). */
#pragma once
#include <math.h>
#include <string.h>
#include "oot_vr_ik.h"

enum { OOT_FINGERS = 5, OOT_FINGER_JOINTS = 3 };

typedef struct {
    float shoulder[3], elbow[3], wrist[3];
    float finger[OOT_FINGERS][OOT_FINGER_JOINTS][3];   /* 0 thumb, 1 index, 2 middle, 3 ring, 4 little; root to tip */
} oot_arm_rest;

typedef struct {
    float clavicle[12], upper[12], fore[12], assist[2][12], hand[12];
    float finger[OOT_FINGERS][OOT_FINGER_JOINTS][12];
    oot_arm_pose joints;
} oot_arm_matrices;

typedef struct {
    float shoulder[3], grip[3];    /* where the shoulder and the controller are */
    float forward[3], up[3];       /* the controller's axes: along the fingers, the back of the hand */
    float pole[3];                 /* where the elbow should point */
    float trigger, squeeze;
    int left, holding;
} oot_arm_target;

static inline void oot_m_identity(float m[12]) {
    static const float i[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    memcpy(m, i, sizeof i);
}
static inline void oot_m_apply(const float m[12], const float p[3], float out[3]) {
    const float r[3] = { m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
                         m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11] };
    memcpy(out, r, sizeof r);
}
static inline void oot_m_mul(const float a[12], const float b[12], float out[12]) {
    float r[12];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j];
        r[4 * i + 3] = a[4 * i] * b[3] + a[4 * i + 1] * b[7] + a[4 * i + 2] * b[11] + a[4 * i + 3];
    }
    memcpy(out, r, sizeof r);
}
/* The rotation about the unit axis through the origin. */
static inline void oot_m_axis(const float a[3], float angle, float m[12]) {
    const float c = cosf(angle), s = sinf(angle), t = 1 - c;
    const float r[12] = { t * a[0] * a[0] + c, t * a[0] * a[1] - s * a[2], t * a[0] * a[2] + s * a[1], 0,
                          t * a[0] * a[1] + s * a[2], t * a[1] * a[1] + c, t * a[1] * a[2] - s * a[0], 0,
                          t * a[0] * a[2] - s * a[1], t * a[1] * a[2] + s * a[0], t * a[2] * a[2] + c, 0 };
    memcpy(m, r, sizeof r);
}
/* The shortest rotation taking unit vector a to unit vector b. */
static inline void oot_m_between(const float a[3], const float b[3], float m[12]) {
    float axis[3]; oot_ik_cross(a, b, axis);
    const float d = fmaxf(-1, fminf(1, oot_ik_dot(a, b)));
    if (oot_ik_norm(axis)) { oot_m_axis(axis, acosf(d), m); return; }
    if (d > 0) { oot_m_identity(m); return; }
    const float x[3] = { 1, 0, 0 }, y[3] = { 0, 1, 0 };
    oot_ik_cross(a, fabsf(a[0]) < 0.9f ? x : y, axis); oot_ik_norm(axis);
    oot_m_axis(axis, (float)M_PI, m);
}
/* x -> to + L (x - from), L a 3x3 in a 3x4. */
static inline void oot_m_about(const float l[12], const float from[3], const float to[3], float out[12]) {
    float r[12]; memcpy(r, l, sizeof r);
    for (int i = 0; i < 3; i++) r[4 * i + 3] = to[i] - (l[4 * i] * from[0] + l[4 * i + 1] * from[1] + l[4 * i + 2] * from[2]);
    memcpy(out, r, sizeof r);
}
/* Stretches by k along the unit axis, after l. */
static inline void oot_m_stretch(float l[12], const float d[3], float k) {
    if (k == 1) return;
    const float e = k - 1;
    const float s[12] = { 1 + e * d[0] * d[0], e * d[0] * d[1], e * d[0] * d[2], 0, e * d[1] * d[0], 1 + e * d[1] * d[1], e * d[1] * d[2], 0,
                          e * d[2] * d[0], e * d[2] * d[1], 1 + e * d[2] * d[2], 0 };
    oot_m_mul(s, l, l);
}
static inline int oot_unit(const float from[3], const float to[3], float out[3]) {
    out[0] = to[0] - from[0]; out[1] = to[1] - from[1]; out[2] = to[2] - from[2];
    return oot_ik_norm(out);
}
/* The orthonormal frame with x along a and y as near b as it can be; rows of the returned 3x4 are its axes. */
static inline int oot_frame(const float a[3], const float b[3], float out[3][3]) {
    float x[3] = { a[0], a[1], a[2] }, y[3] = { b[0], b[1], b[2] }, z[3];
    if (!oot_ik_norm(x)) return 0;
    const float k = oot_ik_dot(y, x);
    for (int i = 0; i < 3; i++) y[i] -= x[i] * k;
    if (!oot_ik_norm(y)) return 0;
    oot_ik_cross(x, y, z);
    memcpy(out[0], x, 12); memcpy(out[1], y, 12); memcpy(out[2], z, 12);
    return 1;
}

static inline int oot_arm_matrices_solve(const oot_arm_rest *r, const oot_arm_target *t, oot_arm_matrices *o) {
    float d1[3], d2[3], f0[3];
    if (!oot_unit(r->shoulder, r->elbow, d1) || !oot_unit(r->elbow, r->wrist, d2) || !oot_unit(r->wrist, r->finger[2][0], f0)) return 0;
    const float upper = sqrtf((r->elbow[0] - r->shoulder[0]) * (r->elbow[0] - r->shoulder[0]) + (r->elbow[1] - r->shoulder[1]) * (r->elbow[1] - r->shoulder[1]) +
                              (r->elbow[2] - r->shoulder[2]) * (r->elbow[2] - r->shoulder[2]));
    const float fore = sqrtf((r->wrist[0] - r->elbow[0]) * (r->wrist[0] - r->elbow[0]) + (r->wrist[1] - r->elbow[1]) * (r->wrist[1] - r->elbow[1]) +
                             (r->wrist[2] - r->elbow[2]) * (r->wrist[2] - r->elbow[2]));
    const float palm = sqrtf((r->finger[2][0][0] - r->wrist[0]) * (r->finger[2][0][0] - r->wrist[0]) + (r->finger[2][0][1] - r->wrist[1]) * (r->finger[2][0][1] - r->wrist[1]) +
                             (r->finger[2][0][2] - r->wrist[2]) * (r->finger[2][0][2] - r->wrist[2]));
    float fwd[3] = { t->forward[0], t->forward[1], t->forward[2] }, back[3] = { t->up[0], t->up[1], t->up[2] };
    if (!oot_ik_norm(fwd) || !oot_ik_norm(back)) return 0;
    const float w[3] = { t->grip[0] - fwd[0] * 0.62f * palm, t->grip[1] - fwd[1] * 0.62f * palm, t->grip[2] - fwd[2] * 0.62f * palm };
    if (!oot_arm_solve(t->shoulder, w, upper, fore, t->pole, &o->joints)) return 0;
    const float *S = o->joints.shoulder, *E = o->joints.elbow, *W = o->joints.wrist;
    float n1[3], n2[3], L[12];
    if (!oot_unit(S, E, n1) || !oot_unit(E, W, n2)) return 0;
    oot_m_identity(L); oot_m_about(L, r->shoulder, S, o->clavicle);
    oot_m_between(d1, n1, L); oot_m_stretch(L, n1, o->joints.stretch); oot_m_about(L, r->shoulder, S, o->upper);
    float fore_l[12];
    oot_m_between(d2, n2, fore_l); oot_m_stretch(fore_l, n2, o->joints.stretch); oot_m_about(fore_l, r->elbow, E, o->fore);

    /* the hand: the rest frame (along the fingers, toward the thumb side) onto the controller's */
    float side0[3]; for (int i = 0; i < 3; i++) side0[i] = r->finger[1][0][i] - r->finger[4][0][i];
    float rest[3][3], want[3][3], right[3], side1[3];
    if (!oot_frame(f0, side0, rest)) return 0;
    oot_ik_cross(fwd, back, right);
    for (int i = 0; i < 3; i++) side1[i] = t->left ? right[i] : -right[i];   /* the thumb is toward the body's middle */
    if (!oot_frame(fwd, side1, want)) return 0;
    float H[12] = {0};
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) H[4 * i + j] = want[0][i] * rest[0][j] + want[1][i] * rest[1][j] + want[2][i] * rest[2][j];
    oot_m_about(H, r->wrist, W, o->hand);

    /* the forearm's twist toward the hand, shared by the two assist bones */
    float rel[12], base[12];
    oot_m_between(d2, n2, base);   /* the forearm without its stretch */
    { const float inverse[12] = { base[0], base[4], base[8], 0, base[1], base[5], base[9], 0, base[2], base[6], base[10], 0 };
      oot_m_mul(H, inverse, rel); }
    /* swing-twist: with q the rotation's quaternion, the twist about n is 2 atan2(q.xyz . n, q.w); both sides over 4 q.w */
    const float vec[3] = { rel[9] - rel[6], rel[2] - rel[8], rel[4] - rel[1] };
    const float tw = 2 * atan2f(oot_ik_dot(vec, n2), 1 + rel[0] + rel[5] + rel[10]);
    static const float share[2] = { 0.45f, 0.85f };
    for (int k = 0; k < 2; k++) {
        float spin[12], l[12]; oot_m_axis(n2, tw * share[k], spin); oot_m_mul(spin, fore_l, l);
        oot_m_about(l, r->elbow, E, o->assist[k]);
    }

    /* the fingers, each joint curled toward the palm from the hand's rest pose */
    const float palm_dir[3] = { -back[0], -back[1], -back[2] };
    for (int f = 0; f < OOT_FINGERS; f++) {
        float curl[3]; oot_finger_curl(f, t->trigger, t->squeeze, t->holding, curl);
        float cur[12]; memcpy(cur, o->hand, sizeof cur);
        for (int j = 0; j < OOT_FINGER_JOINTS; j++) {
            float at[3], next[3], dir[3], axis[3], spin[12], about[12], step[12];
            oot_m_apply(cur, r->finger[f][j], at);
            if (j + 1 < OOT_FINGER_JOINTS) oot_m_apply(cur, r->finger[f][j + 1], next);
            else { for (int i = 0; i < 3; i++) next[i] = at[i] + fwd[i]; }
            if (!oot_unit(at, next, dir)) { memcpy(o->finger[f][j], cur, sizeof cur); continue; }
            oot_ik_cross(dir, palm_dir, axis);
            if (oot_ik_norm(axis)) {
                oot_m_axis(axis, curl[j], spin);
                oot_m_about(spin, at, at, about);
                oot_m_mul(about, cur, step);
                memcpy(cur, step, sizeof cur);
            }
            memcpy(o->finger[f][j], cur, sizeof cur);
        }
    }
    return 1;
}
