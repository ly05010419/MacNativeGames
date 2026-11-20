/* The corner panel and the FPS / shader / load boxes every game shows (user 2026-11-20: "fps pannel 也要统一，测试保证"),
   one copy for every game's gfx/nvn/nvn_mtl.m, which includes it where its own copy was. It is Tears of the Kingdom's
   (its nvn_mtl.m, the newest: the boxes keep out of an iPhone's safe area, the shader line under the FPS box on the phone,
   each panel line yellow on its own change). What differs per game the including file defines before the include:
     static void hud_fps_sizes(uint64_t cur, int *ow, int *oh, int *sw, int *sh);   output and internal resolution, 0 = unknown
     static void hud_pso_counts(uint64_t *fresh, uint64_t *last_ns);               pipelines new this run, when the last came
     static void hud_pso_total(uint64_t *fresh, int64_t *all);                      the PSO line's two numbers
     static uint64_t hud_rt_missed(void);                                           iPhone: shader variants the device lacked
     static void hud_more_lines(uint64_t now);                                      its own extra panel lines (or nothing)
   with the textures and keys it already has (g_fps_tex, g_shd_tex at LOAD_OVERLAY_TEX_W, g_load_tex, their keys and boxes).
   tests/test_hud_panel_shared.py keeps every game on this file. */
// TOTK: the FPS the overlay shows, for the menu bar (frontend.m): game presents per second, -1 before the first
// measurement; *generated: interpolated frames per second while frame interpolation shows them, else 0.
int dm_gfx_fps_now(int *generated) {
    uint64_t gen_at = atomic_load(&g_mfxi_last), cur = atomic_load(&g_cur_frame);
    int gen = gen_at && cur - gen_at < 30 ? fps_overlay_gen_value() : 0;
    if (generated) *generated = gen > 0 ? gen : 0;
    return fps_overlay_value();
}
/* iPhone held sideways (2026-10-25, user: the FPS box was cut off): the notch and the rounded corners cover the screen's
   sides, so the corner boxes keep out of its safe area, which the front end reports in drawable pixels (frontend_ios.m
   runtime_ios_sync_drawable). Only the part of the inset the picture reaches counts: a 16:9 picture between bars needs
   none. Nothing reports it on the Mac, where the boxes stay at the corners. */
