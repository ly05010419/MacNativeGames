/* Text entry for the game (nn::swkbd::ShowKeyboard) with the Mac keyboard.

   On the Switch the call starts the software-keyboard library applet, which takes the screen until the player confirms
   or cancels. Here the player types on the Mac keyboard in a bar inside the game window (runtime_front_text_input in
   runtime/frontend.m; input methods work, Return confirms, Escape cancels). User, 2026-10-17: "no software keyboard,
   just support the user's keyboard". TOTK calls it in one place: naming a horse (main+0x1e5cb58 -> main+0x1adb720:
   user-name preset, at most 9 characters, '@' '%' '\' disabled, UTF-16 output; an empty name counts as cancelled).

   ABI of TOTK 1.0.0's SDK, read from the original sdk+0x3cbd40 (ShowKeyboard with a trigger) and the setters
   MakePreset / SetHeaderText / SetSubText / SetGuideText / SetOkText / SetInitialText (analysis/swkbd/sdk-swkbd-named.c):
   ShowKeyboardArg = KeyboardConfig (0x4d0 bytes), then the work buffer {pointer, size} at +0x4d0 / +0x4d8 (the initial
   text lives there), then the text-check and customize-dictionary buffers. String = {pointer, byte size}
   (GetRequiredStringBufferSize: 0x7d4). The result is 0, or 0x29f when the player cancels (the SDK's value for a cancel
   close result). The disabled-key bits follow the applet's: 1 space, 2 '@', 3 '%', 4 '/', 5 '\', 6 digits (7 download
   code and 8 user name are character sets the applet knows; not applied here).

   TOTK_SWKBD_ANSWER=<text> answers without showing the bar (tests; "<cancel>" cancels, "<initial>" keeps the initial text).
   make probe-swkbd builds the arguments with the guest SDK and calls through main's own PLT stub. */
#include "runtime.h"
#include "totk_ui_lang.h"
#include <TargetConditionals.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    KB_TYPE = 0x00, KB_OK_TEXT = 0x04, KB_DISABLED = 0x1c, KB_HEADER = 0x24, KB_SUB = 0xa6, KB_GUIDE = 0x1a8,
    KB_MAX_LEN = 0x3ac, KB_MIN_LEN = 0x3b0, KB_PASSWORD = 0x3b4, KB_UTF8 = 0x3bd, KB_INIT_OFF = 0x3c0, KB_INIT_LEN = 0x3c4,
    ARG_WORK = 0x4d0, ARG_WORK_SIZE = 0x4d8, ARG_SIZE = 0x500,
    KB_TYPE_NUMPAD = 1, KB_LIMIT = 500, SWKBD_CANCELED = 0x29f,
};

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static size_t u16_len(const uint16_t *s, size_t max) { size_t n = 0; while (n < max && s[n]) n++; return n; }

/* UTF-16 (n units) -> NUL-terminated UTF-8 in cap bytes, cut at a character boundary. Returns the bytes written. */
static size_t utf16_to_utf8(const uint16_t *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    if (!cap) return 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        else if (c >= 0xd800 && c < 0xe000) c = 0xfffd;
        char b[4]; size_t k;
        if (c < 0x80) { b[0] = (char)c; k = 1; }
        else if (c < 0x800) { b[0] = (char)(0xc0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3f)); k = 2; }
        else if (c < 0x10000) { b[0] = (char)(0xe0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 0x3f)); b[2] = (char)(0x80 | (c & 0x3f)); k = 3; }
        else { b[0] = (char)(0xf0 | c >> 18); b[1] = (char)(0x80 | ((c >> 12) & 0x3f)); b[2] = (char)(0x80 | ((c >> 6) & 0x3f)); b[3] = (char)(0x80 | (c & 0x3f)); k = 4; }
        if (o + k + 1 > cap) break;
        memcpy(out + o, b, k); o += k;
    }
    out[o] = 0;
    return o;
}

