/* Ocarina of Time in VR: hearts, magic and rupees on the back of the wrist, shown while the player looks at it as at a
   watch, without the runtime (tests/test_oot_vr_wrist.c runs it). The idea of a wrist panel and of when it shows follows
   HookshotVR's description (vr/menu/wrist_host.c, GPL-3: ideas only, no code); the picture is our own.
   Lengths in metres, angles in degrees, times in seconds. */
#pragma once
#include <math.h>
#include <string.h>

typedef struct {
    float eye_distance;   /* from the eyes to the panel */
    float facing;         /* between the back of the wrist and the way to the eyes */
    float gaze;           /* between where the head looks and the way to the panel */
    float hand_speed;     /* the faster hand, m/s */
    int blocked;          /* a fight, a talk, the sword out: never then */
} oot_wrist_in;
typedef struct { float seen, lost, shown; int on; } oot_wrist_state;

/* How visible the panel is, 0..1: it comes after half a second of looking at a still wrist, stays through small movements
   and goes 0.2 s after the look is lost, at once when blocked. */
static inline float oot_wrist_step(oot_wrist_state *s, const oot_wrist_in *in, float dt) {
    if (!(dt > 0)) dt = 0;
    if (dt > 0.1f) dt = 0.1f;
    const int look = in->eye_distance < 0.40f && in->facing < 35 && in->gaze < 30 && in->hand_speed < 0.6f;
    const int keep = in->eye_distance < 0.45f && in->facing < 45 && in->gaze < 40 && in->hand_speed < 0.9f;
    if (in->blocked) { s->on = 0; s->seen = s->lost = 0; s->shown = 0; return 0; }
    if (!s->on) {
        s->seen = look ? s->seen + dt : 0;
        if (s->seen >= 0.5f) { s->on = 1; s->lost = 0; }
    } else {
        s->lost = keep ? 0 : s->lost + dt;
        if (s->lost >= 0.2f) { s->on = 0; s->seen = 0; }
    }
    s->shown += s->on ? dt / 0.12f : -dt / 0.25f;
    s->shown = s->shown < 0 ? 0 : s->shown > 1 ? 1 : s->shown;
    return s->shown;
}

/* Hearts: life and its most in quarter hearts. How many quarters of heart i (from 0) are full, 0..4; how many hearts in all. */
static inline int oot_heart_count(int max_life) { return max_life <= 0 ? 0 : (max_life + 3) / 4; }
static inline int oot_heart_quarters(int life, int i) { const int q = life - 4 * i; return q < 0 ? 0 : q > 4 ? 4 : q; }

/* The panel in the room from the wrist's pose. back: the unit normal out of the back of the wrist; fingers: the unit way the open
   fingers point; head_right: the head's right. The panel lies on the back of the wrist, a little toward the elbow, and its
   words read from the viewer's left to right whichever arm it is on. */
typedef struct { float center[3], right[3], up[3]; } oot_wrist_quad;
static inline oot_wrist_quad oot_wrist_place(const float grip[3], const float back[3], const float fingers[3], const float head_right[3], float width, float aspect) {
    oot_wrist_quad q;
    float along[3] = { fingers[0], fingers[1], fingers[2] };
    if (along[0] * head_right[0] + along[1] * head_right[1] + along[2] * head_right[2] < 0) for (int i = 0; i < 3; i++) along[i] = -along[i];
    const float up[3] = { back[1] * along[2] - back[2] * along[1], back[2] * along[0] - back[0] * along[2], back[0] * along[1] - back[1] * along[0] };
    for (int i = 0; i < 3; i++) {
        q.center[i] = grip[i] - fingers[i] * 0.08f + back[i] * 0.035f;
        q.right[i] = along[i] * width; q.up[i] = up[i] * width / aspect;
    }
    return q;
}
