/* The first-person arm of Ocarina of Time (zeldaVR/runtime/oot_vr_ik.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_ik.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)
static float dist(const float a[3], const float b[3]) { const float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; return oot_ik_len(d); }

int main(void) {
    const float sh[3] = { 0, 1.4f, 0 }, pole[3] = { 0, -1, 0 };
    oot_arm_pose a;

    /* within reach: the wrist on the target, each bone its length, the elbow on the pole's side */
    float t[3] = { 0.3f, 1.4f, -0.3f };
    assert(oot_arm_solve(sh, t, 0.3f, 0.3f, pole, &a));
    assert(NEAR(dist(a.wrist, t), 0) && NEAR(dist(a.shoulder, a.elbow), 0.3f) && NEAR(dist(a.elbow, a.wrist), 0.3f));
    assert(a.elbow[1] < 1.4f && a.stretch == 1 && a.reached);

    /* a little past the arm: the shoulder gives, the wrist is on the target, no stretch */
    float far1[3] = { 0, 1.4f, -0.62f };
    assert(oot_arm_solve(sh, far1, 0.3f, 0.3f, pole, &a));
    assert(NEAR(dist(a.wrist, far1), 0) && a.shoulder[2] < 0 && dist(a.shoulder, sh) <= 0.0601f && a.reached);

    /* further: it stretches, and the bones stay equal */
    float far2[3] = { 0, 1.4f, -0.75f };
    assert(oot_arm_solve(sh, far2, 0.3f, 0.3f, pole, &a));
    assert(NEAR(dist(a.wrist, far2), 0) && a.stretch > 1 && a.stretch <= 1.4f && a.reached);
    assert(NEAR(dist(a.shoulder, a.elbow), dist(a.elbow, a.wrist)));

    /* out of reach altogether: at most 1.4 times, pointing at the target */
    float far3[3] = { 0, 1.4f, -3 };
    assert(oot_arm_solve(sh, far3, 0.3f, 0.3f, pole, &a));
    assert(NEAR(a.stretch, 1.4f) && !a.reached && a.wrist[2] < -0.8f && NEAR(a.wrist[0], 0) && NEAR(a.wrist[1], 1.4f));

    /* the pole along the arm and a target on the shoulder are still answers */
    float along[3] = { 0, 1, 0 }, up[3] = { 0, 1.8f, 0 };
    assert(oot_arm_solve(sh, up, 0.3f, 0.3f, along, &a) && NEAR(dist(a.shoulder, a.elbow), 0.3f));
    assert(oot_arm_solve(sh, sh, 0.3f, 0.3f, pole, &a) && isfinite(a.elbow[0]) && isfinite(a.elbow[1]));
    float bad[3] = { NAN, 0, 0 };
    assert(!oot_arm_solve(sh, bad, 0.3f, 0.3f, pole, &a) && !oot_arm_solve(sh, t, 0, 0.3f, pole, &a));

    float c[3];
    oot_finger_curl(0, 0, 0, 0, c); assert(NEAR(c[0], 0.10f) && NEAR(c[2], 0.15f));
    oot_finger_curl(0, 1, 0.2f, 0, c); assert(NEAR(c[0], 0.45f) && NEAR(c[2], 0.65f));
    oot_finger_curl(1, 1, 0, 0, c); assert(NEAR(c[0], 1.25f) && NEAR(c[1], 1.55f) && NEAR(c[2], 0.95f));
    oot_finger_curl(3, 0, 1, 0, c); assert(NEAR(c[0], 1.35f) && NEAR(c[1], 1.60f) && NEAR(c[2], 1.00f));
    oot_finger_curl(3, 0, 1, 1, c); assert(NEAR(c[0], 0.25f + 1.10f * 0.6f));   /* around a handle */
    puts("OOT first-person arm: reach, the shoulder's give, the stretch, odd poles; the fingers' curl");
    return 0;
}
