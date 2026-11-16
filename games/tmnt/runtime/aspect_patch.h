/* Other picture shapes on the Mac: key 4 cycles the game's 16:9 -> fitting the window / display (a 21:9 or 32:9 monitor, a
   MacBook's 16:10 in full screen below the notch) -> 21:9 -> 16:9. Fitting is skipped while the window is within 1% of
   16:9 or 21:9, where it would show the same. TOTK_ASPECT=32:9, 16:10, 3440x1440 or 2.39 takes the place of 21:9 and
   starts on it; TOTK_ASPECT=display starts on fitting the window. Not on iPhone / iPad.
   The game keeps drawing into its 16:9 canvas, but with the camera's aspect ratio set to the other one, so the picture
   holds the other view squeezed into it; the present pass then stretches it over the window (nvn_mtl.m
   present_stretch asks runtime_aspect_now), which restores the proportions on a screen of that shape.
   The community's TOTK-Ultrawide mod for emulators (GPL; none of its code is used here) changes the camera parameter
   DefaultAspect (1.777778 in main's read-only data) and a viewport routine's width / height division. DefaultAspect is
   only copied when a camera is made, so it cannot follow a key; instead each place that hands an aspect ratio to the
   perspective projection (sead's +0xb0, found with a watchpoint on the copies) is patched where the value is produced:
   the instruction branches to a stub in main's text padding that runs it and, while a ratio sits on the data page after
   main (+56, 0 = off), puts that instead. The stub keeps its scratch register on the stack, so no liveness is assumed.
   The vertical view stays: wider than 16:9 more is seen at the sides, narrower (16:10) a little less.
   TOTK_ASPECT_VERT=1 (1.4.3 only; 1.2.1 has no room for the stubs) instead widens the camera's fovy below 16:9 so the
   sides stay as at 16:9 (Vert+, aspect_math.h; factor on the data page +60). It stays off by default until its known
   issues (the TODO at aspect_install) are fixed.
   The HUD, drawn into the same 16:9 canvas, is squeezed back by nvn_mtl.m (uw_ui_squeeze). Not adjusted yet: HUD
   markers that follow objects in the world and pixel-snapped positions (5 more routines TOTK-Ultrawide widens in 1.2.1)
   sit off. */
#pragma once
#include "runtime.h"
#include "aspect_math.h"
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Atomic(uint32_t) *g_aspect_slot;   /* points into that page once the stubs are in */
static _Atomic(uint32_t) *g_aspect_vert;   /* the Vert+ factor beside it, when Vert+ stubs are in */
static float g_aspect_wide;               /* key 4's wide step: TOTK_ASPECT's ratio, or 21:9 */
static int g_aspect_named;                /* TOTK_ASPECT gave it */
static atomic_int g_aspect_step;          /* key 4: ASPECT_STEP_GAME, _FIT or _WIDE (aspect_math.h) */
static _Atomic(uint32_t) g_aspect_view;   /* the window's width / height (frontend.m), float bits */

/* TOTK_ASPECT as a ratio (ASPECT_DISPLAY for "display", 0 when unset or "0"). A malformed value stops the start. */
static inline double aspect_requested(void) {
    double a = aspect_parse(getenv("TOTK_ASPECT"));
    if (a == ASPECT_BAD) runtime_fail("TOTK_ASPECT must be <w>:<h>, <w>x<h>, a ratio between 0.5 and 8, or display");
    return a;
}

static inline float aspect_bits_float(uint32_t bits) { float v; memcpy(&v, &bits, 4); return v; }
static inline uint32_t aspect_float_bits(float v) { uint32_t bits; memcpy(&bits, &v, 4); return bits; }

/* The ratio in force (0 = the game's own 16:9): nvn_mtl.m stretches the picture over the window while it is set. */
float runtime_aspect_now(void) { return g_aspect_slot ? aspect_bits_float(atomic_load(g_aspect_slot)) : 0; }

/* The panel (frontend.m): key 4's step, -1 where key 4 is not available; runtime_aspect_now is the ratio it shows. */
int runtime_aspect_step(void) { return g_aspect_slot ? atomic_load(&g_aspect_step) : -1; }

static const char *aspect_step_name(int step) {
    return step == ASPECT_STEP_FIT ? "fit to the window / display" : step == ASPECT_STEP_WIDE ? g_aspect_named ? "TOTK_ASPECT" : "21:9"
                                                                                             : "16:9 (the game's own)";
}

