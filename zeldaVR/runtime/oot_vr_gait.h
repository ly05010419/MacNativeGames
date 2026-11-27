/* Ocarina of Time in VR, first person: the legs step the way Link really goes (tests/test_oot_vr_gait.c runs it).
   The game turns Link toward where he runs; the first-person hips face where the player looks, so running sideways or backwards
   the game's own legs would run ahead of a body that slides. The game also has side, diagonal and back steps (its lock-on cycles):
   gait-<age>.bin, written at packaging from the player's own animation files (games/oot/scripts/make_vr_gait.py), holds them
   with its forward walk, jog and run and its standing pose. Each frame the two cycles nearest Link's direction of travel
   (relative to the hips) are blended, the one nearest his speed in each direction, and his standing pose as he stops. All
   cycles share one phase, counted from the left foot's mid-stance, which moves on by the distance he went over the length of a
   cycle's stride, so the feet keep pace with the ground. The idea follows HookshotVR's notes (ideas only, GPL-3); the file
   layout, the choice and the blend are ours.
   File: "OOTGAIT1", u32 version 1, bones, clips, samples, step; bones x {char[48] name, i16 parent (in this list, -1 the model),
   2 pad}; clips x {char[32] name, u32 frames, first, count, f32 travel x, travel z, stance}; samples x bones x 10 floats
   (quaternion x y z w, translation, scale). */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

enum { OOT_GAIT_BONES = 128, OOT_GAIT_CLIPS = 32, OOT_GAIT_FLOATS = 10, OOT_GAIT_BONE_BYTES = 52, OOT_GAIT_CLIP_BYTES = 56 };

typedef struct { char name[33]; unsigned frames, first, count; float travel[2], stance, speed, dir; } oot_gait_clip;
typedef struct {
    const uint8_t *data; size_t size;
    unsigned bones, clips, samples, step, stand;
    char names[OOT_GAIT_BONES][49];
    int parent[OOT_GAIT_BONES];
    oot_gait_clip clip[OOT_GAIT_CLIPS];
    const uint8_t *rows;
} oot_gait;

static inline uint32_t oot_gait__u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline float oot_gait__f32(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }

/* Opens a gait file (kept by the caller while g is used); NULL or why it is refused. */
static inline const char *oot_gait_open(oot_gait *g, const void *data, size_t size) {
    memset(g, 0, sizeof *g);
    const uint8_t *d = (const uint8_t *)data;
    if (!d || size < 28 || memcmp(d, "OOTGAIT1", 8)) return "not a gait file";
    if (oot_gait__u32(d + 8) != 1) return "an unknown gait file version";
    g->bones = oot_gait__u32(d + 12); g->clips = oot_gait__u32(d + 16); g->samples = oot_gait__u32(d + 20); g->step = oot_gait__u32(d + 24);
    if (!g->bones || g->bones > OOT_GAIT_BONES || !g->clips || g->clips > OOT_GAIT_CLIPS || !g->samples || !g->step || g->step > 8) return "gait counts out of range";
    const uint64_t rows_at = 28 + (uint64_t)g->bones * OOT_GAIT_BONE_BYTES + (uint64_t)g->clips * OOT_GAIT_CLIP_BYTES;
    if (rows_at + (uint64_t)g->samples * g->bones * OOT_GAIT_FLOATS * 4 != size) return "the gait file's size does not match its counts";
    for (unsigned i = 0; i < g->bones; i++) {
        const uint8_t *b = d + 28 + (size_t)i * OOT_GAIT_BONE_BYTES;
        if (memchr(b, 0, 48) == NULL) return "a gait bone name is not ended";
        memcpy(g->names[i], b, 48); g->names[i][48] = 0;
        int16_t parent; memcpy(&parent, b + 48, 2);
        if ((i == 0) != (parent == -1) || parent >= (int)i) return "a gait bone's parent does not come before it";
        g->parent[i] = parent;
    }
    g->stand = UINT32_MAX;
    for (unsigned c = 0; c < g->clips; c++) {
        const uint8_t *b = d + 28 + (size_t)g->bones * OOT_GAIT_BONE_BYTES + (size_t)c * OOT_GAIT_CLIP_BYTES;
        oot_gait_clip *k = &g->clip[c];
        if (memchr(b, 0, 32) == NULL) return "a gait clip name is not ended";
        memcpy(k->name, b, 32); k->name[32] = 0;
        k->frames = oot_gait__u32(b + 32); k->first = oot_gait__u32(b + 36); k->count = oot_gait__u32(b + 40);
        k->travel[0] = oot_gait__f32(b + 44); k->travel[1] = oot_gait__f32(b + 48); k->stance = oot_gait__f32(b + 52);
        if (!k->count || k->first > g->samples || k->count > g->samples - k->first || !k->frames) return "a gait clip's samples are outside the file";
        if (!isfinite(k->travel[0]) || !isfinite(k->travel[1]) || !(k->stance >= 0 && k->stance <= 1)) return "a gait clip's travel is not finite";
        const float length = hypotf(k->travel[0], k->travel[1]);
        k->speed = length * 60.0f / (float)k->frames;   /* m/s at the game's 60 frames a second (unconfirmed: the clips' own rate) */
        k->dir = atan2f(k->travel[0], k->travel[1]);   /* 0 ahead (+z), +pi/2 toward +x (Link's left) */
        if (length < 1e-3f && g->stand == UINT32_MAX) g->stand = c;
    }
    if (g->stand == UINT32_MAX) return "the gait file has no standing pose";
    g->data = d; g->size = size; g->rows = d + rows_at;
    for (uint64_t i = 0; i < (uint64_t)g->samples * g->bones * OOT_GAIT_FLOATS; i++) if (!isfinite(oot_gait__f32(g->rows + i * 4))) { g->data = NULL; return "a gait value is not finite"; }
    return NULL;
}
static inline int oot_gait_find_clip(const oot_gait *g, const char *name) {
    for (unsigned c = 0; c < g->clips; c++) if (!strcmp(g->clip[c].name, name)) return (int)c;
    return -1;
}
static inline int oot_gait_find_bone(const oot_gait *g, const char *name) {
    for (unsigned b = 0; b < g->bones; b++) if (!strcmp(g->names[b], name)) return (int)b;
    return -1;
}

