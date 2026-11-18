/* The picture profiles (games/totk/ios/Sources/picture_profile.c) without UIKit: each profile's fixed values, the scene
   held at the mode's canvas, and the environment the runtime reads (PR #2287). A wrong value here is a profile whose
   text does not say what the game does. */
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
    CHECK(pp_profile_picture(PP_QUALITY, &q) && q.mode == PP_DOCKED && q.scene_w == 1600 && q.taa && q.fps == 30 &&
          !q.switch_size && !q.aspect_169, "Quality: docked, 1600x900, TAA, 30, no Switch size, fill");
    /* 30, not 60 (owner 2026-11-18): on the iPad A16 docked 1600x900 + TAA ran at an unsteady 33 fps (GPU 28.6 ms);
       held at 30 it is steady and cooler. */
    CHECK(pp_profile_picture(PP_PERFORMANCE, &d) && d.mode == PP_HANDHELD && d.scene_w == 0 && !d.taa && d.fps == 60 &&
          !d.switch_size, "Performance: handheld, automatic scene, no TAA, 60");
    CHECK(pp_profile_picture(PP_ECONOMY, &e) && e.mode == PP_HANDHELD && e.scene_w == 640 && !e.taa && e.fps == 30 &&
          e.switch_size, "Economy: handheld, 640x360, 30, Switch size");
    CHECK(!pp_profile_picture(PP_CUSTOM, &c), "Custom has no fixed values");
}

static void scene_held_at_canvas(void) {
    pp_picture p = {PP_HANDHELD, 1920, 0, 60, 0, 0, 0};
    CHECK(pp_scene_effective(&p, PP_GAME_BOTW) == 1280, "handheld 1920x1080 is held at the 1280x720 canvas");
    p.scene_w = 1600; CHECK(pp_scene_effective(&p, PP_GAME_BOTW) == 1280, "handheld 1600x900 is held at the canvas");
    p.scene_w = 0;    CHECK(pp_scene_effective(&p, PP_GAME_BOTW) == 960, "handheld automatic is 960x540");
    p.mode = PP_DOCKED; CHECK(pp_scene_effective(&p, PP_GAME_BOTW) == 1600, "docked automatic is 1600x900");
    p.scene_w = 1920; CHECK(pp_scene_effective(&p, PP_GAME_BOTW) == 1920, "docked 1920x1080 stays");
}

/* Final review (2026-11-18): Tears of the Kingdom's scene is scaled by the canvas (game_patches_v143.c
   render_res_publish), so its handheld scene may be 1600x900 above the 1280x720 canvas, as its old custom layer offered;
   only above 1600x900 does it need the bigger heaps the phone has not. Breath of the Wild cannot go above its canvas. */
static void totk_handheld_scene_limit(void) {
    pp_picture p = {PP_HANDHELD, 1600, 0, 60, 0, 0, 0};
    CHECK(pp_scene_effective(&p, PP_GAME_TOTK) == 1600, "TotK handheld 1600x900 stays");
    CHECK(pp_scene_effective(&p, PP_GAME_BOTW) == 1280, "BotW handheld 1600x900 is held at the canvas");
    p.scene_w = 1920; CHECK(pp_scene_effective(&p, PP_GAME_TOTK) == 1600, "TotK handheld 1920x1080 is held at 1600x900");
    p.mode = PP_DOCKED; CHECK(pp_scene_effective(&p, PP_GAME_TOTK) == 1920, "TotK docked 1920x1080 stays");
    p.mode = PP_HANDHELD; p.scene_w = 1600;
    CHECK(!pp_mfx_scene_acts(&p, PP_GAME_TOTK), "a scene above the canvas leaves the scene upscale nothing to do");
    pp_env e; pp_env_build(&p, PP_MFX_OFF, PP_GAME_TOTK, &e); want(&e, "TOTK_RENDER_RES", "1600x900", "TotK handheld 1600x900");
    CHECK(pp_scene_limit(&p, PP_GAME_TOTK) == 1600 && pp_scene_limit(&p, PP_GAME_BOTW) == 1280, "the limits by game");
}

static void mfx_scene_acts(void) {
    pp_picture p = {PP_HANDHELD, 1280, 0, 60, 0, 0, 0};
    CHECK(!pp_mfx_scene_acts(&p, PP_GAME_BOTW), "a scene the canvas's size leaves the scene upscale nothing to do");
    p.scene_w = 960;  CHECK(pp_mfx_scene_acts(&p, PP_GAME_BOTW), "960x540 into the 720p canvas is upscaled");
    p.mode = PP_DOCKED; p.scene_w = 1920; CHECK(!pp_mfx_scene_acts(&p, PP_GAME_BOTW), "docked 1920x1080 is the canvas");
}

