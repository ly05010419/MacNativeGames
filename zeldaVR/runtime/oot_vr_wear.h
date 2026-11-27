/* Ocarina of Time in VR, first person: what Link wears and how his hand holds things, without the runtime
   (tests/test_oot_vr_wear.c runs it). Facts from HookshotVR's notes (held-items.md 1.10 and 2.4, its generators' lists;
   ideas only, GPL-3); the rules here are ours.
   - The game's own grip: a hand that holds something takes the game's own fingers, each finger's matrix relative to the
     game's hand, put on the VR hand. In the other hand the same grip mirrored across Link's body (x = 0): his skeleton is
     mirrored there, so a relative matrix A becomes X A X, X = diag(-1, 1, 1).
   - Worn on a bone: a thing the game poses on one of his bones moves by as much as that bone moved from the game's pose to the
     VR one (the bracelet on a forearm or wrist).
   - Recognised by its model: a draw's palette size and vertex count are those of one of the player's own models (vr-rig). */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

/* The inverse of a 3x4 affine matrix (rows); 0 when it has none. */
static inline int oot_wear_inverse(const float m[12], float out[12]) {
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g, det = a * A + b * B + c * C;
    if (!(fabsf(det) > 1e-12f) || !isfinite(det)) return 0;
    const float k = 1 / det;
    const float r[9] = { A * k, -(b * i - c * h) * k, (b * f - c * e) * k, B * k, (a * i - c * g) * k, -(a * f - c * d) * k,
                         C * k, -(a * h - b * g) * k, (a * e - b * d) * k };
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 3; x++) out[4 * y + x] = r[3 * y + x];
        out[4 * y + 3] = -(r[3 * y] * m[3] + r[3 * y + 1] * m[7] + r[3 * y + 2] * m[11]);
    }
    return 1;
}
static inline void oot_wear_mul(const float a[12], const float b[12], float out[12]) {
    float r[12];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r[4 * i + j] = a[4 * i] * b[j] + a[4 * i + 1] * b[4 + j] + a[4 * i + 2] * b[8 + j];
        r[4 * i + 3] = a[4 * i] * b[3] + a[4 * i + 1] * b[7] + a[4 * i + 2] * b[11] + a[4 * i + 3];
    }
    memcpy(out, r, sizeof r);
}
static inline int oot_wear_finite(const float m[12]) { for (int i = 0; i < 12; i++) if (!isfinite(m[i])) return 0; return 1; }
/* A skinning entry the game scaled to nothing (|det| under 0.05, HookshotVR's measure): a part it hides. */
static inline int oot_wear_scaled_away(const float m[12]) {
    const float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    return !(fabsf(det) >= 0.05f);
}
/* X A X with X the mirror across x = 0. */
static inline void oot_wear_mirror(const float a[12], float out[12]) {
    float r[12]; memcpy(r, a, sizeof r);
    r[1] = -r[1]; r[2] = -r[2]; r[3] = -r[3]; r[4] = -r[4]; r[8] = -r[8];
    memcpy(out, r, sizeof r);
}

/* A finger's skinning matrix on the VR hand: vr_hand x inverse(game_hand) x game_finger (mirrored when the VR hand is the game's
   other hand). 0, out untouched, when the game's hand has no inverse or a result is not finite. */
static inline int oot_wear_finger(const float vr_hand[12], const float game_hand[12], const float game_finger[12], int mirrored, float out[12]) {
    float inv[12], rel[12], r[12];
    if (!oot_wear_inverse(game_hand, inv)) return 0;
    oot_wear_mul(inv, game_finger, rel);
    if (mirrored) oot_wear_mirror(rel, rel);
    oot_wear_mul(vr_hand, rel, r);
    if (!oot_wear_finite(r)) return 0;
    memcpy(out, r, sizeof r);
    return 1;
}
/* Is the skeleton mirrored across x = 0 where the grip is mirrored: each left joint's mirror image within tol of the right's. */
static inline int oot_wear_mirrored(const float (*left)[3], const float (*right)[3], int n, float tol) {
    for (int k = 0; k < n; k++)
        if (fabsf(left[k][0] + right[k][0]) > tol || fabsf(left[k][1] - right[k][1]) > tol || fabsf(left[k][2] - right[k][2]) > tol) return 0;
    return n > 0;
}

