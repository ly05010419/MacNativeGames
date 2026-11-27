/* Ocarina of Time in VR: level of detail by the true distance to the eye (vr.md row 18), the parts without the runtime
   (tests/test_oot_vr_lod_true.c runs them; the game side is games/oot/runtime/oot_vr_lod_true.h).

   The game picks a model's level of detail, and drops small ones, by its projected size: radius x scale / depth, the
   depth being the model's centre along the view's sight line. When the head turns, what is beside you gets a smaller
   depth and so a finer level; it changes detail as you look around. HookshotVR (GPL-3; facts and addresses only, no
   code) makes the game use the distance to the eye instead, at the 193 places where the depth is summed (camera-ui.md
   C 3.7). Each of them ends with `fadd s<D>, s0, s3` (D 0 or 1) after
       ldp s0, s1, [x<S>, #0xc8] / ldp s2, s3, [x<O>] / fmul s1, s1, s3 / fmadd s0, s0, s2, s1 /
       ldr s1, [x<O>, #8] / ldp s2, s3, [x<S>, #0xd0] / fmadd s0, s2, s1, s0
   (x<S> the render view's snapshot, whose +0xa8 holds the 3x4 view matrix the snapshot was built from, so +0xc8.. is its
   third row; x<O> the model's bounding sphere, centre at +0, radius at +0xc).

   Room. Our hooks spend five words of main's text padding a site (oot_vr_hooks.h); 193 would need 3,860 bytes, and
   1,256 are left. Here a site costs nothing in the padding: its own word becomes `bl <veneer>`, one of two veneers (one
   for D = 0, one for D = 1) that runs the replaced fadd itself, asks the shared hook body and returns to the site's next
   word with `ret`. The site is known from the return address. `bl` writes x30, so a site is used only where x30 is
   provably dead: on every path from the next word the code writes x30 (a call, a load into it) before anything reads
   it, and nothing on the way is an instruction this decoder does not know (oot_lt_x30_dead; on update 131072's image
   192 of the 193 pass, main+0x3628350 does not: its code reads [x30, #8] later). Two veneers, 96 bytes.

   The distance (oot_lt_depth). A snapshot taken from a view we wrote (exactly its 12 floats) is ours. For it, the eye is
   the headset's true eye e, and the view we gave the game may have been moved back along its sight line by b (first
   person's culling view, oot_vr_view.h oot_cull_view: only the z translation is lowered by b, so every depth the game sums
   is the true one + b). The new depth is -(|centre - e| + b): rotation does not change it, and it is never nearer than
   the game's own (|centre - e| >= the true depth). Only for z < 0 (ahead), finite numbers, a sphere pointer that is not 0;
   if the result would be nearer than -z * 0.999 (HookshotVR's check: another view), the game's z stays. */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

enum { OOT_LT_READ = 1, OOT_LT_WRITE = 2, OOT_LT_UNKNOWN = 4 };

/* What the instruction `w` does with general register `reg` (0-30; 31 is never asked): read it, write it, or not known
   to this decoder (an encoding it does not take apart; the callers treat that as a read). A read wins over a write. */