static void env_performance_totk(void) {
    pp_picture p; pp_env e;
    pp_profile_picture(PP_PERFORMANCE, &p);
    pp_env_build(&p, PP_MFX_OFF, PP_GAME_TOTK, &e);
    const char *w = "Performance, TotK, MetalFX off";
    want(&e, "TOTK_OPERATION_MODE", "handheld", w); want(&e, "TOTK_IOS_DRAWABLE_H", "(unset)", w);
    want(&e, "TOTK_RENDER_RES", "960x540", w);      want(&e, "TOTK_MFX_UPSCALE", "0", w);
    want(&e, "TOTK_MFX_OUTPUT", "0", w);            want(&e, "TOTK_TAA", "0", w);
    want(&e, "TOTK_PHONE_FPS", "(unset)", w);        want(&e, "DM_PRESENT_HZ", "(unset)", w);
    want(&e, "TOTK_SWITCH_SIZE", "0", w);           want(&e, "TOTK_ASPECT", "(unset)", w);
    CHECK(e.n == 10, "%s: %d variables, want the 10 the runtime reads", w, e.n);
}

static void env_quality_docked(void) {
    pp_picture p; pp_env e;
    pp_profile_picture(PP_QUALITY, &p);
    pp_env_build(&p, PP_MFX_OFF, PP_GAME_BOTW, &e);
    const char *w = "Quality, BotW";
    want(&e, "TOTK_OPERATION_MODE", "docked", w); want(&e, "TOTK_IOS_DRAWABLE_H", "1080", w);
    want(&e, "TOTK_RENDER_RES", "1600x900", w);   want(&e, "TOTK_TAA", "1", w);
    want(&e, "TOTK_PHONE_FPS", "30", w);
}

static void env_economy_botw_scene_mfx(void) {
    pp_picture p; pp_env e;
    pp_profile_picture(PP_ECONOMY, &p);
    pp_env_build(&p, PP_MFX_SCENE, PP_GAME_BOTW, &e);
    const char *w = "Economy, BotW, MetalFX scene";
    want(&e, "TOTK_RENDER_RES", "640x360", w); want(&e, "TOTK_MFX_UPSCALE", "1", w); want(&e, "TOTK_MFX_OUTPUT", "0", w);
    want(&e, "TOTK_PHONE_FPS", "30", w);       want(&e, "TOTK_SWITCH_SIZE", "1", w);
}

/* Review focus 2: with the Switch size the screen stage enlarges into the Switch-sized view (the drawable is the view's
   own pixels), not the whole screen. */
static void env_switch_size_with_screen_mfx(void) {
    pp_picture p; pp_env e;
    pp_profile_picture(PP_ECONOMY, &p);
    pp_env_build(&p, PP_MFX_SCREEN, PP_GAME_TOTK, &e);
    const char *w = "Economy + MetalFX scene and screen";
    want(&e, "TOTK_IOS_DRAWABLE_H", "0", w); want(&e, "TOTK_MFX_UPSCALE", "1", w);
    want(&e, "TOTK_MFX_OUTPUT", "1", w);     want(&e, "TOTK_SWITCH_SIZE", "1", w);
}

static void handheld_scene_above_canvas_env(void) {
    pp_picture p = {PP_HANDHELD, 1920, 0, 60, 0, 0, 0}; pp_env e;
    pp_env_build(&p, PP_MFX_OFF, PP_GAME_BOTW, &e);
    want(&e, "TOTK_RENDER_RES", "1280x720", "BotW handheld 1920x1080 asked");
}

static void aspect_only_totk(void) {
    pp_picture p = {PP_HANDHELD, 0, 0, 60, 0, 1, 0}; pp_env e;
    pp_env_build(&p, PP_MFX_OFF, PP_GAME_TOTK, &e); want(&e, "TOTK_ASPECT", "0", "TotK 16:9");
    pp_env_build(&p, PP_MFX_OFF, PP_GAME_BOTW, &e); want(&e, "TOTK_ASPECT", "(unset)", "BotW has no aspect row (ultrawide)");
}

