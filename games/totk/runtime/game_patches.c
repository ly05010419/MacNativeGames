#include "game_patches.h"
#include "aspect_patch.h"
#include "game_version.h"
#include "world_survey.h"
#include <TargetConditionals.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <math.h>

/* 60 FPS.
   TOTK's adaptive present-interval controller (main+0x8f6bec, called once per frame from main+0x8f68b4) turns the main
   thread's measured frame work time (system object +0x118, written at main+0x8f6a44 just before the present) into a
   number of 60 Hz ticks, clamps it to the controller's [minimum, maximum] interval and hands the result both to
   nvnWindowSetPresentInterval and to the variable-frame-rate step (main+0x7a83e4: ratio = interval * 0.5, frame time =
   interval / 60). Shipping code uses a minimum of 2 (30 fps), so game speed follows whatever interval is chosen.
   The patch lowers the minimum to 1 (60 fps). The controller still raises the interval to 2 or 3 when a frame's work
   does not fit, with its own hysteresis, so slow scenes fall back to 30 or 20 fps at correct speed.
   Two further sites make the lowering robust: the controller now tries a lower interval when the measured work fits
   it (instead of waiting for a stored candidate that shipping code initialises to the old minimum), and the lowered
   result is floored at 1 so an interval of 0 can never reach the window or the frame-rate step.
   TOTK_FPS60=0 leaves the shipping controller untouched. */
typedef struct { uint64_t offset; uint32_t original, replacement; const char *what; } GamePatch;

static const GamePatch fps60_sites[] = {
    {GAME_ADDR(0x8f6c60), 0x6b08015f, 0x7100055f, "clamp: cmp w10, #1 (was cmp w10, w8 = minimum)"},
    {GAME_ADDR(0x8f6c64), 0x1a89b10a, 0x1a9fa52a, "clamp: csinc w10, w9, wzr, ge (desired = max(desired, 1))"},
    {GAME_ADDR(0x8f6c88), 0xb9401a8a, 0xd503201f, "lower trigger: nop the stored-candidate load (try lower when desired < current)"},
    {GAME_ADDR(0x8f6d18), 0x6b08015f, 0x7100055f, "raise path: cmp w10, #1"},
    {GAME_ADDR(0x8f6d28), 0x1a88c1ac, 0x1a9fc5ac, "raise path: csinc w12, w13, wzr, gt (next lower candidate floors at 1)"},
    {GAME_ADDR(0x8f6d7c), 0xb9400288, 0x52800028, "lower path: mov w8, #1 (minimum interval 1)"},
    {GAME_ADDR(0x8f6d98), 0x2a0a03f5, 0x1a9fa555, "lower path: csinc w21, w10, wzr, ge (result never below 1)"},
};

static int fps60_enabled(void) {
    const char *e = getenv("TOTK_FPS60");
    return !(e && !strcmp(e, "0"));
}

/* One atomic 64-bit value holds ratio (low float) and frame seconds (high
   float). The guest reads the two lanes independently; both always remain
   finite and in range, including if an update falls between the reads. */
static _Atomic uint64_t *frame_step_data;
static uint64_t last_present_ns;
static double frame_samples[5];
static unsigned frame_sample_count, frame_sample_next;

static int realtime_enabled(void) {
    const char *e = getenv("TOTK_REALTIME");
    return !(e && !strcmp(e, "0"));
}

