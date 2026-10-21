/* The game's settings that the launcher's settings window offers (app/settings_window.m), as the environment variables the
   runtime reads, and what the launcher does with them (app/launcher.m). Plain C: games/totk/tests/launch_settings/probe.c
   checks it without AppKit.
   Saved settings live in the app's defaults (org.macnativegames.totk or .botw, key LaunchEnv: name -> value); a setting not
   saved stays unset, so the runtime uses its own default. Precedence: a variable already in the launcher's environment
   (open --env, a shell) > the saved value > the runtime's default. A saved value the runtime would refuse at start
   (runtime_fail / MDIE) is not passed on, so one bad value cannot stop every later start.
   The checks below follow the runtime's own parsers; each entry names the source it follows. */
#pragma once
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(GAME_BOTW) || defined(GAME_BOTW160)
#define LAUNCH_BOTW 1
#else
#define LAUNCH_BOTW 0
#endif
#if defined(TOTK_GAME_V121) || defined(TOTK_GAME_V143)
#define LAUNCH_ASPECT 1   /* aspect_install: game_patches.c (1.2.1) and game_patches_v143.c; other builds fail with TOTK_ASPECT */
#else
#define LAUNCH_ASPECT 0
#endif

typedef const char *(*LaunchCheck)(const char *value);   /* NULL = accepted, else why not */

static inline const char *launch_check_bool(const char *v) {
    return !strcmp(v, "0") || !strcmp(v, "1") ? NULL : "must be 0 or 1";
}
static inline int launch_whole_int(const char *v, long lo, long hi, long *out) {
    char *end; long n = strtol(v, &end, 10);
    if (end == v || *end || n < lo || n > hi) return 0;
    if (out) *out = n;
    return 1;
}
/* hle_nvn.c uncapped_cap: "display" or a rate (clamped to 30..120 there; refused outside it here) */
static inline const char *launch_check_hz(const char *v) {
    return !strcmp(v, "display") || launch_whole_int(v, 30, 120, NULL) ? NULL : "must be display or a rate from 30 to 120";
}
/* totk_mfx.m mfx_init */
static inline const char *launch_check_mfx(const char *v) {
    return !strcmp(v, "0") || !strcmp(v, "interp30") || !strcmp(v, "interp") || !strcmp(v, "interpmax") ? NULL
           : "must be 0, interp30, interp or interpmax";
}
/* gamepad_feedback.m rumble_start */
static inline const char *launch_check_gain(const char *v) {
    char *end; double g = strtod(v, &end);
    return end != v && !*end && g > 0 && g <= 4 ? NULL : "must be a number in (0, 4]";
}
/* motion_fusion.c runtime_motion_axes, without its side effects */
static inline const char *launch_check_axes(const char *v) {
    int used[3] = {0}; const char *p = v;
    for (int i = 0; i < 3; i++) {
        if (*p == '-' || *p == '+') p++;
        if (*p < 'x' || *p > 'z' || used[*p - 'x']) return "must name x, y and z once each, e.g. -x,y,-z";
        used[*p - 'x'] = 1; p++;
        if (i < 2 && *p++ != ',') return "must name x, y and z once each, e.g. -x,y,-z";
    }
    return *p ? "must name x, y and z once each, e.g. -x,y,-z" : NULL;
}
/* gamepad_layout.h gamepad_face_by_position */
static inline const char *launch_check_face(const char *v) {
    return !strcmp(v, "position") || !strcmp(v, "label") ? NULL : "must be position or label";
}
/* fps_overlay.c hud_compact: anything but "corners" is the default box, so only "corners" is saved */
static inline const char *launch_check_hud(const char *v) { return !strcmp(v, "corners") ? NULL : "must be corners"; }
/* aspect_patch.h aspect_requested ("0" is the default and is not saved) */
static inline const char *launch_check_aspect(const char *v) {
    double a = 0, w, h; char rest;
    if ((sscanf(v, "%lf:%lf%c", &w, &h, &rest) == 2 || sscanf(v, "%lfx%lf%c", &w, &h, &rest) == 2) && w > 0 && h > 0) a = w / h;
    else if (sscanf(v, "%lf%c", &w, &rest) == 1) a = w;
    return a >= 0.5 && a <= 8 ? NULL : "must be <w>:<h>, <w>x<h> or a ratio between 0.5 and 8";
}
/* services.c desired_language: the codes its automatic choice can give */
static const char *const launch_languages[] = {"en-US", "ja", "de", "fr", "es", "it", "nl", "ru", "ko", "zh-Hans", "pt",
#ifdef TOTK_GAME_V143
                                               "pt-BR",
#endif
                                               NULL};