/* Which cycles this frame: up to two moving ones (by direction) with their weights, and how much of the walk there is at all
   (the rest is the standing pose). vx, vz: Link's travel in the hips' frame (m/s, +z ahead, +x his left). */
typedef struct { int clip[2]; float weight[2], moving, stride; } oot_gait_pick;
static inline float oot_gait__wrap(float a) { while (a > (float)M_PI) a -= 2 * (float)M_PI; while (a <= -(float)M_PI) a += 2 * (float)M_PI; return a; }
static inline int oot_gait__nearest_speed(const oot_gait *g, float dir, float speed) {
    int best = -1; float best_cost = 1e9f;
    for (unsigned c = 0; c < g->clips; c++) {
        const oot_gait_clip *k = &g->clip[c];
        if (c == g->stand || k->speed <= 0 || fabsf(oot_gait__wrap(k->dir - dir)) > 0.05f) continue;
        const float cost = fabsf(logf(fmaxf(speed, 0.05f) / k->speed));
        if (cost < best_cost) { best_cost = cost; best = (int)c; }
    }
    return best;
}
static inline void oot_gait_choose(const oot_gait *g, float vx, float vz, oot_gait_pick *out) {
    memset(out, 0, sizeof *out); out->clip[0] = out->clip[1] = -1;
    const float speed = hypotf(vx, vz);
    out->moving = speed <= 0.15f ? 0 : speed >= 0.6f ? 1 : (speed - 0.15f) / 0.45f;
    if (!(speed > 1e-3f)) return;
    const float want = atan2f(vx, vz);
    /* the nearest direction the clips have on each side of the wanted one */
    float below = -1e9f, above = 1e9f; int lo = -1, hi = -1;
    for (unsigned c = 0; c < g->clips; c++) {
        if (c == g->stand || g->clip[c].speed <= 0) continue;
        const float d = oot_gait__wrap(g->clip[c].dir - want);
        if (d <= 0 && d > below) { below = d; lo = (int)c; }
        if (d >= 0 && d < above) { above = d; hi = (int)c; }
    }
    if (lo < 0 && hi < 0) { out->moving = 0; return; }
    if (lo < 0) { lo = hi; below = above; }
    if (hi < 0) { hi = lo; above = below; }
    const int a = oot_gait__nearest_speed(g, g->clip[lo].dir, speed), b = oot_gait__nearest_speed(g, g->clip[hi].dir, speed);
    const float span = above - below, wb = span > 1e-4f ? -below / span : 0;
    out->clip[0] = a; out->weight[0] = 1 - wb; out->clip[1] = b; out->weight[1] = wb;
    if (a < 0 || b < 0) { out->moving = 0; return; }
    out->stride = out->weight[0] * hypotf(g->clip[a].travel[0], g->clip[a].travel[1]) + out->weight[1] * hypotf(g->clip[b].travel[0], g->clip[b].travel[1]);
}
/* The shared phase moves on by the distance gone over the stride (0..1, the left foot mid-stance at 0). */
static inline float oot_gait_advance(float phase, float distance, float stride) {
    if (!(stride > 0.05f) || !(distance >= 0) || !isfinite(distance)) return phase;
    phase += distance / stride;
    phase -= floorf(phase);
    return phase >= 1 ? 0 : phase;
}

