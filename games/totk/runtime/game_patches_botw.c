/* Breath of the Wild game-code patches (botw.md): exclusive loads with acquire, the real-time frame step, and the
   TOTK_JQ_CHECK diagnostics. Key 4 (ultrawide) is 1.9.0 only (end of the file); the output size answers "not available"
   until BotW's own sites are found. */
#include <stdatomic.h>
#include "world_survey.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "game_patches.h"
#include "game_version.h"
#include "aspect_math.h"   /* the aspect ratio: TOTK_ASPECT parsing */
#include <TargetConditionals.h>
uint32_t runtime_thread_current_handle(void);
static void botw_aspect_install(Module *m);   /* the aspect ratio, end of the file */
static int patch_output_res(Module *m);   /* native output size, after runtime_output_res */
static void botw_output_tick(void);       /* the size static follows the canvas after the start */
static void botw_aspect_apply(void);

/* iOS cannot write code pages at run time: the iOS export (TOTK_EXPORT_IOS, runtime/loader.c export-ios) runs these
   patches on the Mac, and the patched text and patch area are embedded in the app (ios/tools/gen_embed.py), as for
   Tears of the Kingdom (game_patches.c). On the device the original words are not checked (they are patched already) and
   each patched word is checked instead of written. The switches that leave a patch out (TOTK_REALTIME=0,
   TOTK_PAD_REPEAT=0, TOTK_SLINK_REALTIME=0, TOTK_RENDER_RES=900, TOTK_EXCL_ACQUIRE=0), the brk diagnostics (TOTK_PAD_LOG, TOTK_JQ_CHECK) and the
   native output size (TOTK_OUTPUT_RES) are Mac only; the export ignores them too, so the device checks what it made. */
#if TARGET_OS_IPHONE
#define CODE_PATCHED_AT_BUILD 1
#else
#define CODE_PATCHED_AT_BUILD 0
#endif
static int ios_code(void) { return CODE_PATCHED_AT_BUILD || getenv("TOTK_EXPORT_IOS") != NULL; }   /* the phone's code */
static void put_words(Module *m, uint64_t off, const uint32_t *words, size_t n, const char *what) {
    if (!CODE_PATCHED_AT_BUILD) { memcpy(m->base + off, words, 4 * n); return; }
    for (size_t i = 0; i < n; i++) {
        uint32_t have; memcpy(&have, m->base + off + 4 * i, 4);
        if (have != words[i]) {
            fprintf(stderr, "[botw] %s main+%#llx: embedded %08x, expected patched %08x\n", what, (unsigned long long)(off + 4 * i), have, words[i]);
            runtime_fail("embedded main text was not patched at build time (ios/tools/build_ipa.py export-ios)");
        }
    }
}
static void put_word(Module *m, uint64_t off, uint32_t word, const char *what) { put_words(m, off, &word, 1, what); }

/* Real-time frame step (TOTK_REALTIME=0: as shipped). ksys::VFR (singleton pointer main+0x2d1e170, built at main+0x183eb98)
   keeps one frame ratio at +0x3c: the frame's length in 30 fps frames. Every per-core step the game reads derives from it:
   delta frames (+0x108, about 1,500 readers) = time-speed multiplier x ratio, delta time (+0x138) = that x 1/30 s (+0x158).
   The per-frame update main+0x1c01180 sets the ratio from the present interval (interval / 2: 1.0 at the shipped 30 fps)
   and writes +0x3c only when it changes, so the game ran at twice its speed at 60 fps. Two of its instructions now take the
   ratio from the loader's data page after main, which the host refreshes at each present with the measured frame time x 30
   (the page layout of Tears of the Kingdom's patch: low float ratio, high float seconds):
     main+0x1c0120c  fcmp s1, s0       -> adrp x9, <data page>
     main+0x1c01214  b.eq #0x1c01220   -> ldr s1, [x9]
   and the store of s1 to +0x3c after them now runs every frame. The interval bookkeeping (+0x28 changed, +0x30 / +0x34
   interval ratio and interval, +0x38 / +0x40 previous) stays as shipped.
   One more reader steps by the interval itself instead of the ratio: a fade (0 -> 1 in 5 frames at 30 fps; main+0x1821548
   reads the VFR's interval, +0x48 per core) adds max(interval, 1) x 0.5 x 0.2 per frame. It now adds ratio x 0.2, the
   same value at the shipped 30 fps:
     main+0x1821568  ldr w8, [x8]               -> adrp x8, <data page>
     main+0x182156c  cmp w8, #0                 -> ldr s0, [x8]
     main+0x1821570  csinc w8, w8, wzr, ne      -> nop
     main+0x1821574  ucvtf s0, w8               -> nop
     main+0x1821588  fmul s0, s0, s11 (x 0.5)   -> nop
   (x8 is reloaded two instructions later, the flags are not read again.) The UI's animation step is the same sum
   (main+0x18522e0: max(interval, 1) x 0.5 into +0xb20 of the layout system): at 120 fps
   the game still asks for interval 2, so menus and the HUD animated four times too fast (2026-10-20). It now takes the
   ratio too: main+0x18522ec ldr w8 -> adrp x8, main+0x18522f0 cmp -> ldr s0, [x8], and the csinc / ucvtf / fmul become
   nops (s1 keeps its 0.5; x8 and the flags are not read again before they are set).
   Physics: the frame loop calls the physics step (main+0x1c9c920) with the frame time, the frame ratio and the time
   speed, and the step runs round(6 x ratio) solver substeps (main+0x1c9c958): six at the shipped 30 fps, two at 120 fps,
   where staggered enemies' ragdolls blew apart and flew off (2026-10-20). The ratio argument
   is now always 1.0 (main+0x1618480 ldr s9, [x8] -> fmov s9, #1.0): six substeps every frame, the frame time still sets
   the step. MacNativeGames games/botw/scripts/
   verify_realtime_patch.py runs both with Unicorn. */
static _Atomic uint64_t *frame_step_data;
static uint8_t *frame_step_main;
static uint64_t last_present_ns, report_ns, report_frames;
static double frame_samples[5], report_seconds, report_game, report_short_ms, report_long_ms;
static unsigned frame_sample_count, frame_sample_next, report_short, report_long;
static _Atomic uint32_t pinned_step_us;
#ifdef GAME_BOTW160
enum { VFR_INSTANCE = 0x2d1e170, VFR_RATIO = 0x3c, VFR_DELTA_TIME = 0x138 };
#else   /* 1.9.0: the singleton pointer is main+0x1e93890, the object's layout is the same */
enum { VFR_INSTANCE = 0x1e93890, VFR_RATIO = 0x3c, VFR_DELTA_TIME = 0x138 };
#endif
static _Atomic uint32_t *pad_tick;   /* data page +8: 1 when the next frame's game time crosses a 1/30 s boundary */
static double pad_phase;
static uint64_t report_ticks;
static int g_pad_log_on; static _Atomic uint64_t g_pad_dir_until;   /* TOTK_PAD_LOG */
#ifdef GAME_BOTW160
static uint8_t *g_pad_main;
#endif

