/* Ocarina of Time's button map for the Quest's controllers, without the runtime (tests/test_oot_vr_pad.c runs it). The
   layout follows the game's own buttons, as OcarinaVR's does (ideas only, GPL-3):
     A B X Y as they are, left trigger ZL, right trigger ZR, right stick click R3
     Menu: a tap is +, held 0.6 s recentres
     left stick click alone is L3; held, it is a shift:
       right stick the d-pad, right stick click -, grips L and R, Menu head view / big screen, Y first / third person
   In first person the right stick turns the view (snap or smooth) and never reaches the game. Times in nanoseconds. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

enum { OOT_PAD_A = 1, OOT_PAD_B = 2, OOT_PAD_X = 4, OOT_PAD_Y = 8, OOT_PAD_MENU = 16, OOT_PAD_LSTICK = 32, OOT_PAD_RSTICK = 64 };
enum { OOT_SW_A = 0, OOT_SW_B, OOT_SW_X, OOT_SW_Y, OOT_SW_L3, OOT_SW_R3, OOT_SW_L, OOT_SW_R, OOT_SW_ZL, OOT_SW_ZR, OOT_SW_PLUS,
       OOT_SW_MINUS, OOT_SW_LEFT, OOT_SW_UP, OOT_SW_RIGHT, OOT_SW_DOWN };

typedef struct {
    uint32_t buttons;
    float ltrig, rtrig, lgrip, rgrip;
    float stick[4];       /* lx, ly, rx, ry, dead zone already taken out */
    int first_person;     /* the view is at his eyes: the right stick turns it */
    int grips_are_buttons;/* no gestures: the grips are L and R */
    float snap_deg;       /* 0: smooth */
    float move_turn;      /* radians the left stick is turned by, clockwise (the view against the direction he should go) */
} oot_pad_in;

typedef struct {
    uint64_t bits;
    float stick[4];
    float yaw;            /* radians to add to the first-person heading, + is to the right */
    int recenter, toggle_person, toggle_screen;
} oot_pad_out;

typedef struct {
    uint32_t last;
    uint64_t menu_at, l3_at, pulse_until, at;
    uint64_t pulse_bits;
    int menu_long, chord_used, snap_armed, started;
} oot_pad_state;

enum { OOT_PAD_PULSE_NS = 120000000, OOT_PAD_RECENTER_NS = 600000000 };

static inline void oot_pad_pulse(oot_pad_state *s, uint64_t now, uint64_t bits) { s->pulse_bits = bits; s->pulse_until = now + OOT_PAD_PULSE_NS; }

static inline void oot_pad_step(oot_pad_state *s, uint64_t now, const oot_pad_in *in, oot_pad_out *out) {
    const uint64_t B = 1;
    if (!s->started) { s->started = 1; s->snap_armed = 1; s->at = now; }
    memset(out, 0, sizeof *out);
    const uint32_t b = in->buttons, pressed = b & ~s->last, released = s->last & ~b;
    const float dt = fminf((float)(now - s->at) / 1e9f, 0.05f);
    s->at = now;
    float rx = in->stick[2], ry = in->stick[3];
    const int shift = (b & OOT_PAD_LSTICK) != 0;

    if (pressed & OOT_PAD_LSTICK) { s->l3_at = now; s->chord_used = 0; }
    if (b & OOT_PAD_A) out->bits |= B << OOT_SW_A;
    if (b & OOT_PAD_B) out->bits |= B << OOT_SW_B;
    if (b & OOT_PAD_X) out->bits |= B << OOT_SW_X;
    if (in->ltrig > 0.5f) out->bits |= B << OOT_SW_ZL;
    if (in->rtrig > 0.5f) out->bits |= B << OOT_SW_ZR;

    if (shift) {
        if (fabsf(rx) > 0.5f || fabsf(ry) > 0.5f) {
            s->chord_used = 1;
            if (fabsf(rx) > fabsf(ry)) out->bits |= B << (rx > 0 ? OOT_SW_RIGHT : OOT_SW_LEFT);
            else out->bits |= B << (ry > 0 ? OOT_SW_UP : OOT_SW_DOWN);
        }
        if (b & OOT_PAD_RSTICK) { s->chord_used = 1; out->bits |= B << OOT_SW_MINUS; }
        if (in->lgrip > 0.5f) { s->chord_used = 1; out->bits |= B << OOT_SW_L; }
        if (in->rgrip > 0.5f) { s->chord_used = 1; out->bits |= B << OOT_SW_R; }
        if (pressed & OOT_PAD_MENU) { s->chord_used = 1; out->toggle_screen = 1; }
        if (pressed & OOT_PAD_Y) { s->chord_used = 1; out->toggle_person = 1; }
        s->menu_at = 0; s->menu_long = 0;
        rx = ry = 0;
    } else {
        if (b & OOT_PAD_Y) out->bits |= B << OOT_SW_Y;
        if (b & OOT_PAD_RSTICK) out->bits |= B << OOT_SW_R3;
        if (in->grips_are_buttons) {
            if (in->lgrip > 0.5f) out->bits |= B << OOT_SW_L;
            if (in->rgrip > 0.5f) out->bits |= B << OOT_SW_R;
        }
        if (pressed & OOT_PAD_MENU) { s->menu_at = now; s->menu_long = 0; }
        if ((b & OOT_PAD_MENU) && s->menu_at && !s->menu_long && now - s->menu_at >= OOT_PAD_RECENTER_NS) { s->menu_long = 1; out->recenter = 1; }
        if ((released & OOT_PAD_MENU) && s->menu_at) { if (!s->menu_long) oot_pad_pulse(s, now, B << OOT_SW_PLUS); s->menu_at = 0; }
    }
    if ((released & OOT_PAD_LSTICK) && !s->chord_used) oot_pad_pulse(s, now, B << OOT_SW_L3);
    if (now < s->pulse_until) out->bits |= s->pulse_bits;

    if (in->first_person && !shift) {
        if (in->snap_deg > 0) {
            if (s->snap_armed && fabsf(rx) > 0.6f) { s->snap_armed = 0; out->yaw = copysignf(in->snap_deg * (float)M_PI / 180, rx); }
            else if (fabsf(rx) < 0.3f) s->snap_armed = 1;
        } else if (fabsf(rx) > 0.24f) out->yaw = rx * 120 * (float)M_PI / 180 * dt;
        rx = ry = 0;
    }
    const float c = cosf(in->move_turn), t = sinf(in->move_turn);
    out->stick[0] = in->stick[0] * c + in->stick[1] * t;
    out->stick[1] = -in->stick[0] * t + in->stick[1] * c;
    out->stick[2] = rx; out->stick[3] = ry;
    s->last = b;
}
