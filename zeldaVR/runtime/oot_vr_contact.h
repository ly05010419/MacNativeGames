/* Ocarina of Time in VR: the blade's own contact with what is near Link, and a hit in the air, without the runtime
   (tests/test_oot_vr_contact.c runs it). vr.md rows 44 and 47; the rules are HookshotVR's (held-items.md 1.6-1.8; GPL-3: ideas and
   numbers only, no code).

   Targets (row 44): actors seen in their own updates, within 4 m of Link level and 4 m up or down, whose name gives them a shape:
   an upright capsule from `low` to `high` above the actor's origin with a radius (HookshotVR's own table, first prefix that matches;
   names with Door, Shutter, Wall, Area or _Dictionary are none). Up to 48; a slot is taken again when empty or not seen for 2 s.
   Names already found to be no target are remembered in a 256-entry table by address, so a name is read once.
   Contact: the blade (a capsule along the weapon actor's z, from `from` to `to`, `radius` thick) is swept from its last place to
   this one in 8 steps; the step that goes deepest into a target's capsule is the contact, and the actor may be held there. Only a
   pose under 100 ms old is swept from. A target touched is locked: no new contact on it until the blade has been more than 0.10 m
   out of it and a new stroke has begun out there; a stroke begun inside a locked target sets nothing up.
   The blade's reach is ours (OOT_TOUCH_BLADE_*), one for every weapon: the per-weapon blade lengths HookshotVR uses come from the
   game's own model boxes and are not kept here; the game's own attack sensor (0.8 m and longer for the swords) still finds what
   lies beyond it, as it does for a stroke with no contact.
   In the air (row 47): Link's vertical speed from his place at each of his updates, smoothed over 50 ms (updates under 1 ms apart
   left out, a gap of 250 ms starts over at 0). Faster than 1.5 m/s up or down, a one-handed sword's hit is the jump slash's kind 22
   (the game's own jump slash with the Kokiri Sword, HookshotVR's headset log); the Deku Stick's and the large swords' jump kinds
   were never seen, so they keep their own (101, 13). Times in nanoseconds, lengths in metres (the game's world units). */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

enum { OOT_TARGETS = 48, OOT_NOT_TARGETS = 256, OOT_TOUCH_STEPS = 8, OOT_TARGET_NAME = 40 };
static const float OOT_TARGET_REACH = 4.0f;
static const uint64_t OOT_TARGET_STALE_NS = 2000000000ull;
static const uint64_t OOT_TOUCH_FRESH_NS = 100000000ull;
static const float OOT_TOUCH_CLEAR = 0.10f;
static const float OOT_TOUCH_BLADE_FROM = 0.05f, OOT_TOUCH_BLADE_TO = 0.60f, OOT_TOUCH_BLADE_RADIUS = 0.05f;

typedef struct { float low, high, radius; } oot_target_shape;
typedef struct { uintptr_t actor; float pos[3]; oot_target_shape shape; uint64_t seen; int locked, cleared; char name[OOT_TARGET_NAME]; } oot_target;
typedef struct { oot_target t[OOT_TARGETS]; uintptr_t not_target[OOT_NOT_TARGETS]; } oot_targets;
typedef struct { float from, to, radius; } oot_blade_shape;
typedef struct { int index; float t, gap, along; } oot_contact;

/* The shape a name gives, 0: no target. */
static inline int oot_target_shape_of(const char *name, oot_target_shape *s) {
    static const char *const never[] = { "Door", "Shutter", "Wall", "Area", "_Dictionary" };
    static const struct { const char *prefix; float low, high, radius; } shapes[] = {
        { "Enemy_Dekubaba", 0.0f, 1.3f, 0.30f }, { "Enemy_Skullwalltula", -0.4f, 0.4f, 0.40f }, { "Enemy_Skulltula", -0.5f, 0.5f, 0.45f },
        { "Enemy_", -0.2f, 1.6f, 0.50f }, { "Animal_", 0.0f, 0.5f, 0.35f }, { "Obj_", 0.0f, 0.8f, 0.40f }, { "FldObj_", 0.0f, 0.8f, 0.40f } };
    if (!name || !*name) return 0;
    for (size_t i = 0; i < sizeof never / sizeof *never; i++) if (strstr(name, never[i])) return 0;
    for (size_t i = 0; i < sizeof shapes / sizeof *shapes; i++)
        if (!strncmp(name, shapes[i].prefix, strlen(shapes[i].prefix))) { s->low = shapes[i].low; s->high = shapes[i].high; s->radius = shapes[i].radius; return 1; }
    return 0;
}
static inline int oot_target_near(const float link[3], const float pos[3]) {
    const float dx = pos[0] - link[0], dy = pos[1] - link[1], dz = pos[2] - link[2];
    return isfinite(dx) && isfinite(dy) && isfinite(dz) && dx * dx + dz * dz <= OOT_TARGET_REACH * OOT_TARGET_REACH && fabsf(dy) <= OOT_TARGET_REACH;
}
static inline unsigned oot_not_target_slot(uintptr_t actor) { return (unsigned)((actor >> 4) % OOT_NOT_TARGETS); }
static inline int oot_targets_known_not(const oot_targets *ts, uintptr_t actor) { return actor && ts->not_target[oot_not_target_slot(actor)] == actor; }
static inline void oot_targets_mark_not(oot_targets *ts, uintptr_t actor) { ts->not_target[oot_not_target_slot(actor)] = actor; }
static inline oot_target *oot_targets_find(oot_targets *ts, uintptr_t actor) {
    for (int i = 0; i < OOT_TARGETS; i++) if (actor && ts->t[i].actor == actor) return &ts->t[i];
    return NULL;
}
/* A target seen in its own update: its slot refreshed, or a new one taken (empty or stale). NULL: the table is full. */
static inline oot_target *oot_targets_seen(oot_targets *ts, uintptr_t actor, const float pos[3], const oot_target_shape *shape, const char *name, uint64_t now) {
    oot_target *e = oot_targets_find(ts, actor);
    if (!e) {
        for (int i = 0; i < OOT_TARGETS && !e; i++) if (!ts->t[i].actor || now - ts->t[i].seen > OOT_TARGET_STALE_NS) e = &ts->t[i];
        if (!e) return NULL;
        memset(e, 0, sizeof *e);
        e->actor = actor; e->shape = *shape;
        if (name) { strncpy(e->name, name, OOT_TARGET_NAME - 1); e->name[OOT_TARGET_NAME - 1] = 0; }
    }
    memcpy(e->pos, pos, sizeof e->pos); e->seen = now;
    return e;
}

