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

/* Key 2's frame-rate state as hle_nvn.c nvn_frame_mode_cycle keeps it, on a 144 Hz display (caps every 2, 3, 4 ticks).
   The pacing itself is checked by the runtime's present-interval probe (make probe-gfx). */
int mfx_requested(void);
void mfx_set(int mode);
static int test_cap = -1;   /* -1 auto (72 fps), k: every k-th tick, 0 uncapped */
void nvn_frame_mode_cycle(void) {
    int m = mfx_requested();
    if (m) { if (m == 3) { test_cap = -1; mfx_set(0); } else mfx_set(m + 1); }
    else if (test_cap == -1) test_cap = 2;
    else if (test_cap > 0) test_cap = test_cap < 4 ? test_cap + 1 : 0;
    else mfx_set(1);
}
int nvn_frame_mode(int *fps) {
    if (test_cap == -1) { *fps = 72; return 0; }
    if (test_cap > 0) { *fps = 144 / test_cap; return 1; }
    *fps = 0; return 2;
}
#if defined(TOTK_GAME_V143)
/* 1.4.3 with the output patch on: key 3 cycles the render resolution from 1080p. */
static _Atomic uint64_t scene_test_page[8];
int shortcut_test_scene_init(void) {
    render_page = (uint8_t *)scene_test_page; scene_follows = 1; atomic_store(&scene_idx, scene_start());
    return 1;
}
#else
int shortcut_test_scene_init(void) { return 0; }
#endif
