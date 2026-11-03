/* The arithmetic and machine code of aspect_patch.h, without the runtime, so tests/test_aspect.c can check it: TOTK_ASPECT
   parsing, key 4's steps, the Vert+ factor for ratios below 16:9, and the stubs put behind the camera sites. The UI
   squeeze that goes with them is the graphics layer's (gfx/nvn/totk_ui_squeeze.h).
   Vert+: the game keeps its vertical field of view and the aspect ratio decides how much is seen at the sides, so a ratio
   below 16:9 (a MacBook's 16:10) would show less at the sides. 1.4.3 inlines sead's PerspectiveProjection::setFovy in
   both camera sites: the camera's fovy is loaded, stored at +0xa0, and the sine, cosine and tangent of half of it go to
   +0xa4, +0xa8 and +0xac, just before the aspect ratio (+0xb0) that the sites patch. The Vert+ stub sits on that fovy
   load and widens it to 2 atan(k tan(fovy / 2)), k = (16/9) / ratio, with the game's own tanf and atanf, so the 16:9
   horizontal view stays, more is seen above and below, and every value sead derives (fovy, sine, cosine, tangent) agrees.
   Opt-in (TOTK_ASPECT_VERT=1) until its known issues are fixed: the TODO(Vert+) in aspect_patch.h. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ASPECT_GAME (16.0 / 9.0)
#define ASPECT_DATA_SLOT 56   /* the ratio's float on the data page after main; 0.0 = the game's own */
#define ASPECT_VERT_SLOT 60   /* the Vert+ factor beside it; 0.0 = none */
#define ASPECT_TANF_SLOT 64   /* the game's tanf and atanf (8 bytes each), for the Vert+ stubs */
#define ASPECT_ATANF_SLOT 72
#define ASPECT_STUB 32        /* bytes per plain stub */
#define ASPECT_VSTUB 304      /* bytes per Vert+ stub (74 words, padded) */

enum { ASPECT_BAD = -1 };

/* TOTK_ASPECT as a ratio: 0 when unset or "0", ASPECT_BAD when it is not <w>:<h>, <w>x<h> or a ratio between 0.5 and 8
   ("display", the window's shape, was dropped with fitting the window, user 2026-10-25). */
static inline double aspect_parse(const char *e) {
    if (!e || !*e || !strcmp(e, "0")) return 0;
    double a = 0, w, h; char rest;
    if ((sscanf(e, "%lf:%lf%c", &w, &h, &rest) == 2 || sscanf(e, "%lfx%lf%c", &w, &h, &rest) == 2) && w > 0 && h > 0) a = w / h;
    else if (sscanf(e, "%lf%c", &w, &rest) == 1) a = w;
    return a >= 0.5 && a <= 8 ? a : ASPECT_BAD;
}

/* Is ratio a within 1% of b. */
static inline int aspect_near(double a, double b) { return a > 0 && b > 0 && a / b > 0.99 && a / b < 1.01; }

/* The ratio to put in for a wanted one: 0 (the game's own, no stretch) within 1% of 16:9 or outside 0.5..8. */
static inline float aspect_effective(double a) {
    if (!(a >= 0.5 && a <= 8) || aspect_near(a, ASPECT_GAME)) return 0;
    return (float)a;
}

/* The steps: the game's 16:9, the screen's shape (the phone), wide (the Mac settings panel's ratio, TOTK_ASPECT's, or 21:9). */
#define ASPECT_WIDE (21.0 / 9.0)
enum { ASPECT_STEP_GAME, ASPECT_STEP_FIT, ASPECT_STEP_WIDE };

/* The ratio a step wants (0 = the game's own); aspect_effective turns it into the one put in. */
static inline double aspect_step_ratio(int step, double view, double wide) {
    return step == ASPECT_STEP_FIT ? view : step == ASPECT_STEP_WIDE ? wide : 0;
}

/* Vert+ is opt-in: TOTK_ASPECT_VERT=1 only (otherwise the vertical view is kept and a little less is seen at the sides). */
static inline int aspect_vert_wanted(const char *e) { return e && !strcmp(e, "1"); }

/* The Vert+ factor for ratio a (the half-angle tangent is multiplied by it): (16/9) / a below 16:9, 0 (none) otherwise. */
static inline float aspect_vert_factor(float a) {
    return a > 0 && a < ASPECT_GAME ? (float)(ASPECT_GAME / a) : 0;
}

