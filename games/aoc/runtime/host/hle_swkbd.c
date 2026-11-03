// swkbd software keyboard (system applet) → host native input box. MakePreset / SetInitialTextUtf8 / GetRequired*Size are left to run as is in the SDK,
// here we only read the ShowKeyboardArg they write and write back String per the output convention of the real swkbd applet.
// Layout sources (offsets in the sdk image the host was first written against):
//   ShowKeyboard 0x3a6770 moves KeyboardConfig field by field into the applet arguments: +0x24 title u16[65], +0xa6 subtitle u16[129], +0x1a8 hint u16[257],
//     +0x3ac maximum length, +0x3b0 minimum length, +0x3bd isUseUtf8, +0x3c0 offset of the initial text within the work buffer, +0x3c4 UTF-16 length of the initial text;
//     work buffer pointer / size at +0x4d0 / +0x4d8 (handed to the applet together with CreateLargeStorage)
//   SetInitialTextUtf8 0x3a74c0: UTF-8 → UTF-16 written to work buffer+offset (offset 0x14, at most 500 characters + terminating 0)
//   ReadCloseResultAndString 0x3a63f0: output storage = u32 CloseResult + text area; the text area is copied as is into String{+0 pointer, +8 byte count}
//   ShowKeyboard wrap-up 0x3a6acc: CloseResult == 0 (OK) → Result 0, otherwise (cancel) → 0x29F; GetRequiredStringBufferSize = 0x7D4
// A game that sets isUseUtf8 = 1 after MakePreset reads the result back as a C string; UTF-16 output is supported as well per the original convention.
// No window and DM_KBD unset: return as cancel (no popup, no hang). DM_KBD="text": no popup, return that text directly as OK (windowless testing / lockstep comparison).
#include "hle.h"
#include <stddef.h>
#include <unistd.h>
#include "front.h"

enum { ARG_HEADER = 0x24, ARG_SUB = 0xa6, ARG_GUIDE = 0x1a8, ARG_MAXLEN = 0x3ac, ARG_MINLEN = 0x3b0, ARG_UTF8 = 0x3bd,
       ARG_INIT_OFF = 0x3c0, ARG_INIT_LEN = 0x3c4, ARG_WORKBUF = 0x4d0,
       HEADER_N = 65, SUB_N = 129, GUIDE_N = 257 };
#define SWKBD_RESULT_CANCELED 0x29Fu   // swkbd module 159 / description 1: the constant the SDK returns when CloseResult != 0

// UTF-16 (at most n units, stops at 0) → UTF-8, written to out (cap includes the terminating 0), returns the number of bytes written
size_t swkbd_u16_to_u8(const uint16_t *s, size_t n, char *out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        char b[4]; int k = c < 0x80 ? (b[0] = (char)c, 1) : c < 0x800 ? (b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 63)), 2)
                 : c < 0x10000 ? (b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 63)), b[2] = (char)(0x80 | (c & 63)), 3)
                 : (b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | (c >> 12 & 63)), b[2] = (char)(0x80 | (c >> 6 & 63)), b[3] = (char)(0x80 | (c & 63)), 4);
        if (o + k + 1 > cap) break;
        memcpy(out + o, b, k); o += k;
    }
    if (cap) out[o] = 0;
    return o;
}
// UTF-8 → UTF-16, at most max_units units (surrogate pairs are not split), written to out (cap units including the terminating 0), returns the unit count
size_t swkbd_u8_to_u16(const char *s, size_t max_units, uint16_t *out, size_t cap) {
    size_t o = 0; const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t c; int k = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
        c = k == 1 ? *p : k == 2 ? *p & 31 : k == 3 ? *p & 15 : *p & 7;
        for (int j = 1; j < k; j++) { if ((p[j] & 0xC0) != 0x80) { k = j; break; } c = c << 6 | (p[j] & 63); }
        size_t need = c >= 0x10000 ? 2 : 1;
        if (o + need > max_units || o + need + 1 > cap) break;
        if (need == 2) { c -= 0x10000; out[o++] = (uint16_t)(0xD800 | c >> 10); out[o++] = (uint16_t)(0xDC00 | (c & 0x3FF)); }
        else out[o++] = (uint16_t)c;
        p += k;
    }
    if (cap) out[o] = 0;
    return o;
}

