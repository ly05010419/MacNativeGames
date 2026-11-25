/* zeldaVR, Ocarina of Time, first person: what Link holds goes with the player's hands. A held thing is a model of its own with
   its own skinning palette, posed by the game on his wrist; his arm now goes elsewhere (oot_vr_arms.inc), so its palette moves
   from the grip of the game's hand to the grip of the player's matching hand (oot_vr_hold.h: Link is left-handed), in world space. A model whose root sits within 20 cm of a wrist
   the game posed is held; one within 16 cm of his chest (the sheathed sword, the shield on his back) would float in front of
   the eyes and is not drawn. Pass dump arms04: the slingshot, palette block 3, 164 matrices. Included by oot_vr_arms.inc. */
typedef struct { float grip_game[12], grip_new[12], wrist[3], chest[3]; uint64_t frame; int ok; } oot_hand_track;
static oot_hand_track g_oot_track[2];
static int g_oot_prop_reg = -1, g_oot_prop_side, g_oot_prop_weapon;
static float g_oot_arm_length;      /* game units, shoulder to wrist */
static float g_oot_prop_root[3];   /* the held thing's root, world space */
static uint64_t g_oot_props_moved, g_oot_props_hidden;

/* After his palette is made: how each hand moved, in world space (world = palette + off). */
static void oot_track_hand(int side, const float game_hand[12], const float new_hand[12], const float wrist_rest[3], const float chest_world[3], const float off[3], const float grip_rest[12]) {
    const float to_world[12] = { 1, 0, 0, off[0], 0, 1, 0, off[1], 0, 0, 1, off[2] };
    oot_hand_track *t = &g_oot_track[side];
    oot_m_mul(game_hand, grip_rest, t->grip_game); oot_m_mul(to_world, t->grip_game, t->grip_game);
    oot_m_mul(new_hand, grip_rest, t->grip_new); oot_m_mul(to_world, t->grip_new, t->grip_new);
    oot_m_apply(game_hand, wrist_rest, t->wrist);
    for (int i = 0; i < 3; i++) { t->wrist[i] += off[i]; t->chest[i] = chest_world[i]; }
    t->frame = atomic_load(&g_cur_frame); t->ok = 1;
}
/* A weapon is in his own weapon hand (his left) these last frames: the gesture that puts it away may press the game's button. */
static _Atomic uint64_t g_oot_weapon_frame;
int vr_oot_weapon_in_hand(void) { const uint64_t at = atomic_load(&g_oot_weapon_frame); return at && atomic_load(&g_cur_frame) - at < 12; }
/* What each model keeps its distance to, by its palette size and vertex buffer. */
static oot_hold_watch *oot_prop_watch(uint64_t size, uint64_t vb) {
    static oot_hold_watch watched[OOT_HOLD_WATCHED];
    return oot_hold_watch_for(watched, size, vb);
}
#include "oot_vr_weapon.inc"
/* 0: as the game draws it; 2: its palette moves with a hand; -1: not drawn. */
static int oot_prop_draw(__unsafe_unretained NvShader *vs) {
    g_oot_prop_reg = -1;
    if (!runtime_oot_vr_fp_active || !runtime_oot_vr_fp_active() || vr_oot_body_mode() != 0) return 0;
    if (R.rt.n < 1 || g_pass.tw < 640 || g_pass.th < 360 || g_pass.tw == g_pass.th) return 0;   /* his shadow keeps what he holds */
    const uint64_t now = atomic_load(&g_cur_frame);
    const int live[2] = { g_oot_track[0].ok && now - g_oot_track[0].frame < 4, g_oot_track[1].ok && now - g_oot_track[1].frame < 4 };
    if (!live[0] && !live[1]) return 0;
    int reg = -1;
    for (int i = 0; i < vs->rf.nb && reg < 0; i++) {
        if (vs->rf.b[i].kind != NVN_RB_CBUF || (vs->rf.b[i].reg != 3 && vs->rf.b[i].reg != 4)) continue;
        const uint64_t size = S.ubo[0][vs->rf.b[i].reg].size;
        if (size >= 48 && size <= 48 * 256 && size % 48 == 0) reg = vs->rf.b[i].reg;
    }
    if (reg < 0 || S.ubo[0][1].size < 76) return 0;
    float first[12], model[19];
    pend_materialize(S.ubo[0][reg].addr, (uint8_t *)first, sizeof first, 0);
    pend_materialize(S.ubo[0][1].addr, (uint8_t *)model, sizeof model, 0);
    const float at[3] = { first[3] + model[16], first[7] + model[17], first[11] + model[18] };
    if (!isfinite(at[0]) || !isfinite(at[1]) || !isfinite(at[2])) return 0;
    const float *chest_at = g_oot_track[live[1] ? 1 : 0].chest;
    float dist[3];
    for (int s = 0; s < 3; s++) {
        const float *p = s < 2 ? g_oot_track[s].wrist : chest_at;
        dist[s] = s < 2 && !live[s] ? 9.0f : sqrtf((at[0] - p[0]) * (at[0] - p[0]) + (at[1] - p[1]) * (at[1] - p[1]) + (at[2] - p[2]) * (at[2] - p[2]));
    }
    oot_hold_watch *watch = oot_prop_watch(S.ubo[0][reg].size, S.vbuf[0].address);   /* once a frame: its depth and colour draws decide alike */
    if (watch->frame != now) { watch->frame = now; oot_hold_watch_step(watch, dist); }
    float weapon_game[12];
    const int weapon_out = oot_weapon_state(weapon_game), weapon = weapon_out && oot_weapon_is(S.ubo[0][reg].size, S.vbuf[0].address);
    const int side = weapon ? 0 : watch->side;
    if (!weapon && weapon_out && side == 0 && live[0]) {
        const float root[12] = { first[0], first[1], first[2], at[0], first[4], first[5], first[6], at[1], first[8], first[9], first[10], at[2] };
        oot_weapon_learn(S.ubo[0][reg].size, S.vbuf[0].address, root, g_oot_track[0].grip_game, weapon_game);
    }
    g_oot_prop_weapon = weapon;
    if (side == 0) atomic_store(&g_oot_weapon_frame, now);
    if (oot_body_trace_on()) {
        const float *wl = g_oot_track[0].wrist, *wr = g_oot_track[1].wrist, *ch = g_oot_track[live[1] ? 1 : 0].chest;
        MLOG("[vr-prop] F%llu palette %llu bytes, root %.3f %.3f %.3f: left wrist %.3f, right wrist %.3f, chest %.3f, held by %d (watch frames %u, jitter %.5f %.5f, vb %#llx) rot %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f", (unsigned long long)now,
             (unsigned long long)S.ubo[0][reg].size, (double)at[0], (double)at[1], (double)at[2],
             (double)sqrtf((at[0] - wl[0]) * (at[0] - wl[0]) + (at[1] - wl[1]) * (at[1] - wl[1]) + (at[2] - wl[2]) * (at[2] - wl[2])),
             (double)sqrtf((at[0] - wr[0]) * (at[0] - wr[0]) + (at[1] - wr[1]) * (at[1] - wr[1]) + (at[2] - wr[2]) * (at[2] - wr[2])),
             (double)sqrtf((at[0] - ch[0]) * (at[0] - ch[0]) + (at[1] - ch[1]) * (at[1] - ch[1]) + (at[2] - ch[2]) * (at[2] - ch[2])), side,
             watch->frames, (double)watch->jitter[0], (double)watch->jitter[1], (unsigned long long)S.vbuf[0].address,
             (double)first[0], (double)first[1], (double)first[2], (double)first[4], (double)first[5], (double)first[6], (double)first[8], (double)first[9], (double)first[10]);
    }
    if (side >= 0) {
        g_oot_prop_reg = reg; g_oot_prop_side = side; memcpy(g_oot_prop_root, at, sizeof at);
        if (!g_oot_props_moved++) MLOG("[vr] Link holds a model in his %s hand (palette block %d, %llu matrices)", side ? "right" : "left", reg, (unsigned long long)(S.ubo[0][reg].size / 48));
        return 2;
    }
    if (dist[2] < 0.16f) {
        if (!g_oot_props_hidden++) MLOG("[vr] something he wears on his body is not drawn in first person (palette block %d)", reg);
        return -1;
    }
    return 0;
}
/* The slingshot's and the bow's own models (extracted/oot/vr-rig, oot_vr_items.h): which palette entries are the string. */
enum { OOT_ITEMS = 6 };
static oot_item_rig g_oot_items[OOT_ITEMS];
static _Atomic unsigned g_oot_item_count;
static void oot_items_load(void) {
    static const char *const names[OOT_ITEMS] = { "Weapon_Range_SlingShot", "Weapon_Range_Bow", "Weapon_Range_BowAndArrow_Normal",
        "Weapon_Range_BowAndArrow_Fire", "Weapon_Range_BowAndArrow_Ice", "Weapon_Range_BowAndArrow_Light" };
    unsigned count = 0;
    for (int i = 0; i < OOT_ITEMS; i++) {
        char path[128]; snprintf(path, sizeof path, "extracted/oot/vr-rig/%s.bfres", names[i]);
        FILE *f = fopen(path, "rb");
        if (!f) { MLOG("[vr] %s: no model (an App packaged before it: update it); its string stays with the wood", names[i]); continue; }
        fseek(f, 0, SEEK_END); const long size = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *data = size > 0 && size <= (8L << 20) ? malloc((size_t)size) : NULL;
        const char *e = data && fread(data, 1, (size_t)size, f) == (size_t)size ? NULL : "cannot read it";
        fclose(f);
        oot_bfres m;
        if (!e) e = oot_bfres_open(&m, data, (size_t)size);
        if (!e) e = oot_item_prepare(&m, &g_oot_items[count]);
        free(data);
        if (e) { MLOG("[vr] %s: %s", names[i], e); continue; }
        count++;
    }
    atomic_store(&g_oot_item_count, count);
    MLOG("[vr] the string's bones known for %u of %d two-handed models", count, OOT_ITEMS);
}
static const oot_item_rig *oot_item_for(uint64_t size) {
    const uint32_t stride = S.stream[0].stride;
    if (!stride || !S.vbuf[0].size) return NULL;
    const unsigned verts = (unsigned)(S.vbuf[0].size / stride), count = atomic_load(&g_oot_item_count);
    for (unsigned i = 0; i < count; i++) {
        if (g_oot_items[i].palette_bytes != size) continue;
        for (unsigned s = 0; s < g_oot_items[i].shapes; s++) if (g_oot_items[i].verts[s] == verts) return &g_oot_items[i];
    }
    return NULL;
}
/* The pouch or nock the game has drawn goes to the palm of the player's other hand, and each string bone takes its
   share of that way from where the holding hand carries it, so the string runs straight from the wood to the hand. */
