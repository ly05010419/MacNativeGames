/* The ocarina in the player's hands (zeldaVR/runtime/oot_vr_ocarina.h): taken from the belt, started at the mouth, the notes,
   the song list, the end. */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_ocarina.h"

static const uint64_t MS = 1000000ull, B = 1;
static oot_oc_in base(void) {
    oot_oc_in in; memset(&in, 0, sizeof in);
    for (int i = 0; i < 2; i++) { in.hand[i].have = 1; in.hand[i].lvl[0] = 0.3f; in.hand[i].lvl[2] = -0.3f; in.hand[i].p[1] = -0.3f; in.hand[i].p[2] = -0.3f; }
    in.owned = 1; in.song_menu = 1;
    return in;
}
static void at_belt(oot_oc_in *in, int h) { memcpy(in->hand[h].lvl, OOT_OC_BELT, sizeof OOT_OC_BELT); in->hand[h].p[0] = -0.14f; in->hand[h].p[1] = -0.45f; in->hand[h].p[2] = -0.08f; }
static void at_mouth(oot_oc_in *in, int h) { memcpy(in->hand[h].p, OOT_OC_MOUTH, sizeof OOT_OC_MOUTH); in->hand[h].p[2] -= 0.05f; }
static void away(oot_oc_in *in, int h) { in->hand[h].p[0] = 0.1f; in->hand[h].p[1] = -0.4f; in->hand[h].p[2] = -0.35f; }

