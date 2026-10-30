/* Synthetic data page; the resolution and aspect ratio calls are the production code. */
#if defined(GAME_BOTW)
#include "../games/totk/runtime/game_patches_botw.c"
#elif defined(TOTK_GAME_V143)
#include "../games/totk/runtime/game_patches_v143.c"
#else
#include "../games/totk/runtime/game_patches.c"
#endif
#include <assert.h>

#ifndef GAME_BOTW190
static _Atomic uint64_t page[8];
#endif
#ifdef GAME_BOTW
static uint8_t *testMain;
static uint8_t testCamera[0x100];
static _Atomic uint32_t testAspect;
#endif
void settings_test_output_init(void) {
    assert(runtime_output_size_current() == (1920ull | 1080ull << 32));
#ifdef GAME_BOTW
    /* Zeroed synthetic memory for the production canvas/static slots, never game data. */
    testMain = calloc(1, 0x2cceb18);
    assert(testMain);
#ifdef GAME_BOTW160
    g_out_mode = OUT_AUTO; g_out_w = 2560; g_out_h = 1440;
    g_out_main = testMain; g_out_page = page;
    output_page(1920, 1080);
#endif
#else
    output_w = 2560; output_h = 1440;
    atomic_store(&page[4], pack_size(1920, 1080));
    atomic_store(&output_page, (uint8_t *)page);
#endif
}
void settings_test_output_size(unsigned w, unsigned h) {
#ifdef GAME_BOTW160
    output_page(w, h);
    assert(runtime_output_size_current() == (w | (uint64_t)h << 32));
#elif defined(GAME_BOTW190)
    (void)w; (void)h;   /* This version's production canvas is fixed at 1080p. */
#else
    atomic_store(&page[4], pack_size(w, h));
    assert(runtime_output_size_current() == pack_size(w, h));
#endif
}
/* The aspect ratio as runtime/aspect_patch.h sets it (production code), with a synthetic data-page slot and a window of
   ratio view instead of the stubs aspect_install writes into the game. */
void settings_test_aspect_init(double view) {
#ifdef GAME_BOTW
    g_uw_main = testMain; g_uw_word = &testAspect; g_uw_wide = (float)ASPECT_WIDE;
    uint64_t camera = (uintptr_t)testCamera, vtable = (uintptr_t)testMain + UW_PERSPECTIVE_VTABLE;
    memcpy(testMain + UW_CAMERA_SINGLETON, &camera, 8);
    memcpy(testCamera + 0x38, &vtable, 8);
#else
    static _Atomic(uint32_t) slot;
    g_aspect_wide = (float)ASPECT_WIDE;
    atomic_store(&g_aspect_step, ASPECT_STEP_GAME);
    g_aspect_slot = &slot;
#endif
    runtime_aspect_view(view * 1080, 1080);
}

void settings_test_aspect_frame(void) {
#ifdef GAME_BOTW
    if (!g_uw_main) return;
    /* The synthetic camera consumes the ratio word as the game's patched writer does. */
    uint32_t bits = atomic_load(&testAspect);
    float ratio = (float)ASPECT_GAME;
    if (bits) memcpy(&ratio, &bits, 4);
    memcpy(testCamera + 0xe8, &ratio, 4);
    botw_aspect_apply();
#endif
}
