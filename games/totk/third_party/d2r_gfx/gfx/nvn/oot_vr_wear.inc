/* zeldaVR, Ocarina of Time, first person: what Link wears goes with the VR arms, or is left out, and a hand that holds something
   closes the way the game's own hand does (rows 34-37 of vr.md; facts from HookshotVR's notes and generator lists, held-items.md
   1.10 and 2.4; ideas only, GPL-3; the code is ours).
   - The Goron and Zora tunics (palettes of 120 and 117 matrices) and the silver and golden gauntlets (50 and 52) are models of
     their own, skinned on the same skeleton as adult Link and posed by the game with his arms. Each of their palette entries
     takes the matrix of the arm part its bone belongs to (the nearest named arm bone at or above it, by name, as his body's do),
     corrected from their rest pose to his; only their arm triangles are drawn, as his body's.
   - Child Link's Goron Bracelet is a rigid model of its own (one matrix) that the game puts on a forearm or wrist bone each frame:
     it is carried by as much as that bone moved from the game's pose to the VR one.
   - Bags, quivers, wallets, scales, pouch bottles, the Stone of Agony, boots, the Lens of Truth, hats and masks would hang off the
     first-person body where the game's pose puts them: first person leaves them out (option worn), never a size one of his own
     models has (his swords, shields, sheaths, what he carries).
   - Held things: the VR hand takes the game's own fingers, relative to the game's hand that holds the thing (option held_grip).
   All recognised by palette size and vertex count against the player's own models (extracted/oot/vr-rig, make_vr_rig.py);
   a model the App does not carry is not recognised, and that is logged once. Included by oot_vr_arms.inc. */
#include "../../../../zeldaVR/runtime/oot_vr_wear.h"
int vr_oot_wear(void);       /* totk_vr.m, option wear */
int vr_oot_worn(void);       /* totk_vr.m, option worn */
int vr_oot_held_grip(void);  /* totk_vr.m, option held_grip */

/* An entry the game scaled to nothing (a part it hides, as his default tunic's sleeves under another one): it stays hidden. */
static int oot_wear_collapsed(const uint8_t *entry) { float m[12]; memcpy(m, entry, 48); return oot_wear_scaled_away(m); }
/* This frame's arms, as his body's palette made them: the worn draws of the frame take theirs from here. */
typedef struct { uint64_t frame; int rig, have[2]; oot_arm_matrices done[2]; float off[3], game_fore[2][12], game_hand[2][12], crawl; } oot_arm_frame;
static oot_arm_frame g_oot_arm_frame = { .frame = UINT64_MAX };
/* Which game hand each VR hand holds a thing from these frames (oot_vr_props.inc oot_prop_palette): its grip is that hand's. */
static int g_oot_grip_from[2];
static uint64_t g_oot_grip_at[2];
static void oot_grip_note(int to, int from, uint64_t frame) { if (to >= 0 && to < 2 && from >= 0 && from < 2) { g_oot_grip_from[to] = from; g_oot_grip_at[to] = frame; } }

enum { OOT_WEARS = 4 };
static const char *const g_oot_wear_names[OOT_WEARS] = { "Armor_Adult_Goron", "Armor_Adult_Zora", "Armor_Adult_Gauntlet_Silver", "Armor_Adult_Gauntlet_Gold" };
typedef struct { oot_rig rig; oot_rig_part *parts; float (*fix)[12]; int ok; uint64_t drawn; } oot_wear;
static oot_wear g_oot_wear[OOT_WEARS];
static const char *const g_oot_worn_names[] = { "Item_BombBag_Small", "Item_BombBag_Medium", "Item_BombBag_Large", "Item_BulletBag_Small",
    "Item_BulletBag_Medium", "Item_BulletBag_Large", "Item_NutBag_Small", "Item_NutBag_Medium", "Item_NutBag_Large", "Item_StickBag_Small",
    "Item_StickBag_Medium", "Item_StickBag_Large", "Item_Quiver_Small", "Item_Quiver_Medium", "Item_Quiver_Large", "Item_Wallet_Small",
    "Item_Wallet_Medium", "Item_Wallet_Large", "Item_Scale_Silver", "Item_Scale_Gold", "Item_Bottle_Pouch", "Item_Bottle_Fairy_Pouch",
    "Item_Bottle_Poe_Pouch", "Item_AgonyStone", "Armor_Adult_HeavyBoot", "Armor_Adult_HoverBoot", "Armor_Adult_GlassTruth", "Armor_Child_GlassTruth",
    "Armor_Adult_Default_Head", "Armor_Child_Default_Head", "Armor_Child_Mask_Bunny", "Armor_Child_Mask_Gerudo", "Armor_Child_Mask_Goron",
    "Armor_Child_Mask_Keaton", "Armor_Child_Mask_Skull", "Armor_Child_Mask_Spooky", "Armor_Child_Mask_Truth", "Armor_Child_Mask_Zora",
    "Armor_Child_Mask_Amiibo0", "Armor_Child_Mask_Amiibo1" };
