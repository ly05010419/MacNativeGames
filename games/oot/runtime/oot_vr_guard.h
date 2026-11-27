/* The shield (vr.md rows 52, 53), HookshotVR's way (held-items.md 3; GPL-3: ideas and addresses only).
   Row 53, an untargeted guard faces the way the stick walks Link (first person). The game's shared facing setter main+0x9352f0 (an
   entry, pacibsp; x0 the component, whose +0x18 is its actor) loads the facing vector from x1 (`ldp s3, s2, [x1]`, then s1 = x*x,
   s4 = y*y + x*x, and at main+0x93533c `ldr s1, [x1, #8]`, word 0xbd400821), adds z*z, takes the root, normalises and keeps it as a
   target at +0x1c0 of an object it reaches from the actor (main+0x935388..0x935414). Right after that load, for Link's combat
   component only (his components +0x358, whose +0x18 is his actor: main+0x944e50 is the game's own getter of +0x358) and only
   while his guard is an untargeted one, the vector in registers becomes the stick's way: s3 = x, s2 = 0, s1 = z, s4 = x*x. The
   caller's memory, the turn rate in s0, flags and general registers are not touched. That the vector is the way Link is to face (and
   not its opposite) is HookshotVR's (its yaw + pi is the view's forward); it is not checked in a run of ours.
   The guard is untargeted while, all at once: his action state (player object +0x1f60) is 0x408; the game's guard bit was seen in
   that same state (his components +0x248, a component of class main+0x4b4a258, its u16 at +0x34 bit 0x100: what the game's query
   main+0x3062c40 reads through main+0x2d0b170, after a cast main+0x2d0b1a0 we read as returning that same component, not
   checked); and no lock-on: the combat component's +0x1c38 bit 0 clear and its lock-on point +0x1c14 not a number (HookshotVR's
   headless findings; neither read is checked on our image, so either one saying "locked" leaves the game's own facing).
   The stick's way comes from the gameplay view (oot_vr.h oot_vr_camera, first person only), at most 200 ms old.
   Row 52's game side: whether Link's shield is out (equipment slot 4, a "Weapon_Shield_" actor the game has drawn), for the graphics
   side that turns it (oot_vr_gear.inc). Included by oot_vr.h, after oot_vr_sword.h. */
#pragma once
#include "../zeldaVR/runtime/oot_vr_gear.h"
int vr_oot_guard_body(void);   /* zeldaVR/gfx/oot_vr_gear_options.h */

static const uint64_t OOT_GUARD_CLASS = 0x4b4a258;
enum { OOT_GUARD_OF_COMPONENTS = 0x248, OOT_GUARD_FLAGS = 0x34, OOT_COMBAT_LOCKED = 0x1c38, OOT_COMBAT_LOCK_POINT = 0x1c14, OOT_SHIELD_SLOT = 4 };
static _Atomic uint32_t oot_guard_face_bits[2];
static _Atomic uint64_t oot_guard_face_at, oot_shield_out_at, oot_guard_turned;
static _Atomic uintptr_t oot_guard_combat;   /* Link's combat component while his guard is untargeted, 0 otherwise */
static oot_guard_state oot_guard_now;        /* Link's update only */

/* oot_vr_camera, first person: the way the left stick walks Link (x, z on the ground). */
static void oot_guard_face_note(float dx, float dz) {
    float f[2];
    if (!oot_guard_facing(dx, dz, f)) return;
    atomic_store_explicit(&oot_guard_face_bits[0], oot_vr_float_bits(f[0]), memory_order_relaxed);
    atomic_store_explicit(&oot_guard_face_bits[1], oot_vr_float_bits(f[1]), memory_order_relaxed);
    atomic_store_explicit(&oot_guard_face_at, oot_vr_now(), memory_order_release);
}
/* The graphics side: the game has Link's shield out these last frames. */
int runtime_oot_vr_shield_out(void) { const uint64_t at = atomic_load_explicit(&oot_shield_out_at, memory_order_relaxed); return at && oot_vr_now() - at < 200000000ull; }

