/* zeldaVR, Ocarina of Time, first person: what Link holds goes with his hands. A held thing is a model of its own with its own
   skinning palette, posed by the game on his wrist; his arm now goes elsewhere (oot_vr_arms.inc), so its palette takes the
   same move his hand took: new hand x inverse(game's hand), in world space. A model whose root sits within 20 cm of a wrist
   the game posed is held; one within 16 cm of his chest (the sheathed sword, the shield on his back) would float in front of
   the eyes and is not drawn. Pass dump arms04: the slingshot, palette block 3, 164 matrices. Included by oot_vr_arms.inc. */
typedef struct { float delta[12], wrist[3], chest[3]; uint64_t frame; int ok; } oot_hand_track;
static oot_hand_track g_oot_track[2];
static int g_oot_prop_reg = -1, g_oot_prop_side;
static uint64_t g_oot_props_moved, g_oot_props_hidden;

static void oot_m_inverse(const float m[12], float out[12]) {
    const float r[12] = { m[0], m[4], m[8], 0, m[1], m[5], m[9], 0, m[2], m[6], m[10], 0 };
    memcpy(out, r, sizeof r);
    for (int i = 0; i < 3; i++) out[4 * i + 3] = -(r[4 * i] * m[3] + r[4 * i + 1] * m[7] + r[4 * i + 2] * m[11]);
}
/* After his palette is made: how each hand moved, in world space (world = palette + off). */
static void oot_track_hand(int side, const float game_hand[12], const float new_hand[12], const float wrist_rest[3], const float chest_world[3], const float off[3]) {
    const float to_world[12] = { 1, 0, 0, off[0], 0, 1, 0, off[1], 0, 0, 1, off[2] }, to_palette[12] = { 1, 0, 0, -off[0], 0, 1, 0, -off[1], 0, 0, 1, -off[2] };
    float inverse[12], d[12];
    oot_m_inverse(game_hand, inverse);
    oot_m_mul(new_hand, inverse, d); oot_m_mul(d, to_palette, d); oot_m_mul(to_world, d, d);
    oot_hand_track *t = &g_oot_track[side];
    memcpy(t->delta, d, sizeof d);
    oot_m_apply(game_hand, wrist_rest, t->wrist);
    for (int i = 0; i < 3; i++) { t->wrist[i] += off[i]; t->chest[i] = chest_world[i]; }
    t->frame = atomic_load(&g_cur_frame); t->ok = 1;
}
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
    int side = -1; float best = 0.20f * 0.20f;
    for (int s = 0; s < 2; s++) {
        if (!live[s]) continue;
        const float *w = g_oot_track[s].wrist;
        const float d = (at[0] - w[0]) * (at[0] - w[0]) + (at[1] - w[1]) * (at[1] - w[1]) + (at[2] - w[2]) * (at[2] - w[2]);
        if (d < best) { best = d; side = s; }
    }
    if (side >= 0) {
        g_oot_prop_reg = reg; g_oot_prop_side = side;
        if (!g_oot_props_moved++) MLOG("[vr] Link holds a model in his %s hand (palette block %d, %llu matrices)", side ? "right" : "left", reg, (unsigned long long)(S.ubo[0][reg].size / 48));
        return 2;
    }
    const float *c = g_oot_track[live[1] ? 1 : 0].chest;
    if ((at[0] - c[0]) * (at[0] - c[0]) + (at[1] - c[1]) * (at[1] - c[1]) + (at[2] - c[2]) * (at[2] - c[2]) < 0.16f * 0.16f) {
        if (!g_oot_props_hidden++) MLOG("[vr] something he wears on his body is not drawn in first person (palette block %d)", reg);
        return -1;
    }
    return 0;
}
/* The held model's palette, moved with the hand, in the ring of ours. */
static uint64_t oot_prop_palette(const uint8_t *src, uint64_t size) {
    if (g_oot_prop_reg < 0 || S.ubo[0][1].size < 76) return 0;
    float model[19]; pend_materialize(S.ubo[0][1].addr, (uint8_t *)model, sizeof model, 0);
    const float to_world[12] = { 1, 0, 0, model[16], 0, 1, 0, model[17], 0, 0, 1, model[18] }, to_palette[12] = { 1, 0, 0, -model[16], 0, 1, 0, -model[17], 0, 0, 1, -model[18] };
    float move[12];
    oot_m_mul(g_oot_track[g_oot_prop_side].delta, to_world, move); oot_m_mul(to_palette, move, move);
    uint64_t gpu = 0; uint8_t *to = oot_palette_ring(size, &gpu);
    for (uint64_t k = 0; k + 48 <= size; k += 48) {
        float m[12]; memcpy(m, src + k, 48); oot_m_mul(move, m, m); memcpy(to + k, m, 48);
    }
    return gpu;
}
