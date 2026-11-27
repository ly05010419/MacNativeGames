/* The window view of Ocarina of Time (vr.md row 3), without the runtime (tests/test_oot_vr_window.c runs it): the pictures with no head
   pose of their own (title, menus, loading, cutscenes on a screen, the big screen) in a frame fixed in the room instead of on the curved
   screen, the frame's shape chosen on the VR page. HookshotVR has such a view (vr/headset/window_view.inc); only its ideas and option
   ranges are taken (GPL-3, no code): width 20-300 cm (100), Keep 16:9 (on), height 10-200 cm (56), roundness 0-100 % (0: a rectangle,
   100: an ellipse, |x/a|^n + |y/b|^n = 1 with n = 2 / roundness), feather 0-100 mm (0), tilt -45..45 degrees (0, more leans its top
   away), distance 40-300 cm (90) ahead of the head where it was placed, its middle 12 cm under the eyes, a dark back panel (off).

   Two ways a picture fills the frame:
     through (a world picture whose field of view is known): the frame is a window onto the picture's world. The picture's camera axes
       are the frame's (x right, y up, z out of the frame toward the viewer); a frame point is shown with the picture's pixel along the
       ray from the eye through that point, the picture's tangents being the box L..R across, B..T up at 1 along -z. With the eye where
       it is now the world behind the frame stays put as the head moves (a scene far behind a real window: leaning shows more of the
       other side, each eye its own part); with the eye where the picture was drawn from (vr.md row 3's second step) it is HookshotVR's
       crop "from where that picture was drawn".
     fit (title, menus, loading: the whole canvas, nothing cut, as large as fits; the bars dark).
   Metres, OpenXR axes: y up, -z forward. */
#pragma once
#include <math.h>

enum { OOT_WIN_CURVED = 0, OOT_WIN_FRAME = 1 };
enum { OOT_WIN_FIT = 0, OOT_WIN_THROUGH = 1, OOT_WIN_BACK = 2, OOT_WIN_NONE = 3 };

/* What the VR page holds, in its own units. */
typedef struct { int width_cm, keep169, height_cm, round_pct, feather_mm, tilt_deg, dist_cm, back; } oot_win_opts;
/* The frame in metres and radians. */
typedef struct { float w, h, round, feather, tilt, dist, drop; int back; } oot_win_shape;
/* The frame placed: its middle, its unit axes (x right, y up, z out toward the viewer) and its half width and height. */
typedef struct { float c[3], x[3], y[3], z[3], hw, hh; } oot_win_frame;

static const float OOT_WIN_DROP_M = 0.12f;      /* its middle under the eyes where it was placed */
static const float OOT_WIN_NEAR_M = 0.02f;      /* an eye nearer to its plane than this (or behind it) sees no picture */
static const float OOT_WIN_FADE_M = 0.12f;      /* and over this much more before that, the picture fades out */

static inline int oot_win_clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline oot_win_opts oot_win_default_opts(void) { oot_win_opts o = { 100, 1, 56, 0, 0, 0, 90, 0 }; return o; }
static inline oot_win_shape oot_win_shape_of(const oot_win_opts *o) {
    oot_win_shape s;
    s.w = oot_win_clampi(o->width_cm, 20, 300) * 0.01f;
    s.h = o->keep169 ? s.w * 9 / 16 : oot_win_clampi(o->height_cm, 10, 200) * 0.01f;
    s.round = oot_win_clampi(o->round_pct, 0, 100) * 0.01f;
    s.feather = oot_win_clampi(o->feather_mm, 0, 100) * 0.001f;
    s.tilt = oot_win_clampi(o->tilt_deg, -45, 45) * (float)M_PI / 180;
    s.dist = oot_win_clampi(o->dist_cm, 40, 300) * 0.01f;
    s.drop = OOT_WIN_DROP_M;
    s.back = o->back != 0;
    return s;
}
static inline float oot_win_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
/* The frame for a viewpoint `at` facing the heading yaw (0 along -z, positive to the right, as oot_hud_heading): dist ahead along
   the heading, drop under it, facing it, turned about its own x by the tilt (its top away from the viewpoint). */
