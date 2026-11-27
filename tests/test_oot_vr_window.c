/* The window view of Ocarina of Time (zeldaVR/runtime/oot_vr_window.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_window.h"

#define DEG ((float)M_PI / 180)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

static void options(void) {
    oot_win_opts o = oot_win_default_opts();
    oot_win_shape s = oot_win_shape_of(&o);                            /* HookshotVR's defaults: 1 m wide at 16:9, 90 cm ahead */
    assert(NEAR(s.w, 1) && NEAR(s.h, 0.5625f) && NEAR(s.dist, 0.9f) && NEAR(s.drop, 0.12f));
    assert(s.round == 0 && s.feather == 0 && s.tilt == 0 && !s.back);
    o.keep169 = 0; o.height_cm = 80; s = oot_win_shape_of(&o);         /* its own height without Keep 16:9 */
    assert(NEAR(s.h, 0.8f));
    o = (oot_win_opts){ 1000, 0, 1, 500, -3, 90, 5, 7 }; s = oot_win_shape_of(&o);   /* out of range: clamped */
    assert(NEAR(s.w, 3) && NEAR(s.h, 0.1f) && NEAR(s.round, 1) && s.feather == 0 && NEAR(s.tilt, 45 * DEG) && NEAR(s.dist, 0.4f) && s.back == 1);
}

static void placing(void) {
    oot_win_opts o = oot_win_default_opts();
    oot_win_shape s = oot_win_shape_of(&o);
    const float at[3] = { 1, 1.6f, 2 };
    oot_win_frame f = oot_win_place(at, 0, &s);                         /* facing -z */
    assert(NEAR(f.c[0], 1) && NEAR(f.c[1], 1.48f) && NEAR(f.c[2], 1.1f));
    assert(NEAR(f.x[0], 1) && NEAR(f.y[1], 1) && NEAR(f.z[2], 1));       /* x right, y up, z back toward the viewer */
    assert(NEAR(f.hw, 0.5f) && NEAR(f.hh, 0.28125f));
    float l[3]; oot_win_local(&f, at, l);                                /* the viewpoint: 12 cm above its middle, 90 cm in front */
    assert(NEAR(l[0], 0) && NEAR(l[1], 0.12f) && NEAR(l[2], 0.9f));
    f = oot_win_place(at, 90 * DEG, &s);                                /* turned right: ahead is +x */
    assert(NEAR(f.c[0], 1.9f) && NEAR(f.c[2], 2) && NEAR(f.x[2], 1) && NEAR(f.z[0], -1));
    o.tilt_deg = 30; s = oot_win_shape_of(&o);                          /* its top leans away */
    f = oot_win_place(at, 0, &s);
    assert(f.y[2] < 0 && NEAR(f.y[1], cosf(30 * DEG)) && f.z[1] > 0);
    assert(NEAR(oot_win_dot(f.x, f.y), 0) && NEAR(oot_win_dot(f.y, f.z), 0) && NEAR(oot_win_dot(f.z, f.z), 1));
}

static void shape(void) {
    /* a rectangle: edge distances */
    assert(NEAR(oot_win_inside(0, 0, 0.5f, 0.3f, 0), 0.3f));
    assert(NEAR(oot_win_inside(0.45f, 0, 0.5f, 0.3f, 0), 0.05f));
    assert(oot_win_inside(0.49f, 0.29f, 0.5f, 0.3f, 0) > 0);            /* the corner is in */
    assert(oot_win_inside(0.51f, 0, 0.5f, 0.3f, 0) < 0);
    /* an ellipse at 100 %: the corner is cut, the edge middles stay */
    assert(oot_win_inside(0.49f, 0.29f, 0.5f, 0.3f, 1) < 0);
    assert(oot_win_inside(0.49f, 0, 0.5f, 0.3f, 1) > 0 && oot_win_inside(0, 0.29f, 0.5f, 0.3f, 1) > 0);
    assert(NEAR(oot_win_inside(0.5f * cosf(0.7f), 0.3f * sinf(0.7f), 0.5f, 0.3f, 1), 0));   /* on the ellipse */
    /* rounder corners as it grows */
    const float x = 0.47f, y = 0.27f;
    assert(oot_win_inside(x, y, 0.5f, 0.3f, 0.1f) > oot_win_inside(x, y, 0.5f, 0.3f, 0.5f));
    assert(oot_win_inside(x, y, 0.5f, 0.3f, 0.01f) > 0);                /* almost a rectangle: no overflow */
    /* the feather */
    assert(oot_win_alpha(0, 0, 0.5f, 0.3f, 0, 0) == 1 && oot_win_alpha(0.6f, 0, 0.5f, 0.3f, 0, 0) == 0);
    assert(NEAR(oot_win_alpha(0.49f, 0, 0.5f, 0.3f, 0, 0.02f), 0.5f));
    assert(oot_win_alpha(0.3f, 0, 0.5f, 0.3f, 0, 0.02f) == 1);
}

