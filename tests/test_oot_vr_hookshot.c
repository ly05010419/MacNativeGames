/* The hookshot in the player's hand, and what the hand items share (zeldaVR/runtime/oot_vr_hookshot.h, oot_vr_handitem.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_hookshot.h"

#define MS 1000000ull
static oot_ih_frame frame;
static void hand(int i, float grip, float trigger) {
    oot_ih_hand *h = &frame.hand[i];
    h->have = 1; h->grip = grip; h->trigger = trigger;
    h->f = 0.3f; h->u = -0.3f; h->r = i ? 0.2f : -0.2f;   /* in front of the chest: no gesture's zone */
    h->dir[2] = -1; h->palm[0] = i ? -1.0f : 1.0f;
}
static oot_ih_in at(uint64_t t, uint32_t action) { oot_ih_in in = { t * MS, &frame, 5, action, 1, 0 }; return in; }

static void shared(void) {
    /* the item numbers */
    assert(oot_ih_kind(5) == OOT_IH_HOOKSHOT && oot_ih_kind(6) == OOT_IH_HOOKSHOT && oot_ih_kind(3) == OOT_IH_BOOMERANG);
    assert(oot_ih_kind(2) == OOT_IH_BOMBS && oot_ih_kind(9) == OOT_IH_NONE && oot_ih_kind(14) == OOT_IH_BOTTLE && oot_ih_kind(50) == OOT_IH_BOTTLE);
    assert(oot_ih_kind(18) == OOT_IH_NONE && oot_ih_kind(-1) == OOT_IH_NONE);
    /* hysteresis */
    oot_ih_press p = { 0 };
    assert(!oot_ih_press_step(&p, 0.5f, 0.55f, 0.35f) && oot_ih_press_step(&p, 0.6f, 0.55f, 0.35f) && !oot_ih_press_step(&p, 0.4f, 0.55f, 0.35f) && p.closed);
    assert(!oot_ih_press_step(&p, 0.3f, 0.55f, 0.35f) && !p.closed);
    /* the gestures' zones are not the items' */
    oot_ih_hand h = { 0 }; h.f = -0.1f; h.u = 0; h.r = 0.1f;
    assert(!oot_ih_free_zone(&h, 1) && oot_ih_free_zone(&h, 0));   /* behind the right shoulder: the sword's */
    h.f = 0.3f; h.u = -0.7f; assert(!oot_ih_free_zone(&h, 1));     /* low: picking up */
    h.u = 0; h.f = 0.6f; assert(!oot_ih_free_zone(&h, 1));         /* far ahead */
    h.f = 0.3f; h.u = -0.3f; assert(oot_ih_free_zone(&h, 1));
    /* the pointing line: OpenXR's grip -z; with no tilt, fingers -y, back of the right hand +x, of the left -x */
    const float along[3] = { 0, -1, 0 }, back_r[3] = { 1, 0, 0 }, back_l[3] = { -1, 0, 0 };
    float d[3];
    oot_ih_pointing(1, along, back_r, d); assert(fabsf(d[2] + 1) < 1e-6f && fabsf(d[0]) < 1e-6f && fabsf(d[1]) < 1e-6f);
    oot_ih_pointing(0, along, back_l, d); assert(fabsf(d[2] + 1) < 1e-6f);
    /* the put-away: four calls 300 ms apart, done when it is away, withdrawn by a grip */
    oot_ih_away a = { 0 };
    assert(!oot_ih_away_step(&a, 0, 1));
    oot_ih_away_ask(&a);
    assert(oot_ih_away_step(&a, 1000 * MS, 1) && !oot_ih_away_step(&a, 1100 * MS, 1) && oot_ih_away_step(&a, 1300 * MS, 1));
    assert(oot_ih_away_step(&a, 1600 * MS, 1) && oot_ih_away_step(&a, 1900 * MS, 1) && !oot_ih_away_step(&a, 2200 * MS, 1) && a.failed && !a.asked);
    a.failed = 0; oot_ih_away_ask(&a); assert(!oot_ih_away_step(&a, 3000 * MS, 0) && !a.asked);   /* already away */
    oot_ih_away_ask(&a); oot_ih_away_cancel(&a); assert(!oot_ih_away_step(&a, 3000 * MS, 1));
}

