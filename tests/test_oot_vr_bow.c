/* Aiming Ocarina of Time's bow with both hands (zeldaVR/runtime/oot_vr_bow.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_bow.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

int main(void) {
    oot_bow_out o;
    const float draw[3] = { 0, 0, 0 };
    float bow[3] = { 0, 0, -0.5f };

    oot_bow_step(70, bow, draw, &o);                                  /* the game is not aiming */
    assert(!o.aiming && !o.holding && o.stick[0] == 0);
    oot_bow_step(40, bow, draw, &o);                                  /* straight ahead: no correction */
    assert(o.aiming && o.holding && o.stick[0] == 0 && o.stick[1] == 0 && NEAR(o.dir[2], -1));

    /* a small error is at least the stick's 0.22; a larger one six times the error */
    const float a = 3 * (float)M_PI / 180;
    bow[0] = sinf(a) * 0.5f; bow[2] = -cosf(a) * 0.5f;
    oot_bow_step(40, bow, draw, &o); assert(NEAR(o.stick[0], fmaxf(0.22f, a * 6)));
    const float b = 6 * (float)M_PI / 180;
    bow[0] = -sinf(b) * 0.5f; bow[2] = -cosf(b) * 0.5f;
    oot_bow_step(40, bow, draw, &o); assert(NEAR(o.stick[0], -b * 6));
    const float c = 40 * (float)M_PI / 180;
    bow[0] = sinf(c) * 0.5f; bow[2] = -cosf(c) * 0.5f;
    oot_bow_step(40, bow, draw, &o); assert(NEAR(o.stick[0], 1));     /* at most full */
    /* inside the dead zone */
    const float e = 0.3f * (float)M_PI / 180;
    bow[0] = sinf(e) * 0.5f; bow[2] = -cosf(e) * 0.5f;
    oot_bow_step(40, bow, draw, &o); assert(o.stick[0] == 0);
    /* up is up */
    bow[0] = 0; bow[1] = sinf(b) * 0.5f; bow[2] = -cosf(b) * 0.5f;
    oot_bow_step(40, bow, draw, &o); assert(NEAR(o.stick[1], b * 6) && o.stick[0] == 0);

    /* hands too close or too far apart do not aim */
    float near_hands[3] = { 0, 0, -0.1f }, far_hands[3] = { 0, 0, -1.0f };
    oot_bow_step(40, near_hands, draw, &o); assert(o.aiming && !o.holding && o.stick[0] == 0);
    oot_bow_step(40, far_hands, draw, &o); assert(o.aiming && !o.holding);
    assert(!oot_bow_is_aiming(0) && !oot_bow_is_aiming(46.5f) && oot_bow_is_aiming(46.4f));
    puts("OOT two-handed aim: when the game aims, the hands' line, the stick's floor, dead zone and limit");
    return 0;
}
