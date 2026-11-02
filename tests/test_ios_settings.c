/* The Settings pane's layer -> environment mapping (games/totk/ios/Sources/settings_map.c) without UIKit. */
#include "settings_map.h"
#include <stdio.h>
#include <string.h>

static int failures;
static int wants(const char *got, const char *want);

static void picture(const char *mode, const char *drawable, const char *output, const char *operation,
                    const char *scene_upscale) {
    totk_picture_env e;
    if (!totk_picture_env_build(mode, &e)) {
        failures++; fprintf(stderr, "picture %s: not accepted\n", mode); return;
    }
    if (strcmp(e.drawable, drawable) || strcmp(e.output_upscale, output) || strcmp(e.operation, operation) ||
        wants(e.scene_upscale, scene_upscale)) {
        failures++;
        fprintf(stderr, "picture %s: %s/%s/%s/%s, want %s/%s/%s/%s\n", mode, e.drawable, e.output_upscale, e.operation,
                e.scene_upscale ? e.scene_upscale : "(unset)", drawable, output, operation,
                scene_upscale ? scene_upscale : "(unset)");
    }
}

static int wants(const char *got, const char *want) {
    if (!want) return got != NULL;
    return !got || strcmp(got, want);
}

static void frame(const char *cap, const char *hz, const char *phone) {
    totk_frame_env e;
    if (!totk_frame_env_build(cap, &e)) { failures++; fprintf(stderr, "frame %s: not accepted\n", cap); return; }
    if (wants(e.present_hz, hz) || wants(e.phone_fps, phone)) {
        failures++;
        fprintf(stderr, "frame %s: %s/%s, want %s/%s\n", cap,
                e.present_hz ? e.present_hz : "(unset)", e.phone_fps ? e.phone_fps : "(unset)",
                hz ? hz : "(unset)", phone ? phone : "(unset)");
    }
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

static int picture_accepts(const char *mode) {
    totk_picture_env e;
    return totk_picture_env_build(mode, &e) && e.drawable && e.output_upscale && e.operation;
}

static int frame_cap_accepts(const char *cap) {
    totk_frame_env e;
    return totk_frame_env_build(cap, &e);
}

static int preset_frame_accepts(const char *fps) {
    totk_frame_env e;
    return totk_preset_frame_env_build(fps, &e);
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--picture")) {
        if (picture_accepts(argv[2])) return 0;
        fprintf(stderr, "picture %s: not accepted\n", argv[2]);
        return 1;
    }
    if (argc == 3 && !strcmp(argv[1], "--frame-cap")) {
        if (frame_cap_accepts(argv[2])) return 0;
        fprintf(stderr, "frame cap %s: not accepted\n", argv[2]);
        return 1;
    }
    if (argc == 3 && !strcmp(argv[1], "--preset-fps")) {
        if (preset_frame_accepts(argv[2])) return 0;
        fprintf(stderr, "preset frame rate %s: not accepted\n", argv[2]);
        return 1;
    }
    if (argc == 3 && !strcmp(argv[1], "--layer")) {
        totk_key_layer l = totk_key_layer_of(argv[2]);
        puts(l == TOTK_KEY_PRESET ? "preset" : l == TOTK_KEY_CUSTOM ? "custom" : "always");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--defaults")) {
        printf("%s\n%s\n", totk_picture_mode_default(), totk_frame_cap_default());
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--picture MODE | --frame-cap CAP | --preset-fps FPS | --defaults]\n", argv[0]);
        return 2;
    }
    /* The custom layer: the picture mode owns the drawable, the output upscale and the operation mode (1080 runs the
       game's docked settings); the MetalFX entry also forces the scene upscale on, which the output upscale needs. */
    picture("720", "720", "0", "handheld", NULL);
    picture("1080", "1080", "0", "docked", NULL);
    picture("mfx", "0", "1", "handheld", "1");
    totk_picture_env p = {"keep", "keep", "keep", "keep"};
    if (totk_picture_env_build("custom", &p) || totk_picture_env_build(NULL, &p) ||
        totk_picture_env_build("", &p) || strcmp(p.drawable, "keep") || strcmp(p.operation, "keep")) {
        failures++; fprintf(stderr, "unknown picture modes must be rejected without touching the result\n");
    }

    /* 30 is the phone's own hold, 60 paces the display link; the iOS link itself is fixed at 60 Hz, so nothing above
       60 exists. */
    frame("30", NULL, "30");
    frame("60", "60", NULL);
    totk_frame_env f = {"keep", "keep"};
    if (totk_frame_env_build("90", &f) || totk_frame_env_build(NULL, &f) || strcmp(f.present_hz, "keep")) {
        failures++; fprintf(stderr, "unknown frame caps must be rejected without touching the result\n");
    }

    /* The preset layer: Full HD is the game's docked settings with its own drawable and scene; the frame row's 60 is
       the uncapped path and its 30 holds, exactly what the runtime did before the pane had a custom layer. */
    preset(1, "docked", "1080", "1600x900", "0");
    preset(0, "handheld", NULL, NULL, "0");
    preset_frame("30", NULL, "30");
    preset_frame("60", NULL, NULL);
    totk_frame_env pf = {"keep", "keep"};
    if (totk_preset_frame_env_build("90", &pf) || totk_preset_frame_env_build(NULL, &pf) || strcmp(pf.present_hz, "keep")) {
        failures++; fprintf(stderr, "unknown preset frame rates must be rejected without touching the result\n");
    }

    /* Which layer owns which key is one table (main.m's filter and these tests read it); keys nobody listed are handed
       over in both layers. */
    if (totk_key_layer_of("TOTK_DOCKED") != TOTK_KEY_PRESET || totk_key_layer_of("TOTK_PHONE_FPS") != TOTK_KEY_PRESET ||
        totk_key_layer_of("TOTK_IOS_PICTURE_MODE") != TOTK_KEY_CUSTOM || totk_key_layer_of("TOTK_RENDER_RES") != TOTK_KEY_CUSTOM ||
        totk_key_layer_of("TOTK_FPS_CAP") != TOTK_KEY_CUSTOM || totk_key_layer_of("TOTK_FPS_OVERLAY") != TOTK_KEY_ALWAYS ||
        totk_key_layer_of(NULL) != TOTK_KEY_ALWAYS || totk_key_layer_of("TOTK_IOS_DRAWABLE_H") != TOTK_KEY_ALWAYS) {
        /* A key without a pane row is never handed over, so it must not claim a layer; DRAWABLE_H is the one such key
           with a history (the picture mode replaced its row). */
        failures++; fprintf(stderr, "the key -> layer table is not what main.m and the tests expect\n");
    }
    /* The in-game button: the preset layer saves 30 / 60 in its own row; the custom layer leaves the pane's cap alone. */
    if (!totk_frame_button_key(0) || strcmp(totk_frame_button_key(0), "TOTK_PHONE_FPS") || totk_frame_button_key(1) ||
        totk_frame_button_target(0) != 30 || totk_frame_button_target(1) != 60) {
        failures++; fprintf(stderr, "the in-game frame-rate button mapping is wrong\n");
    }

    /* The values themselves belong to Root.plist (test_ios_settings.py compares them); here only that the defaults are
       modes the pane can actually produce. */
    if (!picture_accepts(totk_picture_mode_default()) || !frame_cap_accepts(totk_frame_cap_default())) {
        failures++; fprintf(stderr, "defaults %s/%s are not pane values\n",
                            totk_picture_mode_default(), totk_frame_cap_default());
    }
    if (failures) return 1;
    puts("ios settings ok");
    return 0;
}