/* UTF-8 -> UTF-16, at most cap units (a character that does not fit whole is dropped); bad bytes become U+FFFD. */
static size_t utf8_to_utf16(const char *s, uint16_t *out, size_t cap) {
    const unsigned char *p = (const unsigned char *)s; size_t n = 0;
    while (*p) {
        uint32_t c; int k;
        if (*p < 0x80) { c = *p; k = 0; }
        else if ((*p & 0xe0) == 0xc0) { c = *p & 0x1f; k = 1; }
        else if ((*p & 0xf0) == 0xe0) { c = *p & 0x0f; k = 2; }
        else if ((*p & 0xf8) == 0xf0) { c = *p & 0x07; k = 3; }
        else { c = 0xfffd; k = 0; }
        p++;
        for (int i = 0; i < k; i++, p++) {
            if ((*p & 0xc0) != 0x80) { c = 0xfffd; break; }
            c = c << 6 | (*p & 0x3f);
        }
        if (c > 0x10ffff || (c >= 0xd800 && c < 0xe000)) c = 0xfffd;
        if (c >= 0x10000) {
            if (n + 2 > cap) break;
            out[n++] = (uint16_t)(0xd800 + ((c - 0x10000) >> 10)); out[n++] = (uint16_t)(0xdc00 + ((c - 0x10000) & 0x3ff));
        } else {
            if (n + 1 > cap) break;
            out[n++] = (uint16_t)c;
        }
    }
    return n;
}

/* Whether the applet would let this character through (single UTF-16 units; pairs are always allowed). */
static int allowed(uint16_t c, uint32_t disabled, int digits_only) {
    int digit = c >= '0' && c <= '9';
    if (digits_only && !digit) return 0;
    if (c < 0x20 || c == 0x7f) return 0;
    if ((disabled & 1u << 1 && c == ' ') || (disabled & 1u << 2 && c == '@') || (disabled & 1u << 3 && c == '%')) return 0;
    if ((disabled & 1u << 4 && c == '/') || (disabled & 1u << 5 && c == '\\') || (disabled & 1u << 6 && digit)) return 0;
    return 1;
}

/* Drop disallowed characters and keep at most max units without splitting a surrogate pair. */
static size_t clean(uint16_t *t, size_t n, size_t max, uint32_t disabled, int digits_only) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        int pair = t[i] >= 0xd800 && t[i] < 0xdc00 && i + 1 < n;
        size_t k = pair ? 2 : 1;
        if (!pair && !allowed(t[i], disabled, digits_only)) continue;
        if (o + k > max) break;
        t[o++] = t[i]; if (pair) t[o++] = t[++i];
    }
    return o;
}

static int negative(void) { const char *e = getenv("TOTK_PROBE_NEGATIVE"); return e ? atoi(e) : 0; }

