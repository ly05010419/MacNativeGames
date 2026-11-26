/* zeldaVR for Ocarina of Time: many small hooks on one shared body (2026-11-25). The first VR sites each carry a whole
   veneer of 63 words (oot_vr_veneer.h); the text padding after main's code has room for about eight more of those
   (text_end 0x3c461d0, read-only data from 0x3c47000), and the hands, the sword's hits and the items need dozens.

   A site's own stub is five words. It runs the displaced instruction where the stack pointer and x30 are the site's,
   keeps x30 on the stack and calls the shared body:
       before:  str x30, [sp, #-16]! / bl body / ldr x30, [sp], #16 / <instruction> / b site+4
       after:   <instruction> / str x30, [sp, #-16]! / bl body / ldr x30, [sp], #16 / b site+4
   ("after": the host sees what the instruction did, as a hook right after a load that it then overrides.)

   The body keeps everything a call may change and what a hook may want to read or set: x0-x30, the flags and q0-q31
   in a frame of OOT_HOOK_FRAME bytes (oot_hook_regs), calls the one host function on the data page with x0 = the
   frame and x1 = the address the stub is returned to (that names the site), and puts every register back from the
   frame, so the host can change any of them. The host may answer with a chain of calls into the game
   (oot_guest_call, up to five arguments each): the body makes them on the game's own stack, one after the other, and writes
   each answer into its record. Words only, no runtime: tests/oot_vr_hooks_native.py runs them. */
#pragma once
#include <stdint.h>
#include <string.h>
#include "oot_vr_veneer.h"

enum { OOT_HOOK_FRAME = 816, OOT_HOOK_STUB_WORDS = 5, OOT_HOOK_BODY_WORDS = 90, OOT_HOOK_ADRP_WORD = 38,
       OOT_HOOK_RET_BEFORE = 8, OOT_HOOK_RET_AFTER = 12 };   /* the stub's return address, from its first word */

/* The frame the host gets. The site's stack pointer is 16 bytes above its end. */
typedef struct {
    uint64_t x[31];        /* x0-x30 as they were at the site */
    uint64_t nzcv;
    uint8_t q[32][16];
    uint64_t stub_return, record, spare[4];
} oot_hook_regs;
_Static_assert(sizeof(oot_hook_regs) == OOT_HOOK_FRAME, "the hook body's frame");

/* A call into the game, made by the body after the host returned it. next: the call after it, 0 none. */
typedef struct oot_guest_call {
    uint64_t fn, x0, x1, x2, x3, answer;
    struct oot_guest_call *next;
    uint64_t x4;
} oot_guest_call;
_Static_assert(sizeof(oot_guest_call) == 64, "the guest call record");

/* An instruction that works the same at another address: no branch, no address or load relative to the pc. */
static inline int oot_hook_movable(uint32_t w) {
    if ((w & 0x7c000000u) == 0x14000000u) return 0;   /* b, bl */
    if ((w & 0xfe000000u) == 0x54000000u) return 0;   /* b.cond */
    if ((w & 0x7e000000u) == 0x34000000u) return 0;   /* cbz, cbnz */
    if ((w & 0x7e000000u) == 0x36000000u) return 0;   /* tbz, tbnz */
    if ((w & 0x1f000000u) == 0x10000000u) return 0;   /* adr, adrp */
    if ((w & 0x3b000000u) == 0x18000000u) return 0;   /* ldr literal */
    if ((w & 0xfe000000u) == 0xd6000000u) return 0;   /* br, blr, ret and their kin */
    return 1;
}

static inline uint32_t oot_hook_bl(uint64_t from, uint64_t to) {
    const uint32_t b = oot_vr_b(from, to);
    return b ? b | 0x80000000u : 0;
}

/* A site's stub at `stub` for the instruction `original` at `site`; body: the shared body's address. */
static inline const char *oot_hook_stub(uint32_t original, int after, uint64_t stub, uint64_t site, uint64_t body,
                                        uint32_t out[OOT_HOOK_STUB_WORDS]) {
    if (!oot_hook_movable(original)) return "OOT VR hook: the instruction at the site depends on its address";
    const uint64_t call_at = stub + (after ? 8 : 4);
    const uint32_t bl = oot_hook_bl(call_at, body), back = oot_vr_b(stub + 16, site + 4);
    if (!bl) return "OOT VR hook: the shared body is out of branch range";
    if (!back) return "OOT VR hook: branch back out of range";
    const uint32_t push = 0xf81f0ffeu, pop = 0xf84107feu;   /* str x30, [sp, #-16]! ; ldr x30, [sp], #16 */
    const uint32_t before_w[OOT_HOOK_STUB_WORDS] = { push, bl, pop, original, back };
    const uint32_t after_w[OOT_HOOK_STUB_WORDS] = { original, push, bl, pop, back };
    memcpy(out, after ? after_w : before_w, sizeof before_w);
    return NULL;
}

/* The shared body at `at`. page: the data page (4 KiB aligned); fn_cell: its offset of the host function's address
   (8-byte aligned, under 32 KiB). Word OOT_HOOK_ADRP_WORD is the ADRP. */