/* Review focus 5: stored values the app does not know fall back instead of reaching the runtime. */
static void garbage_values_fall_back(void) {
    pp_profile pr = PP_ECONOMY; pp_metalfx m = PP_MFX_SCREEN; unsigned w = 123;
    CHECK(!pp_profile_parse("mfx", &pr) && pr == PP_ECONOMY && !pp_profile_parse(NULL, &pr), "unknown profiles are refused, untouched");
    CHECK(!pp_metalfx_parse("mfx", &m) && m == PP_MFX_SCREEN && !pp_metalfx_parse(NULL, &m), "unknown MetalFX values are refused");
    CHECK(!pp_scene_parse("3840x2160", &w) && w == 123 && !pp_scene_parse(NULL, &w), "sizes outside the list are refused");
    CHECK(pp_scene_parse("", &w) && w == 0, "an empty scene is automatic");
    CHECK(pp_scene_parse("800x450", &w) && w == 800, "a listed size");
    CHECK(!strcmp(pp_scene_key(0), "") && !strcmp(pp_scene_key(800), "800x450") && !strcmp(pp_scene_key(1920), "1920x1080"),
          "a width is stored as the size the parser reads back");
    CHECK(!strcmp(pp_scene_key(1234), ""), "a width outside the list is stored as automatic");
    for (int i = PP_QUALITY; i <= PP_CUSTOM; i++) {
        pp_profile back = PP_QUALITY;
        CHECK(pp_profile_parse(pp_profile_key((pp_profile)i), &back) && back == (pp_profile)i, "profile key %d round-trips", i);
    }
    for (int i = PP_MFX_OFF; i <= PP_MFX_SCREEN; i++) {
        pp_metalfx back = PP_MFX_OFF;
        CHECK(pp_metalfx_parse(pp_metalfx_key((pp_metalfx)i), &back) && back == (pp_metalfx)i, "MetalFX key %d round-trips", i);
    }
}

/* Migration (user 2026-11-17): any saved old row becomes Custom with exactly the combination that ran before, so
   the game looks the same after the update until the player picks a profile. Review focus 1: an iPad whose app wrote
   "docked" on its own still opens docked. */
static void migrate_case(const char *what, pp_old old, pp_game g, pp_picture want, pp_metalfx want_mfx) {
    pp_picture got; pp_metalfx mfx = PP_MFX_SCENE;
    pp_migrate(&old, g, &got, &mfx);
    CHECK(got.mode == want.mode && got.scene_w == want.scene_w && got.taa == want.taa && got.fps == want.fps &&
          got.switch_size == want.switch_size && got.aspect_169 == want.aspect_169 && got.present_60 == want.present_60 &&
          mfx == want_mfx,
          "%s: mode %d scene %u taa %d fps %d switch %d aspect %d present_60 %d mfx %d", what, got.mode, got.scene_w, got.taa,
          got.fps, got.switch_size, got.aspect_169, got.present_60, mfx);
}
/* After PR #2287: the display's 60 Hz pace of a Custom combination brought over from the old custom layer, and nothing
   else; Tears of the Kingdom 1.0 / 1.2.1's automatic handheld scene is its own 1280x720, not 960x540. */
static void present_60_and_old_totk_scene(void) {
    pp_picture p; pp_env e;
    pp_profile_picture(PP_PERFORMANCE, &p);
    p.present_60 = 1; pp_env_build(&p, PP_MFX_OFF, PP_GAME_TOTK, &e); want(&e, "DM_PRESENT_HZ", "60", "migrated custom, 60");
    p.fps = 30;       pp_env_build(&p, PP_MFX_OFF, PP_GAME_TOTK, &e); want(&e, "DM_PRESENT_HZ", "(unset)", "migrated custom, 30");
    want(&e, "TOTK_PHONE_FPS", "30", "migrated custom, 30");
    for (int i = PP_QUALITY; i <= PP_ECONOMY; i++) {
        pp_profile_picture((pp_profile)i, &p);
        CHECK(!p.present_60, "profile %d does not pace the display", i);
        pp_env_build(&p, PP_MFX_OFF, PP_GAME_TOTK, &e); want(&e, "DM_PRESENT_HZ", "(unset)", "a fixed profile");
    }
    pp_profile_picture(PP_PERFORMANCE, &p);
    CHECK(pp_scene_effective(&p, PP_GAME_TOTK_OLD) == 1280, "TotK 1.0 / 1.2.1 handheld automatic is its own 1280x720");
    pp_env_build(&p, PP_MFX_OFF, PP_GAME_TOTK_OLD, &e); want(&e, "TOTK_RENDER_RES", "1280x720", "Performance, TotK 1.0 / 1.2.1");
    CHECK(!pp_mfx_scene_acts(&p, PP_GAME_TOTK_OLD), "its 1280x720 scene is the canvas: the scene upscale has nothing to do");
    CHECK(pp_scene_limit(&p, PP_GAME_TOTK_OLD) == 1600, "TotK 1.0 / 1.2.1 handheld goes up to 1600x900 as 1.4.3");
    p.aspect_169 = 1; pp_env_build(&p, PP_MFX_OFF, PP_GAME_TOTK_OLD, &e); want(&e, "TOTK_ASPECT", "0", "TotK 1.0 / 1.2.1 16:9");
    pp_profile_picture(PP_ECONOMY, &p);
    pp_env_build(&p, PP_MFX_OFF, PP_GAME_TOTK_OLD, &e); want(&e, "TOTK_RENDER_RES", "640x360", "Economy keeps its size on 1.0 / 1.2.1");
}

