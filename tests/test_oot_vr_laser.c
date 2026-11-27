/* A controller's laser and the big screen's drag (zeldaVR/runtime/oot_vr_laser.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_laser.h"

#define NEAR(a, b, e) (fabsf((a) - (b)) < (e))

static void quat_x(float deg, float q[4]) { const float h = deg * (float)M_PI / 360; q[0] = sinf(h); q[1] = q[2] = 0; q[3] = cosf(h); }
static void quat_y(float deg, float q[4]) { const float h = deg * (float)M_PI / 360; q[1] = sinf(h); q[0] = q[2] = 0; q[3] = cosf(h); }

int main(void) {
    float o[3], d[3];
    const float id[4] = { 0, 0, 0, 1 }, at[3] = { 0.2f, 1.2f, -0.3f };
    assert(oot_laser_ray(id, at, 0, o, d));
    assert(NEAR(d[0], 0, 1e-6f) && NEAR(d[1], 0, 1e-6f) && NEAR(d[2], -1, 1e-6f) && o[1] == 1.2f);   /* the grip's -z */
    assert(oot_laser_ray(id, at, 30, o, d) && NEAR(d[1], 0.5f, 1e-5f) && d[2] < 0);                     /* + lifts the tip */
    float q[4]; quat_x(-90, q);                                                                         /* the controller tipped down */
    assert(oot_laser_ray(q, at, 0, o, d) && NEAR(d[1], -1, 1e-5f));
    quat_y(-90, q);                                                                                     /* turned to the right */
    assert(oot_laser_ray(q, at, 0, o, d) && NEAR(d[0], 1, 1e-5f));
    const float bad[4] = { 0, 0, 0, 0 }, nan4[4] = { NAN, 0, 0, 1 };
    assert(!oot_laser_ray(bad, at, 0, o, d) && !oot_laser_ray(nan4, at, 0, o, d));

    /* a 1.4 m x 0.91 m panel 1.2 m ahead */
    const float c[3] = { 0, 1.5f, -1.2f }, r[3] = { 1.4f, 0, 0 }, u[3] = { 0, 0.91f, 0 };
    float x, y, t;
    const float eye[3] = { 0, 1.5f, 0 }, ahead[3] = { 0, 0, -1 };
    assert(oot_laser_hit_rect(eye, ahead, c, r, u, 0, 5, &x, &y, &t) && NEAR(x, 0.5f, 1e-5f) && NEAR(y, 0.5f, 1e-5f) && NEAR(t, 1.2f, 1e-5f));
    const float to_corner[3] = { -0.7f, 0.455f, -1.2f }; float n = sqrtf(0.49f + 0.455f * 0.455f + 1.44f);
    const float dc[3] = { to_corner[0] / n, to_corner[1] / n, to_corner[2] / n };
    assert(oot_laser_hit_rect(eye, dc, c, r, u, 0.01f, 5, &x, &y, &t) && NEAR(x, 0, 1e-3f) && NEAR(y, 0, 1e-3f));   /* top left: 0 0 */
    const float behind[3] = { 0, 0, 1 }, past[3] = { 0.8f, 0, -0.6f };
    assert(!oot_laser_hit_rect(eye, behind, c, r, u, 0, 5, &x, &y, &t));                                  /* away from it */
    assert(!oot_laser_hit_rect(eye, past, c, r, u, 0, 5, &x, &y, &t));                                    /* beside it */
    assert(!oot_laser_hit_rect(eye, ahead, c, r, u, 0, 1.0f, &x, &y, &t));                                /* too far */
    /* the hover margin: just off the left edge is kept by a pointer already on it, and reads as the edge */
    const float off_edge[3] = { -0.72f, 0, -1.2f }; n = sqrtf(0.72f * 0.72f + 1.44f);
    const float de[3] = { off_edge[0] / n, 0, off_edge[2] / n };
    assert(!oot_laser_hit_rect(eye, de, c, r, u, 0, 5, &x, &y, &t));
    assert(oot_laser_hit_rect(eye, de, c, r, u, 0.03f, 5, &x, &y, &t) && x == 0);

    /* the trigger: past .75 presses, under .4 lets go, nothing in between */
    oot_laser_button b = { 0 };
    assert(oot_laser_button_step(&b, 0.5f, 0.75f, 0.4f) == 0);
    assert(oot_laser_button_step(&b, 0.8f, 0.75f, 0.4f) == 1);
    assert(oot_laser_button_step(&b, 0.6f, 0.75f, 0.4f) == 0 && b.down);
    assert(oot_laser_button_step(&b, 0.3f, 0.75f, 0.4f) == -1 && !b.down);
    assert(oot_laser_button_step(&b, NAN, 0.75f, 0.4f) == 0);

    /* the big screen: place_screen's frame 2 m ahead along -z, 3 m around the viewer at the origin */
    const float mc[3] = { 0, 1.6f, -2 }, mr[3] = { 2.6f, 0, 0 };
    const float half = 0.9f, height = 3.24f;
    oot_arc_frame base, shown;
    assert(oot_arc_frame_make(mc, mr, 2, 0, 0, 3, half, height, &base));
    assert(NEAR(base.o[2], 0, 1e-5f) && NEAR(base.fw[2], -1, 1e-5f) && NEAR(base.x[0], 1, 1e-5f) && NEAR(base.cy, 1.6f, 1e-6f));
    const float viewer[3] = { 0, 1.6f, 0 };
    assert(oot_laser_hit_arc(viewer, ahead, &base, 0, &x, &y) && NEAR(x, 0.5f, 1e-5f) && NEAR(y, 0.5f, 1e-5f));
    assert(!oot_laser_hit_arc(viewer, behind, &base, 0, &x, &y));
    /* turned 30 degrees to the right: its middle is now 30 degrees to the right */
    assert(oot_arc_frame_make(mc, mr, 2, 30, 0.2f, 3, half, height, &shown));
    const float r30[3] = { sinf(30 * (float)M_PI / 180), 0, -cosf(30 * (float)M_PI / 180) };
    assert(oot_laser_hit_arc(viewer, r30, &shown, 0, &x, &y) && NEAR(x, 0.5f, 1e-4f) && y > 0.5f);   /* the screen rose 0.2 m: its middle is above */
    float a, py;
    assert(oot_laser_cylinder(viewer, r30, &base, 9, &a, &py) && NEAR(a, 30 * (float)M_PI / 180, 1e-4f) && NEAR(py, 1.6f, 1e-5f));
    assert(!oot_arc_frame_make(mc, (const float[3]){ 0, 1, 0 }, 2, 0, 0, 3, half, height, &shown));       /* no level right: refused */

    /* a drag: the right grip on the screen's middle, the hand turns 20 degrees to the right and lifts the ray */
    oot_screen_place place = { 0, 0, 110 };
    oot_screen_drag s = { 0 };
    oot_screen_hands h = { { 0, 1 }, { { 0 } }, { { 0 } }, { { -0.2f, 1.2f, -0.3f }, { 0.2f, 1.2f, -0.3f } }, { 0, 0 }, { 0, 0 } };
    memcpy(h.o[1], viewer, 12); memcpy(h.d[1], ahead, 12);
    assert(oot_arc_frame_make(mc, mr, 2, place.yaw_deg, place.up_m, 3, half, height, &shown));
    h.grip[1] = 1; h.held[1] = 1;
    assert(!oot_screen_drag_step(&s, &h, &base, &shown, &place) && s.mode == OOT_SCREEN_DRAG && s.hand == 1);
    h.grip[1] = 0;
    const float r20[3] = { sinf(20 * (float)M_PI / 180) * 0.995f, 0.0998f, -cosf(20 * (float)M_PI / 180) * 0.995f };
    memcpy(h.d[1], r20, 12);
    assert(!oot_screen_drag_step(&s, &h, &base, &shown, &place));
    assert(NEAR(place.yaw_deg, 20, 0.05f) && place.up_m > 0.25f && place.up_m < 0.35f);
    /* the screen moved under the ray: the same ray keeps the same place (measured on the cylinder as placed, no feedback) */
    const oot_screen_place kept = place;
    assert(oot_arc_frame_make(mc, mr, 2, place.yaw_deg, place.up_m, 3, half, height, &shown));
    assert(!oot_screen_drag_step(&s, &h, &base, &shown, &place) && NEAR(place.yaw_deg, kept.yaw_deg, 1e-4f) && NEAR(place.up_m, kept.up_m, 1e-5f));
    /* the left grip comes: a scale, the hands 0.4 m apart -> 0.6 m: 1.5 times as wide, within 60..160 */
    h.have[0] = 1; memcpy(h.o[0], viewer, 12); memcpy(h.d[0], ahead, 12);
    h.grip[0] = 1; h.held[0] = 1;
    assert(!oot_screen_drag_step(&s, &h, &base, &shown, &place) && s.mode == OOT_SCREEN_SCALE);
    h.grip[0] = 0; h.pos[0][0] = -0.4f;   /* 0.6 m apart */
    assert(!oot_screen_drag_step(&s, &h, &base, &shown, &place) && NEAR(place.deg, 160, 1e-4f));   /* 165 clamped to 160 */
    h.pos[0][0] = -0.1f;                  /* 0.3 m apart: 82.5 */
    assert(!oot_screen_drag_step(&s, &h, &base, &shown, &place) && NEAR(place.deg, 82.5f, 1e-3f));
    /* the left grip let go: dragging again from where the ray is; the right let go: done, to be saved */
    h.held[0] = 0; h.grip[0] = -1;
    assert(!oot_screen_drag_step(&s, &h, &base, &shown, &place) && s.mode == OOT_SCREEN_DRAG);
    h.grip[0] = 0; h.held[1] = 0; h.grip[1] = -1;
    assert(oot_screen_drag_step(&s, &h, &base, &shown, &place) == 1 && s.mode == OOT_SCREEN_IDLE);
    assert(NEAR(place.yaw_deg, kept.yaw_deg, 1e-3f) && NEAR(place.deg, 82.5f, 1e-3f));
    /* a grip pressed with the ray off the screen takes nothing */
    h.grip[1] = 1; h.held[1] = 1; memcpy(h.d[1], behind, 12);
    assert(!oot_screen_drag_step(&s, &h, &base, &shown, &place) && s.mode == OOT_SCREEN_IDLE);
    /* round the back: the turn wraps within -180..180 */
    assert(NEAR(oot_screen_wrap_deg(190), -170, 1e-4f) && NEAR(oot_screen_wrap_deg(-190), 170, 1e-4f));
    puts("oot_vr_laser: ok");
    return 0;
}
