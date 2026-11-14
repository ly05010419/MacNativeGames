/* Production veneer words are assembled by verify_kf_angle_patch.py.
 * atan2f/acosf run on the host; no original game instructions are loaded. */
#include <math.h>
#include <float.h>
#include <stdio.h>

extern float kf_angle(const float q[4]);

int main(void) {
    const double axes[][3] = {{1,0,0}, {0,1,0}, {0,0,1}, {.36,-.48,.8}};
    unsigned checks = 0, lost = 0;
    for (unsigned axis = 0; axis < 4; axis++)
        for (int sign = -1; sign <= 1; sign += 2)
            for (unsigned step = 0; step <= 30000; step++) {
                double angle = step * .0001;
                float q[4] = {axes[axis][0] * sin(angle/2), axes[axis][1] * sin(angle/2),
                              axes[axis][2] * sin(angle/2), sign * cos(angle/2)};
                float got = kf_angle(q), old = 2 * acosf(fabsf(q[3]));
                if (!isfinite(got) || fabs(got - angle) > 5e-7) {
                    fprintf(stderr, "axis=%u sign=%d angle=%.9g got=%.9g\n", axis, sign, angle, got);
                    return 1;
                }
                /* acos amplifies the rounding of w by approximately 4/angle.
                 * Account for that input error, including near .01 radians. */
                double old_tolerance = 2 * FLT_EPSILON / fmax(.01, angle) + 5e-7;
                if (angle >= .01 && fabs(old - got) > old_tolerance) {
                    fprintf(stderr, "large angle differs: %.9g old=%.9g got=%.9g\n", angle, old, got);
                    return 1;
                }
                lost += step > 0 && old == 0;
                checks++;
            }
    if (!lost) { fputs("negative control did not lose small turns\n", stderr); return 1; }
    printf("{\"status\":\"pass\",\"checks\":%u,\"old_zero_small_angles\":%u}\n", checks, lost);
    return 0;
}
