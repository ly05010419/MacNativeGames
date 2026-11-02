// nvn_replay: replays a frame captured with DM_NVN_CAPTURE (dm/gfx/nvn/nvn_capture.inc) against this tree's replay layer, and compares the presented frame's digest with
// the one the live run took. Built like the gfx tests, which include nvn_mtl.m (and hle_nvn.c) in one translation unit: the replayer reaches the replay layer's own tables
// and the host's pool / program / descriptor-pool side tables without any entry point added to them for it.
//   make -C dm/gfx/tests GAME=<id> nvn_replay
//   ./nvn_replay <capture> [--repeat n] [--max-pixels n] [--warm n] [--png path] [--texdump dir] [--writes] [--made]
// --repeat n: n replays, each in a fresh process; --warm n: n replays in one process, each after the one before (the replay layer keeps its state; see docs/development.md)
// --max-pixels n: a replay whose frame differs from the live one in at most n pixels still counts as a match (the replay noise of the game's frame; default 0, bit for bit)
// Exit status: 0 the replayed frame equals the live one (within --max-pixels), 1 it differs, 2 the capture cannot be replayed by this build (another game, record layout or shader
// interface, a corrupt file): a [RED] line says which.
#import <Foundation/Foundation.h>
#include <spawn.h>
#include <sys/wait.h>
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
void crash_install(void);   // host crash.c: the fault handler that also takes the dirty tracker's write faults on pool pages (nvn_dirty.c)

#define RFAIL(...) do { fprintf(stderr, "[RED] nvn_replay: " __VA_ARGS__); fputc('\n', stderr); exit(2); } while (0)
typedef struct { cap_chunk c; uint8_t *d; } chunk;
static chunk *rp_ch; static size_t rp_nch;
typedef struct { uint64_t old_lo, size; uint32_t kind; uint8_t *mem, *orig; } region;   // mem: the fresh mapping; orig: its bytes at capture (for --repeat)
static region *rp_rg; static uint32_t rp_nrg;
static pool_t *rp_po; static uint32_t rp_npo;
static tex_t *rp_to; static uint32_t rp_nto;
static prog_t *rp_pr; static cap_prog *rp_prc; static uint32_t rp_npr;
static dpool_t *rp_dp; static uint32_t rp_ndp;
static sync_t *rp_sy; static uint32_t rp_nsy;
static uint8_t **rp_seg; static uint32_t rp_nseg;   // the segments of the submit being decoded
static uint64_t rp_bad;   // fields that did not decode

// Pool blocks whose bytes after the frame differ from the live run's, and the record fields that read or write them (the pool comparison after the first replay)
static nvn_hmap g_diffblk = {.klen = 8}; static uint32_t g_scan_op; static uint64_t g_scan_hits[OP_COUNT_];
static int scan_addr(void *ctx, uint64_t *field, uint64_t len, int writable) {
    (void)ctx; (void)writable; uint64_t v = *field; if (!cap_is_addr(v)) return 1;
    uint64_t r = cap_addr_region(v), o = cap_addr_off(v), n = len ? len : 4096;
    for (uint64_t b = o / 4096; b <= (o + n - 1) / 4096 && b < o / 4096 + 4096; b++) { uint64_t key = r << 32 | b; if (nvn_hm_get(&g_diffblk, &key)) { g_scan_hits[g_scan_op]++; break; } }
    return 1;
}
static int scan_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size) { (void)ctx; (void)kind; (void)field; (void)payload; (void)size; return 1; }

