/* The wrist calibration's math (zeldaVR/runtime/oot_vr_wristcal.h): known turns in, the angles out, and the round trip. */
#include <stdio.h>
#include <stdlib.h>
#include "../zeldaVR/runtime/oot_vr_wristcal.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static void rot(const float q[4], const float v[3], float o[3]) {
    float m[3][3]; oot_wc_q_matrix(q, m);
    for (int i = 0; i < 3; i++) o[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
}
static void qinv(const float q[4], float o[4]) { o[0] = -q[0]; o[1] = -q[1]; o[2] = -q[2]; o[3] = q[3]; }
/* the largest difference between the hand's axes and the reference's */
static float frame_error(const float hand[4], const float head[4]) {
    float want[3][3], m[3][3], e = 0;
    if (!oot_wc_reference(head, want)) return 99;
    oot_wc_q_matrix(hand, m);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) e = fmaxf(e, fabsf(m[i][j] - want[i][j]));
    return e;
}
static float angle_diff(float a, float b) { float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f; return fabsf(d); }
/* a controller held so that the calibration must find deg: controller = reference * Rx(-tilt) * E^-1 */
static void controller_for(const float head_yaw_deg, const float deg[3], float tilt, float out[4]) {
    float yaw[4], down[4], w[4], t[4], e[4], ei[4], wt[4];
    oot_wc_axis_q(1, head_yaw_deg * (float)M_PI / 180, yaw); oot_wc_axis_q(0, (float)M_PI / 2, down);
    oot_wc_qmul(yaw, down, w);   /* the reference for a head turned by head_yaw: Ry(yaw) Rx(90) */
    oot_wc_axis_q(0, -tilt * (float)M_PI / 180, t); oot_wc_euler_q(deg, e); qinv(e, ei);
    oot_wc_qmul(w, t, wt); oot_wc_qmul(wt, ei, out);
}

