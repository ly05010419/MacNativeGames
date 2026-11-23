#pragma once

/* The presets of Bananza and Pokopia (Full HD and the frame-rate row) as the environment the runtime reads.
   Tears of the Kingdom and Breath of the Wild use the picture profiles (picture_profile.h).
   Kept free of UIKit so tests/test_ios_settings.py checks them on a Mac. */
typedef struct {
    const char *present_hz;      /* DM_PRESENT_HZ, NULL: leave unset */
    const char *phone_fps;       /* TOTK_PHONE_FPS, NULL: leave unset */
} totk_frame_env;

/* The Full HD preset (docked 1 / 0): its own drawable, scene and output upscale; the operation mode follows. */
typedef struct {
    const char *operation;
    const char *drawable;        /* NULL: leave the runtime's handheld default */
    const char *scene;           /* TOTK_RENDER_RES, NULL: leave the runtime's default */
    const char *output_upscale;
} totk_preset_env;

/* docked: the pane's Full HD switch. Always succeeds. */
int totk_preset_env_build(int docked, totk_preset_env *out);
const char *totk_docked_scene(const char *choice);   /* TOTK_DOCKED_SCENE -> the docked TOTK_RENDER_RES */
/* fps is the pane's preset row, "60" (up to the display's rate) or "30" (held). 0 for anything else. */
int totk_preset_frame_env_build(const char *fps, totk_frame_env *out);

/* The in-game frame-rate button (touch_pad.m). */
int totk_frame_button_target(int held30);   /* 30 or the normal rate (60); the runtime only knows 30 or not-30 */
