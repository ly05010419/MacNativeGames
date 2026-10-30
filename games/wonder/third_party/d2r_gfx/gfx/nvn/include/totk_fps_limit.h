/* Mac frame-lock choices shared by the panel, saved-setting validation and NVN. */
#pragma once
#include <stddef.h>
static const int totk_fps_limits[] = {0, 30, 40, 60, 72, 90, 120};
#define TOTK_FPS_LIMIT_COUNT (sizeof totk_fps_limits / sizeof *totk_fps_limits)
static inline int totk_fps_limit_valid(int fps) {
    for (size_t i = 0; i < TOTK_FPS_LIMIT_COUNT; i++) if (totk_fps_limits[i] == fps) return 1;
    return 0;
}
