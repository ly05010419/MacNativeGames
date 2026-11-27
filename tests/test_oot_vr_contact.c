/* zeldaVR/runtime/oot_vr_contact.h: the blade's own contact (vr.md row 44) and the hit in the air (row 47). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_contact.h"

static int near(float a, float b) { return fabsf(a - b) < 1e-3f; }
/* a weapon pose: origin o, blade (z) along d (unit), y up-ish */
static void pose(float o0, float o1, float o2, float d0, float d1, float d2, float m[12]) {
    const float z[3] = { d0, d1, d2 }; float y[3] = { 0, 1, 0 }, x[3];
    const float k = y[0] * z[0] + y[1] * z[1] + y[2] * z[2];
    for (int i = 0; i < 3; i++) y[i] -= z[i] * k;
    float n = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    if (n < 1e-3f) { y[0] = 1; y[1] = y[2] = 0; n = 1; }
    for (int i = 0; i < 3; i++) y[i] /= n;
    x[0] = y[1] * z[2] - y[2] * z[1]; x[1] = y[2] * z[0] - y[0] * z[2]; x[2] = y[0] * z[1] - y[1] * z[0];
    const float o[3] = { o0, o1, o2 };
    for (int i = 0; i < 3; i++) { m[4 * i] = x[i]; m[4 * i + 1] = y[i]; m[4 * i + 2] = z[i]; m[4 * i + 3] = o[i]; }
}

