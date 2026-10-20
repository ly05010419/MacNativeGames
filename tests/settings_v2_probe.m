/* Probe for runtime/settings.h picture v2 (schema 2): outputRes/upscaler/renderRes, the schema-1 picture
   migration, the TOTK_* environment mapping and the restart/pending/unavailable results of the output
   adapter.  Linked with runtime/settings.m and the adapter stubs below by test_settings_v2.py.

   PROBE_MODE_RESULT makes runtime_output_mode_set_ex return 0, -1, -2 or -3.  Every action runs in its own
   process with TOTK_SETTINGS_DIR pointing at a scratch directory; the Python driver inspects the JSON and
   runs the follow-up processes (round trip, migration, corruption, agent isolation). */
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../games/totk/runtime/settings.h"

static int g_set_calls;
static int g_last_res = -1;
static int g_last_up = -1;
static int g_flag_mfx_output = -1;
static int g_flag_mfx_upscale = -1;

static void fail(const char *what, const char *file, int line) {
    fprintf(stderr, "FAIL: %s (%s:%d)\n", what, file, line);
    abort();
}

#define CHECK(cond) do { if (!(cond)) fail(#cond, __FILE__, __LINE__); } while (0)
#define CHECK_INT(got, want) do { \
    long _g = (long)(got), _w = (long)(want); \
    if (_g != _w) { fprintf(stderr, "FAIL: %s (%s:%d): got %ld want %ld\n", #got, __FILE__, __LINE__, _g, _w); \
                    abort(); } \
} while (0)

/* ---- runtime adapter stubs (settings.m declares them itself) ---------- */

void dm_gfx_set_flag(const char *name, int v) {
    if (!strcmp(name, "mfx_output")) g_flag_mfx_output = v;
    else if (!strcmp(name, "mfx_upscale")) g_flag_mfx_upscale = v;
}

int runtime_output_available(void) { return 1; }

int runtime_output_mode_set_ex(int res, int upscaler) {
    g_set_calls++;
    g_last_res = res;
    g_last_up = upscaler;
    const char *e = getenv("PROBE_MODE_RESULT");
    return e && *e ? atoi(e) : 0;
}

/* Settings no longer asks the runtime whether 4K is live: both 4K modes are
   prepared by the next start from the output overlay, and a 2K process reports
   TOTK_SET_RESTART through runtime_output_mode_set_ex (-3). */

int runtime_aspect_available(void) { return 1; }
int runtime_aspect_supported(void) { return 1; }
int runtime_aspect_set_enabled(int on) { (void)on; return 0; }
int runtime_aspect_set_ratio(float ratio) { (void)ratio; return 0; }
void fps_overlay_set_enabled(int on) { (void)on; }
void fps_overlay_set_hints(int on) { (void)on; }
void fps_overlay_set_compact(int on) { (void)on; }
void fps_overlay_set_shader_enabled(int on) { (void)on; }
void totk_sysload_set_enabled(int on) { (void)on; }
void totk_input_reload(void) {}
void totk_gamepad_reload(void) {}

/* ---- helpers --------------------------------------------------------- */

static NSString *dir_path(const char *name) {
    const char *dir = getenv("TOTK_SETTINGS_DIR");
    CHECK(dir && *dir);
    return [NSString stringWithFormat:@"%s/%s", dir, name];
}

static NSDictionary *read_json_path(NSString *path) {
    NSData *data = [NSData dataWithContentsOfFile:path];
    CHECK(data != nil);
    NSError *error = nil;
    id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    CHECK([object isKindOfClass:[NSDictionary class]]);
    return object;
}

static NSDictionary *read_json(const char *name) {
    return read_json_path(dir_path(name));
}

static NSDictionary *settings_dict(NSDictionary *root) {
    id settings = root[@"settings"];
    CHECK([settings isKindOfClass:[NSDictionary class]]);
    return settings;
}

static long json_int(NSDictionary *d, NSString *key) {
    id v = d[key];
    CHECK([v isKindOfClass:[NSNumber class]]);
    return [v longValue];
}

static void check_env(const char *name, const char *want) {
    const char *got = getenv(name);
    if (!want) {
        if (got) fprintf(stderr, "FAIL: %s should be unset, is %s\n", name, got);
        CHECK(got == NULL);
        return;
    }
    if (!got || strcmp(got, want)) {
        fprintf(stderr, "FAIL: %s = %s, want %s\n", name, got ? got : "(unset)", want);
        abort();
    }
}