// --writes: pool ranges the GPU writes in a submission (copies, fills, storage buffers, transform feedback) and the later records of the same submission that read
// them, by op: what a replay layer reading pool memory on the CPU while it encodes would see differently depending on how far the GPU has got
typedef struct { uint64_t r, lo, hi; uint32_t op; } raw_w; static raw_w *g_rw; static size_t g_nrw, g_rwcap; static int g_raw_write; static uint32_t g_raw_op;
static uint64_t g_raw_hits[OP_COUNT_][OP_COUNT_];
static int raw_addr(void *ctx, uint64_t *field, uint64_t len, int writable) {
    (void)ctx; uint64_t v = *field; if (!cap_is_addr(v)) return 1;
    uint64_t r = cap_addr_region(v), lo = cap_addr_off(v), hi = lo + (len ? len : 1);
    if (writable && g_raw_write) { if (g_nrw == g_rwcap) { g_rwcap = g_rwcap ? 2 * g_rwcap : 256; g_rw = realloc(g_rw, g_rwcap * sizeof *g_rw); } g_rw[g_nrw++] = (raw_w){r, lo, hi, g_raw_op}; return 1; }
    for (size_t i = 0; i < g_nrw; i++) if (g_rw[i].r == r && lo < g_rw[i].hi && g_rw[i].lo < hi) { g_raw_hits[g_rw[i].op][g_raw_op]++; break; }
    return 1;
}
static int raw_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size);
static const cap_ops g_raw = {raw_addr, raw_obj};
static int raw_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size) {
    (void)ctx; (void)size; uint64_t v = *field;
    if (kind == CAP_K_TEX && cap_obj_kind(v) == CAP_K_TEXIN && payload) { tex_t *t = (tex_t *)(payload + cap_obj_index(v)); uint64_t m = 0; memcpy(&m, (uint8_t *)t + offsetof(tex_t, mem), 8); raw_addr(NULL, &m, t->L.size, g_raw_op == OP_COPY_T2T || g_raw_op == OP_CLEAR_TEX); }
    return 1;
}
static void raw_walk(const uint8_t *seg, uint32_t len, const uint8_t **segs, const uint32_t *lens, uint32_t nseg, int depth) {
    uint8_t *b = malloc(len + 8); memcpy(b, seg, len);
    for (uint32_t o = 0; o + 8 <= len;) { op_hdr h; memcpy(&h, b + o, 8); if (h.size < 8 || o + h.size > len) break; g_raw_op = h.op < OP_COUNT_ ? h.op : 0;
        if ((h.op == OP_CALL || h.op == OP_JUMP) && depth < 8) { uint64_t v; memcpy(&v, b + o + 8, 8); uint64_t i = cap_obj_index(v); if (i < nseg) raw_walk(segs[i], lens[i], segs, lens, nseg, depth + 1); if (h.op == OP_JUMP) break; o += h.size; continue; }
        g_raw_write = 0; cap_rec_fields(h.op, b + o + 8, h.size, &g_raw, NULL);   // reads first (a copy reads its source), then the writes
        g_raw_write = 1; cap_rec_fields(h.op, b + o + 8, h.size, &g_raw, NULL); g_raw_write = 0;
        o += h.size; }
    free(b);
}
static int dec_addr(void *ctx, uint64_t *field, uint64_t len, int writable) {
    (void)ctx; (void)writable;
    uint64_t v = *field; if (!cap_is_addr(v)) return 1;
    uint32_t r = cap_addr_region(v); uint64_t off = cap_addr_off(v);
    if (r >= rp_nrg || !rp_rg[r].mem || off > rp_rg[r].size || (len && off + len > rp_rg[r].size && rp_rg[r].kind)) { rp_bad++; *field = 0; return 0; }
    *field = (uint64_t)(uintptr_t)rp_rg[r].mem + off; return 1;
}
static int dec_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size);
static const cap_ops rp_dec = {dec_addr, dec_obj};
static int dec_obj(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size) {
    (void)ctx; uint64_t v = *field; if (!v) return 1;
    int k = cap_obj_kind(v); uint64_t i = cap_obj_index(v);
    if (k != kind && !(kind == CAP_K_TEX && k == CAP_K_TEXIN)) { rp_bad++; *field = 0; return 0; }
    void *p = NULL;
    switch (k) {
        case CAP_K_TEX: p = i < rp_nto ? &rp_to[i] : NULL; break;
        case CAP_K_TEXIN: if (payload && i + sizeof(tex_t) <= size) { p = payload + i; cap_tex_fields(p, &rp_dec, NULL); } break;
        case CAP_K_PROG: p = i < rp_npr ? &rp_pr[i] : NULL; break;
        case CAP_K_DPOOL: p = i < rp_ndp ? &rp_dp[i] : NULL; break;
        case CAP_K_SYNC: p = i < rp_nsy ? &rp_sy[i] : NULL; break;
        case CAP_K_POOL: p = i < rp_npo ? &rp_po[i] : NULL; break;
        case CAP_K_SEG: p = i < rp_nseg ? rp_seg[i] : NULL; break;
    }
    if (!p) { rp_bad++; *field = 0; return 0; }
    *field = (uint64_t)(uintptr_t)p; return 1;
}

static const char *head_get(const char *head, size_t n, const char *key, char *out, size_t on) {   // value of "key\tvalue\n" in the header text
    size_t kl = strlen(key);
    for (size_t i = 0; i < n;) {
        const char *e = memchr(head + i, '\n', n - i); size_t ln = e ? (size_t)(e - head - i) : n - i;
        if (ln > kl && !memcmp(head + i, key, kl) && head[i + kl] == '\t') { size_t vl = MIN(ln - kl - 1, on - 1); memcpy(out, head + i + kl + 1, vl); out[vl] = 0; return out; }
        i += ln + 1;
    }
    return NULL;
}
static void apply_env(const char *head, size_t n) {   // the live run's DM_* settings, so the replay takes the same paths (a setting given to the replayer wins)
    static const char *const skip[] = {"DM_NVN_SHOT", "DM_NVN_TSV", "DM_KEYS", "DM_CTL", "DM_WINDOW", "DM_METAL_HUD", "DM_PSO_PREBUILD"};
    for (size_t i = 0; i < n;) {
        const char *e = memchr(head + i, '\n', n - i); size_t ln = e ? (size_t)(e - head - i) : n - i;
        if (ln > 4 && !memcmp(head + i, "env\t", 4)) {
            char kv[4096]; size_t l = MIN(ln - 4, sizeof kv - 1); memcpy(kv, head + i + 4, l); kv[l] = 0;
            char *eq = strchr(kv, '='); if (eq) { *eq = 0; int sk = 0; for (unsigned k = 0; k < sizeof skip / sizeof *skip; k++) sk |= !strncmp(kv, skip[k], strlen(skip[k]));
                if (!sk) setenv(kv, eq + 1, 0); }
        }
        i += ln + 1;
    }
}

static void map_region(uint32_t idx, const cap_regn *h, const uint8_t *bytes) {   // a fresh mapping at the same offset within 64 KiB as the live one (alignment the replay layer may depend on)
    if (idx >= rp_nrg) { rp_rg = realloc(rp_rg, (idx + 1) * sizeof *rp_rg); memset(rp_rg + rp_nrg, 0, (idx + 1 - rp_nrg) * sizeof *rp_rg); rp_nrg = idx + 1; }
    size_t lead = (size_t)(h->old_lo & 0xffff), len = (size_t)h->size + lead + 0x30000;   // room for the alignment and the pool buffers' page rounding
    uint8_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) RFAIL("cannot map region %u (%llu bytes)", idx, (unsigned long long)h->size);
    uint8_t *base = (uint8_t *)(((uintptr_t)m + 0xffff) & ~(uintptr_t)0xffff) + lead;
    memcpy(base, bytes, (size_t)h->size);
    rp_rg[idx] = (region){h->old_lo, h->size, h->kind, base, (uint8_t *)bytes};
}