static inline float oot_touch_clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
/* The closest distance between segments p0-p1 and q0-q1; s: where on the first (0..1). */
static inline float oot_seg_dist(const float p0[3], const float p1[3], const float q0[3], const float q1[3], float *s_out) {
    float d1[3], d2[3], r[3];
    for (int i = 0; i < 3; i++) { d1[i] = p1[i] - p0[i]; d2[i] = q1[i] - q0[i]; r[i] = p0[i] - q0[i]; }
    const float a = d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2], e = d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2];
    const float f = d2[0] * r[0] + d2[1] * r[1] + d2[2] * r[2];
    float s = 0, t = 0;
    if (a <= 1e-12f && e <= 1e-12f) s = t = 0;
    else if (a <= 1e-12f) { s = 0; t = oot_touch_clamp01(f / e); }
    else {
        const float c = d1[0] * r[0] + d1[1] * r[1] + d1[2] * r[2];
        if (e <= 1e-12f) { t = 0; s = oot_touch_clamp01(-c / a); }
        else {
            const float b = d1[0] * d2[0] + d1[1] * d2[1] + d1[2] * d2[2], den = a * e - b * b;
            s = den > 1e-12f ? oot_touch_clamp01((b * f - c * e) / den) : 0;
            t = (b * s + f) / e;
            if (t < 0) { t = 0; s = oot_touch_clamp01(-c / a); }
            else if (t > 1) { t = 1; s = oot_touch_clamp01((b - c) / a); }
        }
    }
    float d = 0;
    for (int i = 0; i < 3; i++) { const float k = (p0[i] + d1[i] * s) - (q0[i] + d2[i] * t); d += k * k; }
    if (s_out) *s_out = s;
    return sqrtf(d);
}
/* The blade's segment for a weapon pose (rows R|t: its z is column 2, its origin column 3). */
static inline int oot_blade_segment(const float m[12], const oot_blade_shape *b, float p0[3], float p1[3]) {
    float z[3] = { m[2], m[6], m[10] };
    const float n = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (!(n > 1e-4f) || !isfinite(n)) return 0;
    for (int i = 0; i < 3; i++) { z[i] /= n; p0[i] = m[4 * i + 3] + z[i] * b->from; p1[i] = m[4 * i + 3] + z[i] * b->to; }
    return 1;
}
/* How far the blade at pose m is from a target's capsule, past both radii (< 0: inside); along: where on the blade (m). */
static inline float oot_blade_gap(const float m[12], const oot_blade_shape *b, const oot_target *t, float *along) {
    float p0[3], p1[3], s = 0;
    if (!oot_blade_segment(m, b, p0, p1)) return INFINITY;
    const float q0[3] = { t->pos[0], t->pos[1] + t->shape.low, t->pos[2] }, q1[3] = { t->pos[0], t->pos[1] + t->shape.high, t->pos[2] };
    const float d = oot_seg_dist(p0, p1, q0, q1, &s);
    if (along) *along = b->from + (b->to - b->from) * s;
    return d - b->radius - t->shape.radius;
}
/* The pose between a and b at t: the place in a line, the axes in a line and made square again (z first). */
static inline void oot_touch_pose(const float a[12], const float b[12], float t, float out[12]) {
    float z[3], y[3], x[3];
    for (int i = 0; i < 3; i++) {
        out[4 * i + 3] = a[4 * i + 3] + (b[4 * i + 3] - a[4 * i + 3]) * t;
        z[i] = a[4 * i + 2] + (b[4 * i + 2] - a[4 * i + 2]) * t;
        y[i] = a[4 * i + 1] + (b[4 * i + 1] - a[4 * i + 1]) * t;
    }
    float n = sqrtf(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
    if (!(n > 1e-6f)) { memcpy(out, t < 0.5f ? a : b, 48); return; }
    for (int i = 0; i < 3; i++) z[i] /= n;
    const float k = y[0] * z[0] + y[1] * z[1] + y[2] * z[2];
    for (int i = 0; i < 3; i++) y[i] -= z[i] * k;
    n = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (!(n > 1e-6f)) { memcpy(out, t < 0.5f ? a : b, 48); return; }
    for (int i = 0; i < 3; i++) y[i] /= n;
    x[0] = y[1] * z[2] - y[2] * z[1]; x[1] = y[2] * z[0] - y[0] * z[2]; x[2] = y[0] * z[1] - y[1] * z[0];
    for (int i = 0; i < 3; i++) { out[4 * i] = x[i]; out[4 * i + 1] = y[i]; out[4 * i + 2] = z[i]; }
}
/* One new weapon pose. Every locked target the blade is now clear of (by more than 0.10 m) is marked cleared. With `live` (a
   stroke's attack is on), the deepest contact of the sweep a -> b with a target not locked and seen these last 2 s: its index,
   -1 none. *locked_touch: a locked target was touched (refused). */
static inline int oot_targets_sweep(oot_targets *ts, const float a[12], const float b[12], const oot_blade_shape *blade, uint64_t now, int live,
                                    oot_contact *best, int *locked_touch) {
    best->index = -1; best->gap = INFINITY; best->t = 1; best->along = 0;
    if (locked_touch) *locked_touch = 0;
    for (int i = 0; i < OOT_TARGETS; i++) {
        oot_target *t = &ts->t[i];
        if (!t->actor || now - t->seen > OOT_TARGET_STALE_NS) continue;
        if (t->locked && oot_blade_gap(b, blade, t, NULL) > OOT_TOUCH_CLEAR) t->cleared = 1;
        if (!live) continue;
        for (int k = 1; k <= OOT_TOUCH_STEPS; k++) {
            const float step = (float)k / OOT_TOUCH_STEPS;
            float pose[12], along = 0;
            oot_touch_pose(a, b, step, pose);
            const float gap = oot_blade_gap(pose, blade, t, &along);
            if (!(gap < 0)) continue;
            if (t->locked) { if (locked_touch) *locked_touch = 1; break; }
            if (gap < best->gap) { best->index = i; best->gap = gap; best->t = step; best->along = along; }
        }
    }
    return best->index;
}
/* A new stroke begins with the blade at pose m: a locked target cleared and now out of reach is free again; 1 when the blade is
   inside one still locked (the stroke sets nothing up). */
static inline int oot_targets_stroke_begins(oot_targets *ts, const float m[12], const oot_blade_shape *blade, uint64_t now) {
    int inside = 0;
    for (int i = 0; i < OOT_TARGETS; i++) {
        oot_target *t = &ts->t[i];
        if (!t->actor || !t->locked || now - t->seen > OOT_TARGET_STALE_NS) continue;
        const float gap = oot_blade_gap(m, blade, t, NULL);
        if (t->cleared && gap > OOT_TOUCH_CLEAR) { t->locked = 0; t->cleared = 0; }
        else if (gap < 0) inside = 1;
    }
    return inside;
}

/* Row 47: Link's vertical speed. */
static const float OOT_AIR_VY = 1.5f;
static const uint64_t OOT_AIR_GAP_NS = 250000000ull, OOT_AIR_MIN_NS = 1000000ull;
static const float OOT_AIR_SMOOTH_S = 0.05f;
typedef struct { int have; float y, vy; uint64_t at; } oot_air;
static inline void oot_air_step(oot_air *a, float y, uint64_t now) {
    if (!isfinite(y)) return;
    if (!a->have || now < a->at || now - a->at >= OOT_AIR_GAP_NS) { a->have = 1; a->y = y; a->vy = 0; a->at = now; return; }
    if (now - a->at < OOT_AIR_MIN_NS) return;
    const float dt = (float)(now - a->at) / 1e9f, raw = (y - a->y) / dt, k = fminf(1.0f, dt / OOT_AIR_SMOOTH_S);
    a->vy += (raw - a->vy) * k; a->y = y; a->at = now;
}
static inline int oot_air_borne(const oot_air *a) { return a->have && fabsf(a->vy) > OOT_AIR_VY; }
enum { OOT_JUMP_KIND = 22 };
/* The attack kind of a hit: the jump slash's in the air for a one-handed sword; otherwise the weapon's own. */
static inline uint32_t oot_hit_kind(uint32_t own, int one_handed, int airborne, int jump_hits) {
    return jump_hits && airborne && one_handed ? (uint32_t)OOT_JUMP_KIND : own;
}
