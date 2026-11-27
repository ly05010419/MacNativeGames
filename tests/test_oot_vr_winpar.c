/* The window view's parallax (zeldaVR/runtime/oot_vr_winpar.h): the whole chain from the head to the frame, with a game view that
   is turned and pitched, against a real window. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_winpar.h"

#define NEAR(a, b, e) (fabsf((a) - (b)) < (e))

/* A game view: turned by yaw, pitched down by pitch, somewhere in the world (rows: the camera's right, up, back; column 3 t). */
static void game_view(float yaw, float pitch, const float pos[3], float m[12]) {
    const float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    const float right[3] = { cy, 0, sy }, back0[3] = { -sy, 0, cy };   /* level: back = +z turned */
    const float up[3] = { -back0[0] * sp, cp, -back0[2] * sp }, back[3] = { back0[0] * cp, sp, back0[2] * cp };   /* looking down by pitch */
    const float *rows[3] = { right, up, back };
    for (int i = 0; i < 3; i++) { memcpy(m + 4 * i, rows[i], 12); m[4 * i + 3] = -oot_win_dot(rows[i], pos); }
}
static void apply(const float m[12], const float w[3], float c[3]) { for (int i = 0; i < 3; i++) c[i] = oot_win_dot(m + 4 * i, w) + m[4 * i + 3]; }
/* The world point the game shows at the room point pl (the frame's axes) while its camera is unmoved: camera coordinates pl - anchor. */
static void world_of(const float m[12], const float anchor_l[3], const float pl[3], float w[3]) {
    const float c[3] = { pl[0] - anchor_l[0], pl[1] - anchor_l[1], pl[2] - anchor_l[2] };
    float d[3]; for (int i = 0; i < 3; i++) d[i] = c[i] - m[4 * i + 3];
    for (int j = 0; j < 3; j++) w[j] = m[j] * d[0] + m[4 + j] * d[1] + m[8 + j] * d[2];   /* R^T (c - t) */
}
/* The picture's (u, v) of a point in camera coordinates c, the picture rendered with the box lrbt. */
static void picture_uv(const float c[3], const float lrbt[4], float uv[2]) {
    const float tx = c[0] / -c[2], ty = c[1] / -c[2];
    uv[0] = (tx - lrbt[0]) / (lrbt[1] - lrbt[0]); uv[1] = (lrbt[3] - ty) / (lrbt[3] - lrbt[2]);
}
/* The frame point the window shows a picture's (u, v) at, seen from `from` (the inverse of oot_win_through_uv). */
static void frame_point(const float from[3], const float lrbt[4], const float uv[2], float p[2]) {
    const float tx = lrbt[0] + uv[0] * (lrbt[1] - lrbt[0]), ty = lrbt[3] - uv[1] * (lrbt[3] - lrbt[2]);
    p[0] = from[0] + tx * from[2]; p[1] = from[1] + ty * from[2];
}
/* A real window: where the ray from the eye e to the point pl (both in the frame's axes) crosses the frame's plane. */
static void real_window(const float e[3], const float pl[3], float p[2]) {
    const float s = e[2] / (e[2] - pl[2]);
    p[0] = e[0] + (pl[0] - e[0]) * s; p[1] = e[1] + (pl[1] - e[1]) * s;
}

typedef struct { oot_win_frame f; float anchor[3], anchor_l[3], m[12], box[4]; } scene;
static scene make_scene(int tilt_deg) {
    scene s; oot_win_opts o = oot_win_default_opts(); o.tilt_deg = tilt_deg;
    const oot_win_shape sh = oot_win_shape_of(&o);
    s.anchor[0] = 0.3f; s.anchor[1] = 1.6f; s.anchor[2] = -0.2f;
    s.f = oot_win_place(s.anchor, 0.4f, &sh);
    oot_win_local(&s.f, s.anchor, s.anchor_l);
    const float pos[3] = { 12, 3, -40 };
    game_view(1.1f, 0.3f, pos, s.m);                      /* the game's own camera, turned and looking down */
    oot_win_symmetric_box(1.25f, 1.2f, s.box);            /* the headset's field of view, as the game renders it */
    return s;
}
/* Where the window shows the room point pl when the head is at `head` (the room): the whole chain. *old: without the parallax
   (the camera unmoved, seen from the eye now: the window as committed in a21e531c). */
