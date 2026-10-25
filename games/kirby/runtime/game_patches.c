#include "game_patches.h"
#include "world_survey.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 60 FPS (main 1.0.0, build id D9BA7DB72FFAFECD07F5F5535DEC05734E59BCE1).
   Kirby's frame limiter (main+0x5330d0) turns its pacing mode ([x19+0x60]) into a frame length in 60 Hz units (w22),
   which the loop then waits out against nn::os::GetSystemTick. Shipping mode 2 gives two units: 30 fps. The patch
   always takes the mode-2 path and makes it one unit: 60 fps. A third site keeps distant animations at the full rate.
   The sites and words are theboy181's community "60fps - Full" patch (switch-ptchtxt-mods, Kirby and the Forgotten
   Land/1.0.0), each checked here against the shipping instruction before it is replaced.
   KIRBY_FPS60=0 keeps the shipping 30 fps limiter. */
typedef struct { uint64_t offset; uint32_t original, replacement; const char *what; } GamePatch;

static const GamePatch fps60_sites[] = {
    {0x5330d8, 0x54000180, 0x1400000c, "limiter: b 0x533108 (was b.eq: always the mode-2 path)"},
    {0x533110, 0x1b087d36, 0x52800036, "limiter: mov w22, #1 (was mul w22, w9, w8: one 60 Hz unit per frame)"},
    {0x7f0d1c, 0x54000225, 0x14000011, "animation LOD: b 0x7f0d60 (was b.pl: distant animation at the full rate)"},
};

/* KIRBY_FIXED_RES=1: keep the internal resolution fixed (theboy181 "Disable Dynamic Resolution v2"): the resolution
   controller at main+0x744ba0 returns before it changes the render size. Off by default. */
static const GamePatch fixed_res_sites[] = {
    {0x744bb0, 0x5400072b, 0x14000039, "dynamic resolution: b 0x744c94 (was b.lt: never change the render size)"},
};

static int env_on(const char *name, int fallback) {
    const char *e = getenv(name);
    return e && *e ? strcmp(e, "0") != 0 : fallback;
}

static void apply(Module *m, const GamePatch *p, size_t n, const char *label) {
    for (size_t i = 0; i < n; i++) {
        uint32_t word;
        if (p[i].offset + sizeof word > m->text_end) runtime_fail("Kirby patch outside main's text");
        memcpy(&word, m->base + p[i].offset, sizeof word);
        if (word != p[i].original) {
            fprintf(stderr, "[kirby] %s main+%#llx: found %08x, expected %08x (%s)\n", label,
                    (unsigned long long)p[i].offset, word, p[i].original, p[i].what);
            runtime_fail("Kirby game patch: main is not the supported 1.0.0 build");
        }
    }
    for (size_t i = 0; i < n; i++) memcpy(m->base + p[i].offset, &p[i].replacement, sizeof p[i].replacement);
    fprintf(stderr, "[kirby] %s: %zu instructions patched\n", label, n);
}

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("game patches expect the main module");
    fprintf(stderr, "[kirby] main mapped at %p\n", (void *)m->base);
    runtime_survey_init(m);
    if (env_on("KIRBY_FPS60", 1)) apply(m, fps60_sites, sizeof fps60_sites / sizeof *fps60_sites, "60 FPS");
    else fprintf(stderr, "[kirby] 60 FPS patch off (KIRBY_FPS60=0): shipping 30 fps limiter\n");
    if (env_on("KIRBY_FIXED_RES", 0)) apply(m, fixed_res_sites, sizeof fixed_res_sites / sizeof *fixed_res_sites, "fixed resolution");
}
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
