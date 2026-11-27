/* The Deku Stick in the player's hand (zeldaVR/runtime/oot_vr_stick.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_stick.h"

#define MS 1000000ull
static oot_ih_frame frame;
static void hand(int i, float grip) {
    oot_ih_hand *h = &frame.hand[i];
    h->have = 1; h->grip = grip; h->f = 0.3f; h->u = -0.3f; h->r = i ? 0.2f : -0.2f; h->dir[2] = -1; h->palm[0] = 1;
}
static oot_ih_in at(uint64_t t) { oot_ih_in in = { t * MS, &frame, OOT_STICK_ITEM, 0, 1, 0 }; return in; }

static void take_and_let_go(void) {
    oot_stick_state s; oot_stick_init(&s); oot_stick_out o;
    hand(0, 0); hand(1, 0);
    oot_ih_in in = at(0); oot_stick_step(&s, &in, 1, 0, 0, &o);
    assert((o.b.mask & OOT_IH_ZR) && !(o.b.add & OOT_IH_ZR) && !o.holding);   /* the real ZR is the stick's while it is selected */
    /* a grip takes it: the game's ZR held for it until it reads out */
    hand(1, 1); in = at(10); oot_stick_step(&s, &in, 1, 0, 0, &o);
    assert(o.event == OOT_STICK_EV_TOOK && (o.b.add & OOT_IH_ZR) && o.holding == 2 && o.b.triggers == 2);
    in = at(200); oot_stick_step(&s, &in, 1, 0, 0, &o); assert(o.b.add & OOT_IH_ZR);
    in = at(400); oot_stick_step(&s, &in, 1, 1, 0, &o); assert(o.event == OOT_STICK_EV_OUT && !(o.b.add & OOT_IH_ZR) && o.holding == 2);
    in = at(800); oot_stick_step(&s, &in, 1, 1, 0, &o); assert(!o.event && !(o.b.add & OOT_IH_ZR) && o.holding == 2);
    /* let go: the game's put-away, again every 300 ms while it reads out, at most four times */
    hand(1, 0); in = at(900); oot_stick_step(&s, &in, 1, 1, 0, &o); assert(o.event == OOT_STICK_EV_LET_GO && o.put_away && !o.holding);
    in = at(1000); oot_stick_step(&s, &in, 1, 1, 0, &o); assert(!o.put_away);
    in = at(1210); oot_stick_step(&s, &in, 1, 1, 0, &o); assert(o.put_away);
    in = at(1300); oot_stick_step(&s, &in, 1, 0, 0, &o); assert(!o.put_away && s.phase == OOT_STICK_IDLE);
}
static void none_left(void) {
    oot_stick_state s; oot_stick_init(&s); oot_stick_out o;
    hand(0, 0); hand(1, 0);
    hand(0, 1); oot_ih_in in = at(0); oot_stick_step(&s, &in, 1, 0, 0, &o); assert((o.b.add & OOT_IH_ZR) && o.holding == 1);
    in = at(650); oot_stick_step(&s, &in, 1, 0, 0, &o); assert(o.b.add & OOT_IH_ZR);
    in = at(720); oot_stick_step(&s, &in, 1, 0, 0, &o); assert(o.event == OOT_STICK_EV_NONE_LEFT && !(o.b.add & OOT_IH_ZR));   /* 700 ms: none left */
    in = at(1100); oot_stick_step(&s, &in, 1, 0, 0, &o); assert(!o.event && !(o.b.add & OOT_IH_ZR));   /* not held again until a new grip */
    hand(0, 0); in = at(1200); oot_stick_step(&s, &in, 1, 0, 0, &o); assert(!o.put_away && s.phase == OOT_STICK_IDLE);
    hand(0, 1); in = at(1300); oot_stick_step(&s, &in, 1, 0, 0, &o); assert(o.event == OOT_STICK_EV_TOOK && (o.b.add & OOT_IH_ZR));
}
static void not_taken(void) {
    oot_stick_state s; oot_stick_init(&s); oot_stick_out o;
    hand(0, 0); hand(1, 0);
    /* the sword out: no grip takes the stick */
    hand(1, 1); oot_ih_in in = at(0); oot_stick_step(&s, &in, 1, 0, 1, &o); assert(!o.event && !o.b.add && !o.holding);
    hand(1, 0); in = at(50); oot_stick_step(&s, &in, 1, 0, 1, &o);
    /* a hand behind the shoulder (the sword's and shield's gestures) takes nothing */
    hand(1, 1); frame.hand[1].f = -0.1f; frame.hand[1].u = 0;
    in = at(100); oot_stick_step(&s, &in, 1, 0, 0, &o); assert(!o.event && !o.holding);
    hand(1, 0); in = at(150); oot_stick_step(&s, &in, 1, 0, 0, &o);
    /* not active (another item, the option off, a scene): nothing, and a stick held is let go of without a call */
    hand(1, 1); in = at(200); oot_stick_step(&s, &in, 0, 0, 0, &o); assert(!o.event && !o.b.mask && !o.holding);
    in = at(250); oot_stick_step(&s, &in, 1, 0, 0, &o); assert(!o.event);   /* the grip was closed before: not a fresh one */
    hand(1, 0); in = at(300); oot_stick_step(&s, &in, 1, 0, 0, &o);
    hand(1, 1); in = at(350); oot_stick_step(&s, &in, 1, 0, 0, &o); assert(o.event == OOT_STICK_EV_TOOK);
    in = at(400); oot_stick_step(&s, &in, 0, 1, 0, &o); assert(!o.holding && !o.put_away && s.phase == OOT_STICK_IDLE);
}
/* The lit state as the game's own query reads it (main+0x8d7df0): burning wins, else the first counted object's state. */
static void lit(void) {
    const oot_stick_chem objs[3] = { { 0x0, 0, 1 }, { 0x4, 0x4, 1 }, { 0x4, 0, 2 } };   /* not counted, counted but hidden by +0x44, burnt */
    assert(oot_stick_lit_state(objs, 1) == 0);
    assert(oot_stick_lit_state(objs, 2) == 0);
    assert(oot_stick_lit_state(objs, 3) == 2);
    const oot_stick_chem burning[2] = { { 0x4, 0, 2 }, { 0x4, 0x8, 1 } };   /* burning wins */
    assert(oot_stick_lit_state(burning, 2) == 1);
    const oot_stick_chem negative[1] = { { 0x4, 0, -1 } };   /* a negative state is skipped, as the game's tbnz on bit 31 */
    assert(oot_stick_lit_state(negative, 1) == 0);
}
int main(void) {
    take_and_let_go(); none_left(); not_taken(); lit();
    puts("oot_vr_stick: ok");
    return 0;
}
