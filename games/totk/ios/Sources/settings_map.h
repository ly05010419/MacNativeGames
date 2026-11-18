#pragma once

/* The presets of Bananza and Pokopia (Full HD and the frame-rate row) as the environment the runtime reads, and the
   Switch-sized picture. Tears of the Kingdom and Breath of the Wild use the picture profiles (picture_profile.h).
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

/* "Switch screen size" (TOTK_SWITCH_SIZE=1, user 2026-11-17): the game picture at the size of the Switch's
   screen, 6.2 inches across at 16:9, centred, so a low-resolution picture is shown smaller and looks sharper.
   totk_ipad_ppi: the panel's pixels per inch for an iPad model identifier (hw.machine): 326 for the iPad minis, 264 for
   every other iPad. totk_switch_size: that picture's size in points for a view of view_w x view_h points on a screen
   of `scale` pixels per point; 0 when it is not smaller than the view (the picture then fills it, as without the row). */
double totk_ipad_ppi(const char *machine);
int totk_switch_size(double view_w, double view_h, double ppi, double scale, double *w, double *h);
