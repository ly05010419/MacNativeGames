/* What Link holds, laid on the player's hand, for Ocarina of Time, without the runtime (tests/test_oot_vr_hold.c runs it).
   Link is left-handed: the game puts his sword or stick in his left hand and his shield on his right arm. A right-handed player
   has them the other way round, so each thing goes to the player's other hand; in left-handed mode it stays.
   A hand's grip frame: x across the palm, y along the fingers, z toward the index finger, origin in the palm. The two hands'
   frames mirror each other with x reversed, so a handle at (a, b, c) in one palm is at (-a, b, c) in the other. Only the place
   is mirrored, never the thing. The shield's pieces do not share one origin, so it is turned as one body, half a turn about the
   grip's z, which brings it from the outside of one forearm to the outside of the other. The idea of the grip frames follows
   OcarinaVR's description (ideas only, GPL-3). */
#pragma once
#include <stdint.h>
#include "oot_vr_pose.h"

static inline void oot_hold_inverse(const float m[12], float out[12]) {
    const float r[12] = { m[0], m[4], m[8], 0, m[1], m[5], m[9], 0, m[2], m[6], m[10], 0 };
    memcpy(out, r, sizeof r);
    for (int i = 0; i < 3; i++) out[4 * i + 3] = -(r[4 * i] * m[3] + r[4 * i + 1] * m[7] + r[4 * i + 2] * m[11]);
}
/* A hand's grip frame in the rest pose, as a 3x4 whose columns are its axes and origin. 0: the hand has no usable frame. */
static inline int oot_hold_grip_rest(const oot_arm_rest *r, float out[12]) {
    float along[3], across[3], axes[3][3];
    if (!oot_unit(r->wrist, r->finger[2][0], along)) return 0;
    for (int i = 0; i < 3; i++) across[i] = r->finger[1][0][i] - r->finger[4][0][i];
    if (!oot_frame(along, across, axes)) return 0;
    const float dx = r->finger[2][0][0] - r->wrist[0], dy = r->finger[2][0][1] - r->wrist[1], dz = r->finger[2][0][2] - r->wrist[2];
    const float palm = 0.62f * sqrtf(dx * dx + dy * dy + dz * dz);
    for (int i = 0; i < 3; i++) {
        out[4 * i] = axes[2][i]; out[4 * i + 1] = axes[0][i]; out[4 * i + 2] = axes[1][i];
        out[4 * i + 3] = r->wrist[i] + along[i] * palm;
    }
    return 1;
}
/* The player's hand for a thing in Link's hand `side` (0 left, 1 right). */
static inline int oot_hold_hand(int side, int left_handed) { return left_handed ? side : 1 - side; }
/* How it changes hands: 0 it stayed; 1 a thing with a handle, its place mirrored; 2 the shield on his right arm, turned as one body. */
enum { OOT_HOLD_SAME, OOT_HOLD_MIRROR, OOT_HOLD_TURN };
static inline int oot_hold_mirrors(int side, int to, int aiming) { return to == side ? OOT_HOLD_SAME : side == 0 || aiming ? OOT_HOLD_MIRROR : OOT_HOLD_TURN; }
/* Which of his hands a thing is in: the one it keeps its distance to while the rest of him moves. Within 12 cm of a wrist, and nearer
   it than his chest, it is held at once (what hangs on his back is nearer the chest, even as the game's hand passes it); up to 40 cm (a sword's model has its origin along the blade) when that distance is steady, steadier than the distance to
   his chest, or as steady and nearer. One step a frame, with the distances to the left wrist, right wrist and chest. */
