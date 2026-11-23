#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
typedef struct { uint64_t where; unsigned kind, symbol; int64_t addend; } Reloc;
typedef struct { const char *name; uint64_t value, size; unsigned defined, binding, type; } Symbol;
typedef struct { uint64_t ret, original; unsigned timer, arg; } Gate;
typedef struct {
 const char *name, *digest; size_t file_size, size;
 uint64_t text_end, ro_off, data_off, patch_off, key_off, init, ctor_flag, init_array, init_size;
 const Symbol *symbols; unsigned nsym; const Reloc *relocs; unsigned nreloc;
 const Gate *gates; unsigned ngate; unsigned *hash, hash_mask;
 uint8_t *base; unsigned relative, symbolic, cross, weak;
} Module;
extern pthread_key_t runtime_keys[8];
_Noreturn void runtime_fail(const char *message);
void *runtime_symbol(const char *name);
void *runtime_override(const char *name);
void runtime_hle_stack_init(uint64_t tsd_offset);
void runtime_gpu_path_probe(const char *which);
void runtime_audio_probe(void);
void *runtime_hle_wrap(void *fn);
void runtime_hle_stack_set(uintptr_t top);
void *runtime_font_override(const char *name);
void *runtime_codec_override(const char *name);
void *runtime_movie_override(const char *name);   /* runtime/movie_host.c: movie::Decoder for VP9 cutscenes */
void *runtime_time_override(const char *name);
void *runtime_applet_override(const char *name);
void *runtime_swkbd_override(const char *name);   /* runtime/swkbd_host.c: nn::swkbd text entry with the Mac keyboard */
void runtime_swkbd_probe(void);
void runtime_swkbd_selftest(void);
void runtime_dump_threads(void);   /* threads.c: every guest thread's wait state and guest return chain (TOTK_INPUT_FILE "threads") */   /* diagnostic "kbtest": show the text entry bar from a thread of its own */
void runtime_frame_step_pin(double ms);                  /* game_patches.c: pin the real-time frame step (diagnostics), 0 = real time */
void runtime_frame_step_update(uint64_t now_ns);
void runtime_thread_note_hle(unsigned index);            /* threads.c: the calling guest thread's last host call (hletrace) */
uint32_t runtime_thread_current_handle(void);            /* threads.c: handle of the calling guest thread, 0 on a host thread */
void *runtime_hle_wrap_named(void *fn, const char *name);/* hle_stack.c: runtime_hle_wrap with a name for diagnostics */
void runtime_hle_trace_set(int mode);                    /* hle_stack.c: 1 count host calls, 2 also log the first call of each, 0 stop and print */
void runtime_hle_describe(unsigned index, char *out, size_t cap);   /* hle_stack.c: readable name of a host-call wrapper */
/* Text entry bar in the game window (runtime/frontend.m, Mac): the player types on the keyboard, Return confirms (1, the text
   as UTF-8 in out), Escape cancels (0). The caller waits; give the virtual core up around it (runtime_core_away). */
typedef struct { const char *prompt, *initial; int max_units, min_units, secure, digits_only; uint32_t disabled; } RuntimeTextRequest;
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size);
void runtime_front_text_test(const char *op, const char *text);   /* diagnostics: "type" text, "enter", "esc" into the open bar */
int runtime_core_away(void);          /* runtime/threads.c: a guest thread gives its virtual core up around a long host wait */
void runtime_core_back(int *away);
void runtime_save_probe(void);
int runtime_fs_save_busy(void);   /* runtime/fs_host.c: the game is writing save data (window close waits) */
uint8_t *runtime_module_base(const char *name);   /* runtime/loader.c: "main", "subsdk0", "sdk" (probes) */
uint64_t runtime_module_permission(uint64_t address,uint64_t size,unsigned permission);
uint64_t runtime_module_lock_permission(uint64_t address,uint64_t size,unsigned mask,unsigned attribute);
int runtime_module_query(uint64_t address,uint64_t *base,uint64_t *size,unsigned *state,unsigned *permission,unsigned *attribute);
void runtime_oe_push_message(uint32_t message);
void hle_bind_system_event(void *event, int autoclear);
uint64_t runtime_svc(unsigned num, uint64_t *regs, const char *module, uint64_t offset);
void runtime_gate(void);
void runtime_thread_init(void);
void runtime_dispatch(uint64_t ret, uint64_t *regs);

uint64_t runtime_image_bytes(void);

