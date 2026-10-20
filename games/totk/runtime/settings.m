/* Settings core for the native port: user-visible values, keymap profiles and their durable file.

   The public API lives in runtime/settings.h; this file owns everything behind it. Values live in two files under
   ~/Library/Application Support/MacNativeGames/TOTK (TOTK_SETTINGS_DIR overrides the directory, and an agent session
   keeps its own copy under <dir>/agent-settings/<owner>):

     settings.json   schemaVersion, settings { interp, outputRes, upscaler, renderRes, picture, aspect,
                     aspectRatio, hudCorners, hints, fpsOverlay, shaderOverlay, loadOverlay, faceLayout,
                     gameLanguage, uiLanguage }
     keymaps.json    schemaVersion, activeProfile, profiles [ { id, name, builtin, keys { keycode: action },
                     controller { map [17], modes [17] } } ]
     settings.lock   flock(2) around every read-modify-write of the two files

   Schema 2 splits the old "picture" preset into the independent target triple
   outputRes (TOTK_RES_*), upscaler (TOTK_UPSCALE_*) and renderRes
   (TOTK_RENDER_*).  A schema 1 file is read through the compatibility mapping
   (0 = 1080 native, 1 = 1440 MetalFX, 2 = 1440 native), marked dirty and
   rewritten as schema 2 on the next save (migration also covers keymaps.json,
   whose format did not change).  "picture" is still written as a derived
   view so a downgrade does not land on a file with no picture at all.

   Values are applied to the runtime by exporting the same TOTK_* variables the modules already read, and by calling
   the module setters for live changes. An explicit TOTK_* variable always wins: which variables were set before the
   first injection is captured at init and exposed through totk_setting_locked(); a locked value is read from the
   environment instead of the file and its setters report TOTK_SET_ERR_LOCKED.

   Writes are atomic: the new bytes go to a temporary file in the same directory, are fsync'ed, the previous file is
   copied to <name>.bak, the temporary file is renamed over the target and the directory is fsync'ed. A file that fails
   to parse (or is too large, or is a symlink) is renamed to <name>.corrupt-<timestamp>, the .bak is tried next, and
   defaults are the last resort. Unknown JSON fields are carried over on save. A file written by a newer schema is
   loaded best-effort but never written: all setters and saves report TOTK_SET_ERR_LOCKED.

   Threading: g_mu protects the values, the profiles and the JSON extras. The hot keymap lookup used by the input
   thread (totk_key_action / totk_key_button / totk_key_axis) reads an atomic snapshot published after every profile
   change, so it never takes g_mu. Reloads into input.c / gamepad.m are always issued after g_mu is released, which
   keeps their locks out of our lock order. */

#import <Foundation/Foundation.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "settings.h"
#include "controller_map.h"
#include "game_version.h"
#include "runtime_settings.h"

/* Implemented in the runtime adapter modules; declared here because those modules are plain C and do not share a
   settings-facing header. All signatures match runtime/game_patches.h, runtime/aspect_patch.h, fps_overlay.h and
   totk_sysload.h. */
void dm_gfx_set_flag(const char *name, int v);
int runtime_output_available(void);
/* TOTK_RES_* + TOTK_UPSCALE_* -> live output mode.  Returns 0 applied, -1
   unavailable, -2 a previous switch is still in flight, -3 stored: restart
   required before this target can be presented.  Implemented by
   runtime/game_patches*.c and declared in runtime/runtime.h; the local
   declaration keeps this translation unit self-contained like the rest of the
   adapter calls above. */
int runtime_output_mode_set_ex(int res, int upscaler);
int runtime_aspect_available(void);
int runtime_aspect_supported(void);
int runtime_aspect_set_enabled(int on);
int runtime_aspect_set_ratio(float ratio);
void fps_overlay_set_enabled(int on);
void fps_overlay_set_hints(int on);
void fps_overlay_set_compact(int on);
void fps_overlay_set_shader_enabled(int on);
void totk_sysload_set_enabled(int on);

#define SETTINGS_SCHEMA 2
#define SETTINGS_MAX_BYTES (256u * 1024u)
#define KEYMAPS_MAX_BYTES (1024u * 1024u)

/* One persisted snapshot of the live values. Languages are UTF-8 codes; empty means "follow the system". */
typedef struct {
    int interp;         /* TOTK_INTERP_* */
    int output_res;     /* TOTK_RES_*: the presented canvas size */
    int upscaler;       /* TOTK_UPSCALE_*: how the scene reaches output_res */
    int render_res;     /* TOTK_RENDER_*: scene render size, GAME = the game's choice */
    int aspect;         /* TOTK_ASPECT on/off */
    float ratio;        /* wide ratio while aspect is on */
    int hud_corners;    /* 0 compact, 1 the lines in the picture's corners */
    int hints;          /* master visibility of the hint line */
    int fps_overlay;
    int shader_overlay;
    int load_overlay;
    int face_layout;    /* TOTK_FACE_* */
    char game_language[8];
    char ui_language[16];
} Values;

typedef struct {
    char id[40];
    char name[TOTK_PROFILE_NAME_MAX];
    int builtin;
    uint8_t action[TOTK_KEYCODE_COUNT];
    uint16_t controller[CONTROLLER_BUTTONS];
    uint8_t controller_modes[CONTROLLER_BUTTONS];
} Profile;

enum {
    APPLY_INTERP,
    APPLY_PICTURE,      /* outputRes + upscaler */
    APPLY_RENDER,       /* renderRes, startup-time patch */
    APPLY_ASPECT,
    APPLY_RATIO,
    APPLY_HUD,
    APPLY_HINTS,
    APPLY_FPS,
    APPLY_SHADER,
    APPLY_LOAD,
    APPLY_FACE,
    APPLY_LANG,
    APPLY_UI_LANG,
    APPLY_PROFILE
};

static pthread_mutex_t g_mu = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static _Atomic int g_state;            /* 0 before init, 1 while loading, 2 done */
static int g_init_rc = -1;

static Values g_file;                  /* values as persisted (no environment overrides) */
static Values g_v;                     /* effective values (locked ones come from the environment) */
static int g_locked_interp;
static int g_locked_picture;           /* TOTK_OUTPUT_RES / TOTK_MFX_* pinned */
static int g_locked_render;            /* TOTK_RENDER_RES pinned */
static int g_locked_aspect;
static int g_locked_hud;
static int g_locked_fps;
static int g_locked_shader;
static int g_locked_load;
static int g_locked_face;
static int g_locked_lang;
static int g_locked_ui_lang;

static Profile g_profile[TOTK_PROFILE_MAX];
static int g_profile_count;
static int g_profile_active;
static _Atomic int g_hot_action[TOTK_KEYCODE_COUNT];

static int g_readonly;                 /* a file came from a newer schema */
static long g_newer_schema;
static int g_loaded;
static int g_dirty_settings;
static int g_dirty_keymaps;
/* outputRes once used the compact 0/1/2 enum; remember that a load accepted
   it so the next save rewrites the canonical 1080/1440/2160 numbers. */
static int g_legacy_output_encoding;
/* Set when a stored change could not be applied to this process: output mode
   refused live (restart-required) or renderRes changed (always startup-time). */
static int g_restart_output;
static int g_restart_render;
static int g_dir_ok;
static char g_dir[PATH_MAX];
static char g_settings_path[PATH_MAX];
static char g_keymaps_path[PATH_MAX];
static char g_lock_path[PATH_MAX];
static char g_err[512];
static _Atomic unsigned g_tmp_seq;

/* Unknown/extra fields of the loaded documents, kept so a later save does not drop them. */
static NSMutableDictionary *g_settings_root;
static NSMutableDictionary *g_settings_obj;
static NSMutableDictionary *g_keymaps_root;
static NSMutableDictionary *g_profile_raw;   /* profile id -> its JSON object */

static void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void err_append(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void values_default(Values *v);
static void default_profiles(void);
static void publish_active(void);

/* ------------------------------------------------------------------------------------------------------------ */
/* Small helpers                                                                                                 */

static void set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
}

static void err_append(const char *fmt, ...) {
    size_t n = strlen(g_err);
    if (n >= sizeof g_err - 1) return;
    if (n) {
        g_err[n++] = ';';
        g_err[n++] = ' ';
        g_err[n] = 0;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err + n, sizeof g_err - n, fmt, ap);
    va_end(ap);
}

static int env_on(const char *name) {
    const char *e = getenv(name);
    return e && strcmp(e, "0") != 0;
}

static int path_join(char *out, size_t cap, const char *dir, const char *name) {
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int path_suffix(char *out, size_t cap, const char *path, const char *suffix) {
    int n = snprintf(out, cap, "%s%s", path, suffix);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

static int valid_language(const char *s, size_t cap) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n >= cap) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '-' && c != '_') return 0;
    }
    return 1;
}

static int valid_name(const char *s) {
    if (!s || !*s) return 0;
    size_t n = strlen(s);
    if (n >= TOTK_PROFILE_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    NSString *str = [[NSString alloc] initWithBytes:s length:n encoding:NSUTF8StringEncoding];
    return str != nil;
}

/* Trim leading/trailing blanks; the result must still be a valid profile name. */
static int trimmed_name(const char *src, char out[TOTK_PROFILE_NAME_MAX]) {
    if (!src) return 0;
    const char *begin = src;
    while (*begin && isspace((unsigned char)*begin)) begin++;
    const char *end = src + strlen(src);
    while (end > begin && isspace((unsigned char)end[-1])) end--;
    size_t n = (size_t)(end - begin);
    if (n == 0 || n >= TOTK_PROFILE_NAME_MAX) return 0;
    memcpy(out, begin, n);
    out[n] = 0;
    return valid_name(out);
}

static int valid_id(const char *s) {
    if (!s || !*s) return 0;
    size_t n = strlen(s);
    if (n >= 40) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '-' && c != '_' && c != '.') return 0;
    }
    return 1;
}

static int reserved_keycode(unsigned code) {
    return code >= 18 && code <= 21;   /* runtime/input.c shortcuts (overlay, MFX, output mode, aspect) */
}

static NSString *profile_key(const char *id) {
    return id ? [NSString stringWithUTF8String:id] : nil;
}

/* A fresh profile id, not used by any loaded profile. Called with g_mu held. */
static void make_id(char out[40]) {
    for (int tries = 0; tries < 1000; tries++) {
        snprintf(out, 40, "p-%08x", (unsigned)arc4random());
        NSString *key = profile_key(out);
        if (!g_profile_raw || ![g_profile_raw objectForKey:key]) return;
    }
    snprintf(out, 40, "p-%u", (unsigned)arc4random());
}

