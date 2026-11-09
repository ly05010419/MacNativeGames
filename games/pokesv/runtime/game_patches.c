#include <math.h>
#include <TargetConditionals.h>
#include "game_patches.h"
#include "game_version.h"
#include <libkern/OSCacheControl.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The player's exefs patches for this game version, applied to main after relocation and before its text is sealed. scripts/prepare_patches.py turns each .pchtxt into patches/<version>/<slug>.tsv ("offset new orig",
   hex), already checked against the original image and the rewrite; here every site is checked again against the loaded
   bytes, and a mismatch stops the start (a wrong version or a rewritten word must never be patched).
   PSV_PATCHES=<slug>,<slug>,... picks them (the launcher sets it from the settings; unset or empty: none). */
static int hexbytes(const char *s, uint8_t *out, size_t cap, size_t *n) {
    size_t k = 0;
    for (; s[0] && s[1] && s[0] != '\t' && s[0] != '\n'; s += 2) {
        unsigned v; if (k == cap || sscanf(s, "%2x", &v) != 1) return 0;
        out[k++] = (uint8_t)v;
    }
    *n = k; return k > 0;
}
static void apply_file(Module *m, const char *slug) {
    char path[512]; snprintf(path, sizeof path, "patches/" PSV_GAME_DIR "/%s.tsv", slug);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[psv] patch %s: %s missing (scripts/prepare_patches.py)\n", slug, path); runtime_fail("game patch table missing"); }
    char line[4096]; unsigned applied = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        uint64_t off = strtoull(line, NULL, 0); char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        uint8_t now[1024], was[1024]; size_t nn, nw;
        if (!a || !b || !hexbytes(a + 1, now, sizeof now, &nn) || !hexbytes(b + 1, was, sizeof was, &nw) || nn != nw)
            runtime_fail("malformed game patch table");
        if (off + nn > m->text_end && off < m->ro_off) runtime_fail("game patch in the rewrite area");
        if (off + nn > m->file_size) runtime_fail("game patch beyond main");
        if (memcmp(m->base + off, was, nw)) { fprintf(stderr, "[psv] patch %s at main+%#llx: original bytes differ\n", slug, (unsigned long long)off); runtime_fail("game patch does not match this game version"); }
        memcpy(m->base + off, now, nn); sys_icache_invalidate(m->base + off, nn); applied++;
    }
    fclose(f);
    fprintf(stderr, "[psv] game patch %s: %u sites\n", slug, applied);
}
/* Frame step (2026-10-30). The frame function (main+0x2b79f60) advances the game clocks by one fixed entry of the
   frame-duration table (16.7, 33.3, 50, 66.7, 83.3 ms), 33.3 ms in the 30 fps mode, whatever the frame took: at a 60 fps
   lock animations ran twice as fast, at 90 three times. The two clock-advance functions now read a table of ours in the
   page after main (the earlier port keeps its frame step there too), filled with the measured present interval (average of the last 10).
   The game's own table stays as it is on purpose: three other sites divide the clock by it to get a time scale (battle
   animations, catching, the Pokédex). Pointing those at the measured time too made that scale always 1, so battles ran
   fast and dropped their animations. Until the first samples the copy holds the game's values. */