static void sides(void) {
    float fade, e[3] = { 0, 0, 0.9f };
    assert(oot_win_side(e, 1, 0, &fade) == OOT_WIN_THROUGH && fade == 1);
    assert(oot_win_side(e, 0, 0, &fade) == OOT_WIN_FIT);
    e[2] = 0.08f; assert(oot_win_side(e, 1, 0, &fade) == OOT_WIN_THROUGH && NEAR(fade, 0.5f));   /* fading near its plane */
    e[2] = 0.01f; assert(oot_win_side(e, 1, 1, &fade) == OOT_WIN_NONE);
    e[2] = -0.5f; assert(oot_win_side(e, 1, 1, &fade) == OOT_WIN_BACK);  /* behind it: the back panel when asked for */
    assert(oot_win_side(e, 1, 0, &fade) == OOT_WIN_NONE);
    e[2] = NAN; assert(oot_win_side(e, 1, 1, &fade) == OOT_WIN_NONE);
}

static void through(void) {
    float box[4]; oot_win_symmetric_box(1.0f, 1.2f, box);               /* a picture 2 x 2.4 in tangents */
    assert(NEAR(box[0], -1.2f) && NEAR(box[1], 1.2f) && NEAR(box[2], -1) && NEAR(box[3], 1));
    float uv[2], e[3] = { 0, 0, 1 };
    assert(oot_win_through_uv(e, 0, 0, box, uv) && NEAR(uv[0], 0.5f) && NEAR(uv[1], 0.5f));   /* straight ahead: the middle */
    assert(oot_win_through_uv(e, 0.6f, 0.5f, box, uv) && NEAR(uv[0], 0.75f) && NEAR(uv[1], 0.25f));
    /* the eye moved left: the same point of the frame shows more of the right side (a window, not a sticker) */
    float uv2[2]; e[0] = -0.3f;
    assert(oot_win_through_uv(e, 0, 0, box, uv2) && uv2[0] > 0.5f && NEAR(uv2[0], 0.5f + 0.3f / 2.4f));
    /* nearer: a wider part of the world through the same frame */
    float a[2], b[2]; e[0] = 0; e[2] = 0.5f;
    assert(oot_win_through_uv(e, 0.4f, 0, box, a)); e[2] = 1;
    assert(oot_win_through_uv(e, 0.4f, 0, box, b) && a[0] > b[0]);
    /* a corner past the picture's edge: a miss */
    e[2] = 0.1f; assert(!oot_win_through_uv(e, 0.5f, 0, box, uv));
    e[2] = 0; assert(!oot_win_through_uv(e, 0, 0, box, uv));
}

static void fit(void) {
    float r[4];
    oot_win_fit(16.0f / 9, 16.0f / 9, r); assert(NEAR(r[0], 1) && NEAR(r[1], 1) && NEAR(r[2], 0) && NEAR(r[3], 0));
    oot_win_fit(1, 16.0f / 9, r);                                        /* a square canvas in a 16:9 frame: bars at the sides */
    assert(NEAR(r[0], 9.0f / 16) && NEAR(r[1], 1) && NEAR(r[2], (1 - 9.0f / 16) / 2) && NEAR(r[3], 0));
    oot_win_fit(2, 1, r);                                                /* wider than the frame: bars above and below */
    assert(NEAR(r[0], 1) && NEAR(r[1], 0.5f) && NEAR(r[3], 0.25f));
    oot_win_fit(0, 1, r); assert(r[0] == 1 && r[1] == 1);
}

static void off_axis(void) {
    /* centred: a symmetric box */
    float box[4], e[3] = { 0, 0, 1 }, fovy, aspect, ox, oy;
    assert(oot_win_frustum(e, 0.5f, 0.25f, box));
    assert(oot_win_sead(box, &fovy, &aspect, &ox, &oy));
    assert(NEAR(fovy, 2 * atanf(0.25f)) && NEAR(aspect, 2) && NEAR(ox, 0) && NEAR(oy, 0));
    /* from above and to the left: the box leans right and down; sead's edges (t a (+-1 + 2 ox), t (+-1 + 2 oy)) give it back */
    e[0] = -0.2f; e[1] = 0.12f; e[2] = 0.9f;
    assert(oot_win_frustum(e, 0.5f, 0.28125f, box));
    assert(oot_win_sead(box, &fovy, &aspect, &ox, &oy));
    assert(ox > 0 && oy < 0);
    const float t = tanf(fovy / 2);
    assert(NEAR(t * aspect * (-1 + 2 * ox), box[0]) && NEAR(t * aspect * (1 + 2 * ox), box[1]));
    assert(NEAR(t * (-1 + 2 * oy), box[2]) && NEAR(t * (1 + 2 * oy), box[3]));
    /* the frame's corners seen from that eye through that box are the picture's corners */
    float uv[2];
    oot_win_through_uv(e, -0.5f, 0.28125f, box, uv); assert(NEAR(uv[0], 0) && NEAR(uv[1], 0));
    oot_win_through_uv(e, 0.5f, -0.28125f, box, uv); assert(NEAR(uv[0], 1) && NEAR(uv[1], 1));
    e[2] = 0; assert(!oot_win_frustum(e, 0.5f, 0.25f, box));
}

int main(void) {
    options(); placing(); shape(); sides(); through(); fit(); off_axis();
    puts("oot_vr_window: ok");
    return 0;
}
