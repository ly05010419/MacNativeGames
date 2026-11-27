/* The bow's and slingshot's string in the player's hands (zeldaVR/runtime/oot_vr_string.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_string.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 2e-3f)
#define NEAR3(v, x, y, z) (NEAR((v)[0], x) && NEAR((v)[1], y) && NEAR((v)[2], z))

/* the bow hand 40 cm ahead of the room's origin, the string resting 10 cm behind it; scale 0.5 game units a metre */
static oot_string_in bow_at(float hand_z, float input) {
    oot_string_in in; memset(&in, 0, sizeof in);
    in.bow_present = 1; in.tracked = 1; in.input = input; in.scale = 0.5f;
    in.handle[2] = -0.20f; in.rest[2] = -0.15f; in.hand[2] = hand_z;
    return in;
}

static void string_tests(void) {
    oot_string_state s; memset(&s, 0, sizeof s); oot_string_out o;
    /* pressed away from the string: nothing, and nothing while it stays pressed when the hand gets there */
    oot_string_in in = bow_at(0.10f, 0.9f); oot_string_step(&s, &in, &o); assert(!(o.flags & OOT_STRING_HELD) && s.blocked);
    in = bow_at(-0.15f, 0.9f); oot_string_step(&s, &in, &o); assert(!(o.flags & OOT_STRING_HELD));
    in = bow_at(-0.15f, 0.1f); oot_string_step(&s, &in, &o); assert(!s.blocked);
    /* pressed at the string (within 15 cm of the room): held; the middle of the string is at the hand */
    in = bow_at(-0.14f, 0.6f); oot_string_step(&s, &in, &o); assert(o.flags & OOT_STRING_HELD); assert(NEAR3(o.string_at, 0, 0, -0.14f));
    /* in between 0.35 and 0.55 it stays held (hysteresis) */
    in = bow_at(-0.14f, 0.45f); oot_string_step(&s, &in, &o); assert(o.flags & OOT_STRING_HELD);
    /* drawn 5 cm of the room (2.5 game units of cm): not committed; 12 cm: committed */
    in = bow_at(-0.15f + 0.025f, 0.9f); oot_string_step(&s, &in, &o); assert(NEAR(o.draw, 0.05f) && !(o.flags & OOT_STRING_COMMITTED));
    in = bow_at(-0.15f + 0.07f, 0.9f); oot_string_step(&s, &in, &o); assert(NEAR(o.draw, 0.14f) && (o.flags & OOT_STRING_COMMITTED));
    /* the aim runs from the drawing hand through the handle */
    assert(NEAR3(o.dir, 0, 0, -1) && NEAR3(o.handle, 0, 0, -0.20f));
    /* the hand goes off to the side: the aim follows it */
    in = bow_at(-0.08f, 0.9f); in.hand[0] = 0.12f; oot_string_step(&s, &in, &o);
    { const float d[3] = { -0.12f, 0, -0.12f }, n = sqrtf(d[0] * d[0] + d[2] * d[2]); assert(NEAR3(o.dir, d[0] / n, 0, d[2] / n)); }
    /* let go: a shot, the line kept 20 frames while the hands move on */
    const float shot[3] = { o.dir[0], o.dir[1], o.dir[2] };
    in = bow_at(-0.08f, 0.1f); in.hand[0] = 0.12f; oot_string_step(&s, &in, &o); assert(o.fired && s.shots == 1 && !(o.flags & (OOT_STRING_HELD | OOT_STRING_COMMITTED)));
    for (int f = 0; f < OOT_STRING_FREEZE_FRAMES - 1; f++) { in = bow_at(0.3f, 0.0f); in.handle[0] = 0.5f; oot_string_step(&s, &in, &o); assert(!o.fired && NEAR3(o.dir, shot[0], shot[1], shot[2]) && NEAR(o.handle[0], 0)); }
    in = bow_at(0.3f, 0.0f); in.handle[0] = 0.5f; oot_string_step(&s, &in, &o); assert(NEAR(o.handle[0], 0.5f));
    /* let go before the commit: no shot */
    memset(&s, 0, sizeof s);
    in = bow_at(-0.15f, 0.9f); oot_string_step(&s, &in, &o); assert(o.flags & OOT_STRING_HELD);
    in = bow_at(-0.12f, 0.9f); oot_string_step(&s, &in, &o); assert(!(o.flags & OOT_STRING_COMMITTED));
    in = bow_at(-0.12f, 0.0f); oot_string_step(&s, &in, &o); assert(!o.fired && s.shots == 0);
    /* a menu takes the string out of the hand, the commit waits; the let-go once it is closed is the shot */
    memset(&s, 0, sizeof s);
    in = bow_at(-0.15f, 0.9f); oot_string_step(&s, &in, &o);
    in = bow_at(-0.05f, 0.9f); oot_string_step(&s, &in, &o); assert(o.flags & OOT_STRING_COMMITTED);
    in = bow_at(-0.05f, 0.9f); in.menu = 1; oot_string_step(&s, &in, &o); assert(!(o.flags & OOT_STRING_HELD) && (o.flags & OOT_STRING_COMMITTED));
    in = bow_at(-0.05f, 0.0f); in.menu = 1; oot_string_step(&s, &in, &o); assert(!o.fired && (o.flags & OOT_STRING_COMMITTED));
    in = bow_at(-0.05f, 0.0f); oot_string_step(&s, &in, &o); assert(o.fired && s.shots == 1);
    /* the hand 1.5 m from the bow is out of it too */
    memset(&s, 0, sizeof s);
    in = bow_at(-0.15f, 0.9f); oot_string_step(&s, &in, &o);
    in = bow_at(-0.15f + 0.8f, 0.9f); oot_string_step(&s, &in, &o); assert(!(o.flags & OOT_STRING_HELD));
    /* the bow gone for more than three frames: everything reset */
    memset(&s, 0, sizeof s);
    in = bow_at(-0.15f, 0.9f); oot_string_step(&s, &in, &o); assert(s.held);
    in.bow_present = 0;
    for (int f = 0; f < 3; f++) { oot_string_step(&s, &in, &o); assert(s.held); }
    oot_string_step(&s, &in, &o); assert(!s.held);
    /* not a number: nothing */
    memset(&s, 0, sizeof s); in = bow_at(NAN, 0.9f); oot_string_step(&s, &in, &o); assert(!o.valid);
    /* the input options */
    assert(oot_string_input(OOT_STRING_EITHER, 0.2f, 0.7f) == 0.7f && oot_string_input(OOT_STRING_TRIGGER, 0.2f, 0.7f) == 0.2f && oot_string_input(OOT_STRING_GRIP, 0.2f, 0.7f) == 0.7f);
}