static _Atomic int64_t *frame_table;
static int64_t frame_table_orig[8];
static int frame_mode60;   /* the game runs its 60 fps mode (frame_mode_keep) */
static _Atomic int64_t *scale_table;   /* the time-scale readers' table (data + 0x40) */
static _Atomic int64_t *ui_scale_table;   /* the second reader's table (data + 0xa0) */
static _Atomic uint32_t pinned_step_us;
static uint64_t last_present_ns;
static double frame_samples[10];   /* 10, not the earlier port's 5: steadier at 90-120 fps, still under 0.1 s */
static unsigned frame_sample_count, frame_sample_next;
static uint32_t adrp(unsigned reg, uint64_t pc, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("frame step table outside ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | reg;
}
static uint32_t branch(uint64_t from, uint64_t to);
/* PSV_TEST_NS=<ns>: the fixed step the frame step tests give (default 33333334, 1/30 s) */
static int64_t test_step_ns(void) { const char *e = getenv("PSV_TEST_NS"); return e && *e ? strtoll(e, NULL, 10) : 33333334; }
static void patch_frame_step(Module *m) {
    /* On by default again (2026-10-30): the GPU address faults first blamed on it also came without it, at one spot in the
       western grassland (5 of 5 runs) and once in 3 runs elsewhere; 3 runs at 60 fps with it, 2 clean. */
    const char *mode = getenv("PSV_FRAME_STEP");   /* "game": the game's fixed table (comparison runs) */
    if (mode && !strcmp(mode, "game")) { fprintf(stderr, "[psv] frame step: the game's own table (PSV_FRAME_STEP=game)\n"); return; }
    uint64_t data = (m->file_size - m->image_off + 0x3fff) & ~0x3fffULL;
    if (data + 0x4000 != m->size) runtime_fail("frame step: no data page after main");
    for (unsigned i = 0; i < 64; i++) if (m->base[data + i]) runtime_fail("frame step: data page after main in use");
    static const int64_t want[5] = {16666667, 33333334, 50000000, 66666667, 83333334};
    /* adrp x8, table GOT page; ldr x8, [x8, #0xbc0]  ->  adrp x8, our page; add x8, x8, #0 */
    static const struct { uint64_t off; uint32_t adrp, ldr; } sites[] = PSV_FRAME_ADVANCE_SITES;
    for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) {
        uint32_t w[2]; memcpy(w, m->base + sites[i].off, 8);
        if (w[0] != sites[i].adrp || w[1] != sites[i].ldr) runtime_fail("frame step: clock-advance instructions differ");
    }
    frame_table = (_Atomic int64_t *)(void *)(m->base + data);
    memcpy(frame_table_orig, want, sizeof want);
    for (int k = 0; k < 5; k++) atomic_store(&frame_table[k], want[k]);
    for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) {
        /* PSV_FRAME_SPLIT=<n> (test): clock-advance site n reads a table of exact 1/30 s at data + 0x180 */
        const char *sp = getenv("PSV_FRAME_SPLIT"); int split = sp && (unsigned)atoi(sp) == i;
        uint32_t w[2] = {adrp(8, (uintptr_t)m->base + sites[i].off, (uintptr_t)m->base + data), split ? 0x91060108 : 0x91000108};
        if (split) { _Atomic int64_t *t = (_Atomic int64_t *)(void *)(m->base + data + 0x180); for (int k = 0; k < 5; k++) atomic_store(&t[k], test_step_ns());
            fprintf(stderr, "[psv] frame step test: clock-advance site %u (main+%#llx) gets exactly 1/30 s\n", i, (unsigned long long)sites[i].off); }
        memcpy(m->base + sites[i].off, w, 8); sys_icache_invalidate(m->base + sites[i].off, 8);
        /* PSV_FRAME_SPLIT_SLOT=<n> (test): at site 0 only time slot n (x1) reads the exact 1/30 s table at data + 0x180 */
        const char *ss_ = getenv("PSV_FRAME_SPLIT_SLOT");
        if (i == 0 && ss_) { unsigned n = (unsigned)atoi(ss_); const uint64_t c = 0x343fea0, site = sites[i].off;
            for (uint64_t k = c; k < c + 24; k++) if (m->base[k]) runtime_fail("frame split slot: text padding in use");
            _Atomic int64_t *t = (_Atomic int64_t *)(void *)(m->base + data + 0x180); for (int k = 0; k < 5; k++) atomic_store(&t[k], test_step_ns());
            /* C: adrp x8, data; add x8, x8, #0; cmp x1, #n; b.ne +8; add x8, x8, #0x180; b site + 8 */
            uint32_t st[6] = {adrp(8, (uintptr_t)m->base + c, (uintptr_t)m->base + data), 0x91000108, 0xf100003f | (n << 10), 0x54000041, 0x91060108, branch(c + 20, site + 8)};
            memcpy(m->base + c, st, sizeof st); uint32_t b2[2] = {branch(site, c), 0xd503201f}; memcpy(m->base + site, b2, 8);
            sys_icache_invalidate(m->base + c, sizeof st); sys_icache_invalidate(m->base + site, 8);
            fprintf(stderr, "[psv] frame step test: clock 0 time slot %u gets exactly 1/30 s\n", n); }
    }
    /* The time-scale readers (60 fps mode, 2026-10-31): they divide the clock by table[index] with the index the object got
       when it was made. Effects made in the 30 fps mode (index 1) ran at half speed and in steps once the 60 fps mode was on
       (the Tera shine, Psystrike). Their table at data + 0x40 holds the current mode's frame in every entry (frame_mode_keep). */
    static const struct { uint64_t adrp_off; uint32_t adrp; uint64_t ldr_off; } ss[] = PSV_FRAME_SCALE_SITES;
    for (unsigned i = 0; i < sizeof ss / sizeof *ss; i++) {
        uint32_t a, l; memcpy(&a, m->base + ss[i].adrp_off, 4); memcpy(&l, m->base + ss[i].ldr_off, 4);
        if (a != ss[i].adrp || l != 0xf945e108) runtime_fail("frame step: time-scale instructions differ");
    }
    scale_table = (_Atomic int64_t *)(void *)(m->base + data + 0x40);
    for (int k = 0; k < 5; k++) atomic_store(&scale_table[k], want[k]);
    /* The second reader (main+0xd96428, global index: the Pokédex registration, menus, battle animations) keeps 1/30 s in the
       60 fps mode (2026-11-02): its scale is then 0.5 per 60 fps frame, as the community "Dynamic FPS" mod sets it (30 / fps).
       With the mode's frame it was 1, and the Pokédex book spun twice as fast. PSV_UI_SCALE=0: the mode's frame as before. */
    const char *u = getenv("PSV_UI_SCALE");
    if (!(u && !strcmp(u, "0"))) { ui_scale_table = (_Atomic int64_t *)(void *)(m->base + data + 0xa0); for (int k = 0; k < 5; k++) atomic_store(&ui_scale_table[k], want[k]); }
    for (unsigned i = 0; i < sizeof ss / sizeof *ss; i++) {
        /* add x8, x8, #0x40, or #0xa0 for the second reader (its own table, ui_scale_table) */
        uint32_t a = adrp(8, (uintptr_t)m->base + ss[i].adrp_off, (uintptr_t)m->base + data), l = i == 1 && ui_scale_table ? 0x91028108 : 0x91010108;
        memcpy(m->base + ss[i].adrp_off, &a, 4); memcpy(m->base + ss[i].ldr_off, &l, 4);
        sys_icache_invalidate(m->base + ss[i].adrp_off, 4); sys_icache_invalidate(m->base + ss[i].ldr_off, 4);
    }
    fprintf(stderr, "[psv] frame step: clocks follow the real frame time (2 guarded sites); time scales divide by the mode's frame (3 guarded sites)\n");
    /* PSV_SLOT_READERS=<hex>,<hex>,... (test): each listed reader of a time slot's delta (ldr d|x, [x0, #8] right after the slot
       call) loads exactly 1/30 s from data + 0x180 instead, through a 3-instruction stub (adrp base, data; ldr Rt, [base, #0x180];
       b back). The base is x0 for a float load (free there) and Rt itself for an integer load. */
    const char *rl = getenv("PSV_SLOT_READERS");
    if (rl && *rl) { char buf[512]; snprintf(buf, sizeof buf, "%s", rl); unsigned k = 0;
        _Atomic int64_t *t = (_Atomic int64_t *)(void *)(m->base + data + 0x180); for (int j = 0; j < 5; j++) atomic_store(&t[j], test_step_ns());
        for (char *sv = NULL, *tok = strtok_r(buf, ",", &sv); tok && k < 16; tok = strtok_r(NULL, ",", &sv), k++) {
            uint64_t site = strtoull(tok, NULL, 16), c = 0x343fec0 + 16 * k; uint32_t w; memcpy(&w, m->base + site, 4);
            int fp = (w & 0xffc00000) == 0xfd400000, gp = (w & 0xffc00000) == 0xf9400000;
            if ((!fp && !gp) || ((w >> 5) & 31) != 0 || ((w >> 10) & 0xfff) != 1) runtime_fail("slot readers: not ldr Rt, [x0, #8]");
            for (uint64_t q = c; q < c + 12; q++) if (m->base[q]) runtime_fail("slot readers: text padding in use");
            unsigned rt = w & 31, base = fp ? 0 : rt;
            uint32_t st[3] = {adrp(base, (uintptr_t)m->base + c, (uintptr_t)m->base + data), (w & 0xffc00000) | (0x30u << 10) | (base << 5) | rt, branch(c + 8, site + 4)};
            memcpy(m->base + c, st, sizeof st); uint32_t b = branch(site, c); memcpy(m->base + site, &b, 4);
            sys_icache_invalidate(m->base + c, sizeof st); sys_icache_invalidate(m->base + site, 4);
            fprintf(stderr, "[psv] slot reader test: main+%#llx reads exactly 1/30 s\n", (unsigned long long)site); } }
}
/* The fixed-step clock (2026-11-02, after the community "Dynamic FPS" 4.0.0 mod by Fl4sh9174, which fixes the indoor stutter the
   same way). At main+0xb92784 a system reads clock 2's frame delta and advances in whole 1/30 s steps: in the 60 fps mode it
   stepped every second frame, and the room camera, which follows it, moved every second frame (jerky walking indoors). In the
   60 fps mode it now gets 1/30 s every frame. As in the mod, the game's own delta stays in state 2 of the manager whose state
   setter is main+0x29364c8 (0 indoors, outdoors and in battles). Two stubs in the free end of the text page:
     main+0x2936584  csel w8, w20, wzr, lo   -> b A;  A: csel; adrp x9, data; str w8, [x9, #0x88]; b back      (state copy)
     main+0xb92784   ldr x1, [x8, #8]        -> b B;  B: adrp x1, data; ldr w1, [x1, #0x88]; cmp w1, #2; b.eq own;
                     adrp x1, data; ldr x1, [x1, #0x80]; cbnz x1, back; own: ldr x1, [x8, #8]; back: b site + 4
   data + 0x80 holds 1/30 s in the 60 fps mode and 0 (the game's delta) otherwise. PSV_FIXED_STEP=0 leaves both sites alone.
   The site is in a Lua binding (main+0xb926f0), and the scripts step in whole 1/30 s of this delta: with it they step every
   60 fps frame, so scripted fixed-step motion runs twice as fast (the ride jump, patch_jump). The exact fix is a 1/60 s script step. */
