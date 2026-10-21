// app/launch_settings.h: the settings window's checks, their agreement with the runtime's parsers, the precedence of the
// environment over saved settings over the game's defaults, and what makes the window open after a failed start.
// Built once per game build (tests/test_launch_settings.py): plain, -DTOTK_GAME_V143=1, -DGAME_BOTW160=1.
#include <stdio.h>
#include "launch_settings.h"
#include "runtime.h"

// runtime/motion_fusion.c is linked in for runtime_motion_axes; these are what else it calls.
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "runtime_fail: %s\n", message); exit(2); }
void runtime_hid_feedback_stats(uint64_t *values, uint64_t *reads) { *values = *reads = 0; }
uint64_t runtime_now_ns(void) { return 0; }

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }
static int ok(const char *name, const char *value) { return launch_setting_check(name, value) == NULL; }

typedef struct { const char *saved[LAUNCH_SETTINGS * 2]; const char *env[8]; } Ctx;
static const char *saved(const char *name, void *c) {
    for (const char **p = ((Ctx *)c)->saved; *p; p += 2) if (!strcmp(p[0], name)) return p[1];
    return NULL;
}
static int in_env(const char *name, void *c) {
    for (const char **p = ((Ctx *)c)->env; *p; p++) if (!strcmp(*p, name)) return 1;
    return 0;
}
static const LaunchValue *find(const LaunchValue *v, const char *name) {
    for (int i = 0; i < LAUNCH_SETTINGS; i++) if (!strcmp(v[i].name, name)) return &v[i];
    return NULL;
}

int main(void) {
    check(ok("TOTK_UNCAPPED", "0") && ok("TOTK_UNCAPPED", "1") && !ok("TOTK_UNCAPPED", "yes") && !ok("TOTK_UNCAPPED", ""), "booleans");
    check(ok("TOTK_UNCAPPED_HZ", "display") && ok("TOTK_UNCAPPED_HZ", "30") && ok("TOTK_UNCAPPED_HZ", "120") && ok("TOTK_UNCAPPED_HZ", "60"), "frame cap accepted");
    check(!ok("TOTK_UNCAPPED_HZ", "29") && !ok("TOTK_UNCAPPED_HZ", "121") && !ok("TOTK_UNCAPPED_HZ", "60hz") && !ok("TOTK_UNCAPPED_HZ", "Display"), "frame cap refused");
    check(ok("TOTK_MFX", "0") && ok("TOTK_MFX", "interp30") && ok("TOTK_MFX", "interp") && ok("TOTK_MFX", "interpmax") && !ok("TOTK_MFX", "1"), "frame interpolation (totk_mfx.m mfx_init)");
    check(ok("TOTK_RUMBLE_GAIN", "0.25") && ok("TOTK_RUMBLE_GAIN", "4") && ok("TOTK_RUMBLE_GAIN", "1.5"), "rumble gain accepted");
    check(!ok("TOTK_RUMBLE_GAIN", "0") && !ok("TOTK_RUMBLE_GAIN", "4.5") && !ok("TOTK_RUMBLE_GAIN", "-1") && !ok("TOTK_RUMBLE_GAIN", "1x"), "rumble gain refused (gamepad_feedback.m)");
    check(ok("TOTK_FACE_BUTTONS", "label") && ok("TOTK_FACE_BUTTONS", "position") && !ok("TOTK_FACE_BUTTONS", "auto"), "face buttons");
    check(ok("TOTK_HUD", "corners") && !ok("TOTK_HUD", "box"), "overlay layout");
    check(ok("TOTK_UI_LANG", "zh") && ok("TOTK_UI_LANG", "en") && !ok("TOTK_UI_LANG", "fr"), "launcher language");
    check(ok("TOTK_LANG", "en-US") && ok("TOTK_LANG", "zh-Hans") && ok("TOTK_LANG", "ja") && !ok("TOTK_LANG", "en-GB") && !ok("TOTK_LANG", "zh-Hant"), "game languages: the automatic ones");
#ifdef TOTK_GAME_V143
    check(ok("TOTK_LANG", "pt-BR"), "pt-BR in 1.4.3");
#else
    check(!ok("TOTK_LANG", "pt-BR"), "no pt-BR before 1.4.3");
#endif
    check(!ok("TOTK_SLOT_OWNER", "user") && !ok("DM_PRESENT_HZ", "0"), "only the window's settings");

    // every signed order of x, y, z, and malformed ones, against the runtime's own parser
    const char *axes[] = {"x", "y", "z"}, *signs[] = {"", "-", "+"};
    int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}}, agree = 1, n = 0;
    for (int p = 0; p < 6; p++) for (int s = 0; s < 27; s++) {
        char spec[16]; snprintf(spec, sizeof spec, "%s%s,%s%s,%s%s", signs[s % 3], axes[perms[p][0]], signs[s / 3 % 3], axes[perms[p][1]], signs[s / 9], axes[perms[p][2]]);
        agree &= ok("TOTK_GYRO_AXES", spec) && runtime_motion_axes(spec) == 1; n++;
    }
    const char *bad[] = {"x,y", "x,y,z,", "x,x,z", "x;y;z", "X,Y,Z", "--x,y,z", "x, y, z", "w,y,z", "xyz", "x,y,z ", "", NULL};
    for (int i = 0; bad[i]; i++) { agree &= !ok("TOTK_GYRO_AXES", bad[i]) && (!*bad[i] || runtime_motion_axes(bad[i]) == -1); n++; }
    check(agree && n == 6 * 27 + 11, "gyro axes agree with runtime_motion_axes");

    const LaunchSetting *aspect = launch_setting("TOTK_ASPECT");
