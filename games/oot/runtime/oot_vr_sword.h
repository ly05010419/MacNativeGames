/* The sword Link holds, as the game's own physics sees it. Its attack shape is a capsule from (0,0,0) to (0,0,0.8), radius 0.15,
   and its sensor tracks the sword actor (Phive/RigidBodyControllerSensorParam/WeaponAtk: IsTrackingActor, no bone). main+0x136ae0
   gives an actor its new place: at main+0x137478 x22 is the actor and x21 the matrix (rows R|t), before the velocity is worked out
   from the old place and the matrix is stored at +0x31c / +0x328. While the weapon is out in first person, the matrix there becomes
   the one the player's hand gives it (oot_vr_weapon.inc), so its hits, their speed and its trail go with the hand.
   TOTK_VR_SWORDLOG=1 logs both. Included by oot_vr.h. */
#pragma once
#include "oot_vr_sword_scan.h"
#include "../zeldaVR/runtime/oot_vr_blade.h"

enum { OOT_SWORD_LOG_LINES = 6000, OOT_EQUIP_OF_COMPONENTS = 0x200, OOT_EQUIP_SLOTS = 0x20, OOT_EQUIP_SLOT_SIZE = 0x18,
       OOT_EQUIP_STATES = 0x13bc, OOT_EQUIP_TO_HAND = 2, OOT_EQUIP_AWAY = 1 | 4, OOT_ACTOR_POS = 0x31c, OOT_ACTOR_ROT = 0x328 };
static const uint64_t OOT_WEAPON_FRESH_NS = 150000000ull;   /* an older hand is no hand: the game's own pose comes back */

static struct {
    pthread_mutex_t mu;
    uintptr_t actor;
    int in_hand, stick;   /* stick: the weapon is the Deku Stick (equipment slot 1), not the sword (slot 0) */
    float game[12], target[12];
    uint64_t game_at, target_at;
} oot_weapon = { .mu = PTHREAD_MUTEX_INITIALIZER };
static _Atomic uintptr_t oot_weapon_actor;
/* The blade's own contact and a hit in the air (oot_vr_touch.h, vr.md rows 44 and 47). */
static void oot_touch_place(const float m[12]);
static int oot_touch_hold(float final[12]);
static int oot_touch_live(unsigned number, int stroke_live);
static uint32_t oot_touch_kind(uintptr_t weapon, int stick, uint32_t own, unsigned number);

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

/* An actor's name begins with this (its SafeString at +0x260). */
static int oot_actor_named(uintptr_t actor, const char *prefix) {
    uint64_t text = 0; char name[32] = { 0 }; const size_t n = strlen(prefix);
    return actor && n < sizeof name && oot_scan_read(actor + 0x260, &text, 8) && text >= 0x100000000ull && oot_scan_read((uintptr_t)text, name, n) && !memcmp(name, prefix, n);
}
/* The game's own "is it drawn" (its query main+0x29f26d0 -> main+0x4a8a60, HookshotVR's notes): the actor's Equipment component
   (components +0x1d8) enabled (+0x98 bit 0) and in state 1 (+0x9c). */
static int oot_equip_drawn(uintptr_t actor) {
    uint64_t parts = 0, equipment = 0; uint8_t enabled = 0; uint32_t state = 0;
    return actor && oot_scan_read(actor + 0x270, &parts, 8) && parts && oot_scan_read((uintptr_t)parts + 0x1d8, &equipment, 8) && equipment &&
           oot_scan_read((uintptr_t)equipment + 0x98, &enabled, 1) && (enabled & 1) && oot_scan_read((uintptr_t)equipment + 0x9c, &state, 4) && state == 1;
}
/* From Link's update: which actor is his weapon, and whether the game last sent it to his hand. The state byte is a request of a
   frame or two: 2 into the hand, 1 or 4 away. The Deku Stick (vr.md row 68) is taken out into slot 1 as "Weapon_DekuStick", a long
   sword to the game (HookshotVR's notes): while the game says it is drawn it is the weapon, and goes with the hand as the sword does. */
