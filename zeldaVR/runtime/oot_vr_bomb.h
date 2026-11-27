/* Ocarina of Time in VR: bombs in the player's hands (vr.md rows 65 and 66), without the runtime (tests/test_oot_vr_bomb.c runs
   it). HookshotVR's way (held-items.md 5; GPL-3, ideas, constants and the game's values only). Two halves:
   - per Link update (oot_bombs_step): the hands. A grip closing near a bomb (its surface within 0.25 m) holds it as gripped; on a
     fruit still on its Bomb Flower the game is asked to pick it (its own A, 150 ms); an open, empty hand facing a bomb out of reach
     rings it (within the pull's reach and cone round the line out of the palm), and a fresh grip then pulls it; a fresh grip in
     no gesture's zone with bombs on the item button takes one from the bag (the game's ZR, 150 ms): the new bomb is held at once,
     Link's own lift ended after 600 ms (the game's "let go", called by the game side). The holding hand's trigger lights it.
     Letting go of the grip throws it (or, option throw_mode 1, the next squeeze), at the hand's last speed x the strength plus
     Link's own, at most 30 m/s;
   - per bomb, in its own update (oot_bomb_own): what the game side does to it. Held: placed in the palm (eased over 100 ms from
     where it was gripped) with the hand's velocity; pulled: flown to the palm at max(9 m/s, distance / 0.25 s), held within 0.25 m,
     dropped after 1.5 s; thrown: its velocity kept on the line a real fall makes (9.81 m/s2 x gravity %; the game's own is about
     28), until it hits something (its motion off the expected by max(4 cm, 35 %)), 0.2 s between updates, or 4 s; ours and unlit:
     the fuse held at a frame before it starts and its effects asleep, woken if the game sets it off (its animation changes);
     collision layer NoHit (14) held or pulled, HitOnlyGround (10) for 1 s after a let-go within 1.2 m of Link, else its own (0).
   A bomb is 0.45 m across (radius 0.22), its centre 0.2187 m above its origin. A bomb not updated for 400 ms is gone. */
#pragma once
#include <stddef.h>
#include "oot_vr_handitem.h"

enum { OOT_BOMB_MAX = 32, OOT_BOMB_LAYER_OWN = 0, OOT_BOMB_LAYER_GROUND = 10, OOT_BOMB_LAYER_NONE = 14 };
static const float OOT_BOMB_R = 0.22f, OOT_BOMB_UP = 0.2187f, OOT_BOMB_UNLIT_FRAME = 25.0f;
static const uint64_t OOT_BOMB_MS = 1000000ull;
static const uint32_t OOT_BOMB_CARRY = 0x06000000u;   /* Link carries something over his head */

typedef struct {
    int throw_mode;        /* 0 letting go throws, 1 the next squeeze throws */
    float strength;        /* the throw's speed, times the hand's (HookshotVR 1.2) */
    float gravity;         /* m/s2 a thrown bomb falls with */
    int pull; float reach, cone;   /* the pull: on, metres, radians either side of the palm's line */
    int flowers, bracelet; /* Bomb Flowers may be taken; the Goron's Bracelet (or a gauntlet) is owned */
    int fuse_hold, quiet;  /* the game side can hold the fuse (its float call is there); unlit bombs' effects asleep */
} oot_bomb_opts;

