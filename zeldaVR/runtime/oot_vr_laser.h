/* Ocarina of Time in VR: a controller's laser, without the runtime (tests/test_oot_vr_laser.c runs it). vr.md row 90 (the VR menu is
   pointed at with it) and row 4 (the big screen is dragged and scaled with it). HookshotVR's ideas and numbers only (GPL-3; its
   vr/menu/vr_menu_layer.inc and vr_menu.inc read for the facts: either controller points, a trigger past 0.75 presses and under 0.4
   lets go; no code taken).

   The ray. HookshotVR points along OpenXR's aim pose. We have no aim pose: the streamer passes the ALVR client's controller motion,
   and ALVR 20.14.0's client sends the grip space for it (alvr/client_openxr/src/interaction.rs line 759, hand_source.grip_space).
   So the ray starts at the grip and goes along the grip's -z (the tube of the closed fingers, the line oot_vr_handitem.h's
   oot_ih_pointing uses for the boomerang), turned about the grip's x by `tilt_deg` (+ lifts the tip). How far the Quest's aim
   pose is turned from its grip is not known to us (unconfirmed): the VR page's "laser angle" (option laser_deg, 0) lets the player
   set it.

   Hits. A flat rectangle (centre c, full-width right r, full-height up u, as the headset's quads are drawn: c + (x - 0.5) r +
   (0.5 - y) u) gives the point's x right and y down, 0..1. The big screen (oot_vr_hud.h oot_arc_size, drawn by totk_vr.m
   oot_arc_draw) is a part of a vertical cylinder of radius R around the viewer's place O: azimuth a (0 along fw, + toward x)
   within +-half, height within +-height/2 of cy; x = a / (2 half) + 0.5, y = 0.5 - (p.y - cy) / height.

   Dragging the big screen (row 4): a grip pressed while the hand's ray is on the screen takes it; while held, the screen turns
   about O by how far the ray's point on the cylinder went round, and rises by how far it went up (both measured on the cylinder
   as placed, so the screen moving under the ray does not feed back). The other grip pressed meanwhile scales it: its width in
   degrees times the hands' distance now over their distance when the second grip came. Letting go of the taking grip ends it.
   Metres, radians inside, degrees in the place; OpenXR axes (y up, -z forward). */
#pragma once
#include <math.h>
#include <string.h>

static inline float oot_laser_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline int oot_laser_finite(const float *v, int n) { for (int i = 0; i < n; i++) if (!isfinite(v[i])) return 0; return 1; }
static inline void oot_laser_qrot(const float q[4], const float v[3], float o[3]) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    o[0] = v[0] + w * tx + (y * tz - z * ty); o[1] = v[1] + w * ty + (z * tx - x * tz); o[2] = v[2] + w * tz + (x * ty - y * tx);
}

/* The ray of a controller at grip pose (q xyzw, p): 1 with a unit direction, 0 when the pose is not usable. */
static inline int oot_laser_ray(const float q[4], const float p[3], float tilt_deg, float origin[3], float dir[3]) {
    if (!oot_laser_finite(q, 4) || !oot_laser_finite(p, 3) || !isfinite(tilt_deg)) return 0;
    const float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(n > 0.5f && n < 1.5f)) return 0;
    const float qn[4] = { q[0] / n, q[1] / n, q[2] / n, q[3] / n }, t = tilt_deg * (float)M_PI / 180;
    const float local[3] = { 0, sinf(t), -cosf(t) };   /* -z turned about x: + tilts it toward +y */
    oot_laser_qrot(qn, local, dir);
    memcpy(origin, p, 12);
    const float l = sqrtf(oot_laser_dot(dir, dir));
    if (!(l > 0.5f)) return 0;
    for (int i = 0; i < 3; i++) dir[i] /= l;
    return 1;
}

/* Where the ray meets the rectangle: 1 with x, y (0..1, y down) and the distance t, when it is in front within max_t and inside
   (or within `margin` of the edge, in the rectangle's own 0..1: a pointer already on it keeps it near the edge). */
static inline int oot_laser_hit_rect(const float o[3], const float d[3], const float c[3], const float r[3], const float u[3],
                                     float margin, float max_t, float *x, float *y, float *t) {
    const float n[3] = { r[1] * u[2] - r[2] * u[1], r[2] * u[0] - r[0] * u[2], r[0] * u[1] - r[1] * u[0] };
    const float rr = oot_laser_dot(r, r), uu = oot_laser_dot(u, u), dn = oot_laser_dot(d, n);
    if (!(rr > 1e-8f) || !(uu > 1e-8f) || !(fabsf(dn) > 1e-8f)) return 0;
    const float co[3] = { c[0] - o[0], c[1] - o[1], c[2] - o[2] }, k = oot_laser_dot(co, n) / dn;
    if (!(k > 0) || !(k <= max_t)) return 0;
    const float pc[3] = { o[0] + d[0] * k - c[0], o[1] + d[1] * k - c[1], o[2] + d[2] * k - c[2] };
    const float hx = oot_laser_dot(pc, r) / rr + 0.5f, hy = 0.5f - oot_laser_dot(pc, u) / uu;
    if (!(hx >= -margin && hx <= 1 + margin && hy >= -margin && hy <= 1 + margin)) return 0;
    *x = fminf(1, fmaxf(0, hx)); *y = fminf(1, fmaxf(0, hy)); *t = k;
    return 1;
}

