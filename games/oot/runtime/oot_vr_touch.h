/* The blade's own contact (vr.md row 44) and a hit in the air (row 47), HookshotVR's way (held-items.md 1.6-1.8; GPL-3: ideas and
   numbers only). The rules are in zeldaVR/runtime/oot_vr_contact.h (unit-tested); this is where they meet the game:
   - Targets are noticed in every actor's own update (oot_vr.h oot_vr_player runs at main+0x2c21728 for every actor, x19 the actor,
     right before the branch main+0x2c2172c `tbnz w8, #1` that HookshotVR's per-actor callback precedes): the actor's place at
     +0x31c, its name from its SafeString at +0x260 (or +0x20 when that gives none, HookshotVR's notes). Only while the weapon is in
     the player's hand and the option is on.
   - Each new place of the weapon actor the graphics side hands over (runtime_oot_vr_weapon_place, the actor's matrix, rows R|t) is
     swept from the last one (if under 100 ms old) while a swing's attack is on. A contact locks that target, buzzes the hand
     (runtime_oot_vr_blade_touched), starts the swing's window again, and holds the weapon actor at the touching pose where the game
     places it (oot_vr_place) for 3 of Link's updates; then the swing's attack ends (oot_touch_live). A swing begun inside a locked
     target sets nothing up.
   - Link's vertical speed comes from his place at each of his hit-sensor updates (oot_link_calc); a one-handed sword's hit in the
     air is the jump slash's kind 22 (oot_touch_kind).
   Logs: "[vr-touch]" for a contact, a refused stroke and (TOTK_VR_BLADELOG=1) each target noticed and each near miss;
   "[vr-blade] ... in the air" for a jump slash. Included by oot_vr.h, after oot_vr_sword.h. */
#pragma once
#include "../zeldaVR/runtime/oot_vr_contact.h"
int vr_oot_blade_contact(void);   /* zeldaVR/gfx/oot_vr_gear_options.h */
int vr_oot_jump_hits(void);

static const oot_blade_shape OOT_TOUCH_BLADE = { 0.05f, 0.60f, 0.05f };   /* OOT_TOUCH_BLADE_FROM / _TO / _RADIUS */
enum { OOT_TOUCH_HOLD_UPDATES = 3, OOT_TOUCH_LOG_LINES = 400 };
static pthread_mutex_t oot_touch_mu = PTHREAD_MUTEX_INITIALIZER;
static oot_targets oot_touch_targets;
static struct { float last[12], pose[12]; uint64_t last_at, held_at; int have, holding; unsigned stroke, updates; float miss; char name[OOT_TARGET_NAME]; } oot_touch;
static _Atomic int oot_touch_on;                                  /* targets are being noticed */
static _Atomic uint32_t oot_touch_link_bits[3];
static _Atomic uint64_t oot_touch_link_at;
static _Atomic unsigned oot_touch_refused = ~0u, oot_touch_done = ~0u, oot_touch_holding = ~0u;   /* swing numbers */
static _Atomic int oot_touch_buzz, oot_touch_lines;
static oot_air oot_touch_air;                                     /* Link's update only */

int runtime_oot_vr_blade_touched(void) { return atomic_exchange(&oot_touch_buzz, 0); }

/* oot_vr_player, every actor's own update. */
static void oot_touch_seen(uintptr_t a) {
    if (!atomic_load_explicit(&oot_touch_on, memory_order_relaxed) || !a || a == atomic_load_explicit(&oot_link_actor, memory_order_relaxed)) return;
    const uint64_t now = oot_vr_now(), link_at = atomic_load_explicit(&oot_touch_link_at, memory_order_relaxed);
    if (!link_at || now - link_at > 200000000ull) return;
    float link[3], pos[3];
    for (int i = 0; i < 3; i++) link[i] = oot_vr_bits_float(atomic_load_explicit(&oot_touch_link_bits[i], memory_order_relaxed));
    memcpy(pos, (const void *)(a + 0x31c), sizeof pos);   /* the per-actor update reads this actor's +0x328 just after */
    if (!oot_target_near(link, pos)) return;
    pthread_mutex_lock(&oot_touch_mu);
    oot_target *t = oot_targets_find(&oot_touch_targets, a);
    if (t) { memcpy(t->pos, pos, sizeof pos); t->seen = now; }
    const int skip = t || oot_targets_known_not(&oot_touch_targets, a);
    pthread_mutex_unlock(&oot_touch_mu);
    if (skip) return;
    char name[OOT_TARGET_NAME] = "";
    for (int which = 0; which < 2 && !name[0]; which++) {   /* +0x260, then +0x20 */
        uint64_t text = 0;
        if (!oot_scan_read(a + (which ? 0x20 : 0x260), &text, 8) || text < 0x100000000ull) continue;
        for (int n = OOT_TARGET_NAME - 1; n >= 7 && !name[0]; n -= 8) if (oot_scan_read((uintptr_t)text, name, (size_t)n)) name[n] = 0; else name[0] = 0;
        for (char *c = name; *c; c++) if (*c < 32 || *c > 126) { name[0] = 0; break; }   /* a byte outside printable text: no name */
    }
    oot_target_shape shape;
    const int target = oot_target_shape_of(name, &shape);
    pthread_mutex_lock(&oot_touch_mu);
    if (!target) oot_targets_mark_not(&oot_touch_targets, a);
    else t = oot_targets_seen(&oot_touch_targets, a, pos, &shape, name, now);
    pthread_mutex_unlock(&oot_touch_mu);
    if (target && t && oot_blade_log_on() && atomic_fetch_add(&oot_touch_lines, 1) < OOT_TOUCH_LOG_LINES)
        fprintf(stderr, "[vr-touch] %.3f a target: %s %#llx at %.2f %.2f %.2f (a capsule %.2f..%.2f m up, radius %.2f)\n", (double)now / 1e9, name, (unsigned long long)a,
                (double)pos[0], (double)pos[1], (double)pos[2], (double)shape.low, (double)shape.high, (double)shape.radius);
}

