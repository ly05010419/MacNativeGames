/* Bombs in the player's hands (zeldaVR/runtime/oot_vr_bomb.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_bomb.h"

#define MS 1000000ull
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)
static oot_ih_frame frame;
static const float feet[3] = { 0, 0, 0 }, upright[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
static oot_bomb_opts opts = { 0, 1.2f, 9.81f, 1, 4.0f, 20 * 0.0174533f, 1, 0, 1, 1 };
static void hand(int i, float grip, float trigger, float x, float y, float z) {
    oot_ih_hand *h = &frame.hand[i];
    h->have = 1; h->grip = grip; h->trigger = trigger; h->f = 0.3f; h->u = -0.3f; h->r = i ? 0.2f : -0.2f;
    h->pos[0] = x; h->pos[1] = y; h->pos[2] = z; h->dir[2] = -1;
    h->palm[0] = 0; h->palm[1] = 0; h->palm[2] = -1;   /* palms ahead */
}
static oot_ih_in at(uint64_t t, uint32_t action) { oot_ih_in in = { t * MS, &frame, 2, action, 1, 0 }; return in; }

int main(void) {
    assert(oot_bomb_fruit_name("Obj_BombFlower_A_01") && oot_bomb_fruit_name("Obj_BombFlower_Fruit_X") && !oot_bomb_fruit_name("Obj_BombFlower_A_03") && !oot_bomb_fruit_name("Obj_BombFlower_Grass_A"));
    float c[3]; const float origin[3] = { 1, 2, 3 }; oot_bomb_centre(origin, upright, c); assert(NEAR(c[1], 2 + OOT_BOMB_UP));

    oot_bombs s; oot_bombs_init(&s); oot_bombs_out o; oot_bomb_cmd cmd;
    hand(0, 0, 0, -0.3f, 1.0f, -0.3f); hand(1, 0, 0, 0.3f, 1.0f, -0.3f);
    oot_ih_in in = at(0, 0); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert((o.b.mask & OOT_IH_ZR) && !o.b.add);
    /* a fresh grip with nothing near takes one from the bag: ZR for 150 ms */
    hand(1, 1, 0, 0.3f, 1.0f, -0.3f); in = at(10, 0); oot_bombs_step(&s, &in, &opts, 1, feet, &o);
    assert(o.event == OOT_BOMB_EV_TAKE && (o.b.add & OOT_IH_ZR) && s.phase == OOT_BOMB_OUT_TAKE);
    in = at(170, 0x06000000); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(!(o.b.add & OOT_IH_ZR));
    /* the new bomb's AI runs: held in that hand at once, upright against the palm; his lift is let go after 600 ms */
    const float lifted[3] = { 0, 1.6f, 0 }; const int b0 = oot_bombs_seen(&s, 0x1000, lifted, 200 * MS, 0, 1); assert(b0 == 0);
    in = at(210, 0x06000000); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(s.b[0].held == 2 && s.b[0].owned && s.phase == OOT_BOMB_OUT_THROW && o.b.triggers == 2);
    oot_bomb_own(&s, 0, &frame, upright, feet, 0x77, 1, 220 * MS, &opts, &cmd); assert(!cmd.place && cmd.frame && cmd.sleep == 1 && cmd.layer == OOT_BOMB_LAYER_NONE);   /* the lift has it */
    s.b[0].seen = 690 * MS; in = at(700, 0x06000000); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(!o.fall);
    s.b[0].seen = 810 * MS; in = at(820, 0x06000000); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(o.fall && s.phase == OOT_BOMB_OUT_CATCH);
    s.b[0].seen = 1020 * MS; in = at(1030, 0); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(s.phase == OOT_BOMB_OUT_NONE && !o.fall);
    /* now placed in the palm, eased in over 100 ms, with the hand's velocity */
    frame.hand[1].vel[0] = 2; s.b[0].seen = 1040 * MS;
    oot_bomb_own(&s, 0, &frame, upright, feet, 0x77, 0, 1040 * MS, &opts, &cmd); assert(cmd.place && cmd.velocity && NEAR(cmd.v[0], 2) && !cmd.sleep);
    oot_bomb_own(&s, 0, &frame, upright, feet, 0x77, 0, 1200 * MS, &opts, &cmd);
    assert(NEAR(cmd.origin[2], -0.3f - 0.25f) && NEAR(cmd.origin[1], 1.0f - OOT_BOMB_UP) && NEAR(cmd.origin[0], 0.3f));
    oot_bomb_own(&s, 0, &frame, upright, feet, 0x77, 0, 1210 * MS, &opts, &cmd);
    /* the holding hand's trigger lights it: the fuse runs, its effects wake */
    hand(1, 1, 0.9f, 0.3f, 1.0f, -0.3f); frame.hand[1].vel[0] = 2; s.b[0].seen = 1290 * MS; in = at(1300, 0); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(o.event == OOT_BOMB_EV_LIT && s.b[0].lit);
    s.b[0].seen = 1310 * MS; oot_bomb_own(&s, 0, &frame, upright, feet, 0x77, 0, 1310 * MS, &opts, &cmd); assert(!cmd.frame && cmd.sleep == -1);
    /* let go: thrown at the hand's last speed x 1.2 plus Link's */
    hand(1, 0, 0, 0.3f, 1.0f, -0.3f); s.b[0].seen = 1390 * MS; in = at(1400, 0); oot_bombs_step(&s, &in, &opts, 1, feet, &o);
    assert(o.event == OOT_BOMB_EV_THROWN && !s.b[0].held && s.b[0].flying && NEAR(s.b[0].fv[0], 2.4f) && NEAR(o.event_speed, 2.4f));
    /* its flight: the velocity on a real fall until it hits something; layer HitOnlyGround near Link for a second */
    s.b[0].seen = 1410 * MS; oot_bomb_own(&s, 0, &frame, upright, feet, 0, 0, 1410 * MS, &opts, &cmd); assert(cmd.velocity && NEAR(cmd.v[0], 2.4f) && cmd.layer == OOT_BOMB_LAYER_GROUND);
    float here[3]; memcpy(here, s.b[0].centre, 12); here[0] += 2.4f * 0.02f;
    oot_bombs_seen(&s, 0x1000, here, 1430 * MS, 0, 1); oot_bomb_own(&s, 0, &frame, upright, feet, 0, 0, 1430 * MS, &opts, &cmd);
    assert(cmd.velocity && NEAR(cmd.v[1], -9.81f * 0.02f) && s.b[0].flying);
    oot_bombs_seen(&s, 0x1000, here, 1550 * MS, 0, 1); oot_bomb_own(&s, 0, &frame, upright, feet, 0, 0, 1550 * MS, &opts, &cmd); assert(!s.b[0].flying && !cmd.velocity);   /* stopped: it hit */
    oot_bombs_seen(&s, 0x1000, here, 2500 * MS, 0, 1); oot_bomb_own(&s, 0, &frame, upright, feet, 0, 0, 2500 * MS, &opts, &cmd); assert(cmd.layer == OOT_BOMB_LAYER_OWN);
    /* a loose bomb of ours within 0.25 m of a closed grip is held as gripped (the catch) */
    hand(0, 1, 0, here[0] - 0.2f, here[1], here[2]); in = at(2510, 0); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(s.b[0].held == 1 && o.event == OOT_BOMB_EV_HELD);
    /* the squeeze-again mode: letting go keeps it, the next squeeze throws */
    opts.throw_mode = 1; hand(0, 0, 0, 0, 1, 0); in = at(2600, 0); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(s.b[0].held == 1);
    hand(0, 1, 0, 0, 1, 0); in = at(2700, 0); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(!s.b[0].held && o.event == OOT_BOMB_EV_THROWN);
    opts.throw_mode = 0;
    /* a game's bomb (not ours) needs the bracelet */
    oot_bombs_init(&s); hand(0, 0, 0, 0, 1, 0); hand(1, 0, 0, 1, 1, 0);
    const float loose[3] = { 0.1f, 1, 0 }; oot_bombs_seen(&s, 0x2000, loose, 3000 * MS, 0, 1);
    hand(0, 1, 0, 0, 1, 0); in = at(3000, 0); oot_bombs_step(&s, &in, &opts, 0, feet, &o); assert(!s.b[0].held && !o.b.mask);
    opts.bracelet = 1; hand(0, 0, 0, 0, 1, 0); in = at(3010, 0); oot_bombs_step(&s, &in, &opts, 0, feet, &o);
    hand(0, 1, 0, 0, 1, 0); in = at(3020, 0); oot_bombs_step(&s, &in, &opts, 0, feet, &o); assert(s.b[0].held == 1);
    /* the pull: an open hand facing a bomb out of reach rings it; within the cone, out of reach, within the pull's reach */
    oot_bombs_init(&s); hand(0, 0, 0, 0, 1, 0); hand(1, 0, 0, 1, 1, 0);
    const float far[3] = { 0, 1, -2 }, aside[3] = { 1.5f, 1, -2 }, too_far[3] = { 0, 1, -5 };
    oot_bombs_seen(&s, 0x3000, far, 4000 * MS, 0, 1); oot_bombs_seen(&s, 0x3001, aside, 4000 * MS, 0, 1); oot_bombs_seen(&s, 0x3002, too_far, 4000 * MS, 0, 1);
    for (int j = 0; j < 3; j++) s.b[j].owned = 1;
    in = at(4000, 0); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(s.ring[0] == 1);
    assert(oot_bomb_ring(&s, 0, &frame.hand[0], feet, &opts, 0) == 1);
    frame.hand[0].palm[0] = 0.60f; frame.hand[0].palm[2] = -0.80f;   /* turned about 37 degrees: the one aside is nearer the line */
    assert(oot_bomb_ring(&s, 0, &frame.hand[0], feet, &opts, 0) == 2);
    frame.hand[0].palm[0] = 0.26f; frame.hand[0].palm[2] = -0.966f;  /* 15 degrees: the first still in its cone */
    assert(oot_bomb_ring(&s, 0, &frame.hand[0], feet, &opts, 1) == 1);
    frame.hand[0].palm[0] = 0; frame.hand[0].palm[2] = -1;
    hand(0, 1, 0, 0, 1, 0); in = at(4010, 0); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(s.b[0].pulled == 1 && o.event == OOT_BOMB_EV_PULL);
    oot_bomb_own(&s, 0, &frame, upright, feet, 0, 0, 4020 * MS, &opts, &cmd);
    assert(cmd.place && cmd.layer == OOT_BOMB_LAYER_NONE && NEAR(cmd.v[2], 9.0f) && cmd.origin[2] < -1.8f && cmd.origin[2] > -2.0f);
    const float close[3] = { 0, 1, -0.4f }; oot_bombs_seen(&s, 0x3000, close, 4200 * MS, 0, 1);
    oot_bomb_own(&s, 0, &frame, upright, feet, 0, 0, 4200 * MS, &opts, &cmd); assert(s.b[0].held == 1 && !s.b[0].pulled && cmd.event == OOT_BOMB_EV_HELD);
    /* a pull that never arrives is dropped after 1.5 s; one let go flies on */
    s.b[1].pulled = 2; s.b[1].pull_at = 4000 * MS; oot_bomb_own(&s, 1, &frame, upright, feet, 0, 0, 5600 * MS, &opts, &cmd); assert(!s.b[1].pulled && cmd.event == OOT_BOMB_EV_DROPPED);
    /* a bomb not updated for 400 ms is gone */
    in = at(4700, 0); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(!s.b[2].actor);
    /* a Bomb Flower: a fresh grip at the fruit on its plant asks the game to pick it (A); his lift gives the fruit to the hand */
    oot_bombs_init(&s); opts.bracelet = 1; hand(0, 0, 0, 0, 1, -0.4f); hand(1, 0, 0, 1, 1, 0);
    const float fruit[3] = { 0, 0.9f, -0.5f }; oot_bombs_seen(&s, 0x4000, fruit, 6000 * MS, 1, 0); assert(s.b[0].on_plant);
    hand(0, 1, 0, 0, 1, -0.4f); in = at(6000, 0); oot_bombs_step(&s, &in, &opts, 0, feet, &o); assert(o.event == OOT_BOMB_EV_PICK && (o.b.add & OOT_IH_A) && !s.b[0].held);
    const float raised[3] = { 0, 1.5f, 0 }; oot_bombs_seen(&s, 0x4000, raised, 6200 * MS, 1, 1); assert(!s.b[0].on_plant && s.b[0].lit);
    in = at(6210, 0x06000000); oot_bombs_step(&s, &in, &opts, 0, feet, &o); assert(o.event == OOT_BOMB_EV_PICKED && s.b[0].held == 1);
    s.b[0].seen = 6400 * MS; in = at(6400, 0x06000000); oot_bombs_step(&s, &in, &opts, 0, feet, &o); assert(!o.fall);
    in = at(6420, 0x06000000); oot_bombs_step(&s, &in, &opts, 0, feet, &o); assert(o.fall);   /* a flower: let go 200 ms into his lift, and every 200 ms */
    in = at(6500, 0x06000000); oot_bombs_step(&s, &in, &opts, 0, feet, &o); assert(!o.fall);
    in = at(6630, 0x06000000); oot_bombs_step(&s, &in, &opts, 0, feet, &o); assert(o.fall);
    s.b[0].seen = 6640 * MS; oot_bomb_own(&s, 0, &frame, upright, feet, 0, 1, 6640 * MS, &opts, &cmd); assert(!cmd.place && !cmd.frame);   /* carried: the lift's */
    /* without the bracelet, or with flowers off, no pick */
    oot_bombs_init(&s); opts.bracelet = 0; oot_bombs_seen(&s, 0x4001, fruit, 7000 * MS, 1, 0); hand(0, 0, 0, 0, 1, -0.4f);
    in = at(7000, 0); oot_bombs_step(&s, &in, &opts, 0, feet, &o); hand(0, 1, 0, 0, 1, -0.4f); in = at(7010, 0); oot_bombs_step(&s, &in, &opts, 0, feet, &o); assert(o.event != OOT_BOMB_EV_PICK);
    /* the game sets an unlit bomb of ours off (its animation's command changes): lit, woken */
    oot_bombs_init(&s); oot_bombs_seen(&s, 0x5000, loose, 8000 * MS, 0, 1); s.b[0].owned = 1;
    oot_bomb_own(&s, 0, NULL, upright, feet, 0x11, 0, 8000 * MS, &opts, &cmd); assert(cmd.frame && cmd.sleep == 1);
    oot_bomb_own(&s, 0, NULL, upright, feet, 0x11, 0, 8100 * MS, &opts, &cmd); assert(cmd.frame && !s.b[0].lit);
    oot_bomb_own(&s, 0, NULL, upright, feet, 0x22, 0, 8200 * MS, &opts, &cmd); assert(s.b[0].lit && cmd.event == OOT_BOMB_EV_LIT && !cmd.frame && cmd.sleep == -1);
    /* without the fuse hold the trigger has nothing to light: nothing held back */
    oot_bombs_init(&s); opts.fuse_hold = 0; oot_bombs_seen(&s, 0x5001, loose, 9000 * MS, 0, 1); s.b[0].owned = 1;
    oot_bomb_own(&s, 0, NULL, upright, feet, 0x11, 0, 9000 * MS, &opts, &cmd); assert(!cmd.frame && !cmd.sleep);
    /* the throw is at most 30 m/s; no hands: nothing held */
    oot_bombs_init(&s); oot_bombs_seen(&s, 0x6000, loose, 10000 * MS, 0, 1); s.b[0].owned = 1; s.b[0].held = 1;
    for (int k = 0; k < 3; k++) { s.b[0].vs[k][0] = 100; } s.b[0].vn = 3;
    hand(0, 0, 0, 0, 1, 0); in = at(10000, 0); oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(NEAR(s.b[0].fv[0], 30));
    s.b[0].held = 1; in.hands = NULL; oot_bombs_step(&s, &in, &opts, 1, feet, &o); assert(!s.b[0].held && !o.b.mask);
    /* Link's velocity from his feet */
    oot_bombs_init(&s); const float f0[3] = { 0, 0, 0 }, f1[3] = { 0.1f, 0, 0 };
    oot_bombs_link(&s, f0, 0); for (int k = 1; k <= 20; k++) { const float fk[3] = { 0.05f * (float)k, 0, 0 }; oot_bombs_link(&s, fk, (uint64_t)k * 50 * MS); }
    assert(s.link_v[0] > 0.9f && s.link_v[0] < 1.01f); (void)f1;
    puts("bombs: ok");
    return 0;
}