static _Atomic int64_t *fixed_step;
static uint32_t branch(uint64_t from, uint64_t to) { return 0x14000000 | (uint32_t)(((int64_t)(to - from) >> 2) & 0x3ffffff); }
static void patch_fixed_step(Module *m) {
    const char *e = getenv("PSV_FIXED_STEP"); if (e && !strcmp(e, "0")) return;
    const uint64_t use = 0xb92784, set = 0x2936584, a = 0x343ffb0, b = 0x343ffc0;
    uint64_t data = (m->file_size - m->image_off + 0x3fff) & ~0x3fffULL; uintptr_t mb = (uintptr_t)m->base;
    uint32_t w1, w2; memcpy(&w1, m->base + use, 4); memcpy(&w2, m->base + set, 4);
    if (w1 != 0xf9400501 || w2 != 0x1a9f3288 || m->text_end > a || ((m->text_end + 0x3fff) & ~0x3fffULL) < b + 32) runtime_fail("fixed step: sites or text padding differ");
    for (uint64_t k = a; k < b + 32; k++) if (m->base[k]) runtime_fail("fixed step: text padding in use");
    uint32_t sa[4] = {0x1a9f3288, adrp(9, mb + a + 4, mb + data), 0xb9008928, branch(a + 12, set + 4)};
    uint32_t sb[8] = {adrp(1, mb + b, mb + data), 0xb9408821, 0x7100083f, 0x54000080, adrp(1, mb + b + 16, mb + data), 0xf9404021, 0xb5000041, 0xf9400501};
    uint32_t back = branch(b + 32, use + 4);
    memcpy(m->base + a, sa, sizeof sa); memcpy(m->base + b, sb, sizeof sb); memcpy(m->base + b + 32, &back, 4);
    w1 = branch(use, b); w2 = branch(set, a); memcpy(m->base + use, &w1, 4); memcpy(m->base + set, &w2, 4);
    sys_icache_invalidate(m->base + a, b + 36 - a); sys_icache_invalidate(m->base + use, 4); sys_icache_invalidate(m->base + set, 4);
    fixed_step = (_Atomic int64_t *)(void *)(m->base + data + 0x80);
    fprintf(stderr, "[psv] fixed step: clock 2 steps by 1/30 s in the 60 fps mode (PSV_FIXED_STEP=0 turns it off)\n");
}
/* Jump height (2026-11-03, after the same mod). At main+0xcce37c the game stores the ride's speed, which a Lua script sets per
   script step (binding main+0x19776c0: 8.51 up at the jump, then 0.633 less per step). With the fixed step the script steps
   every 60 fps frame, and the jump was half as high and half as long. As in the mod, the stored copy's upward part (y >= 0)
   is scaled by fps / 30 * 0.978, capped at 1.95 (so 1.95 at 60 fps), and clamped to 20.5. data + 0xd0 holds the scale and data + 0xd4 the clamp: 1 and
   infinity in the 30 fps mode, the original store. A stub in the free end of the text page:
     main+0xcce37c  str q0, [x19, #0x510]  -> b C;  C: adrp x8, data; ldr s1, [x8, #0xd0]; mov v2.s[0], v0.s[1]; fmul s3, s2, s1;
                    fcmp s2, #0.0; fcsel s2, s3, s2, ge; ldr s4, [x8, #0xd4]; fmin s2, s2, s4; mov v0.s[1], v2.s[0];
                    str q0, [x19, #0x510]; b site + 4
   x8 and v1-v4 are free there: the next instructions load x8 and call through it. PSV_JUMP_SCALE=0 leaves the site alone. */
static _Atomic float *jump_scale;
static void patch_jump(Module *m) {
    const char *e = getenv("PSV_JUMP_SCALE"); if (e && !strcmp(e, "0")) return;
    const uint64_t site = 0xcce37c, c = 0x343ff80;
    uint64_t data = (m->file_size - m->image_off + 0x3fff) & ~0x3fffULL; uintptr_t mb = (uintptr_t)m->base;
    uint32_t w; memcpy(&w, m->base + site, 4);
    if (w != 0x3d814660 || m->text_end > c) runtime_fail("jump scale: site or text padding differ");
    for (uint64_t k = c; k < c + 44; k++) if (m->base[k]) runtime_fail("jump scale: text padding in use");
    uint32_t sc[11] = {adrp(8, mb + c, mb + data), 0xbd40d101, 0x6e042402, 0x1e210843, 0x1e202048, 0x1e22ac62, 0xbd40d504, 0x1e245842,
                       0x6e0c0440, 0x3d814660, branch(c + 40, site + 4)};
    jump_scale = (_Atomic float *)(void *)(m->base + data + 0xd0);
    atomic_store(&jump_scale[0], 1.0f); atomic_store(&jump_scale[1], __builtin_inff());
    memcpy(m->base + c, sc, sizeof sc); w = branch(site, c); memcpy(m->base + site, &w, 4);
    sys_icache_invalidate(m->base + c, sizeof sc); sys_icache_invalidate(m->base + site, 4);
    fprintf(stderr, "[psv] jump scale: upward speed x1.95 in the 60 fps mode (PSV_JUMP_SCALE=0 turns it off)\n");
}
/* The animation distance (2026-11-03, after the same mod, whose notes say it fixes the windmills' animation distance). At
   main+0x20a8d3c a command read from game data compares its first value with 50000 and sets a flag above it. The mod raises the
   limit to 100000: movk w8, #0x4743 -> #0x47c3. PSV_FAR_DISTANCE=0 keeps 50000. */
static void patch_far_distance(Module *m) {
    const char *e = getenv("PSV_FAR_DISTANCE"); if (e && !strcmp(e, "0")) return;
    const uint64_t site = 0x20a8d3c; uint32_t w; memcpy(&w, m->base + site, 4);
    if (w != 0x72a8e868) runtime_fail("far distance: instruction differs");
    w = 0x72a8f868; memcpy(m->base + site, &w, 4); sys_icache_invalidate(m->base + site, 4);
    fprintf(stderr, "[psv] far distance: limit 100000 instead of 50000 (PSV_FAR_DISTANCE=0 turns it off)\n");
}
/* The render resolution (2026-11-04). The engine's settings object (made at main+0x146fde4, the same object whose +0x20 is
   the frame mode byte) holds the screen size at +0x10 / +0x18, 1920 x 1080; the settings parser's defaults at main+0x146e7b4
   and the FlatBuffers reader's defaults for absent fields (main+0x1473da8, the size the running game ends up with) write the same. The render targets are that size and its halves (960x540, 480x270, 240x135). PSV_RENDER_RES=<w>x<h>
   writes another size into both defaults (experimental). */
static unsigned render_w, render_h;
int runtime_render_size(unsigned *w, unsigned *h) { if (!render_w) return 0; *w = render_w; *h = render_h; return 1; }
/* The render size: PSV_RENDER_RES=<w>x<h> first, else the settings panel's choice ("render-res <height>" in settings.txt,
   16:9, read at start: the size is fixed before the game runs). 0 = the game's own 1920x1080. */