static int realtime_enabled(void) { const char *e = getenv("TOTK_REALTIME"); return ios_code() || !(e && !strcmp(e, "0")); }
static uint64_t frame_step_bits(float seconds) {
    float ratio = seconds * 30.0f; uint32_t lo, hi;
    memcpy(&lo, &ratio, 4); memcpy(&hi, &seconds, 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}
/* Diagnostics (TOTK_INPUT_FILE "framestep <ms>"): pin the frame step to a fixed length, 0 back to real time. */
void runtime_frame_step_pin(double ms) {
    uint32_t us = ms > 0 ? (uint32_t)(ms * 1000.0 + 0.5) : 0;
    atomic_store(&pinned_step_us, us);
    if (frame_step_data && us) atomic_store_explicit(frame_step_data, frame_step_bits((float)(us / 1e6)), memory_order_release);
    if (!us) fprintf(stderr, "[botw] frame step back to real time\n");
    else fprintf(stderr, "[botw] frame step pinned at %.3f ms (ratio %.3f, diagnostics)%s\n", us / 1000.0, us / 1e6 * 30.0,
                 frame_step_data ? "" : " -- but the real-time patch is off, nothing changes");
}
/* Every 30 s one line: frames and their real time, the game time the steps added up to, what the game actually reads (the
   VFR's ratio and core 0's delta time), and the intervals outside 1/120..1/15 s with what clamping each of them on its own
   would have added or dropped. */
static void frame_step_report(uint64_t now_ns) {
    if (!report_ns) { report_ns = now_ns; return; }
    if (now_ns - report_ns < 30000000000ull) return;
    float ratio = -1, delta = -1;
    uint64_t vfr = *(const uint64_t *)(const void *)(frame_step_main + VFR_INSTANCE);
    if (vfr) {
        memcpy(&ratio, (const void *)(uintptr_t)(vfr + VFR_RATIO), 4);
        uint64_t p = *(const uint64_t *)(uintptr_t)(vfr + VFR_DELTA_TIME);
        if (p) memcpy(&delta, (const void *)(uintptr_t)p, 4);
    }
    double span = (double)(now_ns - report_ns) * 1e-9;
    char game[96];
    if (atomic_load(&pinned_step_us)) snprintf(game, sizeof game, "step pinned");
    else snprintf(game, sizeof game, "steps add up to %.2f s (%.4f of the frames' time)", report_game, report_seconds > 0 ? report_game / report_seconds : 0);
    char ticks[64] = "";
    if (pad_tick) snprintf(ticks, sizeof ticks, "; pad counts %.1f ticks/s", report_ticks / span);
    fprintf(stderr, "[botw] real-time frame step: %llu frames in %.1f s (%.1f fps, %.2f ms apart on average), %s; the game's VFR ratio "
                    "%.3f, delta time %.2f ms; %u intervals under 1/120 s (clamping each: +%.0f ms), %u over 1/15 s (-%.0f ms)%s\n",
            (unsigned long long)report_frames, span, report_frames / span, report_frames ? report_seconds / report_frames * 1e3 : 0, game,
            ratio, delta * 1e3, report_short, report_short_ms, report_long, report_long_ms, ticks);
    report_ns = now_ns; report_frames = report_ticks = 0; report_seconds = report_game = report_short_ms = report_long_ms = 0; report_short = report_long = 0;
}
static void frame_step_measure(uint64_t now_ns) {
    frame_step_report(now_ns);
    uint64_t prev = last_present_ns;
    last_present_ns = now_ns;
    if (!prev) return;
    double dt = (double)(now_ns - prev) * 1e-9;
    if (dt > 0.25 || dt < 0.001) { frame_sample_count = frame_sample_next = 0; return; }
    report_frames++; report_seconds += dt;
    if (dt < 1.0 / 120.0) { report_short++; report_short_ms += (1.0 / 120.0 - dt) * 1e3; }
    if (dt > 1.0 / 15.0) { report_long++; report_long_ms += (dt - 1.0 / 15.0) * 1e3; }
    if (atomic_load_explicit(&pinned_step_us, memory_order_relaxed)) return;
    /* The raw intervals are averaged and the average is clamped, not each interval: at about 95 fps under the 120 Hz
       clock, intervals shorter than 1/120 s alternate with longer ones, and raising each short one to 1/120 s made the
       game run 6% fast (measured: botw.md). Averaged, the steps add up to the real time at any frame rate (no lower bound:
       a display above 120 Hz presents more often, and a floor of 1/120 s made the game run fast there); only an average
       above 1/15 s is cut, and a hitch is spread over the next frames instead. */
    frame_samples[frame_sample_next++ % 5] = dt;
    if (frame_sample_count < 5) frame_sample_count++;
    double sum = 0;
    for (unsigned i = 0; i < frame_sample_count; i++) sum += frame_samples[i];
    double step = sum / frame_sample_count;
    if (step > 1.0 / 15.0) step = 1.0 / 15.0;
    report_game += step;
    atomic_store_explicit(frame_step_data, frame_step_bits((float)step), memory_order_release);
}
/* The pad tick follows the game time the next frame adds (the step on the page, pinned or measured): one tick per 1/30 s,
   at most one per frame (below 30 fps the counts advance once a frame, as shipped). */
static void pad_tick_advance(void) {
    uint32_t hi = (uint32_t)(atomic_load_explicit(frame_step_data, memory_order_relaxed) >> 32);
    float seconds; memcpy(&seconds, &hi, 4);
    pad_phase += (double)seconds * 30.0;
    uint32_t tick = 0;
    if (pad_phase >= 1.0) { tick = 1; pad_phase -= 1.0; if (pad_phase >= 1.0) pad_phase = 0; }
    report_ticks += tick;
    atomic_store_explicit(pad_tick, tick, memory_order_release);
    if (g_pad_log_on) {
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (now < atomic_load(&g_pad_dir_until)) fprintf(stderr, "[pad] %.4f present thread %#x: tick %u (step %.2f ms)\n", now * 1e-9, runtime_thread_current_handle(), tick, seconds * 1e3);
    }
}
void runtime_frame_step_update(uint64_t now_ns) {
    botw_aspect_apply();   /* the aspect ratio */
    botw_output_tick();    /* key 3 */
    if (!frame_step_data || !now_ns) return;
    frame_step_measure(now_ns);
    if (pad_tick) pad_tick_advance();
}
static uint32_t adrp_reg(unsigned rd, uint64_t site, uint64_t target) {
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(site >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("real-time frame-step data outside ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) | (rd & 31u);
}
#ifdef GAME_BOTW160
/* Pad repeat and hold counts by real time (TOTK_PAD_REPEAT=0: as shipped; botw.md). sead::ControllerBase::
   updateDerivativeParams_ (main+0x126e3b0) counts the frames each button has been held (+0x30 + 4 i) and raises its
   repeat bit (+0x10) when the count reaches the button's delay (+0xb0 + i) and then every pulse (+0xd0 + i) frames.
   The menus set delay 6 / pulse 2 for the stick and d-pad directions (main+0x1a40958) or take them from their data
   (main+0x12143e4); at 120 fps the cursor moved four times as fast as at the shipped 30 fps. The loop over the buttons
   (main+0x126e490..0x126e528, 38 instructions) is rewritten in place: a held button is counted, and its repeat tested,
   only on frames whose game time crosses a 1/30 s boundary (the tick at data page +8, from the same step as the VFR
   ratio), so counts, delays and pulses are 1/30 s again. Released buttons still reset at once; the trigger and release
   bits are computed before the loop and stay per frame. New registers: x16 (the page, then the tick) and x17 (the count's
   address; the shipped loop reused x11 for it and reloaded the hold mask every iteration). MacNativeGames
   games/botw/scripts/verify_pad_repeat_patch.py runs both loops with Unicorn. */
enum { PAD_LOOP = 0x126e490, PAD_LOOP_WORDS = 38, PAD_ADRP = 7 };
static const uint32_t pad_loop_shipped[PAD_LOOP_WORDS] = {
    0xb9410268u, 0x7100051fu, 0xb901166bu, 0xb900127fu, 0x5400044bu, 0x2a1f03e8u, 0xaa1f03e9u, 0x320003eau, 0x14000002u, 0xb941166bu,
    0x1ac9214cu, 0x6a0c017fu, 0x54000180u, 0x8b09026eu, 0x394341cdu, 0x8b090a6bu, 0x340001edu, 0xb940316fu, 0x3942c1ceu, 0x6b0e01eeu,
    0x540000e1u, 0x2a0c0108u, 0xb9001268u, 0x14000008u, 0x8b090a6bu, 0xb900317fu, 0x14000008u, 0x54000089u, 0x1acd09cfu, 0x1b0db9edu,
    0x34fffeedu, 0xb940316cu, 0x1100058cu, 0xb900316cu, 0xb981026bu, 0x91000529u, 0xeb0b013fu, 0x54fffc8bu,
};
static const uint32_t pad_loop_ticked[PAD_LOOP_WORDS] = {
    0xb9410268u, /* ldr   w8, [x19, #0x100]   (button count) */
    0x7100051fu, /* cmp   w8, #1 */
    0xb901166bu, /* str   w11, [x19, #0x114]  (hold) */
    0xb900127fu, /* str   wzr, [x19, #0x10]   (repeat = 0) */
    0x5400044bu, /* b.lt  end */
    0x2a1f03e8u, /* mov   w8, wzr */
    0xaa1f03e9u, /* mov   x9, xzr */
    0,           /* adrp  x16, <data page> */
    0xb9400a10u, /* ldr   w16, [x16, #8]      (tick) */
    0x5280002au, /* mov   w10, #1 */
    0x1ac9214cu, /* loop: lsl w12, w10, w9 */
    0x8b090a71u, /* add   x17, x19, x9, lsl #2 */
    0x6a0c017fu, /* tst   w11, w12 */
    0x54000180u, /* b.eq  released */
    0x34000290u, /* cbz   w16, next           (no tick: count and repeat unchanged) */
    0x8b09026eu, /* add   x14, x19, x9 */
    0x394341cdu, /* ldrb  w13, [x14, #0xd0]   (pulse) */
    0x340001cdu, /* cbz   w13, count */
    0xb940322fu, /* ldr   w15, [x17, #0x30]   (count) */
    0x3942c1ceu, /* ldrb  w14, [x14, #0xb0]   (delay) */
    0x6b0e01eeu, /* subs  w14, w15, w14 */
    0x540000c1u, /* b.ne  past */
    0x2a0c0108u, /* repeat: orr w8, w8, w12 */
    0xb9001268u, /* str   w8, [x19, #0x10] */
    0x14000007u, /* b     count */
    0xb900323fu, /* released: str wzr, [x17, #0x30] */
    0x14000008u, /* b     next */
    0x54000089u, /* past: b.ls count */
    0x1acd09cfu, /* udiv  w15, w14, w13 */
    0x1b0db9edu, /* msub  w13, w15, w13, w14 */
    0x34ffff0du, /* cbz   w13, repeat */
    0xb940322cu, /* count: ldr w12, [x17, #0x30] */
    0x1100058cu, /* add   w12, w12, #1 */
    0xb900322cu, /* str   w12, [x17, #0x30] */
    0xb981026fu, /* next: ldrsw x15, [x19, #0x100] */
    0x91000529u, /* add   x9, x9, #1 */
    0xeb0f013fu, /* cmp   x9, x15 */
    0x54fffcabu, /* b.lt  loop */
};
/* The pause menu's own count (main+0x1214474..0x1214498, 9 instructions, in the menu update main+0x12125b0): each
   direction has a record {mask, 60, countdown, delay 9, pulse 3, fast delay 60, fast pulse 1} (built at main+0x1207d0c);
   while the direction is held, every frame takes one off the countdown, and at zero the controllers' delay / pulse
   switch to the fast pair (at the shipped 30 fps the held count is 60 at that moment, so the cursor goes on at one step
   per 1/30 s without a pause). Per frame, at 120 fps the switch came at 0.5 s with the held count at 25, and the cursor
   stopped until the count reached 60. The countdown now takes one off only on a tick as well (same registers; the
   record address is formed in two adds instead of four instructions, and the "already negative" test is dropped: a
   negative countdown keeps falling by one per tick, which is harmless):
     add x10, x27, x10, lsl #4 / add x11, x10, #0x64000 / adrp x13, <page> / ldr w13, [x13, #8] / cbz w13, done /
     ldr w12, [x11, #0xa00] / sub w13, w12, #1 / str w13, [x11, #0xa00] / cbnz w12, done */
enum { MENU_CD = 0x1214474, MENU_CD_WORDS = 9, MENU_CD_ADRP = 2 };
static const uint32_t menu_cd_shipped[MENU_CD_WORDS] = { 0x8b0a136au, 0x5289400bu, 0x72a000cbu, 0x8b0b014bu, 0xb940016cu, 0x37f80accu, 0x5100058du, 0xb900016du, 0x35000a6cu };
static const uint32_t menu_cd_ticked[MENU_CD_WORDS] = { 0x8b0a136au, 0x9141914bu, 0, 0xb94009adu, 0x34000aedu, 0xb94a016cu, 0x5100058du, 0xb90a016du, 0x35000a6cu };
static void pad_log_arm(Module *m) {
    const char *l = getenv("TOTK_PAD_LOG");
    if (!l || !*l || !strcmp(l, "0") || ios_code()) return;
    /* diagnostics: a brk on the loop's first instruction (ldr w8, [x19, #0x100], the same shipped and patched), see botw_pad_hook */
    uint32_t b = 0xd4200000u | (0x7c0u << 5); memcpy(m->base + PAD_LOOP, &b, 4);
    g_pad_main = m->base; g_pad_log_on = 1;
    fprintf(stderr, "[botw] TOTK_PAD_LOG: every controller update while a direction is held, and the tick at each present\n");
}
static void patch_pad_repeat(Module *m, uint64_t page) {
    const char *e = getenv("TOTK_PAD_REPEAT");
    if (!ios_code() && e && !strcmp(e, "0")) { fprintf(stderr, "[botw] pad repeat and hold counts per frame, as shipped (TOTK_PAD_REPEAT=0)\n"); pad_log_arm(m); return; }
    for (unsigned i = 0; !CODE_PATCHED_AT_BUILD && i < PAD_LOOP_WORDS; i++) {
        uint32_t w; memcpy(&w, m->base + PAD_LOOP + 4 * i, 4);
        if (w != pad_loop_shipped[i]) {
            fprintf(stderr, "[botw] pad repeat main+%#x: found %08x expected %08x\n", PAD_LOOP + 4 * i, w, pad_loop_shipped[i]);
            runtime_fail("pad repeat by real time requires the 1.6.0 sead::ControllerBase loop");
        }
    }
    for (unsigned i = 0; !CODE_PATCHED_AT_BUILD && i < MENU_CD_WORDS; i++) {
        uint32_t w; memcpy(&w, m->base + MENU_CD + 4 * i, 4);
        if (w != menu_cd_shipped[i]) {
            fprintf(stderr, "[botw] menu countdown main+%#x: found %08x expected %08x\n", MENU_CD + 4 * i, w, menu_cd_shipped[i]);
            runtime_fail("pad repeat by real time requires the 1.6.0 pause menu countdown");
        }
    }
    pad_tick = (_Atomic uint32_t *)(uintptr_t)(page + 8);
    atomic_store(pad_tick, 1);
    for (unsigned i = 0; i < PAD_LOOP_WORDS; i++) {
        uint32_t w = i == PAD_ADRP ? adrp_reg(16, (uintptr_t)m->base + PAD_LOOP + 4 * i, page) : pad_loop_ticked[i];
        put_word(m, PAD_LOOP + 4 * i, w, "pad repeat");
    }
    for (unsigned i = 0; i < MENU_CD_WORDS; i++) {
        uint32_t w = i == MENU_CD_ADRP ? adrp_reg(13, (uintptr_t)m->base + MENU_CD + 4 * i, page) : menu_cd_ticked[i];
        put_word(m, MENU_CD + 4 * i, w, "menu countdown");
    }
    fprintf(stderr, "[botw] pad repeat and hold counts by real time: held buttons count once per 1/30 s of game time, and so does "
                    "the pause menu's fast-repeat countdown (%u + %u instructions, all verified; TOTK_PAD_REPEAT=0: once per frame, as shipped)\n",
            PAD_LOOP_WORDS, MENU_CD_WORDS);
    pad_log_arm(m);
}
/* Sound-link timers by real time (TOTK_SLINK_REALTIME=0: as shipped). xlink2 steps its asset timers (delays, durations)
   by the frame rate its system holds at +0x84. The effect system's rate gets the VFR's ratio each frame
   (main+0x1be7918); the sound system's ([main+0x2cc2028]) gets max(raw ratio, 1.0) (main+0x1c59fac..0x1c5a014),
   which is the ratio on the Switch, where a frame is never shorter than 1/30 s. Above 30 fps the 1.0 made every SLink
   timer count rendered frames: Cryonis' placement marker, emitted again when its sound event ends after about 24
   frames, restarted 4-5 times a second at 120 fps instead of about 1.3, so the ice block preview never grew (measured
   2026-10-23; 1.3 a second at 120 fps after the fix). The store now takes the unclamped ratio from the frame-step page;
   the audio frame values computed from the clamped number just before it are unchanged. x9 and s0 are free there (a call follows):
     main+0x1c5a014  str s0, [x8, #0x84]  -> b <veneer>
     veneer          adrp x9, <page> / ldr s0, [x9] / str s0, [x8, #0x84] / b main+0x1c5a018 */
static uint32_t branch_to(uint64_t site, uint64_t target) {   /* b <target>, both offsets in main */
    int64_t delta = (int64_t)target - (int64_t)site;
    if ((delta & 3) || delta < -(1 << 27) || delta >= (1 << 27)) runtime_fail("patch branch outside range");
    return 0x14000000u | ((uint32_t)(delta >> 2) & 0x3ffffffu);
}
static void patch_slink_rate(Module *m, uint64_t page) {
    const char *e = getenv("TOTK_SLINK_REALTIME");
    if (!ios_code() && e && !strcmp(e, "0")) { fprintf(stderr, "[botw] sound-link timers at least one 30 fps frame per frame, as shipped (TOTK_SLINK_REALTIME=0)\n"); return; }
    enum { SITE = 0x1c5a014, BACK = 0x1c5a018 };
    static const struct { uint32_t off, original; } check[] = {
        {0x1c59fa8u, 0xbd400100u}, {0x1c59facu, 0x1e2e1001u}, {0x1c59fb0u, 0x1e214800u},
        {0x1c5a00cu, 0x90008348u}, {0x1c5a010u, 0xf9401508u}, {SITE, 0xbd008500u}, {BACK, 0x91090300u}, {0x1c5a01cu, 0x94133c99u},
    };
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof check / sizeof *check; i++) {   /* the phone's text is patched already */
        uint32_t w; memcpy(&w, m->base + check[i].off, 4);
        if (w != check[i].original) {
            fprintf(stderr, "[botw] sound-link rate main+%#x: found %08x expected %08x\n", check[i].off, w, check[i].original);
            runtime_fail("sound-link timers by real time require the 1.6.0 rate update instructions");
        }
    }
    const uint64_t stub = (m->text_end + 15) & ~15ULL;
    const uint32_t veneer[] = {
        adrp_reg(9, (uintptr_t)m->base + stub, page),
        0xbd400120u,   /* ldr s0, [x9]         (the measured ratio) */
        0xbd008500u,   /* str s0, [x8, #0x84]  (the sound system's rate) */
        branch_to(stub + 12, BACK),
    };
    if (stub + sizeof veneer > ((m->text_end + 0xfff) & ~0xfffULL)) runtime_fail("sound-link veneer layout");
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof veneer; i++) if (m->base[stub + i]) runtime_fail("sound-link veneer padding not empty");
    put_words(m, stub, veneer, sizeof veneer / sizeof *veneer, "sound-link veneer");
    put_word(m, SITE, branch_to(SITE, stub), "sound-link rate");
    fprintf(stderr, "[botw] sound-link timers follow the measured frame ratio (veneer +%#llx; TOTK_SLINK_REALTIME=0: as shipped)\n",
            (unsigned long long)stub);
}
static void patch_realtime(Module *m) {
    /* The loader reserved a writable page immediately after main's image; nothing else uses it in this game. */
    uint64_t data_off = (m->file_size + 0x3fff) & ~0x3fffULL;
    if (data_off + 0x4000 != m->size || data_off & 0xfff) runtime_fail("real-time frame-step page layout");
    /* Every word of both sequences, the replaced ones and their neighbours, must be the 1.6.0 original. */
    static const struct { uint32_t off, original; } check[] = {
        {0x1c0120cu, 0x1e202020u}, {0x1c01210u, 0x3900a008u}, {0x1c01214u, 0x54000060u}, {0x1c01218u, 0xbd003c01u},
        {0x1821564u, 0xf9400108u}, {0x1821568u, 0xb9400108u}, {0x182156cu, 0x7100011fu}, {0x1821570u, 0x1a9f1508u},
        {0x1821574u, 0x1e230100u}, {0x1821578u, 0x1e2c100bu}, {0x182157cu, 0xb0006f68u}, {0x1821580u, 0xbd409109u},
        {0x1821584u, 0xbd435ee1u}, {0x1821588u, 0x1e2b0800u}, {0x182158cu, 0x1e290800u},
        {0x18522ecu, 0xb9400108u}, {0x18522f0u, 0x7100011fu}, {0x18522f4u, 0x1a9f1508u}, {0x18522f8u, 0x1e230100u},
        {0x18522fcu, 0x1e2c1001u}, {0x1852300u, 0x1e210800u}, {0x1852304u, 0xbd0b22a0u},
        {0x161847cu, 0x1e290908u}, {0x1618480u, 0xbd400109u}, {0x1618484u, 0xb94c3680u},
    };
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof check / sizeof *check; i++) {
        uint32_t w; memcpy(&w, m->base + check[i].off, 4);
        if (w != check[i].original) {
            fprintf(stderr, "[botw] real-time frame step main+%#x: found %08x expected %08x\n", check[i].off, w, check[i].original);
            runtime_fail("real-time frame step requires the 1.6.0 VFR update instructions");
        }
    }
    const uint64_t page = (uintptr_t)m->base + data_off;
    const uint32_t nop = 0xd503201fu;
    const struct { uint32_t off, word; } put[] = {
        {0x1c0120cu, adrp_reg(9, (uintptr_t)m->base + 0x1c0120c, page)}, {0x1c01214u, 0xbd400121u},   /* ldr s1, [x9] */
        {0x1821568u, adrp_reg(8, (uintptr_t)m->base + 0x1821568, page)}, {0x182156cu, 0xbd400100u},   /* ldr s0, [x8] */
        {0x1821570u, nop}, {0x1821574u, nop}, {0x1821588u, nop},
        {0x18522ecu, adrp_reg(8, (uintptr_t)m->base + 0x18522ec, page)}, {0x18522f0u, 0xbd400100u},   /* ldr s0, [x8] */
        {0x18522f4u, nop}, {0x18522f8u, nop}, {0x1852300u, nop},
        {0x1618480u, 0x1e2e1009u},   /* fmov s9, #1.0: six physics substeps per frame at any frame rate */
    };
    for (size_t i = 0; i < sizeof put / sizeof *put; i++) put_word(m, put[i].off, put[i].word, "real-time step");
    frame_step_main = m->base;
    frame_step_data = (_Atomic uint64_t *)(void *)(m->base + data_off);
    atomic_store(frame_step_data, frame_step_bits(1.0f / 30.0f));
    fprintf(stderr, "[botw] real-time frame step: the VFR ratio, one interval-stepped fade and the UI animation step follow the measured frame time, physics keeps six substeps a frame "
                    "(%zu instructions, %zu verified); initial 33.33 ms, sampled at each present (TOTK_REALTIME=0: as shipped)\n",
            sizeof put / sizeof *put, sizeof check / sizeof *check);
    patch_pad_repeat(m, page);
    patch_slink_rate(m, page);
}
#else
/* Breath of the Wild 1.9.0: the same real-time step at the 1.9.0 sites (found by disassembly; MacNativeGames
   games/botw/scripts/verify_realtime_patch.py --version botw190 and verify_pad_repeat_patch.py --version botw190 run them
   with Unicorn). ksys::VFR::calc (main+0x151d3d0) joins its ratio paths at the same place as 1.6.0's update:
     main+0x151d484  fcmp s1, s0        -> adrp x9, <data page>
     main+0x151d48c  b.eq #0x151d494    -> ldr s1, [x9]
   so the store of s1 to +0x3c after them runs every frame (the flags are not read again, and x9 is reloaded at +0x151d4a4
   before any read). The fade steps by max(interval, 1) (main+0x583118) x 0.5 x 0.2;
   it now adds ratio x 0.2 (w0 from the helper is left as it is, nothing reads it after the conversion):
     main+0x125f118  ucvtf s0, w0       -> adrp x8, <data page>
     main+0x125f11c  fmov s1, #0.5      -> ldr s0, [x8]
     main+0x125f128  fmul s0, s0, s1    -> nop
   The UI's animation step is the same sum (main+0x1280ec8: max(interval, 1) x 0.5 into +0xb20 of the layout system,
   [main+0x1e374b8]); it now takes the ratio too (s1 keeps its 0.5; x9 is not read again before the call after the store):
     main+0x1280ecc  ucvtf s0, w0       -> adrp x9, <data page>
     main+0x1280ee4  fmul s0, s0, s1    -> ldr s0, [x9]
   Physics: the frame loop calls the physics step (main+0x156f8e8, from main+0x1281084) with the frame time, the frame
   ratio (VFR getter main+0x59c788, +0x3c) and the time speed, and the step runs round(6 x ratio) solver substeps
   (main+0x156f930): six at the shipped 30 fps, two at 120 fps. The ratio argument is now always 1.0, as in 1.6.0:
     main+0x128106c  fmov s9, s0        -> fmov s9, #1.0
   (s9 is read only by the fmov s1, s9 before the call); the frame time still sets the step.
   1.9.0's sead::ControllerBase::updateDerivativeParams_ (main+0xeb0ef0) counts held frames in floats: each held button adds
   s8, a constant 1.0 loaded once before the loop. s8 now comes from the page, the ratio, so counts, delays and pulses are
   1/30 s of game time at any frame rate without a tick:
     main+0xeb0f78  adrp x8, <GOT page> -> adrp x8, <data page>
     main+0xeb0f80  ldr x8, [x8, #0x9e0] -> nop                (the next ldr s8, [x8] reads the ratio)
   The pause menu's fast-repeat countdown (main+0xe67d00) already takes the VFR's delta frames off per frame in 1.9.0, so
   the patched ratio makes it real-time too. */
