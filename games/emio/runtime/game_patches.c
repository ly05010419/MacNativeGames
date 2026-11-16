/* Instruction patches matched against pinned 1.0.0 main. A mismatch stops the boot. */
#include "runtime.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
_Atomic int runtime_survey_pending;

void runtime_game_patches(Module *m) {
 uint32_t original, replacement;
 /* Authentic block restored (was CUT to 2 GiB): six pools sum to 2.25 GiB,
    need the game's 2.5 GiB AllocateMemoryBlock. SetMemoryHeapSize stays at
    3 GiB for host headroom (0.5 GiB leftover for SDK malloc). Rationale
    for the old CUT (leftover-heap memalign) falsified: tables/backings
    come from SDK malloc on the block itself. */
 memcpy(&original, m->base+0x9520, 4);
 if(original!=0x52b40013)
  runtime_fail("AllocateMemoryBlock size mov differs from pinned 1.0.0");
 fprintf(stderr, "[emio] AllocateMemoryBlock authentic 2.5 GiB (revert of 2.0 GiB CUT)\n");
 memcpy(&original, m->base+0x9524, 4);
 if(original!=0xaa1303e0)
  runtime_fail("SetHeapSize mov x0,x19 differs from pinned 1.0.0");
 replacement=0x52b80000; /* mov w0, #0xc0000000 */
 memcpy(m->base+0x9524, &replacement, 4);
 fprintf(stderr, "[emio] heap 3.0 GiB (0.5 GiB leftover for memalign)\n");
 /* BRING-UP CUT (expires when display/mode init is understood): force the display-mode validation
    at main+0xE1BC0 to succeed. The w25/w20 mode fields stay -1 (unset); with the real check the game
    aborts before reaching the title screen. Revisit once input/display init is mapped. */
 memcpy(&original, m->base+0xe1bc0, 4);
 if(original!=0xd10343ff)
  runtime_fail("E1BC0 prologue differs from pinned 1.0.0");
 replacement=0x52800020; /* mov w0, #1 */
 memcpy(m->base+0xe1bc0, &replacement, 4);
 replacement=0xd65f03c0; /* ret */
 memcpy(m->base+0xe1bc4, &replacement, 4);
 fprintf(stderr, "[emio] CUT: bypassed display-mode validation (main+0xE1BC0)\n");
 memcpy(&original, m->base+0xe2450, 4);
 if(original!=0xd10103ff)
  runtime_fail("E2450 prologue differs from pinned 1.0.0");
 replacement=0xd65f03c0; /* ret */
 memcpy(m->base+0xe2450, &replacement, 4);
 fprintf(stderr, "[emio] CUT: bypassed motion-sensor init (main+0xE2450)\n");
 /* EBE20 bypass REMOVED: time services now work, real date query should succeed.
    The mov+ret bypass spun (ret to stale LR). */
 fprintf(stderr, "[emio] date/time query (main+0xEBE20) left intact\n");
}

void runtime_frame_step_pin(double ms) { (void)ms; }
void runtime_frame_step_update(uint64_t ns) { (void)ns; }
int runtime_output_res(unsigned *w, unsigned *h) { (void)w; (void)h; return 0; }
uint64_t runtime_output_res_extra_bytes(void) { return 0; }
int runtime_output_res_toggle(void) { return 0; }
int runtime_output_mode_cycle(void) { return 0; }
void runtime_survey_init(Module *m) { (void)m; }
int runtime_survey_command(const char *line) { (void)line; return 0; }
void runtime_survey_tick(uint64_t frame) { (void)frame; }
void runtime_survey_cancel_user(void) {}
void runtime_autoload_save_file_opened(const char *path) { (void)path; }
void runtime_autoload_user_input(void) {}
