/* Strong test fixture replaces NVN's weak game callback, as BotW does in production. */
int test_game_hold;
int runtime_game_fps_limit(void) { return test_game_hold; }
#include <stdint.h>
/* Unused renderer data referenced by retained NVN registration entries. */
uint64_t g_draw_skip[16], g_t0_ns;
#include <setjmp.h>
__thread sigjmp_buf *runtime_fault_guard;
_Atomic int runtime_survey_pending;
const int32_t runtime_survey_sync[2]={0};

/* New upstream diagnostics stay disabled in this pacing-only probe. */
_Atomic int g_totk_sr_on;