static uint64_t frame_step_bits(float seconds) {
    float ratio = seconds * 30.0f;
    uint32_t lo, hi;
    memcpy(&lo, &ratio, 4);
    memcpy(&hi, &seconds, 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/* Diagnostics (TOTK_INPUT_FILE "framestep <ms>"): pin the frame step to a fixed length, 0 back to real time. 2026-10-17
   this showed the game over "retry" waiting on a step of at least one 30 fps frame. */
static _Atomic uint32_t pinned_step_us;
void runtime_frame_step_pin(double ms) {
    uint32_t us = ms > 0 ? (uint32_t)(ms * 1000.0 + 0.5) : 0;
    atomic_store(&pinned_step_us, us);
    if (frame_step_data && us) atomic_store_explicit(frame_step_data, frame_step_bits((float)(us / 1e6)), memory_order_release);
    if (!us) fprintf(stderr, "[totk] frame step back to real time\n");
    else fprintf(stderr, "[totk] frame step pinned at %.3f ms (ratio %.3f, diagnostics)%s\n", us / 1000.0, us / 1e6 * 30.0,
                 frame_step_data ? "" : " -- but the real-time patch is off, nothing changes");
}
void runtime_frame_step_update(uint64_t now_ns) {
    if (!frame_step_data || !now_ns) return;
    if (atomic_load_explicit(&pinned_step_us, memory_order_relaxed)) { last_present_ns = now_ns; return; }
    if (!last_present_ns) { last_present_ns = now_ns; return; }
    double dt = (double)(now_ns - last_present_ns) * 1e-9;
    last_present_ns = now_ns;
    if (dt > 0.25 || dt < 0.001) { frame_sample_count = frame_sample_next = 0; return; }
    if (dt > 1.0 / 15.0) dt = 1.0 / 15.0;
    frame_samples[frame_sample_next++ % 5] = dt;
    if (frame_sample_count < 5) frame_sample_count++;
    double sum = 0;
    for (unsigned i = 0; i < frame_sample_count; i++) sum += frame_samples[i];
    atomic_store_explicit(frame_step_data, frame_step_bits((float)(sum / frame_sample_count)), memory_order_release);
}

static uint32_t adrp_reg(unsigned rd, uint64_t site, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(site >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("real-time frame-step data outside ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) | (rd & 31u);
}
static uint32_t adrp_x9(uint64_t site, uint64_t target) { return adrp_reg(9, site, target); }
static uint32_t branch_to(uint64_t site, uint64_t target) {   /* b <target>, both offsets in main */
    int64_t delta = (int64_t)target - (int64_t)site;
    if ((delta & 3) || delta < -(1 << 27) || delta >= (1 << 27)) runtime_fail("patch branch outside range");
    return 0x14000000u | ((uint32_t)(delta >> 2) & 0x3ffffffu);
}

/* iOS cannot write code pages at run time: tools/export (TOTK_EXPORT_IOS, runtime/loader.c) runs these same patches on the
   Mac and the patched text is embedded in the app. On the device the loader only checks that the patched words are there. */
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
        fprintf(stderr, "[totk] %s main+%#llx: embedded %08x, expected patched %08x\n", what, (unsigned long long)off, have, word);
        runtime_fail("embedded main text was not patched at build time (ios/tools/build_embed.sh)");
    }
}

/* Hair and cloth (PR #79 for 1.4.3, game_patches_v143.c patch_secondary_animation): the actor animation update builds a
   second clock {seconds = factor / 30, ratio = factor} every rendered frame, so above 30 FPS hair and cloth swing too fast
   (4x at 120 FPS). The ordinary animation clock already follows the real-time step. The older compiler inlines the update
   in three places and calls the shared update (1.2.1 main+0x89822c) from one more; its other caller already passes
   measured time and is left alone. Each inline place gets two veneers: at the fmul, seconds = measured seconds * factor;
   at the store of the factor into the clock's ratio slot, ratio = measured 30 Hz ratio * factor (the ratio is stored
   before the fmul in some places and after it in others). The call gets PR #79's veneer: s2 / s3 *= measured ratio.
   The veneers read the real-time page (ratio at +0, seconds at +4) and save the registers they use on the stack.
   Text padding +160 (after the bounce veneer). TOTK_CLOTH_REALTIME=0 keeps the original clock (Mac). */
/* One inline place: fmul s<r>, s<f>, s<r> (s<r> = 1/30, s<f> = the time factor) at fmul; the factor goes into the ratio
   slot at store, str s<f>, [sp, #off] or (pair) stp s<r>, s<f>, [x29, #off]. */
typedef struct { uint64_t fmul, store; unsigned r, f; int off; int pair; } ClothInline;
#if defined(TOTK_GAME_V121)
static const ClothInline cloth_inline[3] = {{0x82192c, 0x821928, 0, 8, 0x124, 0}, {0x822a64, 0x822a40, 0, 8, 0x124, 0},
                                            {0x83182c, 0x831830, 1, 0, 0x134, 0}};
static const uint64_t cloth_call = 0x8222f0, cloth_update = 0x89822c;
#elif defined(TOTK_GAME_V112)
static const ClothInline cloth_inline[3] = {{0x9d1e64, 0x9d1e74, 0, 8, 0x104, 0}, {0x9d2d34, 0x9d2d2c, 0, 8, 0x104, 0},
                                            {0x9e7648, 0x9e764c, 1, 0, 0x134, 0}};
static const uint64_t cloth_call = 0x9d2774, cloth_update = 0x88a998;
#else
static const ClothInline cloth_inline[3] = {{0x7b5988, 0x7b598c, 0, 8, -0x40, 1}, {0x7b6b28, 0x7b6b2c, 0, 8, -0x40, 1},
                                            {0x7f5ec4, 0x7f5ed0, 1, 0, 0x114, 0}};
static const uint64_t cloth_call = 0x7b6358, cloth_update = 0x82c224;
#endif
static uint32_t stp_x29(unsigned rt, unsigned rt2, int off) { return 0x2d000000u | ((uint32_t)(off / 4) & 0x7f) << 15 | rt2 << 10 | 29u << 5 | rt; }
static uint32_t str_sp(unsigned rt, unsigned off) { return 0xbd000000u | (off / 4) << 10 | 31u << 5 | rt; }
static uint64_t cloth_put(Module *m, uint64_t at, const uint32_t *code, unsigned n) {
    if (!CODE_PATCHED_AT_BUILD) for (unsigned i = 0; i < 4 * n; i++) if (m->base[at + i]) runtime_fail("cloth veneer padding not empty");
    for (unsigned i = 0; i < n; i++) put_word(m, at + 4 * i, code[i], "cloth/hair clock veneer");
    return at + 4 * n;
}
static void cloth_site(Module *m, uint64_t site, uint32_t original, uint32_t replacement) {
    if (!CODE_PATCHED_AT_BUILD) {
        uint32_t w; memcpy(&w, m->base + site, 4);
        if (w != original) {
            fprintf(stderr, "[totk] cloth/hair clock main+%#llx: found %08x expected %08x\n", (unsigned long long)site, w, original);
            runtime_fail("cloth/hair clock: original instruction mismatch");
        }
    }
    put_word(m, site, replacement, "cloth/hair clock");
}
static void patch_cloth_clock(Module *m, uint64_t data_off) {
    const char *e = getenv("TOTK_CLOTH_REALTIME");
    if (!CODE_PATCHED_AT_BUILD && e && !strcmp(e, "0")) return;
    const uint64_t base = ((m->text_end + 15) & ~15ULL) + 160, end = (m->text_end + 0xfff) & ~0xfffULL;
    const uintptr_t mb = (uintptr_t)m->base;
    uint64_t at = base;
    for (unsigned k = 0; k < 3; k++) {
        const ClothInline *c = &cloth_inline[k];
        const unsigned r = c->r, f = c->f;
        /* seconds: s<r> = measured seconds * s<f> */
        const uint32_t sec[6] = {0xf81f0fe9,                            /* str x9, [sp, #-16]! */
                                 adrp_reg(9, mb + at + 4, mb + data_off),
                                 0xbd400520u | r,                       /* ldr s<r>, [x9, #4] */
                                 0x1e200800u | r << 16 | f << 5 | r,    /* fmul s<r>, s<f>, s<r> */
                                 0xf84107e9,                            /* ldr x9, [sp], #16 */
                                 branch_to(at + 20, c->fmul + 4)};
        cloth_site(m, c->fmul, 0x1e200800u | r << 16 | f << 5 | r, branch_to(c->fmul, at));
        at = cloth_put(m, at, sec, 6);
        /* ratio: measured 30 Hz ratio * s<f> into the slot (s31 and x9 saved; sp moves by 32 for sp-relative slots) */
        const uint32_t original = c->pair ? stp_x29(r, f, c->off) : str_sp(f, (unsigned)c->off);
        const uint32_t store = c->pair ? stp_x29(r, 31, c->off) : str_sp(31, (unsigned)c->off + 32);
        const uint32_t rat[11] = {0xd10083ff,                           /* sub sp, sp, #32 */
                                  0xf90003e9,                           /* str x9, [sp] */
                                  0x3d8007ff,                           /* str q31, [sp, #16] */
                                  adrp_reg(9, mb + at + 12, mb + data_off),
                                  0xbd40013f,                           /* ldr s31, [x9] */
                                  0x1e3f0800u | f << 5 | 31u,           /* fmul s31, s<f>, s31 */
                                  store,
                                  0x3dc007ff,                           /* ldr q31, [sp, #16] */
                                  0xf94003e9,                           /* ldr x9, [sp] */
                                  0x910083ff,                           /* add sp, sp, #32 */
                                  branch_to(at + 40, c->store + 4)};
        cloth_site(m, c->store, original, branch_to(c->store, at));
        at = cloth_put(m, at, rat, 11);
    }
    /* the call: s2 = factor / 30, s3 = factor; x8 and s4 are caller-saved and not arguments of the update */
    const uint32_t call[5] = {adrp_reg(8, mb + at, mb + data_off), 0xbd400104 /* ldr s4, [x8] */,
                              0x1e240842 /* fmul s2, s2, s4 */, 0x1e240863 /* fmul s3, s3, s4 */, branch_to(at + 16, cloth_update)};
    cloth_site(m, cloth_call, branch_to(cloth_call, cloth_update) | 0x80000000u, branch_to(cloth_call, at) | 0x80000000u);
    at = cloth_put(m, at, call, 5);
    if (at > end) runtime_fail("cloth/hair clock veneers reach read-only data");
    fprintf(stderr, "[totk] %s cloth/hair clock: real-time seconds and ratio at 3 inline places and 1 call, veneers +%#llx..+%#llx\n",
            TOTK_GAME_VERSION, (unsigned long long)base, (unsigned long long)at);
}

/* 1.0.0: pre-grab Ultrahand targeting calls the shared sensitivity getter at
   main+0x1ce78e8, then uses its result for vertical stick movement without dt.
   Runtime counters confirmed this call in targeting, but not ordinary control.
   The UltraHand object camera at +0x1ceb7a8 is a different path: leave it alone.
   Scale only this call's result by the measured 30 Hz ratio. The shared getter,
   horizontal control and input mapping remain unchanged. Mac/v0 only.
   TOTK_ULTRAHAND_REALTIME=0 disables this correction for A/B comparisons. */
static void patch_ultrahand_vertical(Module *m, uint64_t data_off) {
#if !TARGET_OS_IPHONE && !defined(TOTK_GAME_V112) && !defined(TOTK_GAME_V121)
    const char *e = getenv("TOTK_ULTRAHAND_REALTIME");
    if (e && !strcmp(e, "0")) return;
    const uint64_t site = 0x1ce78e8, getter = 0x1cd3678;
    const uint64_t stub = ((m->text_end + 15) & ~15ULL) + 400;
    const uint64_t end = (m->text_end + 0xfff) & ~0xfffULL;
    const uintptr_t mb = (uintptr_t)m->base;
    if (site + 8 > m->text_end || getter + 8 > m->text_end || stub + 44 > end || end > m->size)
        runtime_fail("Ultrahand vertical patch layout");
    const struct { uint64_t off; uint32_t word; } guards[] = {
        {site, branch_to(site, getter) | 0x80000000u},
        {site + 4, 0xaa1303e0}, /* mov x0, x19 */
        {getter, 0xbd4c2c00},   /* ldr s0, [x0, #0xc2c] */
        {getter + 4, 0xd65f03c0}
    };
    for (unsigned i = 0; i < sizeof guards / sizeof *guards; i++) {
        uint32_t word; memcpy(&word, m->base + guards[i].off, 4);
        if (word != guards[i].word) runtime_fail("Ultrahand vertical: original instruction mismatch");
    }
    for (unsigned i = 0; i < 44; i++)
        if (m->base[stub + i]) runtime_fail("Ultrahand vertical: veneer padding not empty");
    const uint32_t code[] = {
        0xbd4c2c00,              /* ldr s0, [x0, #0xc2c] (original getter) */
        0xd10083ff,              /* sub sp, sp, #32 */
        0xf90003e9,              /* str x9, [sp] */
        0x3d8007ff,              /* str q31, [sp, #16] */
        adrp_reg(9, mb + stub + 16, mb + data_off),
        0xbd40013f,              /* ldr s31, [x9] */
        0x1e3f0800,              /* fmul s0, s0, s31 */
        0x3dc007ff,              /* ldr q31, [sp, #16] */
        0xf94003e9,              /* ldr x9, [sp] */
        0x910083ff,              /* add sp, sp, #32 */
        0xd65f03c0               /* ret (the call site's original link) */
    };
    for (unsigned i = 0; i < sizeof code / sizeof *code; i++)
        put_word(m, stub + 4 * i, code[i], "Ultrahand vertical veneer");
    put_word(m, site, branch_to(site, stub) | 0x80000000u, "Ultrahand vertical call");
    fprintf(stderr, "[totk] 1.0.0 Ultrahand vertical stick: real-time ratio at main+0x1ce78e8 (30 Hz baseline)\n");
#else
    (void)m; (void)data_off;
#endif
}

static void patch_realtime(Module *m) {
    /* The loader reserved a writable page immediately after main's image.
       No game allocation or rewritten-code veneer can overlap this page. */
    uint64_t data_off = (m->file_size + 0x3fff) & ~0x3fffULL;
    if (data_off + 0x4000 != m->size || data_off & 0xfff) runtime_fail("real-time frame-step page layout");
    static const struct { uint64_t off; uint32_t original; } sites[] = {
        {GAME_ADDR(0x7a83f4),0x1e220020}, {GAME_ADDR(0x7a83f8),0x52a84e08}, {GAME_ADDR(0x7a83fc),0x1e270101},
        {GAME_ADDR(0x7a8400),0x1e2c1002}, {GAME_ADDR(0x7a840c),0x1e211801}, {GAME_ADDR(0x7a8410),0x1e220800},
        {GAME_ADDR(0x8f68b8),0xbd425e60}, {GAME_ADDR(0x8f68bc),0x1e2c1001},
        {GAME_ADDR(0x8f68cc),0x5e21d800}, {GAME_ADDR(0x8f68d0),0x1e210800}
    };
    for (size_t i = 0; i < sizeof sites / sizeof *sites; i++) {
        uint32_t word = 0;
        if (sites[i].off + 4 > m->text_end) runtime_fail("real-time frame-step site outside main text");
        memcpy(&word, m->base + sites[i].off, 4);
        if (CODE_PATCHED_AT_BUILD) break;   /* checked against the patched words below */
        if (word != sites[i].original) {
            fprintf(stderr,"[totk] real-time frame-step main+%#llx: found %08x expected %08x\n",
                    (unsigned long long)sites[i].off, word, sites[i].original);
            runtime_fail("real-time frame-step requires matching game-version original instructions");
        }
    }
    const uint32_t nop = 0xd503201f;
    uint32_t repl[] = {
        adrp_x9((uintptr_t)m->base + GAME_ADDR(0x7a83f4), (uintptr_t)m->base + data_off),
        0xbd400120, /* ldr s0, [x9]       : ratio = dt * 30 */
        0xbd400521, /* ldr s1, [x9, #4]   : dt in seconds  */
        nop, nop, nop,
        adrp_x9((uintptr_t)m->base + GAME_ADDR(0x8f68b8), (uintptr_t)m->base + data_off),
        0xbd400120, /* global ratio, same shared value */
        nop, nop
    };
    for (size_t i = 0; i < sizeof sites / sizeof *sites; i++)
        put_word(m, sites[i].off, repl[i], "real-time frame step");
    frame_step_data = (_Atomic uint64_t *)(void *)(m->base + data_off);
    atomic_store(frame_step_data, frame_step_bits(1.0f / 60.0f));
    patch_cloth_clock(m, data_off);
    patch_ultrahand_vertical(m, data_off);
    fprintf(stderr,"[totk] real-time frame step: %zu verified instructions; initial 16.67 ms, sampled at each present\n",
            sizeof sites / sizeof *sites);
}

/* Render resolution (TOTK_RENDER_RES=<w>x<h>, e.g. 960x540).
   main+0xc63064 reads the scene render size {w, h} from the game's parameter tree (main+0xc6325c: handheld 1280x720,
   docked 1600x900), converts it to floats and stores it in the renderer at +0xcc4 / +0xcc8; every scene render target
   is sized from there (traced with TOTK_SIZE_CHAIN: the per-frame render-target requests at main+0xa2818c take their
   size from this view). The community 1008p / 1080p exefs patches change the same place. Here the size comes from the
   loader's data page after main (the real-time frame-step page, +16): three instructions,
     main+0xc630bc  scvtf s0, s8              -> adrp x9, <page>
     main+0xc630c4  str s0, [x19, #0xcc4]     -> ldp s0, s1, [x9, #16]
     main+0xc630c8  scvtf s1, s9              -> str s0, [x19, #0xcc4]
   and the store of s1 at main+0xc630d8 stays. The size read from the parameter is then not used. The host writes the
   page at load, so on the phone (code patched at build) the size is still chosen at run time: TOTK_RENDER_RES, or the
   game's handheld 1280x720 when unset / 0 (the phone always reports handheld mode). The game's final passes scale the
   scene to its output (1280x720 handheld). */
static const GamePatch render_res_sites[3] = {
    {GAME_ADDR(0xc630bc), 0x5e21d900, 0, "scvtf s0, s8 -> adrp x9, <data page>"},
    {GAME_ADDR(0xc630c4), 0xbd0cc660, 0x2d420520, "str s0, [x19, #0xcc4] -> ldp s0, s1, [x9, #16]"},
    {GAME_ADDR(0xc630c8), 0x5e21d921, 0xbd0cc660, "scvtf s1, s9 -> str s0, [x19, #0xcc4]"},
};
static int render_res(unsigned *w, unsigned *h) {
    const char *e = getenv("TOTK_RENDER_RES");
    if (!e || !*e || !strcmp(e, "0")) return 0;
    if (sscanf(e, "%ux%u", w, h) != 2 || *w < 64 || *h < 64 || *w > 4096 || *h > 4096) runtime_fail("TOTK_RENDER_RES must be <width>x<height>");
    return 1;
}
static void patch_render_res(Module *m) {
    uint64_t data_off = (m->file_size + 0x3fff) & ~0x3fffULL;
    unsigned w = 1280, h = 720; int set = render_res(&w, &h);
    if (!CODE_PATCHED_AT_BUILD && !set) return;   /* Mac: the game's own size unless asked */
    uint32_t want[3] = {adrp_x9((uintptr_t)m->base + render_res_sites[0].offset, (uintptr_t)m->base + data_off),
                        render_res_sites[1].replacement, render_res_sites[2].replacement};
    if (CODE_PATCHED_AT_BUILD) {
        uint32_t have; memcpy(&have, m->base + render_res_sites[0].offset, 4);
        if (have == render_res_sites[0].original) { fprintf(stderr, "[totk] render resolution: the game's own (not patched at build)\n"); return; }
        for (int i = 0; i < 3; i++) put_word(m, render_res_sites[i].offset, want[i], "render resolution");   /* checks */
    } else {
        for (int i = 0; i < 3; i++) {
            uint32_t word; memcpy(&word, m->base + render_res_sites[i].offset, 4);
            if (word != render_res_sites[i].original) {
                fprintf(stderr, "[totk] render resolution patch main+%#llx: found %08x, expected %08x (%s)\n",
                        (unsigned long long)render_res_sites[i].offset, word, render_res_sites[i].original, render_res_sites[i].what);
                runtime_fail("render resolution patch: main does not match the selected game version");
            }
        }
        for (int i = 0; i < 3; i++) memcpy(m->base + render_res_sites[i].offset, &want[i], 4);
    }
    float v[2] = {(float)w, (float)h};
    memcpy(m->base + data_off + 16, v, sizeof v);
    fprintf(stderr, "[totk] render resolution patch: scene render size %ux%u%s (3 instructions at main+0xc630bc, value on the data page)\n",
            w, h, set ? "" : " (the game's handheld size; TOTK_RENDER_RES not set)");
}

/* Output resolution ("true 2K", 2026-10-17; key 3 switches it while playing). Two sizes are 1920x1080 in the game:
   1. The swapchain. nnMain writes the framework's display size as a constant before creating the graphics framework
      ("NvInitializeGraphics"); the framework's system object keeps it at +0x9c / +0xa0 and creates the swapchain from it
      (main+0xe9d8c0). It is made the largest output size (2560x1440), fixed for the run:
        main+0xe996ac  mov  x8, #0x780            (1920)   -> mov  x8, #w
        main+0xe996b0  movk x8, #0x438, lsl #32   (1080)   -> movk x8, #h, lsl #32
   2. The output size of the graphics parameters (docked and handheld entries, with inheritance: +0x38 of the entry, getter
      main+0xc632b0; +0x30 is the scene render size, 1600x900 docked). main+0xc63064 reads it for the current mode and gives it
      to the window crop and to the renderer's display slot 0 (main+0xc631e0 -> main+0xc6322c, renderer +0x388); the canvas the
      game composes the frame in is that slot times the display scale (main+0x93b9c4, scale 1.0), and the final copy to the
      swapchain draws that size. It is read from the loader's data page instead (+32, {w, h}), and the renderer the routine
      works on is stored next to it (+40) so the host can ask for a new layout (the getter has no side effects):
        main+0xc630a4  mov  x0, x20               -> adrp x21, <data page>
        main+0xc630a8  bl   main+0xc632b0         -> str  x19, [x21, #40]   (x19 = the renderer)
        main+0xc630ac  ldr  x21, [x0]             -> ldr  x21, [x21, #32]   (the output size)
   Switching while playing (key 3, runtime_output_res_toggle): main+0x1185358 (called from main+0x1185310, an event handler
   the renderer registers by name hash at main+0xec86c8; also main+0xc5fd00) re-runs main+0xc63064 when the operation mode
   changed or bit 0x200 of the renderer's +0xca4 is set, then clears the bit and flags the displays for rebuilding (+0xca0
   bit 0) -- the path a Switch takes when it is docked or undocked. The host writes the other size to the data page, sets
   that bit and posts the "operation mode changed" notification (oe message 30; the mode itself stays docked) so that the
   handler runs; the game lays out its crop, canvas and UI again at the new size. At 1920x1080 the picture fills the
   swapchain's top-left (the crop says so) and the present scales it to the window (MetalFX when the window is bigger: the
   2K output). The scene keeps its render size either way; its final upscale (MetalFX on the Mac, shader a3a570eb) writes
   the canvas.
   TOTK_OUTPUT_RES: auto (the Mac default) = a 2560x1440 swapchain and heaps so that key 3 can switch at any time, starting at
   1920x1080 (the user's choice, 2026-10-17: "default 1080p"; needs the HeapDef overlay built by make romfs-patches; without
   it the game's own 1920x1080, no switching); <w>x<h> = that largest size, starting there (TOTK_OUTPUT_RES=2560x1440 starts
   in true 2K); 0 = the game's own 1920x1080, no switching. iOS and the iOS export (TOTK_EXPORT_IOS) keep the game's own size. Decided once;
   every caller sees the same answer. Mac only. */
static int output_full_at_start = 1;
static int output_res_auto(unsigned *w, unsigned *h) {
#if TARGET_OS_OSX
    if (getenv("TOTK_EXPORT_IOS")) return 0;
    const char *dir = getenv("TOTK_ROMFS_PATCHES_OUT"); char path[4096];
    snprintf(path, sizeof path, "%s/2560x1440/manifest.tsv", dir ? dir : "extracted/" TOTK_GAME_DIR "/romfs-patches-out");
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[totk] true 2K (TOTK_OUTPUT_RES=auto): off, %s is missing (make romfs-patches)\n", path); return 0; }
    fclose(f);
    *w = 2560; *h = 1440; output_full_at_start = 0;
    fprintf(stderr, "[totk] true 2K (TOTK_OUTPUT_RES=auto): starts at 1920x1080 (the default); key 3 switches between 1920x1080 and 2560x1440\n");
    return 1;
#else
    (void)w; (void)h; return 0;
#endif
}
static int output_res(unsigned *w, unsigned *h) {
    static int decided, on; static unsigned cw, ch;
    if (!decided) {
        const char *e = getenv("TOTK_OUTPUT_RES");
        /* iOS shows 720p only (the user, 2026-10-18), auto or not; the iOS export (TOTK_EXPORT_IOS) makes the phone's
           embedded code on the Mac, so it follows the same rule. */
        if (TARGET_OS_IPHONE || getenv("TOTK_EXPORT_IOS")) {
            on = 0;
            if (e && *e && strcmp(e, "0") && strcmp(e, "auto")) fprintf(stderr, "[totk] TOTK_OUTPUT_RES=%s ignored: iOS shows 720p only\n", e);
        } else if (e && !strcmp(e, "0")) on = 0;
        else if (!e || !*e || !strcmp(e, "auto")) on = output_res_auto(&cw, &ch);
        else if (sscanf(e, "%ux%u", &cw, &ch) != 2 || cw < 640 || ch < 360 || cw > 7680 || ch > 4320) runtime_fail("TOTK_OUTPUT_RES must be <width>x<height> (640x360 .. 7680x4320), auto or 0");
        else on = 1;
        decided = 1;
    }
    if (on) { *w = cw; *h = ch; }
    return on;
}
int runtime_output_res(unsigned *w, unsigned *h) { return !CODE_PATCHED_AT_BUILD && output_res(w, h); }
/* Only 1.4.3 counts a bigger scene in its heaps (runtime_output_res_extra_bytes): no scene overlay here. */
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
/* The canvas-sized render targets come from agl::utl::DynamicTextureAllocator (initialised once, main+0x8e1a98): a single
   heap of the HeapDef size "DynamicTextureAllocatorMem" (113664 KiB), carved from the "Graphics" heap (201728 KiB, the heap
   of gsys::SystemTask), with no parent to grow from. At 2560x1440 the title runs out of it (the allocator main+0x798934
   then dereferences the null block, main+0x799390). The HeapDef overlay for the output size (scripts/make_output_res_overlay.py,
   loaded by runtime/romfs.c) scales DynamicTextureAllocatorMem with the output pixels, rounded up to whole MiB, and grows
   Graphics by the same amount; the application pool grows by it too (rounded up to 2 MiB), so no other heap shrinks. */
uint64_t runtime_output_res_extra_bytes(void) {
    unsigned w, h;
    if (!runtime_output_res(&w, &h)) return 0;
    double ratio = (double)w * h / (1920.0 * 1080.0);
    if (ratio <= 1.0) return 0;
    uint64_t kib = (uint64_t)ceil(113664.0 * ratio / 1024.0) * 1024 - 113664;
    return kib << 10;
}
enum { DP_OUTPUT_SIZE = 32, DP_RENDERER = 40 };   /* data page: the output size {w, h} the game reads; the renderer it stores */
static _Atomic(uint8_t *) output_page;
static unsigned output_w, output_h;   /* the largest output size (the swapchain) */
static uint64_t pack_size(unsigned w, unsigned h) { return (uint64_t)h << 32 | w; }
uint64_t runtime_output_size_current(void) {
    uint8_t *page = atomic_load(&output_page);
    return page ? atomic_load((_Atomic uint64_t *)(void *)(page + DP_OUTPUT_SIZE)) : pack_size(1920, 1080);
}
static void patch_output_res(Module *m) {
    unsigned w, h;
    if (CODE_PATCHED_AT_BUILD || !output_res(&w, &h)) return;
    uint64_t data_off = (m->file_size + 0x3fff) & ~0x3fffULL;
    if (data_off + 0x4000 != m->size || data_off & 0xfff) runtime_fail("output resolution data page layout");
    const GamePatch sites[5] = {
        {GAME_ADDR(0xe996ac), 0xd280f008, 0xd2800000u | (w << 5) | 8u, "mov x8, #0x780 (1920) -> mov x8, #w (the swapchain)"},
        {GAME_ADDR(0xe996b0), 0xf2c08708, 0xf2c00000u | (h << 5) | 8u, "movk x8, #0x438, lsl #32 (1080) -> movk x8, #h, lsl #32"},
        {GAME_ADDR(0xc630a4), 0xaa1403e0, adrp_reg(21, (uintptr_t)m->base + GAME_ADDR(0xc630a4), (uintptr_t)m->base + data_off), "mov x0, x20 -> adrp x21, <data page>"},
        {GAME_ADDR(0xc630a8), GAME_OUTPUT_CALL_WORD, 0xf90016b3, "bl main+0xc632b0 (the parameters' output size) -> str x19, [x21, #40] (the renderer)"},
        {GAME_ADDR(0xc630ac), 0xf9400015, 0xf94012b5, "ldr x21, [x0] -> ldr x21, [x21, #32] (the output size)"},
    };
    for (int i = 0; i < 5; i++) {
        uint32_t word; memcpy(&word, m->base + sites[i].offset, 4);
        if (word != sites[i].original) {
            fprintf(stderr, "[totk] output resolution patch main+%#llx: found %08x, expected %08x (%s)\n", (unsigned long long)sites[i].offset, word, sites[i].original, sites[i].what);
            runtime_fail("output resolution patch: main does not match the selected game version");
        }
    }
    uint8_t *page = m->base + data_off; output_w = w; output_h = h;
    atomic_store((_Atomic uint64_t *)(void *)(page + DP_OUTPUT_SIZE), output_full_at_start ? pack_size(w, h) : pack_size(1920, 1080));
    atomic_store((_Atomic uint64_t *)(void *)(page + DP_RENDERER), 0);
    for (int i = 0; i < 5; i++) memcpy(m->base + sites[i].offset, &sites[i].replacement, 4);
    atomic_store(&output_page, page);   /* Publish initialized state to the HUD on the main thread. */
    fprintf(stderr, "[totk] output resolution patch: swapchain %ux%u (main+0xe996ac), the game's output %s from the data page (main+0xc630a4); texture heap and Graphics +%llu KiB (HeapDef overlay)\n",
            w, h, output_full_at_start ? "at that size" : "1920x1080 for now", (unsigned long long)(runtime_output_res_extra_bytes() >> 10));
}
/* Two-state output-size helper retained for internal use: switch between the largest size and 1920x1080. The renderer
   lays out again at the next frame (see patch_output_res). */
int runtime_output_res_toggle(void) {
    if (!output_page) { fprintf(stderr, "[totk] key 3: switching the output size needs a 2K-capable start (TOTK_OUTPUT_RES auto with the HeapDef overlay, or <w>x<h>)\n"); return 0; }
    _Atomic uint64_t *size = (_Atomic uint64_t *)(void *)(output_page + DP_OUTPUT_SIZE);
    int full = atomic_load(size) != pack_size(output_w, output_h);
    atomic_store(size, full ? pack_size(output_w, output_h) : pack_size(1920, 1080));
    uint64_t renderer = atomic_load((_Atomic uint64_t *)(void *)(output_page + DP_RENDERER));
    const char *before = "";
    if (renderer) {
        uint32_t old = __atomic_fetch_or((uint32_t *)(uintptr_t)(renderer + 0xca4), 0x200u, __ATOMIC_SEQ_CST);
        before = old & 0x200u ? " (the previous switch was still pending)" : "";
        runtime_oe_push_message(30);   /* nn::oe "operation mode changed": the game runs its display handler (main+0x1185310) */
    }
    fprintf(stderr, "[totk] key 3: the game's output %ux%u (%s)%s%s\n", full ? output_w : 1920u, full ? output_h : 1080u,
            full ? "true 2K: the canvas and the UI at that size" : "the 2K output: 1080p canvas, MetalFX scales it at present",
            renderer ? " from the next layout" : "; the game has not laid out its displays yet, it takes this size when it does", before);
    return 1;
}
/* Key 3: cycle the active 2K presentation path. The normal startup is 1080p game output with the MetalFX output scaler
   enabled; native 2K switches the game's canvas/UI to 2560x1440; off returns to 1080p and disables the output scaler. */
int runtime_output_mode_cycle(void) {
    if (!output_page) {
        fprintf(stderr, "[totk] key 3: 2K mode cycling needs a 2K-capable start (TOTK_OUTPUT_RES=auto with the HeapDef overlay, or <w>x<h>)\n");
        return 0;
    }
    _Atomic uint64_t *size = (_Atomic uint64_t *)(void *)(output_page + DP_OUTPUT_SIZE);
    int full = atomic_load(size) == pack_size(output_w, output_h);
    extern int mfxup_output_enabled(void);
    extern void dm_gfx_set_flag(const char *name, int value);
    int next_full, next_mfx;
    const char *name;
    if (full) { next_full = 0; next_mfx = 0; name = "关闭 2K"; }
    else if (mfxup_output_enabled()) { next_full = 1; next_mfx = 0; name = "原生 2K"; }
    else { next_full = 0; next_mfx = 1; name = "MetalFX 2K"; }
    if (full != next_full) runtime_output_res_toggle();
    dm_gfx_set_flag("mfx_output", next_mfx);
    dm_gfx_set_flag("mfx_upscale", next_mfx);
    fprintf(stderr, "[totk] key 3: %s\n", name);
    return 1;
}

/* Diagnostic (Mac): TOTK_TRAP_AT=<main offset> writes brk #0 there; the crash handler (runtime/loader.c, SIGTRAP) then prints
   the registers and the call chain at that moment, plus what TOTK_CRASH_MEM / TOTK_CRASH_HEAPFIND / TOTK_CRASH_HEAPWALK ask for,
   and exits: "stop here and show me the state" without a debugger. */
static void patch_trap(Module *m) {
    const char *e = getenv("TOTK_TRAP_AT");
    if (CODE_PATCHED_AT_BUILD || !e || !*e) return;
    uint64_t off = strtoull(e, NULL, 0);
    if (off & 3 || off + 4 > m->text_end) runtime_fail("TOTK_TRAP_AT must be a 4-byte aligned offset in main's text");
    uint32_t brk = 0xd4200000, was;
    memcpy(&was, m->base + off, 4);
    memcpy(m->base + off, &brk, 4);
    fprintf(stderr, "[totk] TOTK_TRAP_AT: brk at main+%#llx (was %08x); the game stops there with a register dump\n", (unsigned long long)off, was);
}

static void check_site(const Module *m, const GamePatch *p) {
    if (p->offset + 4 > m->text_end) runtime_fail("60 FPS patch site outside the main text segment");
    uint32_t word;
    memcpy(&word, m->base + p->offset, sizeof word);
    if (word != p->original) {
        fprintf(stderr, "[totk] 60 FPS patch main+%#llx: found %08x, expected %08x (%s)\n",
                (unsigned long long)p->offset, word, p->original, p->what);
        runtime_fail("60 FPS patch: main does not match the selected game version");
    }
}

/* TOTK_PCHTXT=<file>: an emulator ExeFS patch for main in pchtxt form, as community mods ship them (for example
   TOTK-Ultrawide's aspect-ratio patch), for trying them here. Mac only: the phone's code is patched at build. Its
   @nsobid must be main's build id, read from the extracted ExeFS (extracted/<version>/exefs/main at 0x40); the lines
   after @enabled are "<main address> <bytes>", @disabled switches the lines that follow off again (a section the author
   keeps without applying it) and @stop ends the file. "@flag offset_shift 0x100" must come before the first patch line:
   it says the addresses leave out the NSO header only the IPS form counts, which is how they are applied here; a file
   without it, or with another shift, would land 0x100 bytes off and stops the start. "@flag print_values" (a display
   option of the patching tools) is accepted, any other flag stops the start. The same reading as the build's --mod
   (scripts/mod_exefs.py). It is applied before this runtime's own patches, whose checks of the original words then
   refuse a file that overlaps them. */
static void patch_pchtxt(Module *m) {
    const char *path = getenv("TOTK_PCHTXT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) runtime_fail("TOTK_PCHTXT: cannot open the file");
    char nso[256], want[41] = {0}; uint8_t id[20];
    snprintf(nso, sizeof nso, "extracted/%s/exefs/main", TOTK_GAME_DIR);
    FILE *n = fopen(nso, "rb");
    if (!n || fseek(n, 0x40, SEEK_SET) || fread(id, 1, sizeof id, n) != sizeof id) runtime_fail("TOTK_PCHTXT: cannot read main's build id from extracted/<version>/exefs/main");
    fclose(n);
    for (int i = 0; i < 20; i++) snprintf(want + 2 * i, 3, "%02X", id[i]);
    char line[1024]; int enabled = 0, matched = 0, shifted = 0; unsigned count = 0, bytes = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "@nsobid-", 8)) {
            if (strncasecmp(line + 8, want, 40)) runtime_fail("TOTK_PCHTXT is for another build of main");
            matched = 1; continue;
        }
        if (!strncmp(line, "@enabled", 8)) { enabled = 1; continue; }
        if (!strncmp(line, "@disabled", 9)) { enabled = 0; continue; }
        if (!strncmp(line, "@flag", 5)) {
            char name[64] = "", value[64] = "";
            sscanf(line + 5, "%63s %63s", name, value);
            if (!strcmp(name, "print_values")) continue;
            if (strcmp(name, "offset_shift")) runtime_fail("TOTK_PCHTXT: only @flag offset_shift 0x100 and print_values are supported");
            if (strtoull(value, NULL, 16) != 0x100) runtime_fail("TOTK_PCHTXT: @flag offset_shift must be 0x100 (the addresses are main addresses)");
            shifted = 1; continue;
        }
        if (!strncmp(line, "@stop", 5)) break;
        unsigned long long at; char hex[512];
        if (!enabled || line[0] == '@' || sscanf(line, "%llx %511s", &at, hex) != 2) continue;
        size_t len = strlen(hex);
        if (!matched) runtime_fail("TOTK_PCHTXT: patches before its @nsobid line");
        if (!shifted) runtime_fail("TOTK_PCHTXT: no @flag offset_shift 0x100 before the first patch line");
        if (len % 2 || at + len / 2 > m->file_size) runtime_fail("TOTK_PCHTXT: a patch line is malformed or outside main");
        for (size_t i = 0; i < len / 2; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); m->base[at + i] = (uint8_t)v; }
        count++; bytes += (unsigned)(len / 2);
    }
    fclose(f);
    if (!matched) runtime_fail("TOTK_PCHTXT has no @nsobid line");
    fprintf(stderr, "[totk] TOTK_PCHTXT %s: %u patches, %u bytes in main (build id %s)\n", path, count, bytes, want);
}

