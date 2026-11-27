/* First person's eye height by Link's age and in crawl spaces (zeldaVR/runtime/oot_vr_stature.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_stature.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

int main(void) {
    /* HookshotVR's rule: 160 cm for adult Link, a child's 125/160 of it */
    assert(NEAR(oot_eye_height(160, 0), 1.60f) && NEAR(oot_eye_height(160, 1), 1.25f));
    assert(NEAR(oot_eye_height(176, 1), 1.375f));                         /* the setting moves both */
    assert(NEAR(oot_eye_height(20, 0), 1.00f) && NEAR(oot_eye_height(900, 0), 2.20f));   /* kept to 100..220 cm */
    /* the VR page's 0 (Auto, the default): the heights first person had before (adult 1.55 m, child 1.00 m) */
    assert(NEAR(oot_eye_height_set(0, 0), 1.55f) && NEAR(oot_eye_height_set(0, 1), 1.00f) && NEAR(oot_eye_height_set(-5, 0), 1.55f));
    assert(NEAR(oot_eye_height_set(160, 0), 1.60f) && NEAR(oot_eye_height_set(160, 1), 1.25f));
    /* the crawl eye: 0 is the game's own view height, kept to 15..50 cm (unreadable: 30 cm); a setting is that many cm */
    assert(NEAR(oot_crawl_eye_set(0, 0.29f), 0.29f) && NEAR(oot_crawl_eye_set(0, 0.05f), 0.15f) && NEAR(oot_crawl_eye_set(0, 0), 0.30f));
    assert(NEAR(oot_crawl_eye_set(40, 0.29f), 0.40f) && NEAR(oot_crawl_eye_set(5, 0.29f), 0.15f) && NEAR(oot_crawl_eye_set(80, 0.29f), 0.50f));
    assert(oot_crawl_cm_clamp(-3) == 0 && oot_crawl_cm_clamp(0) == 0);
    /* going down over 0.35 s, eased; all the way: standing eye minus the crawl eye */
    oot_crawl c = {0}; float kept = 0, drop = 0;
    drop = oot_crawl_drop_to(&c, 1, 1.25f, 0.29f, &kept, 0.05f); assert(drop > 0 && drop < 0.1f);
    for (int k = 0; k < 10; k++) drop = oot_crawl_drop_to(&c, 1, 1.25f, 0.29f, &kept, 0.05f);
    assert(NEAR(drop, 1.25f - 0.29f) && NEAR(c.amount, 1));
    /* getting up: the game's view height is a standing one again, the crawl eye used stays the same on the way up */
    const float up1 = oot_crawl_drop_to(&c, 0, 1.25f, 0.91f, &kept, 0.05f);
    assert(NEAR(kept, 0.29f) && up1 < drop && up1 > 0.5f);
    for (int k = 0; k < 10; k++) drop = oot_crawl_drop_to(&c, 0, 1.25f, 0.91f, &kept, 0.05f);
    assert(NEAR(drop, 0) && NEAR(c.amount, 0));
    /* a long gap is one step of 0.1 s at most; a negative one is none */
    c.amount = 0; kept = 0; drop = oot_crawl_drop_to(&c, 1, 1.6f, 0.29f, &kept, 5); assert(NEAR(c.amount, 0.1f / 0.35f));
    const float a = c.amount; oot_crawl_drop_to(&c, 1, 1.6f, 0.29f, &kept, -1); assert(NEAR(c.amount, a));
    /* the arms take the game's crawl as far as the eyes are down, eased the same way */
    assert(NEAR(oot_crawl_pose_weight(0), 0) && NEAR(oot_crawl_pose_weight(1), 1) && NEAR(oot_crawl_pose_weight(0.5f), 0.5f));
    assert(NEAR(oot_crawl_pose_weight(-2), 0) && NEAR(oot_crawl_pose_weight(3), 1) && oot_crawl_pose_weight(0.25f) < 0.25f);
    const float x[12] = { 1, 0, 0, 2, 0, 1, 0, 4, 0, 0, 1, 6 }, y[12] = { 0, 0, 0, 4, 0, 0, 0, 8, 0, 0, 0, 10 };
    float m[12]; oot_matrix_lerp(x, y, 0.5f, m);
    assert(NEAR(m[0], 0.5f) && NEAR(m[3], 3) && NEAR(m[7], 6) && NEAR(m[11], 8));
    oot_matrix_lerp(x, y, 0, m); assert(!memcmp(m, x, sizeof m));
    puts("stature checks passed");
    return 0;
}
