// Host front end (Mac: front_mac.m; iOS: dm/ios separately implements the same-named interface).
#pragma once
#include <stdlib.h>
// Metal reads HUD switches when the first device is made. Called before host preload and again after the launcher applies its settings; explicit environment values win.
static inline void front_hud_env(void) {
    const char *e = getenv("DM_METAL_HUD");
    if (!e || !atoi(e)) return;
    setenv("MTL_HUD_ENABLED", "1", 0);
    // Spatial scaling has no temporal exposure: showing that HUD metric makes the MetalFX rows flicker.
    setenv("MTL_HUD_HIDE_METALFX_METRICS", "1", 0);
}
// Run the guest: with a window the guest runs on its own thread and the main thread belongs to the system UI; without a window it runs directly on the current thread.
void front_run(void (*guest)(void));
// The CAMetalLayer the graphics backend presents frames to (NULL when there is no window)
void *front_metal_layer(void);
// The highest refresh rate of the window's screen in Hz, 0 = not known (Mac front_mac.m; none on iOS, whose screens run at multiples of 60 Hz). Any thread
int front_screen_max_fps(void);
// The frame rate the display link paces the game at (60, 30, 120; nvn_mtl.m link_sync): in fullscreen, a screen whose refresh rate is not a multiple of it is switched, while the game runs,
// to a mode of the same size whose rate is, and back when the window leaves fullscreen or that screen (front_mac.m; none on iOS). Called from the present thread
void front_screen_match(int fps);
// Progress screen of the first-start prepare step (nvn_mtl.m warm_prepare: "Preparing shaders" while the pipelines of the prewarm lists are built before the game's first frame):
// title and done of total shown over the game's view (a dark screen, the title, a bar and a count), updated on each call; title NULL hides it. Any thread; the front end switches to its main
// thread and does not block. Nothing is shown without a window
void front_progress(const char *title, long done, long total);
#include <stdint.h>
// Text input box (for the software keyboard swkbd): the main thread shows a native input box and the calling thread blocks until the user confirms / cancels.
// All text is UTF-8; max_chars is the maximum character count (0 = unlimited, only a hint for the front end; truncation is done uniformly by the caller hle_swkbd.c in UTF-16 units).
// Confirm returns 1 and writes the result into out (including the terminating 0); cancel returns 0;
// with no window (front_metal_layer() == NULL) nothing is shown and -1 is returned.
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap);

// Early start of host loading (main.c): reading the NSO / relocation / forced takeover / prefetch start are moved to a background thread, in parallel with the front end (UIKit launch chain, Metal view);
// the dm_main / main called afterwards waits for it to finish. For call timing and preconditions see the comments in main.c (set DM_ROMFS and the other environment variables and the cwd first; arguments identical item by item to those of the later main).
void dm_host_preload_start(int argc, char **argv);

// Touchscreen (implemented by the host's hle_touch.c; called by the iOS / Mac front end): whenever the set of touch points changes (down / move / up / cancel) the whole set is pushed once; n = 0 = all lifted.
// id = finger slot number 0–15 (the lowest free slot on down, unchanged until up; used directly as finger_id); x / y = normalized coordinates [0,1) on the output frame, origin at top left.
// The host converts to the panel coordinates the game reads, 1280x720 (x·1280, y·720), and latches short taps (a down and up between two reads is reported at least once). May be called from any thread.
typedef struct { uint32_t id; float x, y; } dm_touch;
void dm_touch_set(const dm_touch *t, int n);

// Save not written (called by the host's hle_fs.c; the iOS front end provides a strong implementation of the same name that shows the native prompt "Not enough storage. Progress was not saved"; the default weak implementation only logs).
// Timing: when a save write fails (disk full etc.) within one of the game's save transactions (between two Commits), so that this Commit is refused and the previously committed save is unchanged, it is called once per refused transaction;
// the next transaction that succeeds commits as usual, and the front end need not do any recovery. err = the first failing errno (ENOSPC = 28 means out of space). Called on the game thread; the front end switches to the main thread itself and must not block
void front_save_failed(int err);
