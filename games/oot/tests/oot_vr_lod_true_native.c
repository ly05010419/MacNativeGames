/* Native arm64 test of the depth sums' veneers (zeldaVR/runtime/oot_vr_lod_true.h) on the shared hook body
   (runtime/oot_vr_hooks.h), driven by oot_vr_lod_true_native.py. Built twice: with -DGEN it prints the body's and the two
   veneers' words, laid out as the script lays them (the body, then veneer 0, then veneer 1); without, it runs a "site":
   `bl <veneer>` in place of `fadd s<D>, s0, s3`, as the installer writes it. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/oot_vr_hooks.h"
#include "../../../zeldaVR/runtime/oot_vr_lod_true.h"

enum { BODY = 0x10000, PAGE = 0x40000, FN_CELL = 120, FLAG_CELL = 1024, VEN0 = BODY + OOT_HOOK_BODY_WORDS * 4, VEN_STEP = OOT_LT_VENEER_WORDS * 4 };

#ifdef GEN
int main(void) {
    uint32_t body[OOT_HOOK_BODY_WORDS], v[OOT_LT_VENEER_WORDS];
    const char *e = oot_hook_body(BODY, PAGE, FN_CELL, body);
    if (e) { fprintf(stderr, "%s\n", e); return 1; }
    printf("body");
    for (int i = 0; i < OOT_HOOK_BODY_WORDS; i++) printf(" %08x", body[i]);
    printf("\n");
    for (unsigned d = 0; d < 2; d++) {
        e = oot_lt_veneer(d, VEN0 + d * VEN_STEP, BODY, PAGE, FLAG_CELL, v);
        if (e) { fprintf(stderr, "%s\n", e); return 1; }
        printf("veneer%u", d);
        for (int i = 0; i < OOT_LT_VENEER_WORDS; i++) printf(" %08x", v[i]);
        printf("\n");
    }
    return 0;
}
#else
extern unsigned char hk_page[4096];
extern void lt_run0(const uint64_t in[96], uint64_t out[100]);
extern void lt_run1(const uint64_t in[96], uint64_t out[100]);
extern char lt_after0[], lt_after1[], lt_ven0[], lt_ven1[];

static oot_hook_regs seen;
static uint64_t seen_return;
static int host_calls;
static const float REPLACED = -42.5f;
static oot_guest_call *host(oot_hook_regs *r, uint64_t stub_return) {
    memcpy(&seen, r, sizeof seen); seen_return = stub_return; host_calls++;
    const unsigned d = stub_return == (uintptr_t)lt_ven1 + OOT_LT_VENEER_RETURN * 4;
    memcpy(r->q[d], &REPLACED, 4);
    return NULL;
}
/* in: x0-x30 at 0..30, q0-q31 at 32.. (two words each); out: the same, then the stack pointer after and before */
static void fill(uint64_t in[96], float s0, float s3) {
    for (int i = 0; i < 31; i++) in[i] = 0x0101010101010101ULL * (uint64_t)(i + 1) + (uint64_t)i;
    for (int r = 0; r < 32; r++) { in[32 + 2 * r] = 0x4040404040404040ULL + (uint64_t)r; in[33 + 2 * r] = 0x5050505050505050ULL + (uint64_t)r; }
    uint32_t b; memcpy(&b, &s0, 4); in[32] = 0x7777777700000000ULL | b; memcpy(&b, &s3, 4); in[32 + 6] = 0x6666666600000000ULL | b;
}
static float low(const uint64_t *q) { float f; uint32_t b = (uint32_t)q[0]; memcpy(&f, &b, 4); return f; }

static void check(int d, int on) {
    uint64_t in[96], out[100];
    const float s0 = 3.25f, s3 = -10.5f;
    fill(in, s0, s3);
    uint32_t flag = (uint32_t)on; memcpy(hk_page + FLAG_CELL, &flag, 4);
    host_calls = 0;
    (d ? lt_run1 : lt_run0)(in, out);
    char *after = d ? lt_after1 : lt_after0;
    assert(host_calls == on);
    assert(out[30] == (uint64_t)(uintptr_t)after);                       /* back at the site's next word, x30 = it */
    for (int i = 0; i < 30; i++) assert(out[i] == in[i]);                 /* x0-x29 as they were, x16 too */
    assert(out[98] == out[99]);                                           /* the stack pointer is the site's again */
    const float sum = s0 + s3;
    for (int r = 0; r < 32; r++) {
        const uint64_t *q = &out[32 + 2 * r];
        if (r == d) { assert(low(q) == (on ? REPLACED : sum)); assert((q[0] >> 32) == 0 && q[1] == 0); continue; }   /* a scalar fadd clears the rest */
        assert(q[0] == in[32 + 2 * r] && q[1] == in[33 + 2 * r]);
    }
    if (on) {
        assert(seen_return == (uintptr_t)(d ? lt_ven1 : lt_ven0) + OOT_LT_VENEER_RETURN * 4);   /* which veneer */
        assert(seen.x[30] == (uint64_t)(uintptr_t)after);                 /* the host tells the site from x30 */
        float got; memcpy(&got, seen.q[d], 4); assert(got == sum);        /* the fadd ran before the host */
        for (int i = 0; i < 30; i++) assert(seen.x[i] == in[i]);
    }
}

int main(void) {
    oot_guest_call *(*fn)(oot_hook_regs *, uint64_t) = host;
    memcpy(hk_page + FN_CELL, &fn, 8);
    for (int d = 0; d < 2; d++) for (int on = 0; on < 2; on++) check(d, on);
    puts("native ARM64 OOT VR depth sums: bl into a veneer runs the site's fadd, skips the body while the flag is 0, otherwise hands the "
         "host every register with x30 naming the site, keeps what it set in s<D>, and returns to the site's next word with x0-x29, "
         "q0-q31 and the stack pointer as they were");
    return 0;
}
#endif
