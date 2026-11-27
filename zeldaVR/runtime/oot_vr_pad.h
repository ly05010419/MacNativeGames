/* Ocarina of Time's button map for the Quest's controllers, without the runtime (tests/test_oot_vr_pad.c runs it). The
   layout follows the game's own buttons, as OcarinaVR's does (ideas only, GPL-3):
     A B X Y as they are, left trigger ZL, right trigger ZR, right stick click R3
     Menu: a tap is +, held 0.6 s recentres; held with A pressed, the wrist calibration (zeldaVR/runtime/oot_vr_wristcal.h: that A
       is not the game's until it is let go, and the Menu press gives no + and no later recentre)
     left stick click alone is L3; held, it is a shift:
       right stick the d-pad, right stick click -, grips L and R, Menu head view / big screen, Y first / third person,
       A the item wheel (oot_vr_wheel.h) open / closed, B the equipment wheel
   In first person the right stick turns the view (snap or smooth, at smooth_dps) and never reaches the game; pushed up (within
   30 degrees of up, from the middle) it presses X, the jump, held 100 ms and until it comes back below half (HookshotVR's
   right-stick jump, ideas only: one push, one jump, so the left controller's upper button is free).
   With quick_select on (first person), a clean left stick click is no L3: it holds L for the game, whose own quick select opens
   while L is held and takes the item when L is let go; a second click, the left stick pushed past 0.6 and back under 0.25, or
   15 s let it go (HookshotVR's "game quick select from the stick", ideas only). While the item wheel is open
   the right stick is the wheel's alone and B closes it; that B is not the game's until it is let go. Times in nanoseconds. */
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
    int wheel_open;       /* the item wheel is shown: the right stick and B are its own */
    float smooth_dps;     /* smooth turning, degrees a second at the stick's full push (0: 120) */
    int stick_jump;       /* first person: the right stick pushed up jumps */
    int quick_select;     /* first person: a left stick click holds L for the game's own quick select */
} oot_pad_in;

typedef struct {
    uint64_t bits;
    float stick[4];
    float yaw;            /* radians to add to the first-person heading, + is to the right */
    int recenter, toggle_person, toggle_screen, toggle_mono;
    int toggle_wheel, toggle_gear, wheel_close;
    float wheel_stick[2]; /* the right stick while the wheel is open (0 0 otherwise) */
    int calibrate;        /* Menu held + A: record the wrists (vr.md row 31) */
} oot_pad_out;

typedef struct {
    uint32_t last;
    uint64_t menu_at, l3_at, pulse_until, at;
    uint64_t pulse_bits;
    int menu_long, chord_used, snap_armed, started, b_is_wheels, stick_is_wheels;
    int jump_ready, jumping; uint64_t jump_at;
    int quick, quick_pushed; uint64_t quick_at;
    int a_is_cal;         /* the A of a Menu + A calibration, held: not the game's */
} oot_pad_state;

enum { OOT_PAD_PULSE_NS = 120000000, OOT_PAD_RECENTER_NS = 600000000, OOT_PAD_JUMP_NS = 100000000 };
static const uint64_t OOT_PAD_QUICK_NS = 15000000000ull;

static inline void oot_pad_pulse(oot_pad_state *s, uint64_t now, uint64_t bits) { s->pulse_bits = bits; s->pulse_until = now + OOT_PAD_PULSE_NS; }

