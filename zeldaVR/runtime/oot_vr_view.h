/* Ocarina of Time's first-person view, without the runtime (tests/test_oot_vr_view.c runs it). The game's 3x4 view matrices
   are rows R|t, world to view, looking down -z. A level view of yaw a has right (cos a, 0, sin a) and forward
   (sin a, 0, -cos a).

   The camera stands at Link's eyes: his feet + the eye height + a little forward, so his head is behind the near plane.
   The game sorts what it draws with this view moved back along its sight line (its culling would otherwise drop the hands
   under and behind the eye); the true view goes back in where the game fills the views' data for the GPU, found by the
   exact matrix it was given (oot_cull_*). */
#pragma once
#include <math.h>
#include <string.h>
#include "vr_view_math.h"

static inline float oot_wrap_pi(float a) {
    if (!isfinite(a)) return 0;
    a = fmodf(a + (float)M_PI, 2 * (float)M_PI);
    if (a < 0) a += 2 * (float)M_PI;
    return a - (float)M_PI;
}
static inline float oot_view_yaw(const float m[12]) { return atan2f(m[2], m[0]); }
static inline void oot_level_rows(float yaw, float m[12]) {
    const float c = cosf(yaw), s = sinf(yaw);
    const float r[12] = { c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0 };
    memcpy(m, r, sizeof r);
}

typedef struct {
    float feet[3];     /* Link's feet in the world */
    float yaw;         /* the first-person heading */
    float blend;       /* 0: the game's camera (level), 1: the eyes */
    float eye;         /* eye height above the feet */
    float forward;     /* along the heading */
    float off[3];      /* the head's own movement and the settings' offset, in world axes */
} oot_fp_input;

/* The view before the head's rotation. game: the game's view, levelled. */
static inline void oot_fp_base(const float game[12], const oot_fp_input *in, float out[12]) {
    const float gy = oot_view_yaw(game), yaw = gy + oot_wrap_pi(in->yaw - gy) * in->blend;
    float third[3]; vr_view_eye(game, third);
    const float fx = sinf(in->yaw), fz = -cosf(in->yaw);
    const float eyes[3] = { in->feet[0] + fx * in->forward, in->feet[1] + in->eye, in->feet[2] + fz * in->forward };
    float p[3];
    for (int i = 0; i < 3; i++) p[i] = third[i] + (eyes[i] - third[i]) * in->blend + in->off[i];
    oot_level_rows(yaw, out);
    vr_view_translation(out, p);
}

/* The camera moved back along its sight line: only the z translation changes. */
static inline void oot_cull_view(const float view[12], float back, float out[12]) {
    memcpy(out, view, 12 * sizeof(float));
    out[11] -= back;
}

enum { OOT_CULL_RING = 64 };
typedef struct { float culled[12]; float true_z; int used; } oot_cull_entry;
typedef struct { oot_cull_entry e[OOT_CULL_RING]; unsigned next; } oot_cull_ring;

static inline void oot_cull_put(oot_cull_ring *r, const float culled[12], float true_z) {
    oot_cull_entry *x = &r->e[r->next++ % OOT_CULL_RING];
    memcpy(x->culled, culled, sizeof x->culled);
    x->true_z = true_z;
    x->used = 1;
}
/* The true z translation of a view the game was given; 0 when this matrix is not one of ours. */
static inline int oot_cull_find(const oot_cull_ring *r, const float view[12], float *true_z) {
    for (unsigned k = 0; k < OOT_CULL_RING; k++) {
        const oot_cull_entry *x = &r->e[(r->next - 1 - k) % OOT_CULL_RING];
        if (!x->used) continue;
        int same = fabsf(x->culled[11] - view[11]) < 1e-4f;
        for (int i = 0; same && i < 11; i++) same = fabsf(x->culled[i] - view[i]) < 1e-6f;
        if (same) { *true_z = x->true_z; return 1; }
    }
    return 0;
}
