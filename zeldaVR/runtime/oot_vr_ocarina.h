/* Ocarina of Time in VR: the ocarina in the player's hands (vr.md rows 69, 70), without the runtime (tests/test_oot_vr_ocarina.c
   runs it). The game's side, as HookshotVR's notes give it (held-items.md 7.1-7.2; vr/items/ocarina, GPL-3: facts and the idea
   only, no code): the game's D-pad Left starts its ocarina (event Dm_SY_Ocarina) and it then waits for notes; the notes are its L,
   R, A, Y and X, D-pad Up / Down bend a note a half step; B ends a field ocarina (wait kinds 1 and 3); ZL held opens its song
   list. Ours:
     - the ocarina hangs at the belt, a little left of the middle; a hand closing on it there takes it, opening puts it back;
     - brought to the mouth (within 17 cm of a point 8 cm below and 9 cm ahead of the eyes, away again past 27 cm) it starts the
       game's ocarina once (D-pad Left for 100 ms), unless a scene plays or the game already waits; if the game is not waiting
       within 1.5 s it gives up (Link swims, is in the air, is busy) until the ocarina has left the mouth;
     - at the mouth while the game waits: the left trigger is L, the right trigger R, the right stick up X, left Y, right A (on
       past 0.6, off under 0.4), the left controller's upper button D-pad Up and its lower one D-pad Down; the other hand's grip
       held is ZL, the song list; the face buttons, the triggers and the right stick are then the ocarina's;
     - let go, or away from the mouth for 250 ms while the game waits on the field: B once.
   Hands: p in the head's frame (x right, y up, z back), lvl in the head's level frame (f ahead, r right, u up), metres. The
   switch buttons are oot_vr_pad.h's. Times in nanoseconds. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "oot_vr_pad.h"

static const float OOT_OC_BELT[3] = { 0.08f, -0.14f, -0.45f };     /* f, r, u in the level frame: the belt, left of the middle */
static const float OOT_OC_MOUTH[3] = { 0.0f, -0.08f, -0.09f };      /* x, y, z in the head's frame */
enum { OOT_OC_TAKE_CM = 15, OOT_OC_MOUTH_CM = 17, OOT_OC_AWAY_CM = 27 };
static const uint64_t OOT_OC_PULSE = 100000000ull, OOT_OC_WAIT_FOR = 1500000000ull, OOT_OC_AWAY_FOR = 250000000ull, OOT_OC_QUIET = 300000000ull;

typedef struct { int have; float p[3], lvl[3], grip; } oot_oc_hand;
typedef struct {
    oot_oc_hand hand[2];       /* 0 left, 1 right */
    float ltrig, rtrig, rx, ry;
    int upper, lower;          /* the left controller's upper (Y) and lower (X) buttons */
    int owned;                 /* Link has an ocarina */
    int waiting, wait_kind;    /* the game waits for notes (fresh), and the kind of wait */
    int event;                 /* a scene of the game plays */
    int busy[2];               /* the hand holds something else (the sword, a carried thing) */
    int song_menu;             /* the option: the other hand's grip is the game's song list */
} oot_oc_in;
enum { OOT_OC_SAY_NONE, OOT_OC_SAY_TAKEN, OOT_OC_SAY_BACK, OOT_OC_SAY_MOUTH, OOT_OC_SAY_START, OOT_OC_SAY_WAITS, OOT_OC_SAY_GAVE_UP,
       OOT_OC_SAY_CANCEL, OOT_OC_SAY_AWAY, OOT_OC_SAY_DONE };
typedef struct {
    uint64_t add, clear;       /* switch buttons to press, and to take from what the pad made */
    int take_stick;            /* the right stick is the ocarina's: neither the game's stick nor a turn */
    int buzz[2];
    int say;                   /* the one thing that happened, for the log */
} oot_oc_out;
typedef struct {
    int ready, held, closed[2], at_mouth, armed, started, was_waiting, notes[3], menu;
    uint64_t start_at, away_at, pulse_until, quiet_until;
    uint64_t pulse_bits;
} oot_oc_state;

