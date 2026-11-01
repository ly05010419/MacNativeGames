#include "fps_overlay.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    setenv("TOTK_UI_LANG", "en", 1);
    fps_overlay_info info = {.fps=60, .screen_w=2560, .screen_h=1440, .scene_w=1600, .scene_h=900, .gen=60};
    char text[160];
    fps_overlay_text(&info, text, sizeof text);
    assert(strstr(text, "FPS 60+60") && strstr(text, "2560x1440 (1600x900)"));
    info.scene_w = info.scene_h = 0;
    fps_overlay_text(&info, text, sizeof text); assert(!strstr(text, "("));
    pso_overlay_text(text, sizeof text, 7, 734);
    assert(!strcmp(text, "New render pipelines 7 / total 734"));
    hud_publish(HUD_PSO, text, 1);
    char snapshot[160]; int highlight=0;
    assert(hud_snapshot(HUD_PSO, snapshot, sizeof snapshot, &highlight) && highlight && !strcmp(snapshot,text));
    assert(fps_overlay_full_hud());
    assert(fps_overlay_toggle_hints()==HUD_MODE_MINIMAL && fps_overlay_hints_enabled() && !fps_overlay_full_hud());
    assert(fps_overlay_toggle_hints()==HUD_MODE_OFF && !fps_overlay_hints_enabled());
    assert(fps_overlay_toggle_hints()==HUD_MODE_ALL && fps_overlay_full_hud());
    fps_overlay_set_hud_mode(99); assert(fps_overlay_hud_mode()==HUD_MODE_ALL);
    return 0;
}