static void check_defaults(void) {
    CHECK_INT(totk_setting_output_res(), TOTK_RES_1080);
    CHECK_INT(totk_setting_upscaler(), TOTK_UPSCALE_NATIVE);
    CHECK_INT(totk_setting_render_res(), TOTK_RENDER_GAME);
    CHECK_INT(totk_setting_picture(), TOTK_PICTURE_OFF);
    CHECK_INT(totk_setting_picture_restart_required(), 0);
}

static void check_env_defaults(void) {
    check_env("TOTK_OUTPUT_RES", "auto");
    check_env("TOTK_MFX_UPSCALE", "0");
    check_env("TOTK_MFX_OUTPUT", "0");
    check_env("TOTK_RENDER_RES", NULL);
}

static void check_v2_file(int output, int upscaler, int render, int picture) {
    NSDictionary *root = read_json_path([NSString stringWithUTF8String:totk_settings_path()]);
    CHECK_INT(json_int(root, @"schemaVersion"), 2);
    NSDictionary *s = settings_dict(root);
    CHECK_INT(json_int(s, @"outputRes"), output);
    CHECK_INT(json_int(s, @"upscaler"), upscaler);
    CHECK_INT(json_int(s, @"renderRes"), render);
    CHECK_INT(json_int(s, @"picture"), picture);
}

/* ---- actions --------------------------------------------------------- */

static void action_fresh(void) {
    CHECK_INT(totk_settings_init(), 0);
    CHECK(totk_settings_loaded());
    check_defaults();
    check_env_defaults();
    CHECK_INT(g_set_calls, 0);   /* init is environment-only, no live switch */
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1080, TOTK_UPSCALE_NATIVE, 0, 0);
    printf("pass: fresh schema 2 defaults and env\n");
}

static void action_compat(void) {
    CHECK_INT(totk_settings_init(), 0);
    int calls = g_set_calls;
    CHECK_INT(totk_setting_set_picture(TOTK_PICTURE_MFX), TOTK_SET_OK);
    CHECK_INT(g_set_calls, calls + 1);
    CHECK_INT(g_last_res, TOTK_RES_1440);
    CHECK_INT(g_last_up, TOTK_UPSCALE_METALFX);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_1440);
    CHECK_INT(totk_setting_upscaler(), TOTK_UPSCALE_METALFX);
    CHECK_INT(totk_setting_render_res(), TOTK_RENDER_GAME);
    CHECK_INT(totk_setting_picture(), TOTK_PICTURE_MFX);
    check_env("TOTK_OUTPUT_RES", "auto");
    check_env("TOTK_MFX_UPSCALE", "1");

    CHECK_INT(totk_setting_set_picture(TOTK_PICTURE_NATIVE), TOTK_SET_OK);
    CHECK_INT(g_last_res, TOTK_RES_1440);
    CHECK_INT(g_last_up, TOTK_UPSCALE_NATIVE);
    CHECK_INT(totk_setting_picture(), TOTK_PICTURE_NATIVE);
    check_env("TOTK_OUTPUT_RES", "2560x1440");
    check_env("TOTK_MFX_UPSCALE", "0");

    CHECK_INT(totk_setting_set_picture(TOTK_PICTURE_OFF), TOTK_SET_OK);
    CHECK_INT(g_last_res, TOTK_RES_1080);
    CHECK_INT(g_last_up, TOTK_UPSCALE_NATIVE);
    CHECK_INT(totk_setting_picture(), TOTK_PICTURE_OFF);
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1080, TOTK_UPSCALE_NATIVE, 0, 0);
    printf("pass: picture compatibility mapping\n");
}

