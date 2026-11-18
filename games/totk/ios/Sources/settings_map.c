#include "settings_map.h"
#include <math.h>
#include <string.h>

/* The Full HD preset is the game's docked settings as the Vision Pro build uses them: 1920x1080 output on a 1080-row
   drawable with the scene at 1600x900. Off is the handheld default of the runtime (no drawable or scene override).
   Breath of the Wild sizes its docked scene through its own patch (game_patches_botw.c patch_render_phone, from the
   pane's TOTK_BOTW_SCENE row), so TOTK_RENDER_RES is left alone there. */
/* Tears of the Kingdom's docked scene (the pane's TOTK_DOCKED_SCENE row, the start screen's 1080p original / HD, as
   Breath of the Wild's TOTK_BOTW_SCENE): the Switch's 1600x900, or 1920x1080, the canvas's own size; anything else is
   the Switch's. main.m hands it over as TOTK_RENDER_RES when the preset is docked. */
const char *totk_docked_scene(const char *choice) {
    return choice && !strcmp(choice, "1920x1080") ? "1920x1080" : "1600x900";
}
int totk_preset_env_build(int docked, totk_preset_env *out) {
    *out = docked ? (totk_preset_env){"docked", "1080", "1600x900", "0"}
                  : (totk_preset_env){"handheld", NULL, NULL, "0"};
#ifdef GAME_BOTW
    out->scene = NULL;
#endif
    return 1;
}

/* The preset row's "60" leaves the pacing to the uncapped path (ceiling: the display's rate); "30" is the phone's own
   hold (TOTK_PHONE_FPS), which keeps the game's frame interval in display units. */
int totk_preset_frame_env_build(const char *fps, totk_frame_env *out) {
    if (!fps) return 0;
    if (!strcmp(fps, "30")) *out = (totk_frame_env){NULL, "30"};
    else if (!strcmp(fps, "60")) *out = (totk_frame_env){NULL, NULL};
    else return 0;
    return 1;
}

int totk_frame_button_target(int held30) { return held30 ? 60 : 30; }

/* iPad minis by model identifier (mini 4, 5, 6, A17 Pro); Apple publishes 326 ppi for them and 264 for every other
   iPad. A mini this list does not know gets 264: its picture comes out about a fifth bigger than the Switch's, still
   smaller than the screen. */
double totk_ipad_ppi(const char *machine) {
    static const char *const minis[] = {"iPad5,1", "iPad5,2", "iPad11,1", "iPad11,2", "iPad14,1", "iPad14,2", "iPad16,1", "iPad16,2"};
    if (machine) for (unsigned i = 0; i < sizeof minis / sizeof minis[0]; i++) if (!strcmp(machine, minis[i])) return 326;
    return 264;
}

int totk_switch_size(double view_w, double view_h, double ppi, double scale, double *w, double *h) {
    if (ppi <= 0 || scale <= 0) return 0;
    const double diagonal = 6.2, across = diagonal * 16 / sqrt(16 * 16 + 9 * 9);   /* 5.40 inches wide, 3.04 high */
    double pw = across * ppi / scale, ph = pw * 9 / 16;
    if (pw >= view_w || ph >= view_h) return 0;
    *w = pw; *h = ph;
    return 1;
}
