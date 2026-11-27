/* zeldaVR, Ocarina of Time: the game's own flat aim reticle hidden while the VR one stands in for it (vr.md row 61; HookshotVR's
   notes, aim-items.md 1.8, GPL-3: facts, no code). The game's reticle textures are known by their whole compressed level: BC4
   (NVN RGTC1) unsigned, 32 x 32 or 32 x 64, one level, one layer, and a 64-bit FNV-1a hash of the first 512 or 1024 bytes of
   the texture's storage in the game's memory equal to one of four values (HookshotVR hashes the guest storage too, through its
   texture's GPU address). While it is hidden such a texture's sampled view is one whose four channels read 0, wholly transparent;
   every other texture, other UI in the same draw included, is untouched.
   When: totk_vr.m vr_oot_flat_reticle_hidden (first person while the bow or slingshot is out or aimed these 250 ms, third person
   while our own reticle shows). A change clears the views of the textures known so far and steps the registration generation, as
   TotK's prompt icons do in mtl_exec_list, so argument buffers and texture slot caches take the new view; the descriptor-pool
   snapshots see the cleared slots through treg_clear_view_locked's change log. OOT_VR_FLAT_RETICLE=1 keeps the game's reticle.
   Not checked in a run yet (the test saves have no bow and our runs have not aimed with this code): the first match logs
   "[vr-reticle] the game's flat reticle texture ...". Included by nvn_mtl.m before tex_by_id_p, Ocarina of Time on the Mac only. */
int vr_oot_flat_reticle_hidden(void);   /* zeldaVR/gfx/oot_vr_wrist_draw.h */
enum { OOT_RETICLE_KNOWN = 32, OOT_RETICLE_SEEN = 512 };
static const uint64_t OOT_RETICLE_HASHES[4] = { 0xb21e07c5b9ac6e60ull, 0xa818490e80c585d4ull, 0x307aa2d484a23300ull, 0x71e624b24e9934d8ull };
static struct { int tid; uint64_t serial; } g_oot_reticle_known[OOT_RETICLE_KNOWN];
static int g_oot_reticle_nknown;
static struct { uint64_t serial; int match; } g_oot_reticle_seen[OOT_RETICLE_SEEN];   /* each texture hashed once, by its serial */
static int g_oot_reticle_hidden;   /* as last applied */
static int oot_reticle_keep(void) { static int v = -1; if (v < 0) { const char *e = getenv("OOT_VR_FLAT_RETICLE"); v = e && *e == '1'; } return v; }

static int oot_reticle_candidate(const tex_t *t) {
    return t && t->mem && t->d.format == NVN_FORMAT_RGTC1_UNORM && t->d.width == 32 && (t->d.height == 32 || t->d.height == 64) &&
           t->d.levels <= 1 && t->d.depth <= 1 && t->d.target == 1;
}
/* One of the game's reticle textures (caller holds g_rmu). */
static int oot_reticle_match(const tex_t *t) {
    if (!oot_reticle_candidate(t)) return 0;
    const unsigned slot = (unsigned)((t->serial * 0x9e3779b97f4a7c15ull) >> 55) % OOT_RETICLE_SEEN;
    if (g_oot_reticle_seen[slot].serial == t->serial && t->serial) return g_oot_reticle_seen[slot].match;
    uint8_t raw[1024]; const size_t n = t->d.height == 64 ? 1024 : 512;
    if (!guarded_memcpy(raw, t->mem, n, "flat reticle hash")) return 0;   /* not readable now: asked again next time */
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < n; i++) h = (h ^ raw[i]) * 1099511628211ull;
    int match = 0;
    for (int i = 0; i < 4; i++) match |= h == OOT_RETICLE_HASHES[i];
    g_oot_reticle_seen[slot].serial = t->serial; g_oot_reticle_seen[slot].match = match;
    if (match) MLOG("[vr-reticle] the game's flat reticle texture: serial %llu, %ux%u, hash %016llx", (unsigned long long)t->serial, t->d.width, t->d.height, (unsigned long long)h);
    return match;
}
/* tex_by_id_p, the view just made for tid: the transparent one instead while hidden (caller holds g_rmu). */
static id<MTLTexture> oot_reticle_view(int tid, const tex_t *t, id<MTLTexture> v) {
    if (!v || oot_reticle_keep() || !oot_reticle_match(t)) return v;
    int known = 0;
    for (int i = 0; i < g_oot_reticle_nknown; i++) known |= g_oot_reticle_known[i].tid == tid && g_oot_reticle_known[i].serial == t->serial;
    if (!known && g_oot_reticle_nknown < OOT_RETICLE_KNOWN) { g_oot_reticle_known[g_oot_reticle_nknown].tid = tid; g_oot_reticle_known[g_oot_reticle_nknown].serial = t->serial; g_oot_reticle_nknown++; }
    if (!g_oot_reticle_hidden) return v;
    id<MTLTexture> base = v.parentTexture ? v.parentTexture : v;
    if (base.textureType != MTLTextureType2D) return v;
    const MTLTextureSwizzleChannels zero = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleZero);
    id<MTLTexture> hidden = [base newTextureViewWithPixelFormat:base.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:zero];
    if (!hidden) { static int said; if (!said++) MLOG("[vr-reticle] no transparent view for the reticle texture: shown as it is"); return v; }
    static int said; if (said++ < 4) MLOG("[vr-reticle] texture id %d: the game's flat reticle is hidden", tid);
    return hidden;
}
/* The start of each command list's replay: a change of whether it is hidden reaches the known textures. */
static void oot_reticle_frame(void) {
    const int hide = !oot_reticle_keep() && vr_oot_flat_reticle_hidden();
    if (hide == g_oot_reticle_hidden) return;
    registry_lock();
    g_oot_reticle_hidden = hide;
    int cleared = 0;
    for (int i = 0; i < g_oot_reticle_nknown; i++) {
        const int tid = g_oot_reticle_known[i].tid;
        if (tid > 0 && tid < g_ntreg && g_treg[tid].ok && g_treg[tid].t.serial == g_oot_reticle_known[i].serial && g_treg[tid].view) { treg_clear_view_locked(tid); cleared++; }
    }
    g_reg_gen++;
    registry_unlock();
    static int lines; if (lines++ < 40) MLOG("[vr-reticle] the game's flat reticle %s (%d known textures, %d views made again)", hide ? "hidden" : "shown again", g_oot_reticle_nknown, cleared);
}