static void action_select(void) {
    CHECK_INT(totk_settings_init(), 0);
    check_defaults();

    CHECK_INT(totk_setting_set_output_res(TOTK_RES_1440), TOTK_SET_OK);
    CHECK_INT(g_last_res, TOTK_RES_1440);
    CHECK_INT(g_last_up, TOTK_UPSCALE_NATIVE);
    check_env("TOTK_OUTPUT_RES", "2560x1440");

    CHECK_INT(totk_setting_set_upscaler(TOTK_UPSCALE_METALFX), TOTK_SET_OK);
    CHECK_INT(g_last_up, TOTK_UPSCALE_METALFX);
    check_env("TOTK_OUTPUT_RES", "auto");
    check_env("TOTK_MFX_UPSCALE", "1");
    check_env("TOTK_MFX_OUTPUT", "1");

    /* Render size is patched into the game at load, so it always asks for a restart. */
    CHECK_INT(totk_setting_set_render_res(TOTK_RENDER_1440), TOTK_SET_RESTART);
    CHECK_INT(totk_setting_render_res(), TOTK_RENDER_1440);
    CHECK_INT(totk_setting_picture_restart_required(), 1);
    check_env("TOTK_RENDER_RES", "2560x1440");
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1440, TOTK_UPSCALE_METALFX, TOTK_RENDER_1440, 1);
    printf("pass: select 1440 MetalFX + 1440 render, restart reported\n");
}

static void action_verify(void) {
    CHECK_INT(totk_settings_init(), 0);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_1440);
    CHECK_INT(totk_setting_upscaler(), TOTK_UPSCALE_METALFX);
    CHECK_INT(totk_setting_render_res(), TOTK_RENDER_1440);
    CHECK_INT(totk_setting_picture(), TOTK_PICTURE_MFX);
    check_env("TOTK_OUTPUT_RES", "auto");
    check_env("TOTK_MFX_UPSCALE", "1");
    check_env("TOTK_MFX_OUTPUT", "1");
    check_env("TOTK_RENDER_RES", "2560x1440");
    printf("pass: round trip reload\n");
}

static void action_migrate(void) {
    /* Python wrote schemaVersion 1 settings { picture: 1 } and a schema 1 keymaps file. */
    CHECK_INT(totk_settings_init(), 0);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_1440);
    CHECK_INT(totk_setting_upscaler(), TOTK_UPSCALE_METALFX);
    CHECK_INT(totk_setting_render_res(), TOTK_RENDER_GAME);
    CHECK_INT(totk_setting_picture(), TOTK_PICTURE_MFX);
    check_env("TOTK_OUTPUT_RES", "auto");
    check_env("TOTK_MFX_UPSCALE", "1");

    /* The v1 files are marked dirty by the loaders; a save rewrites both as schema 2. */
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1440, TOTK_UPSCALE_METALFX, 0, 1);
    NSDictionary *root = read_json("keymaps.json");
    CHECK_INT(json_int(root, @"schemaVersion"), 2);
    CHECK_INT(json_int(root, @"activeProfile"), 0);
    NSArray *profiles = root[@"profiles"];
    CHECK([profiles isKindOfClass:[NSArray class]] && profiles.count == 1);
    CHECK([profiles[0][@"name"] isEqualToString:@"Legacy"]);
    printf("pass: schema 1 picture + keymaps migrated to schema 2\n");
}

static void action_envlock(void) {
    /* Python set TOTK_OUTPUT_RES=3840x2160 TOTK_MFX_*=0 TOTK_RENDER_RES=1920x1080. */
    CHECK_INT(totk_settings_init(), 0);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_2160);
    CHECK_INT(totk_setting_upscaler(), TOTK_UPSCALE_NATIVE);
    CHECK_INT(totk_setting_render_res(), TOTK_RENDER_1080);
    CHECK(totk_setting_locked("picture"));
    CHECK(totk_setting_locked("render"));
    CHECK_INT(totk_setting_set_output_res(TOTK_RES_1440), TOTK_SET_ERR_LOCKED);
    CHECK_INT(totk_setting_set_upscaler(TOTK_UPSCALE_METALFX), TOTK_SET_ERR_LOCKED);
    CHECK_INT(totk_setting_set_render_res(TOTK_RENDER_GAME), TOTK_SET_ERR_LOCKED);
    CHECK_INT(g_set_calls, 0);
    /* The stored v1 values stay in the file; the environment is only the live override. */
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1440, TOTK_UPSCALE_METALFX, 0, 1);
    printf("pass: env lock wins and is not persisted\n");
}

