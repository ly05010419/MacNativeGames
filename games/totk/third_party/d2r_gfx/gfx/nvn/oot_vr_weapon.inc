/* zeldaVR, Ocarina of Time: the weapon's actor goes with the player's hand, so its attack shape does (games/oot/runtime/oot_vr_sword.h).
   The model of an actor the game now places at the hand can no longer be told by its distance to his wrist, so it is known by its
   palette size and vertex buffers from the moment the game alone held it. Each frame its palette goes back to the pose the game
   would have given it and on to the player's hand as anything held does, and the actor's place for the next game frame is handed
   over. "flag vr_sword_follow 0" leaves the actor to the game. Included by oot_vr_props.inc. */
extern int runtime_oot_vr_weapon(float game[12], uintptr_t *actor) __attribute__((weak_import));
extern void runtime_oot_vr_weapon_place(const float target[12]) __attribute__((weak_import));
extern int runtime_oot_vr_blade_sample(const float point[3], const float axis[3], float speed, float arc) __attribute__((weak_import));
extern int runtime_oot_vr_blade_touched(void) __attribute__((weak_import));   /* games/oot/runtime/oot_vr_touch.h: the blade touched a target */
#include "../../../../zeldaVR/runtime/oot_vr_blade.h"
int vr_oot_blade(void);         /* totk_vr.m: the blade makes its own hits (the VR page) */
int vr_oot_swing_speed(void);   /* totk_vr.m: tenths of m/s the blade must reach for a swing */
int vr_oot_swing_arc(void);     /* totk_vr.m: cm it must go one way */
enum { OOT_WEAPON_SHAPES = 8 };
static const float OOT_WEAPON_ROOT_REACH = 0.35f;   /* a sword's model has its origin along the blade, 20 cm from the actor */
static struct { uintptr_t actor; uint64_t size, vb[OOT_WEAPON_SHAPES]; unsigned shapes; oot_weapon_fit fit; } g_oot_weapon;
static _Atomic int g_oot_sword_follow = 1;
void oot_sword_follow_set(int on) { atomic_store(&g_oot_sword_follow, on != 0); MLOG("[flag] vr_sword_follow %d", on != 0); }

/* Whether the weapon is out and to follow the hand, and where the game alone puts it. */
static int oot_weapon_state(float game[12]) {
    if (!runtime_oot_vr_weapon || !runtime_oot_vr_weapon_place) return 0;
    uintptr_t actor = 0;
    const int out = runtime_oot_vr_weapon(game, &actor);
    if (actor != g_oot_weapon.actor) { memset(&g_oot_weapon, 0, sizeof g_oot_weapon); g_oot_weapon.actor = actor; }
    return out && atomic_load(&g_oot_sword_follow) && vr_oot_blade();
}
static int oot_weapon_is(uint64_t size, uint64_t vb) {
    if (!g_oot_weapon.shapes || g_oot_weapon.size != size) return 0;
    for (unsigned i = 0; i < g_oot_weapon.shapes; i++) if (g_oot_weapon.vb[i] == vb) return 1;
    return 0;
}
/* A model the game holds rigidly in his weapon hand, near the weapon's actor: that is its model. */
static void oot_weapon_learn(uint64_t size, uint64_t vb, const float root[12], const float grip_game[12], const float actor[12]) {
    const float d[3] = { root[3] - actor[3], root[7] - actor[7], root[11] - actor[11] };
    if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] >= OOT_WEAPON_ROOT_REACH * OOT_WEAPON_ROOT_REACH) return;
    if (g_oot_weapon.shapes && g_oot_weapon.size != size) return;
    if (!g_oot_weapon.shapes) {
        oot_weapon_fit_learn(grip_game, root, actor, &g_oot_weapon.fit);
        g_oot_weapon.size = size;
        MLOG("[vr] the weapon's model is known (%llu matrices): the attack follows the player's hand", (unsigned long long)(size / 48));
    }
    if (g_oot_weapon.shapes < OOT_WEAPON_SHAPES) g_oot_weapon.vb[g_oot_weapon.shapes++] = vb;
}
/* undo: from the palette as drawn to the game's pose. move: on to the hand. publish: this frame's place goes to the game. */
static void oot_weapon_frame(const uint8_t *src, const float to_world[12], const float grip_game[12], const float grip_new[12], int how, int publish,
                             float undo[12], float move[12], float actor[12]) {
    float first[12], drawn[12], inverse[12], root_game[12];
    memcpy(first, src, sizeof first);
    oot_m_mul(to_world, first, drawn);
    oot_hold_inverse(drawn, inverse);
    oot_weapon_place(&g_oot_weapon.fit, grip_game, grip_new, how, root_game, move, actor);
    oot_m_mul(root_game, inverse, undo);
    if (publish) runtime_oot_vr_weapon_place(actor);
}
/* The blade in the room, for the swing (oot_vr_blade.h): the actor's place this frame, back through this frame's view to the
   camera and on to the room with the same head the hands came through (oot_arm_target_for), so that neither Link's running
   and turning nor the head's own movement is in it. The blade runs along the actor's z. Once a game frame. */
