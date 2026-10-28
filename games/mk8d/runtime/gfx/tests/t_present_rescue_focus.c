// Strong focus and fullscreen hooks override the graphics layer's weak, always-active test defaults.
#include <stdint.h>
extern int test_focus;
extern uint64_t test_focus_since;
int dm_focus_now(void) { return test_focus; }
uint64_t dm_focus_in_since(void) { return test_focus_since; }
extern int test_fullscreen;
int front_fullscreen(void) { return test_fullscreen; }
