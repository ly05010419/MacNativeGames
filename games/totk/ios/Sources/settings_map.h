#pragma once

/* The iPad Settings pane's picture mode and frame-rate cap as the environment the runtime reads at launch. Kept free
   of UIKit so tests/test_ios_settings.py checks the table on a Mac. */
typedef struct {
    const char *drawable;        /* TOTK_IOS_DRAWABLE_H: rows, "0" = the panel's native pixels */
    const char *output_upscale;  /* TOTK_MFX_OUTPUT: MetalFX spatial scaling of the canvas into the drawable */
} totk_picture_env;

typedef struct {
    const char *present_hz;      /* DM_PRESENT_HZ, NULL: leave unset */
    const char *phone_fps;       /* TOTK_PHONE_FPS, NULL: leave unset */
} totk_frame_env;

/* mode is one of the pane's picture values; 0 when it is not (out is untouched). */
int totk_picture_env_build(const char *mode, totk_picture_env *out);
/* cap is "30", "60" or "120". 0 when cap is not a pane value (out is untouched). */
int totk_frame_env_build(const char *cap, totk_frame_env *out);

const char *totk_picture_mode_default(void);
const char *totk_frame_cap_default(void);
