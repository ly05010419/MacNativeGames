/* zeldaVR, Ocarina of Time, first person: the arms follow the controllers. Each palette entry of an arm's bone becomes the
   matrix of the part it belongs to (zeldaVR/runtime/oot_vr_pose.h), built from the rest pose.
   Spaces: a palette entry takes a rest-pose point to the model block's space; world = that + the model block's translation
   (vertex uniform 1, +0x40); camera = view (vertex uniform 0) x world. The headset gives the hands relative to the head, which
   is the camera in first person; game units per metre come from Link's arm against the player's (option arm_cm).
   Included by oot_vr_body.inc. */
#include "../../../../zeldaVR/runtime/oot_vr_pose.h"
#include "../../../../zeldaVR/runtime/oot_vr_hold.h"
#include "../../../../zeldaVR/runtime/oot_vr_items.h"
#include "../../../../zeldaVR/runtime/oot_vr_stature.h"
float vr_oot_crawl_amount(void);   /* totk_vr.m: how far into a crawl the eyes are; the arms take the game's crawl pose as far */
#include "../../../../zeldaVR/runtime/oot_vr_handitem.h"
int vr_oot_arm_cm(void);        /* totk_vr.m: the player's arm, shoulder to wrist */
int vr_oot_hand_tilt(void);     /* totk_vr.m: degrees the hand is turned about the controller's side axis */
int vr_oot_holding(void);       /* totk_vr.m: something is in the hand */
int vr_oot_left_handed(void);   /* totk_vr.m: the sword in the player's left hand */
int vr_oot_aiming(void);        /* totk_vr.m: the game is aiming the slingshot or the bow */
void vr_oot_set_world_scale(float s);   /* totk_vr.m: game units per metre of the player */

enum { OOT_PART_NONE, OOT_PART_CLAVICLE, OOT_PART_UPPER, OOT_PART_FORE, OOT_PART_ASSIST_A, OOT_PART_ASSIST_B, OOT_PART_HAND, OOT_PART_FINGER };
typedef struct { uint8_t side, part; } oot_rig_part;
typedef struct { oot_rig_part *parts; oot_arm_rest rest[2]; float grip_rest[2][12], arm_length, chest_rest[3]; int wrist_matrix[2], chest_matrix, ok; } oot_rig_arms;
static oot_rig_arms g_oot_arms[2];

