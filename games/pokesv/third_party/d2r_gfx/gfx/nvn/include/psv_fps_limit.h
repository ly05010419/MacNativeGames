/* Mac frame-lock choices shared by the panel, saved-setting validation and NVN. */
#pragma once
#include <stddef.h>
/* Pokémon Violet (2026-10-31): 30 and 60 are right (the game's 30 and 60 fps modes, whole 60 Hz ticks). Above 60 the game's
   effects run slow or stop (a clock step of 0 ticks stops the game), so 90 and 120 are offered with a warning. */
static const int psv_fps_limits[] = {30, 60, 90, 120};
#define PSV_FPS_SAFE_MAX 60
#define PSV_FPS_LIMIT_COUNT (sizeof psv_fps_limits / sizeof *psv_fps_limits)
static inline int psv_fps_limit_valid(int fps) {
    for (size_t i = 0; i < PSV_FPS_LIMIT_COUNT; i++) if (psv_fps_limits[i] == fps) return 1;
    return 0;
}