static void patch_pad_repeat(Module *m, uint64_t page) {
    const char *e = getenv("TOTK_PAD_REPEAT");
    if (!ios_code() && e && !strcmp(e, "0")) { fprintf(stderr, "[botw] pad repeat and hold counts per frame, as shipped (TOTK_PAD_REPEAT=0)\n"); return; }
    static const struct { uint32_t off, original; } check[] = {
        {0xeb0f78u, 0xd00075e8u}, {0xeb0f7cu, 0xaa1f03f5u}, {0xeb0f80u, 0xf944f108u}, {0xeb0f84u, 0x52800036u}, {0xeb0f88u, 0xbd400108u},
    };
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof check / sizeof *check; i++) {
        uint32_t w; memcpy(&w, m->base + check[i].off, 4);
        if (w != check[i].original) {
            fprintf(stderr, "[botw] pad repeat main+%#x: found %08x expected %08x\n", check[i].off, w, check[i].original);
            runtime_fail("pad repeat by real time requires the 1.9.0 sead::ControllerBase loop");
        }
    }
    const struct { uint32_t off, word; } put[] = {
        {0xeb0f78u, adrp_reg(8, (uintptr_t)m->base + 0xeb0f78, page)}, {0xeb0f80u, 0xd503201fu},
    };
    for (size_t i = 0; i < sizeof put / sizeof *put; i++) put_word(m, put[i].off, put[i].word, "real-time step");
    fprintf(stderr, "[botw] pad repeat and hold counts by real time: held buttons count the VFR ratio per frame (%zu instructions, "
                    "%zu verified; TOTK_PAD_REPEAT=0: one per frame, as shipped)\n", sizeof put / sizeof *put, sizeof check / sizeof *check);
}
static void patch_realtime(Module *m) {
    uint64_t data_off = (m->file_size + 0x3fff) & ~0x3fffULL;
    if (data_off + 0x4000 != m->size || data_off & 0xfff) runtime_fail("real-time frame-step page layout");
    static const struct { uint32_t off, original; } check[] = {
        {0x151d484u, 0x1e202020u}, {0x151d488u, 0x3900a268u}, {0x151d48cu, 0x54000040u}, {0x151d490u, 0xbd003e61u},
        {0x125f114u, 0x97cc9001u}, {0x125f118u, 0x1e230000u}, {0x125f11cu, 0x1e2c1001u}, {0x125f120u, 0xd0002208u},
        {0x125f124u, 0xbd42d662u}, {0x125f128u, 0x1e210800u}, {0x125f12cu, 0xbd417101u}, {0x125f130u, 0x1e210801u},
        {0x1280ec8u, 0x97cc0894u}, {0x1280eccu, 0x1e230000u}, {0x1280ed0u, 0xf0005db3u}, {0x1280ed4u, 0x9112e273u},
        {0x1280ed8u, 0x1e2c1001u}, {0x1280edcu, 0x2a1f03e0u}, {0x1280ee0u, 0xf9400268u}, {0x1280ee4u, 0x1e210800u},
        {0x1280ee8u, 0xbd0b2100u},
        {0x1281064u, 0x97cc6dc9u}, {0x1281068u, 0xf94002a0u}, {0x128106cu, 0x1e204009u}, {0x1281070u, 0x97ccd7ceu},
        {0x1281080u, 0x1e204121u}, {0x1281084u, 0x940bba19u},
    };
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof check / sizeof *check; i++) {
        uint32_t w; memcpy(&w, m->base + check[i].off, 4);
        if (w != check[i].original) {
            fprintf(stderr, "[botw] real-time frame step main+%#x: found %08x expected %08x\n", check[i].off, w, check[i].original);
            runtime_fail("real-time frame step requires the 1.9.0 VFR::calc instructions");
        }
    }
    const uint64_t page = (uintptr_t)m->base + data_off;
    const struct { uint32_t off, word; } put[] = {
        {0x151d484u, adrp_reg(9, (uintptr_t)m->base + 0x151d484, page)}, {0x151d48cu, 0xbd400121u},   /* ldr s1, [x9] */
        {0x125f118u, adrp_reg(8, (uintptr_t)m->base + 0x125f118, page)}, {0x125f11cu, 0xbd400100u},   /* ldr s0, [x8] */
        {0x125f128u, 0xd503201fu},
        {0x1280eccu, adrp_reg(9, (uintptr_t)m->base + 0x1280ecc, page)}, {0x1280ee4u, 0xbd400120u},   /* ldr s0, [x9] */
        {0x128106cu, 0x1e2e1009u},   /* fmov s9, #1.0: six physics substeps per frame at any frame rate */
    };
    for (size_t i = 0; i < sizeof put / sizeof *put; i++) put_word(m, put[i].off, put[i].word, "real-time step");
    frame_step_main = m->base;
    frame_step_data = (_Atomic uint64_t *)(void *)(m->base + data_off);
    atomic_store(frame_step_data, frame_step_bits(1.0f / 30.0f));
    fprintf(stderr, "[botw] real-time frame step: the VFR ratio, one interval-stepped fade and the UI animation step follow the measured frame time, physics keeps six substeps a frame "
                    "(%zu instructions, %zu verified); initial 33.33 ms, sampled at each present (TOTK_REALTIME=0: as shipped)\n",
            sizeof put / sizeof *put, sizeof check / sizeof *check);
    patch_pad_repeat(m, page);
}
#endif
/* Diagnostics (TOTK_JQ_CHECK=1): random crashes look like one job run twice (two threads in the same skinning job, a list
   item consumed twice). brk instructions replace a few instructions of sead::JobQueue::run (main+0x12c0dc0) and of the
   worker manager's wake (main+0x13a0150) and sync (main+0x13a03d0); the SIGTRAP handler (loader.c crash -> botw_jq_hook)
   does the replaced instruction and checks:
   - TAKE / DONE (job load, the instruction after its invoke): an index past the queue's count or total, and a job another
     thread is running at the same time;
   - SYNC_PRE / SYNC_POST (around the wait on a worker's idle event): the wait returning while that worker still runs a
     queue, with the event's state before the wait (2 = already signaled: a stale signal; 1 = woken while busy);
   - SYNC_END: any worker still busy or with queues in its ring when the sync returns (the assertion the release build
     compiled out: its loads of each worker's status are still there);
   - WAKE_SKIP: a worker with an empty ring whose idle event the wake leaves alone while that worker runs a queue. */
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
uint32_t runtime_thread_current_handle(void);
enum { JQ_TAKE, JQ_DONE, SYNC_PRE, SYNC_POST, SYNC_END, WAKE_SKIP, JQ_SITES };
static const struct { uint32_t off, word; const char *what; } g_jq_site[JQ_SITES] = {
    {0x12c1010u, 0xf8686920u, "job load (ldr x0, [x9, x8])"},
    {0x12c1020u, 0x9100075au, "after a job (add x26, x26, #1)"},
    {0x13a0438u, 0xaa1403e0u, "sync: before the idle wait (mov x0, x20)"},
    {0x13a0440u, 0x3607ff20u, "sync: after the idle wait (tbz w0, #0)"},
    {0x13a0508u, 0xb9009a7fu, "sync: end (str wzr, [x19, #0x98])"},
    {0x13a0378u, 0x34000169u, "wake: ring check (cbz w9)"},
};
static uint8_t *g_jq_main; static int g_jq_on;
static struct { _Atomic uint64_t thread, job, jq, since; _Atomic uint32_t index, handle; } g_jq_run[64];
static _Atomic uint64_t g_jq_n, g_jq_bad, g_jq_dup, g_sync_waits, g_sync_busy, g_sync_end_busy, g_wake_skip_busy;
static struct { uint32_t state, status, ring; uint64_t jq; } g_pre[64];   /* per thread slot (no __thread: its first use may allocate) */
/* Exclusive loads with acquire (TOTK_EXCL_ACQUIRE, botw.md): sead's spin locks in this game take the lock with a plain
   ldxr / stxr and only put a dmb ish before the release, so nothing orders the loads inside the critical section after the
   lock is taken. The ARMv8 model lets such a load be satisfied before the ldxr observes the lock free (a control dependency
   does not order loads); the Switch's Cortex-A57 rarely does it, Apple's cores do: two threads read the same "next job"
   index of a sead::JobQueue a few microseconds apart and run the same job twice (TOTK_JQ_CHECK). Every plain ldxr / ldxp
   that opens a real exclusive window (a store-exclusive on the same base register within 16 instructions) gets the acquire
   bit (ldaxr / ldaxp): one bit, the same registers and size, only stronger ordering. */
