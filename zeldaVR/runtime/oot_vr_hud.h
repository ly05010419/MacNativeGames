/* Where Ocarina of Time's HUD hangs in the headset, without the runtime (tests/test_oot_vr_hud.c runs it): a flat 16:9 panel
   2 m ahead and 5 cm below the eyes, 70 degrees wide. It stays where it is while the head looks around and follows, eased,
   once the head has turned more than 20 degrees away from it, until it is within 2 degrees again (the layout OcarinaVR
   describes; ideas only, GPL-3). Metres, OpenXR axes: y up, -z forward. */
#pragma once
#include <math.h>

typedef struct { float yaw; int have, following; } oot_hud_state;
typedef struct { float center[3], right[3], up[3]; } oot_hud_quad;

static inline float oot_hud_wrap(float a) {
    const float two_pi = 2 * (float)M_PI;
    a = fmodf(a + (float)M_PI, two_pi);
    if (a < 0) a += two_pi;
    return a - (float)M_PI;
}
/* The heading of a forward vector: 0 along -z, positive to the right. */
static inline float oot_hud_heading(const float forward[3]) { return atan2f(forward[0], -forward[2]); }

static inline float oot_hud_follow(oot_hud_state *s, float head_yaw, float dt) {
    const float start = 20 * (float)M_PI / 180, stop = 2 * (float)M_PI / 180;
    if (!s->have) { s->have = 1; s->yaw = head_yaw; s->following = 0; return s->yaw; }
    const float d = oot_hud_wrap(head_yaw - s->yaw), m = fabsf(d);
    if (m > start) s->following = 1;
    else if (m < stop) s->following = 0;
    if (s->following) {
        const float take = 1 - expf(-fmaxf(dt, 0) * 6);
        s->yaw = oot_hud_wrap(s->yaw + d * take);
    }
    return s->yaw;
}
/* The whole quad: `right` and `up` are its full width and height. */
static inline oot_hud_quad oot_hud_place(const float head[3], float yaw) {
    const float dist = 2.0f, width = 2 * dist * tanf(35 * (float)M_PI / 180), height = width * 9 / 16;
    const float fx = sinf(yaw), fz = -cosf(yaw);
    oot_hud_quad q = { { head[0] + fx * dist, head[1] - 0.05f, head[2] + fz * dist },
                       { cosf(yaw) * width, 0, sinf(yaw) * width },
                       { 0, height, 0 } };
    return q;
}
/* The game lays its HUD out in the middle 16:9 of the canvas. uv: scale x, y and offset x, y of that band. */
static inline void oot_hud_band(float w, float h, float uv[4]) {
    uv[0] = uv[1] = 1; uv[2] = uv[3] = 0;
    if (!(w > 0) || !(h > 0)) return;
    const float aspect = w / h, wide = 16.0f / 9;
    if (aspect < wide) { uv[1] = aspect / wide; uv[3] = (1 - uv[1]) / 2; }
    else { uv[0] = wide / aspect; uv[2] = (1 - uv[0]) / 2; }
}
