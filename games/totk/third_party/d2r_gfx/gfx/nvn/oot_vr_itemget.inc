/* zeldaVR, Ocarina of Time, first person: an item get's item before the player's eyes (vr.md row 71). The game binds the item's
   model above Link's head and turns him to show it to its camera; from Link's eyes it is over the player's head. HookshotVR's
   notes (held-items.md 8; vr/items/item_get, GPL-3: the idea and its numbers, no code): while an item get may play (any scene but
   a story cinematic or the ocarina: games/oot/runtime/oot_vr_scene.h runtime_oot_vr_item_get_may), a model of at most 8 palette
   matrices (Navi has 13) whose root is near a spot above Link's head is the item; it is drawn 0.5 m in front of and 0.08 m below
   the eyes, held still in the world, placed again after the eyes moved 1 m or 1 s without the item.
   Ours (not confirmed in a run; no save with a chest to open): Link's head joint is not among the bones we track, so "near a spot
   above his head" is taken as "0.15 to 1.0 m above his chest bone (Spine_2, oot_vr_arms.inc) and within 0.55 m of it across".
   The log says each time a model is taken, with its palette size and where its root was against the chest. Included by
   oot_vr_props.inc; only the palette's translation changes (the game's own turning of the item stays). */
extern int runtime_oot_vr_item_get_may(void) __attribute__((weak_import));
extern void runtime_oot_vr_item_shown(void) __attribute__((weak_import));
extern int runtime_vr_base_view(float out[12]) __attribute__((weak_import));
static int g_oot_prop_itemget;
static struct { int placed; float target[3], eye[3]; uint64_t seen_frame, placed_ns, seen_ns; float delta[3]; uint64_t delta_frame, delta_vb; } g_oot_itemget;
static uint64_t oot_itemget_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
/* The eye and the level way ahead in the world, from this draw's camera block (the scene's, 13568 bytes: oot_vr_weapon.inc) or the
   game's view as our camera hook left it. */
static int oot_itemget_eye(float eye[3], float ahead[3]) {
    float v[12];
    if (S.ubo[0][0].size == 13568) pend_materialize(S.ubo[0][0].addr, (uint8_t *)v, sizeof v, 0);
    else if (!runtime_vr_base_view || !runtime_vr_base_view(v)) return 0;
    for (int i = 0; i < 12; i++) if (!isfinite(v[i])) return 0;
    for (int i = 0; i < 3; i++) eye[i] = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
    const float fx = -v[8], fz = -v[10], l = sqrtf(fx * fx + fz * fz);
    if (l < 1e-3f) return 0;
    ahead[0] = fx / l; ahead[1] = 0; ahead[2] = fz / l;
    return 1;
}
/* From oot_prop_draw: is this model (palette `size` bytes, root `at` in the world) the item being shown? Then its move is known. */
static int oot_itemget_take(uint64_t size, const float at[3], const float chest[3]) {
    if (!runtime_oot_vr_item_get_may || !runtime_oot_vr_item_get_may() || size < 48 || size > 8 * 48) return 0;
    const float up = at[1] - chest[1], across = sqrtf((at[0] - chest[0]) * (at[0] - chest[0]) + (at[2] - chest[2]) * (at[2] - chest[2]));
    if (!(up > 0.15f && up < 1.0f && across < 0.55f)) return 0;
    float eye[3], ahead[3];
    if (!oot_itemget_eye(eye, ahead)) return 0;
    const uint64_t frame = atomic_load(&g_cur_frame), now = oot_itemget_now();
    if (g_oot_itemget.delta_frame == frame && g_oot_itemget.delta_vb == S.vbuf[0].address) return 1;   /* its other draw this frame */
    const float moved = sqrtf((eye[0] - g_oot_itemget.eye[0]) * (eye[0] - g_oot_itemget.eye[0]) + (eye[1] - g_oot_itemget.eye[1]) * (eye[1] - g_oot_itemget.eye[1]) + (eye[2] - g_oot_itemget.eye[2]) * (eye[2] - g_oot_itemget.eye[2]));
    if (!g_oot_itemget.placed || moved > 1.0f || now - g_oot_itemget.seen_ns > 1000000000ull) {
        for (int i = 0; i < 3; i++) { g_oot_itemget.target[i] = eye[i] + ahead[i] * 0.5f; g_oot_itemget.eye[i] = eye[i]; }
        g_oot_itemget.target[1] -= 0.08f; g_oot_itemget.placed = 1; g_oot_itemget.placed_ns = now;
        MLOG("[vr-itemget] an item get's model (%llu palette matrices) goes before the eyes: its root %.2f m above Link's chest and %.2f m across, now at %.2f %.2f %.2f",
             (unsigned long long)(size / 48), (double)up, (double)across, (double)g_oot_itemget.target[0], (double)g_oot_itemget.target[1], (double)g_oot_itemget.target[2]);
    }
    for (int i = 0; i < 3; i++) g_oot_itemget.delta[i] = g_oot_itemget.target[i] - at[i];
    g_oot_itemget.seen_ns = now; g_oot_itemget.seen_frame = frame; g_oot_itemget.delta_frame = frame; g_oot_itemget.delta_vb = S.vbuf[0].address;
    if (runtime_oot_vr_item_shown) runtime_oot_vr_item_shown();
    return 1;
}
/* The item's palette moved by the delta, in the ring of ours. */
static uint64_t oot_itemget_palette(const uint8_t *src, uint64_t size) {
    if (size < 48 || size > 8 * 48) return 0;
    uint64_t gpu = 0; uint8_t *out = oot_palette_ring(size, &gpu);
    for (uint64_t k = 0; k + 48 <= size; k += 48) {
        float m[12]; memcpy(m, src + k, 48);
        m[3] += g_oot_itemget.delta[0]; m[7] += g_oot_itemget.delta[1]; m[11] += g_oot_itemget.delta[2];
        memcpy(out + k, m, 48);
    }
    return gpu;
}
