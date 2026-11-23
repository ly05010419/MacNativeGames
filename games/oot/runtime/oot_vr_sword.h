/* The sword Link holds, as the game's own physics sees it. Its attack shape is a capsule from (0,0,0) to (0,0,0.8), radius 0.15,
   and its sensor tracks the sword actor (Phive/RigidBodyControllerSensorParam/WeaponAtk: IsTrackingActor, no bone). main+0x136ae0
   gives an actor its new place: at main+0x137478 x22 is the actor and x21 the matrix (rows R|t), before the velocity is worked out
   from the old place and the matrix is stored at +0x31c / +0x328. While the weapon is out in first person, the matrix there becomes
   the one the player's hand gives it (oot_vr_weapon.inc), so its hits, their speed and its trail go with the hand.
   TOTK_VR_SWORDLOG=1 logs both. Included by oot_vr.h. */
#pragma once
#include "oot_vr_sword_scan.h"

enum { OOT_SWORD_LOG_LINES = 6000, OOT_EQUIP_OF_COMPONENTS = 0x200, OOT_EQUIP_SLOTS = 0x20, OOT_EQUIP_SLOT_SIZE = 0x18,
       OOT_EQUIP_STATES = 0x13bc, OOT_EQUIP_TO_HAND = 2, OOT_EQUIP_AWAY = 1 | 4, OOT_ACTOR_POS = 0x31c, OOT_ACTOR_ROT = 0x328 };
static const uint64_t OOT_WEAPON_FRESH_NS = 150000000ull;   /* an older hand is no hand: the game's own pose comes back */

static struct {
    pthread_mutex_t mu;
    uintptr_t actor;
    int in_hand;
    float game[12], target[12];
    uint64_t game_at, target_at;
} oot_weapon = { .mu = PTHREAD_MUTEX_INITIALIZER };
static _Atomic uintptr_t oot_weapon_actor;

/* The actor in one of the wearer's equipment slots, 0 when the slot is empty or its link is stale. */
static uintptr_t oot_vr_equip_actor(uintptr_t components, unsigned slot, uint8_t *state) {
    const uintptr_t user = oot_vr_load64(components + OOT_EQUIP_OF_COMPONENTS);
    if (!user) return 0;
    const uintptr_t link = user + OOT_EQUIP_SLOTS + slot * OOT_EQUIP_SLOT_SIZE, holder = oot_vr_load64(link + 8);
    uint32_t link_id, holder_id;
    memcpy(&link_id, (const void *)(link + 16), 4);
    if (!holder || link_id == ~0u) return 0;
    memcpy(&holder_id, (const void *)(holder + 0x48), 4);
    if (holder_id != link_id) return 0;
    memcpy(state, (const void *)(user + OOT_EQUIP_STATES + slot), 1);
    return oot_vr_load64(holder + 0x40);
}

/* From Link's update: which actor is his weapon, and whether the game last sent it to his hand. The state byte is a request of a
   frame or two: 2 into the hand, 1 or 4 away. */
static void oot_vr_weapon_note(uintptr_t components) {
    uint8_t state = 0;
    const uintptr_t actor = oot_vr_equip_actor(components, 0, &state);
    pthread_mutex_lock(&oot_weapon.mu);
    if (actor != oot_weapon.actor) {
        oot_weapon.actor = actor; oot_weapon.in_hand = 0; oot_weapon.game_at = oot_weapon.target_at = 0;
        atomic_store(&oot_weapon_actor, actor);
    }
    if (actor && (state & OOT_EQUIP_TO_HAND)) oot_weapon.in_hand = 1;
    else if (state & OOT_EQUIP_AWAY) oot_weapon.in_hand = 0;
    pthread_mutex_unlock(&oot_weapon.mu);
}

/* The graphics side: is the weapon out, and where the game alone would put it. */
int runtime_oot_vr_weapon(float game[12], uintptr_t *actor) {
    pthread_mutex_lock(&oot_weapon.mu);
    const int ready = oot_weapon.actor && oot_weapon.in_hand && oot_weapon.game_at && oot_vr_now() - oot_weapon.game_at < OOT_WEAPON_FRESH_NS;
    if (ready) memcpy(game, oot_weapon.game, sizeof oot_weapon.game);
    *actor = oot_weapon.actor;
    pthread_mutex_unlock(&oot_weapon.mu);
    return ready;
}
void runtime_oot_vr_weapon_place(const float target[12]) {
    for (int i = 0; i < 12; i++) if (!isfinite(target[i])) return;
    pthread_mutex_lock(&oot_weapon.mu);
    memcpy(oot_weapon.target, target, sizeof oot_weapon.target);
    oot_weapon.target_at = oot_vr_now();
    pthread_mutex_unlock(&oot_weapon.mu);
}

static void oot_vr_sword_log(uintptr_t actor, int placed, const float game[12], const float final[12]) {
    static int on = -1; static _Atomic int lines;
    if (on < 0) { const char *e = getenv("TOTK_VR_SWORDLOG"); on = e && *e == '1'; }
    if (!on || atomic_fetch_add(&lines, 1) >= OOT_SWORD_LOG_LINES) return;
    const uintptr_t name_at = oot_vr_load64(actor + 0x260);
    fprintf(stderr, "[vr-sword] %.3f %#llx %.32s placed %d game %.3f %.3f %.3f final %.3f %.3f %.3f tip %.3f %.3f %.3f\n", (double)oot_vr_now() / 1e9,
            (unsigned long long)actor, name_at ? (const char *)name_at : "?", placed, (double)game[3], (double)game[7], (double)game[11],
            (double)final[3], (double)final[7], (double)final[11], (double)(final[3] + final[2] * 0.8f), (double)(final[7] + final[6] * 0.8f), (double)(final[11] + final[10] * 0.8f));
}

/* main+0x137478, every actor that takes a new place: only the weapon is touched. */
static void oot_vr_place(uint8_t *actor, uint8_t *target) {
    const uintptr_t a = (uintptr_t)actor;
    if (!a || !target || a != atomic_load_explicit(&oot_weapon_actor, memory_order_relaxed)) return;
    float game[12], final[12];
    memcpy(game, target, sizeof game);
    for (int i = 0; i < 12; i++) if (!isfinite(game[i])) return;
    memcpy(final, game, sizeof final);
    const uint64_t now = oot_vr_now();
    pthread_mutex_lock(&oot_weapon.mu);
    int placed = 0;
    if (a == oot_weapon.actor) {
        memcpy(oot_weapon.game, game, sizeof game); oot_weapon.game_at = now;
        placed = oot_weapon.in_hand && oot_weapon.target_at && now - oot_weapon.target_at < OOT_WEAPON_FRESH_NS;
        if (placed) memcpy(final, oot_weapon.target, sizeof final);
    }
    pthread_mutex_unlock(&oot_weapon.mu);
    if (placed) { memcpy(target, final, sizeof final); oot_vr_sword_scan(a, game, final); }
    oot_vr_cut_log(game, final);
    if (placed) oot_vr_window_diff(a, atomic_load(&oot_vr_cuts_written));
    oot_vr_sword_log(a, placed, game, final);
}
