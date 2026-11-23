#include "game_patches.h"
#include "aspect_patch.h"
#include "game_version.h"
#include "world_survey.h"
#include "control_policy.h"
#include <TargetConditionals.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdatomic.h>
#include <math.h>

/* 60 FPS.
   TOTK's adaptive present-interval controller (main+0x8f6bec, called once per frame from main+0x8f68b4) turns the main
   thread's measured frame work time (system object +0x118, written at main+0x8f6a44 just before the present) into a
   number of 60 Hz ticks, clamps it to the controller's [minimum, maximum] interval and hands the result both to
   nvnWindowSetPresentInterval and to the variable-frame-rate step (main+0x7a83e4: ratio = interval * 0.5, frame time =
   interval / 60). Shipping code uses a minimum of 2 (30 fps), so game speed follows whatever interval is chosen.
   The patch lowers the minimum to 1 (60 fps). The controller still raises the interval to 2 or 3 when a frame's work
   does not fit, with its own hysteresis, so slow scenes fall back to 30 or 20 fps at correct speed.
   Two further sites make the lowering robust: the controller now tries a lower interval when the measured work fits
   it (instead of waiting for a stored candidate that shipping code initialises to the old minimum), and the lowered
   result is floored at 1 so an interval of 0 can never reach the window or the frame-rate step.
   TOTK_FPS60=0 leaves the shipping controller untouched. */
typedef struct { uint64_t offset; uint32_t original, replacement; const char *what; } GamePatch;



/* One atomic 64-bit value holds ratio (low float) and frame seconds (high
   float). The guest reads the two lanes independently; both always remain
   finite and in range, including if an update falls between the reads. */
static _Atomic uint64_t *frame_step_data;
static uint64_t last_present_ns;
static double frame_samples[5];
static unsigned frame_sample_count, frame_sample_next;


