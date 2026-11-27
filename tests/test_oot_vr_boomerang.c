/* The boomerang in the player's hand (zeldaVR/runtime/oot_vr_boomerang.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_boomerang.h"

#define MS 1000000ull
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)
static oot_ih_frame frame;
static void hand(int i, float grip, float trigger) {
    oot_ih_hand *h = &frame.hand[i];
    h->have = 1; h->grip = grip; h->trigger = trigger; h->f = 0.3f; h->u = -0.3f; h->r = i ? 0.2f : -0.2f;
    h->pos[0] = i ? 0.2f : -0.2f; h->pos[1] = 1.0f; h->pos[2] = 0;
    h->dir[0] = 0; h->dir[1] = 0; h->dir[2] = -1; h->palm[0] = i ? -1.0f : 1.0f;
}
static oot_ih_in at(uint64_t t, uint32_t action) { oot_ih_in in = { t * MS, &frame, 3, action, 1, 0 }; return in; }
enum { REST = 0x100400, RAISED = 0x180400 };

int main(void) {
    /* the target: 60 m along the line from the hand through the mark 10 m out */
    float o[3] = { 0, 1.5f, 0 }, d[3] = { 0, 0, -1 }, start[3] = { 0.3f, 1.0f, 0 }, t[3];
    oot_boom_target(o, d, start, t);
    const float m[3] = { 0, 1.5f, -10 }, v[3] = { m[0] - start[0], m[1] - start[1], m[2] - start[2] }, n = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    assert(NEAR(t[0], start[0] + 60 * v[0] / n) && NEAR(t[1], start[1] + 60 * v[1] / n) && NEAR(t[2], start[2] + 60 * v[2] / n));
    const float on_mark[3] = { 0, 1.5f, -10 }; oot_boom_target(o, d, on_mark, t); assert(NEAR(t[2], -60) && NEAR(t[1], 1.5f));   /* within 10 cm: along the line */
    /* the at-rest test */
    assert(oot_boom_at_rest(REST) && !oot_boom_at_rest(RAISED) && !oot_boom_at_rest(0x400) && oot_boom_raised(RAISED));

    oot_boom_state s; oot_boom_init(&s); oot_boom_out out;
    hand(0, 0, 0); hand(1, 0, 0);
    oot_ih_in in = at(0, 0); oot_boom_step(&s, &in, 1, 0, &out); assert(out.b.mask & OOT_IH_ZR);   /* the real ZR is the boomerang's */
    /* not out: a grip takes it, ZR held until it rests (Link raises it if ZR stays: lowered with B) */
    hand(1, 1, 0); in = at(10, 0); oot_boom_step(&s, &in, 1, 0, &out); assert(s.phase == OOT_BOOM_TAKING && out.event == OOT_BOOM_EV_TOOK && (out.b.add & OOT_IH_ZR));
    in = at(50, 0x2180400); oot_boom_step(&s, &in, 1, 0, &out); assert(s.phase == OOT_BOOM_LOWERING);
    in = at(60, 0x2180400); oot_boom_step(&s, &in, 1, 0, &out); assert((out.b.add & OOT_IH_ZR) && (out.b.add & OOT_IH_B));
    in = at(200, 0x2180400); oot_boom_step(&s, &in, 1, 0, &out); assert(!(out.b.add & OOT_IH_B));        /* B for 100 ms of every 300 */
    in = at(370, 0x2180400); oot_boom_step(&s, &in, 1, 0, &out); assert(out.b.add & OOT_IH_B);
    in = at(400, REST); oot_boom_step(&s, &in, 1, 0, &out); assert(s.phase == OOT_BOOM_HELD && out.holding == 2);
    in = at(420, REST); oot_boom_step(&s, &in, 1, 0, &out); assert(!out.b.add && out.b.triggers == 2 && !out.aiming);
    /* the trigger aims, let go throws along the holding hand's line */
    hand(1, 1, 0.9f); in = at(450, RAISED); oot_boom_step(&s, &in, 1, 0, &out); assert((out.b.add & OOT_IH_ZR) && out.aiming && out.aim_hand == 1);
    frame.hand[1].dir[0] = 1; frame.hand[1].dir[2] = 0;
    hand(1, 1, 0.1f); frame.hand[1].dir[0] = 1; frame.hand[1].dir[2] = 0;
    in = at(600, RAISED); oot_boom_step(&s, &in, 1, 0, &out);
    assert(s.phase == OOT_BOOM_THROWN && out.event == OOT_BOOM_EV_THROWN && !(out.b.add & OOT_IH_ZR) && s.have_line && NEAR(s.line_d[0], 1) && NEAR(s.line_o[0], 0.25f) && s.have_start);
    in = at(650, 0x400); oot_boom_step(&s, &in, 1, 0, &out); assert(s.flown && out.holding == 0);   /* in flight: not in the hand */
    in = at(2000, REST); oot_boom_step(&s, &in, 1, 0, &out); assert(s.phase == OOT_BOOM_HELD && out.event == OOT_BOOM_EV_BACK);   /* caught, still gripped */
    /* let go: put away by the game's call */
    hand(1, 0, 0); in = at(2100, REST); oot_boom_step(&s, &in, 1, 0, &out); assert(s.phase == OOT_BOOM_AWAY && out.put_away && out.holding == -2);
    in = at(2200, REST); oot_boom_step(&s, &in, 1, 0, &out); assert(!out.put_away);
    in = at(2420, REST); oot_boom_step(&s, &in, 1, 0, &out); assert(out.put_away);
    in = at(2500, 0); oot_boom_step(&s, &in, 1, 0, &out); assert(s.phase == OOT_BOOM_IDLE);
    /* already at rest when gripped: held at once; let go while raised: lowered first */
    oot_boom_init(&s); hand(0, 0, 0); hand(1, 0, 0); in = at(3000, REST); oot_boom_step(&s, &in, 1, 0, &out);
    hand(0, 1, 0); in = at(3010, REST); oot_boom_step(&s, &in, 1, 0, &out); assert(s.phase == OOT_BOOM_HELD && s.hand == 0 && out.b.triggers == 1);
    hand(0, 1, 0.9f); in = at(3020, RAISED); oot_boom_step(&s, &in, 1, 0, &out);
    hand(0, 0, 0.9f); in = at(3030, RAISED); oot_boom_step(&s, &in, 1, 0, &out); assert(s.phase == OOT_BOOM_AWAY && !s.lowered && (out.b.add & OOT_IH_B) && !out.put_away);
    in = at(3100, REST); oot_boom_step(&s, &in, 1, 0, &out); assert(s.lowered && out.put_away);
    /* the put-away gives up after four calls: B for 100 ms */
    in = at(3400, REST); oot_boom_step(&s, &in, 1, 0, &out); in = at(3700, REST); oot_boom_step(&s, &in, 1, 0, &out);
    in = at(4000, REST); oot_boom_step(&s, &in, 1, 0, &out); assert(out.put_away);
    in = at(4300, REST); oot_boom_step(&s, &in, 1, 0, &out); assert(out.event == OOT_BOOM_EV_GAVE_UP && (out.b.add & OOT_IH_B));
    in = at(4450, REST); oot_boom_step(&s, &in, 1, 0, &out); assert(!(out.b.add & OOT_IH_B));
    in = at(5700, REST); oot_boom_step(&s, &in, 1, 0, &out); assert(s.phase == OOT_BOOM_IDLE);   /* over after 2.5 s */
    /* the empty hand aims (mode 1): its trigger and its line; both triggers are the boomerang's */
    oot_boom_init(&s); hand(0, 0, 0); hand(1, 0, 0); in = at(6000, REST); oot_boom_step(&s, &in, 1, 1, &out);
    hand(1, 1, 0); in = at(6010, REST); oot_boom_step(&s, &in, 1, 1, &out); assert(s.phase == OOT_BOOM_HELD && out.b.triggers == 3);
    hand(1, 1, 0.9f); in = at(6020, REST); oot_boom_step(&s, &in, 1, 1, &out); assert(!out.aiming);   /* the holding hand's trigger does not aim */
    hand(1, 1, 0); hand(0, 0, 0.9f); in = at(6030, RAISED); oot_boom_step(&s, &in, 1, 1, &out); assert(out.aiming && out.aim_hand == 0);
    hand(0, 0, 0); frame.hand[0].dir[1] = 1; frame.hand[0].dir[2] = 0;
    in = at(6100, RAISED); oot_boom_step(&s, &in, 1, 1, &out); assert(s.phase == OOT_BOOM_THROWN && NEAR(s.line_d[1], 1) && NEAR(s.start[0], 0.2f));
    /* the headset aims (mode 2) */
    oot_boom_init(&s); hand(0, 0, 0); hand(1, 0, 0); frame.have_head = 1; frame.eye[1] = 1.4f; frame.fwd[2] = -1;
    in = at(7000, REST); oot_boom_step(&s, &in, 1, 2, &out);
    hand(1, 1, 0); in = at(7010, REST); oot_boom_step(&s, &in, 1, 2, &out);
    hand(1, 1, 0.9f); in = at(7020, RAISED); oot_boom_step(&s, &in, 1, 2, &out); assert(out.aiming && out.aim_hand == -1);
    hand(1, 1, 0); in = at(7100, RAISED); oot_boom_step(&s, &in, 1, 2, &out); assert(s.have_line && NEAR(s.line_o[1], 1.4f) && NEAR(s.line_d[2], -1));
    /* not thrown after all (never left the hand): back after 1.5 s */
    in = at(8700, RAISED); oot_boom_step(&s, &in, 1, 2, &out); assert(s.phase == OOT_BOOM_HELD);
    /* the taking times out after 700 ms */
    oot_boom_init(&s); hand(0, 0, 0); hand(1, 0, 0); in = at(9000, 0); oot_boom_step(&s, &in, 1, 0, &out);
    hand(1, 1, 0); in = at(9010, 0); oot_boom_step(&s, &in, 1, 0, &out); in = at(9720, 0); oot_boom_step(&s, &in, 1, 0, &out); assert(s.phase == OOT_BOOM_IDLE && s.hand < 0);
    /* not active: no mask */
    in = at(9800, 0); oot_boom_step(&s, &in, 0, 0, &out); assert(!out.b.mask && !out.b.add);
    puts("boomerang: ok");
    return 0;
}