static inline unsigned oot_lt_gpr(uint32_t w, unsigned reg) {
    const unsigned d = w & 31, n = (w >> 5) & 31, t2 = (w >> 10) & 31, m = (w >> 16) & 31;
    const unsigned any = d == reg || n == reg || t2 == reg || m == reg;
    if ((w & 0x1c000000u) == 0x14000000u) {                       /* branches, exceptions, system */
        if ((w & 0x7c000000u) == 0x14000000u) return (w >> 31) && reg == 30 ? OOT_LT_WRITE : 0;   /* b, bl */
        if ((w & 0xff000010u) == 0x54000000u) return 0;          /* b.cond */
        if ((w & 0x7c000000u) == 0x34000000u) return d == reg ? OOT_LT_READ : 0;   /* cbz, cbnz, tbz, tbnz */
        if (w == 0xd503201fu || (w & 0xffffff3fu) == 0xd503241fu) return 0;        /* nop, bti */
        if ((w & 0xfffff01fu) == 0xd503301fu) return 0;          /* clrex, dsb, dmb, isb, sb */
        if ((w & 0xfff00000u) == 0xd5300000u) return d == reg ? OOT_LT_WRITE : 0;  /* mrs */
        if ((w & 0xfff00000u) == 0xd5100000u) return d == reg ? OOT_LT_READ : 0;   /* msr (register) */
        if ((w & 0xfffffc1fu) == 0xd63f0000u) return n == reg ? OOT_LT_READ : reg == 30 ? OOT_LT_WRITE : 0;   /* blr */
        return OOT_LT_UNKNOWN;                                   /* br, ret, pointer authentication, svc ... */
    }
    if ((w & 0x0a000000u) == 0x08000000u) {                       /* loads and stores */
        const unsigned v = (w >> 26) & 1;
        if ((w & 0x3a000000u) == 0x28000000u) {                   /* ldp / stp, every addressing form */
            if (n == reg) return OOT_LT_READ;
            if (!v && (d == reg || t2 == reg)) return (w >> 22) & 1 ? OOT_LT_WRITE : OOT_LT_READ;
            return 0;
        }
        const int imm12 = (w & 0x3b000000u) == 0x39000000u, imm9 = (w & 0x3b200000u) == 0x38000000u;
        const int regoff = (w & 0x3b200c00u) == 0x38200800u;
        if (imm12 || imm9 || regoff) {                            /* ldr / str (register) */
            if (n == reg || (regoff && m == reg)) return OOT_LT_READ;
            if (v || d != reg) return 0;
            const unsigned size = w >> 30, opc = (w >> 22) & 3;
            if (size == 3 && opc == 2) return OOT_LT_UNKNOWN;     /* prfm: no register in Rt */
            return opc ? OOT_LT_WRITE : OOT_LT_READ;
        }
        if ((w & 0x3b000000u) == 0x18000000u) return !v && d == reg ? OOT_LT_UNKNOWN : 0;   /* ldr literal (prfm too) */
        if ((w & 0xbe000000u) == 0x0c000000u) return n == reg || m == reg ? OOT_LT_READ : 0; /* ld1 / st1 ... (vectors) */
        return any ? OOT_LT_UNKNOWN : 0;                          /* exclusives, atomics, ldapr ...: registers only in the four fields */
    }
    if ((w & 0x1c000000u) == 0x10000000u) {                       /* data processing, immediate */
        if ((w & 0x1f000000u) == 0x10000000u) return d == reg ? OOT_LT_WRITE : 0;   /* adr, adrp */
        const unsigned c = w & 0x1f800000u, opc = (w >> 29) & 3;
        if (c == 0x11000000u || c == 0x12000000u) return n == reg ? OOT_LT_READ : d == reg ? OOT_LT_WRITE : 0;   /* add/sub, logical */
        if (c == 0x12800000u) return d != reg ? 0 : opc == 3 ? OOT_LT_READ : opc == 1 ? OOT_LT_UNKNOWN : OOT_LT_WRITE;   /* movk / movn, movz */
        if (c == 0x13000000u) return n == reg ? OOT_LT_READ : d != reg ? 0 : opc == 1 ? OOT_LT_READ : opc == 3 ? OOT_LT_UNKNOWN : OOT_LT_WRITE;   /* bfm / sbfm, ubfm */
        if (c == 0x13800000u) return n == reg || m == reg ? OOT_LT_READ : d == reg ? OOT_LT_WRITE : 0;   /* extr */
        return any ? OOT_LT_UNKNOWN : 0;
    }
    if ((w & 0x0e000000u) == 0x0a000000u) {                       /* data processing, register */
        const unsigned c = w & 0x1f000000u;
        if (c == 0x1b000000u) return n == reg || m == reg || t2 == reg ? OOT_LT_READ : d == reg ? OOT_LT_WRITE : 0;   /* madd ... */
        if (c == 0x0a000000u || c == 0x0b000000u) return n == reg || m == reg ? OOT_LT_READ : d == reg ? OOT_LT_WRITE : 0;   /* logical, add/sub */
        if ((w & 0x1fe00000u) == 0x1a400000u) return n == reg || (!(w & 0x800u) && m == reg) ? OOT_LT_READ : 0;   /* ccmp, ccmn */
        if ((w & 0x1fe0fc00u) == 0x1a000000u || (w & 0x1fe00000u) == 0x1a800000u || (w & 0x5fe00000u) == 0x1ac00000u)
            return n == reg || m == reg ? OOT_LT_READ : d == reg ? OOT_LT_WRITE : 0;   /* adc, sbc, csel ..., 2-source */
        if ((w & 0x5fff0000u) == 0x5ac00000u) return n == reg ? OOT_LT_READ : d == reg ? OOT_LT_WRITE : 0;   /* rbit, rev, clz ... */
        return any ? OOT_LT_UNKNOWN : 0;
    }
    if ((w & 0x0e000000u) == 0x0e000000u) {                       /* floating point and vectors */
        const int to_gpr = (w & 0x5f20fc00u) == 0x1e200000u      /* fmov / fcvtzs / scvtf ... with an integer register */
                        || (w & 0x5f200000u) == 0x1e000000u      /* the same with a fixed point */
                        || (w & 0x9fe08400u) == 0x0e000400u;     /* dup / ins / umov / smov */
        return to_gpr && (d == reg || n == reg) ? OOT_LT_UNKNOWN : 0;
    }
    return OOT_LT_UNKNOWN;                                        /* SVE, SME, reserved */
}

