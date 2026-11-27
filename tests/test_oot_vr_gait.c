/* The first-person legs' cycles (zeldaVR/runtime/oot_vr_gait.h) on the file tests/test_oot_vr_gait.py makes from a made-up
   skeleton: Root, Skl_Root 0.5 m up, Waist, Leg_1_L, Ankle_L 0.4 m below it; every cycle 8 frames, its Root travelling as
   named, the left leg swinging up 0.8 rad at mid-cycle. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../zeldaVR/runtime/oot_vr_gait.h"

#define NEAR(a, b, e) (fabsf((a) - (b)) < (e))
static oot_gait g;
static oot_gait_local pose[OOT_GAIT_BONES], scratch[2 * OOT_GAIT_BONES];
static float world[OOT_GAIT_BONES * 12], frame[OOT_GAIT_BONES * 12];
static const float I[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
static int clip(const char *name) { const int c = oot_gait_find_clip(&g, name); assert(c >= 0); return c; }
static float ankle_at(float vx, float vz, float phase) {
    oot_gait_pick p; oot_gait_choose(&g, vx, vz, &p); oot_gait_pose(&g, &p, phase, pose, scratch); oot_gait_world(&g, pose, I, world, frame);
    return world[3 * 12 + 7];
}

int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n); assert(d && fread(d, 1, (size_t)n, f) == (size_t)n); fclose(f);
    /* refused: cut short, another file, a value not finite */
    assert(oot_gait_open(&g, d, (size_t)n - 4)); assert(oot_gait_open(&g, d, 20));
    { uint8_t *bad = malloc((size_t)n); memcpy(bad, d, (size_t)n); bad[0] = 'X'; assert(oot_gait_open(&g, bad, (size_t)n));
      memcpy(bad, d, (size_t)n); const float nan = NAN; memcpy(bad + n - 4, &nan, 4); assert(oot_gait_open(&g, bad, (size_t)n)); free(bad); }
    const char *e = oot_gait_open(&g, d, (size_t)n);
    if (e) { printf("%s\n", e); return 1; }
    /* the posed bones: Skl_Root first, then Waist and below; Spine_1 is not one */
    assert(g.bones == 4 && !strcmp(g.names[0], "Skl_Root") && !strcmp(g.names[1], "Waist") && !strcmp(g.names[3], "Ankle_L"));
    assert(g.parent[0] == -1 && g.parent[1] == 0 && g.parent[2] == 1 && g.parent[3] == 2 && oot_gait_find_bone(&g, "Spine_1") < 0);
    assert(g.clips == 14 && g.stand == (unsigned)clip("Wait") && g.clip[g.stand].count == 1);
    const oot_gait_clip *walk = &g.clip[clip("Move_Walk")];
    assert(walk->frames == 8 && walk->count == 4 && NEAR(walk->travel[0], 0, 1e-4f) && NEAR(walk->travel[1], 1, 1e-4f));
    assert(NEAR(walk->speed, 7.5f, 1e-3f) && NEAR(walk->dir, 0, 1e-4f));
    assert(walk->stance < 0.05f || walk->stance > 0.95f);   /* planted at the cycle's start and end */
    assert(NEAR(g.clip[clip("LockOn_Run_L")].dir, (float)M_PI / 2, 1e-4f) && NEAR(g.clip[clip("LockOn_Run_BL")].dir, (float)M_PI, 1e-4f));

    oot_gait_pick p;
    oot_gait_choose(&g, 0, 7.5f, &p);   /* ahead at the walk's speed: the walk alone */
    assert(p.clip[0] == clip("Move_Walk") && NEAR(p.weight[0] + p.weight[1], 1, 1e-5f) && (p.clip[1] == p.clip[0] || p.weight[1] < 1e-5f));
    assert(NEAR(p.moving, 1, 1e-6f) && NEAR(p.stride, 1, 1e-4f));
    oot_gait_choose(&g, 0, 15, &p); assert(p.clip[0] == clip("Move_Jog"));            /* faster: the jog */
    oot_gait_choose(&g, 15, 0, &p); assert(p.clip[0] == clip("LockOn_Run_L"));        /* to his left at a run */
    oot_gait_choose(&g, 7.5f, 0, &p); assert(p.clip[0] == clip("LockOn_Walk_L"));     /* ... at a walk */
    oot_gait_choose(&g, -7.5f, 0, &p); assert(p.clip[0] == clip("LockOn_Walk_R"));
    oot_gait_choose(&g, 0, -15, &p); assert(p.clip[0] == clip("LockOn_Run_BL") && NEAR(p.weight[0], 1, 1e-4f));   /* backwards */
    {   /* half way between ahead and ahead-left: the two, half each */
        const float a = (float)M_PI / 8; oot_gait_choose(&g, 7.5f * sinf(a), 7.5f * cosf(a), &p);
        const int ahead = p.clip[0] == clip("Move_Walk") ? 0 : 1;
        assert(p.clip[ahead] == clip("Move_Walk") && p.clip[1 - ahead] == clip("LockOn_Run_LF") && NEAR(p.weight[0], 0.5f, 1e-3f));
        assert(NEAR(p.stride, 0.5f * 1 + 0.5f * hypotf(1.4f, 1.4f), 1e-3f));
    }
    oot_gait_choose(&g, 0, 0.1f, &p); assert(p.moving == 0);                         /* standing */
    oot_gait_choose(&g, 0, 0.375f, &p); assert(NEAR(p.moving, 0.5f, 1e-4f));         /* starting to walk */
    assert(NEAR(oot_gait_advance(0.9f, 0.2f, 1), 0.1f, 1e-5f) && NEAR(oot_gait_advance(0.3f, 5, 0.01f), 0.3f, 1e-6f) && NEAR(oot_gait_advance(0.3f, NAN, 1), 0.3f, 1e-6f));

    /* standing: the rest pose, the ankle 0.1 m up */
    assert(NEAR(ankle_at(0, 0, 0.5f), 0.1f, 1e-4f));
    /* walking: planted at the shared phase 0 (the left foot mid-stance), lifted half a cycle later */
    assert(ankle_at(0, 7.5f, 0) < 0.11f);
    { const float up = ankle_at(0, 7.5f, 0.5f); assert(up > 0.18f && up < 0.23f); }
    /* unit quaternions after every blend */
    { const float a = (float)M_PI / 8; ankle_at(7.5f * sinf(a), 7.5f * cosf(a), 0.37f);
      for (unsigned b = 0; b < g.bones; b++) { const float *q = pose[b].q; assert(NEAR(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1, 1e-4f)); } }
    free(d);
    puts("gait checks passed");
    return 0;
}