/* Link's hit sensor update (oot_link_calc), first: his place and vertical speed, whether targets are wanted, the hold counted. */
static void oot_touch_calc(void) {
    const uintptr_t actor = atomic_load_explicit(&oot_link_actor, memory_order_relaxed);
    const uint64_t now = oot_vr_now();
    if (actor) {
        float p[3]; memcpy(p, (const void *)(actor + 0x31c), sizeof p);
        if (isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2])) {
            for (int i = 0; i < 3; i++) atomic_store_explicit(&oot_touch_link_bits[i], oot_vr_float_bits(p[i]), memory_order_relaxed);
            atomic_store_explicit(&oot_touch_link_at, now, memory_order_relaxed);
            oot_air_step(&oot_touch_air, p[1], now);
        }
    }
    pthread_mutex_lock(&oot_weapon.mu);
    const int in_hand = oot_weapon.actor && oot_weapon.in_hand && oot_weapon.target_at && now - oot_weapon.target_at < OOT_WEAPON_FRESH_NS;
    pthread_mutex_unlock(&oot_weapon.mu);
    const int on = in_hand && vr_oot_blade_contact();
    if (atomic_exchange(&oot_touch_on, on) != on && on) fprintf(stderr, "[vr-touch] %.3f the blade's own contact is on: targets within %.0f m are noticed\n", (double)now / 1e9, (double)OOT_TARGET_REACH);
    pthread_mutex_lock(&oot_touch_mu);
    if (oot_touch.holding && ++oot_touch.updates >= OOT_TOUCH_HOLD_UPDATES) {
        oot_touch.holding = 0; atomic_store(&oot_touch_done, oot_touch.stroke); atomic_store(&oot_touch_holding, ~0u);
    }
    pthread_mutex_unlock(&oot_touch_mu);
}