/* The instruction may write a floating-point / vector register (its class does, or it loads into one). */
static inline int oot_lt_writes_fp(uint32_t w) {
    if ((w & 0x0e000000u) == 0x0e000000u) return 1;
    if ((w & 0x0a000000u) == 0x08000000u && ((w >> 26) & 1)) return 1;
    return 0;
}

/* x30 is dead at the word after `at`: every path from code[at + 1] writes x30 before it reads it, within `limit` words.
   code: the module's words, nwords of them; words [gap_lo, gap_hi) are not code (main's read-only data and data, between
   its text and the rewriter's patch area, which the text branches into). 0 with *why when that cannot be shown.
   visited: scratch of `cap` words (a power of two). */
enum { OOT_LT_BRANCHES = 512 };
static inline int oot_lt_x30_dead(const uint32_t *text, uint64_t nwords, uint64_t gap_lo, uint64_t gap_hi, uint64_t at,
                                  uint32_t *visited, unsigned cap, unsigned limit, const char **why) {
    uint64_t todo[OOT_LT_BRANCHES]; unsigned ntodo = 0, steps = 0;
    memset(visited, 0xff, cap * sizeof *visited);
    todo[ntodo++] = at + 1;
    while (ntodo) {
        uint64_t pc = todo[--ntodo];
        for (;;) {
            if (pc >= nwords || pc >= 0xffffffffu || (pc >= gap_lo && pc < gap_hi)) { *why = "a path leaves the code"; return 0; }
            unsigned h = (unsigned)(pc * 2654435761u) & (cap - 1), probes = 0;
            while (visited[h] != 0xffffffffu && visited[h] != (uint32_t)pc && probes < cap) { h = (h + 1) & (cap - 1); probes++; }
            if (visited[h] == (uint32_t)pc) break;               /* this word was followed already */
            if (probes >= cap || ++steps > limit) { *why = "the paths are too long to follow"; return 0; }
            visited[h] = (uint32_t)pc;
            const uint32_t w = text[pc];
            const unsigned g = oot_lt_gpr(w, 30);
            if (g & OOT_LT_READ) { *why = "x30 is read"; return 0; }
            if (g & OOT_LT_UNKNOWN) { *why = "an instruction that may use x30 or leave (ret, br, svc ...)"; return 0; }
            if (g & OOT_LT_WRITE) break;                          /* written first: this path is done */
            int64_t off = 0; int cond = 0;
            if ((w & 0xfc000000u) == 0x14000000u) off = (int32_t)(w << 6) >> 6;                       /* b */
            else if ((w & 0xff000010u) == 0x54000000u) { off = (int32_t)(w << 8) >> 13; cond = 1; }   /* b.cond */
            else if ((w & 0x7e000000u) == 0x34000000u) { off = (int32_t)(w << 8) >> 13; cond = 1; }   /* cbz, cbnz */
            else if ((w & 0x7e000000u) == 0x36000000u) { off = (int32_t)(w << 13) >> 18; cond = 1; }  /* tbz, tbnz */
            else { pc++; continue; }
            if ((int64_t)pc + off < 0) { *why = "a path leaves the code"; return 0; }
            if (!cond) { pc = (uint64_t)((int64_t)pc + off); continue; }
            if (ntodo >= sizeof todo / sizeof *todo) { *why = "too many branches to follow"; return 0; }
            todo[ntodo++] = (uint64_t)((int64_t)pc + off);
            pc++;
        }
    }
    return 1;
}