static inline oot_win_frame oot_win_place(const float at[3], float yaw, const oot_win_shape *s) {
    oot_win_frame f;
    const float fx = sinf(yaw), fz = -cosf(yaw), ct = cosf(s->tilt), st = sinf(s->tilt);
    f.c[0] = at[0] + fx * s->dist; f.c[1] = at[1] - s->drop; f.c[2] = at[2] + fz * s->dist;
    f.x[0] = cosf(yaw); f.x[1] = 0; f.x[2] = sinf(yaw);
    /* up leans toward the heading (away from the viewer) by the tilt; z = x cross y */
    f.y[0] = fx * st; f.y[1] = ct; f.y[2] = fz * st;
    f.z[0] = f.x[1] * f.y[2] - f.x[2] * f.y[1]; f.z[1] = f.x[2] * f.y[0] - f.x[0] * f.y[2]; f.z[2] = f.x[0] * f.y[1] - f.x[1] * f.y[0];
    f.hw = s->w / 2; f.hh = s->h / 2;
    return f;
}
/* A point of the room in the frame's own axes (metres from its middle; z > 0 in front of it). */
static inline void oot_win_local(const oot_win_frame *f, const float p[3], float out[3]) {
    const float d[3] = { p[0] - f->c[0], p[1] - f->c[1], p[2] - f->c[2] };
    out[0] = oot_win_dot(d, f->x); out[1] = oot_win_dot(d, f->y); out[2] = oot_win_dot(d, f->z);
}
/* How far a point (x, y) of the frame's plane is inside its shape, metres (negative outside): the rectangle's edge distance, or with
   a roundness the superellipse's, measured along the line from the middle (scaled by the smaller half size). The Metal shader
   (zeldaVR/gfx/oot_vr_window_draw.h) computes the same. */
static inline float oot_win_inside(float x, float y, float hw, float hh, float round) {
    if (!(hw > 0) || !(hh > 0)) return -1;
    if (round <= 0.005f) return fminf(hw - fabsf(x), hh - fabsf(y));
    const float n = 2 / fminf(round, 1), ax = fabsf(x) / hw, ay = fabsf(y) / hh, m = fmaxf(ax, ay);
    if (m <= 0) return fminf(hw, hh);
    const float r = m * powf(powf(ax / m, n) + powf(ay / m, n), 1 / n);   /* 1 on the edge, the terms at most 1: no overflow */
    return (1 - r) * fminf(hw, hh);
}
/* The frame's cover at a point: 1 inside, 0 outside, a ramp over the feather inside the edge (none: a hard edge). */
static inline float oot_win_alpha(float x, float y, float hw, float hh, float round, float feather) {
    const float d = oot_win_inside(x, y, hw, hh, round);
    if (feather <= 0) return d >= 0 ? 1.0f : 0.0f;
    return d <= 0 ? 0 : d >= feather ? 1 : d / feather;
}
/* How the frame is seen from an eye (in the frame's axes): its front (through or fit), its back panel, or nothing; fade: how much of
   the picture shows (0 at OOT_WIN_NEAR_M from the plane, 1 from OOT_WIN_NEAR_M + OOT_WIN_FADE_M). */
static inline int oot_win_side(const float eye_local[3], int through, int back, float *fade) {
    const float z = eye_local[2];
    *fade = 1;
    if (!(z == z)) return OOT_WIN_NONE;
    if (z <= 0) return back ? OOT_WIN_BACK : OOT_WIN_NONE;
    if (z <= OOT_WIN_NEAR_M) return OOT_WIN_NONE;
    *fade = fminf(1, (z - OOT_WIN_NEAR_M) / OOT_WIN_FADE_M);
    return through ? OOT_WIN_THROUGH : OOT_WIN_FIT;
}
/* Through: the picture's (u, v) (0..1 across and down) for the frame point (px, py) seen from the eye `e` (frame axes, e[2] > 0),
   the picture's tangent box lrbt = left, right, bottom, top. 0 when the ray misses the picture (an eye too near, or the box empty). */
