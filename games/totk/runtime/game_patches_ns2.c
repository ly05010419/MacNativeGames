/* Switch 2 games (GAME_NS2 in runtime/game_version.h; first Donkey Kong Bananza, GAME_DKB, docs/ns2-port.md): the patch file
   the Makefile builds in place of TOTK's game_patches.c. No game-code site is patched: the game runs its own frame pacing
   and output size, and the output-size and aspect hooks the frontend calls are inert. Exclusive loads take acquire, as on
   Breath of the Wild and Super Mario Bros. Wonder (games/wonder/runtime/game_patches_wonder.c): sead's spin locks, which
   Bananza's engine (sead / agl / gsys) uses too, take the lock with a plain ldxr. */
#include "game_patches.h"
#include "game_version.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No frame-step patch: these are what the frontend and the frame lock call. */
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }

int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
int runtime_render_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }   /* no larger scene size for the heaps (romfs.c) */
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
float runtime_aspect_now(void) { return 0; }
int runtime_aspect_step(void) { return -1; }   /* no aspect setting: the shortcut list leaves it out */
void runtime_aspect_view(double w, double h) { (void)w; (void)h; }
int runtime_output_modes(void) { return 0; }
int runtime_output_mode(void) { return OUTPUT_MODE_OFF; }
int runtime_output_mode_set(int mode) { (void)mode; return 0; }
int runtime_output_native(int mode, unsigned *w, unsigned *h) { (void)mode; (void)w; (void)h; return 0; }
int runtime_aspect_set_ratio(float r) { (void)r; return 0; }
/* TOTK's VR and camera-distance hooks (zeldaVR, the settings panel) are left to their weak defaults (zeldaVR/gfx/vr_defaults.c):
   no sites in a Switch 2 game, so VR and the camera row stay off. */
float runtime_aspect_ratio(void) { return 0; }
float runtime_aspect_start(void) { return 0; }

/* Exclusive loads with acquire (TOTK_EXCL_ACQUIRE=0: as shipped): every plain ldxr / ldxp that opens a real exclusive window
   (a store-exclusive on the same base register within 16 instructions) gets the acquire bit. Why: game_patches_wonder.c. */
static int excl_ld(uint32_t w) { return (w & 0x3ffffc00u) == 0x085f7c00u || (w & 0xbfff8000u) == 0x887f0000u; }   /* ldxr{b,h} / ldxp */
static int excl_st(uint32_t w) { return (w & 0x3fe07c00u) == 0x08007c00u || (w & 0xbfe00000u) == 0x88200000u; }   /* st{l}xr{b,h} / st{l}xp */
static unsigned excl_acquire(uint8_t *code, size_t n) {
    unsigned done = 0; uint32_t *w = (uint32_t *)(void *)code; size_t words = n / 4;
    for (size_t i = 0; i < words; i++) {
        if (!excl_ld(w[i])) continue;
        uint32_t rn = (w[i] >> 5) & 31;
        for (size_t j = i + 1; j < words && j <= i + 16; j++) if (excl_st(w[j]) && ((w[j] >> 5) & 31) == rn) { w[i] |= 0x8000u; done++; break; }
    }
    return done;
}

void runtime_game_patches(Module *m) {
    if (strcmp(m->name, "main")) runtime_fail("game patches expect the main module");
    if (getenv("TOTK_PCHTXT") || getenv("TOTK_ASPECT") || (getenv("TOTK_OUTPUT_RES") && strcmp(getenv("TOTK_OUTPUT_RES"), "0")))
        fprintf(stderr, "[totk] %s: TOTK_PCHTXT, TOTK_ASPECT and TOTK_OUTPUT_RES are not supported; ignored\n", GAME_NAME);
    const char *a = getenv("TOTK_EXCL_ACQUIRE");
    if (!a || strcmp(a, "0")) {
        unsigned t = excl_acquire(m->base, m->text_end), p = excl_acquire(m->base + m->patch_off, m->file_size - m->patch_off);
        fprintf(stderr, "[totk] exclusive loads with acquire: %u in the code, %u in the rewriter's patch area (TOTK_EXCL_ACQUIRE=0: as shipped)\n", t, p);
    } else fprintf(stderr, "[totk] exclusive loads as shipped (TOTK_EXCL_ACQUIRE=0)\n");
}

/* The game paces itself (its own present interval and frame-rate controller): no host frame limit. */
int runtime_game_fps_limit(void) { return 0; }
