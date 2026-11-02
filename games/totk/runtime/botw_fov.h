/* Breath of the Wild 1.9.0's field of view (the settings panel's effects page, graphics-fov.txt, TOTK_FOV_SCALE; Mac
   only), without the runtime, so tests/test_botw_fov.c can check it.
   The main camera's update (main+0x10eda5c, the camera singleton's views at +0x28 / +0x118) loads the camera's vertical
   FOV (radians, [x20, #0x74]) at main+0x10edad8 and passes it to sead's PerspectiveProjection::setFovy (main+0xeb4898:
   fovy at +0xa0, the sine, cosine and tangent of its half at +0xa4..+0xac, so every value it derives agrees). The load
   becomes a branch to a stub in main's text padding that keeps the game's value on the data page after main (+0x4c, for
   the log) and reads the factor there (+0x48) once. Then fov_result: a factor of exactly 1.0 (1x, "100", the reset)
   skips the arithmetic, so setFovy gets the game's word bit for bit, whatever it holds (a NaN, an infinity, more than
   2.875 rad); any other factor gives fmin(FOV x factor, 2.875 rad = 164.7 degrees) in single precision: a NaN stays a
   NaN, +inf and products over 2.875 become 2.875, the rest is the product. It returns to the setFovy call. x8 and s2
   are free there (neither is an argument of setFovy, which writes w8 itself; the code after the call sets both before
   reading them, with or without the ultrawide writer at main+0x10edae0..0x10edb10, which this leaves alone); the flags
   are not touched. The game's zooms scale with it when they go
   through the camera's FOV, as on 1.4.3. Nothing is written unless every check holds; a mismatch leaves the game's code
   and the field of view as shipped. Not tried in the game yet: the picture, the live change, zooms, cutscenes, the
   edges of the culling frustum at 1.5x. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FOV_SITE 0x10edad8u
#define FOV_BACK 0x10edadcu         /* bl main+0xeb4898: setFovy */
#define FOV_STUB_SKIP 32u           /* the stub's place in the padding after the text, past 32 bytes left free */
#define FOV_STUB_WORDS 11
#define FOV_CAP 2.875f            /* rad: fmov s2, #2.875 */
#define FOV_FACTOR_SLOT 0x48u       /* the data page: 1.9.0 uses +0 (the frame step) and +0x20 (the aspect) */
#define FOV_GAME_SLOT 0x4cu
#define FOV_GUARDS 5
#define FOV_MIN 0.5
#define FOV_MAX 2.0

/* The words checked before anything is written: the camera update around the load, the call and setFovy's store. */
static const struct { uint32_t off, original; } fov_guard[FOV_GUARDS] = {
    {0x10edad0u, 0x39006276u},   /* strb w22, [x19, #0x18] */
    {0x10edad4u, 0xbd00ca60u},   /* str s0, [x19, #0xc8] */
    {0x10edad8u, 0xbd407680u},   /* ldr s0, [x20, #0x74]: the camera's vertical FOV */
    {0x10edadcu, 0x97f71b6fu},   /* bl main+0xeb4898 */
    {0xeb48acu, 0xbd00a000u},    /* str s0, [x0, #0xa0]: setFovy keeps its argument */
};

static inline uint32_t fov_branch(uint64_t site, uint64_t target) {   /* b <target>; 0 outside the range */
    int64_t delta = (int64_t)target - (int64_t)site;
    if ((delta & 3) || delta < -(1 << 27) || delta >= (1 << 27)) return 0;
    return 0x14000000u | ((uint32_t)(delta >> 2) & 0x3ffffffu);
}
static inline uint32_t fov_adrp(unsigned rd, uint64_t pc, uint64_t target) {   /* adrp <rd>, <target>; 0 outside */
    int64_t pages = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    if (pages < -(1 << 20) || pages >= (1 << 20)) return 0;
    uint32_t imm = (uint32_t)pages & 0x1fffffu;
    return 0x90000000u | ((imm & 3u) << 29) | ((imm >> 2) << 5) | (rd & 31u);
}
static inline uint64_t fov_stub_at(uint64_t text_end) { return ((text_end + 15) & ~15ULL) + FOV_STUB_SKIP; }

/* The stub at main+stub (base: where main is mapped, for the adrp; data: the data page's offset in main). 0 = a branch
   or the adrp is out of range. */
