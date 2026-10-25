#include "game_patches.h"
#include "world_survey.h"
#include <stdio.h>

/* No game-specific instruction patches are registered for ACNH 1.2.1. */
void runtime_game_patches(Module *m) {
    runtime_survey_init(m);
    fprintf(stderr,"[acnh] verified main mapped at %p; no game instruction patches\n", (void *)m->base);
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
