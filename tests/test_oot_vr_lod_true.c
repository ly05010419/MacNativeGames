/* zeldaVR/runtime/oot_vr_lod_true.h: the register decoder, the x30 proof, the depth sum's check, the veneer's words and
   the distance (vr.md row 18). Instruction words in, register effects out; every word is what `xcrun clang -c` assembles for the instruction named beside it. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../zeldaVR/runtime/oot_vr_lod_true.h"

enum { R = OOT_LT_READ, W = OOT_LT_WRITE, U = OOT_LT_UNKNOWN };
static void gpr(uint32_t w, unsigned reg, unsigned want) {
    const unsigned got = oot_lt_gpr(w, reg);
    if (got != want) { fprintf(stderr, "%08x x%u: %u, not %u\n", w, reg, got, want); assert(0); }
}

static void decoder(void) {
    gpr(0x94000010u, 30, W); gpr(0x94000010u, 19, 0);              /* bl */
    gpr(0x14000010u, 30, 0);                                        /* b */
    gpr(0xd63f0100u, 30, W); gpr(0xd63f0100u, 8, R);                /* blr x8 */
    gpr(0xd63f03c0u, 30, R);                                        /* blr x30 */
    gpr(0xd65f03c0u, 30, U); gpr(0xd61f0100u, 30, U);               /* ret, br x8 */
    gpr(0xd503237fu, 30, U); gpr(0xd50323ffu, 30, U);               /* pacibsp, autibsp */
    gpr(0xd503201fu, 30, 0); gpr(0xd503245fu, 30, 0);               /* nop, bti c */
    gpr(0xd4000001u, 30, U);                                        /* svc #0 */
    gpr(0xa8c17bfdu, 30, W); gpr(0xa8c17bfdu, 29, W);               /* ldp x29, x30, [sp], #16 */
    gpr(0xa9bf7bfdu, 30, R);                                        /* stp x29, x30, [sp, #-16]! */
    gpr(0xf94007c9u, 30, R); gpr(0xf94007c9u, 9, W);                /* ldr x9, [x30, #8] */
    gpr(0xf84207feu, 30, W);                                        /* ldr x30, [sp], #32 */
    gpr(0xf81f0ffeu, 30, R);                                        /* str x30, [sp, #-16]! */
    gpr(0xf87e6928u, 30, R);                                        /* ldr x8, [x9, x30] */
    gpr(0x2d4606a0u, 30, 0); gpr(0x2d4607c0u, 30, R);               /* ldp s0, s1, [x21, #0x30]; [x30, #0x30] */
    gpr(0xbd400bc1u, 30, R); gpr(0xbd400bdeu, 30, R);               /* ldr s1, [x30, #8]; ldr s30, [x30, #8] */
    gpr(0xbd400adeu, 30, 0);                                        /* ldr s30, [x22, #8]: s30 is not x30 */
    gpr(0xf9800100u, 30, 0); gpr(0xf980011eu, 30, U);               /* prfm pldl1keep, [x8]; prfm with Rt 30 */
    gpr(0xaa0303feu, 30, W); gpr(0xaa0303feu, 3, R);                /* mov x30, x3 */
    gpr(0x9100339eu, 30, W); gpr(0x9100339eu, 28, R);               /* add x30, x28, #0xc */
    gpr(0xf2a0001eu, 30, R); gpr(0xd2a0001eu, 30, W);               /* movk x30, #0, lsl #16; movz */
    gpr(0xd37ffb08u, 8, W); gpr(0xd37ffb08u, 24, R);                /* lsl x8, x24, #1 */
    gpr(0x9b1e5108u, 30, R);                                        /* madd x8, x8, x30, x20 */
    gpr(0xb400001eu, 30, R); gpr(0x3600001eu, 30, R);               /* cbz x30 / tbz w30, #0 */
    gpr(0x1e232800u, 30, 0); gpr(0x1f010040u, 30, 0);               /* fadd s0, s0, s3; fmadd s0, s2, s1, s0 */
    gpr(0x1e2703deu, 30, U);                                        /* fmov s30, w30 */
    gpr(0x1e26001eu, 30, U); gpr(0x1e2203c0u, 30, U);               /* fmov w30, s0; scvtf s0, w30 */
    gpr(0x1e02fbc0u, 30, U);                                        /* scvtf s0, w30, #2 (fixed point) */
    gpr(0x4e081fc0u, 30, U);                                        /* ins v0.d[0], x30 */
    gpr(0xd53bd05eu, 30, W);                                        /* mrs x30, tpidr_el0 */
    gpr(0x04000000u, 30, U);                                        /* an SVE word */
    gpr(0xc8dffc1eu, 30, U);                                        /* ldar x30, [x0] */
}

