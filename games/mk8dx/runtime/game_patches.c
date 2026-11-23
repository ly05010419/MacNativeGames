/* Mario Kart 8 Deluxe 4.0.1: game-code patches and the picture keys' game side.

   The bring-up runs the game as shipped: no patch is applied to main until one is verified (each site checked against the
   original instruction word first, as CONTRIBUTING.md asks, and recorded in CHANGELOG.md). Odyssey's render size (key 3)
   and camera aspect ratio (key 4) patches are that game's own addresses, so they are not carried over: key 3 is MetalFX on /
   off for the output, key 4 is unavailable, the docked picture is the game's own 1920x1080. */
#include "game_patches.h"
#include "game_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void runtime_game_patches(Module *main_module) {
    (void)main_module;
    fprintf(stderr, "[" GAME_LOG "] game patches: none (%s %s)\n", GAME_NAME, TOTK_GAME_VERSION);
}

void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
uint64_t runtime_render_pool_extra(void) { return 0; }
const char *runtime_picture_name(void) { return NULL; }
/* The scene is the game's own docked 1920x1080; the panel offers no other size. */
int runtime_scene_resolution_max(unsigned *w, unsigned *h) { *w = 1920; *h = 1080; return 1; }
int runtime_scene_resolution(unsigned *w, unsigned *h) { return runtime_scene_resolution_max(w, h); }
int runtime_scene_resolution_set(unsigned w, unsigned h) {
    if (w == 1920 && h == 1080) return 1;
    fprintf(stderr, "[" GAME_LOG "] settings: scene resolution %ux%u refused (the game's own 1920x1080 only)\n", w, h);
    return 0;
}
/* Key 3 (runtime/input.c): MetalFX upscaling of the picture to the window, on / off. */
int runtime_output_mode_cycle(void) {
    extern int mfxup_enabled(void);
    extern void dm_gfx_set_flag(const char *name, int value);
    int next = !mfxup_enabled();
    dm_gfx_set_flag("mfx_output", next);
    dm_gfx_set_flag("mfx_upscale", next);
    fprintf(stderr, "[" GAME_LOG "] key 3: %s\n", next ? "MetalFX upscales the picture to the window" : "MetalFX off");
    return 1;
}

/* Key 4 (aspect ratio): not available until the game's camera projections are found and verified. */
float runtime_aspect_ratio(void) { return 0; }
float runtime_aspect_start(void) { return 0; }
int runtime_aspect_set_ratio(float ratio) { (void)ratio; return 0; }
float runtime_aspect_now(void) { return 0; }
int runtime_aspect_step(void) { return -1; }
void runtime_aspect_view(double w, double h) { (void)w; (void)h; }
int runtime_aspect_toggle(void) { return 0; }