static uint64_t frame_step_bits(float seconds) {
    float ratio = seconds * 30.0f;
    uint32_t lo, hi;
    memcpy(&lo, &ratio, 4);
    memcpy(&hi, &seconds, 4);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/* Diagnostics (TOTK_INPUT_FILE "framestep <ms>"): pin the frame step to a fixed length, 0 back to real time. 2026-10-17
   this showed the game over "retry" waiting on a step of at least one 30 fps frame. */
static _Atomic uint32_t pinned_step_us;
void runtime_frame_step_pin(double ms) {
    uint32_t us = ms > 0 ? (uint32_t)(ms * 1000.0 + 0.5) : 0;
    atomic_store(&pinned_step_us, us);
    if (frame_step_data && us) atomic_store_explicit(frame_step_data, frame_step_bits((float)(us / 1e6)), memory_order_release);
    if (!us) fprintf(stderr, "[totk] frame step back to real time\n");
    else fprintf(stderr, "[totk] frame step pinned at %.3f ms (ratio %.3f, diagnostics)%s\n", us / 1000.0, us / 1e6 * 30.0,
                 frame_step_data ? "" : " -- but the real-time patch is off, nothing changes");
}
static void render_control_latch(void);
static _Atomic uint64_t render_control_request;
static _Atomic uint64_t render_control_applied;
void runtime_frame_step_update(uint64_t now_ns) {
    render_control_latch();
    if (!frame_step_data || !now_ns) return;
    if (atomic_load_explicit(&pinned_step_us, memory_order_relaxed)) { last_present_ns = now_ns; return; }
    if (!last_present_ns) { last_present_ns = now_ns; return; }
    double dt = (double)(now_ns - last_present_ns) * 1e-9;
    last_present_ns = now_ns;
    if (dt > 0.25 || dt < 0.001) { frame_sample_count = frame_sample_next = 0; return; }
    double minimum = atomic_load(&render_control_request) ? 1.0 / 1000.0 : 1.0 / 120.0;
    if (dt < minimum) dt = minimum;
    if (dt > 1.0 / 15.0) dt = 1.0 / 15.0;
    frame_samples[frame_sample_next++ % 5] = dt;
    if (frame_sample_count < 5) frame_sample_count++;
    double sum = 0;
    for (unsigned i = 0; i < frame_sample_count; i++) sum += frame_samples[i];
    atomic_store_explicit(frame_step_data, frame_step_bits((float)(sum / frame_sample_count)), memory_order_release);
}


/* iOS cannot write code pages at run time: tools/export (TOTK_EXPORT_IOS, runtime/loader.c) runs these same patches on the
   Mac and the patched text is embedded in the app. On the device the loader only checks that the patched words are there. */
#if TARGET_OS_IPHONE
#define CODE_PATCHED_AT_BUILD 1
#else
#define CODE_PATCHED_AT_BUILD 0
#endif



static _Atomic uint64_t *render_control_page;

/* Output resolution ("true 2K", 2026-10-17; key 3 switches it while playing). Two sizes are 1920x1080 in the game:
   1. The swapchain. nnMain writes the framework's display size as a constant before creating the graphics framework
      ("NvInitializeGraphics"); the framework's system object keeps it at +0x9c / +0xa0 and creates the swapchain from it
      (main+0xe9d8c0). It is made the largest output size (2560x1440), fixed for the run:
        main+0xe996ac  mov  x8, #0x780            (1920)   -> mov  x8, #w
        main+0xe996b0  movk x8, #0x438, lsl #32   (1080)   -> movk x8, #h, lsl #32
   2. The output size of the graphics parameters (docked and handheld entries, with inheritance: +0x38 of the entry, getter
      main+0xc632b0; +0x30 is the scene render size, 1600x900 docked). main+0xc63064 reads it for the current mode and gives it
      to the window crop and to the renderer's display slot 0 (main+0xc631e0 -> main+0xc6322c, renderer +0x388); the canvas the
      game composes the frame in is that slot times the display scale (main+0x93b9c4, scale 1.0), and the final copy to the
      swapchain draws that size. It is read from the loader's data page instead (+32, {w, h}), and the renderer the routine
      works on is stored next to it (+40) so the host can ask for a new layout (the getter has no side effects):
        main+0xc630a4  mov  x0, x20               -> adrp x21, <data page>
        main+0xc630a8  bl   main+0xc632b0         -> str  x19, [x21, #40]   (x19 = the renderer)
        main+0xc630ac  ldr  x21, [x0]             -> ldr  x21, [x21, #32]   (the output size)
   Switching while playing (key 3, runtime_output_res_toggle): main+0x1185358 (called from main+0x1185310, an event handler
   the renderer registers by name hash at main+0xec86c8; also main+0xc5fd00) re-runs main+0xc63064 when the operation mode
   changed or bit 0x200 of the renderer's +0xca4 is set, then clears the bit and flags the displays for rebuilding (+0xca0
   bit 0) -- the path a Switch takes when it is docked or undocked. The host writes the other size to the data page, sets
   that bit and posts the "operation mode changed" notification (oe message 30; the mode itself stays docked) so that the
   handler runs; the game lays out its crop, canvas and UI again at the new size. At 1920x1080 the picture fills the
   swapchain's top-left (the crop says so) and the present scales it to the window (MetalFX when the window is bigger: the
   2K output). The scene keeps its render size either way; its final upscale (MetalFX on the Mac, shader a3a570eb) writes
   the canvas.
   TOTK_OUTPUT_RES: auto (the Mac default) = a 2560x1440 swapchain and heaps so that key 3 can switch at any time, starting at
   1920x1080 (the user's choice, 2026-10-17: "default 1080p"; needs the HeapDef overlay built by make romfs-patches; without
   it the game's own 1920x1080, no switching); <w>x<h> = that largest size, starting there (TOTK_OUTPUT_RES=2560x1440 starts
   in true 2K); 0 = the game's own 1920x1080, no switching. iOS and the iOS export (TOTK_EXPORT_IOS) keep the game's own size. Decided once;
   every caller sees the same answer. Mac only. */
static int output_full_at_start = 1;
static int output_res_auto(unsigned *w, unsigned *h) {
#if TARGET_OS_OSX
    if (getenv("TOTK_EXPORT_IOS")) return 0;
    const char *dir = getenv("TOTK_ROMFS_PATCHES_OUT"); char path[4096];
    snprintf(path, sizeof path, "%s/2560x1440/manifest.tsv", dir ? dir : "extracted/" TOTK_GAME_DIR "/romfs-patches-out");
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[totk] true 2K (TOTK_OUTPUT_RES=auto): off, %s is missing (make romfs-patches)\n", path); return 0; }
    fclose(f);
    *w = 2560; *h = 1440; output_full_at_start = 0;
    fprintf(stderr, "[totk] true 2K (TOTK_OUTPUT_RES=auto): starts at 1920x1080 (the default); key 3 switches between 1920x1080 and 2560x1440\n");
    return 1;
#else
    (void)w; (void)h; return 0;
#endif
}
static int output_res(unsigned *w, unsigned *h) {
    static int decided, on; static unsigned cw, ch;
    if (!decided) {
        const char *e = getenv("TOTK_OUTPUT_RES");
        /* iOS shows 720p only (the user, 2026-10-18), auto or not; the iOS export (TOTK_EXPORT_IOS) makes the phone's
           embedded code on the Mac, so it follows the same rule. */
        if (TARGET_OS_IPHONE || getenv("TOTK_EXPORT_IOS")) {
            on = 0;
            if (e && *e && strcmp(e, "0") && strcmp(e, "auto")) fprintf(stderr, "[totk] TOTK_OUTPUT_RES=%s ignored: iOS shows 720p only\n", e);
        } else if (e && !strcmp(e, "0")) on = 0;
        else if (!e || !*e || !strcmp(e, "auto")) on = output_res_auto(&cw, &ch);
        else if (sscanf(e, "%ux%u", &cw, &ch) != 2 || cw < 640 || ch < 360 || cw > 7680 || ch > 4320) runtime_fail("TOTK_OUTPUT_RES must be <width>x<height> (640x360 .. 7680x4320), auto or 0");
        else on = 1;
        decided = 1;
    }
    if (on) { *w = cw; *h = ch; }
    return on;
}
int runtime_output_res(unsigned *w, unsigned *h) { return !CODE_PATCHED_AT_BUILD && output_res(w, h); }
/* The canvas-sized render targets come from agl::utl::DynamicTextureAllocator (initialised once, main+0x8e1a98): a single
   heap of the HeapDef size "DynamicTextureAllocatorMem" (113664 KiB), carved from the "Graphics" heap (201728 KiB, the heap
   of gsys::SystemTask), with no parent to grow from. At 2560x1440 the title runs out of it (the allocator main+0x798934
   then dereferences the null block, main+0x799390). The HeapDef overlay for the output size (scripts/make_output_res_overlay.py,
   loaded by runtime/romfs.c) scales DynamicTextureAllocatorMem with the output pixels, rounded up to whole MiB, and grows
   Graphics by the same amount; the application pool grows by it too (rounded up to 2 MiB), so no other heap shrinks. */
uint64_t runtime_output_res_extra_bytes(void) {
    unsigned w, h;
    if (!runtime_output_res(&w, &h)) return 0;
    double ratio = (double)w * h / (1600.0 * 900.0); // reserve for native 3D at the maximum selectable canvas, too
    if (ratio <= 1.0) return 0;
    uint64_t kib = (uint64_t)ceil(113664.0 * ratio / 1024.0) * 1024 - 113664;
    return kib << 10;
}
enum { DP_OUTPUT_SIZE = 32, DP_RENDERER = 40 };   /* data page: the output size {w, h} the game reads; the renderer it stores */
static _Atomic(uint8_t *) output_page;
static unsigned output_w, output_h;   /* the largest output size (the swapchain) */
static uint64_t pack_size(unsigned w, unsigned h) { return (uint64_t)h << 32 | w; }
uint64_t runtime_output_size_current(void) {
    uint8_t *page = atomic_load(&output_page);
    return page ? atomic_load((_Atomic uint64_t *)(void *)(page + DP_OUTPUT_SIZE)) : pack_size(1920, 1080);
}
/* Two-state output-size helper retained for internal use: switch between the largest size and 1920x1080. The renderer
   lays out again at the next frame (see patch_output_res). */
int runtime_output_res_toggle(void) {
    if (!output_page) { fprintf(stderr, "[totk] key 3: switching the output size needs a 2K-capable start (TOTK_OUTPUT_RES auto with the HeapDef overlay, or <w>x<h>)\n"); return 0; }
    _Atomic uint64_t *size = (_Atomic uint64_t *)(void *)(output_page + DP_OUTPUT_SIZE);
    int full = atomic_load(size) != pack_size(output_w, output_h);
    atomic_store(size, full ? pack_size(output_w, output_h) : pack_size(1920, 1080));
    uint64_t renderer = atomic_load((_Atomic uint64_t *)(void *)(output_page + DP_RENDERER));
    const char *before = "";
    if (renderer) {
        uint32_t old = __atomic_fetch_or((uint32_t *)(uintptr_t)(renderer + 0xca4), 0x200u, __ATOMIC_SEQ_CST);
        before = old & 0x200u ? " (the previous switch was still pending)" : "";
        runtime_oe_push_message(30);   /* nn::oe "operation mode changed": the game runs its display handler (main+0x1185310) */
    }
    fprintf(stderr, "[totk] key 3: the game's output %ux%u (%s)%s%s\n", full ? output_w : 1920u, full ? output_h : 1080u,
            full ? "true 2K: the canvas and the UI at that size" : "the 2K output: 1080p canvas, MetalFX scales it at present",
            renderer ? " from the next layout" : "; the game has not laid out its displays yet, it takes this size when it does", before);
    return 1;
}
/* A single request contains canvas w/h, scene w/h and the upscale preset.
   The UI never rewrites guest text or touches renderer memory. Present latches
   it into the already-patched data page before the next layout, like key 3. */
int runtime_render_configure(unsigned w,unsigned h,int quality) {
    uint64_t scene=totk_control_render(w,h,quality);if(!scene)return 0;
    uint64_t request=w|((uint64_t)h<<12)|((scene&4095)<<24)|(((scene>>32)&4095)<<36)|((uint64_t)quality<<48);
    atomic_store(&render_control_request,request);return 1;
}
uint64_t runtime_render_applied(void){return atomic_load(&render_control_applied);}
static void render_control_latch(void) {
    uint64_t request=atomic_load(&render_control_request);
    if(!request||request==atomic_load(&render_control_applied)||!render_control_page||!output_page)return;
    unsigned w=request&4095,h=(request>>12)&4095,rw=(request>>24)&4095,rh=(request>>36)&4095;
    if(w>output_w||h>output_h){fprintf(stderr,"[totk-controls] target %ux%u exceeds allocated canvas %ux%u; request declined\n",w,h,output_w,output_h);return;}
    float f[2]={(float)rw,(float)rh};uint64_t bits;memcpy(&bits,f,8);atomic_store(render_control_page,bits);
    atomic_store((_Atomic uint64_t *)(void *)(output_page+DP_OUTPUT_SIZE),pack_size(w,h));
    uint64_t renderer=atomic_load((_Atomic uint64_t *)(void *)(output_page+DP_RENDERER));
    if(renderer){__atomic_fetch_or((uint32_t *)(uintptr_t)(renderer+0xca4),0x200u,__ATOMIC_SEQ_CST);runtime_oe_push_message(30);}
    extern void dm_gfx_set_flag(const char *,int);
    int up=(request>>48)!=0;dm_gfx_set_flag("mfx_upscale",up);dm_gfx_set_flag("mfx_output",0);
    atomic_store(&render_control_applied,request);
    fprintf(stderr,"[totk-controls] applied scene=%ux%u canvas=%ux%u MetalFX=%d (guest layout requested)\n",rw,rh,w,h,up);
}
/* Key 3: cycle the active 2K presentation path. The normal startup is 1080p game output with the MetalFX output scaler
   enabled; native 2K switches the game's canvas/UI to 2560x1440; off returns to 1080p and disables the output scaler. */
int runtime_output_mode_cycle(void) {
    if (!output_page) {
        fprintf(stderr, "[totk] key 3: 2K mode cycling needs a 2K-capable start (TOTK_OUTPUT_RES=auto with the HeapDef overlay, or <w>x<h>)\n");
        return 0;
    }
    _Atomic uint64_t *size = (_Atomic uint64_t *)(void *)(output_page + DP_OUTPUT_SIZE);
    int full = atomic_load(size) == pack_size(output_w, output_h);
    extern int mfxup_output_enabled(void);
    extern void dm_gfx_set_flag(const char *name, int value);
    int next_full, next_mfx;
    const char *name;
    if (full) { next_full = 0; next_mfx = 0; name = "关闭 2K"; }
    else if (mfxup_output_enabled()) { next_full = 1; next_mfx = 0; name = "原生 2K"; }
    else { next_full = 0; next_mfx = 1; name = "MetalFX 2K"; }
    if (full != next_full) runtime_output_res_toggle();
    dm_gfx_set_flag("mfx_output", next_mfx);
    dm_gfx_set_flag("mfx_upscale", next_mfx);
    fprintf(stderr, "[totk] key 3: %s\n", name);
    return 1;
}



/* TOTK_PCHTXT=<file>: an emulator ExeFS patch for main in pchtxt form, as community mods ship them (for example
   TOTK-Ultrawide's aspect-ratio patch), for trying them here. Mac only: the phone's code is patched at build. Its
   @nsobid must be main's build id, read from the extracted ExeFS (extracted/<version>/exefs/main at 0x40); the lines
   between @enabled and @stop are "<main address> <bytes>" (offset_shift 0x100 is the NSO header only the IPS form
   counts). It is applied before this runtime's own patches, whose checks of the original words then refuse a file that
   overlaps them. */
static void patch_pchtxt(Module *m) {
    const char *path = getenv("TOTK_PCHTXT");
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) runtime_fail("TOTK_PCHTXT: cannot open the file");
    char nso[256], want[41] = {0}; uint8_t id[20];
    snprintf(nso, sizeof nso, "extracted/%s/exefs/main", TOTK_GAME_DIR);
    FILE *n = fopen(nso, "rb");
    if (!n || fseek(n, 0x40, SEEK_SET) || fread(id, 1, sizeof id, n) != sizeof id) runtime_fail("TOTK_PCHTXT: cannot read main's build id from extracted/<version>/exefs/main");
    fclose(n);
    for (int i = 0; i < 20; i++) snprintf(want + 2 * i, 3, "%02X", id[i]);
    char line[1024]; int enabled = 0, matched = 0; unsigned count = 0, bytes = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "@nsobid-", 8)) {
            if (strncasecmp(line + 8, want, 40)) runtime_fail("TOTK_PCHTXT is for another build of main");
            matched = 1; continue;
        }
        if (!strncmp(line, "@enabled", 8)) { enabled = 1; continue; }
        if (!strncmp(line, "@stop", 5)) break;
        unsigned long long at; char hex[512];
        if (!enabled || line[0] == '@' || sscanf(line, "%llx %511s", &at, hex) != 2) continue;
        size_t len = strlen(hex);
        if (!matched) runtime_fail("TOTK_PCHTXT: patches before its @nsobid line");
        if (len % 2 || at + len / 2 > m->file_size) runtime_fail("TOTK_PCHTXT: a patch line is malformed or outside main");
        for (size_t i = 0; i < len / 2; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); m->base[at + i] = (uint8_t)v; }
        count++; bytes += (unsigned)(len / 2);
    }
    fclose(f);
    if (!matched) runtime_fail("TOTK_PCHTXT has no @nsobid line");
    fprintf(stderr, "[totk] TOTK_PCHTXT %s: %u patches, %u bytes in main (build id %s)\n", path, count, bytes, want);
}

