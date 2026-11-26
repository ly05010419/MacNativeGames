/* Native arm64 test of runtime/oot_vr_hooks.h (driven by oot_vr_hooks_native.py). Built twice: with -DGEN it prints
   the body's and the stubs' words (the script lays them into assembly); without, it runs them.
   Layout the words are made for: the body, then each stub followed by one branch of the script's to its return label. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../runtime/oot_vr_hooks.h"

enum { BODY = 0x10000, PAGE = 0x40000, FN_CELL = 120, STUB0 = BODY + OOT_HOOK_BODY_WORDS * 4, STUB_STEP = OOT_HOOK_STUB_WORDS * 4 + 4 };

#ifdef GEN
/* before: ldr x8, [x19, #0x20]; after: ldr w8, [x25, #0x10]; before: str x10, [sp, #0x10] (it needs the site's own sp) */
static const uint32_t SITE_WORD[3] = { 0xf9401268u, 0xb9401328u, 0xf9000beau };
static const int SITE_AFTER[3] = { 0, 1, 0 };
int main(void) {
    uint32_t body[OOT_HOOK_BODY_WORDS], stub[OOT_HOOK_STUB_WORDS];
    const char *e = oot_hook_body(BODY, PAGE, FN_CELL, body);
    if (e) { fprintf(stderr, "%s\n", e); return 1; }
    printf("body");
    for (int i = 0; i < OOT_HOOK_BODY_WORDS; i++) printf(" %08x", body[i]);
    printf("\n");
    for (int k = 0; k < 3; k++) {
        const uint64_t at = STUB0 + (uint64_t)k * STUB_STEP;
        e = oot_hook_stub(SITE_WORD[k], SITE_AFTER[k], at, at + 16, BODY, stub);   /* "site + 4" is the word after the stub */
        if (e) { fprintf(stderr, "%s\n", e); return 1; }
        printf("stub%d", k);
        for (int i = 0; i < OOT_HOOK_STUB_WORDS; i++) printf(" %08x", stub[i]);
        printf("\n");
    }
    /* Refusals: instructions that depend on where they run, a cell that is not aligned, a body too far away. */
    static const uint32_t fixed[] = { 0x14000010u, 0x94000010u, 0x54000040u, 0xb4000040u, 0x36000040u, 0x90000010u, 0x10000010u, 0x58000050u, 0xd63f0200u, 0xd65f03c0u };
    for (unsigned i = 0; i < sizeof fixed / sizeof *fixed; i++) if (oot_hook_movable(fixed[i]) || !oot_hook_stub(fixed[i], 0, STUB0, STUB0 + 16, BODY, stub)) return 2;
    static const uint32_t free_words[] = { 0xd503237fu, 0xb9400428u, 0xbd01e262u, 0x91008260u, 0x1e2a3842u, 0x2d150a61u, 0x79412a68u, 0xbd400821u, 0x1e232800u };
    for (unsigned i = 0; i < sizeof free_words / sizeof *free_words; i++) if (!oot_hook_movable(free_words[i])) return 3;
    if (!oot_hook_body(BODY, PAGE, 121, body) || !oot_hook_body(BODY, PAGE + 1, FN_CELL, body)) return 4;
    if (!oot_hook_stub(SITE_WORD[0], 0, STUB0, STUB0 + 16, BODY + 0x10000000ull, stub)) return 5;
    if (body[OOT_HOOK_ADRP_WORD] != oot_vr_adrp(16, BODY + OOT_HOOK_ADRP_WORD * 4, PAGE)) return 6;
    return 0;
}
#else
extern unsigned char hk_page[4096];
extern void run_site0(const uint64_t in[31], uint64_t out[128]);
extern void run_site1(const uint64_t in[31], uint64_t out[128]);
extern void run_site2(const uint64_t in[31], uint64_t out[128]);
static void run_site(int which, const uint64_t in[31], uint64_t out[128]) { (which == 0 ? run_site0 : which == 1 ? run_site1 : run_site2)(in, out); }

static oot_hook_regs seen;
static uint64_t seen_return, guest_args[2][5];
static int guest_calls, mode;
static oot_guest_call calls[2];

static uint64_t guest_fn(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    if (guest_calls < 2) { guest_args[guest_calls][0] = a; guest_args[guest_calls][1] = b; guest_args[guest_calls][2] = c; guest_args[guest_calls][3] = d; guest_args[guest_calls][4] = e; }
    guest_calls++;
    return a + 1000;
}
/* The host: what it saw, what it changes, the calls it asks for. */
static oot_guest_call *host(oot_hook_regs *r, uint64_t stub_return) {
    memcpy(&seen, r, sizeof seen);
    seen_return = stub_return;
    if (mode == 1) {                        /* registers set by the hook */
        r->x[8] = 0xffffffffull; r->x[3] = 0x3333; r->x[21] = 0x2121; r->x[30] = 0x7070707070707070ull;
        memset(r->q[5], 0x5a, 16); memset(r->q[9], 0x9b, 16);
        r->nzcv = 0x30000000ull;
        return NULL;
    }
    if (mode == 2) {                        /* two calls into the game, chained */
        calls[0] = (oot_guest_call){ (uint64_t)(uintptr_t)guest_fn, 11, 12, 13, 14, 0, &calls[1], 15 };
        calls[1] = (oot_guest_call){ (uint64_t)(uintptr_t)guest_fn, 21, 22, 23, 24, 0, NULL, 25 };
        return &calls[0];
    }
    return NULL;
}