/* b from -> to, or 0 when out of range. */
static inline uint32_t aspect_b(uint64_t from, uint64_t to) {
    int64_t d = (int64_t)to - (int64_t)from;
    if ((d & 3) || d < -(1 << 27) || d >= (1 << 27)) return 0;
    return 0x14000000u | ((uint32_t)(d >> 2) & 0x3ffffff);
}
/* adrp x<r> at `at` for the page of `to` (offsets from a page aligned base), or 0 when out of range. */
static inline uint32_t aspect_adrp(unsigned r, uint64_t at, uint64_t to) {
    int64_t pages = (int64_t)(to >> 12) - (int64_t)(at >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) return 0;
    uint32_t imm = (uint32_t)pages & 0x1fffff;
    return 0x90000000u | ((imm & 3) << 29) | ((imm >> 2) << 5) | r;
}

/* Is ins one a site may hold: fdiv s<d>, ... or ldr s<d>, [x<n>, #imm]. */
static inline int aspect_site_ok(uint32_t ins) {
    return (ins & 0xffe0fc00) == 0x1e201800 || (ins & 0xffc00000) == 0xbd400000;
}
static inline int aspect_str_s(uint32_t w, unsigned off) { return (w & 0xffc00000) == 0xbd000000 && ((w >> 10) & 0xfff) * 4 == off; }

/* The projection register of a camera site: words[0..7] are site-16 .. site+12, the site at words[4]. 1.4.3:
   str s0, [x<p>, #0xac] (the tangent) shortly before, str s<d>, [x<p>, #0xb0] (the ratio the site loads) shortly after.
   -1 when the pattern is not there or the register collides with the stub's own. */
static inline int aspect_vert_reg(const uint32_t words[8]) {
    unsigned d = words[4] & 31;
    if (!aspect_site_ok(words[4]) || (d >= 16 && d <= 18)) return -1;
    for (int a = 5; a < 8; a++) {
        if (!aspect_str_s(words[a], 0xb0) || (words[a] & 31) != d) continue;
        unsigned p = (words[a] >> 5) & 31;
        if (p == 10 || p == 11 || p == 31) return -1;
        for (int b = 0; b < 4; b++) if (aspect_str_s(words[b], 0xac) && ((words[b] >> 5) & 31) == p) return (int)p;
    }
    return -1;
}

/* The camera's fovy load for a camera site whose projection is x<p>: words[0..15] are site-64 .. site-4. 1.4.3:
   ldr s<f>, [x<c>, #imm] (the camera's fovy), fmul (half of it), str s<f>, [x<p>, #0xa0]. The index of the load, or -1. */
static inline int aspect_fovy_at(const uint32_t words[16], unsigned p) {
    for (int j = 0; j + 2 < 16; j++) {
        uint32_t f = words[j] & 31;
        if ((words[j] & 0xffc00000) == 0xbd400000 && (words[j + 1] & 0xff20fc00) == 0x1e200800 && ((words[j + 1] >> 5) & 31) == f &&
            aspect_str_s(words[j + 2], 0xa0) && (words[j + 2] & 31) == f && ((words[j + 2] >> 5) & 31) == p) return j;
    }
    return -1;
}

/* The plain stub (8 words) at offset s for the site's instruction ins: run it and, while a ratio sits in the data page's
   slot, put that instead. It keeps its scratch register on the stack, so no liveness is assumed. NULL or what failed. */
static inline const char *aspect_stub(uint32_t ins, uint64_t s, uint64_t site, uint64_t data_off, uint32_t out[8]) {
    uint32_t adrp = aspect_adrp(10, s + 8, data_off), back = aspect_b(s + 28, site + 4);
    if (!adrp) return "aspect patch: data page out of ADRP range";
    if (!back) return "aspect patch: branch out of range";
    uint32_t stub[8] = {
        ins,                                                              /* the game's own ratio into s<d> */
        0xf81f0fea,                                                       /* str x10, [sp, #-16]! */
        adrp,                                                             /* adrp x10, <data page> */
        0xb9400000u | ((uint32_t)(ASPECT_DATA_SLOT / 4) << 10) | (10 << 5) | 10, /* ldr w10, [x10, #slot] */
        0x3400004au,                                                      /* cbz w10, +8: off, keep it */
        0x1e270000u | (10 << 5) | (ins & 31),                             /* fmov s<d>, w10: the ratio */
        0xf84107ea,                                                       /* ldr x10, [sp], #16 */
        back};                                                            /* b <after the instruction> */
    memcpy(out, stub, sizeof stub);
    return NULL;
}

/* The Vert+ stub (74 words) at offset s for the fovy load ins of a camera site: run it and, while the Vert+ factor k is
   set, widen s<f> to 2 atanf(k tanf(s<f> / 2)), calling the game's tanf and atanf (data page +64, +72). The game's
   code runs on as if nothing had happened: x0-x17, x30, q0-q31 and the flags are kept on the stack (672 bytes), and
   s<f> comes back from its slot there. */
