// Enter a generated stub with synthetic guest registers. Preserve the native
// caller's callee-saved registers; capture guest outputs before restoring.
.text
.p2align 2
.globl _timing_call
_timing_call:
    stp x29, x30, [sp, #-176]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    str x1, [sp, #160]
    mov x10, x0
    mov x11, x1
    ldr x19, [x11]
    ldr x21, [x11, #8]
    ldr x23, [x11, #16]
    ldr x24, [x11, #24]
    ldr x27, [x11, #32]
    ldr x0, [x11, #40]
    ldr x8, [x11, #48]
    ldr x9, [x11, #56]
    ldr s1, [x11, #64]
    blr x10
    ldr x11, [sp, #160]
    str x8, [x11, #48]
    str x9, [x11, #56]
    str s0, [x11, #68]
    str s9, [x11, #72]
    ldp d14, d15, [sp, #144]
    ldp d12, d13, [sp, #128]
    ldp d10, d11, [sp, #112]
    ldp d8, d9, [sp, #96]
    ldp x27, x28, [sp, #80]
    ldp x25, x26, [sp, #64]
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #176
    ret
