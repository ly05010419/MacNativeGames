// Geometric proof of a full-coverage first draw (pure function, unit test tests/t_cover.c): whether this draw touches every pixel of the whole block of that attachment level.
// Conditions (all must hold): the scissor covers the whole block; the viewport is exactly (0, 0, W, H) (NDC [-1,1]^2 is the whole block); some non-culled triangle encloses the four corners in NDC (closed containment:
// pixel centers are all inside the square, so the four corners being in the closed triangle means the whole block is inside); every vertex has w = 1 and z in [0,1] (unchanged by perspective divide and depth clipping).
// Winding: in NDC (y up) counter-clockwise is front; odd-numbered triangles of a strip have reversed vertex order. Anything unrecognized does not hold.
#pragma once
#include <math.h>

enum { NVN_COVER_TRIANGLES = 4, NVN_COVER_TRIANGLE_STRIP = 5 };   // NVNdrawPrimitive
enum { NVN_COVER_CULL_NONE = 0, NVN_COVER_CULL_FRONT = 1, NVN_COVER_CULL_BACK = 2, NVN_COVER_CULL_BOTH = 3 };   // NVNface

static inline int nvn_cover(const float (*v)[4], int n, int prim, int cull, int front_ccw,
                            int has_vp, const float vp[4], int has_sc, const int sc[4], long W, long H) {
    if (W <= 0 || H <= 0 || n < 3) return 0;
    if (has_sc && !(sc[0] <= 0 && sc[1] <= 0 && (long)sc[0] + sc[2] >= W && (long)sc[1] + sc[3] >= H)) return 0;
    if (has_vp && !(vp[0] == 0 && vp[1] == 0 && vp[2] == (float)W && vp[3] == (float)H)) return 0;
    if (prim != NVN_COVER_TRIANGLES && prim != NVN_COVER_TRIANGLE_STRIP) return 0;
    if (cull == NVN_COVER_CULL_BOTH) return 0;
    for (int k = 0; k < n; k++)
        if (v[k][3] != 1.0f || !(v[k][2] >= 0.0f && v[k][2] <= 1.0f) || !isfinite(v[k][0]) || !isfinite(v[k][1])) return 0;
    static const double C[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    int ntri = prim == NVN_COVER_TRIANGLES ? n / 3 : n - 2;
    for (int k = 0; k < ntri; k++) {
        int i0 = prim == NVN_COVER_TRIANGLES ? 3 * k : k, i1 = i0 + 1, i2 = i0 + 2;
        if (prim == NVN_COVER_TRIANGLE_STRIP && (k & 1)) { int x = i1; i1 = i2; i2 = x; }
        double A[2] = {v[i0][0], v[i0][1]}, B[2] = {v[i1][0], v[i1][1]}, D[2] = {v[i2][0], v[i2][1]};
        double d = (B[0] - A[0]) * (D[1] - A[1]) - (B[1] - A[1]) * (D[0] - A[0]);
        if (d == 0) continue;
        int front = front_ccw ? d > 0 : d < 0;
        if ((cull == NVN_COVER_CULL_BACK && !front) || (cull == NVN_COVER_CULL_FRONT && front)) continue;
        int in = 1;
        for (int c = 0; c < 4 && in; c++) {
            const double *p = C[c];
            double e0 = (B[0] - A[0]) * (p[1] - A[1]) - (B[1] - A[1]) * (p[0] - A[0]);
            double e1 = (D[0] - B[0]) * (p[1] - B[1]) - (D[1] - B[1]) * (p[0] - B[0]);
            double e2 = (A[0] - D[0]) * (p[1] - D[1]) - (A[1] - D[1]) * (p[0] - D[0]);
            in = d > 0 ? (e0 >= 0 && e1 >= 0 && e2 >= 0) : (e0 <= 0 && e1 <= 0 && e2 <= 0);
        }
        if (in) return 1;
    }
    return 0;
}
