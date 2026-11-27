#pragma once
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
typedef struct { uint64_t where; unsigned kind, symbol; int64_t addend; } Reloc;
typedef struct { const char *name; uint64_t value, size; unsigned defined, binding, type; } Symbol;
typedef struct { uint64_t ret, original; unsigned timer, arg; } Gate;
typedef struct {
 const char *name, *digest; size_t file_size, size;
 uint64_t text_end, ro_off, data_off, patch_off, key_off, prepatch_off, prepatch_size, init, ctor_flag, init_array, init_size;
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
int runtime_virtual_pool(uint64_t address,uint64_t size);
int runtime_virtual_pool_committed(uint64_t address,uint64_t size);
void runtime_patch_virtual_pool(uint8_t *main_base,uint8_t *stub_page);
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
void runtime_session_prepare(void); /* boot: mandatory user ownership and exclusive shader-cache writer */
void runtime_game_slot_acquire(const char *mode);   /* boot: per-user ownership; GPU probes: exclusive slot */
void *runtime_front_override(const char *name);
int runtime_front_focus(void);
void runtime_shortcut_help_toggle(void);
int fps_overlay_hud_mode(void);
int fps_overlay_full_hud(void);
void fps_overlay_set_hud_mode(int mode);
int runtime_fps_cap_now(void);        /* key 8: 30 -> 60 -> 90 -> 120 FPS (default 60) */
int runtime_fps_cap_cycle(void);
int runtime_aspect_toggle(void);   /* key 4: 16:9 -> fit to the window -> 21:9 */
int runtime_aspect_step(void);     /* key 4's step: 0 16:9, 1 fit to the window, 2 21:9 */
void runtime_aspect_view(double w, double h);   /* frontend.m: the window's drawable size */
float runtime_aspect_now(void);     /* the ratio in force, 0 = the game's own 16:9 */
int hud_compact(void);   /* gfx/nvn/fps_overlay.c: the overlay lines go to the window's corner panel (TOTK_HUD=corners: in the picture) */
int hud_snapshot(int line, char *out, size_t n, int *highlight);   /* line 0..2: FPS, shaders, load; 0 = not published */

void *runtime_account_override(const char *name);

void runtime_gfx_probe(int negative);
void runtime_vpool_probe(void);

void runtime_memory_probe(void);
void runtime_publish_os_resource_manager(void);   /* services.c: fill the unconstructed stack-guard allocator */

void runtime_input_key(unsigned code,int down);
void runtime_keys_clear(void);   /* held keyboard keys only; controllers keep reporting while the window is in the background */
void runtime_input_clear(void);
int runtime_handheld_mode(void);   /* TOTK_OPERATION_MODE=handheld (runtime/services.c) */
void *runtime_input_override(const char *name);
void *runtime_by3_override(const char *name);   /* runtime/system_services.c: Bayonetta 3 network and system functions */
#define RUNTIME_MAX_GAMEPADS 2
typedef struct { uint64_t buttons; int32_t lx,ly,rx,ry; int connected; } RuntimeGamepadState;
void runtime_gamepad_start(void);
void runtime_gamepad_read(RuntimeGamepadState *out);
void runtime_gamepad_read_index(unsigned index, RuntimeGamepadState *out);
unsigned runtime_gamepad_count(void);
void runtime_input_gamepads_changed(void);   /* a pad was attached or removed; tell the game on its next read */
/* Forget held buttons and sticks: macOS stops delivering controller events (including releases) while the app is in
   the background, so a button held at focus loss would otherwise stay pressed for the game. */
void runtime_gamepad_clear(void);
int runtime_gamepad_type_cycle(void);
int runtime_gamepad_type_set(int type);
int runtime_gamepad_type(int *detected);
int runtime_mouse_toggle(void);
int runtime_mouse_enabled(void);
void runtime_mouse_set_hook(void (*hook)(void));
void runtime_mouse_move(double dx, double dy);
void runtime_mouse_button(int button, int down);
void runtime_mouse_release(void);

/* iOS settings, main queue only. Targets are Switch bit indices, -1 = disabled.
   Save succeeds before the live layout changes. Reset affects only the 16 pad buttons. */
/* Per-port configuration, main queue only, on both platforms. */
int runtime_gamepad_slot_count(void);
int runtime_gamepad_slot_type(int slot);
int runtime_gamepad_slot_type_set(int slot, int type);
int runtime_gamepad_slot_mapping_get(int slot, int physical);
int runtime_gamepad_slot_mapping_set(int slot, int physical, int target);
int runtime_gamepad_slot_mapping_reset(int slot);
/* Target-first editor: group 0 = controller, 1 = mouse. -2 means multiple
   existing inputs; opening the editor must not collapse them. */
int runtime_input_binding_get(int slot, int group, int target);
int runtime_input_binding_set(int slot, int group, int target, int physical);
int runtime_input_binding_reset(int slot, int group);
/* Analog output binding: target 0/1 = NS left/right stick; source 0 = off,
   1 = physical left stick, 2 = physical right stick. Mouse target is 0/off,
   1/left, 2/right, 3/both and is available only on player 1. */
int runtime_input_stick_get(int slot, int target);
int runtime_input_stick_set(int slot, int target, int source);
int runtime_input_mouse_stick_get(void);
int runtime_input_mouse_stick_set(int target);
#define INPUT_MOTION_PLAYERS 2
/* Six-axis source: 0 automatic (existing behavior), 1 controller, 2 mouse, 3 off. */
int runtime_input_gyro_get(int slot);
int runtime_input_gyro_set(int slot, int source);
int runtime_input_motion_allowed(int slot);


int runtime_gamepad_connected(unsigned index); /* does not consume button edges */
int runtime_gamepad_mapping_get(int physical);
int runtime_gamepad_mapping_set(int physical, int target);
int runtime_gamepad_mapping_reset(void);
/* iOS coalesced keyboard configuration (main queue); 0 key means unbound.
   Slot selection is zero-based, -1 disables; key codes are USB HID usages. */
int runtime_keyboard_slot(void);
int runtime_keyboard_slot_count(void);
int runtime_keyboard_bind(int slot);
int runtime_keyboard_mapping_get(int slot, int action);
int runtime_keyboard_mapping_set(int slot, int action, int key);
int runtime_keyboard_mapping_reset(int slot);
void runtime_keyboard_suspend(int suspended);


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
/* Indexed motion is required for local co-op; legacy helpers below address P1. */
int runtime_motion_read_index(unsigned slot, RuntimeMotionState *out, int max);
void runtime_motion_start_index(unsigned slot);
void runtime_motion_feed_index(unsigned slot, const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);
void runtime_motion_live_index(unsigned slot, int on);
void runtime_motion_deadzone_default_index(unsigned slot, double deg_s);
void runtime_gamepad_hid_motion(unsigned slot, const uint8_t *p, int len);
void runtime_gamepad_hid_motion_reset(unsigned slot);
int runtime_motion_read(RuntimeMotionState *out, int max);   /* newest first; 0 = no motion now */
void runtime_motion_start(void);   /* the game (re)started its sensor: angle and direction start over */
void runtime_motion_feed(const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);   /* runtime/motion_fusion.c */
int runtime_motion_axes(const char *spec);   /* TOTK_GYRO_AXES ("-x,y,-z"): 1 set, 0 unset, -1 malformed */
void runtime_motion_deadzone_default(double deg_s);   /* when TOTK_GYRO_DEADZONE is unset: 1.2 for HID motion, else 0 */
void runtime_motion_live(int on);   /* a controller with motion attached (1) / detached (0) */
void runtime_motion_mouse(int on);
void runtime_motion_feed_mouse(const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);
void runtime_feedback_quiet(void);   /* focus lost: stop rumbling */
#ifdef __OBJC__
@class GCController;
void runtime_feedback_attach(GCController *controller);
void runtime_feedback_detach(GCController *controller);
void runtime_feedback_set_slot(GCController *controller, int slot);
#endif

/* Main-queue settings lifecycle; keyboard_mac_key consumes configured AppKit input. */
void runtime_input_settings_suspend(int suspended);
int runtime_keyboard_mac_key(unsigned code, int down);
void runtime_keyboard_clear(void);
int runtime_input_settings_active(void);

#ifdef __OBJC__
@class NSArray, NSDictionary, NSString;
/* Main queue. Device IDs are opaque connection tokens, not persisted hardware IDs. */
NSArray *runtime_gamepad_devices(void);
NSString *runtime_gamepad_device_binding(int slot);
NSString *runtime_gamepad_device_label(int slot);
int runtime_gamepad_device_bind(int slot, NSString *identity);
#endif

/* macOS only, explicit opt-in; no permission request during normal startup. */
int runtime_keyboard_physical_enable(int enabled);
int runtime_keyboard_physical_enabled(void);
void runtime_keyboard_physical_focus(int focused);
#ifdef __OBJC__
NSString *runtime_keyboard_physical_status(void);
NSArray *runtime_keyboard_physical_devices(void);
NSString *runtime_keyboard_physical_binding(int slot);
NSString *runtime_keyboard_physical_label(int slot);
int runtime_keyboard_physical_bind(int slot, NSString *identity);
#endif

#include "input_source.h"
int runtime_input_source(int slot); /* lock-free, available to guest threads */
int runtime_input_source_allows(int slot, unsigned device);
int runtime_input_source_set(int slot, int source); /* main queue, persist before apply */
void runtime_mouse_refresh_source(void); /* re-evaluate gyro ownership after port source changes */