/* High frame rates (#61, sky-island trampolines): main+0xce3fec (1.0.0) compares X+0x28 with 100 and, when greater,
   scales the velocity Link takes over from the ground body by 0.1. X is the game's time object ([GOT]+0xc8; X+0x38 the
   30 Hz ratio, X+0x28 = 30 / (ratio * time scale)); at the shipping 30 Hz the check means "time slowed below 0.3", but
   above 100 FPS it fires with time running normally and the trampolines stop bouncing. A veneer in main's text padding
   (+128, after the 1.2.1 ultrawide veneers) compares X+0x28 * X+0x38 = 30 / time scale instead: the same value at 30 Hz,
   independent of the frame rate. Same code in all four versions (1.4.3: game_patches_v143.c). TOTK_HIGHFPS_FIX=0 keeps
   the original check (Mac). */
static void patch_slowmo_bounce(Module *m) {
    const char *e = getenv("TOTK_HIGHFPS_FIX");
    if (!CODE_PATCHED_AT_BUILD && e && !strcmp(e, "0")) return;
    const uint64_t site = GAME_ADDR(0xce3fec), stub = ((m->text_end + 15) & ~15ULL) + 128;
    if (stub + 20 > ((m->text_end + 15) & ~15ULL) + 0x400) runtime_fail("bounce veneer layout");
    static const uint32_t original[3] = {0xbd402900,   /* ldr s0, [x8, #0x28] */
                                         0x52a85908,   /* mov w8, #0x42c80000 (100.0) */
                                         0x1e270101};  /* fmov s1, w8 */
    if (!CODE_PATCHED_AT_BUILD) {
        for (unsigned i = 0; i < 3; i++) {
            uint32_t w; memcpy(&w, m->base + site + 4 * i, 4);
            if (w != original[i]) runtime_fail("bounce check: original instruction mismatch");
        }
        for (unsigned i = 0; i < 20; i++) if (m->base[stub + i]) runtime_fail("bounce veneer padding not empty");
    }
    const uint32_t veneer[5] = {0xbd402900,   /* ldr s0, [x8, #0x28]: X+0x28 */
                                0xbd403901,   /* ldr s1, [x8, #0x38]: the 30 Hz ratio (the next original instruction sets s1 again) */
                                0x1e210800,   /* fmul s0, s0, s1 */
                                0x52a85908,   /* mov w8, #0x42c80000: the original 100.0 */
                                branch_to(stub + 16, site + 8)};
    for (unsigned i = 0; i < 5; i++) put_word(m, stub + 4 * i, veneer[i], "bounce veneer");
    put_word(m, site, branch_to(site, stub), "bounce check");
    fprintf(stderr, "[totk] %s high-FPS fix: trampoline slow-motion check uses the time scale, veneer +%#llx\n",
            TOTK_GAME_VERSION, (unsigned long long)stub);
}

