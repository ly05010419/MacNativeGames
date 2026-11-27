/* zeldaVR, Ocarina of Time, first person: the legs (row 32 of vr.md). With the option legs on, his body shows his legs too (Waist
   and below), facing where the player looks (the arms' forward, eased), and stepping the way he really goes: the game's own walk,
   run, side, diagonal and back cycles and his standing pose, chosen and blended by his direction of travel relative to the hips
   and his speed (zeldaVR/runtime/oot_vr_gait.h), all in step by the distance he goes. The hips sit where the game's own hips are.
   The cycles: extracted/oot/vr-rig/gait-<age>.bin (games/oot/scripts/make_vr_gait.py, from the player's own animation files).
   Without that file the legs are not drawn, and that is logged once. In a crawl they take the game's own pose with the arms.
   Diagnostics: TOTK_VR_BLADELOG=1 logs the cycles chosen when they change. Included by oot_vr_arms.inc. */
#include "../../../../zeldaVR/runtime/oot_vr_gait.h"
int vr_oot_legs(void);   /* totk_vr.m, option legs */

typedef struct {
    uint8_t *file; oot_gait g; int ok, waist;
    int slot[OOT_GAIT_BONES];          /* each posed bone's smooth palette entry, -1 none */
    float inv[OOT_GAIT_BONES][12];     /* its inverse rest */
    float root[12], waist_rest[3];     /* Root's rest (the first bone's parent), the Waist's rest joint */
    float phase, yaw, vel[2], last_off[3] /* his Waist in the world, the frame before */, mats[OOT_GAIT_BONES][12];
    uint64_t last_ns, made_frame; int have_yaw, have_off, made, clips_said[2];
    oot_gait_local pose[OOT_GAIT_BONES], scratch[2 * OOT_GAIT_BONES];
    float world[OOT_GAIT_BONES][12], frame[OOT_GAIT_BONES][12];
} oot_legs;
static oot_legs g_oot_legs[2];
static int g_oot_legs_log = -1;

static const char *oot_legs_prepare(oot_legs *l, const oot_rig *r, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return "no file (an App packaged before the stepping legs: update it)";
    fseek(f, 0, SEEK_END); const long size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > (16L << 20)) { fclose(f); return "unexpected size"; }
    l->file = malloc((size_t)size);
    if (!l->file || fread(l->file, 1, (size_t)size, f) != (size_t)size) { fclose(f); return "cannot read it"; }
    fclose(f);
    const char *e = oot_gait_open(&l->g, l->file, (size_t)size);
    if (e) return e;
    const oot_bfres *m = &r->m;
    float *rest = malloc((size_t)m->bone_count * 48);
    if (!rest || (e = oot_bfres_rest_world(m, rest, m->bone_count))) { free(rest); return e ? e : "out of memory"; }
    l->waist = oot_gait_find_bone(&l->g, "Waist");
    int slots = 0;
    for (unsigned k = 0; k < l->g.bones && !e; k++) {
        const int b = oot_bfres_find_bone(m, l->g.names[k]);
        oot_bfres_bone x;
        if (b < 0 || oot_bfres_bone_at(m, (unsigned)b, &x)) { e = "a cycle's bone is not in his skeleton"; break; }
        if (k == 0) {   /* the first posed bone's parent: Root, at its rest */
            if (x.parent >= 0) memcpy(l->root, rest + (size_t)x.parent * 12, 48); else oot_m_identity(l->root);
        } else {
            const int want = oot_bfres_find_bone(m, l->g.names[l->g.parent[k]]);
            if (want != x.parent) { e = "a cycle's bone has another parent than in his skeleton"; break; }
        }
        l->slot[k] = x.smooth >= 0 && (unsigned)x.smooth < m->smooth_count ? x.smooth : -1;
        if (l->slot[k] >= 0 && (e = oot_bfres_inverse_rest(m, (unsigned)l->slot[k], l->inv[k]))) break;
        slots += l->slot[k] >= 0;
        if ((int)k == l->waist) { l->waist_rest[0] = rest[b * 12 + 3]; l->waist_rest[1] = rest[b * 12 + 7]; l->waist_rest[2] = rest[b * 12 + 11]; }
    }
    free(rest);
    if (!e && (l->waist < 0 || l->slot[l->waist] < 0)) e = "no Waist with a palette entry";
    if (!e && !slots) e = "no palette entries";
    return e;
}
static void oot_legs_load(void) {
    static const char *const paths[2] = { "extracted/oot/vr-rig/gait-child.bin", "extracted/oot/vr-rig/gait-adult.bin" };
    for (int i = 0; i < 2; i++) {
        oot_legs *l = &g_oot_legs[i];
        const char *e = g_oot_rigs[i].legs ? oot_legs_prepare(l, &g_oot_rigs[i], paths[i]) : "his model has no Waist";
        l->ok = !e;
        if (e) MLOG("[vr-legs] %s: %s; first person does not draw his legs", paths[i], e);
        else MLOG("[vr-legs] %s Link's legs step: %u bones, %u cycles, %u samples (%s)", i ? "adult" : "child", l->g.bones, l->g.clips, l->g.samples, paths[i]);
    }
}
static int oot_legs_on(const oot_rig *r) {
    const int i = r == &g_oot_rigs[0] ? 0 : r == &g_oot_rigs[1] ? 1 : -1;
    return i >= 0 && g_oot_legs[i].ok && vr_oot_legs();
}
static int oot_legs_logging(void) { if (g_oot_legs_log < 0) { const char *e = getenv("TOTK_VR_BLADELOG"); g_oot_legs_log = e && *e == '1'; } return g_oot_legs_log; }
/* This frame's legs into the palette copy `to` (his body's, made from `src`): fw the arms' forward in the palette's space, off the
   model block's translation (his place in the world), crawl how far the arms take the game's pose. Once a frame; his other draws
   of the frame take the same matrices. */
