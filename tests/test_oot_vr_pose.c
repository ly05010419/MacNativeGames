/* The first-person arm's skinning matrices (zeldaVR/runtime/oot_vr_pose.h), on a made-up right arm in a T pose. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_pose.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 2e-3f)
static float dist(const float a[3], const float b[3]) { const float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; return oot_ik_len(d); }
static void rigid(const float m[12]) {   /* its 3x3 is a rotation */
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        const float d = m[4 * i] * m[4 * j] + m[4 * i + 1] * m[4 * j + 1] + m[4 * i + 2] * m[4 * j + 2];
        assert(NEAR(d, i == j ? 1 : 0));
    }
}

int main(void) {
    oot_arm_rest r = { { 0.1f, 1, 0 }, { 0.25f, 1, 0 }, { 0.4f, 1, 0 }, {{{0}}} };
    /* fingers along +x from the wrist: thumb toward -z (the body's front), then index to little from front to back */
    for (int f = 0; f < 5; f++) for (int j = 0; j < 3; j++) {
        r.finger[f][j][0] = 0.45f + 0.015f * (float)j; r.finger[f][j][1] = 1; r.finger[f][j][2] = -0.02f + 0.01f * (float)f;
    }
    const float upper = dist(r.shoulder, r.elbow), fore = dist(r.elbow, r.wrist), palm = dist(r.wrist, r.finger[2][0]);

    oot_arm_target t = { { 0.17f, 1.2f, 0.05f }, { 0.25f, 1.1f, -0.25f }, { 0, 0, -1 }, { 0, 1, 0 }, { 0.5f, -1, 0.4f }, 0, 0, 0, 0 };
    oot_arm_matrices o; float p[3];
    assert(oot_arm_matrices_solve(&r, &t, &o));
    rigid(o.upper); rigid(o.hand); rigid(o.assist[0]); rigid(o.assist[1]);
    /* each joint lands where the solve put it, from both of the parts that meet there */
    oot_m_apply(o.upper, r.shoulder, p); assert(NEAR(dist(p, t.shoulder), 0));
    oot_m_apply(o.upper, r.elbow, p); assert(NEAR(dist(p, o.joints.elbow), 0));
    oot_m_apply(o.fore, r.elbow, p); assert(NEAR(dist(p, o.joints.elbow), 0));
    oot_m_apply(o.fore, r.wrist, p); assert(NEAR(dist(p, o.joints.wrist), 0));
    oot_m_apply(o.hand, r.wrist, p); assert(NEAR(dist(p, o.joints.wrist), 0));
    assert(NEAR(dist(o.joints.shoulder, o.joints.elbow), upper) && NEAR(dist(o.joints.elbow, o.joints.wrist), fore));
    assert(o.joints.elbow[1] < 1.2f);                                          /* toward the pole: down */
    /* the wrist sits behind the grip by 0.62 of the palm, the hand points along the controller */
    const float want[3] = { t.grip[0], t.grip[1], t.grip[2] + 0.62f * palm };
    assert(NEAR(dist(o.joints.wrist, want), 0));
    float mid[3]; oot_m_apply(o.hand, r.finger[2][0], mid);
    assert(NEAR(mid[2] - o.joints.wrist[2], -palm) && NEAR(mid[0], o.joints.wrist[0]) && NEAR(mid[1], o.joints.wrist[1]));
    /* right hand: the index finger is on the side toward the body's middle (-x) of the little one */
    float a[3], b[3]; oot_m_apply(o.hand, r.finger[1][0], a); oot_m_apply(o.hand, r.finger[4][0], b);
    assert(a[0] < b[0]);
    t.left = 1; assert(oot_arm_matrices_solve(&r, &t, &o));
    oot_m_apply(o.hand, r.finger[1][0], a); oot_m_apply(o.hand, r.finger[4][0], b);
    assert(a[0] > b[0]);
    t.left = 0;

    /* a closed hand curls toward the palm (down) and keeps the finger's length; the open one stays above it */
    t.squeeze = 1; t.trigger = 1; assert(oot_arm_matrices_solve(&r, &t, &o));
    float root[3], tip[3];
    oot_m_apply(o.finger[3][0], r.finger[3][0], root);
    oot_m_apply(o.finger[3][2], r.finger[3][2], tip);
    assert(tip[1] < root[1] - 0.005f);
    float j1a[3], j1b[3]; oot_m_apply(o.finger[3][0], r.finger[3][1], j1a); oot_m_apply(o.finger[3][1], r.finger[3][1], j1b);
    assert(NEAR(dist(j1a, j1b), 0));                                            /* the joints stay joined */
    assert(NEAR(dist(root, j1a), 0.015f));
    rigid(o.finger[3][2]);

    /* out of reach: stretched along the bone, the joints still meet */
    oot_arm_target far = t; far.grip[2] = -0.62f; far.trigger = far.squeeze = 0;
    assert(oot_arm_matrices_solve(&r, &far, &o) && o.joints.stretch > 1);
    oot_m_apply(o.upper, r.elbow, p); assert(NEAR(dist(p, o.joints.elbow), 0));
    oot_m_apply(o.fore, r.elbow, p); assert(NEAR(dist(p, o.joints.elbow), 0));
    oot_m_apply(o.fore, r.wrist, p); assert(NEAR(dist(p, o.joints.wrist), 0));

    /* the forearm's assists take part of the hand's twist: with the hand rolled 90 degrees about the arm they differ from it */
    oot_arm_target rolled = t; rolled.trigger = rolled.squeeze = 0; rolled.up[0] = 1; rolled.up[1] = 0;
    assert(oot_arm_matrices_solve(&r, &rolled, &o));
    const float off[3] = { 0.3f, 1.05f, 0 }; float q0[3], q1[3], qf[3];
    oot_m_apply(o.assist[0], off, q0); oot_m_apply(o.assist[1], off, q1); oot_m_apply(o.fore, off, qf);
    assert(dist(q0, qf) > 1e-3f && dist(q1, qf) > dist(q0, qf));

    oot_arm_rest bad = r; bad.elbow[0] = bad.shoulder[0];
    assert(!oot_arm_matrices_solve(&bad, &t, &o));
    puts("OOT first-person arm matrices: joints meet, the palm on the controller, thumb side, curl, stretch, forearm twist");
    return 0;
}
