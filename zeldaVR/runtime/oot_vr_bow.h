/* Aiming Ocarina of Time's bow and slingshot with both hands, without the runtime (tests/test_oot_vr_bow.c runs it). The game
   narrows its camera while it aims (under 46.5 degrees); then the line from the drawing hand to the bow hand, between 14 and
   95 cm long, is where the shot should go, and its error against the game's aim becomes the right stick: six times the error
   in radians, at least 0.22 so the game's own dead zone does not swallow it, nothing inside half a degree (the behaviour
   OcarinaVR describes; ideas only, GPL-3). Directions in the frame of the game's aim: x right, y up, -z ahead. */
#pragma once
#include <math.h>

typedef struct { int aiming, holding; float stick[2]; float dir[3]; } oot_bow_out;

static inline int oot_bow_is_aiming(float game_fov_deg) { return game_fov_deg > 1 && game_fov_deg < 46.5f; }

static inline float oot_bow_axis(float error) {
    const float dead = 0.5f * (float)M_PI / 180;
    if (fabsf(error) < dead) return 0;
    return copysignf(fminf(1, fmaxf(0.22f, fabsf(error) * 6)), error);
}
/* bow, draw: the two hands' positions in that frame. */
static inline void oot_bow_step(float game_fov_deg, const float bow[3], const float draw[3], oot_bow_out *o) {
    o->aiming = oot_bow_is_aiming(game_fov_deg); o->holding = 0; o->stick[0] = o->stick[1] = 0;
    o->dir[0] = 0; o->dir[1] = 0; o->dir[2] = -1;
    if (!o->aiming) return;
    const float d[3] = { bow[0] - draw[0], bow[1] - draw[1], bow[2] - draw[2] };
    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(len >= 0.14f && len <= 0.95f)) return;
    o->holding = 1;
    for (int i = 0; i < 3; i++) o->dir[i] = d[i] / len;
    o->stick[0] = oot_bow_axis(atan2f(o->dir[0], -o->dir[2]));
    o->stick[1] = oot_bow_axis(asinf(fmaxf(-1, fminf(1, o->dir[1]))));
}
