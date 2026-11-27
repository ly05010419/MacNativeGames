#include "game_patches.h"
#include "world_survey.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Frame rate and resolution settings (main 1.0.0, build id D9BA7DB72FFAFECD07F5F5535DEC05734E59BCE1), switchable while
   the game runs (keys 3 and 4, the Settings menu) and kept in the app's preferences.

   Frame rate. Kirby's main loop (the function that returns to main+0x532e30 after a frame's render) paces itself in
   main+0x5330d0..0x5331c4: it reads a pacing mode ([x19+0x60], 0 / 1 / 2) and turns it into a frame length in 60 Hz
   periods, w22 = [x19+0x64] * [x19+0x10 / +0x24 / +0x3c]. All three paths meet at main+0x53311c, where a wait loop
   starts: nn::os::GetSystemTick, the time since the frame began ([x19+0x50]) in periods of 16,666,666 ns (0xfe502a),
   and nn::os::SleepThread(10 us) until w22 periods have passed. Shipping gameplay runs with w22 = 2, 30 fps.
   The loop's first instruction after that call (main+0x533120, ldr x8, [x19, #0x50]) now branches to a veneer in main's
   text padding that caps w22 at the host's frame-length word (1 = 60 fps; 0 = no cap, shipping), then runs the replaced
   load and returns, so every pacing mode is capped the same way and the game's own GetSystemTick call stays in place. Found by following the guest call chain of the frame's command-list calls (KIRBY_EVT_TRACE) to this
   function and its GetSystemTick / SleepThread imports. At 60 fps the game keeps real-time speed: measured by holding
   a direction for the same time at both settings (the Kirby port's pull request).

   Resolution. The render-target resize at main+0x744ba0 (x0 = the target set, w1 x w2 = the requested size, clamped to
   the set's maximum at +0x4c / +0x50) is entered through a second veneer: with the host's fixed-resolution word set,
   every request becomes the target's maximum, so dynamic resolution never lowers it.
   Native 1440p also patches the canvas/crop and promotes scene target descriptors before allocation.

   Every replaced word is checked against the shipping instruction first. KIRBY_FPS60=0 installs nothing (shipping code
   only, no switching). KIRBY_FPS=30|60 and KIRBY_RES=dynamic|1080|1440 override the saved choice at launch. */
typedef struct { uint64_t offset; uint32_t original, replacement; const char *what; } GamePatch;

enum { FPS_SITE = 0x533120, FPS_BACK = 0x533124,
       RES_SITE = 0x744bac, RES_BACK = 0x744bb0,
       TEXT_END = 0x808ac0, VENEER_FPS = 0x808b00, VENEER_RES = 0x808b40,
       RT_CREATE = 0x7443c4, VENEER_CREATE = 0x808b80, PADDING_END = 0x809000 };

uint64_t dm_nvn_frame_count(void);
static volatile uint32_t *g_settings;   /* [0] frame length in 60 Hz units (0 = shipping), [1] fixed resolution */
static int g_installed;
static int g_canvas_1440;

/* The target descriptor has initial dimensions at +8/+12 and allocation limits at
   +20/+24. HAL's scene presets also contain 1600x900 and half/quarter-size targets.
   Promote those together before allocation, leaving square shadow maps, assets and
   the movie decoder alone. A per-thread copy preserves the original preset data. */
static const uint32_t *native_target_descriptor(const uint32_t *source) {
    static _Thread_local uint32_t copy[10];
    unsigned w = source[5], h = source[6], nw = 0, nh = 0;
    if ((w == 1920 && h == 1080) || (w == 1600 && h == 900) || (w == 2560 && h == 1440)) { nw = 2560; nh = 1440; }
    if ((w == 960 && h == 540) || (w == 800 && h == 450)) { nw = 1280; nh = 720; }
    if ((w == 480 && h == 270) || (w == 400 && h == 225)) { nw = 640; nh = 360; }
    if (!nw) return source;
    memcpy(copy, source, sizeof copy);
    copy[2] = copy[5] = nw; copy[3] = copy[6] = nh;
    const char *log = getenv("DM_NVN_LOG");
    if (log && !strcmp(log, "1")) {
        static _Atomic unsigned logged;
        if (atomic_fetch_add(&logged, 1) < 24)
            fprintf(stderr, "[kirby] render target %ux%u / max %ux%u -> %ux%u\n",
                    source[2], source[3], w, h, nw, nh);
    }
    return copy;
}

static int resolution_preference(void) {
    const char *r = getenv("KIRBY_RES");
    if (r && *r) return !strcmp(r, "1440") ? 2 : !strcmp(r, "1080") ? 1 : 0;
    Boolean ok = false;
    int value = (int)CFPreferencesGetAppIntegerValue(CFSTR("KirbyResolution"), CFSTR("local.projectidea.kirby"), &ok);
    if (ok) return value == 1440 ? 2 : value == 1080 ? 1 : 0;
    value = (int)CFPreferencesGetAppIntegerValue(CFSTR("KirbyFixedResolution"), CFSTR("local.projectidea.kirby"), &ok);
    return ok && value ? 1 : 0;
}

static void save_resolution(int mode) {
    int value = mode == 2 ? 1440 : mode == 1 ? 1080 : 0;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &value);
    CFPreferencesSetAppValue(CFSTR("KirbyResolution"), n, CFSTR("local.projectidea.kirby"));
    CFPreferencesAppSynchronize(CFSTR("local.projectidea.kirby")); CFRelease(n);
}

static uint32_t a64_b(uint64_t pc, uint64_t to) { return 0x14000000u | (uint32_t)(((int64_t)(to - pc) >> 2) & 0x3ffffff); }
static uint32_t a64_cbz_w(unsigned rt, uint64_t pc, uint64_t to) { return 0x34000000u | (uint32_t)((((int64_t)(to - pc) >> 2) & 0x7ffff) << 5) | rt; }
static uint32_t a64_ldr_w(unsigned rt, unsigned rn, unsigned off) { return 0xb9400000u | (off / 4) << 10 | rn << 5 | rt; }
static int a64_adrp(unsigned rd, uint64_t pc, uint64_t to, uint32_t *out) {
    int64_t d = ((int64_t)(to & ~0xfffull) - (int64_t)(pc & ~0xfffull)) >> 12;
    if (d < -(1 << 20) || d >= (1 << 20)) return 0;
    *out = 0x90000000u | (uint32_t)(d & 3) << 29 | (uint32_t)((d >> 2) & 0x7ffff) << 5 | rd; return 1;
}

static void check(Module *m, uint64_t off, uint32_t want, const char *what) {
    uint32_t word; memcpy(&word, m->base + off, 4);
    if (word != want) {
        fprintf(stderr, "[kirby] main+%#llx: found %08x, expected %08x (%s)\n", (unsigned long long)off, word, want, what);
        runtime_fail("Kirby game patch: main is not the supported 1.0.0 build");
    }
}
static void put(Module *m, uint64_t off, uint32_t word) { memcpy(m->base + off, &word, 4); }

/* The settings words: a host page within ADRP range (+-4 GiB) of main's text, so the veneers can read it directly. */
static volatile uint32_t *settings_page(Module *m) {
    uint64_t text = (uint64_t)(uintptr_t)m->base;
    for (uint64_t step = 1; step <= 64; step++) {
        uint64_t hint = ((text + m->size + 0xffffff) & ~0xffffffull) + step * 0x4000000ull;
        void *p = mmap((void *)(uintptr_t)hint, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) continue;
        uint32_t probe;
        if (a64_adrp(9, text + VENEER_FPS, (uint64_t)(uintptr_t)p, &probe) && a64_adrp(9, text + VENEER_RES, (uint64_t)(uintptr_t)p, &probe)) return p;
        munmap(p, 0x4000);
    }
    return NULL;
}

static int pref_int(const char *key, int fallback) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8); Boolean ok = false;
    CFIndex v = CFPreferencesGetAppIntegerValue(k, kCFPreferencesCurrentApplication, &ok); CFRelease(k);
    return ok ? (int)v : fallback;
}
static void pref_set(const char *key, int value) {
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8); CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &value);
    CFPreferencesSetAppValue(k, n, kCFPreferencesCurrentApplication); CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication);
    CFRelease(n); CFRelease(k);
}