static void action_restart(void) {
    CHECK_INT(totk_settings_init(), 0);
    CHECK_INT(totk_setting_output_capable(TOTK_RES_2160, TOTK_UPSCALE_NATIVE), 1);
    setenv("PROBE_MODE_RESULT", "-3", 1);
    CHECK_INT(totk_setting_set_output_res(TOTK_RES_2160), TOTK_SET_RESTART);
    CHECK_INT(g_last_res, TOTK_RES_2160);
    CHECK_INT(g_last_up, TOTK_UPSCALE_NATIVE);
    CHECK_INT(totk_setting_picture_restart_required(), 1);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_2160);
    check_env("TOTK_OUTPUT_RES", "3840x2160");
    check_env("TOTK_MFX_UPSCALE", "0");
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_2160, TOTK_UPSCALE_NATIVE, 0, 2);

    /* Going back to a mode the adapter can switch live clears the badge. */
    setenv("PROBE_MODE_RESULT", "0", 1);
    CHECK_INT(totk_setting_set_output_res(TOTK_RES_1080), TOTK_SET_OK);
    CHECK_INT(totk_setting_picture_restart_required(), 0);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_1080);
    check_env("TOTK_OUTPUT_RES", "auto");
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1080, TOTK_UPSCALE_NATIVE, 0, 0);
    printf("pass: 4K native restart-required and live revert\n");
}

static void action_pending(void) {
    CHECK_INT(totk_settings_init(), 0);
    setenv("PROBE_MODE_RESULT", "-2", 1);
    CHECK_INT(totk_setting_set_output_res(TOTK_RES_1440), TOTK_SET_ERR_PENDING);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_1080);
    CHECK_INT(totk_setting_picture_restart_required(), 0);
    check_env("TOTK_OUTPUT_RES", "auto");
    check_env("TOTK_MFX_UPSCALE", "0");
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1080, TOTK_UPSCALE_NATIVE, 0, 0);
    printf("pass: pending leaves stored state untouched\n");
}

static void action_unavailable(void) {
    CHECK_INT(totk_settings_init(), 0);
    setenv("PROBE_MODE_RESULT", "-1", 1);
    CHECK_INT(totk_setting_set_output_res(TOTK_RES_1440), TOTK_SET_ERR_UNAVAILABLE);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_1080);
    check_env("TOTK_OUTPUT_RES", "auto");
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1080, TOTK_UPSCALE_NATIVE, 0, 0);
    printf("pass: unavailable leaves stored state untouched\n");
}

static void action_corrupt(void) {
    /* Python wrote garbage settings.json and a valid v1 settings.json.bak. */
    int rc = totk_settings_init();
    CHECK_INT(rc, 1);
    const char *err = totk_settings_error();
    CHECK(err && *err);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_1440);
    CHECK_INT(totk_setting_upscaler(), TOTK_UPSCALE_NATIVE);
    CHECK_INT(totk_setting_picture(), TOTK_PICTURE_NATIVE);
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1440, TOTK_UPSCALE_NATIVE, 0, 2);
    printf("pass: corrupt main restored from .bak\n");
}

static void action_isolation(void) {
    CHECK_INT(totk_settings_init(), 0);
    const char *path = totk_settings_path();
    CHECK(path && strstr(path, "/agent-settings/probe-agent/settings.json"));
    CHECK_INT(totk_setting_set_output_res(TOTK_RES_1440), TOTK_SET_OK);
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1440, TOTK_UPSCALE_NATIVE, 0, 2);
    printf("pass: agent settings isolated\n");
}

static int env_expect(const char *name) {
    const char *e = getenv(name);
    CHECK(e && *e);
    return atoi(e);
}

static void action_capable(void) {
    CHECK_INT(totk_settings_init(), 0);
    CHECK_INT(totk_setting_output_capable(TOTK_RES_1080, TOTK_UPSCALE_NATIVE), 1);
    CHECK_INT(totk_setting_output_capable(TOTK_RES_1080, TOTK_UPSCALE_METALFX), 0);
    CHECK_INT(totk_setting_output_capable(TOTK_RES_1440, TOTK_UPSCALE_NATIVE), 1);
    CHECK_INT(totk_setting_output_capable(TOTK_RES_1440, TOTK_UPSCALE_METALFX), 1);
    CHECK_INT(totk_setting_output_capable(TOTK_RES_2160, TOTK_UPSCALE_NATIVE),
              env_expect("PROBE_EXPECT_NATIVE4K"));
    CHECK_INT(totk_setting_output_capable(TOTK_RES_2160, TOTK_UPSCALE_METALFX),
              env_expect("PROBE_EXPECT_MFX4K"));
    CHECK_INT(totk_setting_output_capable(999, TOTK_UPSCALE_NATIVE), 0);
    CHECK_INT(totk_setting_output_capable(TOTK_RES_2160, 99), 0);
    CHECK_INT(g_set_calls, 0);
    printf("pass: capability gates (both 4K modes need the output overlay)\n");
}

