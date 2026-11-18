/* The picture profiles (games/totk/ios/Sources/picture_profile.c) without UIKit: each profile's fixed values, the scene
   held at the mode's canvas, and the environment the runtime reads (docs/superpowers/specs/2026-11-17-ios-picture-
   profiles-design.md). A wrong value here is a profile whose text does not say what the game does. */
#include "picture_profile.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static const char *get(const pp_env *e, const char *name) {
    for (int i = 0; i < e->n; i++) if (!strcmp(e->v[i].name, name)) return e->v[i].value ? e->v[i].value : "(unset)";
    return "(missing)";
}
static void want(const pp_env *e, const char *name, const char *value, const char *what) {
    const char *got = get(e, name);
    CHECK(!strcmp(got, value), "%s: %s = %s, want %s", what, name, got, value);
}

static void profiles_are_the_spec_values(void) {
    pp_picture q, d, e, c;
    CHECK(pp_profile_picture(PP_QUALITY, &q) && q.mode == PP_DOCKED && q.scene_w == 1600 && q.taa && q.fps == 60 &&
          !q.switch_size && !q.aspect_169, "Qualidade: docked, 1600x900, TAA, 60, no Switch size, fill");
    CHECK(pp_profile_picture(PP_PERFORMANCE, &d) && d.mode == PP_HANDHELD && d.scene_w == 0 && !d.taa && d.fps == 60 &&
          !d.switch_size, "Desempenho: handheld, automatic scene, no TAA, 60");
    CHECK(pp_profile_picture(PP_ECONOMY, &e) && e.mode == PP_HANDHELD && e.scene_w == 640 && !e.taa && e.fps == 30 &&
          e.switch_size, "Economia: handheld, 640x360, 30, Switch size");
    CHECK(!pp_profile_picture(PP_CUSTOM, &c), "Personalizado has no fixed values");
}

static void scene_held_at_canvas(void) {
    pp_picture p = {PP_HANDHELD, 1920, 0, 60, 0, 0};
    CHECK(pp_scene_effective(&p) == 1280, "handheld 1920x1080 is held at the 1280x720 canvas");
    p.scene_w = 1600; CHECK(pp_scene_effective(&p) == 1280, "handheld 1600x900 is held at the canvas");
    p.scene_w = 0;    CHECK(pp_scene_effective(&p) == 960, "handheld automatic is 960x540");
    p.mode = PP_DOCKED; CHECK(pp_scene_effective(&p) == 1600, "docked automatic is 1600x900");
    p.scene_w = 1920; CHECK(pp_scene_effective(&p) == 1920, "docked 1920x1080 stays");
}

static void mfx_scene_acts(void) {
    pp_picture p = {PP_HANDHELD, 1280, 0, 60, 0, 0};
    CHECK(!pp_mfx_scene_acts(&p), "a scene the canvas's size leaves the scene upscale nothing to do");
    p.scene_w = 960;  CHECK(pp_mfx_scene_acts(&p), "960x540 into the 720p canvas is upscaled");
    p.mode = PP_DOCKED; p.scene_w = 1920; CHECK(!pp_mfx_scene_acts(&p), "docked 1920x1080 is the canvas");
}

static void env_desempenho_totk(void) {
    pp_picture p; pp_env e;
    pp_profile_picture(PP_PERFORMANCE, &p);
    pp_env_build(&p, PP_MFX_OFF, PP_GAME_TOTK, &e);
    const char *w = "Desempenho, TotK, MetalFX off";
    want(&e, "TOTK_OPERATION_MODE", "handheld", w); want(&e, "TOTK_IOS_DRAWABLE_H", "(unset)", w);
    want(&e, "TOTK_RENDER_RES", "960x540", w);      want(&e, "TOTK_MFX_UPSCALE", "0", w);
    want(&e, "TOTK_MFX_OUTPUT", "0", w);            want(&e, "TOTK_TAA", "0", w);
    want(&e, "TOTK_PHONE_FPS", "(unset)", w);        want(&e, "DM_PRESENT_HZ", "(unset)", w);
    want(&e, "TOTK_SWITCH_SIZE", "0", w);           want(&e, "TOTK_ASPECT", "(unset)", w);
    CHECK(e.n == 10, "%s: %d variables, want the 10 the runtime reads", w, e.n);
}

static void env_qualidade_docked(void) {
    pp_picture p; pp_env e;
    pp_profile_picture(PP_QUALITY, &p);
    pp_env_build(&p, PP_MFX_OFF, PP_GAME_BOTW, &e);
    const char *w = "Qualidade, BotW";
    want(&e, "TOTK_OPERATION_MODE", "docked", w); want(&e, "TOTK_IOS_DRAWABLE_H", "1080", w);
    want(&e, "TOTK_RENDER_RES", "1600x900", w);   want(&e, "TOTK_TAA", "1", w);
}

