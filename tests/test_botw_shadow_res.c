/* games/totk/runtime/botw_shadow_res.h without the game: TOTK_SHADOW_RES / shadows.txt parsing and precedence, the
   choice holding for the run while the panel saves the next one, and the patch on a fake main built here (only the
   four shipped words at their offsets; no game code in the repository): off reads and writes nothing, on changes
   exactly the four words, and a word that differs at any one site is refused with nothing written. */
#include "../games/totk/runtime/botw_shadow_res.h"
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Worked out by hand from the A64 encoding (MOVZ 0xd2800000 / MOVK 0xf2800000, hw << 21, imm16 << 5, Rd = 8). */
static const uint32_t shipped[4] = {0xd2803c08u, 0xf2a05a08u, 0xf2c05a08u, 0xf2e07008u};   /* 480, 720, 720, 896 */
static const uint32_t patched[4] = {0xd2810008u, 0xf2a10008u, 0xf2c10008u, 0xf2e10008u};   /* 2048 at each */
#define IMAGE 0x1300000u   /* above the last site */

static uint8_t *fake_main(void) {
    uint8_t *m = malloc(IMAGE);
    if (!m) { printf("FAIL: no memory\n"); exit(1); }
    for (uint32_t i = 0; i < IMAGE; i++) m[i] = (uint8_t)(i * 131u + 7u);
    for (unsigned i = 0; i < 4; i++) memcpy(m + shadow_res_site[i].off, &shipped[i], 4);
    return m;
}
static unsigned words_changed(const uint8_t *a, const uint8_t *b) {
    unsigned n = 0;
    for (uint32_t i = 0; i < IMAGE; i += 4) n += memcmp(a + i, b + i, 4) != 0;
    return n;
}

static void words(void) {
    for (unsigned i = 0; i < 4; i++) {
        CHECK(shadow_res_site[i].original == shipped[i], "site %u original %08x", i, shadow_res_site[i].original);
        CHECK(shadow_res_word(i) == patched[i], "site %u word %08x, not %08x", i, shadow_res_word(i), patched[i]);
        CHECK(((shipped[i] >> 21) & 3) == i && ((patched[i] >> 21) & 3) == i, "site %u keeps its 16-bit position", i);
        CHECK((shipped[i] & 31) == 8 && (patched[i] & 31) == 8, "site %u keeps x8", i);
        CHECK(((patched[i] >> 5) & 0xffff) == 2048, "site %u moves 2048", i);
    }
    const unsigned sizes[4] = {480, 720, 720, 896};
    for (unsigned i = 0; i < 4; i++) CHECK(((shipped[i] >> 5) & 0xffff) == sizes[i], "site %u ships %u", i, sizes[i]);
    CHECK(shadow_res_site[1].off == shadow_res_site[0].off + 4 && shadow_res_site[3].off == shadow_res_site[0].off + 12, "one straight run");
}

static void parsing(void) {
    CHECK(shadow_res_parse("2048", 0) == 2048 && shadow_res_parse("0", 0) == 0, "the variable's two values");
    CHECK(shadow_res_parse("2048\n", 1) == 2048 && shadow_res_parse(" off \n", 1) == 0, "the file's two values, blanks dropped");
    const char *bad_env[] = {"", "1", "1024", "2048x", "4096", "off", "on", "-2048", "20 48"};
    for (unsigned i = 0; i < sizeof bad_env / sizeof *bad_env; i++) CHECK(shadow_res_parse(bad_env[i], 0) < 0, "variable \"%s\" refused", bad_env[i]);
    const char *bad_file[] = {"", "\n", "0", "on", "1024", "OFF", "2048 2048"};
    for (unsigned i = 0; i < sizeof bad_file / sizeof *bad_file; i++) CHECK(shadow_res_parse(bad_file[i], 1) < 0, "file \"%s\" refused", bad_file[i]);
}

