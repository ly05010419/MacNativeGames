/* Ocarina of Time in VR: how the sword and the shield sit in the player's hands, and when Link's guard is an untargeted one, without
   the runtime (tests/test_oot_vr_gear.c runs it). vr.md rows 42, 52 and 53; HookshotVR's ideas and numbers (held-items.md 1.3, 3,
   its held_gear.inc read for the shield's three poses; GPL-3: ideas and facts only, no code).

   The player's grip frame (oot_vr_hold.h; built in oot_vr_arms.inc oot_arm_target_for and oot_vr_pose.h oot_arm_matrices_solve):
   from OpenXR's grip pose, the hand's forward is the controller's -y (turned about its x by the hand tilt), its back the controller's
   +x for the right hand and -x for the left, and the solve makes the thumb side forward x back (right hand) or back x forward (left).
   Worked through, the grip frame's columns are, for both hands: x = the controller's +x, y = the hand's forward (the open fingers),
   z = the controller's -z (toward the thumb, the fist's axis). So the controller's side axis is the grip frame's x, in both hands;
   the back of the hand, the outside of the forearm, is +x in the right hand and -x in the left.

   Row 42, the sword's forward tilt: the held weapon turned about the grip's x by `degrees`, the blade from the fist's axis (z) toward
   the fingers (y), as HookshotVR's "Sword angle" leans its blade from -z toward -y of the controller. Ours is relative to how the
   game holds the weapon (the game's grip is kept, oot_vr_weapon.inc), so 0 is the hold players have now.
   Row 52, the shield: 0 along the forearm as the game wears it; 1 turned a quarter turn about the grip's z (the thumb's side stays
   up) to face where the fist points while the game guards (ZL held); 2 always so. The turn is about the shield's own origin, its
   grip in the palm, and eases over 120 ms. The face starts on the outside of the forearm (oot_vr_hold.h: the half turn takes it
   from the outside of one forearm to the outside of the other), +x in the right hand and -x in the left, and a turn of +90 degrees
   (right) or -90 degrees (left) about z takes it to +y.
   Row 53, an untargeted guard: Link's action state 0x408 with the game's guard bit seen during that same state, and no lock-on. The
   guard bit flickers while 0x408 holds (HookshotVR's headset log), so once seen it is kept until the state changes. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "oot_vr_pose.h"

/* Row 42. out = grip turned about its own x: the blade along z leans toward y by `degrees`. */
static inline void oot_gear_tilt(const float grip[12], float degrees, float out[12]) {
    if (!isfinite(degrees) || degrees == 0) { memcpy(out, grip, 48); return; }
    const float x[3] = { 1, 0, 0 };
    float r[12]; oot_m_axis(x, -degrees * (float)M_PI / 180.0f, r);   /* z -> (0, sin, cos) */
    oot_m_mul(grip, r, out);
}

/* Row 52. */
enum { OOT_SHIELD_ARM = 0, OOT_SHIELD_GUARD = 1, OOT_SHIELD_FIST = 2 };
static const uint64_t OOT_SHIELD_TURN_NS = 120000000ull;
typedef struct { float amount; uint64_t at; } oot_shield_turn;
/* 0 on the arm .. 1 facing where the fist points, eased toward what the mode wants. */
static inline float oot_shield_turn_step(oot_shield_turn *s, uint64_t now, int mode, int guarding) {
    const float want = mode == OOT_SHIELD_FIST || (mode == OOT_SHIELD_GUARD && guarding) ? 1.0f : 0.0f;
    const float step = s->at && now >= s->at ? fminf(1.0f, (float)(now - s->at) / (float)OOT_SHIELD_TURN_NS) : 1.0f;
    s->at = now;
    const float d = want - s->amount;
    s->amount += d > step ? step : d < -step ? -step : d;
    return s->amount;
}
static inline float oot_shield_turn_angle(int right_hand, float amount) { return (right_hand ? 1.0f : -1.0f) * amount * (float)M_PI * 0.5f; }
/* The move placing the shield (from its root at `root` in the game's pose) turned about the grip's z through the shield's new origin. */
static inline int oot_shield_turn_move(const float grip[12], const float root[3], float angle, float move[12]) {
    float axis[3] = { grip[2], grip[6], grip[10] }, at[3], r[12], t[12], out[12];
    if (angle == 0 || !isfinite(angle) || !oot_ik_norm(axis)) return 0;
    oot_m_apply(move, root, at);
    oot_m_axis(axis, angle, r);
    oot_m_about(r, at, at, t);
    oot_m_mul(t, move, out);
    memcpy(move, out, sizeof out);
    return 1;
}

/* Row 53. Called once per Link update. Returns 1 while the guard is untargeted and confirmed. */
enum { OOT_ACTION_GUARD = 0x408 };
typedef struct { uint32_t action; int seen; } oot_guard_state;
static inline int oot_guard_untargeted(oot_guard_state *g, uint32_t action, int guard_bit, int locked_on) {
    if (action != g->action) { g->action = action; g->seen = 0; }
    if (action != OOT_ACTION_GUARD || locked_on) { g->seen = 0; return 0; }
    if (guard_bit) g->seen = 1;
    return g->seen;
}
/* The facing the guard is given: the way the stick walks Link, on the ground, unit length; 0 when it has no length. */
static inline int oot_guard_facing(float dx, float dz, float out[2]) {
    const float n = sqrtf(dx * dx + dz * dz);
    if (!(n > 1e-4f) || !isfinite(n)) return 0;
    out[0] = dx / n; out[1] = dz / n;
    return 1;
}
