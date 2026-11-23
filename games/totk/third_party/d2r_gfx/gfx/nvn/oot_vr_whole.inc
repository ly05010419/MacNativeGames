/* zeldaVR, Ocarina of Time, first person: Link's arms and what he holds are never faded out. The game fades its hero when its own
   camera comes near him (aiming upward, a narrow place), and in first person that camera is nowhere the player can see. The opacity
   is word 107 of the material block (uniform block 2 of the vertex and fragment stages). It goes back to 1 for these draws only, and
   they write their depth again, since each eye's picture is made from the depth. "flag vr_keep_whole 0" lets the game fade him.
   The idea follows OcarinaVR's description (ideas only, GPL-3). Included by oot_vr_arms.inc. */
enum { OOT_OPACITY_AT = 428, OOT_MATERIAL_MIN = 432, OOT_MATERIAL_MAX = 4096 };
static _Atomic int g_oot_keep_whole = 1;
static uint64_t g_oot_whole_blocks, g_oot_whole_depth;

void oot_keep_whole_set(int on) { atomic_store(&g_oot_keep_whole, on != 0); MLOG("[flag] vr_keep_whole = %d", on != 0); }
static float oot_block_opacity(const p_bind_buf *u) {
    if (!u->addr || u->size < OOT_MATERIAL_MIN || u->size > OOT_MATERIAL_MAX) return -1;
    float a = -1; pend_materialize(u->addr + OOT_OPACITY_AT, (uint8_t *)&a, sizeof a, 0);
    return a;
}
/* This draw of his arms or of what he holds is one the game has faded. */
static int oot_whole_faded(void) {
    if (!atomic_load_explicit(&g_oot_keep_whole, memory_order_relaxed)) return 0;
    const int faded = oot_hold_faded(oot_block_opacity(&S.ubo[0][2])) || oot_hold_faded(oot_block_opacity(&S.ubo[1][2]));
    if (faded && !g_oot_whole_depth++) MLOG("[vr] the game faded Link: his arms and what he holds stay whole and write their depth");
    return faded;
}
/* The material block with its opacity back to 1, in the ring of ours; 0 leaves the game's. */
static uint64_t oot_whole_block(const p_bind_buf *u) {
    if (!atomic_load_explicit(&g_oot_keep_whole, memory_order_relaxed) || !oot_hold_faded(oot_block_opacity(u))) return 0;
    uint64_t gpu = 0; uint8_t *to = oot_palette_ring(u->size, &gpu);
    pend_materialize(u->addr, to, (size_t)u->size, 0);
    const float one = 1; memcpy(to + OOT_OPACITY_AT, &one, sizeof one);
    g_oot_whole_blocks++;
    return gpu;
}