void runtime_fs_init(void);
void *runtime_fs_override(const char *name);
void runtime_fs_probe(void);

void *runtime_observer(const char *name);

void runtime_startup_checkpoint(uintptr_t caller);

uint64_t runtime_now_ns(void);
uint32_t svc_thread_main_init(void);
int svc_thread(int num,uint64_t *regs);

void *runtime_gfx_override(const char *name);
int runtime_gfx_fault(void *addr,int is_write,void *context);
/* A thread that reads guest memory it does not own (uniform snapshot copies) sets this around the read: a SIGBUS / SIGSEGV
   on that thread then returns 1 from sigsetjmp instead of taking the process down (loader.c signal handler). */
#include <setjmp.h>
extern __thread sigjmp_buf *runtime_fault_guard;
void dm_guest_write_prepare(void *addr,size_t n);

void runtime_trace_guest(uint64_t fp,uint64_t lr);

void runtime_front_run(void(*callback)(void));
void runtime_boot_progress(const char *stage,uint64_t done,uint64_t total);
void runtime_boot_status(const char *stage,const char *detail);
void runtime_boot_count(const char *stage,uint64_t done,uint64_t total);   /* a boot step counted in items (shader import) */
/* True 2K (runtime/game_patches.c, Mac): TOTK_OUTPUT_RES=<w>x<h> sets the game's own output size. runtime_output_res gives it
   (0 when unset); runtime_output_res_extra_bytes is what the HeapDef overlay (scripts/make_output_res_overlay.py, same formula)
   adds to the texture heap and to Graphics, and so to the application pool (runtime/services.c). */
int runtime_output_res(unsigned *w,unsigned *h);
uint64_t runtime_output_res_extra_bytes(void);
int runtime_output_res_toggle(void);                    /* legacy two-state output size switch */
void runtime_warm_progress(uint64_t done,uint64_t total,uint64_t shaders,uint64_t shader_total,uint64_t pipelines,uint64_t pipeline_total);
void runtime_warm_done(void);
void runtime_game_slot_acquire(const char *mode);   /* runtime/game_slot.c: one game at a time */
void *runtime_front_override(const char *name);
int runtime_front_focus(void);
void runtime_shortcut_help_toggle(void);
int fps_overlay_toggle_hints(void);   /* shared key 1 toggle for FPS, shader and system-load overlays */
int runtime_aspect_toggle(void);   /* runtime/aspect_patch.h: key 4, 16:9 <-> ultrawide (Mac, 1.2.1 / 1.4.3); 0 where unavailable */
int runtime_aspect_set_ratio(float r);   /* runtime/aspect_patch.h: the settings panel, 0 = 16:9; 0 where unavailable */
int runtime_frame_target_set(float ms);   /* runtime/game_patches.c: the game's target frame time (1.4.0), 0 = no sleep; 0 where unavailable */
float runtime_aspect_now(void);     /* the ratio in force, 0 = the game's own 16:9 */
int fps_overlay_hud_mode(void);     /* gfx/nvn/fps_overlay.c (shared): the settings panel's overlay mode, HUD_MODE_* */
int fps_overlay_full_hud(void);
int hud_compact(void);   /* gfx/nvn/fps_overlay.c: the overlay lines go to the window's corner panel (TOTK_HUD=corners: in the picture) */
int hud_snapshot(int line, char *out, size_t n, int *highlight);   /* a HUD_* line (fps_overlay.h); 0 = not published */

void *runtime_account_override(const char *name);

void runtime_gfx_probe(int negative);
void runtime_vpool_probe(void);

void runtime_memory_probe(void);