static void oot_legs_pose(int ri, const uint8_t *src, uint8_t *to, uint64_t size, const float fw[3], const float off[3], float crawl) {
    oot_legs *l = &g_oot_legs[ri];
    if (!l->ok || !vr_oot_legs()) return;
    const uint64_t frame = atomic_load(&g_cur_frame);
    if (l->made_frame != frame) {
        l->made_frame = frame; l->made = 0;
        const uint64_t ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        float dt = l->last_ns ? (float)((double)(ns - l->last_ns) / 1e9) : 0; l->last_ns = ns;
        if (!(dt >= 0) || dt > 0.25f) dt = 0;   /* a pause, a load: start over without a jump */
        /* Where he is: the game's own Waist, in the world. The model block's translation alone is not his place: it is an origin
           the game moves in whole steps (batch7, 2026-11-27: it stayed 0 100 10 while he walked, then jumped to 0 100 20, while
           his Waist in the palette went 5.93 -> 9.47 and 1.47 -> 3.38 along z). */
        const int ws = l->slot[l->waist];
        if ((uint64_t)(ws + 1) * 48 > size) return;
        float game_waist[12], gw[3], at[3];
        memcpy(game_waist, src + (size_t)ws * 48, 48); oot_m_apply(game_waist, l->waist_rest, gw);
        for (int i = 0; i < 3; i++) at[i] = gw[i] + off[i];
        float d[2] = { 0, 0 };
        if (l->have_off) { d[0] = at[0] - l->last_off[0]; d[1] = at[2] - l->last_off[2]; }
        if (!(fabsf(d[0]) < 2 && fabsf(d[1]) < 2)) d[0] = d[1] = 0;   /* a warp */
        memcpy(l->last_off, at, sizeof l->last_off); l->have_off = 1;
        if (dt > 0) { const float k = fminf(1, dt * 10); l->vel[0] += (d[0] / dt - l->vel[0]) * k; l->vel[1] += (d[1] / dt - l->vel[1]) * k; }
        const int say_now = oot_legs_logging() && frame % 36 == 0;   /* the diagnostic below: twice a second */
        if (say_now) MLOG("[vr-legs] F%llu his Waist at %.3f %.3f %.3f (the game's model block %.1f %.1f %.1f), moved %.4f %.4f in %.3f s, travel %.2f %.2f a second; "
                          "the game's hips face %.0f degrees", (unsigned long long)frame, (double)at[0], (double)at[1], (double)at[2], (double)off[0], (double)off[1], (double)off[2],
                          (double)d[0], (double)d[1], (double)dt, (double)l->vel[0], (double)l->vel[1], (double)(atan2f(game_waist[2], game_waist[10]) * 57.2958f));
        const float h = hypotf(fw[0], fw[2]);
        if (h > 0.2f) {
            const float want = atan2f(fw[0], fw[2]);
            l->yaw = l->have_yaw ? oot_gait__wrap(l->yaw + oot_gait__wrap(want - l->yaw) * fminf(1, dt * 6)) : want; l->have_yaw = 1;
        }
        if (!l->have_yaw) return;
        const float c = cosf(l->yaw), s = sinf(l->yaw);
        /* the hips' frame: +z ahead (sin, 0, cos), +x his left (cos, 0, -sin) */
        const float vz = l->vel[0] * s + l->vel[1] * c, vx = l->vel[0] * c - l->vel[1] * s;
        oot_gait_pick pick; oot_gait_choose(&l->g, vx, vz, &pick);
        l->phase = oot_gait_advance(l->phase, hypotf(l->vel[0], l->vel[1]) * dt * (pick.moving > 0), pick.stride);
        oot_gait_pose(&l->g, &pick, l->phase, l->pose, l->scratch);
        oot_gait_world(&l->g, l->pose, l->root, &l->world[0][0], &l->frame[0][0]);
        /* placed: turned to the hips' yaw about the up axis, its Waist where the game's Waist is */
        const float *ow = l->world[l->waist];
        float place[12] = { c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0 };
        place[3] = gw[0] - (c * ow[3] + s * ow[11]); place[7] = gw[1] - ow[7]; place[11] = gw[2] - (-s * ow[3] + c * ow[11]);
        int finite = 1;
        for (unsigned k = 0; k < l->g.bones && finite; k++) {
            if (l->slot[k] < 0) continue;
            float m[12]; oot_m_mul(l->world[k], l->inv[k], m); oot_m_mul(place, m, l->mats[k]);
            finite = oot_wear_finite(l->mats[k]);
        }
        if (!finite) { static int said; if (!said++) MLOG("[vr-legs] a leg matrix is not finite: the legs keep the game's pose"); return; }
        l->made = 1;
        if (say_now) {   /* where the legs went: the Waist and the last posed bone (a toe), relative to the game's Waist */
            float foot[3]; const float *t = l->world[l->g.bones - 1]; oot_m_apply(place, (float[3]){ t[3], t[7], t[11] }, foot);
            MLOG("[vr-legs] F%llu the legs are made: hips %.0f degrees, moving %.2f, %s; the game's Waist %.2f %.2f %.2f, %s at %.2f %.2f %.2f (palette space)",
                 (unsigned long long)frame, (double)(l->yaw * 57.2958f), (double)pick.moving, pick.clip[0] >= 0 ? l->g.clip[pick.clip[0]].name : "standing",
                 (double)gw[0], (double)gw[1], (double)gw[2], l->g.names[l->g.bones - 1], (double)foot[0], (double)foot[1], (double)foot[2]);
        }
        static int said;
        if (!said++) MLOG("[vr-legs] first person: %s Link's legs step with the hips facing %.0f degrees, the Waist at %.2f %.2f %.2f", ri ? "adult" : "child",
                          (double)(l->yaw * 57.2958f), (double)gw[0], (double)gw[1], (double)gw[2]);
        if (oot_legs_logging() && (pick.clip[0] != l->clips_said[0] || pick.clip[1] != l->clips_said[1])) {
            l->clips_said[0] = pick.clip[0]; l->clips_said[1] = pick.clip[1];
            MLOG("[vr-legs] F%llu travel %.2f m/s ahead %.2f left %.2f: %s %.2f + %s %.2f, moving %.2f, phase %.2f", (unsigned long long)frame,
                 (double)hypotf(vx, vz), (double)vz, (double)vx, pick.clip[0] >= 0 ? l->g.clip[pick.clip[0]].name : "-", (double)pick.weight[0],
                 pick.clip[1] >= 0 ? l->g.clip[pick.clip[1]].name : "-", (double)pick.weight[1], (double)pick.moving, (double)l->phase);
        }
    }
    if (!l->made) return;
    for (unsigned k = 0; k < l->g.bones; k++) {
        const int slot = l->slot[k];
        if (slot < 0 || (uint64_t)(slot + 1) * 48 > size || oot_wear_collapsed(src + (size_t)slot * 48)) continue;
        float m[12]; memcpy(m, l->mats[k], 48);
        if (crawl > 0) { float game[12]; memcpy(game, src + (size_t)slot * 48, 48); oot_matrix_lerp(m, game, crawl, m); }
        memcpy(to + (size_t)slot * 48, m, 48);
    }
}