typedef struct {
    uintptr_t actor;
    float centre[3];
    uint64_t born, seen, prev_at, pull_at, released_at, settle_until, seat_at, flight_at;
    int flower, on_plant, owned, lit, touched, asleep, held, pulled, flying, layer, vn;
    float seat_from[3], fv[3], prev[3], vs[3][3];
    uint64_t command;
} oot_bomb;
enum { OOT_BOMB_OUT_NONE, OOT_BOMB_OUT_TAKE, OOT_BOMB_OUT_THROW, OOT_BOMB_OUT_CATCH };
typedef struct {
    oot_bomb b[OOT_BOMB_MAX];
    oot_ih_press grip[2], trig[2];
    int ring[2];                       /* the bomb each open hand faces, index + 1 */
    int phase, phase_hand, phase_bomb; uint64_t phase_at;   /* a bomb from the bag */
    uint64_t zr_until, a_until;
    int pick_hand, pick_bomb; uint64_t pick_at;              /* a Bomb Flower asked to be picked, index + 1 */
    uint64_t fall_at, carry_at; int falls;
    float link_v[3], link_prev[3]; uint64_t link_at;
} oot_bombs;
enum { OOT_BOMB_EV_HELD = 1, OOT_BOMB_EV_THROWN, OOT_BOMB_EV_LIT, OOT_BOMB_EV_PULL, OOT_BOMB_EV_TAKE, OOT_BOMB_EV_NONE_CAME, OOT_BOMB_EV_PICK, OOT_BOMB_EV_PICKED, OOT_BOMB_EV_PICK_FAILED, OOT_BOMB_EV_DROPPED };
typedef struct { oot_ih_buttons b; int fall; int event, event_hand, event_bomb; float event_speed; } oot_bombs_out;
typedef struct { int place; float origin[3]; int velocity; float v[3]; int stop_spin; int layer; int frame; int sleep; int event; } oot_bomb_cmd;

static inline void oot_bombs_init(oot_bombs *s) { memset(s, 0, sizeof *s); for (int i = 0; i < OOT_BOMB_MAX; i++) s->b[i].layer = -1; }
/* A Bomb Flower's fruit by its actor's name (not the plant's: _A_02, _A_03 and Obj_BombFlower_Grass_*). */
static inline int oot_bomb_fruit_name(const char *n) {
    return !strcmp(n, "Obj_BombFlower_A_01") || !strcmp(n, "Obj_BombFlower_A_04") || !strncmp(n, "Obj_BombFlower_Fruit", 20);
}
static inline void oot_bomb_centre(const float origin[3], const float rot[9], float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = origin[k] + OOT_BOMB_UP * rot[3 * k + 1];   /* rot row-major, columns the actor's axes */
}
static inline float oot_bomb_level(const float a[3], const float b[3]) { return sqrtf((a[0] - b[0]) * (a[0] - b[0]) + (a[2] - b[2]) * (a[2] - b[2])); }

