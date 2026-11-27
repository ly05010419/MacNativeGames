/* Ocarina of Time: the bow and the slingshot taken with a grip and held, without the runtime (tests/test_oot_vr_bowgrip.c runs it).
   The behaviour follows HookshotVR's notes on its bow controls (GPL-3: facts and ideas, no code; aim-items.md 0.5, 1.6):
     idle         the bow already out (the player's own ZR): a grip on a hand the sword, the shield or a carried thing does not
                  claim takes it (holding). Not out, the item button holding the bow or the slingshot, no wheel open: such a
                  grip takes it out (the game's ZR held until it is out, at most 0.7 s).
     holding      the game's ZR is held only while the string's draw is committed (oot_vr_string.h), so the game aims while the
                  player draws and the string's let-go is the game's shot. The grip let go with nothing drawn puts it away by
                  the game's own sheathe-all (asked once; the game side tries again). The game putting it away by itself: idle.
     putting away the grip taking it again while it is still out withdraws the ask; if it is gone just as the grip took it again,
                  it is taken out once more.
   While the bow owns things the player's own ZR is not the game's, and the bow hand's grip is not a gesture. A solution that
   drops out for less than 1.5 s keeps its last answer, so a dropout is never a let-go.
   Without the grip option (or in third person) only the string fires: while the bow is out and the solution is there, the game's
   ZR follows the commit; the press of the player's own ZR that brought it out passes until it is let go. With no solution at all
   (an App without the bow's model, the hands not tracked) the player's ZR is the game's as before. Grips: pressed above
   0.55, let go below 0.35. Times in nanoseconds. */
#pragma once
#include <stdint.h>
#include <string.h>

enum { OOT_BOWGRIP_IDLE, OOT_BOWGRIP_TAKING_OUT, OOT_BOWGRIP_HOLDING, OOT_BOWGRIP_PUTTING_AWAY };
static const uint64_t OOT_BOWGRIP_TAKE_NS = 700000000ull, OOT_BOWGRIP_GONE_NS = 150000000ull, OOT_BOWGRIP_KEEP_NS = 1500000000ull,
                      OOT_BOWGRIP_AWAY_NS = 2000000000ull;

typedef struct {
    uint64_t now;
    float grip[2];        /* each controller's grip */
    int first_person, grip_hold;   /* the view is at his eyes; the VR page's "a grip takes the bow" */
    int bow_selected;     /* the item button holds the bow or the slingshot */
    int bow_out;          /* the game has it out in Link's hand */
    int claimed[2];       /* that hand is the sword's, the shield's, carries something, or grips behind the shoulder */
    int wheel_open;       /* the item wheel or the game's quick select is open */
    int fresh;            /* the string's solution is there this frame */
    int held, committed;  /* its flags */
    int real_zr;          /* the ZR of the player's own buttons */
} oot_bowgrip_in;

typedef struct {
    int zr;               /* the game's ZR */
    int put_away;         /* ask the game's sheathe-all */
    int cancel_put_away;  /* withdraw that ask */
    int bow_hand;         /* the hand that holds it, -1 none */
    int owns[2];          /* that hand's grip is the bow's, not a gesture */
    const char *said;     /* what changed, for the log (NULL nothing) */
} oot_bowgrip_out;

typedef struct {
    int state, bow_hand, down[2], passing, answer;
    uint64_t since, answer_at, gone_since;
} oot_bowgrip;

