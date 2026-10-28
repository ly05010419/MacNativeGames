/* Super Mario Odyssey 1.3.0 / 1.4.1 (the same executables), key 4 (runtime/input.c): 16:9 -> fitting the window / display
   -> 21:9 -> 16:9, the steps of Zelda's key 4 (aspect_math.h; TOTK_ASPECT=21:9, 32:9, 3440x1440, 2.39 or display starts on
   that step). The game keeps drawing into its 16:9 canvas with the camera's aspect ratio set to the other one; the present
   stretches the picture over the window (nvn_mtl.m present_stretch asks runtime_aspect_now) and the HUD is squeezed back
   to 16:9 (uw_ui_squeeze).

   Odyssey has no code patch for it. Every camera projection of the game is a sead::PerspectiveProjection, vtable
   main+0x13540f8, with the layout Breath of the Wild 1.9.0 has (games/totk game_patches_botw.c): dirty +8, near +0x98, far
   +0x9c, fovy +0xa0, sin, cos and tan of half the fovy, aspect +0xb0. Found by reading the guest heap for that layout
   (2026-10-22, Wooded Kingdom: 95 such objects, two of them 40 deg / 16:9 / near 25 / far 500000 as the camera block of
   the frame says; 21:9 written into both widened the picture). So while key 4 is on, the ratio is written into
   every projection of that class that holds 16:9 and the projection is marked dirty, at every present: the game puts
   16:9 back into one of them every frame. Off puts 16:9 back where this code wrote.

   A thread finds the objects: it copies the guest heap through the kernel (mach_vm_read_overwrite, so a page the game
   makes inaccessible meanwhile is an error, not a crash; reading such a page directly stopped the game once) and looks
   for the vtable with the fovy signature behind it (a 1 MiB copy that fails is read again page by page; skipping it
   whole missed the camera once). It runs when key 4 leaves 16:9, when a found object stops looking like a projection,
   and every 10 s while key 4 is on (a new scene makes new cameras); about 1.5 s of one core per run, off the game
   thread. runtime_aspect_now answers the ratio only while a found projection holds it, so after a scene change the
   picture is not stretched until the new scene's cameras are found (about 1.5 s; the old ones fail the check at once).
   A camera made later inside the same scene is found by the next periodic run, up to 10 s, and drawn at 16:9 inside
   the stretched picture until then. A check of the frame's camera block (vertex uniform 5) cannot tell: its rows hold
   the view as well as the projection (the ratio read from them was 0.87 at 16:9, 2026-10-22). */
#include "runtime.h"
#include "aspect_math.h"
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ODYSSEY_PROJ_VTABLE 0x13540f8   /* sead::PerspectiveProjection's vtable, main+ (1.3.0 and 1.4.1) */
#define ODYSSEY_PROJ_MAX 128
#define ODYSSEY_RESCAN_NS 10000000000ull

static const Module *g_aspect_main;
static float g_wide;                       /* key 4's wide step: TOTK_ASPECT's ratio, or 21:9 */
static int g_named;                        /* TOTK_ASPECT gave it */
static atomic_int g_step;                  /* ASPECT_STEP_GAME, _FIT or _WIDE */
static _Atomic(uint32_t) g_view;           /* the window's width / height (frontend.m), float bits */
static _Atomic(uint32_t) g_want;           /* the ratio key 4 wants now, float bits (0 = the game's own) */
static _Atomic(uint32_t) g_shown;          /* the ratio a found camera holds (runtime_aspect_now), float bits */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static uint8_t *g_found[ODYSSEY_PROJ_MAX];     /* the scanner's list (g_lock) */
static int g_nfound, g_scan_req, g_scans;
static uint8_t *g_mine[ODYSSEY_PROJ_MAX];      /* present thread: objects that hold a ratio this code wrote */
static int g_nmine;
static float g_mine_ratio;

static float bits_float(uint32_t b) { float v; memcpy(&v, &b, 4); return v; }
static uint32_t float_bits(float v) { uint32_t b; memcpy(&b, &v, 4); return b; }

/* The object's first 0xb8 bytes through the kernel: 0 when they cannot be read. */
static int proj_read(const uint8_t *o, uint8_t out[0xb8]) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)o, 0xb8, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == 0xb8;
}
/* A sead::PerspectiveProjection of this game: the vtable, and a fovy with the sin, cos and tan of its half. */
static int proj_valid(const uint8_t *b) {
    uint64_t vt; memcpy(&vt, b, 8);
    if (!g_aspect_main || vt != (uint64_t)(uintptr_t)(g_aspect_main->base + ODYSSEY_PROJ_VTABLE)) return 0;
    float f[4]; memcpy(f, b + 0xa0, sizeof f);
    if (!(f[0] > 0.1f && f[0] < 3.0f)) return 0;
    float h = f[0] * 0.5f;
    return fabsf(f[1] - sinf(h)) < 1e-5f && fabsf(f[2] - cosf(h)) < 1e-5f && fabsf(f[3] - tanf(h)) < 1e-4f;
}

