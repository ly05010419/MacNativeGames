/* Breath of the Wild 1.9.0's sun shadow map at 2048 per cascade (TOTK_SHADOW_RES=2048 or the settings panel's
   shadows.txt; Mac only, off by default), without the runtime, so tests/test_botw_shadow_res.c can check it.
   The sun's shadow is a set of cascades whose size the game's cascade controller picks from a five-level table it
   writes at its start (main+0x124ee70, the table at +0x44, one 16-bit size per level): 480, 720, 720, 896 and 2048.
   The first four come from one 64-bit constant built in x8 by four moves, main+0x124eef8..0x124ef04; the fifth (2048,
   the ShadowMap's declared maximum, "Min=32,Max=2048") is stored apart. Docked play uses level 3: a 896x896 map in
   three Depth16Unorm layers (4.59 MiB) and a half-size R16Unorm copy (1.31 MiB). With the four moves all set to 2048
   the map is 2048x2048 (24 MiB) and its copy 1024x1024 (6 MiB): about 24 MiB more from the render-target heap.
   Seen on a Mac (2026-10-27, Great Plateau, with a 2560x1440 scene): sharper shadow edges and the light through leaves
   in small, defined spots instead of large blurred ones; no acne or banding seen. Not measured: how much of that heap
   is free (a failed reservation is not logged), and the shader's filter constant that goes with the size.
   The table is written once, at the start, so the choice holds for the whole run: the panel's row saves the next one. */
#pragma once
#include <ctype.h>
#include <stdint.h>
#include <string.h>

#define SHADOW_RES_SIZE 2048u
#define SHADOW_RES_SITES 4
enum { SHADOW_FROM_NONE, SHADOW_FROM_ENV, SHADOW_FROM_FILE };

/* The four moves as shipped: movz x8, #480 / movk x8, #720, lsl #16 / movk x8, #720, lsl #32 / movk x8, #896, lsl #48. */
static const struct { uint32_t off, original; } shadow_res_site[SHADOW_RES_SITES] = {
    {0x124eef8u, 0xd2803c08u}, {0x124eefcu, 0xf2a05a08u}, {0x124ef00u, 0xf2c05a08u}, {0x124ef04u, 0xf2e07008u},
};

/* The same move with 2048: movz for the first, movk for the others, each at its own 16-bit position. */
static inline uint32_t shadow_res_word(unsigned i) {
    return (i ? 0xf2800000u : 0xd2800000u) | (i << 21) | (SHADOW_RES_SIZE << 5) | 8u;
}

/* One value: 2048 on, 0 the game's own, -1 not valid. The variable takes 2048 or 0, shadows.txt 2048 or off; the blanks
   around a word are dropped (a file ends in a newline). */
static inline int shadow_res_parse(const char *text, int file) {
    while (*text && isspace((unsigned char)*text)) text++;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) n--;
    if (n == 4 && !strncmp(text, "2048", 4)) return (int)SHADOW_RES_SIZE;
    if (!file && n == 1 && text[0] == '0') return 0;
    if (file && n == 3 && !strncmp(text, "off", 3)) return 0;
    return -1;
}

/* What a launch applies and where it came from. TOTK_SHADOW_RES decides when it is set, even to a value that is not
   valid (then off, bad set); else shadows.txt; else off. saved is the panel's choice for the next launch. */
struct shadow_res_state { int applied, from, bad, saved; };
static inline struct shadow_res_state shadow_res_start(const char *env, const char *file) {
    struct shadow_res_state s = {0, SHADOW_FROM_NONE, 0, 0};
    int f = file ? shadow_res_parse(file, 1) : 0;
    s.saved = f > 0 ? f : 0;
    if (env) { int v = shadow_res_parse(env, 0); s.from = SHADOW_FROM_ENV; s.bad = v < 0; s.applied = v > 0 ? v : 0; }
    else if (file) { s.from = SHADOW_FROM_FILE; s.bad = f < 0; s.applied = s.saved; }
    return s;
}
/* The panel's row: only the next launch's choice changes, never what this one applied. */
static inline void shadow_res_save(struct shadow_res_state *s, int size) { s->saved = size > 0 ? (int)SHADOW_RES_SIZE : 0; }

/* Off (size 0): nothing is read or written. On: the four original words are checked first, and only when all four are
   as shipped are the four written; otherwise nothing is written and the first site that differs is returned (1-4),
   with the word found there. 0 = done. */
static inline int shadow_res_apply(uint8_t *base, int size, uint32_t *found) {
    if (size <= 0) return 0;
    for (unsigned i = 0; i < SHADOW_RES_SITES; i++) {
        uint32_t have; memcpy(&have, base + shadow_res_site[i].off, 4);
        if (have != shadow_res_site[i].original) { if (found) *found = have; return (int)i + 1; }
    }
    for (unsigned i = 0; i < SHADOW_RES_SITES; i++) { uint32_t w = shadow_res_word(i); memcpy(base + shadow_res_site[i].off, &w, 4); }
    return 0;
}