static const char *const g_oot_own_names[] = { "Weapon_Sword_Kokiri", "Weapon_Sword_Master", "Weapon_Sword_Biggoron", "Weapon_Shield_Deku",
    "Weapon_Shield_Hylian", "Weapon_Shield_Mirror", "Weapon_Sheath_Kokiri", "Weapon_Sheath_Master", "Weapon_Sheath_Biggoron", "Armor_Adult_Normal",
    "Weapon_Range_SlingShot", "Weapon_Range_Bow", "Weapon_Range_BowAndArrow_Normal", "Weapon_Range_BowAndArrow_Fire",
    "Weapon_Range_BowAndArrow_Ice", "Weapon_Range_BowAndArrow_Light" };
enum { OOT_WORN_NAMES = sizeof g_oot_worn_names / sizeof *g_oot_worn_names, OOT_WEAR_BRACELET = 100 };
static oot_wear_table g_oot_wear_table;
static uint8_t g_oot_worn_said[OOT_WORN_NAMES];
/* The bracelet: its palette slot is the first; the bones it may ride (left forearm, left hand, right forearm, right hand). */
static int g_oot_bracelet_rides = -1;
static int g_oot_fore_matrix[2][2] = { { -1, -1 }, { -1, -1 } };   /* [rig][side]: Arm_2's palette entry */
static int g_oot_finger_matrix[2][2][OOT_FINGERS][OOT_FINGER_JOINTS];   /* [rig][side][finger][joint], -1 none */
static int g_oot_grip_mirrors[2];                     /* [rig]: his hands mirror each other across x = 0 at rest */