static unsigned show_keyboard(void *out_string, const void *arg_ptr) {
    const uint8_t *arg = arg_ptr; uint64_t *str = out_string;
    if (!arg || !str) runtime_fail("software keyboard: null argument");
    uint32_t type = rd32(arg + KB_TYPE), disabled = rd32(arg + KB_DISABLED), password = rd32(arg + KB_PASSWORD);
    int32_t max_len = (int32_t)rd32(arg + KB_MAX_LEN), min_len = (int32_t)rd32(arg + KB_MIN_LEN);
    int utf8 = arg[KB_UTF8] != 0;
    if (max_len <= 0 || max_len > KB_LIMIT || negative() == 22) max_len = KB_LIMIT;   /* 22: probe's negative control */
    if (min_len < 0) min_len = 0;
    if (min_len > max_len) min_len = max_len;
    char header[0x41 * 4], sub[0x81 * 4], guide[0x101 * 4], ok[9 * 4], initial[KB_LIMIT * 4 + 1] = "";
    utf16_to_utf8((const uint16_t *)(arg + KB_HEADER), u16_len((const uint16_t *)(arg + KB_HEADER), 0x40), header, sizeof header);
    utf16_to_utf8((const uint16_t *)(arg + KB_SUB), u16_len((const uint16_t *)(arg + KB_SUB), 0x80), sub, sizeof sub);
    utf16_to_utf8((const uint16_t *)(arg + KB_GUIDE), u16_len((const uint16_t *)(arg + KB_GUIDE), 0x100), guide, sizeof guide);
    utf16_to_utf8((const uint16_t *)(arg + KB_OK_TEXT), u16_len((const uint16_t *)(arg + KB_OK_TEXT), 8), ok, sizeof ok);
    uint64_t work = rd64(arg + ARG_WORK), work_size = rd64(arg + ARG_WORK_SIZE);
    int32_t init_off = (int32_t)rd32(arg + KB_INIT_OFF), init_len = (int32_t)rd32(arg + KB_INIT_LEN);
    if (init_len > 0 && work && init_off >= 0 && (uint64_t)init_off + (uint64_t)init_len * 2 <= work_size)
        utf16_to_utf8((const uint16_t *)(uintptr_t)(work + (uint64_t)init_off), (size_t)(init_len > KB_LIMIT ? KB_LIMIT : init_len), initial, sizeof initial);
    char prompt[sizeof header + sizeof guide + 8];
    snprintf(prompt, sizeof prompt, "%s%s%s", *header ? header : *guide ? guide : *sub ? sub : TOTK_UI("输入文字","Enter text"),
             *header && (*guide || *sub) ? " — " : "", *header ? (*guide ? guide : sub) : "");

    char text[KB_LIMIT * 4 + 1] = ""; int confirmed = 0;
    const char *forced = getenv("TOTK_SWKBD_ANSWER");
    if (forced) {
        confirmed = strcmp(forced, "<cancel>") != 0;
        snprintf(text, sizeof text, "%s", strcmp(forced, "<initial>") ? forced : initial);
    } else {
#if TARGET_OS_OSX
        RuntimeTextRequest req = { prompt, initial, max_len, min_len, password != 0, type == KB_TYPE_NUMPAD, disabled };
        int away = runtime_core_away();   /* the other guest threads keep running while the player types */
        confirmed = runtime_front_text_input(&req, text, sizeof text);
        runtime_core_back(&away);
#else
        fprintf(stderr, "[totk] keyboard: text entry is not available on this platform; cancelled\n");
#endif
    }
    uint16_t u[KB_LIMIT * 2];
    size_t n = confirmed ? clean(u, utf8_to_utf16(text, u, KB_LIMIT * 2), (size_t)max_len, disabled, type == KB_TYPE_NUMPAD) : 0;
    if (confirmed && (int32_t)n < min_len) { confirmed = 0; n = 0; }
    uint64_t buf = str[0], size = str[1];
    if (buf && size) {
        if (utf8) utf16_to_utf8(u, n, (char *)(uintptr_t)buf, size);
        else {
            size_t cap = size / 2;   /* units, one kept for the terminator */
            if (cap) {
                if (n > cap - 1) { n = cap - 1; if (n && u[n - 1] >= 0xd800 && u[n - 1] < 0xdc00) n--; }
                memcpy((void *)(uintptr_t)buf, u, n * 2); ((uint16_t *)(uintptr_t)buf)[n] = 0;
            }
        }
    }
    char shown[sizeof text]; utf16_to_utf8(u, n, shown, sizeof shown);
    if (password) snprintf(shown, sizeof shown, "(password, %zu characters, not logged)", n);
    fprintf(stderr, "[totk] keyboard: \"%s\" (type %u, %d-%d characters, disabled keys %#x, %s out)%s -> %s%s%s\n", prompt, type, min_len,
            max_len, disabled, utf8 ? "UTF-8" : "UTF-16", forced ? " [TOTK_SWKBD_ANSWER]" : "", confirmed ? "\"" : "cancelled",
            confirmed ? shown : "", confirmed ? "\"" : "");
    return confirmed ? 0 : SWKBD_CANCELED;
}

void *runtime_swkbd_override(const char *name) {
    if (!strcmp(name, "_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE")) return show_keyboard;
    return NULL;
}

/* Diagnostic command "kbtest" (TOTK_INPUT_FILE, runtime/input.c): asks for text the way TOTK names a horse (user-name
   limits: 9 characters, '@' '%' '\' disabled), on a thread of its own so that "kbtype" / "kbenter" / "kbesc" can answer
   the bar while it waits. Logs what the game would get. */
