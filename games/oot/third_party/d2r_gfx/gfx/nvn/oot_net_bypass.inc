/* OOT (2026-11-08; the user allows lower picture quality for Ocarina of Time on the Mac): the game's spatial upscaling
   network at the end of each frame, replaced by a MetalFX spatial upscale. Default on since 2026-11-08 evening (the user,
   after comparing: with the game's network thin branches flickered continuously at the fixed 1536x864, with MetalFX they
   did not; docked 42.1 -> 45.2 fps in the same-run switch of 145ee444). OOT_NET_BYPASS=0 or "flag net_bypass 0" (or the
   settings panel, saved in graphics-net-bypass.txt) keeps the game's network. The flicker on the network path is not
   fixed by this, only not shown; its cause (the network fed by the fixed resolution) is still to be found.

   What the frame does (TOTK_BIND_TRACE and dispatch timings, claude/oot-net-bypass c2 / c3 / d1, by storage and serial):
     fragment            writes T, the tonemapped picture, in a 1536x864 texture (NVN format 0x3f); its viewport is the
                         rendered region, which the game's dynamic resolution moves (1536x864 at first, about 683x384 in
                         the settled village)
     compute  f8965bff   T -> 384x216 (0x14), plus two storage buffers it writes          kept: other readers unknown
     compute  d17eefaf   384x216 + a resident 192x128 table -> B (0x25)                    skipped
     compute  a335c1db   B, T (texture slot 3) -> C (0x40, image slot 2)                   skipped
     compute  network    C -> O, the 2048x1152 canvas (0x3f); fa3033d0 at full resolution, d99fbeeb at the low one;
                         1.44-1.47 ms a frame either way                                    replaced: MetalFX T -> O
     fragment            the HUD, drawn onto O
   The network is recognised by what it reads (the first dispatch after a335c1db, in the same frame, that samples C),
   not by its key, so every resolution variant is covered. B and C are read by nothing else in the frame.

   Safety: d17eefaf and a335c1db are skipped only in frames that start with a scaler ready and T's rendered region known,
   so the network never runs on a stale C unless the MetalFX encode itself fails (logged; bypass waits until ready again).
   MetalFX writes O directly when O's usage allows it; otherwise into its own texture, copied to O when the formats match.
   Neither possible: the game's network stays (logged once). */
#define NB_PREP_KEY "d17eefaff4f0bc23"
#define NB_MIX_KEY "a335c1db6270c355"
enum { NB_T_SLOT = 3, NB_C_SLOT = 2, NB_DIRECT_TRIES = 30, NB_VP = 8 };
static _Atomic int g_nb_on = -1;
static int g_nb_ready, g_nb_own, g_nb_tries, g_nb_gave_up;   // replay thread only
static uint64_t g_nb_skip_frame = ~0ull, g_nb_mix_frame = ~0ull, g_nb_c_serial, g_nb_done, g_nb_fail, g_nb_novp;
static __strong id<MTLTexture> g_nb_t; static const void *g_nb_t_mem;
static struct { const void *mem; float w, h; uint64_t frame; } g_nb_vp[NB_VP]; static int g_nb_vp_next;
static int netbp_on(void) {
    int v = atomic_load_explicit(&g_nb_on, memory_order_relaxed);
    if (v < 0) { const char *e = getenv("OOT_NET_BYPASS"); v = e ? strcmp(e, "0") != 0 : 1; atomic_store(&g_nb_on, v);
        if (!v) MLOG("[net-bypass] OOT_NET_BYPASS=0: the game's own end-of-frame network stays (MetalFX is the default)");
        else MLOG("[net-bypass] %s: the end-of-frame network is replaced by MetalFX once a scaler is ready", e ? "OOT_NET_BYPASS=1" : "default"); }
    return v;
}
static void netbp_set(int on) { atomic_store(&g_nb_on, on != 0); MLOG("[flag] net_bypass = %d", on != 0); }
/* Every draw: the viewport of the render target it draws into (the rendered region of T, also the FPS panel's internal resolution). */
static void netbp_note_draw(void) {
    if (!R.has_rt || !R.rt.color[0] || !S.has_vp) return;
    const void *mem = (const void *)R.rt.color[0]->mem; uint64_t f = atomic_load(&g_cur_frame);
    for (int i = 0; i < NB_VP; i++) if (g_nb_vp[i].mem == mem) { g_nb_vp[i].w = S.vp[0][2]; g_nb_vp[i].h = S.vp[0][3]; g_nb_vp[i].frame = f; return; }
    int i = g_nb_vp_next++ % NB_VP; g_nb_vp[i].mem = mem; g_nb_vp[i].w = S.vp[0][2]; g_nb_vp[i].h = S.vp[0][3]; g_nb_vp[i].frame = f;
}
static int netbp_region(const void *mem, uint64_t f, NSUInteger *w, NSUInteger *h) {
    for (int i = 0; i < NB_VP; i++) if (g_nb_vp[i].mem == mem && g_nb_vp[i].frame == f && g_nb_vp[i].w >= 1 && g_nb_vp[i].h >= 1) {
        *w = (NSUInteger)lrintf(g_nb_vp[i].w); *h = (NSUInteger)lrintf(g_nb_vp[i].h); return 1; }
    return 0;
}
static int netbp_tid(int slot, int image) { return image ? (slot < 32 ? (int)(S.img[5][slot] & 0xfffff) : 0) : (slot < 128 ? (int)(S.tex[5][slot] & 0xfffff) : 0); }
/* The texture table (g_treg) is reallocated when a game thread registers a texture past its end (mtl_reg_tex, under g_rmu):
   these run on the replay thread, so every read takes the lock (as TOTK's tex_fast_lk, 9280ef3f; g_rmu is reentrant). */