/* macOS virtual key codes -> names for the keys the game uses; anything else prints as "Key <code>". */
static const char *keycode_name(unsigned code) {
    switch (code) {
        case 0: return "A";      case 1: return "S";      case 2: return "D";
        case 3: return "F";      case 4: return "H";      case 5: return "G";
        case 6: return "Z";      case 7: return "X";      case 8: return "C";
        case 9: return "V";      case 11: return "B";     case 12: return "Q";
        case 13: return "W";     case 14: return "E";     case 15: return "R";
        case 16: return "Y";     case 17: return "T";     case 18: return "1";
        case 19: return "2";     case 20: return "3";     case 21: return "4";
        case 22: return "6";     case 23: return "5";     case 24: return "=";
        case 25: return "9";     case 26: return "7";     case 27: return "-";
        case 28: return "8";     case 29: return "0";     case 30: return "]";
        case 31: return "O";     case 32: return "U";     case 33: return "[";
        case 34: return "I";     case 35: return "P";     case 36: return "Return";
        case 37: return "L";     case 38: return "J";     case 39: return "'";
        case 40: return "K";     case 41: return ";";     case 42: return "\\";
        case 43: return ",";     case 44: return "/";     case 45: return "N";
        case 46: return "M";     case 47: return ".";     case 48: return "Tab";
        case 49: return "Space"; case 50: return "`";     case 51: return "Delete";
        case 53: return "Escape"; case 55: return "Cmd";  case 56: return "Shift";
        case 57: return "CapsLock"; case 58: return "Option"; case 59: return "Control";
        case 60: return "Right Shift"; case 61: return "Right Option"; case 62: return "Right Control";
        case 63: return "Fn";    case 64: return "F17";   case 65: return "Keypad .";
        case 67: return "Keypad *"; case 69: return "Keypad +"; case 71: return "Keypad Clear";
        case 75: return "Keypad /"; case 76: return "Keypad Enter"; case 78: return "Keypad -";
        case 79: return "F18";   case 80: return "F19";   case 81: return "Keypad =";
        case 82: return "Keypad 0"; case 83: return "Keypad 1"; case 84: return "Keypad 2";
        case 85: return "Keypad 3"; case 86: return "Keypad 4"; case 87: return "Keypad 5";
        case 88: return "Keypad 6"; case 89: return "Keypad 7"; case 90: return "F20";
        case 91: return "Keypad 8"; case 92: return "Keypad 9"; case 93: return "JIS Yen";
        case 94: return "JIS Underscore"; case 95: return "Keypad ,"; case 96: return "F5";
        case 97: return "F6";    case 98: return "F7";    case 99: return "F3";
        case 100: return "F8";   case 101: return "F9";   case 103: return "F11";
        case 105: return "F13";  case 106: return "F16";  case 107: return "F14";
        case 109: return "F10";  case 111: return "F12";  case 113: return "F15";
        case 114: return "Help"; case 115: return "Home"; case 116: return "Page Up";
        case 117: return "Forward Delete"; case 118: return "F4"; case 119: return "End";
        case 120: return "F2";   case 121: return "Page Down"; case 122: return "F1";
        case 123: return "Left"; case 124: return "Right"; case 125: return "Down";
        case 126: return "Up";
        default: break;
    }
    static _Thread_local char buf[32];
    snprintf(buf, sizeof buf, "Key %u", code);
    return buf;
}

/* TOTK_ACTION_* -> Switch button bit (the order controller_map.h documents), -1 for none and the stick actions. */
static const int g_action_bit[TOTK_ACTION_COUNT] = {
    -1, 0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 4, 5, 12, 13, 14, 15,
    -1, -1, -1, -1, -1, -1, -1, -1
};

