/* Native macOS settings, profiles and persistence.
 *
 * The settings core owns the user-visible values.  It is deliberately pure C
 * (Foundation is used only inside settings.m) so the launcher, the runtime and
 * the test probes can share it.  Modules apply the values through this API;
 * the AppKit layer in preferences.m must not poke module internals.
 *
 * Persistence lives under ~/Library/Application Support/MacNativeGames/TOTK.
 * Explicit TOTK_* environment variables always win over the file, and agent
 * sessions (TOTK_SLOT_OWNER != user) write to an isolated directory. */
#ifndef TOTK_SETTINGS_H
#define TOTK_SETTINGS_H

#include <stddef.h>
#include <stdint.h>

enum {
    TOTK_INTERP_OFF = 0,
    TOTK_INTERP_30 = 1,
    TOTK_INTERP_HALF = 2,
    TOTK_INTERP_MAX = 3
};

/* Legacy picture preset (schema 1).  Kept for preferences.m and old callers;
   it is a view of output_res/upscaler: 0 = 1080 native, 1 = 1440 MetalFX,
   2 = 1440 native.  Persisted v2 files use outputRes/upscaler/renderRes. */
enum {
    TOTK_PICTURE_OFF = 0,
    TOTK_PICTURE_MFX = 1,
    TOTK_PICTURE_NATIVE = 2
};

/* Output target: the size of the canvas the game presents (1080, 1440 or
   2160).  The numbers are the resolution in lines and match the runtime's
   TOTK_RES_* contract; they are persisted, so do not renumber them.  2160 is
   experimental and needs a restart unless the process already started at
   3840x2160.  Schema-2 files written before this encoding used 0/1/2 and are
   still accepted (and rewritten) by the loader. */
enum {
    TOTK_RES_1080 = 1080,
    TOTK_RES_1440 = 1440,
    TOTK_RES_2160 = 2160
};

/* How the game's scene is brought to the output target. */
enum {
    TOTK_UPSCALE_NATIVE = 0,
    TOTK_UPSCALE_METALFX = 1
};

/* Scene render resolution.  GAME means "whatever the game chose"; the fixed
   choices override TOTK_RENDER_RES and take effect on the next start. */
enum {
    TOTK_RENDER_GAME = 0,
    TOTK_RENDER_720 = 1,
    TOTK_RENDER_1080 = 2,
    TOTK_RENDER_1440 = 3,
    TOTK_RENDER_2160 = 4
};

enum {
    TOTK_FACE_AUTO = 0,
    TOTK_FACE_POSITION = 1,
    TOTK_FACE_LABEL = 2
};

/* One logical target for a physical key.  Button actions 1..16 map to the
   Switch Pro Controller button bits; axis actions 17..24 map to a stick
   direction.  The numbers are persisted, so do not renumber them. */
enum {
    TOTK_ACTION_NONE = 0,
    TOTK_ACTION_A = 1,
    TOTK_ACTION_B = 2,
    TOTK_ACTION_X = 3,
    TOTK_ACTION_Y = 4,
    TOTK_ACTION_L = 5,
    TOTK_ACTION_R = 6,
    TOTK_ACTION_ZL = 7,
    TOTK_ACTION_ZR = 8,
    TOTK_ACTION_PLUS = 9,
    TOTK_ACTION_MINUS = 10,
    TOTK_ACTION_LSTICK = 11,
    TOTK_ACTION_RSTICK = 12,
    TOTK_ACTION_DLEFT = 13,
    TOTK_ACTION_DUP = 14,
    TOTK_ACTION_DRIGHT = 15,
    TOTK_ACTION_DDOWN = 16,
    TOTK_ACTION_LSTICK_LEFT = 17,
    TOTK_ACTION_LSTICK_RIGHT = 18,
    TOTK_ACTION_LSTICK_UP = 19,
    TOTK_ACTION_LSTICK_DOWN = 20,
    TOTK_ACTION_RSTICK_LEFT = 21,
    TOTK_ACTION_RSTICK_RIGHT = 22,
    TOTK_ACTION_RSTICK_UP = 23,
    TOTK_ACTION_RSTICK_DOWN = 24,
    TOTK_ACTION_COUNT = 25
};

enum {
    TOTK_SET_OK = 0,
    /* The target was stored, but the runtime could not apply it live: it
       takes effect on the next start (see
       totk_setting_picture_restart_required).  Positive so callers that only
       test rc >= 0 still treat it as saved. */
    TOTK_SET_RESTART = 1,
    TOTK_SET_ERR_UNAVAILABLE = -1,
    TOTK_SET_ERR_INVALID = -2,
    TOTK_SET_ERR_SAVE = -3,
    TOTK_SET_ERR_LOCKED = -4,
    /* The runtime adapter has a switch in flight; nothing was stored or
       changed.  Retry when the frame settles. */
    TOTK_SET_ERR_PENDING = -5
};

#define TOTK_PROFILE_MAX 8
#define TOTK_PROFILE_NAME_MAX 64
#define TOTK_KEYCODE_COUNT 128

typedef struct {
    char id[40];
    char name[TOTK_PROFILE_NAME_MAX];
    int builtin;
} TOTKProfileInfo;

