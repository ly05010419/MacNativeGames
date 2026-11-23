/* Ocarina of Time's motion controls (zeldaVR/runtime/oot_vr_gesture.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_gesture.h"

#define MS 1000000ull
static oot_gesture_state s;
static uint64_t t = 1000 * MS;
static oot_hand h[2];
static oot_gesture_out step(uint64_t ms) { t += ms * MS; oot_gesture_out o; oot_gesture_step(&s, t, h, &o); return o; }
static void reset(void) {
    memset(&s, 0, sizeof s); memset(h, 0, sizeof h);
    h[0].have = h[1].have = 1; h[0].f = h[1].f = 0.3f; h[0].r = -0.2f; h[1].r = 0.2f; h[0].u = h[1].u = -0.3f;
    step(10);
}

int main(void) {
    oot_gesture_out o;

    /* draw the sword behind the right shoulder */
    reset();
    h[1].f = -0.1f; h[1].r = 0.2f; h[1].u = 0.1f; h[1].grip = 0.9f;
    o = step(10); assert(o.bits & OOT_GEST_Y); assert(s.sword); assert(o.buzz[1]);
    o = step(50); assert(o.bits & OOT_GEST_Y);
    o = step(60); assert(!(o.bits & OOT_GEST_Y));

    /* a swing: once, not again until the tip has slowed, and never within 220 ms */
    h[1].f = 0.3f; h[1].tip_speed = 0.5f; step(300);
    h[1].tip_speed = 3.0f; o = step(10); assert(o.bits & OOT_GEST_Y);
    o = step(120); assert(!(o.bits & OOT_GEST_Y));              /* still fast: no second press */
    h[1].tip_speed = 0.5f; step(10); h[1].tip_speed = 3.0f;
    o = step(10); assert(!(o.bits & OOT_GEST_Y));               /* ready again, but 150 ms after the last */
    h[1].tip_speed = 0.5f; step(100); h[1].tip_speed = 3.0f;
    o = step(10); assert(o.bits & OOT_GEST_Y);
    h[1].tip_speed = 0;

    /* the hand open for 0.45 s puts it away; a shorter opening does not */
    step(150);
    h[1].grip = 0.1f; step(10); o = step(300); assert(s.sword && !(o.bits & OOT_GEST_B));
    h[1].grip = 0.9f; step(10);
    h[1].grip = 0.1f; step(10); step(440); assert(s.sword);
    o = step(20); assert(!s.sword && (o.bits & OOT_GEST_B));

    /* grip between the two thresholds keeps the state */
    reset();
    h[1].grip = 0.5f; step(10); assert(!s.closed[1]);
    h[1].grip = 0.7f; step(10); assert(s.closed[1]);
    h[1].grip = 0.5f; step(10); assert(s.closed[1]);

    /* the shield: taken behind the left shoulder, held in front while closed, gone when the hand opens */
    reset();
    h[0].f = -0.1f; h[0].r = -0.2f; h[0].u = 0.1f; h[0].grip = 0.9f;
    o = step(10); assert(s.shield && o.buzz[0]);
    h[0].f = 0.3f; o = step(10); assert(o.bits & OOT_GEST_ZL);
    h[0].grip = 0.1f; o = step(10); assert(!s.shield && !(o.bits & OOT_GEST_ZL));

    /* pick up low and slowly, drop by opening; a fast closing picks up nothing */
    reset();
    h[1].u = -0.7f; h[1].grip = 0.9f; o = step(10); assert(s.carrying && (o.bits & OOT_GEST_A));
    step(200); h[1].grip = 0.1f; o = step(10); assert(!s.carrying && (o.bits & OOT_GEST_A));
    reset();
    h[1].u = -0.7f; h[1].v[0] = 2; h[1].grip = 0.9f; o = step(10); assert(!s.carrying && !(o.bits & OOT_GEST_A));
    /* hands closing at rest in front of the body do nothing */
    reset();
    h[0].grip = h[1].grip = 0.9f; o = step(10); assert(!o.bits && !s.sword && !s.shield && !s.carrying);
    /* a hand that is not tracked changes nothing */
    reset();
    h[1].have = 0; h[1].f = -0.1f; h[1].grip = 0.9f; o = step(10); assert(!s.sword && !o.bits);
    puts("OOT motion controls: draw, swing, put away, shield, pick up and put down");
    return 0;
}
