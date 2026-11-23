/* Synthetic data page; the resolution and aspect ratio calls are the production code. */
#if defined(GAME_BOTW)
#include "../games/totk/runtime/game_patches_botw.c"
#elif defined(TOTK_GAME_V143)
#include "../games/totk/runtime/game_patches_v143.c"
#else
#include "../games/totk/runtime/game_patches.c"
#endif
#include <assert.h>
#include <limits.h>
#include <sys/stat.h>

static _Atomic uint64_t page[8];
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
/* A swapchain as if the app carried the 4K heap overlay, for the native-4K mode tests; restore before the rows matter. */
void settings_test_output_4k(int on) {
#ifndef GAME_BOTW
    if (on) { output_w = 3840; output_h = 2160; }
    else { output_w = 2560; output_h = 1440; }
#else
    (void)on;
#endif
}
/* This run's app as if built for a larger canvas (w x h: 2560x1440, 3840x2160 or 5120x2880): the render sizes the
   settings panel offers follow. Tears of the Kingdom: the swapchain's size (1.4.3's 4K + MetalFX to 5K follows it too,
   picture_4k_to_5k_available). Breath of the Wild: an automatic start (output_parse) with that largest canvas, the
   synthetic canvas page of settings_test_output_init (1.9.0's too, which is otherwise fixed at 1080p here). Its native
   4K / 5K also need the app's marker files (native_4k_app / native_5k_app), which production reads once and keeps: the
   first cap at 4K or 5K shows them in a synthetic app folder for that read, and from then on the largest canvas alone
   decides. Markers already read as a 2K app's fail here instead of leaving the test on a 2K app. */
#ifdef GAME_BOTW
static void test_app_markers(unsigned w) {
    char app[PATH_MAX], dir[PATH_MAX], file[PATH_MAX], cwd[PATH_MAX];
    snprintf(app, sizeof app, "%s/botw-app-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    assert(mkdtemp(app) && getcwd(cwd, sizeof cwd));
    snprintf(dir, sizeof dir, "%s/extracted", app); assert(!mkdir(dir, 0700));
    snprintf(dir, sizeof dir, "%s/extracted/" TOTK_GAME_DIR, app); assert(!mkdir(dir, 0700));
    const char *names[2] = {"native-4k", "native-5k"};
    for (int i = 0; i < 1 + (w >= 5120); i++) {
        snprintf(file, sizeof file, "%s/%s", dir, names[i]);
        FILE *f = fopen(file, "w"); assert(f); fclose(f);
    }
    assert(!chdir(app));   /* production reads extracted/<game>/native-4k relative to the app's folder */
    assert(native_4k_app() && native_5k_app() == (w >= 5120));
    assert(!chdir(cwd));
    for (int i = 0; i < 1 + (w >= 5120); i++) { snprintf(file, sizeof file, "%s/%s", dir, names[i]); unlink(file); }
    rmdir(dir); snprintf(dir, sizeof dir, "%s/extracted", app); rmdir(dir); rmdir(app);
}
#endif
void settings_test_output_cap(unsigned w, unsigned h) {
#ifndef GAME_BOTW
    output_w = w; output_h = h;
#else
    static int markers;
    if (w >= 3840 && !markers) { test_app_markers(w); markers = 1; }
    g_out_mode = OUT_AUTO; g_out_w = w; g_out_h = h;
    g_out_main = testMain; g_out_page = page;
    if (!atomic_load(&g_out_cur)) output_page(1920, 1080);   /* an automatic start's first canvas (patch_output_res) */
#endif
}
#ifdef GAME_BOTW
/* The game's first present (runtime_frame_step_update): botw_output_tick, once, with 1.9.0's start rule for MetalFX. */
void settings_test_output_first_present(void) { botw_output_tick(); }
#endif
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

#ifdef GAME_BOTW190
/* The field of view's row as it is when patch_fov found the camera code not as shipped (it never runs here). */
void settings_test_fov_refused(int refused) { g_fov_refused = refused; }
/* The factor's word as the stub reads it, on a synthetic data-page slot (patch_fov never runs here). */
static _Atomic uint32_t testFovFactor;
void settings_test_fov_init(void) { g_fov_factor = &testFovFactor; }
uint32_t settings_test_fov_word(void) { return atomic_load(&testFovFactor); }
#endif
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
