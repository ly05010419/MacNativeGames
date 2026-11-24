/* Dark Souls Remastered 1.0.2: 60 FPS patch (TOTK_FPS60=0 disables).
   Two verified instructions in main (matching masagrator's Switch 60 FPS patch, mapped to 1.0.2):
     main+0x712510: mov w1, #2 (0x321f0fe1) -> mov w1, #1 (0x321e0fe1)  [target frame interval 1 vsync = 60 Hz]
     main+0x6729d4: ldr s8, [x8, #0x5ec] (0xbd45ed08, 1/30 s) -> ldr s8, [x8, #0x758] (0xbd475908, 1/60 s) [timestep] */
#include "game_patches.h"
#include "game_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t offset; uint32_t original, replacement; const char *what; } GamePatch;

static const GamePatch dsr_fps60_sites[] = {
    {0x00712510, 0x321f0fe1, 0x321e0fe1, "mov w1, #2 -> mov w1, #1 (60 Hz frame pacing)"},
    {0x006729d4, 0xbd45ed08, 0xbd475908, "ldr s8, [x8, #0x5ec] (1/30) -> [x8, #0x758] (1/60)"},
};

void runtime_game_patches(Module *main_module) {
    const char *e = getenv("TOTK_FPS60");
    if (e && !strcmp(e, "0")) {
        fprintf(stderr, "[" GAME_LOG "] 60 FPS patch off (TOTK_FPS60=0): shipping 30 FPS (%s %s)\n", GAME_NAME, TOTK_GAME_VERSION);
        return;
    }
    if (!main_module || !main_module->base) return;
    const size_t n = sizeof dsr_fps60_sites / sizeof dsr_fps60_sites[0];
    for (size_t i = 0; i < n; i++) {
        uint32_t word = 0;
        memcpy(&word, main_module->base + dsr_fps60_sites[i].offset, 4);
        if (word != dsr_fps60_sites[i].original) {
            fprintf(stderr, "[" GAME_LOG "] 60 FPS patch skipped: main+%#llx found %08x expected %08x (%s)\n",
                    (unsigned long long)dsr_fps60_sites[i].offset, word, dsr_fps60_sites[i].original, dsr_fps60_sites[i].what);
            return;
        }
    }
    for (size_t i = 0; i < n; i++)
        memcpy(main_module->base + dsr_fps60_sites[i].offset, &dsr_fps60_sites[i].replacement, 4);
    fprintf(stderr, "[" GAME_LOG "] 60 FPS patch applied: %zu instructions (%s %s)\n", n, GAME_NAME, TOTK_GAME_VERSION);
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
