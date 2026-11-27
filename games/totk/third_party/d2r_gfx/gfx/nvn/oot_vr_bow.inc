/* zeldaVR, Ocarina of Time, first person: the bow and the slingshot in the player's two hands (vr.md rows 55, 57-60, 62;
   HookshotVR's notes, aim-items.md 1.5, 1.9 and 2, GPL-3: facts and ideas, no code). Once a game frame, from the held model's own
   palette (oot_vr_props.inc oot_prop_palette):
   - the string's solution (zeldaVR/runtime/oot_vr_string.h): where the string rests on the VR bow, the drawing hand and its
     trigger / grip (the VR page's "string hand input"), whether it is held, drawn, committed, let go; published to the pad's thread
     (totk_vr.m, which holds the game's ZR while the draw is committed) and to the game side (the shot's line, oot_vr_sword.h);
   - the strings and straps posed from it: each string bone turned about its tip and stretched so that the middle of the string is
     at the drawing hand while it is held and back at its rest when it is not, whatever the game's own draw does; the pouch turned
     about the fork and moved there. The slingshot's handle is its fork (the shot leaves along pouch -> fork), the bow's the palm;
   - the bow drawn at the VR page's size about the handle (HookshotVR's default 130 %; the slingshot keeps its size);
   - the game's own arrow nocked on the game's own bow (Item_Arrow_*, one rigid matrix, known by its vertex counts from the player's
     files, extracted/oot/vr-rig) moved onto the VR string, its notch on the string and its shaft along the aim;
   - a held fire, ice or light arrow's effects: their particle camera block (vertex uniform block 10, 0x210 bytes) moved by the change
     from the game's arrow to the VR one, at the VR page's opacity (default 20 %), when the effect's matrix (block 11) is within 0.8 m
     of the game's arrow (oot_arrow_fx_block, from nvn_mtl.m's uniform binding).
   What is not known on our image and is checked as it goes (nothing is changed when a check fails, one log line): the arrow's
   palette in vertex block 3 or 4 like the other held models; the effect blocks 10 and 11 at the indices HookshotVR's renderer reads.
   TOTK_VR_BLADELOG=1: a line for every grab, commit and shot. Included by oot_vr_props.inc. */
#include "../../../../zeldaVR/runtime/oot_vr_string.h"
int vr_oot_string_input(void);   /* totk_vr.m: 0 trigger or grip, 1 trigger, 2 grip, 3 no string (the item button, as before) */
int vr_oot_bow_size(void);       /* totk_vr.m: percent */
int vr_oot_arrow_fx(void);       /* totk_vr.m: percent */
int vr_oot_bow_hand(void);       /* totk_vr.m: the hand the grip took the bow with, -1 none */
int vr_oot_bow_menu(void);       /* totk_vr.m: a wheel or the settings are open */
void vr_oot_bow_publish(int flags, float draw, int fired, int bow_hand);   /* totk_vr.m: to the pad's thread */
extern int runtime_oot_vr_arrow_kind(void) __attribute__((weak_import));   /* games/oot/runtime/oot_vr_bowstate.h */

