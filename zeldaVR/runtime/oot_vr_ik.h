/* The first-person arm of Ocarina of Time, without the runtime (tests/test_oot_vr_ik.c runs it): a two-bone solve from the
   shoulder to the controller, and how far each finger curls. Our own solver; the proportions follow OcarinaVR's description
   (ideas only, GPL-3): the arm reaches 99.5 % of its length, then the shoulder gives up to 6 cm toward the hand, then the arm
   stretches up to 1.4 times, so a hand never drifts off the controller in normal play. */
#pragma once
#include <math.h>
#include <string.h>

typedef struct { float shoulder[3], elbow[3], wrist[3]; float stretch; int reached; } oot_arm_pose;

static inline float oot_ik_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float oot_ik_len(const float a[3]) { return sqrtf(oot_ik_dot(a, a)); }
static inline void oot_ik_cross(const float a[3], const float b[3], float out[3]) {
    const float c[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    memcpy(out, c, sizeof c);
}
static inline int oot_ik_norm(float a[3]) {
    const float l = oot_ik_len(a);
    if (!(l > 1e-6f)) return 0;
    a[0] /= l; a[1] /= l; a[2] /= l;
    return 1;
}

/* upper, fore: the bones' lengths; pole: where the elbow should point (any length, need not be perpendicular). */
static inline int oot_arm_solve(const float shoulder[3], const float target[3], float upper, float fore, const float pole[3], oot_arm_pose *out) {
    if (!(upper > 1e-4f) || !(fore > 1e-4f)) return 0;
    for (int i = 0; i < 3; i++) if (!isfinite(shoulder[i]) || !isfinite(target[i]) || !isfinite(pole[i])) return 0;
    float s[3] = { shoulder[0], shoulder[1], shoulder[2] }, d[3] = { target[0] - s[0], target[1] - s[1], target[2] - s[2] };
    float dist = oot_ik_len(d);
    float dir[3] = { d[0], d[1], d[2] };
    if (!oot_ik_norm(dir)) { dir[0] = 0; dir[1] = -1; dir[2] = 0; dist = 0; }
    const float full = upper + fore, reach = 0.995f * full;
    float scale = 1;
    int reached = 1;
    if (dist > reach) {
        const float give = fminf(0.06f, dist - reach);
        for (int i = 0; i < 3; i++) s[i] += dir[i] * give;
        dist -= give;
        if (dist > reach) { scale = fminf(dist / reach, 1.4f); if (dist > reach * scale) { dist = reach * scale; reached = 0; } }
    }
    const float a = upper * scale, b = fore * scale;
    /* the elbow: along the arm by the law of cosines, off it toward the pole */
    const float dd = fmaxf(dist, 1e-4f), along = (a * a - b * b + dd * dd) / (2 * dd), off = sqrtf(fmaxf(a * a - along * along, 0));
    float p[3] = { pole[0], pole[1], pole[2] };
    const float k = oot_ik_dot(p, dir);
    for (int i = 0; i < 3; i++) p[i] -= dir[i] * k;
    if (!oot_ik_norm(p)) {   /* the pole lies along the arm: any perpendicular */
        const float x[3] = { 1, 0, 0 }, y[3] = { 0, 1, 0 };
        oot_ik_cross(dir, fabsf(dir[0]) < 0.9f ? x : y, p);
        oot_ik_norm(p);
    }
    for (int i = 0; i < 3; i++) {
        out->shoulder[i] = s[i];
        out->elbow[i] = s[i] + dir[i] * along + p[i] * off;
        out->wrist[i] = s[i] + dir[i] * dist;
    }
    out->stretch = scale; out->reached = reached;
    return 1;
}

/* How far a finger's three joints curl, radians. finger: 0 thumb, 1 index, 2..4 the others; holding: a handle is in the hand,
   so the fingers stop around it (the grip counts up to 0.6). */
static inline void oot_finger_curl(int finger, float trigger, float grip, int holding, float out[3]) {
    trigger = fmaxf(0, fminf(1, trigger));
    grip = fmaxf(0, fminf(holding ? 0.6f : 1, grip));
    if (finger == 0) {
        const float k = fmaxf(trigger, grip);
        out[0] = 0.10f + 0.35f * k; out[1] = 0.10f + 0.35f * k; out[2] = 0.15f + 0.50f * k;
    } else if (finger == 1) {
        out[0] = 0.20f + 1.05f * trigger; out[1] = 0.30f + 1.25f * trigger; out[2] = 0.20f + 0.75f * trigger;
    } else {
        out[0] = 0.25f + 1.10f * grip; out[1] = 0.35f + 1.25f * grip; out[2] = 0.25f + 0.75f * grip;
    }
}
