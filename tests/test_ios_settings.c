/* The Settings pane's mode -> environment mapping (games/totk/ios/Sources/settings_map.c) without UIKit. */
#include "settings_map.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void picture(const char *mode, const char *drawable, const char *output) {
    totk_picture_env e;
    if (!totk_picture_env_build(mode, &e)) {
        failures++; fprintf(stderr, "picture %s: not accepted\n", mode); return;
    }
    if (strcmp(e.drawable, drawable) || strcmp(e.output_upscale, output)) {
        failures++;
        fprintf(stderr, "picture %s: %s/%s, want %s/%s\n", mode, e.drawable, e.output_upscale, drawable, output);
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

static int picture_accepts(const char *mode) {
    totk_picture_env e;
    return totk_picture_env_build(mode, &e) && e.drawable && e.output_upscale;
}

static int frame_cap_accepts(const char *cap) {
    totk_frame_env e;
    /* 120 is a pane value whose mapping is "no override": the uncapped path already runs up to the display's rate. */
    return totk_frame_env_build(cap, &e) && (e.present_hz || e.phone_fps || !strcmp(cap, "120"));
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
    if (argc == 2 && !strcmp(argv[1], "--defaults")) {
        printf("%s\n%s\n", totk_picture_mode_default(), totk_frame_cap_default());
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [--picture MODE | --frame-cap CAP | --defaults]\n", argv[0]);
        return 2;
    }
    /* The picture mode owns the drawable and the output upscale only; the scene upscale is the pane's own switch. */
    picture("720", "720", "0");
    picture("1080", "1080", "0");
    picture("mfx", "0", "1");
    totk_picture_env p = {"keep", "keep"};
    if (totk_picture_env_build("custom", &p) || totk_picture_env_build(NULL, &p) ||
        totk_picture_env_build("", &p) || strcmp(p.drawable, "keep")) {
        failures++; fprintf(stderr, "unknown picture modes must be rejected without touching the result\n");
    }

    /* 30 is the phone's own hold, 60 paces the display link, 120 leaves the pacing to the uncapped path (the
       display's own rate). */
    frame("30", NULL, "30");
    frame("60", "60", NULL);
    frame("120", NULL, NULL);
    totk_frame_env f = {"keep", "keep"};
    if (totk_frame_env_build("90", &f) || totk_frame_env_build(NULL, &f) || strcmp(f.present_hz, "keep")) {
        failures++; fprintf(stderr, "unknown frame caps must be rejected without touching the result\n");
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
