/* Ocarina of Time in VR: child Link's boomerang in the player's hand (vr.md row 64), without the runtime
   (tests/test_oot_vr_boomerang.c runs it). HookshotVR's way (aim-items.md 4.2-4.5; GPL-3, ideas and the game's values only).
   Link's action state: 0x100000 it is in his hand, 0x80000 raised over his head, 0x400 the item's own state; at rest = 0x100400
   without 0x80000. The game raises it while ZR is held and throws it when ZR is let go; B lowers it without a throw.
     Idle      a fresh grip (in no gesture's zone) takes it: Held if it is already at rest, else Taking
     Taking    ZR held; at rest after 100 ms -> Held; raised -> Lowering; 700 ms -> Idle
     Lowering  ZR held, B for 100 ms every 300 ms, until at rest (ZR then let go throws nothing); 1 s -> Held anyway
     Held      the aim hand's trigger held: ZR held (raised, aiming); let go: thrown along the aim's line, frozen then -> Thrown;
               the grip let go -> Away
     Thrown    flown once 0x100000 goes; back when flown and at rest, not flown after 1.5 s, or 8 s: Held if gripped, else Away
     Away      lowered first if raised; then the game's put-away (oot_ih_away), B for 100 ms if that gave up; over when 0x100000
               goes or after 2.5 s
   The aim (option "boomerang_throw"): 0 the holding hand's trigger and pointing line, 1 the other hand's, 2 the holding hand's
   trigger and the headset's line. The game flies it from his hand toward a point straight ahead of its own camera, bending
   toward that camera's line; oot_boom_target puts that point 60 m out along the line from the hand through the aim's mark 10 m
   out, so the bend is nearly nil and it flies through the mark (HookshotVR, measured: within 0.07 m over 10 m). */
#pragma once
#include "oot_vr_handitem.h"

enum { OOT_BOOM_IDLE, OOT_BOOM_TAKING, OOT_BOOM_LOWERING, OOT_BOOM_HELD, OOT_BOOM_THROWN, OOT_BOOM_AWAY };
static const uint64_t OOT_BOOM_MS = 1000000ull;
static inline int oot_boom_in_hand(uint32_t s) { return (s & 0x100000u) != 0; }
static inline int oot_boom_raised(uint32_t s) { return (s & 0x80000u) != 0; }
static inline int oot_boom_at_rest(uint32_t s) { return (s & 0x100400u) == 0x100400u && !(s & 0x80000u); }

typedef struct {
    int phase, hand, aiming, flown, lowered, b_fallback;
    uint64_t since, b_at, aim_at, thrown_at;
    oot_ih_press grip[2], trigger[2];
    int have_line; float line_o[3], line_d[3], start[3]; int have_start;
    oot_ih_away away;
} oot_boom_state;
enum { OOT_BOOM_EV_TOOK = 1, OOT_BOOM_EV_THROWN, OOT_BOOM_EV_BACK, OOT_BOOM_EV_AWAY, OOT_BOOM_EV_GAVE_UP };
typedef struct {
    oot_ih_buttons b;
    int holding;          /* the holding hand + 1; -2 while it is being put away; 0 none */
    int put_away, event;
    int aiming, aim_hand; /* the reticle: shown while aiming, along aim_hand's line (-1: the headset's) */
} oot_boom_out;

static inline void oot_boom_init(oot_boom_state *s) { memset(s, 0, sizeof *s); s->hand = -1; }
static inline int oot_boom_aim_hand(const oot_boom_state *s, int mode) { return mode == 1 ? 1 - s->hand : s->hand; }
/* The line the aim follows now. 0: none. */
static inline int oot_boom_line(const oot_ih_frame *f, int hand, int mode, float o[3], float d[3]) {
    if (mode == 2) {
        if (!f->have_head) return 0;
        memcpy(o, f->eye, 12); memcpy(d, f->fwd, 12);
    } else {
        const oot_ih_hand *h = &f->hand[hand];
        if (!oot_ih_hand_ok(h)) return 0;
        memcpy(d, h->dir, 12);
        for (int i = 0; i < 3; i++) o[i] = h->pos[i] + 0.05f * h->dir[i];   /* it starts 5 cm ahead of the hand */
    }
    return oot_ih_finite(o, 3) && oot_ih_unit(d);
}
/* The point the game's flight is told to head for: 60 m out along the line from where it leaves (start) through the mark 10 m
   along the aim's line; when the mark is within 10 cm of the start, 60 m along the aim's line itself. */