static int excl_ld(uint32_t w) { return (w & 0x3ffffc00u) == 0x085f7c00u || (w & 0xbfff8000u) == 0x887f0000u; }   /* ldxr{b,h} / ldxp */
static int excl_st(uint32_t w) { return (w & 0x3fe07c00u) == 0x08007c00u || (w & 0xbfe00000u) == 0x88200000u; }   /* st{l}xr{b,h} / st{l}xp */
static unsigned excl_acquire(uint8_t *code, size_t n, int write) {   /* write 0: only count what is left */
    unsigned done = 0; uint32_t *w = (uint32_t *)(void *)code; size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        if (!excl_ld(w[i])) continue;
        uint32_t rn = (w[i] >> 5) & 31;
        for (size_t j = i + 1; j < words && j <= i + 16; j++) if (excl_st(w[j]) && ((w[j] >> 5) & 31) == rn) { if (write) w[i] |= 0x8000u; done++; break; }
    }
    return done;
}
/* Docked render size (TOTK_RENDER_RES=900: as shipped). The display-mode routine (main+0x1821644, run when the operation
   mode is set, before the render targets are made) stores the screen width 1920.0 and the render width 1600.0 as one
   pair (+0x1290 / +0x1294 of the graphics system); the renderer scales its targets by render / screen width, so docked
   play draws the scene at 1600x900 and upscales it into the 1920x1080 canvas. 1920.0 makes it native 1080p: every scene
   target is made at 1920x1080 (and 960x540 for the half-size ones; pass dumps 2026-10-20) at the same frame rate on the
   Great Plateau. Above the screen size the game fails to make its heaps, and a 2560x1440 screen needs larger heaps than
   the game asks for (PlacementMgr fails after a 320 MB render budget at main+0x1804c44): not done.
   main+0x1821674 movk x8, #0x44c8, lsl #48 (1600.0) -> movk x8, #0x44f0, lsl #48 (1920.0).
   1.9.0's display-mode routine is main+0x1266074 (a jump on the mode byte +0x11be; docked and mode 2 store the same pair
   to +0x1210 / +0x1214 and crop 1920x1080; handheld stores 1280.0 / 1280.0 and is left as shipped):
   main+0x12660c8 movk x8, #0x44c8, lsl #48 -> movk x8, #0x44f0, lsl #48. */
#ifdef GAME_BOTW160
enum { RENDER_MOVK = 0x1821674 };
#else
enum { RENDER_MOVK = 0x12660c8 };
#endif
static void patch_render_1080(Module *m) {
    const char *e = getenv("TOTK_RENDER_RES");
    if (!ios_code() && e && !strcmp(e, "900")) { fprintf(stderr, "[botw] docked render size 1600x900, as shipped (TOTK_RENDER_RES=900)\n"); return; }
    static const struct { uint32_t off, original; } check[] = {
#ifdef GAME_BOTW160
        {0x1821670u, 0xd2a89e08u}, {0x1821674u, 0xf2e89908u}, {0x1821678u, 0xf9094b88u},
#else   /* mov x8, #0x44f00000 / mov x2, sp / mov w1, wzr / movk / str x8, [x19, #0x1210] */
        {0x12660bcu, 0xd2a89e08u}, {0x12660c0u, 0x910003e2u}, {0x12660c4u, 0x2a1f03e1u}, {0x12660c8u, 0xf2e89908u},
        {0x12660ccu, 0xf9090a68u},
#endif
    };
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof check / sizeof *check; i++) {
        uint32_t w; memcpy(&w, m->base + check[i].off, 4);
        if (w != check[i].original) {
            fprintf(stderr, "[botw] render size main+%#x: found %08x expected %08x\n", check[i].off, w, check[i].original);
            runtime_fail("1080p render size requires the " TOTK_GAME_VERSION " display-mode instructions");
        }
    }
    const uint32_t movk = 0xf2e89e08u;   /* movk x8, #0x44f0, lsl #48 */
    put_word(m, RENDER_MOVK, movk, "docked render size");
    fprintf(stderr, "[botw] docked render size 1920x1080 (shipped: 1600x900; TOTK_RENDER_RES=900 keeps it)\n");
}
/* The phone's render size (user 2026-10-23: "内部960×540，输出 用默认 放大机制"): the iPhone / iPad runs the handheld
   mode, whose branch of the 1.6.0 display-mode routine (jump table at main+0x26164f8: mode 1 -> main+0x18216c8) stores
   the screen width 1280.0 and the render width 1280.0 as one pair:
     main+0x18216c8  mov x8, #0x44a00000              (1280.0, the screen)
     main+0x18216cc  movk x8, #0x44a0, lsl #48        (1280.0, the render width) -> movk x8, #0x4470, lsl #48 (960.0)
   The renderer scales its targets by render / screen width, as docked play does at 1600 / 1920, so the scene is drawn at
   960x540 and the game's own upscale draws it into the 1280x720 canvas (MetalFX does not replace it on the phone:
   totk_mfx_up.m mfxup_enabled).
   1.9.0: the display-mode routine main+0x1266074 jumps by a byte table (main+0x1725916, base main+0x12660bc): mode 1 ->
   main+0x1266114, the same pair (str x8, [x19, #0x1210]); mode 3 (VR) computes its own and is left alone:
     main+0x1266114  mov x8, #0x44a00000
     main+0x1266118  movk x8, #0x44a0, lsl #48        -> movk x8, #0x4470, lsl #48 (960.0)
   (its dynamic-resolution scale, main+0x12661e8, divides the render width +0x1214 by 1280.0 in handheld: 0.75). */
