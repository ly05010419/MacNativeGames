/* A bottle in the player's hand (zeldaVR/runtime/oot_vr_bottle.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_bottle.h"

#define MS 1000000ull
static oot_ih_frame frame;
static void hand(int i, float grip, float trigger) {
    oot_ih_hand *h = &frame.hand[i];
    h->have = 1; h->grip = grip; h->trigger = trigger; h->f = 0.3f; h->u = -0.3f; h->r = i ? 0.2f : -0.2f; h->dir[2] = -1; h->palm[0] = 1;
}
static oot_ih_in at(uint64_t t) { oot_ih_in in = { t * MS, &frame, 14, 0, 1, 0 }; return in; }

int main(void) {
    oot_bottle_state s; oot_bottle_init(&s); oot_bottle_out o;
    hand(0, 0, 0); hand(1, 0, 0);
    oot_ih_in in = at(0); oot_bottle_step(&s, &in, 1, 0, 0, &o); assert((o.b.mask & OOT_IH_ZR) && !o.draw);
    /* a grip takes it out: drawn by the game's call, again every 300 ms until it reads drawn */
    /* the game's stow of slot 1 is refused while it is being drawn too: Link's own AI asks a stow every frame, and it turned the
       draw's request (2) into a stow (4) before the game took it (batch15, 2026-11-27) */
    hand(0, 1, 0); in = at(10); oot_bottle_step(&s, &in, 1, 0, 0, &o); assert(s.phase == OOT_BOTTLE_HELD && o.draw && o.keep_drawn && o.holding == 1 && o.b.triggers == 1 && o.event == OOT_BOTTLE_EV_TOOK);
    in = at(100); oot_bottle_step(&s, &in, 1, 0, 0, &o); assert(!o.draw && o.keep_drawn);
    in = at(320); oot_bottle_step(&s, &in, 1, 0, 0, &o); assert(o.draw);
    in = at(400); oot_bottle_step(&s, &in, 1, 0, 1, &o); assert(!o.draw);   /* not during a scene */
    in = at(700); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(o.keep_drawn && !o.draw);
    /* the trigger: the hold lifted, stowed; once stowed ZR for 130 ms; the game's scene; drawn again 400 ms after it */
    hand(0, 1, 0.9f); in = at(800); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(s.phase == OOT_BOTTLE_USING && o.stow && !o.keep_drawn && o.event == OOT_BOTTLE_EV_USE);
    in = at(850); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(!o.stow && !(o.b.add & OOT_IH_ZR));
    in = at(1110); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(o.stow);
    in = at(1150); oot_bottle_step(&s, &in, 1, 0, 0, &o); assert(s.ready && (o.b.add & OOT_IH_ZR) && o.event == OOT_BOTTLE_EV_TAPPED);
    in = at(1270); oot_bottle_step(&s, &in, 1, 1, 1, &o); assert(o.b.add & OOT_IH_ZR);
    in = at(1290); oot_bottle_step(&s, &in, 1, 1, 1, &o); assert(!(o.b.add & OOT_IH_ZR));
    in = at(3000); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(s.phase == OOT_BOTTLE_USING);
    in = at(3390); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(s.phase == OOT_BOTTLE_USING);
    in = at(3410); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(s.phase == OOT_BOTTLE_HELD && o.event == OOT_BOTTLE_EV_USED);
    hand(0, 1, 0); in = at(3500); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(o.keep_drawn);
    /* used with no scene: back after 1.5 s */
    hand(0, 1, 0.9f); in = at(4000); oot_bottle_step(&s, &in, 1, 1, 0, &o);
    in = at(4100); oot_bottle_step(&s, &in, 1, 0, 0, &o); assert(s.ready);
    in = at(5450); oot_bottle_step(&s, &in, 1, 0, 0, &o); assert(s.phase == OOT_BOTTLE_USING);
    in = at(5510); oot_bottle_step(&s, &in, 1, 0, 0, &o); assert(s.phase == OOT_BOTTLE_HELD);
    /* the stow not taken within 1 s: the press dropped */
    hand(0, 1, 0); in = at(5600); oot_bottle_step(&s, &in, 1, 1, 0, &o);
    hand(0, 1, 0.9f); in = at(5700); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(s.phase == OOT_BOTTLE_USING);
    in = at(6710); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(s.phase == OOT_BOTTLE_HELD && o.event == OOT_BOTTLE_EV_DROPPED && !(o.b.add & OOT_IH_ZR));
    /* let go: stowed every 300 ms while it reads drawn, at most four times */
    hand(0, 0, 0); in = at(7000); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(s.phase == OOT_BOTTLE_STOWING && o.stow && !o.holding && o.event == OOT_BOTTLE_EV_LET_GO);
    in = at(7100); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(!o.stow && !o.keep_drawn);
    in = at(7310); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(o.stow);
    in = at(7620); oot_bottle_step(&s, &in, 1, 1, 0, &o); in = at(7930); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(o.stow);
    in = at(8240); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(!o.stow && s.phase == OOT_BOTTLE_IDLE && o.event == OOT_BOTTLE_EV_GAVE_UP);
    /* a grip behind the shoulder is the sword's, not the bottle's */
    frame.hand[1].f = -0.1f; frame.hand[1].u = 0; frame.hand[1].r = 0.1f; frame.hand[1].grip = 1;
    in = at(9000); oot_bottle_step(&s, &in, 1, 0, 0, &o); assert(s.phase == OOT_BOTTLE_IDLE);
    /* not active (another item, third person): no mask, a bottle out is stowed */
    hand(1, 0, 0); hand(0, 1, 0); in = at(9100); oot_bottle_step(&s, &in, 1, 0, 0, &o); in = at(9200); oot_bottle_step(&s, &in, 1, 1, 0, &o); assert(o.keep_drawn);
    in = at(9300); oot_bottle_step(&s, &in, 0, 1, 0, &o); assert(!o.b.mask && !o.keep_drawn && s.phase == OOT_BOTTLE_STOWING);
    puts("bottle: ok");
    return 0;
}
