#pragma once

/* The iPad Settings pane as the environment the runtime reads at launch. Kept free of UIKit so
   tests/test_ios_settings.py checks the tables on a Mac.
   Two layers, chosen by the pane's "Use custom settings" switch (TOTK_CUSTOM_PICTURE):
   - presets (off, the default): Full HD (TOTK_DOCKED) and the frame-rate row (TOTK_PHONE_FPS);
   - custom (on): the picture mode, the scene resolution and the frame-rate cap.
   A layer owns its keys completely, so the two never write the same environment variable. */
typedef struct {
    const char *drawable;        /* TOTK_IOS_DRAWABLE_H: rows, "0" = the panel's native pixels */
    const char *output_upscale;  /* TOTK_MFX_OUTPUT: MetalFX spatial scaling of the canvas into the drawable */
    const char *operation;       /* TOTK_OPERATION_MODE: "handheld" or "docked" (the 1080 mode uses the docked settings) */
    const char *scene_upscale;   /* TOTK_MFX_UPSCALE: "1" forces it on (the output upscale needs it); NULL keeps the pane's switch */
} totk_picture_env;

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

/* mode is one of the pane's picture values; 0 when it is not (out is untouched). */
int totk_picture_env_build(const char *mode, totk_picture_env *out);
/* cap is "30" or "60". 0 when cap is not a pane value (out is untouched). The iOS display link is fixed at 60 Hz, so
   there is no higher entry: the uncapped path could never show more than 60 either. */
int totk_frame_env_build(const char *cap, totk_frame_env *out);
/* docked: the pane's Full HD switch. Always succeeds. */
int totk_preset_env_build(int docked, totk_preset_env *out);
/* fps is the pane's preset row, "60" (up to the display's rate) or "30" (held). 0 for anything else. */
int totk_preset_frame_env_build(const char *fps, totk_frame_env *out);

const char *totk_picture_mode_default(void);
const char *totk_frame_cap_default(void);

/* Which layer of the pane owns a key. ALWAYS: the pane's other rows (overlays, controllers, ...), handed over whatever
   the switch says. A key that moves to a layer must be listed here, so main.m's filter and the tests share one table. */
typedef enum { TOTK_KEY_ALWAYS, TOTK_KEY_PRESET, TOTK_KEY_CUSTOM } totk_key_layer;
totk_key_layer totk_key_layer_of(const char *key);

/* The in-game frame-rate button (touch_pad.m). The preset layer saves the choice under its own row; the custom layer
   keeps the cap in the pane, so the button only holds 30 until the next launch and saves nothing (NULL). */
const char *totk_frame_button_key(int custom);
int totk_frame_button_target(int held30);   /* 30 or the normal rate (60); the runtime only knows 30 or not-30 */