static inline void oot_boom_target(const float o[3], const float d[3], const float start[3], float out[3]) {
    float mark[3], v[3];
    for (int i = 0; i < 3; i++) { mark[i] = o[i] + 10.0f * d[i]; v[i] = mark[i] - start[i]; }
    const float n = sqrtf(oot_ih_dot(v, v));
    if (!(n >= 0.1f)) { for (int i = 0; i < 3; i++) out[i] = o[i] + 60.0f * d[i]; return; }
    for (int i = 0; i < 3; i++) out[i] = start[i] + 60.0f * v[i] / n;
}
static inline void oot_boom_pulse_b(oot_boom_state *s, uint64_t now, oot_boom_out *o) {
    if (!s->b_at || now - s->b_at >= 300 * OOT_BOOM_MS) s->b_at = now;
    if (now - s->b_at < 100 * OOT_BOOM_MS) o->b.add |= OOT_IH_B;
}
static inline void oot_boom_go(oot_boom_state *s, int phase, uint64_t now) { s->phase = phase; s->since = now; s->b_at = 0; }
/* Into Away: lowered first when it is up, or when the trigger was just pressed to aim and it is not yet at rest. */
static inline void oot_boom_away(oot_boom_state *s, uint64_t now, uint32_t st, oot_boom_out *o) {
    s->lowered = !(oot_boom_raised(st) || (s->aiming && now - s->aim_at < 1000 * OOT_BOOM_MS && !oot_boom_at_rest(st)));
    oot_boom_go(s, OOT_BOOM_AWAY, now); s->hand = -1; s->aiming = 0; s->b_fallback = 0;
    oot_ih_away_cancel(&s->away);
    if (s->lowered) oot_ih_away_ask(&s->away);
    o->event = OOT_BOOM_EV_AWAY;
}