static int rp_pools_added;
static void add_pools(const uint8_t *d, size_t n) {   // the replay layer's pool list, as hle_nvn.c registers a pool (MemoryPoolInitialize)
    for (size_t o = 0; o + sizeof(cap_padd) <= n; o += sizeof(cap_padd)) {
        cap_padd a; memcpy(&a, d + o, sizeof a);
        if (!dec_addr(NULL, &a.mem, 0, 0) || !a.mem) RFAIL("a pool lies in no captured region");
        static nvn_hmap seen = {.klen = 8}; if (nvn_hm_get(&seen, &a.mem)) continue; nvn_hm_put(&seen, &a.mem, (void *)1);
        mtl_pool_add((void *)(uintptr_t)a.mem, a.size); nvn_dirty_add_range((void *)(uintptr_t)a.mem, a.size); rp_pools_added++;
    }
}

// Fill a texture from a TPIX chunk: made as the replay layer makes it (tex_mtl), then each level and slice copied in from the captured bytes
static int opt_writes, opt_made;   // --writes: GPU writes and the reads after them in each submission; --made: list the textures the frame made itself
static nvn_hmap g_filled = {.klen = 8};   // host textures the capture's contents filled
static void fill_tex(const uint8_t *d, size_t n) {
    cap_tpix h; if (n < sizeof h) RFAIL("short texture chunk");
    memcpy(&h, d, sizeof h); cap_tex_fields(&h.t, &rp_dec, NULL);
    const cap_tsub *subs = (const cap_tsub *)(d + sizeof h); const uint8_t *px = d + sizeof h + h.nsub * sizeof *subs;
    id<MTLTexture> t = tex_mtl(&h.t); if (t.parentTexture) t = t.parentTexture;
    if (t) { uint64_t k = (uint64_t)(uintptr_t)(__bridge void *)t; nvn_hm_put(&g_filled, &k, (void *)1); }
    if (!t) RFAIL("texture ser=%llu %ux%u: the replay layer made no texture for it", (unsigned long long)h.t.serial, h.width, h.height);
    uint32_t slices = (uint32_t)(t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 * t.arrayLength : t.arrayLength);
    if (t.pixelFormat != h.pixfmt || t.width != h.width || t.height != h.height || t.mipmapLevelCount != h.levels || slices != h.slices)
        RFAIL("texture ser=%llu: captured as %ux%u format %u, %u levels, %u slices; this build makes it %lux%lu format %lu, %lu levels, %u slices (the replay layer's texture creation changed)",
              (unsigned long long)h.t.serial, h.width, h.height, h.pixfmt, h.levels, h.slices, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, (unsigned long)t.mipmapLevelCount, slices);
    uint64_t total = 0; for (uint32_t i = 0; i < h.nsub; i++) total += subs[i].bytes;
    id<MTLBuffer> b = [g_dev newBufferWithBytes:px length:MAX(total, 16u) options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder]; uint64_t off = 0;
    for (uint32_t i = 0; i < h.nsub; i++) { const cap_tsub *s = &subs[i];
        [e copyFromBuffer:b sourceOffset:off sourceBytesPerRow:s->row sourceBytesPerImage:s->img sourceSize:MTLSizeMake(s->w, s->h, s->d) toTexture:t destinationSlice:s->slice
           destinationLevel:s->level destinationOrigin:MTLOriginMake(0, 0, 0) options:cap_plane_opt(t.pixelFormat, (int)s->plane)];
        off += s->bytes; }
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
}

static void check_programs(const char *cap_chain) {   // every program resolves to shaders with the interface the records were made for
    int missing = 0;
    for (uint32_t i = 0; i < rp_npr; i++) {
        NvProg *o = prog_get(&rp_pr[i]); uint64_t now[6]; cap_iface(o, now);
        for (int st = 0; st < 6; st++) {
            if (now[st] == rp_prc[i].iface[st]) continue;
            char key[17] = "?"; for (unsigned s = 0; s < rp_prc[i].nstage && s < 6; s++) if (rp_prc[i].ok >> s & 1) { snprintf(key, sizeof key, "%02x%02x%02x%02x%02x%02x%02x%02x", rp_prc[i].key[s][0], rp_prc[i].key[s][1],
                rp_prc[i].key[s][2], rp_prc[i].key[s][3], rp_prc[i].key[s][4], rp_prc[i].key[s][5], rp_prc[i].key[s][6], rp_prc[i].key[s][7]); }
            if (!now[st] || !rp_prc[i].iface[st]) { missing++; fprintf(stderr, "[RED] nvn_replay: program #%u (keys from %s) stage %d: %s in this build's libraries than at capture\n", i, key, st,
                now[st] ? "a shader now, none" : "no shader now, one"); continue; }
            RFAIL("program #%u (a key %s) stage %d: the translated shader's bindings or inputs differ from the ones the records were captured against (capture chain %s, this build %s): "
                  "the records may not fit it, not replaying", i, key, st, cap_chain, DMSH_CHAIN);
        }
    }
    if (missing) RFAIL("%d program stages resolve differently than at capture: the same libraries (DM_MSL_DIR, DM_NVN_SHMAP) are needed", missing);
}

