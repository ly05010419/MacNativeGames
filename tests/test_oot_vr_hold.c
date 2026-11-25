/* What Link holds, on the player's hand (zeldaVR/runtime/oot_vr_hold.h). */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_hold.h"

#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

static float det3(const float m[12]) {
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
}
/* A left hand at rest, and the right one its mirror image across x = 0. */
static void hands(oot_arm_rest *left, oot_arm_rest *right) {
    memset(left, 0, sizeof *left);
    left->wrist[0] = -0.3f; left->wrist[1] = 1;
    for (int f = 0; f < OOT_FINGERS; f++) {
        left->finger[f][0][0] = -0.38f; left->finger[f][0][1] = 1; left->finger[f][0][2] = 0.03f - 0.015f * (float)f;
    }
    *right = *left;
    right->wrist[0] = -left->wrist[0];
    for (int f = 0; f < OOT_FINGERS; f++) right->finger[f][0][0] = -left->finger[f][0][0];
}

int main(void) {
    oot_arm_rest l, r; float gl[12], gr[12], back[12], moved[12], p[3];
    hands(&l, &r);
    assert(oot_hold_grip_rest(&l, gl) && oot_hold_grip_rest(&r, gr));
    assert(NEAR(det3(gl), 1) && NEAR(det3(gr), 1));
    /* mirrored hands: along the fingers and toward the index mirror in x; across the palm the x component does not change sign */
    assert(NEAR(gl[1], -gr[1]) && NEAR(gl[6], gr[6]) && NEAR(gl[0], gr[0]) && NEAR(gl[4], -gr[4]));

    /* a right-handed player has Link's left-hand thing in the right hand, and his right-arm thing in the left */
    assert(oot_hold_hand(0, 0) == 1 && oot_hold_hand(1, 0) == 0);
    assert(oot_hold_hand(0, 1) == 0 && oot_hold_hand(1, 1) == 1);
    assert(oot_hold_mirrors(0, 1, 0) == OOT_HOLD_MIRROR);   /* his sword */
    assert(oot_hold_mirrors(1, 0, 0) == OOT_HOLD_TURN);     /* his shield */
    assert(oot_hold_mirrors(1, 0, 1) == OOT_HOLD_MIRROR);   /* the slingshot or bow while the game aims */
    assert(!oot_hold_mirrors(0, 0, 0) && !oot_hold_mirrors(1, 1, 1));  /* it stayed in its hand */

    /* staying in the same hand, unmoved: nothing changes */
    const float root[3] = { -0.33f, 1.05f, 0.02f };
    oot_hold_move(gl, gl, root, 0, moved);
    oot_m_apply(moved, root, p);
    assert(NEAR(p[0], root[0]) && NEAR(p[1], root[1]) && NEAR(p[2], root[2]));

    /* passed to the other hand, mirrored: at (-a, b, c) of that palm, which is the mirror image of where it was */
    oot_hold_move(gl, gr, root, 1, moved);
    oot_m_apply(moved, root, p);
    assert(NEAR(p[0], -root[0]) && NEAR(p[1], root[1]) && NEAR(p[2], root[2]));
    assert(NEAR(det3(moved), 1));                      /* the thing itself is not turned inside out */

    /* the shield: half a turn about the grip's z, as one rigid body: (a, b, c) of one grip is (-a, -b, c) of the other */
    { float in_a[3], in_b[3], other[3];
      oot_hold_inverse(gl, back); oot_m_apply(back, root, in_a);
      oot_hold_move(gl, gr, root, OOT_HOLD_TURN, moved); oot_m_apply(moved, root, other);
      oot_hold_inverse(gr, back); oot_m_apply(back, other, in_b);
      assert(NEAR(in_b[0], -in_a[0]) && NEAR(in_b[1], -in_a[1]) && NEAR(in_b[2], in_a[2]) && NEAR(det3(moved), 1)); }
    /* not mirrored: the same numbers in the other palm */
    float in_left[3], in_right[3];
    oot_hold_inverse(gl, back); oot_m_apply(back, root, in_left);
    oot_hold_move(gl, gr, root, 0, moved);
    oot_m_apply(moved, root, p);
    oot_hold_inverse(gr, back); oot_m_apply(back, p, in_right);
    assert(NEAR(in_left[0], in_right[0]) && NEAR(in_left[1], in_right[1]) && NEAR(in_left[2], in_right[2]));
    assert(NEAR(det3(moved), 1));
    /* which hand: near at once; farther when the distance is steady; on his back it stays on his back */
    { oot_hold_watch w; memset(&w, 0, sizeof w);
      const float near_hand[3] = { 0.08f, 0.5f, 0.3f }; oot_hold_watch_step(&w, near_hand); assert(w.side == 0);
      memset(&w, 0, sizeof w);
      for (int i = 0; i < 40; i++) {   /* a sword: 23 cm from the left wrist, always; the chest's distance changes as he moves */
          const float sword[3] = { 0.227f, 0.5f + 0.02f * (float)(i % 3), 0.33f + 0.02f * (float)(i % 2) };
          oot_hold_watch_step(&w, sword);
          if (i < 10) assert(w.side == -1);
      }
      assert(w.side == 0);
      memset(&w, 0, sizeof w);
      for (int i = 0; i < 40; i++) {   /* on his back: steady against the chest, the hands move */
          const float back[3] = { 0.32f + 0.004f * (float)(i % 2), 0.33f + 0.004f * (float)(i % 3), 0.151f };
          oot_hold_watch_step(&w, back); assert(w.side == -1);
      }
      memset(&w, 0, sizeof w);
      for (int i = 0; i < 40; i++) {   /* the sheath on his back, with the game's hand going past it */
          const float sheath[3] = { 0.40f, 0.14f + 0.05f * (float)(i % 2), 0.151f };
          oot_hold_watch_step(&w, sheath); assert(w.side == -1);
      }
      memset(&w, 0, sizeof w);
      for (int i = 0; i < 40; i++) { const float still[3] = { 0.32f, 0.33f, 0.151f }; oot_hold_watch_step(&w, still); assert(w.side == -1); }   /* nothing moves: the nearer is the chest */
      memset(&w, 0, sizeof w);
      const float held[3] = { 0.1f, 0.5f, 0.3f }, gone[3] = { 0.9f, 0.9f, 0.15f };
      oot_hold_watch_step(&w, held); assert(w.side == 0); oot_hold_watch_step(&w, gone); assert(w.side == -1);   /* put away */
      memset(&w, 0, sizeof w);
      for (int i = 0; i < 20; i++) { const float in_hand[3] = { 0.227f, 0.5f, 0.33f + 0.02f * (float)(i % 2) }; oot_hold_watch_step(&w, in_hand); }
      assert(w.side == 0);
      const float on_back[3] = { 0.32f, 0.5f, 0.151f }; oot_hold_watch_step(&w, on_back); assert(w.side == -1); }   /* back on his back, still within reach of the hand */
    /* a faded model: strictly between; 0 in a block is not an opacity */
    assert(oot_hold_faded(0.2f) && !oot_hold_faded(1) && !oot_hold_faded(0) && !oot_hold_faded(-1));

    /* the string: 0 at the fork's tip, 1 at the pouch, in proportion along it */
    const float tip[3] = { 0, -0.07f, 0.19f }, pouch[3] = { 0.26f, -0.07f, 0.19f }, third[3] = { 0.13f, -0.07f, 0.19f };
    assert(NEAR(oot_hold_share(tip, tip, pouch), 0) && NEAR(oot_hold_share(pouch, tip, pouch), 1) && NEAR(oot_hold_share(third, tip, pouch), 0.5f));
    assert(oot_hold_share(tip, tip, tip) == 0);
    const float past[3] = { 0.40f, -0.07f, 0.19f };
    assert(NEAR(oot_hold_share(past, tip, pouch), 1));          /* never past the pouch */
    assert(oot_hold_drawn(0.03f, 0.2f) == 0 && NEAR(oot_hold_drawn(0.13f, 0.2f), 0.5f) && oot_hold_drawn(0.3f, 0.2f) == 1);
    float holder[12], puller[12], placed[12];
    oot_m_identity(holder); oot_m_identity(puller); puller[3] = 0.1f; puller[7] = -0.05f;
    oot_hold_blend(holder, puller, 0, placed); assert(NEAR(placed[3], 0) && NEAR(placed[7], 0));          /* the wood: the holding hand */
    oot_hold_blend(holder, puller, 1, placed); assert(NEAR(placed[3], 0.1f) && NEAR(placed[7], -0.05f));  /* the pouch: the pulling hand */
    oot_hold_blend(holder, puller, 0.5f, placed); assert(NEAR(placed[3], 0.05f) && NEAR(placed[7], -0.025f) && NEAR(placed[0], 1));  /* straight between */
    /* The town draws about 150 palette and vertex pairs a frame (2026-11-23): each keeps its record from frame to frame. */
    static oot_hold_watch table[OOT_HOLD_WATCHED];
    for (uint64_t frame = 1; frame <= 5; frame++)
        for (uint64_t k = 0; k < 200; k++) {
            oot_hold_watch *w = oot_hold_watch_for(table, 48 * (1 + k % 7), 0x716d000000ull + k * 0x400);
            if (w->frame != frame) { w->frame = frame; w->frames++; }
        }
    for (uint64_t k = 0; k < 200; k++) assert(oot_hold_watch_for(table, 48 * (1 + k % 7), 0x716d000000ull + k * 0x400)->frames == 5);
    /* The weapon: its actor goes where the player's hand takes it, the palette root keeps its place against the actor. */
    const float sword_grip[12] = { 1, 0, 0, 1, 0, 1, 0, 2, 0, 0, 1, 3 }, sword_actor[12] = { 1, 0, 0, 1, 0, 1, 0, 2.1f, 0, 0, 1, 3.2f };
    const float sword_root[12] = { 1, 0, 0, 1, 0, 1, 0, 2.1f, 0, 0, 1, 3.404f };
    const float player_grip[12] = { 0, 0, 1, 1.5f, 0, 1, 0, 2, -1, 0, 0, 3 };   /* a quarter turn about y, half a metre along x */
    oot_weapon_fit fit; float root_game[12], weapon_move[12], actor_at[12];
    oot_weapon_fit_learn(sword_grip, sword_root, sword_actor, &fit);
    oot_weapon_place(&fit, sword_grip, sword_grip, OOT_HOLD_SAME, root_game, weapon_move, actor_at);
    for (int i = 0; i < 12; i++) { assert(NEAR(actor_at[i], sword_actor[i])); assert(NEAR(root_game[i], sword_root[i])); }
    oot_weapon_place(&fit, sword_grip, player_grip, OOT_HOLD_SAME, root_game, weapon_move, actor_at);
    assert(NEAR(actor_at[3], 1.7f) && NEAR(actor_at[7], 2.1f) && NEAR(actor_at[11], 3));      /* 0.2 along the hand's z, 0.1 up */
    assert(NEAR(actor_at[2], 1) && NEAR(actor_at[8], -1) && NEAR(det3(actor_at), 1));         /* turned with the hand, not mirrored */
    puts("OOT held things: which hand, the palm's side mirrored, the shield as it is, never turned inside out");
    return 0;
}
