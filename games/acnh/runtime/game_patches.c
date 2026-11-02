#include "game_patches.h"
#include "game_version.h"
#include "world_survey.h"
#include <stdio.h>

/* ACNH 1.2.1 game-code hooks: one thread per context in main+0x1be5b30 (acnh_ctx_lock.c; ACNH_CTX_LOCK=0 leaves it out),
   diagnostics-only draw hook (acnh_draw_hook.c, ACNH_DRAWHOOK=1), and LocationAdjuster NULL guards (acnh_ui_patch.c). */
void acnh_ctx_lock_install(Module *m);
void acnh_draw_hook_install(Module *m);
void acnh_ui_patch_install(Module *m);
void runtime_game_patches(Module *m) {
    runtime_survey_init(m);
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
float runtime_aspect_now(void) { return 0; }