static id<MTLTexture> netbp_tex(int tid, int image) {
    void *tv = NULL;
    if (tid) { if (image) { registry_lock(); (void)image_fast_rid(tid, &tv); registry_unlock(); } else (void)tex_fast_rid_lk(tid, &tv); }
    return (__bridge id<MTLTexture>)tv;
}
static uint64_t netbp_serial(int tid) { registry_lock(); uint64_t s = tid > 0 && tid < g_ntreg ? g_treg[tid].t.serial : 0; registry_unlock(); return s; }
static const void *netbp_mem(int tid) { registry_lock(); const void *m = tid > 0 && tid < g_ntreg ? (const void *)g_treg[tid].t.mem : NULL; registry_unlock(); return m; }
/* The network: a dispatch that samples C in the frame a335c1db wrote it. Its image binding is O. */
static int netbp_is_network_c(NvShader *cs, int *o_slot, uint64_t c_serial);
static int netbp_is_network(NvShader *cs, int *o_slot) { return netbp_is_network_c(cs, o_slot, g_nb_c_serial); }
static int netbp_is_network_c(NvShader *cs, int *o_slot, uint64_t c_serial) {
    int reads_c = 0; *o_slot = -1;
    for (int i = 0; i < cs->rf.nb; i++) { const nvn_rbind *b = &cs->rf.b[i];
        if (b->kind == NVN_RB_TEX) { int tid = netbp_tid(b->reg, 0); if (tid && netbp_serial(tid) == c_serial) reads_c = 1; }
        else if (b->kind == NVN_RB_IMG && *o_slot < 0) *o_slot = b->reg; }
    return reads_c && *o_slot >= 0;
}
static _Atomic uint32_t g_scene_w, g_scene_h;   // the FPS panel's internal resolution (defined with the compose hook below)
/* a335c1db reads T, the frame's tonemapped picture: T's rendered region is the size the game rendered this frame. */
static void netbp_publish_internal(void) {
    int tt = netbp_tid(NB_T_SLOT, 0); NSUInteger w = 0, h = 0;
    const void *mem = netbp_mem(tt);
    if (mem && netbp_region(mem, atomic_load(&g_cur_frame), &w, &h)) { atomic_store(&g_scene_w, (uint32_t)w); atomic_store(&g_scene_h, (uint32_t)h); }
}
/* 1: this dispatch is skipped or replaced; 0: it runs as usual. Called on the replay thread before the dispatch. */
/* The UI of a frame (2026-11-09, the aspect ratio's UI squeeze): the draws into O, the canvas the network writes, after the
   network in the same frame (nh1's frame log: every draw on the canvas after it is a HUD quad). Recorded whether or not
   the bypass is on. */
