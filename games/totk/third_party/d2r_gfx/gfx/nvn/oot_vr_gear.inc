/* zeldaVR, Ocarina of Time, first person: the sword's forward tilt in the player's hand and the shield's three poses (vr.md rows 42,
   52; the rules in zeldaVR/runtime/oot_vr_gear.h, unit-tested). The weapon's grip is turned about its x, the controller's side axis,
   before the weapon is moved to it, so the weapon's actor (and the attack shape that follows it) leans with what is drawn. The shield
   is the model Link holds in his right hand with a palette of two matrices (HookshotVR: Skl_Root and ArmStrap) while the game has his
   shield out (games/oot/runtime/oot_vr_guard.h runtime_oot_vr_shield_out); it is turned about the grip's z through its own origin.
   A shield model of another size, or none recognised, stays along the forearm (logged once). Included by oot_vr_props.inc. */
#include "../../../../zeldaVR/runtime/oot_vr_gear.h"
int vr_oot_sword_tilt(void);    /* zeldaVR/gfx/oot_vr_gear_options.h */
int vr_oot_shield_pose(void);
int vr_oot_zl_held(void);
extern int runtime_oot_vr_shield_out(void) __attribute__((weak_import));
enum { OOT_SHIELD_PALETTE = 2 * 48 };

/* The player's grip for the weapon, turned by the VR page's tilt. */
static const float *oot_gear_weapon_grip(const float grip[12], float out[12]) {
    const int deg = vr_oot_sword_tilt();
    static int said = 0;
    if (deg != said) { said = deg; MLOG("[vr-gear] the weapon leans %d degrees forward in the hand (about the controller's side axis)", deg); }
    oot_gear_tilt(grip, (float)deg, out);
    return out;
}
/* A model Link holds, moved by `move` from the game's hand to the player's: turned when it is the shield and the pose asks. */
static void oot_gear_shield(uint64_t size, int from, int to, int item, const float root[3], float move[12]) {
    static oot_shield_turn turn; static int known = -1, at_end = -1, lines;
    const int out = from == 1 && !item && runtime_oot_vr_shield_out && runtime_oot_vr_shield_out();
    if (!out) return;
    const int shield = size == OOT_SHIELD_PALETTE;
    if (shield != known && lines < 20) {
        known = shield; lines++;
        if (shield) MLOG("[vr-gear] the shield is the model of %llu matrices in Link's right hand: its pose follows the VR page", (unsigned long long)(size / 48));
        else MLOG("[vr-gear] with the shield out, Link's right hand holds a model of %llu matrices: not taken as the shield", (unsigned long long)(size / 48));
    }
    if (!shield) return;
    const int mode = vr_oot_shield_pose(), guarding = vr_oot_zl_held();
    const float amount = oot_shield_turn_step(&turn, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), mode, guarding);
    const int end = amount >= 1 ? 1 : amount <= 0 ? 0 : -1;
    if (end >= 0 && end != at_end && lines < 200) {
        at_end = end; lines++;
        MLOG("[vr-gear] the shield is %s (pose %d, ZL %d, the player's %s hand)", end ? "turned to face where the fist points" : "along the forearm", mode, guarding, to ? "right" : "left");
    }
    if (amount > 0) oot_shield_turn_move(g_oot_track[to].grip_new, root, oot_shield_turn_angle(to == 1, amount), move);
}