int main(void) {
    const float level[4] = { 0, 0, 0, 1 };

    /* the reference itself: the controller's -z up, x right, y back for a head looking along -z */
    {
        float want[3][3]; CHECK(oot_wc_reference(level, want));
        CHECK(fabsf(want[0][0] - 1) < 1e-6f && fabsf(want[2][1] - 1) < 1e-6f && fabsf(want[1][2] + 1) < 1e-6f);
        const float s = sqrtf(0.5f), held[4] = { s, 0, 0, s };   /* Rx(90): exactly the reference */
        float deg[3] = { 9, 9, 9 }; CHECK(oot_wc_solve(held, level, 0, deg));
        CHECK(fabsf(deg[0]) < 0.01f && fabsf(deg[1]) < 0.01f && fabsf(deg[2]) < 0.01f);
        /* a controller with no turn at all (fingers down, thumb ahead): pitch 90 */
        CHECK(oot_wc_solve(level, level, 0, deg));
        CHECK(fabsf(deg[0] - 90) < 0.01f && fabsf(deg[1]) < 0.01f && fabsf(deg[2]) < 0.01f);
    }

    /* known turns in, the same angles out, for heads turned any way and any hand tilt; the hand is then the reference */
    {
        const float sets[][3] = { { 0, 0, 0 }, { 30, -20, 15 }, { -150, 60, 170 }, { 45, 85, -100 }, { -5, -70, 3 } };
        const float yaws[] = { 0, 37, -120, 179 }, tilts[] = { 0, 20, -45, 60 };
        for (unsigned s = 0; s < sizeof sets / sizeof *sets; s++)
            for (unsigned y = 0; y < sizeof yaws / sizeof *yaws; y++)
                for (unsigned t = 0; t < sizeof tilts / sizeof *tilts; t++) {
                    float raw[4], head[4], deg[3], hand[4];
                    controller_for(yaws[y], sets[s], tilts[t], raw);
                    oot_wc_axis_q(1, yaws[y] * (float)M_PI / 180, head);
                    CHECK(oot_wc_solve(raw, head, tilts[t], deg));
                    for (int k = 0; k < 3; k++) CHECK(angle_diff(deg[k], sets[s][k]) < 0.05f);
                    CHECK(oot_wc_apply(raw, deg, tilts[t], hand));
                    CHECK(frame_error(hand, head) < 1e-4f);
                }
    }

    /* only the head's level forward counts: a head looking 30 degrees down and leaning 10 degrees gives the same angles */
    {
        const float want[3] = { 25, -10, 40 };
        float raw[4], head[4], yaw[4], pitch[4], roll[4], yp[4], deg[3];
        controller_for(50, want, 0, raw);
        oot_wc_axis_q(1, 50 * (float)M_PI / 180, yaw); oot_wc_axis_q(0, -30 * (float)M_PI / 180, pitch); oot_wc_axis_q(2, 10 * (float)M_PI / 180, roll);
        oot_wc_qmul(yaw, pitch, yp); oot_wc_qmul(yp, roll, head);
        CHECK(oot_wc_solve(raw, head, 0, deg));
        for (int k = 0; k < 3; k++) CHECK(angle_diff(deg[k], want[k]) < 0.05f);
    }

    /* yaw 90 (gimbal lock): the angles differ, the hand they make is still the reference */
    {
        const float lock[3] = { 20, 90, 35 };
        float raw[4], deg[3], hand[4];
        controller_for(0, lock, 0, raw);
        CHECK(oot_wc_solve(raw, level, 0, deg));
        CHECK(fabsf(deg[1] - 90) < 0.5f);
        oot_wc_apply(raw, deg, 0, hand);
        CHECK(frame_error(hand, level) < 2e-3f);
    }

    /* the defaults (all 0) are today's hands: tilt t turns the fingers' axis (0, -1, 0) to (0, -cos t, -sin t) and leaves +x */
    {
        const float zero[3] = { 0, 0, 0 }, tilts[] = { 0, 25, -60 };
        float raw[4] = { 0.3f, -0.5f, 0.2f, 0.78f }; oot_wc_qnorm(raw);
        for (unsigned t = 0; t < sizeof tilts / sizeof *tilts; t++) {
            const float a = tilts[t] * (float)M_PI / 180, old_along[3] = { 0, -cosf(a), -sinf(a) }, old_back[3] = { 1, 0, 0 }, fingers[3] = { 0, -1, 0 };
            float hand[4], n1[3], n2[3], b1[3], b2[3];
            CHECK(oot_wc_apply(raw, zero, tilts[t], hand));
            rot(hand, fingers, n1); rot(raw, old_along, n2); rot(hand, old_back, b1); rot(raw, old_back, b2);
            for (int i = 0; i < 3; i++) { CHECK(fabsf(n1[i] - n2[i]) < 1e-5f); CHECK(fabsf(b1[i] - b2[i]) < 1e-5f); }
        }
    }

    /* whole degrees, as the options keep them: the hand within 1.5 degrees of the reference; a random sweep of controllers */
    {
        srand(31);
        float worst = 0;
        for (int n = 0; n < 4000; n++) {
            float raw[4], head[4], deg[3], whole[3], hand[4];
            for (int i = 0; i < 4; i++) raw[i] = (float)((double)rand() / RAND_MAX) * 2 - 1;
            if (!oot_wc_qnorm(raw)) continue;
            const float hy = (float)((double)rand() / RAND_MAX) * 360 - 180, tilt = (float)(rand() % 121 - 60);
            oot_wc_axis_q(1, hy * (float)M_PI / 180, head);
            CHECK(oot_wc_solve(raw, head, tilt, deg));
            CHECK(deg[0] >= -180.001f && deg[0] <= 180.001f && deg[1] >= -90.001f && deg[1] <= 90.001f && deg[2] >= -180.001f && deg[2] <= 180.001f);
            oot_wc_apply(raw, deg, tilt, hand);
            CHECK(frame_error(hand, head) < 2e-3f);
            for (int k = 0; k < 3; k++) whole[k] = (float)oot_wc_whole(deg[k]);
            oot_wc_apply(raw, whole, tilt, hand);
            /* the angle between the reached hand and the reference */
            float want[3][3], m[3][3], tr = 0; oot_wc_reference(head, want); oot_wc_q_matrix(hand, m);
            for (int i = 0; i < 3; i++) for (int k = 0; k < 3; k++) tr += m[k][i] * want[k][i];
            const float ang = acosf(fmaxf(-1, fminf(1, (tr - 1) / 2))) * 180 / (float)M_PI;
            if (ang > worst) worst = ang;
        }
        CHECK(worst < 1.5f);
    }

    /* what cannot be calibrated: nothing changes */
    {
        float deg[3] = { 7, 8, 9 }, hand[4];
        const float down[4] = { -sqrtf(0.5f), 0, 0, sqrtf(0.5f) }, nan_q[4] = { NAN, 0, 0, 1 }, none[4] = { 0, 0, 0, 0 };
        CHECK(!oot_wc_solve(level, down, 0, deg));    /* looking straight down: no body right */
        CHECK(!oot_wc_solve(nan_q, level, 0, deg));
        CHECK(!oot_wc_solve(none, level, 0, deg));
        CHECK(!oot_wc_solve(level, level, NAN, deg));
        CHECK(deg[0] == 7 && deg[1] == 8 && deg[2] == 9);
        const float bad[3] = { NAN, 0, 0 }, raw[4] = { 0.1f, 0.2f, 0.3f, 0.927f };
        CHECK(!oot_wc_apply(raw, bad, 0, hand) && !memcmp(hand, raw, sizeof hand));
        CHECK(!oot_wc_apply(none, deg, 0, hand) && !memcmp(hand, none, sizeof hand));
        CHECK(oot_wc_whole(181.4f) == 180 && oot_wc_whole(-200) == -180 && oot_wc_whole(-0.4f) == 0 && oot_wc_whole(12.5f) == 13);
    }

    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("wrist calibration: all checks pass\n");
    return 0;
}
