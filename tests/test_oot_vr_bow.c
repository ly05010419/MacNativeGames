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
    {   /* the shot leaves the hand that holds the bow, along the two hands, with the game's speed and reach */
        float start[3] = { 10, 1.2f, 5 }, vel[3] = { 0, 0, -1.875f }, target[3] = { 10, 1.2f, -25 };
        const float handle[3] = { 10.3f, 1.5f, 4.8f }, pull[3] = { 10.3f, 1.2f, 5.2f };
        assert(oot_shot_steer(handle, pull, start, vel, target));
        assert(NEAR(start[0], 10.3f) && NEAR(start[1], 1.5f) && NEAR(start[2], 4.8f));
        assert(NEAR(sqrtf(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]), 1.875f) && NEAR(vel[0], 0) && NEAR(vel[1], 1.875f * 0.6f) && NEAR(vel[2], -1.875f * 0.8f));
        assert(NEAR(target[0], 10.3f) && NEAR(target[1], 1.5f + 30 * 0.6f) && NEAR(target[2], 4.8f - 30 * 0.8f));
        /* untouched: hands together, the bow far from the game's own shot, numbers that are not sane */
        float s2[3] = { 10, 1.2f, 5 }, v2[3] = { 0, 0, -1.875f }, t2[3] = { 10, 1.2f, -25 };
        const float close_pull[3] = { 10.3f, 1.5f, 4.85f }, far_handle[3] = { 13, 1.5f, 5 }, far_pull[3] = { 13, 1.5f, 5.4f };
        assert(!oot_shot_steer(handle, close_pull, s2, v2, t2) && !oot_shot_steer(far_handle, far_pull, s2, v2, t2));
        assert(s2[0] == 10 && v2[2] == -1.875f && t2[2] == -25);
        float v0[3] = { 0, 0, 0 }, vnan[3] = { NAN, 0, 0 };
        assert(!oot_shot_steer(handle, pull, s2, v0, t2) && !oot_shot_steer(handle, pull, s2, vnan, t2) && !oot_shot_steer(handle, pull, s2, v2, s2));
    }
    puts("OOT two-handed aim: when the game aims, the hands' line, the stick's floor, dead zone and limit; the shot along the hands");
    return 0;
}
