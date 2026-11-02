#include "world_survey.h"

/* The TotK world survey reads TotK's actor and scene structures; Astral Chain has none of them. Commands are refused. */
_Atomic int runtime_survey_pending;
const int32_t runtime_survey_sync[2] = {0, -1};   /* read by hle_nvn.c only while a survey is pending, which never happens here */
void runtime_survey_init(Module *main_module) { (void)main_module; }
int runtime_survey_command(const char *line) { (void)line; return 0; }
void runtime_survey_tick(uint64_t frame) { (void)frame; }
void runtime_survey_cancel_user(void) {}