/* The words of the depth sum for snapshot register s and sphere register o, in the order the notes give them. */
enum { OOT_LT_SUM_WORDS = 7, OOT_LT_WINDOW = 12 };
static inline void oot_lt_sum_words(unsigned s, unsigned o, uint32_t out[OOT_LT_SUM_WORDS]) {
    out[0] = 0x2d400000u | (0xc8u / 4) << 15 | 1u << 10 | s << 5 | 0;   /* ldp s0, s1, [x<s>, #0xc8] */
    out[1] = 0x2d400000u | 3u << 10 | o << 5 | 2;                       /* ldp s2, s3, [x<o>] */
    out[2] = 0x1e230821u;                                               /* fmul s1, s1, s3 */
    out[3] = 0x1f020400u;                                               /* fmadd s0, s0, s2, s1 */
    out[4] = 0xbd400000u | (8u / 4) << 10 | o << 5 | 1;                 /* ldr s1, [x<o>, #8] */
    out[5] = 0x2d400000u | (0xd0u / 4) << 15 | 3u << 10 | s << 5 | 2;   /* ldp s2, s3, [x<s>, #0xd0] */
    out[6] = 0x1f010040u;                                               /* fmadd s0, s2, s1, s0 */
}
static inline uint32_t oot_lt_site_word(unsigned d) { return 0x1e232800u | (d & 1); }   /* fadd s<d>, s0, s3 */

/* The site at word `at` is the end of the depth sum for (d, s, o): its word, the sum's seven words in the twelve before it
   in an order that computes the depth, nothing else there writing x<s>, x<o> or a float register. */
static inline int oot_lt_site_ok(const uint32_t *text, uint64_t nwords, uint64_t at, unsigned d, unsigned s, unsigned o, const char **why) {
    if (at < OOT_LT_WINDOW || at >= nwords || d > 1 || s > 28 || o > 28 || s == o) { *why = "out of the text"; return 0; }
    if (text[at] != oot_lt_site_word(d)) { *why = "the site's word differs"; return 0; }
    uint32_t want[OOT_LT_SUM_WORDS]; int pos[OOT_LT_SUM_WORDS];
    oot_lt_sum_words(s, o, want);
    for (int k = 0; k < OOT_LT_SUM_WORDS; k++) {
        pos[k] = -1;
        for (int i = 0; i < OOT_LT_WINDOW; i++) if (text[at - OOT_LT_WINDOW + i] == want[k]) pos[k] = i;
        if (pos[k] < 0) { *why = "the depth sum is not before the site"; return 0; }
    }
    const int a = pos[0] > pos[1] ? pos[0] : pos[1], b = pos[4] < pos[5] ? pos[4] : pos[5], c = pos[4] > pos[5] ? pos[4] : pos[5];
    if (!(a < pos[2] && pos[2] < pos[3] && pos[3] < b && c < pos[6])) { *why = "the depth sum's words are in another order"; return 0; }
    const int first = pos[0] < pos[1] ? pos[0] : pos[1];
    for (int i = first; i < OOT_LT_WINDOW; i++) {
        int mine = 0; for (int k = 0; k < OOT_LT_SUM_WORDS; k++) mine |= pos[k] == i;
        const uint32_t w = text[at - OOT_LT_WINDOW + i];
        if (mine) continue;
        if (oot_lt_writes_fp(w) || (oot_lt_gpr(w, s) & (OOT_LT_WRITE | OOT_LT_UNKNOWN)) || (oot_lt_gpr(w, o) & (OOT_LT_WRITE | OOT_LT_UNKNOWN))) {
            *why = "another instruction inside the depth sum"; return 0; }
    }
    return 1;
}