int main(void) {
    oot_target_shape s;
    assert(oot_target_shape_of("Enemy_Dekubaba", &s) && near(s.high, 1.3f) && near(s.radius, 0.30f));
    assert(oot_target_shape_of("Enemy_Skulltula_Big", &s) && near(s.radius, 0.45f));
    assert(oot_target_shape_of("Enemy_Octorok", &s) && near(s.low, -0.2f) && near(s.radius, 0.50f));
    assert(oot_target_shape_of("Obj_Pot_A", &s) && near(s.high, 0.8f));
    assert(!oot_target_shape_of("Obj_Door_Wood", &s) && !oot_target_shape_of("Obj_AreaTrigger", &s) && !oot_target_shape_of("Npc_Kokiri", &s));
    assert(!oot_target_shape_of("", &s) && !oot_target_shape_of(NULL, &s));
    {
        const float link[3] = { 0, 0, 0 }, a[3] = { 2.5f, 3.9f, 2.5f }, b[3] = { 3, 0, 3 }, c[3] = { 0, 4.1f, 0 };
        assert(oot_target_near(link, a) && !oot_target_near(link, b) && !oot_target_near(link, c));
    }
    static oot_targets ts;
    {   /* the table: refresh, reuse after 2 s, the not-a-target cache */
        const float p[3] = { 1, 0, 0 };
        oot_target_shape_of("Enemy_Dekubaba", &s);
        oot_target *t = oot_targets_seen(&ts, 0x1000, p, &s, "Enemy_Dekubaba", 1000000000ull);
        assert(t && oot_targets_find(&ts, 0x1000) == t && !strcmp(t->name, "Enemy_Dekubaba"));
        for (uintptr_t k = 1; k < OOT_TARGETS; k++) assert(oot_targets_seen(&ts, 0x1000 + 16 * k, p, &s, "x", 1000000000ull));
        assert(!oot_targets_seen(&ts, 0x9000, p, &s, "x", 1500000000ull));    /* full */
        assert(oot_targets_seen(&ts, 0x9000, p, &s, "x", 3100000000ull));     /* a stale slot taken */
        assert(!oot_targets_known_not(&ts, 0x2340)); oot_targets_mark_not(&ts, 0x2340); assert(oot_targets_known_not(&ts, 0x2340));
        memset(&ts, 0, sizeof ts);
    }
    {   /* segments */
        const float p0[3] = { 0, 0, 0 }, p1[3] = { 1, 0, 0 }, q0[3] = { 0.5f, -1, 0.3f }, q1[3] = { 0.5f, 1, 0.3f };
        float sp; assert(near(oot_seg_dist(p0, p1, q0, q1, &sp), 0.3f) && near(sp, 0.5f));
    }
    const oot_blade_shape blade = { OOT_TOUCH_BLADE_FROM, OOT_TOUCH_BLADE_TO, OOT_TOUCH_BLADE_RADIUS };
    {   /* a fast swing that passes a Deku Baba between two frames is found, at the step that goes deepest */
        const float at[3] = { 0, 0, 0.5f };
        oot_target_shape_of("Enemy_Dekubaba", &s);
        oot_targets_seen(&ts, 0x5000, at, &s, "Enemy_Dekubaba", 1000000000ull);
        float a[12], b[12]; pose(-0.3f, 1.0f, 0, 1, 0, 0, a);   /* blade along +x, left of the target, 1 m up */
        pose(-0.3f, 1.0f, 0, 0.0f, 0, 1, b);                    /* turned to +z: through it */
        oot_contact c; int locked = 0;
        assert(oot_targets_sweep(&ts, a, b, &blade, 1050000000ull, 1, &c, &locked) == 0 && !locked);
        assert(c.gap < 0 && c.t > 0 && c.t <= 1 && c.along >= blade.from && c.along <= blade.to);
        assert(oot_targets_sweep(&ts, a, b, &blade, 1050000000ull, 0, &c, &locked) == -1);   /* no attack on: no contact */
        float far_a[12], far_b[12]; pose(-0.3f, 1.0f, -2, 1, 0, 0, far_a); pose(-0.3f, 1.0f, -2, 0.7071f, 0, 0.7071f, far_b);
        assert(oot_targets_sweep(&ts, far_a, far_b, &blade, 1050000000ull, 1, &c, &locked) == -1);   /* missed */
        assert(oot_targets_sweep(&ts, a, b, &blade, 3100000000ull, 1, &c, &locked) == -1);           /* stale target */
        /* locked: touching it again is refused, until the blade is out by 0.10 m and a new stroke begins out there */
        ts.t[0].locked = 1;
        assert(oot_targets_sweep(&ts, a, b, &blade, 1060000000ull, 1, &c, &locked) == -1 && locked);
        assert(oot_targets_stroke_begins(&ts, b, &blade, 1070000000ull) == 1);   /* begun inside: nothing set up */
        assert(ts.t[0].locked);
        oot_targets_sweep(&ts, b, far_a, &blade, 1080000000ull, 0, &c, &locked);  /* the blade leaves it */
        assert(ts.t[0].cleared);
        assert(oot_targets_stroke_begins(&ts, far_a, &blade, 1090000000ull) == 0 && !ts.t[0].locked);
        assert(oot_targets_sweep(&ts, a, b, &blade, 1100000000ull, 1, &c, &locked) == 0);
        /* the pose between keeps a square frame */
        float m[12]; oot_touch_pose(a, b, 0.5f, m);
        const float z[3] = { m[2], m[6], m[10] }, y[3] = { m[1], m[5], m[9] };
        assert(near(z[0] * z[0] + z[1] * z[1] + z[2] * z[2], 1) && near(z[0] * y[0] + z[1] * y[1] + z[2] * y[2], 0));
        assert(near(z[0], 0.7071f) && near(z[2], 0.7071f));
        memset(&ts, 0, sizeof ts);
    }
    {   /* row 47: in the air */
        oot_air a = { 0 };
        const uint64_t f = 16666667ull;
        oot_air_step(&a, 0, 1000000000ull);
        for (int i = 1; i <= 10; i++) oot_air_step(&a, 0.05f * (float)i, 1000000000ull + (uint64_t)i * f);   /* 3 m/s up */
        assert(oot_air_borne(&a) && a.vy > 2.5f && a.vy < 3.1f);
        oot_air_step(&a, 0.5f, 1000000000ull + 10 * f + 500000ull);   /* under 1 ms: left out */
        assert(a.vy > 2.5f);
        oot_air_step(&a, 0.5f, 1000000000ull + 10 * f + 300000000ull); /* a gap: over at 0 */
        assert(!oot_air_borne(&a) && a.vy == 0);
        for (int i = 1; i <= 10; i++) oot_air_step(&a, 0.5f + 0.01f * (float)i, 1300000000ull + 10 * f + (uint64_t)i * f);   /* walking up a slope, 0.6 m/s */
        assert(!oot_air_borne(&a));
        assert(oot_hit_kind(13, 1, 1, 1) == 22 && oot_hit_kind(13, 1, 0, 1) == 13 && oot_hit_kind(13, 1, 1, 0) == 13);
        assert(oot_hit_kind(101, 0, 1, 1) == 101 && oot_hit_kind(13, 0, 1, 1) == 13);   /* the stick, a large sword: their own */
        oot_air_step(&a, NAN, 1400000000ull); assert(isfinite(a.vy));
    }
    printf("oot_vr_contact: all passed\n");
    return 0;
}