static void action_native_incapable(void) {
    CHECK_INT(totk_settings_init(), 0);
    CHECK_INT(totk_setting_output_capable(TOTK_RES_2160, TOTK_UPSCALE_NATIVE), 0);
    CHECK_INT(totk_setting_set_output_res(TOTK_RES_2160), TOTK_SET_ERR_UNAVAILABLE);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_1080);
    CHECK_INT(totk_setting_picture_restart_required(), 0);
    CHECK_INT(g_set_calls, 0);   /* the gate refuses before the adapter is asked */
    check_env("TOTK_OUTPUT_RES", "auto");
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1080, TOTK_UPSCALE_NATIVE, 0, 0);
    printf("pass: native 4K without the overlay is unavailable and not persisted\n");
}

static void action_mfx4k_gate(void) {
    /* A 2K process reports RESTART from the adapter (-3); the target must be
       persisted with the 4K start environment so the next start is live.  A
       process already at 4K reports OK and keeps the same environment. */
    CHECK_INT(totk_settings_init(), 0);
    int capable = env_expect("PROBE_EXPECT_MFX4K");
    CHECK_INT(totk_setting_output_capable(TOTK_RES_2160, TOTK_UPSCALE_METALFX), capable);
    CHECK_INT(totk_setting_set_output_res(TOTK_RES_1440), TOTK_SET_OK);
    CHECK_INT(totk_setting_set_upscaler(TOTK_UPSCALE_METALFX), TOTK_SET_OK);
    if (capable) {
        setenv("PROBE_MODE_RESULT", "-3", 1);
        CHECK_INT(totk_setting_set_output_res(TOTK_RES_2160), TOTK_SET_RESTART);
        CHECK_INT(totk_setting_output_res(), TOTK_RES_2160);
        CHECK_INT(totk_setting_upscaler(), TOTK_UPSCALE_METALFX);
        CHECK_INT(totk_setting_picture_restart_required(), 1);
        check_env("TOTK_OUTPUT_RES", "3840x2160");
        check_env("TOTK_MFX_UPSCALE", "1");
        check_env("TOTK_MFX_OUTPUT", "1");
        CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
        check_v2_file(TOTK_RES_2160, TOTK_UPSCALE_METALFX, 0, 1);

        setenv("PROBE_MODE_RESULT", "0", 1);
        CHECK_INT(totk_setting_set_upscaler(TOTK_UPSCALE_METALFX), TOTK_SET_OK);
        CHECK_INT(totk_setting_picture_restart_required(), 0);
        CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
        check_v2_file(TOTK_RES_2160, TOTK_UPSCALE_METALFX, 0, 1);
        printf("pass: MetalFX 4K persists across a restart and applies live at 4K\n");
    } else {
        CHECK_INT(totk_setting_set_output_res(TOTK_RES_2160), TOTK_SET_ERR_UNAVAILABLE);
        CHECK_INT(totk_setting_output_res(), TOTK_RES_1440);
        CHECK_INT(totk_setting_upscaler(), TOTK_UPSCALE_METALFX);
        check_env("TOTK_OUTPUT_RES", "auto");
        check_env("TOTK_MFX_UPSCALE", "1");
        check_env("TOTK_MFX_OUTPUT", "1");
        CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
        check_v2_file(TOTK_RES_1440, TOTK_UPSCALE_METALFX, 0, 1);
        printf("pass: MetalFX 4K refused (and not persisted) without the output overlay\n");
    }
}

static void action_render_clamp(void) {
    /* Python wrote outputRes=1080 with renderRes=2160: the invalid combination
       that used to crash the game during boot.  It must be normalized before
       any environment is exported, and the 4K render must stay unselectable. */
    CHECK_INT(totk_settings_init(), 0);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_1080);
    CHECK_INT(totk_setting_render_res(), TOTK_RENDER_GAME);
    check_env("TOTK_RENDER_RES", NULL);
    CHECK_INT(totk_setting_set_render_res(TOTK_RENDER_2160), TOTK_SET_ERR_UNAVAILABLE);
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1080, TOTK_UPSCALE_NATIVE, TOTK_RENDER_GAME, TOTK_PICTURE_OFF);
    printf("pass: invalid 4K scene render is normalized and cannot be selected\n");
}

