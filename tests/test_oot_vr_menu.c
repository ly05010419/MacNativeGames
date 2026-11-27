/* The VR menu's layout, open button and mask (zeldaVR/runtime/oot_vr_menu.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_menu.h"

#define NEAR(a, b, e) (fabsf((a) - (b)) < (e))
#define PX(x) ((float)(x) / OOT_MENU_W)
#define PY(y) ((float)(y) / OOT_MENU_H)
static const uint64_t MS = 1000000ull;

int main(void) {
    /* the layout: 23 rows are three pages; a row's buttons by kind */
    int kinds[23];
    for (int i = 0; i < 23; i++) kinds[i] = i % 4;   /* switch, choices, slider, info, ... */
    assert(oot_menu_pages(23) == 3 && oot_menu_pages(0) == 1 && oot_menu_pages(10) == 1 && oot_menu_pages(11) == 2);
    const float row1 = PY(OOT_MENU_TOP + OOT_MENU_ROW_H + OOT_MENU_ROW_H / 2);   /* the second row of a page */
    oot_menu_hit h = oot_menu_hit_at(PX(200), row1, 0, 23, kinds);
    assert(h.kind == OOT_MENU_HIT_ROW && h.row == 1);
    h = oot_menu_hit_at(PX(980), row1, 0, 23, kinds); assert(h.kind == OOT_MENU_HIT_LESS && h.row == 1);
    h = oot_menu_hit_at(PX(1520), row1, 0, 23, kinds); assert(h.kind == OOT_MENU_HIT_MORE && h.row == 1);
    h = oot_menu_hit_at(PX(1200), row1, 0, 23, kinds); assert(h.kind == OOT_MENU_HIT_ROW && h.row == 1);   /* choices: the middle is the row */
    const float row2 = PY(OOT_MENU_TOP + 2 * OOT_MENU_ROW_H + 10);
    h = oot_menu_hit_at(PX(1255), row2, 0, 23, kinds); assert(h.kind == OOT_MENU_HIT_TRACK && h.row == 2 && NEAR(h.frac, 0.5f, 1e-3f));
    const float row3 = PY(OOT_MENU_TOP + 3 * OOT_MENU_ROW_H + 10);
    h = oot_menu_hit_at(PX(980), row3, 0, 23, kinds); assert(h.kind == OOT_MENU_HIT_ROW && h.row == 3);    /* info: no buttons */
    h = oot_menu_hit_at(PX(980), row3, 2, 23, kinds); assert(h.kind == OOT_MENU_HIT_NONE);                /* page 3 has 3 rows (20-22) */
    h = oot_menu_hit_at(PX(980), PY(OOT_MENU_TOP + 10), 2, 23, kinds); assert(h.kind == OOT_MENU_HIT_LESS && h.row == 20);
    const float foot = PY(OOT_MENU_FOOT_Y0 + 40);
    assert(oot_menu_hit_at(PX(100), foot, 0, 23, kinds).kind == OOT_MENU_HIT_NONE);    /* no page before the first */
    assert(oot_menu_hit_at(PX(1300), foot, 0, 23, kinds).kind == OOT_MENU_HIT_NEXT);
    assert(oot_menu_hit_at(PX(100), foot, 2, 23, kinds).kind == OOT_MENU_HIT_PREV);
    assert(oot_menu_hit_at(PX(1300), foot, 2, 23, kinds).kind == OOT_MENU_HIT_NONE);   /* none after the last */
    assert(oot_menu_hit_at(PX(1500), PY(50), 0, 23, kinds).kind == OOT_MENU_HIT_CLOSE);
    assert(oot_menu_hit_at(NAN, 0.5f, 0, 23, kinds).kind == OOT_MENU_HIT_NONE && oot_menu_hit_at(1.2f, 0.5f, 0, 23, kinds).kind == OOT_MENU_HIT_NONE);
    assert(NEAR(oot_menu_track_frac(PX(OOT_MENU_MID_X0 - 100)), 0, 1e-6f) && NEAR(oot_menu_track_frac(PX(OOT_MENU_MID_X1 + 100)), 1, 1e-6f));

    /* values */
    assert(oot_menu_slider_value(30, 80, 0.5f) == 55 && oot_menu_slider_value(-100, 100, 0) == -100 && oot_menu_slider_value(0, 300, 2) == 300);
    assert(oot_menu_slider_value(0, 10, NAN) == 0);
    assert(oot_menu_slider_step(30, 80) == 1 && oot_menu_slider_step(0, 300) == 6 && oot_menu_slider_step(5, 5) == 1);
    char out[8];
    oot_menu_copy_utf8(out, sizeof out, "\xe7\xac\xac\xe4\xb8\x80\xe4\xba\xba");   /* three 3-byte characters into 8 bytes: two fit */
    assert(strlen(out) == 6);
    oot_menu_copy_utf8(out, sizeof out, "abc"); assert(!strcmp(out, "abc"));
    oot_menu_copy_utf8(out, sizeof out, NULL); assert(!out[0]);

    /* the place: 1.2 m ahead along the heading, 0.1 m below the eyes, 1.4 m wide, facing the head */
    const float head[3] = { 0.5f, 1.6f, 1 };
    oot_menu_quad q = oot_menu_place(head, 0);
    assert(NEAR(q.c[0], 0.5f, 1e-5f) && NEAR(q.c[1], 1.5f, 1e-5f) && NEAR(q.c[2], -0.2f, 1e-5f) && NEAR(q.r[0], 1.4f, 1e-5f) && NEAR(q.u[1], 0.91f, 1e-5f));
    q = oot_menu_place(head, (float)M_PI / 2);   /* looking to the right (+x) */
    assert(NEAR(q.c[0], 1.7f, 1e-5f) && NEAR(q.c[2], 1, 1e-5f) && NEAR(q.r[2], 1.4f, 1e-5f));

    /* the open button: the left stick click held alone 1 s, once */
    oot_menu_open_state s = { 0 };
    uint64_t t = 1000 * MS;
    assert(!oot_menu_open_step(&s, t, OOT_MENU_PAD_LSTICK, 0, OOT_MENU_BUTTON_L3));
    assert(!oot_menu_open_step(&s, t + 900 * MS, OOT_MENU_PAD_LSTICK, 0, OOT_MENU_BUTTON_L3));
    assert(oot_menu_open_step(&s, t + 1000 * MS, OOT_MENU_PAD_LSTICK, 0, OOT_MENU_BUTTON_L3));
    assert(!oot_menu_open_step(&s, t + 1500 * MS, OOT_MENU_PAD_LSTICK, 0, OOT_MENU_BUTTON_L3));   /* held on: once */
    assert(!oot_menu_open_step(&s, t + 1600 * MS, 0, 0, OOT_MENU_BUTTON_L3));
    /* not as a shift chord: with the right stick (busy) or A pressed meanwhile, held as long, nothing */
    t += 5000 * MS;
    assert(!oot_menu_open_step(&s, t, OOT_MENU_PAD_LSTICK, 0, OOT_MENU_BUTTON_L3));
    assert(!oot_menu_open_step(&s, t + 300 * MS, OOT_MENU_PAD_LSTICK, 1, OOT_MENU_BUTTON_L3));
    assert(!oot_menu_open_step(&s, t + 1200 * MS, OOT_MENU_PAD_LSTICK, 0, OOT_MENU_BUTTON_L3));
    assert(!oot_menu_open_step(&s, t + 1300 * MS, 0, 0, OOT_MENU_BUTTON_L3));
    assert(!oot_menu_open_step(&s, t + 1400 * MS, OOT_MENU_PAD_LSTICK | OOT_MENU_PAD_A, 0, OOT_MENU_BUTTON_L3));
    assert(!oot_menu_open_step(&s, t + 2600 * MS, OOT_MENU_PAD_LSTICK | OOT_MENU_PAD_A, 0, OOT_MENU_BUTTON_L3));
    assert(!oot_menu_open_step(&s, t + 2700 * MS, 0, 0, OOT_MENU_BUTTON_L3));
    /* the Menu button: 1.5 s; with the left stick held it is the shift's (big screen), nothing */
    t += 5000 * MS;
    assert(!oot_menu_open_step(&s, t, OOT_MENU_PAD_MENU, 0, OOT_MENU_BUTTON_MENU));
    assert(!oot_menu_open_step(&s, t + 1000 * MS, OOT_MENU_PAD_MENU, 0, OOT_MENU_BUTTON_MENU));
    assert(oot_menu_open_step(&s, t + 1500 * MS, OOT_MENU_PAD_MENU, 0, OOT_MENU_BUTTON_MENU));
    assert(!oot_menu_open_step(&s, t + 1600 * MS, 0, 0, OOT_MENU_BUTTON_MENU));
    assert(!oot_menu_open_step(&s, t + 2000 * MS, OOT_MENU_PAD_MENU | OOT_MENU_PAD_LSTICK, 0, OOT_MENU_BUTTON_MENU));
    assert(!oot_menu_open_step(&s, t + 4000 * MS, OOT_MENU_PAD_MENU | OOT_MENU_PAD_LSTICK, 0, OOT_MENU_BUTTON_MENU));
    /* off: never */
    assert(!oot_menu_open_step(&s, t + 9000 * MS, OOT_MENU_PAD_LSTICK, 0, OOT_MENU_BUTTON_OFF));
    assert(!oot_menu_open_step(&s, t + 19000 * MS, OOT_MENU_PAD_LSTICK, 0, OOT_MENU_BUTTON_OFF));

    /* the mask: while open; after it closes, until everything is let go and 200 ms more */
    oot_menu_mask m = { 0 };
    t = 100000 * MS;
    assert(!oot_menu_mask_step(&m, t, 0, 1));                 /* never opened: the game has everything */
    assert(oot_menu_mask_step(&m, t, 1, 0));
    assert(oot_menu_mask_step(&m, t + 10 * MS, 0, 1));        /* closed with B still down */
    assert(oot_menu_mask_step(&m, t + 500 * MS, 0, 1));
    assert(oot_menu_mask_step(&m, t + 510 * MS, 0, 0));       /* let go: 200 ms more */
    assert(oot_menu_mask_step(&m, t + 700 * MS, 0, 0));
    assert(!oot_menu_mask_step(&m, t + 711 * MS, 0, 0));
    assert(!oot_menu_mask_step(&m, t + 800 * MS, 0, 1));      /* and then the game's again */
    puts("oot_vr_menu: ok");
    return 0;
}
