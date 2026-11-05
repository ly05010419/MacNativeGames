/* Pokopia 2.0.1: main+0x22d580 calculates the pending display size;
 * +0x22d1e0 commits it; +0x22d638 calls the engine's resize (+0x1797280)
 * and FSR resize notification (+0x110ce00). Snapshot the request before
 * calculation. The four size getters read this frame's consistent pair.
 * Invalidate only its cached mode byte to enter the native rebuild path. */
#pragma once
#include "bananza_display.h"
enum { POKOPIA_DISPLAY_WORDS=11, POKOPIA_RESIZE_WORDS=11 };
static inline void pokopia_display_code(uint32_t code[POKOPIA_DISPLAY_WORDS],uintptr_t stub,uintptr_t page,uintptr_t resume) {
    const uint32_t words[POKOPIA_DISPLAY_WORDS]={
        0x90000010,0x91002210,0xc8dffe11,0xf940060f,0xeb0f023f,
        0x54000080, /* b.eq original */
        0xf9000a00, /* str x0,[x16,#16]: display controller */
        0x9100220f,0xc89ffdf1, /* stlr into this frame's size */
        0xaa0003f3, /* original mov x19,x0 */
        0x14000000
    };
    memcpy(code,words,sizeof words);
    int64_t delta=((int64_t)(page&~4095ULL)-(int64_t)(stub&~4095ULL))/4096;
    uint32_t imm=(uint32_t)delta&0x1fffff;
    code[0]=0x90000010|((imm&3)<<29)|((imm>>2)<<5);
    code[POKOPIA_DISPLAY_WORDS-1]=bananza_display_branch(stub+sizeof words-4,resume);
}

/* main+0x1f9bcc8 snapshots the scene's selected standard UI camera, after
 * waiting for the preceding UI work and before dispatching its next draw.
 * A custom camera (index -1) skips this path. Each slot owns a camera (+0x10),
 * its adapter (+0x180) and logical canvas (+0x300). The native size setter
 * updates all three before the snapshot; font layout uses this same canvas. */
enum { POKOPIA_UI_WORDS=19 };
static inline void pokopia_ui_code(uint32_t code[POKOPIA_UI_WORDS],uintptr_t stub,uintptr_t page,uintptr_t resume,uintptr_t resize) {
    const uint32_t words[POKOPIA_UI_WORDS]={
        0x90000010,0xf9400a10,0xd360fe11,0x92407e10,
        0xb943040b,0x9bb07d6c,0xd341fe2d,0x8b0d018c,
        0x9ad1098c,0xb943000a,0x6b0c015f,0x540000c0,
        0xa9be7be0,0x29022fec,0x910043e1,0x94000000,
        0xa8c27be0,0x91008001,0x14000000
    };
    memcpy(code,words,sizeof words);
    int64_t delta=((int64_t)(page&~4095ULL)-(int64_t)(stub&~4095ULL))/4096;
    uint32_t imm=(uint32_t)delta&0x1fffff;
    code[0]=0x90000010|((imm&3)<<29)|((imm>>2)<<5);
    code[15]=bananza_display_branch(stub+60,resize)|0x80000000u;
    code[18]=bananza_display_branch(stub+72,resume);
}
/* The draw callback sees committed dimensions. Native mode IDs can remain
 * equal across custom dimensions; invalidate its cached ID on extent changes. */
static inline void pokopia_resize_code(uint32_t code[POKOPIA_RESIZE_WORDS],uintptr_t stub,uintptr_t page,uintptr_t resume) {
    const uint32_t words[POKOPIA_RESIZE_WORDS]={
        0x90000010,0x91008210,0xf9402411,0xf940020f,0xeb0f023f,
        0x54000080,0x52801fee,0x3902000e,0xc89ffe11,
        0x39420008, /* original ldrb w8,[x0,#0x80] */
        0x14000000
    };
    memcpy(code,words,sizeof words);
    int64_t delta=((int64_t)(page&~4095ULL)-(int64_t)(stub&~4095ULL))/4096;
    uint32_t imm=(uint32_t)delta&0x1fffff;
    code[0]=0x90000010|((imm&3)<<29)|((imm>>2)<<5);
    code[POKOPIA_RESIZE_WORDS-1]=bananza_display_branch(stub+sizeof words-4,resume);
}
/* Native window creation argument copied at main+0x17953f4. Flag +2
 * enables the engine's bounded resize path; +0x94/+0x98 are its maximum
 * swapchain extent. Without it the engine tears down the entire graphics
 * context (main+0x1794fc0), requiring a full graphics resource lifecycle. */
static inline int pokopia_window_capacity(void *buffer,size_t n,unsigned w,unsigned h) {
    if(n!=0xa0||!w||!h||w>3840||h>2160)return 0;
    uint8_t *p=buffer;if(p[2]>1)return 0;
    p[2]=1;memcpy(p+0x94,&w,4);memcpy(p+0x98,&h,4);return 1;
}

/* main+0x8cd860 transfers the active gameplay camera into its render view.
 * +0x1f20 on the view is its own projection aspect (not copied by the game);
 * +0x14c on the gameplay camera drives its projection and world/screen math.
 * Change both before native view/frustum construction, and recompute the
 * gameplay camera's native view/projection cache when its aspect changes. */
enum { POKOPIA_CAMERA_WORDS=19 };
static inline void pokopia_camera_code(uint32_t code[POKOPIA_CAMERA_WORDS],uintptr_t stub,uintptr_t page,uintptr_t resume,uintptr_t update) {
    const uint32_t words[POKOPIA_CAMERA_WORDS]={
        0xaa0003f3,0x90000010,0xf9400a10,0xd360fe11,
        0x1e230200,0x1e230221,0x1e211800,0xbd1f2260,
        0xbd414e81,0x1e212000,0x54000100,0xbd014e80,
        0xa9bf7be0,0xaa1403e0,0x52800021,0x52800022,
        0x94000000,0xa8c17be0,0x14000000
    };
    memcpy(code,words,sizeof words);
    int64_t delta=((int64_t)(page&~4095ULL)-(int64_t)((stub+4)&~4095ULL))/4096;
    uint32_t imm=(uint32_t)delta&0x1fffff;
    code[1]=0x90000010|((imm&3)<<29)|((imm>>2)<<5);
    code[16]=bananza_display_branch(stub+64,update)|0x80000000u;
    code[18]=bananza_display_branch(stub+72,resume);
}