int main(int argc, char **argv) { @autoreleasepool {
    const char *path = NULL, *png = NULL, *tdir = NULL, *maxpx_arg = "0"; int repeat = 1, warm = 1, index = 1;
    setvbuf(stdout, NULL, _IOLBF, 0); crash_install();   // pool pages the replay writes may be write-protected by the dirty tracker, as in the host
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = MAX(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--max-pixels") && i + 1 < argc) maxpx_arg = argv[++i];
        else if (!strcmp(argv[i], "--texdump") && i + 1 < argc) tdir = argv[++i];   // every colour target of each replay as <dir>/r<n>_<serial*100+level>.png (DM_NVN_TEXDUMP), to compare replays target by target
        else if (!strcmp(argv[i], "--warm") && i + 1 < argc) warm = MAX(1, atoi(argv[++i]));
        else if (!strcmp(argv[i], "--index") && i + 1 < argc) index = MAX(1, atoi(argv[++i]));   // (the n-th fresh replay of --repeat)
        else if (!strcmp(argv[i], "--writes")) opt_writes = 1;
        else if (!strcmp(argv[i], "--made")) opt_made = 1;
        else if (argv[i][0] != '-' && !path) path = argv[i];
        else { fprintf(stderr, "usage: %s <capture> [--repeat n] [--max-pixels n] [--warm n] [--png path] [--texdump dir] [--writes] [--made]\n", argv[0]); return 2; }
    }
    if (!path) { fprintf(stderr, "usage: %s <capture> [--repeat n] [--max-pixels n] [--warm n] [--png path] [--texdump dir] [--writes] [--made]\n", argv[0]); return 2; }
    uint64_t maxpx = strtoull(maxpx_arg, NULL, 10);
    if (repeat > 1) {   // each replay in a fresh process: a replay in the same process starts from the replay layer's state the one before left (--warm), which changes the frame
        int ok = 0, worst = 0;
        for (int k = 1; k <= repeat; k++) {
            char idx[16]; snprintf(idx, sizeof idx, "%d", k); const char *args[16]; int na = 0;
            args[na++] = argv[0]; args[na++] = path; args[na++] = "--index"; args[na++] = idx; args[na++] = "--max-pixels"; args[na++] = maxpx_arg;
            if (png) { args[na++] = "--png"; args[na++] = png; }
            if (tdir) { args[na++] = "--texdump"; args[na++] = tdir; }
            args[na] = NULL;
            pid_t pid; extern char **environ; int st = 0; char self[1024]; uint32_t sn = sizeof self;   // this executable by its path (argv[0] may have come from PATH)
            if (_NSGetExecutablePath(self, &sn)) strlcpy(self, argv[0], sizeof self);
            if (posix_spawn(&pid, self, NULL, NULL, (char *const *)args, environ) || waitpid(pid, &st, 0) < 0) RFAIL("cannot run %s", self);
            int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 2; ok += rc == 0; worst = MAX(worst, rc);
        }
        printf("%d of %d fresh replays match the live frame%s\n", ok, repeat, maxpx ? [NSString stringWithFormat:@" (within %llu pixels)", (unsigned long long)maxpx].UTF8String : "");
        return worst;
    }
    uint64_t tl0 = now_ns();
    cap_reader rd; int lr = cap_load(&rd, path);
    if (lr <= 0) RFAIL("%s: %s", path, lr < 0 ? "not a capture of format 1" : "cannot read");
    for (;;) { cap_chunk c; uint8_t *d; int r = cap_next(&rd, &c, &d); if (r < 0) RFAIL("%s: corrupt chunk at byte %zu", path, rd.pos); if (!r) break;
        rp_ch = realloc(rp_ch, (rp_nch + 1) * sizeof *rp_ch); rp_ch[rp_nch++] = (chunk){c, d}; }
    cap_unload(&rd);
    // header: game, layout, chain; then the live run's settings
    char *head = NULL; size_t hn = 0; const char *lay = NULL; size_t ln = 0; const char *stat = ""; size_t sn = 0;
    for (size_t i = 0; i < rp_nch; i++) { if (rp_ch[i].c.tag == CAP_HEAD) { head = (char *)rp_ch[i].d; hn = rp_ch[i].c.raw; } else if (rp_ch[i].c.tag == CAP_LAYT) { lay = (const char *)rp_ch[i].d; ln = rp_ch[i].c.raw; }
        else if (rp_ch[i].c.tag == CAP_STAT) { stat = (const char *)rp_ch[i].d; sn = rp_ch[i].c.raw; } }
    if (!head || !lay) RFAIL("%s: no header", path);
    char title[64] = "", chain[64] = "?", commit[128] = "?", frame[32] = "?", build[64] = "?", v[64];
    head_get(head, hn, "title", title, sizeof title); head_get(head, hn, "chain", chain, sizeof chain); head_get(head, hn, "commit", commit, sizeof commit);
    head_get(head, hn, "frame", frame, sizeof frame); head_get(head, hn, "build", build, sizeof build);
    uint64_t mtl_frame = head_get(head, hn, "mtl_frame", v, sizeof v) ? strtoull(v, NULL, 10) : 0;
    if (strcmp(title, DM_GAME_ID)) RFAIL("the capture is of %s, this replayer is built for %s (make GAME=%s nvn_replay)", title, DM_GAME_ID, title);
    if (ln != strlen(cap_layout()) || memcmp(lay, cap_layout(), ln)) RFAIL("the capture's record layout differs from this build's (nvn_obj.h changed since the capture): capture again");
    printf("capture %s: %s frame %s, build %s, host %s, chain %s (this build: chain %s)\n", path, title, frame, build, commit, chain, DMSH_CHAIN);
    apply_env(head, hn);
    mtl_init();
    if (head_get(stat, sn, "tex_pool_n", v, sizeof v)) mtl_set_tex_pool_n(atoi(v));
    if (head_get(stat, sn, "origin_ll", v, sizeof v)) mtl_set_origin_lower_left(atoi(v));
    if (head_get(stat, sn, "depth_zero", v, sizeof v)) mtl_set_depth_zero(atoi(v));
    int qll = head_get(stat, sn, "queue_origin_ll", v, sizeof v) ? atoi(v) : 1;
    // regions first (every later decode needs them), then the object tables, sized before any is filled (objects point at each other)
    uint32_t nsub = 0; const cap_pres *pres = NULL;
#define GROW(tab, n, idx) do { if ((idx) >= (n)) n = (idx) + 1; } while (0)
    for (size_t i = 0; i < rp_nch; i++) {
        cap_chunk *c = &rp_ch[i].c; const uint8_t *d = rp_ch[i].d; uint32_t idx;
        if (c->tag == CAP_REGN) { cap_regn h; if (c->raw < sizeof h) RFAIL("short region chunk"); memcpy(&h, d, sizeof h); if (h.size != c->raw - sizeof h) RFAIL("region %u: size mismatch", h.index); map_region(h.index, &h, d + sizeof h); }
        if (c->tag == CAP_SUBM) nsub = MAX(nsub, c->submit);
        if (c->tag == CAP_PRES && c->raw == sizeof *pres) pres = (const cap_pres *)d;
        if (c->tag == CAP_POBJ) for (size_t o = 0; o + sizeof(cap_pobj) <= c->raw; o += sizeof(cap_pobj)) { memcpy(&idx, d + o, 4); GROW(rp_po, rp_npo, idx); }
        if (c->tag == CAP_TOBJ) for (size_t o = 0; o + 8 + sizeof(tex_t) <= c->raw; o += 8 + sizeof(tex_t)) { memcpy(&idx, d + o, 4); GROW(rp_to, rp_nto, idx); }
        if (c->tag == CAP_PROG) for (size_t o = 0; o + sizeof(cap_prog) <= c->raw; o += sizeof(cap_prog)) { memcpy(&idx, d + o, 4); GROW(rp_pr, rp_npr, idx); }
        if (c->tag == CAP_DPOL) for (size_t o = 0; o + sizeof(cap_dpol) <= c->raw; o += sizeof(cap_dpol)) { memcpy(&idx, d + o, 4); GROW(rp_dp, rp_ndp, idx); }
        if (c->tag == CAP_SYNC && c->raw >= 4) { memcpy(&idx, d, 4); rp_nsy = MAX(rp_nsy, idx); }
    }
#undef GROW
    if (!pres) RFAIL("%s: no present (the capture did not finish)", path);
    rp_po = calloc(rp_npo + 1, sizeof *rp_po); rp_to = calloc(rp_nto + 1, sizeof *rp_to); rp_pr = calloc(rp_npr + 1, sizeof *rp_pr); rp_prc = calloc(rp_npr + 1, sizeof *rp_prc);
    rp_dp = calloc(rp_ndp + 1, sizeof *rp_dp); rp_sy = calloc(rp_nsy + 1, sizeof *rp_sy);
    for (size_t i = 0; i < rp_nch; i++) {   // pools first: descriptor pools and textures point at them
        cap_chunk *c = &rp_ch[i].c; const uint8_t *d = rp_ch[i].d; if (c->tag != CAP_POBJ) continue;
        for (size_t o = 0; o + sizeof(cap_pobj) <= c->raw; o += sizeof(cap_pobj)) { cap_pobj p; memcpy(&p, d + o, sizeof p);
            uint64_t m = p.mem; dec_addr(NULL, &m, 0, 0); rp_po[p.index] = (pool_t){MAGIC('P', 'O', 'O', 'L'), p.flags, (uint8_t *)(uintptr_t)m, p.size, NULL, p.vmode}; }
    }
    for (size_t i = 0; i < rp_nch; i++) {
        cap_chunk *c = &rp_ch[i].c; const uint8_t *d = rp_ch[i].d;
        if (c->tag == CAP_TOBJ) for (size_t o = 0; o + 8 + sizeof(tex_t) <= c->raw; o += 8 + sizeof(tex_t)) { uint32_t idx; memcpy(&idx, d + o, 4); memcpy(&rp_to[idx], d + o + 8, sizeof(tex_t)); }
        if (c->tag == CAP_PROG) for (size_t o = 0; o + sizeof(cap_prog) <= c->raw; o += sizeof(cap_prog)) { cap_prog p; memcpy(&p, d + o, sizeof p);
            rp_prc[p.index] = p; prog_t *pr = &rp_pr[p.index]; pr->magic = MAGIC('P', 'R', 'O', 'G'); pr->nstage = p.nstage; pr->serial = p.serial; }
        if (c->tag == CAP_DPOL) for (size_t o = 0; o + sizeof(cap_dpol) <= c->raw; o += sizeof(cap_dpol)) { cap_dpol p; memcpy(&p, d + o, sizeof p);
            uint64_t po = p.pool; dec_obj(NULL, CAP_K_POOL, &po, NULL, 0); rp_dp[p.index] = (dpool_t){(const pool_t *)(uintptr_t)po, p.off, p.n}; }
    }
    // pointers into the tables are fixed now (no more realloc): decode the texture objects, install the program keys and descriptor-pool bases in hle_nvn.c's side tables
    for (uint32_t i = 0; i < rp_nto; i++) cap_tex_fields(&rp_to[i], &rp_dec, NULL);
    for (uint32_t i = 0; i < rp_npr; i++) { prog_keys_t *k = pk_get(&rp_pr[i], 1); memcpy(k->key, rp_prc[i].key, sizeof k->key); k->ok = rp_prc[i].ok; }
    for (size_t i = 0; i < rp_nch; i++) if (rp_ch[i].c.tag == CAP_DPOL) for (size_t o = 0; o + sizeof(cap_dpol) <= rp_ch[i].c.raw; o += sizeof(cap_dpol)) { cap_dpol p; memcpy(&p, rp_ch[i].d + o, sizeof p);
        if (g_ntpb < NDPOOL) g_tpb[g_ntpb++] = (struct dpool_base){&rp_dp[p.index], p.tex_base}; if (g_nspb < NDPOOL) g_spb[g_nspb++] = (struct dpool_base){&rp_dp[p.index], p.samp_base}; }
    // how the replay layer held each texture: live ones are initialized (kept in the serial table, not the small ghost ring), CPU-written ones uploaded from their pools
    uint64_t nlive = 0, ncpu = 0;
    for (size_t i = 0; i < rp_nch; i++) if (rp_ch[i].c.tag == CAP_TKND) for (size_t o = 0; o + sizeof(cap_tknd) <= rp_ch[i].c.raw; o += sizeof(cap_tknd)) {
        cap_tknd *k = (cap_tknd *)(rp_ch[i].d + o); cap_tex_fields(&k->t, &rp_dec, NULL);
        if (k->kind & CAP_TK_LIVE) { mtl_tex_init(&k->t); nlive++; } }
    for (size_t i = 0; i < rp_nch; i++) if (rp_ch[i].c.tag == CAP_TKND) for (size_t o = 0; o + sizeof(cap_tknd) <= rp_ch[i].c.raw; o += sizeof(cap_tknd)) {
        cap_tknd *k = (cap_tknd *)(rp_ch[i].d + o); if (k->kind & CAP_TK_CPU) { mtl_tex_cpu_written(&k->t); ncpu++; } }
    for (size_t i = 0; i < rp_nch; i++) if (rp_ch[i].c.tag == CAP_TREG) for (size_t o = 0; o + sizeof(cap_treg) <= rp_ch[i].c.raw; o += sizeof(cap_treg)) {
        cap_treg *e = (cap_treg *)(rp_ch[i].d + o); if (e->ok) cap_tex_fields(&e->t, &rp_dec, NULL); }
    for (size_t i = 0; i < rp_nch; i++) if (rp_ch[i].c.tag == CAP_PADD && rp_ch[i].c.submit <= 1) add_pools(rp_ch[i].d, rp_ch[i].c.raw);
    if (rp_bad) RFAIL("%llu captured pointers name no captured object or region", (unsigned long long)rp_bad);
    check_programs(chain);
    double load_ms = (now_ns() - tl0) / 1e6;
    printf("loaded in %.0f ms: %u regions, %d pools, %u textures, %u programs, %u descriptor pools, %u submits; %llu live and %llu CPU-written textures marked\n",
           load_ms, rp_nrg, rp_pools_added, rp_nto, rp_npr, rp_ndp, nsub, (unsigned long long)nlive, (unsigned long long)ncpu);

    if (opt_writes) for (uint32_t sub = 1; sub <= nsub; sub++) {
        const uint8_t *segs[65536]; uint32_t lens[65536], nseg = 0, nh = 0; const uint32_t *top = NULL;
        for (size_t i = 0; i < rp_nch; i++) { if (rp_ch[i].c.submit != sub) continue;
            if (rp_ch[i].c.tag == CAP_SEGM) { uint32_t idx, len; memcpy(&idx, rp_ch[i].d, 4); memcpy(&len, rp_ch[i].d + 4, 4); if (idx < 65536) { segs[idx] = rp_ch[i].d + 8; lens[idx] = len; nseg = MAX(nseg, idx + 1); } }
            if (rp_ch[i].c.tag == CAP_SUBM) { memcpy(&nh, rp_ch[i].d, 4); top = (const uint32_t *)(rp_ch[i].d + 4); } }
        g_nrw = 0; for (uint32_t i = 0; i < nh; i++) if (top[i] < nseg) raw_walk(segs[top[i]], lens[top[i]], segs, lens, nseg, 0);
        printf("submit %u: %zu GPU writes to pools; later reads of them (writer op → reader op × count):", sub, g_nrw);
        for (int a = 0; a < OP_COUNT_; a++) for (int b = 0; b < OP_COUNT_; b++) if (g_raw_hits[a][b]) printf(" %d→%d×%llu", a, b, (unsigned long long)g_raw_hits[a][b]);
        printf("\n"); memset(g_raw_hits, 0, sizeof g_raw_hits);
    }
    const uint8_t *lpix = NULL; uint64_t lpixn = 0;   // the live frame's pixels (captures made before LPIX have only its digest)
    for (size_t i = 0; i < rp_nch; i++) if (rp_ch[i].c.tag == CAP_LPIX) { lpix = rp_ch[i].d; lpixn = rp_ch[i].c.raw; }
    char live[65]; cap_hex(pres->sha256, live); int same_all = 1, ok_all = 1; uint64_t worst_px = 0;
    for (int rep = 0; rep < warm; rep++) {
        uint64_t exec_ns = 0, setup_ns = 0, pass0 = 0, draw0 = 0, frame_no = rep == 0 && mtl_frame ? mtl_frame : 1000000 + (uint64_t)rep;   // the first replay takes the live frame number (frame-keyed diagnostics)
        if (tdir) { char tp[1200]; snprintf(tp, sizeof tp, "%s/r%d_%%d.png", tdir, index + rep); dm_gfx_texdump_next(tp); g_td_frame = (long)frame_no; }
        if (rep) for (uint32_t r = 0; r < rp_nrg; r++) if (rp_rg[r].mem) memcpy(rp_rg[r].mem, rp_rg[r].orig, (size_t)rp_rg[r].size);   // back to the state at the first submit
        for (uint32_t s = 1; s <= nsub; s++) {
            uint64_t t0 = now_ns();
            for (size_t i = 0; i < rp_nch; i++) {   // what changed before this submit, in the order it was captured
                cap_chunk *c = &rp_ch[i].c; uint8_t *d = rp_ch[i].d; if (c->submit != s) continue;
                if (c->tag == CAP_PADD && s > 1) add_pools(d, c->raw);
                else if (c->tag == CAP_PDIF) for (size_t o = 0; o + 24 <= c->raw;) { uint32_t r; uint64_t off, len; memcpy(&r, d + o, 4); memcpy(&off, d + o + 8, 8); memcpy(&len, d + o + 16, 8);
                    if (r >= rp_nrg || off + len > rp_rg[r].size || o + 24 + len > c->raw) RFAIL("a pool change outside its region"); memcpy(rp_rg[r].mem + off, d + o + 24, (size_t)len); o += 24 + (size_t)len; }
                else if (c->tag == CAP_TREG) for (size_t o = 0; o + sizeof(cap_treg) <= c->raw; o += sizeof(cap_treg)) { cap_treg *e = (cap_treg *)(d + o); mtl_reg_tex(e->tid, e->ok ? &e->t : NULL, e->has_v ? &e->v : NULL); }
                else if (c->tag == CAP_SREG) for (size_t o = 0; o + sizeof(cap_sreg) <= c->raw; o += sizeof(cap_sreg)) { cap_sreg e; memcpy(&e, d + o, sizeof e); mtl_reg_samp(e.sid, &e.p); }
                else if (c->tag == CAP_TPIX) fill_tex(d, c->raw);
            }
            // this submit's segments: copied, decoded (calls and jumps point at the copies), handed to the replay as the host hands the game's handles
            uint32_t nh = 0; const uint32_t *top = NULL; rp_nseg = 0;
            for (size_t i = 0; i < rp_nch; i++) { cap_chunk *c = &rp_ch[i].c; if (c->submit != s) continue;
                if (c->tag == CAP_SEGM) { uint32_t idx, len; memcpy(&idx, rp_ch[i].d, 4); memcpy(&len, rp_ch[i].d + 4, 4); if (idx >= rp_nseg) { rp_seg = realloc(rp_seg, (idx + 1) * sizeof *rp_seg); rp_nseg = idx + 1; }
                    rp_seg[idx] = malloc(len + 8); memcpy(rp_seg[idx], rp_ch[i].d + 8, len); memset(rp_seg[idx] + len, 0, 8); }
                if (c->tag == CAP_SUBM) { memcpy(&nh, rp_ch[i].d, 4); top = (const uint32_t *)(rp_ch[i].d + 4); } }
            for (size_t i = 0; i < rp_nch; i++) { cap_chunk *c = &rp_ch[i].c; if (c->submit != s || c->tag != CAP_SEGM) continue;
                uint32_t idx, len; memcpy(&idx, rp_ch[i].d, 4); memcpy(&len, rp_ch[i].d + 4, 4);
                for (uint32_t o = 0; o < len;) { op_hdr h; memcpy(&h, rp_seg[idx] + o, 8); if (h.size < 8 || o + h.size > len) RFAIL("submit %u segment %u: corrupt record at %u", s, idx, o);
                    if (!cap_rec_fields(h.op, rp_seg[idx] + o + 8, h.size, &rp_dec, NULL) || rp_bad) RFAIL("submit %u segment %u: op %u at %u does not decode", s, idx, h.op, o); o += h.size; } }
            uint64_t *hs = malloc((size_t)MAX(nh, 1) * 8); for (uint32_t i = 0; i < nh; i++) { if (top[i] >= rp_nseg) RFAIL("submit %u: handle %u names no segment", s, i); hs[i] = (uint64_t)(uintptr_t)rp_seg[top[i]]; }
            uint64_t t1 = now_ns(); setup_ns += t1 - t0;
            if (s == 1) mtl_stats(&pass0, &draw0);
            mtl_set_frame(frame_no);
            mtl_set_queue_origin_lower_left(qll);
            mtl_sub_begin(); nvn_ft_poll(); mtl_sub_index(hs, (int)nh);
            for (uint32_t i = 0; i < nh; i++) { mtl_exec_rest(hs + i + 1, (int)(nh - i - 1)); mtl_exec((const uint8_t *)(uintptr_t)hs[i]); }
            mtl_exec_rest(NULL, 0); mtl_sub_commit();
            exec_ns += now_ns() - t1;
            mtl_finish();   // the capture read the next submit's inputs after the GPU finished this one: so does the replay
            free(hs); for (uint32_t i = 0; i < rp_nseg; i++) free(rp_seg[i]);
        }
        tex_t pt = {0}; uint64_t pv = pres->tex; if (!dec_obj(NULL, CAP_K_TEX, &pv, NULL, 0)) RFAIL("the presented texture is not in the capture"); pt = *(const tex_t *)(uintptr_t)pv;
        uint8_t md[32]; uint64_t bytes; uint32_t w, h, pf; char out[65], pngp[1100]; cap_buf mine = {0};
        snprintf(pngp, sizeof pngp, "%s", png ? png : ""); if (!png) snprintf(pngp, sizeof pngp, "%s.replay%d.png", path, index + rep);   // one image per replay: replays of one build can be compared too
        if (!cap_digest(&pt, md, &bytes, &w, &h, &pf, pngp, &mine)) RFAIL("the presented texture cannot be read back");
        cap_hex(md, out); int same = !memcmp(md, pres->sha256, 32); same_all &= same;
        // which pixels differ from the live frame: their count, the largest difference of one byte, and the box around them
        uint64_t npx = 0; uint32_t big = 0, x0 = UINT32_MAX, x1 = 0, y0 = UINT32_MAX, y1 = 0; int have_px = !same && lpix && lpixn == mine.n && w == pres->width && h == pres->height && pf == pres->pixfmt && w && h;
        if (have_px) { size_t bpp = (size_t)(mine.n / ((uint64_t)w * h));
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) { const uint8_t *a = lpix + ((size_t)y * w + x) * bpp, *b = mine.p + ((size_t)y * w + x) * bpp; uint32_t d = 0;
                for (size_t k = 0; k < bpp; k++) d = MAX(d, (uint32_t)abs((int)a[k] - (int)b[k]));
                if (d) { npx++; big = MAX(big, d); x0 = MIN(x0, x); x1 = MAX(x1, x); y0 = MIN(y0, y); y1 = MAX(y1, y); } } }
        int ok = same || (have_px && npx <= maxpx); ok_all &= ok; if (have_px) worst_px = MAX(worst_px, npx);
        cap_buf_free(&mine);
        if (g_td_frame == (long)frame_no && !g_td_done) td_dump();
        if (rep == 0) {   // textures the GPU writes that the frame made itself (no captured content): a second replay starts from what this one left in them
            uint64_t n = 0, bytes = 0; nvn_hmap *maps[2] = {&g_by_serial, &g_ghost};
            for (int m = 0; m < 2; m++) for (uint32_t i = 0; i < maps[m]->cap; i++) { if (!maps[m]->vals[i]) continue; id o = (__bridge id)maps[m]->vals[i];
                if (![o conformsToProtocol:@protocol(MTLTexture)]) continue; id<MTLTexture> t = o; if (t.parentTexture) t = t.parentTexture;
                uint64_t k = (uint64_t)(uintptr_t)(__bridge void *)t; if (t.buffer || nvn_hm_get(&g_filled, &k)) continue;
                uint64_t ser; memcpy(&ser, maps[m]->keys + (size_t)i * 8, 8); n++; bytes += t.allocatedSize;
                if (opt_made) printf("  made by the frame: ser=%llu %lux%lux%lu format %lu levels %lu%s\n", (unsigned long long)ser, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.arrayLength, (unsigned long)t.pixelFormat, (unsigned long)t.mipmapLevelCount, m ? " (ghost)" : ""); }
            printf("render targets and images the frame made without captured content: %llu (%.1f MB)\n", (unsigned long long)n, bytes / 1048576.0);
        }
        if (1) {   // pool blocks after the frame against the live run's: written live but not here = the CPU wrote them while the frame ran (or a GPU write the replay missed)
            uint64_t live_only = 0, rep_only = 0, both = 0, blocks = 0; uint32_t worst = 0, worst_n = 0; nvn_hm_clear(&g_diffblk, NULL);
            for (size_t i = 0; i < rp_nch; i++) { if (rp_ch[i].c.tag != CAP_PHSH) continue; const uint8_t *d = rp_ch[i].d;
                for (size_t o = 0; o + 8 <= rp_ch[i].c.raw;) { uint32_t r, nb; memcpy(&r, d + o, 4); memcpy(&nb, d + o + 4, 4); o += 8; uint32_t rn = 0;
                    for (uint32_t k = 0; k < nb && o + 8 <= rp_ch[i].c.raw; k++, o += 8) {
                        if (r >= rp_nrg || !rp_rg[r].mem) continue; uint64_t off = (uint64_t)k * 4096; size_t len = (size_t)MIN(4096, rp_rg[r].size - off); uint64_t lh; memcpy(&lh, d + o, 8);
                        uint64_t h0 = cap_hash_block(rp_rg[r].orig + off, len), h1 = cap_hash_block(rp_rg[r].mem + off, len); blocks++;
                        if (h1 == lh) continue;
                        rn++; if (lh != h0 && h1 == h0) live_only++; else if (lh == h0) rep_only++; else both++; uint64_t key = (uint64_t)r << 32 | k; nvn_hm_put(&g_diffblk, &key, (void *)1); }
                    if (rn > worst_n) { worst_n = rn; worst = r; } } }
            if (g_diffblk.n && rep == 0) {   // which records touch them (unbounded buffer bindings are taken as their first 4 KiB)
                cap_ops sc = {scan_addr, scan_obj};
                for (size_t i = 0; i < rp_nch; i++) { if (rp_ch[i].c.tag != CAP_SEGM) continue; uint32_t len; memcpy(&len, rp_ch[i].d + 4, 4); uint8_t *b = malloc(len); memcpy(b, rp_ch[i].d + 8, len);
                    for (uint32_t o = 0; o + 8 <= len;) { op_hdr h; memcpy(&h, b + o, 8); if (h.size < 8 || o + h.size > len) break; g_scan_op = h.op < OP_COUNT_ ? h.op : 0; cap_rec_fields(h.op, b + o + 8, h.size, &sc, NULL); o += h.size; }
                    free(b); }
                printf("records touching the differing pool blocks, by op:"); for (int op = 0; op < OP_COUNT_; op++) if (g_scan_hits[op]) printf(" %d×%llu", op, (unsigned long long)g_scan_hits[op]); printf("\n");
            }
            printf("pool blocks after the frame: %llu of %llu differ from the live run (%llu written only live, %llu written only in the replay, %llu written in both with other bytes; most in region %u, %u blocks)\n",
                   (unsigned long long)(live_only + rep_only + both), (unsigned long long)blocks, (unsigned long long)live_only, (unsigned long long)rep_only, (unsigned long long)both, worst, worst_n);
        }   // DM_NVN_TEXDUMP of the live frame: the replay has no next frame to trigger it
        double gpu = mtl_gpu_ms(frame_no);
        uint64_t pass1, draw1; mtl_stats(&pass1, &draw1);
        if (opt_writes) { uint64_t a[6]; mtl_vbuf_stats(&a[0], &a[1], &a[2], &a[3], &a[4], &a[5]); printf("vbuf: resolve %llu reuse %llu set %llu skip %llu unaligned copies %llu (%llu bytes)\n", a[0], a[1], a[2], a[3], a[4], a[5]); }
        if (have_px) printf("replay %d: %llu pixels differ from the live frame (largest byte difference %u, x %u-%u y %u-%u)\n", index + rep, (unsigned long long)npx, big, x0, x1, y0, y1);
        printf("replay %d: digest %s %s live %s (%ux%u format %u); submit CPU %.2f ms (+ %.2f ms applying the captured state), GPU %.2f ms; %llu passes, %llu draws, %llu segments\n", index + rep, out, same ? "==" : "!=", live, w, h, pf, exec_ns / 1e6, setup_ns / 1e6, gpu,
               (unsigned long long)(pass1 - pass0), (unsigned long long)(draw1 - draw0), (unsigned long long)g_seg + 1);
    }
    if (same_all) printf("MATCH: the replayed frame equals the live frame\n");
    else if (ok_all) printf("MATCH within --max-pixels %llu: the replayed frame differs from the live frame in at most %llu pixels\n", (unsigned long long)maxpx, (unsigned long long)worst_px);
    else printf("[RED] MISMATCH: the replayed frame differs from the live frame%s\n", lpix ? "" : " (a capture without the live pixels: compared by digest only)");
    return ok_all ? 0 : 1;
} }
