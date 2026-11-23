/* Where Ocarina of Time's HUD hangs in the headset (zeldaVR/runtime/oot_vr_hud.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_hud.h"

#define DEG ((float)M_PI / 180)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

int main(void) {
    const float ahead[3] = { 0, 0, -1 }, right[3] = { 1, 0, 0 };
    assert(NEAR(oot_hud_heading(ahead), 0));
    assert(NEAR(oot_hud_heading(right), 90 * DEG));
    assert(NEAR(oot_hud_wrap(190 * DEG), -170 * DEG));

    oot_hud_state s = {0};
    assert(NEAR(oot_hud_follow(&s, 10 * DEG, 0.01f), 10 * DEG));      /* the first pose places it */
    assert(NEAR(oot_hud_follow(&s, 29 * DEG, 0.1f), 10 * DEG));       /* 19 degrees away: it stays */
    float y = oot_hud_follow(&s, 40 * DEG, 0.1f);                     /* 30 degrees away: it follows */
    assert(y > 10 * DEG && y < 40 * DEG);
    for (int i = 0; i < 200; i++) y = oot_hud_follow(&s, 40 * DEG, 0.02f);
    assert(fabsf(y - 40 * DEG) <= 2.01f * DEG && !s.following);       /* and stops within 2 degrees */
    /* the short way round across +-180 */
    oot_hud_state w = { 170 * DEG, 1, 0 };
    y = oot_hud_follow(&w, -160 * DEG, 0.1f);
    assert(y > 170 * DEG || y < -170 * DEG);

    const float head[3] = { 1, 1.6f, 2 };
    oot_hud_quad q = oot_hud_place(head, 0);
    assert(NEAR(q.center[0], 1) && NEAR(q.center[1], 1.55f) && NEAR(q.center[2], 0));
    assert(NEAR(q.right[0], 4 * tanf(35 * DEG)) && NEAR(q.up[1], q.right[0] * 9 / 16));
    q = oot_hud_place(head, 90 * DEG);
    assert(NEAR(q.center[0], 3) && NEAR(q.center[2], 2) && NEAR(q.right[2], 4 * tanf(35 * DEG)));

    float uv[4];
    oot_hud_band(1344, 1408, uv);                                     /* the VR canvas: the middle band */
    assert(NEAR(uv[0], 1) && NEAR(uv[2], 0) && NEAR(uv[1], (1344.f / 1408) / (16.f / 9)) && NEAR(uv[3] * 2 + uv[1], 1));
    oot_hud_band(1920, 1080, uv);
    assert(NEAR(uv[0], 1) && NEAR(uv[1], 1) && NEAR(uv[2], 0) && NEAR(uv[3], 0));
    oot_hud_band(2520, 1080, uv);
    assert(NEAR(uv[1], 1) && NEAR(uv[0], (16.f / 9) / (2520.f / 1080)) && NEAR(uv[2] * 2 + uv[0], 1));
    puts("OOT HUD panel: stays, follows past 20 degrees, stops within 2, the short way round, placed 2 m ahead, the 16:9 band");
    return 0;
}
