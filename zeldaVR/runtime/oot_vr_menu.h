/* Ocarina of Time in VR: the VR menu in the room, without the runtime (tests/test_oot_vr_menu.c runs it). vr.md row 90. HookshotVR's
   ideas and numbers only (GPL-3; its vr/menu/vr_menu_layer.inc and vr_menu_api.inc read for the facts, no code taken): a panel
   1.4 m wide and 0.65 as tall, 1.2 m ahead along the head's level heading and 0.1 m below the eyes, facing the head; placed once
   when it opens and left there ("fixed in the world"), or following the head once it has turned more than 20 degrees away; either
   controller points at it; while it is open the game is told the buttons, triggers and sticks are let go, and after it closes
   they stay let go until every button and trigger is (so the press that closed it never reaches the game).

   Its rows are the VR settings page's own (games/oot/runtime/settings_vr_menu.h hands them over as oot_menu_row, one table: the
   page is the one source). Each page of the panel shows OOT_MENU_PER_PAGE rows; a row has its title on the left and on the right
   a "less" button, the value (or a slider's track) and a "more" button; the header has a close button, the footer the previous
   and next page.

   The open button (option menu_button; our buttons are listed in oot_vr_pad.h): 1 (default) the left stick click held alone for
   1 s: alone it is no button to the game until it is let go (the left-stick shift), and a click let go without anything else
   pressed is L3: held this long and opening the menu, the menu's mask swallows that L3. 2 the Menu button held 1.5 s: held, it
   recentres at 0.6 s as it does now, and no + is sent. 0: no button opens it. HookshotVR's own (Menu held 0.5 s, or Y, or both
   sticks) are all game buttons or our recentre here. B, or the open button again, closes it. Times in nanoseconds. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

/* ---- the rows, as the settings page has them ---- */
enum { OOT_MENU_ROWS_MAX = 160, OOT_MENU_TITLE = 256, OOT_MENU_VALUE = 128 };
enum { OOT_MENU_SWITCH = 0, OOT_MENU_CHOICES = 1, OOT_MENU_SLIDER = 2, OOT_MENU_INFO = 3 };
typedef struct {
    char title[OOT_MENU_TITLE];   /* UTF-8, in the panel's language */
    char value[OOT_MENU_VALUE];   /* the value as the page shows it: the choice's name, on / off, the number */
    int kind;                     /* OOT_MENU_* */
    int now;                      /* the value: a switch 0 / 1, a slider its number, choices the index of the chosen one (-1 none) */
    int min, max;                 /* a slider's range; choices 0 .. n - 1 */
    int enabled;                  /* 0: greyed out on the page, and not changed here */
} oot_menu_row;
/* How a change is asked for (runtime_vr_menu_change): step by +-1 (a switch flips, choices go round, a slider moves by
   oot_menu_slider_step), or set a slider to a value. */
enum { OOT_MENU_STEP = 0, OOT_MENU_SET = 1 };
static inline int oot_menu_slider_step(int min, int max) { const int s = (int)lroundf((float)(max - min) / 50); return s < 1 ? 1 : s; }
static inline int oot_menu_slider_value(int min, int max, float frac) {
    if (!isfinite(frac)) frac = 0;
    frac = fminf(1, fmaxf(0, frac));
    return min + (int)lroundf(frac * (float)(max - min));
}
/* A copy of UTF-8 text into n bytes, cut before a character that does not fit (never in the middle of one). */
static inline void oot_menu_copy_utf8(char *out, size_t n, const char *in) {
    if (!n) return;
    size_t len = in ? strlen(in) : 0;
    if (len >= n) { len = n - 1; while (len > 0 && ((unsigned char)in[len] & 0xc0) == 0x80) len--; }
    if (len) memcpy(out, in, len);
    out[len] = 0;
}