static inline const char *aspect_vstub(uint32_t ins, uint64_t s, uint64_t site, uint64_t data_off, uint32_t out[74]) {
    uint32_t adrp1 = aspect_adrp(16, s + 29 * 4, data_off), adrp2 = aspect_adrp(16, s + 37 * 4, data_off), back = aspect_b(s + 73 * 4, site + 4);
    if (!adrp1 || !adrp2) return "aspect patch: data page out of ADRP range";
    if (!back) return "aspect patch: branch out of range";
    const uint32_t QF = 160 + 16 * (ins & 31);   /* s<f>'s slot */
    uint32_t w[74], n = 0;
    w[n++] = ins;                                                            /*  0 the camera's fovy into s<f> */
    w[n++] = 0xd10a83ff;                                                     /*  1 sub sp, sp, #672 */
    for (uint32_t i = 0; i < 18; i += 2) w[n++] = 0xa9000000u | (i << 15) | ((i + 1) << 10) | (31 << 5) | i;   /* stp x<i>, x<i+1>, [sp, #8i] */
    w[n++] = 0xd53b4210;                                                     /* 11 mrs x16, nzcv */
    w[n++] = 0xa9000000u | (18u << 15) | (16 << 10) | (31 << 5) | 30;        /* 12 stp x30, x16, [sp, #144] */
    for (uint32_t i = 0; i < 32; i += 2) w[n++] = 0xad000000u | (((160 + 16 * i) / 16) << 15) | ((i + 1) << 10) | (31 << 5) | i;   /* stp q<i>, q<i+1> */
    w[n++] = adrp1;                                                          /* 29 adrp x16, <data page> */
    w[n++] = 0xb9400000u | ((ASPECT_VERT_SLOT / 4) << 10) | (16 << 5) | 17;  /* 30 ldr w17, [x16, #k] */
    w[n++] = 0x34000000u | ((45 - 31) << 5) | 17;                            /* 31 cbz w17, 45: no Vert+ */
    w[n++] = 0xbd400000u | ((QF / 4) << 10) | (31 << 5) | 0;                 /* 32 ldr s0, [sp, #s<f>] */
    w[n++] = 0x1e2c1001;                                                     /* 33 fmov s1, #0.5 */
    w[n++] = 0x1e210800;                                                     /* 34 fmul s0, s0, s1 */
    w[n++] = 0xf9400000u | ((ASPECT_TANF_SLOT / 8) << 10) | (16 << 5) | 16;  /* 35 ldr x16, [x16, #tanf] */
    w[n++] = 0xd63f0200;                                                     /* 36 blr x16 */
    w[n++] = adrp2;                                                          /* 37 adrp x16, <data page> */
    w[n++] = 0xb9400000u | ((ASPECT_VERT_SLOT / 4) << 10) | (16 << 5) | 17;  /* 38 ldr w17, [x16, #k] */
    w[n++] = 0x1e270221;                                                     /* 39 fmov s1, w17 */
    w[n++] = 0x1e210800;                                                     /* 40 fmul s0, s0, s1 */
    w[n++] = 0xf9400000u | ((ASPECT_ATANF_SLOT / 8) << 10) | (16 << 5) | 16; /* 41 ldr x16, [x16, #atanf] */
    w[n++] = 0xd63f0200;                                                     /* 42 blr x16 */
    w[n++] = 0x1e202800;                                                     /* 43 fadd s0, s0, s0 */
    w[n++] = 0xbd000000u | ((QF / 4) << 10) | (31 << 5) | 0;                 /* 44 str s0, [sp, #s<f>]: the wider fovy */
    for (uint32_t i = 0; i < 32; i += 2) w[n++] = 0xad400000u | (((160 + 16 * i) / 16) << 15) | ((i + 1) << 10) | (31 << 5) | i;   /* 45 ldp q<i>, q<i+1> */
    w[n++] = 0xa9400000u | (18u << 15) | (16 << 10) | (31 << 5) | 30;        /* 61 ldp x30, x16, [sp, #144] */
    w[n++] = 0xd51b4210;                                                     /* 62 msr nzcv, x16 */
    for (uint32_t i = 0; i < 18; i += 2) w[n++] = 0xa9400000u | (i << 15) | ((i + 1) << 10) | (31 << 5) | i;   /* ldp x<i>, x<i+1> */
    w[n++] = 0x910a83ff;                                                     /* 72 add sp, sp, #672 */
    w[n++] = back;                                                           /* 73 b <after the load> */
    if (n != 74) return "aspect patch: Vert+ stub size";
    memcpy(out, w, sizeof w);
    return NULL;
}