static _Atomic int g_hud_safe_w, g_hud_safe_h, g_hud_safe_side, g_hud_safe_bottom;
void mtl_hud_safe_area(int w, int h, int side, int bottom) {
    int was_side = atomic_exchange(&g_hud_safe_side, side), was_bottom = atomic_exchange(&g_hud_safe_bottom, bottom);
    int changed = was_side != side || was_bottom != bottom;
    atomic_store(&g_hud_safe_w, w); atomic_store(&g_hud_safe_h, h);
    if (changed) MLOG("[hud] 安全区：两侧 %d 像素、底部 %d 像素（画布 %dx%d），角上的提示框避开它", side, bottom, w, h);
}
static double hud_inset_right(MTLViewport vp) {
    int w = atomic_load(&g_hud_safe_w), s = atomic_load(&g_hud_safe_side); double gap = w - (vp.originX + vp.width);
    return w > 0 && s > gap ? s - gap : 0;
}
static double hud_inset_bottom(MTLViewport vp) {
    int h = atomic_load(&g_hud_safe_h), b = atomic_load(&g_hud_safe_bottom); double gap = h - (vp.originY + vp.height);
    return h > 0 && b > gap ? b - gap : 0;
}
// TOTK: FPS, output resolution and (internal resolution) in the top-right corner of the game picture (pixels come from fps_overlay.c). Rasterises into
// the slot's texture only when what is shown changes, and returns the viewport and source rectangle to draw.
static int fps_overlay_prepare(int slot, MTLViewport vp, MTLViewport *ovp, float uv[8]) {
    if (!fps_overlay_hints_enabled() || !fps_overlay_enabled() || slot < 0 || slot > 3 || !g_fps_tex[slot]) return 0;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint8_t px[FPS_OVERLAY_TEX_W * FPS_OVERLAY_TEX_H * 4];
    int scale = fps_overlay_scale(vp.height);
    uint64_t mfx_at = atomic_load(&g_mfxup_last), gen_at = atomic_load(&g_mfxi_last), cur = atomic_load(&g_cur_frame);   // the last game frame MetalFX did the upscale (" MFX") / an interpolated frame was shown ("+50")
    int gen = gen_at && cur - gen_at < 30 ? fps_overlay_gen_value() : 0;
    int ow = 0, oh = 0, sw = 0, sh = 0; hud_fps_sizes(cur, &ow, &oh, &sw, &sh);   // the game's: output and internal resolution (0 = not known)
    fps_overlay_info info = {fps_overlay_value(), ow ? ow : (int)lround(vp.width), oh ? oh : (int)lround(vp.height), mfx_at && cur - mfx_at < 30, gen > 0 ? gen : 0, atomic_load(&g_pic_big),
                             sw, sh};
    { static int lw, lh, lsw, lsh; if (info.screen_w != lw || info.screen_h != lh || info.scene_w != lsw || info.scene_h != lsh) {   // what the overlay's resolutions now say
        lw = info.screen_w; lh = info.screen_h; lsw = info.scene_w; lsh = info.scene_h;
        MLOG("[overlay] resolution shown: output %dx%d, internal %dx%d (0 = not known); drawable viewport %.0fx%.0f", lw, lh, lsw, lsh, vp.width, vp.height); } }
    if (hud_compact()) { char text[96]; fps_overlay_text(&info, text, sizeof text); hud_publish(HUD_FPS, text, 0); return 0; }   // shown in the window's corner panel
    pthread_mutex_lock(&mu);
    if (g_fps_key[slot].scale != scale || memcmp(&g_fps_key[slot].info, &info, sizeof info)) {
        fps_overlay_raster(&info, scale, px, &g_fps_box[slot][0], &g_fps_box[slot][1]);
        [g_fps_tex[slot] replaceRegion:MTLRegionMake2D(0, 0, FPS_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H) mipmapLevel:0 withBytes:px bytesPerRow:FPS_OVERLAY_TEX_W * 4];
        g_fps_key[slot].info = info; g_fps_key[slot].scale = scale;
    }
    float bw = g_fps_box[slot][0], bh = g_fps_box[slot][1], W = FPS_OVERLAY_TEX_W, H = FPS_OVERLAY_TEX_H;
    pthread_mutex_unlock(&mu);
    double m = 3.0 * scale;
    *ovp = (MTLViewport){vp.originX + vp.width - m - bw - hud_inset_right(vp), vp.originY + m, bw, bh, 0, 1};   // top-right corner
    const float u[8] = {0, 0, bw / W, bh / H, 0.5f / W, 0.5f / H, (bw - 0.5f) / W, (bh - 0.5f) / H};
    memcpy(uv, u, sizeof u);
    return 1;
}
/* TOTK automatic shader collection, bottom-left box (user, 2026-10-16 night: "is anything new being found and collected"):
   "SHADER +<variants new to the translation cache this run>/<variants in the cache> PSO +<pipelines added to the warm
   lists this run>", yellow for 3 s after something new was collected. Same texture size, font and scale as the FPS box. */
/* A corner-panel line, yellow for 3 s after its text (its numbers) changed: each line for its own kind (user 2026-11-16).
   Before, one "something was collected" time turned the box yellow, so a shader library or compute pipeline row made it
   yellow with no number that changed. The first text of a line is not a change. */