/* The scanner: the heap in 1 MiB copies (overlapping by the object size, so none is cut at a seam). */
static void scan_heap(void) {
    extern void runtime_heap_range(uint8_t **base, size_t *size);
    uint8_t *heap; size_t size; runtime_heap_range(&heap, &size);
    uint8_t *found[ODYSSEY_PROJ_MAX]; int n = 0;
    enum { CHUNK = 1 << 20, OBJ = 0xb8 };
    static uint8_t buf[CHUNK + OBJ];
    uint64_t want_vt = (uint64_t)(uintptr_t)(g_aspect_main->base + ODYSSEY_PROJ_VTABLE);
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), unread = 0;
    for (size_t at = 0; heap && at < size && n < ODYSSEY_PROJ_MAX; at += CHUNK) {
        size_t len = size - at < CHUNK + OBJ ? size - at : CHUNK + OBJ;
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)(heap + at), len, (mach_vm_address_t)(uintptr_t)buf, &got) != KERN_SUCCESS || got != len) {
            /* a page in it cannot be read (the game keeps some inaccessible): page by page, unreadable ones as zeros
               (no vtable there). Skipping the whole MiB missed the camera on the first try. */
            for (size_t pg = 0; pg < len; pg += 0x4000) {
                size_t l = len - pg < 0x4000 ? len - pg : 0x4000;
                got = 0;
                if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)(heap + at + pg), l, (mach_vm_address_t)(uintptr_t)(buf + pg), &got) != KERN_SUCCESS || got != l) {
                    memset(buf + pg, 0, l);
                    if (pg < CHUNK) unread += l;
                }
            }
        }
        for (size_t i = 0; i + OBJ <= len && i < CHUNK && n < ODYSSEY_PROJ_MAX; i += 8) {
            uint64_t v; memcpy(&v, buf + i, 8);
            if (v != want_vt || !proj_valid(buf + i)) continue;
            float a; memcpy(&a, buf + i + 0xb0, 4);   // only the 16:9 views (and the ones holding the wanted ratio)
            if (a == (float)ASPECT_GAME || a == bits_float(atomic_load(&g_want))) found[n++] = heap + at + i;
        }
    }
    pthread_mutex_lock(&g_lock);
    memcpy(g_found, found, sizeof found); g_nfound = n; g_scans++;
    int scans = g_scans;
    pthread_mutex_unlock(&g_lock);
    if (scans <= 3 || scans % 60 == 0) {
        fprintf(stderr, "[odyssey] key 4: %d 16:9 camera projections found (scan %d, %.0f ms, %llu KiB not readable)\n", n, scans,
                (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6, (unsigned long long)(unread >> 10));
        for (int i = 0; i < n && scans <= 3; i++) {
            uint8_t b[0xb8]; float f[3]; if (!proj_read(found[i], b)) continue;
            memcpy(f, b + 0x98, sizeof f);
            fprintf(stderr, "[odyssey] key 4:   heap+%#lx near %g far %g fovy %.2f deg dirty %u\n", (unsigned long)(found[i] - heap), f[0], f[1], f[2] * 57.29578f, b[8]);
        }
    }
}
static void *scanner(void *arg) {
    (void)arg;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        while (!g_scan_req && !bits_float(atomic_load(&g_want))) pthread_cond_wait(&g_wake, &g_lock);
        if (!g_scan_req) {   // on, nothing asked: wait out the rescan period
            struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += ODYSSEY_RESCAN_NS / 1000000000ull;
            pthread_cond_timedwait(&g_wake, &g_lock, &t);
            if (!bits_float(atomic_load(&g_want))) continue;
        }
        g_scan_req = 0;
        pthread_mutex_unlock(&g_lock);
        scan_heap();
        pthread_mutex_lock(&g_lock);
    }
    return NULL;
}
static void scanner_start(void) { pthread_t t; if (!pthread_create(&t, NULL, scanner, NULL)) pthread_detach(t); }
static void scan_request(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, scanner_start);
    pthread_mutex_lock(&g_lock); g_scan_req = 1; pthread_cond_signal(&g_wake); pthread_mutex_unlock(&g_lock);
}

/* Present thread (runtime_frame_step_update): put the wanted ratio into the found projections that hold 16:9 (or the
   ratio written before), or 16:9 back into the ones this code wrote. */