/* The big screen's frame: O the viewer's place it curves around, x its right, fw its forward (level), cy its middle's height. */
typedef struct { float o[3], x[3], fw[3], cy, radius, half, height; } oot_arc_frame;
/* From place_screen's frame (menu_center 2 m ahead along the level forward, menu_right its right times its width), turned by
   yaw_deg to the right about O and raised by up_m; half / height from oot_arc_size. 0 when the frame is not usable. */
static inline int oot_arc_frame_make(const float center[3], const float right[3], float dist, float yaw_deg, float up_m, float radius,
                                     float half, float height, oot_arc_frame *f) {
    const float rl = sqrtf(right[0] * right[0] + right[2] * right[2]);
    if (!(rl > 1e-3f) || !oot_laser_finite(center, 3) || !isfinite(yaw_deg) || !isfinite(up_m) || !(radius > 0) || !(half > 0) || !(height > 0)) return 0;
    const float x0[3] = { right[0] / rl, 0, right[2] / rl }, fw0[3] = { x0[2], 0, -x0[0] };
    const float a = yaw_deg * (float)M_PI / 180, ca = cosf(a), sa = sinf(a);
    for (int i = 0; i < 3; i++) f->o[i] = center[i] - fw0[i] * dist;
    /* turned to the right by a: x0 -> x0 cos a - fw0 sin a ... written for a level vector v as (v.x cos - v.z sin, v.x sin + v.z cos) */
    f->x[0] = x0[0] * ca - x0[2] * sa; f->x[1] = 0; f->x[2] = x0[0] * sa + x0[2] * ca;
    f->fw[0] = f->x[2]; f->fw[1] = 0; f->fw[2] = -f->x[0];
    f->cy = center[1] + up_m; f->radius = radius; f->half = half; f->height = height;
    return 1;
}
/* Where the ray leaves the cylinder (the viewer is inside it): 1 with its azimuth a (radians, 0 along fw, + toward x) and height. */
static inline int oot_laser_cylinder(const float o[3], const float d[3], const oot_arc_frame *f, float max_t, float *a, float *py) {
    const float qx = o[0] - f->o[0], qz = o[2] - f->o[2], A = d[0] * d[0] + d[2] * d[2];
    if (!(A > 1e-6f)) return 0;
    const float B = 2 * (qx * d[0] + qz * d[2]), C = qx * qx + qz * qz - f->radius * f->radius, disc = B * B - 4 * A * C;
    if (!(disc >= 0)) return 0;
    const float t = (-B + sqrtf(disc)) / (2 * A);
    if (!(t > 0) || !(t <= max_t)) return 0;
    const float p[3] = { qx + d[0] * t, o[1] + d[1] * t, qz + d[2] * t };
    const float lx = p[0] * f->x[0] + p[2] * f->x[2], lf = p[0] * f->fw[0] + p[2] * f->fw[2];
    *a = atan2f(lx, lf); *py = p[1];
    return 1;
}
/* The ray on the screen itself: x, y in 0..1 (y down), within `margin` of the edges. */
static inline int oot_laser_hit_arc(const float o[3], const float d[3], const oot_arc_frame *f, float margin, float *x, float *y) {
    float a, py;
    if (!oot_laser_cylinder(o, d, f, 3 * f->radius, &a, &py)) return 0;
    const float hx = a / (2 * f->half) + 0.5f, hy = 0.5f - (py - f->cy) / f->height;
    if (!(hx >= -margin && hx <= 1 + margin && hy >= -margin && hy <= 1 + margin)) return 0;
    *x = fminf(1, fmaxf(0, hx)); *y = fminf(1, fmaxf(0, hy));
    return 1;
}

/* A trigger or a grip as a button, with hysteresis: down past `on`, up under `off`. Returns +1 on the press, -1 on the let go. */
typedef struct { int down; } oot_laser_button;
static inline int oot_laser_button_step(oot_laser_button *b, float value, float on, float off) {
    if (!isfinite(value)) value = 0;
    if (!b->down && value > on) { b->down = 1; return 1; }
    if (b->down && value < off) { b->down = 0; return -1; }
    return 0;
}
enum { OOT_LASER_TRIGGER_ON_PCT = 75, OOT_LASER_TRIGGER_OFF_PCT = 40 };   /* HookshotVR's trigger: > .75 presses, < .4 lets go */