static void hud_line_publish(int line, const char *text, uint64_t now) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static char prev[HUD_LINES][HUD_TEXT_MAX]; static uint64_t changed[HUD_LINES];
    if (line < 0 || line >= HUD_LINES) return;
    pthread_mutex_lock(&mu);
    if (strcmp(prev[line], text)) { if (prev[line][0]) changed[line] = now; snprintf(prev[line], sizeof prev[line], "%s", text); }
    int hl = changed[line] && now - changed[line] < 3000000000ull;
    pthread_mutex_unlock(&mu);
    hud_publish(line, text, hl);
}
/* TOTK_HUD_LOG=<seconds> (a diagnostic, off unless set; 2026-11-16): every so often the corner panel's lines go into the
   game log as the panel shows them, "（黄）" for a yellow one. The panel is a view over the game's picture, so wshot does
   not see it; a test reads what the player would read here. */
static void hud_log_lines(uint64_t now) {
    static double every = -1; static uint64_t next;
    if (every < 0) { const char *e = getenv("TOTK_HUD_LOG"); every = e ? atof(e) : 0; }
    if (every <= 0 || now < next) return;
    next = now + (uint64_t)(every * 1e9);
    static const int order[] = HUD_ORDER;
    for (size_t o = 0; o < sizeof order / sizeof *order; o++) {
        char t[HUD_TEXT_MAX]; int hl = 0;
        if (hud_snapshot(order[o], t, sizeof t, &hl)) MLOG("[面板] %s%s", hl ? "（黄）" : "", t);
    }
}
static int shader_overlay_prepare(int slot, MTLViewport vp, MTLViewport *ovp, float uv[8]) {
    if (!fps_overlay_full_hud() || !shader_overlay_enabled() || slot < 0 || slot > 3 || !g_shd_tex[slot]) return 0;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint8_t px[LOAD_OVERLAY_TEX_W * FPS_OVERLAY_TEX_H * 4];
    uint64_t fresh = 0, last = 0; int64_t total = -1; totk_rt_counts(&fresh, &total, &last);
    uint64_t pso = 0, plast = 0; hud_pso_counts(&pso, &plast);   // the game's: pipelines new this run, when the last one came
#if TARGET_OS_IPHONE
    {   // the phone translates nothing: show the shaders it is missing, yellow for 3 s after a new one
        static _Atomic uint64_t seen, seen_ns; uint64_t m = hud_rt_missed();
        if (m != atomic_exchange(&seen, m)) atomic_store(&seen_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
        fresh = m; last = atomic_load(&seen_ns); plast = 0;
    }
#endif
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), newest = last > plast ? last : plast;
    char text[160]; shader_overlay_text(text, 48, fresh, total, pso);
    int hl = newest && now - newest < 3000000000ull, scale = fps_overlay_scale(vp.height);
    /* Total: all the pipelines there are (the archive's, plus this run's new ones, which reach it at the next start), not
       only those built so far this run; never fewer than those (par_total). */
    uint64_t pso_new = 0; int64_t pso_all = 0; hud_pso_total(&pso_new, &pso_all);
    char line[64]; pso_overlay_text(line, sizeof line, pso_new, pso_all);
    if (hud_compact()) {   // the window's corner panel: every kind on its own line (hud_line_publish)
        hud_line_publish(HUD_SHADER, text, now); hud_line_publish(HUD_PSO, line, now);
        hud_more_lines(now);   // the game's own lines (Tears of the Kingdom: shader libraries, play, kinds, misc, misses)
        hitch_overlay_publish(now);   // every long frame (hle_nvn.c notes them), the last ones split into their parts
        hud_log_lines(now);
        return 0;
    }
    size_t at = strlen(text); snprintf(text + at, sizeof text - at, "   %s", line);   // picture corners (the iPhone): one line
    pthread_mutex_lock(&mu);
    if (g_shd_key[slot].scale != scale || g_shd_key[slot].hl != hl || strcmp(g_shd_key[slot].text, text)) {
        fps_overlay_raster_line_tex(text, hl, scale, px, LOAD_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H, &g_shd_box[slot][0], &g_shd_box[slot][1]);
        [g_shd_tex[slot] replaceRegion:MTLRegionMake2D(0, 0, LOAD_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H) mipmapLevel:0 withBytes:px bytesPerRow:LOAD_OVERLAY_TEX_W * 4];
        snprintf(g_shd_key[slot].text, sizeof g_shd_key[slot].text, "%s", text); g_shd_key[slot].hl = hl; g_shd_key[slot].scale = scale;
    }
    float bw = g_shd_box[slot][0], bh = g_shd_box[slot][1], W = LOAD_OVERLAY_TEX_W, H = FPS_OVERLAY_TEX_H;
    pthread_mutex_unlock(&mu);
    double m = 3.0 * scale;
#if TARGET_OS_IPHONE
    double below = fps_overlay_hints_enabled() && fps_overlay_enabled() ? g_fps_box[slot][1] + m : 0;   // under the FPS box (top-right)
    *ovp = (MTLViewport){vp.originX + vp.width - m - bw - hud_inset_right(vp), vp.originY + m + below, bw, bh, 0, 1};
#else
    *ovp = (MTLViewport){vp.originX + m, vp.originY + vp.height - m - bh, bw, bh, 0, 1};   // bottom-left corner
#endif
    const float u[8] = {0, 0, bw / W, bh / H, 0.5f / W, 0.5f / H, (bw - 0.5f) / W, (bh - 0.5f) / H};
    memcpy(uv, u, sizeof u);
    return 1;
}
/* TOTK load line, bottom-right (user, 2026-10-16 night: every CPU core and the GPU, one line like the FPS counter):
   "GPU 99% (70%)  P 55% 56% 55% 53%  E 43% 29% 20% 12%  9%  7%  T 47%" (GPU first, user 2026-10-25) -- the GPU as macOS reports it (our
   keep-alive keeps it near 100 %) and in brackets the GPU time of the game's own work. Values from totk_sysload.c (0.5 s). */