typedef struct { uint64_t size, vb, frame; float last[3], jitter[3], held_at; unsigned frames; int side; } oot_hold_watch;
static inline void oot_hold_watch_step(oot_hold_watch *w, const float d[3]) {
    if (!w->frames) w->side = -1;
    else for (int i = 0; i < 3; i++) w->jitter[i] += (fabsf(d[i] - w->last[i]) - w->jitter[i]) * 0.15f;
    const int first = !w->frames;
    memcpy(w->last, d, sizeof w->last); w->frames++;
    if (w->side >= 0 && fabsf(d[w->side] - w->held_at) < 0.04f) return;   /* it stays in that hand: a thing in a hand keeps its distance */
    w->side = -1;
    const int s = d[0] <= d[1] ? 0 : 1;
    if (d[s] < 0.12f && d[s] < d[2]) { w->side = s; w->held_at = d[s]; return; }
    if (d[2] < 0.17f && d[2] < d[s]) return;   /* on his body */
    if (first || d[s] >= 0.40f || w->frames < 12 || w->jitter[s] >= 0.0012f) return;
    if (w->jitter[s] < 0.5f * w->jitter[2] || d[s] < d[2]) { w->side = s; w->held_at = d[s]; }
}
/* The opacity the game gives a faded model; some materials hold 0 there at all times, which is not an opacity. */
static inline int oot_hold_faded(float opacity) { return opacity > 0.02f && opacity < 0.999f; }
/* How much of the way the string is drawn: nothing up to 30 % of the reach, all of it at the full reach. */
static inline float oot_hold_drawn(float far, float reach) {
    if (reach <= 0) return 0;
    return fmaxf(0, fminf(1, (far - 0.3f * reach) / (0.7f * reach)));
}
/* A bone's share of the drawing hand: 0 at the tip it hangs from, 1 at the pouch or nock, in proportion between. */
static inline float oot_hold_share(const float bone[3], const float tip[3], const float middle[3]) {
    const float a[3] = { bone[0] - tip[0], bone[1] - tip[1], bone[2] - tip[2] }, b[3] = { middle[0] - tip[0], middle[1] - tip[1], middle[2] - tip[2] };
    const float len = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    return len > 1e-5f ? fminf(1, sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) / len) : 0;
}
static inline void oot_hold_blend(const float a[12], const float b[12], float t, float out[12]) {
    for (int i = 0; i < 12; i++) out[i] = a[i] + (b[i] - a[i]) * t;
}
/* The rigid move taking a thing at `root` in the grip `from` to its place in the grip `to`. */
static inline void oot_hold_move(const float from[12], const float to[12], const float root[3], int how, float out[12]) {
    float inverse[12], in_grip[3], grip[12];
    oot_hold_inverse(from, inverse);
    oot_m_apply(inverse, root, in_grip);
    memcpy(grip, to, sizeof grip);
    if (how == OOT_HOLD_MIRROR) for (int i = 0; i < 3; i++) grip[4 * i + 3] -= 2 * in_grip[0] * to[4 * i];
    if (how == OOT_HOLD_TURN) for (int i = 0; i < 3; i++) { grip[4 * i] = -to[4 * i]; grip[4 * i + 1] = -to[4 * i + 1]; }
    oot_m_mul(grip, inverse, out);
}
/* The record of one palette size and vertex buffer. A town draws about 150 such pairs a frame, so the table is far larger and
   a pair seen last frame is never what makes room for a new one. */
enum { OOT_HOLD_WATCHED = 1024, OOT_HOLD_PROBES = 32 };
static inline oot_hold_watch *oot_hold_watch_for(oot_hold_watch *table, uint64_t size, uint64_t vb) {
    const unsigned at = (unsigned)((((vb >> 4) ^ size) * 2654435761ull) >> 16) % OOT_HOLD_WATCHED;
    oot_hold_watch *oldest = &table[at];
    for (unsigned i = 0; i < OOT_HOLD_PROBES; i++) {
        oot_hold_watch *e = &table[(at + i) % OOT_HOLD_WATCHED];
        if (e->size == size && e->vb == vb) return e;
        if (e->frame < oldest->frame) oldest = e;
    }
    memset(oldest, 0, sizeof *oldest);
    oldest->size = size; oldest->vb = vb; oldest->frame = ~0ull;
    return oldest;
}
/* The weapon in his hand is an actor of its own, and the game's attack shape follows that actor. Its model's palette root keeps a
   fixed place against his grip and against the actor, learned once while the game alone poses it. */
typedef struct { float in_grip[12], from_actor[12]; } oot_weapon_fit;
static inline void oot_weapon_fit_learn(const float grip[12], const float root[12], const float actor[12], oot_weapon_fit *out) {
    float inverse[12];
    oot_hold_inverse(grip, inverse); oot_m_mul(inverse, root, out->in_grip);
    oot_hold_inverse(actor, inverse); oot_m_mul(inverse, root, out->from_actor);
}
/* From the game's grip and the player's: the root as the game has it, how it moves, and where the actor goes. */
static inline void oot_weapon_place(const oot_weapon_fit *fit, const float grip_game[12], const float grip_new[12], int how,
                                    float root_game[12], float move[12], float actor[12]) {
    float placed[12], inverse[12];
    const float at[3] = { 0, 0, 0 };
    float root_at[3];
    oot_m_mul(grip_game, fit->in_grip, root_game);
    oot_m_apply(root_game, at, root_at);
    oot_hold_move(grip_game, grip_new, root_at, how, move);
    oot_m_mul(move, root_game, placed);
    oot_hold_inverse(fit->from_actor, inverse);
    oot_m_mul(placed, inverse, actor);
}
