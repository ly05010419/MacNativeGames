/* Standalone lazy-binding bridge, adapted from the SSBU port. A matching PLT
   stub supplies the slot address in x16. The resolver target must switch to a
   host stack when needed and return the resolved address; this bridge is not
   connected to game startup. It preserves arguments before tail-branching. */
.text
    .globl _runtime_lazy_bind_entry
    .p2align 2
_runtime_lazy_bind_entry:
    stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    mov x0, x16
    adrp x16, _runtime_lazy_bind_target@PAGE
    ldr x16, [x16, _runtime_lazy_bind_target@PAGEOFF]
    blr x16
    mov x16, x0
    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldr x8, [sp, #80]
    ldp x6, x7, [sp, #64]
    ldp x4, x5, [sp, #48]
    ldp x2, x3, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #224
    br x16