static int load_overlay_prepare(int slot, MTLViewport vp, MTLViewport *ovp, float uv[8]) {
    if (!fps_overlay_full_hud() || !sysload_enabled() || slot < 0 || slot > 3 || !g_load_tex[slot]) return 0;
    sysload_start();
    sysload_info li; if (!sysload_get(&li)) return 0;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint8_t px[LOAD_OVERLAY_TEX_W * FPS_OVERLAY_TEX_H * 4];
    char text[96]; sysload_text(text, sizeof text, &li);
    if (hud_compact()) { hud_publish(HUD_LOAD, text, 0); return 0; }
    int scale = fps_overlay_scale(vp.height);
    pthread_mutex_lock(&mu);
    if (g_load_key[slot].scale != scale || strcmp(g_load_key[slot].text, text)) {
        fps_overlay_raster_line_tex(text, 0, scale, px, LOAD_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H, &g_load_box[slot][0], &g_load_box[slot][1]);
        [g_load_tex[slot] replaceRegion:MTLRegionMake2D(0, 0, LOAD_OVERLAY_TEX_W, FPS_OVERLAY_TEX_H) mipmapLevel:0 withBytes:px bytesPerRow:LOAD_OVERLAY_TEX_W * 4];
        snprintf(g_load_key[slot].text, sizeof g_load_key[slot].text, "%s", text); g_load_key[slot].scale = scale;
    }
    float bw = g_load_box[slot][0], bh = g_load_box[slot][1], W = LOAD_OVERLAY_TEX_W, H = FPS_OVERLAY_TEX_H;
    pthread_mutex_unlock(&mu);
    double m = 3.0 * scale;
    *ovp = (MTLViewport){vp.originX + vp.width - m - bw - hud_inset_right(vp), vp.originY + vp.height - m - bh - hud_inset_bottom(vp), bw, bh, 0, 1};   // bottom-right corner
    const float u[8] = {0, 0, bw / W, bh / H, 0.5f / W, 0.5f / H, (bw - 0.5f) / W, (bh - 0.5f) / H};
    memcpy(uv, u, sizeof u);
    return 1;
}