static inline int oot_win_through_uv(const float e[3], float px, float py, const float lrbt[4], float uv[2]) {
    if (!(e[2] > 0) || !(lrbt[1] > lrbt[0]) || !(lrbt[3] > lrbt[2])) return 0;
    const float tx = (px - e[0]) / e[2], ty = (py - e[1]) / e[2];
    uv[0] = (tx - lrbt[0]) / (lrbt[1] - lrbt[0]);
    uv[1] = (lrbt[3] - ty) / (lrbt[3] - lrbt[2]);
    return uv[0] >= 0 && uv[0] <= 1 && uv[1] >= 0 && uv[1] <= 1;
}
/* A picture rendered with a symmetric view: vertical half tangent th, width over height a. */
static inline void oot_win_symmetric_box(float th, float a, float lrbt[4]) {
    lrbt[0] = -th * a; lrbt[1] = th * a; lrbt[2] = -th; lrbt[3] = th;
}
/* Fit: where the whole picture (aspect pa = width / height) sits in the frame (aspect fa), as the frame's fraction 0..1 across and
   down: scale sx, sy and offset ox, oy; a frame point at (s, t) shows the picture's ((s - ox) / sx, (t - oy) / sy). */
static inline void oot_win_fit(float pa, float fa, float r[4]) {
    r[0] = r[1] = 1; r[2] = r[3] = 0;
    if (!(pa > 0) || !(fa > 0)) return;
    if (pa > fa) { r[1] = fa / pa; r[3] = (1 - r[1]) / 2; }
    else { r[0] = pa / fa; r[2] = (1 - r[0]) / 2; }
}

/* vr.md row 3, second step (research: no caller yet). The off-axis view from an eye through the frame's four corners: the tangents of
   its left, right, bottom and top edges at 1 along -z (the eye in the frame's axes, e[2] > 0). */
static inline int oot_win_frustum(const float e[3], float hw, float hh, float lrbt[4]) {
    if (!(e[2] > 0) || !(hw > 0) || !(hh > 0)) return 0;
    lrbt[0] = (-hw - e[0]) / e[2]; lrbt[1] = (hw - e[0]) / e[2];
    lrbt[2] = (-hh - e[1]) / e[2]; lrbt[3] = (hh - e[1]) / e[2];
    return 1;
}
/* The same box as the game's sead perspective projection holds it (read on our image, update 131072): +0xa0 the vertical angle,
   +0xac its half tangent t, +0xb0 the aspect a, +0xb4 / +0xb8 an offset (ox, oy). The matrix update main+0x3485cc0 (and its inlined
   copies at main+0x4f1e8, main+0x23b9a8, main+0x33b82c) puts 2 * ox, 2 * oy in the third column, so the edges are at
   t * a * (-1 + 2 ox) .. t * a * (1 + 2 ox) across and t * (-1 + 2 oy) .. t * (1 + 2 oy) up; the culling snapshot (main+0x33af64..
   0x33af70) takes the same 1 +- 2 * offset. */
static inline int oot_win_sead(const float lrbt[4], float *fovy, float *aspect, float *ox, float *oy) {
    const float tx = (lrbt[1] - lrbt[0]) / 2, ty = (lrbt[3] - lrbt[2]) / 2;
    if (!(tx > 0) || !(ty > 0)) return 0;
    *fovy = 2 * atanf(ty); *aspect = tx / ty;
    *ox = (lrbt[1] + lrbt[0]) / (4 * tx); *oy = (lrbt[3] + lrbt[2]) / (4 * ty);
    return 1;
}