static int oot_arms_bone(const oot_bfres *m, const char *stem, int side, char finger, int joint) {
    char name[40];
    if (finger) snprintf(name, sizeof name, "Finger_%c_%d_%c", finger, joint, side ? 'R' : 'L');
    else snprintf(name, sizeof name, "%s_%c", stem, side ? 'R' : 'L');
    return oot_bfres_find_bone(m, name);
}
static void oot_arms_at(const float *world, int bone, float out[3]) {
    out[0] = world[bone * 12 + 3]; out[1] = world[bone * 12 + 7]; out[2] = world[bone * 12 + 11];
}
static const char *oot_arms_prepare(const oot_rig *r, oot_rig_arms *a) {
    const oot_bfres *m = &r->m; const unsigned nb = m->bone_count, nm = oot_bfres_matrix_count(m);
    float *world = malloc((size_t)nb * 48); int *tag = calloc(nb, sizeof(int));
    a->parts = calloc(nm, sizeof *a->parts);
    if (!world || !tag || !a->parts) { free(world); free(tag); return "out of memory"; }
    const char *e = oot_bfres_rest_world(m, world, nb);
    static const struct { const char *stem; int part; } named[] = {
        {"Clavicle", OOT_PART_CLAVICLE}, {"Arm_1", OOT_PART_UPPER}, {"Arm_2", OOT_PART_FORE},
        {"Ph_Wrist_Assist_A", OOT_PART_ASSIST_A}, {"Ph_Wrist_Assist_B", OOT_PART_ASSIST_B}, {"Wrist", OOT_PART_HAND} };
    for (int side = 0; side < 2 && !e; side++) {
        int joints[3] = {0};
        for (size_t k = 0; k < sizeof named / sizeof *named; k++) {
            const int b = oot_arms_bone(m, named[k].stem, side, 0, 0);
            if (b < 0) { e = "an arm bone is missing"; break; }
            tag[b] = 1 + side + 2 * named[k].part;
            if (named[k].part == OOT_PART_UPPER) joints[0] = b;
            if (named[k].part == OOT_PART_FORE) joints[1] = b;
            if (named[k].part == OOT_PART_HAND) joints[2] = b;
        }
        if (e) break;
        oot_arms_at(world, joints[0], a->rest[side].shoulder);
        oot_arms_at(world, joints[1], a->rest[side].elbow);
        oot_arms_at(world, joints[2], a->rest[side].wrist);
        for (int f = 0; f < OOT_FINGERS && !e; f++) for (int j = 0; j < OOT_FINGER_JOINTS; j++) {
            const int b = oot_arms_bone(m, NULL, side, (char)('A' + f), j + 1);
            if (b < 0) { e = "a finger bone is missing"; break; }
            tag[b] = 1 + side + 2 * (OOT_PART_FINGER + f * OOT_FINGER_JOINTS + j);
            oot_arms_at(world, b, a->rest[side].finger[f][j]);
        }
        if (!e && !oot_hold_grip_rest(&a->rest[side], a->grip_rest[side])) e = "a hand has no grip frame";
    }
    if (!e) {
        a->wrist_matrix[0] = a->wrist_matrix[1] = a->chest_matrix = -1;
        const int wl = oot_bfres_find_bone(m, "Wrist_L"), wr = oot_bfres_find_bone(m, "Wrist_R"), chest = oot_bfres_find_bone(m, "Spine_2");
        for (unsigned k = 0; k < nm && k < m->smooth_count; k++) {
            const int b = oot_bfres_matrix_bone(m, k);
            if (b == wl) a->wrist_matrix[0] = (int)k;
            if (b == wr) a->wrist_matrix[1] = (int)k;
            if (b == chest) a->chest_matrix = (int)k;
        }
        if (chest >= 0) oot_arms_at(world, chest, a->chest_rest);
        for (unsigned k = 0; k < nm && k < m->smooth_count && !e; k++) {   /* a bone takes the nearest named bone at or above it */
            int b = oot_bfres_matrix_bone(m, k);
            while (b >= 0 && !tag[b]) { oot_bfres_bone bone; e = oot_bfres_bone_at(m, (unsigned)b, &bone); if (e) break; b = bone.parent; }
            if (b >= 0 && tag[b]) { a->parts[k].side = (uint8_t)((tag[b] - 1) & 1); a->parts[k].part = (uint8_t)((tag[b] - 1) >> 1); }
        }
        const float *s = a->rest[1].shoulder, *el = a->rest[1].elbow, *w = a->rest[1].wrist;
        a->arm_length = sqrtf((el[0] - s[0]) * (el[0] - s[0]) + (el[1] - s[1]) * (el[1] - s[1]) + (el[2] - s[2]) * (el[2] - s[2])) +
                        sqrtf((w[0] - el[0]) * (w[0] - el[0]) + (w[1] - el[1]) * (w[1] - el[1]) + (w[2] - el[2]) * (w[2] - el[2]));
        a->ok = !e && a->arm_length > 0.01f;
    }
    free(world); free(tag);
    return e;
}
static void oot_q_rotate(const float q[4], const float v[3], float o[3]) {
    const float t[3] = { 2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0]) };
    const float r[3] = { v[0] + q[3] * t[0] + q[1] * t[2] - q[2] * t[1], v[1] + q[3] * t[1] + q[2] * t[0] - q[0] * t[2],
                         v[2] + q[3] * t[2] + q[0] * t[1] - q[1] * t[0] };
    memcpy(o, r, sizeof r);
}
/* Camera space to the palette's: a point, and a direction. */
static void oot_cam_to_palette(const float view[12], const float off[3], const float c[3], float out[3]) {
    const float x = c[0] - view[3], y = c[1] - view[7], z = c[2] - view[11];
    out[0] = view[0] * x + view[4] * y + view[8] * z - off[0];
    out[1] = view[1] * x + view[5] * y + view[9] * z - off[1];
    out[2] = view[2] * x + view[6] * y + view[10] * z - off[2];
}
/* This frame's camera block (world to camera), kept for the diagnostics that turn a game point back into the room (oot_vr_bow.inc). */
static float g_oot_view_now[12];
static uint64_t g_oot_view_frame = UINT64_MAX;
static void oot_cam_dir_to_palette(const float view[12], const float c[3], float out[3]) {
    const float r[3] = { view[0] * c[0] + view[4] * c[1] + view[8] * c[2], view[1] * c[0] + view[5] * c[1] + view[9] * c[2],
                         view[2] * c[0] + view[6] * c[1] + view[10] * c[2] };
    memcpy(out, r, sizeof r);
}
/* The headset's input for this game frame, read once. Link's depth pass and his colour pass are separate draws, and the colour
   pass keeps only the pixels whose depth equals the depth pass's: both must be posed from the same controllers.
   Diagnostics: "flag vr_arm_per_draw 1" reads them at every draw again, "flag vr_arm_jitter 1" moves each reading by 1 mm. */