// Core (called directly by unit tests): decode the display parameters from arg, get the user text through input (returns 1 OK / 0 cancel / -1 no window), write back out per the arg convention
// The game's offline character name rule (original image, judged byte by byte): 2–15 bytes, first and last must be a "letter", '-' and '_' at most one in total, the rest must be letters. The original game only accepts ASCII letters;
// hle_charname.c also lets bytes ≥0x80 through (Chinese names), so the rule becomes: English letters or Chinese characters, at most 15 UTF-8 bytes in total (3 bytes per Chinese character, at most 5).
// While the naming panel is open the front end blocks on the spot by this rule and writes the rule in the dialog. diag.c maintains this flag from UI messages.
int g_swkbd_name_panel;
int g_swkbd_numeric_max = -1;   // ≥0 = this input is a quantity box, value = upper limit (set by dm_textinput.c, read by the front end input box, set back to -1 after use)
int (*g_swkbd_unfilter)(void);   // registered by the host (dm_charname_unfilter in hle_charname.c; NULL in unit tests): keyboard requests from the naming panel bypass the game's ASCII typing filter
typedef int (*swkbd_input_fn)(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap);
Result swkbd_show(void *out, const void *arg_, swkbd_input_fn input) {
    const uint8_t *arg = arg_;
    char title[HEADER_N * 3 + 1], sub[SUB_N * 3 + 1], guide[GUIDE_N * 3 + 1], init[501 * 3 + 1] = "", text[1002 * 3 + 1];
    swkbd_u16_to_u8((const uint16_t *)(arg + ARG_HEADER), HEADER_N, title, sizeof title);
    swkbd_u16_to_u8((const uint16_t *)(arg + ARG_SUB), SUB_N, sub, sizeof sub);
    swkbd_u16_to_u8((const uint16_t *)(arg + ARG_GUIDE), GUIDE_N, guide, sizeof guide);
    int32_t maxlen, ilen, ioff; memcpy(&maxlen, arg + ARG_MAXLEN, 4); memcpy(&ilen, arg + ARG_INIT_LEN, 4); memcpy(&ioff, arg + ARG_INIT_OFF, 4);
    const uint8_t *work; memcpy(&work, arg + ARG_WORKBUF, 8);
    if (ilen > 0 && work) swkbd_u16_to_u8((const uint16_t *)(work + ioff), (size_t)ilen, init, sizeof init);

    const char *script = getenv("DM_KBD");
    // The guest clock keeps running while the keyboard is up, as on the Switch: the applet blocks the calling thread and the game's other threads go on in real time. d2r-mobile froze it here
    // (monsters should not attack while the player typed), which left the threads that wait on the clock spinning: Octopath Traveler 0's sound stuttered and the input box lagged on an iPhone
    // (5,035 wakeups a second while the box was up, about 1,800 otherwise; 7,936 on the Mac with the box held 8 s). The iOS input box froze it a second time around its own wait (front_ios.m),
    // which kept the stutter until that went too. The inline keyboard below still freezes it: there the host's input box blocks a game that on hardware keeps running and steps by real time (Monster Hunter Rise)
    // Test switch DM_TEST_SWKBD_HOLD_MS=<ms> (with DM_KBD): the keyboard stays up that long before DM_KBD's text is returned, the way a player types into the input box (for reproducing on the Mac what the game's other threads do meanwhile)
    static int hold = -1; if (hold < 0) { const char *h = getenv("DM_TEST_SWKBD_HOLD_MS"); hold = h ? atoi(h) : 0; }
    if (script && hold > 0) usleep((useconds_t)hold * 1000);
    int r = script ? (snprintf(text, sizeof text, "%s", script), 1) : input(title, sub, guide, init, maxlen > 0 ? maxlen : 0, text, sizeof text);
    g_swkbd_numeric_max = -1;   // the numeric box mark applies to this one time only (set by dm_textinput.c before bringing up the keyboard)
    LOG("swkbd: title '%s' initial '%s' limit %d → %s%s%s", title, init, maxlen,
        r > 0 ? "OK '" : r == 0 ? "cancel" : "no window, treated as cancel", r > 0 ? text : "", r > 0 ? "'" : "");
    if (r <= 0) return SWKBD_RESULT_CANCELED;
    if (g_swkbd_name_panel && g_swkbd_unfilter && g_swkbd_unfilter()) LOG("swkbd: naming panel -- turned off the game's ASCII input filter on the returned string (Chinese names allowed, see hle_charname.c)");

    uint8_t *buf; uint64_t cap; memcpy(&buf, (uint8_t *)out, 8); memcpy(&cap, (uint8_t *)out + 8, 8);
    if (!buf || !cap) return RES_OK;                         // same as the SDK: if String has no buffer, the text is not read
    memset(buf, 0, cap);
    uint16_t u16[1003]; size_t n = swkbd_u8_to_u16(text, maxlen > 0 ? (size_t)maxlen : 1002, u16, 1003);   // truncate to the character limit (counted in UTF-16 units)
    if (arg[ARG_UTF8]) swkbd_u16_to_u8(u16, n, (char *)buf, cap);
    else memcpy(buf, u16, (n + 1) * 2 <= cap ? n * 2 : (cap / 2 - 1) * 2);
    { const char *lag = getenv("DM_TEST_SWKBD_LAG_MS"); if (lag) usleep((useconds_t)atoi(lag) * 1000); }   // test: slow down "keyboard handed back → the game's thread gets the result" to verify that auto-accept does not jump the gun   // front end path cleanup (dm_cursor.c): auto-accept after the name is typed on the naming page
    return RES_OK;
}

