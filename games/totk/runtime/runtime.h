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
int mtl_pipeline_archive_tool(int argc, char **argv);   // Mac only (nvn_mtl.m): the device's pipeline archive for packaging
void runtime_audio_probe(void);
void runtime_audio_output_restart(int recreate);   /* audio_mixer.c: start the outputs again after an audio session interruption (iOS) */
void *runtime_hle_wrap(void *fn);
void runtime_hle_stack_set(uintptr_t top);
void *runtime_font_override(const char *name);
void *runtime_codec_override(const char *name);
void *runtime_movie_override(const char *name);   /* runtime/movie_host.c: movie::Decoder for VP9 cutscenes */
void *runtime_time_override(const char *name);
void *runtime_applet_override(const char *name);
void *runtime_swkbd_override(const char *name);   /* runtime/swkbd_host.c: nn::swkbd text entry with keyboard or controller */
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
/* Text entry in the game window (runtime/frontend.m, Mac): controller keyboard and Mac typing, Return confirms (1, the text
   as UTF-8 in out), Escape cancels (0). The caller waits; give the virtual core up around it (runtime_core_away). */
typedef struct { const char *prompt, *initial; int max_units, min_units, secure, digits_only; uint32_t disabled; } RuntimeTextRequest;
int runtime_front_text_input(const RuntimeTextRequest *req, char *out, size_t out_size);
void runtime_front_text_test(const char *op, const char *text);   /* diagnostics: "type" text, "enter", "esc" into the open bar */
int runtime_core_away(void);          /* runtime/threads.c: a guest thread gives its virtual core up around a long host wait */
void runtime_core_back(int *away);
void runtime_spin_wait(unsigned module, uint64_t head);   // threads.c: a pass of a gated spin wait (loader.c runtime_dispatch)
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
int runtime_render_res(unsigned *w, unsigned *h);   /* TOTK_RENDER_RES above 1600x900 that the heaps count (1.4.3 on the Mac; 0 elsewhere) */
/* The settings panel's resolution (key 1, frontend.m): 1080p, MetalFX scaling 1080p up to the window, or the game's own
   canvas at its native size (2K; runtime_output_native gives it), the same with MetalFX scaling it up to the window
   (user 2026-10-25: "4K" as 2K + MetalFX, a 4K swapchain's heaps being too big for one GPU buffer); also 720p, a
   1280x720 canvas whose scene follows it (user 2026-10-25, in place of the Switch's own handheld / docked sizes: the game
   reads its docked scene size only at the start, so a switch to it never took effect).
   runtime_output_modes: a bit (1 << mode) for each this game and start offer; 0 when the resolution cannot change. */
enum { OUTPUT_MODE_OFF, OUTPUT_MODE_METALFX, OUTPUT_MODE_NATIVE, OUTPUT_MODE_NATIVE_METALFX, OUTPUT_MODE_720P };
int runtime_output_modes(void);
int runtime_output_mode(void);
int runtime_output_mode_set(int mode);                  /* returns 0 where that mode is not offered */
int runtime_output_native(int mode, unsigned *w, unsigned *h);   /* the canvas of OUTPUT_MODE_NATIVE / _NATIVE_METALFX; 0 when not offered */
uint64_t runtime_output_size_current(void);              /* configured canvas: width in low 32 bits, height in high 32 bits */
void runtime_warm_progress(uint64_t done,uint64_t total,uint64_t shaders,uint64_t shader_total,uint64_t pipelines,uint64_t pipeline_total);
void runtime_warm_done(void);
void runtime_audio_hold(int on);   /* audio_mixer.c: silent output while the first frame waits for the warm-up */
void runtime_game_slot_acquire(const char *mode);   /* runtime/game_slot.c: one game at a time */
void *runtime_front_override(const char *name);
int runtime_front_focus(void);
void runtime_settings_toggle(void);   /* key 1 (runtime/input.c): the settings panel opens or closes (frontend.m; nothing on the phone) */
void fps_overlay_set_hud_mode(int mode);   /* gfx/nvn/fps_overlay.h HUD_MODE_*: the panel's overlay choice */
int fps_overlay_hud_mode(void);
int fps_overlay_full_hud(void);
int runtime_aspect_set_ratio(float r);   /* the settings panel (aspect_patch.h; game_patches_botw.c): 16:9, 16:10, 21:9, 32:9 ...; 0 where unavailable */
float runtime_aspect_ratio(void);        /* the ratio picked (16:9 = the game's own); 0 where the shape cannot be changed */
float runtime_aspect_start(void);        /* TOTK_ASPECT's ratio, 0 without it (the panel offers it too) */
void runtime_aspect_view(double w, double h);   /* frontend.m: the window's drawable size, which key 4 follows */
float runtime_aspect_now(void);     /* the ratio in force, 0 = the game's own 16:9 */
void runtime_front_resize_aspect(double ratio);   /* Mac: resize the window to a selected picture ratio */
int hud_compact(void);   /* gfx/nvn/fps_overlay.c: the overlay lines go to the window's corner panel (TOTK_HUD=corners: in the picture) */
int hud_snapshot(int line, char *out, size_t n, int *highlight);   /* line 0..3: FPS, shaders, render pipelines, load; 0 = not published */