static unsigned render_pref_h;   /* the panel's saved choice (applies at the next start) */
int runtime_render_res_fixed(void) { const char *e = getenv("PSV_RENDER_RES"); return e && *e; }
unsigned runtime_render_res_pref(void) { return render_pref_h; }
void runtime_render_res_pref_set(unsigned h) { render_pref_h = h == 1440 || h == 1800 || h == 2160 ? h : 0; }
static int render_size(unsigned *w, unsigned *h) {
    static int decided, on; static unsigned cw, ch;
    if (!decided) {
        decided = 1; const char *e = getenv("PSV_RENDER_RES");
        if (TARGET_OS_IPHONE) return 0;   /* iOS keeps the game's size: its heap cap and memory are fixed (services.c) */
        if (e && *e) {
            if (!strcmp(e, "0")) on = 0;
            else if (sscanf(e, "%ux%u", &cw, &ch) != 2 || cw < 640 || ch < 360 || cw > 7680 || ch > 4320 || (cw | ch) & 7) runtime_fail("PSV_RENDER_RES must be <width>x<height>, multiples of 8, 640x360 .. 7680x4320");
            else on = cw != 1920 || ch != 1080;
        } else {
            const char *dir = getenv("PSV_PROFILE_DIR"); char path[1024], name[32]; int v;
            snprintf(path, sizeof path, "%s/settings.txt", dir && *dir ? dir : "user-data");
            FILE *f = fopen(path, "r");
            if (f) { while (fscanf(f, "%31s %d", name, &v) == 2) if (!strcmp(name, "render-res")) runtime_render_res_pref_set((unsigned)v); fclose(f); }
            if (render_pref_h) { ch = render_pref_h; cw = ch * 16 / 9; on = 1; }
        }
    }
    if (on) { *w = cw; *h = ch; }
    return on;
}
static void patch_render_res(Module *m) {
    unsigned w, h;
    if (!render_size(&w, &h)) return;
    static const struct { uint64_t off; uint32_t orig; int is_h; char group; } sites[] = {
        {0x146fe00, 0x5280f008, 0, 'a'}, {0x146fe04, 0x52808709, 1, 'a'},   /* mov w8, #1920 / mov w9, #1080 (settings object) */
        {0x146e7b4, 0x5280f008, 0, 'a'}, {0x146e7b8, 0x52808709, 1, 'a'},   /* the parser's defaults */
        {0x1473da8, 0x5280f008, 0, 'a'}, {0x1473db0, 0x52808708, 1, 'a'},   /* the FlatBuffers reader's defaults for absent fields */
        {0x166950c, 0x5280f004, 0, 'b'}, {0x16694fc, 0x52808705, 1, 'b'},   /* nnMain's framework: a {x, y, w, h} entry (main+0xc8037c) */
        {0x1664100, 0x5280f009, 0, 'c'}, {0x16640f8, 0x5280870a, 1, 'c'},   /* nnMain's framework: a texture description */
        {0x2635370, 0x5280f00a, 0, 'd'}, {0x2635374, 0x5280870b, 1, 'd'},
        {0xbc519c, 0x5280f008, 0, 'e'}, {0xbc5194, 0x52808709, 1, 'e'},
        {0x14b0020, 0x5280f008, 0, 'f'}, {0x14b0024, 0x5280870a, 1, 'f'},
        {0x16694bc, 0x52a89e08, 2, 'g'}, {0x16694c4, 0x52a890e8, 3, 'g'},   /* mov w8, #1920.0f / #1080.0f: nnMain's framework, main+0xc80894 (x, y, w, h) */
        {0x16696a0, 0x52a89e08, 2, 'g'}, {0x16696a8, 0x52a890e8, 3, 'g'},
        /* the display manager's size per operation mode, [obj+0x430] {handheld, docked} widths and [obj+0x438] heights
           (main+0xbd7014): the window crop and, times the display's rectangle, the viewport of the main passes
           (main+0xdfa800 -> parameters read by main+0xb6d220). Docked follows the render size, handheld two thirds of it. */
        {0xbd7018, 0xf2c0f008, 0, 'h'}, {0xbd7024, 0xf2c08708, 1, 'h'},   /* movk x8, #1920 / #1080, lsl #32 (docked) */
        {0xbd7014, 0xd280a008, 4, 'h'}, {0xbd7020, 0xd2805a08, 5, 'h'},   /* mov x8, #1280 / #720 (handheld) */
        /* fixed half-size (960x540) targets of seven renderers: half the render size */
        {0xe939c4, 0x5280780a, 8, 'i'}, {0xe939c8, 0x5280438b, 9, 'i'},
        {0x116f498, 0x5280780a, 8, 'i'}, {0x116f49c, 0x5280438b, 9, 'i'},
        {0x135d1e8, 0x52807808, 8, 'i'}, {0x135d1f0, 0x52804388, 9, 'i'},
        {0x1664240, 0x52807808, 8, 'i'}, {0x1664248, 0x52804389, 9, 'i'},
        {0x17d9e64, 0x52807808, 8, 'i'}, {0x17d9e68, 0x5280438a, 9, 'i'},
        {0x2635c90, 0x5280780a, 8, 'i'}, {0x2635c94, 0x5280438b, 9, 'i'},
        {0x2636298, 0x5280780a, 8, 'i'}, {0x263629c, 0x5280438b, 9, 'i'},
    };
    const char *groups = getenv("PSV_RENDER_SITES"); if (!groups || !*groups) groups = "abcdefghi";   /* diagnostic: which site groups */
    for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) { uint32_t have; memcpy(&have, m->base + sites[i].off, 4); if (have != sites[i].orig) runtime_fail("render resolution: instruction differs"); }
    for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) {
        if (!strchr(groups, sites[i].group)) continue;
        uint32_t v = sites[i].is_h & 1 ? h : w, word;
        if (sites[i].is_h & 4) v = v * 2 / 3 & ~7u;   /* handheld: two thirds, as 1280x720 is of 1920x1080 */
        if (sites[i].is_h & 8) v /= 2;   /* a half-size target */
        if (sites[i].is_h & 2) {   /* a float's upper half (movz w, #imm, lsl #16): exact only when its lower half is zero */
            float f = (float)v; uint32_t bits; memcpy(&bits, &f, 4);
            if (bits & 0xffff) runtime_fail("PSV_RENDER_RES: this size is not exact as a 16-bit float immediate");
            word = (sites[i].orig & 0xffe0001fu) | (bits >> 16) << 5;
        } else word = (sites[i].orig & 0xffe0001fu) | v << 5;
        memcpy(m->base + sites[i].off, &word, 4); sys_icache_invalidate(m->base + sites[i].off, 4);
    }
    render_w = w; render_h = h;
    fprintf(stderr, "[psv] render resolution: %ux%u instead of 1920x1080 (%s)\n", w, h, runtime_render_res_fixed() ? "PSV_RENDER_RES" : "settings panel");
}
/* The GPU memory pools follow the render size (2026-11-04). The settings parser (main+0x146e724) replaces three pool sizes the
   game data gives with smaller ones: [cfg+0x18] 384 -> 350 MiB (flags 0x22), [cfg+0x20] 320 -> 268 MiB (flags 0xa1,
   compressible: the render targets), [cfg+0x28] 48 -> 50 MiB. They are sized for 1080p: with bigger fixed render targets
   a 256 KiB block allocation returned null (crash at main+0xc281d0). With PSV_RENDER_RES the first two grow by the pixel
   ratio (PSV_RENDER_POOLS=<letters>: t the 350 MiB pool, r the 268 MiB pool; default both), and the application pool by
   the same bytes. */
