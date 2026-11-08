/* Ocarina of Time 1.0.0: the output size (layer render targets and the NVN crop) is DisplayConfig's base size cfg+0x10, written only by
 * setBaseSize(cfg, const int2 *size) at main+0x1087f40 (sets cfg+8 bit 21 when it changes; a leaf that clobbers w8-w11 only). Its callers are
 * the display init (main+0x1087c68) and the docked/handheld recompute in the per-frame cfg update (main+0x650600). On bit 21 that update
 * republishes the crop (main+0x650540 -> main+0xe9a850), main+0x652260 sizes layer 0 from cfg+0x10, and main+0x6549d0 -> main+0x6531c0 ->
 * main+0x9cd030 re-creates its targets: the engine's own resize path. The scene follows the layer every frame (main+0x654e70).
 * Page after main (loader.c): p[0] capacity, p[1] host request (h<<32|w, stored atomically), p[2] the size setBaseSize was last given, p[3] cfg. */
#pragma once
#include "bananza_display.h"
enum { OOT_BASE_WORDS=7, OOT_FRAME_WORDS=11 };
/* At setBaseSize's entry: every request becomes the host's size, snapshotted once into p[2] (the function reads *x1 three times). */
static inline void oot_base_code(uint32_t code[OOT_BASE_WORDS],uintptr_t stub,uintptr_t page,uintptr_t resume) {
    const uint32_t words[OOT_BASE_WORDS]={
        0x90000010, /* adrp x16, page (relocated below) */
        0x91002210, /* add x16,x16,#8 */
        0xc8dffe11, /* ldar x17,[x16]: the request */
        0xa9008211, /* stp x17,x0,[x16,#8]: p[2] = this call's size, p[3] = cfg */
        0x91002201, /* add x1,x16,#8 */
        0x29422408, /* original ldp w8,w9,[x0,#0x10] */
        0x14000000  /* b main+0x1087f44 (relocated below) */
    };
    memcpy(code,words,sizeof words);
    int64_t delta=((int64_t)(page&~4095ULL)-(int64_t)(stub&~4095ULL))/4096;
    uint32_t imm=(uint32_t)delta&0x1fffff;
    code[0]=0x90000010|((imm&3)<<29)|((imm>>2)<<5);
    code[OOT_BASE_WORDS-1]=bananza_display_branch(stub+sizeof words-4,resume);
}
/* main+0x6502f8, the cfg update after its prologue (x19-x22, x29, x30 saved; x0 = cfg, x1 live until main+0x65030c; x8-x11 and the flags
 * written before they are read): a base that differs from the request calls setBaseSize, so this same update sees bit 21. */
static inline void oot_frame_code(uint32_t code[OOT_FRAME_WORDS],uintptr_t stub,uintptr_t page,uintptr_t resume,uintptr_t set_base) {
    const uint32_t words[OOT_FRAME_WORDS]={
        0x90000010, /* adrp x16, page (relocated below) */
        0x91002210, /* add x16,x16,#8 */
        0xc8dffe11, /* ldar x17,[x16]: the request */
        0xf9400810, /* ldr x16,[x0,#0x10]: the base */
        0xeb11021f, /* cmp x16,x17 */
        0x54000080, /* b.eq original */
        0xa9bf7be1, /* stp x1,x30,[sp,#-16]! */
        0x94000000, /* bl setBaseSize (relocated below; x0 kept) */
        0xa8c17be1, /* ldp x1,x30,[sp],#16 */
        0x2941240b, /* original ldp w11,w9,[x0,#8] */
        0x14000000  /* b main+0x6502fc (relocated below) */
    };
    memcpy(code,words,sizeof words);
    int64_t delta=((int64_t)(page&~4095ULL)-(int64_t)(stub&~4095ULL))/4096;
    uint32_t imm=(uint32_t)delta&0x1fffff;
    code[0]=0x90000010|((imm&3)<<29)|((imm>>2)<<5);
    code[7]=bananza_display_branch(stub+28,set_base)|0x80000000u;
    code[OOT_FRAME_WORDS-1]=bananza_display_branch(stub+sizeof words-4,resume);
}