static void precedence(void) {
    struct shadow_res_state s = shadow_res_start(NULL, NULL);
    CHECK(!s.applied && s.from == SHADOW_FROM_NONE && !s.bad && !s.saved, "neither: off");
    s = shadow_res_start(NULL, "2048\n");
    CHECK(s.applied == 2048 && s.from == SHADOW_FROM_FILE && !s.bad && s.saved == 2048, "the file alone");
    s = shadow_res_start(NULL, "off\n");
    CHECK(!s.applied && s.from == SHADOW_FROM_FILE && !s.bad, "the file says off");
    s = shadow_res_start(NULL, "huge\n");
    CHECK(!s.applied && s.from == SHADOW_FROM_FILE && s.bad && !s.saved, "a file that is not valid: off, said");
    s = shadow_res_start("2048", NULL);
    CHECK(s.applied == 2048 && s.from == SHADOW_FROM_ENV && !s.bad, "the variable alone");
    s = shadow_res_start("0", "2048\n");
    CHECK(!s.applied && s.from == SHADOW_FROM_ENV && s.saved == 2048, "the variable before the file (the file still the saved choice)");
    s = shadow_res_start("2048", "off\n");
    CHECK(s.applied == 2048 && s.from == SHADOW_FROM_ENV, "the variable before the file, the other way");
    s = shadow_res_start("1024", "2048\n");
    CHECK(!s.applied && s.from == SHADOW_FROM_ENV && s.bad, "a variable that is not valid still decides: off, said");
    s = shadow_res_start("", "2048\n");
    CHECK(!s.applied && s.from == SHADOW_FROM_ENV && s.bad, "an empty variable is set");
}

static void persistence(void) {
    struct shadow_res_state run = shadow_res_start(NULL, "2048\n");
    shadow_res_save(&run, 0);
    CHECK(run.applied == 2048 && run.saved == 0, "the row turned off: this run keeps 2048, the next is off");
    struct shadow_res_state next = shadow_res_start(NULL, run.saved ? "2048\n" : "off\n");
    CHECK(!next.applied, "the next launch reads off");
    shadow_res_save(&next, 2048);
    CHECK(!next.applied && next.saved == 2048, "the row turned on: this run stays as shipped");
    next = shadow_res_start(NULL, next.saved ? "2048\n" : "off\n");
    CHECK(next.applied == 2048, "the launch after reads 2048");
    shadow_res_save(&next, 1);
    CHECK(next.saved == 2048, "any on value saves 2048");
}

static void patching(void) {
    uint8_t *m = fake_main(), *copy = fake_main();
    CHECK(shadow_res_apply(NULL, 0, NULL) == 0, "off touches no memory (a null main is not read)");
    CHECK(shadow_res_apply(m, 0, NULL) == 0 && !memcmp(m, copy, IMAGE), "off: the image is the same, byte for byte");
    uint32_t found = 1;
    CHECK(shadow_res_apply(m, 2048, &found) == 0 && found == 1, "on: done");
    CHECK(words_changed(m, copy) == 4, "on: exactly four words changed, not %u", words_changed(m, copy));
    for (unsigned i = 0; i < 4; i++) {
        uint32_t w; memcpy(&w, m + shadow_res_site[i].off, 4);
        CHECK(w == patched[i], "on: site %u is %08x", i, w);
    }
    CHECK(shadow_res_apply(m, 2048, &found) == 1 && found == patched[0] && words_changed(m, copy) == 4, "a second pass refuses (already patched) and writes nothing");
    for (unsigned bad = 0; bad < 4; bad++) {
        uint8_t *t = fake_main();
        uint32_t other = shipped[bad] ^ 0x20u; memcpy(t + shadow_res_site[bad].off, &other, 4);
        uint8_t *before = malloc(IMAGE); memcpy(before, t, IMAGE);
        found = 0;
        int r = shadow_res_apply(t, 2048, &found);
        CHECK(r == (int)bad + 1 && found == other, "site %u differs: refused there (%d, %08x)", bad, r, found);
        CHECK(!memcmp(t, before, IMAGE), "site %u differs: nothing written, not even the sites before it", bad);
        free(before); free(t);
    }
    free(m); free(copy);
}

int main(void) {
    words(); parsing(); precedence(); persistence(); patching();
    if (failures) { printf("%d failures\n", failures); return 1; }
    printf("shadow res ok\n");
    return 0;
}