static inline const char *oot_hook_body(uint64_t at, uint64_t page, uint32_t fn_cell, uint32_t out[OOT_HOOK_BODY_WORDS]) {
    if ((page & 4095) || (fn_cell & 7) || fn_cell >= 32768) return "OOT VR hook body: data page cell";
    const uint32_t adrp = oot_vr_adrp(16, at + OOT_HOOK_ADRP_WORD * 4, page);
    if (!adrp) return "OOT VR hook body: data page out of ADRP range";
    uint32_t w[OOT_HOOK_BODY_WORDS], n = 0;
    const uint32_t sp = 31u << 5;
    w[n++] = 0xd10003ffu | (OOT_HOOK_FRAME << 10);                                   /*  0 sub sp, sp, #frame */
    for (uint32_t i = 0; i < 30; i += 2) w[n++] = 0xa9000000u | (i << 15) | ((i + 1) << 10) | sp | i;   /*  1 stp x<i>, x<i+1>, [sp, #8i] */
    w[n++] = 0xf9400000u | ((OOT_HOOK_FRAME / 8) << 10) | sp | 16u;                  /* 16 ldr x16, [sp, #frame]: the site's x30 */
    w[n++] = 0xd53b4211u;                                                            /* 17 mrs x17, nzcv */
    w[n++] = 0xa9000000u | (30u << 15) | (17u << 10) | sp | 16u;                     /* 18 stp x16, x17, [sp, #240] */
    for (uint32_t i = 0; i < 32; i += 2) w[n++] = 0xad000000u | (((256 + 16 * i) / 16) << 15) | ((i + 1) << 10) | sp | i;   /* 19 stp q<i>, q<i+1> */
    w[n++] = 0xf9000000u | ((768 / 8) << 10) | sp | 30u;                             /* 35 str x30, [sp, #768]: back to the stub */
    w[n++] = 0x910003e0u;                                                            /* 36 mov x0, sp */
    w[n++] = 0xaa1e03e1u;                                                            /* 37 mov x1, x30 */
    w[n++] = adrp;                                                                   /* 38 adrp x16, <data page> */
    w[n++] = 0xf9400000u | ((fn_cell / 8) << 10) | (16u << 5) | 16u;                 /* 39 ldr x16, [x16, #fn] */
    w[n++] = 0xd63f0200u;                                                            /* 40 blr x16: the host */
    const uint32_t loop = n;
    w[n++] = 0xb4000000u | (12u << 5);                                               /* 41 cbz x0, <53> */
    w[n++] = 0xf9000000u | ((776 / 8) << 10) | sp | 0u;                              /* 42 str x0, [sp, #776]: the record */
    w[n++] = 0xf9400010u;                                                            /* 43 ldr x16, [x0] */
    w[n++] = 0xa9410801u;                                                            /* 44 ldp x1, x2, [x0, #16] */
    w[n++] = 0xf9401003u;                                                            /* 45 ldr x3, [x0, #32] */
    w[n++] = 0xf9401c04u;                                                            /* 46 ldr x4, [x0, #56] */
    w[n++] = 0xf9400400u;                                                            /* 47 ldr x0, [x0, #8] */
    w[n++] = 0xd63f0200u;                                                            /* 48 blr x16: the game */
    w[n++] = 0xf9400000u | ((776 / 8) << 10) | sp | 9u;                              /* 49 ldr x9, [sp, #776] */
    w[n++] = 0xf9000000u | (5u << 10) | (9u << 5) | 0u;                              /* 50 str x0, [x9, #40]: its answer */
    w[n++] = 0xf9400000u | (6u << 10) | (9u << 5) | 0u;                              /* 51 ldr x0, [x9, #48]: the next call */
    w[n] = 0x14000000u | ((loop - n) & 0x03ffffffu); n++;                            /* 52 b <41> */
    for (uint32_t i = 0; i < 32; i += 2) w[n++] = 0xad400000u | (((256 + 16 * i) / 16) << 15) | ((i + 1) << 10) | sp | i;   /* 53 ldp q<i>, q<i+1> */
    w[n++] = 0xa9400000u | (30u << 15) | (17u << 10) | sp | 16u;                     /* 69 ldp x16, x17, [sp, #240] */
    w[n++] = 0xd51b4211u;                                                            /* 70 msr nzcv, x17 */
    w[n++] = 0xf9000000u | ((OOT_HOOK_FRAME / 8) << 10) | sp | 16u;                  /* 71 str x16, [sp, #frame]: x30 as the host left it */
    w[n++] = 0xf9400000u | ((768 / 8) << 10) | sp | 30u;                             /* 72 ldr x30, [sp, #768] */
    for (uint32_t i = 0; i < 30; i += 2) w[n++] = 0xa9400000u | (i << 15) | ((i + 1) << 10) | sp | i;   /* 73 ldp x<i>, x<i+1> */
    w[n++] = 0x910003ffu | (OOT_HOOK_FRAME << 10);                                   /* 88 add sp, sp, #frame */
    w[n++] = 0xd65f03c0u;                                                            /* 89 ret: into the stub */
    if (n != OOT_HOOK_BODY_WORDS) return "OOT VR hook body: size";
    memcpy(out, w, sizeof w);
    return NULL;
}
