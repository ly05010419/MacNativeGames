/* Ultrawide pictures on the Mac: key 4 switches between the game's 16:9 and a wider aspect ratio (TOTK_ASPECT, default
   21:9; also 32:9, 3440x1440 or 2.39). TOTK_ASPECT set also starts the game with it on. Not on iPhone / iPad.
   The game keeps drawing into its 16:9 canvas, but with the camera's aspect ratio set to the wider one, so the picture
   holds the wider view squeezed horizontally; the present pass then stretches it over the window (nvn_mtl.m
   present_stretch asks runtime_aspect_now), which restores the proportions on a screen of that shape.
   The community's TOTK-Ultrawide mod for emulators (GPL; none of its code is used here) changes the camera parameter
   DefaultAspect (1.777778 in main's read-only data) and a viewport routine's width / height division. DefaultAspect is
   only copied when a camera is made, so it cannot follow a key; instead each place that hands an aspect ratio to the
   perspective projection (sead's +0xb0, found with a watchpoint on the copies) is patched where the value is produced:
   the instruction branches to a stub in main's text padding that runs it and, while a ratio sits on the data page after
   main (+56, 0 = off), puts that instead. The stub keeps its scratch register on the stack, so no liveness is assumed.
   Not adjusted yet: the HUD, drawn into the same 16:9 canvas, is stretched with the picture; HUD markers that follow
   objects in the world and pixel-snapped positions (5 more routines TOTK-Ultrawide widens in 1.2.1) sit off. */
#pragma once
#include "runtime.h"
#include <TargetConditionals.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASPECT_DATA_SLOT 56   /* the ratio's float on the data page after main; 0.0 = the game's own */

static _Atomic(uint32_t) *g_aspect_slot;   /* points into that page once the stubs are in */
static float g_aspect_wide;               /* the ratio key 4 switches to */

/* TOTK_ASPECT as a ratio; 0 when unset or "0". A malformed value stops the start. */
static inline float aspect_requested(void) {
    const char *e = getenv("TOTK_ASPECT");
    if (!e || !*e || !strcmp(e, "0")) return 0;
    double a = 0, w, h; char rest;
    if ((sscanf(e, "%lf:%lf%c", &w, &h, &rest) == 2 || sscanf(e, "%lfx%lf%c", &w, &h, &rest) == 2) && w > 0 && h > 0) a = w / h;
    else if (sscanf(e, "%lf%c", &w, &rest) == 1) a = w;
    if (!(a >= 0.5 && a <= 8)) runtime_fail("TOTK_ASPECT must be <w>:<h>, <w>x<h> or a ratio between 0.5 and 8");
    return (float)a;
}

/* The ratio in force (0 = the game's own 16:9): nvn_mtl.m stretches the picture over the window while it is set. */
float runtime_aspect_now(void) {
    if (!g_aspect_slot) return 0;
    uint32_t bits = atomic_load(g_aspect_slot); float v; memcpy(&v, &bits, 4); return v;
}

/* Key 4 (runtime/input.c). Returns 0 where it is not available (iPhone / iPad, other game versions). */
int runtime_aspect_toggle(void) {
    if (!g_aspect_slot) return 0;
    float next = runtime_aspect_now() ? 0 : g_aspect_wide; uint32_t bits; memcpy(&bits, &next, 4);
    atomic_store(g_aspect_slot, bits);
    fprintf(stderr, "[totk] key 4: aspect ratio %s\n", next ? "wide" : "16:9 (the game's own)");
    if (next) fprintf(stderr, "[totk] aspect ratio %.4f\n", next);
    return 1;
}

static inline uint32_t aspect_branch(uint64_t from, uint64_t to) {
    int64_t d = (int64_t)to - (int64_t)from;
    if ((d & 3) || d < -(1 << 27) || d >= (1 << 27)) runtime_fail("aspect patch: branch out of range");
    return 0x14000000u | ((uint32_t)(d >> 2) & 0x3ffffff);
}

#define ASPECT_STUB 32   /* bytes per patched instruction */

/* sites: instructions that leave the ratio in an s register (fdiv s<d>, ... or ldr s<d>, [x<n>, #imm]). Stubs go to
   main's text padding from stub_off, checked to be zero and inside the last text page; data_off is the page the loader
   reserves after main. Mac only. */
static inline void aspect_install(Module *m, const char *version, const uint64_t *sites, unsigned nsite, uint64_t stub_off, uint64_t data_off) {
#if TARGET_OS_IPHONE
    (void)m; (void)version; (void)sites; (void)nsite; (void)stub_off; (void)data_off;
#else
    float start = aspect_requested();
    g_aspect_wide = start ? start : 21.0f / 9.0f;
    uint64_t page_end = (m->text_end + 0x3fff) & ~0x3fffULL;
    if (stub_off < m->text_end || stub_off + ASPECT_STUB * nsite > page_end) runtime_fail("aspect patch: no room in main's text padding");
    if (data_off + ASPECT_DATA_SLOT + 4 > m->size) runtime_fail("aspect patch: no data page after main");
    for (unsigned i = 0; i < ASPECT_STUB * nsite; i++) if (m->base[stub_off + i]) runtime_fail("aspect patch: text padding in use");
    for (unsigned i = 0; i < nsite; i++) {
        uint32_t ins; memcpy(&ins, m->base + sites[i], 4);
        if ((ins & 0xffe0fc00) != 0x1e201800 && (ins & 0xffc00000) != 0xbd400000)
            runtime_fail("aspect patch: the camera code does not match this game version");
        uint64_t s = stub_off + ASPECT_STUB * i;
        int64_t pages = (int64_t)(data_off >> 12) - (int64_t)((s + 8) >> 12);   /* the module base is page aligned */
        if (pages < -(1 << 20) || pages >= (1 << 20)) runtime_fail("aspect patch: data page out of ADRP range");
        uint32_t imm = (uint32_t)pages & 0x1fffff;
        uint32_t stub[8] = {
            ins,                                                              /* the game's own ratio into s<d> */
            0xf81f0fea,                                                       /* str x10, [sp, #-16]! */
            0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | 10,         /* adrp x10, <data page> */
            0xb9400000u | ((uint32_t)(ASPECT_DATA_SLOT / 4) << 10) | (10 << 5) | 10, /* ldr w10, [x10, #slot] */
            0x3400004au,                                                      /* cbz w10, +8: off, keep it */
            0x1e270000u | (10 << 5) | (ins & 31),                             /* fmov s<d>, w10: the wide ratio */
            0xf84107ea,                                                       /* ldr x10, [sp], #16 */
            aspect_branch(s + 28, sites[i] + 4)};                             /* b <after the instruction> */
        memcpy(m->base + s, stub, sizeof stub);
        uint32_t jump = aspect_branch(sites[i], s);
        memcpy(m->base + sites[i], &jump, 4);
    }
    g_aspect_slot = (_Atomic(uint32_t) *)(void *)(m->base + data_off + ASPECT_DATA_SLOT);
    uint32_t bits = 0; if (start) memcpy(&bits, &start, 4);
    atomic_store(g_aspect_slot, bits);
    fprintf(stderr, "[totk] %s ultrawide: key 4 switches 16:9 / %.4f (TOTK_ASPECT), starts %s; %u camera site(s), stubs +%#llx\n",
            version, g_aspect_wide, start ? "wide" : "at 16:9", nsite, (unsigned long long)stub_off);
#endif
}