#if LAUNCH_ASPECT
    check(aspect && ok("TOTK_ASPECT", "21:9") && ok("TOTK_ASPECT", "3440x1440") && ok("TOTK_ASPECT", "2.39") && ok("TOTK_ASPECT", "16:10"), "aspect accepted");
    check(!ok("TOTK_ASPECT", "0.4") && !ok("TOTK_ASPECT", "9") && !ok("TOTK_ASPECT", "wide") && !ok("TOTK_ASPECT", "21:0") && !ok("TOTK_ASPECT", "21:9:1"), "aspect refused (aspect_patch.h)");
#else
    check(!aspect, "no aspect ratio where the game has no aspect patch");
#endif
#if LAUNCH_BOTW
    check(!launch_setting("TOTK_FPS60") && !strcmp(launch_setting("TOTK_UNCAPPED_INTERVAL")->def, "1"), "Breath of the Wild: no 60 fps patch, steady pacing on by default");
#else
    check(launch_setting("TOTK_FPS60") && !strcmp(launch_setting("TOTK_UNCAPPED_INTERVAL")->def, "0"), "Tears of the Kingdom: steady pacing off by default");
#endif

    // precedence: environment > saved > the game's default; a refused saved value is not passed on
    Ctx c = {{"TOTK_UNCAPPED_HZ", "60", "TOTK_GPU_KEEPALIVE", "0", "TOTK_RUMBLE_GAIN", "9", "TOTK_SLOT_OWNER", "claude", NULL}, {"TOTK_GPU_KEEPALIVE", "TOTK_GYRO", NULL}};
    LaunchValue v[LAUNCH_SETTINGS];
    launch_settings_resolve(saved, in_env, &c, v);
    check(find(v, "TOTK_UNCAPPED_HZ")->source == LAUNCH_SAVED && !strcmp(find(v, "TOTK_UNCAPPED_HZ")->value, "60"), "saved value used");
    check(find(v, "TOTK_GPU_KEEPALIVE")->source == LAUNCH_ENV && !strcmp(find(v, "TOTK_GPU_KEEPALIVE")->value, "0"), "environment beats saved (and the saved value is reported)");
    check(find(v, "TOTK_GYRO")->source == LAUNCH_ENV && !find(v, "TOTK_GYRO")->value, "environment without a saved value");
    check(find(v, "TOTK_RUMBLE_GAIN")->source == LAUNCH_IGNORED && find(v, "TOTK_RUMBLE_GAIN")->problem, "refused saved value ignored");
    check(find(v, "TOTK_MFX")->source == LAUNCH_DEFAULT && !find(v, "TOTK_MFX")->value, "unsaved: the game's default");
    check(!find(v, "TOTK_SLOT_OWNER"), "unknown saved names are not passed on");

    // the window opens by itself after a start that failed on a setting...
    char line[512];
    const LaunchSetting *f = launch_failed_setting("[launcher] settings\n[totk] boot\n[totk] FAIL: TOTK_RUMBLE_GAIN must be a number in (0, 4]\n", line, sizeof line);
    check(f && !strcmp(f->name, "TOTK_RUMBLE_GAIN") && strstr(line, "FAIL"), "runtime_fail on a setting");
    f = launch_failed_setting("[dm] [nvn-mtl] [mfx] FATAL TOTK_MFX must be interp30, interp, interpmax or 0, got 2\n", line, sizeof line);
    check(f && !strcmp(f->name, "TOTK_MFX"), "MDIE on a setting (whole word: not TOTK_MFX_UPSCALE)");
    check(!launch_failed_setting("[totk] FAIL: language code too long\n[totk] TOTK_RUMBLE_GAIN=2 used\n", line, sizeof line), "a failure that names no setting");
    // ...or crashed within a minute: "acquire" without "exit" (runtime/game_slot.c history)
    struct tm tm = {0}; tm.tm_year = 126; tm.tm_mon = 9; tm.tm_mday = 20; tm.tm_hour = 12; tm.tm_isdst = -1;
    time_t t0 = mktime(&tm);
    const char *crashed = "2026-10-20T11:00:00 acquire owner=user pid=10 mode=game purpose=player\n"
                          "2026-10-20T11:30:00 exit owner=user pid=10 mode=game purpose=player held_s=1800\n"
                          "2026-10-20T12:00:00 acquire owner=user pid=11 mode=game purpose=player\n";
    check(launch_early_crash(crashed, t0 + 20, 60) == 20, "crash 20 s after start");
    check(launch_early_crash(crashed, t0 + 600, 60) == -1, "crash 10 minutes in: no window");
    const char *exited = "2026-10-20T12:00:00 acquire owner=user pid=11 mode=game purpose=player\n"
                         "2026-10-20T12:00:20 exit owner=user pid=11 mode=game purpose=player held_s=20\n";
    check(launch_early_crash(exited, t0 + 20, 60) == -1, "a game that exited");
    const char *agent = "2026-10-20T12:00:00 acquire owner=claude pid=12 mode=game purpose=test\n";
    check(launch_early_crash(agent, t0 + 20, 60) == -1, "an agent's test is not the player's");

    printf("{\"status\":\"%s\",\"probe\":\"launch-settings\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
    return failures != 0;
}
