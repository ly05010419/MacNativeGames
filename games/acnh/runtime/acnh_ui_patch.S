/* Animal Crossing: New Horizons 1.2.1 -- Guards against null LocationAdjuster dereferences
   in nn::ui2d layout calculation/rendering (main+0x1c6e5b0).
   When a pane has LocationAdjust bit 4 or 5 set in flags (w8), but DrawInfo.pLocationAdjuster
   ([x22, #0xc0]) is NULL, the game code unconditionally loaded [adjuster + 0x28] and crashed
   with signal 11, fault=0x28.
   These hooks detect null [x22, #0xc0] and branch safely to the unscaled pane size fallback
   at main+0x1c6e608, or load default scale factors in loc3. */

.text

    .globl _acnh_ui_bit4_hook
    .p2align 2
_acnh_ui_bit4_hook:
    ldr  x9, [x22, #0xc0]
    cbz  x9, 1f
    // Non-null path: execute replaced original instructions + remaining
    ldr  x9, [x19]
    ldp  s0, s1, [x9, #0x18]
    ldp  s2, s3, [x9, #0x10]
    ldr  x9, [x22, #0xc0]
    fsub s0, s0, s2
    ldp  s2, s4, [x9, #0x28]
    adrp x16, _acnh_ui_resume_bit4@PAGE
    ldr  x16, [x16, _acnh_ui_resume_bit4@PAGEOFF]
    br   x16

1:  // Null path: log hit and branch to unscaled fallback at main+0x1c6e608
    sub  sp, sp, #32
    stp  x29, x30, [sp]
    stp  x8, x9, [sp, #16]
    mov  w0, #4
    bl   _acnh_ui_log_null_hit
    ldp  x8, x9, [sp, #16]
    ldp  x29, x30, [sp]
    add  sp, sp, #32
    adrp x16, _acnh_ui_fallback@PAGE
    ldr  x16, [x16, _acnh_ui_fallback@PAGEOFF]
    br   x16

    .globl _acnh_ui_bit5_hook
    .p2align 2
_acnh_ui_bit5_hook:
    ldr  x9, [x22, #0xc0]
    cbz  x9, 1f
    // Non-null path: execute replaced original instructions + remaining
    ldr  x9, [x19]
    ldp  s0, s1, [x9, #0x18]
    ldp  s2, s3, [x9, #0x10]
    fsub s0, s0, s2
    ldr  x9, [x22, #0xc0]
    ldp  s2, s4, [x9, #0x28]
    adrp x16, _acnh_ui_resume_bit5@PAGE
    ldr  x16, [x16, _acnh_ui_resume_bit5@PAGEOFF]
    br   x16

1:  // Null path: log hit and branch to unscaled fallback at main+0x1c6e608
    sub  sp, sp, #32
    stp  x29, x30, [sp]
    stp  x8, x9, [sp, #16]
    mov  w0, #5
    bl   _acnh_ui_log_null_hit
    ldp  x8, x9, [sp, #16]
    ldp  x29, x30, [sp]
    add  sp, sp, #32
    adrp x16, _acnh_ui_fallback@PAGE
    ldr  x16, [x16, _acnh_ui_fallback@PAGEOFF]
    br   x16

    .globl _acnh_ui_loc3_hook
    .p2align 2
_acnh_ui_loc3_hook:
    ldr  x8, [x22, #0xc0]
    cbz  x8, 1f
    // Non-null path: execute replaced original instructions
    fdiv s1, s1, s9
    fadd s0, s3, s0
    ldp  s2, s3, [x8, #0x28]
    adrp x16, _acnh_ui_resume_loc3@PAGE
    ldr  x16, [x16, _acnh_ui_resume_loc3@PAGEOFF]
    br   x16

1:  // Null path: use default UV scale 1.0f and resume at main+0x1c6e964
    fdiv s1, s1, s9
    fadd s0, s3, s0
    fneg s0, s0
    fmov s2, #1.0
    fmov s3, #1.0
    ldr  x23, [x19, #0x20]
    stp  s2, s3, [sp, #0x40]
    sub  sp, sp, #32
    stp  x29, x30, [sp]
    stp  x8, x9, [sp, #16]
    mov  w0, #3
    bl   _acnh_ui_log_null_hit
    ldp  x8, x9, [sp, #16]
    ldp  x29, x30, [sp]
    add  sp, sp, #32
    adrp x16, _acnh_ui_resume_loc3_null@PAGE
    ldr  x16, [x16, _acnh_ui_resume_loc3_null@PAGEOFF]
    br   x16