static uint32_t text[256];
static uint32_t visited[256];
static int dead(unsigned at, unsigned n) { const char *why = ""; return oot_lt_x30_dead(text, n, 200, 220, at, visited, 256, 512, &why); }
static uint32_t b_to(unsigned from, unsigned to) { return 0x14000000u | ((to - from) & 0x03ffffffu); }
static uint32_t beq_to(unsigned from, unsigned to) { return 0x54000000u | ((to - from) & 0x7ffffu) << 5; }
static uint32_t cbz_to(unsigned from, unsigned to) { return 0xb4000000u | ((to - from) & 0x7ffffu) << 5 | 8; }

static void proof(void) {
    memset(text, 0, sizeof text);
    /* straight: site, an fmul, a call */
    text[10] = 0x1e232800u; text[11] = 0x1e230821u; text[12] = 0x94000010u;
    assert(dead(10, 200));
    /* the read HookshotVR's main+0x3628350 has: ldr x9, [x30, #8] before any call */
    text[12] = 0xf94007c9u; text[13] = 0x94000010u;
    assert(!dead(10, 200));
    /* both ways of a b.eq: one calls, the other returns (x30 is the caller's: read) */
    text[11] = beq_to(11, 20); text[12] = 0x94000010u; text[20] = 0xd65f03c0u;
    assert(!dead(10, 200));
    text[20] = 0xa8c17bfdu;                                         /* ldp x29, x30: written first */
    assert(dead(10, 200));
    /* a loop with nothing on x30, then a call; and a branch that loops forever */
    text[11] = 0x91000508u; text[12] = cbz_to(12, 11); text[13] = 0x94000010u;
    assert(dead(10, 200));
    text[13] = b_to(13, 11);
    assert(dead(10, 200));
    /* into the data (words 200-219) or off the end */
    text[13] = b_to(13, 205);
    assert(!dead(10, 200));
    text[13] = b_to(13, 230); text[230] = 0x94000010u;              /* the rewriter's patch area after the data */
    assert(dead(10, 256));
    assert(!dead(10, 230));
    /* an instruction the decoder does not take apart */
    text[13] = 0x04000000u;
    assert(!dead(10, 200));
}

/* the depth sums as clang assembles them: snapshot x21, sphere x22 (the same words as main+0x84120..0x8413c on update 131072),
   and snapshot x9, sphere x23 with a mov inside (main+0x363a960..0x363a980) */
static const uint32_t SUM_A[8] = { 0x2d5906a0u, 0x2d400ec2u, 0x1e230821u, 0x1f020400u, 0xbd400ac1u, 0x2d5a0ea2u, 0x1f010040u, 0x1e232800u };
static const uint32_t SUM_B[9] = { 0x2d400ee2u, 0xaa1403f3u, 0x2d590520u, 0x1e230821u, 0x1f020400u, 0xbd400ae1u, 0x2d5a0d22u, 0x1f010040u, 0x1e232800u };
static int site(const uint32_t *words, unsigned n, unsigned d, unsigned s, unsigned o) {
    uint32_t t[32] = { 0 }; memcpy(t + 20 - (n - 1), words, n * 4);
    for (unsigned i = 0; i < 20 - (n - 1); i++) t[i] = 0xd503201fu;
    const char *why = ""; return oot_lt_site_ok(t, 32, 20, d, s, o, &why);
}

static void sums(void) {
    uint32_t w[OOT_LT_SUM_WORDS]; oot_lt_sum_words(21, 22, w);
    for (int i = 0; i < 7; i++) assert(w[i] == SUM_A[i]);
    assert(oot_lt_site_word(0) == 0x1e232800u && oot_lt_site_word(1) == 0x1e232801u);
    assert(site(SUM_A, 8, 0, 21, 22));
    assert(site(SUM_B, 9, 0, 9, 23));
    assert(!site(SUM_A, 8, 0, 21, 23));                             /* other registers */
    assert(!site(SUM_A, 8, 1, 21, 22));                             /* fadd s1 expected */
    uint32_t c[9]; memcpy(c, SUM_B, sizeof c);
    c[1] = 0xaa0003e9u; assert(!site(c, 9, 0, 9, 23));              /* mov x9, x0 inside: the snapshot register changes */
    c[1] = 0x1e2c1003u; assert(!site(c, 9, 0, 9, 23));              /* fmov s3, #0.5 inside */
    memcpy(c, SUM_A, 32); c[4] = SUM_A[6]; c[6] = SUM_A[4];        /* the last fmadd before its loads */
    assert(!site(c, 8, 0, 21, 22));
}

