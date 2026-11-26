/* The wrist panel: when it shows, the hearts, where it lies (zeldaVR/runtime/oot_vr_wrist.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_wrist.h"

static oot_wrist_state s;
static float run(const oot_wrist_in *in, float seconds) { float v = 0; for (float t = 0; t < seconds - 1e-4f; t += 0.0125f) v = oot_wrist_step(&s, in, 0.0125f); return v; }

int main(void) {
    const oot_wrist_in look = { 0.30f, 10, 10, 0.1f, 0 }, away = { 0.60f, 80, 60, 0.1f, 0 }, moving = { 0.30f, 10, 10, 0.75f, 0 }, fast = { 0.30f, 10, 10, 1.5f, 0 };
    oot_wrist_in blocked = look; blocked.blocked = 1;

    memset(&s, 0, sizeof s);
    assert(run(&look, 0.4f) == 0 && !s.on);                       /* not before half a second of looking */
    assert(run(&look, 0.15f) > 0 && s.on);                        /* then it fades in */
    assert(run(&look, 0.2f) == 1);
    assert(run(&moving, 1.0f) == 1 && s.on);                      /* a little movement keeps it */
    run(&away, 0.15f); assert(s.on);                              /* the look lost for a moment: still there */
    run(&away, 0.1f); assert(!s.on);                              /* 0.2 s: it goes */
    assert(run(&away, 0.3f) == 0);
    run(&look, 1.0f); assert(s.on); assert(oot_wrist_step(&s, &blocked, 0.0125f) == 0 && !s.on);   /* a fight: gone at once */
    memset(&s, 0, sizeof s); assert(run(&moving, 2.0f) == 0);     /* a moving hand never brings it up */
    memset(&s, 0, sizeof s); run(&look, 1.0f); run(&fast, 0.25f); assert(!s.on);
    memset(&s, 0, sizeof s); run(&look, 0.3f); run(&away, 0.05f); run(&look, 0.3f); assert(!s.on);   /* the half second starts again */

    /* hearts by quarters */
    assert(oot_heart_count(12) == 3 && oot_heart_count(13) == 4 && oot_heart_count(0) == 0 && oot_heart_count(80) == 20);
    assert(oot_heart_quarters(11, 0) == 4 && oot_heart_quarters(11, 1) == 4 && oot_heart_quarters(11, 2) == 3 && oot_heart_quarters(11, 3) == 0);
    assert(oot_heart_quarters(0, 0) == 0);

    /* the left arm across the chest, fingers to the right, the back of the wrist up: words run left to right, their tops away */
    const float grip[3] = { 0, 1.2f, -0.3f }, upw[3] = { 0, 1, 0 }, right[3] = { 1, 0, 0 }, leftw[3] = { -1, 0, 0 };
    oot_wrist_quad q = oot_wrist_place(grip, upw, right, right, 0.11f, 1.6f);
    assert(fabsf(q.right[0] - 0.11f) < 1e-6f && fabsf(q.up[2] + 0.11f / 1.6f) < 1e-6f && fabsf(q.up[1]) < 1e-6f);
    assert(fabsf(q.center[0] + 0.08f) < 1e-6f && fabsf(q.center[1] - 1.235f) < 1e-6f);           /* toward the elbow, off the skin */
    /* the right arm, fingers to the left: the same reading direction, the panel toward its own elbow */
    q = oot_wrist_place(grip, upw, leftw, right, 0.11f, 1.6f);
    assert(fabsf(q.right[0] - 0.11f) < 1e-6f && fabsf(q.up[2] + 0.11f / 1.6f) < 1e-6f && fabsf(q.center[0] - 0.08f) < 1e-6f);
    puts("the wrist panel: half a second of looking, kept through small movements, gone when blocked; hearts by quarters; its place");
    return 0;
}