/* runtime_oot_vr_weapon_place (the graphics side, once a game frame): the sweep from the last place to this one. */
static void oot_touch_place(const float m[12]) {
    if (!atomic_load_explicit(&oot_touch_on, memory_order_relaxed)) { pthread_mutex_lock(&oot_touch_mu); oot_touch.have = 0; pthread_mutex_unlock(&oot_touch_mu); return; }
    const uint64_t now = oot_vr_now();
    pthread_mutex_lock(&oot_blade.mu);
    const unsigned number = oot_blade.stroke.number; const int live = oot_stroke_live(&oot_blade.stroke, now);
    pthread_mutex_unlock(&oot_blade.mu);
    int contact = 0, refused = 0, locked = 0, missed = 0; float along = 0, gap = 0, miss = 0; char name[OOT_TARGET_NAME] = "";
    uintptr_t who = 0; unsigned ended = 0;
    pthread_mutex_lock(&oot_touch_mu);
    if (live && number != oot_touch.stroke) {   /* a new stroke */
        if (oot_touch.stroke && oot_touch.miss < 0.6f && oot_touch.miss > 0) { missed = 1; miss = oot_touch.miss; ended = oot_touch.stroke; memcpy(name, oot_touch.name, sizeof name); }
        oot_touch.stroke = number; oot_touch.holding = 0; oot_touch.miss = INFINITY; oot_touch.name[0] = 0;
        refused = oot_targets_stroke_begins(&oot_touch_targets, m, &OOT_TOUCH_BLADE, now);
        if (refused) atomic_store(&oot_touch_refused, number);
    }
    const int sweeping = live && number == oot_touch.stroke && !refused && atomic_load(&oot_touch_refused) != number && atomic_load(&oot_touch_done) != number && !oot_touch.holding;
    if (oot_touch.have && now - oot_touch.last_at < OOT_TOUCH_FRESH_NS) {
        oot_contact c;
        const int i = oot_targets_sweep(&oot_touch_targets, oot_touch.last, m, &OOT_TOUCH_BLADE, now, sweeping, &c, &locked);
        if (i >= 0) {
            oot_target *t = &oot_touch_targets.t[i];
            t->locked = 1; t->cleared = 0;
            oot_touch_pose(oot_touch.last, m, c.t, oot_touch.pose);
            oot_touch.holding = 1; oot_touch.updates = 0; oot_touch.held_at = now;
            contact = 1; along = c.along; gap = c.gap; who = t->actor; memcpy(name, t->name, sizeof name);
        } else if (sweeping) {   /* the nearest a target came (the log of a miss) */
            for (int k = 0; k < OOT_TARGETS; k++) {
                const oot_target *t = &oot_touch_targets.t[k];
                if (!t->actor || now - t->seen > OOT_TARGET_STALE_NS || t->locked) continue;
                const float g = oot_blade_gap(m, &OOT_TOUCH_BLADE, t, NULL);
                if (g < oot_touch.miss) { oot_touch.miss = g; memcpy(oot_touch.name, t->name, sizeof oot_touch.name); }
            }
        }
    }
    memcpy(oot_touch.last, m, sizeof oot_touch.last); oot_touch.last_at = now; oot_touch.have = 1;
    pthread_mutex_unlock(&oot_touch_mu);
    if (contact) {
        atomic_store(&oot_touch_holding, number);
        atomic_store(&oot_touch_buzz, 1);
        pthread_mutex_lock(&oot_blade.mu);
        if (oot_blade.stroke.number == number) oot_blade.stroke.began_at = now;   /* the window starts again at the contact */
        pthread_mutex_unlock(&oot_blade.mu);
    }
    const int log = atomic_load(&oot_touch_lines) < OOT_TOUCH_LOG_LINES;
    if (contact && log) { atomic_fetch_add(&oot_touch_lines, 1); fprintf(stderr, "[vr-touch] %.3f swing %u touches %s %#llx %.2f m along the blade (%.2f m deep): the weapon held there for %d of Link's updates\n",
                                                                          (double)now / 1e9, number, name[0] ? name : "?", (unsigned long long)who, (double)along, (double)-gap, OOT_TOUCH_HOLD_UPDATES); }
    if (refused && log) { atomic_fetch_add(&oot_touch_lines, 1); fprintf(stderr, "[vr-touch] %.3f swing %u begins inside a target it hit before: no attack\n", (double)now / 1e9, number); }
    static unsigned told_locked;
    if (locked && sweeping && told_locked != number && log) { told_locked = number; atomic_fetch_add(&oot_touch_lines, 1); fprintf(stderr, "[vr-touch] %.3f swing %u touches a target it hit before: refused until the blade leaves it\n", (double)now / 1e9, number); }
    if (missed && oot_blade_log_on() && log) { atomic_fetch_add(&oot_touch_lines, 1); fprintf(stderr, "[vr-touch] %.3f swing %u missed %s by %.2f m\n", (double)now / 1e9, ended, name, (double)miss); }
}
/* oot_vr_place, the weapon's new place where the game sets it: the touching pose while the contact is held. */
static int oot_touch_hold(float final[12]) {
    pthread_mutex_lock(&oot_touch_mu);
    const int held = oot_touch.holding && oot_vr_now() - oot_touch.held_at < 250000000ull;
    if (held) memcpy(final, oot_touch.pose, sizeof oot_touch.pose);
    pthread_mutex_unlock(&oot_touch_mu);
    return held;
}
/* oot_blade_calc: is the swing's attack on (the stroke's own answer, the contact's hold and its end). */
static int oot_touch_live(unsigned number, int stroke_live) {
    if (!atomic_load_explicit(&oot_touch_on, memory_order_relaxed)) return stroke_live;
    if (atomic_load(&oot_touch_refused) == number || atomic_load(&oot_touch_done) == number) return 0;
    return stroke_live || atomic_load(&oot_touch_holding) == number;
}
/* oot_blade_calc, the kind of the attack, decided at a swing's first setup and kept for it: in the air a one-handed sword's is the
   jump slash's. Link's update only. */
static uint32_t oot_touch_kind(uintptr_t weapon, int stick, uint32_t own, unsigned number) {
    static uintptr_t known; static int one_handed, known_stick = -1; static unsigned decided = ~0u; static uint32_t decided_own, kind;
    if (weapon != known || stick != known_stick) {
        known = weapon; known_stick = stick;
        one_handed = !stick && (oot_actor_named(weapon, "Weapon_Sword_Kokiri") || oot_actor_named(weapon, "Weapon_Sword_Master"));
    }
    if (number == decided && own == decided_own) return kind;
    decided = number; decided_own = own;
    kind = oot_hit_kind(own, one_handed, oot_air_borne(&oot_touch_air), vr_oot_jump_hits());
    if (kind != own && number <= OOT_BLADE_LOG_LINES)
        fprintf(stderr, "[vr-blade] %.3f swing %u in the air (Link's vertical speed %.2f m/s): the jump slash's kind %u\n", (double)oot_vr_now() / 1e9, number, (double)oot_touch_air.vy, kind);
    return kind;
}