/* TOTK_ACTION_* stick actions -> (axis, sign). Axis ids match runtime/input.c: 0 lx, 1 ly, 2 rx, 3 ry. */
static const struct { int axis; int sign; } g_action_axis[TOTK_ACTION_COUNT] = {
    {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
    {0, -1}, {0, 1}, {1, 1}, {1, -1},
    {2, -1}, {2, 1}, {3, 1}, {3, -1}
};

static const char *const g_action_names[TOTK_ACTION_COUNT] = {
    "None", "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "-", "LStick", "RStick",
    "D-Left", "D-Up", "D-Right", "D-Down",
    "LStick Left", "LStick Right", "LStick Up", "LStick Down",
    "RStick Left", "RStick Right", "RStick Up", "RStick Down"
};

/* ------------------------------------------------------------------------------------------------------------ */
/* Defaults                                                                                                      */

static void assign_action(Profile *p, int code, int action) {
    if (code >= 0 && code < TOTK_KEYCODE_COUNT) p->action[code] = (uint8_t)action;
}

/* The README key map and the controller map that input.c / gamepad.m have always used. */
static void profile_defaults(Profile *p) {
    memset(p, 0, sizeof *p);
    assign_action(p, 38, TOTK_ACTION_A);
    assign_action(p, 40, TOTK_ACTION_B);
    assign_action(p, 49, TOTK_ACTION_B);
    assign_action(p, 34, TOTK_ACTION_X);
    assign_action(p, 32, TOTK_ACTION_Y);
    assign_action(p, 12, TOTK_ACTION_L);
    assign_action(p, 14, TOTK_ACTION_R);
    assign_action(p, 58, TOTK_ACTION_ZL);
    assign_action(p, 56, TOTK_ACTION_ZR);
    assign_action(p, 36, TOTK_ACTION_PLUS);
    assign_action(p, 48, TOTK_ACTION_MINUS);
    assign_action(p, 53, TOTK_ACTION_MINUS);
    assign_action(p, 6, TOTK_ACTION_LSTICK);
    assign_action(p, 8, TOTK_ACTION_RSTICK);
    assign_action(p, 3, TOTK_ACTION_DLEFT);
    assign_action(p, 17, TOTK_ACTION_DUP);
    assign_action(p, 4, TOTK_ACTION_DRIGHT);
    assign_action(p, 5, TOTK_ACTION_DDOWN);
    assign_action(p, 0, TOTK_ACTION_LSTICK_LEFT);   /* WASD */
    assign_action(p, 2, TOTK_ACTION_LSTICK_RIGHT);
    assign_action(p, 13, TOTK_ACTION_LSTICK_UP);
    assign_action(p, 1, TOTK_ACTION_LSTICK_DOWN);
    assign_action(p, 123, TOTK_ACTION_RSTICK_LEFT); /* arrows */
    assign_action(p, 124, TOTK_ACTION_RSTICK_RIGHT);
    assign_action(p, 126, TOTK_ACTION_RSTICK_UP);
    assign_action(p, 125, TOTK_ACTION_RSTICK_DOWN);
    controller_map_default(p->controller, p->controller_modes);
}

static void values_default(Values *v) {
    memset(v, 0, sizeof *v);
    v->interp = TOTK_INTERP_OFF;
    v->output_res = TOTK_RES_1080;
    v->upscaler = TOTK_UPSCALE_NATIVE;
    v->render_res = TOTK_RENDER_GAME;
    v->aspect = 0;
    v->ratio = 21.0f / 9.0f;
    v->hud_corners = 0;         /* compact, as fps_overlay.c defaults on the Mac */
    v->hints = 1;
    v->fps_overlay = 1;
    v->shader_overlay = 1;
    v->load_overlay = 1;
    v->face_layout = TOTK_FACE_AUTO;
    v->game_language[0] = 0;
    v->ui_language[0] = 0;
}

static void default_profiles(void) {
    g_profile_raw = [NSMutableDictionary dictionary];
    g_profile_count = 1;
    g_profile_active = 0;
    Profile *p = &g_profile[0];
    profile_defaults(p);
    snprintf(p->id, sizeof p->id, "default");
    snprintf(p->name, sizeof p->name, "Default");
    p->builtin = 1;
    g_profile_raw[@"default"] = [NSMutableDictionary dictionary];
}

static void publish_active(void) {
    int a = g_profile_active;
    if (a < 0 || a >= g_profile_count) a = 0;
    const Profile *p = &g_profile[a];
    for (int i = 0; i < TOTK_KEYCODE_COUNT; i++)
        atomic_store_explicit(&g_hot_action[i], p->action[i], memory_order_relaxed);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Environment: which TOTK_* variables the user set before us, and the values they imply                          */

static void capture_locks(void) {
    g_locked_interp = getenv("TOTK_MFX") != NULL;
    g_locked_picture = getenv("TOTK_OUTPUT_RES") != NULL || getenv("TOTK_MFX_UPSCALE") != NULL ||
                       getenv("TOTK_MFX_OUTPUT") != NULL;
    g_locked_render = getenv("TOTK_RENDER_RES") != NULL;
    g_locked_aspect = getenv("TOTK_ASPECT") != NULL;
    g_locked_hud = getenv("TOTK_HUD") != NULL;
    g_locked_fps = getenv("TOTK_FPS_OVERLAY") != NULL;
    g_locked_shader = getenv("TOTK_SHADER_OVERLAY") != NULL;
    g_locked_load = getenv("TOTK_LOAD_OVERLAY") != NULL;
    g_locked_face = getenv("TOTK_FACE_BUTTONS") != NULL;
    g_locked_lang = getenv("TOTK_LANG") != NULL;
    g_locked_ui_lang = getenv("TOTK_UI_LANG") != NULL;
}

static int parse_mfx(const char *e) {
    if (!e || !*e || !strcmp(e, "0")) return TOTK_INTERP_OFF;
    if (!strcmp(e, "interp30")) return TOTK_INTERP_30;
    if (!strcmp(e, "interp")) return TOTK_INTERP_HALF;
    if (!strcmp(e, "interpmax")) return TOTK_INTERP_MAX;
    return TOTK_INTERP_OFF;
}

static const char *mfx_env(int mode) {
    switch (mode) {
        case TOTK_INTERP_30: return "interp30";
        case TOTK_INTERP_HALF: return "interp";
        case TOTK_INTERP_MAX: return "interpmax";
        default: return "0";
    }
}

/* TOTK_ASPECT as a ratio; 0 for off/unset/malformed. Same grammar as aspect_patch.h. */
static float parse_ratio(const char *e) {
    if (!e || !*e || !strcmp(e, "0")) return 0;
    double a = 0, w = 0, h = 0;
    char rest = 0;
    if (((sscanf(e, "%lf:%lf%c", &w, &h, &rest) == 2) || (sscanf(e, "%lfx%lf%c", &w, &h, &rest) == 2)) &&
        w > 0 && h > 0)
        a = w / h;
    else if (sscanf(e, "%lf%c", &w, &rest) == 1)
        a = w;
    if (!(a >= 0.5 && a <= 8.0)) return 0;
    return (float)a;
}

static void ratio_string(char *out, size_t cap, float ratio) {
    if (fabsf(ratio - 21.0f / 9.0f) < 1e-4f) snprintf(out, cap, "21:9");
    else if (fabsf(ratio - 32.0f / 9.0f) < 1e-4f) snprintf(out, cap, "32:9");
    else if (fabsf(ratio - 3440.0f / 1440.0f) < 1e-5f) snprintf(out, cap, "3440:1440");
    else snprintf(out, cap, "%.6g", (double)ratio);
}

/* Parse "<w>x<h>" (the grammar game_patches*.c accepts).  0 when malformed. */
static int parse_size(const char *e, unsigned *w, unsigned *h) {
    unsigned pw = 0, ph = 0;
    char rest = 0;
    if (!e || sscanf(e, "%ux%u%c", &pw, &ph, &rest) != 2) return 0;
    if (pw < 64 || ph < 64) return 0;
    *w = pw;
    *h = ph;
    return 1;
}

static int res_from_size(unsigned w, unsigned h) {
    if (w >= 3840 || h >= 2160) return TOTK_RES_2160;
    if (w >= 2560 || h >= 1440) return TOTK_RES_1440;
    return TOTK_RES_1080;
}

/* The output target implied by the variables the user pinned.  TOTK_OUTPUT_RES
   and the two MetalFX flags are the only description of the target that the
   runtime accepts, and "auto" is shared by 1080 and both 1440/2160 MetalFX
   modes; in the locked case MetalFX + auto means the live 2K mode. */
static void output_from_env(Values *v) {
    const char *res = getenv("TOTK_OUTPUT_RES");
    int mfx = env_on("TOTK_MFX_UPSCALE") && env_on("TOTK_MFX_OUTPUT");
    v->upscaler = mfx ? TOTK_UPSCALE_METALFX : TOTK_UPSCALE_NATIVE;
    v->output_res = TOTK_RES_1080;
    if (!res || !*res || !strcmp(res, "auto")) {
        if (mfx) v->output_res = TOTK_RES_1440;
        return;
    }
    if (!strcmp(res, "0")) return;
    unsigned w = 0, h = 0;
    if (parse_size(res, &w, &h)) v->output_res = res_from_size(w, h);
}

static int render_from_env(void) {
    unsigned w = 0, h = 0;
    const char *e = getenv("TOTK_RENDER_RES");
    if (!parse_size(e, &w, &h)) return TOTK_RENDER_GAME;
    if (w == 1280 && h == 720) return TOTK_RENDER_720;
    if (w == 1920 && h == 1080) return TOTK_RENDER_1080;
    if (w == 2560 && h == 1440) return TOTK_RENDER_1440;
    if (w == 3840 && h == 2160) return TOTK_RENDER_2160;
    /* A hand-written size has no dedicated enum; round to the nearest step so
       the UI still shows the effective scene scale. */
    if (w <= 1280 && h <= 720) return TOTK_RENDER_720;
    if (w <= 1920 && h <= 1080) return TOTK_RENDER_1080;
    if (w <= 2560 && h <= 1440) return TOTK_RENDER_1440;
    return TOTK_RENDER_2160;
}

static const char *render_env(int render) {
    switch (render) {
        case TOTK_RENDER_720: return "1280x720";
        case TOTK_RENDER_1080: return "1920x1080";
        case TOTK_RENDER_1440: return "2560x1440";
        case TOTK_RENDER_2160: return "3840x2160";
        default: return NULL;   /* GAME: leave the variable unset */
    }
}

/* The RomFS output overlay directory the runtime reads for a true-2K/4K
   start: TOTK_ROMFS_PATCHES_OUT or the per-version default (romfs.c). */
static int output_overlay_dir(char *out, size_t cap) {
    const char *root = getenv("TOTK_ROMFS_PATCHES_OUT");
    if (root && *root) {
        int n = snprintf(out, cap, "%s", root);
        return n > 0 && (size_t)n < cap ? 0 : -1;
    }
    int n = snprintf(out, cap, "extracted/%s/romfs-patches-out", TOTK_GAME_DIR);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

/* 1 when <root>/<w>x<h>/manifest.tsv exists and is a non-empty regular file.
   romfs.c's overlay_load() requires that manifest before the game can start at
   that swapchain size; an absent/empty file would make the start fail. */
static int output_overlay_manifest(unsigned w, unsigned h) {
    char dir[PATH_MAX], tier[32], tier_path[PATH_MAX], path[PATH_MAX];
    struct stat st;
    if (output_overlay_dir(dir, sizeof dir) != 0) return 0;
    if (snprintf(tier, sizeof tier, "%ux%u", w, h) >= (int)sizeof tier) return 0;
    if (path_join(tier_path, sizeof tier_path, dir, tier) != 0) return 0;
    if (path_join(path, sizeof path, tier_path, "manifest.tsv") != 0) return 0;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static int output_overlay_4k(void) { return output_overlay_manifest(3840, 2160); }
static int output_overlay_2k(void) { return output_overlay_manifest(2560, 1440); }

/* Legacy schema-1 preset -> the v2 triple.  renderRes did not exist in v1. */
static void picture_compat_apply(Values *v, int picture) {
    if (picture == TOTK_PICTURE_MFX) {
        v->output_res = TOTK_RES_1440;
        v->upscaler = TOTK_UPSCALE_METALFX;
    } else if (picture == TOTK_PICTURE_NATIVE) {
        v->output_res = TOTK_RES_1440;
        v->upscaler = TOTK_UPSCALE_NATIVE;
    } else {
        v->output_res = TOTK_RES_1080;
        v->upscaler = TOTK_UPSCALE_NATIVE;
    }
}

/* The derived compatibility view.  1080 MetalFX collapses to the MetalFX
   preset; 2160 collapses to the same upscaler choice because the old menu had
   only three entries (new callers use the v2 getters instead). */
static int picture_value(const Values *v) {
    if (v->output_res == TOTK_RES_1080 && v->upscaler == TOTK_UPSCALE_NATIVE) return TOTK_PICTURE_OFF;
    if (v->upscaler == TOTK_UPSCALE_METALFX) return TOTK_PICTURE_MFX;
    return TOTK_PICTURE_NATIVE;
}

static void env_overrides(Values *v) {
    if (g_locked_interp) v->interp = parse_mfx(getenv("TOTK_MFX"));
    if (g_locked_picture) output_from_env(v);
    if (g_locked_render) v->render_res = render_from_env();
    if (g_locked_aspect) {
        float r = parse_ratio(getenv("TOTK_ASPECT"));
        v->aspect = r > 0;
        if (r > 0) v->ratio = r;
    }
    if (g_locked_hud) {
        const char *e = getenv("TOTK_HUD");
        v->hud_corners = e && !strcmp(e, "corners");
    }
    if (g_locked_fps) v->fps_overlay = env_on("TOTK_FPS_OVERLAY");
    if (g_locked_shader) v->shader_overlay = env_on("TOTK_SHADER_OVERLAY");
    if (g_locked_load) v->load_overlay = env_on("TOTK_LOAD_OVERLAY");
    if (g_locked_face) {
        const char *e = getenv("TOTK_FACE_BUTTONS");
        v->face_layout = e && !strcmp(e, "label") ? TOTK_FACE_LABEL :
                         e && !strcmp(e, "position") ? TOTK_FACE_POSITION : TOTK_FACE_AUTO;
    }
    if (g_locked_lang) {
        const char *e = getenv("TOTK_LANG");
        if (e && valid_language(e, sizeof v->game_language)) snprintf(v->game_language, sizeof v->game_language, "%s", e);
    }
    if (g_locked_ui_lang) {
        const char *e = getenv("TOTK_UI_LANG");
        if (e && valid_language(e, sizeof v->ui_language)) snprintf(v->ui_language, sizeof v->ui_language, "%s", e);
    }
}

/* setenv(overwrite = 0) while loading keeps an explicit variable; overwrite = 1 is for a live user change. */
static void env_set(const char *name, const char *value, int overwrite) {
    if (!value || !*value) {
        if (overwrite) unsetenv(name);
        return;
    }
    if (setenv(name, value, overwrite ? 1 : 0) != 0)
        fprintf(stderr, "[totk] settings: cannot set %s: %s\n", name, strerror(errno));
}

static void picture_env_export(const Values *v, int overwrite) {
    /* A 4K target (native or MetalFX) needs the game to start at
       3840x2160: that is what gives the present a 4K swapchain, and
       TOTK_MFX_OUTPUT then reaches it from the 1080p canvas.  A 2K MetalFX
       target still starts from "auto", where the adapter picks 2K. */
    const char *out = "auto";
    int mfx = v->upscaler == TOTK_UPSCALE_METALFX;
    if (v->output_res == TOTK_RES_2160) {
        if (output_overlay_4k()) {
            out = "3840x2160";
        } else {
            /* Keep the stored 4K preference; start at the best size this
               installation can actually load.  The UI shows 4K as needing
               the overlay and a restart, not as active.  The fallback is a
               native start (no MetalFX flags), exactly like native 4K:
               "auto" would itself require the 2K overlay and fail. */
            mfx = 0;
            if (output_overlay_2k()) {
                out = "2560x1440";
                fprintf(stderr, "[totk] settings: 4K %s needs the 3840x2160 output overlay; starting at 2560x1440 native (the stored 4K target is kept)\n",
                        v->upscaler == TOTK_UPSCALE_METALFX ? "MetalFX" : "native");
            } else {
                out = "0";
                fprintf(stderr, "[totk] settings: 4K %s needs the 3840x2160 output overlay and no 2K overlay is installed; starting at 1080p native (the stored 4K target is kept)\n",
                        v->upscaler == TOTK_UPSCALE_METALFX ? "MetalFX" : "native");
            }
        }
    } else if (v->output_res == TOTK_RES_1440 && v->upscaler == TOTK_UPSCALE_NATIVE) {
        out = "2560x1440";
    }
    env_set("TOTK_OUTPUT_RES", out, overwrite);
    env_set("TOTK_MFX_UPSCALE", mfx ? "1" : "0", overwrite);
    env_set("TOTK_MFX_OUTPUT", mfx ? "1" : "0", overwrite);
}

static void render_env_export(const Values *v, int overwrite) {
    const char *size = render_env(v->render_res);
    if (size) env_set("TOTK_RENDER_RES", size, overwrite);
    else if (overwrite) unsetenv("TOTK_RENDER_RES");
}

static void apply_env_all(const Values *v) {
    if (!g_locked_interp) env_set("TOTK_MFX", mfx_env(v->interp), 0);
    if (!g_locked_picture) picture_env_export(v, 0);
    if (!g_locked_render) render_env_export(v, 0);
    if (!g_locked_aspect && runtime_aspect_supported()) {
        char buf[32];
        if (v->aspect) ratio_string(buf, sizeof buf, v->ratio);
        else snprintf(buf, sizeof buf, "0");
        env_set("TOTK_ASPECT", buf, 0);
    }
    if (!g_locked_hud) env_set("TOTK_HUD", v->hud_corners ? "corners" : "compact", 0);
    if (!g_locked_fps) env_set("TOTK_FPS_OVERLAY", v->fps_overlay ? "1" : "0", 0);
    if (!g_locked_shader) env_set("TOTK_SHADER_OVERLAY", v->shader_overlay ? "1" : "0", 0);
    if (!g_locked_load) env_set("TOTK_LOAD_OVERLAY", v->load_overlay ? "1" : "0", 0);
    if (!g_locked_face) {
        const char *face = v->face_layout == TOTK_FACE_LABEL ? "label" :
                           v->face_layout == TOTK_FACE_POSITION ? "position" : "";
        env_set("TOTK_FACE_BUTTONS", face, 0);
    }
    if (!g_locked_lang) env_set("TOTK_LANG", v->game_language, 0);
    if (!g_locked_ui_lang) env_set("TOTK_UI_LANG", v->ui_language, 0);
}

/* Live apply. Runs without g_mu: the module setters take their own locks. The picture/aspect adapters can refuse a
   switch (no 2K start, layout pending, the aspect patch not installed yet); their setters call this before committing,
   so a refusal never changes the persisted or effective value. Returns TOTK_SET_OK, TOTK_SET_RESTART (picture only:
   the stored target needs a process restart), TOTK_SET_ERR_PENDING or TOTK_SET_ERR_UNAVAILABLE. */
static int apply_live(int which, const Values *v) {
    char buf[32];
    switch (which) {
        case APPLY_INTERP:
            env_set("TOTK_MFX", mfx_env(v->interp), 1);
            dm_gfx_set_flag("mfx_interp", v->interp);
            break;
        case APPLY_PICTURE: {
            if (!totk_setting_output_capable(v->output_res, v->upscaler))
                return TOTK_SET_ERR_UNAVAILABLE;
            int rc = runtime_output_mode_set_ex(v->output_res, v->upscaler);
            if (rc == -3) {
                /* Persisted target runs from the next start; export the
                   environment the patches read so that start reaches it. */
                picture_env_export(v, 1);
                return TOTK_SET_RESTART;
            }
            if (rc == -2) return TOTK_SET_ERR_PENDING;
            if (rc != 0) return TOTK_SET_ERR_UNAVAILABLE;
            picture_env_export(v, 1);
            break;
        }
        case APPLY_RENDER:
            /* TOTK_RENDER_RES is consumed once, while the game module is
               patched at load; there is no live path. */
            render_env_export(v, 1);
            return TOTK_SET_RESTART;
        case APPLY_ASPECT:
            if (!runtime_aspect_supported()) return TOTK_SET_ERR_UNAVAILABLE;
            /* The ratio has to be in place before the wide slot is switched on. */
            if (runtime_aspect_set_ratio(v->ratio) != 0) return TOTK_SET_ERR_UNAVAILABLE;
            if (runtime_aspect_set_enabled(v->aspect) != 0) return TOTK_SET_ERR_UNAVAILABLE;
            if (v->aspect) ratio_string(buf, sizeof buf, v->ratio);
            else snprintf(buf, sizeof buf, "0");
            env_set("TOTK_ASPECT", buf, 1);
            break;
        case APPLY_RATIO:
            if (!runtime_aspect_supported()) return TOTK_SET_ERR_UNAVAILABLE;
            if (runtime_aspect_set_ratio(v->ratio) != 0) return TOTK_SET_ERR_UNAVAILABLE;
            if (v->aspect) ratio_string(buf, sizeof buf, v->ratio);
            else snprintf(buf, sizeof buf, "0");
            env_set("TOTK_ASPECT", buf, 1);
            break;
        case APPLY_HUD:
            env_set("TOTK_HUD", v->hud_corners ? "corners" : "compact", 1);
            fps_overlay_set_compact(v->hud_corners ? 0 : 1);
            break;
        case APPLY_HINTS:
            fps_overlay_set_hints(v->hints);
            break;
        case APPLY_FPS:
            env_set("TOTK_FPS_OVERLAY", v->fps_overlay ? "1" : "0", 1);
            fps_overlay_set_enabled(v->fps_overlay);
            break;
        case APPLY_SHADER:
            env_set("TOTK_SHADER_OVERLAY", v->shader_overlay ? "1" : "0", 1);
            fps_overlay_set_shader_enabled(v->shader_overlay);
            break;
        case APPLY_LOAD:
            env_set("TOTK_LOAD_OVERLAY", v->load_overlay ? "1" : "0", 1);
            totk_sysload_set_enabled(v->load_overlay);
            break;
        case APPLY_FACE:
            env_set("TOTK_FACE_BUTTONS", v->face_layout == TOTK_FACE_LABEL ? "label" :
                    v->face_layout == TOTK_FACE_POSITION ? "position" : "", 1);
            totk_gamepad_reload();
            break;
        case APPLY_LANG:
            env_set("TOTK_LANG", v->game_language, 1);
            break;
        case APPLY_UI_LANG:
            env_set("TOTK_UI_LANG", v->ui_language, 1);
            break;
        case APPLY_PROFILE:
            totk_input_reload();
            totk_gamepad_reload();
            break;
        default:
            break;
    }
    return TOTK_SET_OK;
}

static void apply_all_init(const Values *v) {
    apply_env_all(v);
    /* Startup gets the picture/aspect/interp through the environment (the patches read it once); the other modules
       have live setters that do not need game memory, so they are applied directly as well. */
    totk_input_reload();
    totk_gamepad_reload();
    fps_overlay_set_compact(v->hud_corners ? 0 : 1);
    fps_overlay_set_hints(v->hints);
    fps_overlay_set_enabled(v->fps_overlay);
    fps_overlay_set_shader_enabled(v->shader_overlay);
    totk_sysload_set_enabled(v->load_overlay);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* File primitives                                                                                               */

static NSData *read_limited(const char *path, size_t limit, int *state) {
    *state = -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        *state = errno == ENOENT ? 0 : -1;
        return nil;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > limit) {
        close(fd);
        return nil;
    }
    NSMutableData *data = [NSMutableData dataWithCapacity:(NSUInteger)(st.st_size > 0 ? st.st_size : 1)];
    char buf[16384];
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return nil;
        }
        if (n == 0) break;
        total += (size_t)n;
        if (total > limit) {
            close(fd);
            return nil;
        }
        [data appendBytes:buf length:(NSUInteger)n];
    }
    close(fd);
    *state = 1;
    return data;
}

/* Keep a corrupt file for inspection instead of deleting it. */
static void archive_file(const char *path) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    for (int i = 0; i < 100; i++) {
        char dst[PATH_MAX];
        int n = i == 0 ? snprintf(dst, sizeof dst, "%s.corrupt-%s", path, stamp)
                       : snprintf(dst, sizeof dst, "%s.corrupt-%s-%d", path, stamp, i);
        if (n <= 0 || (size_t)n >= sizeof dst) break;
        if (access(dst, F_OK) == 0) continue;
        if (rename(path, dst) == 0) {
            fprintf(stderr, "[totk] settings: archived corrupt %s as %s\n", path, dst);
            return;
        }
        break;
    }
    fprintf(stderr, "[totk] settings: cannot archive corrupt %s: %s\n", path, strerror(errno));
}

static void sync_dir(void) {
    int fd = open(g_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

static int write_atomic_raw(const char *path, NSData *data) {
    char tmp[PATH_MAX];
    unsigned seq = atomic_fetch_add_explicit(&g_tmp_seq, 1, memory_order_relaxed);
    int n = snprintf(tmp, sizeof tmp, "%s.tmp.%ld.%u", path, (long)getpid(), seq);
    if (n <= 0 || (size_t)n >= sizeof tmp) {
        set_error("settings: path too long: %s", path);
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        set_error("settings: cannot create %s: %s", tmp, strerror(errno));
        return -1;
    }
    const uint8_t *bytes = data.bytes;
    size_t left = data.length;
    while (left) {
        ssize_t w = write(fd, bytes, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            set_error("settings: cannot write %s: %s", tmp, strerror(errno));
            close(fd);
            unlink(tmp);
            return -1;
        }
        bytes += w;
        left -= (size_t)w;
    }
    if (fsync(fd) != 0) {
        set_error("settings: cannot fsync %s: %s", tmp, strerror(errno));
        close(fd);
        unlink(tmp);
        return -1;
    }
    if (close(fd) != 0) {
        set_error("settings: cannot close %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        set_error("settings: cannot rename %s: %s", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }
    sync_dir();
    return 0;
}

/* Copy the current file to <path>.bak (best effort: a missing/unreadable target has nothing worth keeping). */
static void backup_current(const char *path, size_t limit) {
    int state = 0;
    NSData *old = read_limited(path, limit, &state);
    if (state != 1 || old.length == 0) return;
    char bak[PATH_MAX];
    if (path_suffix(bak, sizeof bak, path, ".bak") != 0) return;
    if (write_atomic_raw(bak, old) != 0) {
        fprintf(stderr, "[totk] settings: cannot back up %s (%s)\n", path, g_err);
    } else {
        fprintf(stderr, "[totk] settings: backed up %s\n", path);
    }
}

static int write_atomic(const char *path, NSData *data, size_t limit, int make_backup) {
    if (make_backup) backup_current(path, limit);
    return write_atomic_raw(path, data);
}

static NSData *json_data(NSDictionary *root) {
    NSError *error = nil;
    NSData *data = [NSJSONSerialization dataWithJSONObject:root
                                                   options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                                                     error:&error];
    if (!data) {
        set_error("settings: cannot serialize: %s",
                  error.localizedDescription ? error.localizedDescription.UTF8String : "unknown error");
        return nil;
    }
    return data;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Directory setup                                                                                               */

static NSString *directory_with_owner(NSString *base) {
    const char *owner = getenv("TOTK_SLOT_OWNER");
    if (!owner || !*owner || !strcmp(owner, "user")) return base;
    char safe[72];
    size_t n = 0;
    for (size_t i = 0; owner[i] && n + 1 < sizeof safe; i++) {
        unsigned char c = (unsigned char)owner[i];
        safe[n++] = (isalnum(c) || c == '-' || c == '_' || c == '.') ? (char)c : '_';
    }
    safe[n] = 0;
    if (!n) snprintf(safe, sizeof safe, "agent");
    if (n >= 64) safe[63] = 0;
    /* Never let the owner name become a path component that means "this" or "parent" directory. */
    if (!strcmp(safe, ".") || !strcmp(safe, "..")) snprintf(safe, sizeof safe, "agent");
    return [[base stringByAppendingPathComponent:@"agent-settings"] stringByAppendingPathComponent:profile_key(safe)];
}

static int make_settings_dir(NSString *base, NSError **error_out) {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSError *error = nil;
    if (![fm createDirectoryAtPath:base withIntermediateDirectories:YES
                        attributes:@{NSFilePosixPermissions: @0700} error:&error] &&
        ![fm fileExistsAtPath:base]) {
        if (error_out) *error_out = error;
        return 0;
    }
    const char *c = base.fileSystemRepresentation;
    if (!c) return 0;
    if (strlen(c) >= sizeof g_dir) return 0;
    snprintf(g_dir, sizeof g_dir, "%s", c);
    chmod(g_dir, 0700);
    return access(g_dir, W_OK | X_OK) == 0;
}

static void resolve_paths(void) {
    NSString *base = nil;
    const char *env = getenv("TOTK_SETTINGS_DIR");
    if (env && *env) {
        NSString *s = [NSString stringWithUTF8String:env];
        if (s) {
            s = [s stringByExpandingTildeInPath];
            if (!s.isAbsolutePath) {
                NSString *cwd = [NSFileManager defaultManager].currentDirectoryPath;
                s = [cwd stringByAppendingPathComponent:s];
            }
            base = s;
        }
    }
    if (!base) {
        base = [[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support"]
                stringByAppendingPathComponent:@"MacNativeGames/TOTK"];
    }
    base = directory_with_owner(base);

    NSError *error = nil;
    g_dir_ok = make_settings_dir(base, &error);
    if (!g_dir_ok) {
        NSString *fallback = [[NSTemporaryDirectory() stringByAppendingPathComponent:@"MacNativeGames/TOTK"]
                              stringByAppendingPathComponent:@""];
        fallback = directory_with_owner(fallback);
        g_dir_ok = make_settings_dir(fallback, &error);
    }
    if (!g_dir_ok) {
        g_dir[0] = 0;
        set_error("settings: cannot create a settings directory (%s)",
                  error.localizedDescription ? error.localizedDescription.UTF8String : "unknown error");
        return;
    }
    if (path_join(g_settings_path, sizeof g_settings_path, g_dir, "settings.json") != 0 ||
        path_join(g_keymaps_path, sizeof g_keymaps_path, g_dir, "keymaps.json") != 0 ||
        path_join(g_lock_path, sizeof g_lock_path, g_dir, "settings.lock") != 0) {
        g_dir_ok = 0;
        set_error("settings: directory path is too long");
        g_dir[0] = 0;
    }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Loading                                                                                                       */

typedef enum { JSON_MISSING = 0, JSON_OK = 1, JSON_BAD = -1 } JsonState;

static JsonState read_json(const char *path, size_t limit, NSDictionary **out) {
    int state = 0;
    NSData *data = read_limited(path, limit, &state);
    if (state == 0) return JSON_MISSING;
    if (state < 0 || !data) {
        fprintf(stderr, "[totk] settings: cannot read %s\n", path);
        return JSON_BAD;
    }
    NSError *error = nil;
    id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    if (![object isKindOfClass:[NSDictionary class]]) {
        fprintf(stderr, "[totk] settings: %s is not a JSON object: %s\n", path,
                error.localizedDescription ? error.localizedDescription.UTF8String : "wrong root type");
        return JSON_BAD;
    }
    *out = [object isKindOfClass:[NSMutableDictionary class]] ? object : [object mutableCopy];
    return JSON_OK;
}

static long schema_of(NSDictionary *root) {
    id version = root[@"schemaVersion"];
    return [version isKindOfClass:[NSNumber class]] ? [version longValue] : SETTINGS_SCHEMA;
}

static int number_int(id object, int *out) {
    if (![object isKindOfClass:[NSNumber class]]) return 0;
    *out = [object intValue];
    return 1;
}

static int number_bool(id object, int *out) {
    if (![object isKindOfClass:[NSNumber class]]) return 0;
    *out = [object boolValue] ? 1 : 0;
    return 1;
}

static NSString *string_obj(id object) {
    return [object isKindOfClass:[NSString class]] ? object : nil;
}

/* Returns 0 or -1 for a structurally invalid document (wrong type under "settings"); individual bad values are
   ignored so one hand-edited field does not throw the whole file away. */
static int parse_settings(NSDictionary *root, Values *v) {
    id object = root[@"settings"];
    if (object && ![object isKindOfClass:[NSDictionary class]]) return -1;
    if (![object isKindOfClass:[NSDictionary class]]) return 0;
    NSDictionary *s = object;
    int n;
    if (number_int(s[@"interp"], &n) && n >= 0 && n <= TOTK_INTERP_MAX) v->interp = n;
    int have_v2 = 0;
    if (number_int(s[@"outputRes"], &n)) {
        /* Canonical values are the line counts (settings.h).  Schema-2 files
           written by the first v2 build used 0/1/2; accept and rewrite them. */
        int canonical = n;
        if (n == 0 || n == 1 || n == 2) {
            canonical = n == 0 ? TOTK_RES_1080 : n == 1 ? TOTK_RES_1440 : TOTK_RES_2160;
            g_legacy_output_encoding = 1;
        }
        if (canonical == TOTK_RES_1080 || canonical == TOTK_RES_1440 || canonical == TOTK_RES_2160) {
            v->output_res = canonical;
            have_v2 = 1;
        }
    }
    if (number_int(s[@"upscaler"], &n) && n >= TOTK_UPSCALE_NATIVE && n <= TOTK_UPSCALE_METALFX) {
        v->upscaler = n;
        have_v2 = 1;
    }
    if (number_int(s[@"renderRes"], &n) && n >= TOTK_RENDER_GAME && n <= TOTK_RENDER_2160) {
        v->render_res = n;
        have_v2 = 1;
    }
    /* Schema 1 (or a partly hand-edited v2 file) still has the preset.  Only
       use it when no v2 field was present, so a v2 file can never be dragged
       back to 1440 by a stale picture value. */
    if (!have_v2 && number_int(s[@"picture"], &n) && n >= 0 && n <= TOTK_PICTURE_NATIVE)
        picture_compat_apply(v, n);
    if (number_bool(s[@"aspect"], &n)) v->aspect = n;
    id ratio = s[@"aspectRatio"];
    if ([ratio isKindOfClass:[NSNumber class]]) {
        double r = [ratio doubleValue];
        if (isfinite(r) && r >= 0.5 && r <= 8.0) v->ratio = (float)r;
    }
    if (number_bool(s[@"hudCorners"], &n)) v->hud_corners = n;
    if (number_bool(s[@"hints"], &n)) v->hints = n;
    if (number_bool(s[@"fpsOverlay"], &n)) v->fps_overlay = n;
    if (number_bool(s[@"shaderOverlay"], &n)) v->shader_overlay = n;
    if (number_bool(s[@"loadOverlay"], &n)) v->load_overlay = n;
    if (number_int(s[@"faceLayout"], &n) && n >= 0 && n <= TOTK_FACE_LABEL) v->face_layout = n;
    NSString *language = string_obj(s[@"gameLanguage"]);
    if (language && valid_language(language.UTF8String, sizeof v->game_language))
        snprintf(v->game_language, sizeof v->game_language, "%s", language.UTF8String);
    NSString *ui = string_obj(s[@"uiLanguage"]);
    if (ui && valid_language(ui.UTF8String, sizeof v->ui_language))
        snprintf(v->ui_language, sizeof v->ui_language, "%s", ui.UTF8String);
    return 0;
}

static void reset_keymaps_state(void) {
    g_keymaps_root = nil;
    g_profile_raw = nil;
    g_profile_count = 0;
    g_profile_active = 0;
}

static void parse_keys(id object, uint8_t actions[TOTK_KEYCODE_COUNT]) {
    if ([object isKindOfClass:[NSDictionary class]]) {
        NSDictionary *d = object;
        for (id key in d) {
            NSString *name = string_obj(key);
            id value = d[key];
            if (!name || ![value isKindOfClass:[NSNumber class]]) continue;
            const char *c = name.UTF8String;
            if (!c || !*c) continue;
            char *end = NULL;
            long code = strtol(c, &end, 10);
            long action = [value longValue];
            if (end == c || *end || code < 0 || code >= TOTK_KEYCODE_COUNT) continue;
            if (action < 0 || action >= TOTK_ACTION_COUNT) continue;
            if (reserved_keycode((unsigned)code)) continue;
            actions[code] = (uint8_t)action;
        }
    } else if ([object isKindOfClass:[NSArray class]]) {
        NSArray *array = object;
        for (NSUInteger i = 0; i < array.count && i < TOTK_KEYCODE_COUNT; i++) {
            id value = array[i];
            if (![value isKindOfClass:[NSNumber class]]) continue;
            long action = [value longValue];
            if (action < 0 || action >= TOTK_ACTION_COUNT) continue;
            if (reserved_keycode((unsigned)i)) continue;
            actions[i] = (uint8_t)action;
        }
    }
}

static void parse_controller(id object, uint16_t map[CONTROLLER_BUTTONS], uint8_t modes[CONTROLLER_BUTTONS]) {
    controller_map_default(map, modes);
    if (![object isKindOfClass:[NSDictionary class]]) return;
    NSDictionary *d = object;
    id map_object = d[@"map"];
    if ([map_object isKindOfClass:[NSArray class]]) {
        NSArray *array = map_object;
        for (NSUInteger i = 0; i < array.count && i < CONTROLLER_BUTTONS; i++) {
            id value = array[i];
            if (![value isKindOfClass:[NSNumber class]]) continue;
            map[i] = (uint16_t)([value unsignedIntValue] & 0xffffu);
        }
    }
    id modes_object = d[@"modes"];
    if ([modes_object isKindOfClass:[NSArray class]]) {
        NSArray *array = modes_object;
        for (NSUInteger i = 0; i < array.count && i < CONTROLLER_BUTTONS; i++) {
            id value = array[i];
            if (![value isKindOfClass:[NSNumber class]]) continue;
            modes[i] = (uint8_t)([value unsignedIntValue] & 0xffu);
        }
    }
}

/* Fills g_profile / g_profile_count / g_profile_active / g_keymaps_root / g_profile_raw. */
static int parse_keymaps(NSDictionary *root, int *newer) {
    *newer = schema_of(root) > SETTINGS_SCHEMA;
    reset_keymaps_state();
    g_keymaps_root = [root mutableCopy];
    g_profile_raw = [NSMutableDictionary dictionary];
    id profiles_object = root[@"profiles"];
    if (!profiles_object) return 0;
    if (![profiles_object isKindOfClass:[NSArray class]]) return -1;
    NSArray *profiles = profiles_object;
    NSMutableSet *seen = [NSMutableSet set];
    for (NSUInteger i = 0; i < profiles.count && g_profile_count < TOTK_PROFILE_MAX; i++) {
        id object = profiles[i];
        if (![object isKindOfClass:[NSDictionary class]]) continue;
        NSDictionary *d = object;
        Profile profile;
        profile_defaults(&profile);

        char id[40] = {0};
        NSString *id_string = string_obj(d[@"id"]);
        const char *id_c = id_string ? id_string.UTF8String : NULL;
        if (id_c && valid_id(id_c) && ![seen containsObject:id_string]) snprintf(id, sizeof id, "%s", id_c);
        if (!id[0]) make_id(id);
        snprintf(profile.id, sizeof profile.id, "%s", id);
        [seen addObject:profile_key(profile.id)];

        NSString *name_string = string_obj(d[@"name"]);
        const char *name_c = name_string ? name_string.UTF8String : NULL;
        if (name_c && valid_name(name_c)) snprintf(profile.name, sizeof profile.name, "%s", name_c);
        else snprintf(profile.name, sizeof profile.name, "Profile %d", g_profile_count + 1);

        int builtin = 0;
        if (number_bool(d[@"builtin"], &builtin)) profile.builtin = builtin;
        else if (!strcmp(profile.id, "default")) profile.builtin = 1;

        parse_keys(d[@"keys"], profile.action);
        parse_controller(d[@"controller"], profile.controller, profile.controller_modes);
        g_profile[g_profile_count] = profile;
        g_profile_raw[profile_key(profile.id)] = [d mutableCopy];
        g_profile_count++;
    }
    if (g_profile_count == 0) return 0;

    id active_object = root[@"activeProfile"];
    int active = 0;
    if (number_int(active_object, &active)) {
        if (active < 0) active = 0;
        if (active >= g_profile_count) active = g_profile_count - 1;
    } else if (string_obj(active_object)) {
        const char *wanted = string_obj(active_object).UTF8String;
        active = 0;
        if (wanted) {
            for (int i = 0; i < g_profile_count; i++) {
                if (!strcmp(g_profile[i].id, wanted)) {
                    active = i;
                    break;
                }
            }
        }
    }
    g_profile_active = active;
    return 0;
}

static int load_settings_file(void) {
    char backup[PATH_MAX];
    if (path_suffix(backup, sizeof backup, g_settings_path, ".bak") != 0) return 0;
    int corrupt = 0, used_backup = 0, newer = 0;
    long newer_version = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        const char *path = attempt == 0 ? g_settings_path : backup;
        NSDictionary *root = nil;
        JsonState state = read_json(path, SETTINGS_MAX_BYTES, &root);
        if (state == JSON_MISSING) {
            if (attempt == 0) return 0;
            break;
        }
        if (state == JSON_BAD) {
            corrupt = 1;
            archive_file(path);
            continue;
        }
        Values values;
        values_default(&values);
        long version = schema_of(root);
        g_legacy_output_encoding = 0;
        if (parse_settings(root, &values) != 0) {
            corrupt = 1;
            archive_file(path);
            continue;
        }
        newer = version > SETTINGS_SCHEMA;
        newer_version = version;
        g_file = values;
        g_settings_root = [root mutableCopy];
        g_settings_obj = [root[@"settings"] isKindOfClass:[NSDictionary class]] ? [root[@"settings"] mutableCopy]
                                                                               : [NSMutableDictionary dictionary];
        /* A schema 1 document was read through the picture mapping above; a
           schema-2 document from the first v2 build used 0/1/2 for outputRes.
           Mark both for a canonical rewrite (flush on exit / next save). */
        if (version < SETTINGS_SCHEMA || g_legacy_output_encoding) g_dirty_settings = 1;
        if (attempt == 1) used_backup = 1;
        if (used_backup && !newer) {
            /* Put the usable backup back in place so the next start is a clean load. */
            NSData *data = json_data(root);
            if (data) write_atomic(g_settings_path, data, SETTINGS_MAX_BYTES, 0);
        }
        if (corrupt) err_append("settings.json was corrupt; %s", used_backup ? "restored settings.json.bak"
                                                                             : "used defaults");
        if (used_backup && !corrupt) err_append("settings.json unreadable; restored settings.json.bak");
        if (newer) {
            g_readonly = 1;
            g_newer_schema = newer_version;
            err_append("settings.json schema %ld is newer than %d; read-only", newer_version, SETTINGS_SCHEMA);
        }
        return corrupt || used_backup || newer ? 1 : 0;
    }
    values_default(&g_file);
    g_settings_root = nil;
    g_settings_obj = nil;
    if (corrupt) {
        err_append("settings.json and its backup were unusable; using defaults");
        return 1;
    }
    return 0;
}

static int load_keymaps_file(void) {
    char backup[PATH_MAX];
    if (path_suffix(backup, sizeof backup, g_keymaps_path, ".bak") != 0) return 0;
    int corrupt = 0, used_backup = 0, newer = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        const char *path = attempt == 0 ? g_keymaps_path : backup;
        NSDictionary *root = nil;
        JsonState state = read_json(path, KEYMAPS_MAX_BYTES, &root);
        if (state == JSON_MISSING) {
            if (attempt == 0) return 0;
            break;
        }
        if (state == JSON_BAD) {
            corrupt = 1;
            archive_file(path);
            reset_keymaps_state();
            continue;
        }
        int file_newer = 0;
        if (parse_keymaps(root, &file_newer) != 0) {
            corrupt = 1;
            archive_file(path);
            reset_keymaps_state();
            continue;
        }
        newer = file_newer;
        if (!newer && schema_of(root) < SETTINGS_SCHEMA) g_dirty_keymaps = 1;   /* v1 format, rewrite as v2 */
        if (attempt == 1) used_backup = 1;
        if (used_backup && !newer) {
            NSData *data = json_data(root);
            if (data) write_atomic(g_keymaps_path, data, KEYMAPS_MAX_BYTES, 0);
        }
        if (corrupt) err_append("keymaps.json was corrupt; %s", used_backup ? "restored keymaps.json.bak"
                                                                            : "using the built-in profile");
        if (used_backup && !corrupt) err_append("keymaps.json unreadable; restored keymaps.json.bak");
        if (newer) {
            g_readonly = 1;
            g_newer_schema = MAX(g_newer_schema, schema_of(root));
            err_append("keymaps.json schema %ld is newer than %d; read-only", schema_of(root), SETTINGS_SCHEMA);
        }
        return corrupt || used_backup || newer ? 1 : 0;
    }
    reset_keymaps_state();
    if (corrupt) {
        err_append("keymaps.json and its backup were unusable; using the built-in profile");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Saving                                                                                                        */

static NSMutableDictionary *build_settings_root(void) {
    NSMutableDictionary *root = g_settings_root ? [g_settings_root mutableCopy] : [NSMutableDictionary dictionary];
    root[@"schemaVersion"] = @(SETTINGS_SCHEMA);
    NSMutableDictionary *settings = g_settings_obj ? [g_settings_obj mutableCopy] : [NSMutableDictionary dictionary];
    settings[@"interp"] = @(g_file.interp);
    settings[@"outputRes"] = @(g_file.output_res);
    settings[@"upscaler"] = @(g_file.upscaler);
    settings[@"renderRes"] = @(g_file.render_res);
    settings[@"picture"] = @(picture_value(&g_file));
    settings[@"aspect"] = @(g_file.aspect);
    settings[@"aspectRatio"] = @(g_file.ratio);
    settings[@"hudCorners"] = @(g_file.hud_corners);
    settings[@"hints"] = @(g_file.hints);
    settings[@"fpsOverlay"] = @(g_file.fps_overlay);
    settings[@"shaderOverlay"] = @(g_file.shader_overlay);
    settings[@"loadOverlay"] = @(g_file.load_overlay);
    settings[@"faceLayout"] = @(g_file.face_layout);
    settings[@"gameLanguage"] = [NSString stringWithUTF8String:g_file.game_language];
    settings[@"uiLanguage"] = [NSString stringWithUTF8String:g_file.ui_language];
    root[@"settings"] = settings;
    return root;
}

static NSMutableDictionary *build_keymaps_root(void) {
    NSMutableDictionary *root = g_keymaps_root ? [g_keymaps_root mutableCopy] : [NSMutableDictionary dictionary];
    root[@"schemaVersion"] = @(SETTINGS_SCHEMA);
    root[@"activeProfile"] = @(g_profile_active);
    NSMutableArray *profiles = [NSMutableArray arrayWithCapacity:(NSUInteger)g_profile_count];
    for (int i = 0; i < g_profile_count; i++) {
        Profile *profile = &g_profile[i];
        NSString *key = profile_key(profile->id);
        NSMutableDictionary *object = g_profile_raw && g_profile_raw[key] ? [g_profile_raw[key] mutableCopy]
                                                                         : [NSMutableDictionary dictionary];
        object[@"id"] = key ? key : @"";
        object[@"name"] = [NSString stringWithUTF8String:profile->name];
        object[@"builtin"] = @(profile->builtin);

        NSMutableDictionary *keys = [object[@"keys"] isKindOfClass:[NSDictionary class]]
                                    ? [object[@"keys"] mutableCopy] : [NSMutableDictionary dictionary];
        for (id existing in keys.allKeys) {
            NSString *name = string_obj(existing);
            if (!name) continue;
            const char *c = name.UTF8String;
            if (!c) continue;
            char *end = NULL;
            long code = strtol(c, &end, 10);
            if (end != c && !*end && code >= 0 && code < TOTK_KEYCODE_COUNT) [keys removeObjectForKey:existing];
        }
        for (int code = 0; code < TOTK_KEYCODE_COUNT; code++) {
            if (!profile->action[code]) continue;
            keys[[NSString stringWithFormat:@"%d", code]] = @(profile->action[code]);
        }
        object[@"keys"] = keys;

        NSMutableDictionary *controller = [object[@"controller"] isKindOfClass:[NSDictionary class]]
                                          ? [object[@"controller"] mutableCopy] : [NSMutableDictionary dictionary];
        NSMutableArray *map = [NSMutableArray arrayWithCapacity:CONTROLLER_BUTTONS];
        NSMutableArray *modes = [NSMutableArray arrayWithCapacity:CONTROLLER_BUTTONS];
        for (int j = 0; j < CONTROLLER_BUTTONS; j++) {
            [map addObject:@(profile->controller[j])];
            [modes addObject:@(profile->controller_modes[j])];
        }
        controller[@"map"] = map;
        controller[@"modes"] = modes;
        object[@"controller"] = controller;
        [profiles addObject:object];
    }
    root[@"profiles"] = profiles;
    return root;
}

static int save_files(int want_settings, int want_keymaps) {
    if (g_readonly) {
        set_error("settings are read-only (schema %ld is newer than %d)", g_newer_schema, SETTINGS_SCHEMA);
        return TOTK_SET_ERR_LOCKED;
    }
    if (!g_dir_ok) {
        if (!g_err[0]) set_error("settings directory is unavailable");
        return TOTK_SET_ERR_SAVE;
    }
    int lock_fd = open(g_lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        set_error("settings: cannot open %s: %s", g_lock_path, strerror(errno));
        return TOTK_SET_ERR_SAVE;
    }
    if (flock(lock_fd, LOCK_EX) != 0) {
        set_error("settings: cannot lock %s: %s", g_lock_path, strerror(errno));
        close(lock_fd);
        return TOTK_SET_ERR_SAVE;
    }
    int result = TOTK_SET_OK;
    if (want_settings && result == TOTK_SET_OK) {
        NSData *data = json_data(build_settings_root());
        if (!data) result = TOTK_SET_ERR_SAVE;
        else if (write_atomic(g_settings_path, data, SETTINGS_MAX_BYTES, 1) != 0) result = TOTK_SET_ERR_SAVE;
        else g_dirty_settings = 0;
    }
    if (want_keymaps && result == TOTK_SET_OK) {
        NSData *data = json_data(build_keymaps_root());
        if (!data) result = TOTK_SET_ERR_SAVE;
        else if (write_atomic(g_keymaps_path, data, KEYMAPS_MAX_BYTES, 1) != 0) result = TOTK_SET_ERR_SAVE;
        else g_dirty_keymaps = 0;
    }
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    return result;
}

static int persist(int keymaps) {
    if (keymaps) g_dirty_keymaps = 1;
    else g_dirty_settings = 1;
    return save_files(!keymaps, keymaps);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Init                                                                                                          */

static int render_res_allowed(const Values *v, int render);
static int normalize_render_res(Values *v);

static void do_init(void) {
    atomic_store_explicit(&g_state, 1, memory_order_release);
    pthread_mutex_lock(&g_mu);

    values_default(&g_file);
    g_v = g_file;
    g_err[0] = 0;
    g_dirty_settings = g_dirty_keymaps = 0;
    g_restart_output = g_restart_render = 0;
    g_readonly = 0;
    g_newer_schema = 0;
    g_profile_count = 0;
    g_profile_active = 0;
    g_profile_raw = nil;
    g_settings_root = nil;
    g_settings_obj = nil;
    g_keymaps_root = nil;
    capture_locks();
    resolve_paths();

    int result = 0;
    if (g_dir_ok) {
        int settings = load_settings_file();
        int keymaps = load_keymaps_file();
        if (settings > 0 || keymaps > 0) result = 1;
        if (normalize_render_res(&g_file)) g_dirty_settings = 1;
        g_v = g_file;
        env_overrides(&g_v);
        normalize_render_res(&g_v);
        if (g_profile_count == 0) default_profiles();
        publish_active();
        g_loaded = 1;
    } else {
        g_v = g_file;
        env_overrides(&g_v);
        normalize_render_res(&g_v);
        default_profiles();
        publish_active();
        result = -1;
    }

    Values effective = g_v;
    int rc = result;
    pthread_mutex_unlock(&g_mu);

    /* Apply outside g_mu: the input/gamepad reloads take their own locks. */
    apply_all_init(&effective);
    g_init_rc = rc;
    atomic_store_explicit(&g_state, 2, memory_order_release);
}

static void ensure_init(void) {
    if (atomic_load_explicit(&g_state, memory_order_acquire) != 0) return;
    pthread_once(&g_once, do_init);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Session info                                                                                                  */

int totk_settings_init(void) {
    ensure_init();
    return g_init_rc;
}

int totk_settings_loaded(void) {
    ensure_init();
    return g_loaded;
}

const char *totk_settings_error(void) {
    ensure_init();
    return g_err;
}

const char *totk_settings_path(void) {
    ensure_init();
    return g_settings_path;
}

int totk_settings_save_now(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int result = save_files(1, 1);
    pthread_mutex_unlock(&g_mu);
    return result;
}

void totk_settings_flush(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    if (!g_readonly && (g_dirty_settings || g_dirty_keymaps)) save_files(1, 1);
    pthread_mutex_unlock(&g_mu);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Live values                                                                                                   */

int totk_setting_locked(const char *name) {
    ensure_init();
    if (!name) return 0;
    if (!strcmp(name, "interp")) return g_locked_interp;
    if (!strcmp(name, "picture")) return g_locked_picture;
    if (!strcmp(name, "render")) return g_locked_render;
    if (!strcmp(name, "aspect")) return g_locked_aspect;
    if (!strcmp(name, "hud")) return g_locked_hud;
    if (!strcmp(name, "fps")) return g_locked_fps;
    if (!strcmp(name, "shader")) return g_locked_shader;
    if (!strcmp(name, "load")) return g_locked_load;
    if (!strcmp(name, "face")) return g_locked_face;
    if (!strcmp(name, "lang")) return g_locked_lang;
    if (!strcmp(name, "ui_lang")) return g_locked_ui_lang;
    return 0;   /* hints has no environment variable of its own */
}

int totk_capability_output(void) {
    ensure_init();
    return runtime_output_available() > 0;
}

int totk_setting_output_capable(int res, int upscaler) {
    ensure_init();
    if (res != TOTK_RES_1080 && res != TOTK_RES_1440 && res != TOTK_RES_2160) return 0;
    if (upscaler != TOTK_UPSCALE_NATIVE && upscaler != TOTK_UPSCALE_METALFX) return 0;
    /* 1080p is only the native "off" mode; MetalFX at 1080p is not a
       separate presentation mode (it would be MetalFX 2K). */
    if (res == TOTK_RES_1080) return upscaler == TOTK_UPSCALE_NATIVE;
    if (res == TOTK_RES_1440) return 1;
    /* Both 4K modes need the game to START at 3840x2160, which needs the 4K
       HeapDef overlay.  The 4K drawable can be requested now (the start
       environment is persisted), so do not require the current process to
       already be 4K: a 2K process applies this as TOTK_SET_RESTART.  Without
       the overlay the start fails in romfs.c, so keep the stored preference
       but refuse to apply it. */
    return output_overlay_4k();
}

int totk_capability_interp(void) {
    ensure_init();
    return !env_on("TOTK_MFX_DISABLE");
}

int totk_capability_aspect(void) {
    ensure_init();
    /* Older game versions have no aspect patch; the persisted value is kept for versions that do. The patch's own
       contract is supported || available: available only appears on the supported versions, but it is false before
       aspect_install runs. */
    return runtime_aspect_supported() || runtime_aspect_available() > 0;
}

int totk_setting_interp(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = g_v.interp;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_interp(int mode) {
    ensure_init();
    if (mode < 0 || mode > TOTK_INTERP_MAX) return TOTK_SET_ERR_INVALID;
    pthread_mutex_lock(&g_mu);
    if (g_locked_interp || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    g_file.interp = mode;
    g_v.interp = mode;
    int result = persist(0);
    Values values = g_v;
    pthread_mutex_unlock(&g_mu);
    apply_live(APPLY_INTERP, &values);
    return result;
}

/* A 4K scene render target is only valid with a native 4K output.  It is not
   a free supersampling choice: the first v2 build allowed it with a 1080p
   output, which crashed in the game's renderer during boot.  MetalFX 4K keeps
   a 1080p canvas, so it must not use a 4K scene target either. */
static int render_res_allowed(const Values *v, int render) {
    if (render != TOTK_RENDER_2160) return 1;
    return v->output_res == TOTK_RES_2160 && v->upscaler == TOTK_UPSCALE_NATIVE;
}

static int normalize_render_res(Values *v) {
    if (render_res_allowed(v, v->render_res)) return 0;
    fprintf(stderr, "[totk] settings: a 4K scene render requires native 4K output; using the game default\n");
    v->render_res = TOTK_RENDER_GAME;
    return 1;
}

/* Apply a picture-v2 proposal and commit it only when the adapter accepted it
   (TOTK_SET_OK) or stored it for the next start (TOTK_SET_RESTART).  Pending,
   unavailable and locked results leave g_file/g_v untouched. */
static int commit_picture_change(int which, const Values *proposed_in) {
    Values proposed = *proposed_in;
    int render_clamped = 0;
    if (which == APPLY_PICTURE && !render_res_allowed(&proposed, proposed.render_res)) {
        proposed.render_res = TOTK_RENDER_GAME;
        render_clamped = 1;
    }
    int applied = apply_live(which, &proposed);
    if (applied != TOTK_SET_OK && applied != TOTK_SET_RESTART) return applied;
    pthread_mutex_lock(&g_mu);
    int locked = which == APPLY_RENDER ? g_locked_render : g_locked_picture;
    if (locked || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    if (which == APPLY_RENDER) {
        g_file.render_res = proposed.render_res;
        g_v.render_res = proposed.render_res;
        g_restart_render = applied == TOTK_SET_RESTART;
    } else {
        g_file.output_res = proposed.output_res;
        g_v.output_res = proposed.output_res;
        g_file.upscaler = proposed.upscaler;
        g_v.upscaler = proposed.upscaler;
        g_restart_output = applied == TOTK_SET_RESTART;
        if (render_clamped || g_file.render_res != proposed.render_res) {
            /* Also export the next start's TOTK_RENDER_RES.  This is a safety
               clamp, so it overrides an explicit environment lock. */
            g_file.render_res = proposed.render_res;
            g_v.render_res = proposed.render_res;
            g_restart_render = 1;
            render_env_export(&proposed, 1);
        }
    }
    int saved = persist(0);
    pthread_mutex_unlock(&g_mu);
    return saved == TOTK_SET_OK ? applied : saved;
}

int totk_setting_picture(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = picture_value(&g_v);
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_picture(int mode) {
    ensure_init();
    if (mode < 0 || mode > TOTK_PICTURE_NATIVE) return TOTK_SET_ERR_INVALID;
    pthread_mutex_lock(&g_mu);
    if (g_locked_picture || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    Values proposed = g_v;
    picture_compat_apply(&proposed, mode);   /* keep the independent renderRes */
    pthread_mutex_unlock(&g_mu);
    return commit_picture_change(APPLY_PICTURE, &proposed);
}

int totk_setting_output_res(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = g_v.output_res;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_output_res(int res) {
    ensure_init();
    if (res != TOTK_RES_1080 && res != TOTK_RES_1440 && res != TOTK_RES_2160) return TOTK_SET_ERR_INVALID;
    pthread_mutex_lock(&g_mu);
    if (g_locked_picture || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    Values proposed = g_v;
    proposed.output_res = res;
    pthread_mutex_unlock(&g_mu);
    return commit_picture_change(APPLY_PICTURE, &proposed);
}

int totk_setting_upscaler(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = g_v.upscaler;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_upscaler(int upscaler) {
    ensure_init();
    if (upscaler < TOTK_UPSCALE_NATIVE || upscaler > TOTK_UPSCALE_METALFX) return TOTK_SET_ERR_INVALID;
    pthread_mutex_lock(&g_mu);
    if (g_locked_picture || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    Values proposed = g_v;
    proposed.upscaler = upscaler;
    pthread_mutex_unlock(&g_mu);
    return commit_picture_change(APPLY_PICTURE, &proposed);
}

int totk_setting_render_res(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = g_v.render_res;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_render_res(int render) {
    ensure_init();
    if (render < TOTK_RENDER_GAME || render > TOTK_RENDER_2160) return TOTK_SET_ERR_INVALID;
    pthread_mutex_lock(&g_mu);
    if (g_locked_render || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    if (g_v.render_res == render) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_OK;
    }
    Values proposed = g_v;
    proposed.render_res = render;
    int allowed = render_res_allowed(&proposed, render);
    pthread_mutex_unlock(&g_mu);
    if (!allowed) return TOTK_SET_ERR_UNAVAILABLE;
    return commit_picture_change(APPLY_RENDER, &proposed);
}

int totk_setting_picture_restart_required(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = g_restart_output || g_restart_render;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_aspect(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = g_v.aspect;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_aspect(int on) {
    ensure_init();
    on = on ? 1 : 0;
    if (!runtime_aspect_supported() && !runtime_aspect_available()) return TOTK_SET_ERR_UNAVAILABLE;
    pthread_mutex_lock(&g_mu);
    if (g_locked_aspect || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    Values proposed = g_v;
    proposed.aspect = on;
    pthread_mutex_unlock(&g_mu);
    int applied = apply_live(APPLY_ASPECT, &proposed);
    if (applied != TOTK_SET_OK) return applied;
    pthread_mutex_lock(&g_mu);
    if (g_locked_aspect || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    g_file.aspect = on;
    g_v.aspect = on;
    int result = persist(0);
    pthread_mutex_unlock(&g_mu);
    return result;
}

float totk_setting_aspect_ratio(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    float value = g_v.ratio;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_aspect_ratio(float ratio) {
    ensure_init();
    if (!(ratio >= 0.5f && ratio <= 8.0f)) return TOTK_SET_ERR_INVALID;
    if (!runtime_aspect_supported() && !runtime_aspect_available()) return TOTK_SET_ERR_UNAVAILABLE;
    pthread_mutex_lock(&g_mu);
    if (g_locked_aspect || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    Values proposed = g_v;
    proposed.ratio = ratio;
    pthread_mutex_unlock(&g_mu);
    int applied = apply_live(APPLY_RATIO, &proposed);
    if (applied != TOTK_SET_OK) return applied;
    pthread_mutex_lock(&g_mu);
    if (g_locked_aspect || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    g_file.ratio = ratio;
    g_v.ratio = ratio;
    int result = persist(0);
    pthread_mutex_unlock(&g_mu);
    return result;
}

int totk_setting_hud_corners(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = g_v.hud_corners;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_hud_corners(int on) {
    ensure_init();
    on = on ? 1 : 0;
    pthread_mutex_lock(&g_mu);
    if (g_locked_hud || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    g_file.hud_corners = on;
    g_v.hud_corners = on;
    int result = persist(0);
    Values values = g_v;
    pthread_mutex_unlock(&g_mu);
    apply_live(APPLY_HUD, &values);
    return result;
}

int totk_setting_hints(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = g_v.hints;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_hints(int on) {
    ensure_init();
    on = on ? 1 : 0;
    pthread_mutex_lock(&g_mu);
    if (g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    g_file.hints = on;
    g_v.hints = on;
    int result = persist(0);
    Values values = g_v;
    pthread_mutex_unlock(&g_mu);
    apply_live(APPLY_HINTS, &values);
    return result;
}

int totk_setting_fps_overlay(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = g_v.fps_overlay;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_fps_overlay(int on) {
    ensure_init();
    on = on ? 1 : 0;
    pthread_mutex_lock(&g_mu);
    if (g_locked_fps || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    g_file.fps_overlay = on;
    g_v.fps_overlay = on;
    int result = persist(0);
    Values values = g_v;
    pthread_mutex_unlock(&g_mu);
    apply_live(APPLY_FPS, &values);
    return result;
}

int totk_setting_shader_overlay(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = g_v.shader_overlay;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_shader_overlay(int on) {
    ensure_init();
    on = on ? 1 : 0;
    pthread_mutex_lock(&g_mu);
    if (g_locked_shader || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    g_file.shader_overlay = on;
    g_v.shader_overlay = on;
    int result = persist(0);
    Values values = g_v;
    pthread_mutex_unlock(&g_mu);
    apply_live(APPLY_SHADER, &values);
    return result;
}

int totk_setting_load_overlay(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = g_v.load_overlay;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_load_overlay(int on) {
    ensure_init();
    on = on ? 1 : 0;
    pthread_mutex_lock(&g_mu);
    if (g_locked_load || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    g_file.load_overlay = on;
    g_v.load_overlay = on;
    int result = persist(0);
    Values values = g_v;
    pthread_mutex_unlock(&g_mu);
    apply_live(APPLY_LOAD, &values);
    return result;
}

int totk_setting_face_layout(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int value = g_v.face_layout;
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_face_layout(int layout) {
    ensure_init();
    if (layout < 0 || layout > TOTK_FACE_LABEL) return TOTK_SET_ERR_INVALID;
    pthread_mutex_lock(&g_mu);
    if (g_locked_face || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    g_file.face_layout = layout;
    g_v.face_layout = layout;
    int result = persist(0);
    Values values = g_v;
    pthread_mutex_unlock(&g_mu);
    apply_live(APPLY_FACE, &values);
    return result;
}

static _Thread_local char g_language_out[16];

static const char *language_get(const char *value) {
    snprintf(g_language_out, sizeof g_language_out, "%s", value);
    return g_language_out;
}

const char *totk_setting_game_language(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    const char *value = language_get(g_v.game_language);
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_game_language(const char *code) {
    ensure_init();
    code = code ? code : "";
    if (!valid_language(code, 8)) return TOTK_SET_ERR_INVALID;
    pthread_mutex_lock(&g_mu);
    if (g_locked_lang || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    snprintf(g_file.game_language, sizeof g_file.game_language, "%s", code);
    snprintf(g_v.game_language, sizeof g_v.game_language, "%s", code);
    int result = persist(0);
    Values values = g_v;
    pthread_mutex_unlock(&g_mu);
    apply_live(APPLY_LANG, &values);
    return result;
}

const char *totk_setting_ui_language(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    const char *value = language_get(g_v.ui_language);
    pthread_mutex_unlock(&g_mu);
    return value;
}

int totk_setting_set_ui_language(const char *code) {
    ensure_init();
    code = code ? code : "";
    if (!valid_language(code, 16)) return TOTK_SET_ERR_INVALID;
    pthread_mutex_lock(&g_mu);
    if (g_locked_ui_lang || g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    snprintf(g_file.ui_language, sizeof g_file.ui_language, "%s", code);
    snprintf(g_v.ui_language, sizeof g_v.ui_language, "%s", code);
    int result = persist(0);
    Values values = g_v;
    pthread_mutex_unlock(&g_mu);
    apply_live(APPLY_UI_LANG, &values);
    return result;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Profiles                                                                                                      */

int totk_profile_count(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int count = g_profile_count;
    pthread_mutex_unlock(&g_mu);
    return count;
}

int totk_profile_active(void) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int active = g_profile_active;
    pthread_mutex_unlock(&g_mu);
    return active;
}

static _Thread_local char g_profile_id_out[40];
static _Thread_local char g_profile_name_out[TOTK_PROFILE_NAME_MAX];

const char *totk_profile_id(int index) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    const char *result = NULL;
    if (index >= 0 && index < g_profile_count) {
        snprintf(g_profile_id_out, sizeof g_profile_id_out, "%s", g_profile[index].id);
        result = g_profile_id_out;
    }
    pthread_mutex_unlock(&g_mu);
    return result;
}

const char *totk_profile_name(int index) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    const char *result = NULL;
    if (index >= 0 && index < g_profile_count) {
        snprintf(g_profile_name_out, sizeof g_profile_name_out, "%s", g_profile[index].name);
        result = g_profile_name_out;
    }
    pthread_mutex_unlock(&g_mu);
    return result;
}

int totk_profile_builtin(int index) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    int builtin = index >= 0 && index < g_profile_count ? g_profile[index].builtin : 0;
    pthread_mutex_unlock(&g_mu);
    return builtin;
}

int totk_profile_set_active(int index) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    if (g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    if (index < 0 || index >= g_profile_count) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_INVALID;
    }
    int result = TOTK_SET_OK;
    if (index != g_profile_active) {
        g_profile_active = index;
        publish_active();
        result = persist(1);
    }
    pthread_mutex_unlock(&g_mu);
    totk_profile_apply();
    return result;
}

int totk_profile_add(const char *name, int copy_from) {
    ensure_init();
    char clean[TOTK_PROFILE_NAME_MAX];
    if (!trimmed_name(name, clean)) return TOTK_SET_ERR_INVALID;
    pthread_mutex_lock(&g_mu);
    if (g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    if (g_profile_count >= TOTK_PROFILE_MAX || copy_from < -1 || copy_from >= g_profile_count) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_INVALID;
    }
    Profile profile;
    profile_defaults(&profile);
    if (copy_from >= 0) {
        const Profile *source = &g_profile[copy_from];
        memcpy(profile.action, source->action, sizeof profile.action);
        memcpy(profile.controller, source->controller, sizeof profile.controller);
        memcpy(profile.controller_modes, source->controller_modes, sizeof profile.controller_modes);
    }
    make_id(profile.id);
    snprintf(profile.name, sizeof profile.name, "%s", clean);
    profile.builtin = 0;
    int index = g_profile_count;
    g_profile[index] = profile;
    g_profile_raw[profile_key(profile.id)] = [NSMutableDictionary dictionary];
    g_profile_count++;
    g_profile_active = index;
    publish_active();
    int result = persist(1);
    pthread_mutex_unlock(&g_mu);
    totk_profile_apply();
    return result == TOTK_SET_OK ? index : result;
}

int totk_profile_rename(int index, const char *name) {
    ensure_init();
    char clean[TOTK_PROFILE_NAME_MAX];
    if (!trimmed_name(name, clean)) return TOTK_SET_ERR_INVALID;
    pthread_mutex_lock(&g_mu);
    if (g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    if (index < 0 || index >= g_profile_count) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_INVALID;
    }
    snprintf(g_profile[index].name, sizeof g_profile[index].name, "%s", clean);
    int result = persist(1);
    pthread_mutex_unlock(&g_mu);
    return result;
}

int totk_profile_delete(int index) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    if (g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    if (index < 0 || index >= g_profile_count || g_profile_count <= 1) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_INVALID;
    }
    if (g_profile_raw) [g_profile_raw removeObjectForKey:profile_key(g_profile[index].id)];
    for (int i = index; i < g_profile_count - 1; i++) g_profile[i] = g_profile[i + 1];
    g_profile_count--;
    memset(&g_profile[g_profile_count], 0, sizeof g_profile[g_profile_count]);
    if (g_profile_active > index) g_profile_active--;
    else if (g_profile_active == index) g_profile_active = index < g_profile_count ? index : g_profile_count - 1;
    publish_active();
    int result = persist(1);
    pthread_mutex_unlock(&g_mu);
    totk_profile_apply();
    return result;
}

int totk_profile_reset(int index) {
    ensure_init();
    pthread_mutex_lock(&g_mu);
    if (g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    if (index < 0 || index >= g_profile_count) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_INVALID;
    }
    Profile *profile = &g_profile[index];
    char id[sizeof profile->id];
    char name[sizeof profile->name];
    int builtin = profile->builtin;
    snprintf(id, sizeof id, "%s", profile->id);
    snprintf(name, sizeof name, "%s", profile->name);
    profile_defaults(profile);
    snprintf(profile->id, sizeof profile->id, "%s", id);
    snprintf(profile->name, sizeof profile->name, "%s", name);
    profile->builtin = builtin;
    if (index == g_profile_active) publish_active();
    int result = persist(1);
    int active = index == g_profile_active;
    pthread_mutex_unlock(&g_mu);
    if (active) totk_input_reload();
    return result;
}

int totk_profile_action(int index, int keycode) {
    ensure_init();
    if (keycode < 0 || keycode >= TOTK_KEYCODE_COUNT || reserved_keycode((unsigned)keycode))
        return TOTK_ACTION_NONE;
    pthread_mutex_lock(&g_mu);
    int action = index >= 0 && index < g_profile_count ? g_profile[index].action[keycode] : TOTK_ACTION_NONE;
    pthread_mutex_unlock(&g_mu);
    return action;
}

int totk_profile_set_action(int index, int keycode, int action) {
    ensure_init();
    if (keycode < 0 || keycode >= TOTK_KEYCODE_COUNT || reserved_keycode((unsigned)keycode) ||
        action < 0 || action >= TOTK_ACTION_COUNT)
        return TOTK_SET_ERR_INVALID;
    pthread_mutex_lock(&g_mu);
    if (g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    if (index < 0 || index >= g_profile_count) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_INVALID;
    }
    g_profile[index].action[keycode] = (uint8_t)action;
    if (index == g_profile_active) publish_active();
    int result = persist(1);
    int active = index == g_profile_active;
    pthread_mutex_unlock(&g_mu);
    if (active) totk_input_reload();
    return result;
}

/* Decode one physical button's map word: exactly one Switch button bit, or -1 for "not mapped". */
static int map_target(uint16_t value) {
    if (!value) return -1;
    for (int bit = 0; bit < CONTROLLER_SWITCH_BUTTONS; bit++)
        if (value & (1u << bit)) return bit;
    return -1;
}

int totk_profile_controller_target(int index, int physical) {
    ensure_init();
    if (physical < 0 || physical >= CONTROLLER_BUTTONS) return -1;
    pthread_mutex_lock(&g_mu);
    int target = index >= 0 && index < g_profile_count
                 ? map_target(g_profile[index].controller[physical]) : -1;
    pthread_mutex_unlock(&g_mu);
    return target;
}

int totk_profile_controller_mode(int index, int physical) {
    ensure_init();
    if (physical < 0 || physical >= CONTROLLER_BUTTONS) return CONTROLLER_HOLD;
    pthread_mutex_lock(&g_mu);
    int mode = index >= 0 && index < g_profile_count
               ? controller_mode_kind(g_profile[index].controller_modes[physical]) : CONTROLLER_HOLD;
    pthread_mutex_unlock(&g_mu);
    return mode;
}

int totk_profile_controller_fire(int index, int physical) {
    ensure_init();
    if (physical < 0 || physical >= CONTROLLER_BUTTONS) return 0;
    pthread_mutex_lock(&g_mu);
    int fire = index >= 0 && index < g_profile_count
               ? controller_mode_fire(g_profile[index].controller_modes[physical]) : 0;
    pthread_mutex_unlock(&g_mu);
    return fire;
}

int totk_profile_set_controller(int index, int physical, int target, int mode, int fire) {
    ensure_init();
    if (physical < 0 || physical >= CONTROLLER_BUTTONS || mode < 0 || mode > CONTROLLER_AIM ||
        target < -1 || target >= CONTROLLER_SWITCH_BUTTONS)
        return TOTK_SET_ERR_INVALID;
    if (target < 0) {
        mode = CONTROLLER_HOLD;
        fire = 0;
    } else if (mode == CONTROLLER_AIM) {
        if (fire < 0 || fire >= CONTROLLER_BUTTONS || fire == physical) return TOTK_SET_ERR_INVALID;
    } else {
        fire = 0;
    }
    pthread_mutex_lock(&g_mu);
    if (g_readonly) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_LOCKED;
    }
    if (index < 0 || index >= g_profile_count) {
        pthread_mutex_unlock(&g_mu);
        return TOTK_SET_ERR_INVALID;
    }
    Profile *profile = &g_profile[index];
    profile->controller[physical] = target < 0 ? 0 : (uint16_t)(1u << target);
    profile->controller_modes[physical] = (uint8_t)(mode | (fire << 3));
    int result = persist(1);
    int active = index == g_profile_active;
    pthread_mutex_unlock(&g_mu);
    if (active) totk_gamepad_reload();
    return result;
}

void totk_profile_apply(void) {
    ensure_init();
    totk_input_reload();
    totk_gamepad_reload();
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Keymap lookups (hot path: atomic snapshot, no g_mu)                                                           */

int totk_key_action(unsigned keycode) {
    ensure_init();
    if (keycode >= TOTK_KEYCODE_COUNT || reserved_keycode(keycode)) return TOTK_ACTION_NONE;
    return atomic_load_explicit(&g_hot_action[keycode], memory_order_relaxed);
}

uint64_t totk_key_button(unsigned keycode) {
    ensure_init();
    if (keycode >= TOTK_KEYCODE_COUNT || reserved_keycode(keycode)) return 0;
    int action = atomic_load_explicit(&g_hot_action[keycode], memory_order_relaxed);
    if (action < 0 || action >= TOTK_ACTION_COUNT) return 0;
    int bit = g_action_bit[action];
    return bit < 0 ? 0 : (uint64_t)1 << bit;
}

int totk_key_axis(unsigned keycode, int *axis, int *sign) {
    ensure_init();
    if (keycode >= TOTK_KEYCODE_COUNT || reserved_keycode(keycode)) return 0;
    int action = atomic_load_explicit(&g_hot_action[keycode], memory_order_relaxed);
    if (action < TOTK_ACTION_LSTICK_LEFT || action > TOTK_ACTION_RSTICK_DOWN) return 0;
    if (axis) *axis = g_action_axis[action].axis;
    if (sign) *sign = g_action_axis[action].sign;
    return 1;
}

const char *totk_action_name(int action) {
    if (action < 0 || action >= TOTK_ACTION_COUNT) return "Unknown";
    return g_action_names[action];
}

const char *totk_keycode_name(unsigned keycode) {
    if (keycode >= TOTK_KEYCODE_COUNT) return "Unknown";
    return keycode_name(keycode);
}