static void rumble_tests(void) {
    assert(oot_string_rumble(0.02f) == 0 && NEAR(oot_string_rumble(0.03f + 0.45f), 0.80f) && NEAR(oot_string_rumble(2), 0.80f) && NEAR(oot_string_rumble(0.255f), 0.44f));
    oot_string_buzz b = { 0, 0 }; float a = 0;
    assert(oot_string_buzz_step(&b, 0.2f, 1000, &a) && NEAR(a, oot_string_rumble(0.2f)));
    assert(!oot_string_buzz_step(&b, 0.21f, 1000 + 50000000ull, &a));          /* a small change within 90 ms */
    assert(oot_string_buzz_step(&b, 0.21f, 1000 + 95000000ull, &a));           /* 90 ms on */
    assert(oot_string_buzz_step(&b, 0.40f, 1000 + 100000000ull, &a));          /* a large change */
    assert(!oot_string_buzz_step(&b, 0.01f, 1000 + 300000000ull, &a) && b.sent == 0);
}

static void strap_tests(void) {
    const float tip[3] = { 0, 0, 0 }, from[3] = { 1, 0, 0 }, to[3] = { 0, 2, 0 }; float m[12], p[3];
    assert(oot_string_swing(tip, from, to, m));
    oot_m_apply(m, tip, p); assert(NEAR3(p, 0, 0, 0));
    oot_m_apply(m, from, p); assert(NEAR3(p, 0, 2, 0));
    const float half[3] = { 0.5f, 0, 0 }; oot_m_apply(m, half, p); assert(NEAR3(p, 0, 1, 0));   /* along the line: in proportion */
    const float side[3] = { 0, 0, 0.1f }; oot_m_apply(m, side, p); assert(NEAR3(p, 0, 0, 0.1f));   /* across it: not stretched */
    const float far[3] = { 0, 100, 0 }; assert(oot_string_swing(tip, from, far, m)); oot_m_apply(m, from, p); assert(NEAR3(p, 0, 5, 0));   /* at most 5 times */
    assert(!oot_string_swing(tip, tip, to, m));
    const float fork[3] = { 0, 0, 0 }, pf[3] = { 0, 0, 1 }, pt[3] = { 1, 0, 0 };
    assert(oot_string_pouch(fork, pf, pt, m));
    oot_m_apply(m, pf, p); assert(NEAR3(p, 1, 0, 0));
    const float above[3] = { 0, 0.1f, 1 }; oot_m_apply(m, above, p); assert(NEAR3(p, 1, 0.1f, 0));
    const float behind[3] = { 0, 0, 1.1f }; oot_m_apply(m, behind, p); assert(NEAR3(p, 1.1f, 0, 0));   /* turned with the line */
}