static void action_render_native4k(void) {
    /* A native 4K output may use a 4K scene render; returning to 1080p must
       drop the render target in the same commit and export the next start. */
    CHECK_INT(totk_settings_init(), 0);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_2160);
    CHECK_INT(totk_setting_upscaler(), TOTK_UPSCALE_NATIVE);
    CHECK_INT(totk_setting_render_res(), TOTK_RENDER_2160);
    check_env("TOTK_RENDER_RES", "3840x2160");
    CHECK_INT(totk_setting_set_output_res(TOTK_RES_1080), TOTK_SET_OK);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_1080);
    CHECK_INT(totk_setting_render_res(), TOTK_RENDER_GAME);
    check_env("TOTK_RENDER_RES", NULL);
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_1080, TOTK_UPSCALE_NATIVE, TOTK_RENDER_GAME, TOTK_PICTURE_OFF);
    printf("pass: 4K render is only valid on native 4K and drops with the output\n");
}

static void action_fallback(void) {
    /* Python wrote schema-2 outputRes=2160 with the upscaler under test and
       controls the overlay tree; init must keep that target but export a
       startable size.  A missing 4K overlay falls back to a native start. */
    int upscaler = env_expect("PROBE_EXPECT_UPSCALER");
    CHECK_INT(totk_settings_init(), 0);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_2160);
    CHECK_INT(totk_setting_upscaler(), upscaler);
    check_env("TOTK_OUTPUT_RES", getenv("PROBE_EXPECT_OUTPUT"));
    check_env("TOTK_MFX_UPSCALE", getenv("PROBE_EXPECT_MFX"));
    check_env("TOTK_MFX_OUTPUT", getenv("PROBE_EXPECT_MFX"));
    CHECK_INT(totk_setting_output_capable(TOTK_RES_2160, upscaler),
              env_expect("PROBE_EXPECT_CAP"));
    CHECK_INT(totk_setting_picture_restart_required(), 0);   /* the flag is for user-applied changes */
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_2160, upscaler, 0,
                  upscaler == TOTK_UPSCALE_METALFX ? TOTK_PICTURE_MFX : TOTK_PICTURE_NATIVE);
    printf("pass: 4K fallback keeps the stored target (upscaler %d)\n", upscaler);
}

static void action_legacy_encoding(void) {
    /* Python wrote schema-2 outputRes=2 (the first v2 encoding). */
    CHECK_INT(totk_settings_init(), 0);
    CHECK_INT(totk_setting_output_res(), TOTK_RES_2160);
    CHECK_INT(totk_setting_upscaler(), TOTK_UPSCALE_NATIVE);
    CHECK_INT(totk_settings_save_now(), TOTK_SET_OK);
    check_v2_file(TOTK_RES_2160, TOTK_UPSCALE_NATIVE, 0, 2);
    printf("pass: legacy 0/1/2 outputRes rewritten as resolution values\n");
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 2) {
            fprintf(stderr, "usage: %s <fresh|compat|select|verify|migrate|envlock|restart|pending|unavailable|corrupt|isolation|capable|native-incapable|mfx4k|fallback|legacy>\n",
                    argv[0]);
            return 2;
        }
        const char *action = argv[1];
        if (!strcmp(action, "fresh")) action_fresh();
        else if (!strcmp(action, "compat")) action_compat();
        else if (!strcmp(action, "select")) action_select();
        else if (!strcmp(action, "verify")) action_verify();
        else if (!strcmp(action, "migrate")) action_migrate();
        else if (!strcmp(action, "envlock")) action_envlock();
        else if (!strcmp(action, "restart")) action_restart();
        else if (!strcmp(action, "pending")) action_pending();
        else if (!strcmp(action, "unavailable")) action_unavailable();
        else if (!strcmp(action, "corrupt")) action_corrupt();
        else if (!strcmp(action, "isolation")) action_isolation();
        else if (!strcmp(action, "capable")) action_capable();
        else if (!strcmp(action, "native-incapable")) action_native_incapable();
        else if (!strcmp(action, "mfx4k")) action_mfx4k_gate();
        else if (!strcmp(action, "fallback")) action_fallback();
        else if (!strcmp(action, "render-clamp")) action_render_clamp();
        else if (!strcmp(action, "render-native4k")) action_render_native4k();
        else if (!strcmp(action, "legacy")) action_legacy_encoding();
        else {
            fprintf(stderr, "unknown action: %s\n", action);
            return 2;
        }
    }
    return 0;
}