static inline const char *launch_check_lang(const char *v) {
    for (int i = 0; launch_languages[i]; i++) if (!strcmp(v, launch_languages[i])) return NULL;
    return "is not one of the game's languages";
}
/* include/totk_ui_lang.h totk_ui_zh */
static inline const char *launch_check_ui_lang(const char *v) { return !strcmp(v, "zh") || !strcmp(v, "en") ? NULL : "must be zh or en"; }

/* def: what the runtime does when the variable is unset, as the value that means the same (for the window and the log). */
typedef struct { const char *name, *def; LaunchCheck check; } LaunchSetting;
static const LaunchSetting launch_settings[] = {
    {"TOTK_UNCAPPED", "1", launch_check_bool},            /* hle_nvn.c present_cfg */
    {"TOTK_UNCAPPED_HZ", "120", launch_check_hz},         /* hle_nvn.c uncapped_cap */
    {"TOTK_UNCAPPED_INTERVAL", LAUNCH_BOTW ? "1" : "0", launch_check_bool},   /* nvn_obj.h UNCAPPED_INTERVAL_FROM_ENV */
    {"TOTK_GPU_KEEPALIVE", "1", launch_check_bool},       /* nvn_mtl.m keepalive_start */
    {"TOTK_MFX", "0", launch_check_mfx},                  /* totk_mfx.m mfx_init */
#if !LAUNCH_BOTW
    {"TOTK_FPS60", "1", launch_check_bool},               /* game_patches.c fps60_enabled, game_patches_v143.c */
#endif
    {"TOTK_RUMBLE", "1", launch_check_bool},              /* hid_feedback.c env_on */
    {"TOTK_RUMBLE_GAIN", "1", launch_check_gain},         /* gamepad_feedback.m rumble_start */
    {"TOTK_GYRO", "1", launch_check_bool},                /* hid_feedback.c env_on */
    {"TOTK_GYRO_AXES", "x,y,z", launch_check_axes},       /* motion_fusion.c runtime_motion_axes */
    {"TOTK_FACE_BUTTONS", "auto", launch_check_face},     /* gamepad_layout.h: Nintendo controllers by label, others by position */
    {"TOTK_MFX_UPSCALE", "0", launch_check_bool},         /* totk_mfx_up.m mfxup_enabled (key 3: MetalFX 2K) */
    {"TOTK_MFX_OUTPUT", "0", launch_check_bool},          /* totk_mfx_up.m mfxup_output_enabled (key 3: MetalFX 2K) */
#if LAUNCH_ASPECT
    {"TOTK_ASPECT", "16:9", launch_check_aspect},         /* aspect_patch.h aspect_requested */
#endif
    {"TOTK_HUD", "box", launch_check_hud},                /* fps_overlay.c hud_compact */
    {"TOTK_FPS_OVERLAY", "1", launch_check_bool},         /* fps_overlay.c fps_overlay_enabled */
    {"TOTK_SHADER_OVERLAY", "1", launch_check_bool},      /* fps_overlay.c shader_overlay_enabled */
    {"TOTK_LOAD_OVERLAY", "1", launch_check_bool},        /* totk_sysload.c sysload_enabled */
    {"TOTK_LANG", "auto", launch_check_lang},             /* services.c desired_language */
    {"TOTK_UI_LANG", "auto", launch_check_ui_lang},       /* include/totk_ui_lang.h */
};
enum { LAUNCH_SETTINGS = sizeof launch_settings / sizeof *launch_settings };

static inline const LaunchSetting *launch_setting(const char *name) {
    for (int i = 0; i < LAUNCH_SETTINGS; i++) if (!strcmp(launch_settings[i].name, name)) return &launch_settings[i];
    return NULL;
}
/* NULL if the window offers name and the runtime takes value, else why not. */
static inline const char *launch_setting_check(const char *name, const char *value) {
    const LaunchSetting *s = launch_setting(name);
    if (!s) return "is not a setting of this game";
    if (!value || !*value) return "is empty";
    return s->check(value);
}

