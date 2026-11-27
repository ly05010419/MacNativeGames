/* Ocarina of Time VR: the wrist calibration and the six wrist fine-tunes (vr.md row 31). Included by totk_vr.m (GAME_OOT), after
   the hand tilt option. The math is zeldaVR/runtime/oot_vr_wristcal.h (unit test tests/test_oot_vr_wristcal.c).

   vr_oot_hands is the one reader of the controllers for everything that uses a hand's orientation: Link's arms and what his hands
   hold (oot_vr_arms.inc, from there the held items' frame sent to the game side and the sword's place), the gestures' sword tip,
   the wrist panel, the boomerang's mark, the ocarina in the hand and the item wheel over the hand. Each controller's orientation is
   turned by its wrist's three angles and by the hand tilt (hand = controller * E(pitch, yaw, roll) * Rx(tilt)); positions,
   velocities and the controller's angular velocity are the controller's (the turn is fixed in the controller's own frame, so the
   angular velocity in the room is the same). All 0 and tilt 0: the controller's own orientation, as the hands were before.
   Only the debug marks at the controllers (option hand_marks) read the controllers as they are, which is their purpose.

   Options (the VR page, degrees, -180..180, 0 by default as HookshotVR's vr_wrist_{l,r}_{pitch,yaw,roll}): wrist_l_pitch,
   wrist_l_yaw, wrist_l_roll, wrist_r_pitch, wrist_r_yaw, wrist_r_roll. The calibration (the VR page's button, or Menu held + A in
   the game: oot_vr_pad.h) needs the headset and both controllers; the player holds both hands ahead, palms facing each other,
   thumbs up (HookshotVR's reference pose), and the six options are set so that both hands are that pose now. */
#include "../runtime/oot_vr_wristcal.h"
enum { OOT_WC_PITCH, OOT_WC_YAW, OOT_WC_ROLL };
static int oot_wc_clamp(int v) { return v < -180 ? -180 : v > 180 ? 180 : v; }
/* side 0 the left wrist, 1 the right; axis 0 pitch, 1 yaw, 2 roll */
int vr_oot_wrist_deg(int side, int axis) {
    int v = 0;
    switch ((side ? 3 : 0) + (axis < 0 ? 0 : axis > 2 ? 2 : axis)) {
        case 0: v = vr_option("wrist_l_pitch", 0); break;
        case 1: v = vr_option("wrist_l_yaw", 0); break;
        case 2: v = vr_option("wrist_l_roll", 0); break;
        case 3: v = vr_option("wrist_r_pitch", 0); break;
        case 4: v = vr_option("wrist_r_yaw", 0); break;
        default: v = vr_option("wrist_r_roll", 0); break;
    }
    return oot_wc_clamp(v);
}
static void oot_wc_store(int side, int axis, int deg) {
    deg = oot_wc_clamp(deg);
    switch ((side ? 3 : 0) + (axis < 0 ? 0 : axis > 2 ? 2 : axis)) {
        case 0: vr_option_set("wrist_l_pitch", deg); break;
        case 1: vr_option_set("wrist_l_yaw", deg); break;
        case 2: vr_option_set("wrist_l_roll", deg); break;
        case 3: vr_option_set("wrist_r_pitch", deg); break;
        case 4: vr_option_set("wrist_r_yaw", deg); break;
        default: vr_option_set("wrist_r_roll", deg); break;
    }
}
void vr_oot_set_wrist_deg(int side, int axis, int deg) {
    static const char *const names[3] = { "pitch", "yaw", "roll" };
    oot_wc_store(side, axis, deg);
    VLOG("[vr-wristcal] the %s wrist's %s: %d degrees", side ? "right" : "left", names[axis < 0 ? 0 : axis > 2 ? 2 : axis], vr_oot_wrist_deg(side, axis));
}
/* The controllers as the hands: see above. Called on the VR thread, the pad's thread and the graphics' (each reads the options). */
void vr_oot_hands(vr_hand out[2]) {
    vr_link_hands(out);
    const float tilt = (float)vr_oot_hand_tilt();
    for (int k = 0; k < 2; k++) {
        if (!out[k].have) continue;
        const float deg[3] = { (float)vr_oot_wrist_deg(k, OOT_WC_PITCH), (float)vr_oot_wrist_deg(k, OOT_WC_YAW), (float)vr_oot_wrist_deg(k, OOT_WC_ROLL) };
        float q[4];
        oot_wc_apply(out[k].q, deg, tilt, q);
        memcpy(out[k].q, q, sizeof q);
    }
}
/* Records the wrists: both controllers, as they are held now, become the reference hands (how: what asked, for the log). 1 when
   done; otherwise nothing changes and one line says why. */
int vr_oot_wrist_calibrate(const char *how) {
    vr_hand h[2]; vr_pose head;
    vr_link_hands(h);   /* the controllers as they are: the calibration replaces the angles, it does not add to them */
    const int have_head = vr_link_pose(&head);
    if (!have_head || !h[0].have || !h[1].have) {
        VLOG("[vr-wristcal] not calibrated (%s): it needs the headset and both controllers tracked (headset %d, left %d, right %d)", how, have_head, h[0].have, h[1].have);
        return 0;
    }
    const int tilt = vr_oot_hand_tilt();
    float deg[2][3];
    for (int k = 0; k < 2; k++)
        if (!oot_wc_solve(h[k].q, head.q, (float)tilt, deg[k])) {
            VLOG("[vr-wristcal] not calibrated (%s): the %s controller's or the headset's orientation is not usable, or the headset looks straight up or down (look ahead)", how, k ? "right" : "left");
            return 0;
        }
    int whole[2][3];
    for (int k = 0; k < 2; k++) for (int a = 0; a < 3; a++) { whole[k][a] = oot_wc_whole(deg[k][a]); oot_wc_store(k, a, whole[k][a]); }
    VLOG("[vr-wristcal] calibrated (%s): left pitch %d yaw %d roll %d, right pitch %d yaw %d roll %d degrees (hand tilt %d kept)", how,
         whole[0][0], whole[0][1], whole[0][2], whole[1][0], whole[1][1], whole[1][2], tilt);
    for (int k = 0; k < 2; k++) vr_link_send_haptic(k, 60000000ull, 160, 0.5f);   /* both hands: recorded */
    return 1;
}