/* Row 4: the big screen's place, and its drag. */
typedef struct { float yaw_deg, up_m, deg; } oot_screen_place;
enum { OOT_SCREEN_IDLE = 0, OOT_SCREEN_DRAG = 1, OOT_SCREEN_SCALE = 2 };
typedef struct { int mode, hand; float a0, y0, dist0; oot_screen_place at0; } oot_screen_drag;
typedef struct {
    int have[2];            /* the hand's ray is known */
    float o[2][3], d[2][3]; /* its ray */
    float pos[2][3];        /* the hand's place (for the scale) */
    int grip[2];            /* +1 pressed this frame, -1 let go, 0 neither (oot_laser_button_step) */
    int held[2];            /* the grip is down now */
} oot_screen_hands;
static const float OOT_SCREEN_UP_MAX = 1.5f, OOT_SCREEN_DEG_MIN = 60, OOT_SCREEN_DEG_MAX = 160;
static inline float oot_screen_wrap_deg(float a) { a = fmodf(a + 180, 360); if (a < 0) a += 360; return a - 180; }
/* One headset frame. base: the screen's frame as placed with no turn and no rise (oot_arc_frame_make with yaw 0, up 0), its half
   and height as drawn now; shown: the frame as drawn now. Returns 1 when a drag or a scale ended this frame (the place to be saved). */
static inline int oot_screen_drag_step(oot_screen_drag *s, const oot_screen_hands *h, const oot_arc_frame *base, const oot_arc_frame *shown,
                                       oot_screen_place *place) {
    float x, y, a, py;
    if (s->mode == OOT_SCREEN_IDLE) {
        for (int k = 0; k < 2; k++)
            if (h->have[k] && h->grip[k] > 0 && oot_laser_hit_arc(h->o[k], h->d[k], shown, 0.02f, &x, &y) &&
                oot_laser_cylinder(h->o[k], h->d[k], base, 3 * base->radius, &a, &py)) {
                s->mode = OOT_SCREEN_DRAG; s->hand = k; s->a0 = a; s->y0 = py; s->at0 = *place;
                return 0;
            }
        return 0;
    }
    const int k = s->hand, other = 1 - k;
    if (!h->held[k] || h->grip[k] < 0) { s->mode = OOT_SCREEN_IDLE; return 1; }
    if (s->mode == OOT_SCREEN_DRAG && h->have[other] && h->grip[other] > 0) {
        const float dd[3] = { h->pos[0][0] - h->pos[1][0], h->pos[0][1] - h->pos[1][1], h->pos[0][2] - h->pos[1][2] };
        const float dist = sqrtf(oot_laser_dot(dd, dd));
        if (dist > 0.05f && isfinite(dist)) { s->mode = OOT_SCREEN_SCALE; s->dist0 = dist; s->at0 = *place; }
        return 0;
    }
    if (s->mode == OOT_SCREEN_SCALE) {
        if (!h->held[other]) {   /* back to dragging, from where the ray is now */
            if (h->have[k] && oot_laser_cylinder(h->o[k], h->d[k], base, 3 * base->radius, &a, &py)) { s->mode = OOT_SCREEN_DRAG; s->a0 = a; s->y0 = py; s->at0 = *place; }
            else { s->mode = OOT_SCREEN_IDLE; return 1; }
            return 0;
        }
        const float dd[3] = { h->pos[0][0] - h->pos[1][0], h->pos[0][1] - h->pos[1][1], h->pos[0][2] - h->pos[1][2] };
        const float dist = sqrtf(oot_laser_dot(dd, dd));
        if (dist > 0.02f && isfinite(dist)) place->deg = fminf(OOT_SCREEN_DEG_MAX, fmaxf(OOT_SCREEN_DEG_MIN, s->at0.deg * dist / s->dist0));
        return 0;
    }
    if (h->have[k] && oot_laser_cylinder(h->o[k], h->d[k], base, 3 * base->radius, &a, &py)) {
        float da = a - s->a0;
        while (da > (float)M_PI) da -= 2 * (float)M_PI;
        while (da < -(float)M_PI) da += 2 * (float)M_PI;
        place->yaw_deg = oot_screen_wrap_deg(s->at0.yaw_deg + da * 180 / (float)M_PI);
        place->up_m = fminf(OOT_SCREEN_UP_MAX, fmaxf(-OOT_SCREEN_UP_MAX, s->at0.up_m + (py - s->y0)));
    }
    return 0;
}