int main(void) {
    oot_oc_state s; memset(&s, 0, sizeof s); oot_oc_out o; uint64_t t = 1000 * MS;
    oot_oc_in in = base();
    /* nothing owned: the belt is empty */
    in.owned = 0; at_belt(&in, 0); in.hand[0].grip = 1; oot_ocarina_step(&s, t, &in, &o); assert(s.held == -1 && !o.say);
    in.hand[0].grip = 0; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o);
    /* owned: the left hand closing at the belt takes it; a hand elsewhere or busy does not */
    in.owned = 1; in.busy[0] = 1; in.hand[0].grip = 1; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(s.held == -1);
    in.hand[0].grip = 0; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); in.busy[0] = 0;
    in.hand[1].grip = 1; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(s.held == -1);   /* the right hand is not at the belt */
    in.hand[0].grip = 1; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o);
    assert(s.held == 0 && o.say == OOT_OC_SAY_TAKEN && o.buzz[0] && (o.clear & B << OOT_SW_A));
    /* half closed holds it */
    in.hand[0].grip = 0.45f; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(s.held == 0);
    in.hand[1].grip = 0;
    /* to the mouth: the game's D-pad Left for 100 ms, once */
    at_mouth(&in, 0); t += 400 * MS; oot_ocarina_step(&s, t, &in, &o);
    assert(s.at_mouth && o.say == OOT_OC_SAY_START && (o.add & B << OOT_SW_LEFT));
    t += 50 * MS; oot_ocarina_step(&s, t, &in, &o); assert(o.add & B << OOT_SW_LEFT);
    t += 60 * MS; oot_ocarina_step(&s, t, &in, &o); assert(!(o.add & B << OOT_SW_LEFT));
    /* the game waits (a field ocarina): the notes */
    in.waiting = 1; in.wait_kind = 1; in.event = 1; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o);
    assert(o.say == OOT_OC_SAY_WAITS && o.take_stick && (o.clear & B << OOT_SW_ZL) && (o.clear & B << OOT_SW_X));
    in.ltrig = 0.9f; in.ry = 0.7f; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o);
    assert((o.add & B << OOT_SW_L) && (o.add & B << OOT_SW_X) && !(o.add & B << OOT_SW_R) && !(o.add & B << OOT_SW_ZL));
    in.ry = 0.5f; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(o.add & B << OOT_SW_X);    /* holds until under 0.4 */
    in.ry = 0.3f; in.ltrig = 0; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(!(o.add & (B << OOT_SW_X | B << OOT_SW_L)));
    in.rx = -0.8f; in.rtrig = 1; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert((o.add & B << OOT_SW_Y) && (o.add & B << OOT_SW_R) && !(o.add & B << OOT_SW_A));
    in.rx = 0.8f; in.rtrig = 0; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert((o.add & B << OOT_SW_A) && !(o.add & B << OOT_SW_Y));
    in.rx = 0; in.upper = 1; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert((o.add & B << OOT_SW_UP) && !(o.add & B << OOT_SW_A));
    in.upper = 0; in.lower = 1; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(o.add & B << OOT_SW_DOWN);
    in.lower = 0;
    /* the other hand's grip: the song list (the game's wait stops meanwhile, the ocarina stays at the mouth) */
    in.hand[1].grip = 1; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(o.add & B << OOT_SW_ZL);
    in.waiting = 0; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert((o.add & B << OOT_SW_ZL) && o.say != OOT_OC_SAY_DONE && s.was_waiting);
    in.hand[1].grip = 0; in.waiting = 1; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(!(o.add & B << OOT_SW_ZL));
    /* away from the mouth for 250 ms on the field: B once */
    away(&in, 0); t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(o.say == OOT_OC_SAY_AWAY && !o.take_stick);
    t += 200 * MS; oot_ocarina_step(&s, t, &in, &o); assert(!(o.add & B << OOT_SW_B));
    t += 60 * MS; oot_ocarina_step(&s, t, &in, &o); assert(o.say == OOT_OC_SAY_CANCEL && (o.add & B << OOT_SW_B));
    in.waiting = 0; in.event = 0; t += 200 * MS; oot_ocarina_step(&s, t, &in, &o); assert(!(o.add & B << OOT_SW_B) && s.armed);
    /* a song lesson's wait (kind 2) is not ended by B; the start gives up when the game never waits */
    at_mouth(&in, 0); t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(o.say == OOT_OC_SAY_START);
    t += 1600 * MS; oot_ocarina_step(&s, t, &in, &o); assert(o.say == OOT_OC_SAY_GAVE_UP && !s.started && !s.armed);
    t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(!(o.add & B << OOT_SW_LEFT));   /* not again until it left the mouth */
    away(&in, 0); t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(s.armed);
    in.waiting = 1; in.wait_kind = 2; in.event = 1; at_mouth(&in, 0); t += 10 * MS; oot_ocarina_step(&s, t, &in, &o);
    assert(!(o.add & B << OOT_SW_LEFT) && o.take_stick);   /* the game waits already: no start, the notes at once */
    away(&in, 0); t += 400 * MS; oot_ocarina_step(&s, t, &in, &o); t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(!(o.add & B << OOT_SW_B));
    /* let go while the game waits on the field: back on the belt and B */
    in.wait_kind = 3; at_mouth(&in, 0); t += 10 * MS; oot_ocarina_step(&s, t, &in, &o);
    in.hand[0].grip = 0; t += 10 * MS; oot_ocarina_step(&s, t, &in, &o);
    assert(s.held == -1 && o.say == OOT_OC_SAY_CANCEL && (o.add & B << OOT_SW_B) && (o.clear & B << OOT_SW_A));
    /* a dialogue's wait (kind 13) is the game's: no notes, no B */
    assert(!oot_oc_buttons(13) && oot_oc_buttons(7) && oot_oc_buttons(11));
    /* a scene playing: no start */
    in.waiting = 0; in.event = 1; at_belt(&in, 1); in.hand[1].grip = 1; t += 400 * MS; oot_ocarina_step(&s, t, &in, &o); assert(s.held == 1);
    at_mouth(&in, 1); t += 10 * MS; oot_ocarina_step(&s, t, &in, &o); assert(!(o.add & B << OOT_SW_LEFT) && s.at_mouth);
    puts("the ocarina: belt, mouth, start once, give up, the notes, the song list, B on the field only");
    return 0;
}