static void arrow_tests(void) {
    /* the bow's root turned a quarter about y and moved; an arrow along its -x beside the string, nocked */
    const float bow[12] = { 0, 0, 1, 10, 0, 1, 0, 2, -1, 0, 0, 3 };   /* bow x -> world -z, bow z -> world x */
    float rel[12] = { 0, 1, 0, 0.1f, 0, 0, -1, 0.02f, 1, 0, 0, 0 };   /* columns: arrow x = bow y, y = bow -z, z (shaft) = bow -x... */
    rel[8] = 0; rel[2] = -1; rel[10] = 0;                              /* shaft = bow -x */
    rel[0] = 0; rel[4] = 1; rel[1] = 0; rel[5] = 0; rel[6] = 0; rel[9] = -1;   /* the rest: a proper rotation */
    float arrow[12]; oot_m_mul(bow, rel, arrow);
    assert(oot_arrow_nocked(bow, arrow));
    float moved[12]; memcpy(moved, rel, sizeof rel); moved[3] = 0.5f; { float a2[12]; oot_m_mul(bow, moved, a2); assert(!oot_arrow_nocked(bow, a2)); }   /* too far along */
    memcpy(moved, rel, sizeof rel); moved[7] = 0.2f; { float a2[12]; oot_m_mul(bow, moved, a2); assert(!oot_arrow_nocked(bow, a2)); }               /* too far across */
    memcpy(moved, rel, sizeof rel); moved[2] = 1; { float a2[12]; oot_m_mul(bow, moved, a2); assert(!oot_arrow_nocked(bow, a2)); }                 /* the wrong way */
    /* placed: the notch on the string, the shaft along the aim, its size kept */
    float game[12]; memcpy(game, arrow, sizeof arrow); for (int i = 0; i < 3; i++) game[4 * i + 2] *= 2;   /* twice as long */
    const float nock[3] = { 1, 2, 3 }, dir[3] = { 0, 0, -1 }, limbs[3] = { 0, 1, 0 }; float out[12], p[3];
    assert(oot_arrow_place(game, nock, dir, limbs, out));
    const float notch[3] = { 0, 0, -OOT_ARROW_NOTCH }; oot_m_apply(out, notch, p); assert(NEAR3(p, 1, 2, 3));
    assert(NEAR(out[2], 0) && NEAR(out[6], 0) && NEAR(out[10], -2));     /* +z along the aim, twice as long */
    assert(NEAR(out[1], 0) && NEAR(out[5], -1) && NEAR(out[9], 0));      /* -y along the limbs */
    { float x[3] = { out[0], out[4], out[8] }, y[3] = { out[1], out[5], out[9] }, z[3] = { out[2], out[6], out[10] }, c[3]; oot_ik_cross(x, y, c);
      assert(oot_ik_dot(c, z) > 0); }                                    /* right-handed */
    const float bad[3] = { 0, 0, 0 }; assert(!oot_arrow_place(game, nock, bad, limbs, out));
}