static void migration(void) {
    migrate_case("migrate_ipad_auto_docked", (pp_old){.docked = 1}, PP_GAME_BOTW,
                 (pp_picture){PP_DOCKED, 1600, 0, 60, 0, 0, 0}, PP_MFX_OFF);
    migrate_case("migrate_totk_custom_mfx", (pp_old){.custom_picture = 1, .picture_mode = "mfx", .render_res = "800x450", .fps_cap = "30"},
                 PP_GAME_TOTK, (pp_picture){PP_HANDHELD, 800, 0, 30, 0, 0, 0}, PP_MFX_SCREEN);
    /* the old custom layer's 60 cap paced the display (DM_PRESENT_HZ=60): it stays so (after PR #2287) */
    migrate_case("migrate_totk_custom_1080", (pp_old){.custom_picture = 1, .picture_mode = "1080", .render_res = "1600x900", .fps_cap = "60"},
                 PP_GAME_TOTK, (pp_picture){PP_DOCKED, 1600, 0, 60, 0, 0, 1}, PP_MFX_OFF);
    migrate_case("migrate_totk_custom_default_cap", (pp_old){.custom_picture = 1, .picture_mode = "720"},
                 PP_GAME_TOTK, (pp_picture){PP_HANDHELD, 960, 0, 60, 0, 0, 1}, PP_MFX_OFF);
    migrate_case("migrate_totk_old_custom", (pp_old){.custom_picture = 1, .picture_mode = "720", .fps_cap = "60", .totk_old = 1},
                 PP_GAME_TOTK_OLD, (pp_picture){PP_HANDHELD, 960, 0, 60, 0, 0, 1}, PP_MFX_OFF);
    migrate_case("migrate_botw_hd", (pp_old){.docked = 1, .docked_scene = "1920x1080", .phone_fps = "30"}, PP_GAME_BOTW,
                 (pp_picture){PP_DOCKED, 1920, 0, 30, 0, 0, 0}, PP_MFX_OFF);
    migrate_case("migrate_handheld", (pp_old){.phone_fps = "60"}, PP_GAME_TOTK,
                 (pp_picture){PP_HANDHELD, 960, 0, 60, 0, 0, 0}, PP_MFX_OFF);
    migrate_case("migrate_totk_old_handheld", (pp_old){.totk_old = 1}, PP_GAME_TOTK,
                 (pp_picture){PP_HANDHELD, 1280, 0, 60, 0, 0, 0}, PP_MFX_OFF);
    migrate_case("migrate_mfx_and_taa", (pp_old){.mfx_upscale = 1, .taa = 1, .aspect = "16:9"}, PP_GAME_TOTK,
                 (pp_picture){PP_HANDHELD, 960, 1, 60, 0, 1, 0}, PP_MFX_SCENE);
    migrate_case("migrate_botw_aspect_ignored", (pp_old){.aspect = "0"}, PP_GAME_BOTW,
                 (pp_picture){PP_HANDHELD, 960, 0, 60, 0, 0, 0}, PP_MFX_OFF);
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
    totk_handheld_scene_limit();
    env_performance_totk();
    env_quality_docked();
    env_economy_botw_scene_mfx();
    env_switch_size_with_screen_mfx();
    handheld_scene_above_canvas_env();
    aspect_only_totk();
    garbage_values_fall_back();
    migration();
    present_60_and_old_totk_scene();
    if (failures) return 1;
    puts("picture profiles ok");
    return 0;
}