void runtime_game_patches(Module *m) {
    runtime_survey_init(m);
    const size_t n = sizeof fps60_sites / sizeof fps60_sites[0];
    if (strcmp(m->name, "main")) runtime_fail("game patches expect the main module");
    if (CODE_PATCHED_AT_BUILD) {
        /* Embedded variant: the code was patched at build time (ios/tools/build_app.sh exports with the real-time frame
           step and the 60 FPS patch), so the phone is not held at the shipping 30 fps; here we only verify the words. */
        patch_realtime(m);
        patch_render_res(m);
        patch_slowmo_bounce(m);
        for (size_t i = 0; i < n; i++) {
            uint32_t word;
            memcpy(&word, m->base + fps60_sites[i].offset, sizeof word);
            if (word != fps60_sites[i].replacement) {
                fprintf(stderr, "[totk] 60 FPS patch main+%#llx: found %08x, expected the patched %08x (%s)\n",
                        (unsigned long long)fps60_sites[i].offset, word, fps60_sites[i].replacement, fps60_sites[i].what);
                runtime_fail("60 FPS patch: embedded main was not exported with TOTK_FPS60 on");
            }
        }
        fprintf(stderr, "[totk] embedded main: real-time frame step and 60 FPS patch (%zu instructions) applied at build time\n", n);
        return;
    }
    patch_pchtxt(m);   /* first: the runtime's own patches check the original words they replace */
    if (realtime_enabled()) patch_realtime(m);
    else fprintf(stderr,"[totk] real-time frame step off (TOTK_REALTIME=0)\n");
    patch_render_res(m);
    patch_output_res(m);
    patch_slowmo_bounce(m);
    patch_trap(m);
#if defined(TOTK_GAME_V121)
    { static const uint64_t sites[] = {0xcb4b10, 0x12521cc, 0x19b36f0, 0x1ecdf68};   /* camera x2, viewport x2 (aspect_patch.h) */
      aspect_install(m, "1.2.1", sites, 4, 2, (m->text_end + 15) & ~15ULL, 0,   /* no free padding for Vert+ stubs */
                     (m->file_size + 0x3fff) & ~0x3fffULL); }
#else
    if (aspect_requested()) runtime_fail("TOTK_ASPECT: only 1.2.1 and 1.4.3 so far");
#endif
    if (!fps60_enabled()) {
        fprintf(stderr, "[totk] 60 FPS patch off (TOTK_FPS60=0): shipping present interval range 2..3\n");
        return;
    }
    for (size_t i = 0; i < n; i++) check_site(m, &fps60_sites[i]);   /* verify all before writing any */
    for (size_t i = 0; i < n; i++) memcpy(m->base + fps60_sites[i].offset, &fps60_sites[i].replacement, 4);
    fprintf(stderr, "[totk] %s 60 FPS patch: %zu instructions (interval range 1..3)\n", TOTK_GAME_VERSION, n);
}
