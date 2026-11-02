/* games/totk/runtime/botw_fov.h without the game: graphics-fov.txt and TOTK_FOV_SCALE parsing, the factor's range, the
   stub's words, and the patch on a fake main built here (only the shipped words the patch checks, the ultrawide writer's
   words after them, and a zeroed text padding and data page, at 1.9.0's offsets; no game code in the repository). The
   stub is run by a small interpreter of its own eleven instructions, from the load to the setFovy call; a word that
   differs, a padding or data slot that is not empty, a missing data page or a bad factor is refused with nothing
   written. At 1x the game's word must come through bit for bit whatever it is (NaN, infinities, more than the cap);
   at other factors it must be fov_result's, the log's computed value. UW_ORIGINAL (from the test runner: game_patches_botw.c's uw_writer_original) checks that the ultrawide
   writer and this patch can go in either order. */
#include "../games/totk/runtime/botw_fov.h"
#include <stdio.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* The 1.9.0 layout of the app's main (e84d): text ends at main+0x15a1c64, the file at main+0x20a7dd4, so the data page
   is main+0x20a8000. */
#define TEXT_END 0x15a1c64u
#define FILE_SIZE 0x20a7dd4u
#define DATA 0x20a8000u
#define IMAGE (DATA + 0x4000u)
#define BASE 0x300000000ull   /* where the fake main is "mapped", for the adrp */
#define STUB 0x15a1c90u       /* the text end, 16-byte aligned, + 32 */
#define UW_AT 0x10edae0u
static const uint32_t uw_original[] = {UW_ORIGINAL};
#define UW_N (sizeof uw_original / sizeof *uw_original)

