#include "game_patches.h"
#include <libkern/OSCacheControl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <TargetConditionals.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Pokémon Legends: Arceus: the player's 1.1.1 exefs patches (60 fps, 900p, LOD, film grain), applied to main after relocation and
   before its text is sealed. scripts/prepare_patches.py turns each .pchtxt into patches/<slug>.tsv ("offset new orig",
   hex), already checked against the original image and the rewrite; here every site is checked again against the loaded
   bytes, and a mismatch stops the start (a wrong version or a rewritten word must never be patched).
   PLA_PATCHES=<slug>,<slug>,... picks them (the launcher sets it from the settings; unset or empty: none). */
	/* Experimental 60 fps track switched at runtime (key 5): the game's own frame step reads our word, so flipping the
	   7 instructions back and forth is enough - the pacer follows runtime_frame_step_active(). */
	static uint8_t *g_step_site; static uint32_t *g_step_orig, *g_step_repl; static int g_step_can_toggle;
	void runtime_frame_step_toggle(void);
static int apply_realtime(Module *m, int strict);
static int pla_realtime_mode(void);
static int hexbytes(const char *s, uint8_t *out, size_t cap, size_t *n) {
    size_t k = 0;
    for (; s[0] && s[1] && s[0] != '\t' && s[0] != '\n'; s += 2) {
        unsigned v; if (k == cap || sscanf(s, "%2x", &v) != 1) return 0;
        out[k++] = (uint8_t)v;
    }
    *n = k; return k > 0;
}
static void apply_file(Module *m, const char *slug) {
    char path[512]; snprintf(path, sizeof path, "patches/%s.tsv", slug);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[pla] patch %s: %s missing (scripts/prepare_patches.py)\n", slug, path); runtime_fail("game patch table missing"); }
    char line[4096]; unsigned applied = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        uint64_t off = strtoull(line, NULL, 0); char *a = strchr(line, '\t'), *b = a ? strchr(a + 1, '\t') : NULL;
        uint8_t now[1024], was[1024]; size_t nn, nw;
        if (!a || !b || !hexbytes(a + 1, now, sizeof now, &nn) || !hexbytes(b + 1, was, sizeof was, &nw) || nn != nw)
            runtime_fail("malformed game patch table");
        if (off + nn > m->text_end && off < m->ro_off) runtime_fail("game patch in the rewrite area");
        if (off + nn > m->file_size) runtime_fail("game patch beyond main");
        if (memcmp(m->base + off, was, nw)) { fprintf(stderr, "[pla] patch %s at main+%#llx: original bytes differ\n", slug, (unsigned long long)off); runtime_fail("game patch does not match this game version"); }
        memcpy(m->base + off, now, nn); sys_icache_invalidate(m->base + off, nn); applied++;
    }
    fclose(f);
    fprintf(stderr, "[pla] game patch %s: %u sites\n", slug, applied);
}
#include "arm64_atomics.h"
/* Normalize the compiler's exchange sequence across the verified text before sealing it executable. */
static void apply_compat(Module *m) {
    size_t words = m->text_end / sizeof(uint32_t);
    if (m->text_end > m->file_size || (m->text_end & 3)) runtime_fail("invalid module text bounds");
    uint8_t *targets = calloc(words, 1);
    if (!targets) runtime_fail("atomic branch-target table allocation failed");
    size_t count = arm64_lower_exchanges((uint32_t *)m->base, words, targets);
    free(targets);
    if (count) {
        sys_icache_invalidate(m->base, m->text_end);
        fprintf(stderr, "[pla] lowered %zu compiler exchange loops to LSE atomics\n", count);
    }
}
void runtime_game_patches(Module *main_module) {
    apply_compat(main_module);
    int mode = pla_realtime_mode();
    if (!mode) fprintf(stderr, "[pla] real-time frame step: off (PLA_REALTIME=0)\n");
    else if (!apply_realtime(main_module, mode == 2)) { /* soft: the game keeps its own step and the port keeps its pacing */ }
    const char *list = getenv("PLA_PATCHES");
    if (!list || !*list) { fprintf(stderr, "[pla] game patches: none (PLA_PATCHES)\n"); return; }
    char buf[1024]; snprintf(buf, sizeof buf, "%s", list);
    for (char *save = NULL, *s = strtok_r(buf, ",", &save); s; s = strtok_r(NULL, ",", &save)) {
        for (const char *c = s; *c; c++) if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')) runtime_fail("PLA_PATCHES: bad patch name");
        apply_file(main_module, s);
    }
}