/* A thing posed by the game on one bone, carried as that bone went from the game's pose to the VR one. All in one space (world):
   out = vr_bone x inverse(game_bone) x thing. 0 when the game's bone has no inverse. */
static inline int oot_wear_carry(const float vr_bone[12], const float game_bone[12], const float thing[12], float out[12]) {
    float inv[12], d[12], r[12];
    if (!oot_wear_inverse(game_bone, inv)) return 0;
    oot_wear_mul(vr_bone, inv, d); oot_wear_mul(d, thing, r);
    if (!oot_wear_finite(r)) return 0;
    memcpy(out, r, sizeof r);
    return 1;
}
/* The distance from p to the segment a..b. */
static inline float oot_wear_segment(const float p[3], const float a[3], const float b[3]) {
    const float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, ap[3] = { p[0] - a[0], p[1] - a[1], p[2] - a[2] };
    const float l = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = l > 1e-12f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / l : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const float d[3] = { ap[0] - ab[0] * t, ap[1] - ab[1] * t, ap[2] - ab[2] * t };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
/* Which of four bones (left forearm, left hand, right forearm, right hand: their segments) a worn thing rides: the nearest
   within reach, keeping the one it rode while within keep. -1: none. */
static inline int oot_wear_rides(const float at[3], const float seg[4][2][3], int was, float reach, float keep) {
    int best = -1; float near = 1e9f;
    for (int k = 0; k < 4; k++) { const float d = oot_wear_segment(at, seg[k][0], seg[k][1]); if (d < near) { near = d; best = k; } }
    if (was >= 0 && was < 4 && oot_wear_segment(at, seg[was][0], seg[was][1]) <= keep) return was;
    return near <= reach ? best : -1;
}

/* The models a draw can be recognised as: palette bytes and the vertex count of one of their shapes. */
enum { OOT_WEAR_KINDS = 128, OOT_WEAR_SHAPES = 16 };
enum { OOT_WEAR_WORN = 1, OOT_WEAR_CARRIED = 2, OOT_WEAR_OWN = 3 };
typedef struct { uint64_t palette; unsigned verts[OOT_WEAR_SHAPES], shapes; int kind, index; } oot_wear_model;
typedef struct { oot_wear_model m[OOT_WEAR_KINDS]; unsigned n; } oot_wear_table;
static inline int oot_wear_add(oot_wear_table *t, uint64_t palette, const unsigned *verts, unsigned shapes, int kind, int index) {
    if (t->n >= OOT_WEAR_KINDS || !palette || !shapes) return 0;
    oot_wear_model *m = &t->m[t->n++];
    memset(m, 0, sizeof *m);
    m->palette = palette; m->kind = kind; m->index = index;
    m->shapes = shapes < OOT_WEAR_SHAPES ? shapes : OOT_WEAR_SHAPES;
    memcpy(m->verts, verts, m->shapes * sizeof *verts);
    return 1;
}
/* What a draw (palette bytes, vertex count) is: 0 nothing known. A worn thing of the same size as one of Link's own models (a
   sword, a shield, a sheath, what he carries) is never taken for worn: that size stays his own. */
static inline const oot_wear_model *oot_wear_find(const oot_wear_table *t, uint64_t palette, unsigned verts) {
    const oot_wear_model *found = NULL;
    for (unsigned i = 0; i < t->n; i++) {
        const oot_wear_model *m = &t->m[i];
        if (m->palette != palette) continue;
        for (unsigned s = 0; s < m->shapes; s++) {
            if (m->verts[s] != verts) continue;
            if (m->kind != OOT_WEAR_WORN) return m;   /* his own or carried: that wins */
            if (!found) found = m;
        }
    }
    return found;
}
