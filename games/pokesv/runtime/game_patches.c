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
        uint32_t w[2] = {adrp(8, (uintptr_t)m->base + sites[i].off, (uintptr_t)m->base + data), 0x91000108};
        memcpy(m->base + sites[i].off, w, 8); sys_icache_invalidate(m->base + sites[i].off, 8);
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
void runtime_game_patches(Module *main_module) {
    patch_frame_step(main_module);
    patch_fixed_step(main_module);
    patch_jump(main_module);
    patch_far_distance(main_module);
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
    if (frame_table && us) frame_table_store(us / 1e6);
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
    if (quant && frame_mode60 && nvn_fps_lock() <= 60) {   /* never 0 ticks: a step of 0 stopped the game (at its second frame, and in the world) */
        static double acc; const double tick = 16666667e-9; acc += sum / frame_sample_count;
        int n = (int)(acc / tick); if (n < 1) n = 1; if (n > 4) n = 4; acc -= n * tick; if (acc < -tick) acc = -tick; if (acc > tick) acc = tick;
        for (int k = 0; k < 5; k++) atomic_store_explicit(&frame_table[k], (int64_t)n * 16666667, memory_order_relaxed);
        if (n > 1) step_doubles++;
        if (nvn_trace) { nvn_trace("@step_ticks", n); nvn_trace("@step_dt_ms", dt * 1e3); }
    } else frame_table_store(sum / frame_sample_count);
}

/* The earlier port's host hooks in the shared graphics layer and the input keys call keep their names and answer "off". */
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