HLE_FORCE("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", "software keyboard applet → host native input box (DM_KBD gives the text directly)",
          Result, h_swkbd_Show, void *out, const void *arg) {
    return swkbd_show(out, arg, front_text_input);
}

// ---------- InlineKeyboard (Super Mario 3D World + Bowser's Fury) ----------
// On hardware Initialize starts a resident keyboard applet (swkbd::detail::InlineKeyboardImpl::Launch -> applet::CreateLibraryApplet); after that Calc collects its results every frame and calls the callbacks the game registered,
// Appear / Disappear / SetInputText / SetCursorPos send requests through applet storage, and GetImage takes the keyboard picture from shared memory. The host has no applet proxy: entering the save select after the character select, Initialize crashed on a null pointer in CreateLibraryApplet.
// Every entry that touches the applet is taken over here. Appear arms one request; the next Calc (on hardware the callbacks are called from Calc too) asks for the text the way ShowKeyboard does
// (DM_KBD, else the front end's input box) and hands it to the game's changed-string and enter callbacks, or calls its cancel callback (no callbacks, no input box, or the player cancelled).
// No picture (GetImage reports none). Monster Hunter Rise names the hunter, the Palico and the Palamute through it; with every Appear counted as a cancel the name box stayed empty for good
// The constructor, GetRequiredWorkBufferSize / GetImageMemoryRequirement / GetRequiredStringBufferSize and InputText::Set are pure computations and keep running in the SDK
typedef void (*ikb_cb0)(void);
// fin_init / released: callbacks the applet makes from Calc once Initialize / UnsetUserWordInfo have finished (Monster Hunter Rise, SDK 11); the host makes them on the next Calc
// The arguments of the changed-string and enter callbacks, as the game reads them (Monster Hunter Rise's callbacks main+0x6fb3910 / main+0x6fb3afc): the text inline, then its length;
// the changed-string one adds three cursor positions. AppearArg: textMaxLength at +0x20 (as libnx's SwkbdAppearArg)
typedef struct { uint16_t str[502]; int32_t length, dic_start, dic_end, cursor; } ikb_str_arg;
_Static_assert(offsetof(ikb_str_arg, length) == 0x3ec && offsetof(ikb_str_arg, cursor) == 0x3f8, "swkbd ChangedStringArg / DecidedEnterArg");
typedef void (*ikb_cb1)(const ikb_str_arg *);
// state: what Calc returns (nn::swkbd::State as Monster Hunter Rise reads it at main+0x6fb3e6c: it finishes an entered name only when Calc returns 1 after a non-zero value):
// 0 until initialization has finished, 1 ready / hidden, 3 shown. The hook used to return nothing, leaving w0 as whatever the host last held
static struct { const void *kb; ikb_cb0 cancel, fin_init, released, finished; ikb_cb1 enter, changed; int pending, fin_init_due, released_due, finished_due, max_len, state; } g_ikb[4];   // finished: called on the Calc after Disappear (the keyboard has closed)
static pthread_mutex_t g_ikb_mu = PTHREAD_MUTEX_INITIALIZER;
static int ikb_slot(const void *kb) {   // the caller holds the lock
    for (int i = 0; i < 4; i++) if (g_ikb[i].kb == kb) return i;
    for (int i = 0; i < 4; i++) if (!g_ikb[i].kb) { g_ikb[i].kb = kb; return i; }
    return -1;
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard10InitializeEPv", "inline keyboard: no applet started (the host has no applet proxy), reports success", unsigned, h_ikb_Init, void *kb, void *work) {
    (void)work; pthread_mutex_lock(&g_ikb_mu); int i = ikb_slot(kb); if (i >= 0) g_ikb[i].fin_init_due = 1; pthread_mutex_unlock(&g_ikb_mu);
    LOG("swkbd: inline keyboard Initialize %p (the host starts no applet; Appear asks for the text the way ShowKeyboard does)%s", kb, i < 0 ? "[RED] registration table full" : "");
    return 1;
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard8FinalizeEv", "inline keyboard: no applet started, nothing to close", void, h_ikb_Fin, void *kb) {
    pthread_mutex_lock(&g_ikb_mu); for (int i = 0; i < 4; i++) if (g_ikb[i].kb == kb) memset(&g_ikb[i], 0, sizeof g_ikb[i]); pthread_mutex_unlock(&g_ikb_mu);
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboardD1Ev", "inline keyboard destructor: no applet started, only clears the registration", void, h_ikb_Dtor, void *kb) { h_ikb_Fin(kb); }
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard24SetDecidedCancelCallbackEPFvvE", "inline keyboard: records the cancel callback", void, h_ikb_SetCancel, void *kb, ikb_cb0 fn) {
    pthread_mutex_lock(&g_ikb_mu); int i = ikb_slot(kb); if (i >= 0) g_ikb[i].cancel = fn; pthread_mutex_unlock(&g_ikb_mu);
}
#define IKB_IGNORE_CB(mangled, cname) \
    HLE_FORCE(mangled, "inline keyboard: callbacks are recorded, never called (the keyboard never confirms / edits text / moves the cursor)", void, cname, void *kb, void *fn) { (void)kb; (void)fn; }