/* ---- the panel's layout, in its picture's pixels (x right, y down) ---- */
enum { OOT_MENU_W = 1600, OOT_MENU_H = 1040, OOT_MENU_TOP = 100, OOT_MENU_ROW_H = 84, OOT_MENU_PER_PAGE = 10 };
enum { OOT_MENU_LESS_X0 = 940, OOT_MENU_LESS_X1 = 1020, OOT_MENU_MID_X0 = 1030, OOT_MENU_MID_X1 = 1480, OOT_MENU_MORE_X0 = 1490, OOT_MENU_MORE_X1 = 1560 };
enum { OOT_MENU_CLOSE_X0 = 1470, OOT_MENU_CLOSE_X1 = 1570, OOT_MENU_CLOSE_Y0 = 15, OOT_MENU_CLOSE_Y1 = 85 };
enum { OOT_MENU_FOOT_Y0 = OOT_MENU_H - 90, OOT_MENU_FOOT_Y1 = OOT_MENU_H - 10, OOT_MENU_PREV_X0 = 40, OOT_MENU_PREV_X1 = 380, OOT_MENU_NEXT_X0 = 1220, OOT_MENU_NEXT_X1 = 1560 };
enum { OOT_MENU_HIT_NONE = 0, OOT_MENU_HIT_ROW, OOT_MENU_HIT_LESS, OOT_MENU_HIT_MORE, OOT_MENU_HIT_TRACK, OOT_MENU_HIT_PREV, OOT_MENU_HIT_NEXT, OOT_MENU_HIT_CLOSE };
typedef struct { int kind, row; float frac; } oot_menu_hit;
static inline int oot_menu_pages(int n) { return n <= 0 ? 1 : (n + OOT_MENU_PER_PAGE - 1) / OOT_MENU_PER_PAGE; }
/* What is at (x, y), 0..1 across and down the panel, on this page of n rows (kinds: each row's OOT_MENU_*). */
static inline oot_menu_hit oot_menu_hit_at(float x, float y, int page, int n, const int *kinds) {
    oot_menu_hit h = { OOT_MENU_HIT_NONE, -1, 0 };
    if (!isfinite(x) || !isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1) return h;
    const float px = x * OOT_MENU_W, py = y * OOT_MENU_H;
    const int pages = oot_menu_pages(n);
    if (px >= OOT_MENU_CLOSE_X0 && px < OOT_MENU_CLOSE_X1 && py >= OOT_MENU_CLOSE_Y0 && py < OOT_MENU_CLOSE_Y1) { h.kind = OOT_MENU_HIT_CLOSE; return h; }
    if (py >= OOT_MENU_FOOT_Y0 && py < OOT_MENU_FOOT_Y1) {
        if (px >= OOT_MENU_PREV_X0 && px < OOT_MENU_PREV_X1 && page > 0) h.kind = OOT_MENU_HIT_PREV;
        else if (px >= OOT_MENU_NEXT_X0 && px < OOT_MENU_NEXT_X1 && page + 1 < pages) h.kind = OOT_MENU_HIT_NEXT;
        return h;
    }
    if (py < OOT_MENU_TOP || py >= OOT_MENU_TOP + OOT_MENU_PER_PAGE * OOT_MENU_ROW_H) return h;
    const int slot = (int)((py - OOT_MENU_TOP) / OOT_MENU_ROW_H), row = page * OOT_MENU_PER_PAGE + slot;
    if (row < 0 || row >= n) return h;
    h.row = row; h.kind = OOT_MENU_HIT_ROW;
    const int kind = kinds ? kinds[row] : OOT_MENU_INFO;
    if (kind == OOT_MENU_INFO) return h;
    if (px >= OOT_MENU_LESS_X0 && px < OOT_MENU_LESS_X1) h.kind = OOT_MENU_HIT_LESS;
    else if (px >= OOT_MENU_MORE_X0 && px < OOT_MENU_MORE_X1) h.kind = OOT_MENU_HIT_MORE;
    else if (kind == OOT_MENU_SLIDER && px >= OOT_MENU_MID_X0 && px < OOT_MENU_MID_X1) {
        h.kind = OOT_MENU_HIT_TRACK; h.frac = (px - OOT_MENU_MID_X0) / (float)(OOT_MENU_MID_X1 - OOT_MENU_MID_X0);
    }
    return h;
}
/* A slider's knob, held by the trigger, follows the pointer across the track even off its row: the fraction at x. */
static inline float oot_menu_track_frac(float x) {
    const float px = x * OOT_MENU_W, f = (px - OOT_MENU_MID_X0) / (float)(OOT_MENU_MID_X1 - OOT_MENU_MID_X0);
    return isfinite(f) ? fminf(1, fmaxf(0, f)) : 0;
}