static uint8_t *fake_main(void) {
    uint8_t *m = malloc(IMAGE);
    if (!m) { printf("FAIL: no memory\n"); exit(1); }
    for (uint32_t i = 0; i < TEXT_END; i++) m[i] = (uint8_t)(i * 131u + 7u);
    memset(m + TEXT_END, 0, IMAGE - TEXT_END);
    for (unsigned i = 0; i < FOV_GUARDS; i++) memcpy(m + fov_guard[i].off, &fov_guard[i].original, 4);
    memcpy(m + UW_AT, uw_original, sizeof uw_original);
    return m;
}
static uint8_t *copy_of(const uint8_t *m) { uint8_t *c = malloc(IMAGE); if (c) memcpy(c, m, IMAGE); return c; }
static unsigned bytes_changed(const uint8_t *a, const uint8_t *b) {
    unsigned n = 0;
    for (uint32_t i = 0; i < IMAGE; i++) n += a[i] != b[i];
    return n;
}
static uint32_t word_at(const uint8_t *m, uint32_t off) { uint32_t w; memcpy(&w, m + off, 4); return w; }
static uint32_t bits_of(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static float float_of(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static void set_factor(uint8_t *m, uint32_t bits) { memcpy(m + DATA + FOV_FACTOR_SLOT, &bits, 4); }
/* FMIN with FPCR.DN clear: a NaN operand gives that NaN, quiet (a signalling one first); -0 is below +0. */
static float arm_fmin(float a, float b) {
    uint32_t x = bits_of(a), y = bits_of(b);
    if (isnan(a) || isnan(b)) {
        int sa = isnan(a) && !(x & 0x400000u), sb = isnan(b) && !(y & 0x400000u);
        return float_of((sa || (!sb && isnan(a)) ? x : y) | 0x400000u);
    }
    return a < b || (a == b && signbit(a)) ? a : b;
}

typedef struct { uint64_t x[32], sp, nzcv; float s[32]; } Regs;
/* An address: BASE + a main offset (the data page), or a host pointer (the camera object). */
static uint8_t *at(uint8_t *m, uint64_t a) { return a >= BASE && a < BASE + IMAGE ? m + (a - BASE) : (uint8_t *)(uintptr_t)a; }
/* Runs main from the load (main+0x10edad8) until it reaches the setFovy call (main+0x10edadc). Only the stub's
   instructions are known; anything else fails. Returns 0 at the call, -1 otherwise. */
static int run(uint8_t *m, Regs *r) {
    uint64_t pc = FOV_SITE;
    for (int steps = 0; steps < 16; steps++) {
        if (pc == FOV_BACK) return 0;
        uint32_t w = word_at(m, (uint32_t)pc);
        unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
        if ((w & 0xfc000000u) == 0x14000000u) { int32_t imm = (int32_t)(w << 6) >> 6; pc += (int64_t)imm * 4; continue; }   /* b */
        if ((w & 0x9f000000u) == 0x90000000u) {   /* adrp */
            int32_t imm = (int32_t)(((((w >> 5) & 0x7ffffu) << 2) | ((w >> 29) & 3u)) << 11) >> 11;
            r->x[rd] = (((BASE + pc) >> 12) + (int64_t)imm) << 12; pc += 4; continue;
        }
        if ((w & 0xff000000u) == 0x34000000u) {   /* cbz w */
            int32_t imm = (int32_t)(((w >> 5) & 0x7ffffu) << 13) >> 13;
            pc += (uint32_t)r->x[rd] ? 4 : (int64_t)imm * 4; continue;
        }
        if ((w & 0xffc00000u) == 0xb9400000u) { uint32_t v; memcpy(&v, at(m, r->x[rn] + ((w >> 10) & 0xfff) * 4), 4); r->x[rd] = v; pc += 4; continue; }   /* ldr w */
        if ((w & 0xfffffc00u) == 0x1e270000u) { r->s[rd] = float_of((uint32_t)r->x[rn]); pc += 4; continue; }   /* fmov s, w */
        if ((w & 0xff800000u) == 0x52000000u && !(w & (1u << 15))) {   /* eor w, w, #imm (one 32-bit element) */
            unsigned immr = (w >> 16) & 31, ones = ((w >> 10) & 31) + 1;
            uint32_t run = ones == 32 ? ~0u : (1u << ones) - 1, mask = immr ? run >> immr | run << (32 - immr) : run;
            r->x[rd] = (uint32_t)r->x[rn] ^ mask; pc += 4; continue;
        }
        if ((w & 0xffc00000u) == 0xbd400000u) { memcpy(&r->s[rd], at(m, r->x[rn] + ((w >> 10) & 0xfff) * 4), 4); pc += 4; continue; }   /* ldr s */
        if ((w & 0xffc00000u) == 0xbd000000u) { memcpy(at(m, r->x[rn] + ((w >> 10) & 0xfff) * 4), &r->s[rd], 4); pc += 4; continue; }   /* str s */
        if ((w & 0xffe0fc00u) == 0x1e200800u) { r->s[rd] = r->s[rn] * r->s[rm]; pc += 4; continue; }   /* fmul s */
        if ((w & 0xffe0fc00u) == 0x1e205800u) { r->s[rd] = arm_fmin(r->s[rn], r->s[rm]); pc += 4; continue; }   /* fmin s */
        if ((w & 0xffe01fe0u) == 0x1e201000u) {   /* fmov s, #imm (VFPExpandImm) */
            uint32_t i8 = (w >> 13) & 0xff, b6 = (i8 >> 6) & 1;
            uint32_t b = (i8 >> 7) << 31 | (b6 ? 0x7cu : 0x80u) << 23 | ((i8 >> 4) & 3u) << 23 | (i8 & 15u) << 19;
            memcpy(&r->s[rd], &b, 4); pc += 4; continue;
        }
        printf("unknown word %08x at main+%#llx\n", w, (unsigned long long)pc); return -1;
    }
    return -1;
}
/* The camera's FOV through the load as main has it now: s0 at the setFovy call, or NAN. Checks that only x8, s0 and s2
   changed (the rest as the camera update has them: x0 the projection, x19 the view, x20 the camera, x22 = 1, x30). */
static float through(uint8_t *m, float game) {
    static uint8_t camera[0x100];
    memcpy(camera + 0x74, &game, 4);
    Regs r; memset(&r, 0, sizeof r);
    for (unsigned i = 0; i < 31; i++) r.x[i] = 0x1111000000000000ull + i;
    for (unsigned i = 0; i < 32; i++) r.s[i] = 100.0f + (float)i;
    r.x[20] = (uintptr_t)camera; r.sp = 0x5ee0; r.nzcv = 0x60000000u;
    Regs before = r;
    if (run(m, &r)) return NAN;
    for (unsigned i = 0; i < 32; i++) {
        if (i != 8) CHECK(r.x[i] == before.x[i], "x%u kept", i);
        if (i != 0 && i != 2) CHECK(bits_of(r.s[i]) == bits_of(before.s[i]), "s%u kept", i);
    }
    CHECK(r.sp == before.sp && r.nzcv == before.nzcv, "sp and the flags kept");
    return r.s[0];
}

static void words(void) {
    CHECK(fov_guard[2].off == FOV_SITE && fov_guard[3].off == FOV_BACK, "the load, then the call");
    CHECK(fov_branch(FOV_BACK, 0xeb4898u) == (fov_guard[3].original & ~0x80000000u), "the call goes to setFovy");
    CHECK(fov_stub_at(TEXT_END) == STUB, "the stub's place");
    uint32_t w[FOV_STUB_WORDS];
    CHECK(fov_stub_words(w, BASE, STUB, DATA), "the stub builds");
    /* As the system assembler gives them (clang -c, otool -tv). */
    CHECK(w[0] == 0xbd407680u && w[2] == 0xbd004d00u && w[3] == 0xb9404908u && w[4] == 0x1e270102u, "load, keep, factor");
    CHECK(w[5] == 0x52091908u && w[6] == 0x34000088u, "eor w8, w8, #0x3f800000 and cbz w8 to the way back");
    CHECK(w[7] == 0x1e220800u && w[8] == 0x1e20f002u && w[9] == 0x1e225800u, "fmul, fmov s2, #2.875 and fmin");
    CHECK(FOV_CAP == float_of(0x40380000u), "the cap is the fmov's 2.875");
    /* The real layout's words: the adrp from stub + 4, the way back and the site's branch. */
    CHECK(w[1] == 0xf0005828u && w[10] == 0x17ed2f89u && fov_branch(FOV_SITE, STUB) == 0x1412d06eu,
          "golden words %08x %08x %08x", w[1], w[10], fov_branch(FOV_SITE, STUB));
    /* The adrp is the stub's second word: its page is that word's (a stub ending a page shows it; fov_stub_at never
       puts one there, 16-byte aligned). */
    CHECK(fov_stub_words(w, BASE, 0x15a1ffcu, DATA) && w[1] == fov_adrp(8, BASE + 0x15a2000u, BASE + DATA) &&
          w[1] != fov_adrp(8, BASE + 0x15a1ffcu, BASE + DATA), "the adrp's own page");
    CHECK(fov_branch(0, 1u << 27) == 0 && fov_branch(0, 2) == 0 && fov_adrp(8, 0, 1ull << 33) == 0, "out of range: 0");
    CHECK(((FILE_SIZE + 0x3fffu) & ~0x3fffu) == DATA, "the data page follows the file");
}

static void parsing(void) {
    CHECK(fov_parse_file("100") == 100 && fov_parse_file("150\n") == 150 && fov_parse_file(" 80 ") == 80, "the panel's");
    CHECK(fov_parse_file("50") == 50 && fov_parse_file("200") == 200, "the ends");
    const char *bad[] = {"", "\n", "49", "201", "abc", "120x", "nan", "inf", "1 2"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) CHECK(fov_parse_file(bad[i]) < 0, "file \"%s\" refused", bad[i]);
    CHECK(fov_parse_env("1.5") == 1.5 && fov_parse_env("0.5") == 0.5 && fov_parse_env("2") == 2 && fov_parse_env("1") == 1, "env");
    const char *worse[] = {"", "0", "-1", "0.49", "2.01", "abc", "nan", "1.5x", "inf"};
    for (unsigned i = 0; i < sizeof worse / sizeof *worse; i++) CHECK(fov_parse_env(worse[i]) == 0, "env \"%s\" refused", worse[i]);
    uint32_t b = 0x12345678u;
    const double off[] = {0, -1, NAN, INFINITY, 3, 0.49, 2.01};
    for (unsigned i = 0; i < sizeof off / sizeof *off; i++) CHECK(!fov_factor_bits(off[i], &b) && b == 0x12345678u, "factor %g refused", off[i]);
    CHECK(fov_factor_bits(0.5, &b) && b == bits_of(0.5f) && fov_factor_bits(2, &b) && b == bits_of(2.0f), "0.5 and 2 taken");
}

static void patching(void) {
    uint8_t *m = fake_main(), *shipped = copy_of(m);
    CHECK(bits_of(through(m, 0.87f)) == bits_of(0.87f), "as shipped: the camera's FOV");
    uint32_t found = 1;
    CHECK(fov_apply(m, BASE, TEXT_END, DATA, IMAGE, 1.0, &found) == 0 && found == 1, "done");
    uint32_t w[FOV_STUB_WORDS]; fov_stub_words(w, BASE, STUB, DATA);
    CHECK(word_at(m, FOV_SITE) == 0x1412d06eu && !memcmp(m + STUB, w, sizeof w), "the branch and the stub");
    CHECK(word_at(m, DATA + FOV_FACTOR_SLOT) == bits_of(1.0f), "the factor");
    CHECK(bytes_changed(m, shipped) <= 4 + sizeof w + 4, "nothing else written (%u bytes)", bytes_changed(m, shipped));
    CHECK(!memcmp(m, shipped, FOV_SITE) && !memcmp(m + FOV_SITE + 4, shipped + FOV_SITE + 4, STUB - FOV_SITE - 4), "only there");
    /* 1x: the game's own word, bit for bit, whatever it holds, and kept for the log. */
    const uint32_t game[] = {bits_of(0.87f), bits_of(0.0001f), bits_of(1.5f), bits_of(2.875f), 0x80000000u,
                             bits_of(2.9f), bits_of(3.0f), bits_of(100.0f), 0x7f7fffffu, 0x7f800000u, 0xff800000u,
                             0x7fc00000u, 0x7fc12345u, 0xffc00001u, 0x7fa00001u, 0x00000001u, bits_of(-0.87f)};
    const unsigned games = sizeof game / sizeof *game;
    for (unsigned i = 0; i < games; i++) {
        CHECK(bits_of(through(m, float_of(game[i]))) == game[i], "1x keeps %08x", game[i]);
        CHECK(word_at(m, DATA + FOV_GAME_SLOT) == game[i], "%08x kept for the log", game[i]);
        CHECK(bits_of(fov_result(float_of(game[i]), 1.0f)) == game[i], "the log's 1x: %08x", game[i]);
    }
    set_factor(m, bits_of(1.5f));
    CHECK(through(m, 0.87f) == 0.87f * 1.5f && word_at(m, DATA + FOV_GAME_SLOT) == bits_of(0.87f), "1.5x, the game's kept");
    CHECK(through(m, 3.0f) == 2.875f && through(m, INFINITY) == 2.875f, "capped at 2.875 rad");
    CHECK(through(m, 1.9f) == 1.9f * 1.5f && through(m, -INFINITY) == -INFINITY, "under the cap: the product");
    CHECK(isnan(through(m, NAN)) && isnan(through(m, float_of(0x7fa00001u))), "a NaN stays a NaN");
    /* Any factor but 1.0 goes through the arithmetic, and the stub gives what the log computes. */
    const uint32_t factors[] = {bits_of(1.5f), bits_of(0.8f), bits_of(0.5f), bits_of(2.0f), 0x3f800001u, 0x3f7fffffu};
    for (unsigned j = 0; j < sizeof factors / sizeof *factors; j++) {
        set_factor(m, factors[j]);
        for (unsigned i = 0; i < games; i++) {
            float got = through(m, float_of(game[i])), want = fov_result(float_of(game[i]), float_of(factors[j]));
            CHECK(isnan(want) ? isnan(got) : bits_of(got) == bits_of(want), "factor %08x, game %08x: %08x, the log %08x",
                  factors[j], game[i], bits_of(got), bits_of(want));
        }
    }
    set_factor(m, 0x3f800001u);
    CHECK(through(m, 3.0f) == 2.875f, "only exactly 1.0 skips the cap");
    set_factor(m, bits_of(0.8f));
    CHECK(through(m, 0.87f) == 0.87f * 0.8f, "0.8x");
    found = 0;
    CHECK(fov_apply(m, BASE, TEXT_END, DATA, IMAGE, 1.0, &found) == 3 && found == 0x1412d06eu, "a second pass refuses");
    set_factor(m, bits_of(1.0f));
    CHECK(bits_of(through(m, 3.0f)) == bits_of(3.0f), "back to 1x: the game's word again");
    uint8_t *again = copy_of(m);
    CHECK(fov_apply(m, BASE, TEXT_END, DATA, IMAGE, 1.0, NULL) == 3 && !memcmp(m, again, IMAGE), "and writes nothing");
    free(again);
    for (unsigned bad = 0; bad < FOV_GUARDS; bad++) {
        uint8_t *t = fake_main();
        uint32_t other = fov_guard[bad].original ^ 0x20u; memcpy(t + fov_guard[bad].off, &other, 4);
        uint8_t *before = copy_of(t);
        found = 0;
        int r = fov_apply(t, BASE, TEXT_END, DATA, IMAGE, 1.5, &found);
        CHECK(r == (int)bad + 1 && found == other, "guard %u differs: refused there (%d, %08x)", bad, r, found);
        CHECK(!memcmp(t, before, IMAGE), "guard %u differs: nothing written", bad);
        free(before); free(t);
    }
    const uint32_t taken[] = {STUB, STUB + 4 * FOV_STUB_WORDS - 1, DATA + FOV_FACTOR_SLOT, DATA + FOV_GAME_SLOT + 3};
    for (unsigned i = 0; i < sizeof taken / sizeof *taken; i++) {
        uint8_t *t = fake_main(); t[taken[i]] = 1;
        uint8_t *before = copy_of(t);
        CHECK(fov_apply(t, BASE, TEXT_END, DATA, IMAGE, 1.5, NULL) == -1 && !memcmp(t, before, IMAGE), "main+%#x taken", taken[i]);
        free(before); free(t);
    }
    uint8_t *t = fake_main(), *before = copy_of(t);
    CHECK(fov_apply(t, BASE, TEXT_END, DATA, DATA, 1.5, NULL) == -1 && !memcmp(t, before, IMAGE), "no data page");
    CHECK(fov_apply(t, BASE, TEXT_END, DATA + 8, IMAGE, 1.5, NULL) == -1 && !memcmp(t, before, IMAGE), "page not aligned");
    CHECK(fov_apply(t, BASE, 0x15a1fe0u, DATA, IMAGE, 1.5, NULL) == -1 && !memcmp(t, before, IMAGE), "no room");
    const double off[] = {0, -1, NAN, 3};
    for (unsigned i = 0; i < 4; i++) CHECK(fov_apply(t, BASE, TEXT_END, DATA, IMAGE, off[i], NULL) == -2 && !memcmp(t, before, IMAGE), "factor %g", off[i]);
    free(before); free(t); free(m); free(shipped);
}

/* The ultrawide writer (game_patches_botw.c botw_aspect_writer) checks its 13 words at main+0x10edae0 and rewrites them;
   this patch checks the 4 before them and setFovy. Either order gives the same main. */
static void with_ultrawide(void) {
    uint32_t marker[UW_N]; for (unsigned i = 0; i < UW_N; i++) marker[i] = 0xd503201fu;   /* stands for the rewrite */
    CHECK(UW_N == 13 && uw_original[0] == 0xbd406e80u, "the ultrawide writer's words");
    uint8_t *a = fake_main(), *b = fake_main();
    CHECK(fov_apply(a, BASE, TEXT_END, DATA, IMAGE, 1.2, NULL) == 0, "the field of view first");
    CHECK(!memcmp(a + UW_AT, uw_original, sizeof uw_original), "the ultrawide's own check still holds");
    memcpy(a + UW_AT, marker, sizeof marker);
    memcpy(b + UW_AT, marker, sizeof marker);
    CHECK(fov_apply(b, BASE, TEXT_END, DATA, IMAGE, 1.2, NULL) == 0, "the ultrawide first: the guards still hold");
    CHECK(!memcmp(a, b, IMAGE), "the same main either way");
    free(a); free(b);
}

int main(void) {
    words(); parsing(); patching(); with_ultrawide();
    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("botw fov ok\n");
    return 0;
}