typedef struct { uint64_t frame; int have, holding, main_hand; vr_hand hands[2]; vr_pose head; vr_controls pad; } oot_arm_input;
static _Atomic int g_oot_arm_per_draw, g_oot_arm_jitter;
void oot_arm_set_per_draw(int on) { atomic_store(&g_oot_arm_per_draw, on != 0); MLOG("[flag] vr_arm_per_draw = %d", on != 0); }
void oot_arm_set_jitter(int on) { atomic_store(&g_oot_arm_jitter, on != 0); MLOG("[flag] vr_arm_jitter = %d", on != 0); }
static const oot_arm_input *oot_arm_input_now(void) {
    static oot_arm_input in = { .frame = UINT64_MAX };
    static unsigned readings;
    const uint64_t frame = atomic_load(&g_cur_frame);
    if (in.frame == frame && !atomic_load_explicit(&g_oot_arm_per_draw, memory_order_relaxed)) return in.have ? &in : NULL;
    in.frame = frame;
    memset(&in.pad, 0, sizeof in.pad);
    vr_link_hands(in.hands);
    in.have = vr_link_pose(&in.head);
    (void)vr_link_controls(&in.pad);
    in.holding = vr_oot_holding();
    in.main_hand = vr_oot_left_handed() ? 0 : 1;
    if (atomic_load_explicit(&g_oot_arm_jitter, memory_order_relaxed)) {
        const float d = (++readings & 1) ? 0.001f : -0.001f;
        for (int side = 0; side < 2; side++) in.hands[side].p[2] += d;
    }
    return in.have ? &in : NULL;
}
/* One arm's target in the palette's space. */
static void oot_arm_target_for(int side, const vr_hand *hand, const vr_pose *head, const vr_controls *pad, const float view[12], const float off[3],
                               float scale, const float up_c[3], const float fw_c[3], const float rt_c[3], float tilt, int holding, oot_arm_target *t) {
    const float qi[4] = { -head->q[0], -head->q[1], -head->q[2], head->q[3] }, out = side ? 1.f : -1.f;
    float sh_c[3], pole_c[3], rel[3], grip_c[3], f_c[3], u_c[3], f_h[3], u_h[3];
    for (int i = 0; i < 3; i++) {
        sh_c[i] = scale * (out * 0.17f * rt_c[i] - 0.20f * up_c[i] - 0.07f * fw_c[i]);
        pole_c[i] = -up_c[i] + out * 0.55f * rt_c[i] - 0.4f * fw_c[i];
        rel[i] = hand->p[i] - head->p[i];
    }
    oot_q_rotate(qi, rel, grip_c);
    for (int i = 0; i < 3; i++) grip_c[i] *= scale;
    /* OpenXR's grip pose on a hand: -y along the open fingers, -z toward the thumb, +x out of the back of the right hand and the palm
       of the left. The tilt turns the hand about that x. */
    const float c = cosf(tilt), s = sinf(tilt), along[3] = { 0, -c, -s }, back[3] = { side ? 1.f : -1.f, 0, 0 };
    oot_q_rotate(hand->q, along, f_h); oot_q_rotate(hand->q, back, u_h);
    oot_q_rotate(qi, f_h, f_c); oot_q_rotate(qi, u_h, u_c);
    memset(t, 0, sizeof *t);
    oot_cam_to_palette(view, off, sh_c, t->shoulder); oot_cam_to_palette(view, off, grip_c, t->grip);
    oot_cam_dir_to_palette(view, f_c, t->forward); oot_cam_dir_to_palette(view, u_c, t->up); oot_cam_dir_to_palette(view, pole_c, t->pole);
    t->left = !side; t->trigger = side ? pad->rtrig : pad->ltrig; t->squeeze = side ? pad->rgrip : pad->lgrip; t->holding = holding;
}
static const float *oot_part_matrix(const oot_arm_matrices *o, unsigned part) {
    switch (part) {
        case OOT_PART_CLAVICLE: return o->clavicle;
        case OOT_PART_UPPER: return o->upper;
        case OOT_PART_FORE: return o->fore;
        case OOT_PART_ASSIST_A: return o->assist[0];
        case OOT_PART_ASSIST_B: return o->assist[1];
        case OOT_PART_HAND: return o->hand;
        default: { const unsigned g = part - OOT_PART_FINGER; return o->finger[g / OOT_FINGER_JOINTS][g % OOT_FINGER_JOINTS]; }
    }
}
/* size bytes of a ring the GPU reads this frame. */
static uint8_t *oot_palette_ring(uint64_t size, uint64_t *gpu) {
    enum { RING = 4 << 20 }; static __strong id<MTLBuffer> ring; static uint64_t ring_gpu; static size_t at;
    if (!ring) { ring = [g_dev newBufferWithLength:RING options:MTLResourceStorageModeShared]; res_add(ring); ring_gpu = ring.gpuAddress; }
    const size_t need = ((size_t)size + 255) & ~(size_t)255;
    if (at + need > RING) at = 0;
    uint8_t *to = (uint8_t *)ring.contents + at;
    *gpu = ring_gpu + at; at += need;
    return to;
}
/* The hands for the items held in them (games/oot/runtime/oot_vr_hand_items.h, vr.md rows 63-67), from the same controllers as the
   arms: each hand's grip, velocity, pointing line and palm in the game's world, its trigger and grip, where it is from the head in the
   room; the head's eye and forward in the world. Sent once a frame. */