static void oot_vr_weapon_note(uintptr_t components) {
    uint8_t state = 0, stick_state = 0;
    uintptr_t actor = oot_vr_equip_actor(components, 0, &state);
    const uintptr_t stick = oot_vr_equip_actor(components, 1, &stick_state);
    const int stick_out = stick && (oot_actor_named(stick, "Weapon_DekuStick") || oot_actor_named(stick, "Item_DekuStick")) && oot_equip_drawn(stick);
    if (stick_out) actor = stick;
    pthread_mutex_lock(&oot_weapon.mu);
    if (actor != oot_weapon.actor) {
        oot_weapon.actor = actor; oot_weapon.in_hand = 0; oot_weapon.game_at = oot_weapon.target_at = 0;
        atomic_store(&oot_weapon_actor, actor);
        if (stick_out) fprintf(stderr, "[vr-blade] %.3f the Deku Stick is out: it goes with the hand, its hits are kind %d\n", (double)oot_vr_now() / 1e9, OOT_ATTACK_STICK);
    }
    oot_weapon.stick = stick_out;
    if (stick_out) oot_weapon.in_hand = 1;
    else if (actor && (state & OOT_EQUIP_TO_HAND)) oot_weapon.in_hand = 1;
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
    oot_touch_place(target);
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

/* TOTK_VR_BLADELOG=1: the enemies near Link, from their own places: when one comes within 8 m and when it is there no more
   (it died, or left), so a run shows whether the blade's attack takes them. Read only. */
enum { OOT_ENEMIES = 16 };
static struct { pthread_mutex_t mu; struct { uintptr_t actor; char name[40]; uint64_t seen; float pos[3]; } e[OOT_ENEMIES]; uint64_t checked; } oot_enemies = { .mu = PTHREAD_MUTEX_INITIALIZER };
static int oot_blade_log_on(void);
static void oot_enemy_note(uintptr_t a, const uint8_t *target) {
    float m[12], feet[3]; uint64_t age = 0, text = 0; char name[40];
    memcpy(m, target, sizeof m);
    if (!runtime_oot_vr_feet(feet, &age) || age > 500000000ull) return;
    const float d[3] = { m[3] - feet[0], m[7] - feet[1], m[11] - feet[2] }, dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(dist < 60.0f)) return;
    const uint64_t now = oot_vr_now();
    pthread_mutex_lock(&oot_enemies.mu);
    int at = -1, free_at = -1;
    for (int i = 0; i < OOT_ENEMIES; i++) { if (oot_enemies.e[i].actor == a) at = i; else if (!oot_enemies.e[i].actor && free_at < 0) free_at = i; }
    if (at >= 0) { oot_enemies.e[at].seen = now; oot_enemies.e[at].pos[0] = m[3]; oot_enemies.e[at].pos[1] = m[7]; oot_enemies.e[at].pos[2] = m[11]; }
    pthread_mutex_unlock(&oot_enemies.mu);
    if (at >= 0 || free_at < 0) return;
    if (!oot_scan_read(a + 0x260, &text, 8) || text < 0x100000000ull || !oot_scan_read((uintptr_t)text, name, 7) || memcmp(name, "Enemy_", 6)) return;
    if (!oot_scan_read((uintptr_t)text, name, sizeof name - 1)) name[6] = 0;
    name[sizeof name - 1] = 0;
    for (char *c = name; *c; c++) if (*c < 32 || *c > 126) { *c = 0; break; }
    pthread_mutex_lock(&oot_enemies.mu);
    if (!oot_enemies.e[free_at].actor) {
        oot_enemies.e[free_at].actor = a; oot_enemies.e[free_at].seen = now; memcpy(oot_enemies.e[free_at].name, name, sizeof name);
        fprintf(stderr, "[vr-enemy] %.3f %s %#llx comes within %.1f m of Link (at %.1f %.1f %.1f, Link at %.1f %.1f %.1f)\n", (double)now / 1e9, name, (unsigned long long)a, (double)dist,
                (double)m[3], (double)m[7], (double)m[11], (double)feet[0], (double)feet[1], (double)feet[2]);
    }
    pthread_mutex_unlock(&oot_enemies.mu);
}
static void oot_enemy_check(void) {   /* Link's update */
    const uint64_t now = oot_vr_now();
    if (now - oot_enemies.checked < 250000000ull) return;
    oot_enemies.checked = now;
    float feet[3]; uint64_t age = 0; const int have = runtime_oot_vr_feet(feet, &age);
    pthread_mutex_lock(&oot_enemies.mu);
    for (int i = 0; i < OOT_ENEMIES; i++) {
        if (!oot_enemies.e[i].actor || now - oot_enemies.e[i].seen < 1000000000ull) continue;
        const float *p = oot_enemies.e[i].pos; const float d[3] = { p[0] - feet[0], p[1] - feet[1], p[2] - feet[2] };
        fprintf(stderr, "[vr-enemy] %.3f %s %#llx is there no more (last %.1f m from Link)\n", (double)now / 1e9, oot_enemies.e[i].name, (unsigned long long)oot_enemies.e[i].actor,
                have ? (double)sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) : -1.0);
        oot_enemies.e[i].actor = 0;
    }
    pthread_mutex_unlock(&oot_enemies.mu);
}
/* main+0x137478, every actor that takes a new place: only the weapon is touched. */
static void oot_vr_place(uint8_t *actor, uint8_t *target) {
    const uintptr_t a = (uintptr_t)actor;
    if (a && target && oot_blade_log_on() && a != atomic_load_explicit(&oot_weapon_actor, memory_order_relaxed)) oot_enemy_note(a, target);
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
    if (placed) { oot_touch_hold(final); memcpy(target, final, sizeof final); oot_vr_sword_scan(a, game, final); }
    oot_vr_cut_log(game, final);
    if (placed) oot_vr_window_diff(a, atomic_load(&oot_vr_cuts_written));
    oot_vr_sword_log(a, placed, game, final);
}