/* A bomb seen: by its own bomb AI (ai 1) or as a fruit waiting on its plant (flower 1, ai 0). Index, or -1 (table full of bombs in hands). */
static inline int oot_bombs_seen(oot_bombs *s, uintptr_t actor, const float centre[3], uint64_t now, int flower, int ai) {
    int at = -1, free_at = -1, oldest = -1;
    for (int i = 0; i < OOT_BOMB_MAX; i++) {
        oot_bomb *b = &s->b[i];
        if (b->actor == actor) { at = i; break; }
        if (!b->actor) { if (free_at < 0) free_at = i; }
        else if (!b->held && !b->pulled && (oldest < 0 || b->seen < s->b[oldest].seen)) oldest = i;
    }
    if (at < 0) {
        at = free_at >= 0 ? free_at : oldest;
        if (at < 0) return -1;
        memset(&s->b[at], 0, sizeof s->b[at]);
        s->b[at].actor = actor; s->b[at].born = now; s->b[at].layer = -1;
        s->b[at].flower = flower; s->b[at].on_plant = flower && !ai;
    }
    oot_bomb *b = &s->b[at];
    if (ai && b->on_plant) { b->on_plant = 0; b->lit = 1; b->settle_until = now + 700 * OOT_BOMB_MS; }   /* picked: its fuse is the game's */
    memcpy(b->centre, centre, 12); b->seen = now;
    return at;
}
static inline void oot_bombs_gone(oot_bombs *s, uint64_t now) {
    for (int i = 0; i < OOT_BOMB_MAX; i++) {
        oot_bomb *b = &s->b[i];
        if (!b->actor || now - b->seen < (b->on_plant ? 5000 : 400) * OOT_BOMB_MS) continue;
        if (s->phase_bomb == i + 1) s->phase_bomb = 0;
        if (s->pick_bomb == i + 1) s->pick_bomb = 0;
        for (int h = 0; h < 2; h++) if (s->ring[h] == i + 1) s->ring[h] = 0;
        memset(b, 0, sizeof *b); b->layer = -1;
    }
}
/* Link's own velocity from where his feet were: samples 1-200 ms apart, an axis over 30 m/s ignored, smoothed over ~0.1 s. */
static inline void oot_bombs_link(oot_bombs *s, const float feet[3], uint64_t now) {
    if (s->link_at) {
        const float dt = (float)(now - s->link_at) / 1e9f;
        if (dt >= 0.001f && dt <= 0.2f) {
            const float k = fminf(1, 12 * dt);
            for (int i = 0; i < 3; i++) { const float v = (feet[i] - s->link_prev[i]) / dt; if (fabsf(v) <= 30) s->link_v[i] += (v - s->link_v[i]) * k; }
        }
        if (dt < 0.001f) return;
    }
    memcpy(s->link_prev, feet, 12); s->link_at = now;
}
static inline int oot_bomb_takeable(const oot_bomb *b, const oot_bomb_opts *p) { return b->owned || (p->bracelet && (!b->flower || p->flowers)); }
/* The bomb an open hand faces, index + 1, 0 none. `was`: the one it faced last time keeps 1.4 times the cone and wins near ties. */
static inline int oot_bomb_ring(const oot_bombs *s, int hand, const oot_ih_hand *h, const float feet[3], const oot_bomb_opts *p, int was) {
    int best = 0; float best_score = 1e9f;
    for (int j = 0; j < OOT_BOMB_MAX; j++) {
        const oot_bomb *b = &s->b[j];
        if (!b->actor || b->held || (b->pulled && b->pulled != hand + 1)) continue;
        const int fruit = b->on_plant;
        if (!(fruit ? p->bracelet && p->flowers : oot_bomb_takeable(b, p))) continue;
        if (fruit && oot_bomb_level(b->centre, feet) > 1.6f) continue;
        if (oot_ih_dist(b->centre, h->pos) - OOT_BOMB_R <= 0.25f) continue;   /* in the grip's reach */
        float v[3]; for (int i = 0; i < 3; i++) v[i] = b->centre[i] - (h->pos[i] + 0.03f * h->palm[i]);
        const float d = sqrtf(oot_ih_dot(v, v));
        if (!(d > 1e-4f) || d > p->reach) continue;
        const float c = oot_ih_dot(v, h->palm) / d, angle = acosf(c > 1 ? 1 : c < -1 ? -1 : c);
        const int keep = was == j + 1;
        if (angle > p->cone * (keep ? 1.4f : 1)) continue;
        const float score = angle * (keep ? 0.8f : 1);
        if (score < best_score) { best_score = score; best = j + 1; }
    }
    return best;
}
static inline void oot_bomb_hold(oot_bombs *s, int j, int hand, const oot_ih_hand *h, uint64_t now) {
    oot_bomb *b = &s->b[j];
    for (int i = 0; i < 2; i++) if (s->ring[i] == j + 1) s->ring[i] = 0;
    b->held = hand + 1; b->pulled = 0; b->flying = 0; b->owned = 1; b->touched = 1; b->vn = 0;
    for (int i = 0; i < 3; i++) b->seat_from[i] = b->centre[i] - h->pos[i];
    b->seat_at = now;
}
static inline void oot_bombs_out_event(oot_bombs_out *o, int ev, int hand, int bomb) { if (!o->event) { o->event = ev; o->event_hand = hand; o->event_bomb = bomb; } }