static void shown(const scene *s, const float head[3], const float pl[3], float p[2], float old[2]) {
    float w[3]; world_of(s->m, s->anchor_l, pl, w);
    float off[3]; assert(oot_win_par_offset(&s->f, s->anchor, head, off));
    float m[12]; memcpy(m, s->m, sizeof m); assert(oot_win_par_view(m, off));
    float c[3], uv[2]; apply(m, w, c); picture_uv(c, s->box, uv);
    float from[3]; oot_win_par_from(&s->f, s->anchor, off, from);
    frame_point(from, s->box, uv, p);
    float back[2]; assert(oot_win_through_uv(from, p[0], p[1], s->box, back) || 1);   /* the draw's own mapping gives the same (u, v) */
    assert(NEAR(back[0], uv[0], 1e-4f) && NEAR(back[1], uv[1], 1e-4f));
    float e[3]; oot_win_local(&s->f, head, e);
    float c0[3]; apply(s->m, w, c0); float uv0[2]; picture_uv(c0, s->box, uv0); frame_point(e, s->box, uv0, old);
}

static void chain(int tilt) {
    const scene s = make_scene(tilt);
    const float *r = s.f.x, *u = s.f.y, *z = s.f.z;        /* the frame's axes in the room */
    const float h1[3] = { s.anchor[0] - 0.1f * r[0], s.anchor[1] - 0.1f * r[1], s.anchor[2] - 0.1f * r[2] };
    const float h2[3] = { s.anchor[0] + 0.1f * r[0], s.anchor[1] + 0.1f * r[1], s.anchor[2] + 0.1f * r[2] };   /* 20 cm to the right */
    /* crouched 15 cm and 20 cm nearer the frame: still where a real window shows it */
    const float h3[3] = { s.anchor[0] - 0.15f * u[0] - 0.2f * z[0], s.anchor[1] - 0.15f * u[1] - 0.2f * z[1], s.anchor[2] - 0.15f * u[2] - 0.2f * z[2] };
    const float near_pt[3] = { 0.05f, -0.04f, -0.5f }, far_pt[3] = { -40, 25, -1000 };   /* half a metre behind the frame; a kilometre */
    float p1[2], p2[2], o1[2], o2[2], real1[2], real2[2], e1[3], e2[3];
    oot_win_local(&s.f, h1, e1); oot_win_local(&s.f, h2, e2);
    /* a near point lands where a real window shows it, from each head place */
    shown(&s, h1, near_pt, p1, o1); shown(&s, h2, near_pt, p2, o2);
    real_window(e1, near_pt, real1); real_window(e2, near_pt, real2);
    assert(NEAR(p1[0], real1[0], 1e-4f) && NEAR(p1[1], real1[1], 1e-4f));
    assert(NEAR(p2[0], real2[0], 1e-4f) && NEAR(p2[1], real2[1], 1e-4f));
    { float p3[2], o3[2], e3[3], real3[2]; oot_win_local(&s.f, h3, e3); shown(&s, h3, near_pt, p3, o3); real_window(e3, near_pt, real3);
      assert(NEAR(p3[0], real3[0], 1e-4f) && NEAR(p3[1], real3[1], 1e-4f) && fabsf(p3[1] - p1[1]) > 0.01f); }
    /* and it moves against the frame as the head moves: 20 cm across, 0.5 m behind a frame about 0.9 m away: about 7 cm, the same way
       (a real window: 20 cm x 0.5 / (0.5 + the eye's distance)) */
    const float shift = p2[0] - p1[0], want = 0.2f * 0.5f / (0.5f + e1[2]);
    assert(NEAR(shift, want, 1e-4f) && shift > 0.05f && shift < 0.09f);
    /* without the parallax the near point kept its place against the eye's view: it moved with the head, 20 cm (as if at infinity) */
    assert(fabsf((o2[0] - o1[0]) - 0.2f) < 0.01f);
    /* a far point: where the window without the parallax showed it, within a fraction of a millimetre */
    float f1[2], f1o[2], f2[2], f2o[2];
    shown(&s, h1, far_pt, f1, f1o); shown(&s, h2, far_pt, f2, f2o);
    assert(NEAR(f1[0], f1o[0], 5e-4f) && NEAR(f1[1], f1o[1], 5e-4f) && NEAR(f2[0], f2o[0], 5e-4f) && NEAR(f2[1], f2o[1], 5e-4f));
    /* at the anchor the parallax changes nothing, near or far */
    float a0[2], a0o[2]; shown(&s, s.anchor, near_pt, a0, a0o);
    assert(NEAR(a0[0], a0o[0], 1e-5f) && NEAR(a0[1], a0o[1], 1e-5f));
}