IKB_IGNORE_CB("_ZN2nn5swkbd14InlineKeyboard22SetMovedCursorCallbackEPFvPKNS0_14MovedCursorArgEE", h_ikb_SetMoved)
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard23SetDecidedEnterCallbackEPFvPKNS0_15DecidedEnterArgEE", "inline keyboard: records the enter callback", void, h_ikb_SetEnter, void *kb, ikb_cb1 fn) {
    pthread_mutex_lock(&g_ikb_mu); int i = ikb_slot(kb); if (i >= 0) g_ikb[i].enter = fn; pthread_mutex_unlock(&g_ikb_mu);
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard24SetChangedStringCallbackEPFvPKNS0_16ChangedStringArgEE", "inline keyboard: records the changed-string callback", void, h_ikb_SetChanged, void *kb, ikb_cb1 fn) {
    pthread_mutex_lock(&g_ikb_mu); int i = ikb_slot(kb); if (i >= 0) g_ikb[i].changed = fn; pthread_mutex_unlock(&g_ikb_mu);
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard27SetFinishedKeyboardCallbackEPFvvE", "inline keyboard: records the finished callback, called on the next Calc after Disappear", void, h_ikb_SetFinished, void *kb, ikb_cb0 fn) {
    pthread_mutex_lock(&g_ikb_mu); int i = ikb_slot(kb); if (i >= 0) g_ikb[i].finished = fn; pthread_mutex_unlock(&g_ikb_mu);
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard6AppearERKNS0_9AppearArgE", "inline keyboard appears: the next Calc asks for the text (DM_KBD or the front end's input box)", void, h_ikb_Appear, void *kb, const void *arg) {
    int32_t max = 0; if (arg) memcpy(&max, (const uint8_t *)arg + 0x20, 4);
    pthread_mutex_lock(&g_ikb_mu); int i = ikb_slot(kb); if (i >= 0) { g_ikb[i].pending = 1; g_ikb[i].state = 3; g_ikb[i].max_len = max > 0 && max <= 500 ? max : 0; } pthread_mutex_unlock(&g_ikb_mu);
    LOG("swkbd: inline keyboard Appear %p (at most %d characters) → text asked for on the next tick", kb, max);
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard9DisappearEv", "inline keyboard hidden: the finished callback follows on the next Calc", void, h_ikb_Disappear, void *kb) {
    pthread_mutex_lock(&g_ikb_mu); int i = ikb_slot(kb); if (i >= 0) { g_ikb[i].pending = 0; g_ikb[i].finished_due = 1; if (g_ikb[i].state) g_ikb[i].state = 1; } pthread_mutex_unlock(&g_ikb_mu);
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard4CalcEv", "inline keyboard per-frame update: a pending Appear gets the text and the game's callbacks; returns the keyboard state", int, h_ikb_Calc, void *kb) {
    ikb_cb0 fn = NULL, init = NULL, rel = NULL, fin = NULL; ikb_cb1 enter = NULL, changed = NULL; int ask = 0, max = 0;
    pthread_mutex_lock(&g_ikb_mu); int i = ikb_slot(kb);
    if (i >= 0 && g_ikb[i].fin_init_due) { if (!g_ikb[i].state) g_ikb[i].state = 1; if (g_ikb[i].fin_init) { g_ikb[i].fin_init_due = 0; init = g_ikb[i].fin_init; } }
    if (i >= 0 && g_ikb[i].released_due && g_ikb[i].released) { g_ikb[i].released_due = 0; rel = g_ikb[i].released; }
    if (i >= 0 && g_ikb[i].finished_due) { g_ikb[i].finished_due = 0; fin = g_ikb[i].finished; }
    if (i >= 0 && g_ikb[i].pending) { g_ikb[i].pending = 0; ask = 1; fn = g_ikb[i].cancel; enter = g_ikb[i].enter; changed = g_ikb[i].changed; max = g_ikb[i].max_len; }
    pthread_mutex_unlock(&g_ikb_mu);
    if (init) init();   // guest code, called on the guest thread that called Calc (as on hardware)
    if (rel) rel();
    if (fin) fin();
    if (!ask) return i >= 0 ? g_ikb[i].state : 0;
    char text[502 * 3 + 1]; int r = 0;
    if (enter) {   // as swkbd_show: DM_KBD answers without a box; the guest clock stands still while the box is up
        const char *script = getenv("DM_KBD");
        if (script) { snprintf(text, sizeof text, "%s", script); r = 1; }
        else { guest_clock_freeze_for(2, 1); r = front_text_input("", "", "", "", max, text, sizeof text); guest_clock_freeze_for(2, 0); }
    }
    LOG("swkbd: inline keyboard %p → %s%s%s", kb, r > 0 ? "OK '" : enter ? "cancel" : "no enter callback, cancel", r > 0 ? text : "", r > 0 ? "'" : "");
    pthread_mutex_lock(&g_ikb_mu); if (g_ikb[i].kb == kb) g_ikb[i].state = 1; pthread_mutex_unlock(&g_ikb_mu);   // the keyboard closes once the text is decided or cancelled
    if (r <= 0) { if (fn) fn(); return 1; }
    static ikb_str_arg a; memset(&a, 0, sizeof a);   // one keyboard request at a time; static keeps 1 KB off the guest stack
    size_t n = swkbd_u8_to_u16(text, max > 0 ? (size_t)max : 500, a.str, 502);
    a.length = (int32_t)n; a.cursor = (int32_t)n;
    if (changed) changed(&a);
    enter(&a);
    return 1;
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard29SetFinishedInitializeCallbackEPFvvE", "inline keyboard: records the finished-initialize callback, called on the next Calc after Initialize", void, h_ikb_SetFinInit, void *kb, ikb_cb0 fn) {
    pthread_mutex_lock(&g_ikb_mu); int i = ikb_slot(kb); if (i >= 0) g_ikb[i].fin_init = fn; pthread_mutex_unlock(&g_ikb_mu);
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard31SetReleasedUserWordInfoCallbackEPFvvE", "inline keyboard: records the released-user-words callback, called on the next Calc after UnsetUserWordInfo", void, h_ikb_SetReleased, void *kb, ikb_cb0 fn) {
    pthread_mutex_lock(&g_ikb_mu); int i = ikb_slot(kb); if (i >= 0) g_ikb[i].released = fn; pthread_mutex_unlock(&g_ikb_mu);
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard15SetUserWordInfoERKNS0_12UserWordInfoE", "inline keyboard user words (sent through applet storage): the host keeps none", Result, h_ikb_SetWords, void *kb, const void *info) { (void)kb; (void)info; return RES_OK; }
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard17UnsetUserWordInfoEv", "inline keyboard user words released: the released callback follows on the next Calc", Result, h_ikb_UnsetWords, void *kb) {
    pthread_mutex_lock(&g_ikb_mu); int i = ikb_slot(kb); if (i >= 0) g_ikb[i].released_due = 1; pthread_mutex_unlock(&g_ikb_mu); return RES_OK;
}
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard25SetCustomizedDictionariesERKNS0_23CustomizedDictionarySetE", "inline keyboard dictionaries (applet storage): the host keeps none", Result, h_ikb_SetDict, void *kb, const void *d) { (void)kb; (void)d; return RES_OK; }
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard27UnsetCustomizedDictionariesEv", "inline keyboard dictionaries: none kept", Result, h_ikb_UnsetDict, void *kb) { (void)kb; return RES_OK; }
// Appearance setters (sent to the applet with the next request): the host shows no keyboard, so they are ignored
#define IKB_IGNORE(mangled, cname, ...) HLE_FORCE(mangled, "inline keyboard appearance setting (the host does not show the keyboard, ignored)", void, cname, void *kb, __VA_ARGS__) { (void)kb; }
IKB_IGNORE("_ZN2nn5swkbd14InlineKeyboard13SetWindowModeENS1_10WindowModeE", h_ikb_SetWinMode, int m)
IKB_IGNORE("_ZN2nn5swkbd14InlineKeyboard14SetFooterAlphaEf", h_ikb_SetFooterA, float a)
IKB_IGNORE("_ZN2nn5swkbd14InlineKeyboard14SetKeytopAlphaEf", h_ikb_SetKeyA, float a)
IKB_IGNORE("_ZN2nn5swkbd14InlineKeyboard14SetKeytopScaleEf", h_ikb_SetKeyScale, float a)
IKB_IGNORE("_ZN2nn5swkbd14InlineKeyboard18SetKeytopTranslateEff", h_ikb_SetKeyMove, float x, float y)
IKB_IGNORE("_ZN2nn5swkbd14InlineKeyboard15SetTouchEnabledEb", h_ikb_SetTouch, unsigned on)
IKB_IGNORE("_ZN2nn5swkbd14InlineKeyboard26SetHardwareKeyboardEnabledEb", h_ikb_SetHwKbd, unsigned on)
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard26IsUsedTouchPointByKeyboardEii", "inline keyboard touch hit test: the keyboard is never shown", unsigned, h_ikb_TouchHit, void *kb, int x, int y) { (void)kb; (void)x; (void)y; return 0; }
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard12GetMaxHeightEv", "inline keyboard height on screen: 400 of the 720 lines (the keyboard is never shown)", int, h_ikb_MaxH, void *kb) { (void)kb; return 400; }
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard12SetInputTextERKNS0_9InputTextE", "inline keyboard: set text (the host does not show it, ignored)", void, h_ikb_SetText, void *kb, const void *t) { (void)kb; (void)t; }
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard12SetCursorPosEi", "inline keyboard: set cursor (the host does not show it, ignored)", void, h_ikb_SetCursor, void *kb, int pos) { (void)kb; (void)pos; }
HLE_FORCE("_ZN2nn5swkbd14InlineKeyboard8GetImageEPvm", "inline keyboard image: none (returns false)", unsigned, h_ikb_GetImage, void *kb, void *buf, size_t n) { (void)kb; (void)buf; (void)n; return 0; }
// The keyboard draws into a 1280 × 720 picture on hardware; Monster Hunter Rise makes three textures of this size for it (a 0 × 0 answer made its texture creation return null, then a null read at main+0x7713044)
HLE_FORCE("_ZNK2nn5swkbd14InlineKeyboard13GetWindowSizeEPiS2_", "inline keyboard window size: 1280 × 720 as on hardware (no picture is ever delivered)", void, h_ikb_WinSize, const void *kb, int *w, int *h) { (void)kb; if (w) *w = 1280; if (h) *h = 720; }
