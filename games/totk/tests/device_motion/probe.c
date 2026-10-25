// ios/Sources/device_motion_axes.h: the iPhone's own motion (its portrait frame) in the Switch's six-axis frame.
#include <math.h>
#include <stdio.h>
#include "device_motion_axes.h"

static int failures, checked;
static void check(int ok, const char *what) { checked++; if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); } }
static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

static int maps_to(int o, const double in[3], double x, double y, double z) {
    double out[3]; device_motion_to_landscape(o, in, out);
    return near(out[0], x, 1e-12) && near(out[1], y, 1e-12) && near(out[2], z, 1e-12);
}

int main(void) {
    const int lr = DEVICE_MOTION_UI_LANDSCAPE_RIGHT, ll = DEVICE_MOTION_UI_LANDSCAPE_LEFT;

    check(maps_to(lr, (double[3]){0, 0, -1}, 0, 0, -1),
          "landscape right, phone lying face up: still lying face up (z = -1 G)");
    check(maps_to(lr, (double[3]){0, -1, 0}, 1, 0, 0),
          "landscape right: the device's bottom edge down maps to the controller's right");

    check(maps_to(lr, (double[3]){1, 0, 0}, 0, 1, 0), "landscape right: device right maps to the controller's top edge");
    check(maps_to(ll, (double[3]){1, 0, 0}, 0, -1, 0), "landscape left: device right maps to the controller's bottom edge");
    check(maps_to(ll, (double[3]){0, 1, 0}, 1, 0, 0), "landscape left: the device's top edge maps to the controller's right");

    const double v[3] = {0.3, -0.7, 0.2};
    for (int i = 0; i < 2; i++) {
        int o = i ? ll : lr;
        double out[3]; device_motion_to_landscape(o, v, out);
        check(near(out[2], v[2], 1e-12), "the axis out of the screen passes through unchanged");
        check(near(sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2]), sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]), 1e-12),
              "a length is kept: it is a rotation, so acceleration and rotation rate map alike");
    }

    for (int i = 0; i < 2; i++) {
        int o = i ? ll : lr;
        double m[3][3];
        for (int c = 0; c < 3; c++) {
            double u[3] = {0, 0, 0}; u[c] = 1, device_motion_to_landscape(o, u, m[c]);   // the images of the unit axes are the columns
        }
        double det = m[0][0]*(m[1][1]*m[2][2] - m[1][2]*m[2][1]) - m[0][1]*(m[1][0]*m[2][2] - m[1][2]*m[2][0])
                   + m[0][2]*(m[1][0]*m[2][1] - m[1][1]*m[2][0]);
        check(near(det, 1, 1e-12), "determinant +1: a proper rotation, no mirror (both orientations)");
    }

    check(maps_to(1, (double[3]){0.3, -0.7, 0.2}, 0.7, 0.3, 0.2),
          "an orientation we do not handle (portrait) falls back to landscape right");

    printf("{\"status\":\"%s\",\"probe\":\"device-motion\",\"checked\":%d,\"failures\":%d}\n", failures ? "fail" : "pass", checked, failures);
    return failures != 0;
}
