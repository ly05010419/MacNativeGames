/* Bananza v458752's display calculation (main+0x30f840). Applied inside
 * that game's thread, before its original flags load at +0x30f85c.
 * SystemTask+0xd8 owns this 0x210-byte display object (not +0xd0).
 * +0x10 = canvas, +0xd4/+0x138 = docked/handheld canvas,
 * +0x1a0 = requested display extent. Flags +8 bit 4 requests its native
 * resize; bit 6 keeps the exact extent. The original path at +0x30fb54
 * publishes the canvas and sets renderer+0x7d0 bit 8 to rebuild targets.
 * Only request/ack words cross threads; the host never edits this object.
 */
#pragma once
#include <stdint.h>
#include <string.h>
enum { BANANZA_DISPLAY_WORDS=21 };
static inline uint32_t bananza_display_branch(uintptr_t pc,uintptr_t to) {
    return 0x14000000u|((uint32_t)((int64_t)(to-pc)/4)&0x3ffffff);
}
static inline void bananza_display_code(uint32_t code[BANANZA_DISPLAY_WORDS],uintptr_t stub,uintptr_t page,uintptr_t resume) {
    const uint32_t words[BANANZA_DISPLAY_WORDS]={
        0x90000010, /* adrp x16, page (relocated below) */
        0x91002210, /* add x16,x16,#8: request, ack, display pointer */
        0xc8dffe11, /* ldar x17,[x16] */
        0xf940060f, /* ldr x15,[x16,#8] */
        0xeb0f023f, /* cmp x17,x15 */
        0x540001c0, /* b.eq original */
        0xf9000a00, /* str x0,[x16,#16] */
        0xf9000811, /* str x17,[x0,#0x10] */
        0xf80d4011, /* stur x17,[x0,#0xd4] */
        0xf9009c11, /* str x17,[x0,#0x138] */
        0xf900d011, /* str x17,[x0,#0x1a0] */
        0xb940080e, /* ldr w14,[x0,#8] */
        0x321c01ce, /* orr w14,w14,#0x10 */
        0x321a01ce, /* orr w14,w14,#0x40 */
        0xb900080e, /* str w14,[x0,#8] */
        0x52801fee, /* mov w14,#255 */
        0x3907140e, /* strb w14,[x0,#0x1c5]: invalidate cached mode */
        0x9100220f, /* add x15,x16,#8 */
        0xc89ffdf1, /* stlr x17,[x15]: request consumed */
        0x2941200b, /* original ldp w11,w8,[x0,#8] */
        0x14000000  /* b resume (relocated below) */
    };
    memcpy(code,words,sizeof words);
    int64_t delta=((int64_t)(page&~4095ULL)-(int64_t)(stub&~4095ULL))/4096;
    uint32_t imm=(uint32_t)delta&0x1fffff;
    code[0]=0x90000010|((imm&3)<<29)|((imm>>2)<<5);
    code[BANANZA_DISPLAY_WORDS-1]=bananza_display_branch(stub+sizeof words-4,resume);
}

/* Native camera -> render projection transfer (main+0x31f3bc). The renderer
 * receives this exact projection from its registered camera collection. Change
 * its aspect (+0xb0), mark it dirty (+8), and use the native matrix update
 * before the adapter derives frustum bounds and uniforms (main+0x31f570).
 * This is upstream of projection/frustum construction, not a GPU matrix edit. */
enum { BANANZA_CAMERA_WORDS=21 };
static inline void bananza_camera_code(uint32_t code[BANANZA_CAMERA_WORDS],uintptr_t stub,uintptr_t page,uintptr_t resume,uintptr_t update) {
    const uint32_t words[BANANZA_CAMERA_WORDS]={
        0xf9400901,0x90000010,0xf9400a10,0xd360fe11,
        0x1e230200,0x1e230221,0x1e211800,0xbd40b021,
        0x1e212000,0x54000160,0xbd00b020,0x39402030,
        0x32000210,0x39002030,0xa9be07e0,0xf9000bfe,
        0xaa0103e0,0x94000000,0xf9400bfe,0xa8c207e0,0x14000000
    };
    memcpy(code,words,sizeof words);
    int64_t delta=((int64_t)(page&~4095ULL)-(int64_t)((stub+4)&~4095ULL))/4096;
    uint32_t imm=(uint32_t)delta&0x1fffff;
    code[1]=0x90000010|((imm&3)<<29)|((imm>>2)<<5);
    code[17]=bananza_display_branch(stub+68,update)|0x80000000u;
    code[20]=bananza_display_branch(stub+80,resume);
}