/* Real-time frame step (PLA_REALTIME=1, off by default). Same mechanism as the TotK ports: the host measures the
   interval between presented pictures and writes it into an 8-byte word in main's private page after the rewritten
   image (low float: the step expressed in 30 fps frames, high float: the step in seconds). The game's main loop
   (main+0x27812dc) asks its clock object for a raw time, divides by 1e9 and passes that float to
   TaskManager::Update (main+0x282841c), which subtracts it from every task timer — that float is the game's own
   per-frame step. The main loop's conversion is replaced by a load of the seconds lane:
     main+0x27814b4  ldr  d0,[x0,#8]          -> adrp x9, <page>
     main+0x27814b8  mov  x8,#0xcd6500000000  -> ldr  s0,[x9,#4]     seconds lane
     main+0x27814bc  movk x8,#0x41cd,lsl#48   -> nop
     main+0x27814c0  fmov d1,x8               -> nop
     main+0x27814c4  scvtf d0,d0              -> nop
     main+0x27814c8  fdiv d0,d0,d1            -> nop
     main+0x27814cc  fcvt s0,d0               -> nop
   (0x27814d0 mov x0,x22 and 0x27814d4 bl TaskManager::Update are untouched). Every original word is verified first; a
   mismatch stops the start rather than patching a different build. With the switch off no word is written and the game
   keeps its own step. The patch works only because the loader reserves this writable page after main (loader.c). */
static _Atomic uint64_t *frame_step_data;
static uint64_t last_present_ns;
static double frame_samples[5];
static unsigned frame_sample_count, frame_sample_next;

/* 0 = off (PLA_REALTIME=0), 1 = on (the default: the native 60 fps track), 2 = strict (PLA_REALTIME=1: a text mismatch
   stops the start instead of falling back, for testing one exact game image). */
static int pla_realtime_mode(void) {
    /* Default off (2026-11-04): with the 60 fps step the game advanced twice as fast as real time, because part of
       PLA's simulation is advanced once per update rather than by dt. 30 fps keeps the speed right; key 2 turns on
       30+30 interpolation for a 60 Hz picture. PLA_REALTIME=1 selects the native 60 fps track (experimental: too fast
       until the per-update logic is found). */
    const char *e = getenv("PLA_REALTIME");
    if (!e || !*e) return 0;
    if (!strcmp(e, "0") || !strcmp(e, "off")) return 0;
    return 2;
}