static float g_oot_room_scale;   /* game units per metre of the player (oot_body_palette) */
static void oot_blade_publish(const float actor[12], int hand) {
    if (runtime_oot_vr_blade_touched && runtime_oot_vr_blade_touched()) vr_link_send_haptic(hand, 50000000ull, 170, 0.7f);   /* a contact (HookshotVR: 0.7 for 50 ms) */
    if (!runtime_oot_vr_blade_sample || !(g_oot_room_scale > 0.05f) || S.ubo[0][0].size != 13568) return;
    /* once a game frame: the weapon's palette is made for more than one pass of a frame (blade3: two samples 11 ms apart with
       the same place, and the second one ended every swing) */
    static uint64_t sent = UINT64_MAX; const uint64_t frame = atomic_load(&g_cur_frame);
    if (frame == sent) return;
    sent = frame;
    const oot_arm_input *in = oot_arm_input_now();
    if (!in) return;
    {   /* and only with a new reading of that hand: the game can draw more frames than the headset sends poses (blade5: 90
           frames a second against 72 poses), and a frame that has the last frame's pose would read as a hand that stopped */
        static float seen[7]; float now_pose[7];
        memcpy(now_pose, in->hands[hand].p, 12); memcpy(now_pose + 3, in->hands[hand].q, 16);
        if (!memcmp(seen, now_pose, sizeof seen)) return;
        memcpy(seen, now_pose, sizeof seen);
    }
    float view[12]; pend_materialize(S.ubo[0][0].addr, (uint8_t *)view, sizeof view, 0);
    for (int i = 0; i < 12; i++) if (!isfinite(view[i])) return;
    float cam[3], dir[3], at[3], axis[3], point[3];
    for (int i = 0; i < 3; i++) {
        cam[i] = (view[4 * i] * actor[3] + view[4 * i + 1] * actor[7] + view[4 * i + 2] * actor[11] + view[4 * i + 3]) / g_oot_room_scale;
        dir[i] = view[4 * i] * actor[2] + view[4 * i + 1] * actor[6] + view[4 * i + 2] * actor[10];
    }
    if (!oot_ik_norm(dir)) return;
    oot_q_rotate(in->head.q, cam, at); oot_q_rotate(in->head.q, dir, axis);
    for (int i = 0; i < 3; i++) point[i] = in->head.p[i] + at[i] + axis[i] * OOT_STROKE_POINT;
    if (runtime_oot_vr_blade_sample(point, axis, (float)vr_oot_swing_speed() / 10.0f, (float)vr_oot_swing_arc() / 100.0f))
        vr_link_send_haptic(hand, 25000000ull, 170, 0.3f);   /* this swing counts */
}