static void fill(uint64_t in[31], const void *x19, const void *x25) {
    for (int i = 0; i < 31; i++) in[i] = 0x0101010101010101ULL * (uint64_t)(i + 1) + (uint64_t)i;
    in[19] = (uint64_t)(uintptr_t)x19; in[25] = (uint64_t)(uintptr_t)x25;
}
static void same_q(const uint64_t *out, int r, unsigned char want) {
    const unsigned char *q = (const unsigned char *)&out[32] + 16 * r;
    for (int b = 0; b < 16; b++) assert(q[b] == want);
}

int main(void) {
    _Alignas(16) unsigned char object[0x100] = {0};
    const uint64_t field = 0xfeedfacecafebeefULL; const uint32_t handle = 0x00c0ffeeu;
    memcpy(object + 0x20, &field, 8); memcpy(object + 0x10, &handle, 4);
    oot_guest_call *(*fn)(oot_hook_regs *, uint64_t) = host;
    memcpy(hk_page + FN_CELL, &fn, 8);
    uint64_t in[31], out[128];

    /* 0: a hook before the instruction that changes nothing: every register comes back, the load runs after it */
    mode = 0; fill(in, object, object); run_site(0, in, out);
    for (int i = 0; i < 31; i++) assert(seen.x[i] == in[i]);                         /* the host saw the site's registers, x30 too */
    assert(seen.nzcv == 0x60000000ull);
    for (int r = 0; r < 32; r++) for (int b = 0; b < 16; b++) assert(seen.q[r][b] == (unsigned char)(0x40 + r));
    for (int i = 0; i < 31; i++) assert(out[i] == (i == 8 ? field : in[i]));         /* x8: the displaced ldr x8, [x19, #0x20] */
    assert(out[31] == 0x60000000ull);
    for (int r = 0; r < 32; r++) same_q(out, r, (unsigned char)(0x40 + r));
    assert(out[100] == out[101]);                                                      /* the stack pointer is the site's again */
    const uint64_t first_return = seen_return;

    /* 1: a hook after the instruction: it sees the loaded value and what it sets holds */
    mode = 1; fill(in, object, object); run_site(1, in, out);
    assert(seen.x[8] == handle);                                                     /* ldr w8, [x25, #0x10] ran first */
    assert(seen_return == first_return + STUB_STEP + 4);                             /* each site has its own return address */
    assert(out[8] == 0xffffffffull && out[3] == 0x3333 && out[21] == 0x2121 && out[30] == 0x7070707070707070ull);
    for (int i = 0; i < 31; i++) if (i != 8 && i != 3 && i != 21 && i != 30) assert(out[i] == in[i]);
    assert(out[31] == 0x30000000ull);
    for (int r = 0; r < 32; r++) same_q(out, r, r == 5 ? 0x5a : r == 9 ? 0x9b : (unsigned char)(0x40 + r));
    assert(out[100] == out[101]);

    /* 2: calls into the game after the host: made in order with their arguments, their answers kept, registers untouched */
    mode = 2; guest_calls = 0; fill(in, object, object); run_site(0, in, out);
    assert(guest_calls == 2);
    for (int k = 0; k < 2; k++) for (int a = 0; a < 5; a++) assert(guest_args[k][a] == (uint64_t)(k ? 21 + a : 11 + a));   /* five arguments each */
    assert(calls[0].answer == 1011 && calls[1].answer == 1021);
    for (int i = 0; i < 31; i++) assert(out[i] == (i == 8 ? field : in[i]));
    for (int r = 0; r < 32; r++) same_q(out, r, (unsigned char)(0x40 + r));
    assert(out[31] == 0x60000000ull && out[100] == out[101]);

    /* 3: a displaced store relative to the stack pointer lands in the site's own frame */
    mode = 0; fill(in, object, object); run_site(2, in, out);
    assert(out[102] == in[10]);                                                       /* str x10, [sp, #0x10] */
    assert(out[100] == out[101]);
    puts("native ARM64 OOT VR hooks: the shared body hands the host x0-x30, the flags and q0-q31, puts back what the host "
         "left in the frame, runs the displaced instruction before or after with the site's own stack pointer, and makes "
         "the chained calls into the game with their answers kept");
    return 0;
}
#endif
