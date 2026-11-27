/* zeldaVR/runtime/oot_vr_gear.h: the sword's tilt in the hand, the shield's three poses, an untargeted guard (vr.md rows 42, 52, 53). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_gear.h"

static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }
static void col(const float m[12], int j, float out[3]) { out[0] = m[j]; out[1] = m[4 + j]; out[2] = m[8 + j]; }

/* The grip frame built by oot_vr_pose.h from a controller at rest: its x is the controller's +x in both hands. */
static void grip_from_controller(int right, float tilt_deg, float out[3][3]) {
    const float c = cosf(tilt_deg * (float)M_PI / 180), s = sinf(tilt_deg * (float)M_PI / 180);
    const float fwd[3] = { 0, -c, -s }, back[3] = { right ? 1.f : -1.f, 0, 0 };
    float r[3], side[3], want[3][3];
    oot_ik_cross(fwd, back, r);
    for (int i = 0; i < 3; i++) side[i] = right ? -r[i] : r[i];   /* t->left = !side */
    assert(oot_frame(fwd, side, want));
    /* grip columns: x = want[2], y = want[0], z = want[1] (oot_hold_grip_rest takes the rest frame the same way) */
    memcpy(out[0], want[2], 12); memcpy(out[1], want[0], 12); memcpy(out[2], want[1], 12);
}

int main(void) {
    for (int right = 0; right < 2; right++) for (int tilt = -40; tilt <= 40; tilt += 20) {
        float g[3][3]; grip_from_controller(right, (float)tilt, g);
        assert(near(g[0][0], 1) && near(g[0][1], 0) && near(g[0][2], 0));   /* x: the controller's side axis */
        assert(near(g[2][0], 0));                                            /* z: in the controller's y-z plane */
    }
    {   /* at no hand tilt, z is the controller's -z (toward the thumb) in both hands */
        float g[3][3]; grip_from_controller(1, 0, g); assert(near(g[2][2], -1));
        grip_from_controller(0, 0, g); assert(near(g[2][2], -1));
    }
    {   /* row 42: the blade along the grip's z leans toward its y; x stays; 0 changes nothing */
        const float grip[12] = { 1, 0, 0, 0.1f, 0, 1, 0, 0.2f, 0, 0, 1, 0.3f };
        float out[12], z[3], x[3];
        oot_gear_tilt(grip, 35, out); col(out, 2, z); col(out, 0, x);
        assert(near(z[0], 0) && near(z[1], sinf(35 * (float)M_PI / 180)) && near(z[2], cosf(35 * (float)M_PI / 180)));
        assert(near(x[0], 1) && near(x[1], 0) && near(x[2], 0));
        assert(near(out[3], 0.1f) && near(out[7], 0.2f) && near(out[11], 0.3f));   /* about the grip's origin */
        oot_gear_tilt(grip, 0, out); for (int i = 0; i < 12; i++) assert(out[i] == grip[i]);
        oot_gear_tilt(grip, NAN, out); for (int i = 0; i < 12; i++) assert(out[i] == grip[i]);
    }
    {   /* row 52: the turn eases in 120 ms and back; mode 0 never turns; mode 1 only while guarding; mode 2 always */
        oot_shield_turn s = { 0, 0 };
        assert(oot_shield_turn_step(&s, 1000000000ull, OOT_SHIELD_ARM, 1) == 0);
        s = (oot_shield_turn){ 0, 0 };
        assert(oot_shield_turn_step(&s, 1000000000ull, OOT_SHIELD_FIST, 0) == 1);   /* the first frame takes it at once */
        s = (oot_shield_turn){ 0, 1000000000ull };
        assert(near(oot_shield_turn_step(&s, 1060000000ull, OOT_SHIELD_GUARD, 1), 0.5f));
        assert(near(oot_shield_turn_step(&s, 1120000000ull, OOT_SHIELD_GUARD, 1), 1));
        assert(near(oot_shield_turn_step(&s, 1150000000ull, OOT_SHIELD_GUARD, 0), 0.75f));
        assert(near(oot_shield_turn_step(&s, 1400000000ull, OOT_SHIELD_GUARD, 0), 0));
    }
    for (int right = 0; right < 2; right++) {   /* the face (outside of the forearm) goes where the fist points (+y); z stays */
        const float grip[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
        float move[12] = { 1, 0, 0, 0.5f, 0, 1, 0, 0, 0, 0, 1, 0 };   /* the shield moved 0.5 along x */
        const float root[3] = { 0, 0, 0 }, face_out = right ? 1.f : -1.f;
        assert(oot_shield_turn_move(grip, root, oot_shield_turn_angle(right, 1), move));
        float p[3], q[3], up[3]; const float face[3] = { face_out, 0, 0 }, z[3] = { 0, 0, 1 };
        oot_m_apply(move, root, p); assert(near(p[0], 0.5f) && near(p[1], 0) && near(p[2], 0));   /* about its own origin */
        oot_m_apply(move, face, q); assert(near(q[0] - p[0], 0) && near(q[1] - p[1], 1) && near(q[2] - p[2], 0));
        oot_m_apply(move, z, up); assert(near(up[2] - p[2], 1));
        float none[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
        assert(!oot_shield_turn_move(grip, root, 0, none));
    }
    {   /* row 53: confirmed only within 0x408 after the guard bit was seen; a lock-on or another state revokes it */
        oot_guard_state g = { 0, 0 };
        assert(!oot_guard_untargeted(&g, 0x408, 0, 0));
        assert(oot_guard_untargeted(&g, 0x408, 1, 0));
        assert(oot_guard_untargeted(&g, 0x408, 0, 0));   /* the bit flickers: kept */
        assert(!oot_guard_untargeted(&g, 0x408, 1, 1));  /* locked on */
        assert(!oot_guard_untargeted(&g, 0x408, 0, 0));  /* needs the bit again */
        assert(oot_guard_untargeted(&g, 0x408, 1, 0));
        assert(!oot_guard_untargeted(&g, 0x420, 1, 0));  /* strafe */
        assert(!oot_guard_untargeted(&g, 0x408, 0, 0));
        float f[2];
        assert(oot_guard_facing(3, 4, f) && near(f[0], 0.6f) && near(f[1], 0.8f));
        assert(!oot_guard_facing(0, 0, f) && !oot_guard_facing(NAN, 1, f));
    }
    printf("oot_vr_gear: all passed\n");
    return 0;
}