#ifdef GAME_BOTW160
enum { PHONE_RENDER = 0x18216c8u, PHONE_RENDER_STR = 0xf9094b88u };   /* str x8, [x28, #0x1290] */
#else
enum { PHONE_RENDER = 0x1266114u, PHONE_RENDER_STR = 0xf9090a68u };   /* str x8, [x19, #0x1210] */
#endif
static void patch_phone_render(Module *m) {
    if (!ios_code()) return;
    const struct { uint32_t off, original; } check[] = {{PHONE_RENDER, 0xd2a89408u}, {PHONE_RENDER + 4, 0xf2e89408u}, {PHONE_RENDER + 8, PHONE_RENDER_STR}};
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof check / sizeof *check; i++) {
        uint32_t w; memcpy(&w, m->base + check[i].off, 4);
        if (w != check[i].original) {
            fprintf(stderr, "[botw] handheld render size main+%#x: found %08x expected %08x\n", check[i].off, w, check[i].original);
            runtime_fail("the phone's render size requires the " TOTK_GAME_VERSION " handheld display-mode instructions");
        }
    }
    put_word(m, PHONE_RENDER + 4, 0xf2e88e08u, "handheld render size");   /* movk x8, #0x4470, lsl #48: 960.0 */
    fprintf(stderr, "[botw] handheld render size 960x540, the game's own upscale to the 1280x720 canvas (phone)\n");
}
void runtime_game_patches(Module *m) {
    fprintf(stderr, "[botw] %s game-code patches\n", TOTK_GAME_VERSION);
    runtime_survey_init(m);   /* TOTK_SURVEY=1: survey pos / move (world_survey.c) */
    if (!patch_output_res(m)) patch_render_1080(m);
    patch_phone_render(m);
    botw_aspect_install(m);
    if (realtime_enabled()) patch_realtime(m);
    else fprintf(stderr, "[botw] real-time frame step off (TOTK_REALTIME=0): the game advances by its present interval\n");
    { const char *a = getenv("TOTK_EXCL_ACQUIRE");
      if (ios_code() || !a || strcmp(a, "0")) {
          const int w = !CODE_PATCHED_AT_BUILD;   /* the device: the export set them all, in the text and the patch area */
          unsigned t = excl_acquire(m->base, m->text_end, w), p = excl_acquire(m->base + m->patch_off, m->file_size - m->patch_off, w);
          if (!w && (t || p)) { fprintf(stderr, "[botw] %u + %u exclusive loads without acquire in the embedded code\n", t, p); runtime_fail("embedded main text was not patched at build time (ios/tools/build_ipa.py export-ios)"); }
          if (!w) fprintf(stderr, "[botw] exclusive loads with acquire: set at build time (iOS), none left\n"); else
          fprintf(stderr, "[botw] exclusive loads with acquire: %u in the code, %u in the rewriter's patch area (TOTK_EXCL_ACQUIRE=0: as shipped)\n", t, p);
      } else fprintf(stderr, "[botw] exclusive loads as shipped (TOTK_EXCL_ACQUIRE=0)\n"); }
    const char *e = getenv("TOTK_JQ_CHECK"); if (!e || !*e || !strcmp(e, "0") || ios_code()) return;
    for (int i = 0; i < JQ_SITES; i++) { uint32_t w; memcpy(&w, m->base + g_jq_site[i].off, 4);
        if (w != g_jq_site[i].word) { fprintf(stderr, "[botw] TOTK_JQ_CHECK: main+%#x is %08x, not %08x (%s): not armed\n", g_jq_site[i].off, w, g_jq_site[i].word, g_jq_site[i].what); return; } }
    for (int i = 0; i < JQ_SITES; i++) { uint32_t b = 0xd4200000u | ((0x7a1u + (uint32_t)i) << 5); memcpy(m->base + g_jq_site[i].off, &b, 4); }
    g_jq_main = m->base; g_jq_on = 1;
    fprintf(stderr, "[botw] TOTK_JQ_CHECK: job-queue and worker sync checks armed at %d sites\n", JQ_SITES);
}
static int jq_slot(uint64_t th) {
    for (int k = 0; k < 64; k++) if (atomic_load(&g_jq_run[k].thread) == th) return k;
    for (int k = 0; k < 64; k++) { uint64_t z = 0; if (atomic_compare_exchange_strong(&g_jq_run[k].thread, &z, th)) return k; }
    return -1;
}
static double jq_ms(uint64_t since) { static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb); return since ? (double)(mach_absolute_time() - since) * tb.numer / tb.denom / 1e6 : -1; }
#define U32(a) (*(const uint32_t *)(uintptr_t)(a))
#define U64(a) (*(const uint64_t *)(uintptr_t)(a))
#ifdef GAME_BOTW160
/* TOTK_PAD_LOG: at the controller loop's first instruction (x19 = the controller, w11 = the new hold mask): the load it
   replaced, and one line per update while a stick or d-pad direction (bits 16..23) is or was held: time, thread, the
   controller, the tick the loop will read, new and previous hold, the previous update's repeat bits, counts 16..23. */