/* A bone name of an arm part: its side and part (the names oot_arms_prepare tags in his body). */
static int oot_wear_tag(const char *name, size_t n, int *side, int *part) {
    if (n < 3 || name[n - 2] != '_' || (name[n - 1] != 'L' && name[n - 1] != 'R')) return 0;
    *side = name[n - 1] == 'R';
    static const struct { const char *stem; int part; } named[] = {
        {"Clavicle", OOT_PART_CLAVICLE}, {"Arm_1", OOT_PART_UPPER}, {"Arm_2", OOT_PART_FORE},
        {"Ph_Wrist_Assist_A", OOT_PART_ASSIST_A}, {"Ph_Wrist_Assist_B", OOT_PART_ASSIST_B}, {"Wrist", OOT_PART_HAND} };
    for (size_t k = 0; k < sizeof named / sizeof *named; k++)
        if (strlen(named[k].stem) == n - 2 && !memcmp(name, named[k].stem, n - 2)) { *part = named[k].part; return 1; }
    if (n == 12 && !memcmp(name, "Finger_", 7) && name[7] >= 'A' && name[7] <= 'E' && name[8] == '_' && name[9] >= '1' && name[9] <= '3') {
        *part = OOT_PART_FINGER + (name[7] - 'A') * OOT_FINGER_JOINTS + (name[9] - '1'); return 1; }
    return 0;
}
/* A tunic's or gauntlets' entries against adult Link's body: the part of each and its rest pose taken to his. */
static const char *oot_wear_prepare(oot_wear *w, const oot_rig *body) {
    const oot_bfres *m = &w->rig.m, *b = &body->m;
    const unsigned nm = oot_bfres_matrix_count(m);
    float *rest = malloc((size_t)m->bone_count * 48), *brest = malloc((size_t)b->bone_count * 48);
    w->parts = calloc(nm, sizeof *w->parts); w->fix = calloc(nm, sizeof *w->fix);
    const char *e = !rest || !brest || !w->parts || !w->fix ? "out of memory" : oot_bfres_rest_world(m, rest, m->bone_count);
    if (!e) e = oot_bfres_rest_world(b, brest, b->bone_count);
    unsigned tagged = 0;
    for (unsigned k = 0; k < nm && !e; k++) {
        int bone = oot_bfres_matrix_bone(m, k), side = 0, part = 0;
        oot_bfres_bone x;
        while (bone >= 0 && !(e = oot_bfres_bone_at(m, (unsigned)bone, &x)) && !oot_wear_tag(x.name, x.name_len, &side, &part)) bone = x.parent;
        if (e || bone < 0) continue;
        const int twin = oot_bfres_find_bone(b, x.name);
        float inv[12];
        if (twin < 0 || !oot_wear_inverse(rest + (size_t)bone * 12, inv)) continue;
        oot_wear_mul(brest + (size_t)twin * 12, inv, w->fix[k]);   /* his rest pose of that bone x inverse(theirs) */
        if (k >= m->smooth_count) oot_wear_mul(w->fix[k], rest + (size_t)oot_bfres_matrix_bone(m, k) * 12, w->fix[k]);   /* a rigid entry is the bone's world */
        w->parts[k].side = (uint8_t)side; w->parts[k].part = (uint8_t)part; tagged++;
    }
    free(rest); free(brest);
    if (!e && !tagged) e = "no entry on an arm bone";
    return e;
}
static void oot_wear_add_file(const char *name, int kind, int index) {
    char path[160]; snprintf(path, sizeof path, "extracted/oot/vr-rig/%s.bfres", name);
    FILE *f = fopen(path, "rb");
    if (!f) { if (kind != OOT_WEAR_OWN) MLOG("[vr-worn] %s: no model in the App (one packaged before it: update it); not recognised", name); return; }
    fseek(f, 0, SEEK_END); const long size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *data = size > 0 && size <= (16L << 20) ? malloc((size_t)size) : NULL;
    const char *e = data && fread(data, 1, (size_t)size, f) == (size_t)size ? NULL : "cannot read it";
    fclose(f);
    oot_bfres m;
    if (!e) e = oot_bfres_open(&m, data, (size_t)size);
    unsigned verts[OOT_WEAR_SHAPES], n = 0;
    for (unsigned s = 0; !e && s < m.shape_count && n < OOT_WEAR_SHAPES; s++) { oot_bfres_shape sh; if (!oot_bfres_shape_at(&m, s, &sh)) verts[n++] = sh.vertex_count; }
    if (!e && !oot_wear_add(&g_oot_wear_table, (uint64_t)oot_bfres_matrix_count(&m) * 48, verts, n, kind, index)) e = "no shapes, or the table is full";
    if (e) MLOG("[vr-worn] %s: %s; not recognised", name, e);
    free(data);
}
/* Each hand's finger entries, and whether his two hands mirror each other at rest (the mirrored grip needs it). */
static void oot_grip_prepare(int ri) {
    const oot_bfres *m = &g_oot_rigs[ri].m;
    float left[1 + OOT_FINGERS * OOT_FINGER_JOINTS][3], right[1 + OOT_FINGERS * OOT_FINGER_JOINTS][3];
    float *rest = malloc((size_t)m->bone_count * 48);
    int n = 0, whole = rest && !oot_bfres_rest_world(m, rest, m->bone_count);
    for (int side = 0; side < 2; side++) {
        char name[24]; snprintf(name, sizeof name, "Arm_2_%c", side ? 'R' : 'L');
        oot_bfres_bone x; const int fore = oot_bfres_find_bone(m, name);
        g_oot_fore_matrix[ri][side] = fore >= 0 && !oot_bfres_bone_at(m, (unsigned)fore, &x) ? x.smooth : -1;
        snprintf(name, sizeof name, "Wrist_%c", side ? 'R' : 'L');
        const int wrist = oot_bfres_find_bone(m, name);
        if (wrist >= 0 && whole) memcpy(side ? right[0] : left[0], (float[3]){ rest[wrist * 12 + 3], rest[wrist * 12 + 7], rest[wrist * 12 + 11] }, 12);
        else whole = 0;
        for (int f = 0; f < OOT_FINGERS; f++) for (int j = 0; j < OOT_FINGER_JOINTS; j++) {
            snprintf(name, sizeof name, "Finger_%c_%d_%c", 'A' + f, j + 1, side ? 'R' : 'L');
            const int b = oot_bfres_find_bone(m, name);
            g_oot_finger_matrix[ri][side][f][j] = b >= 0 && !oot_bfres_bone_at(m, (unsigned)b, &x) ? x.smooth : -1;
            float *to = side ? right[1 + f * OOT_FINGER_JOINTS + j] : left[1 + f * OOT_FINGER_JOINTS + j];
            if (b >= 0 && whole) { to[0] = rest[b * 12 + 3]; to[1] = rest[b * 12 + 7]; to[2] = rest[b * 12 + 11]; }
            else if (b < 0) to[0] = to[1] = to[2] = 0;
        }
    }
    n = 1 + OOT_FINGERS * OOT_FINGER_JOINTS;
    g_oot_grip_mirrors[ri] = whole && oot_wear_mirrored((const float (*)[3])left, (const float (*)[3])right, n, 0.002f);
    free(rest);
    MLOG("[vr-grip] %s Link: finger entries %d..%d, the hands %s at rest (the other hand's grip %s)", ri ? "adult" : "child",
         g_oot_finger_matrix[ri][0][0][0], g_oot_finger_matrix[ri][1][4][2], g_oot_grip_mirrors[ri] ? "mirror each other" : "do not mirror each other",
         g_oot_grip_mirrors[ri] ? "mirrored" : "not used: the buttons' fist");
}
static void oot_wear_load(void) {
    for (int i = 0; i < OOT_WEARS; i++) {
        char path[128]; snprintf(path, sizeof path, "extracted/oot/vr-rig/%s.bfres", g_oot_wear_names[i]);
        oot_wear *w = &g_oot_wear[i];
        const char *e = oot_rig_load_one(&w->rig, path);
        if (!e) e = oot_wear_prepare(w, &g_oot_rigs[1]);
        w->ok = !e;
        if (e) MLOG("[vr-wear] %s: %s; it keeps the game's pose", g_oot_wear_names[i], e);
        else { unsigned arms = 0; for (unsigned s = 0; s < w->rig.nshapes; s++) arms += (w->rig.shapes[s].any & OOT_MARK_ARMS) != 0;
               MLOG("[vr-wear] %s on the VR arms: palette %u bytes, %u shapes, %u with arm triangles", g_oot_wear_names[i], w->rig.palette_bytes, w->rig.nshapes, arms); }
    }
    for (int i = 0; i < OOT_WORN_NAMES; i++) oot_wear_add_file(g_oot_worn_names[i], OOT_WEAR_WORN, i);
    for (size_t i = 0; i < sizeof g_oot_own_names / sizeof *g_oot_own_names; i++) oot_wear_add_file(g_oot_own_names[i], OOT_WEAR_OWN, (int)i);
    oot_wear_add_file("Item_Gauntlet_Goron", OOT_WEAR_CARRIED, OOT_WEAR_BRACELET);
    for (int ri = 0; ri < 2; ri++) oot_grip_prepare(ri);
    MLOG("[vr-worn] %u of Link's models known by their size (what he wears, carries and holds)", g_oot_wear_table.n);
}

