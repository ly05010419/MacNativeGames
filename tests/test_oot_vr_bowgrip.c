/* The bow taken with a grip and held (zeldaVR/runtime/oot_vr_bowgrip.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_bowgrip.h"

static const uint64_t MS = 1000000ull;
static oot_bowgrip_in base(uint64_t t) {
    oot_bowgrip_in in; memset(&in, 0, sizeof in);
    in.now = 1000 * MS + t * MS; in.first_person = 1; in.grip_hold = 1; in.bow_selected = 1;
    return in;
}

int main(void) {
    oot_bowgrip s; memset(&s, 0, sizeof s); oot_bowgrip_out o; oot_bowgrip_in in;
    /* a grip takes it out: the game's ZR held until it is out, then released; that hand owns its grip */
    in = base(0); oot_bowgrip_step(&s, &in, &o); assert(!o.zr && o.bow_hand < 0);
    in = base(10); in.grip[0] = 0.9f; oot_bowgrip_step(&s, &in, &o); assert(o.zr && s.state == OOT_BOWGRIP_TAKING_OUT && o.bow_hand == 0 && o.owns[0] && !o.owns[1]);
    in = base(100); in.grip[0] = 0.9f; oot_bowgrip_step(&s, &in, &o); assert(o.zr);
    in = base(300); in.grip[0] = 0.9f; in.bow_out = 1; oot_bowgrip_step(&s, &in, &o); assert(!o.zr && s.state == OOT_BOWGRIP_HOLDING);
    /* holding, the string not drawn: no ZR, and the player's own ZR is not the game's while a solution is there */
    in = base(320); in.grip[0] = 0.9f; in.bow_out = 1; in.fresh = 1; in.real_zr = 1; oot_bowgrip_step(&s, &in, &o); assert(!o.zr);
    /* the string held (the other hand owns its grip too), then committed: ZR held */
    in = base(340); in.grip[0] = 0.9f; in.bow_out = 1; in.fresh = 1; in.held = 1; oot_bowgrip_step(&s, &in, &o); assert(!o.zr && o.owns[1]);
    in = base(360); in.grip[0] = 0.9f; in.bow_out = 1; in.fresh = 1; in.held = 1; in.committed = 1; oot_bowgrip_step(&s, &in, &o); assert(o.zr);
    /* the solution drops out for a moment: the answer is kept, no shot */
    in = base(800); in.grip[0] = 0.9f; in.bow_out = 1; oot_bowgrip_step(&s, &in, &o); assert(o.zr);
    /* the string let go: ZR released (the game's shot) */
    in = base(820); in.grip[0] = 0.9f; in.bow_out = 1; in.fresh = 1; oot_bowgrip_step(&s, &in, &o); assert(!o.zr && s.state == OOT_BOWGRIP_HOLDING);
    /* the bow grip let go while drawing: kept */
    in = base(830); in.grip[0] = 0.1f; in.bow_out = 1; in.fresh = 1; in.held = 1; oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_HOLDING);
    /* let go with nothing drawn: put away (asked once) */
    in = base(840); in.grip[0] = 0.1f; in.bow_out = 1; in.fresh = 1; oot_bowgrip_step(&s, &in, &o); assert(o.put_away && s.state == OOT_BOWGRIP_PUTTING_AWAY);
    in = base(850); in.grip[0] = 0.1f; in.bow_out = 1; in.fresh = 1; oot_bowgrip_step(&s, &in, &o); assert(!o.put_away);
    /* taken again while it is still out: the ask withdrawn, held */
    in = base(860); in.grip[0] = 0.9f; in.bow_out = 1; in.fresh = 1; oot_bowgrip_step(&s, &in, &o); assert(o.cancel_put_away && s.state == OOT_BOWGRIP_HOLDING);
    /* let go again; it goes away; idle once gone for 150 ms */
    in = base(900); in.grip[0] = 0.1f; in.bow_out = 1; in.fresh = 1; oot_bowgrip_step(&s, &in, &o); assert(o.put_away);
    in = base(920); oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_PUTTING_AWAY);
    in = base(1100); oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_IDLE && o.bow_hand < 0);
    /* gone just as the grip took it again: taken out again */
    memset(&s, 0, sizeof s);
    in = base(0); in.bow_out = 1; oot_bowgrip_step(&s, &in, &o);
    in = base(10); in.bow_out = 1; in.grip[1] = 0.9f; oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_HOLDING && o.bow_hand == 1);   /* out already: held at once */
    in = base(20); in.bow_out = 1; in.grip[1] = 0.1f; oot_bowgrip_step(&s, &in, &o); assert(o.put_away);
    in = base(30); in.bow_out = 1; in.grip[1] = 0.9f; oot_bowgrip_step(&s, &in, &o); assert(o.cancel_put_away);
    in = base(40); in.bow_out = 1; in.grip[1] = 0.1f; oot_bowgrip_step(&s, &in, &o); assert(o.put_away);
    in = base(50); in.bow_out = 0; in.grip[1] = 0.9f; oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_TAKING_OUT && o.zr);
    /* nothing comes out in 0.7 s: idle */
    in = base(800); in.grip[1] = 0.9f; oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_IDLE && !o.zr);
    /* a claimed hand (the sword's) takes nothing; the item button not the bow: nothing; the wheel open: nothing */
    memset(&s, 0, sizeof s);
    in = base(0); in.grip[1] = 0.9f; in.claimed[1] = 1; oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_IDLE);
    in = base(10); in.grip[0] = 0.9f; in.bow_selected = 0; oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_IDLE);
    in = base(20); in.grip[0] = 0.1f; oot_bowgrip_step(&s, &in, &o);
    in = base(30); in.grip[0] = 0.9f; in.wheel_open = 1; oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_IDLE);
    /* the game puts it away by itself: idle */
    memset(&s, 0, sizeof s);
    in = base(0); in.bow_out = 1; in.grip[0] = 0.9f; oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_HOLDING);
    in = base(10); in.grip[0] = 0.9f; oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_HOLDING);
    in = base(200); in.grip[0] = 0.9f; oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_IDLE);
    /* holding with no string solution at all (no model, no hands): the player's ZR fires as before */
    memset(&s, 0, sizeof s);
    in = base(0); in.bow_out = 1; in.grip[0] = 0.9f; oot_bowgrip_step(&s, &in, &o);
    in = base(10); in.bow_out = 1; in.grip[0] = 0.9f; in.real_zr = 1; oot_bowgrip_step(&s, &in, &o); assert(o.zr);
    /* third person, or the option off: only the string fires; the ZR press that brought it out passes until let go */
    memset(&s, 0, sizeof s);
    in = base(0); in.grip_hold = 0; in.real_zr = 1; oot_bowgrip_step(&s, &in, &o); assert(o.zr);
    in = base(10); in.grip_hold = 0; in.real_zr = 1; in.bow_out = 1; in.fresh = 1; oot_bowgrip_step(&s, &in, &o); assert(o.zr);   /* still the same press */
    in = base(20); in.grip_hold = 0; in.bow_out = 1; in.fresh = 1; oot_bowgrip_step(&s, &in, &o); assert(!o.zr);
    in = base(30); in.grip_hold = 0; in.real_zr = 1; in.bow_out = 1; in.fresh = 1; oot_bowgrip_step(&s, &in, &o); assert(!o.zr);   /* a new press: the string's */
    in = base(40); in.grip_hold = 0; in.bow_out = 1; in.fresh = 1; in.committed = 1; oot_bowgrip_step(&s, &in, &o); assert(o.zr);
    in = base(50); in.grip_hold = 0; in.real_zr = 1; oot_bowgrip_step(&s, &in, &o); assert(o.zr);                                   /* the bow not out: the buttons */
    /* leaving first person while holding: the buttons again */
    memset(&s, 0, sizeof s);
    in = base(0); in.bow_out = 1; in.grip[0] = 0.9f; oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_HOLDING);
    in = base(10); in.bow_out = 1; in.grip[0] = 0.9f; in.first_person = 0; oot_bowgrip_step(&s, &in, &o); assert(s.state == OOT_BOWGRIP_IDLE && o.said);
    puts("oot_vr_bowgrip: all passed");
    return 0;
}
