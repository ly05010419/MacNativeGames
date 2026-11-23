/* zeldaVR, Ocarina of Time: the weapon's actor goes with the player's hand, so its attack shape does (games/oot/runtime/oot_vr_sword.h).
   The model of an actor the game now places at the hand can no longer be told by its distance to his wrist, so it is known by its
   palette size and vertex buffers from the moment the game alone held it. Each frame its palette goes back to the pose the game
   would have given it and on to the player's hand as anything held does, and the actor's place for the next game frame is handed
   over. "flag vr_sword_follow 0" leaves the actor to the game. Included by oot_vr_props.inc. */
extern int runtime_oot_vr_weapon(float game[12], uintptr_t *actor) __attribute__((weak_import));
extern void runtime_oot_vr_weapon_place(const float target[12]) __attribute__((weak_import));
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
    return out && atomic_load(&g_oot_sword_follow);
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
                             float undo[12], float move[12]) {
    float first[12], drawn[12], inverse[12], root_game[12], actor[12];
    memcpy(first, src, sizeof first);
    oot_m_mul(to_world, first, drawn);
    oot_hold_inverse(drawn, inverse);
    oot_weapon_place(&g_oot_weapon.fit, grip_game, grip_new, how, root_game, move, actor);
    oot_m_mul(root_game, inverse, undo);
    if (publish) runtime_oot_vr_weapon_place(actor);
}