static inline void oot_bowgrip_step(oot_bowgrip *s, const oot_bowgrip_in *in, oot_bowgrip_out *o) {
    memset(o, 0, sizeof *o); o->bow_hand = -1;
    int fresh_grip[2];
    for (int h = 0; h < 2; h++) {
        const int was = s->down[h];
        if (in->grip[h] > 0.55f) s->down[h] = 1; else if (in->grip[h] < 0.35f) s->down[h] = 0;
        fresh_grip[h] = s->down[h] && !was;
    }
    if (in->fresh) { s->answer = in->committed; s->answer_at = in->now; }
    else if (s->answer_at && in->now - s->answer_at >= OOT_BOWGRIP_KEEP_NS) s->answer = 0;
    const int live = s->answer_at && in->now - s->answer_at < OOT_BOWGRIP_KEEP_NS, answer = live && s->answer;   /* live: the string's solution is (or was within 1.5 s) there */
    if (in->bow_out) s->gone_since = 0; else if (!s->gone_since) s->gone_since = in->now;
    const int gone = !in->bow_out && in->now - s->gone_since >= OOT_BOWGRIP_GONE_NS;
    const int grip_mode = in->first_person && in->grip_hold;
    if (!grip_mode && s->state != OOT_BOWGRIP_IDLE) { s->state = OOT_BOWGRIP_IDLE; o->said = "no grip mode (third person or the option off): the bow is the buttons' again"; }
    if (!in->real_zr) s->passing = 0;
    switch (s->state) {
    case OOT_BOWGRIP_IDLE: {
        if (!grip_mode) {   /* only the string fires */
            if (!in->bow_out && in->real_zr) s->passing = 1;
            o->zr = in->bow_out && live && !s->passing ? answer : in->real_zr;
            break;
        }
        o->zr = in->real_zr;
        int hand = -1;
        for (int h = 0; h < 2; h++) if (fresh_grip[h] && !in->claimed[h] && hand < 0) hand = h;
        if (hand < 0) break;
        if (in->bow_out) { s->state = OOT_BOWGRIP_HOLDING; s->bow_hand = hand; o->said = "the grip takes the bow that is out"; o->zr = 0; }
        else if (in->bow_selected && !in->wheel_open) { s->state = OOT_BOWGRIP_TAKING_OUT; s->bow_hand = hand; s->since = in->now; o->zr = 1; o->said = "the grip takes the bow out (the game's ZR)"; }
        break; }
    case OOT_BOWGRIP_TAKING_OUT:
        if (in->bow_out) { s->state = OOT_BOWGRIP_HOLDING; o->said = "the bow is out: held"; }
        else if (in->now - s->since > OOT_BOWGRIP_TAKE_NS) { s->state = OOT_BOWGRIP_IDLE; o->said = "nothing came out in 0.7 s: the grip lets it be"; }
        else o->zr = 1;
        break;
    case OOT_BOWGRIP_HOLDING:
        if (gone) { s->state = OOT_BOWGRIP_IDLE; o->said = "the game put the bow away"; break; }
        if (!s->down[s->bow_hand] && !in->held && !in->committed) { s->state = OOT_BOWGRIP_PUTTING_AWAY; s->since = in->now; o->put_away = 1; o->said = "the grip let go: put away by the game's sheathe-all"; break; }
        o->zr = live ? answer : in->real_zr;   /* no string at all (no model of it, no hands): the buttons fire as before */
        break;
    case OOT_BOWGRIP_PUTTING_AWAY:
        if (!in->bow_out) {
            if (s->down[s->bow_hand]) { s->state = OOT_BOWGRIP_TAKING_OUT; s->since = in->now; o->zr = 1; o->said = "gone just as the grip took it again: taken out again"; }
            else if (gone) { s->state = OOT_BOWGRIP_IDLE; o->said = "put away"; }
            break;
        }
        if (fresh_grip[s->bow_hand]) { s->state = OOT_BOWGRIP_HOLDING; o->cancel_put_away = 1; o->said = "the grip took it again: the put-away withdrawn"; break; }
        if (in->now - s->since > OOT_BOWGRIP_AWAY_NS) { s->state = OOT_BOWGRIP_HOLDING; o->said = "still out 2 s after the put-away: held again"; }
        break;
    }
    if (s->state != OOT_BOWGRIP_IDLE) {
        o->bow_hand = s->bow_hand; o->owns[s->bow_hand] = 1;
        if (in->held) o->owns[1 - s->bow_hand] = 1;
    }
}