static inline void oot_pad_step(oot_pad_state *s, uint64_t now, const oot_pad_in *in, oot_pad_out *out) {
    const uint64_t B = 1;
    if (!s->started) { s->started = 1; s->snap_armed = 1; s->jump_ready = 1; s->at = now; }
    memset(out, 0, sizeof *out);
    const uint32_t b = in->buttons, pressed = b & ~s->last, released = s->last & ~b;
    const float dt = fminf((float)(now - s->at) / 1e9f, 0.05f);
    s->at = now;
    float rx = in->stick[2], ry = in->stick[3];
    const int shift = (b & OOT_PAD_LSTICK) != 0;

    if (pressed & OOT_PAD_LSTICK) { s->l3_at = now; s->chord_used = 0; }
    const int calibrate = !shift && (b & OOT_PAD_MENU) && (pressed & OOT_PAD_A);
    if (calibrate) { s->a_is_cal = 1; out->calibrate = 1; }
    if (!(b & OOT_PAD_A)) s->a_is_cal = 0;
    if ((b & OOT_PAD_A) && !shift && !s->a_is_cal) out->bits |= B << OOT_SW_A;
    if (in->wheel_open && (pressed & OOT_PAD_B)) { s->b_is_wheels = 1; out->wheel_close = 1; }
    if (!(b & OOT_PAD_B)) s->b_is_wheels = 0;
    if ((b & OOT_PAD_B) && !s->b_is_wheels && !shift) out->bits |= B << OOT_SW_B;
    if ((b & OOT_PAD_X) && !shift) out->bits |= B << OOT_SW_X;
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
        if (pressed & OOT_PAD_X) { s->chord_used = 1; out->toggle_mono = 1; }
        if (b & OOT_PAD_A) s->chord_used = 1;
        if (pressed & OOT_PAD_A) out->toggle_wheel = 1;
        if (b & OOT_PAD_B) s->chord_used = 1;
        if ((pressed & OOT_PAD_B) && !in->wheel_open) out->toggle_gear = 1;
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
        if (calibrate) s->menu_long = 1;   /* this Menu press was the calibration's */
        if ((b & OOT_PAD_MENU) && s->menu_at && !s->menu_long && now - s->menu_at >= OOT_PAD_RECENTER_NS) { s->menu_long = 1; out->recenter = 1; }
        if ((released & OOT_PAD_MENU) && s->menu_at) { if (!s->menu_long) oot_pad_pulse(s, now, B << OOT_SW_PLUS); s->menu_at = 0; }
    }
    if ((released & OOT_PAD_LSTICK) && !s->chord_used) {
        if (in->quick_select && in->first_person) {   /* the game's quick select: L held from this click to the next */
            s->quick = !s->quick; s->quick_at = now; s->quick_pushed = 0;
        } else oot_pad_pulse(s, now, B << OOT_SW_L3);
    }
    if (s->quick) {
        const float push = sqrtf(in->stick[0] * in->stick[0] + in->stick[1] * in->stick[1]);
        if (push > 0.6f) s->quick_pushed = 1;
        /* the game's own menu takes the view while it is open (no first person then): that does not let L go */
        if ((s->quick_pushed && push < 0.25f) || now - s->quick_at >= OOT_PAD_QUICK_NS || !in->quick_select) s->quick = 0;
        else if (!shift) out->bits |= B << OOT_SW_L;
    }
    if (now < s->pulse_until) out->bits |= s->pulse_bits;

    /* a push made while the wheel is open stays the wheel's until the stick is back in the middle, also once the wheel has closed */
    const int pushed = fabsf(rx) > 0.25f || fabsf(ry) > 0.25f;
    if (in->wheel_open && pushed) s->stick_is_wheels = 1;
    if (!pushed) s->stick_is_wheels = 0;
    if (in->wheel_open && !shift) { out->wheel_stick[0] = rx; out->wheel_stick[1] = ry; rx = ry = 0; }
    else if (s->stick_is_wheels) rx = ry = 0;
    if (in->first_person && !shift && in->stick_jump && !in->wheel_open && !s->stick_is_wheels) {
        if (s->jumping) {
            if (ry < 0.5f && now - s->jump_at >= OOT_PAD_JUMP_NS) s->jumping = 0;
            else out->bits |= B << OOT_SW_X;
        } else if (s->jump_ready && ry > 0.7f && fabsf(rx) < 0.577f * ry) {
            s->jump_ready = 0; s->jumping = 1; s->jump_at = now; out->bits |= B << OOT_SW_X;
        }
        if (fabsf(rx) > 0.5f) s->jump_ready = 0;   /* a turn, not a jump */
        else if (!s->jumping && fabsf(rx) < 0.3f && fabsf(ry) < 0.3f) s->jump_ready = 1;
    } else { s->jumping = 0; s->jump_ready = 1; }
    if (in->first_person && !shift) {
        if (in->snap_deg > 0) {
            if (s->snap_armed && fabsf(rx) > 0.6f) { s->snap_armed = 0; out->yaw = copysignf(in->snap_deg * (float)M_PI / 180, rx); }
            else if (fabsf(rx) < 0.3f) s->snap_armed = 1;
        } else if (fabsf(rx) > 0.24f) out->yaw = rx * (in->smooth_dps > 0 ? in->smooth_dps : 120) * (float)M_PI / 180 * dt;
        rx = ry = 0;
    }
    const float c = cosf(in->move_turn), t = sinf(in->move_turn);
    out->stick[0] = in->stick[0] * c + in->stick[1] * t;
    out->stick[1] = -in->stick[0] * t + in->stick[1] * c;
    out->stick[2] = rx; out->stick[3] = ry;
    s->last = b;
}