/* ---- The blade's own hits (2026-11-25; the user, 11-23: "只要 模型剑 碰到怪物 就算击中"). ---------------------------------
   While the player swings the sword he holds (oot_vr_blade.h, measured in the room by the graphics side), the game's own
   attack is set up on Link's hit sensor every frame, at the entry of that sensor's update (main+0x2ea3ef0, x0 the sensor):
   the update retires every attack that was not set up again since the last one, so setting it up there keeps it on and not
   setting it up ends it. The setup is the game's own function main+0x8d29c0(sensor, info), the one its attack node calls;
   the weapon's sensor follows the weapon's actor, which follows the hand (oot_vr_place above). Damage, guards, grass, pots and
   switches stay the game's. Link's sensor is his components' +0x3b8.
   Where these are from: HookshotVR's notes (vr/items/sword/blade_hits.inc, GPL-3: facts and ideas, no code), the sites checked
   against our image (vr研究.md 2026-11-25). TOTK_VR_BLADELOG=1 logs every setup the game itself makes on Link's sensor
   (main+0x8d29c0, right after its first load: x0 the sensor, x1 the info) and every sensor switched off (main+0x8d56f0). */
enum { OOT_LINK_SENSOR_OF_COMPONENTS = 0x3b8, OOT_BLADE_LOG_LINES = 400 };
static const uint64_t OOT_SENSOR_SETUP = 0x8d29c0, OOT_SENSOR_OFF = 0x8d56f0, OOT_SENSOR_CALC = 0x2ea3ef0;
static struct {
    pthread_mutex_t mu;
    oot_stroke stroke;
    unsigned told;              /* the swing whose first setup was logged */
    unsigned setups;            /* setups made for the current swing */
    uint8_t before;             /* the sensor's +0x20 before the swing's first setup */
} oot_blade = { .mu = PTHREAD_MUTEX_INITIALIZER };
static _Atomic uintptr_t oot_link_sensor;
static _Atomic uint64_t oot_blade_setups, oot_blade_swings;
static _Atomic int oot_blade_ours;      /* our own setup is running: the log marks it */
static _Atomic uint32_t oot_blade_kind = OOT_ATTACK_SLASH;
/* While a swing's attack is on, the weapon's own hit sensor is suspended and its weapon shape switched off, as the game does
   for its own slash (main+0x8d6340(sensor, 4), main+0x8d56f0(sensor, 0); resumed with main+0x2ea6230(sensor, 4)), so that
   the hit goes through Link's sensor: HookshotVR's default, the one its notes say hit enemies in the headset. Our own runs
   had no enemy within reach (three saves, 2026-11-25), only grass, which is cut either way. "flag vr_blade_suspend 0" (or
   TOTK_VR_BLADE_SUSPEND=0) leaves the weapon's own sensor on. */
