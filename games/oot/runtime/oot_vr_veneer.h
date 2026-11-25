/* zeldaVR for Ocarina of Time (Mac, 2026-11-22, user: "开始开发 oot的vr mac版本"): the machine code of the two VR sites,
   without the runtime, so tests/oot_vr_veneer_native.py can run the very words natively. oot_vr.h installs them.

   The camera call (OOT_VR_CALL_WORDS): main+0x4c5068 is the first instruction after the game wrote its gameplay view's
   3x4 matrix (view +8..+0x37; x20 the view, x19 its camera). It is not a call boundary, so the veneer keeps everything a
   host call may change: x0-x17, x30, the flags and q0-q31 on the stack (672 bytes, as aspect_math.h aspect_vstub does),
   calls the host function whose address waits in the data page (fn cell) with x0 = x20, x1 = x19, restores it all, runs
   the displaced instruction and branches back after it.

   The field of view (OOT_VR_FOV_WORDS): main+0x4c52f8 ldr s0, [x19, #0x13c] reads the camera's vertical field of view
   (radians) for its projection. The veneer runs that load and, while the data page's fov cell holds a non-zero angle,
   uses that one instead. x16 and s2 are free there and the flags are not read before the next call (main+0x4c5308):
   the game's camera itself is never written. */
#pragma once
#include <stdint.h>
#include <string.h>

enum { OOT_VR_CALL_WORDS = 63, OOT_VR_FOV_WORDS = 6, OOT_VR_FRAME = 672 };

/* B from `from` to `to`; 0 when out of range. */
static inline uint32_t oot_vr_b(uint64_t from, uint64_t to) {
    const int64_t d = (int64_t)to - (int64_t)from;
    if ((d & 3) || d < -(1LL << 27) || d >= (1LL << 27)) return 0;
    return 0x14000000u | ((uint32_t)(d / 4) & 0x03ffffffu);
}
/* ADRP x<reg> at pc to target's 4 KiB page; 0 when out of range. */
static inline uint32_t oot_vr_adrp(unsigned reg, uint64_t pc, uint64_t target) {
    const int64_t pages = (int64_t)(target >> 12) - (int64_t)(pc >> 12);
    if (pages < -(1LL << 20) || pages >= (1LL << 20)) return 0;
    const uint32_t p = (uint32_t)pages;
    return 0x90000000u | (p & 3u) << 29 | ((p >> 2) & 0x7ffffu) << 5 | (reg & 31u);
}

/* A host call veneer at stub for the instruction `original` at site, which must not depend on the pc. page: the data page
   (4 KiB aligned); fn_cell: its offset of the host function's address (8-byte aligned, under 32 KiB); the host gets
   x0 = x<arg0>, x1 = x<arg1> (each 19..28, which the veneer does not change). Word 30 is the ADRP. */
