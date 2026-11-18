/* OOT_COPY_SITES=1 (diagnostic, off by default): bytes and calls of every host copy by nvn_mtl.m source line, so the
   host copy work on the GPU (a Metal 4 compute encoder per batch, unlabelled in Metal System Trace: 128 ms of GPU time
   per second in claude/oot-fps/trace01, 2026-11-06) can be attributed to the code that issues it. Every 300 game frames
   the 15 heaviest lines are logged as per-frame averages and the counters restart. Included after the copy wrappers;
   the macros below wrap them (a macro is not re-expanded inside its own expansion, so the inner names call the
   functions). Texture copies are counted as texels x the texture's allocated bytes per texel (an estimate; mips count). */
enum { COPY_SITE_LINES = 32768 };
static struct { _Atomic uint64_t calls, bytes; } g_copy_site[COPY_SITE_LINES];
static _Atomic int g_copy_sites_enabled = -1;
static int copy_sites_on(void) { int v=atomic_load_explicit(&g_copy_sites_enabled,memory_order_relaxed); if(v<0){const char *e=getenv("OOT_COPY_SITES");v=e&&atoi(e);atomic_store(&g_copy_sites_enabled,v);}return v; }
static inline void copy_site_note(int line, uint64_t bytes) {
    if (__builtin_expect(!copy_sites_on(), 1) || line < 0 || line >= COPY_SITE_LINES) return;
    atomic_fetch_add_explicit(&g_copy_site[line].calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_copy_site[line].bytes, bytes, memory_order_relaxed);
}
static inline uint64_t copy_tex_bytes(id<MTLTexture> t, MTLSize z) {
    uint64_t texels = (uint64_t)z.width * z.height * (z.depth ? z.depth : 1), all = (uint64_t)t.width * t.height * (t.depth ? t.depth : 1) * (t.arrayLength ? t.arrayLength : 1);
    return all ? texels * (uint64_t)t.allocatedSize / all : 0;
}
typedef struct { int line; uint64_t bytes, calls; } copy_site_row;
static int copy_site_cmp(const void *a, const void *b) { uint64_t x = ((const copy_site_row *)a)->bytes, y = ((const copy_site_row *)b)->bytes; return x < y ? 1 : x > y ? -1 : 0; }
static void copy_sites_report(void) {
    if (!copy_sites_on()) return;
    static uint64_t last; uint64_t f = atomic_load(&g_cur_frame);
    if (!last) { last = f; return; }
    if (f < last + 300) return;
    uint64_t frames = f - last; last = f;
    static copy_site_row rows[COPY_SITE_LINES]; int n = 0; uint64_t all = 0, calls = 0;
    for (int i = 0; i < COPY_SITE_LINES; i++) {
        uint64_t c = atomic_exchange_explicit(&g_copy_site[i].calls, 0, memory_order_relaxed), b = atomic_exchange_explicit(&g_copy_site[i].bytes, 0, memory_order_relaxed);
        if (c) { rows[n++] = (copy_site_row){i, b, c}; all += b; calls += c; }
    }
    qsort(rows, (size_t)n, sizeof rows[0], copy_site_cmp);
    MLOG("[copy-sites] F%llu: %llu frames, %.2f MB and %.1f copies per frame", (unsigned long long)f, (unsigned long long)frames, all / 1048576.0 / frames, (double)calls / frames);
    for (int k = 0; k < n && k < 15; k++)
        MLOG("[copy-sites]   nvn_mtl.m:%d %.2f MB/frame, %.1f copies/frame", rows[k].line, rows[k].bytes / 1048576.0 / frames, (double)rows[k].calls / frames);
}
#define enc_b_copy_buffer(e, s, so, d, doff, n) (copy_site_note(__LINE__, (uint64_t)(n)), enc_b_copy_buffer(e, s, so, d, doff, n))
#define enc_b_copy_buffer_texture(e, s, so, row, img, z, d, sl, lv, o) (copy_site_note(__LINE__, (uint64_t)(img) * ((z).depth ? (z).depth : 1)), enc_b_copy_buffer_texture(e, s, so, row, img, z, d, sl, lv, o))
#define enc_b_copy_texture_buffer(e, s, sl, lv, o, z, d, doff, row, img) (copy_site_note(__LINE__, (uint64_t)(img) * ((z).depth ? (z).depth : 1)), enc_b_copy_texture_buffer(e, s, sl, lv, o, z, d, doff, row, img))
#define enc_b_copy_texture(e, s, sl, lv, so, z, d, dl, dv, doo) (copy_site_note(__LINE__, copy_tex_bytes(s, z)), enc_b_copy_texture(e, s, sl, lv, so, z, d, dl, dv, doo))
