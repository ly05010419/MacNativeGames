#include "game_patches.h"
#include "game_version.h"
#include <stdint.h>
#include <stdio.h>

/* Mario Kart 8 Deluxe 4.0.1: no game-code patches. The game boots and plays without one (nxnative's MK8D profile needs
   none either), so main runs as rewritten. A later patch must name main's build ID and check each site's original
   instruction before writing it, as CONTRIBUTING.md asks. */
void runtime_game_patches(Module *main_module) {
    (void)main_module;
    fprintf(stderr, "[mk8d] game patches: none\n");
}
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_render_pool_extra(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
const char *runtime_picture_name(void) { return NULL; }   /* key 3 is MetalFX on / off: the game renders its own 1080p */
/* Key 3 (runtime/input.c): MetalFX upscaling of the game's picture to the window, on / off (nvn_mtl.m output_upscale;
   nothing happens while the window is not bigger than the picture). */
int runtime_output_mode_cycle(void) {
    extern int mfxup_enabled(void);
    extern void dm_gfx_set_flag(const char *name, int value);
    int next = !mfxup_enabled();
    dm_gfx_set_flag("mfx_output", next);
    dm_gfx_set_flag("mfx_upscale", next);
    fprintf(stderr, "[mk8d] key 3: %s\n", next ? "MetalFX upscales the picture to the window" : "MetalFX off");
    return 1;
}
/* Key 4 (aspect ratio) needs the game's camera projections; MK8D has none wired yet, so it reports itself unavailable. */
float runtime_aspect_now(void) { return 0; }
int runtime_aspect_step(void) { return -1; }
void runtime_aspect_view(double w, double h) { (void)w; (void)h; }
int runtime_aspect_toggle(void) { return 0; }
