/* Animal Crossing: New Horizons' own part of the controller input: the hooks the shared runtime/gamepad.m and gamepad_core.h call
   (shared/README.md). Included once, by gamepad.m, before gamepad_core.h. */
#ifndef ACNH_GAMEPAD_GAME_H
#define ACNH_GAMEPAD_GAME_H

/* No mouse mode: the mouse's buttons belong to the keyboard layout only (runtime/input.c, keyboard_map.h), and
   controls.txt has no mouse lines (controller_map.h). Every other hook group is left out: nothing beyond the shared input. */
#define GAMEPAD_MOUSE_MODE 0
#endif
