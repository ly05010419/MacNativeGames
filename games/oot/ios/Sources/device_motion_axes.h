/* iPhone / iPad motion in the Switch's six-axis frame (runtime/motion_fusion.c: x right, y toward the top edge, z out
   of the face). Core Motion reports in the device's portrait frame; the game is played in landscape, so the phone held
   sideways is the controller. Pure C so tests can build it on the Mac (tests/test_device_motion.py). */
#ifndef DEVICE_MOTION_AXES_H
#define DEVICE_MOTION_AXES_H
enum { DEVICE_MOTION_UI_LANDSCAPE_RIGHT = 3, DEVICE_MOTION_UI_LANDSCAPE_LEFT = 4 };   /* UIInterfaceOrientation values */
/* LandscapeRight: the device's top edge is on the player's left; LandscapeLeft: on the right. A rotation, so acceleration
   and rotation rate map alike. */
static inline void device_motion_to_landscape(int ui_orientation, const double in[3], double out[3]) {
    const double x = in[0], y = in[1], z = in[2];
    if (ui_orientation == DEVICE_MOTION_UI_LANDSCAPE_LEFT) { out[0] = y; out[1] = -x; }
    else { out[0] = -y; out[1] = x; }
    out[2] = z;
}
#endif