static const struct { uint64_t off; uint32_t orig; uint32_t mib; char key; } render_pools[] = {
    {0x146e738, 0x52a2bc09, 350, 't'},   /* mov w9, #0x15e00000 */
    {0x146e770, 0x52a21809, 268, 'r'},   /* mov w9, #0x10c00000 */
};
static double render_ratio(void) {
    unsigned w, h;
    if (!render_size(&w, &h) || (double)w * h <= 1920.0 * 1080.0) return 1;
    return (double)w * h / (1920.0 * 1080.0);
}
static int render_pool_on(char key) { const char *e = getenv("PSV_RENDER_POOLS"); return !e || strchr(e, key); }
static uint32_t render_pool_mib(unsigned i) { return (uint32_t)ceil(render_pools[i].mib * render_ratio() / 2.0) * 2; }   /* whole 2 MiB */
static uint64_t render_pool_extra(void) {
    uint64_t extra = 0;
    for (unsigned i = 0; i < sizeof render_pools / sizeof *render_pools; i++) if (render_pool_on(render_pools[i].key)) extra += (uint64_t)(render_pool_mib(i) - render_pools[i].mib) << 20;
    return extra;
}
static void patch_render_pools(Module *m) {
    if (render_ratio() <= 1) return;
    for (unsigned i = 0; i < sizeof render_pools / sizeof *render_pools; i++) {
        if (!render_pool_on(render_pools[i].key)) continue;
        uint32_t have; memcpy(&have, m->base + render_pools[i].off, 4); if (have != render_pools[i].orig) runtime_fail("render pools: instruction differs");
        uint32_t mib = render_pool_mib(i), imm = mib << 4;   /* the immediate counts 64 KiB units (lsl #16) */
        if (imm > 0xffff) runtime_fail("render pools: size too large");
        uint32_t word = (have & 0xffe0001fu) | imm << 5; memcpy(m->base + render_pools[i].off, &word, 4); sys_icache_invalidate(m->base + render_pools[i].off, 4);
        fprintf(stderr, "[psv] render resolution: GPU pool %u MiB -> %u MiB\n", render_pools[i].mib, mib);
    }
}
/* Viewport probe (diagnostic, 2026-11-04; PSV_VIEWPORT_PROBE=1). main+0xb53978 gives a pass the size of its target's
   texture wrapper ([wrapper + 0x68] / + 0x70); with a bigger render size the main passes still get 1920 there. A stub in the
   free end of the text page keeps the last wrapper whose width is 1920 at data + 0x100, for a write watch ("watch"):
     main+0xb539fc  ldp x9, x10, [x8, #0x68]  -> b D;  D: ldp x9, x10, [x8, #0x68]; cmp x9, #0x780; b.ne back;
                    adrp x11, data; str x8, [x11, #0x100]; back: b site + 4      (x11 and the flags are dead there) */
static void patch_viewport_probe(Module *m) {
    if (!getenv("PSV_VIEWPORT_PROBE")) return;
    const uint64_t site = 0xb539fc, d = 0x343ff40;
    uint64_t data = (m->file_size - m->image_off + 0x3fff) & ~0x3fffULL; uintptr_t mb = (uintptr_t)m->base;
    uint32_t w; memcpy(&w, m->base + site, 4);
    if (w != 0xa946a909 || m->text_end > d) runtime_fail("viewport probe: site or text padding differ");
    for (uint64_t k = d; k < d + 24; k++) if (m->base[k]) runtime_fail("viewport probe: text padding in use");
    int any = !strcmp(getenv("PSV_VIEWPORT_PROBE"), "any");   /* =any: every pass, to see that the site runs at all */
    uint32_t sd[6] = {0xa946a909, 0xf11e013f, any ? 0xd503201f : 0x54000061, adrp(11, mb + d + 12, mb + data), 0xf9008168, branch(d + 20, site + 4)};
    memcpy(m->base + d, sd, sizeof sd); w = branch(site, d); memcpy(m->base + site, &w, 4);
    sys_icache_invalidate(m->base + d, sizeof sd); sys_icache_invalidate(m->base + site, 4);
    /* the depth-target branch: main+0xb53ab8 ldp x8, x9, [x21, #0x68] -> b E; E: ldp; cmp x8, #0x780; b.ne back;
       adrp x10, data; str x21, [x10, #0x108]; back: b site + 4      (x10 and the flags are dead there) */
    const uint64_t site2 = 0xb53ab8, e2 = 0x343ff60; memcpy(&w, m->base + site2, 4);
    if (w != 0xa946a6a8) runtime_fail("viewport probe: depth site differs");
    for (uint64_t k = e2; k < e2 + 24; k++) if (m->base[k]) runtime_fail("viewport probe: text padding in use");
    uint32_t se[6] = {0xa946a6a8, 0xf11e011f, any ? 0xd503201f : 0x54000061, adrp(10, mb + e2 + 12, mb + data), 0xf9008555, branch(e2 + 20, site2 + 4)};
    memcpy(m->base + e2, se, sizeof se); w = branch(site2, e2); memcpy(m->base + site2, &w, 4);
    sys_icache_invalidate(m->base + e2, sizeof se); sys_icache_invalidate(m->base + site2, 4);
    fprintf(stderr, "[psv] viewport probe: last 1920-wide texture wrapper at %p (data + 0x100)\n", (void *)(m->base + data + 0x100));
}
/* Draw distance (2026-11-04). The scene's trinity_ModelLodSetting tables (world/scene/parts/field/world_resident/
   common_resident_*.trscn) give per model category three distances: two LOD switches and the distance at which the model
   is no longer drawn (measured in the grassland: 30/40/60, 40/80/100, 80/150/250, 100/200/300, 180/280/400, 350/500/1000).
   main+0x2095a80 copies each category's three floats out of the file; a stub at its first load scales them by data + 0xe8
   (and keeps the last raw three at data + 0x140 for the log):
     ldr x8, [x3]  -> b S;  S: adrp x9, data; ldr d0, [x3]; ldr s1, [x3, #8]; str d0, [x9, #0x140]; str s1, [x9, #0x148];
                               ldr s2, [x9, #0xe8]; fmul v0.2s, v0.2s, v2.s[0]; fmul s1, s1, s2; fmov x8, d0; fmov w9, s1;
                               b site + 8   (the original also loads w9 = [x3 + 8])
   v0-v2 are free there (the function takes no float arguments). A mod that edits the same tables (Team Seri's LOD mod) is
   scaled on top. The tables are read when the world loads, so a change applies at the next start.
   PSV_DRAW_DIST=<factor> (0.5 .. 4) decides first, else the panel's choice ("draw-dist <percent>" in settings.txt). */
