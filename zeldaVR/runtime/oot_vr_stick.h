/* Ocarina of Time in VR: the Deku Stick taken by a grip (vr.md row 68), without the runtime (tests/test_oot_vr_stick.c runs it).
   HookshotVR's way (held-items.md 4.1-4.2; GPL-3, ideas and the game's values only):
     - with the stick on the item button (the game's item 0), a fresh grip (0.55 on, 0.35 off) in a hand outside the gestures'
       zones, and not while the sword is out, takes it: the game's own ZR is held for it until the stick reads out (the game's
       own take: ZR -> action 0 -> 0x8 -> 0x408; batch4, 2026-11-27, took it out with the trigger); not out after 700 ms: none
       left, not held again until a new grip. HookshotVR asks the game's per-slot draw of slot 1 (main+0x6b6820) in its blade
       mode and holds ZR otherwise; on our image that call did not bring the stick out (batch11, 2026-11-27: asked twice, slot 1
       stayed "the stick (drawn 0)" for 700 ms each time), why is not known yet (the call's early returns: main+0x6b6974 for an
       actor in states 7-11, main+0x6b6988 when main+0xce7ab0 answers 1), so the game's own button is used;
     - the stick in the hand swings by the blade logic as a weapon (games/oot/runtime/oot_vr_sword.h, its kind 101);
     - let go: the game's put-away (main+0x4a8690), again every 300 ms while it reads out, at most four times;
     - the real ZR does not reach the game while the stick is selected, and the holding hand's trigger is the stick's.
   Lit: the game's own query (main+0x8d7df0, read on our image 2026-11-27) walks the stick's chemistry objects; an object counts
   when its +0x268 byte has bit 2, its +0x44 half-word has neither 0x4 nor 0x10 and its state byte +0x35 is not negative; state 1
   is burning (2 burnt, 3 exploded). For the log only. */
#pragma once
#include "oot_vr_handitem.h"

enum { OOT_STICK_ITEM = 0 };
enum { OOT_STICK_IDLE, OOT_STICK_HELD };
enum { OOT_STICK_EV_TOOK = 1, OOT_STICK_EV_OUT, OOT_STICK_EV_LET_GO, OOT_STICK_EV_NONE_LEFT };
static const uint64_t OOT_STICK_DRAW_FOR = 700000000ull;
typedef struct {
    int phase, hand, came_out, gave_up;
    uint64_t took_at;
    oot_ih_press grip[2];
    oot_ih_away away;
} oot_stick_state;
typedef struct {
    oot_ih_buttons b;
    int holding;     /* hand + 1 */
    int put_away;    /* the game's put-away now */
    int event;
} oot_stick_out;

static inline void oot_stick_init(oot_stick_state *s) { memset(s, 0, sizeof *s); s->hand = -1; }

/* active: the stick is on the item button, the option is on, first person, no scene. out: slot 1's actor is the stick and reads
   drawn. sword_out: slot 0's sword reads drawn. */
static inline void oot_stick_step(oot_stick_state *s, const oot_ih_in *in, int active, int out, int sword_out, oot_stick_out *o) {
    memset(o, 0, sizeof *o);
    const uint64_t now = in->now;
    int closing[2] = { 0, 0 };
    if (in->hands)   /* the grips are followed always: a grip already closed when the stick becomes active is not a fresh one */
        for (int i = 0; i < 2; i++) closing[i] = oot_ih_hand_ok(&in->hands->hand[i]) && oot_ih_press_step(&s->grip[i], in->hands->hand[i].grip, 0.55f, 0.35f);
    if (!active || !in->hands) {   /* another item, a scene: the game has the stick as it likes */
        s->phase = OOT_STICK_IDLE; s->hand = -1; oot_ih_away_cancel(&s->away);
        return;
    }
    o->b.mask |= OOT_IH_ZR;
    o->put_away = oot_ih_away_step(&s->away, now, out);
    if (s->phase == OOT_STICK_IDLE && !sword_out) {
        for (int i = 1; i >= 0; i--) {
            if (!closing[i] || !oot_ih_free_zone(&in->hands->hand[i], i == 1)) continue;
            s->phase = OOT_STICK_HELD; s->hand = i; s->took_at = now; s->came_out = out; s->gave_up = 0;
            oot_ih_away_cancel(&s->away); o->put_away = 0;
            o->event = OOT_STICK_EV_TOOK;
            break;
        }
    }
    if (s->phase != OOT_STICK_HELD) return;
    if (!s->grip[s->hand].closed) {   /* let go */
        s->phase = OOT_STICK_IDLE; s->hand = -1; o->event = OOT_STICK_EV_LET_GO;
        if (out) { oot_ih_away_ask(&s->away); o->put_away = oot_ih_away_step(&s->away, now, out); }
        return;
    }
    o->holding = s->hand + 1; o->b.triggers |= 1 << s->hand;
    if (out) {
        if (!s->came_out) { s->came_out = 1; if (!o->event) o->event = OOT_STICK_EV_OUT; }
    } else if (!s->came_out && !s->gave_up) {
        if (now - s->took_at > OOT_STICK_DRAW_FOR) { s->gave_up = 1; o->event = OOT_STICK_EV_NONE_LEFT; }
        else o->b.add |= OOT_IH_ZR;   /* the game's own take, until it reads out */
    }
}

/* One chemistry object of the stick as read: its +0x268 byte, +0x44 half-word and +0x35 state. */
typedef struct { uint8_t flags; uint16_t hidden; int8_t state; } oot_stick_chem;
/* 1 burning (wins), else the first counted object's state (2 burnt, 3 exploded, 0 nothing). */
static inline int oot_stick_lit_state(const oot_stick_chem *c, int n) {
    int first = 0;
    for (int i = 0; i < n; i++) {
        if (!(c[i].flags & 4) || (c[i].hidden & 0x14) || c[i].state < 0) continue;
        if (c[i].state == 1) return 1;
        if (!first) first = c[i].state;
    }
    return first;
}