static int botw_pad_hook(ucontext_t *u) {
    uint64_t *x = u->uc_mcontext->__ss.__x, c = x[19]; uint32_t hold = (uint32_t)x[11];
    x[8] = *(const uint32_t *)(uintptr_t)(c + 0x100);
    uint32_t old = *(const uint32_t *)(uintptr_t)(c + 0x114), rep = *(const uint32_t *)(uintptr_t)(c + 0x10);
    if ((hold | old) & 0xff0000u) {
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        atomic_store(&g_pad_dir_until, now + 300000000ull);
        const uint32_t *n = (const uint32_t *)(uintptr_t)(c + 0x30);
        const uint8_t *dl = (const uint8_t *)(uintptr_t)(c + 0xb0), *pu = (const uint8_t *)(uintptr_t)(c + 0xd0);
        fprintf(stderr, "[pad] %.4f update thread %#x ctrl %#llx tick %u hold %#x (was %#x) repeat %#x counts16-23 %u %u %u %u %u %u %u %u "
                        "delay/pulse16-23 %u/%u %u/%u %u/%u %u/%u %u/%u %u/%u %u/%u %u/%u\n",
                now * 1e-9, runtime_thread_current_handle(), (unsigned long long)c, pad_tick ? atomic_load(pad_tick) : 1, hold, old, rep,
                n[16], n[17], n[18], n[19], n[20], n[21], n[22], n[23], dl[16], pu[16], dl[17], pu[17], dl[18], pu[18], dl[19], pu[19],
                dl[20], pu[20], dl[21], pu[21], dl[22], pu[22], dl[23], pu[23]);
    }
    u->uc_mcontext->__ss.__pc += 4; return 1;
}
#endif
int botw_jq_hook(void *context) {   /* SIGTRAP from loader.c crash(): 1 = it was one of ours, handled */
#ifdef GAME_BOTW160
    if (g_pad_log_on && ((ucontext_t *)context)->uc_mcontext->__ss.__pc == (uintptr_t)g_pad_main + PAD_LOOP) return botw_pad_hook(context);
#endif
    if (!g_jq_on) return 0;
    ucontext_t *u = context; uint64_t pc = u->uc_mcontext->__ss.__pc, *x = u->uc_mcontext->__ss.__x;
    int site = -1; for (int i = 0; i < JQ_SITES; i++) if (pc == (uintptr_t)g_jq_main + g_jq_site[i].off) site = i;
    if (site < 0) return 0;
    uint64_t th = 0; pthread_threadid_np(NULL, &th); uint32_t h = runtime_thread_current_handle();
    uint64_t next_pc = pc + 4;
    switch (site) {
    case JQ_TAKE: {
        int k = jq_slot(th);
        uint64_t jq = x[21], job = U64(x[9] + x[8]); uint32_t i = (uint32_t)x[25];
        uint32_t count = U32(jq + 0x80), total = U32(jq + 0x90), next = U32(jq + 0x94);
        x[0] = job; atomic_fetch_add(&g_jq_n, 1);
        if ((i >= count || i >= total) && atomic_fetch_add(&g_jq_bad, 1) < 40)
            fprintf(stderr, "[jq-check] thread %#x runs index %u of queue %#llx: count %u total %u next %u end %lld (job %#llx)\n",
                    h, i, (unsigned long long)jq, count, total, next, (long long)x[22], (unsigned long long)job);
        for (int j = 0; j < 64; j++) if (j != k && job && atomic_load(&g_jq_run[j].job) == job && atomic_fetch_add(&g_jq_dup, 1) < 40)
            fprintf(stderr, "[jq-check] job %#llx index %u of queue %#llx started by thread %#x while thread %#x runs it (index %u of queue %#llx, for %.3f ms); count %u total %u next %u\n",
                    (unsigned long long)job, i, (unsigned long long)jq, h, atomic_load(&g_jq_run[j].handle), atomic_load(&g_jq_run[j].index),
                    (unsigned long long)atomic_load(&g_jq_run[j].jq), jq_ms(atomic_load(&g_jq_run[j].since)), count, total, next);
        if (k >= 0) { atomic_store(&g_jq_run[k].jq, jq); atomic_store(&g_jq_run[k].index, i); atomic_store(&g_jq_run[k].handle, h);
                      atomic_store(&g_jq_run[k].since, mach_absolute_time()); atomic_store(&g_jq_run[k].job, job); }
        break; }
    case JQ_DONE: {
        int k = jq_slot(th); x[26] += 1; if (k >= 0) atomic_store(&g_jq_run[k].job, 0);
        uint64_t n = atomic_load(&g_jq_n);
        if ((n & 0x3ffff) == 0)
            fprintf(stderr, "[jq-check] %llu jobs run, %llu past the count / total, %llu run twice at once; %llu sync waits, %llu returned while the worker ran, %llu syncs ended with a worker busy, %llu wakes left a busy worker's idle event\n",
                    (unsigned long long)n, (unsigned long long)atomic_load(&g_jq_bad), (unsigned long long)atomic_load(&g_jq_dup),
                    (unsigned long long)atomic_load(&g_sync_waits), (unsigned long long)atomic_load(&g_sync_busy),
                    (unsigned long long)atomic_load(&g_sync_end_busy), (unsigned long long)atomic_load(&g_wake_skip_busy));
        break; }
    case SYNC_PRE: {   /* x20 = worker + 0x170 (its idle event) */
        uint64_t w = x[20] - 0x170; int k = jq_slot(th);
        if (k >= 0) { g_pre[k].state = U32(x[20]); g_pre[k].status = U32(w + 0x100); g_pre[k].jq = U64(w + 0x130); g_pre[k].ring = U32(w + 0x120); }
        x[0] = x[20]; break; }
    case SYNC_POST: {
        uint64_t w = x[20] - 0x170; int k = jq_slot(th);
        if (x[0] & 1) {
            __typeof__(g_pre[0]) pre = {0}; if (k >= 0) pre = g_pre[k];
            atomic_fetch_add(&g_sync_waits, 1);
            uint32_t status = U32(w + 0x100); uint64_t jq = U64(w + 0x130);
            if ((status != 0 || jq != 0) && atomic_fetch_add(&g_sync_busy, 1) < 40)
                fprintf(stderr, "[jq-check] sync (thread %#x): the wait on worker %#llx (core %u) returned while it runs queue %#llx (status %u, ring %u, event %u); before the wait: event %u, status %u, queue %#llx, ring %u\n",
                        h, (unsigned long long)w, U32(w + 0xfc), (unsigned long long)jq, status, U32(w + 0x120), U32(x[20]),
                        pre.state, pre.status, (unsigned long long)pre.jq, pre.ring);
        } else next_pc = (uintptr_t)g_jq_main + 0x13a0424u;
        break; }
    case SYNC_END: {   /* x19 = the worker manager */
        uint64_t mgr = x[19]; uint32_t n = U32(mgr + 0x78); uint64_t arr = U64(mgr + 0x80);
        for (uint32_t i = 0; i < n && i < 8; i++) { uint64_t w = U64(arr + 8 * i); uint32_t status = U32(w + 0x100), ring = U32(w + 0x120); uint64_t jq = U64(w + 0x130);
            if ((status != 0 || ring != 0 || jq != 0) && atomic_fetch_add(&g_sync_end_busy, 1) < 40)
                fprintf(stderr, "[jq-check] sync (thread %#x) returns while worker %u (core %u) has status %u, queue %#llx, ring %u, idle event %u\n",
                        h, i, U32(w + 0xfc), status, (unsigned long long)jq, ring, U32(w + 0x170)); }
        *(uint32_t *)(uintptr_t)(mgr + 0x98) = 0; break; }
    case WAKE_SKIP: {  /* x20 = the worker, w9 = its ring count */
        uint64_t w = x[20];
        if ((uint32_t)x[9] == 0) {
            uint32_t status = U32(w + 0x100); uint64_t jq = U64(w + 0x130);
            if ((status != 0 || jq != 0) && atomic_fetch_add(&g_wake_skip_busy, 1) < 40)
                fprintf(stderr, "[jq-check] wake (thread %#x): worker core %u has an empty ring but runs queue %#llx (status %u, idle event %u): its idle event stays as it is\n",
                        h, U32(w + 0xfc), (unsigned long long)jq, status, U32(w + 0x170));
            next_pc = (uintptr_t)g_jq_main + 0x13a03a4u;
        }
        break; }
    }
    u->uc_mcontext->__ss.__pc = next_pc; return 1;
}
#undef U32
#undef U64
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }   /* no scene size above 1600x900 for the heaps (romfs.c) */
#ifdef GAME_BOTW160
/* Output size, 1.6.0: key 3 cycles off -> MetalFX 2K -> native 2K -> off, as on Tears of the Kingdom 1.4.3 (user
   2026-10-23). The game takes its TV size from two places (both found in the code): a function-local static {1920, 1080}
   at main+0x2cceb10 that six guarded initialisers write (the framework makes the swapchain from it once at the start; the
   system task's TV size, the dungeon map and the noise gauge read it), and the docked branch of the display-mode function
   (main+0x1821670..0x18216ac), which writes the screen and render widths (+0x1290: 1920.0 and the shipped 1600.0), the
   system task's TV size (+0x4c0, which Renderer::calc pushes to the TV display every frame) and the window crop.
   TOTK_OUTPUT_RES unset or auto (the default): the initialisers make a 2560x1440 swapchain, the docked branch takes its
   sizes from the data page after the module (+16 {screen, render width} floats, +24 {width, height}), starting at
   1920x1080 drawn in the swapchain's top-left (the window crop), and key 3 rewrites them and asks the display-mode
   function to apply the current mode again (+0x123d = +0x123e of *(main+0x2cceaf0), as the game itself does at
   main+0x1821624; it takes the request under its lock at main+0x1821644). The static follows the canvas after the start.
   <w>x<h>: that size from the start, fixed. 0: the shipped 1920x1080 swapchain, no switching (patch_render_1080 then
   renders the scene at 1080p). Heaps are fixed in the code (no HeapDef): render targets 115 -> 320 MiB, the graphics
   system (the swapchain) 48 -> 64 MiB, SetMemoryHeapSize and the root block +224 MiB, the host's application pool the
   same (runtime_output_res_extra_bytes, services.c); native 2K was played without a heap failure (user 2026-10-23). */
enum { OUT_OFF, OUT_AUTO, OUT_FIXED };
static int g_out_mode = -1;
static unsigned g_out_w, g_out_h;            /* the largest canvas: the swapchain and the heaps are made for it */
static _Atomic uint64_t g_out_cur;           /* the canvas now, w | h << 32 */
static _Atomic uint64_t *g_out_page;         /* the data page + 16: [0] {screen, render width} floats, [1] {width, height} */
static uint8_t *g_out_main;
static void output_parse(void) {
    if (g_out_mode >= 0) return;
    if (ios_code()) { g_out_mode = OUT_OFF; return; }   /* the phone: the game's own size (handheld 720p) */
    const char *e = getenv("TOTK_OUTPUT_RES"); unsigned a, b; char tail;
    if (e && !strcmp(e, "0")) { g_out_mode = OUT_OFF; return; }
    if (!e || !*e || !strcmp(e, "auto")) { g_out_mode = OUT_AUTO; g_out_w = 2560; g_out_h = 1440; return; }
    if (sscanf(e, "%ux%u%c", &a, &b, &tail) != 2 || a < 1280 || b < 720 || a > 7680 || b > 4320)
        runtime_fail("TOTK_OUTPUT_RES must be auto, 0 or <width>x<height> (1280x720 .. 7680x4320)");
    g_out_mode = OUT_FIXED; g_out_w = a; g_out_h = b;
}
int runtime_output_res(unsigned *w, unsigned *h) {
    output_parse();
    if (g_out_mode == OUT_OFF) return 0;
    if (w) *w = g_out_w;
    if (h) *h = g_out_h;
    return 1;
}
uint64_t runtime_output_size_current(void) { uint64_t c = atomic_load(&g_out_cur); return c ? c : 1920ull | (1080ull << 32); }
#define OUTPUT_EXTRA (224ull << 20)
uint64_t runtime_output_res_extra_bytes(void) { return runtime_output_res(NULL, NULL) ? OUTPUT_EXTRA : 0; }
static uint32_t movz_x(unsigned rd, unsigned imm16, unsigned hw) { return 0xd2800000u | (hw << 21) | (imm16 << 5) | rd; }
static uint32_t movk_x(unsigned rd, unsigned imm16, unsigned hw) { return 0xf2800000u | (hw << 21) | (imm16 << 5) | rd; }
static uint64_t output_floats(unsigned screen, unsigned render) {
    float a = (float)screen, b = (float)render; uint32_t x, y; memcpy(&x, &a, 4); memcpy(&y, &b, 4);
    return x | ((uint64_t)y << 32);
}
static void output_page(unsigned w, unsigned h) {   /* what the docked branch writes from its next run on */
    const char *e = getenv("TOTK_RENDER_RES");
    unsigned render = w == 1920 && h == 1080 && e && !strcmp(e, "900") ? 1600 : w;   /* the shipped 1600x900 scene at 1080p only */
    atomic_store(&g_out_page[0], output_floats(w, render));
    atomic_store(&g_out_page[1], w | ((uint64_t)h << 32));
    atomic_store(&g_out_cur, w | ((uint64_t)h << 32));
}
static void output_static(void) { uint64_t c = atomic_load(&g_out_cur); memcpy(g_out_main + 0x2cceb10, &c, 8); }
static int output_reapply(void) {
    uint64_t g; memcpy(&g, g_out_main + 0x2cceaf0, 8);
    if (!g) return 0;
    volatile uint8_t *gfx = (volatile uint8_t *)(uintptr_t)g;
    uint8_t mode = gfx[0x123e];
    if (mode != 0 && mode != 2) return 0;   /* docked only: handheld and VR keep their own sizes */
    gfx[0x123d] = mode;
    return 1;
}
static void output_switch(unsigned w, unsigned h) {
    output_page(w, h); output_static();
    int ok = output_reapply();
    fprintf(stderr, "[botw] output %ux%u%s\n", w, h, ok ? " (the display mode applies it at the next frame)" : ": the game is not in a docked mode now; it applies when it is");
}
/* At each present: once the framework has made the swapchain from the static, the static follows the canvas (the dungeon
   map and the noise gauge scale by it every call). */
