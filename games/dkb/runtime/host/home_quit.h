// Quit with the Home button (Mac front end, front_mac.m; LNPANDY/NX2Apple#336). Holding Home (the logo button on Xbox and PlayStation pads) for HQ_HOLD_S seconds opens a
// "Quit?" prompt over the game: A quits (the same exit as ⌘Q), B or a new press of Home closes it. Letting go of Home before the time is up does nothing. A and B are
// GameController's buttonA and buttonB (by label: the bottom button on an Xbox or PlayStation pad, the one printed A on a Nintendo pad), not the Switch buttons the game
// gets. Pure, so tests/t_home_quit.c checks it without a controller or a window.
//
// The hold works wherever the press reaches the host: pad_keep_system_buttons claims Home on every pad (preferredSystemGestureState = GCSystemGestureStateDisabled), and
// macOS 27 respects the claim when the Home button setting for apps lets the app decide (front_mac.m logs the setting). When the system takes Home anyway (it opens the
// Games app or the player's chosen app), the game window loses the focus and whatever was shown closes (hq_cancel). DM_HOME_QUIT=0 turns it off. A front end passes the
// texts in the player's language as DM_HOME_QUIT_TEXT (hq_text_line, docs/frontends.md), English otherwise.
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define HQ_HOLD_S 2.0    // how long Home is held before the prompt opens
#define HQ_RING_S 0.3    // the hold indicator shows only after this, so a short press shows nothing

typedef enum {
    HQ_IDLE,      // nothing shown, the game has its input
    HQ_HOLDING,   // Home is held: the indicator fills up
    HQ_PROMPT,    // the prompt is open: the game gets no controller input
    HQ_SETTLE,    // the prompt was closed: the game gets controller input again once Home, A and B are all up, so the B that closed it never reaches the game
    HQ_QUIT,      // A was pressed in the prompt: the front end quits (final)
} hq_phase;

typedef struct {
    hq_phase phase;
    double since;     // when the hold started (seconds, any clock that only goes forward)
    int home;         // Home was down at the last step (a press is a change from up to down)
    int a_up, b_up;   // A and B have been up since the prompt opened: a button already held when it opens does not count
} home_quit;

// Whether the hold does anything: on unless DM_HOME_QUIT=0 (env: its value, NULL when unset)
static inline int hq_enabled(const char *env) { return !(env && !strcmp(env, "0")); }

// One step with the buttons as they are now (pressed on any controller) at time now. Called on every change of a button and, while Home is held, from a timer.
static inline hq_phase hq_step(home_quit *q, double now, int home, int a, int b) {
    int pressed = home && !q->home;
    q->home = home;
    switch (q->phase) {
    case HQ_IDLE:
        if (pressed) { q->phase = HQ_HOLDING; q->since = now; }
        break;
    case HQ_HOLDING:
        if (!home) q->phase = HQ_IDLE;   // let go too early
        else if (now - q->since >= HQ_HOLD_S) { q->phase = HQ_PROMPT; q->a_up = !a; q->b_up = !b; }
        break;
    case HQ_PROMPT:
        if (!a) q->a_up = 1;
        if (!b) q->b_up = 1;
        if ((b && q->b_up) || pressed) q->phase = HQ_SETTLE;   // B or Home closes it; with A and B at once the safe one wins
        else if (a && q->a_up) q->phase = HQ_QUIT;
        break;
    case HQ_SETTLE:
        if (!home && !a && !b) q->phase = HQ_IDLE;
        break;
    case HQ_QUIT:
        break;
    }
    return q->phase;
}

// The window lost focus (another app, the Games app the system opened for Home): whatever was shown closes. A Home still held needs a new press
static inline void hq_cancel(home_quit *q) { if (q->phase != HQ_QUIT) { q->phase = HQ_IDLE; q->home = 1; } }

// The game gets no controller input in these phases
static inline int hq_blocks_input(hq_phase p) { return p == HQ_PROMPT || p == HQ_SETTLE || p == HQ_QUIT; }

// How full the hold indicator is (0 to 1), -1 while it is not shown
static inline double hq_ring(const home_quit *q, double now) {
    if (q->phase != HQ_HOLDING) return -1;
    double t = (now - q->since - HQ_RING_S) / (HQ_HOLD_S - HQ_RING_S);
    return t < 0 ? -1 : t > 1 ? 1 : t;
}

// The texts, in this order, one per line of DM_HOME_QUIT_TEXT: the hold indicator, the prompt's title, its note, the quit button, the cancel button
enum { HQ_TEXT_HOLD, HQ_TEXT_TITLE, HQ_TEXT_NOTE, HQ_TEXT_QUIT, HQ_TEXT_CANCEL, HQ_TEXTS };
// Line i of text into out; an empty or missing line (or no text) gives def. The title's default has the game's name: def is the caller's
static inline void hq_text_line(const char *text, int i, const char *def, char *out, size_t cap) {
    const char *s = text;
    for (int k = 0; s && k < i; k++) { s = strchr(s, '\n'); if (s) s++; }
    size_t n = 0;
    if (s) { const char *e = strchr(s, '\n'); n = e ? (size_t)(e - s) : strlen(s); }
    if (!n) { snprintf(out, cap, "%s", def); return; }
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n); out[n] = 0;
}

// GameController's Home button setting for apps (GCControllerHomeButtonSettingInAppAction, macOS 27) as the word the log line names it by:
// "system" (the system keeps Home whatever the app asks), "app" (the app decides), "off" (the system does nothing with Home in apps),
// "unavailable" (could not be read)
static inline const char *hq_in_app_name(long v) {
    return v == 0 ? "system" : v == 1 ? "app" : v == -1 ? "unavailable" : v > 1 ? "off" : "unavailable";
}
