/* Synthetic data page; the getter and key-3 cycle are the production code. */
#if defined(TOTK_GAME_V143)
#include "../games/totk/runtime/game_patches_v143.c"
#else
#include "../games/totk/runtime/game_patches.c"
#endif
#include <assert.h>

static _Atomic uint64_t page[8];
void shortcut_test_output_init(void) {
    assert(runtime_output_size_current() == pack_size(1920, 1080));
    output_w = 2560; output_h = 1440;
    atomic_store(&page[4], pack_size(1920, 1080));
    atomic_store(&output_page, (uint8_t *)page);
}
void shortcut_test_output_size(unsigned w, unsigned h) {
    atomic_store(&page[4], pack_size(w, h));
    assert(runtime_output_size_current() == pack_size(w, h));
}
/* Key 4 as runtime/aspect_patch.h runs it (the production cycle), with a synthetic data-page slot and a window of
   ratio view instead of the stubs aspect_install writes into the game. */
void shortcut_test_aspect_init(double view) {
    static _Atomic(uint32_t) slot;
    g_aspect_wide = (float)ASPECT_WIDE;
    atomic_store(&g_aspect_step, ASPECT_STEP_GAME);
    g_aspect_slot = &slot;
    runtime_aspect_view(view * 1080, 1080);
}
