/* The presets' environment (Bananza / Pokopia; games/totk/ios/Sources/settings_map.c) and the Switch-sized picture
   without UIKit. Tears of the Kingdom and Breath of the Wild use the picture profiles (tests/test_picture_profile.c). */
#include "settings_map.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

static int wants(const char *got, const char *want) {
    if (!want) return got != NULL;
    return !got || strcmp(got, want);
}

static void preset(int docked, const char *operation, const char *drawable, const char *scene, const char *output) {
    totk_preset_env e;
    if (!totk_preset_env_build(docked, &e)) { failures++; fprintf(stderr, "preset %d: not accepted\n", docked); return; }
    if (wants(e.drawable, drawable) || wants(e.scene, scene) || wants(e.output_upscale, output) || wants(e.operation, operation)) {
        failures++;
        fprintf(stderr, "preset %d: %s/%s/%s/%s, want %s/%s/%s/%s\n", docked,
                e.operation ? e.operation : "(unset)", e.drawable ? e.drawable : "(unset)",
                e.scene ? e.scene : "(unset)", e.output_upscale ? e.output_upscale : "(unset)",
                operation ? operation : "(unset)", drawable ? drawable : "(unset)",
                scene ? scene : "(unset)", output ? output : "(unset)");
    }
}

static void preset_frame(const char *fps, const char *hz, const char *phone) {
    totk_frame_env e;
    if (!totk_preset_frame_env_build(fps, &e)) { failures++; fprintf(stderr, "preset frame %s: not accepted\n", fps); return; }
    if (wants(e.present_hz, hz) || wants(e.phone_fps, phone)) {
        failures++;
        fprintf(stderr, "preset frame %s: %s/%s, want %s/%s\n", fps,
                e.present_hz ? e.present_hz : "(unset)", e.phone_fps ? e.phone_fps : "(unset)",
                hz ? hz : "(unset)", phone ? phone : "(unset)");
    }
}

static int preset_frame_accepts(const char *fps) {
    totk_frame_env e;
    return totk_preset_frame_env_build(fps, &e);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--preset-fps")) {
        if (preset_frame_accepts(argv[2])) return 0;
        fprintf(stderr, "preset frame rate %s: not accepted\n", argv[2]);
        return 1;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--preset-fps FPS]\n", argv[0]);
        return 2;
    }
    /* The presets: Full HD is the game's docked settings with its own drawable and scene; the frame row's 60 is the
       uncapped path and its 30 holds. */
    preset(1, "docked", "1080", "1600x900", "0");
    preset(0, "handheld", NULL, NULL, "0");
    preset_frame("30", NULL, "30");
    preset_frame("60", NULL, NULL);
    totk_frame_env pf = {"keep", "keep"};
    if (totk_preset_frame_env_build("90", &pf) || totk_preset_frame_env_build(NULL, &pf) || strcmp(pf.present_hz, "keep")) {
        failures++; fprintf(stderr, "unknown preset frame rates must be rejected without touching the result\n");
    }
    /* A docked scene row: HD is the canvas's 1920x1080, anything else the Switch's 1600x900. */
    if (strcmp(totk_docked_scene("1920x1080"), "1920x1080") || strcmp(totk_docked_scene("1600x900"), "1600x900") ||
        strcmp(totk_docked_scene(NULL), "1600x900") || strcmp(totk_docked_scene("3840x2160"), "1600x900")) {
        failures++; fprintf(stderr, "the docked scene must map to 1600x900 or 1920x1080\n");
    }
    /* The in-game button switches between 30 and the normal rate. */
    if (totk_frame_button_target(0) != 30 || totk_frame_button_target(1) != 60) {
        failures++; fprintf(stderr, "the in-game frame-rate button mapping is wrong\n");
    }

    /* "Switch screen size": the picture is as big as the Switch's 6.2-inch 16:9 screen on any iPad, which needs the
       panel's real density (a mini packs more pixels per inch); a view not bigger than that keeps filling it. */
    {
        double w = 0, h = 0;
        if (!totk_switch_size(1180, 820, totk_ipad_ppi("iPad15,7"), 2, &w, &h) || fabs(w - 713.3) > 0.5 || fabs(h - 401.2) > 0.5 ||
            fabs(w * 2 / 264 - 5.404) > 0.01) {   /* the iPad (A16): 5.40 inches wide */
            failures++; fprintf(stderr, "Switch size on the iPad (A16): %.1fx%.1f points, want 713.3x401.2\n", w, h);
        }
        if (!totk_switch_size(1133, 744, totk_ipad_ppi("iPad16,1"), 2, &w, &h) || fabs(w - 880.8) > 0.5) {   /* the mini, 326 ppi */
            failures++; fprintf(stderr, "Switch size on the iPad mini: %.1f points wide, want 880.8\n", w);
        }
        w = h = -1;
        if (totk_switch_size(700, 400, 264, 2, &w, &h) || w != -1 || totk_switch_size(1180, 820, 0, 2, &w, &h)) {
            failures++; fprintf(stderr, "a view no bigger than the Switch's screen (or no density) must keep filling it\n");
        }
        if (totk_ipad_ppi("iPad14,2") != 326 || totk_ipad_ppi("iPad15,7") != 264 || totk_ipad_ppi(NULL) != 264) {
            failures++; fprintf(stderr, "iPad densities: minis 326, the others 264\n");
        }
    }
    if (failures) return 1;
    puts("ios settings ok");
    return 0;
}