typedef struct { float pull[3], drawn; const oot_item_rig *item; } oot_prop_strung;
static int oot_prop_string(const oot_item_rig *item, int to, const uint8_t *src, uint64_t frame, const float to_world[12], const float move[12], oot_prop_strung *s) {
    if (!item) return 0;
    const oot_hand_track *player = &g_oot_track[1 - to];
    if (!player->ok || frame - player->frame >= 4) return 0;
    const float drawn = oot_item_drawn(item, src, 0.40f * g_oot_arm_length);
    if (drawn <= 0) return 0;
    float center[12], world[12], placed[12], at[3];
    memcpy(center, src + (size_t)item->center_entry * 48, 48);
    oot_m_mul(to_world, center, world); oot_m_mul(move, world, placed); oot_m_apply(placed, item->center_rest, at);
    s->pull[0] = player->grip_new[3] - at[0]; s->pull[1] = player->grip_new[7] - at[1]; s->pull[2] = player->grip_new[11] - at[2];
    s->drawn = drawn; s->item = item;
    static int said;
    if (!said) {
        said = 1;
        MLOG("[vr] the string or pouch goes to the player's other hand (%u matrices, drawn %.2f, the way %.3f %.3f %.3f)", item->matrices, (double)drawn,
             (double)s->pull[0], (double)s->pull[1], (double)s->pull[2]);
    }
    return 1;
}
/* The held model's palette, moved to the player's hand, in the ring of ours. Once per thing and frame: its depth and colour draws
   must have the same pose, and the colour pass keeps only the pixels whose depth is equal. */
