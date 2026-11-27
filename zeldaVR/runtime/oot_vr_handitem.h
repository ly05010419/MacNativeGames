/* Ocarina of Time in VR: what the items held in the player's hands share, without the runtime (tests/test_oot_vr_hookshot.c and
   the other item tests run it). The hookshot, the boomerang, the bombs and the bottles (vr.md rows 63-67) are each a small state
   machine in its own header; each gets the same input once per Link update and answers with the game's buttons to add and to
   take away, which hands' triggers it uses, and the calls it wants made on the game's thread.
   Ideas, constants and the game's action-state values follow HookshotVR's notes (aim-items.md 0, 3, 4; held-items.md 0, 5, 6;
   GPL-3: facts only, no code). Positions are the game's world (Link-sized metres), times nanoseconds. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

/* One hand, as the graphics side last saw it (games/totk/third_party/d2r_gfx/gfx/nvn/oot_vr_arms.inc). */
typedef struct {
    int have;
    float pos[3], vel[3];   /* the grip, in the game's world */
    float dir[3];           /* the controller's pointing line (unit) */
    float palm[3];          /* straight out of the palm (unit) */
    float trigger, grip;    /* 0..1 */
    float f, r, u;          /* the hand from the head in the room, metres: ahead, right, up (the gestures' zones) */
} oot_ih_hand;
typedef struct { oot_ih_hand hand[2]; int have_head; float eye[3], fwd[3]; uint64_t at; } oot_ih_frame;

/* What every item machine is given. */
typedef struct {
    uint64_t now;
    const oot_ih_frame *hands;   /* NULL or stale: no hands */
    int item;                    /* the game's number of the item on the item button, -1 none or unknown */
    uint32_t action;             /* Link's action state (player object +0x1f60) */
    int first_person, in_event;
} oot_ih_in;

/* The game's buttons (oot_vr_pad.h numbering) an item adds and takes away, and which hands' triggers it owns (the pad then sends
   neither ZL nor ZR from that trigger). */
enum { OOT_IH_A = 1u << 0, OOT_IH_B = 1u << 1, OOT_IH_ZL = 1u << 8, OOT_IH_ZR = 1u << 9 };
typedef struct { uint32_t add, mask; int triggers; } oot_ih_buttons;
static inline void oot_ih_merge(oot_ih_buttons *to, const oot_ih_buttons *from) { to->add |= from->add; to->mask |= from->mask; to->triggers |= from->triggers; }

/* The item numbers (the item wheel's table, oot_vr_wheel.h; HookshotVR's radial_items.h). */
enum { OOT_IH_NONE, OOT_IH_HOOKSHOT, OOT_IH_BOOMERANG, OOT_IH_BOMBS, OOT_IH_BOTTLE };
static inline int oot_ih_kind(int item) {
    switch (item) {
        case 5: case 6: return OOT_IH_HOOKSHOT;            /* hookshot, longshot */
        case 3: return OOT_IH_BOOMERANG;
        case 2: return OOT_IH_BOMBS;
        case 14: case 15: case 16: case 17: case 50: return OOT_IH_BOTTLE;
        default: return OOT_IH_NONE;
    }
}

/* A grip or trigger closes above `on` and opens below `off` (HookshotVR: 0.55 / 0.35). 1 on the frame it closes. */
typedef struct { int closed; } oot_ih_press;
static inline int oot_ih_press_step(oot_ih_press *p, float v, float on, float off) {
    const int was = p->closed;
    if (v > on) p->closed = 1; else if (v < off) p->closed = 0;
    return p->closed && !was;
}
/* A fresh grip may take an item only where no gesture wants it: not behind the shoulder (the sword, the shield) and not low or far
   ahead (picking things up), the same zones as oot_vr_gesture.h. Our own rule: HookshotVR has no grip gestures to share with. */
static inline int oot_ih_free_zone(const oot_ih_hand *h, int right) {
    const int behind = h->f < -0.02f && h->u > -0.42f && h->u < 0.30f && fabsf(h->r) < 0.50f && (right ? h->r > -0.05f : h->r < 0.05f);
    const int reach = h->u < -0.55f || h->f > 0.50f;
    return !behind && !reach;
}

/* The game's put-away by its own call (main+0x4a8690, games/oot/runtime/oot_vr_putaway.h): asked at the let-go, again every 300 ms
   while the item still reads as out, at most four times, then given up. A grip taking it again first withdraws a call not yet made. */
enum { OOT_IH_AWAY_TRIES = 4 };
static const uint64_t OOT_IH_AWAY_GAP = 300000000ull;
typedef struct { int asked, tries, failed; uint64_t last; } oot_ih_away;
static inline void oot_ih_away_ask(oot_ih_away *a) { if (!a->asked) { a->asked = 1; a->tries = 0; a->failed = 0; a->last = 0; } }
static inline void oot_ih_away_cancel(oot_ih_away *a) { a->asked = 0; }
/* 1: make the call now. out: the item still reads as out. */
static inline int oot_ih_away_step(oot_ih_away *a, uint64_t now, int out) {
    if (!a->asked) return 0;
    if (!out) { a->asked = 0; return 0; }
    if (a->tries >= OOT_IH_AWAY_TRIES) { a->asked = 0; a->failed = 1; return 0; }
    if (a->tries && now - a->last < OOT_IH_AWAY_GAP) return 0;
    a->tries++; a->last = now;
    return 1;
}

/* Small vector helpers. */
static inline float oot_ih_dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float oot_ih_dist(const float a[3], const float b[3]) {
    const float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return sqrtf(oot_ih_dot(d, d));
}
static inline int oot_ih_unit(float v[3]) {
    const float n = sqrtf(oot_ih_dot(v, v));
    if (!(n > 1e-6f) || !isfinite(n)) return 0;
    for (int i = 0; i < 3; i++) v[i] /= n;
    return 1;
}
static inline int oot_ih_finite(const float *v, int n) { for (int i = 0; i < n; i++) if (!isfinite(v[i])) return 0; return 1; }
/* The controller's pointing line from its grip axes: along the fingers' direction `along` and out of the back of the hand `back`
   (OpenXR's grip: -y along the open fingers, +x out of the back of the right hand and the palm of the left, so `back` is +x for the
   right and -x for the left). The line through the tube of the closed fingers, little finger to thumb, is grip -z: back x along for
   the right hand, along x back for the left. */
static inline void oot_ih_pointing(int right, const float along[3], const float back[3], float out[3]) {
    const float *a = right ? back : along, *b = right ? along : back;
    out[0] = a[1] * b[2] - a[2] * b[1]; out[1] = a[2] * b[0] - a[0] * b[2]; out[2] = a[0] * b[1] - a[1] * b[0];
}
/* A hand's data is good to use. */
static inline int oot_ih_hand_ok(const oot_ih_hand *h) {
    return h->have && oot_ih_finite(h->pos, 3) && oot_ih_finite(h->vel, 3) && oot_ih_finite(h->dir, 3) && oot_ih_finite(h->palm, 3) && isfinite(h->trigger) && isfinite(h->grip);
}