static inline const char *oot_vr_host_veneer(uint32_t original, uint64_t stub, uint64_t site, uint64_t page, uint32_t fn_cell,
                                             unsigned arg0, unsigned arg1, uint32_t out[OOT_VR_CALL_WORDS]) {
    if ((page & 4095) || (fn_cell & 7) || fn_cell >= 32768) return "OOT VR camera veneer: data page cell";
    if (arg0 < 19 || arg0 > 28 || arg1 < 19 || arg1 > 28) return "OOT VR camera veneer: argument register";
    const uint32_t adrp = oot_vr_adrp(16, stub + 30 * 4, page), back = oot_vr_b(stub + 62 * 4, site + 4);
    if (!adrp) return "OOT VR camera veneer: data page out of ADRP range";
    if (!back) return "OOT VR camera veneer: branch back out of range";
    uint32_t w[OOT_VR_CALL_WORDS], n = 0;
    w[n++] = 0xd10a83ffu;                                                         /*  0 sub sp, sp, #672 */
    for (uint32_t i = 0; i < 18; i += 2)                                          /*  1 stp x<i>, x<i+1>, [sp, #8i] */
        w[n++] = 0xa9000000u | (i << 15) | ((i + 1) << 10) | (31u << 5) | i;
    w[n++] = 0xd53b4210u;                                                         /* 10 mrs x16, nzcv */
    w[n++] = 0xa9000000u | (18u << 15) | (16u << 10) | (31u << 5) | 30u;          /* 11 stp x30, x16, [sp, #144] */
    for (uint32_t i = 0; i < 32; i += 2)                                          /* 12 stp q<i>, q<i+1>, [sp, #160+16i] */
        w[n++] = 0xad000000u | (((160 + 16 * i) / 16) << 15) | ((i + 1) << 10) | (31u << 5) | i;
    w[n++] = 0xaa0003e0u | (arg0 << 16);                                          /* 28 mov x0, x<arg0> */
    w[n++] = 0xaa0003e1u | (arg1 << 16);                                          /* 29 mov x1, x<arg1> */
    w[n++] = adrp;                                                                /* 30 adrp x16, <data page> */
    w[n++] = 0xf9400000u | ((fn_cell / 8) << 10) | (16u << 5) | 16u;              /* 31 ldr x16, [x16, #fn] */
    w[n++] = 0xd63f0200u;                                                         /* 32 blr x16 */
    for (uint32_t i = 0; i < 32; i += 2)                                          /* 33 ldp q<i>, q<i+1> */
        w[n++] = 0xad400000u | (((160 + 16 * i) / 16) << 15) | ((i + 1) << 10) | (31u << 5) | i;
    w[n++] = 0xa9400000u | (18u << 15) | (16u << 10) | (31u << 5) | 30u;          /* 49 ldp x30, x16, [sp, #144] */
    w[n++] = 0xd51b4210u;                                                         /* 50 msr nzcv, x16 */
    for (uint32_t i = 0; i < 18; i += 2)                                          /* 51 ldp x<i>, x<i+1> */
        w[n++] = 0xa9400000u | (i << 15) | ((i + 1) << 10) | (31u << 5) | i;
    w[n++] = 0x910a83ffu;                                                         /* 60 add sp, sp, #672 */
    w[n++] = original;                                                            /* 61 the displaced instruction */
    w[n++] = back;                                                                /* 62 b <after it> */
    if (n != OOT_VR_CALL_WORDS) return "OOT VR camera veneer: size";
    memcpy(out, w, sizeof w);
    return NULL;
}
/* The camera site: x0 = x20 the view, x1 = x19 its camera. */
static inline const char *oot_vr_call_veneer(uint32_t original, uint64_t stub, uint64_t site, uint64_t page,
                                             uint32_t fn_cell, uint32_t out[OOT_VR_CALL_WORDS]) {
    return oot_vr_host_veneer(original, stub, site, page, fn_cell, 20, 19, out);
}

/* The field-of-view veneer at stub for the load `original` (ldr s0, ...) at site; fov_cell: the data page's float
   (4-byte aligned, under 16 KiB), 0 = the game's own angle. Word 1 is the ADRP. */
static inline const char *oot_vr_fov_veneer(uint32_t original, uint64_t stub, uint64_t site, uint64_t page,
                                            uint32_t fov_cell, uint32_t out[OOT_VR_FOV_WORDS]) {
    if ((page & 4095) || (fov_cell & 3) || fov_cell >= 16384) return "OOT VR field of view: data page cell";
    if ((original & 0xffc0001fu) != 0xbd400000u) return "OOT VR field of view: the site is not ldr s0, [xN, #imm]";
    const uint32_t adrp = oot_vr_adrp(16, stub + 4, page), back = oot_vr_b(stub + 20, site + 4);
    if (!adrp) return "OOT VR field of view: data page out of ADRP range";
    if (!back) return "OOT VR field of view: branch back out of range";
    const uint32_t w[OOT_VR_FOV_WORDS] = {
        original,                                                 /* 0 ldr s0, [x19, #0x13c]: the game's angle */
        adrp,                                                     /* 1 adrp x16, <data page> */
        0xbd400000u | ((fov_cell / 4) << 10) | (16u << 5) | 2u,   /* 2 ldr s2, [x16, #fov] */
        0x1e202048u,                                              /* 3 fcmp s2, #0.0 */
        0x1e201c40u,                                              /* 4 fcsel s0, s2, s0, ne */
        back};                                                    /* 5 b <after the load> */
    memcpy(out, w, sizeof w);
    return NULL;
}