/* Puts the ratio of key 4's step (and its Vert+ factor) on the data page; logs a change. */
static void aspect_apply(void) {
    if (!g_aspect_slot) return;
    double want = aspect_step_ratio(atomic_load(&g_aspect_step), aspect_bits_float(atomic_load(&g_aspect_view)), g_aspect_wide);
    float a = aspect_effective(want), k = g_aspect_vert ? aspect_vert_factor(a) : 0;
    uint32_t old = atomic_exchange(g_aspect_slot, aspect_float_bits(a));
    if (g_aspect_vert) atomic_store(g_aspect_vert, aspect_float_bits(k));
    if (old != aspect_float_bits(a)) {
        if (a && k) fprintf(stderr, "[totk] aspect ratio %.4f, vertical view widened %.3fx (Vert+)\n", a, k);
        else if (a) fprintf(stderr, "[totk] aspect ratio %.4f\n", a);
        else fprintf(stderr, "[totk] aspect ratio 16:9 (the game's own)\n");
    }
}

/* frontend.m: the window's drawable is w x h pixels (on every change). While key 4 fits the window, the view follows. */
void runtime_aspect_view(double w, double h) {
    if (!(w > 0 && h > 0)) return;
    atomic_store(&g_aspect_view, aspect_float_bits((float)(w / h)));
    if (atomic_load(&g_aspect_step) == ASPECT_STEP_FIT) aspect_apply();
}

/* Key 4 (runtime/input.c). Returns 0 where it is not available (iPhone / iPad, other game versions). */
int runtime_aspect_toggle(void) {
    if (!g_aspect_slot) return 0;
    int step = aspect_next_step(atomic_load(&g_aspect_step), aspect_bits_float(atomic_load(&g_aspect_view)), g_aspect_wide);
    atomic_store(&g_aspect_step, step);
    aspect_apply();
    float a = runtime_aspect_now();
    if (step == ASPECT_STEP_GAME) fprintf(stderr, "[totk] key 4: 16:9 (the game's own)\n");
    else if (a) fprintf(stderr, "[totk] key 4: %s, aspect ratio %.4f%s\n", aspect_step_name(step), a,
                        step == ASPECT_STEP_FIT ? "; the view follows the window until key 4 again" : "");
    else fprintf(stderr, "[totk] key 4: %s, which is 16:9 here\n", aspect_step_name(step));
    return 1;
}

/* sites: instructions that leave the ratio in an s register (fdiv s<d>, ... or ldr s<d>, [x<n>, #imm]), each with a
   plain stub from stub_off (ASPECT_STUB bytes per site). The first ncam are camera sites: when vert_off is not 0 and
   the inlined setFovy is there, their fovy load also gets a Vert+ stub (ASPECT_VSTUB bytes each, from vert_off). Both
   areas are main's text padding, checked to be zero and inside the last text page; data_off is the page the loader
   reserves after main. */