/* Link's hit sensor update (oot_link_calc): the shield out, and whether his guard is untargeted. */
static void oot_guard_calc(void) {
    const uintptr_t actor = atomic_load_explicit(&oot_link_actor, memory_order_relaxed);
    uint64_t parts = 0;
    if (!actor || !oot_vr_base || !oot_scan_read(actor + 0x270, &parts, 8) || !parts) { atomic_store(&oot_guard_combat, 0); return; }
    const uint64_t now = oot_vr_now();
    {   /* the shield */
        uint8_t state = 0; const uintptr_t shield = oot_vr_equip_actor((uintptr_t)parts, OOT_SHIELD_SLOT, &state);
        static int said = -1;
        const int out = shield && oot_actor_named(shield, "Weapon_Shield_") && (oot_equip_drawn(shield) || (state & OOT_EQUIP_TO_HAND));
        if (out) atomic_store_explicit(&oot_shield_out_at, now, memory_order_relaxed);
        if (out != said) { said = out; fprintf(stderr, "[vr-guard] %.3f Link's shield is %s\n", (double)now / 1e9, out ? "out (slot 4)" : "not out"); }
    }
    uint64_t combat = 0, owner = 0, guard = 0, cls = 0; uint32_t locked_word = 0; uint16_t flags = 0; float lock_x = 0;
    const int read = oot_scan_read((uintptr_t)parts + OOT_PLAYER_OF_COMPONENTS, &combat, 8) && combat && oot_scan_read((uintptr_t)combat + 0x18, &owner, 8) && owner == actor &&
                     oot_scan_read((uintptr_t)combat + OOT_COMBAT_LOCKED, &locked_word, 4) && oot_scan_read((uintptr_t)combat + OOT_COMBAT_LOCK_POINT, &lock_x, 4) &&
                     oot_scan_read((uintptr_t)parts + OOT_GUARD_OF_COMPONENTS, &guard, 8) && guard && oot_scan_read((uintptr_t)guard, &cls, 8) &&
                     cls == (uint64_t)(uintptr_t)(oot_vr_base + OOT_GUARD_CLASS) && oot_scan_read((uintptr_t)guard + OOT_GUARD_FLAGS, &flags, 2);
    static int said_read = -1;
    if (read != said_read) { said_read = read; if (!read) fprintf(stderr, "[vr-guard] Link's combat or guard component does not read as expected: the game's own facing\n"); }
    const int locked = (locked_word & 1) || isfinite(lock_x);
    const int untargeted = read && vr_oot_guard_body() && atomic_load(&oot_vr_fp_on) &&
                           oot_guard_untargeted(&oot_guard_now, atomic_load(&oot_vr_action_now), (flags & 0x100) != 0, locked);
    const uintptr_t was = atomic_exchange(&oot_guard_combat, untargeted ? (uintptr_t)combat : 0);
    if (!was != !untargeted) fprintf(stderr, "[vr-guard] %.3f %s (action state %#x, guard bit %d, lock-on %d)\n", (double)now / 1e9,
                                     untargeted ? "an untargeted guard: it faces the way the stick walks" : "the guard is the game's own again",
                                     atomic_load(&oot_vr_action_now), (flags & 0x100) != 0, locked);
}
/* main+0x93533c, after `ldr s1, [x1, #8]` in the shared facing setter. */
static oot_guest_call *oot_guard_face(oot_hook_regs *r) {
    const uintptr_t combat = atomic_load_explicit(&oot_guard_combat, memory_order_relaxed);
    if (!combat || (uintptr_t)r->x[0] != combat) return NULL;
    const uint64_t at = atomic_load_explicit(&oot_guard_face_at, memory_order_acquire);
    if (!at || oot_vr_now() - at > 200000000ull) return NULL;
    const float x = oot_vr_bits_float(atomic_load_explicit(&oot_guard_face_bits[0], memory_order_relaxed));
    const float z = oot_vr_bits_float(atomic_load_explicit(&oot_guard_face_bits[1], memory_order_relaxed)), y = 0, xx = x * x;
    memcpy(r->q[3], &x, 4); memcpy(r->q[2], &y, 4); memcpy(r->q[1], &z, 4); memcpy(r->q[4], &xx, 4);
    const uint64_t n = atomic_fetch_add_explicit(&oot_guard_turned, 1, memory_order_relaxed) + 1;
    if (n <= 3 || !(n & (n - 1))) fprintf(stderr, "[vr-guard] %.3f the guard faces %.2f 0 %.2f (%llu facings set)\n", (double)oot_vr_now() / 1e9, (double)x, (double)z, (unsigned long long)n);
    return NULL;
}
