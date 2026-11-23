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
    /* third person: the right stick is the game's */
    reset();
    in = n; in.stick[2] = 0.7f; in.stick[3] = 0.4f; o = step(in, 10); assert(o.stick[2] == 0.7f && o.stick[3] == 0.4f && !o.yaw);

    /* the left stick turned clockwise by move_turn */
    reset();
    in = n; in.stick[1] = 1; in.move_turn = (float)M_PI / 2; o = step(in, 10);
    assert(fabsf(o.stick[0] - 1) < 1e-5f && fabsf(o.stick[1]) < 1e-5f);
    puts("OOT Quest controller map: buttons, Menu tap / hold, the left-stick shift, turning, the move direction");
    return 0;
}
