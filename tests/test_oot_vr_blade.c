/* A swing of the held sword, told from the blade's movement (zeldaVR/runtime/oot_vr_blade.h). */
#include <assert.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_blade.h"

#define MS 1000000ull
static oot_stroke s;
static uint64_t t;
static float p[3];
static const float SPEED = 2.6f, ARC = 0.25f;
static const float up[3] = { 0, 1, 0 };

static void reset(void) { memset(&s, 0, sizeof s); t = 5000 * MS; p[0] = p[1] = p[2] = 0; oot_stroke_step(&s, p, up, t, SPEED, ARC); }
/* Moves the point for `ms` at `v` m/s along d (a unit vector), a sample every 14 ms. Returns the swings begun and the ends seen. */
static int begun, ended;
static void move(const float d[3], float v, unsigned ms, const float *axis) {
    for (unsigned k = 0; k < ms; k += 14) {
        t += 14 * MS;
        for (int i = 0; i < 3; i++) p[i] += d[i] * v * 0.014f;
        const int r = oot_stroke_step(&s, p, axis, t, SPEED, ARC);
        if (r > 0) begun++; else if (r < 0) ended++;
    }
}

int main(void) {
    const float right[3] = { 1, 0, 0 }, left[3] = { -1, 0, 0 }, down[3] = { 0, -1, 0 };

    /* a hand that holds still, or moves slowly, never swings */
    reset(); begun = ended = 0; move(right, 0, 500, up); assert(!begun && !s.active);
    move(right, 1.0f, 1000, up); assert(!begun);                    /* a metre at 1 m/s */
    move(right, 2.0f, 600, up); assert(!begun);                     /* fast enough to count as moving, never as fast as a swing */

    /* a swing: begins once the blade has gone its arc, once; ends when it slows */
    reset(); begun = ended = 0;
    move(right, 4.0f, 42, up); assert(!begun);                      /* 17 cm: not yet */
    move(right, 4.0f, 42, up); assert(begun == 1 && s.active && s.number == 1 && !s.thrust);
    assert(oot_stroke_live(&s, t));
    move(right, 4.0f, 100, up); assert(begun == 1);                 /* still the same swing */
    move(right, 0.5f, 28, up); assert(ended == 1 && !s.active && !oot_stroke_live(&s, t));

    /* the same reading sent twice between two real ones (two passes of one frame) does not end the swing */
    reset(); begun = ended = 0;
    for (int k = 0; k < 8; k++) {
        t += 11 * MS; p[0] += 4.0f * 0.022f; int r = oot_stroke_step(&s, p, up, t, SPEED, ARC); if (r > 0) begun++; else if (r < 0) ended++;
        t += 11 * MS; r = oot_stroke_step(&s, p, up, t, SPEED, ARC); if (r > 0) begun++; else if (r < 0) ended++;
    }
    assert(begun == 1 && ended == 0 && s.active);
    t += 250 * MS; assert(!oot_stroke_live(&s, t));                 /* and a hand whose readings stop ends it by silence */

    /* a short flick is none, however fast */
    reset(); begun = 0; move(right, 6.0f, 28, up); move(right, 0, 100, up); assert(!begun);

    /* a wiggle: every turn back starts the count again */
    reset(); begun = 0;
    for (int k = 0; k < 6; k++) { move(right, 5.0f, 28, up); move(left, 5.0f, 28, up); }
    assert(!begun);

    /* a forehand and at once a backhand: two swings */
    reset(); begun = ended = 0;
    move(right, 4.0f, 140, up); assert(begun == 1);
    move(left, 4.0f, 140, up); assert(begun == 2 && ended == 1 && s.number == 2);

    /* along the blade: tip first is a thrust, tip trailing is pulling the sword back and never a swing */
    reset(); begun = 0; move(up, 4.0f, 140, up); assert(begun == 1 && s.thrust);
    reset(); begun = 0; move(down, 4.0f, 140, up); assert(!begun);
    reset(); begun = 0; move(down, 4.0f, 140, NULL); assert(begun == 1);   /* the blade's direction not known: any way counts */

    /* the attack stays on 300 ms at most, and not past a silence */
    reset(); begun = 0; move(right, 4.0f, 98, up); assert(begun == 1);
    assert(oot_stroke_live(&s, t) && !oot_stroke_live(&s, t + 201 * MS));                    /* no sample for 201 ms */
    move(right, 4.0f, 280, up); assert(s.active && begun == 1 && !oot_stroke_live(&s, t));   /* a long swing: the attack stops after 300 ms */
    t += 250 * MS; p[0] += 1.0f; assert(oot_stroke_step(&s, p, up, t, SPEED, ARC) == -1 && !s.active);   /* the gap ended it; the jump is no swing */
    move(right, 0, 28, up); assert(begun == 1);

    /* what the game is told about the attack */
    uint8_t info[OOT_ATTACK_INFO_BYTES]; uint32_t w; float f; uint64_t q;
    oot_attack_info(2, OOT_ATTACK_SLASH, 1.0f, 1, info);
    memcpy(&w, info, 4); assert(w == 2); memcpy(&w, info + 4, 4); assert(w == 13); memcpy(&w, info + 8, 4); assert(w == 0);
    memcpy(&f, info + 12, 4); assert(f == 1.0f); memcpy(&q, info + 16, 8); assert(q == 1); memcpy(&w, info + 24, 4); assert(w == 18);
    memcpy(&w, info + 28, 4); assert(w == 0);
    puts("a swing of the sword: its arc and speed, once a stroke, none for a flick, a wiggle or a pull back");
    return 0;
}