/* Frame rate (1.4.0). The game holds itself at 30 FPS: once a frame, main+0x7a3e70 (a callback of the frame function
   main+0xb7bd90, registered at main+0xb5f15c) takes a target frame time of 33.333 ms (main+0x13c94dc, read only there),
   raises it to the GPU's recent frame time when that is longer, and sleeps (svcSleepThread) whatever is left of it after
   this frame's work. Game time does not count frames: main+0x79f320 advances it by the measured frame time (ticks x 0.001,
   at most 0.35 s), so a shorter target only makes more frames. The two loads of the target read the data page after main
   instead (+60, a float in ms), which the settings panel's frame limit writes (runtime_frame_target_set):
     main+0x7a3e9c  adrp x12, 0x13c9000         -> adrp x12, <data page>   (x12 is not read again before it is rewritten)
     main+0x7a3ea8  ldr  s0, [x12, #0x4dc]      -> ldr  s0, [x12, #60]
     main+0x7a3f18  ldr  s2, [x12, #0x4dc]      -> ldr  s2, [x12, #60]
   The window's present interval the game still asks for (2) is not followed while the panel paces the frames
   (nvn_control_pacing). */
#define FRAME_TARGET_SLOT 60
static _Atomic(uint32_t) *g_frame_target;
static float g_frame_target_pending = -1;   /* asked before main was loaded: written at install */
int runtime_frame_target_set(float ms) {
    if (!(ms >= 0 && ms <= 100)) ms = 1000.0f / 30;
    if (!g_frame_target) { g_frame_target_pending = ms; return 0; }
    uint32_t bits; memcpy(&bits, &ms, 4);
    if (atomic_exchange(g_frame_target, bits) != bits) fprintf(stderr, "[totk] frame target %.3f ms (%s)\n", ms, ms > 0 ? "the game sleeps up to it" : "no sleep");
    return 1;
}
static void frame_target_install(Module *m, uint64_t data_off) {
#if TARGET_OS_IPHONE
    (void)m; (void)data_off;
#else
    static const uint64_t at[3] = {0x7a3e9c, 0x7a3ea8, 0x7a3f18};
    static const uint32_t want[3] = {0xd000612c, 0xbd44dd80, 0xbd44dd82};
    for (int i = 0; i < 3; i++) { uint32_t w; memcpy(&w, m->base + at[i], 4); if (w != want[i]) { fprintf(stderr, "[totk] frame target: main+%#llx is %08x, not %08x; the game keeps its 30 FPS\n", (unsigned long long)at[i], w, want[i]); return; } }
    if (data_off + FRAME_TARGET_SLOT + 4 > m->size) runtime_fail("frame target: no data page after main");
    int64_t pages = (int64_t)(data_off >> 12) - (int64_t)(at[0] >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("frame target: data page out of ADRP range");
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    uint32_t word[3] = {0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | 12,
                        0xbd400000u | (FRAME_TARGET_SLOT / 4) << 10 | 12 << 5 | 0,
                        0xbd400000u | (FRAME_TARGET_SLOT / 4) << 10 | 12 << 5 | 2};
    g_frame_target = (_Atomic(uint32_t) *)(void *)(m->base + data_off + FRAME_TARGET_SLOT);
    float ms = 1000.0f / 30; uint32_t bits; memcpy(&bits, &ms, 4); atomic_store(g_frame_target, bits);
    for (int i = 0; i < 3; i++) memcpy(m->base + at[i], &word[i], 4);
    if (g_frame_target_pending >= 0) runtime_frame_target_set(g_frame_target_pending);
    fprintf(stderr, "[totk] 1.4.0 frame rate: the game's 30 FPS target follows the settings' frame limit\n");
#endif
}

void runtime_game_patches(Module *m) {
    /* The player's own patches, if any, then this runtime's (1.4.0: picture shape, frame rate). (The copied Tears of the Kingdom body after
       this, 60 FPS and real-time step at that game's addresses, was never reached and is gone, after PR #218.) */
    patch_pchtxt(m);
    /* Other picture shapes (aspect_patch.h): in 1.4.0 every engine camera that builds a perspective matrix (main+0x6aab4,
       0x6f14c, 0x787c4 ... : fovy at +0x40, the matrix from +0x44) asks for its aspect ratio with a virtual call (vtable
       +0x60; near and far at +0x50 / +0x58). Eleven camera classes share one getter, main+0x67be0, which divides the
       screen's width by its height (fdiv s0, s0, s1 at main+0x67c24); that division is the site. No ratio is stored
       anywhere in memory, so it cannot be found by its value: the getter came from the cameras' vtables. */
    if (!strcmp(TOTK_GAME_VERSION, "1.4.0")) {
        static const uint64_t sites[] = {0x67c24};
        aspect_install(m, "1.4.0", sites, 1, (m->text_end + 15) & ~15ULL, (m->file_size + 0x3fff) & ~0x3fffULL);
        frame_target_install(m, (m->file_size + 0x3fff) & ~0x3fffULL);
    } else if (aspect_requested()) runtime_fail("TOTK_ASPECT: Luigi's Mansion 3 1.4.0 only so far");
}