void *runtime_account_override(const char *name);

void runtime_gfx_probe(int negative);
void runtime_vpool_probe(void);

void runtime_memory_probe(void);

void runtime_input_key(unsigned code,int down);   /* a window key: the player's layout (runtime/keyboard_map.h) */
void runtime_input_mouse(int button,int down);    /* runtime/gamepad.m, mouse mode: a mouse button through the same layout */
void runtime_keyboard_capture(int on);   /* iOS only (runtime/gamepad.m, TARGET_OS_IPHONE): a UIKit text field takes the hardware keyboard; it must not also drive the game */
void runtime_input_clear(void);
uint64_t runtime_keyboard_buttons(void);   /* runtime/input.c: the Switch buttons the held keys drive now (no lock) */
struct KeyMap;   /* runtime/keyboard_map.h */
void runtime_keymap_get(struct KeyMap *out);       /* runtime/input.c: the keyboard and mouse layout in force */
void runtime_keymap_set(const struct KeyMap *map); /* in force at once, saved in keyboard.txt (the settings panel) */
void runtime_keymap_reset(void);                   /* the default layout, keyboard.txt removed */
int runtime_handheld_mode(void);   /* TOTK_OPERATION_MODE=handheld (runtime/services.c) */
void *runtime_input_override(const char *name);
typedef struct { uint64_t buttons; int32_t lx,ly,rx,ry; int connected; } RuntimeGamepadState;
void runtime_gamepad_start(void);
void runtime_gamepad_switch_left(void *controller, int x, int y);
void runtime_gamepad_switch_right(void *controller, int x, int y);
void runtime_gamepad_read(RuntimeGamepadState *out);
/* Mac software keyboard: exclusive controller capture; UI reads physical positions (controller_map.h),
   independent of gameplay remaps. Main-thread only. Closing waits for release before gameplay resumes. */
void runtime_gamepad_text_capture(int on);
void runtime_gamepad_text_read(RuntimeGamepadState *out);
int runtime_gamepad_game_input(void);   /* any thread: 1 while the game gets the controllers (no capture, nor its release wait) */
/* macOS opts in; the controller's settings button (L3 + R3 together, or the one picked in the key bindings) is latched
   independently of guest polling. */
void runtime_gamepad_settings_enable(int enabled);
int runtime_gamepad_settings_requested(void);
int runtime_gamepad_settings_button(void);               /* -1: L3 + R3 together; else the physical button (controller_map.h) */
void runtime_gamepad_settings_button_set(int physical);   /* the key bindings; saved in controller-settings.txt, -1 the default */
/* Forget held buttons and sticks: macOS stops delivering controller events (including releases) while the app is in
   the background, so a button held at focus loss would otherwise stay pressed for the game. */
void runtime_gamepad_clear(void);
int runtime_gamepad_type_set(int type);     /* runtime/gamepad.m: the settings panel's controller type (gamepad_layout.h GAMEPAD_TYPE_*), saved; returns the type in force */
int runtime_gamepad_type(int *detected);    /* runtime/gamepad.m, main thread: the picked type; *detected = auto's guess for the connected controller, -1 none */
int runtime_gamepad_battery_percent(void);  /* runtime/gamepad.m, main thread: the first controller's battery as a whole percent, -1 none or unknown */
int runtime_setting_word(const char *name, char *word, size_t size);   /* runtime/gamepad.m: a one-word setting file next to controls.txt; 1 read, 0 none */
void runtime_setting_save(const char *name, const char *word, const char *what);   /* runtime/gamepad.m: saves one; what names it in a failure line */
int runtime_shared_setting_word(const char *name, char *word, size_t size);   /* runtime/gamepad.m: the same, shared by both games (the folder above controls.txt's) */
void runtime_shared_setting_save(const char *name, const char *word, const char *what);
void runtime_setting_remove(const char *name);   /* runtime/gamepad.m: back to the default (a settings panel reset) */
/* runtime/gamepad.m, main thread: the controller layout (controller_map.h, CONTROLLER_BUTTONS entries each) in force;
   set puts one in force and saves it in controls.txt, reset goes back to the default and removes the file. */
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
void runtime_touch_pad_set(const RuntimeGamepadState *state);   /* on-screen touch controls (iOS) */
int runtime_phone_fps(void);                /* d2r_gfx hle_nvn.c, iPhone / iPad: 30 while held at 30 (TOTK_PHONE_FPS), else 60 */
void runtime_phone_fps_set(int fps);        /* 30 holds 30, anything else up to 60: the touch controls' button, at once */
/* Controller rumble and motion (opt-in TOTK_RUMBLE=1 / TOTK_GYRO=1): the game's hid side in runtime/hid_feedback.c, the
   Mac controller's haptics and motion sensors in runtime/gamepad_feedback.m. */
