#pragma once
#include "runtime.h"
#include <stdatomic.h>

/* Test-profile-only control, executed on the game's frame-end thread after its GPU fence. */
extern _Atomic int runtime_survey_pending;
extern const int32_t runtime_survey_sync[2];
void runtime_survey_init(Module *main_module);
int runtime_survey_command(const char *line);
void runtime_survey_tick(uint64_t frame);
void runtime_survey_cancel_user(void);
/* Set by the 1.4.3 patches in survey sessions: the benchmark camera block (world_survey.c). */
extern uint8_t *runtime_bench_cam;