enum { LAUNCH_DEFAULT, LAUNCH_SAVED, LAUNCH_ENV, LAUNCH_IGNORED };
typedef struct { const char *name, *value, *problem; int source; } LaunchValue;
/* What each setting is at this launch. saved(name): the saved value or NULL; in_env(name): the variable was in the
   launcher's environment before the launcher changed anything. value: what to set (LAUNCH_SAVED), the refused value
   (LAUNCH_IGNORED), the saved value overridden by the environment or NULL (LAUNCH_ENV), NULL (LAUNCH_DEFAULT). */
static inline void launch_settings_resolve(const char *(*saved)(const char *name, void *ctx), int (*in_env)(const char *name, void *ctx),
                                           void *ctx, LaunchValue out[LAUNCH_SETTINGS]) {
    for (int i = 0; i < LAUNCH_SETTINGS; i++) {
        const char *name = launch_settings[i].name, *v = saved(name, ctx);
        LaunchValue r = {name, v, NULL, LAUNCH_DEFAULT};
        if (in_env(name, ctx)) r.source = LAUNCH_ENV;
        else if (v) { r.problem = launch_setting_check(name, v); r.source = r.problem ? LAUNCH_IGNORED : LAUNCH_SAVED; }
        out[i] = r;
    }
}

/* The last start failing on a setting: the last "[totk] FAIL:" (runtime_fail) or "FATAL" (MDIE) line of the previous log
   that names one of the settings as a whole word. Returns the setting (or NULL) and copies the line into line. */
static inline const LaunchSetting *launch_failed_setting(const char *log, char *line, size_t n) {
    const LaunchSetting *found = NULL;
    for (const char *p = log; p && *p;) {
        const char *end = strchr(p, '\n'); size_t len = end ? (size_t)(end - p) : strlen(p);
        char text[512]; size_t k = len < sizeof text - 1 ? len : sizeof text - 1;
        memcpy(text, p, k); text[k] = 0;
        if (strstr(text, "[totk] FAIL:") || strstr(text, "FATAL ")) {
            for (int i = 0; i < LAUNCH_SETTINGS; i++) {
                size_t nl = strlen(launch_settings[i].name);
                for (const char *q = strstr(text, launch_settings[i].name); q; q = strstr(q + 1, launch_settings[i].name)) {
                    char after = q[nl];
                    if (!(isupper((unsigned char)after) || isdigit((unsigned char)after) || after == '_')) {
                        found = &launch_settings[i];
                        if (line && n) snprintf(line, n, "%s", text);
                        break;
                    }
                }
            }
        }
        p = end ? end + 1 : NULL;
    }
    return found;
}

/* A player's game that ended without exiting (a crash: runtime/game_slot.c writes "exit" from atexit, which a signal skips)
   within limit seconds of taking the game slot. history: <slot>/history.log; died: when the game stopped (its log's last
   write). Returns the seconds it ran, or -1. */
static inline long launch_early_crash(const char *history, time_t died, long limit) {
    long pid = 0; time_t started = 0;
    for (const char *p = history; p && *p;) {
        const char *end = strchr(p, '\n'); size_t len = end ? (size_t)(end - p) : strlen(p);
        char text[1024]; size_t k = len < sizeof text - 1 ? len : sizeof text - 1;
        memcpy(text, p, k); text[k] = 0;
        char ts[32], event[32]; long line_pid = 0; const char *pp = strstr(text, " pid=");
        if (sscanf(text, "%31s %31s", ts, event) == 2 && pp && strstr(text, " owner=user ")) {
            line_pid = atol(pp + 5);
            if (!strcmp(event, "acquire")) {
                struct tm tm = {0};
                if (sscanf(ts, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
                    tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_isdst = -1;
                    started = mktime(&tm); pid = line_pid;
                }
            } else if (!strcmp(event, "exit") && line_pid == pid) pid = 0;
        }
        p = end ? end + 1 : NULL;
    }
    if (!pid || !started || died < started || died - started > limit) return -1;
    return (long)(died - started);
}