/* Link's update. active: bombs on the item button, the option on, first person, no scene. carrying: Link's action has 0x06000000. */
static inline void oot_bombs_step(oot_bombs *s, const oot_ih_in *in, const oot_bomb_opts *p, int active, const float feet[3], oot_bombs_out *o) {
    memset(o, 0, sizeof *o);
    const uint64_t now = in->now;
    const int carrying = (in->action & OOT_BOMB_CARRY) != 0;
    oot_bombs_gone(s, now);
    if (!in->hands || !in->first_person || in->in_event) {
        for (int j = 0; j < OOT_BOMB_MAX; j++) s->b[j].held = s->b[j].pulled = 0;
        for (int i = 0; i < 2; i++) { s->grip[i].closed = s->trig[i].closed = 0; s->ring[i] = 0; }
        s->phase = OOT_BOMB_OUT_NONE; s->pick_bomb = 0;
        return;
    }
    const oot_ih_frame *f = in->hands;
    if (active) o->b.mask |= OOT_IH_ZR;   /* the real ZR does nothing: the trigger lights instead */
    for (int i = 0; i < 2; i++) {
        const oot_ih_hand *h = &f->hand[i];
        const int ok = oot_ih_hand_ok(h);
        const int closing = ok && oot_ih_press_step(&s->grip[i], h->grip, 0.55f, 0.35f);
        if (!ok) s->grip[i].closed = 0;
        const int closed = s->grip[i].closed;
        const int lighting = ok && oot_ih_press_step(&s->trig[i], h->trigger, 0.6f, 0.4f);
        int held = 0, pulled = 0;
        for (int j = 0; j < OOT_BOMB_MAX; j++) { if (s->b[j].held == i + 1) held = j + 1; if (s->b[j].pulled == i + 1) pulled = j + 1; }
        if (pulled && !closed) { s->b[pulled - 1].pulled = 0; s->b[pulled - 1].released_at = now; oot_bombs_out_event(o, OOT_BOMB_EV_DROPPED, i, pulled); pulled = 0; }   /* it flies on by itself */
        if (held) {
            oot_bomb *b = &s->b[held - 1];
            if (lighting && !b->lit) { b->lit = 1; oot_bombs_out_event(o, OOT_BOMB_EV_LIT, i, held); }
            const int throws = p->throw_mode == 1 ? closing : !closed;
            if (throws) {
                b->held = 0; b->released_at = now;
                if (!b->on_plant) {
                    float v[3] = { 0, 0, 0 }; const int n = b->vn < 3 ? b->vn : 3;
                    for (int k = 0; k < n; k++) for (int c = 0; c < 3; c++) v[c] += b->vs[k][c] / (float)n;
                    for (int c = 0; c < 3; c++) v[c] = v[c] * p->strength + s->link_v[c];
                    const float sp = sqrtf(oot_ih_dot(v, v));
                    if (sp > 30) for (int c = 0; c < 3; c++) v[c] *= 30 / sp;
                    memcpy(b->fv, v, 12); b->flying = 1; b->flight_at = now; b->prev_at = 0;
                    oot_bombs_out_event(o, OOT_BOMB_EV_THROWN, i, held); o->event_speed = sp > 30 ? 30 : sp;
                }
                if (s->phase_bomb == held) s->phase = OOT_BOMB_OUT_NONE;
            }
            continue;
        }
        if (closed && !pulled && (p->throw_mode != 1 || closing)) {
            int near = 0; float near_d = 1e9f;
            for (int j = 0; j < OOT_BOMB_MAX; j++) {
                const oot_bomb *b = &s->b[j];
                if (!b->actor || !(b->on_plant ? p->bracelet && p->flowers : oot_bomb_takeable(b, p))) continue;
                if ((b->held && !closing) || (b->pulled && b->pulled != i + 1) || (b->on_plant && !closing) || s->phase_bomb == j + 1) continue;
                const float d = oot_ih_dist(b->centre, h->pos) - OOT_BOMB_R;
                if (d <= 0.25f && d < near_d) { near_d = d; near = j + 1; }
            }
            if (near && s->b[near - 1].on_plant) {
                s->a_until = now + 150 * OOT_BOMB_MS; s->pick_hand = i; s->pick_bomb = near; s->pick_at = now;
                oot_bombs_out_event(o, OOT_BOMB_EV_PICK, i, near);
            } else if (near) {
                oot_bomb_hold(s, near - 1, i, h, now);
                oot_bombs_out_event(o, OOT_BOMB_EV_HELD, i, near);
                continue;
            } else if (closing && s->ring[i] && s->b[s->ring[i] - 1].on_plant) {
                s->a_until = now + 150 * OOT_BOMB_MS; s->pick_hand = i; s->pick_bomb = s->ring[i]; s->pick_at = now;
                oot_bombs_out_event(o, OOT_BOMB_EV_PICK, i, s->ring[i]);
            } else if (closing && s->ring[i] && p->pull) {
                oot_bomb *b = &s->b[s->ring[i] - 1];
                b->pulled = i + 1; b->pull_at = now; b->flying = 0; b->touched = 1; b->prev_at = 0; b->settle_until = now + 700 * OOT_BOMB_MS;
                oot_bombs_out_event(o, OOT_BOMB_EV_PULL, i, s->ring[i]);
                s->ring[i] = 0;
            } else if (closing && active && s->phase == OOT_BOMB_OUT_NONE && oot_ih_free_zone(h, i == 1)) {
                s->zr_until = now + 150 * OOT_BOMB_MS; s->phase = OOT_BOMB_OUT_TAKE; s->phase_hand = i; s->phase_at = now; s->phase_bomb = 0;
                oot_bombs_out_event(o, OOT_BOMB_EV_TAKE, i, 0);
            }
        }
        s->ring[i] = !closed && ok && p->pull ? oot_bomb_ring(s, i, h, feet, p, s->ring[i]) : 0;
    }
    /* a bomb from the bag */
    if (s->phase == OOT_BOMB_OUT_TAKE) {
        int got = 0;
        for (int j = 0; j < OOT_BOMB_MAX && !got; j++) if (s->b[j].actor && !s->b[j].flower && s->b[j].born >= s->phase_at && !s->b[j].held) got = j + 1;
        const oot_ih_hand *h = &f->hand[s->phase_hand];
        if (got && oot_ih_hand_ok(h)) {
            oot_bomb *b = &s->b[got - 1];
            oot_bomb_hold(s, got - 1, s->phase_hand, h, now);
            for (int c = 0; c < 3; c++) b->seat_from[c] = h->palm[c] * (0.03f + OOT_BOMB_R);   /* upright against the palm at once */
            b->settle_until = now + 700 * OOT_BOMB_MS;
            s->phase = OOT_BOMB_OUT_THROW; s->phase_bomb = got; s->phase_at = now;
            oot_bombs_out_event(o, OOT_BOMB_EV_HELD, s->phase_hand, got);
        } else if (now - s->phase_at > 1200 * OOT_BOMB_MS) { s->phase = OOT_BOMB_OUT_NONE; oot_bombs_out_event(o, OOT_BOMB_EV_NONE_CAME, s->phase_hand, 0); }
    } else if (s->phase == OOT_BOMB_OUT_THROW && now - s->phase_at >= 600 * OOT_BOMB_MS) {
        o->fall = 1;   /* his lift is done: the game's own let-go, once */
        const oot_ih_hand *h = &f->hand[s->phase_hand];
        if (s->phase_bomb && oot_ih_hand_ok(h)) { oot_bomb *b = &s->b[s->phase_bomb - 1]; for (int c = 0; c < 3; c++) b->seat_from[c] = b->centre[c] - h->pos[c]; b->seat_at = now; }
        s->phase = OOT_BOMB_OUT_CATCH; s->phase_at = now;
    } else if (s->phase == OOT_BOMB_OUT_CATCH && now - s->phase_at >= 200 * OOT_BOMB_MS) { s->phase = OOT_BOMB_OUT_NONE; s->phase_bomb = 0; }
    /* a Bomb Flower the game was asked to pick: Link lifts it (carrying), the hand gets the fruit his lift raised */
    if (s->pick_bomb) {
        if (carrying) {
            int fruit = s->pick_bomb; float top = -1e9f;
            for (int j = 0; j < OOT_BOMB_MAX; j++) {
                const oot_bomb *b = &s->b[j];
                if (!b->actor || !b->flower || oot_bomb_level(b->centre, feet) > 0.8f || b->centre[1] - feet[1] <= 0.7f) continue;
                if (b->centre[1] > top) { top = b->centre[1]; fruit = j + 1; }
            }
            const oot_ih_hand *h = &f->hand[s->pick_hand];
            if (oot_ih_hand_ok(h)) { oot_bomb_hold(s, fruit - 1, s->pick_hand, h, now); s->b[fruit - 1].on_plant = 0; s->b[fruit - 1].settle_until = now + 700 * OOT_BOMB_MS; }
            oot_bombs_out_event(o, OOT_BOMB_EV_PICKED, s->pick_hand, fruit);
            s->pick_bomb = 0;
        } else if (now - s->pick_at > 1500 * OOT_BOMB_MS) { oot_bombs_out_event(o, OOT_BOMB_EV_PICK_FAILED, s->pick_hand, s->pick_bomb); s->pick_bomb = 0; }
    }
    /* Link carries what a hand holds (he lifted it): the game's let-go every 600 ms (a flower 200 ms), at most 10 times */
    if (!carrying) { s->falls = 0; s->carry_at = 0; }
    else {
        if (!s->carry_at) s->carry_at = now;
        int flower = -1;
        for (int j = 0; j < OOT_BOMB_MAX; j++) if (s->b[j].held) flower = flower > 0 ? 1 : s->b[j].flower;
        const uint64_t from = s->fall_at > s->carry_at ? s->fall_at : s->carry_at;
        if (s->phase == OOT_BOMB_OUT_NONE && s->falls < 10 && flower >= 0 && now - from >= (flower ? 200 : 600) * OOT_BOMB_MS) { o->fall = 1; s->falls++; }
    }
    if (o->fall) s->fall_at = now;
    for (int j = 0; j < OOT_BOMB_MAX; j++) if (s->b[j].held) o->b.triggers |= 1 << (s->b[j].held - 1);   /* a hand holding a bomb: its trigger lights it */
    if (now < s->zr_until) o->b.add |= OOT_IH_ZR;
    if (now < s->a_until) o->b.add |= OOT_IH_A;
}