static void env_economia_botw_scene_mfx(void) {
    pp_picture p; pp_env e;
    pp_profile_picture(PP_ECONOMY, &p);
    pp_env_build(&p, PP_MFX_SCENE, PP_GAME_BOTW, &e);
    const char *w = "Economia, BotW, MetalFX scene";
    want(&e, "TOTK_RENDER_RES", "640x360", w); want(&e, "TOTK_MFX_UPSCALE", "1", w); want(&e, "TOTK_MFX_OUTPUT", "0", w);
    want(&e, "TOTK_PHONE_FPS", "30", w);       want(&e, "TOTK_SWITCH_SIZE", "1", w);
}

/* Review focus 2: with the Switch size the screen stage enlarges into the Switch-sized view (the drawable is the view's
   own pixels), not the whole screen. */
static void env_switch_size_with_screen_mfx(void) {
    pp_picture p; pp_env e;
    pp_profile_picture(PP_ECONOMY, &p);
    pp_env_build(&p, PP_MFX_SCREEN, PP_GAME_TOTK, &e);
    const char *w = "Economia + MetalFX scene and screen";
    want(&e, "TOTK_IOS_DRAWABLE_H", "0", w); want(&e, "TOTK_MFX_UPSCALE", "1", w);
    want(&e, "TOTK_MFX_OUTPUT", "1", w);     want(&e, "TOTK_SWITCH_SIZE", "1", w);
}

static void handheld_scene_above_canvas_env(void) {
    pp_picture p = {PP_HANDHELD, 1920, 0, 60, 0, 0}; pp_env e;
    pp_env_build(&p, PP_MFX_OFF, PP_GAME_TOTK, &e);
    want(&e, "TOTK_RENDER_RES", "1280x720", "handheld 1920x1080 asked");
}

static void aspect_only_totk(void) {
    pp_picture p = {PP_HANDHELD, 0, 0, 60, 0, 1}; pp_env e;
    pp_env_build(&p, PP_MFX_OFF, PP_GAME_TOTK, &e); want(&e, "TOTK_ASPECT", "0", "TotK 16:9");
    pp_env_build(&p, PP_MFX_OFF, PP_GAME_BOTW, &e); want(&e, "TOTK_ASPECT", "(unset)", "BotW has no aspect row (ultrawide)");
}

/* Review focus 5: stored values the app does not know fall back instead of reaching the runtime. */
static void garbage_values_fall_back(void) {
    pp_profile pr = PP_ECONOMY; pp_metalfx m = PP_MFX_SCREEN; unsigned w = 123;
    CHECK(!pp_profile_parse("mfx", &pr) && pr == PP_ECONOMY && !pp_profile_parse(NULL, &pr), "unknown profiles are refused, untouched");
    CHECK(!pp_metalfx_parse("mfx", &m) && m == PP_MFX_SCREEN && !pp_metalfx_parse(NULL, &m), "unknown MetalFX values are refused");
    CHECK(!pp_scene_parse("3840x2160", &w) && w == 123 && !pp_scene_parse(NULL, &w), "sizes outside the list are refused");
    CHECK(pp_scene_parse("", &w) && w == 0, "an empty scene is Automática");
    CHECK(pp_scene_parse("800x450", &w) && w == 800, "a listed size");
    for (int i = PP_QUALITY; i <= PP_CUSTOM; i++) {
        pp_profile back = PP_QUALITY;
        CHECK(pp_profile_parse(pp_profile_key((pp_profile)i), &back) && back == (pp_profile)i, "profile key %d round-trips", i);
    }
    for (int i = PP_MFX_OFF; i <= PP_MFX_SCREEN; i++) {
        pp_metalfx back = PP_MFX_OFF;
        CHECK(pp_metalfx_parse(pp_metalfx_key((pp_metalfx)i), &back) && back == (pp_metalfx)i, "MetalFX key %d round-trips", i);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--keys")) {   /* the Settings pane's values (tests/test_ios_packaging.py) */
        for (int i = PP_QUALITY; i <= PP_CUSTOM; i++) printf("%s%s", i ? " " : "", pp_profile_key((pp_profile)i));
        printf("\n");
        for (int i = PP_MFX_OFF; i <= PP_MFX_SCREEN; i++) printf("%s%s", i ? " " : "", pp_metalfx_key((pp_metalfx)i));
        printf("\n");
        return 0;
    }
    profiles_are_the_spec_values();
    scene_held_at_canvas();
    mfx_scene_acts();
    env_desempenho_totk();
    env_qualidade_docked();
    env_economia_botw_scene_mfx();
    env_switch_size_with_screen_mfx();
    handheld_scene_above_canvas_env();
    aspect_only_totk();
    garbage_values_fall_back();
    if (failures) return 1;
    puts("picture profiles ok");
    return 0;
}