static uint64_t g_ui_mix_frame = ~0ull, g_ui_c_serial, g_ui_frame = ~0ull, g_ui_canvas;
static int oot_ui_draw(void) {
    return g_ui_canvas && atomic_load(&g_cur_frame) == g_ui_frame && R.rt.n > 0 && R.rt.color[0] && R.rt.color[0]->serial == g_ui_canvas;
}
static void netbp_note_ui(NvShader *cs) {
    uint64_t f = atomic_load(&g_cur_frame);
    if (!strncmp(cs->key, NB_MIX_KEY, 16)) { int tc = netbp_tid(NB_C_SLOT, 1); g_ui_c_serial = tc ? netbp_serial(tc) : 0; g_ui_mix_frame = f; return; }
    int o_slot; if (g_ui_mix_frame != f || !g_ui_c_serial || !netbp_is_network_c(cs, &o_slot, g_ui_c_serial)) return;
    int to = netbp_tid(o_slot, 1); uint64_t canvas = to ? netbp_serial(to) : 0; if (canvas) { g_ui_canvas = canvas; g_ui_frame = f; }
    g_ui_mix_frame = ~0ull;
}
static int netbp_dispatch(NvShader *cs) {
    netbp_note_ui(cs);
    if (!strncmp(cs->key, NB_MIX_KEY, 16)) netbp_publish_internal();
    if (!netbp_on()) { g_nb_ready = 0; return 0; }
    uint64_t f = atomic_load(&g_cur_frame);
    if (!strncmp(cs->key, NB_PREP_KEY, 16)) { if (g_nb_ready) { g_nb_skip_frame = f; return 1; } return 0; }
    if (!strncmp(cs->key, NB_MIX_KEY, 16)) {
        int tt = netbp_tid(NB_T_SLOT, 0), tc = netbp_tid(NB_C_SLOT, 1);
        g_nb_t = netbp_tex(tt, 0); g_nb_t_mem = tt ? netbp_mem(tt) : NULL;
        g_nb_c_serial = tc ? netbp_serial(tc) : 0; g_nb_mix_frame = f;
        return g_nb_skip_frame == f;
    }
    int o_slot; if (g_nb_mix_frame != f || !g_nb_c_serial || !netbp_is_network(cs, &o_slot)) return 0;
    g_nb_mix_frame = ~0ull;   // one network a frame
    int skipping = g_nb_skip_frame == f;
    prepare_shader_images(cs, 5);   // O as the game's image binding would have it (created / given write usage)
    id<MTLTexture> t = g_nb_t, o = netbp_tex(netbp_tid(o_slot, 1), 1); NSUInteger cw = 0, ch = 0;
    int have_region = netbp_region(g_nb_t_mem, f, &cw, &ch);
    const char *bad = !t ? "no T" : !o ? "no O" : t.textureType != MTLTextureType2D || o.textureType != MTLTextureType2D ? "not 2D" :
                      !have_region ? "T's rendered region not seen this frame" : cw > t.width || ch > t.height ? "region larger than T" :
                      cw >= o.width || ch >= o.height ? "region not smaller than O" : NULL;
    { static uint64_t seen; if (!seen++ || (!bad && seen % 1800 == 0)) MLOG("[net-bypass] network %.16s: T %lux%lu fmt %lu region %lux%lu -> O %lux%lu fmt %lu usage %#lx%s%s", cs->key,
          (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, (unsigned long)cw, (unsigned long)ch, (unsigned long)o.width, (unsigned long)o.height,
          (unsigned long)o.pixelFormat, (unsigned long)o.usage, bad ? "; not replaced: " : "", bad ? bad : ""); }
    if (bad) { g_nb_ready = 0; if (!have_region) g_nb_novp++; if (skipping) { g_nb_fail++; MLOG("【红】[net-bypass] frame %llu: %s after skipping its input; the network ran on the previous C", (unsigned long long)f, bad); } return 0; }
    if (!skipping) {   // the network runs this frame; get a scaler ready for the next ones
        if (g_nb_gave_up) return 0;
        if (!g_nb_own) { g_nb_ready = mfxup_prepare(MFXUP_DOCKED, t, o, o.width, o.height, MFXUP_PERCEPTUAL); if (!g_nb_ready && ++g_nb_tries > NB_DIRECT_TRIES) { g_nb_own = 1; g_nb_tries = 0; MLOG("[net-bypass] MetalFX cannot write O directly; trying its own texture and a copy"); } }
        else { g_nb_ready = mfxup_prepare(MFXUP_DOCKED, t, nil, o.width, o.height, MFXUP_PERCEPTUAL); if (!g_nb_ready && ++g_nb_tries > NB_DIRECT_TRIES) { g_nb_gave_up = 1; MLOG("【红】[net-bypass] no MetalFX scaler for T fmt %lu -> O fmt %lu: the game's network stays", (unsigned long)t.pixelFormat, (unsigned long)o.pixelFormat); } }
        if (g_nb_ready) MLOG("[net-bypass] scaler ready (%s); replacing from the next frame", g_nb_own ? "own texture + copy" : "direct");
        return 0;
    }
    rp_end(); blit_close(); rs_sync();   // MetalFX encodes on the command buffer itself
    pt_mark(g_cb, 'X', nil);
    id<MTLTexture> w = mfxup_encode_ex(MFXUP_DOCKED, g_cb, t, cw, ch, g_nb_own ? nil : o, o.width, o.height, MFXUP_PERCEPTUAL);
    if (w && g_nb_own) {
        if (w.pixelFormat != o.pixelFormat) { MLOG("【红】[net-bypass] MetalFX's own texture is format %lu, O is %lu: no copy; the game's network stays", (unsigned long)w.pixelFormat, (unsigned long)o.pixelFormat); g_nb_gave_up = 1; w = nil; }
        else enc_b_copy_texture(blit(), w, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(o.width, o.height, 1), o, 0, 0, MTLOriginMake(0, 0, 0));
    }
    mfxup_queue_residency();
    if (!w) { g_nb_ready = 0; g_nb_fail++; MLOG("【红】[net-bypass] MetalFX encode failed in frame %llu: the network ran on the previous C", (unsigned long long)f); return 0; }
    g_nb_done++;
    if (g_nb_done == 1 || g_nb_done % 300 == 0) MLOG("[net-bypass] replaced %llu frames (%s), region %lux%lu, failed %llu, region unknown %llu", (unsigned long long)g_nb_done,
                                                     g_nb_own ? "own texture + copy" : "direct", (unsigned long)cw, (unsigned long)ch, (unsigned long long)g_nb_fail, (unsigned long long)g_nb_novp);
    return 1;
}
int netbp_flag(void) { return netbp_on(); }   // the settings panel
void netbp_set_flag(int on) { netbp_set(on); }   // "flag net_bypass 0|1" (the flag handler comes earlier in nvn_mtl.m)
