#pragma once

/* Entry points from runtime/settings.h into the input adapters. totk_profile_apply() and the settings core call
   these after loading a keymap profile or changing the face-button layout; both clear the state derived from the
   old map so a held button is not released through a stale mapping. Implemented in runtime/input.c and
   runtime/gamepad.m. */
void totk_input_reload(void);
void totk_gamepad_reload(void);