static inline int fov_stub_words(uint32_t w[FOV_STUB_WORDS], uint64_t base, uint64_t stub, uint64_t data) {
    w[0] = fov_guard[2].original;                          /* ldr s0, [x20, #0x74]: the game's FOV */
    w[1] = fov_adrp(8, base + stub + 4, base + data);      /* adrp x8, <data page> */
    w[2] = 0xbd000100u | ((FOV_GAME_SLOT / 4) << 10);      /* str s0, [x8, #0x4c]: kept for the log */
    w[3] = 0xb9400108u | ((FOV_FACTOR_SLOT / 4) << 10);    /* ldr w8, [x8, #0x48]: the factor, read once */
    w[4] = 0x1e270102u;                                    /* fmov s2, w8 */
    w[5] = 0x52091908u;                                    /* eor w8, w8, #0x3f800000: 0 when it is 1.0 */
    w[6] = 0x34000088u;                                    /* cbz w8, w[10]: 1x, the game's word as it is */
    w[7] = 0x1e220800u;                                    /* fmul s0, s0, s2 */
    w[8] = 0x1e20f002u;                                    /* fmov s2, #2.875 */
    w[9] = 0x1e225800u;                                    /* fmin s0, s0, s2: a NaN stays a NaN */
    w[10] = fov_branch(stub + 40, FOV_BACK);               /* b main+0x10edadc: the setFovy call */
    return w[1] && w[10];
}
/* What the stub hands setFovy for the game's FOV g and the factor's word k (the log's "computed" degrees). */
static inline float fov_result(float g, float k) {
    if (k == 1.0f) return g;
    float p = g * k;
    return p != p || p < FOV_CAP ? p : FOV_CAP;
}

/* The factor as the stub reads it: finite and within 0.5..2, or nothing is stored (0 would make the FOV 0). */
static inline int fov_factor_bits(double k, uint32_t *bits) {
    if (!isfinite(k) || k < FOV_MIN || k > FOV_MAX) return 0;
    float f = (float)k; memcpy(bits, &f, 4); return 1;
}
static inline int fov_number(const char *text, double *value) {   /* a whole word, blanks around it dropped */
    char *end; double d = strtod(text, &end);
    if (end == text) return 0;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end || !isfinite(d)) return 0;
    *value = d; return 1;
}
/* graphics-fov.txt: hundredths of the game's FOV, 50..200 (the panel saves 80..150); -1 not valid. */
static inline int fov_parse_file(const char *text) {
    double d; if (!fov_number(text, &d) || d < 50 || d > 200) return -1;
    return (int)lround(d);
}
/* TOTK_FOV_SCALE: the factor, 0.5..2; 0 not valid. */
static inline double fov_parse_env(const char *text) {
    double d; return fov_number(text, &d) && d >= FOV_MIN && d <= FOV_MAX ? d : 0;
}

/* Install: the five words are checked first, then the stub's padding (zero, inside the 4 KiB page after the text), the
   data page and its two slots (zero), the ranges and the factor. Only when all hold are the factor, the stub and, last,
   the branch written. 0 = done; 1-5 = that guard is not as shipped (its word in *found); -1 = no room; -2 = the factor
   is not 0.5..2. Nothing is written unless 0 is returned. */
static inline int fov_apply(uint8_t *main, uint64_t base, uint64_t text_end, uint64_t data, uint64_t size, double k,
                            uint32_t *found) {
    uint32_t bits;
    if (!fov_factor_bits(k, &bits)) return -2;
    for (unsigned i = 0; i < FOV_GUARDS; i++) {
        uint32_t have; memcpy(&have, main + fov_guard[i].off, 4);
        if (have != fov_guard[i].original) { if (found) *found = have; return (int)i + 1; }
    }
    const uint64_t stub = fov_stub_at(text_end);
    if (stub + 4 * FOV_STUB_WORDS > ((text_end + 0xfff) & ~0xfffULL) || data & 0xfff || data + 0x50 > size) return -1;
    for (unsigned i = 0; i < 4 * FOV_STUB_WORDS; i++) if (main[stub + i]) return -1;
    for (unsigned i = FOV_FACTOR_SLOT; i < FOV_GAME_SLOT + 4; i++) if (main[data + i]) return -1;
    uint32_t w[FOV_STUB_WORDS], jump = fov_branch(FOV_SITE, stub);
    if (!jump || !fov_stub_words(w, base, stub, data)) return -1;
    memcpy(main + data + FOV_FACTOR_SLOT, &bits, 4);   /* first: no frame may read a factor of 0 */
    memcpy(main + stub, w, sizeof w);
    memcpy(main + FOV_SITE, &jump, 4);
    return 0;
}
