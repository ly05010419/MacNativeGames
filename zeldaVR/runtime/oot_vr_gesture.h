/* Ocarina of Time's motion controls, without the runtime (tests/test_oot_vr_gesture.c runs it). Hands in the head's level
   frame, metres: f ahead, r to the right, u up. The gestures press the game's own buttons, as OcarinaVR's description of
   them goes (ideas only, GPL-3):
     a hand closing behind the shoulder: right draws the sword (Y), left takes the shield (ZL, held while the left hand stays
       closed in front of the body)
     a swing with the sword out: Y for 100 ms, at most every 220 ms, ready again once the tip is below 0.96 m/s
     the right hand open for 0.45 s with the sword out: puts it away (B)
     a hand closing low or far ahead, with nothing in it, moving slowly: picks up (A); opening again throws it or puts it down (A)
   "Closed" is a grip above 0.6, "open" one at or under 0.3, in between it stays as it was. Times in nanoseconds. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef struct { int have; float f, r, u; float v[3]; float tip_speed; float grip; } oot_hand;

enum { OOT_GEST_A = 1, OOT_GEST_B = 2, OOT_GEST_Y = 4, OOT_GEST_ZL = 8 };

typedef struct {
    int started, closed[2], sword, shield, carrying, swing_armed;
    uint64_t y_until, a_until, b_until, last_swing, open_since;
    float carry_peak;
} oot_gesture_state;

typedef struct { uint32_t bits; int buzz[2]; } oot_gesture_out;

static inline int oot_gesture_behind(const oot_hand *h, int right) {
    return h->f < -0.02f && h->u > -0.42f && h->u < 0.30f && fabsf(h->r) < 0.50f && (right ? h->r > -0.05f : h->r < 0.05f);
}
static inline int oot_gesture_reach(const oot_hand *h) { return h->u < -0.25f - 0.30f || h->f > 0.25f + 0.25f; }
static inline float oot_gesture_speed(const oot_hand *h) { return sqrtf(h->v[0] * h->v[0] + h->v[1] * h->v[1] + h->v[2] * h->v[2]); }

static inline void oot_gesture_step(oot_gesture_state *s, uint64_t now, const oot_hand hands[2], oot_gesture_out *out) {
    enum { L = 0, R = 1 };
    const uint64_t PULSE = 100000000ull, GAP = 220000000ull, OPEN = 450000000ull;
    memset(out, 0, sizeof *out);
    if (!s->started) { s->started = 1; s->swing_armed = 1; }
    int closing[2] = { 0, 0 }, opening[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        if (!hands[i].have) continue;
        const int was = s->closed[i];
        if (hands[i].grip > 0.6f) s->closed[i] = 1; else if (hands[i].grip <= 0.3f) s->closed[i] = 0;
        closing[i] = s->closed[i] && !was; opening[i] = !s->closed[i] && was;
    }
    if (hands[R].have) {
        const oot_hand *h = &hands[R];
        if (closing[R] && oot_gesture_behind(h, 1) && !s->carrying) {
            if (!s->sword) { s->sword = 1; s->y_until = now + PULSE; s->swing_armed = 0; s->last_swing = now; out->buzz[R] = 1; }
        } else if (closing[R] && !s->sword && !s->carrying && oot_gesture_reach(h) && oot_gesture_speed(h) < 1.0f) {
            s->carrying = 1; s->carry_peak = 0; s->a_until = now + PULSE; out->buzz[R] = 1;
        }
        if (s->carrying) {
            s->carry_peak = fmaxf(s->carry_peak, oot_gesture_speed(h));
            if (opening[R]) { s->carrying = 0; s->a_until = now + PULSE; }   /* thrown or put down, the game's same button */
        }
        if (s->sword) {
            if (s->swing_armed && h->tip_speed > 2.4f && now - s->last_swing >= GAP) {
                s->swing_armed = 0; s->last_swing = now; s->y_until = now + PULSE; out->buzz[R] = 1;
            } else if (h->tip_speed < 0.96f) s->swing_armed = 1;
            if (h->grip <= 0.3f) {
                if (!s->open_since) s->open_since = now;
                else if (now - s->open_since > OPEN) { s->sword = 0; s->open_since = 0; s->b_until = now + PULSE; }
            } else s->open_since = 0;
        } else s->open_since = 0;
    }
    if (hands[L].have) {
        const oot_hand *h = &hands[L];
        if (closing[L] && oot_gesture_behind(h, 0) && !s->shield) { s->shield = 1; out->buzz[L] = 1; }
        if (opening[L]) s->shield = 0;
        if (s->shield && s->closed[L] && (h->f > 0.05f || oot_gesture_behind(h, 0))) out->bits |= OOT_GEST_ZL;
    }
    if (now < s->y_until) out->bits |= OOT_GEST_Y;
    if (now < s->a_until) out->bits |= OOT_GEST_A;
    if (now < s->b_until) out->bits |= OOT_GEST_B;
}