static void *selftest_thread(void *unused) {
    (void)unused;
    uint8_t *arg = calloc(1, ARG_SIZE); uint16_t *out = calloc(1, 0x7d4); uint8_t *work = calloc(1, 0x1000);
    if (!arg || !out || !work) runtime_fail("keyboard self-test allocation");
    uint32_t max = 9, disabled = 0x12c; uint64_t w = (uint64_t)(uintptr_t)work, ws = 0x1000;
    static const uint16_t title[] = {0x6d4b, 0x8bd5, 0xff1a, 0x7ed9, 0x9a6c, 0x53d6, 0x540d, 0};   /* 测试：给马取名 */
    memcpy(arg + KB_HEADER, title, sizeof title); memcpy(arg + KB_MAX_LEN, &max, 4); memcpy(arg + KB_DISABLED, &disabled, 4);
    memcpy(arg + ARG_WORK, &w, 8); memcpy(arg + ARG_WORK_SIZE, &ws, 8);
    uint64_t str[2] = { (uint64_t)(uintptr_t)out, 0x7d4 };
    unsigned r = show_keyboard(str, arg);
    char text[64]; utf16_to_utf8(out, u16_len(out, 0x3e9), text, sizeof text);
    fprintf(stderr, "[totk] keyboard self-test: result %#x, text \"%s\"\n", r, text);
    free(arg); free(out); free(work);
    return NULL;
}
void runtime_swkbd_selftest(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, selftest_thread, NULL)) runtime_fail("keyboard self-test thread");
    pthread_detach(t);
}