#define RUNTIME_MOTION_MAX 32
typedef struct { uint64_t sampling, delta_ns; float acc[3], gyro[3], angle[3], dir[9]; } RuntimeMotionState;   /* G, rev/s, rev, 3 axes */
int runtime_rumble_enabled(void);
int runtime_gyro_enabled(void);
void *runtime_hid_feedback_override(const char *name);
int runtime_hid_feedback_flag(const char *name, int v);   /* "flag rumble|gyro 0|1"; 0 = not one of these names */
/* The settings panel's rumble (gyro 0) and gyro aiming (gyro 1) switches (runtime/hid_feedback.c): locked when TOTK_RUMBLE /
   TOTK_GYRO or an agent's test session decides; present when the game has the device in this run; the choice in force, and
   choosing saves rumble.txt / gyro.txt and mutes or unmutes a present device at once. Its strength
   (runtime/gamepad_feedback.m): locked when TOTK_RUMBLE_GAIN is set; set saves rumble-gain.txt and applies at once. */
int runtime_feedback_locked(int gyro);
int runtime_feedback_present(int gyro);
int runtime_feedback_choice(int gyro);
void runtime_feedback_choose(int gyro, int on);
int runtime_rumble_gain_locked(void);
float runtime_rumble_gain(void);
void runtime_rumble_gain_set(float gain);
void runtime_hid_feedback_stats(uint64_t *values, uint64_t *motion_reads);
void runtime_rumble_set(unsigned side, float amp_low, float freq_low, float amp_high, float freq_high);   /* side 0 left, 1 right */
/* The settings panel's DualSense bow trigger switch (runtime/gamepad_feedback.m, main queue): available for Breath of the Wild
   1.9.0 and Tears of the Kingdom 1.4.3; locked when TOTK_DUALSENSE_TRIGGERS or an agent's test session decides; the choice
   in force; choosing saves dualsense-triggers.txt and turns the connected DualSense's triggers on or off at once. */
int runtime_triggers_available(void);
int runtime_triggers_locked(void);
int runtime_triggers_choice(void);
void runtime_triggers_choose(int on);
void runtime_triggers_release(void);   /* the game ends (atexit, and the window's close, whose _exit skips it): both triggers off */
int runtime_motion_read(RuntimeMotionState *out, int max);   /* newest first; 0 = no motion now */
void runtime_motion_start(void);   /* the game (re)started its sensor: angle and direction start over */
void runtime_motion_feed(const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);   /* runtime/motion_fusion.c */
int runtime_motion_axes(const char *spec);   /* TOTK_GYRO_AXES ("-x,y,-z"): 1 set, 0 unset, -1 malformed */
void runtime_motion_deadzone_default(double deg_s);   /* when TOTK_GYRO_DEADZONE is unset: 1.2 for HID motion, else 0 */
void runtime_motion_live(int on);   /* a controller with motion attached (1) / detached (0) */
void runtime_motion_phone(int on);  /* the iPhone's own sensor feeds the six-axis device (1) / stops (0) */
void runtime_motion_mouse(int on);  /* mouse mode (key 7) takes the sensor over (1) / gives it back to the controller (0) */
void runtime_motion_feed_mouse(const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);   /* already in the reference frame */
void runtime_feedback_quiet(void);   /* focus lost: stop rumbling */
#ifdef __OBJC__
@class GCController;
void runtime_feedback_attach(GCController *controller);
void runtime_feedback_active(void);   /* the app became active: manual-activation controllers need their sensors on again */
void runtime_feedback_detach(GCController *controller);
#endif
