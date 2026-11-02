/* Mario vs. Donkey Kong 1.0.1 (GAME_MVDK in runtime/game_version.h; this folder's Makefile builds it in place of the TOTK
   host's game_patches.c): exclusive loads with acquire, as games/wonder (the same sead engine). No game-code sites yet: the
   game advances one step per 60 Hz frame, so the host holds it at 60 fps. The output-size and aspect hooks the frontend
   calls are inert. */
#include "game_patches.h"
#include "game_version.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No real-time frame step for this game (games/wonder has one); the diagnostics call stays a no-op. */
void runtime_frame_step_pin(double ms) { (void)ms; fprintf(stderr, "[mvdk] frame step: not available for this game\n"); }
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


/* Exclusive loads with acquire (TOTK_EXCL_ACQUIRE; from upstream 58ac238, found on Breath of the Wild 1.6.0): sead's spin
   locks take the lock with a plain ldxr / stxr and only put a dmb ish before the release, so nothing orders the loads inside
   the critical section after the lock is taken. The ARMv8 model lets such a load be satisfied before the ldxr observes the
   lock free; the Switch's Cortex-A57 rarely does it, Apple's cores do: two workers read the same "next job" index of a
   sead::JobQueue and run the same job twice, and a later job uses what the first run freed. Every plain ldxr / ldxp that
   opens a real exclusive window (a store-exclusive on the same base register within 16 instructions) gets the acquire bit
   (ldaxr / ldaxp): one bit, the same registers and size, only stronger ordering. */
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
        fprintf(stderr, "[totk] %s: TOTK_PCHTXT, TOTK_ASPECT and TOTK_OUTPUT_RES are not supported yet; ignored\n", GAME_NAME);
    const char *a = getenv("TOTK_EXCL_ACQUIRE");
    if (!a || strcmp(a, "0")) {
        unsigned t = excl_acquire(m->base, m->text_end), p = excl_acquire(m->base + m->patch_off, m->file_size - m->patch_off);
        fprintf(stderr, "[totk] exclusive loads with acquire: %u in the code, %u in the rewriter's patch area (TOTK_EXCL_ACQUIRE=0: as shipped)\n", t, p);
    } else fprintf(stderr, "[totk] exclusive loads as shipped (TOTK_EXCL_ACQUIRE=0)\n");
}

/* One 60 fps step per frame (no real-time frame step): the host holds the game at 60 fps (runtime_game_fps_limit, hle_nvn.c). */
int runtime_game_fps_limit(void) { return 60; }
