#include "settings_map.h"
#include <string.h>

/* The picture mode owns the drawable, the output upscale and the operation: the game renders into a canvas, and the
   mode either matches the drawable to it (720 or 1080 rows) or hands the canvas to MetalFX and lets it fill the panel's
   own pixels. The 1080 mode uses the game's docked settings, as the pane's Full HD preset does. The output upscale only
   runs while the scene upscale's master switch is on, so the MetalFX entry turns that switch on itself: a pane switch
   left off would otherwise make the entry do nothing. */
static const struct { const char *mode; totk_picture_env env; } PICTURE[] = {
    {"720",  {"720",  "0", "handheld", NULL}},
    {"1080", {"1080", "0", "docked",   NULL}},
    {"mfx",  {"0",    "1", "handheld", "1"}},
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
   The iOS display link is fixed at 60 Hz (frontend_ios.m TOTK_IOS_LINK_HZ), so there is no entry above 60: the uncapped
   path is capped at 60 by the link anyway. */
int totk_frame_env_build(const char *cap, totk_frame_env *out) {
    if (!cap) return 0;
    totk_frame_env e;
    if (!strcmp(cap, "30")) e = (totk_frame_env){NULL, "30"};
    else if (!strcmp(cap, "60")) e = (totk_frame_env){"60", NULL};
    else return 0;
    *out = e;
    return 1;
}

/* The Full HD preset is the game's docked settings as the Vision Pro build uses them: 1920x1080 output on a 1080-row
   drawable with the scene at 1600x900. Off is the handheld default of the runtime (no drawable or scene override).
   Breath of the Wild renders its docked scene through its own patch (game_patches_botw.c patch_render_1080) and its
   pane has no scene row, so the scene is left to the game there. */
int totk_preset_env_build(int docked, totk_preset_env *out) {
    *out = docked ? (totk_preset_env){"docked", "1080", "1600x900", "0"}
                  : (totk_preset_env){"handheld", NULL, NULL, "0"};
#ifdef GAME_BOTW
    out->scene = NULL;
#endif
    return 1;
}

/* The preset row's "60" is NOT the custom cap's "60": the preset leaves the pacing to the uncapped path (ceiling: the
   display's rate), while the custom 60 paces the display link at 60. So the preset cannot reuse totk_frame_env_build,
   whose 60 means the fixed pacing. "30" is the same hold in both. */
int totk_preset_frame_env_build(const char *fps, totk_frame_env *out) {
    if (!fps) return 0;
    if (!strcmp(fps, "30")) *out = (totk_frame_env){NULL, "30"};
    else if (!strcmp(fps, "60")) *out = (totk_frame_env){NULL, NULL};
    else return 0;
    return 1;
}

/* The keys the pane's layers own. Every other pane row is handed over whichever layer is active. Only keys that have a
   row can appear here: main.m's loop walks the pane's own specs, so a key without one is never handed over at all. */
static const struct { const char *key; totk_key_layer layer; } LAYERS[] = {
    {"TOTK_DOCKED",           TOTK_KEY_PRESET},
    {"TOTK_PHONE_FPS",        TOTK_KEY_PRESET},
    {"TOTK_IOS_PICTURE_MODE", TOTK_KEY_CUSTOM},
    {"TOTK_RENDER_RES",       TOTK_KEY_CUSTOM},
    {"TOTK_FPS_CAP",          TOTK_KEY_CUSTOM},
};
totk_key_layer totk_key_layer_of(const char *key) {
    if (key) for (unsigned i = 0; i < sizeof LAYERS / sizeof LAYERS[0]; i++) if (!strcmp(LAYERS[i].key, key)) return LAYERS[i].layer;
    return TOTK_KEY_ALWAYS;
}

const char *totk_frame_button_key(int custom) { return custom ? NULL : "TOTK_PHONE_FPS"; }
int totk_frame_button_target(int held30) { return held30 ? 60 : 30; }

/* 720p with the 960x540 scene is the combination that keeps 60 fps the longest on an iPad before the SoC throttles,
   and it is the coolest (measured 2026-10-23: GPU 12.9 ms at 960x540 against 26.2 ms at 1280x720 on the same draw
   count). Must match Root.plist's DefaultValue; tests/test_ios_settings.py checks that. */
const char *totk_picture_mode_default(void) { return "720"; }
const char *totk_frame_cap_default(void) { return "60"; }
