// TOTK: the game's states for Apple's performance tools on macOS 27 (StateReporting; TOTK_STATE_REPORTING=1, off by
// default; docs/metal-performance-diagnostics.md). See totk_state_report.m. Mac only: elsewhere every call is a no-op.
#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <TargetConditionals.h>

/* The graphics configuration in force: what the renderer reads every frame, not what a panel asked for. Built on the game
   thread once per game frame while reporting is on and compared with the last one. */
typedef struct {
    int out_w, out_h;              // the game picture: the crop of the last game frame (present_encode)
    int aspect_milli;              // the aspect ratio picked, x1000 (0: the game's 16:9)
    int upscale, taa, fi_mode;     // MetalFX upscaling (0 off, 1 scene, 2 scene and window output), edge smoothing, frame interpolation (MFX_*)
    int temporal;                  // MetalFX temporal upscaling at the game's upscale draw (TOTK_MFX_TEMPORAL)
    int cap, cap_src, own_clock;   // frame-rate cap in fps (0: none), why (SR_CAP_*), a held rate paced by the game's own clock
    int paced_hz, present_path;    // the display rate the pacing uses; 0 display link, 1 present thread, 2 present thread after a display-link stall
    int view_w, view_h;            // the window's drawable in pixels (main thread; stable metadata only, not in the label)
    int display_hz, fullscreen;    // the refresh rate of the window's screen, full screen (main thread)
} totk_sr_gfx;
enum { SR_CAP_GAME, SR_CAP_UNCAPPED, SR_CAP_UNCAPPED_INTERVAL, SR_CAP_LOCK, SR_CAP_INTERP, SR_CAP_MOVIE, SR_CAP_PHONE, SR_CAP_NONE, SR_CAP_DISPLAY };
/* The game's counters, sent with every state change: the report takes their change over a state. */
typedef struct { uint64_t game_frames, generated, merged, translations, libraries, pipelines, create_ns, metal_bytes; int scene_w, scene_h; } totk_sr_counters;

#if TARGET_OS_OSX
extern _Atomic int g_totk_sr_on;   // set while reports are made; the hooks read it (one load) and do nothing else when 0
static inline int totk_sr_on(void) { return atomic_load_explicit(&g_totk_sr_on, memory_order_relaxed); }
void totk_sr_set(int on);          // flag state_reporting 0|1 (input file): on and off within one session, for A/B runs
void totk_sr_start(const char *game, const char *version);   // window opening (runtime/frontend.m, main thread)
void totk_sr_warmup(int archive_valid);                       // the start-up shader / pipeline warm-up begins
void totk_sr_picture(int key, int visible);                   // the start-up screen is gone: the game picture shows (main thread)
void totk_sr_window(int key, int visible);                    // the window gained or lost the keyboard, or was covered (main thread)
void totk_sr_settings(int open);                              // the settings panel opened or closed (main thread)
void totk_sr_view(int w, int h, int display_hz, int fullscreen);   // drawable size, the screen's rate, full screen (main thread)
void totk_sr_closing(void);                                   // the window closes: returns once the report is made (bounded)
/* hle_nvn.c, game thread, once per game frame, only while totk_sr_on(): this frame's cap and its display ticks. */
void totk_sr_frame(int cap, int cap_src, int own_clock, int paced_hz, unsigned ticks);
void mtl_sr_picture(totk_sr_gfx *s, uint64_t *upscaled_at, uint64_t *antialiased_at);   // nvn_mtl.m, from its atomics (any thread)
void mtl_sr_counters(totk_sr_counters *c);                    // nvn_mtl.m (any thread)
#else
static inline int totk_sr_on(void) { return 0; }
static inline void totk_sr_set(int on) { if (on) fprintf(stderr, "[dm] [nvn-mtl] [state] state_reporting: Mac only (macOS 27)\n"); }
static inline void totk_sr_warmup(int archive_valid) { (void)archive_valid; }
static inline void totk_sr_frame(int cap, int cap_src, int own_clock, int paced_hz, unsigned ticks) { (void)cap; (void)cap_src; (void)own_clock; (void)paced_hz; (void)ticks; }
#endif