static void install(Module *m) {
    const uint64_t text = (uint64_t)(uintptr_t)m->base;
    if (m->text_end > TEXT_END || PADDING_END > m->ro_off) runtime_fail("Kirby game patch: no text padding for the veneers");
    for (uint64_t o = VENEER_FPS; o < VENEER_RES + 0x20; o += 4) check(m, o, 0, "text padding (veneer space)");
    check(m, FPS_SITE, 0xf9402a68, "limiter: ldr x8, [x19, #0x50] (wait loop, after GetSystemTick)");
    check(m, RES_SITE, 0x7100043f, "resize: cmp w1, #1");
    if (g_canvas_1440) {
        check(m, RT_CREATE, 0xb00018e8, "render-target constructor: adrp x8");
        for (unsigned off = VENEER_CREATE; off < VENEER_CREATE + 72; off += 4) check(m, off, 0, "target constructor veneer space");
        /* HAL's renderer constructor and its output-target descriptor both own a packed
           width/height pair. Change those before allocation; the engine then creates its
           own targets, viewports and sampling constants at the selected size.
           Logical UI coordinates and the 1080p movie decoder remain unchanged. */
        const GamePatch canvas[] = {
            {0x745698, 0xd280f008, 0xd2814008, "renderer width: 1920 -> 2560"},
            {0x74569c, 0xf2c08708, 0xf2c0b408, "renderer height: 1080 -> 1440"},
            {0x745e0c, 0xd280f008, 0xd2814008, "output target width: 1920 -> 2560"},
            {0x745e10, 0xf2c08708, 0xf2c0b408, "output target height: 1080 -> 1440"},
            {0x8ac780, 1920, 2560, "docked canvas/crop width"},
            {0x8ac784, 1080, 1440, "docked canvas/crop height"},
        };
        for (unsigned i = 0; i < sizeof canvas / sizeof *canvas; i++)
            check(m, canvas[i].offset, canvas[i].original, canvas[i].what);
        for (unsigned i = 0; i < sizeof canvas / sizeof *canvas; i++)
            put(m, canvas[i].offset, canvas[i].replacement);
        uint32_t page;
        a64_adrp(8, text + VENEER_CREATE + 52, text + 0xa61000, &page);
        const uint32_t create[] = {
            0xa9bc07e0, 0xa9010fe2, 0xa90217e4, 0xa9031fe6, /* save x0..x7 */
            0xaa0303e0, 0x58000170, 0xd63f0200, 0xaa0003f1, /* x0=x3; call helper; x17=result */
            0xa9431fe6, 0xa94217e4, 0xa9410fe2, 0xa8c407e0, /* restore arguments */
            0xaa1103e3, page, a64_b(text + VENEER_CREATE + 56, text + RT_CREATE + 4), 0xd503201f,
        };
        for (unsigned i = 0; i < sizeof create / 4; i++) put(m, VENEER_CREATE + i * 4, create[i]);
        uintptr_t helper = (uintptr_t)native_target_descriptor;
        memcpy(m->base + VENEER_CREATE + 64, &helper, 8);
        put(m, RT_CREATE, a64_b(text + RT_CREATE, text + VENEER_CREATE));
    }
    if (!(g_settings = settings_page(m))) runtime_fail("Kirby game patch: no settings page within reach of main");
    const uint64_t set = (uint64_t)(uintptr_t)g_settings; const unsigned lo = (unsigned)(set & 0xfff);
    uint32_t adrp;
    /* frame rate: w9 = units; if set, w22 = min(w22, w9); then the replaced load and back into the loop
       (x9 is free here: the GetSystemTick call before it clobbers it and the loop reloads it; x0 holds the tick) */
    a64_adrp(9, text + VENEER_FPS, set, &adrp);
    const uint32_t fps[] = {adrp, a64_ldr_w(9, 9, lo), a64_cbz_w(9, text + VENEER_FPS + 8, text + VENEER_FPS + 0x14),
                            0x6b0902df /* cmp w22, w9 */, 0x1a96c136 /* csel w22, w9, w22, gt */,
                            0xf9402a68 /* ldr x8, [x19, #0x50] */, a64_b(text + VENEER_FPS + 0x18, text + FPS_BACK)};
    for (unsigned i = 0; i < sizeof fps / 4; i++) put(m, VENEER_FPS + 4 * i, fps[i]);
    /* resolution: w9 = fixed; if set, w1 / w2 = the set's maximum width / height; then the original cmp w1, #1 */
    a64_adrp(9, text + VENEER_RES, set, &adrp);
    const uint32_t res[] = {adrp, a64_ldr_w(9, 9, lo + 4), a64_cbz_w(9, text + VENEER_RES + 8, text + VENEER_RES + 0x14),
                            a64_ldr_w(1, 0, 0x4c), a64_ldr_w(2, 0, 0x50), 0x7100043f /* cmp w1, #1 */,
                            a64_b(text + VENEER_RES + 0x18, text + RES_BACK)};
    for (unsigned i = 0; i < sizeof res / 4; i++) put(m, VENEER_RES + 4 * i, res[i]);
    put(m, FPS_SITE, a64_b(text + FPS_SITE, text + VENEER_FPS));
    put(m, RES_SITE, a64_b(text + RES_SITE, text + VENEER_RES));
    g_installed = 1;
}

