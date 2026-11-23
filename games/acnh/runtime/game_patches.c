#include "game_patches.h"
#include "aspect_patch.h"   /* shared/runtime: the picture in the screen's shape, as the Zelda ports' */
#include "game_version.h"
#include "world_survey.h"
#include <stdio.h>

/* ACNH 1.2.1 game-code hooks: one thread per context in main+0x1be5b30 (acnh_ctx_lock.c; ACNH_CTX_LOCK=0 leaves it out),
   diagnostics-only draw hook (acnh_draw_hook.c, ACNH_DRAWHOOK=1), and LocationAdjuster NULL guards (acnh_ui_patch.c). */
void acnh_ctx_lock_install(Module *m);
void acnh_draw_hook_install(Module *m);
void acnh_ui_patch_install(Module *m);
/* The screen's shape (user 2026-11-20: "动森 全屏支持 iphone 不要黑边 真正全屏"; shared/runtime/aspect_patch.h, as Tears of the
   Kingdom's): the three places 3.0.3's sead::PerspectiveProjection takes its aspect ratio, the viewport's width over its
   height into +0xb0 (the projection made, main+0x3db3654; its fovy set, main+0x3db3830; the field camera's own, main+0x1513cc0,
   which NXNative's notes on this game call the views that follow the field camera), each the fdiv that leaves the ratio
   in s0. While a ratio sits on the data page after main the stub puts it instead: the camera sees the wider view (more
   at the sides, shapes in proportion), the 16:9 canvas holds it squeezed across and the present widens it over the
   screen (nvn_mtl.m present_stretch), the HUD squeezed back (uw_ui_squeeze). iPhone / iPad: always the screen's shape
   (TOTK_ASPECT=0 for 16:9); Mac: TOTK_ASPECT. The stubs sit in main's text padding (from text_end, 3 x 32 bytes); the
   iOS export runs this on the Mac, so the embedded text carries them. */
#if ACNH_VERSION_RAW == 2228224
static void acnh_aspect_install(Module *m) {
    static const uint64_t sites[] = {0x1513cc0, 0x3db3654, 0x3db3830};
    uint64_t data_off = (m->file_size + 0x3fff) & ~0x3fffULL;   /* main's extra page (loader.c map_module) */
    aspect_install(m, "3.0.3", sites, sizeof sites / sizeof *sites, 0, (m->text_end + 15) & ~15ULL, 0, data_off);
}
#endif
void runtime_game_patches(Module *m) {
    runtime_survey_init(m);
#if ACNH_VERSION_RAW == 2228224
    acnh_aspect_install(m);
#endif
#if ACNH_VERSION_RAW == 458752
    acnh_ctx_lock_install(m);
    acnh_draw_hook_install(m);
    acnh_ui_patch_install(m);
#endif
    fprintf(stderr,"[acnh] verified main mapped at %p\n", (void *)m->base);
}
void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t now_ns) { (void)now_ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
uint64_t runtime_output_size_current(void) { return (uint64_t)1080 << 32 | 1920; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
int runtime_aspect_toggle(void) { return 0; }
#if ACNH_VERSION_RAW != 2228224   /* 1.2.1 / 3.0.1: no aspect sites found; the game's own 16:9 */
float runtime_aspect_now(void) { return 0; }
void runtime_aspect_view(double w, double h) { (void)w; (void)h; }
#endif