/* ---------- probe (make probe-swkbd): guest SDK arguments, main's PLT stub, no window ---------- */
#include "game_version.h"
enum { MAIN_PLT_SHOW_KEYBOARD = 0, MAIN_GOT_SHOW_KEYBOARD = 0 };   /* Metroid Dread: main's ShowKeyboard PLT/GOT not mapped yet; the probe refuses to run */
static unsigned checks;
static void need(int ok, const char *what) { checks++; if (!ok) { fprintf(stderr, "swkbd probe: %s\n", what); runtime_fail("software keyboard probe failed"); } }
static void *sym(const char *name) { void *p = runtime_symbol(name); if (!p) { fprintf(stderr, "swkbd probe: no %s\n", name); runtime_fail("software keyboard probe: guest symbol"); } return p; }
static int u16eq(const uint16_t *a, const char *utf8) {
    uint16_t b[64]; size_t n = utf8_to_utf16(utf8, b, 63); b[n] = 0;
    for (size_t i = 0; i <= n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static void *probe_thread(void *unused) {
    (void)unused; runtime_thread_init();
    typedef void (*MakePreset)(void *, int);
    typedef void (*SetText)(void *, const uint16_t *);
    typedef uint64_t (*Size)(void);
    typedef uint64_t (*WorkSize)(int);
    typedef unsigned (*Show)(void *, const void *);
    MakePreset make = (MakePreset)sym("_ZN2nn5swkbd10MakePresetEPNS0_14KeyboardConfigENS0_6PresetE");
    SetText header = (SetText)sym("_ZN2nn5swkbd13SetHeaderTextEPNS0_14KeyboardConfigEPKDs");
    SetText guide = (SetText)sym("_ZN2nn5swkbd12SetGuideTextEPNS0_14KeyboardConfigEPKDs");
    SetText initial = (SetText)sym("_ZN2nn5swkbd14SetInitialTextEPNS0_15ShowKeyboardArgEPKDs");
    Size string_size = (Size)sym("_ZN2nn5swkbd27GetRequiredStringBufferSizeEv");
    WorkSize work_size = (WorkSize)sym("_ZN2nn5swkbd25GetRequiredWorkBufferSizeEb");
    if (!MAIN_PLT_SHOW_KEYBOARD) runtime_fail("software keyboard probe: Metroid Dread PLT addresses are not mapped");
    uint8_t *main_base = runtime_module_base("main");
    need(main_base != NULL, "main module base");
    void *got; memcpy(&got, main_base + MAIN_GOT_SHOW_KEYBOARD, 8);
    need(got == runtime_hle_wrap((void *)show_keyboard), "main's ShowKeyboard GOT slot holds the host keyboard");
    Show show = (Show)(void *)(main_base + MAIN_PLT_SHOW_KEYBOARD);
    need(string_size() == 0x7d4 && work_size(0) == 0x1000, "SDK buffer sizes");

    uint8_t *arg = calloc(1, ARG_SIZE); uint64_t str[2] = { (uint64_t)(uintptr_t)malloc(0x7d4), 0x7d4 };
    uint8_t *wbuf = calloc(1, 0x1000);
    need(arg && str[0] && wbuf, "allocations");
    uint16_t *out = (uint16_t *)(uintptr_t)str[0];
    struct { int preset; uint32_t type, max, min, disabled; int utf8; const char *initial, *answer, *want; unsigned result; } cases[] = {
        {2, 0, 9, 0, 0x12c, 0, NULL, "Epona", "Epona", 0},                       /* TOTK's horse naming */
        {2, 0, 9, 0, 0x12c, 0, NULL, "ABCDEFGHIJKL", "ABCDEFGHI", 0},             /* at most 9 */
        {2, 0, 9, 0, 0x12c, 0, NULL, "a@b%c\\d e/f", "abcd e/f", 0},             /* '@' '%' '\' disabled */
        {2, 0, 9, 0, 0x12c, 0, NULL, "<cancel>", "", SWKBD_CANCELED},
        {0, 2, 0, 0, 0, 1, NULL, "小马Ab", "小马Ab", 0},                          /* UTF-8 out (checked below) */
        {0, 2, 3, 0, 0, 0, NULL, "小马🐴", "小马", 0},                            /* no half surrogate pair */
        {0, 2, 0, 0, 0, 0, "Zelda", "<initial>", "Zelda", 0},                     /* initial text from the work buffer */
        {0, 1, 0, 0, 0, 0, NULL, "a1b2c3", "123", 0},                            /* number pad */
        {0, 2, 9, 3, 0, 0, NULL, "ab", "", SWKBD_CANCELED},                       /* shorter than the minimum */
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        memset(arg, 0, ARG_SIZE); memset(wbuf, 0, 0x1000); memset(out, 0x55, 0x7d4);
        make(arg, cases[i].preset);
        static const uint16_t title[] = {0x540d, 0x5b57, 0};   /* 名字 */
        static const uint16_t hint[] = {'h', 'i', 'n', 't', 0};
        header(arg, title); guide(arg, hint);
        memcpy(arg + KB_TYPE, &cases[i].type, 4); memcpy(arg + KB_MAX_LEN, &cases[i].max, 4); memcpy(arg + KB_MIN_LEN, &cases[i].min, 4);
        if (cases[i].disabled) memcpy(arg + KB_DISABLED, &cases[i].disabled, 4);
        arg[KB_UTF8] = (uint8_t)cases[i].utf8;
        uint64_t w = (uint64_t)(uintptr_t)wbuf, ws = 0x1000;
        memcpy(arg + ARG_WORK, &w, 8); memcpy(arg + ARG_WORK_SIZE, &ws, 8);
        if (cases[i].initial) { uint16_t t[16]; size_t n = utf8_to_utf16(cases[i].initial, t, 15); t[n] = 0; initial(arg, t); }
        setenv("TOTK_SWKBD_ANSWER", cases[i].answer, 1);
        unsigned r = show(str, arg);
        char what[160]; snprintf(what, sizeof what, "case %zu (%s): result %#x", i, cases[i].answer, r);
        need(r == cases[i].result, what);
        if (cases[i].utf8) need(!strcmp((const char *)out, cases[i].want), "UTF-8 output");
        else need(u16eq(out, cases[i].want), "UTF-16 output");
    }
    unsetenv("TOTK_SWKBD_ANSWER");
    printf("{\"status\":\"pass\",\"probe\":\"probe-swkbd\",\"checks\":%u,\"scope\":\"arguments built by the guest SDK (MakePreset, SetHeaderText, SetGuideText, SetInitialText), call through main's PLT stub and GOT slot: TOTK's horse-name preset, length limit, disabled keys, cancel, UTF-8 output, surrogate pairs, initial text from the work buffer, number pad, minimum length; answers from TOTK_SWKBD_ANSWER (the window bar is not exercised)\"}\n", checks);
    return NULL;
}
void runtime_swkbd_probe(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, probe_thread, NULL) || pthread_join(t, NULL)) runtime_fail("software keyboard probe thread");
}