static _Atomic float *draw_dist;
static unsigned draw_dist_pref = 100;   /* percent, the panel's saved choice */
int runtime_draw_dist_fixed(void) { const char *e = getenv("PSV_DRAW_DIST"); return e && *e; }
unsigned runtime_draw_dist_pref(void) { return draw_dist_pref; }
void runtime_draw_dist_pref_set(unsigned pct) { draw_dist_pref = pct == 150 || pct == 200 || pct == 300 ? pct : 100; }
static void patch_draw_dist(Module *m) {
    const uint64_t site = 0x2095aa0, c = 0x343fe00;
    uint64_t data = (m->file_size - m->image_off + 0x3fff) & ~0x3fffULL; uintptr_t mb = (uintptr_t)m->base;
    uint32_t w[2]; memcpy(w, m->base + site, 8);
    if (w[0] != 0xf9400068 || w[1] != 0xb9400869 || m->text_end > c) runtime_fail("draw distance: site or text padding differ");
    for (uint64_t k = c; k < c + 44; k++) if (m->base[k]) runtime_fail("draw distance: text padding in use");
    const char *e = getenv("PSV_DRAW_DIST"); float f;
    if (e && *e) { f = strtof(e, NULL); if (!(f >= 0.5f && f <= 4.0f)) runtime_fail("PSV_DRAW_DIST must be 0.5 .. 4"); }
    else {
        const char *dir = getenv("PSV_PROFILE_DIR"); char path[1024], name[32]; int v;
        snprintf(path, sizeof path, "%s/settings.txt", dir && *dir ? dir : "user-data");
        FILE *fp = fopen(path, "r");
        if (fp) { while (fscanf(fp, "%31s %d", name, &v) == 2) if (!strcmp(name, "draw-dist")) runtime_draw_dist_pref_set((unsigned)v); fclose(fp); }
        f = draw_dist_pref / 100.0f;
    }
    if (f == 1.0f) { fprintf(stderr, "[psv] draw distance: the game's own (PSV_DRAW_DIST or the panel)\n"); return; }
    draw_dist = (_Atomic float *)(void *)(m->base + data + 0xe8); atomic_store(draw_dist, f);
    uint32_t st[11] = {adrp(9, mb + c, mb + data), 0xfd400060, 0xbd400861, 0xfd00a120, 0xbd014921, 0xbd40e922, 0x0f829000, 0x1e220821,
                       0x9e660008, 0x1e260029, branch(c + 40, site + 8)};
    memcpy(m->base + c, st, sizeof st); uint32_t b = branch(site, c); memcpy(m->base + site, &b, 4);
    sys_icache_invalidate(m->base + c, sizeof st); sys_icache_invalidate(m->base + site, 4);
    fprintf(stderr, "[psv] draw distance: model LOD and draw distances x%.2f\n", f);
}
/* The job system's lost wake-up (2026-11-04, the hang once per 5 to 15 minutes). The job system's "done" setter at
   main+0x2d0010c stores done = 1 (strb, plain) at system + 0x20c and then reads the main thread's worker state with ldar: state 2
   (awake) means no wake-up. The main thread, back from a wake-up, stores state 0 (stlr), takes its queue lock (ldaxr) and then
   reads done before it sleeps. ARMv8 lets the plain store become visible after the later ldar, so the setter can read the old
   state 2 while the main thread reads the old done 0: the main thread sleeps for ever with done = 1 (seen in the hang: system +
   0x20c = 1, all queues empty, every worker asleep). A dmb ish between the store and the load closes it:
     main+0x2d00120  strb w9, [x8, #0x20c]  -> b C;  C: strb w9, [x8, #0x20c]; dmb ish; b site + 4
   PSV_JOB_FENCE=0 leaves the site alone. */
static void patch_job_fence(Module *m) {
    const char *e = getenv("PSV_JOB_FENCE"); if (e && !strcmp(e, "0")) return;
    const uint64_t site = 0x2d00120, c = 0x343fe80;
    uint32_t w; memcpy(&w, m->base + site, 4);
    if (w != 0x39083109 || m->text_end > c) runtime_fail("job fence: site or text padding differ");
    for (uint64_t k = c; k < c + 12; k++) if (m->base[k]) runtime_fail("job fence: text padding in use");
    uint32_t st[3] = {0x39083109, 0xd5033bbf, branch(c + 8, site + 4)};
    memcpy(m->base + c, st, sizeof st); w = branch(site, c); memcpy(m->base + site, &w, 4);
    sys_icache_invalidate(m->base + c, sizeof st); sys_icache_invalidate(m->base + site, 4);
    fprintf(stderr, "[psv] job fence: barrier after the job system's done flag (PSV_JOB_FENCE=0 turns it off)\n");
}
/* The effect step (2026-11-05, the redraw after a ball throw at 60 fps). The effect animation player at main+0xb83d1c adds
   delta / (table[index] - 1) to the effect's frame (+0x88) and evaluates the animation there. Effect animations are keyed in
   1/30 s, and the ball throw's catch camera is one with a cut in it: evaluated between two keys at the cut (every second
   60 fps frame, or a hair short of a key at 30 fps), the camera stood about 3.3 km out along the throw for one frame, and the
   world streaming rebuilt everything around it. Measured: a step of 2 frames per 60 fps frame (twice as fast) and 0, 2, 0, 2
   (normal speed) both end the redraw; a first step of 2, or an exact 1.0 per frame, do not.
   Above 30 fps every running effect now steps by data + 0x1c4 frames: whole 1/30 s steps (2 frames of the 60 fps mode's
   1/60 s) handed out as the game's time adds up (effect_step_tick). A paused effect (step 0) stays 0. A negative value keeps
   the game's own step (the 30 fps mode, where whole ticks already land on the keys). Effects move at 30 Hz in the 60 fps
   mode, at their normal speed. PSV_EFFECT_STEP=0 leaves the site alone.
     main+0xb83d8c  ldr s0, [x20, #0x88]; fadd s0, s8, s0  ->  b C; nop
     C: ldr s0, [x20, #0x88]; adrp x16, data; ldr s16, [x16, #0x1c4]; fcmp s16, #0.0; b.lt keep; fcmp s8, #0.0; b.eq keep;
        fmov s8, s16; keep: fadd s0, s8, s0; b site + 8 */