static void aspect_present(void) {
    float want = bits_float(atomic_load(&g_want));
    uint8_t snap[0xb8];
    if (!want) {
        for (int i = 0; i < g_nmine; i++) {
            float a; if (!proj_read(g_mine[i], snap) || !proj_valid(snap)) continue;
            memcpy(&a, snap + 0xb0, 4);
            if (a == g_mine_ratio) { float g = (float)ASPECT_GAME; memcpy(g_mine[i] + 0xb0, &g, 4); g_mine[i][8] = 1; }
        }
        g_nmine = 0; atomic_store(&g_shown, 0);
        return;
    }
    uint8_t *list[ODYSSEY_PROJ_MAX]; int n = 0;
    if (pthread_mutex_trylock(&g_lock) == 0) { n = g_nfound; memcpy(list, g_found, sizeof list); pthread_mutex_unlock(&g_lock); }
    else return;   // the scanner is publishing; the next present writes
    int held = 0, gone = 0, nmine = 0;
    for (int i = 0; i < n; i++) {
        float a;
        if (!proj_read(list[i], snap) || !proj_valid(snap)) { gone++; continue; }
        memcpy(&a, snap + 0xb0, 4);
        if (a == want) held++;
        else if (a == (float)ASPECT_GAME || (g_mine_ratio && a == g_mine_ratio)) {
            memcpy(list[i] + 0xb0, &want, 4); list[i][8] = 1; held++;
        } else continue;   // another ratio (4:3, 1:1): not the game's 16:9 view, left as it is
        if (nmine < ODYSSEY_PROJ_MAX) g_mine[nmine++] = list[i];
    }
    g_nmine = nmine; g_mine_ratio = want;
    atomic_store(&g_shown, held ? float_bits(want) : 0);
    if (gone) scan_request();
}

/* Puts the ratio of key 4's step in force; logs a change. */
static void aspect_apply(void) {
    double r = aspect_step_ratio(atomic_load(&g_step), bits_float(atomic_load(&g_view)), g_wide);
    float a = aspect_effective(r);
    uint32_t old = atomic_exchange(&g_want, float_bits(a));
    if (old == float_bits(a)) return;
    if (a) { fprintf(stderr, "[odyssey] aspect ratio %.4f\n", a); scan_request(); }
    else fprintf(stderr, "[odyssey] aspect ratio 16:9 (the game's own)\n");
}

/* game_patches.c: the main module, and TOTK_ASPECT. Not on iPhone / iPad (no window to fit, no wide display). */
void odyssey_aspect_install(const Module *main_module) {
    if (TARGET_OS_IPHONE) return;
    double a = aspect_parse(getenv("TOTK_ASPECT"));
    if (a == ASPECT_BAD) runtime_fail("TOTK_ASPECT must be <w>:<h>, <w>x<h>, a ratio between 0.5 and 8, or display");
    g_aspect_main = main_module;
    g_named = a > 0;
    g_wide = a > 0 ? (float)a : (float)ASPECT_WIDE;
    atomic_store(&g_step, a == ASPECT_DISPLAY ? ASPECT_STEP_FIT : a > 0 ? ASPECT_STEP_WIDE : ASPECT_STEP_GAME);
    fprintf(stderr, "[odyssey] key 4: 16:9 -> fit to the window -> %s %.4f -> 16:9 (camera projections, vtable main+%#x)\n",
            g_named ? "TOTK_ASPECT" : "21:9", g_wide, ODYSSEY_PROJ_VTABLE);
    aspect_apply();
}
void odyssey_aspect_present(void) { if (g_aspect_main) aspect_present(); }

/* The ratio in force (0 = the game's own 16:9): only once a camera holds it, so the picture is never stretched over a
   16:9 view. */
float runtime_aspect_now(void) { return bits_float(atomic_load(&g_shown)); }
/* The panel (frontend.m): key 4's step, -1 where key 4 is not available. */
int runtime_aspect_step(void) { return g_aspect_main ? atomic_load(&g_step) : -1; }
/* frontend.m: the window's drawable is w x h pixels (on every change). While key 4 fits the window, the view follows. */
void runtime_aspect_view(double w, double h) {
    if (!(w > 0 && h > 0)) return;
    atomic_store(&g_view, float_bits((float)(w / h)));
    if (g_aspect_main && atomic_load(&g_step) == ASPECT_STEP_FIT) aspect_apply();
}
int runtime_aspect_toggle(void) {
    if (!g_aspect_main) return 0;
    int step = aspect_next_step(atomic_load(&g_step), bits_float(atomic_load(&g_view)), g_wide);
    atomic_store(&g_step, step);
    aspect_apply();
    float a = bits_float(atomic_load(&g_want));
    const char *name = step == ASPECT_STEP_FIT ? "fit to the window / display" : step == ASPECT_STEP_WIDE ? g_named ? "TOTK_ASPECT" : "21:9" : "16:9";
    if (step == ASPECT_STEP_GAME) fprintf(stderr, "[odyssey] key 4: 16:9 (the game's own)\n");
    else if (a) fprintf(stderr, "[odyssey] key 4: %s, aspect ratio %.4f\n", name, a);
    else fprintf(stderr, "[odyssey] key 4: %s, which is 16:9 here\n", name);
    return 1;
}