static uint64_t oot_prop_palette(uint64_t addr, const uint8_t *src, uint64_t size) {
    enum { MADE = 64 };
    static struct { uint64_t frame, addr, size, gpu; } made[MADE];
    const uint64_t frame = atomic_load(&g_cur_frame);
    const unsigned slot = (unsigned)((addr >> 4) * 2654435761u >> 26) % MADE;
    if (made[slot].frame == frame && made[slot].addr == addr && made[slot].size == size) return made[slot].gpu;
    if (g_oot_prop_reg < 0 || S.ubo[0][1].size < 76) return 0;
    const int from = g_oot_prop_side, wanted = oot_hold_hand(from, vr_oot_left_handed());
    const int to = g_oot_track[wanted].ok && frame - g_oot_track[wanted].frame < 4 ? wanted : from;
    float model[19]; pend_materialize(S.ubo[0][1].addr, (uint8_t *)model, sizeof model, 0);
    const float to_world[12] = { 1, 0, 0, model[16], 0, 1, 0, model[17], 0, 0, 1, model[18] }, to_palette[12] = { 1, 0, 0, -model[16], 0, 1, 0, -model[17], 0, 0, 1, -model[18] };
    const oot_item_rig *item = oot_item_for(size);   /* the slingshot or the bow: one handle, mirrored like the sword */
    const int how = oot_hold_mirrors(from, to, item != NULL || vr_oot_aiming()), weapon = g_oot_prop_weapon && from == 0;
    float move[12], undo[12];   /* the weapon: its palette already follows the hand a frame late; undo takes it back to the game's pose */
    if (weapon) oot_weapon_frame(src, to_world, g_oot_track[from].grip_game, g_oot_track[to].grip_new, how, to == wanted, undo, move);
    else oot_hold_move(g_oot_track[from].grip_game, g_oot_track[to].grip_new, g_oot_prop_root, how, move);
    oot_prop_strung strung; const int two = oot_prop_string(item, to, src, frame, to_world, move, &strung);
    uint64_t gpu = 0; uint8_t *out = oot_palette_ring(size, &gpu);
    for (uint64_t k = 0; k + 48 <= size; k += 48) {
        float m[12], world[12], placed[12]; memcpy(m, src + k, 48);
        oot_m_mul(to_world, m, world);
        if (weapon) { float game_pose[12]; oot_m_mul(undo, world, game_pose); memcpy(world, game_pose, sizeof world); }
        oot_m_mul(move, world, placed);
        if (two) {
            const float share = strung.item->share[k / 48] * strung.drawn;
            placed[3] += strung.pull[0] * share; placed[7] += strung.pull[1] * share; placed[11] += strung.pull[2] * share;
        }
        oot_m_mul(to_palette, placed, m); memcpy(out + k, m, 48);
    }
    static int said;
    if (!said && to != from) { said = 1; MLOG("[vr] what Link holds in his %s hand is in the player's %s hand", from ? "right" : "left", to ? "right" : "left"); }
    made[slot].frame = frame; made[slot].addr = addr; made[slot].size = size; made[slot].gpu = gpu;
    return gpu;
}