/* The veneer for D: the site's own fadd, then (only while the flag cell holds non-zero) the shared hook body, then back to
   the site's next word through x30. at: its address; page: the data page (4 KiB aligned); cell: the flag's offset (4-byte
   aligned, under 16 KiB). Word OOT_LT_VENEER_ADRP is the ADRP, OOT_LT_VENEER_RETURN the word the body returns to. */
enum { OOT_LT_VENEER_WORDS = 12, OOT_LT_VENEER_ADRP = 2, OOT_LT_VENEER_RETURN = 8 };
static inline const char *oot_lt_veneer(unsigned d, uint64_t at, uint64_t body, uint64_t page, uint32_t cell, uint32_t out[OOT_LT_VENEER_WORDS]) {
    if ((page & 4095) || (cell & 3) || cell >= 16384 || d > 1) return "OOT VR true-distance detail: veneer arguments";
    const int64_t pages = (int64_t)(page >> 12) - (int64_t)((at + OOT_LT_VENEER_ADRP * 4) >> 12);
    if (pages < -(1LL << 20) || pages >= (1LL << 20)) return "OOT VR true-distance detail: data page out of ADRP range";
    const int64_t to_body = (int64_t)body - (int64_t)(at + 7 * 4);
    if ((to_body & 3) || to_body < -(1LL << 27) || to_body >= (1LL << 27)) return "OOT VR true-distance detail: hook body out of branch range";
    const uint32_t p = (uint32_t)pages;
    const uint32_t w[OOT_LT_VENEER_WORDS] = {
        oot_lt_site_word(d),                                         /*  0 fadd s<d>, s0, s3: the site's own word */
        0xf81f0ff0u,                                                 /*  1 str x16, [sp, #-16]! */
        0x90000000u | (p & 3u) << 29 | ((p >> 2) & 0x7ffffu) << 5 | 16u,   /*  2 adrp x16, <data page> */
        0xb9400000u | (cell / 4) << 10 | 16u << 5 | 16u,             /*  3 ldr w16, [x16, #cell] */
        0x34000000u | 6u << 5 | 16u,                                 /*  4 cbz w16, <10> (six words on) */
        0xf84107f0u,                                                 /*  5 ldr x16, [sp], #16 */
        0xf81f0ffeu,                                                 /*  6 str x30, [sp, #-16]! */
        0x94000000u | ((uint32_t)(to_body / 4) & 0x03ffffffu),       /*  7 bl <body> */
        0xf84107feu,                                                 /*  8 ldr x30, [sp], #16 */
        0xd65f03c0u,                                                 /*  9 ret */
        0xf84107f0u,                                                 /* 10 ldr x16, [sp], #16 */
        0xd65f03c0u,                                                 /* 11 ret */
    };
    memcpy(out, w, sizeof w);
    return NULL;
}
/* The site's new word: bl <veneer>; 0 when out of range. */
static inline uint32_t oot_lt_site_bl(uint64_t site, uint64_t veneer) {
    const int64_t d = (int64_t)veneer - (int64_t)site;
    if ((d & 3) || d < -(1LL << 27) || d >= (1LL << 27)) return 0;
    return 0x94000000u | ((uint32_t)(d / 4) & 0x03ffffffu);
}

/* The eye of a 3x4 view matrix (rows R|t, world to view): -R^T t. */
static inline void oot_lt_eye(const float m[12], float e[3]) {
    for (int j = 0; j < 3; j++) e[j] = -(m[j] * m[3] + m[4 + j] * m[7] + m[8 + j] * m[11]);
}
/* The new depth for a model's centre c whose depth along the view is z, the true eye e and the view moved back by b. */
static inline int oot_lt_depth(float z, const float c[3], const float e[3], float b, float *out) {
    if (!(z < 0) || !isfinite(z) || !isfinite(b) || b < 0) return 0;
    for (int i = 0; i < 3; i++) if (!isfinite(c[i]) || !isfinite(e[i])) return 0;
    const float dx = c[0] - e[0], dy = c[1] - e[1], dz = c[2] - e[2];
    const float far = sqrtf(dx * dx + dy * dy + dz * dz) + b;
    if (!isfinite(far) || far < -z * 0.999f) return 0;
    *out = -far;
    return 1;
}