static const uint64_t OOT_SENSOR_SUSPEND = 0x8d6340, OOT_SENSOR_RESUME = 0x2ea6230;
static _Atomic int oot_blade_suspend = -1;
static int oot_blade_suspends(void) {
    int v = atomic_load(&oot_blade_suspend);
    if (v < 0) { const char *e = getenv("TOTK_VR_BLADE_SUSPEND"); v = !(e && *e == '0'); atomic_store(&oot_blade_suspend, v); }
    return v;
}
static uintptr_t oot_blade_suspended, oot_blade_suspended_actor;   /* Link's update only */
void runtime_oot_vr_blade_suspend(int on) { atomic_store(&oot_blade_suspend, on != 0); fprintf(stderr, "[vr-blade] [flag] the weapon's own sensor is %s during a swing\n", on ? "suspended" : "left on"); }

static int oot_blade_log_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VR_BLADELOG"); on = e && *e == '1'; } return on; }
/* "flag vr_blade_kind <n>" (diagnostic): the attack kind the blade sets up. */
void runtime_oot_vr_blade_kind(int kind) { if (kind > 0) atomic_store(&oot_blade_kind, (uint32_t)kind); fprintf(stderr, "[vr-blade] [flag] attack kind %u\n", atomic_load(&oot_blade_kind)); }

/* The graphics side, once a game frame while the weapon is in the player's hand: a point of the blade and its direction in
   the room. speed (m/s) and arc (m): the player's settings. 1: a swing has just begun. */
int runtime_oot_vr_blade_sample(const float point[3], const float axis[3], float speed, float arc) {
    for (int i = 0; i < 3; i++) if (!isfinite(point[i]) || !isfinite(axis[i])) return 0;
    const uint64_t now = oot_vr_now();
    pthread_mutex_lock(&oot_blade.mu);
    const int r = oot_stroke_step(&oot_blade.stroke, point, axis, now, speed, arc);
    const unsigned number = oot_blade.stroke.number; const float peak = oot_blade.stroke.peak, path = oot_blade.stroke.path; const int thrust = oot_blade.stroke.thrust;
    const float now_speed = oot_blade.stroke.speed; const int pull = oot_blade.stroke.pull, active = oot_blade.stroke.active;
    pthread_mutex_unlock(&oot_blade.mu);
    {   /* TOTK_VR_BLADELOG=1: every reading of a moving blade */
        static _Atomic int lines;
        if (oot_blade_log_on() && (now_speed > 0.4f || r) && atomic_fetch_add(&lines, 1) < 3000)
            fprintf(stderr, "[vr-blade] %.3f sample at %.3f %.3f %.3f axis %.2f %.2f %.2f: %.2f m/s, path %.2f, peak %.1f, pull %d, active %d, result %d\n", (double)now / 1e9,
                    (double)point[0], (double)point[1], (double)point[2], (double)axis[0], (double)axis[1], (double)axis[2], (double)now_speed, (double)path, (double)peak, pull, active, r);
    }
    if (r > 0) {
        atomic_fetch_add(&oot_blade_swings, 1);
        if (number <= OOT_BLADE_LOG_LINES) fprintf(stderr, "[vr-blade] %.3f swing %u begins (%s): %.2f m at up to %.1f m/s\n", (double)now / 1e9, number, thrust ? "thrust" : "slash", (double)path, (double)peak);
    }
    return r > 0;
}

