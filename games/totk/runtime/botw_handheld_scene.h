#pragma once
#include <stdint.h>
#include <string.h>

/* A separate pair of floats on the main module's writable data page. */
#define BOTW_HANDHELD_PAIR 0x28u
#define BOTW_HANDHELD_LOAD (0xf9400108u | ((BOTW_HANDHELD_PAIR / 8u) << 10)) /* ldr x8, [x8, #0x28] */

static inline unsigned botw_handheld_render_width(const char *value) {
    if (value && !strcmp(value, "640x360")) return 640;
    if (value && !strcmp(value, "800x450")) return 800;
    return 960; /* unchanged default; reject unsupported or malformed sizes */
}