static void botw_output_tick(void) {
    static int done;
    if (done || g_out_mode != OUT_AUTO || !g_out_page) return;
    done = 1; output_static();
}
static int patch_output_res(Module *m) {
    unsigned w, h; if (!runtime_output_res(&w, &h)) return 0;
    if (w > 0xffff || h > 0xffff) runtime_fail("TOTK_OUTPUT_RES too large");
    static const uint32_t size_init[] = {0xe563dc, 0xe56410, 0x113fb48, 0x1140208, 0x1186ce0, 0x1805028};
    static const struct { uint32_t off, original; } check[] = {
        {0x1821670, 0xd2a89e08}, {0x1821674, 0xf2e89908}, {0x1821678, 0xf9094b88}, {0x182167c, 0xf000a508}, {0x1821680, 0xf9408108},
        {0x1821684, 0xd280f009}, {0x1821688, 0xf2c08709}, {0x182168c, 0xf9026109}, {0x18216ac, 0xf8054109},
        {0x1804c44, 0x52a0e608}, {0xe550a0, 0x320807e0}, {0xe536dc, 0x52b86000}, {0xe53fa0, 0x52b82015},
    };
    uint32_t word;
    for (size_t i = 0; i < sizeof size_init / sizeof *size_init; i++) {
        memcpy(&word, m->base + size_init[i], 4); uint32_t k; memcpy(&k, m->base + size_init[i] + 12, 4);
        if (word != 0xd280f009u || k != 0xf2c08709u) { fprintf(stderr, "[botw] output size main+%#x: %08x / %08x\n", size_init[i], word, k); runtime_fail("the output size requires the 1.6.0 size initialisers"); }
    }
    for (size_t i = 0; !CODE_PATCHED_AT_BUILD && i < sizeof check / sizeof *check; i++) {
        memcpy(&word, m->base + check[i].off, 4);
        if (word != check[i].original) { fprintf(stderr, "[botw] output size main+%#x: found %08x expected %08x\n", check[i].off, word, check[i].original); runtime_fail("the output size requires the 1.6.0 display-mode and heap instructions"); }
    }
    uint64_t data_off = (m->file_size + 0x3fff) & ~0x3fffULL;
    if (data_off + 0x4000 != m->size) runtime_fail("the output size: no data page after the main module");
    const uint64_t page = (uintptr_t)m->base + data_off;
    g_out_main = m->base; g_out_page = (_Atomic uint64_t *)(void *)(page + 16);
    output_page(g_out_mode == OUT_AUTO ? 1920 : w, g_out_mode == OUT_AUTO ? 1080 : h);
    uint32_t mw = movz_x(9, w, 0), mh = movk_x(9, h, 2);   /* the swapchain at the largest size */
    for (size_t i = 0; i < sizeof size_init / sizeof *size_init; i++) { memcpy(m->base + size_init[i], &mw, 4); memcpy(m->base + size_init[i] + 12, &mh, 4); }
    const uint32_t branch[4] = {
        adrp_reg(9, (uintptr_t)m->base + 0x1821670, page),   /* adrp x9, page */
        0xf9400928u,                                         /* ldr x8, [x9, #16]: {screen, render width} */
        0xf9400d29u,                                         /* ldr x9, [x9, #24]: {width, height} (at 0x1821684) */
        0xd503201fu,                                         /* nop */
    };
    memcpy(m->base + 0x1821670, &branch[0], 4); memcpy(m->base + 0x1821674, &branch[1], 4);
    memcpy(m->base + 0x1821684, &branch[2], 4); memcpy(m->base + 0x1821688, &branch[3], 4);
    const uint32_t heap_render = 0x52a28008u;   /* mov w8, #0x14000000: 320 MiB */
    const uint32_t heap_gfx = 0x320603e0u;      /* mov w0, #0x4000000: 64 MiB */
    const uint32_t heap_size = 0x52ba2000u;     /* mov w0, #0xd1000000: SetMemoryHeapSize +224 MiB */
    const uint32_t heap_root = 0x52b9e015u;     /* mov w21, #0xcf000000: the root block +224 MiB */
    memcpy(m->base + 0x1804c44, &heap_render, 4); memcpy(m->base + 0xe550a0, &heap_gfx, 4);
    memcpy(m->base + 0xe536dc, &heap_size, 4); memcpy(m->base + 0xe53fa0, &heap_root, 4);
    fprintf(stderr, "[botw] output: a %ux%u swapchain, starting at %s; heaps: render 320 MiB, graphics 64 MiB, +224 MiB (23 instructions verified)%s\n",
            w, h, g_out_mode == OUT_AUTO ? "1920x1080" : "that size", g_out_mode == OUT_AUTO ? "; key 3: off -> MetalFX 2K -> native 2K" : "");
    return 1;
}
#else
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_size_current(void) { return 1920ull | (1080ull << 32); }   /* the canvas this game draws (frontend.m's shortcut text); no output-size patch yet */
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
static int patch_output_res(Module *m) { (void)m; return 0; }
static void botw_output_tick(void) {}
#endif
/* Pre-rendered movies (the memories, the intro): the movie player advances one movie frame per game frame, so above
   30 fps they ran fast, out of step with their sound, and a cutscene could wait for ever (2026-10-20). main+0x2cbef40
   holds the movie object while one plays (stored at main+0x1600e74 when it is made, cleared at main+0x1a4d6f0 after it
   is freed; the community "60 FPS + FMV Cutscenes Speed Fix" for 1.6.0 keys on the same word). While it is set the
   frame pacing holds the game at exactly 30 fps (hle_nvn.c game_hold_fps, the pacing of the 30+30 mode); the real-time
   step then gives the shipped ratio 1.0. runtime/movie_host.c hands it every movie frame (TOTK_MOVIE_PACE).
   TOTK_MOVIE_30=0 turns it off.
   1.9.0: main+0x1e565a8 is the same word, the instance pointer of the movie player (a 0x30-byte object whose +0x20 is
   the decoder and +0x28 its heap). The movie manager's start (main+0x13fc558) makes it through main+0x13fc9cc, which
   stores it at main+0x13fca24; its stop (main+0x13fc68c) frees it through main+0x13fca3c, which clears the word at
   main+0x13fca74 after the delete. */
#ifdef GAME_BOTW160
enum { MOVIE_WORD = 0x2cbef40 };
#else
enum { MOVIE_WORD = 0x1e565a8 };
#endif
int runtime_game_fps_limit(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_MOVIE_30"); on = !(e && !strcmp(e, "0")); }
    if (!on || !frame_step_main) return 0;
    uint64_t movie; memcpy(&movie, frame_step_main + MOVIE_WORD, 8);
    static int was; if (!!movie != was) { was = !!movie; fprintf(stderr, "[botw] movie %s: frame pacing %s\n", was ? "starts" : "ends", was ? "held at 30 fps" : "back to normal"); }
    return movie ? 30 : 0;
}
/* The settings panel's picture (runtime.h OUTPUT_MODE_*). MetalFX 2K: MetalFX replaces the game's own upscale of the
   scene into the 1080p canvas (nvn_mtl.m mfxup_dock, BOTW_MFXUP_DOCK_KEY) and upscales the canvas into a window or full
   screen bigger than 1080p at the present (the output stage). 1.6.0 with a 2K-capable start (TOTK_OUTPUT_RES auto, the
   default) also offers native 2K (the canvas and the scene at 2560x1440, output_switch) and 720p (both 1280x720);
   otherwise off and MetalFX 2K. */
static int output_switchable(void) {
#ifdef GAME_BOTW160
    return g_out_mode == OUT_AUTO && g_out_page;
#else
    return 0;
#endif
}
static unsigned output_width_now(void) {
#ifdef GAME_BOTW160
    return (unsigned)(atomic_load(&g_out_cur) & 0xffffffffu);
#else
    return 1920;
#endif
}
/* No 4K here yet: the heaps are patched in the code for the 2K canvas (patch_output_res). */
int runtime_output_native(int mode, unsigned *w, unsigned *h) {
#ifdef GAME_BOTW160
    if ((mode == OUTPUT_MODE_NATIVE || mode == OUTPUT_MODE_NATIVE_METALFX) && g_out_mode == OUT_AUTO && g_out_page) { *w = g_out_w; *h = g_out_h; return 1; }
#endif
    (void)mode; (void)w; (void)h; return 0;
}
int runtime_output_modes(void) {
    unsigned w, h;
    return 1 << OUTPUT_MODE_OFF | 1 << OUTPUT_MODE_METALFX | (output_switchable() ? 1 << OUTPUT_MODE_720P : 0) |
           (runtime_output_native(OUTPUT_MODE_NATIVE, &w, &h) ? 1 << OUTPUT_MODE_NATIVE | 1 << OUTPUT_MODE_NATIVE_METALFX : 0);
}
int runtime_output_mode(void) {
    extern int mfxup_enabled(void);
    if (output_switchable()) {
        unsigned w = output_width_now();
        if (w == 1280) return OUTPUT_MODE_720P;
        if (w > 1920) return mfxup_enabled() ? OUTPUT_MODE_NATIVE_METALFX : OUTPUT_MODE_NATIVE;
    }
    return mfxup_enabled() ? OUTPUT_MODE_METALFX : OUTPUT_MODE_OFF;
}
int runtime_output_mode_set(int mode) {
    extern void dm_gfx_set_flag(const char *name, int value);
    if (mode < OUTPUT_MODE_OFF || mode > OUTPUT_MODE_720P || !(runtime_output_modes() & 1 << mode)) return 0;
    int mfx = mode == OUTPUT_MODE_METALFX || mode == OUTPUT_MODE_NATIVE_METALFX;   /* 2K + MetalFX: the output stage scales the 2K canvas to the window */
    dm_gfx_set_flag("mfx_output", mfx);
    dm_gfx_set_flag("mfx_upscale", mfx);
#ifdef GAME_BOTW160
    if (output_switchable()) {
        unsigned w = 1920, h = 1080;
        if (mode == OUTPUT_MODE_720P) { w = 1280; h = 720; } else if (mode == OUTPUT_MODE_NATIVE || mode == OUTPUT_MODE_NATIVE_METALFX) { w = g_out_w; h = g_out_h; }
        if (output_width_now() != w) output_switch(w, h);
    }
#endif
    fprintf(stderr, "[botw] picture: %s\n", mode == OUTPUT_MODE_NATIVE ? "native 2K" : mode == OUTPUT_MODE_NATIVE_METALFX ? "native 2K, MetalFX to the window" : mode == OUTPUT_MODE_720P ? "720p" :
            mode == OUTPUT_MODE_METALFX ? "MetalFX 2K" : "off (1080p)");
    return 1;
}
/* Key 4 (runtime/input.c): ultrawide on / off (TOTK_ASPECT, default 21:9; also 32:9, 3440x1440 or 2.39; TOTK_ASPECT set
   also starts with it on). The game keeps drawing into its 16:9 canvas with the main camera's aspect ratio set to the wider
   one, so the picture holds the wider view squeezed horizontally; the present stretches it over the window
   (nvn_mtl.m present_stretch) and the UI drawn after the scene is squeezed back to the middle 16:9 (uw_ui_squeeze).
   The main camera's projection is a sead::PerspectiveProjection (1.9.0: vtable main+0x1d05380; aspect +0xb0, dirty +8) at +0x38
   in a singleton made at main+0x10ed664 (pointer main+0x1e39d10: two views at +0x28 and +0x118, each with its projection
   at +0x10). The game sets that aspect once, when the singleton is made, and recomputes the matrix every frame from the
   fields (the near plane changes per frame), so the ratio is written into the field and the projection marked dirty: at
   the next present after the key, and at every present while it is on (a new setup would write 16:9 again). Off puts
   16:9 back. Only a 16:9 value is replaced (the constructor's 4:3 before the setup is left alone); a main camera of
   another class stops the game. Mac only.
   1.6.0 (2026-10-23): the same singleton, made inline after the "Camera" heap at main+0x1838a6c..0x1838be4 (the same
   0x208-byte object, views at +0x28 / +0x118, the projection vtable stored at +0x38 and +0x128, the 4:3 aspect at
   +0xe8, the dirty halfword at +0x40), pointer main+0x2cc4da0, projection vtable main+0x2bb4d48. */
#ifdef GAME_BOTW190
#define UW_CAMERA_SINGLETON 0x1e39d10
#define UW_PERSPECTIVE_VTABLE 0x1d05380
#else
#define UW_CAMERA_SINGLETON 0x2cc4da0
#define UW_PERSPECTIVE_VTABLE 0x2bb4d48
#endif
static uint8_t *g_uw_main;
static float g_uw_wide;
static float g_uw_start;   /* TOTK_ASPECT's ratio, 0 without it */
static _Atomic(uint32_t) g_uw_on;
static int g_uw_written;   /* the field holds the wide ratio this code wrote (present thread only) */
static _Atomic int g_uw_effective;   /* the wide ratio stayed in the field from one present to the next: the picture is wide */
static float aspect_requested(void) {   /* TOTK's parser (aspect_math.h); "display" is not taken here */
    double a = aspect_parse(getenv("TOTK_ASPECT"));
    if (a < 0) runtime_fail("TOTK_ASPECT must be <w>:<h>, <w>x<h> or a ratio between 0.5 and 8");
    return (float)a;
}
#ifdef GAME_BOTW160
/* 1.6.0 writes the main projection's aspect every frame (2026-10-23: a write of ours was undone 600 times in 10 s, the
   picture flickered between wide and 16:9): the camera's update main+0x15c2030 pushes the active camera into the
   singleton's views and stores the layer rectangle's width / height at main+0x15c214c (str s0, [x19, #0xe8]; x19 the
   singleton, +0xe8 = projection +0x38 + aspect +0xb0). The straight run main+0x15c2118..0x15c214c (no branch lands in it)
   is rewritten so that store takes a float from the data page after the module (+0x20) when it is not 0.0, else the
   game's own ratio as shipped: near / far copied with one ldp / stp, the three repeated dirty-byte stores before the
   aspect folded into one, x9 / s1..s3 / flags free there. Key 4 then only sets that word (g_uw_word). */