static inline void oot_boom_step(oot_boom_state *s, const oot_ih_in *in, int active, int mode, oot_boom_out *o) {
    memset(o, 0, sizeof *o); o->aim_hand = -1;
    const uint64_t now = in->now; const uint32_t st = in->action;
    if (mode < 0 || mode > 2) mode = 0;
    if (!active || !in->hands) {
        if (!active && s->phase != OOT_BOOM_IDLE && s->phase != OOT_BOOM_AWAY) { oot_boom_go(s, OOT_BOOM_IDLE, now); s->hand = -1; }
        for (int i = 0; i < 2; i++) s->grip[i].closed = s->trigger[i].closed = 0;
        if (s->phase == OOT_BOOM_AWAY) o->put_away = oot_ih_away_step(&s->away, now, oot_boom_in_hand(st));
        return;
    }
    const oot_ih_frame *f = in->hands;
    int closing[2] = { 0, 0 }, trig_was[2];
    for (int i = 0; i < 2; i++) {
        const int ok = oot_ih_hand_ok(&f->hand[i]);
        closing[i] = ok && oot_ih_press_step(&s->grip[i], f->hand[i].grip, 0.55f, 0.35f);
        trig_was[i] = s->trigger[i].closed;
        (void)oot_ih_press_step(&s->trigger[i], ok ? f->hand[i].trigger : 0, 0.55f, 0.35f);
    }
    o->b.mask |= OOT_IH_ZR;   /* the real ZR is the boomerang's while it is selected (HookshotVR) */
    switch (s->phase) {
    case OOT_BOOM_IDLE:
        for (int i = 1; i >= 0; i--) {
            if (!closing[i] || !oot_ih_free_zone(&f->hand[i], i == 1)) continue;
            s->hand = i; s->aiming = 0; oot_ih_away_cancel(&s->away);
            oot_boom_go(s, oot_boom_at_rest(st) ? OOT_BOOM_HELD : OOT_BOOM_TAKING, now);
            o->event = OOT_BOOM_EV_TOOK;
            if (s->phase == OOT_BOOM_TAKING) o->b.add |= OOT_IH_ZR;
            break;
        }
        break;
    case OOT_BOOM_TAKING:
        o->b.add |= OOT_IH_ZR;
        if (oot_boom_at_rest(st) && now - s->since > 100 * OOT_BOOM_MS) oot_boom_go(s, OOT_BOOM_HELD, now);
        else if (oot_boom_raised(st)) oot_boom_go(s, OOT_BOOM_LOWERING, now);
        else if (now - s->since >= 700 * OOT_BOOM_MS) { oot_boom_go(s, OOT_BOOM_IDLE, now); s->hand = -1; }
        break;
    case OOT_BOOM_LOWERING:
        if (oot_boom_at_rest(st) || now - s->since >= 1000 * OOT_BOOM_MS) { oot_boom_go(s, OOT_BOOM_HELD, now); break; }
        o->b.add |= OOT_IH_ZR; oot_boom_pulse_b(s, now, o);
        break;
    case OOT_BOOM_HELD: {
        const int aim = oot_boom_aim_hand(s, mode);
        if (!s->grip[s->hand].closed) { oot_boom_away(s, now, st, o); break; }
        if (s->trigger[aim].closed) { if (!s->aiming) s->aim_at = now; s->aiming = 1; o->b.add |= OOT_IH_ZR; }
        else if (s->aiming && trig_was[aim]) {   /* let go: thrown along the line as it is now */
            s->aiming = 0;
            s->have_line = oot_boom_line(f, aim, mode, s->line_o, s->line_d);
            s->have_start = oot_ih_hand_ok(&f->hand[s->hand]);
            if (s->have_start) memcpy(s->start, f->hand[s->hand].pos, 12);
            s->flown = 0; s->thrown_at = now; oot_boom_go(s, OOT_BOOM_THROWN, now);
            o->event = OOT_BOOM_EV_THROWN;
        } else s->aiming = 0;
        break; }
    case OOT_BOOM_THROWN: {
        if (!oot_boom_in_hand(st)) s->flown = 1;
        const uint64_t t = now - s->thrown_at;
        if ((s->flown && oot_boom_at_rest(st)) || (!s->flown && t > 1500 * OOT_BOOM_MS) || t > 8000 * OOT_BOOM_MS) {
            if (s->grip[s->hand].closed) { oot_boom_go(s, OOT_BOOM_HELD, now); o->event = OOT_BOOM_EV_BACK; } else oot_boom_away(s, now, st, o);
        }
        break; }
    default: break;
    }
    if (s->phase == OOT_BOOM_AWAY) {
        if (!s->lowered) {
            if (oot_boom_at_rest(st) || now - s->since >= 1000 * OOT_BOOM_MS) { s->lowered = 1; oot_ih_away_ask(&s->away); }
            else { o->b.add |= OOT_IH_ZR; oot_boom_pulse_b(s, now, o); }
        }
        if (s->lowered) {
            o->put_away = oot_ih_away_step(&s->away, now, oot_boom_in_hand(st));
            if (s->away.failed) { s->away.failed = 0; s->b_fallback = 1; s->b_at = now; o->event = OOT_BOOM_EV_GAVE_UP; }
            if (s->b_fallback && now - s->b_at < 100 * OOT_BOOM_MS) o->b.add |= OOT_IH_B;   /* the only item here with a B fallback */
        }
        if (!oot_boom_in_hand(st) || now - s->since >= 2500 * OOT_BOOM_MS) { oot_boom_go(s, OOT_BOOM_IDLE, now); s->b_fallback = 0; oot_ih_away_cancel(&s->away); }
        else o->holding = -2;
    } else if (s->hand >= 0) {
        const int aim = oot_boom_aim_hand(s, mode);
        o->b.triggers |= 1 << s->hand | 1 << aim;
        o->holding = s->phase != OOT_BOOM_THROWN || (!s->flown && now - s->thrown_at < 1500 * OOT_BOOM_MS) ? s->hand + 1 : 0;
        o->aiming = s->phase == OOT_BOOM_HELD && s->aiming;
        o->aim_hand = mode == 2 ? -1 : aim;
    }
}