/* One bone's local pose: quaternion, translation, scale. */
typedef struct { float q[4], t[3], s[3]; } oot_gait_local;
static inline void oot_gait__row(const oot_gait *g, unsigned sample, unsigned bone, oot_gait_local *o) {
    const uint8_t *p = g->rows + ((size_t)sample * g->bones + bone) * OOT_GAIT_FLOATS * 4;
    float v[OOT_GAIT_FLOATS]; memcpy(v, p, sizeof v);
    memcpy(o->q, v, 16); memcpy(o->t, v + 4, 12); memcpy(o->s, v + 7, 12);
}
static inline void oot_gait__mix(oot_gait_local *a, const oot_gait_local *b, float w) {   /* a toward b by w (quaternions by the shorter way) */
    if (w <= 0) return;
    const float sign = a->q[0] * b->q[0] + a->q[1] * b->q[1] + a->q[2] * b->q[2] + a->q[3] * b->q[3] < 0 ? -1.f : 1.f;
    float n = 0;
    for (int i = 0; i < 4; i++) { a->q[i] += (sign * b->q[i] - a->q[i]) * w; n += a->q[i] * a->q[i]; }
    n = n > 1e-12f ? 1 / sqrtf(n) : 0;
    for (int i = 0; i < 4; i++) a->q[i] *= n;
    if (n == 0) a->q[3] = 1;
    for (int i = 0; i < 3; i++) { a->t[i] += (b->t[i] - a->t[i]) * w; a->s[i] += (b->s[i] - a->s[i]) * w; }
}
/* A clip's pose at the shared phase. */
static inline void oot_gait_clip_pose(const oot_gait *g, unsigned clip, float phase, oot_gait_local *out) {
    const oot_gait_clip *k = &g->clip[clip];
    float at = phase + k->stance; at -= floorf(at); at *= (float)k->count;
    unsigned i0 = (unsigned)at; if (i0 >= k->count) i0 = k->count - 1;
    const unsigned i1 = (i0 + 1) % k->count; const float t = at - (float)i0;
    for (unsigned b = 0; b < g->bones; b++) {
        oot_gait_local next; oot_gait__row(g, k->first + i0, b, &out[b]); oot_gait__row(g, k->first + i1, b, &next);
        oot_gait__mix(&out[b], &next, t);
    }
}
/* The pose of this frame: the picked cycles blended, then toward standing as far as Link is not moving. */
static inline void oot_gait_pose(const oot_gait *g, const oot_gait_pick *p, float phase, oot_gait_local *out, oot_gait_local *scratch) {
    oot_gait_clip_pose(g, g->stand, 0, out);
    if (!(p->moving > 0) || p->clip[0] < 0 || p->clip[1] < 0) return;
    oot_gait_clip_pose(g, (unsigned)p->clip[0], phase, scratch);
    if (p->weight[1] > 0 && p->clip[1] != p->clip[0]) {
        oot_gait_local *other = scratch + g->bones;
        oot_gait_clip_pose(g, (unsigned)p->clip[1], phase, other);
        for (unsigned b = 0; b < g->bones; b++) oot_gait__mix(&scratch[b], &other[b], p->weight[1]);
    }
    for (unsigned b = 0; b < g->bones; b++) oot_gait__mix(&out[b], &scratch[b], p->moving);
}
/* Model-space matrices (3x4, rows) of the posed bones. A bone's scale is its own (its children do not take it), as the game's
   skeleton does. root: the model-space matrix of the first bone's parent (Root's rest). */
static inline void oot_gait_world(const oot_gait *g, const oot_gait_local *pose, const float root[12], float *world, float *frame) {
    for (unsigned b = 0; b < g->bones; b++) {
        const float *q = pose[b].q, *t = pose[b].t, *s = pose[b].s;
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float r[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                             2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                             2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) };
        const float local[12] = { r[0], r[1], r[2], t[0], r[3], r[4], r[5], t[1], r[6], r[7], r[8], t[2] };
        const float *parent = g->parent[b] < 0 ? root : frame + (size_t)g->parent[b] * 12;
        float *f = frame + (size_t)b * 12, *o = world + (size_t)b * 12;
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) f[4 * i + j] = parent[4 * i] * local[j] + parent[4 * i + 1] * local[4 + j] + parent[4 * i + 2] * local[8 + j];
            f[4 * i + 3] = parent[4 * i] * local[3] + parent[4 * i + 1] * local[7] + parent[4 * i + 2] * local[11] + parent[4 * i + 3];
        }
        for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) o[4 * i + j] = f[4 * i + j] * s[j]; o[4 * i + 3] = f[4 * i + 3]; }
    }
}