static inline void aspect_install(Module *m, const char *version, const uint64_t *sites, unsigned nsite, unsigned ncam,
                                  uint64_t stub_off, uint64_t vert_off, uint64_t data_off) {
#if TARGET_OS_IPHONE
    (void)m; (void)version; (void)sites; (void)nsite; (void)ncam; (void)stub_off; (void)vert_off; (void)data_off;
#else
    double req = aspect_requested();
    /* TODO(Vert+): opt-in (TOTK_ASPECT_VERT=1) until these are fixed. Known issues:
       - Some UI effects (the button-press effect, for one) sit in the wrong place below 16:9: they are drawn outside
         the HUD quads nvn_mtl.m flattens (uw_ui_squeeze) and probably need the same flattening.
       - Lighting: the first Vert+ changed only the projection's half-angle tangent, sine and cosine and left fovy
         (+0xa0) at the game's value; on 1.4.3 at 16:10 (2940x1846, key 4 on) most sunlit ground lost its light and
         shadows sat wrong. Widening the camera's fovy itself before setFovy derives the rest (this stub) fixed it in
         an A/B at that spot, and the user saw correct sun lighting, shadows and horizon in full screen on a build
         with main at 9e8a514, and again on the latest build; only the UI effects above remain. Which pass read the
         stale value was never found; anything else that reads the camera's field of view (other cameras, a copy of
         fovy) has not been checked. */
    int vert = vert_off && aspect_vert_wanted(getenv("TOTK_ASPECT_VERT"));
    void *tanf_at = vert ? runtime_symbol("tanf") : NULL, *atanf_at = vert ? runtime_symbol("atanf") : NULL;
    if (vert && (!tanf_at || !atanf_at)) { fprintf(stderr, "[totk] aspect patch: the game's tanf / atanf not found, no Vert+\n"); vert = 0; }
    uint64_t page_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    if (stub_off < m->text_end || stub_off + ASPECT_STUB * nsite > page_end) runtime_fail("aspect patch: no room in main's text padding");
    if (vert && (vert_off < m->text_end || vert_off + ASPECT_VSTUB * ncam > page_end)) runtime_fail("aspect patch: no room for the Vert+ stubs");
    if (data_off + ASPECT_ATANF_SLOT + 8 > m->size) runtime_fail("aspect patch: no data page after main");
    for (unsigned i = 0; i < ASPECT_STUB * nsite; i++) if (m->base[stub_off + i]) runtime_fail("aspect patch: text padding in use");
    for (unsigned i = 0; vert && i < ASPECT_VSTUB * ncam; i++) if (m->base[vert_off + i]) runtime_fail("aspect patch: text padding for Vert+ in use");
    unsigned nvert = 0;
    for (unsigned i = 0; i < nsite; i++) {
        uint32_t ins; memcpy(&ins, m->base + sites[i], 4);
        if (!aspect_site_ok(ins)) runtime_fail("aspect patch: the camera code does not match this game version");
        uint32_t code[74] = {0};
        uint64_t s = stub_off + ASPECT_STUB * i;
        const char *err = aspect_stub(ins, s, sites[i], data_off, code);
        uint32_t jump = aspect_b(sites[i], s);
        if (err || !jump) runtime_fail(err ? err : "aspect patch: branch out of range");
        if (vert && i < ncam) {   /* Vert+: the fovy load before it, found from the projection register */
            uint32_t around[8], before[16]; memcpy(around, m->base + sites[i] - 16, sizeof around); memcpy(before, m->base + sites[i] - 64, sizeof before);
            int p = aspect_vert_reg(around), j = p >= 0 ? aspect_fovy_at(before, (unsigned)p) : -1;
            if (j < 0) fprintf(stderr, "[totk] aspect patch: camera site +%#llx is not the expected setFovy, no Vert+ there\n", (unsigned long long)sites[i]);
            else {
                uint64_t fs = sites[i] - 64 + 4 * (uint64_t)j, vs = vert_off + ASPECT_VSTUB * i;
                uint32_t vcode[74], vjump = aspect_b(fs, vs);
                const char *verr = aspect_vstub(before[j], vs, fs, data_off, vcode);
                if (verr || !vjump) runtime_fail(verr ? verr : "aspect patch: branch out of range");
                memcpy(m->base + vs, vcode, sizeof vcode);
                memcpy(m->base + fs, &vjump, 4);
                nvert++;
            }
        }
        memcpy(m->base + s, code, ASPECT_STUB);
        memcpy(m->base + sites[i], &jump, 4);
    }
    if (nvert) {
        uint64_t fa = (uint64_t)(uintptr_t)tanf_at, fb = (uint64_t)(uintptr_t)atanf_at;
        memcpy(m->base + data_off + ASPECT_TANF_SLOT, &fa, 8); memcpy(m->base + data_off + ASPECT_ATANF_SLOT, &fb, 8);
    }
    g_aspect_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + ASPECT_DATA_SLOT);
    if (nvert) g_aspect_vert = (_Atomic(uint32_t) *)(void *)(m->base + data_off + ASPECT_VERT_SLOT);
    atomic_store(g_aspect_slot, 0);
    if (g_aspect_vert) atomic_store(g_aspect_vert, 0);
    g_aspect_named = req > 0;
    g_aspect_wide = req > 0 ? (float)req : (float)ASPECT_WIDE;
    int step = req > 0 ? ASPECT_STEP_WIDE : req == ASPECT_DISPLAY ? ASPECT_STEP_FIT : ASPECT_STEP_GAME;
    atomic_store(&g_aspect_step, step);
    aspect_apply();
    char wide[48]; if (g_aspect_named) snprintf(wide, sizeof wide, "%.4f (TOTK_ASPECT)", g_aspect_wide); else snprintf(wide, sizeof wide, "21:9");
    fprintf(stderr, "[totk] %s aspect ratio: key 4 cycles 16:9 / fit to the window / %s, starts at %s; %u site(s), %s, stubs +%#llx\n",
            version, wide, aspect_step_name(step), nsite,
            nvert ? "Vert+ below 16:9 (TOTK_ASPECT_VERT=1, experimental: some UI effects sit off)" : "vertical view kept below 16:9 (TOTK_ASPECT_VERT=1: Vert+, experimental)",
            (unsigned long long)stub_off);
#endif
}