extern void runtime_oot_vr_hands_world(const oot_ih_frame *f) __attribute__((weak_import));
static void oot_hand_for_items(int side, const oot_arm_target *t, const vr_hand *hand, const vr_pose *head, const vr_controls *pad,
                               const float qi[4], const float view[12], const float off[3], float scale, oot_ih_hand *o) {
    float v_c[3], pf[3]; const float ahead[3] = { 0, 0, -1 };
    memset(o, 0, sizeof *o);
    for (int i = 0; i < 3; i++) o->pos[i] = t->grip[i] + off[i];
    oot_q_rotate(qi, hand->v, v_c);
    for (int i = 0; i < 3; i++) v_c[i] *= scale;
    oot_cam_dir_to_palette(view, v_c, o->vel);
    oot_ih_pointing(side, t->forward, t->up, o->dir);
    for (int i = 0; i < 3; i++) o->palm[i] = -t->up[i];   /* the back of the hand's opposite */
    o->trigger = side ? pad->rtrig : pad->ltrig; o->grip = side ? pad->rgrip : pad->lgrip;
    oot_q_rotate(head->q, ahead, pf);
    const float n = sqrtf(pf[0] * pf[0] + pf[2] * pf[2]), d[3] = { hand->p[0] - head->p[0], hand->p[1] - head->p[1], hand->p[2] - head->p[2] };
    if (n > 1e-3f) { const float fx = pf[0] / n, fz = pf[2] / n; o->f = d[0] * fx + d[2] * fz; o->r = -d[0] * fz + d[2] * fx; o->u = d[1]; }
    o->have = oot_ih_unit(o->dir) && oot_ih_unit(o->palm) && oot_ih_hand_ok(o);
}
static void oot_hands_for_items_send(oot_ih_frame *f, const float view[12]) {
    static uint64_t sent = UINT64_MAX;
    const uint64_t frame = atomic_load(&g_cur_frame);
    if (!runtime_oot_vr_hands_world || sent == frame) return;
    sent = frame;
    const float zero[3] = { 0, 0, 0 }, ahead[3] = { 0, 0, -1 };
    oot_cam_to_palette(view, zero, zero, f->eye); oot_cam_dir_to_palette(view, ahead, f->fwd);   /* no offset: the world itself */
    f->have_head = oot_ih_finite(f->eye, 3) && oot_ih_unit(f->fwd);
    runtime_oot_vr_hands_world(f);
}
#include "oot_vr_whole.inc"
#include "oot_vr_props.inc"
#include "oot_vr_wear.inc"
#include "oot_vr_legs.inc"
/* The palette with the arms on the controllers, in a ring of ours; 0 leaves the game's. */
static uint64_t oot_body_palette(const uint8_t *src, uint64_t size) {
    if (vr_oot_body_mode() != 0) return 0;
    if (size != 13440 && size != 13488) return oot_wear_palette(src, size);   /* a tunic or gauntlets (oot_vr_wear.inc) */
    const oot_rig *r = oot_rig_for(size);
    if (!r) return 0;
    oot_rig_arms *arms = &g_oot_arms[r == &g_oot_rigs[0] ? 0 : 1];
    if (!arms->parts) {
        const char *e = oot_arms_prepare(r, arms);
        if (e) MLOG("[vr] Link's arms: %s; they stay as the game poses them", e);
        else MLOG("[vr] Link's arms: %.3f game units from shoulder to wrist", (double)arms->arm_length);
    }
    if (!arms->ok) return 0;
    g_oot_arm_length = arms->arm_length;
    const oot_arm_input *input = oot_arm_input_now();
    if (!input) return 0;
    const vr_hand *hands = input->hands; const vr_pose head = input->head; const vr_controls pad = input->pad;
    float view[12], model[19];
    if (S.ubo[0][0].size != 13568 || S.ubo[0][1].size < sizeof model) return 0;
    pend_materialize(S.ubo[0][0].addr, (uint8_t *)view, sizeof view, 0);
    pend_materialize(S.ubo[0][1].addr, (uint8_t *)model, sizeof model, 0);
    for (int i = 0; i < 12; i++) if (!isfinite(view[i])) return 0;
    memcpy(g_oot_view_now, view, sizeof view); g_oot_view_frame = input->frame;
    const float off[3] = { model[16], model[17], model[18] };
    if (!isfinite(off[0]) || !isfinite(off[1]) || !isfinite(off[2])) return 0;
    const float qi[4] = { -head.q[0], -head.q[1], -head.q[2], head.q[3] };
    const float scale = fmaxf(0.30f, fminf(1.50f, arms->arm_length / ((float)vr_oot_arm_cm() / 100.0f)));
    vr_oot_set_world_scale(scale);
    g_oot_room_scale = scale;
    const float world_up[3] = { 0, 1, 0 }, ahead[3] = { 0, 0, -1 };
    float up_c[3], fw_c[3], rt_c[3];
    oot_q_rotate(qi, world_up, up_c);
    { const float k = oot_ik_dot(ahead, up_c); for (int i = 0; i < 3; i++) fw_c[i] = ahead[i] - up_c[i] * k; }
    if (!oot_ik_norm(fw_c)) return 0;
    oot_ik_cross(fw_c, up_c, rt_c);
    oot_arm_matrices done[2]; int have[2] = { 0, 0 };
    const float tilt = (float)vr_oot_hand_tilt() * (float)M_PI / 180;
    oot_ih_frame for_items; memset(&for_items, 0, sizeof for_items);
    for (int side = 0; side < 2; side++) {
        if (!hands[side].have) continue;
        oot_arm_target t;
        oot_arm_target_for(side, &hands[side], &head, &pad, view, off, scale, up_c, fw_c, rt_c, tilt, side == input->main_hand && input->holding, &t);
        have[side] = oot_arm_matrices_solve(&arms->rest[side], &t, &done[side]);
        oot_hand_for_items(side, &t, &hands[side], &head, &pad, qi, view, off, scale, &for_items.hand[side]);
    }
    oot_hands_for_items_send(&for_items, view);
    if (!have[0] && !have[1]) return 0;
    const int ri = r == &g_oot_rigs[0] ? 0 : 1;
    oot_grip_fingers(ri, arms, src, size, done, have);   /* a hand that holds something: the game's own grip (oot_vr_wear.inc) */
    static uint64_t crawl_frame = UINT64_MAX; static float crawl_now;   /* read once a frame: his depth and colour draws must match */
    if (crawl_frame != input->frame) { crawl_frame = input->frame; crawl_now = oot_crawl_pose_weight(vr_oot_crawl_amount()); }
    const float crawl = crawl_now;   /* in a crawl space: the game's own crawl pose, on hands and knees */
    { static int crawled; if ((crawl > 0.5f) != crawled) { crawled = crawl > 0.5f; MLOG("[vr-crawl] first person: Link's arms %s", crawled ? "take the game's crawl pose" : "follow the controllers again"); } }
    uint64_t gpu = 0; uint8_t *to = oot_palette_ring(size, &gpu);
    memcpy(to, src, (size_t)size);
    for (unsigned k = 0; k < r->m.smooth_count && (uint64_t)(k + 1) * 48 <= size; k++) {
        const oot_rig_part p = arms->parts[k];
        if (p.part == OOT_PART_NONE || !have[p.side]) continue;
        if (oot_wear_collapsed(src + (size_t)k * 48)) continue;   /* a part the game hides (its default sleeves under another tunic): hidden */
        if (crawl > 0) { float m[12], game[12]; memcpy(game, src + (size_t)k * 48, 48); oot_matrix_lerp(oot_part_matrix(&done[p.side], p.part), game, crawl, m); memcpy(to + (size_t)k * 48, m, 48); }
        else memcpy(to + (size_t)k * 48, oot_part_matrix(&done[p.side], p.part), 48);
    }
    { float fw_p[3]; oot_cam_dir_to_palette(view, fw_c, fw_p); oot_legs_pose(ri, src, to, size, fw_p, off, crawl); }   /* the legs (oot_vr_legs.inc) */
    oot_arm_frame_note(ri, arms, src, size, done, have, off, crawl);
    { float chest[3] = { 0, 0, 0 };
      if (arms->chest_matrix >= 0) { float c[12]; memcpy(c, src + (size_t)arms->chest_matrix * 48, 48); oot_m_apply(c, arms->chest_rest, chest); for (int i = 0; i < 3; i++) chest[i] += off[i]; }
      for (int side = 0; side < 2; side++) {
          if (!have[side] || arms->wrist_matrix[side] < 0) continue;
          float game_hand[12]; memcpy(game_hand, src + (size_t)arms->wrist_matrix[side] * 48, 48);
          oot_track_hand(side, game_hand, done[side].hand, arms->rest[side].wrist, chest, off, arms->grip_rest[side]);
      } }
    static int said;
    if (!said) {
        said = 1;
        const float *w = done[have[1] ? 1 : 0].joints.wrist; float c[3];
        for (int i = 0; i < 3; i++) c[i] = view[4 * i] * (w[0] + off[0]) + view[4 * i + 1] * (w[1] + off[1]) + view[4 * i + 2] * (w[2] + off[2]) + view[4 * i + 3];
        MLOG("[vr] Link's arms follow the controllers: %.2f game units a metre, model offset %.2f %.2f %.2f, a wrist at camera %.2f %.2f %.2f",
             (double)scale, (double)off[0], (double)off[1], (double)off[2], (double)c[0], (double)c[1], (double)c[2]);
    }
    return gpu;
}