/* The VR side: the blade's place reached the game within the last half second (else a swing presses the button as before). */
int runtime_oot_vr_blade_fresh(void) {
    pthread_mutex_lock(&oot_blade.mu);
    const uint64_t at = oot_blade.stroke.have ? oot_blade.stroke.at : 0;
    pthread_mutex_unlock(&oot_blade.mu);
    return at && oot_vr_now() - at < 500000000ull && atomic_load_explicit(&oot_link_sensor, memory_order_relaxed) != 0;
}
/* main+0x2ea3ef0, the entry of a hit sensor's update: Link's, while a swing lasts, gets the attack set up again. */
static oot_guest_call *oot_blade_calc(oot_hook_regs *r) {
    static oot_guest_call call, more[2]; static uint8_t info[OOT_ATTACK_INFO_BYTES];
    const uintptr_t sensor = (uintptr_t)r->x[0];
    if (!sensor || sensor != atomic_load_explicit(&oot_link_sensor, memory_order_relaxed)) return NULL;
    const uint64_t now = oot_vr_now();
    pthread_mutex_lock(&oot_weapon.mu);
    const int in_hand = oot_weapon.actor && oot_weapon.in_hand && oot_weapon.target_at && now - oot_weapon.target_at < OOT_WEAPON_FRESH_NS;
    const uintptr_t weapon = oot_weapon.actor; const int weapon_alive = weapon && oot_weapon.game_at && now - oot_weapon.game_at < OOT_WEAPON_FRESH_NS;
    const int stick = oot_weapon.stick;
    pthread_mutex_unlock(&oot_weapon.mu);
    pthread_mutex_lock(&oot_blade.mu);
    const unsigned number = oot_blade.stroke.number;
    const int live = in_hand && oot_touch_live(number, oot_stroke_live(&oot_blade.stroke, now));
    /* +0x20 is set by a setup and moved to +0x21 by the sensor's own step before each update (HookshotVR's notes; blade6: +0x20
       reads 0 here in every frame of a swing whose attack the game took): at this entry +0x21 says what the last frame's setup left */
    uint8_t state = 0; memcpy(&state, (const void *)(sensor + 0x21), 1);
    int first = 0, report = 0; unsigned setups = 0; uint8_t before = 0;
    if (live) {
        if (oot_blade.told != number) { oot_blade.told = number; oot_blade.setups = 0; oot_blade.before = state; first = 1; }
        else if (oot_blade.setups == 1) { report = 1; before = oot_blade.before; }
        setups = ++oot_blade.setups;
    }
    pthread_mutex_unlock(&oot_blade.mu);
    (void)setups;
    if (!live) {   /* the swing is over: the weapon's own sensor back, if we suspended it and the weapon is still there */
        const uintptr_t was = oot_blade_suspended, was_actor = oot_blade_suspended_actor;
        oot_blade_suspended = oot_blade_suspended_actor = 0;
        if (!was || !weapon_alive || was_actor != weapon) return NULL;
        call = (oot_guest_call){ (uint64_t)(uintptr_t)(oot_vr_base + OOT_SENSOR_RESUME), (uint64_t)was, 4, 0, 0, 0, NULL, 0 };
        { static int lines; if (oot_blade_log_on() && lines++ < 60) fprintf(stderr, "[vr-blade] %.3f the weapon's own sensor %#llx resumed\n", (double)now / 1e9, (unsigned long long)was); }
        return &call;
    }
    const uint32_t kind = oot_touch_kind(weapon, stick, stick ? (uint32_t)OOT_ATTACK_STICK : atomic_load(&oot_blade_kind), number);   /* the Deku Stick's own kind (HookshotVR: 101); the jump slash's in the air */
    if (report && number <= OOT_BLADE_LOG_LINES)   /* one update later: what the first setup did to the sensor */
        fprintf(stderr, "[vr-blade] %.3f swing %u: the attack (kind %u) is %s (sensor +0x21 %#x -> %#x)\n", (double)now / 1e9, number, kind,
                state ? "set up on Link's sensor" : "not on Link's sensor", before, state);
    oot_attack_info(2, kind, 1.0f, 1, info);
    call = (oot_guest_call){ (uint64_t)(uintptr_t)(oot_vr_base + OOT_SENSOR_SETUP), (uint64_t)sensor, (uint64_t)(uintptr_t)info, 0, 0, 0, NULL, 0 };
    if (first && oot_blade_suspends() && weapon_alive && !oot_blade_suspended) {
        uint64_t parts = 0, own = 0;
        if (oot_scan_read(weapon + 0x270, &parts, 8) && parts && oot_scan_read((uintptr_t)parts + OOT_LINK_SENSOR_OF_COMPONENTS, &own, 8) && own && own != sensor) {
            more[0] = (oot_guest_call){ (uint64_t)(uintptr_t)(oot_vr_base + OOT_SENSOR_SUSPEND), own, 4, 0, 0, 0, &more[1], 0 };
            more[1] = (oot_guest_call){ (uint64_t)(uintptr_t)(oot_vr_base + OOT_SENSOR_OFF), own, 0, 0, 0, 0, NULL, 0 };
            call.next = &more[0];
            oot_blade_suspended = (uintptr_t)own; oot_blade_suspended_actor = weapon;
            { static int lines; if (oot_blade_log_on() && lines++ < 60) fprintf(stderr, "[vr-blade] %.3f the weapon's own sensor %#llx suspended for swing %u\n", (double)now / 1e9, (unsigned long long)own, number); }
        }
    }
    atomic_store(&oot_blade_ours, 1);
    atomic_fetch_add(&oot_blade_setups, 1);
    return &call;
}
/* main+0x8d29c0 after its first load (x0 the sensor, x1 the info), and main+0x8d56f0's entry (x0 the sensor, x1 the kind): the log. */
static oot_guest_call *oot_blade_setup_seen(oot_hook_regs *r) {
    const int ours = atomic_exchange(&oot_blade_ours, 0);
    static _Atomic int lines;
    if (!oot_blade_log_on() || (uintptr_t)r->x[0] != atomic_load_explicit(&oot_link_sensor, memory_order_relaxed) || !r->x[1]) return NULL;
    if (atomic_fetch_add(&lines, 1) >= OOT_BLADE_LOG_LINES) return NULL;
    uint32_t w[8]; uint8_t state; memcpy(w, (const void *)(uintptr_t)r->x[1], sizeof w); memcpy(&state, (const void *)(uintptr_t)(r->x[0] + 0x20), 1);
    float rate; memcpy(&rate, &w[3], 4);
    fprintf(stderr, "[vr-blade] %.3f %s setup on Link's sensor: direction %u kind %u attributes %#x rate %.2f sensors %#x%08x +0x18 %u +0x1c %#x (sensor +0x20 %#x, return %#llx)\n",
            (double)oot_vr_now() / 1e9, ours ? "our" : "the game's", w[0], w[1], w[2], (double)rate, w[5], w[4], w[6], w[7], state,
            (unsigned long long)(r->x[30] - (uint64_t)(uintptr_t)oot_vr_base));
    return NULL;
}
static oot_guest_call *oot_blade_off_seen(oot_hook_regs *r) {
    static _Atomic int lines;
    if (!oot_blade_log_on() || (uintptr_t)r->x[0] != atomic_load_explicit(&oot_link_sensor, memory_order_relaxed)) return NULL;
    if (atomic_fetch_add(&lines, 1) >= OOT_BLADE_LOG_LINES) return NULL;
    fprintf(stderr, "[vr-blade] %.3f a sensor of Link's switched off: kind %llu (return %#llx)\n", (double)oot_vr_now() / 1e9, (unsigned long long)r->x[1],
            (unsigned long long)(r->x[30] - (uint64_t)(uintptr_t)oot_vr_base));
    return NULL;
}
/* From Link's update: his hit sensor, and his action state when it changes (the log). */
static void oot_blade_link(uintptr_t components, uintptr_t player) {
    const uintptr_t sensor = oot_vr_load64(components + OOT_LINK_SENSOR_OF_COMPONENTS);
    if (sensor != atomic_exchange(&oot_link_sensor, sensor)) fprintf(stderr, "[vr-blade] Link's hit sensor %#llx\n", (unsigned long long)sensor);
    if (!oot_blade_log_on()) return;
    oot_enemy_check();
    static uint32_t last = ~0u; static int lines; uint32_t action; memcpy(&action, (const void *)(player + 0x1f60), 4);
    if (action != last && lines < OOT_BLADE_LOG_LINES) { lines++; fprintf(stderr, "[vr-blade] %.3f Link's action state %#x (was %#x)\n", (double)oot_vr_now() / 1e9, action, last); }
    last = action;
}