static void offsets(void) {
    const scene s = make_scene(0);
    float off[3];
    assert(oot_win_par_offset(&s.f, s.anchor, s.anchor, off) && off[0] == 0 && off[1] == 0 && off[2] == 0);
    /* 10 cm along the frame's right, 5 cm toward it (its z is out toward the viewer) */
    const float h[3] = { s.anchor[0] + 0.1f * s.f.x[0] - 0.05f * s.f.z[0], s.anchor[1], s.anchor[2] + 0.1f * s.f.x[2] - 0.05f * s.f.z[2] };
    assert(oot_win_par_offset(&s.f, s.anchor, h, off) && NEAR(off[0], 0.1f, 1e-5f) && NEAR(off[1], 0, 1e-5f) && NEAR(off[2], -0.05f, 1e-5f));
    const float far_head[3] = { s.anchor[0] + 3 * s.f.x[0], s.anchor[1] - 5, s.anchor[2] + 3 * s.f.x[2] };   /* at most a metre each way */
    assert(oot_win_par_offset(&s.f, s.anchor, far_head, off) && NEAR(off[0], 1, 1e-5f) && NEAR(off[1], -1, 1e-5f));
    const float bad[3] = { NAN, 1, 0 };
    assert(!oot_win_par_offset(&s.f, s.anchor, bad, off));
    /* the view: moved in its own axes, never turned; refused (unchanged) when not finite or beyond a metre */
    float m[12]; memcpy(m, s.m, sizeof m);
    const float o[3] = { 0.1f, -0.2f, 0.3f };
    assert(oot_win_par_view(m, o));
    for (int i = 0; i < 3; i++) { assert(NEAR(m[4 * i + 3], s.m[4 * i + 3] - o[i], 1e-6f)); for (int j = 0; j < 3; j++) assert(m[4 * i + j] == s.m[4 * i + j]); }
    /* the camera's place moved by +o along its own axes */
    float e0[3], e1[3]; for (int j = 0; j < 3; j++) { e0[j] = -(s.m[j] * s.m[3] + s.m[4 + j] * s.m[7] + s.m[8 + j] * s.m[11]); e1[j] = -(m[j] * m[3] + m[4 + j] * m[7] + m[8 + j] * m[11]); }
    for (int j = 0; j < 3; j++) assert(NEAR(e1[j] - e0[j], o[0] * s.m[j] + o[1] * s.m[4 + j] + o[2] * s.m[8 + j], 1e-4f));
    memcpy(m, s.m, sizeof m);
    const float big[3] = { 1.5f, 0, 0 }, nan3[3] = { 0, NAN, 0 };
    assert(!oot_win_par_view(m, big) && !oot_win_par_view(m, nan3) && !memcmp(m, s.m, sizeof m));
    m[5] = INFINITY; assert(!oot_win_par_view(m, o) && m[3] == s.m[3]);
}

static void ring(void) {
    static oot_wpar_ring r;
    const float a[3] = { 0.1f, 0.2f, 0.9f }, b[3] = { -0.1f, 0.1f, 0.8f };
    float got[3];
    assert(!oot_wpar_get(&r, 5, got));                       /* nothing yet: a head-look picture */
    oot_wpar_put(&r, 5, a);
    assert(oot_wpar_get(&r, 5, got) && got[0] == a[0] && got[2] == a[2]);
    assert(!oot_wpar_get(&r, 69, got));                      /* the same slot, another sequence */
    oot_wpar_put(&r, 69, b);
    assert(!oot_wpar_get(&r, 5, got) && oot_wpar_get(&r, 69, got) && got[0] == b[0]);
    oot_wpar_put(&r, 0, a); assert(!oot_wpar_get(&r, 0, got));   /* sequence 0 is "no pose" */
    float e[3] = { 0, 0.12f, 0.9f };
    oot_wpar_eye(e, 0, 0.032f); assert(NEAR(e[0], -0.032f, 1e-6f));
    oot_wpar_eye(e, 1, 0.032f); assert(NEAR(e[0], 0, 1e-6f));
    oot_wpar_eye(e, -1, 0.032f); oot_wpar_eye(e, 1, 0); assert(NEAR(e[0], 0, 1e-6f));
}

int main(void) {
    chain(0); chain(20); chain(-30);
    offsets();
    ring();
    printf("oot_vr_winpar: all passed\n");
    return 0;
}
