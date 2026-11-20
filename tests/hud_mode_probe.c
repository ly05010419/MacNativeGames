/* fps_overlay.c's start mode from TOTK_HUD_MODE (tests/test_hud_mode_choice.py): prints the mode it starts on. */
#include "fps_overlay.h"
#include <stdio.h>
int main(void) { printf("%d\n", fps_overlay_hud_mode()); return 0; }
