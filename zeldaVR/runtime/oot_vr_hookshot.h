/* Ocarina of Time in VR: the hookshot and longshot in the player's hand (vr.md row 63), without the runtime
   (tests/test_oot_vr_hookshot.c runs it). HookshotVR's way (aim-items.md 3.3-3.4; GPL-3, ideas and the game's values only):
     - a fresh grip (in no gesture's zone, oot_vr_handitem.h) takes it in that hand; if it was not seen out in the last 300 ms the
       game's ZR is held until it is seen out (after the grip) or 700 ms pass, then let go: it comes out and rests, no shot;
     - that hand's trigger is the game's ZR (held aims, let go fires); the real ZR and that trigger's own button are not sent;
     - a shot is the trigger let go while the action state has 0x200000; it lasts until the state is 0x480 or 0, or has 0x80000000,
       or 6 s; a grip let go during a shot keeps the hookshot in the hand until the shot is over;
     - let go otherwise, the game's own put-away (oot_ih_away) while it reads as out.
   Out: bit 31 clear and (0x400 set or the low byte 0x80). Where it aims is the game side's (games/oot/runtime/oot_vr_hand_items.h). */
#pragma once
#include "oot_vr_handitem.h"

static const uint64_t OOT_HOOK_FRESH = 300000000ull, OOT_HOOK_TAKE = 700000000ull, OOT_HOOK_SHOT_MAX = 6000000000ull;
static inline int oot_hook_out(uint32_t s) { return !(s & 0x80000000u) && ((s & 0x400u) || (s & 0xffu) == 0x80u); }

typedef struct {
    int hand, taking, keep;          /* keep: let go during a shot, kept until it ends */
    uint64_t take_at, out_seen, shot_at;
    oot_ih_press grip[2], trigger;
    oot_ih_away away;
} oot_hook_state;
enum { OOT_HOOK_TOOK = 1, OOT_HOOK_SHOT, OOT_HOOK_LET_GO, OOT_HOOK_GAVE_UP };
typedef struct { oot_ih_buttons b; int holding; int put_away; int event; } oot_hook_out_t;

static inline void oot_hook_release(oot_hook_state *s, int out) {
    s->hand = -1; s->taking = 0; s->keep = 0; s->shot_at = 0; s->trigger.closed = 0;
    if (out) oot_ih_away_ask(&s->away);
}
/* active: the hookshot or longshot on the item button, the option on, first person, no scene of the game, hands fresh. */
static inline void oot_hook_step(oot_hook_state *s, const oot_ih_in *in, int active, oot_hook_out_t *o) {
    memset(o, 0, sizeof *o);
    const uint64_t now = in->now; const uint32_t st = in->action; const int out = oot_hook_out(st);
    if (out) s->out_seen = now;
    if (!active || !in->hands) {
        if (s->hand >= 0 && !active) s->hand = -1;
        s->taking = s->keep = 0; s->shot_at = 0;
        for (int i = 0; i < 2; i++) s->grip[i].closed = 0;
        o->put_away = oot_ih_away_step(&s->away, now, out);
        return;
    }
    int closing[2] = { 0, 0 };
    for (int i = 0; i < 2; i++) {
        const oot_ih_hand *h = &in->hands->hand[i];
        closing[i] = oot_ih_hand_ok(h) && oot_ih_press_step(&s->grip[i], h->grip, 0.55f, 0.35f);
    }
    if (s->hand < 0) {
        for (int i = 1; i >= 0; i--) {
            if (!closing[i] || !oot_ih_free_zone(&in->hands->hand[i], i == 1)) continue;
            s->hand = i; s->keep = 0; s->shot_at = 0; s->trigger.closed = 0;
            oot_ih_away_cancel(&s->away);
            s->taking = !s->out_seen || now - s->out_seen > OOT_HOOK_FRESH;
            s->take_at = now;
            o->event = OOT_HOOK_TOOK;
            break;
        }
    }
    if (s->hand >= 0) {
        const oot_ih_hand *h = &in->hands->hand[s->hand];
        const int was = s->trigger.closed;
        (void)oot_ih_press_step(&s->trigger, oot_ih_hand_ok(h) ? h->trigger : 0, 0.55f, 0.35f);
        if (s->taking && ((out && s->out_seen > s->take_at) || now - s->take_at >= OOT_HOOK_TAKE)) s->taking = 0;
        if (s->taking) o->b.add |= OOT_IH_ZR;
        else {
            if (s->trigger.closed) o->b.add |= OOT_IH_ZR;
            if (was && !s->trigger.closed && (st & 0x200000u)) { s->shot_at = now; o->event = OOT_HOOK_SHOT; }
        }
        if (s->shot_at && (st == 0x480u || st == 0 || (st & 0x80000000u) || now - s->shot_at > OOT_HOOK_SHOT_MAX)) s->shot_at = 0;
        const int hand = s->hand;
        if (!s->grip[s->hand].closed) { if (s->shot_at) s->keep = 1; else { oot_hook_release(s, out); o->event = OOT_HOOK_LET_GO; } }
        else if (s->keep) s->keep = 0;   /* gripped again during the shot */
        if (s->hand >= 0 && s->keep && !s->shot_at) { oot_hook_release(s, out); o->event = OOT_HOOK_LET_GO; }
        if (s->hand >= 0) { o->b.mask |= OOT_IH_ZR; o->b.triggers |= 1 << hand; }
        else o->b.add &= ~OOT_IH_ZR;
    }
    o->holding = s->hand >= 0 && out ? s->hand + 1 : 0;
    o->put_away = oot_ih_away_step(&s->away, now, out);
    if (s->away.failed) { s->away.failed = 0; o->event = OOT_HOOK_GAVE_UP; }
}
static inline void oot_hook_init(oot_hook_state *s) { memset(s, 0, sizeof *s); s->hand = -1; }