static inline float oot_oc_dist(const float a[3], const float b[3]) {
    const float d[3] = { a[0] - b[0], a[1] - b[1], a[2] - b[2] };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
static inline void oot_oc_pulse(oot_oc_state *s, uint64_t now, uint64_t bits) { s->pulse_bits = bits; s->pulse_until = now + OOT_OC_PULSE; }
static inline int oot_oc_field(int kind) { return kind == 1 || kind == 3; }
/* The waits whose notes are the player's buttons (1, 3 the field, 2 a lesson, 7 the Skull Kids, 10 / 11 the scarecrow); 13, a dialogue's, stays the game's. */
static inline int oot_oc_buttons(int kind) { return kind == 1 || kind == 2 || kind == 3 || kind == 7 || kind == 10 || kind == 11; }

static inline void oot_ocarina_step(oot_oc_state *s, uint64_t now, const oot_oc_in *in, oot_oc_out *out) {
    const uint64_t B = 1;
    memset(out, 0, sizeof *out);
    if (!s->ready) { s->ready = 1; s->held = -1; s->armed = 1; }
    int closing[2] = { 0, 0 }, opening[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        const int was = s->closed[i];
        if (!in->hand[i].have) continue;
        if (in->hand[i].grip > 0.55f) s->closed[i] = 1; else if (in->hand[i].grip < 0.35f) s->closed[i] = 0;
        closing[i] = s->closed[i] && !was; opening[i] = !s->closed[i] && was;
    }
    const int waiting = in->waiting && in->owned && oot_oc_buttons(in->wait_kind);
    if (s->held < 0) {   /* on the belt: a free hand closing on it takes it */
        for (int i = 1; i >= 0 && in->owned; i--) {
            if (!closing[i] || in->busy[i] || oot_oc_dist(in->hand[i].lvl, OOT_OC_BELT) > OOT_OC_TAKE_CM / 100.0f) continue;
            s->held = i; s->at_mouth = 0; s->armed = 1; out->buzz[i] = 1; out->say = OOT_OC_SAY_TAKEN; s->quiet_until = now + OOT_OC_QUIET;
            break;
        }
        if (now < s->quiet_until) out->clear |= B << OOT_SW_A;   /* the hand closing low is no pick-up */
        if (now < s->pulse_until) out->add |= s->pulse_bits;
        return;
    }
    const int h = s->held;
    if (!in->owned || !in->hand[h].have || opening[h]) {   /* let go: back on the belt; the game's field ocarina ends */
        if (waiting && oot_oc_field(in->wait_kind)) { oot_oc_pulse(s, now, B << OOT_SW_B); out->say = OOT_OC_SAY_CANCEL; }
        else out->say = OOT_OC_SAY_BACK;
        s->held = -1; s->at_mouth = 0; s->armed = 1; s->started = 0; s->was_waiting = 0; s->menu = 0; s->quiet_until = now + OOT_OC_QUIET;
        memset(s->notes, 0, sizeof s->notes);
        out->clear |= B << OOT_SW_A;
        if (now < s->pulse_until) out->add |= s->pulse_bits;
        return;
    }
    const float mouth = oot_oc_dist(in->hand[h].p, OOT_OC_MOUTH);
    if (!s->at_mouth && mouth <= OOT_OC_MOUTH_CM / 100.0f) { s->at_mouth = 1; s->away_at = 0; if (!out->say) out->say = OOT_OC_SAY_MOUTH; }
    else if (s->at_mouth && mouth > OOT_OC_AWAY_CM / 100.0f) { s->at_mouth = 0; s->away_at = now; if (!out->say) out->say = OOT_OC_SAY_AWAY; }
    if (s->at_mouth && s->armed && !waiting && !in->event) {   /* start the game's ocarina */
        s->armed = 0; s->started = 1; s->start_at = now; oot_oc_pulse(s, now, B << OOT_SW_LEFT); out->say = OOT_OC_SAY_START; out->buzz[h] = 1;
    }
    if (s->started && !s->was_waiting && !waiting && now - s->start_at > OOT_OC_WAIT_FOR) { s->started = 0; out->say = OOT_OC_SAY_GAVE_UP; }
    if (waiting && s->at_mouth && !s->was_waiting) { s->was_waiting = 1; out->say = OOT_OC_SAY_WAITS; }
    if (s->was_waiting && !waiting && !s->menu && !(in->song_menu && s->at_mouth && in->hand[1 - h].have && in->hand[1 - h].grip > 0.55f)) {
        s->was_waiting = 0; s->started = 0; if (!out->say) out->say = OOT_OC_SAY_DONE;   /* a song played, or the game ended it */
    }
    if (!s->at_mouth && !waiting && !s->started) s->armed = 1;   /* away with no wait: ready for the next */
    if (!s->at_mouth && waiting && oot_oc_field(in->wait_kind) && s->away_at && now - s->away_at >= OOT_OC_AWAY_FOR) {
        oot_oc_pulse(s, now, B << OOT_SW_B); s->away_at = 0; out->say = OOT_OC_SAY_CANCEL;
    }
    if (s->at_mouth && (waiting || s->menu)) {   /* the notes */
        out->take_stick = 1;
        out->clear |= B << OOT_SW_ZL | B << OOT_SW_ZR | B << OOT_SW_X | B << OOT_SW_Y | B << OOT_SW_L | B << OOT_SW_R;
        if (in->ltrig > 0.55f) out->add |= B << OOT_SW_L;
        if (in->rtrig > 0.55f) out->add |= B << OOT_SW_R;
        const float push[3] = { in->ry, -in->rx, in->rx };   /* up X, left Y, right A */
        const int bit[3] = { OOT_SW_X, OOT_SW_Y, OOT_SW_A };
        for (int k = 0; k < 3; k++) {
            if (s->notes[k] ? push[k] < 0.4f : push[k] > 0.6f) s->notes[k] = !s->notes[k];
            if (s->notes[k]) out->add |= B << bit[k];
        }
        if (in->upper) out->add |= B << OOT_SW_UP;
        if (in->lower) out->add |= B << OOT_SW_DOWN;
        s->menu = in->song_menu && in->hand[1 - h].have && in->hand[1 - h].grip > 0.55f;
        if (s->menu) out->add |= B << OOT_SW_ZL;
    } else { memset(s->notes, 0, sizeof s->notes); s->menu = 0; }
    if (now < s->pulse_until) out->add |= s->pulse_bits;
}
/* Where the ocarina is drawn: in the hand that holds it, or at the belt (in the level frame), for the picture. */
static inline int oot_ocarina_held(const oot_oc_state *s) { return s->held; }
