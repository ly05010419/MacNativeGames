#pragma once
#include "keyboard_map.h"   /* struct KeyMap: the settings panel and runtime_input_* APIs */
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
typedef struct { uint64_t where; unsigned kind, symbol; int64_t addend; } Reloc;
typedef struct { const char *name; uint64_t value, size; unsigned defined, binding, type; } Symbol;
typedef struct { uint64_t ret, original; unsigned timer, arg; } Gate;
typedef struct {
 const char *name, *digest, *patch_digest; size_t file_size, size, patch_size;
 /* patch_off is signed: images whose span exceeds a B's reach keep their patch area below the base
    (third_party/d2r/patch_nso.py); patch_off < 0 means base+patch_off is the arena's first byte. */
 uint64_t text_end, ro_off, data_off, key_off, init, ctor_flag, init_array, init_size;
 int64_t patch_off;
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
uint64_t runtime_desired_language(void);   /* services.c: the game language as a Switch language code (8 bytes) */

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
int runtime_output_mode_cycle(void);                    /* key 3: MetalFX 2K -> native 2K -> off */
uint64_t runtime_output_size_current(void);              /* configured canvas: width in low 32 bits, height in high 32 bits */
void runtime_warm_progress(uint64_t done,uint64_t total,uint64_t shaders,uint64_t shader_total,uint64_t pipelines,uint64_t pipeline_total);
void runtime_warm_done(void);
void runtime_game_slot_acquire(const char *mode);   /* runtime/game_slot.c: one game at a time */
void *runtime_front_override(const char *name);
int runtime_front_focus(void);
void runtime_shortcut_help_toggle(void);
int fps_overlay_toggle_hints(void);   /* shared key 1 toggle for FPS, shader and system-load overlays */
int runtime_aspect_toggle(void);   /* runtime/aspect_patch.h: key 4, 16:9 <-> ultrawide (Mac, 1.2.1 / 1.4.3); 0 where unavailable */
float runtime_aspect_now(void);     /* the ratio in force, 0 = the game's own 16:9 */
int hud_compact(void);   /* gfx/nvn/fps_overlay.c: the overlay lines go to the window's corner panel (TOTK_HUD=corners: in the picture) */
int hud_snapshot(int line, char *out, size_t n, int *highlight);   /* line 0..2: FPS, shaders, load; 0 = not published */

void *runtime_account_override(const char *name);

void runtime_gfx_probe(int negative);
void runtime_vpool_probe(void);

void runtime_memory_probe(void);

void runtime_input_key(unsigned code,int down);
void runtime_input_clear(void);
int runtime_handheld_mode(void);   /* TOTK_OPERATION_MODE=handheld (runtime/services.c) */
void *runtime_input_override(const char *name);
void *runtime_alc_override(const char *name);   /* runtime/system_services.c: Astral Chain network and system functions */
typedef struct { uint64_t buttons; int32_t lx,ly,rx,ry; int connected; } RuntimeGamepadState;
void runtime_gamepad_start(void);
void runtime_gamepad_read(RuntimeGamepadState *out);
/* Forget held buttons and sticks: macOS stops delivering controller events (including releases) while the app is in
   the background, so a button held at focus loss would otherwise stay pressed for the game. */
void runtime_gamepad_clear(void);
void runtime_touch_pad_set(const RuntimeGamepadState *state);   /* on-screen touch controls (iOS) */
/* Controller rumble and motion (opt-in TOTK_RUMBLE=1 / TOTK_GYRO=1): the game's hid side in runtime/hid_feedback.c, the
   Mac controller's haptics and motion sensors in runtime/gamepad_feedback.m. */
#define RUNTIME_MOTION_MAX 32
typedef struct { uint64_t sampling, delta_ns; float acc[3], gyro[3], angle[3], dir[9]; } RuntimeMotionState;   /* G, rev/s, rev, 3 axes */
int runtime_rumble_enabled(void);
int runtime_gyro_enabled(void);
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

/* From the TotK tree: the settings panel, mouse mode and key bindings the copied runtime files use. */
void runtime_audio_output_restart(int recreate);   /* audio_mixer.c: start the outputs again after an audio session interruption (iOS) */
void runtime_spin_wait(unsigned module, uint64_t head);   // threads.c: a pass of a gated spin wait (loader.c runtime_dispatch)
int runtime_render_res(unsigned *w, unsigned *h);   /* TOTK_RENDER_RES above 1600x900 that the heaps count (1.4.3 on the Mac; 0 elsewhere) */
enum { OUTPUT_MODE_OFF, OUTPUT_MODE_METALFX, OUTPUT_MODE_NATIVE, OUTPUT_MODE_NATIVE_METALFX, OUTPUT_MODE_720P };
int runtime_output_mode(void);   /* the mode in force (this port: always OFF) */
int runtime_output_modes(void);
int runtime_output_mode_set(int mode);                  /* returns 0 where that mode is not offered */
int runtime_output_native(int mode, unsigned *w, unsigned *h);   /* the canvas of OUTPUT_MODE_NATIVE / _NATIVE_METALFX; 0 when not offered */
void runtime_audio_hold(int on);   /* audio_mixer.c: silent output while the first frame waits for the warm-up */
void runtime_settings_toggle(void);   /* key 1 (runtime/input.c): the settings panel opens or closes (frontend.m; nothing on the phone) */
int runtime_aspect_set_ratio(float r);   /* the settings panel (aspect_patch.h; game_patches_botw.c): 16:9, 16:10, 21:9, 32:9 ...; 0 where unavailable */
float runtime_aspect_ratio(void);        /* the ratio picked (16:9 = the game's own); 0 where the shape cannot be changed */
float runtime_aspect_start(void);        /* TOTK_ASPECT's ratio, 0 without it (the panel offers it too) */
void runtime_aspect_view(double w, double h);   /* frontend.m: the window's drawable size, which key 4 follows */
void runtime_front_resize_aspect(double ratio);   /* Mac: resize the window to a selected picture ratio */
void runtime_input_mouse(int button,int down);    /* runtime/gamepad.m, mouse mode: a mouse button through the same layout */
void runtime_keyboard_capture(int on);   /* iOS only (runtime/gamepad.m, TARGET_OS_IPHONE): a UIKit text field takes the hardware keyboard; it must not also drive the game */
uint64_t runtime_keyboard_buttons(void);   /* runtime/input.c: the Switch buttons the held keys drive now (no lock) */
void runtime_keymap_get(struct KeyMap *out);       /* runtime/input.c: the keyboard and mouse layout in force */
void runtime_keymap_set(const struct KeyMap *map); /* in force at once, saved in keyboard.txt (the settings panel) */
void runtime_keymap_reset(void);                   /* the default layout, keyboard.txt removed */
void runtime_gamepad_switch_left(void *controller, int x, int y);
void runtime_gamepad_switch_right(void *controller, int x, int y);
void runtime_gamepad_text_capture(int on);
void runtime_gamepad_text_read(RuntimeGamepadState *out);
int runtime_gamepad_game_input(void);   /* any thread: 1 while the game gets the controllers (no capture, nor its release wait) */
void runtime_gamepad_settings_enable(int enabled);
int runtime_gamepad_settings_requested(void);
int runtime_gamepad_settings_button(void);               /* -1: L3 + R3 together; else the physical button (controller_map.h) */
void runtime_gamepad_settings_button_set(int physical);   /* the key bindings; saved in controller-settings.txt, -1 the default */
int runtime_gamepad_type_set(int type);     /* runtime/gamepad.m: the settings panel's controller type (gamepad_layout.h GAMEPAD_TYPE_*), saved; returns the type in force */
int runtime_gamepad_type(int *detected);    /* runtime/gamepad.m, main thread: the picked type; *detected = auto's guess for the connected controller, -1 none */
int runtime_gamepad_battery_percent(void);  /* runtime/gamepad.m, main thread: the first controller's battery as a whole percent, -1 none or unknown */
int runtime_setting_word(const char *name, char *word, size_t size);   /* runtime/gamepad.m: a one-word setting file next to controls.txt; 1 read, 0 none */
void runtime_setting_save(const char *name, const char *word, const char *what);   /* runtime/gamepad.m: saves one; what names it in a failure line */
int runtime_shared_setting_word(const char *name, char *word, size_t size);   /* runtime/gamepad.m: the same, shared by both games (the folder above controls.txt's) */
void runtime_shared_setting_save(const char *name, const char *word, const char *what);
void runtime_setting_remove(const char *name);   /* runtime/gamepad.m: back to the default (a settings panel reset) */
void runtime_controller_map_get(uint16_t *map, uint8_t *modes);
void runtime_controller_map_set(const uint16_t *map, const uint8_t *modes);
void runtime_controller_map_reset(void);
int runtime_mouse_sensitivity(void);          /* runtime/gamepad.m: mouse mode's sensitivity, 1-100 (50) */
int runtime_mouse_sensitivity_locked(void);   /* TOTK_MOUSE_SENSITIVITY decides (the panel's slider greyed out) */
void runtime_mouse_sensitivity_set(int value);   /* the settings panel's slider: at once, saved in mouse-sensitivity.txt */
int runtime_mouse_set(int on);              /* runtime/gamepad.m: the settings panel's mouse mode, saved; returns it */
int runtime_mouse_enabled(void);
void runtime_mouse_set_hook(void (*hook)(void));   /* the window (frontend.m): mouse mode changed, on the main queue */
void runtime_mouse_move(double dx, double dy);      /* points, AppKit's deltaX / deltaY (down positive) */
void runtime_mouse_button(int button, int down);    /* main thread; 0 left, 1 right, 2 middle, 3 and 4 the side buttons */
void runtime_mouse_release(void);                   /* main thread: buttons up, stick centred */
int runtime_phone_fps(void);                /* d2r_gfx hle_nvn.c, iPhone / iPad: 30 while held at 30 (TOTK_PHONE_FPS), else 60 */
void runtime_phone_fps_set(int fps);        /* 30 holds 30, anything else up to 60: the touch controls' button, at once */
int runtime_feedback_locked(int gyro);
int runtime_feedback_present(int gyro);
int runtime_feedback_choice(int gyro);
void runtime_feedback_choose(int gyro, int on);
int runtime_rumble_gain_locked(void);
float runtime_rumble_gain(void);
void runtime_rumble_gain_set(float gain);
int runtime_triggers_available(void);
int runtime_triggers_locked(void);
int runtime_triggers_choice(void);
void runtime_triggers_choose(int on);
void runtime_triggers_release(void);   /* the game ends (atexit, and the window's close, whose _exit skips it): both triggers off */
void runtime_motion_phone(int on);  /* the iPhone's own sensor feeds the six-axis device (1) / stops (0) */
void runtime_motion_mouse(int on);  /* mouse mode (key 7) takes the sensor over (1) / gives it back to the controller (0) */
void runtime_motion_feed_mouse(const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);   /* already in the reference frame */
void runtime_feedback_active(void);   /* the app became active: manual-activation controllers need their sensors on again */
void runtime_gamepad_hid_battery(int left, int percent);   /* runtime/gamepad.m, any thread: a Switch controller's battery read from its HID reports (left: a paired set's left Joy-Con), -1 none */
int runtime_gamepad_battery_left_percent(void);  /* runtime/gamepad.m: a paired Joy-Con set's left one, -1 none; the percent above is then the right one */
