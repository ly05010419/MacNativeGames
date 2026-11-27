/* What Link wears and the game's own grip in first person (zeldaVR/runtime/oot_vr_wear.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_wear.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)
static int same(const float a[12], const float b[12]) { for (int i = 0; i < 12; i++) if (!NEAR(a[i], b[i])) return 0; return 1; }
/* A rotation about an axis by an angle, then a translation. */
static void pose(float ax, float ay, float az, float angle, float tx, float ty, float tz, float m[12]) {
    const float n = sqrtf(ax * ax + ay * ay + az * az), x = ax / n, y = ay / n, z = az / n, c = cosf(angle), s = sinf(angle), t = 1 - c;
    const float r[12] = { t * x * x + c, t * x * y - s * z, t * x * z + s * y, tx, t * x * y + s * z, t * y * y + c, t * y * z - s * x, ty,
                          t * x * z - s * y, t * y * z + s * x, t * z * z + c, tz };
    memcpy(m, r, sizeof r);
}
static void apply(const float m[12], const float p[3], float o[3]) {
    for (int i = 0; i < 3; i++) o[i] = m[4 * i] * p[0] + m[4 * i + 1] * p[1] + m[4 * i + 2] * p[2] + m[4 * i + 3];
}

int main(void) {
    float a[12], b[12], inv[12], id[12], m[12];
    static const float I[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    /* the inverse, scaled too (a stretched arm's matrix) */
    pose(1, 2, 3, 0.7f, 0.3f, -1, 2, a); a[0] *= 1.3f; a[4] *= 1.3f; a[8] *= 1.3f;
    assert(oot_wear_inverse(a, inv)); oot_wear_mul(a, inv, id); assert(same(id, I));
    const float flat[12] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0 }; assert(!oot_wear_inverse(flat, inv));
    /* the mirror twice is nothing; once, a point's image is the mirror image of the point's */
    oot_wear_mirror(a, b); oot_wear_mirror(b, m); assert(same(m, a));
    { const float p[3] = { 0.2f, 0.5f, -0.1f }, xp[3] = { -0.2f, 0.5f, -0.1f }; float q[3], r[3];
      apply(a, p, q); apply(b, xp, r); assert(NEAR(r[0], -q[0]) && NEAR(r[1], q[1]) && NEAR(r[2], q[2])); }

    /* the game's grip: with the VR hand where the game's hand is, each finger is the game's own */
    float hand[12], finger[12], out[12];
    pose(0, 1, 0, 0.4f, -0.3f, 1.0f, 0.1f, hand); pose(1, 0, 0, 0.9f, -0.35f, 0.98f, 0.12f, finger);
    assert(oot_wear_finger(hand, hand, finger, 0, out) && same(out, finger));
    /* the VR hand turned and moved: the finger keeps its place relative to the hand */
    float vr[12], rel_game[12], rel_vr[12]; pose(0, 0, 1, 1.1f, 0.5f, 1.2f, -0.2f, vr);
    assert(oot_wear_finger(vr, hand, finger, 0, out));
    oot_wear_inverse(hand, inv); oot_wear_mul(inv, finger, rel_game); oot_wear_inverse(vr, inv); oot_wear_mul(inv, out, rel_vr);
    assert(same(rel_game, rel_vr));
    /* the other hand: the left hand's grip mirrored on a right hand that is the left's mirror image gives the mirror image of
       the left's fingers */
    float rhand[12], rfinger_want[12]; oot_wear_mirror(hand, rhand); oot_wear_mirror(finger, rfinger_want);
    assert(oot_wear_finger(rhand, hand, finger, 1, out) && same(out, rfinger_want));
    /* a game hand with no inverse: nothing is changed */
    memcpy(out, I, sizeof out); assert(!oot_wear_finger(vr, flat, finger, 0, out) && same(out, I));
    /* mirrored skeletons: within the tolerance */
    { const float l[2][3] = { { -0.3f, 1, 0.1f }, { -0.35f, 0.98f, 0.12f } }, r[2][3] = { { 0.3f, 1, 0.1f }, { 0.351f, 0.98f, 0.12f } };
      assert(oot_wear_mirrored(l, r, 2, 0.002f) && !oot_wear_mirrored(l, r, 2, 0.0005f) && !oot_wear_mirrored(l, r, 0, 0.1f)); }

    /* carried on a bone: unmoved bone, unmoved thing; the bone moved, the thing by as much */
    float thing[12], bone[12], moved[12]; pose(1, 1, 0, 0.3f, 0.1f, 0.9f, 0.4f, thing); pose(0, 1, 1, 0.6f, 0.1f, 0.95f, 0.35f, bone);
    assert(oot_wear_carry(bone, bone, thing, out) && same(out, thing));
    pose(1, 0, 0, -0.8f, 0.4f, 1.3f, 0.0f, moved);
    assert(oot_wear_carry(moved, bone, thing, out));
    oot_wear_inverse(bone, inv); oot_wear_mul(inv, thing, rel_game); oot_wear_inverse(moved, inv); oot_wear_mul(inv, out, rel_vr);
    assert(same(rel_game, rel_vr));
    /* which bone it rides: the nearest within reach, kept within keep */
    float seg[4][2][3] = { { { -0.2f, 1, 0 }, { -0.4f, 1, 0 } }, { { -0.4f, 1, 0 }, { -0.5f, 1, 0 } },
                           { { 0.2f, 1, 0 }, { 0.4f, 1, 0 } }, { { 0.4f, 1, 0 }, { 0.5f, 1, 0 } } };
    const float near_left_fore[3] = { -0.3f, 1.03f, 0 }, near_right_hand[3] = { 0.47f, 1.0f, 0.05f }, far_away[3] = { 0, 3, 0 };
    assert(oot_wear_rides(near_left_fore, (const float (*)[2][3])seg, -1, 0.2f, 0.6f) == 0);
    assert(oot_wear_rides(near_right_hand, (const float (*)[2][3])seg, -1, 0.2f, 0.6f) == 3);
    assert(oot_wear_rides(far_away, (const float (*)[2][3])seg, -1, 0.2f, 0.6f) == -1);
    { const float drifted[3] = { -0.45f, 1.3f, 0 };   /* nearer the left hand now, still within keep of the forearm: stays */
      assert(oot_wear_rides(drifted, (const float (*)[2][3])seg, 0, 0.2f, 0.6f) == 0); assert(oot_wear_rides(drifted, (const float (*)[2][3])seg, -1, 0.4f, 0.6f) == 1); }
    assert(NEAR(oot_wear_segment((const float[3]){ 0, 1, 0 }, (const float[3]){ -1, 0, 0 }, (const float[3]){ 1, 0, 0 }), 1));

    /* a part the game hides is scaled to nothing: it stays hidden; a turned, moved or stretched one is not */
    { float z[12] = { 0.001f, 0, 0, 3, 0, 0.001f, 0, 1, 0, 0, 0.001f, 2 }; assert(oot_wear_scaled_away(z));
      assert(!oot_wear_scaled_away(a) && !oot_wear_scaled_away(hand) && !oot_wear_scaled_away(I));
      float nan_m[12]; memcpy(nan_m, I, sizeof nan_m); nan_m[0] = NAN; assert(oot_wear_scaled_away(nan_m)); }
    /* recognised by size: worn, but never a size of his own */
    oot_wear_table t; memset(&t, 0, sizeof t);
    const unsigned bag[2] = { 8481, 300 }, sword[1] = { 3620 }, clash[1] = { 300 };
    assert(oot_wear_add(&t, 48, bag, 2, OOT_WEAR_WORN, 7) && oot_wear_add(&t, 48, sword, 1, OOT_WEAR_OWN, 0) && oot_wear_add(&t, 48, clash, 1, OOT_WEAR_OWN, 1));
    assert(!oot_wear_add(&t, 0, bag, 2, OOT_WEAR_WORN, 8) && !oot_wear_add(&t, 48, bag, 0, OOT_WEAR_WORN, 8));
    const oot_wear_model *f = oot_wear_find(&t, 48, 8481); assert(f && f->kind == OOT_WEAR_WORN && f->index == 7);
    f = oot_wear_find(&t, 48, 300); assert(f && f->kind == OOT_WEAR_OWN);   /* the same size as one of his own: his own */
    f = oot_wear_find(&t, 48, 3620); assert(f && f->kind == OOT_WEAR_OWN);
    assert(!oot_wear_find(&t, 96, 8481) && !oot_wear_find(&t, 48, 1));
    puts("wear checks passed");
    return 0;
}