static void veneer(void) {
    uint32_t v[OOT_LT_VENEER_WORDS];
    const uint64_t at = 0x3c46300, body = 0x3c46000, page = 0x86bc000;
    assert(!oot_lt_veneer(1, at, body, page, 1024, v));
    assert(v[0] == 0x1e232801u);
    assert(v[1] == 0xf81f0ff0u && v[5] == 0xf84107f0u && v[10] == 0xf84107f0u);   /* str / ldr x16 */
    assert(v[6] == 0xf81f0ffeu && v[8] == 0xf84107feu);                           /* str / ldr x30 */
    assert(v[9] == 0xd65f03c0u && v[11] == 0xd65f03c0u);
    const int64_t pages = (int64_t)((v[2] >> 29) & 3) | (int64_t)((v[2] >> 5) & 0x7ffff) << 2;
    assert((v[2] & 0x9f00001fu) == 0x90000010u && (((at + 8) >> 12) + pages) << 12 == page);
    assert(v[3] == (0xb9400000u | 256u << 10 | 16u << 5 | 16u));                  /* ldr w16, [x16, #1024] */
    assert((v[4] & 0xff00001fu) == 0x34000010u && ((v[4] >> 5) & 0x7ffff) == 6);  /* cbz w16 to word 10 */
    const int32_t off = (int32_t)(v[7] << 6) >> 6;
    assert((v[7] & 0xfc000000u) == 0x94000000u && at + 28 + (int64_t)off * 4 == body);
    assert(oot_lt_veneer(0, at, at + 28 + (1u << 27), page, 1024, v));            /* the body out of range */
    assert(oot_lt_veneer(0, at, body, page + 8, 1024, v) && oot_lt_veneer(0, at, body, page, 1022, v) && oot_lt_veneer(2, at, body, page, 1024, v));
    assert(oot_lt_site_bl(0x8413c, 0x3c46300) == (0x94000000u | ((0x3c46300u - 0x8413cu) / 4)));
    assert(oot_lt_site_bl(0x1000, 0x1000 + (1u << 27)) == 0);
}

static void distance(void) {
    /* a level view of yaw 0 at (1, 2, 3): rows (1 0 0 | -1), (0 1 0 | -2), (0 0 1 | -3) */
    const float m[12] = { 1, 0, 0, -1, 0, 1, 0, -2, 0, 0, 1, -3 };
    float e[3]; oot_lt_eye(m, e);
    assert(fabsf(e[0] - 1) < 1e-6f && fabsf(e[1] - 2) < 1e-6f && fabsf(e[2] - 3) < 1e-6f);
    /* turned 90 degrees about y: the eye stays */
    const float t[12] = { 0, 0, -1, 3, 0, 1, 0, -2, 1, 0, 0, -1 };
    oot_lt_eye(t, e);
    assert(fabsf(e[0] - 1) < 1e-6f && fabsf(e[1] - 2) < 1e-6f && fabsf(e[2] - 3) < 1e-6f);
    /* a centre 3 m ahead and 4 m aside: depth 3 along the view, distance 5 */
    const float eye[3] = { 0, 0, 0 }, c[3] = { 4, 0, -3 };
    float out = 0;
    assert(oot_lt_depth(-3, c, eye, 0, &out) && fabsf(out + 5) < 1e-6f);
    /* the same with the view moved back 0.5 for the game's sorting: its depth was 3.5, the distance + 0.5 */
    assert(oot_lt_depth(-3.5f, c, eye, 0.5f, &out) && fabsf(out + 5.5f) < 1e-6f);
    /* a turn of the head does not change it: the same centre at depth 5 straight ahead */
    assert(oot_lt_depth(-5, c, eye, 0, &out) && fabsf(out + 5) < 1e-6f);
    /* behind or on the eye's plane, not finite, a negative move back: the game's own */
    assert(!oot_lt_depth(0, c, eye, 0, &out) && !oot_lt_depth(2, c, eye, 0, &out) && !oot_lt_depth(NAN, c, eye, 0, &out));
    const float bad[3] = { INFINITY, 0, 0 };
    assert(!oot_lt_depth(-3, bad, eye, 0, &out) && !oot_lt_depth(-3, c, eye, -1, &out));
    /* nearer than the game's own depth: another view, left alone */
    assert(!oot_lt_depth(-9, c, eye, 0, &out));
    assert(oot_lt_depth(-5.004f, c, eye, 0, &out));                 /* within HookshotVR's 0.1 % */
}

int main(void) {
    decoder(); proof(); sums(); veneer(); distance();
    puts("OOT VR true-distance detail: decoder, x30 proof, depth sums, veneer words and distance");
    return 0;
}