/* A bomb's own update. origin, rot: where it is (actor +0x31c, +0x328); command: its fuse animation's current command (0 unknown). */
static inline void oot_bomb_own(oot_bombs *s, int j, const oot_ih_frame *f, const float rot[9], const float feet[3], uint64_t command, int carrying,
                                uint64_t now, const oot_bomb_opts *p, oot_bomb_cmd *c) {
    memset(c, 0, sizeof *c); c->layer = -1; c->frame = 0;
    oot_bomb *b = &s->b[j];
    /* ours and unlit: the fuse held; the game setting it off (an arrow, fire, a blast) changes its animation's command, except in the
       700 ms after a pick or a pull-out, when the command may change by itself */
    const int hold = b->owned && !b->lit && p->fuse_hold && !b->flower;
    if (hold && command) {
        if (now < b->settle_until || !b->command) b->command = command;
        else if (command != b->command) { b->lit = 1; c->event = OOT_BOMB_EV_LIT; }
    }
    c->frame = b->owned && !b->lit && p->fuse_hold && !b->flower;
    const int sleep = c->frame && p->quiet;
    if (sleep && !b->asleep) { c->sleep = 1; b->asleep = 1; }
    else if (!sleep && b->asleep) { c->sleep = -1; b->asleep = 0; }
    const int layer = b->held || b->pulled ? OOT_BOMB_LAYER_NONE
                    : now - b->released_at < 1000 * OOT_BOMB_MS && b->released_at && oot_bomb_level(b->centre, feet) < 1.2f ? OOT_BOMB_LAYER_GROUND : OOT_BOMB_LAYER_OWN;
    if (b->touched && layer != b->layer) { c->layer = layer; b->layer = layer; }
    const uint64_t last = b->prev_at; b->prev_at = now;
    if (b->held && f) {
        const oot_ih_hand *h = &f->hand[b->held - 1];
        const int pulling_out = s->phase_bomb == j + 1 && (s->phase == OOT_BOMB_OUT_TAKE || s->phase == OOT_BOMB_OUT_THROW);
        if (!oot_ih_hand_ok(h) || carrying || b->on_plant || pulling_out) return;
        float t = (float)(now - b->seat_at) / (100.0f * (float)OOT_BOMB_MS); t = t < 0 ? 0 : t > 1 ? 1 : t; t = t * t * (3 - 2 * t);
        for (int k = 0; k < 3; k++) {
            const float seat = b->seat_from[k] + (h->palm[k] * (0.03f + OOT_BOMB_R) - b->seat_from[k]) * t;
            c->origin[k] = h->pos[k] + seat - OOT_BOMB_UP * rot[3 * k + 1];
            c->v[k] = h->vel[k] + s->link_v[k];
            b->vs[b->vn % 3][k] = h->vel[k];
        }
        b->vn++;
        c->place = c->velocity = c->stop_spin = 1;
        return;
    }
    if (b->pulled && f) {
        const oot_ih_hand *h = &f->hand[b->pulled - 1];
        if (!oot_ih_hand_ok(h) || now - b->pull_at > 1500 * OOT_BOMB_MS) { b->pulled = 0; b->released_at = now; c->event = OOT_BOMB_EV_DROPPED; return; }
        float to[3], d[3];
        for (int k = 0; k < 3; k++) { to[k] = h->pos[k] + h->palm[k] * (0.03f + OOT_BOMB_R); d[k] = to[k] - b->centre[k]; }
        const float dist = sqrtf(oot_ih_dot(d, d));
        if (dist < 0.25f) { const int hand = b->pulled - 1; oot_bomb_hold(s, j, hand, h, now); c->event = OOT_BOMB_EV_HELD; return; }
        const float dt = last ? fminf((float)(now - last) / 1e9f, 0.05f) : 0.016f, speed = fmaxf(9.0f, dist / 0.25f), step = fminf(dist, speed * dt);
        for (int k = 0; k < 3; k++) { const float u = d[k] / dist; c->origin[k] = b->centre[k] + u * step - OOT_BOMB_UP * rot[3 * k + 1]; c->v[k] = u * speed; }
        c->place = c->velocity = c->stop_spin = 1;
        return;
    }
    if (b->flying) {
        const uint64_t t = now - b->flight_at;
        if (last && b->flight_at < last) {
            const float dt = (float)(now - last) / 1e9f;
            float miss[3]; for (int k = 0; k < 3; k++) miss[k] = (b->centre[k] - b->prev[k]) - b->fv[k] * dt;
            const float expect = sqrtf(oot_ih_dot(b->fv, b->fv)) * dt;
            if (dt > 0.2f || (t > 100 * OOT_BOMB_MS && sqrtf(oot_ih_dot(miss, miss)) > fmaxf(0.04f, 0.35f * expect)) || t > 4000 * OOT_BOMB_MS) { b->flying = 0; memcpy(b->prev, b->centre, 12); return; }
            b->fv[1] -= p->gravity * dt;
        }
        memcpy(b->prev, b->centre, 12);
        memcpy(c->v, b->fv, 12); c->velocity = 1;
    }
}