void runtime_input_key(unsigned code,int down);
void runtime_input_key_modifiers(unsigned code,int down,unsigned modifiers);
void runtime_input_clear(void);
uint64_t runtime_input_buttons(void);   /* runtime/input.c: held buttons (keyboard and controller), npad bit order */
void runtime_guest_regions(uint64_t base[3],uint64_t size[3]);   /* runtime/services.c: heap, alias and ASLR regions */
void runtime_cheats_launch(Module *main_module);   /* runtime/cheats.c: before the modules are sealed */
int runtime_cheats_count(void);
const char *runtime_cheats_name(int i);
int runtime_cheats_state(int i);   /* CHEAT_ON | CHEAT_AT_LAUNCH | CHEAT_APPLIED | CHEAT_REFUSED */
const char *runtime_cheats_problem(int i);
int runtime_cheats_set(int i,int on);   /* returns the new state; saved, and the profile backed up before the first cheat */
enum { CHEAT_ON = 1, CHEAT_AT_LAUNCH = 2, CHEAT_APPLIED = 4, CHEAT_REFUSED = 8 };
void runtime_input_key_synthetic(unsigned code,int down);   /* scripted input: the fixed test layout (runtime/input.c) */
void runtime_input_mouse(int button,int down);   /* a mouse button over the game window: 0 left, 1 right, 2 middle, 3-4 side */
struct KeyMap;
void runtime_keymap_get(struct KeyMap *out);   /* the keyboard and mouse layout in force (keyboard_map.h) */
void runtime_keymap_set(const struct KeyMap *map);   /* in force at once, saved in keyboard.txt */
void runtime_keymap_reset(void);   /* the default, keyboard.txt removed */
void runtime_settings_toggle(void);   /* key 1 / the controller shortcut: open or close the settings panel (frontend.m) */
/* The settings panel's files, one word each next to controls.txt (TOTK_CONTROLS): runtime/gamepad.m. A run without
   TOTK_CONTROLS (a test) neither reads nor writes them. */
int runtime_setting_word(const char *name, char *word, size_t size);
void runtime_setting_save(const char *name, const char *word, const char *what);
void runtime_setting_remove(const char *name);
int runtime_handheld_mode(void);   /* TOTK_OPERATION_MODE=handheld (runtime/services.c) */
void *runtime_input_override(const char *name);
typedef struct { uint64_t buttons; int32_t lx,ly,rx,ry; int connected; } RuntimeGamepadState;
void runtime_gamepad_start(void);
void runtime_gamepad_read(RuntimeGamepadState *out);
void runtime_gamepad_set_overlay(int (*hook)(uint64_t held, uint64_t pressed));   /* main queue; 1: keep the controllers from the game */
uint64_t runtime_gamepad_held(void);   /* held buttons, leaving the press edges for runtime_gamepad_read */
/* Forget held buttons and sticks: macOS stops delivering controller events (including releases) while the app is in
   the background, so a button held at focus loss would otherwise stay pressed for the game. */
void runtime_gamepad_clear(void);
void runtime_touch_pad_set(const RuntimeGamepadState *state);   /* on-screen touch controls (iOS) */
/* The settings panel (runtime/settings_panel.h) and runtime/gamepad.m (from games/acnh): the controller layout
   (controller_map.h), the controller type (gamepad_layout.h GAMEPAD_TYPE_*), the controller's way to the panel (L3 + R3,
   or one button), and the controller read by the panel while it owns the input. */
void runtime_controller_map_get(uint16_t *map, uint8_t *modes);
void runtime_controller_map_set(const uint16_t *map, const uint8_t *modes);
void runtime_controller_map_reset(void);
int runtime_gamepad_type(int *detected);
int runtime_gamepad_type_set(int type);
int runtime_gamepad_battery_percent(void);
int runtime_gamepad_battery_left_percent(void);   /* a Joy-Con pair's left one, -1 none or unknown (the corner box) */
int runtime_gamepad_settings_button(void);
void runtime_gamepad_settings_button_set(int physical);
void runtime_gamepad_settings_enable(int enabled);
int runtime_gamepad_settings_requested(void);
void runtime_gamepad_text_capture(int on);
void runtime_gamepad_text_read(RuntimeGamepadState *out);
int runtime_gamepad_screenshot_button(void);               /* the button held 3 s for a screenshot (controller_map.h), -1 none */
void runtime_gamepad_screenshot_button_set(int physical); /* the key bindings; saved in controller-screenshot.txt (none: "none"), the touchpad the default */
/* The controller's capture button (runtime/gamepad.m): the Share button of a controller GameController reports, or bit 5
   of a Switch controller's 0x30 HID report. The hook is called once per press, on the main queue; the window (frontend.m)
   sets it to save a picture of the game's next frame (runtime/screenshot.h). */
void runtime_capture_set_hook(void (*hook)(void));
void runtime_capture_press(void) __attribute__((weak));   /* gamepad.m (weak: probes that link input.c alone): the capture button pressed once (the input file's "capture") */
void runtime_gamepad_switch_capture(void *controller, int pressed);   /* Switch 0x30 report: 1 while the button is down (any queue) */
/* DSU (cemuhook) motion and input over UDP, as games/totk (runtime/dsu_client.h, included by gamepad.m; the input page's
   DSU rows, runtime/dsu_settings_rows.h). Off by default. */
