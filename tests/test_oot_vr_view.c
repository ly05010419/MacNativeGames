/* Ocarina of Time's first-person view (zeldaVR/runtime/oot_vr_view.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_view.h"

static int close3(const float a[3], float x, float y, float z) {
    return fabsf(a[0] - x) < 1e-4f && fabsf(a[1] - y) < 1e-4f && fabsf(a[2] - z) < 1e-4f;
}

int main(void) {
    assert(fabsf(oot_wrap_pi(3 * (float)M_PI) - (float)-M_PI) < 1e-4f || fabsf(oot_wrap_pi(3 * (float)M_PI) - (float)M_PI) < 1e-4f);
    assert(fabsf(oot_wrap_pi(0.5f) - 0.5f) < 1e-6f);
    assert(oot_wrap_pi(NAN) == 0);

    /* yaw 0 faces -z; a quarter turn faces +x */
    float game[12]; oot_level_rows(0, game);
    const float third[3] = { 10, 5, -3 };
    vr_view_translation(game, third);
    assert(fabsf(oot_view_yaw(game)) < 1e-6f);

    oot_fp_input in = { { 1, 2, 3 }, (float)M_PI / 2, 1, 1.0f, 0.14f, { 0, 0, 0 } };
    float v[12], p[3];
    oot_fp_base(game, &in, v);
    vr_view_eye(v, p);
    assert(close3(p, 1 + 0.14f, 3, 3));                     /* feet + eye height + forward along +x */
    assert(fabsf(oot_wrap_pi(oot_view_yaw(v) - (float)M_PI / 2)) < 1e-5f);

    /* the head's own movement adds in world axes */
    in.off[0] = 0.1f; in.off[1] = -0.2f; in.off[2] = 0.3f;
    oot_fp_base(game, &in, v); vr_view_eye(v, p);
    assert(close3(p, 1.24f, 2.8f, 3.3f));
    in.off[0] = in.off[1] = in.off[2] = 0;

    /* blend 0 is the game's camera, half way is half way, the short way round */
    in.blend = 0; oot_fp_base(game, &in, v); vr_view_eye(v, p);
    assert(close3(p, 10, 5, -3) && fabsf(oot_view_yaw(v)) < 1e-6f);
    in.blend = 0.5f; oot_fp_base(game, &in, v); vr_view_eye(v, p);
    assert(close3(p, (10 + 1.14f) / 2, 4, 0));
    assert(fabsf(oot_view_yaw(v) - (float)M_PI / 4) < 1e-5f);
    in.yaw = 3.0f; float g2[12]; oot_level_rows(-3.0f, g2); vr_view_translation(g2, third);
    oot_fp_base(g2, &in, v);                                /* -3 to 3 rad is 0.28 rad across pi, not 6 */
    assert(fabsf(oot_wrap_pi(oot_view_yaw(v) - oot_wrap_pi(-3.0f - 0.1416f))) < 1e-3f);

    /* the culling view moves the camera back along its sight line, and only that */
    in.yaw = 0.7f; in.blend = 1; oot_fp_base(game, &in, v);
    float c[12], before[3], after[3], f[3];
    oot_cull_view(v, 1.0f, c);
    vr_view_eye(v, before); vr_view_eye(c, after);
    for (int i = 0; i < 3; i++) f[i] = -v[8 + i];            /* row 2 is the camera's back */
    assert(close3(after, before[0] - f[0], before[1] - f[1], before[2] - f[2]));
    for (int i = 0; i < 11; i++) assert(c[i] == v[i]);

    /* the true view comes back by the exact matrix, not by a near one */
    static oot_cull_ring ring;
    float z = 0;
    assert(!oot_cull_find(&ring, c, &z));
    for (int n = 0; n < 5; n++) { float o[12]; memcpy(o, c, sizeof o); o[3] += (float)n; oot_cull_put(&ring, o, (float)n); }
    oot_cull_put(&ring, c, v[11]);
    assert(oot_cull_find(&ring, c, &z) && z == v[11]);
    float other[12]; memcpy(other, c, sizeof other); other[0] += 0.001f;
    assert(!oot_cull_find(&ring, other, &z));
    memcpy(other, c, sizeof other); other[11] += 0.01f;
    assert(!oot_cull_find(&ring, other, &z));
    for (int n = 0; n < OOT_CULL_RING; n++) { float o[12] = { (float)n + 100 }; oot_cull_put(&ring, o, 0); }
    assert(!oot_cull_find(&ring, c, &z));                    /* a whole ring later it is gone */
    puts("OOT first-person view: eyes, head movement, blend, culling view and its exact way back");
    return 0;
}