static void fx_tests(void) {
    /* a camera at (0, 1, 5) looking down -z, perspective; the effect at the game's arrow (0, 1, 4.5) */
    float c[OOT_FX_CAMERA_FLOATS]; memset(c, 0, sizeof c);
    float v[12] = { 1, 0, 0, 0, 0, 1, 0, -1, 0, 0, 1, -5 }, iv[12]; oot_fx_to44(v, c + OOT_FX_VIEW);
    assert(oot_string_inverse(v, iv)); oot_fx_to44(iv, c + OOT_FX_INVERSE_VIEW);
    c[0] = 0; c[1] = 1; c[2] = 5; c[4] = 0; c[5] = 0; c[6] = -1;
    float *pr = c + OOT_FX_PROJECTION; pr[0] = 1.2f; pr[5] = 2.1f; pr[10] = -1.0f; pr[11] = -1; pr[14] = -0.1f;
    float eye[3]; assert(oot_fx_eye(c, eye) && NEAR3(eye, 0, 1, 5));
    float effect[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 1, 4.5f, 1 };
    const float game_arrow[3] = { 0, 1, 4.4f }; assert(oot_fx_source(effect, game_arrow, eye));
    const float far_arrow[3] = { 0, 1, 2 }; assert(!oot_fx_source(effect, far_arrow, eye));
    effect[14] = 5; assert(!oot_fx_source(effect, game_arrow, eye)); effect[14] = 4.5f;
    /* the VR arrow is 1 m to the right of the game's: the particle at the game's arrow is seen there */
    const float move[12] = { 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 1, 0 };
    float before[OOT_FX_CAMERA_FLOATS]; memcpy(before, c, sizeof c);
    const float at[3] = { 0, 1, 4.5f };
    assert(oot_fx_camera(c, move, at, 1.0f));
    /* clip position of the particle: P V' p equals P V (move p) */
    float p4[4] = { at[0], at[1], at[2], 1 }, q4[4] = { at[0] + 1, at[1], at[2], 1 };
    float a[4], b[4], ca[4], cb[4];
    for (int r = 0; r < 4; r++) { a[r] = 0; b[r] = 0; for (int k = 0; k < 4; k++) { a[r] += c[OOT_FX_VIEW + k * 4 + r] * p4[k]; b[r] += before[OOT_FX_VIEW + k * 4 + r] * q4[k]; } }
    for (int r = 0; r < 4; r++) { ca[r] = 0; cb[r] = 0; for (int k = 0; k < 4; k++) { ca[r] += c[OOT_FX_PROJECTION + k * 4 + r] * a[k]; cb[r] += before[OOT_FX_PROJECTION + k * 4 + r] * b[k]; } }
    for (int r = 0; r < 4; r++) assert(NEAR(ca[r], cb[r]));
    assert(NEAR3(c, -1, 1, 5) && NEAR3(c + 4, 0, 0, -1));                /* the camera's place by the inverse move */
    /* the fade: 0.4 x (stretched depth) - 1 comes out at the opacity asked for */
    assert(NEAR(a[2] / (before[OOT_FX_VIEW + 2 * 4 + 2] * at[2] + before[OOT_FX_VIEW + 14]), OOT_FX_WHOLE));
    memcpy(c, before, sizeof c); assert(oot_fx_camera(c, move, at, 0.2f));
    { float z = 0; for (int k = 0; k < 4; k++) z += c[OOT_FX_VIEW + k * 4 + 2] * p4[k]; assert(NEAR(fminf(1, fmaxf(0, 0.4f * fabsf(z) - 1)), 0.2f)); }
    /* not a camera: refused */
    memcpy(c, before, sizeof c); c[OOT_FX_PROJECTION + 11] = 0; assert(!oot_fx_camera(c, move, at, 1));
}

/* the string's rest kept against the bow's root: a point taken to the root's own frame and back */
static void rest_tests(void) {
    const float m[12] = { 0, -2, 0, 1,  2, 0, 0, 2,  0, 0, 2, 3 };   /* turned 90 degrees about z, scaled 2, moved 1 2 3 */
    const float p[3] = { 3, 4, 5 };
    float local[3], back[3];
    assert(oot_string_local(m, p, local));
    oot_m_apply(m, local, back);
    for (int i = 0; i < 3; i++) assert(fabsf(back[i] - p[i]) < 1e-5f);
    assert(fabsf(local[0] - 1) < 1e-5f && fabsf(local[1] + 1) < 1e-5f && fabsf(local[2] - 1) < 1e-5f);
    const float flat[12] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 0, 0 };   /* no z: not a frame */
    assert(!oot_string_local(flat, p, local));
    const float nan_p[3] = { NAN, 0, 0 };
    assert(!oot_string_local(m, nan_p, local));
}
int main(void) {
    rest_tests();
    string_tests(); rumble_tests(); strap_tests(); arrow_tests(); fx_tests();
    puts("oot_vr_string: all passed");
    return 0;
}
