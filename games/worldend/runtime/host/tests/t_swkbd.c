// swkbd unit test (takes seconds): lay out a ShowKeyboardArg by hand following the SDK layout, have swkbd_show write back the String, and all expected bytes are computed by hand.
// Build and run: cd dm/host/tests && clang -O1 -I.. -o /tmp/t_swkbd t_swkbd.c ../hle_swkbd.c && /tmp/t_swkbd
#include "../hle.h"
#include <stdio.h>

Result swkbd_show(void *out, const void *arg, int (*input)(const char *, const char *, const char *, const char *, int, char *, int));
int front_text_input(const char *t, const char *s, const char *g, const char *i, int m, char *o, int c) { (void)t; (void)s; (void)g; (void)i; (void)m; (void)o; (void)c; return -1; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); printf(__VA_ARGS__); printf("\n"); } } while (0)

// Input stub: record the arguments received and return as configured
static const char *g_reply; static int g_ret, g_called; static char g_title[256], g_init[256]; static int g_max;
static int stub(const char *t, const char *s, const char *g, const char *i, int m, char *o, int c) {
    (void)s; (void)g; g_called++; snprintf(g_title, sizeof g_title, "%s", t); snprintf(g_init, sizeof g_init, "%s", i); g_max = m;
    if (g_ret > 0) snprintf(o, c, "%s", g_reply);
    return g_ret;
}

static uint8_t arg[0x500], work[0x1000], sbuf[0x7d4];
static struct { uint8_t *p; uint64_t n; } str = {sbuf, sizeof sbuf};
static void mkarg(int utf8, int maxlen, const uint16_t *init, int ilen) {
    memset(arg, 0, sizeof arg); memset(work, 0, sizeof work); memset(sbuf, 0xAA, sizeof sbuf);
    static const uint16_t title[] = {0x8D77, 0x540D, 0};                 // U+8D77 U+540D, the game's "Name your character" title
    memcpy(arg + 0x24, title, sizeof title);
    memcpy(arg + 0x3ac, &maxlen, 4); arg[0x3bd] = (uint8_t)utf8;
    int off = 0x14; memcpy(arg + 0x3c0, &off, 4); memcpy(arg + 0x3c4, &ilen, 4);   // the way SetInitialTextUtf8 writes it
    memcpy(work + off, init, (size_t)ilen * 2);
    uint8_t *w = work; memcpy(arg + 0x4d0, &w, 8);
    g_called = 0;
}
static int bytes_eq(const uint8_t *want, size_t n) {   // the first n bytes equal the expectation, and everything after to the end of the buffer is 0
    if (memcmp(sbuf, want, n)) return 0;
    for (size_t i = n; i < sizeof sbuf; i++) if (sbuf[i]) return 0;
    return 1;
}

void guest_clock_freeze_for(int reason, int on) { (void)reason; (void)on; }   // the real implementation is in hle_os.c; the unit test does not care about the clock
int main(void) {
    unsetenv("DM_KBD");
    static const uint16_t hero[] = {'H', 'e', 'r', 'o'};

    printf("1 OK, UTF-8 output (what the game actually uses)\n");
    mkarg(1, 16, hero, 4); g_ret = 1; g_reply = "Ünï\xE5\x90\x8D";   // reply with a CJK character (U+540D, UTF-8 escaped)
    Result r = swkbd_show(&str, arg, stub);
    static const uint8_t w1[] = {0xC3, 0x9C, 0x6E, 0xC3, 0xAF, 0xE5, 0x90, 0x8D, 0x00};   // U+00DC n U+00EF U+540D + terminating 0
    CHECK(r == 0, "Result %#x expected 0", r);
    CHECK(bytes_eq(w1, sizeof w1), "UTF-8 write-back bytes differ");
    CHECK(!strcmp(g_title, "\xE8\xB5\xB7\xE5\x90\x8D"), "title decoded as '%s', expected U+8D77 U+540D", g_title);
    CHECK(!strcmp(g_init, "Hero"), "initial text decoded as '%s', expected Hero", g_init);
    CHECK(g_max == 16, "limit %d expected 16", g_max);

    printf("2 cancel → 0x29F, buffer untouched\n");
    mkarg(1, 16, hero, 4); g_ret = 0;
    r = swkbd_show(&str, arg, stub);
    CHECK(r == 0x29F, "Result %#x expected 0x29F", r);
    CHECK(sbuf[0] == 0xAA && sbuf[sizeof sbuf - 1] == 0xAA, "cancel changed the buffer");

    printf("3 no window (input returns -1) → treated as cancel 0x29F\n");
    mkarg(1, 16, hero, 4); g_ret = -1;
    CHECK(swkbd_show(&str, arg, stub) == 0x29F, "no window did not return cancel");

    printf("4 UTF-16 output (isUseUtf8 = 0)\n");
    mkarg(0, 16, hero, 0); g_ret = 1; g_reply = "A\xE5\x90\x8D";   // 'A' + U+540D (UTF-8 escaped)
    r = swkbd_show(&str, arg, stub);
    static const uint8_t w4[] = {0x41, 0x00, 0x0D, 0x54, 0x00, 0x00};   // 'A' U+540D terminating 0 (little endian)
    CHECK(r == 0 && bytes_eq(w4, sizeof w4), "UTF-16 write-back bytes differ");
    CHECK(g_init[0] == 0, "initial length 0 should give no initial text");

    printf("5 maximum length truncation (3 characters)\n");
    mkarg(1, 3, hero, 4); g_ret = 1; g_reply = "ABCDE";
    swkbd_show(&str, arg, stub);
    static const uint8_t w5[] = {'A', 'B', 'C', 0};
    CHECK(bytes_eq(w5, sizeof w5), "truncation result differs");

    printf("6 truncation does not split a surrogate pair (limit 2 units, a + U+1F600 needs 3 units)\n");
    mkarg(1, 2, hero, 4); g_ret = 1; g_reply = "a\xF0\x9F\x98\x80";
    swkbd_show(&str, arg, stub);
    static const uint8_t w6[] = {'a', 0};
    CHECK(bytes_eq(w6, sizeof w6), "surrogate pair split or not truncated");

    printf("7 DM_KBD gives the text directly: input box not called\n");
    setenv("DM_KBD", "Kbd", 1);
    mkarg(1, 16, hero, 4); g_ret = 0;
    r = swkbd_show(&str, arg, stub);
    static const uint8_t w7[] = {'K', 'b', 'd', 0};
    CHECK(r == 0 && bytes_eq(w7, sizeof w7) && g_called == 0, "DM_KBD path differs (input box called %d times)", g_called);
    unsetenv("DM_KBD");

    printf("%s: %d/%d\n", fails ? "FAILED" : "all passed", checks - fails, checks);
    return fails != 0;
}
