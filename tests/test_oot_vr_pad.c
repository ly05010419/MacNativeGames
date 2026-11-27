/* Ocarina of Time's button map for the Quest's controllers (zeldaVR/runtime/oot_vr_pad.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_pad.h"

#define MS 1000000ull
#define BIT(x) (1ull << OOT_SW_##x)

static oot_pad_state s;
static uint64_t t = 1000 * MS;
static oot_pad_out step(oot_pad_in in, uint64_t after_ms) { t += after_ms * MS; oot_pad_out o; oot_pad_step(&s, t, &in, &o); return o; }
static void reset(void) { memset(&s, 0, sizeof s); }

int main(void) {
    oot_pad_in n = {0}, in;
    oot_pad_out o;

    reset();
    in = n; in.buttons = OOT_PAD_A | OOT_PAD_B | OOT_PAD_X | OOT_PAD_Y; in.ltrig = in.rtrig = 0.9f;
    o = step(in, 10);
    assert(o.bits == (BIT(A) | BIT(B) | BIT(X) | BIT(Y) | BIT(ZL) | BIT(ZR)));
    in = n; in.buttons = OOT_PAD_RSTICK; o = step(in, 10); assert(o.bits == BIT(R3));
    in = n; in.lgrip = in.rgrip = 1; o = step(in, 10); assert(!o.bits);          /* grips are the gestures' */
    in.grips_are_buttons = 1; o = step(in, 10); assert(o.bits == (BIT(L) | BIT(R)));

    /* Menu: a tap is +, held 0.6 s recentres once and gives no + */
    reset();
    in = n; in.buttons = OOT_PAD_MENU; o = step(in, 10); assert(!o.bits && !o.recenter);
    o = step(n, 100); assert(o.bits == BIT(PLUS) && !o.recenter);
    o = step(n, 130); assert(!o.bits);
    in = n; in.buttons = OOT_PAD_MENU; step(in, 10);
    o = step(in, 599); assert(!o.recenter);
    o = step(in, 2); assert(o.recenter);
    o = step(in, 100); assert(!o.recenter);
    o = step(n, 10); assert(!o.bits);

    /* left stick click alone is L3; held it is a shift and gives no L3 */
    reset();
    in = n; in.buttons = OOT_PAD_LSTICK; o = step(in, 10); assert(!o.bits);
    o = step(n, 10); assert(o.bits == BIT(L3));
    o = step(n, 130); assert(!o.bits);
    in = n; in.buttons = OOT_PAD_LSTICK; step(in, 10);
    in.stick[2] = 0.9f; o = step(in, 10); assert(o.bits == BIT(RIGHT) && o.stick[2] == 0);
    in.stick[2] = -0.9f; o = step(in, 10); assert(o.bits == BIT(LEFT));
    in.stick[2] = 0; in.stick[3] = 0.9f; o = step(in, 10); assert(o.bits == BIT(UP));
    in.stick[3] = -0.9f; o = step(in, 10); assert(o.bits == BIT(DOWN));
    in.stick[3] = 0; in.buttons |= OOT_PAD_RSTICK; o = step(in, 10); assert(o.bits == BIT(MINUS));
    in.buttons = OOT_PAD_LSTICK; in.lgrip = in.rgrip = 1; o = step(in, 10); assert(o.bits == (BIT(L) | BIT(R)));
    in.lgrip = in.rgrip = 0; in.buttons = OOT_PAD_LSTICK | OOT_PAD_Y; o = step(in, 10); assert(o.toggle_person && !(o.bits & BIT(Y)));
    o = step(in, 10); assert(!o.toggle_person);
    in.buttons = OOT_PAD_LSTICK | OOT_PAD_MENU; o = step(in, 10); assert(o.toggle_screen && !o.recenter);
    o = step(n, 10); assert(!(o.bits & BIT(L3)) && !(o.bits & BIT(PLUS)));

    /* first person: snap turning, once a push, the stick does not reach the game */
    reset();
    in = n; in.first_person = 1; in.snap_deg = 30; in.stick[2] = 0.9f; in.stick[3] = 0.9f;
    o = step(in, 10); assert(fabsf(o.yaw - (float)M_PI / 6) < 1e-5f && o.stick[2] == 0 && o.stick[3] == 0);
    o = step(in, 10); assert(o.yaw == 0);
    in.stick[2] = 0.1f; step(in, 10);
    in.stick[2] = -0.9f; o = step(in, 10); assert(fabsf(o.yaw + (float)M_PI / 6) < 1e-5f);
    /* smooth: 120 degrees a second, a long gap counts as 50 ms */
    reset();
    in = n; in.first_person = 1; in.stick[2] = 1; step(in, 10);
    o = step(in, 10); assert(fabsf(o.yaw - 120 * (float)M_PI / 180 * 0.01f) < 1e-4f);
    o = step(in, 500); assert(fabsf(o.yaw - 120 * (float)M_PI / 180 * 0.05f) < 1e-4f);
    /* smooth at another speed: 240 degrees a second */
    reset();
    in = n; in.first_person = 1; in.smooth_dps = 240; in.stick[2] = 1; step(in, 10);
    o = step(in, 10); assert(fabsf(o.yaw - 240 * (float)M_PI / 180 * 0.01f) < 1e-4f);
    /* first person, the right stick pushed up jumps: X held at least 100 ms and until the stick is below half, one push one jump */
    reset();
    in = n; in.first_person = 1; in.stick_jump = 1; in.snap_deg = 30; step(in, 10);
    in.stick[3] = 0.9f; o = step(in, 10); assert((o.bits & BIT(X)) && o.yaw == 0 && o.stick[3] == 0);
    in.stick[3] = 0.4f; o = step(in, 50); assert(o.bits & BIT(X));                 /* held the first 100 ms */
    o = step(in, 60); assert(!(o.bits & BIT(X)));
    in.stick[3] = 0.9f; o = step(in, 10); assert(!(o.bits & BIT(X)));              /* not back in the middle: no second jump */
    in.stick[3] = 0; step(in, 10);
    in.stick[3] = 0.9f; o = step(in, 10); assert(o.bits & BIT(X));
    o = step(in, 500); assert(o.bits & BIT(X));                                    /* held while pushed */
    in.stick[3] = 0; o = step(in, 10); assert(!(o.bits & BIT(X)));
    /* a push sideways (a turn) is no jump, even when it then goes up */
    in.stick[2] = 0.9f; in.stick[3] = 0.2f; o = step(in, 10); assert(!(o.bits & BIT(X)) && o.yaw != 0);
    in.stick[2] = 0.2f; in.stick[3] = 0.9f; o = step(in, 10); assert(!(o.bits & BIT(X)));
    in.stick[2] = 0; in.stick[3] = 0; step(in, 10);
    in.stick[2] = 0.6f; in.stick[3] = 0.8f; o = step(in, 10); assert(!(o.bits & BIT(X)));   /* more than 30 degrees off up */
    /* off, or third person, or the wheel open: no jump */
    reset();
    in = n; in.first_person = 1; in.stick[3] = 0.9f; o = step(in, 10); assert(!(o.bits & BIT(X)));
    reset();
    in = n; in.stick_jump = 1; in.stick[3] = 0.9f; o = step(in, 10); assert(!(o.bits & BIT(X)) && o.stick[3] == 0.9f);
    reset();
    in = n; in.first_person = 1; in.stick_jump = 1; in.wheel_open = 1; in.stick[3] = 0.9f; o = step(in, 10); assert(!(o.bits & BIT(X)));
    /* the game's quick select: a clean left stick click holds L until the next click */
    reset();
    in = n; in.first_person = 1; in.quick_select = 1; in.buttons = OOT_PAD_LSTICK; o = step(in, 10); assert(!o.bits);
    in.buttons = 0; o = step(in, 10); assert(o.bits == BIT(L));                    /* no L3: L held */
    o = step(in, 1000); assert(o.bits == BIT(L));
    in.first_person = 0; o = step(in, 10); assert(o.bits == BIT(L));                /* the game's menu has the view: still held */
    in.first_person = 1;
    in.buttons = OOT_PAD_LSTICK; step(in, 10); in.buttons = 0; o = step(in, 10); assert(!(o.bits & BIT(L)) && !(o.bits & BIT(L3)));
    /* ... or the left stick pushed out and back: let go (the game takes the item) */
    in.buttons = OOT_PAD_LSTICK; step(in, 10); in.buttons = 0; o = step(in, 10); assert(o.bits == BIT(L));
    in.stick[0] = 0.9f; o = step(in, 10); assert(o.bits & BIT(L));
    in.stick[0] = 0.1f; o = step(in, 10); assert(!(o.bits & BIT(L)));
    /* ... or 15 s */
    in.stick[0] = 0; in.buttons = OOT_PAD_LSTICK; step(in, 10); in.buttons = 0; o = step(in, 10); assert(o.bits == BIT(L));
    o = step(in, 15000); assert(!(o.bits & BIT(L)));
    /* off, or third person: the click is L3 as before */
    reset();
    in = n; in.first_person = 1; in.buttons = OOT_PAD_LSTICK; step(in, 10); in.buttons = 0; o = step(in, 10); assert(o.bits == BIT(L3));
    reset();
    in = n; in.quick_select = 1; in.buttons = OOT_PAD_LSTICK; step(in, 10); in.buttons = 0; o = step(in, 10); assert(o.bits == BIT(L3));
    /* third person: the right stick is the game's */
    reset();
    in = n; in.stick[2] = 0.7f; in.stick[3] = 0.4f; o = step(in, 10); assert(o.stick[2] == 0.7f && o.stick[3] == 0.4f && !o.yaw);

    /* the left stick turned clockwise by move_turn */
    reset();
    in = n; in.stick[1] = 1; in.move_turn = (float)M_PI / 2; o = step(in, 10);
    assert(fabsf(o.stick[0] - 1) < 1e-5f && fabsf(o.stick[1]) < 1e-5f);
    /* the left stick pressed + X: one picture for both eyes; X alone is the game's X */
    in = n; in.buttons = OOT_PAD_LSTICK; step(in, 10);
    in.buttons = OOT_PAD_LSTICK | OOT_PAD_X; o = step(in, 10); assert(o.toggle_mono && !(o.bits & (1ull << OOT_SW_X)));
    in = n; step(in, 10);
    in.buttons = OOT_PAD_X; o = step(in, 10); assert(!o.toggle_mono && (o.bits & (1ull << OOT_SW_X)));
    /* the item wheel: the left stick pressed + A opens and closes it, and that A is not the game's */
    reset();
    in = n; in.buttons = OOT_PAD_LSTICK; step(in, 10);
    in.buttons = OOT_PAD_LSTICK | OOT_PAD_A; o = step(in, 10); assert(o.toggle_wheel && !(o.bits & BIT(A)));
    o = step(in, 10); assert(!o.toggle_wheel && !(o.bits & BIT(A)));
    o = step(n, 10); assert(!(o.bits & BIT(L3)));
    in = n; in.buttons = OOT_PAD_A; o = step(in, 200); assert(o.bits == BIT(A) && !o.toggle_wheel);   /* A alone is the game's A */
    /* the left stick pressed + B: the equipment wheel, and that B is not the game's */
    reset();
    in = n; in.buttons = OOT_PAD_LSTICK; step(in, 10);
    in.buttons = OOT_PAD_LSTICK | OOT_PAD_B; o = step(in, 10); assert(o.toggle_gear && !o.toggle_wheel && !(o.bits & BIT(B)));
    o = step(in, 10); assert(!o.toggle_gear && !(o.bits & BIT(B)));
    o = step(n, 10); assert(!(o.bits & BIT(L3)));
    /* open, the right stick is the wheel's: it does not turn the view and does not reach the game */
    reset();
    in = n; in.wheel_open = 1; in.first_person = 1; in.snap_deg = 30; in.stick[2] = 0.9f; in.stick[3] = 0.2f;
    o = step(in, 10); assert(o.yaw == 0 && o.stick[2] == 0 && o.stick[3] == 0 && o.wheel_stick[0] == 0.9f && o.wheel_stick[1] == 0.2f);
    in.first_person = 0; o = step(in, 10); assert(o.stick[2] == 0 && o.stick[3] == 0 && o.wheel_stick[0] == 0.9f);
    /* B closes it, and that press is not the game's B, however long it is held after the wheel has gone */
    in = n; in.wheel_open = 1; in.buttons = OOT_PAD_B; o = step(in, 10); assert(o.wheel_close && !(o.bits & BIT(B)));
    in.wheel_open = 0; o = step(in, 10); assert(!o.wheel_close && !(o.bits & BIT(B)));
    o = step(n, 10); in = n; in.buttons = OOT_PAD_B; o = step(in, 10); assert(o.bits == BIT(B) && !o.wheel_close);
    /* closed with the stick still pushed: that push stays the wheel's, it does not turn the view; back in the middle, it is the view's */
    reset();
    in = n; in.wheel_open = 1; in.first_person = 1; in.snap_deg = 30; in.stick[2] = 0.9f; o = step(in, 10); assert(o.yaw == 0);
    in.wheel_open = 0; o = step(in, 10); assert(o.yaw == 0 && o.stick[2] == 0 && o.wheel_stick[0] == 0);
    o = step(in, 10); assert(o.yaw == 0);
    in.stick[2] = 0.1f; o = step(in, 10); assert(o.yaw == 0);
    in.stick[2] = 0.9f; o = step(in, 10); assert(fabsf(o.yaw - (float)M_PI / 6) < 1e-5f);
    /* closed, nothing of it: the stick is the game's and the view's again */
    reset();
    in = n; in.stick[2] = 0.9f; o = step(in, 10); assert(o.stick[2] == 0.9f && o.wheel_stick[0] == 0 && o.wheel_stick[1] == 0);
    puts("OOT Quest controller map: buttons, Menu tap / hold, the left-stick shift, turning and its speed, the jump on the right stick, the game's quick select, the move direction, the item wheel");
    return 0;
}
