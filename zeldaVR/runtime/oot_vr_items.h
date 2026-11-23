/* The two-handed things of Ocarina of Time, from their own models (without the runtime). The slingshot's pouch is the bone Ph_Belt,
   its elastic the chains Ph_Belt_1 / Ph_String_1..3 out to the fork's tips; the bow's string is String_2 at the nock and String_1 at
   each tip. Each palette entry gets its share of the drawing hand: 0 at the wood, 1 at the pouch or nock, in proportion along the
   string, so that a straight string stays straight. */
#pragma once
#include <stdlib.h>
#include <string.h>
#include "oot_bfres.h"
#include "oot_vr_hold.h"

enum { OOT_ITEM_MATRICES = 64, OOT_ITEM_SHAPES = 16 };

typedef struct {
    unsigned palette_bytes, matrices, shapes, root_entry, center_entry;
    unsigned verts[OOT_ITEM_SHAPES];
    float center_rest[3], share[OOT_ITEM_MATRICES];
} oot_item_rig;

static inline int oot_item_side(const char *name) {
    const size_t n = strlen(name);
    if (n < 2 || name[n - 2] != '_') return -1;
    return name[n - 1] == 'L' ? 0 : name[n - 1] == 'R' ? 1 : -1;
}
static inline int oot_item_on_string(const char *name) {
    return !strncmp(name, "Ph_String_", 10) || !strncmp(name, "String_", 7) || !strncmp(name, "Ph_Belt_", 8);
}
static inline void oot_item_at(const float *world, int bone, float out[3]) {
    out[0] = world[bone * 12 + 3]; out[1] = world[bone * 12 + 7]; out[2] = world[bone * 12 + 11];
}
static inline int oot_item_entry(const oot_bfres *m, int bone) {
    oot_bfres_bone b;
    if (bone < 0 || oot_bfres_bone_at(m, (unsigned)bone, &b)) return -1;
    return b.smooth >= 0 ? b.smooth : b.rigid;
}
static inline const char *oot_item_shares(const oot_bfres *m, const float *world, int belt, const int anchor[2], const int center[2], oot_item_rig *out) {
    float tip[2][3], middle[2][3];
    for (int s = 0; s < 2; s++) { oot_item_at(world, anchor[s], tip[s]); oot_item_at(world, center[s], middle[s]); }
    for (unsigned b = 0; b < m->bone_count; b++) {
        oot_bfres_bone bone; const char *e = oot_bfres_bone_at(m, b, &bone);
        if (e) return e;
        float share = 0, at[3]; oot_item_at(world, (int)b, at);
        const int side = oot_item_side(bone.name);
        if ((int)b == belt || !strcmp(bone.name, "Ph_DekuSeed_1")) share = 1;
        else if (side >= 0 && oot_item_on_string(bone.name)) share = oot_hold_share(at, tip[side], middle[side]);
        if (bone.smooth >= 0 && (unsigned)bone.smooth < out->matrices) out->share[bone.smooth] = share;
        if (bone.rigid >= 0 && (unsigned)bone.rigid < out->matrices) out->share[bone.rigid] = share;
    }
    return NULL;
}
/* A model with a string. An error for one without. */
static inline const char *oot_item_prepare(const oot_bfres *m, oot_item_rig *out) {
    memset(out, 0, sizeof *out);
    out->matrices = oot_bfres_matrix_count(m);
    if (!out->matrices || out->matrices > OOT_ITEM_MATRICES) return "unexpected number of matrices";
    out->palette_bytes = out->matrices * 48;
    const int root = oot_bfres_find_bone(m, "Skl_Root"), belt = oot_bfres_find_bone(m, "Ph_Belt");
    const int anchor[2] = { oot_bfres_find_bone(m, belt >= 0 ? "Ph_String_3_L" : "String_1_L"), oot_bfres_find_bone(m, belt >= 0 ? "Ph_String_3_R" : "String_1_R") };
    const int center[2] = { belt >= 0 ? belt : oot_bfres_find_bone(m, "String_2_L"), belt >= 0 ? belt : oot_bfres_find_bone(m, "String_2_R") };
    if (root < 0 || anchor[0] < 0 || anchor[1] < 0 || center[0] < 0 || center[1] < 0) return "no string in this model";
    const int root_entry = oot_item_entry(m, root), center_entry = oot_item_entry(m, center[0]);
    if (root_entry < 0 || center_entry < 0) return "the string has no palette entry";
    out->root_entry = (unsigned)root_entry; out->center_entry = (unsigned)center_entry;
    out->shapes = m->shape_count < OOT_ITEM_SHAPES ? m->shape_count : OOT_ITEM_SHAPES;
    for (unsigned s = 0; s < out->shapes; s++) {
        oot_bfres_shape shape; const char *e = oot_bfres_shape_at(m, s, &shape);
        if (e) return e;
        out->verts[s] = shape.vertex_count;
    }
    float *world = malloc((size_t)m->bone_count * 48);
    if (!world) return "out of memory";
    const char *e = oot_bfres_rest_world(m, world, m->bone_count);
    if (!e) { oot_item_at(world, center[0], out->center_rest); e = oot_item_shares(m, world, belt, anchor, center, out); }
    free(world);
    return e;
}
/* How far the game has drawn the string: the pouch or nock against where the wood would carry it. */
static inline float oot_item_drawn(const oot_item_rig *r, const uint8_t *palette, float reach) {
    float root[12], center[12], a[3], b[3];
    memcpy(root, palette + (size_t)r->root_entry * 48, 48); memcpy(center, palette + (size_t)r->center_entry * 48, 48);
    oot_m_apply(root, r->center_rest, a); oot_m_apply(center, r->center_rest, b);
    const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    return oot_hold_drawn(sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), reach);
}