static _Atomic float *effect_step;
static void patch_effect_step(Module *m) {
    const char *e = getenv("PSV_EFFECT_STEP"); if (e && !strcmp(e, "0")) return;
    const uint64_t site = 0xb83d8c, c = 0x343fe40;
    uint64_t data = (m->file_size - m->image_off + 0x3fff) & ~0x3fffULL; uintptr_t mb = (uintptr_t)m->base;
    uint32_t w[2]; memcpy(w, m->base + site, 8);
    if (w[0] != 0xbd408a80 || w[1] != 0x1e202900 || m->text_end > c) runtime_fail("effect step: site or text padding differ");
    for (uint64_t k = c; k < c + 40; k++) if (m->base[k]) runtime_fail("effect step: text padding in use");
    effect_step = (_Atomic float *)(void *)(m->base + data + 0x1c4); atomic_store(effect_step, -1.0f);
    uint32_t st[10] = {0xbd408a80, adrp(16, mb + c + 4, mb + data), 0xbd41c610, 0x1e202208, 0x5400008b, 0x1e202108, 0x54000040,
                       0x1e204208, 0x1e202900, branch(c + 36, site + 8)};
    memcpy(m->base + c, st, sizeof st); uint32_t b[2] = {branch(site, c), 0xd503201f}; memcpy(m->base + site, b, 8);
    sys_icache_invalidate(m->base + c, sizeof st); sys_icache_invalidate(m->base + site, 8);
    fprintf(stderr, "[psv] effect step: effects step in whole 1/30 s above 30 fps (PSV_EFFECT_STEP=0 turns it off)\n");
}
/* The tick reader (2026-11-05, effects missing above 60 fps). main+0x20397a4 reads time slot 1's delta, divides it by 1/60 s
   and truncates (fcvtzs): whole 60 Hz ticks, times a speed, for an effect system (main+0xb85c44). At 90 and 120 fps a frame is
   under one tick, so it got 0 and attack and catch effects never showed. It now loads data + 0x1c8 instead: whole ticks handed
   out as the game's time adds up (tick_reader_tick), as a delta of n x 16666667 ns. At 30 and 60 fps that is the frame's own
   step. PSV_TICK_READER=0 leaves the site alone.
     main+0x20397a4  ldr x8, [x0, #8]  ->  b C;  C: adrp x8, data; ldr x8, [x8, #0x1c8]; b site + 4 */
static _Atomic int64_t *tick_reader;
static void patch_tick_reader(Module *m) {
    const char *e = getenv("PSV_TICK_READER"); if (e && !strcmp(e, "0")) return;
    const uint64_t site = 0x20397a4, c = 0x343fe68;
    uint64_t data = (m->file_size - m->image_off + 0x3fff) & ~0x3fffULL; uintptr_t mb = (uintptr_t)m->base;
    uint32_t w; memcpy(&w, m->base + site, 4);
    if (w != 0xf9400408 || m->text_end > c) runtime_fail("tick reader: site or text padding differ");
    for (uint64_t k = c; k < c + 12; k++) if (m->base[k]) runtime_fail("tick reader: text padding in use");
    tick_reader = (_Atomic int64_t *)(void *)(m->base + data + 0x1c8); atomic_store(tick_reader, 16666667);
    uint32_t st[3] = {adrp(8, mb + c, mb + data), 0xf940e508, branch(c + 8, site + 4)};
    memcpy(m->base + c, st, sizeof st); w = branch(site, c); memcpy(m->base + site, &w, 4);
    sys_icache_invalidate(m->base + c, sizeof st); sys_icache_invalidate(m->base + site, 4);
    fprintf(stderr, "[psv] tick reader: effects get whole 60 Hz ticks above 60 fps (PSV_TICK_READER=0 turns it off)\n");
}
/* frames: this game frame's step in 60 Hz ticks (may be a fraction above 60 fps) */
static void tick_reader_tick(double ticks) {
    if (!tick_reader) return;
    static double acc; acc += ticks; int n = (int)(acc + 1e-6); acc -= n; if (acc < 0) acc = 0;
    atomic_store_explicit(tick_reader, (int64_t)n * 16666667, memory_order_relaxed);
}
/* Called after each frame's clock step: frames = the effect frames this game frame adds in the 60 fps mode (its step / 1/60 s). */
static void effect_step_tick(double frames) {
    if (!effect_step) return;
    if (!frame_mode60) { atomic_store_explicit(effect_step, -1.0f, memory_order_relaxed); return; }
    static double acc; acc += frames; double out = 0;
    while (acc >= 2.0 - 1e-6) { out += 2.0; acc -= 2.0; }
    atomic_store_explicit(effect_step, (float)out, memory_order_relaxed);
}
static void draw_dist_log(void) {
    if (!draw_dist) return; static float last[3]; static int n;
    _Atomic float *raw = draw_dist + 22;   /* data + 0x140 */
    float v[3]; for (int k = 0; k < 3; k++) v[k] = atomic_load_explicit(&raw[k], memory_order_relaxed);
    if (n < 40 && memcmp(v, last, sizeof v)) { memcpy(last, v, sizeof v); n++;
        fprintf(stderr, "[psv] draw distance: a category %.0f / %.0f / %.0f -> x%.2f\n", v[0], v[1], v[2], atomic_load(draw_dist)); }
}
void runtime_game_patches(Module *main_module) {
    patch_viewport_probe(main_module);
    patch_render_res(main_module);
    patch_render_pools(main_module);
    patch_frame_step(main_module);
    patch_fixed_step(main_module);
    patch_jump(main_module);
    patch_far_distance(main_module);
    patch_draw_dist(main_module);
    patch_job_fence(main_module);
    patch_effect_step(main_module);
    if (frame_table) patch_tick_reader(main_module);   /* only with the frame step: it feeds the ticks */
    /* PSV_STOP_AT_START=1: the process stops itself before the game's first instruction, so a debugger can attach and set
       watchpoints on startup state (lldb's continue resumes it). */
    if (getenv("PSV_STOP_AT_START")) { fprintf(stderr, "[psv] stopped for a debugger (PSV_STOP_AT_START), pid %d\n", getpid()); raise(SIGSTOP); }
    const char *list = getenv("PSV_PATCHES");
    if (!list || !*list) { fprintf(stderr, "[psv] game patches: none (PSV_PATCHES)\n"); return; }
    char buf[1024]; snprintf(buf, sizeof buf, "%s", list);
    for (char *save = NULL, *s = strtok_r(buf, ",", &save); s; s = strtok_r(NULL, ",", &save)) {
        for (const char *c = s; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) runtime_fail("PSV_PATCHES: bad patch name");
        apply_file(main_module, s);
    }
}
static void frame_table_store(double dt) {
    for (int k = 0; k < 5; k++) atomic_store_explicit(&frame_table[k], dt > 0 ? (int64_t)(dt * 1e9 + 0.5) : frame_table_orig[k], memory_order_relaxed);
}
void runtime_frame_step_pin(double ms) {
    uint32_t us = ms > 0 ? (uint32_t)(ms * 1000.0 + 0.5) : 0; atomic_store(&pinned_step_us, us);
    if (frame_table && us) frame_table_store(ms / 1e3);   /* the exact value asked: "framestep 33.333334" gives the game's 33333334 ns */
    fprintf(stderr, "[psv] frame step %s (pin %.3f ms)\n", us ? "pinned" : "real time", us / 1000.0);
}
/* The game's own 60 fps mode (2026-10-31): with a frame lock of 40 or more, the step byte and the time-scale index are kept at 0
   (60 fps), else at 1 (30 fps), every present. With the measured clock step the time scales are then 1 at 60 fps (as on the
   Switch 2) and 0.5 at 120, instead of 0.5 and 0.25. PSV_FRAME_MODE=game leaves both to the game. */