/* Loads settings.json/keymaps.json, resolves environment precedence and
   applies the active profile.  Returns 0 on a clean load, 1 when defaults or
   a backup were used (see totk_settings_error), and -1 only when even the
   fallback directory cannot be used.  Safe to call more than once. */
int totk_settings_init(void);
int totk_settings_loaded(void);
const char *totk_settings_error(void);
const char *totk_settings_path(void);

/* Persist pending changes now.  Returns TOTK_SET_OK or TOTK_SET_ERR_SAVE. */
int totk_settings_save_now(void);
void totk_settings_flush(void);

/* ---- live settings ---------------------------------------------------- */
int totk_setting_interp(void);
int totk_setting_set_interp(int mode);

int totk_setting_picture(void);
int totk_setting_set_picture(int mode);

/* Picture v2.  output_res is TOTK_RES_*, upscaler TOTK_UPSCALE_*, render_res
   TOTK_RENDER_*.  The setters apply the target through
   runtime_output_mode_set_ex(res, upscaler) and return TOTK_SET_OK,
   TOTK_SET_RESTART, or a TOTK_SET_ERR_* code.  On any error the persisted
   values are unchanged; on TOTK_SET_RESTART the new target is stored and
   takes effect on the next start. */
int totk_setting_output_res(void);
int totk_setting_set_output_res(int res);
int totk_setting_upscaler(void);
int totk_setting_set_upscaler(int upscaler);
int totk_setting_render_res(void);
int totk_setting_set_render_res(int render);

/* 1 while the stored target (output mode or scene render size) differs from
   the mode this process started with and a restart is needed to apply it. */
int totk_setting_picture_restart_required(void);

/* 1 when (res, upscaler) can be presented on this installation.  1080 and
   1440 are always available.  Both 2160 modes need the 3840x2160 RomFS output
   overlay (TOTK_ROMFS_PATCHES_OUT, default extracted/<game>/romfs-patches-out)
   so the next start can use a 4K swapchain; MetalFX 4K reaches it from the
   usual 1080p canvas.  Applying 4K from a 2K process stores the target and
   reports TOTK_SET_RESTART.  The UI must disable a mode when this returns 0;
   setters refuse it with TOTK_SET_ERR_UNAVAILABLE. */
int totk_setting_output_capable(int res, int upscaler);

int totk_setting_aspect(void);
int totk_setting_set_aspect(int on);
float totk_setting_aspect_ratio(void);
int totk_setting_set_aspect_ratio(float ratio);

int totk_setting_hud_corners(void);
int totk_setting_set_hud_corners(int on);

int totk_setting_hints(void);
int totk_setting_set_hints(int on);

int totk_setting_fps_overlay(void);
int totk_setting_set_fps_overlay(int on);

int totk_setting_shader_overlay(void);
int totk_setting_set_shader_overlay(int on);

int totk_setting_load_overlay(void);
int totk_setting_set_load_overlay(int on);

int totk_setting_face_layout(void);
int totk_setting_set_face_layout(int layout);

/* Empty string means "follow the system / use the game default". */
const char *totk_setting_game_language(void);
int totk_setting_set_game_language(const char *code);
const char *totk_setting_ui_language(void);
int totk_setting_set_ui_language(const char *code);

/* True when the named TOTK_* variable was set by the user before we injected
   defaults.  Accepted names: "interp", "picture", "render", "aspect", "hud",
   "fps", "shader", "load", "face", "lang", "ui_lang". */
int totk_setting_locked(const char *name);

/* Capability gates.  The runtime registers its real capabilities; the UI must
   disable an option when the matching call returns 0. */
int totk_capability_output(void);
int totk_capability_interp(void);
int totk_capability_aspect(void);

/* ---- keymap profiles -------------------------------------------------- */
int totk_profile_count(void);
int totk_profile_active(void);
const char *totk_profile_id(int index);
const char *totk_profile_name(int index);
int totk_profile_builtin(int index);

int totk_profile_set_active(int index);
/* copy_from < 0 starts from the default map; otherwise copies that profile. */
int totk_profile_add(const char *name, int copy_from);
int totk_profile_rename(int index, const char *name);
int totk_profile_delete(int index);
int totk_profile_reset(int index);

/* action for keycode in the given profile, or TOTK_ACTION_NONE. */
int totk_profile_action(int index, int keycode);
int totk_profile_set_action(int index, int keycode, int action);

/* Controller map: target is a 0-based Switch button index, or -1 for none.
   mode is CONTROLLER_HOLD/TAP/TOGGLE/AIM; fire is the physical index for AIM. */
int totk_profile_controller_target(int index, int physical);
int totk_profile_controller_mode(int index, int physical);
int totk_profile_controller_fire(int index, int physical);
int totk_profile_set_controller(int index, int physical, int target, int mode, int fire);

/* Re-reads the active profile into input.c/gamepad.m and clears held state. */
void totk_profile_apply(void);

/* Runtime lookups.  They read the active profile without touching the JSON. */
int totk_key_action(unsigned keycode);
uint64_t totk_key_button(unsigned keycode);
int totk_key_axis(unsigned keycode, int *axis, int *sign);

/* Stable English names for the UI; returns a pointer to a static string. */
const char *totk_action_name(int action);
const char *totk_keycode_name(unsigned keycode);

#endif
