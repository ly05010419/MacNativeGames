/* Ocarina of Time in VR: a swing of the sword the player holds, told from the blade's own movement in the room, without the
   runtime (tests/test_oot_vr_blade.c runs it). The user, 2026-11-23: "不要 原来的出招节奏了 只要 模型剑 碰到怪物 就算击中，
   原来的 游戏动画 也不要了". While a swing lasts the game's own attack is kept on Link's hit sensor
   (games/oot/runtime/oot_vr_sword.h), so what the blade passes through is hit by the game's own rules, with no button and
   none of his attack animations.

   A swing is a run of movement of one point of the blade (up to OOT_STROKE_POINT metres from the hand, in the room, so
   that Link's running and turning are no swing): faster than `speed` at its peak, `arc` metres gone one way, most of that
   in a line (a wiggle is none), and not a pull back along the blade with the tip trailing. It is over when the point slows
   under OOT_STROKE_KEEP or turns back; the next may begin at once (a backhand after a forehand).
   The idea of measuring the stroke at the blade and these rules follow HookshotVR's description of its sword
   (vr/items/sword/blade_hits.inc, GPL-3: ideas only, no code). Times in nanoseconds, lengths in metres. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

static const float OOT_STROKE_POINT = 0.6f;       /* how far along the blade the measured point is, at most */
static const float OOT_STROKE_KEEP = 1.2f;        /* m/s: slower than this, the run of movement is over */
static const float OOT_STROKE_PULL = 0.6f;        /* a run that starts this much against the blade's direction is a pull back */
static const float OOT_STROKE_STRAIGHT = 0.6f;    /* the straight distance must be this much of the path */
static const float OOT_STROKE_THRUST = 0.75f;     /* a swing this much along the blade, tip first, is a thrust */
enum { OOT_STROKE_SPEED_DEFAULT = 26, OOT_STROKE_ARC_DEFAULT = 25 };   /* tenths of m/s; cm */
static const uint64_t OOT_STROKE_GAP_NS = 200000000ull;   /* no sample for this long: start over */
static const uint64_t OOT_BLADE_MAX_NS = 300000000ull;    /* the attack stays on this long at most for one swing */

typedef struct {
    int have, active, pull, thrust;
    unsigned number;            /* swings so far */
    uint64_t at, began_at;
    float last[3], from[3], way[3];
    float path, peak, speed;
} oot_stroke;

/* One sample of the blade's point; axis: the blade's direction, tip first, a unit vector (NULL: not known).
   1: a swing has just begun, -1: one has just ended, 0: nothing new. */
static inline int oot_stroke_step(oot_stroke *s, const float point[3], const float *axis, uint64_t now, float speed, float arc) {
    if (!s->have || now < s->at || now - s->at > OOT_STROKE_GAP_NS) {
        const int ended = s->active ? -1 : 0;
        s->have = 1; s->active = 0; s->path = s->peak = s->speed = 0; s->pull = 0;
        memcpy(s->last, point, sizeof s->last); s->at = now;
        return ended;
    }
    const float dt = (float)(now - s->at) / 1e9f;
    if (dt < 0.001f) return 0;
    /* the very same place again is the same reading sent twice, not a hand that stopped: a tracked hand never holds still to
       the last bit. (A hand whose tracking froze ends the swing by its silence: oot_stroke_live.) */
    if (!memcmp(point, s->last, sizeof s->last)) return 0;
    const float d[3] = { point[0] - s->last[0], point[1] - s->last[1], point[2] - s->last[2] };
    const float gone = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    int result = 0;
    s->speed = gone / dt;
    if (s->speed < OOT_STROKE_KEEP || gone < 1e-5f) {
        if (s->active) result = -1;
        s->active = 0; s->path = s->peak = 0;
    } else {
        const float u[3] = { d[0] / gone, d[1] / gone, d[2] / gone };
        if (s->path > 0 && u[0] * s->way[0] + u[1] * s->way[1] + u[2] * s->way[2] < 0) {   /* it turned back: a new run */
            if (s->active) result = -1;
            s->active = 0; s->path = s->peak = 0;
        }
        if (s->path == 0) {
            memcpy(s->from, s->last, sizeof s->from); memcpy(s->way, u, sizeof s->way);
            const float along = axis ? u[0] * axis[0] + u[1] * axis[1] + u[2] * axis[2] : 0;
            s->pull = along <= -OOT_STROKE_PULL;
            s->thrust = along >= OOT_STROKE_THRUST;
        } else {   /* the way bends with an arc */
            float w[3] = { s->way[0] + u[0], s->way[1] + u[1], s->way[2] + u[2] };
            const float n = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            if (n > 1e-6f) { w[0] /= n; w[1] /= n; w[2] /= n; memcpy(s->way, w, sizeof w); } else memcpy(s->way, u, sizeof u);
        }
        s->path += gone;
        if (s->speed > s->peak) s->peak = s->speed;
        const float c[3] = { point[0] - s->from[0], point[1] - s->from[1], point[2] - s->from[2] };
        const float straight = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        if (!s->active && !s->pull && s->path >= arc && s->peak >= speed && straight >= OOT_STROKE_STRAIGHT * s->path) {
            s->active = 1; s->number++; s->began_at = now; result = 1;
        }
    }
    memcpy(s->last, point, sizeof s->last); s->at = now;
    return result;
}

/* Is the game's attack to be on now: during a swing, for OOT_BLADE_MAX_NS at most, and only with a fresh sample. */
static inline int oot_stroke_live(const oot_stroke *s, uint64_t now) {
    return s->active && now >= s->began_at && now - s->began_at <= OOT_BLADE_MAX_NS && now - s->at <= OOT_STROKE_GAP_NS;
}

/* What the game is told about one attack of Link's hit sensor: 32 bytes (HookshotVR's notes on the game's own node,
   checked against the game's own swings in our log before it is used). direction 2 and kind 13 are the one-handed
   slash's; the sensors' mask 1 is the weapon's alone. */
enum { OOT_ATTACK_INFO_BYTES = 32, OOT_ATTACK_SLASH = 13, OOT_ATTACK_JUMP = 22, OOT_ATTACK_STICK = 101 };
static inline void oot_attack_info(uint32_t direction, uint32_t kind, float rate, uint64_t sensors, uint8_t out[OOT_ATTACK_INFO_BYTES]) {
    const uint32_t zero = 0, eighteen = 18;
    memset(out, 0, OOT_ATTACK_INFO_BYTES);
    memcpy(out + 0, &direction, 4); memcpy(out + 4, &kind, 4); memcpy(out + 8, &zero, 4); memcpy(out + 12, &rate, 4);
    memcpy(out + 16, &sensors, 8); memcpy(out + 24, &eighteen, 4);
}