int main(void) {
    shared();
    oot_hook_state s; oot_hook_init(&s); oot_hook_out_t o;
    hand(0, 0, 0); hand(1, 0, 0);
    /* not active (another item, or third person): nothing */
    oot_ih_in in = at(0, 0);
    oot_hook_step(&s, &in, 0, &o); assert(!o.b.add && !o.b.mask && s.hand < 0);
    /* a grip behind the right shoulder is the sword's */
    frame.hand[1].f = -0.1f; frame.hand[1].u = 0; frame.hand[1].r = 0.1f; frame.hand[1].grip = 1;
    in = at(10, 0); oot_hook_step(&s, &in, 1, &o); assert(s.hand < 0 && !o.b.add);
    hand(1, 0, 0); in = at(20, 0); oot_hook_step(&s, &in, 1, &o);
    /* a grip in front takes it: not out, so ZR is held until it is seen out, then let go */
    hand(1, 1, 0); in = at(30, 0); oot_hook_step(&s, &in, 1, &o);
    assert(s.hand == 1 && o.event == OOT_HOOK_TOOK && (o.b.add & OOT_IH_ZR) && (o.b.mask & OOT_IH_ZR) && o.b.triggers == 2 && !o.holding);
    in = at(60, 0x80); oot_hook_step(&s, &in, 1, &o); assert(!(o.b.add & OOT_IH_ZR) && o.holding == 2);   /* seen out (0x80): ZR let go */
    in = at(90, 0x480); oot_hook_step(&s, &in, 1, &o); assert(!(o.b.add & OOT_IH_ZR) && o.holding == 2 && (o.b.mask & OOT_IH_ZR));
    /* the trigger aims (ZR held), let go fires */
    hand(1, 1, 0.9f); in = at(120, 0x600480); oot_hook_step(&s, &in, 1, &o); assert(o.b.add & OOT_IH_ZR);
    hand(1, 1, 0.1f); in = at(150, 0x600480); oot_hook_step(&s, &in, 1, &o); assert(!(o.b.add & OOT_IH_ZR) && o.event == OOT_HOOK_SHOT && s.shot_at);
    /* the grip let go during the shot: kept until the shot is over, then the game's put-away */
    hand(1, 0, 0); in = at(180, 0xe00480); oot_hook_step(&s, &in, 1, &o); assert(s.hand == 1 && s.keep && !o.put_away);
    in = at(400, 0x20000480); oot_hook_step(&s, &in, 1, &o); assert(s.hand == 1);
    in = at(500, 0x480); oot_hook_step(&s, &in, 1, &o); assert(s.hand < 0 && o.event == OOT_HOOK_LET_GO && o.put_away && !o.b.mask);
    in = at(600, 0x480); oot_hook_step(&s, &in, 1, &o); assert(!o.put_away);                 /* 300 ms apart */
    in = at(820, 0x480); oot_hook_step(&s, &in, 1, &o); assert(o.put_away);
    in = at(900, 0); oot_hook_step(&s, &in, 1, &o); assert(!o.put_away && !s.away.asked);    /* away */
    /* gripped again within 300 ms of seeing it out: no take-out press */
    oot_hook_init(&s); hand(0, 0, 0); in = at(1000, 0x480); oot_hook_step(&s, &in, 1, &o);
    hand(0, 1, 0); in = at(1100, 0x480); oot_hook_step(&s, &in, 1, &o); assert(s.hand == 0 && !s.taking && o.b.triggers == 1 && o.holding == 1);
    /* let go not in a shot: put away at once; a grip again withdraws the call not yet made */
    hand(0, 0, 0); in = at(1200, 0x480); oot_hook_step(&s, &in, 1, &o); assert(o.put_away && s.hand < 0);
    hand(0, 1, 0); in = at(1300, 0x480); oot_hook_step(&s, &in, 1, &o); assert(s.hand == 0 && !s.away.asked && !o.put_away);
    /* take-out times out after 700 ms when it never shows */
    oot_hook_init(&s); hand(0, 0, 0); in = at(5000, 0); oot_hook_step(&s, &in, 1, &o);
    hand(0, 1, 0); in = at(5010, 0); oot_hook_step(&s, &in, 1, &o); assert(s.taking);
    in = at(5600, 0); oot_hook_step(&s, &in, 1, &o); assert(s.taking && (o.b.add & OOT_IH_ZR));
    in = at(5720, 0); oot_hook_step(&s, &in, 1, &o); assert(!s.taking && !(o.b.add & OOT_IH_ZR));
    /* a shot never ending is over after 6 s */
    s.shot_at = 6000 * MS; hand(0, 0, 0); in = at(6100, 0xe00480); oot_hook_step(&s, &in, 1, &o); assert(s.keep && s.hand == 0);
    in = at(12100, 0xe00480); oot_hook_step(&s, &in, 1, &o); assert(s.hand < 0);
    /* no fresh hands: nothing is added */
    oot_hook_init(&s); in = at(20000, 0x480); in.hands = NULL; oot_hook_step(&s, &in, 1, &o); assert(!o.b.add && !o.b.mask);
    puts("hookshot: ok");
    return 0;
}