/* The arm parts' matrices of this frame for a worn draw: his body's palette must have been made this frame or the one before. */
static const oot_arm_frame *oot_arm_frame_now(int rig) {
    const uint64_t now = atomic_load(&g_cur_frame);
    const oot_arm_frame *a = &g_oot_arm_frame;
    return a->frame != UINT64_MAX && a->rig == rig && now - a->frame <= 1 && (a->have[0] || a->have[1]) ? a : NULL;
}
/* -1: not drawn; 0: as the game draws it; 1: its arm triangles with its palette on the VR arms; 2: carried on an arm (the bracelet). */
static int oot_wear_draw(const p_draw_idx *di, __unsafe_unretained NvShader *vs) {
    g_oot_prop_bracelet = 0;
    if (!vs || !runtime_oot_vr_fp_active || !runtime_oot_vr_fp_active() || vr_oot_body_mode() != 0) return 0;
    if (R.rt.n < 1 || g_pass.tw < 640 || g_pass.th < 360 || g_pass.tw == g_pass.th) return 0;   /* his shadow keeps them as they are */
    if (atomic_load_explicit(&g_oot_rig_state, memory_order_acquire) != 2) return 0;
    const uint32_t stride = S.stream[0].stride;
    if (!stride || !S.vbuf[0].size) return 0;
    const unsigned verts = (unsigned)(S.vbuf[0].size / stride);
    for (int i = 0; i < vs->rf.nb; i++) {
        if (vs->rf.b[i].kind != NVN_RB_CBUF || (vs->rf.b[i].reg != 3 && vs->rf.b[i].reg != 4)) continue;
        const int reg = vs->rf.b[i].reg; const uint64_t size = S.ubo[0][reg].size;
        if (!size || size % 48 || size > 48 * 256) continue;
        for (int w = 0; w < OOT_WEARS && di; w++) {   /* a tunic or gauntlets: their arm triangles on the VR arms */
            oot_wear *x = &g_oot_wear[w];
            if (!x->ok || x->rig.palette_bytes != size || !vr_oot_wear()) continue;
            const oot_rig_shape *shape = NULL;
            for (unsigned s = 0; s < x->rig.nshapes && !shape; s++) if (x->rig.shapes[s].verts == verts) shape = &x->rig.shapes[s];
            if (!shape) continue;
            if (!oot_arm_frame_now(1)) return 0;   /* no VR arms this frame: as the game poses it */
            if (!(shape->any & OOT_MARK_ARMS)) return -1;
            body_tris_t *bt = oot_body_tris(di, shape, OOT_MARK_ARMS);
            if (!bt || !bt->n) return -1;
            g_body_tris_cur = bt; g_oot_palette_reg = reg;
            if (!x->drawn++) MLOG("[vr-wear] %s: first person shows its arms on the VR arms (%d of %d indices, palette block %d)", g_oot_wear_names[w], bt->n, di->count, reg);
            return 1;
        }
        const oot_wear_model *m = oot_wear_find(&g_oot_wear_table, size, verts);
        if (!m || m->kind == OOT_WEAR_OWN) continue;
        float first[12], model[19];
        if (S.ubo[0][1].size < 76) return 0;
        pend_materialize(S.ubo[0][reg].addr, (uint8_t *)first, sizeof first, 0);
        pend_materialize(S.ubo[0][1].addr, (uint8_t *)model, sizeof model, 0);
        const float at[3] = { first[3] + model[16], first[7] + model[17], first[11] + model[18] };
        if (!isfinite(at[0]) || !isfinite(at[1]) || !isfinite(at[2])) return 0;
        const uint64_t now = atomic_load(&g_cur_frame);
        const oot_hand_track *t = &g_oot_track[g_oot_track[1].ok ? 1 : 0];
        if (!t->ok || now - t->frame >= 4) return 0;
        const float dx = at[0] - t->chest[0], dy = at[1] - t->chest[1], dz = at[2] - t->chest[2];
        if (dx * dx + dy * dy + dz * dz > 1.5f * 1.5f) return 0;   /* not on Link (one in a shop, on someone else): as the game draws it */
        if (m->kind == OOT_WEAR_WORN) {
            if (!vr_oot_worn()) return 0;
            if (m->index >= 0 && m->index < OOT_WORN_NAMES && !g_oot_worn_said[m->index]++)
                MLOG("[vr-worn] first person leaves out %s (palette %llu bytes, %u vertices)", g_oot_worn_names[m->index], (unsigned long long)size, verts);
            return -1;
        }
        if (m->index == OOT_WEAR_BRACELET && vr_oot_wear()) {   /* the bracelet: on the bone it rides, as that bone went to the VR arm */
            const oot_arm_frame *a = oot_arm_frame_now(0);
            if (!a) return 0;
            const oot_rig_arms *arms = &g_oot_arms[0];
            float seg[4][2][3];
            for (int side = 0; side < 2; side++) {
                const float *rest[3] = { arms->rest[side].elbow, arms->rest[side].wrist, arms->rest[side].finger[2][0] };
                for (int k = 0; k < 2; k++) for (int e = 0; e < 2; e++) {
                    oot_m_apply(k ? a->game_hand[side] : a->game_fore[side], rest[k + e], seg[side * 2 + k][e]);
                    for (int c = 0; c < 3; c++) seg[side * 2 + k][e][c] += a->off[c];
                }
            }
            const int was = g_oot_bracelet_rides, rides = oot_wear_rides(at, (const float (*)[2][3])seg, was, 0.20f, 0.60f);
            g_oot_bracelet_rides = rides;
            if (rides != was && rides >= 0) MLOG("[vr-wear] the Goron Bracelet rides his %s %s: it goes with the VR arm", rides >= 2 ? "right" : "left", rides & 1 ? "hand" : "forearm");
            if (rides < 0 || !a->have[rides / 2]) return 0;
            g_oot_prop_reg = reg; g_oot_prop_bracelet = 1;
            return 2;
        }
    }
    return 0;
}
/* A tunic's or gauntlets' palette size (nvn_mtl.m: the palette of a draw oot_wear_draw gave its arm triangles). */
static int oot_wear_palette_size(uint64_t size) {
    for (int i = 0; i < OOT_WEARS; i++) if (g_oot_wear[i].ok && g_oot_wear[i].rig.palette_bytes == size) return 1;
    return 0;
}
/* A tunic's or gauntlets' palette on the VR arms, in the ring of ours; 0 leaves the game's. */
static uint64_t oot_wear_palette(const uint8_t *src, uint64_t size) {
    oot_wear *w = NULL;
    for (int i = 0; i < OOT_WEARS && !w; i++) if (g_oot_wear[i].ok && g_oot_wear[i].rig.palette_bytes == size) w = &g_oot_wear[i];
    const oot_arm_frame *a = w ? oot_arm_frame_now(1) : NULL;
    if (!a || S.ubo[0][1].size < 76) return 0;
    float model[19]; pend_materialize(S.ubo[0][1].addr, (uint8_t *)model, sizeof model, 0);
    const float shift[3] = { a->off[0] - model[16], a->off[1] - model[17], a->off[2] - model[18] };   /* his body's palette space to this model's */
    if (!isfinite(shift[0]) || !isfinite(shift[1]) || !isfinite(shift[2])) return 0;
    uint64_t gpu = 0; uint8_t *to = oot_palette_ring(size, &gpu);
    memcpy(to, src, (size_t)size);
    for (unsigned k = 0; (uint64_t)(k + 1) * 48 <= size; k++) {
        const oot_rig_part p = w->parts[k];
        if (p.part == OOT_PART_NONE || !a->have[p.side] || oot_wear_collapsed(src + (size_t)k * 48)) continue;
        float m[12]; oot_wear_mul(oot_part_matrix(&a->done[p.side], p.part), w->fix[k], m);
        m[3] += shift[0]; m[7] += shift[1]; m[11] += shift[2];
        if (a->crawl > 0) { float game[12]; memcpy(game, src + (size_t)k * 48, 48); oot_matrix_lerp(m, game, a->crawl, m); }
        if (oot_wear_finite(m)) memcpy(to + (size_t)k * 48, m, 48);
    }
    return gpu;
}
/* The bracelet's palette: carried as the bone it rides went from the game's pose to the VR one; 0 leaves the game's. */
static uint64_t oot_wear_bracelet_palette(const uint8_t *src, uint64_t size) {
    const oot_arm_frame *a = oot_arm_frame_now(0);
    const int rides = g_oot_bracelet_rides;
    if (!a || rides < 0 || size < 48 || S.ubo[0][1].size < 76) return 0;
    const int side = rides / 2, hand = rides & 1;
    float model[19]; pend_materialize(S.ubo[0][1].addr, (uint8_t *)model, sizeof model, 0);
    float vr[12], game[12], thing[12], out[12];
    memcpy(vr, hand ? a->done[side].hand : a->done[side].fore, 48); memcpy(game, hand ? a->game_hand[side] : a->game_fore[side], 48);
    memcpy(thing, src, 48);
    vr[3] += a->off[0]; vr[7] += a->off[1]; vr[11] += a->off[2];        /* all three in world space */
    game[3] += a->off[0]; game[7] += a->off[1]; game[11] += a->off[2];
    thing[3] += model[16]; thing[7] += model[17]; thing[11] += model[18];
    if (a->crawl > 0) oot_matrix_lerp(vr, game, a->crawl, vr);
    if (!oot_wear_carry(vr, game, thing, out)) return 0;
    out[3] -= model[16]; out[7] -= model[17]; out[11] -= model[18];
    if (!oot_wear_finite(out)) return 0;
    uint64_t gpu = 0; uint8_t *to = oot_palette_ring(size, &gpu);
    memcpy(to, src, (size_t)size); memcpy(to, out, 48);
    static int said;
    if (!said++) MLOG("[vr-wear] the Goron Bracelet is carried on the VR %s %s", side ? "right" : "left", hand ? "hand" : "forearm");
    return gpu;
}
/* The game's own grip on what a VR hand holds: its fingers as the game's hand that holds the thing has them. */
static void oot_grip_fingers(int ri, const oot_rig_arms *arms, const uint8_t *src, uint64_t size, oot_arm_matrices done[2], const int have[2]) {
    if (!vr_oot_held_grip()) return;
    const uint64_t now = atomic_load(&g_cur_frame);
    static uint64_t latched = UINT64_MAX; static int from_now[2];   /* decided once a frame: his depth and colour draws must match */
    if (latched != now) { latched = now; for (int k = 0; k < 2; k++) from_now[k] = g_oot_grip_at[k] && now - g_oot_grip_at[k] < 4 ? g_oot_grip_from[k] : -1; }
    for (int side = 0; side < 2; side++) {
        if (!have[side] || from_now[side] < 0) continue;
        const int from = from_now[side], mirrored = from != side;
        if (mirrored && !g_oot_grip_mirrors[ri]) continue;
        const int hm = arms->wrist_matrix[from];
        if (hm < 0 || (uint64_t)(hm + 1) * 48 > size) continue;
        float game_hand[12]; memcpy(game_hand, src + (size_t)hm * 48, 48);
        int set = 0;
        for (int f = 0; f < OOT_FINGERS; f++) for (int j = 0; j < OOT_FINGER_JOINTS; j++) {
            const int k = g_oot_finger_matrix[ri][from][f][j];
            if (k < 0 || (uint64_t)(k + 1) * 48 > size) continue;
            float game_finger[12]; memcpy(game_finger, src + (size_t)k * 48, 48);
            set += oot_wear_finger(done[side].hand, game_hand, game_finger, mirrored, done[side].finger[f][j]);
        }
        static int said[2];
        if (set && !said[side]++) MLOG("[vr-grip] the player's %s hand holds with the game's %s hand's own grip (%d finger joints%s)", side ? "right" : "left",
                                       from ? "right" : "left", set, mirrored ? ", mirrored" : "");
    }
}
/* After his body's palette: what the worn draws of this frame need. */
static void oot_arm_frame_note(int ri, const oot_rig_arms *arms, const uint8_t *src, uint64_t size, const oot_arm_matrices done[2], const int have[2], const float off[3], float crawl) {
    oot_arm_frame *a = &g_oot_arm_frame;
    a->frame = atomic_load(&g_cur_frame); a->rig = ri; a->crawl = crawl;
    memcpy(a->done, done, sizeof a->done); memcpy(a->have, have, sizeof a->have); memcpy(a->off, off, sizeof a->off);
    for (int side = 0; side < 2; side++) {
        const int f = g_oot_fore_matrix[ri][side], h = arms->wrist_matrix[side];
        if (f < 0 || h < 0 || (uint64_t)(f + 1) * 48 > size || (uint64_t)(h + 1) * 48 > size) { a->have[side] = 0; continue; }
        memcpy(a->game_fore[side], src + (size_t)f * 48, 48); memcpy(a->game_hand[side], src + (size_t)h * 48, 48);
    }
}