int runtime_dsu_motion_active(void);     /* fresh DSU six-axis samples currently own motion, not just an open socket */
typedef struct {
    int enabled, slot, locked;
    char endpoint[264];
} RuntimeDSUSettings;
typedef struct {
    int connected, sensor;
    double acc[3], gyro[3];   /* gyro in degrees/second for the settings display */
    uint32_t buttons;
    uint8_t axes[4];
    char error[160];
} RuntimeDSUStatus;
void runtime_dsu_settings(RuntimeDSUSettings *out);   /* main queue; environment overrides saved preferences */
void runtime_dsu_status(RuntimeDSUStatus *out);       /* main queue; passive snapshot */
int runtime_dsu_apply(const RuntimeDSUSettings *settings); /* 0 applied and saved, -1 invalid, -2 environment locked */
void runtime_feedback_motion_source_changed(void);   /* gamepad_feedback.m: switch between DSU and native motion without changing rumble */
/* Controller rumble and motion (opt-in TOTK_RUMBLE=1 / TOTK_GYRO=1): the game's hid side in runtime/hid_feedback.c, the
   Mac controller's haptics and motion sensors in runtime/gamepad_feedback.m. */
#define RUNTIME_MOTION_MAX 32
typedef struct { uint64_t sampling, delta_ns; float acc[3], gyro[3], angle[3], dir[9]; } RuntimeMotionState;   /* G, rev/s, rev, 3 axes */
int runtime_rumble_enabled(void);
int runtime_gyro_enabled(void);
/* The settings panel's rumble switch: in force at once when the game has the vibration devices (it got them at start), else
   from the next launch; saved in rumble.txt. TOTK_RUMBLE or an agent's test session decide first (locked). */
int runtime_rumble_locked(void);
int runtime_rumble_choice(void);
void runtime_rumble_choose(int on);
/* The shared settings panel's rumble and gyro rows (shared/runtime/settings_panel.h settings_feedback_row): gyro 0 is the
   rumble switch above; gyro 1 is motion, which TOTK_GYRO alone decides here (locked). runtime/hid_feedback.c. */
int runtime_feedback_locked(int gyro);
int runtime_feedback_present(int gyro);
int runtime_feedback_choice(int gyro);
void runtime_feedback_choose(int gyro, int on);
/* Named by the shared panel's rumble strength and mouse sensitivity rows, which this game does not have (its rumble has no
   strength setting and its mouse only clicks): declared for the panel's helpers, defined nowhere, so a row that used them
   would fail to link instead of doing nothing. */
int runtime_rumble_gain_locked(void);
float runtime_rumble_gain(void);
void runtime_rumble_gain_set(float gain);
int runtime_mouse_sensitivity(void);
int runtime_mouse_sensitivity_locked(void);
void runtime_mouse_sensitivity_set(int value);
void *runtime_hid_feedback_override(const char *name);
int runtime_hid_feedback_flag(const char *name, int v);   /* "flag rumble|gyro 0|1"; 0 = not one of these names */
void runtime_hid_feedback_stats(uint64_t *values, uint64_t *motion_reads);
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high);   /* side 0 left, 1 right */
int runtime_motion_read(RuntimeMotionState *out, int max);   /* newest first; 0 = no motion now */
void runtime_motion_start(void);   /* the game (re)started its sensor: angle and direction start over */
void runtime_motion_feed(const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);   /* runtime/motion_fusion.c */
int runtime_motion_axes(const char *spec);   /* TOTK_GYRO_AXES ("-x,y,-z"): 1 set, 0 unset, -1 malformed */
void runtime_motion_deadzone_default(double deg_s);   /* when TOTK_GYRO_DEADZONE is unset: 1.2 for HID motion, else 0 */
void runtime_motion_live(int on);   /* a controller with motion attached (1) / detached (0) */
void runtime_feedback_quiet(void);   /* focus lost: stop rumbling */
#ifdef __OBJC__
@class GCController;
void runtime_feedback_attach(GCController *controller);
void runtime_feedback_detach(GCController *controller);
#endif

/* Unified macOS controls; preference loading precedes guest-code patching. */
void runtime_controls_prepare(void);
int runtime_controls_handle_key(unsigned code,unsigned modifiers);
void runtime_front_control_test(const char *command);
int runtime_render_configure(unsigned w,unsigned h,int quality);
uint64_t runtime_render_applied(void);