/* ---- The shot along the player's two hands (2026-11-25). --------------------------------------------------------------------
   main+0x2dff2d0 is the game's shoot function (x0 the shooter; its +0x18 is the actor that shoots). Inside it the launch is
   worked out by main+0x2e03560 or main+0x2e03630 (x19 the launch record: +0x193c where the shot starts, +0x1948 its velocity,
   +0x1954 the point it flies toward, three floats each); at main+0x2e035f0 and main+0x2e036d0 all three have just been
   written. While the shooter is Link and the player holds a bow or slingshot in two hands (the graphics side says where the
   hands are, oot_vr_props.inc), the launch is turned to the hands (oot_vr_bow.h oot_shot_steer). Other shooters, and Link
   without the two hands, are not touched. TOTK_VR_BLADELOG=1 logs each of Link's shots. */
#include "../zeldaVR/runtime/oot_vr_bow.h"
static struct { pthread_mutex_t mu; float handle[3], pull[3]; uint64_t at; } oot_two_hands = { .mu = PTHREAD_MUTEX_INITIALIZER };
static _Atomic uintptr_t oot_link_actor;
static _Thread_local uint64_t oot_shot_began;   /* when Link's shoot call began on this thread, 0 none */
static _Atomic uint64_t oot_shots_steered;
void runtime_oot_vr_two_hands(const float handle[3], const float pull[3]) {
    for (int i = 0; i < 3; i++) if (!isfinite(handle[i]) || !isfinite(pull[i])) return;
    pthread_mutex_lock(&oot_two_hands.mu);
    memcpy(oot_two_hands.handle, handle, 12); memcpy(oot_two_hands.pull, pull, 12); oot_two_hands.at = oot_vr_now();
    pthread_mutex_unlock(&oot_two_hands.mu);
}
static oot_guest_call *oot_shot_entry(oot_hook_regs *r) {
    uint64_t actor = 0;
    oot_shot_began = r->x[0] && oot_scan_read((uintptr_t)r->x[0] + 0x18, &actor, 8) && actor && actor == atomic_load_explicit(&oot_link_actor, memory_order_relaxed) ? oot_vr_now() : 0;
    return NULL;
}
static oot_guest_call *oot_shot_launch(oot_hook_regs *r) {
    const uint64_t now = oot_vr_now(), began = oot_shot_began;
    if (!began || now - began > 20000000ull || !r->x[19]) return NULL;
    float handle[3], pull[3], start[3], velocity[3], target[3];
    pthread_mutex_lock(&oot_two_hands.mu);
    const int fresh = oot_two_hands.at && now - oot_two_hands.at < 150000000ull;
    memcpy(handle, oot_two_hands.handle, 12); memcpy(pull, oot_two_hands.pull, 12);
    pthread_mutex_unlock(&oot_two_hands.mu);
    uint8_t *record = (uint8_t *)(uintptr_t)r->x[19];
    memcpy(start, record + 0x193c, 12); memcpy(velocity, record + 0x1948, 12); memcpy(target, record + 0x1954, 12);
    float s[3], v[3], t[3]; memcpy(s, start, 12); memcpy(v, velocity, 12); memcpy(t, target, 12);
    const int steered = fresh && oot_shot_steer(handle, pull, s, v, t);
    if (steered) { memcpy(record + 0x193c, s, 12); memcpy(record + 0x1948, v, 12); memcpy(record + 0x1954, t, 12); atomic_fetch_add(&oot_shots_steered, 1); }
    static _Atomic int lines;
    if ((oot_blade_log_on() || steered) && atomic_fetch_add(&lines, 1) < 200)
        fprintf(stderr, "[vr-shot] %.3f Link's shot %s: start %.2f %.2f %.2f velocity %.3f %.3f %.3f toward %.2f %.2f %.2f%s; hands %s, handle %.2f %.2f %.2f pull %.2f %.2f %.2f; now start %.2f %.2f %.2f velocity %.3f %.3f %.3f\n",
                (double)now / 1e9, steered ? "turned to the hands" : "left to the game", (double)start[0], (double)start[1], (double)start[2], (double)velocity[0], (double)velocity[1], (double)velocity[2],
                (double)target[0], (double)target[1], (double)target[2], "", fresh ? "fresh" : "not there", (double)handle[0], (double)handle[1], (double)handle[2], (double)pull[0], (double)pull[1], (double)pull[2],
                (double)s[0], (double)s[1], (double)s[2], (double)v[0], (double)v[1], (double)v[2]);
    return NULL;
}