extern int nvn_fps_lock(void);
static void frame_mode_keep(void) {
    static int mode = -1; if (mode < 0) { const char *e = getenv("PSV_FRAME_MODE"); mode = !(e && !strcmp(e, "game")); }
    uint8_t *mb = runtime_module_base("main"); if (!mb) return;
    if (!mode) {   /* the game's own choice, logged when it changes */
        uintptr_t obj = *(volatile uintptr_t *)(mb + PSV_FRAME_OBJECT_GLOBAL), rate = *(volatile uintptr_t *)(mb + PSV_FRAME_RATE_GLOBAL);
        uintptr_t cfg = obj ? *(volatile uintptr_t *)(obj + 0x30) : 0; if (!cfg || !rate) return;
        static int last = -1; int now = (*(volatile int8_t *)(cfg + 0x20) & 0xff) | (*(volatile int32_t *)(rate + 0xd0) & 0xff) << 8;
        if (now != last) { last = now; fprintf(stderr, "[psv] frame mode (game): step %d, index %d (settings %#lx, rate %#lx)\n", now & 0xff, now >> 8, (unsigned long)cfg, (unsigned long)rate); }
        return;
    }
    if (fixed_step) { static int last = -1; int st = *(volatile int32_t *)((volatile uint8_t *)fixed_step + 8);   /* data + 0x88: the manager state the stub saw */
        if (st != last) { last = st; fprintf(stderr, "[psv] fixed step: manager state %d t=%.3f\n", st, runtime_now_ns() / 1e9); } }
    int want = nvn_fps_lock() >= 40 ? 0 : 1; frame_mode60 = !want;
    if (fixed_step) atomic_store_explicit(fixed_step, frame_mode60 ? 33333334 : 0, memory_order_relaxed);
    if (jump_scale) { atomic_store_explicit(&jump_scale[0], frame_mode60 ? 1.95f : 1.0f, memory_order_relaxed); atomic_store_explicit(&jump_scale[1], frame_mode60 ? 20.5f : __builtin_inff(), memory_order_relaxed); }
    if (scale_table) for (int k = 0; k < 5; k++) atomic_store_explicit(&scale_table[k], want ? frame_table_orig[k] : 16666667, memory_order_relaxed);   /* the mode's frame for every index */
    if (ui_scale_table) for (int k = 0; k < 5; k++) atomic_store_explicit(&ui_scale_table[k], want ? frame_table_orig[k] : 33333334, memory_order_relaxed);   /* 1/30 s: scale 0.5 per 60 fps frame */
    uintptr_t obj = *(volatile uintptr_t *)(mb + PSV_FRAME_OBJECT_GLOBAL), rate = *(volatile uintptr_t *)(mb + PSV_FRAME_RATE_GLOBAL);
    uintptr_t cfg = obj ? *(volatile uintptr_t *)(obj + 0x30) : 0;
    if (!cfg || !rate) return;
    volatile int8_t *step = (volatile int8_t *)(cfg + 0x20); volatile int32_t *index = (volatile int32_t *)(rate + 0xd0);
    if (*step == want && *index == want) return;
    if ((*step != 0 && *step != 1) || (*index != 0 && *index != 1)) { static int said; if (!said++) fprintf(stderr, "[psv] frame mode: unexpected step %d / index %d, left alone\n", *step, *index); return; }
    *step = (int8_t)want; *index = want;
    fprintf(stderr, "[psv] frame mode: %d fps steps (frame lock %d)\n", want ? 30 : 60, nvn_fps_lock());
}
extern void nvn_trace(const char *name, double v) __attribute__((weak_import));
static uint64_t step_doubles;
uint64_t runtime_frame_step_doubles(void) { return step_doubles; }   /* gfx/nvn/hle_nvn.c: the slow-frame line */   /* gfx/nvn/hle_nvn.c: a row of the DM_NVN_TSV trace */
void runtime_frame_step_update(uint64_t now_ns) {
    frame_mode_keep();
    draw_dist_log();
    if (!frame_table || !now_ns) return;
    if (atomic_load_explicit(&pinned_step_us, memory_order_relaxed)) { last_present_ns = now_ns; return; }
    if (!last_present_ns) { last_present_ns = now_ns; return; }
    double dt = (double)(now_ns - last_present_ns) * 1e-9; last_present_ns = now_ns;
    if (dt > 0.25 || dt < 0.001) { frame_sample_count = frame_sample_next = 0; return; }
    if (dt > 1.0 / 15.0) dt = 1.0 / 15.0;   /* a hitch counts at most 1/15 s, as in the earlier port */
    frame_samples[frame_sample_next++ % 10] = dt; if (frame_sample_count < 10) frame_sample_count++;
    double sum = 0; for (unsigned i = 0; i < frame_sample_count; i++) sum += frame_samples[i];
    /* 60 fps mode (2026-10-31): whole 60 Hz ticks, the remainder carried to the next frame. Effects (the Tera shine, the grass in
       the air, Psystrike) jerked at a 60 fps lock and stopped at 120 although walking was right: some of the game counts whole
       ticks of the clock step, and a measured step just under 16.67 ms (or 8.3 ms at 120) gave 0 ticks. The game's own 60 fps
       mode only ever steps by whole ticks. At least one tick per frame: a step of 0 stopped the game, at its second
       frame and in the world (tried for 120 fps, 2026-10-31). So only up to a 60 fps lock; above it the measured step stays and
       effects there still run slow. PSV_FRAME_QUANT=0: the measured step. */
    static int quant = -1; if (quant < 0) { const char *e = getenv("PSV_FRAME_QUANT"); quant = !(e && !strcmp(e, "0")); }
    /* The 30 fps mode too (2026-11-05): a counter on clock 0 (main+0x92dd20) steps in whole 1/30 s. The ball throw's catch camera
       starts one, and a first frame a hair under 33,333,334 ns gave it no step: for that frame the camera stood about 3.3 km out
       along the throw, and the world streaming rebuilt everything (the redraw after a throw). Whole ticks, rounded, give exactly
       2 ticks = 33,333,334 ns at 30 fps. Above 30 fps the effect step (patch_effect_step) keeps effects on whole 1/30 s. */
    if (quant && nvn_fps_lock() <= 60) {   /* never 0 ticks: a step of 0 stopped the game (at its second frame, and in the world) */
        static double acc; const double tick = 16666667e-9; acc += sum / frame_sample_count;
        int n = frame_mode60 ? (int)(acc / tick) : (int)(acc / tick + 0.5); if (n < 1) n = 1; if (n > 4) n = 4; acc -= n * tick; if (acc < -tick) acc = -tick; if (acc > tick) acc = tick;
        for (int k = 0; k < 5; k++) atomic_store_explicit(&frame_table[k], (int64_t)n * 16666667, memory_order_relaxed);
        effect_step_tick(n); tick_reader_tick(n);
        if (n > 1) step_doubles++;
        if (nvn_trace) { nvn_trace("@step_ticks", n); nvn_trace("@step_dt_ms", dt * 1e3); }
    } else { frame_table_store(sum / frame_sample_count); effect_step_tick(sum / frame_sample_count / 16666667e-9); tick_reader_tick(sum / frame_sample_count / 16666667e-9); }
}

/* The earlier port's host hooks in the shared graphics layer and the input keys call keep their names and answer "off". */
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
/* The render resolution's GPU pools (patch_render_pools): what they grow by, added to the application pool (services.c). */
static uint64_t render_pool_extra(void);
uint64_t runtime_output_res_extra_bytes(void) { return render_pool_extra(); }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
