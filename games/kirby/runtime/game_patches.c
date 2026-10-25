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

   Frame rate. Kirby's frame limiter (main+0x5330d0) turns its pacing mode ([x19+0x60]) into a frame length in 60 Hz units
   (w22), which the loop then waits out against nn::os::GetSystemTick; shipping gameplay uses two units, 30 fps. The
   mode test at main+0x5330d8 now branches to a veneer in main's text padding that reads the host's frame-length word:
   1 sets w22 = 1 (60 fps, as theboy181's "60fps - Full" patch), 0 falls through to the shipping test and multipliers
   (30 fps). Game speed follows real time either way. The "Full" patch's third site (main+0x7f0d1c, distant animation at
   the full rate) stays applied.

   Resolution. The render-target resize at main+0x744ba0 (x0 = the target set, w1 x w2 = the requested size, clamped to
   the set's maximum at +0x4c / +0x50) is entered through a second veneer: with the host's fixed-resolution word set,
   every request becomes the maximum, 1920x1080, so dynamic resolution never lowers it.

   Every replaced word is checked against the shipping instruction first. KIRBY_FPS60=0 installs nothing (shipping code
   only, no switching). KIRBY_FPS=30|60 and KIRBY_RES=dynamic|1080 override the saved choice at launch. */
typedef struct { uint64_t offset; uint32_t original, replacement; const char *what; } GamePatch;

enum { FPS_SITE = 0x5330d8, FPS_BACK_EQ = 0x533108, FPS_BACK_NE = 0x5330dc, FPS_DONE = 0x53311c,
       RES_SITE = 0x744bac, RES_BACK = 0x744bb0, ANIM_SITE = 0x7f0d1c,
       TEXT_END = 0x808ac0, VENEER_FPS = 0x808b00, VENEER_RES = 0x808b40, PADDING_END = 0x809000 };

uint64_t dm_nvn_frame_count(void);
static volatile uint32_t *g_settings;   /* [0] frame length in 60 Hz units (0 = shipping), [1] fixed resolution */
static int g_installed;

static uint32_t a64_b(uint64_t pc, uint64_t to) { return 0x14000000u | (uint32_t)(((int64_t)(to - pc) >> 2) & 0x3ffffff); }
static uint32_t a64_bne(uint64_t pc, uint64_t to) { return 0x54000001u | (uint32_t)((((int64_t)(to - pc) >> 2) & 0x7ffff) << 5); }
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
    check(m, FPS_SITE, 0x54000180, "limiter: b.eq 0x533108 (pacing mode 2)");
    check(m, RES_SITE, 0x7100043f, "resize: cmp w1, #1");
    check(m, ANIM_SITE, 0x54000225, "animation LOD: b.pl 0x7f0d60");
    if (!(g_settings = settings_page(m))) runtime_fail("Kirby game patch: no settings page within reach of main");
    const uint64_t set = (uint64_t)(uintptr_t)g_settings; const unsigned lo = (unsigned)(set & 0xfff);
    uint32_t adrp;
    /* frame rate: w9 = units; 0 -> the shipping mode test (its flags are still those of cmp w8, #2); else w22 = units */
    a64_adrp(9, text + VENEER_FPS, set, &adrp);
    const uint32_t fps[] = {adrp, a64_ldr_w(9, 9, lo), a64_cbz_w(9, text + VENEER_FPS + 8, text + VENEER_FPS + 0x14),
                            0x2a0903f6 /* mov w22, w9 */, a64_b(text + VENEER_FPS + 0x10, text + FPS_DONE),
                            /* b.cond reaches +-1 MiB only: hop over an unconditional b (+-128 MiB) back to the limiter */
                            a64_bne(text + VENEER_FPS + 0x14, text + VENEER_FPS + 0x1c), a64_b(text + VENEER_FPS + 0x18, text + FPS_BACK_EQ),
                            a64_b(text + VENEER_FPS + 0x1c, text + FPS_BACK_NE)};
    for (unsigned i = 0; i < sizeof fps / 4; i++) put(m, VENEER_FPS + 4 * i, fps[i]);
    /* resolution: w9 = fixed; if set, w1 / w2 = the set's maximum width / height; then the original cmp w1, #1 */
    a64_adrp(9, text + VENEER_RES, set, &adrp);
    const uint32_t res[] = {adrp, a64_ldr_w(9, 9, lo + 4), a64_cbz_w(9, text + VENEER_RES + 8, text + VENEER_RES + 0x14),
                            a64_ldr_w(1, 0, 0x4c), a64_ldr_w(2, 0, 0x50), 0x7100043f /* cmp w1, #1 */,
                            a64_b(text + VENEER_RES + 0x18, text + RES_BACK)};
    for (unsigned i = 0; i < sizeof res / 4; i++) put(m, VENEER_RES + 4 * i, res[i]);
    put(m, FPS_SITE, a64_b(text + FPS_SITE, text + VENEER_FPS));
    put(m, RES_SITE, a64_b(text + RES_SITE, text + VENEER_RES));
    put(m, ANIM_SITE, 0x14000011);   /* b 0x7f0d60: distant animation at the full rate */
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
    g_settings[1] = on != 0; pref_set("KirbyFixedResolution", on != 0);
    fprintf(stderr, "[kirby] F%llu resolution %s\n", (unsigned long long)dm_nvn_frame_count(), on ? "fixed 1920x1080" : "dynamic");
}

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("game patches expect the main module");
    fprintf(stderr, "[kirby] main mapped at %p\n", (void *)m->base);
    runtime_survey_init(m);
    const char *off = getenv("KIRBY_FPS60");
    if (off && !strcmp(off, "0")) { fprintf(stderr, "[kirby] game patches off (KIRBY_FPS60=0): shipping 30 fps, dynamic resolution\n"); return; }
    install(m);
    const char *f = getenv("KIRBY_FPS"), *r = getenv("KIRBY_RES");
    int fps = f && *f ? atoi(f) : pref_int("KirbyFrameRate", 60);
    int fixed = r && *r ? !strcmp(r, "1080") : pref_int("KirbyFixedResolution", 0);
    g_settings[0] = fps == 30 ? 0 : 1; g_settings[1] = fixed != 0;
    fprintf(stderr, "[kirby] frame rate %d fps, resolution %s (keys 3 / 4, Settings menu)\n", fps == 30 ? 30 : 60, fixed ? "fixed 1920x1080" : "dynamic");
}
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
/* Key 3: dynamic resolution <-> fixed 1920x1080. */
int runtime_output_mode_cycle(void) { runtime_kirby_set_fixed_res(!runtime_kirby_fixed_res()); return g_installed; }
/* Key 4: 60 <-> 30 fps (Kirby has no ultrawide sites). */
int runtime_aspect_toggle(void) { runtime_kirby_set_fps(runtime_kirby_fps() == 60 ? 30 : 60); return g_installed; }
float runtime_aspect_now(void) { return 0; }
