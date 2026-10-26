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
void runtime_audio_output_restart(int recreate);   /* audio_mixer.c: start the outputs again after an audio session interruption (iOS) */
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
extern char **runtime_main_argv;                 /* relaunch.c: main's arguments, for starting the game again */
_Noreturn void runtime_relaunch(const char *why);  /* relaunch.c: the game asked to launch itself (Q74) */
uint8_t *runtime_module_base(const char *name);   /* runtime/loader.c: "main", "subsdk0", "sdk" (probes) */
uint64_t runtime_module_permission(uint64_t address,uint64_t size,unsigned permission);
uint64_t runtime_module_lock_permission(uint64_t address,uint64_t size,unsigned mask,unsigned attribute);
int runtime_module_query(uint64_t address,uint64_t *base,uint64_t *size,unsigned *state,unsigned *permission,unsigned *attribute);
void runtime_oe_push_message(uint32_t message);
void hle_bind_system_event(void *event, int autoclear);
void hle_signal_system_event(void *event);   /* threads.c: signal an event bound with hle_bind_system_event (any thread) */
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
int runtime_output_res_toggle(void);                    /* legacy two-state output size switch */
int runtime_output_mode_cycle(void);                    /* key 3: MetalFX 2K -> native 2K -> off */
uint64_t runtime_render_pool_extra(void);               /* bytes the bigger render target pool adds to the application pool */
const char *runtime_picture_name(void);
const char *runtime_interp_name(void);                   /* key 2's current state for the shortcut box, NULL = the shared wording */                  /* key 3's current state for the shortcut box, NULL = the 2K canvas wording */
uint64_t runtime_output_size_current(void);              /* configured canvas: width in low 32 bits, height in high 32 bits */
void runtime_warm_progress(uint64_t done,uint64_t total,uint64_t shaders,uint64_t shader_total,uint64_t pipelines,uint64_t pipeline_total);
void runtime_warm_done(void);
void runtime_game_slot_acquire(const char *mode);   /* runtime/game_slot.c: one game at a time */
void *runtime_front_override(const char *name);
int runtime_front_focus(void);
void runtime_shortcut_help_toggle(void);
int fps_overlay_toggle_hints(void);   /* key 1: all -> minimal (FPS only) -> off; returns the new mode, 0 = off */
int fps_overlay_hud_mode(void);
int fps_overlay_full_hud(void);
int runtime_aspect_toggle(void);   /* runtime/aspect_patch.h: key 4, 16:9 -> fit to the window / display -> 21:9 (or TOTK_ASPECT) (Mac, 1.2.1 / 1.4.3); 0 where unavailable */
int runtime_aspect_step(void);     /* key 4's step: 0 16:9, 1 fit to the window, 2 21:9 / TOTK_ASPECT; -1 where unavailable */
void runtime_aspect_view(double w, double h);   /* frontend.m: the window's drawable size, which key 4 follows */
float runtime_aspect_now(void);     /* the ratio in force, 0 = the game's own 16:9 */
int hud_compact(void);   /* gfx/nvn/fps_overlay.c: the overlay lines go to the window's corner panel (TOTK_HUD=corners: in the picture) */
int hud_snapshot(int line, char *out, size_t n, int *highlight);   /* line 0..3: FPS, shaders, render pipelines, load; 0 = not published */

void *runtime_account_override(const char *name);
void *runtime_audio_out_override(const char *name);   /* runtime/audio_out.c: nn::audio::AudioOut (Super Smash Bros. Ultimate) */
void *runtime_ro_override(const char *name);       /* runtime/ro_host.c: nn::ro, the NRO modules Super Smash Bros. Ultimate loads */
int runtime_ro_query(uint64_t a, uint64_t *base, uint64_t *size, unsigned *state, unsigned *perm);   /* ro_host.c: QueryMemory of an NRO */
void *runtime_system_override(const char *name);   /* runtime/system_services.c: offline network and system functions */
void *runtime_font_observer(const char *name);     /* runtime/system_services.c: SMO_TRACE_FONT=1 font engine trace */

void runtime_gfx_probe(int negative);
void runtime_vpool_probe(void);

void runtime_memory_probe(void);

void runtime_input_key(unsigned code,int down);
void runtime_input_clear(void);
int runtime_handheld_mode(void);   /* TOTK_OPERATION_MODE=handheld (runtime/services.c) */
void *runtime_input_override(const char *name);
typedef struct { uint64_t buttons; int32_t lx,ly,rx,ry; int connected; } RuntimeGamepadState;
void runtime_gamepad_start(void);
void runtime_gamepad_read(RuntimeGamepadState *out);
/* Forget held buttons and sticks: macOS stops delivering controller events (including releases) while the app is in
   the background, so a button held at focus loss would otherwise stay pressed for the game. */
void runtime_gamepad_clear(void);
int runtime_gamepad_type_cycle(void);       /* runtime/gamepad.m: key 6, the controller type auto -> Xbox -> PlayStation -> Switch (gamepad_layout.h GAMEPAD_TYPE_*) */
int runtime_gamepad_type(int *detected);    /* runtime/gamepad.m, main thread: the picked type; *detected = auto's guess for the connected controller, -1 none */
int runtime_mouse_toggle(void);             /* runtime/gamepad.m: key 7, mouse mode on (1) / off (0) */
int runtime_aoc_owned(int *fighters);       /* runtime/services.c (Q96): DLC owned (*fighters of them), -1 when turned off */
void runtime_album_set_photo_dir(const char *dir);   /* runtime/album_service.c (Q56): the Photos menu's folder, for the next photo */
int runtime_mouse_enabled(void);
void runtime_mouse_set_hook(void (*hook)(void));   /* the window (frontend.m): mouse mode changed, on the main queue */
void runtime_mouse_move(double dx, double dy);      /* points, AppKit's deltaX / deltaY (down positive) */
void runtime_mouse_button(int button, int down);    /* main thread; 0 left, 1 right, 2 middle, 3 and 4 the side buttons */
void runtime_mouse_release(void);                   /* main thread: buttons up, stick centred */
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
void runtime_motion_mouse(int on);  /* mouse mode (key 7) takes the sensor over (1) / gives it back to the controller (0) */
void runtime_motion_feed_mouse(const double acc_g[3], const double gyro_rad_s[3], uint64_t now_ns);   /* already in the reference frame */
void runtime_feedback_quiet(void);   /* focus lost: stop rumbling */
#ifdef __OBJC__
@class GCController;
void runtime_feedback_attach(GCController *controller);
void runtime_feedback_detach(GCController *controller);
#endif