#define UW_WRITER_AT 0x15c2118u
static const uint32_t uw_writer_original[] = {0x39010277, 0xb9406ec8, 0xb900d268, 0x39010277, 0xb94072c8, 0xb900d668, 0x39010277,
                                              0x2d4806a0, 0x2d470ea2, 0x39010277, 0x1e233821, 0x1e223800, 0x1e211800, 0xbd00ea60};
static void uw_writer_words(uint32_t *out, uint32_t adrp) {
    const uint32_t patched[] = {
        0x39010277,   /* strb w23, [x19, #0x40]       dirty */
        0x294da6c8,   /* ldp w8, w9, [x22, #0x6c]     near, far */
        0x291a2668,   /* stp w8, w9, [x19, #0xd0] */
        0x2d4806a0,   /* ldp s0, s1, [x21, #0x40]     the layer rectangle */
        0x2d470ea2,   /* ldp s2, s3, [x21, #0x38] */
        0x1e233821,   /* fsub s1, s1, s3 */
        0x1e223800,   /* fsub s0, s0, s2 */
        0x1e211800,   /* fdiv s0, s0, s1              the game's ratio */
        adrp,         /* adrp x8, <data page> */
        0xbd402101,   /* ldr s1, [x8, #0x20]          ours, 0.0 = none */
        0x1e202028,   /* fcmp s1, #0.0 */
        0x1e201c20,   /* fcsel s0, s1, s0, ne */
        0x39010277,   /* strb w23, [x19, #0x40]       dirty */
        0xbd00ea60,   /* str s0, [x19, #0xe8]         the aspect */
    };
    memcpy(out, patched, sizeof patched);
}
#else
/* 1.9.0 keeps a written ratio at the title but writes the aspect every frame in the world too (2026-10-25, iPhone 14:
   from the first world frames the picture flickered between full screen and 720p, as 1.6.0 did). Its camera update
   main+0x10eda5c holds the projection at x19 + 0x10 (aspect +0xc0 = projection +0xb0, dirty byte +0x18) and after the
   fovy call stores near / far and the layer rectangle's width / height in the straight run main+0x10edae0..0x10edb10
   (13 words, no branch lands in it, x8 / x9 / s1..s3 / flags free before the return). The same rewrite: near / far with
   one ldp / stp, one dirty-byte store, the aspect from the data page (+0x20) when it is not 0.0. */
#define UW_WRITER_AT 0x10edae0u
static const uint32_t uw_writer_original[] = {0xbd406e80, 0x39006276, 0xbd00aa60, 0xbd407280, 0x39006276, 0xbd00ae60, 0x2d4806a0,
                                              0x2d470ea2, 0x39006276, 0x1e233821, 0x1e223800, 0x1e211800, 0xbd00c260};
static void uw_writer_words(uint32_t *out, uint32_t adrp) {
    const uint32_t patched[] = {
        0x39006276,   /* strb w22, [x19, #0x18]       dirty */
        0x294da688,   /* ldp w8, w9, [x20, #0x6c]     near, far */
        0x29152668,   /* stp w8, w9, [x19, #0xa8] */
        0x2d4806a0,   /* ldp s0, s1, [x21, #0x40]     the layer rectangle */
        0x2d470ea2,   /* ldp s2, s3, [x21, #0x38] */
        0x1e233821,   /* fsub s1, s1, s3 */
        0x1e223800,   /* fsub s0, s0, s2 */
        0x1e211800,   /* fdiv s0, s0, s1              the game's ratio */
        adrp,         /* adrp x8, <data page> */
        0xbd402101,   /* ldr s1, [x8, #0x20]          ours, 0.0 = none */
        0x1e202028,   /* fcmp s1, #0.0 */
        0x1e201c20,   /* fcsel s0, s1, s0, ne */
        0xbd00c260,   /* str s0, [x19, #0xc0]         the aspect */
    };
    memcpy(out, patched, sizeof patched);
}
#endif
#define UW_WRITER_N (sizeof uw_writer_original / sizeof *uw_writer_original)
static _Atomic uint32_t *g_uw_word;
static void botw_aspect_writer(Module *m) {
    for (unsigned i = 0; !CODE_PATCHED_AT_BUILD && i < UW_WRITER_N; i++) {
        uint32_t w; memcpy(&w, m->base + UW_WRITER_AT + 4 * i, 4);
        if (w != uw_writer_original[i]) { fprintf(stderr, "[botw] ultrawide main+%#x: found %08x expected %08x\n", UW_WRITER_AT + 4 * i, w, uw_writer_original[i]); runtime_fail("ultrawide requires this game version's camera update"); }
    }
    uint64_t data_off = (m->file_size + 0x3fff) & ~0x3fffULL;
    if (data_off + 0x4000 != m->size) runtime_fail("ultrawide: no data page after the main module");
    const uint64_t page = (uintptr_t)m->base + data_off;
    uint32_t patched[UW_WRITER_N];
    uw_writer_words(patched, adrp_reg(8, (uintptr_t)m->base + UW_WRITER_AT + 32, page));
    put_words(m, UW_WRITER_AT, patched, UW_WRITER_N, "ultrawide aspect writer");
    g_uw_word = (_Atomic uint32_t *)(void *)(page + 0x20);
}
static void botw_aspect_word(int on) { float v = on ? g_uw_wide : 0.0f; uint32_t b; memcpy(&b, &v, 4); if (g_uw_word) atomic_store(g_uw_word, b); }
static float g_uw_view;                  /* runtime_aspect_view, below */
static void botw_aspect_phone(float a);
static void botw_aspect_install(Module *m) {
    float start = aspect_requested();
    g_uw_wide = start ? start : (float)ASPECT_WIDE; g_uw_start = start;
    g_uw_main = m->base; atomic_store(&g_uw_on, start != 0);
    botw_aspect_writer(m); botw_aspect_word(start != 0);
    fprintf(stderr, "[botw] ultrawide: the settings panel picks 16:9 / 16:10 / 21:9 / 32:9, starts %s (%.4f)\n", start ? "wide" : "at 16:9", g_uw_wide);
    if (g_uw_view > 0) botw_aspect_phone(g_uw_view);   /* the phone's screen was reported first */
}
static void botw_aspect_apply(void) {
    if (!g_uw_main) return;
    uint64_t s; memcpy(&s, g_uw_main + UW_CAMERA_SINGLETON, 8);
    if (!s) { static int told; if (atomic_load(&g_uw_on) && !told++) fprintf(stderr, "[botw] ultrawide: the main camera is not made yet\n"); return; }   /* the camera singleton is not made yet */
    uint8_t *proj = (uint8_t *)(uintptr_t)(s + 0x38);
    uint64_t vt; memcpy(&vt, proj, 8);
    if (vt != (uintptr_t)g_uw_main + UW_PERSPECTIVE_VTABLE) runtime_fail("ultrawide: the main camera is not a sead::PerspectiveProjection in this game version");
    float now; memcpy(&now, proj + 0xb0, 4);
    const float game = 16.0f / 9.0f;
    if (atomic_load(&g_uw_on)) {
        if (now == g_uw_wide) { atomic_store(&g_uw_effective, 1); return; }
        if (fabsf(now - game) > 1e-4f) {   /* not set up yet (the constructor's 4:3) or another ratio: left as it is */
            static float told; if (now != told) { told = now; fprintf(stderr, "[botw] ultrawide: main camera aspect %g, not 16:9: left as it is\n", now); }
            return;
        }
        atomic_store(&g_uw_effective, 0);
        memcpy(proj + 0xb0, &g_uw_wide, 4);
        static unsigned rewrites;   /* the game set 16:9 again after a write of ours: it sets the aspect each frame here */
        if (!g_uw_written && !rewrites) fprintf(stderr, "[botw] ultrawide: main camera %p, aspect %g -> %g\n", (void *)proj, now, g_uw_wide);
        else if (g_uw_written && (++rewrites == 1 || rewrites % 600 == 0)) fprintf(stderr, "[botw] ultrawide: the game set 16:9 again (%u times)\n", rewrites);
        g_uw_written = 1;
    } else {
        if (!g_uw_written || now != g_uw_wide) return;   /* only the ratio this code wrote is put back */
        memcpy(proj + 0xb0, &game, 4); g_uw_written = 0; atomic_store(&g_uw_effective, 0);
    }
    proj[8] = 1;   /* dirty: the matrix is rebuilt from the fields */
}
/* The present stretches the canvas only while the camera really draws wide (g_uw_effective): a ratio the game puts back,
   or a camera not made yet, leaves the picture 16:9 instead of stretching it (2026-10-23, 1.6.0 stretched). */
float runtime_aspect_now(void) { return g_uw_main && atomic_load(&g_uw_on) && atomic_load(&g_uw_effective) ? g_uw_wide : 0; }
/* The settings panel (frontend.m, which also gives the window that shape): a ratio, 0.5..8; within 1% of 16:9 the
   game's own. The camera writer takes the new ratio from the data page at the next frame, also from one wide ratio to
   another. */
int runtime_aspect_set_ratio(float r) {
    if (!g_uw_main || !(r >= 0.5f && r <= 8)) return 0;
    int on = !aspect_near(r, ASPECT_GAME);
    if (on) g_uw_wide = r;
    atomic_store(&g_uw_on, on); botw_aspect_word(on);
    fprintf(stderr, "[botw] aspect: %s (%.4f)\n", on ? "wide" : "16:9 (the game's own)", on ? g_uw_wide : ASPECT_GAME);
    return 1;
}
float runtime_aspect_ratio(void) { return g_uw_main ? (atomic_load(&g_uw_on) ? g_uw_wide : (float)ASPECT_GAME) : 0; }
float runtime_aspect_start(void) { return g_uw_start; }
/* iPhone (frontend_ios.m reports the drawable once the view is made): the phone fills its screen as Tears of the Kingdom
   does there (aspect_patch.h, user 2026-10-23 "iphone 全屏"): the wide ratio becomes the screen's shape (2.16 on an
   iPhone 14) and is on from the start, so the camera draws that view into the 1280x720 canvas, the present widens it over
   the screen and the UI is squeezed back to 16:9 (uw_ui_squeeze). No extra memory. A screen not wider than 16:9 (an iPad)
   keeps the game's 16:9 with bars; TOTK_ASPECT=0 (or 16:9) keeps it everywhere. Mac: the settings panel picks the ratio. */
/* g_uw_view: the drawable's shape, which may come before the main module is patched (botw_aspect_install takes it then). */
static void botw_aspect_phone(float a) {
#if TARGET_OS_IPHONE
    const char *e = getenv("TOTK_ASPECT");
    if ((e && (!strcmp(e, "0") || !strcmp(e, "16:9"))) || !(a > 16.0f / 9.0f * 1.01f) || a > 8.0f) {
        fprintf(stderr, "[botw] screen shape %.4f: the game's 16:9%s\n", a, e && *e ? " (TOTK_ASPECT)" : ""); return;
    }
    g_uw_wide = a; atomic_store(&g_uw_on, 1); botw_aspect_word(1);
    fprintf(stderr, "[botw] full screen: the camera takes the screen's shape %.4f (TOTK_ASPECT=0: 16:9)\n", a);
#else
    (void)a;
#endif
}
void runtime_aspect_view(double w, double h) {
    if (!(w > 0 && h > 0)) return;
    g_uw_view = (float)(w / h);
    if (g_uw_main) botw_aspect_phone(g_uw_view);
}
