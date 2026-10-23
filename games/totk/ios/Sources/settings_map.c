#include "settings_map.h"
#include <string.h>

/* The picture mode owns the drawable only: the game renders into a 1920x1080 canvas, and the mode either matches the
   drawable to that canvas (720 or 1080 rows) or hands the canvas to MetalFX and lets it fill the panel's own pixels. */
static const struct { const char *mode; totk_picture_env env; } PICTURE[] = {
    {"720",  {"720",  "0"}},
    {"1080", {"1080", "0"}},
    {"mfx",  {"0",    "1"}},
};

int totk_picture_env_build(const char *mode, totk_picture_env *out) {
    if (!mode) return 0;
    for (unsigned i = 0; i < sizeof PICTURE / sizeof PICTURE[0]; i++) {
        if (strcmp(PICTURE[i].mode, mode)) continue;
        *out = PICTURE[i].env;
        return 1;
    }
    return 0;
}

/* A cap is a demand on the display: 60 paces it through DM_PRESENT_HZ, 30 is the phone's own hold
   (TOTK_PHONE_FPS), which keeps the game's frame interval in display units; pacing the clock at 30 Hz instead raised
   the game's interval to 3 display units and the title ran at about 10 fps (hle_nvn.c phone_holds_30).
   120 leaves the pacing alone: the iOS default is the uncapped path, whose ceiling is the display's own rate (120 on a
   ProMotion iPad, 60 elsewhere). Setting DM_PRESENT_HZ=120 would turn that path off, and the app's own frame pacing
   does not reach above the refresh rate anyway. */
int totk_frame_env_build(const char *cap, totk_frame_env *out) {
    if (!cap) return 0;
    totk_frame_env e;
    if (!strcmp(cap, "30")) e = (totk_frame_env){NULL, "30"};
    else if (!strcmp(cap, "60")) e = (totk_frame_env){"60", NULL};
    else if (!strcmp(cap, "120")) e = (totk_frame_env){NULL, NULL};
    else return 0;
    *out = e;
    return 1;
}

/* 720p with the 960x540 scene is the combination that keeps 60 fps the longest on an iPad before the SoC throttles,
   and it is the coolest (measured 2026-10-23: GPU 12.9 ms at 960x540 against 26.2 ms at 1280x720 on the same draw
   count). Must match Root.plist's DefaultValue; tests/test_ios_settings.py checks that. */
const char *totk_picture_mode_default(void) { return "720"; }
const char *totk_frame_cap_default(void) { return "60"; }
