/* Super Mario Bros. Wonder 1.0.0 and 1.2.1 (GAME_WONDER, WONDER_V121 in runtime/game_version.h; this folder's Makefile builds
   it in place of the TOTK host's game_patches.c): the real-time frame step and exclusive loads with acquire. The game
   runs at its own output size; the output-size and aspect hooks the frontend calls are inert until sites are found for it. */
#include "game_patches.h"
#include <stdatomic.h>
#include "game_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Real-time frame step (TOTK_REALTIME=0: as shipped, held at 60 fps). The game's VFR (pointer main+0x36427b0) keeps a frame
   ratio at +8 (the frame's length in 60 fps frames) and a frame time at +0xc; the per-core steps (+0, +0x10 + 0x28 x core:
   ratio and delta time) are derived from the ratio and the time speed. The per-frame update (two branches, main+0xc9528 and
   main+0xc9588) sets them from the present interval: ratio = interval, time = interval / 60, so the game ran at the
   frame rate's speed (half at 30 fps, twice at 120). Both branches now load the ratio and the time from the loader's data
   page after main, which the host refreshes at each present with the measured frame time (low float: seconds x 60, high
   float: seconds):
     ldr s0, [x22, #0x260] -> adrp x8, <page>       mov w8, #60.0 -> ldr s0, [x8]
     mov x9, xzr           -> ldr s1, [x8, #4]      fmov s1, w8   -> mov x9, xzr
     scvtf s0, s0          -> nop                   fdiv s1, ..   -> nop
   (x8 is set again by the next instruction, add x8, x20, #0x88.)
   1.2.1 (VFR main+0x37cf340, branches main+0xc5a28 / main+0xc5a8c) has the same block with an adrp x11 after the constant,
   which stays where it is (its page is PC-relative) and is used after the block:
     ldr s0, [x24, #0x260] -> adrp x8, <page>       adrp x11, ..     (kept)        mov x9, xzr (kept), add x8 (kept),
     mov w8, #60.0         -> ldr s0, [x8]          fmov s1, w8   -> ldr s1, [x8, #4]   mov w10 (kept), scvtf / fdiv -> nop */
static _Atomic uint64_t *frame_step_data;
static uint8_t *frame_step_main;
static uint64_t last_present_ns, report_ns, report_frames;
static double frame_samples[5], report_seconds, report_game;
static unsigned frame_sample_count, frame_sample_next;
static _Atomic uint32_t pinned_step_us;
enum { VFR_RATIO = 8 };
/* Per version: the VFR pointer, the two branches and their original words; map gives each replacement word: an original
   word by index, or ADRP8 / LDR_RATIO / LDR_SECONDS / NOP. */
enum { ADRP8 = -1, LDR_RATIO = -2, LDR_SECONDS = -3, NOP = -4, SITE_WORDS = 9 };
#ifdef WONDER_V121
enum { VFR_INSTANCE = 0x37cf340 };
static const uint64_t branch[2] = {0xc5a28, 0xc5a8c};
static const unsigned site_words = 9;
static const uint32_t original[SITE_WORDS] = {0xbd426300, 0x52a84e08, 0xb001514b, 0x1e270101, 0xaa1f03e9, 0x91022288, 0x52aff80a, 0x5e21d800, 0x1e211801};
static const int map[SITE_WORDS] = {ADRP8, LDR_RATIO, 2, LDR_SECONDS, 4, 5, 6, NOP, NOP};
#else
enum { VFR_INSTANCE = 0x36427b0 };
static const uint64_t branch[2] = {0xc9528, 0xc9588};
static const unsigned site_words = 8;
static const uint32_t original[SITE_WORDS] = {0xbd4262c0, 0x52a84e08, 0xaa1f03e9, 0x1e270101, 0x91022288, 0x52aff80a, 0x5e21d800, 0x1e211801};
static const int map[SITE_WORDS] = {ADRP8, LDR_RATIO, LDR_SECONDS, 2, 4, 5, NOP, NOP};
#endif

static int realtime_enabled(void) { const char *e = getenv("TOTK_REALTIME"); return !(e && !strcmp(e, "0")); }
static uint64_t frame_step_bits(float seconds) {
    float ratio = seconds * 60.0f; uint32_t lo, hi;
    memcpy(&lo, &ratio, 4); memcpy(&hi, &seconds, 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
/* Diagnostics (TOTK_INPUT_FILE "framestep <ms>"): pin the frame step to a fixed length, 0 back to real time. */
void runtime_frame_step_pin(double ms) {
    uint32_t us = ms > 0 ? (uint32_t)(ms * 1000.0 + 0.5) : 0;
    atomic_store(&pinned_step_us, us);
    if (frame_step_data && us) atomic_store_explicit(frame_step_data, frame_step_bits((float)(us / 1e6)), memory_order_release);
    if (!us) fprintf(stderr, "[wonder] frame step back to real time\n");
    else fprintf(stderr, "[wonder] frame step pinned at %.3f ms (ratio %.3f, diagnostics)%s\n", us / 1000.0, us / 1e6 * 60.0,
                 frame_step_data ? "" : " -- but the real-time patch is off, nothing changes");
}
/* Every 30 s one line: frames and their real time, the game time the steps added up to and the ratio the game holds. */
static void frame_step_report(uint64_t now_ns) {
    if (!report_ns) { report_ns = now_ns; return; }
    if (now_ns - report_ns < 30000000000ull) return;
    float ratio = -1;
    uint64_t vfr = *(const uint64_t *)(const void *)(frame_step_main + VFR_INSTANCE);
    if (vfr) memcpy(&ratio, (const void *)(uintptr_t)(vfr + VFR_RATIO), 4);
    double span = (double)(now_ns - report_ns) * 1e-9;
    fprintf(stderr, "[wonder] real-time frame step: %llu frames in %.1f s (%.1f fps), steps add up to %.2f s (%.4f of the frames' time); the game's VFR ratio %.3f\n",
            (unsigned long long)report_frames, span, report_frames / span, report_game, report_seconds > 0 ? report_game / report_seconds : 0, ratio);
    report_ns = now_ns; report_frames = 0; report_seconds = report_game = 0;
}
/* As Breath of the Wild (game_patches_botw.c): the intervals are averaged over five frames and only an average above
   1/15 s is cut, so the steps add up to the real time at any frame rate and a hitch is spread over the next frames. */
void runtime_frame_step_update(uint64_t now_ns) {
    if (!frame_step_data || !now_ns) return;
    frame_step_report(now_ns);
    uint64_t prev = last_present_ns;
    last_present_ns = now_ns;
    if (!prev) return;
    double dt = (double)(now_ns - prev) * 1e-9;
    if (dt > 0.25 || dt < 0.001) { frame_sample_count = frame_sample_next = 0; return; }
    report_frames++; report_seconds += dt;
    if (atomic_load_explicit(&pinned_step_us, memory_order_relaxed)) return;
    frame_samples[frame_sample_next++ % 5] = dt;
    if (frame_sample_count < 5) frame_sample_count++;
    double sum = 0;
    for (unsigned i = 0; i < frame_sample_count; i++) sum += frame_samples[i];
    double step = sum / frame_sample_count;
    if (step > 1.0 / 15.0) step = 1.0 / 15.0;
    report_game += step;
    atomic_store_explicit(frame_step_data, frame_step_bits((float)step), memory_order_release);
}
static uint32_t adrp_reg(unsigned rd, uint64_t site, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(site >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("real-time frame-step data outside ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) | (rd & 31u);
}
static void patch_realtime(Module *m) {
    uint64_t data_off = (m->file_size + 0x3fff) & ~0x3fffULL;
    if (data_off + 0x4000 != m->size || data_off & 0xfff) runtime_fail("real-time frame-step page layout");
    for (int b = 0; b < 2; b++) for (unsigned i = 0; i < site_words; i++) {
        uint32_t w; memcpy(&w, m->base + branch[b] + 4 * i, 4);
        if (w != original[i]) {
            fprintf(stderr, "[wonder] real-time frame step main+%#llx: found %08x expected %08x\n", (unsigned long long)(branch[b] + 4 * i), w, original[i]);
            runtime_fail("real-time frame step requires matching game-version original instructions");
        }
    }
    for (int b = 0; b < 2; b++) {
        uint32_t repl[SITE_WORDS];
        for (unsigned i = 0; i < site_words; i++)
            repl[i] = map[i] == ADRP8 ? adrp_reg(8, (uintptr_t)m->base + branch[b] + 4 * i, (uintptr_t)m->base + data_off) :
                      map[i] == LDR_RATIO ? 0xbd400100u :     /* ldr s0, [x8]     : ratio = seconds x 60 */
                      map[i] == LDR_SECONDS ? 0xbd400501u :   /* ldr s1, [x8, #4] : seconds */
                      map[i] == NOP ? 0xd503201fu : original[map[i]];
        memcpy(m->base + branch[b], repl, 4 * site_words);
    }
    frame_step_main = m->base;
    frame_step_data = (_Atomic uint64_t *)(void *)(m->base + data_off);
    atomic_store(frame_step_data, frame_step_bits(1.0f / 60.0f));
    fprintf(stderr, "[wonder] real-time frame step: %u verified instructions at main+%#llx / main+%#llx; initial 16.67 ms, sampled at each present\n",
            2 * site_words, (unsigned long long)branch[0], (unsigned long long)branch[1]);
}
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }   /* no larger scene size for the heaps (romfs.c) */
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
int runtime_aspect_step(void) { return -1; }   /* no aspect setting: the shortcut list leaves it out */
void runtime_aspect_view(double w, double h) { (void)w; (void)h; }
/* The settings panel's Resolution and Aspect ratio rows (settings_panel.h): neither is offered, so both rows stay out. */
int runtime_output_modes(void) { return 0; }
int runtime_output_mode(void) { return OUTPUT_MODE_OFF; }
int runtime_output_mode_set(int mode) { (void)mode; return 0; }
int runtime_output_native(int mode, unsigned *w, unsigned *h) { (void)mode; (void)w; (void)h; return 0; }
int runtime_aspect_set_ratio(float r) { (void)r; return 0; }
float runtime_aspect_ratio(void) { return 0; }
float runtime_aspect_start(void) { return 0; }

/* Exclusive loads with acquire (TOTK_EXCL_ACQUIRE; from upstream 58ac238, found on Breath of the Wild 1.6.0): sead's spin
   locks take the lock with a plain ldxr / stxr and only put a dmb ish before the release, so nothing orders the loads inside
   the critical section after the lock is taken. The ARMv8 model lets such a load be satisfied before the ldxr observes the
   lock free; the Switch's Cortex-A57 rarely does it, Apple's cores do: two workers read the same "next job" index of a
   sead::JobQueue and run the same job twice, and a later job uses what the first run freed. Every plain ldxr / ldxp that
   opens a real exclusive window (a store-exclusive on the same base register within 16 instructions) gets the acquire bit
   (ldaxr / ldaxp): one bit, the same registers and size, only stronger ordering. */
static int excl_ld(uint32_t w) { return (w & 0x3ffffc00u) == 0x085f7c00u || (w & 0xbfff8000u) == 0x887f0000u; }   /* ldxr{b,h} / ldxp */
static int excl_st(uint32_t w) { return (w & 0x3fe07c00u) == 0x08007c00u || (w & 0xbfe00000u) == 0x88200000u; }   /* st{l}xr{b,h} / st{l}xp */
static unsigned excl_acquire(uint8_t *code, size_t n) {
    unsigned done = 0; uint32_t *w = (uint32_t *)(void *)code; size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        if (!excl_ld(w[i])) continue;
        uint32_t rn = (w[i] >> 5) & 31;
        for (size_t j = i + 1; j < words && j <= i + 16; j++) if (excl_st(w[j]) && ((w[j] >> 5) & 31) == rn) { w[i] |= 0x8000u; done++; break; }
    }
    return done;
}

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("game patches expect the main module");
    if (getenv("TOTK_PCHTXT") || getenv("TOTK_ASPECT") || (getenv("TOTK_OUTPUT_RES") && strcmp(getenv("TOTK_OUTPUT_RES"), "0")))
        fprintf(stderr, "[totk] %s: TOTK_PCHTXT, TOTK_ASPECT and TOTK_OUTPUT_RES are not supported yet; ignored\n", GAME_NAME);
    if (realtime_enabled()) patch_realtime(m);
    else fprintf(stderr, "[wonder] frame step as shipped (TOTK_REALTIME=0): held at 60 fps\n");
    const char *a = getenv("TOTK_EXCL_ACQUIRE");
    if (!a || strcmp(a, "0")) {
        unsigned t = excl_acquire(m->base, m->text_end), p = excl_acquire(m->base + m->patch_off, m->file_size - m->patch_off);
        fprintf(stderr, "[totk] exclusive loads with acquire: %u in the code, %u in the rewriter's patch area (TOTK_EXCL_ACQUIRE=0: as shipped)\n", t, p);
    } else fprintf(stderr, "[totk] exclusive loads as shipped (TOTK_EXCL_ACQUIRE=0)\n");
}

/* With the frame step as shipped (TOTK_REALTIME=0) the game advances one 60 fps step per frame, so the host holds it at
   60 fps (runtime_game_fps_limit, hle_nvn.c); with the real-time step any frame rate keeps its speed. */
int runtime_game_fps_limit(void) { return realtime_enabled() ? 0 : 60; }