int runtime_kirby_fps(void) { return g_installed && g_settings[0] == 1 ? 60 : 30; }
int runtime_kirby_min_interval(void) { return g_installed && runtime_kirby_fps() == 30 ? 2 : 0; }
int runtime_kirby_fixed_res(void) { return g_installed && g_settings[1]; }
int runtime_kirby_settings_available(void) { return g_installed; }
void runtime_kirby_set_fps(int fps) {
    if (!g_installed) return;
    g_settings[0] = fps == 60 ? 1 : 0; pref_set("KirbyFrameRate", fps == 60 ? 60 : 30);
    fprintf(stderr, "[kirby] F%llu frame rate %d fps\n", (unsigned long long)dm_nvn_frame_count(), fps == 60 ? 60 : 30);
}
void runtime_kirby_set_fixed_res(int on) {
    if (!g_installed) return;
    if (g_canvas_1440) return; /* Native 1440p owns larger allocations until the next launch. */
    g_settings[1] = on != 0; pref_set("KirbyFixedResolution", on != 0);
    save_resolution(on ? 1 : 0);
    fprintf(stderr, "[kirby] F%llu resolution %s\n", (unsigned long long)dm_nvn_frame_count(), on ? "fixed 1920x1080" : "dynamic");
}
int runtime_kirby_resolution(void) { return g_canvas_1440 ? 2 : runtime_kirby_fixed_res() ? 1 : 0; }
/* Return 1 when changing the canvas needs a restart; never resize live allocations. */
int runtime_kirby_set_resolution(int mode) {
    if (!g_installed || mode < 0 || mode > 2) return 0;
    save_resolution(mode);
    if ((mode == 2) != g_canvas_1440) return 1;
    if (!g_canvas_1440) runtime_kirby_set_fixed_res(mode == 1);
    return 0;
}

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("game patches expect the main module");
    fprintf(stderr, "[kirby] main mapped at %p\n", (void *)m->base);
    runtime_survey_init(m);
    const char *off = getenv("KIRBY_FPS60");
    if (off && !strcmp(off, "0")) { fprintf(stderr, "[kirby] game patches off (KIRBY_FPS60=0): shipping 30 fps, dynamic resolution\n"); return; }
    int resolution = resolution_preference();
    g_canvas_1440 = resolution == 2;
    install(m);
    const char *f = getenv("KIRBY_FPS");
    int fps = f && *f ? atoi(f) : pref_int("KirbyFrameRate", 60);
    int fixed = resolution != 0;
    g_settings[0] = fps == 30 ? 0 : 1; g_settings[1] = fixed != 0;
    fprintf(stderr, "[kirby] frame rate %d fps, resolution %s (keys 3 / 4, Settings menu)\n", fps == 30 ? 30 : 60,
            g_canvas_1440 ? "native 2560x1440" : fixed ? "fixed 1920x1080" : "dynamic");
}
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return g_canvas_1440 ? (uint64_t)1440 << 32 | 2560 : (uint64_t)1080 << 32 | 1920; }
/* Key 3: dynamic resolution <-> fixed 1920x1080. */
int runtime_output_mode_cycle(void) { if (!g_canvas_1440) runtime_kirby_set_fixed_res(!runtime_kirby_fixed_res()); return g_installed; }
/* Key 4: 60 <-> 30 fps (Kirby has no ultrawide sites). */
int runtime_aspect_toggle(void) { runtime_kirby_set_fps(runtime_kirby_fps() == 60 ? 30 : 60); return g_installed; }
float runtime_aspect_now(void) { return 0; }