static uint64_t frame_step_bits(float seconds) {
    float ratio = seconds * 30.0f;   /* PLA's native logic rate: the step in 30 fps frames */
    uint32_t lo, hi;
    memcpy(&lo, &ratio, 4);
    memcpy(&hi, &seconds, 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

static uint32_t adrp_reg(unsigned rd, uint64_t site, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(site >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("real-time frame-step data outside ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) | (rd & 31u);
}

/* iOS cannot write code pages at run time: the export runs these patches on the Mac and the app carries the patched
   text (TARGET_OS_IPHONE). There the loader only checks the words are already patched. */
#if TARGET_OS_IPHONE
#define CODE_PATCHED_AT_BUILD 1
#else
#define CODE_PATCHED_AT_BUILD 0
#endif
static void put_word(Module *m, uint64_t off, uint32_t word, const char *what) {
    if (!CODE_PATCHED_AT_BUILD) { memcpy(m->base + off, &word, 4); return; }
    uint32_t have;
    memcpy(&have, m->base + off, 4);
    if (have != word) {
        fprintf(stderr, "[pla] %s main+%#llx: embedded %08x, expected patched %08x\n", what, (unsigned long long)off, have, word);
        runtime_fail("embedded main text was not patched at build time (ios/tools/build_embed.sh)");
    }
}

static int frame_step_active;
/* The loader asks this before letting the port present on every display tick: the 60 fps track is only safe when the
   game's own step really reads our word (without it the game would run at double speed). */
int runtime_frame_step_active(void) { return frame_step_active; }
/* key 5: switch the experimental 60 fps track on or off while playing (the game keeps its own 30 fps step when off). */
void runtime_frame_step_toggle(void) {
    if (!g_step_can_toggle) { fprintf(stderr, "[pla] real-time frame step: not patchable in this build\n"); return; }
    if (frame_step_active) {
        memcpy(g_step_site, g_step_orig, 7 * 4); sys_icache_invalidate(g_step_site, 7 * 4); frame_step_active = 0;
        fprintf(stderr, "[pla] real-time frame step: off (30 fps track, key 5 turns the experimental 60 fps track on)\n");
    } else {
        memcpy(g_step_site, g_step_repl, 7 * 4); sys_icache_invalidate(g_step_site, 7 * 4); frame_step_active = 1;
        if (frame_step_data) atomic_store(frame_step_data, frame_step_bits(1.0f / 60.0f));
        fprintf(stderr, "[pla] real-time frame step: on (experimental 60 fps track - the game runs faster than real time, key 5 goes back)\n");
    }
}

static int apply_realtime(Module *m, int strict) {
    /* 7 words of the main loop's clock -> seconds conversion. Offsets are the same in main.elf and the rewritten image
       (base-relative offset == vaddr). */
    static const uint64_t site = 0x27814b4ULL;
    static const uint32_t original[7] = {0xfd400400, 0xd2d9aca8, 0xf2e839a8, 0x9e670101, 0x5e61d800, 0x1e611800, 0x1e624000};
    uint32_t repl[7];
    uint64_t data_off = ((uint64_t)m->file_size + 0x3fff) & ~0x3fffULL;
    if (data_off & 0x3fff || data_off + 0x4000 > m->size || site + sizeof original > m->text_end) {
        fprintf(stderr, "[pla] real-time frame step: no room for the data page (size %llu, text end %llu)\n",
                (unsigned long long)m->size, (unsigned long long)m->text_end);
        if (strict) runtime_fail("real-time frame-step page layout");
        fprintf(stderr, "[pla] real-time frame step: 60 fps track off, the game keeps its own step\n");
        return 0;
    }
    for (size_t i = 0; i < sizeof original / sizeof *original; i++) {
        uint32_t have;
        memcpy(&have, m->base + site + 4 * i, 4);
        if (!CODE_PATCHED_AT_BUILD && have != original[i]) {
            fprintf(stderr, "[pla] real-time frame step main+%#llx: found %08x expected %08x\n",
                    (unsigned long long)(site + 4 * i), have, original[i]);
            if (strict) runtime_fail("real-time frame step requires matching game-version original instructions");
            fprintf(stderr, "[pla] real-time frame step: this is not the image the track was made for, 60 fps stays off and the game keeps its own step (PLA_REALTIME=1 makes this fatal)\n");
            return 0;
        }
    }
    const uint32_t nop = 0xd503201f;
    repl[0] = adrp_reg(9, (uintptr_t)m->base + site, (uintptr_t)m->base + data_off);
    repl[1] = 0xbd400520;   /* ldr s0, [x9, #4] : the step in seconds */
    repl[2] = nop;
    repl[3] = nop;
    repl[4] = nop;
    repl[5] = nop;
    repl[6] = nop;
    if (CODE_PATCHED_AT_BUILD)
        for (size_t i = 0; i < 7; i++) put_word(m, site + 4 * i, repl[i], "real-time frame step");
    else {
        memcpy(m->base + site, repl, sizeof repl);
        sys_icache_invalidate(m->base + site, sizeof repl);
    }
    frame_step_data = (_Atomic uint64_t *)(void *)(m->base + data_off);
    atomic_store(frame_step_data, frame_step_bits(1.0f / 60.0f));
    frame_step_active = 1;
    {   /* keep what a hotkey needs to switch the track on and off while the game runs */
        static uint32_t saved_orig[7], saved_repl[7];
        memcpy(saved_orig, original, sizeof original); memcpy(saved_repl, repl, sizeof repl);
        g_step_site = m->base + site; g_step_orig = saved_orig; g_step_repl = saved_repl; g_step_can_toggle = 1; }
    fprintf(stderr, "[pla] real-time frame step: 7 verified instructions at main+0x%llx; initial 16.67 ms, sampled at each present; the port presents every display tick\n",
            (unsigned long long)site);
    return 1;
}

/* Diagnostics (TOTK_INPUT_FILE "framestep <ms>"): pin the step to a fixed length, 0 back to real time. */
static _Atomic uint32_t pinned_step_us;
void runtime_frame_step_pin(double ms) {
    uint32_t us = ms > 0 ? (uint32_t)(ms * 1000.0 + 0.5) : 0;
    atomic_store(&pinned_step_us, us);
    if (frame_step_data && us) atomic_store_explicit(frame_step_data, frame_step_bits((float)(us / 1e6)), memory_order_release);
    if (!us) fprintf(stderr, "[pla] frame step back to real time\n");
    else fprintf(stderr, "[pla] frame step pinned at %.3f ms (ratio %.3f, diagnostics)%s\n", us / 1000.0, us / 1e6 * 30.0,
                 frame_step_data ? "" : " -- but the real-time patch is off, nothing changes");
}

/* One line every 30 s (PLA_FRAME_STEP_REPORT_SEC overrides, for short test runs): frames, wall seconds, the steps the
   game received, and how much of the frames' time they add up to. A ratio of 1.000 means the game's own step tracks
   wall-clock time, which is the acceptance for the real 60 fps track (docs/next-steps-fps-upscaling-stability.md B2). */
static uint64_t report_frames;
static double report_wall, report_steps;
static uint64_t report_since_ns;
static void frame_step_report(uint64_t now_ns) {
    uint64_t interval_ns = (uint64_t)(30.0 * 1e9);
    const char *e = getenv("PLA_FRAME_STEP_REPORT_SEC");
    if (e && *e) interval_ns = (uint64_t)(atof(e) * 1e9);
    if (!report_since_ns) { report_since_ns = now_ns; return; }
    if (now_ns - report_since_ns < interval_ns) return;
    double fps = report_wall > 0 ? report_frames / report_wall : 0;
    fprintf(stderr, "[pla] real-time frame step: %llu frames in %.1f s (%.2f fps), the game's steps add up to %.2f s (%.4f of the frames' time); mean step %.3f ms\n",
            (unsigned long long)report_frames, report_wall, fps, report_steps, report_wall > 0 ? report_steps / report_wall : 0,
            report_frames ? report_steps * 1000.0 / report_frames : 0);
    report_frames = 0;
    report_wall = report_steps = 0;
    report_since_ns = now_ns;
}

void runtime_frame_step_update(uint64_t now_ns) {
    if (!frame_step_data || !now_ns) return;
    frame_step_report(now_ns);
    uint32_t pinned = atomic_load_explicit(&pinned_step_us, memory_order_relaxed);
    if (pinned) {   /* diagnostics: keep the game's word at the pinned length and report it like real time */
        double step = pinned / 1e6;
        atomic_store_explicit(frame_step_data, frame_step_bits((float)step), memory_order_release);
        if (last_present_ns) {
            double dt = (double)(now_ns - last_present_ns) * 1e-9;
            if (dt > 0.0001 && dt < 0.25) { report_frames++; report_wall += dt; report_steps += step; }
        }
        last_present_ns = now_ns;
        return;
    }
    if (!last_present_ns) { last_present_ns = now_ns; return; }
    double dt = (double)(now_ns - last_present_ns) * 1e-9;
    last_present_ns = now_ns;
    if (dt > 0.25 || dt < 0.001) { frame_sample_count = frame_sample_next = 0; return; }
    if (dt < 1.0 / 120.0) dt = 1.0 / 120.0;
    if (dt > 1.0 / 15.0) dt = 1.0 / 15.0;
    frame_samples[frame_sample_next++ % 5] = dt;
    if (frame_sample_count < 5) frame_sample_count++;
    double sum = 0;
    for (unsigned i = 0; i < frame_sample_count; i++) sum += frame_samples[i];
    double step = sum / frame_sample_count;
    atomic_store_explicit(frame_step_data, frame_step_bits((float)step), memory_order_release);
    report_frames++;
    report_wall += dt;
    report_steps += step;
}

/* The TotK host hooks the shared graphics layer and the input keys call keep their names and answer "off". */
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
/* Key 3: cycle the picture. PLA's canvas and output size are baked into the game (1920x1080, no patch changes them), so
   the picture option this port has is the present: a 1:1 blit, or the MetalFX output scaler that draws the 1080p picture
   at the window size (a 2K/4K display) instead of stretching it bilinearly. The HUD reads the same state. */
int runtime_output_mode_cycle(void) {
    extern int mfxup_output_enabled(void);
    extern void dm_gfx_set_flag(const char *name, int value);
    int on = mfxup_output_enabled();
    dm_gfx_set_flag("mfx_output", on ? 0 : 1);
    dm_gfx_set_flag("mfx_upscale", on ? 0 : 1);   /* the present gate requires both (see nvn_mtl.m mfxup_output); the scene-stage upscale stays idle while the canvas equals the render size */
    fprintf(stderr, "[pla] key 3: 画质：%s\n", on ? "关闭 MetalFX 超分（1:1 输出）" : "MetalFX 超分（1080p -> 窗口）");
    return 1;
}
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