/* ---- where it hangs ---- */
static const float OOT_MENU_WIDTH_M = 1.4f, OOT_MENU_AHEAD_M = 1.2f, OOT_MENU_BELOW_M = 0.1f;
typedef struct { float c[3], r[3], u[3]; } oot_menu_quad;   /* centre, full-width right, full-height up */
/* The panel for a head at p looking along heading yaw (radians, 0 along -z, + to the right): upright, facing the head. */
static inline oot_menu_quad oot_menu_place(const float p[3], float yaw) {
    const float fx = sinf(yaw), fz = -cosf(yaw), w = OOT_MENU_WIDTH_M, h = OOT_MENU_WIDTH_M * (float)OOT_MENU_H / OOT_MENU_W;
    oot_menu_quad q = { { p[0] + fx * OOT_MENU_AHEAD_M, p[1] - OOT_MENU_BELOW_M, p[2] + fz * OOT_MENU_AHEAD_M },
                        { cosf(yaw) * w, 0, sinf(yaw) * w }, { 0, h, 0 } };
    return q;
}

/* ---- the open button ---- */
enum { OOT_MENU_BUTTON_OFF = 0, OOT_MENU_BUTTON_L3 = 1, OOT_MENU_BUTTON_MENU = 2 };
static const uint64_t OOT_MENU_L3_NS = 1000000000ull, OOT_MENU_MENU_NS = 1500000000ull, OOT_MENU_SETTLE_NS = 200000000ull;
enum { OOT_MENU_PAD_A = 1, OOT_MENU_PAD_B = 2, OOT_MENU_PAD_X = 4, OOT_MENU_PAD_Y = 8, OOT_MENU_PAD_MENU = 16, OOT_MENU_PAD_LSTICK = 32, OOT_MENU_PAD_RSTICK = 64 };
typedef struct { uint64_t at; int down, clean, fired; } oot_menu_open_state;
/* One reading of the controllers: buttons (VR_PAD_*), `busy` = anything else pushed (a stick off centre, a trigger or a grip past
   half). Returns 1 when the menu is to open or close now. */
static inline int oot_menu_open_step(oot_menu_open_state *s, uint64_t now, uint32_t buttons, int busy, int option) {
    uint32_t key; uint64_t hold;
    if (option == OOT_MENU_BUTTON_L3) { key = OOT_MENU_PAD_LSTICK; hold = OOT_MENU_L3_NS; }
    else if (option == OOT_MENU_BUTTON_MENU) { key = OOT_MENU_PAD_MENU; hold = OOT_MENU_MENU_NS; }
    else { memset(s, 0, sizeof *s); return 0; }
    const int down = (buttons & key) != 0, others = (buttons & ~key) != 0 || busy;
    if (!down) { s->down = s->fired = 0; return 0; }
    if (!s->down) { s->down = 1; s->at = now; s->clean = !others; s->fired = 0; }
    if (others) s->clean = 0;
    if (s->clean && !s->fired && now - s->at >= hold) { s->fired = 1; return 1; }
    return 0;
}

/* ---- the controllers kept from the game ---- */
typedef struct { int holding; uint64_t free_at; } oot_menu_mask;
/* open: the menu is open. any_down: a button or a trigger is still down (the grips are not counted: what a hand holds stays
   held). Returns 1 while the game is to get nothing from the controllers. */
static inline int oot_menu_mask_step(oot_menu_mask *m, uint64_t now, int open, int any_down) {
    if (open) { m->holding = 1; m->free_at = 0; return 1; }
    if (!m->holding) return 0;
    if (any_down) { m->free_at = 0; return 1; }
    if (!m->free_at) m->free_at = now + OOT_MENU_SETTLE_NS;   /* a pulse the pad sends on a let go (L3, 120 ms) ends inside this */
    if (now >= m->free_at) { m->holding = 0; m->free_at = 0; return 0; }
    return 1;
}