static oot_string_state g_oot_string; static oot_string_out g_oot_string_out;
static uint64_t g_oot_string_frame = ~0ull;
static struct { uint64_t frame; float game_root[12], vr_root[12]; } g_oot_bow_seen = { .frame = ~0ull };
static struct { uint64_t frame; float game[12], vr[12]; } g_oot_arrow_placed = { .frame = ~0ull };
static int g_oot_prop_arrow;   /* this draw is the nocked arrow (oot_prop_draw) */
static float g_oot_bow_center_off;   /* the posed string's middle against where it should be, game units (the log) */
static int oot_blade_log(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VR_BLADELOG"); on = e && *e == '1'; } return on; }

/* The arrows' own models: their vertex counts (a palette of one matrix). */
enum { OOT_ARROWS = 4, OOT_ARROW_SHAPES = 4 };
static struct { unsigned shapes, verts[OOT_ARROW_SHAPES]; } g_oot_arrow_rigs[OOT_ARROWS];
static _Atomic unsigned g_oot_arrow_count;
static void oot_arrows_load(void) {   /* from oot_items_load, on the loading queue */
    static const char *const names[OOT_ARROWS] = { "Item_Arrow_Normal", "Item_Arrow_Fire", "Item_Arrow_Ice", "Item_Arrow_Light" };
    unsigned count = 0;
    for (int i = 0; i < OOT_ARROWS; i++) {
        char path[128]; snprintf(path, sizeof path, "extracted/oot/vr-rig/%s.bfres", names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { MLOG("[vr-arrow] %s: no model (an App packaged before 2026-11-27: update it); the nocked arrow stays where the game puts it", names[i]); continue; }
        fseek(f, 0, SEEK_END); const long size = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *data = size > 0 && size <= (8L << 20) ? malloc((size_t)size) : NULL;
        const char *e = data && fread(data, 1, (size_t)size, f) == (size_t)size ? NULL : "cannot read it";
        fclose(f);
        oot_bfres m;
        if (!e) e = oot_bfres_open(&m, data, (size_t)size);
        if (!e && oot_bfres_matrix_count(&m) != 1) e = "not one rigid matrix";
        if (!e) {
            g_oot_arrow_rigs[count].shapes = m.shape_count < OOT_ARROW_SHAPES ? m.shape_count : OOT_ARROW_SHAPES;
            for (unsigned s = 0; s < g_oot_arrow_rigs[count].shapes && !e; s++) { oot_bfres_shape shape; e = oot_bfres_shape_at(&m, s, &shape); if (!e) g_oot_arrow_rigs[count].verts[s] = shape.vertex_count; }
        }
        free(data);
        if (e) { MLOG("[vr-arrow] %s: %s", names[i], e); continue; }
        count++;
    }
    atomic_store(&g_oot_arrow_count, count);
    MLOG("[vr-arrow] %u of %d arrow models known", count, OOT_ARROWS);
}
static int oot_arrow_model(uint64_t size) {
    const uint32_t stride = S.stream[0].stride;
    if (size != 48 || !stride || !S.vbuf[0].size) return 0;
    const unsigned verts = (unsigned)(S.vbuf[0].size / stride), count = atomic_load(&g_oot_arrow_count);
    for (unsigned i = 0; i < count; i++) for (unsigned s = 0; s < g_oot_arrow_rigs[i].shapes; s++) if (g_oot_arrow_rigs[i].verts[s] == verts) return 1;
    return 0;
}
/* oot_prop_draw: the game's arrow on the game's own bow, this frame or the last. first: its palette's one matrix; off: the block's translation. */
static int oot_arrow_draw(uint64_t size, const float first[12], const float off[3]) {
    const uint64_t now = atomic_load(&g_cur_frame);
    if (!oot_arrow_model(size) || g_oot_bow_seen.frame == ~0ull || now - g_oot_bow_seen.frame > 1 || !g_oot_string_out.valid) return 0;
    const float arrow[12] = { first[0], first[1], first[2], first[3] + off[0], first[4], first[5], first[6], first[7] + off[1], first[8], first[9], first[10], first[11] + off[2] };
    const int nocked = oot_arrow_nocked(g_oot_bow_seen.game_root, arrow);
    static int said;
    if (nocked && !said++) MLOG("[vr-arrow] the game's arrow is on its own bow: it goes onto the VR string");
    return nocked;
}
/* The bow drawn at the VR page's size, about the handle: the grip stays at the controller. */
static void oot_bow_sized(float move[12], const float handle[3]) {
    const float k = (float)vr_oot_bow_size() / 100.0f;
    if (!(k > 0.5f && k < 3.0f) || fabsf(k - 1) < 1e-3f) return;
    float l[12]; oot_m_identity(l); for (int i = 0; i < 3; i++) l[5 * i] = k;
    oot_m_about(l, handle, handle, l);
    oot_m_mul(l, move, move);
}
/* Where the item's parts are this frame, moved to the player's hand. */
typedef struct { float rest[3], mid[3], tip[2][3], fork[3], handle[3], root_vr[12], root_game[12]; } oot_bow_parts;
static int oot_bow_parts_at(const oot_item_rig *item, const uint8_t *src, const float to_world[12], const float move[12], const float palm[3], oot_bow_parts *p) {
    float m[12], w[12];
    memcpy(m, src + (size_t)item->root_entry * 48, 48); oot_m_mul(to_world, m, p->root_game); oot_m_mul(move, p->root_game, p->root_vr);
    oot_m_apply(p->root_vr, item->center_rest, p->rest);
    memcpy(m, src + (size_t)item->center_entry * 48, 48); oot_m_mul(to_world, m, w); oot_m_mul(move, w, w); oot_m_apply(w, item->center_rest, p->mid);
    for (int s = 0; s < 2; s++) { memcpy(m, src + (size_t)item->tip_entry[s] * 48, 48); oot_m_mul(to_world, m, w); oot_m_mul(move, w, w); oot_m_apply(w, item->tip_rest[s], p->tip[s]); }
    for (int i = 0; i < 3; i++) p->fork[i] = (p->tip[0][i] + p->tip[1][i]) / 2;
    {   /* the slingshot's handle is its fork, at rest on the wood */
        const float fork_rest[3] = { (item->tip_rest[0][0] + item->tip_rest[1][0]) / 2, (item->tip_rest[0][1] + item->tip_rest[1][1]) / 2, (item->tip_rest[0][2] + item->tip_rest[1][2]) / 2 };
        if (item->bow) memcpy(p->handle, palm, 12); else oot_m_apply(p->root_vr, fork_rest, p->handle);
    }
    return oot_string_finite(p->rest, 3) && oot_string_finite(p->mid, 3) && oot_string_finite(p->handle, 3) && oot_string_finite(p->tip[0], 3) && oot_string_finite(p->tip[1], 3);
}
/* Once a frame: the string's solution for the held item. to: the player's bow hand. */
static const oot_string_out *oot_bow_solve(const oot_item_rig *item, const oot_bow_parts *p, int to, uint64_t frame) {
    if (g_oot_string_frame == frame) return g_oot_string_out.valid ? &g_oot_string_out : NULL;
    if (g_oot_string_frame != ~0ull && frame - g_oot_string_frame > OOT_STRING_ABSENT_FRAMES + 1) oot_string_reset(&g_oot_string);   /* not drawn for a while */
    g_oot_string_frame = frame;
    const oot_arm_input *input = oot_arm_input_now();
    const int pull = 1 - to;
    oot_string_in in; memset(&in, 0, sizeof in);
    in.bow_present = 1; in.menu = vr_oot_bow_menu(); in.scale = g_oot_room_scale;
    in.tracked = input && input->hands[pull].have && g_oot_track[pull].ok && frame - g_oot_track[pull].frame < 4;
    if (input) in.input = oot_string_input(vr_oot_string_input(), pull ? input->pad.rtrig : input->pad.ltrig, pull ? input->pad.rgrip : input->pad.lgrip);
    memcpy(in.handle, p->handle, 12); memcpy(in.rest, p->rest, 12);
    in.hand[0] = g_oot_track[pull].grip_new[3]; in.hand[1] = g_oot_track[pull].grip_new[7]; in.hand[2] = g_oot_track[pull].grip_new[11];
    const int was = g_oot_string_out.flags;
    oot_string_step(&g_oot_string, &in, &g_oot_string_out);
    const oot_string_out *o = &g_oot_string_out;
    vr_oot_bow_publish(o->valid ? o->flags : 0, o->draw, o->fired, to);
    memcpy(g_oot_bow_seen.game_root, p->root_game, 48); memcpy(g_oot_bow_seen.vr_root, p->root_vr, 48); g_oot_bow_seen.frame = frame;
    if (o->valid && (o->flags != was || o->fired)) {
        static int lines;
        if (lines++ < 12 || oot_blade_log())
            MLOG("[vr-bow] F%llu the %s's string %s (drawn %.2f m, the aim %.2f %.2f %.2f, %s hand pulls, %u shots)", (unsigned long long)frame, item->bow ? "bow" : "slingshot",
                 o->fired ? "let go: the shot" : (o->flags & OOT_STRING_COMMITTED) ? ((o->flags & OOT_STRING_HELD) ? "drawn and committed: the game's draw is held" : "out of the hand, the draw kept") :
                 (o->flags & OOT_STRING_HELD) ? "held" : "at rest", (double)o->draw, (double)o->dir[0], (double)o->dir[1], (double)o->dir[2], pull ? "the right" : "the left", g_oot_string.shots);
    }
    return o->valid ? o : NULL;
}
/* A palette entry of the held item, already moved to the hand (placed, world): its string or pouch to `target`. */
static void oot_bow_pose_entry(const oot_item_rig *item, unsigned k, const oot_bow_parts *p, const float target[3], float placed[12]) {
    if (k >= item->matrices) return;
    float change[12]; const int side = item->side[k];
    if (side == 0 || side == 1) { if (!oot_string_swing(p->tip[side], p->mid, target, change)) return; }
    else if (side == OOT_ITEM_POUCH) { if (!oot_string_pouch(p->fork, p->mid, target, change)) return; }
    else return;
    oot_m_mul(change, placed, placed);
    if (k == item->center_entry) { float at[3]; oot_m_apply(placed, item->center_rest, at); g_oot_bow_center_off = oot_string_dist(at, target); }
}
/* After a held item's palette is posed: the log, the first times the string or pouch is at the drawing hand (its middle's distance
   from the hand, game units: 0 is the pose working). */
static void oot_bow_posed_log(const oot_item_rig *item, const oot_string_out *sol, uint64_t frame) {
    static int said; static int was;
    const int held = (sol->flags & OOT_STRING_HELD) != 0;
    if (held && !was && (said++ < 6 || oot_blade_log()))
        MLOG("[vr-bow] F%llu the %s %s follows the drawing hand (its middle %.4f from the hand, drawn %.2f m)", (unsigned long long)frame, item->bow ? "bow's" : "slingshot's",
             item->bow ? "string" : "pouch and bands", (double)g_oot_bow_center_off, (double)sol->draw);
    was = held;
}
/* The nocked arrow's palette (its one matrix) in our ring: on the VR string, along the aim. 0: left as the game has it. */
static uint64_t oot_arrow_palette(const uint8_t *src, uint64_t size, const float off[3], uint64_t frame) {
    if (size != 48 || g_oot_bow_seen.frame == ~0ull || frame - g_oot_bow_seen.frame > 1 || !g_oot_string_out.valid) return 0;
    float m[12]; memcpy(m, src, 48);
    const float game[12] = { m[0], m[1], m[2], m[3] + off[0], m[4], m[5], m[6], m[7] + off[1], m[8], m[9], m[10], m[11] + off[2] };
    const float *b = g_oot_bow_seen.vr_root;
    float x[3] = { b[0], b[4], b[8] }, y[3] = { b[1], b[5], b[9] }, z[3] = { b[2], b[6], b[10] };
    const float sx = sqrtf(oot_ik_dot(x, x)), sy = sqrtf(oot_ik_dot(y, y)), sz = sqrtf(oot_ik_dot(z, z));
    if (!oot_ik_norm(y) || !oot_ik_norm(z) || !(sx > 1e-4f)) return 0;
    float nock[3];   /* beside the string as the game's arrow is: 3.4 cm along the bow's y, 1.1 cm along its limbs (HookshotVR's recording) */
    for (int i = 0; i < 3; i++) nock[i] = g_oot_string_out.string_at[i] + y[i] * 0.034f * sy + z[i] * 0.011f * sz;
    float placed[12];
    if (!oot_arrow_place(game, nock, g_oot_string_out.dir, z, placed)) return 0;
    memcpy(g_oot_arrow_placed.game, game, 48); memcpy(g_oot_arrow_placed.vr, placed, 48); g_oot_arrow_placed.frame = frame;
    const float to_palette[12] = { 1, 0, 0, -off[0], 0, 1, 0, -off[1], 0, 0, 1, -off[2] };
    uint64_t gpu = 0; uint8_t *out = oot_palette_ring(size, &gpu);
    oot_m_mul(to_palette, placed, m); memcpy(out, m, 48);
    static int said;
    if (!said++) MLOG("[vr-arrow] F%llu the nocked arrow is on the VR string (notch %.2f %.2f %.2f, the aim %.2f %.2f %.2f)", (unsigned long long)frame,
                      (double)nock[0], (double)nock[1], (double)nock[2], (double)g_oot_string_out.dir[0], (double)g_oot_string_out.dir[1], (double)g_oot_string_out.dir[2]);
    return gpu;
}
/* The effects of a held magic arrow: live while the VR arrow was placed these last two frames (first person). */
static int g_oot_fx_live;
static void oot_arrow_fx_note(__unsafe_unretained NvShader *vs) {   /* oot_body_draw, every draw */
    const uint64_t now = atomic_load(&g_cur_frame);
    g_oot_fx_live = g_oot_arrow_placed.frame != ~0ull && now - g_oot_arrow_placed.frame <= 2 && runtime_oot_vr_fp_active && runtime_oot_vr_fp_active() &&
                    runtime_oot_vr_arrow_kind && runtime_oot_vr_arrow_kind() > 0;
    if (g_oot_fx_live && vs && S.ubo[0][10].size == 0x210) vs->ab_pass = 0;   /* its camera block may be ours: not the last draw's arguments */
}
/* nvn_mtl.m's uniform binding, vertex block 10 of 0x210 bytes while an effect may be the arrow's: its camera moved. 0: as it is. */
static uint64_t oot_arrow_fx_block(const p_bind_buf *u) {
    if (!g_oot_fx_live || !u->addr || u->size != 0x210 || S.ubo[0][11].size < 64 || !S.ubo[0][11].addr) return 0;
    float c[OOT_FX_CAMERA_FLOATS], effect[16], eye[3];
    pend_materialize(u->addr, (uint8_t *)c, sizeof c, 0);
    pend_materialize(S.ubo[0][11].addr, (uint8_t *)effect, sizeof effect, 0);
    const float game_at[3] = { g_oot_arrow_placed.game[3], g_oot_arrow_placed.game[7], g_oot_arrow_placed.game[11] };
    if (!oot_fx_eye(c, eye) || !oot_fx_source(effect, game_at, eye)) return 0;
    float inv[12], move[12];
    if (!oot_string_inverse(g_oot_arrow_placed.game, inv)) return 0;
    oot_m_mul(g_oot_arrow_placed.vr, inv, move);
    const float at[3] = { effect[12], effect[13], effect[14] };
    if (!oot_fx_camera(c, move, at, (float)vr_oot_arrow_fx() / 100.0f)) return 0;
    uint64_t gpu = 0; uint8_t *out = oot_palette_ring(sizeof c, &gpu); memcpy(out, c, sizeof c);
    static int said;
    if (!said++) MLOG("[vr-arrow] a magic arrow's effect is drawn on the VR arrow (its camera moved, opacity %d %%)", vr_oot_arrow_fx());
    return gpu;
}
