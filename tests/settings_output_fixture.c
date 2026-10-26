/* Synthetic data page; the resolution and aspect ratio calls are the production code. */
#if defined(TOTK_GAME_V143)
#include "../games/totk/runtime/game_patches_v143.c"
#else
#include "../games/totk/runtime/game_patches.c"
#endif
#include <assert.h>

static _Atomic uint64_t page[8];
void settings_test_output_init(void) {
    assert(runtime_output_size_current() == pack_size(1920, 1080));
    output_w = 2560; output_h = 1440;
    atomic_store(&page[4], pack_size(1920, 1080));
    atomic_store(&output_page, (uint8_t *)page);
#if defined(TOTK_GAME_V143)
    static uint8_t scene[64];   /* the render-resolution patch's data page: the MetalFX presets set the scene there */
    render_page = scene; render_canvas = 1;
#endif
}
/* The scene size the 1.4.3 render-resolution page holds now (the asked size; 0 elsewhere). */
uint64_t settings_test_scene(void) {
#if defined(TOTK_GAME_V143)
    return render_canvas ? 0 : (uint64_t)render_h << 32 | render_w;
#else
    return 0;
#endif
}
/* A new start with this picture-mode file (1.4.3): the canvas and preset it starts in; -1 elsewhere. */
int settings_test_picture_restart(uint64_t *size) {
#if defined(TOTK_GAME_V143)
    static _Atomic uint64_t fresh[8];
    atomic_store(&picture_preset, 0); picture_mode_restore((uint8_t *)fresh);
    *size = atomic_load(&fresh[4]); return atomic_load(&picture_preset);
#else
    (void)size; return -1;
#endif
}
void settings_test_output_size(unsigned w, unsigned h) {
    atomic_store(&page[4], pack_size(w, h));
    assert(runtime_output_size_current() == pack_size(w, h));
}
/* The aspect ratio as runtime/aspect_patch.h sets it (production code), with a synthetic data-page slot and a window of
   ratio view instead of the stubs aspect_install writes into the game. */
void settings_test_aspect_init(double view) {
    static _Atomic(uint32_t) slot;
    g_aspect_wide = (float)ASPECT_WIDE;
    atomic_store(&g_aspect_step, ASPECT_STEP_GAME);
    g_aspect_slot = &slot;
    runtime_aspect_view(view * 1080, 1080);
}
