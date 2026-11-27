/* Ocarina of Time in VR, first person: how high the eyes are, without the runtime (tests/test_oot_vr_stature.c runs it).
   - Standing: an eye height for adult Link (option eye_cm, 160 by default) and child Link's as much smaller as he is (x 125/160),
     HookshotVR's rule (camera-ui.md 3.2; ideas only, GPL-3). The VR page's camera up / down offset still goes on top.
   - A crawl space: the eyes come down to a crawl eye height, the player's (option crawl_cm) or, at 0 (Auto), the game's own view
     height for Link kept between 15 and 50 cm (camera-ui.md 3.5); the first-person arms take the game's own crawl pose, on hands
     and knees, as far as the eyes have come down (oot_crawl_pose_weight). */
#pragma once
#include <math.h>
#include <string.h>
#include "oot_vr_comfort.h"

enum { OOT_EYE_ADULT_CM = 160, OOT_EYE_MIN_CM = 100, OOT_EYE_MAX_CM = 220, OOT_CRAWL_MIN_CM = 15, OOT_CRAWL_MAX_CM = 50 };

static inline int oot_eye_cm_clamp(int cm) { return cm < OOT_EYE_MIN_CM ? OOT_EYE_MIN_CM : cm > OOT_EYE_MAX_CM ? OOT_EYE_MAX_CM : cm; }
/* Metres above Link's feet: the adult's setting, child Link's scaled by 125/160. */
static inline float oot_eye_height(int adult_cm, int child) {
    const float adult = (float)oot_eye_cm_clamp(adult_cm) / 100.0f;
    return child ? adult * 125.0f / 160.0f : adult;
}
/* The eye height the VR page asks for: 0 (the default since the merge of 2026-11-27) is Auto, the heights first person has had
   (adult 1.55 m, child 1.00 m; zeldaVR/gfx/totk_vr.m before row 6), anything else the adult's setting and a child's in proportion. */
enum { OOT_EYE_AUTO = 0 };
static inline float oot_eye_height_set(int adult_cm, int child) { return adult_cm <= OOT_EYE_AUTO ? (child ? 1.00f : 1.55f) : oot_eye_height(adult_cm, child); }
/* 0 is Auto (the game's own crawl view height); anything else is that many cm, kept to 15..50. */
static inline int oot_crawl_cm_clamp(int cm) { return cm <= 0 ? 0 : cm < OOT_CRAWL_MIN_CM ? OOT_CRAWL_MIN_CM : cm > OOT_CRAWL_MAX_CM ? OOT_CRAWL_MAX_CM : cm; }
static inline float oot_crawl_eye_set(int set_cm, float game_height) {
    const int cm = oot_crawl_cm_clamp(set_cm);
    return cm ? (float)cm / 100.0f : oot_crawl_eye(game_height);
}
/* Like oot_crawl_drop, to a given crawl eye height. The height used is kept while getting up (the game's view height is a
   standing one again then): *kept holds it. */
static inline float oot_crawl_drop_to(oot_crawl *c, int crawling, float standing_eye, float crawl_eye, float *kept, float dt) {
    if (!(dt > 0)) dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    if (crawling && crawl_eye > 0) *kept = crawl_eye;
    c->amount += crawling ? dt / 0.35f : -dt / 0.35f;
    c->amount = c->amount < 0 ? 0 : c->amount > 1 ? 1 : c->amount;
    const float eye = *kept > 0 ? *kept : 0.30f, down = standing_eye - eye, a = c->amount;
    return (down > 0 ? down : 0) * a * a * (3 - 2 * a);
}
/* How much of the game's own pose the arms take: as far as the eyes have come down into the crawl. */
static inline float oot_crawl_pose_weight(float amount) {
    const float a = amount < 0 ? 0 : amount > 1 ? 1 : amount;
    return a * a * (3 - 2 * a);
}
/* a + (b - a) w, entry by entry (two skinning matrices: a short blend between two poses). */
static inline void oot_matrix_lerp(const float a[12], const float b[12], float w, float out[12]) {
    float r[12];
    for (int i = 0; i < 12; i++) r[i] = a[i] + (b[i] - a[i]) * w;
    memcpy(out, r, sizeof r);
}
