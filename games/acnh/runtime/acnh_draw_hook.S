/* Animal Crossing: New Horizons only -- ACNH_DRAWHOOK=1 entry / exit hook of the per-object draw function main+0x1bf40d0
   (x0 = object, w1 = ring slot), installed by acnh_draw_hook.c. Diagnostics only: the function itself still runs on the
   game's stack, its frame 0x110 bytes deeper; the two loggers run on the thread's host stack through runtime_hle_switch.
   main+0x1bf40d0 is rewritten to "ldr x16, #8; br x16; .quad _acnh_draw_hook", so x30 is the game caller's return. */
.text

    .globl _acnh_draw_hook
    .p2align 2
_acnh_draw_hook:
    sub sp, sp, #0x110
    stp x29, x30, [sp]
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    stp q0, q1, [sp, #112]
    stp q2, q3, [sp, #144]
    stp q4, q5, [sp, #176]
    stp q6, q7, [sp, #208]
    mov x2, x30                         // acnh_draw_enter(object, slot, caller)
    adrp x16, _acnh_draw_enter@PAGE
    add x16, x16, _acnh_draw_enter@PAGEOFF
    mov x17, #0
    bl _runtime_hle_switch
    ldp x0, x1, [sp, #16]
    ldp x2, x3, [sp, #32]
    ldp x4, x5, [sp, #48]
    ldp x6, x7, [sp, #64]
    ldr x8, [sp, #80]
    ldp q0, q1, [sp, #112]
    ldp q2, q3, [sp, #144]
    ldp q4, q5, [sp, #176]
    ldp q6, q7, [sp, #208]
    bl acnh_draw_body                   // the original function; it returns here
    stp x0, x1, [sp, #88]
    stp q0, q1, [sp, #240]
    ldp x0, x1, [sp, #16]               // acnh_draw_exit(object, slot)
    adrp x16, _acnh_draw_exit@PAGE
    add x16, x16, _acnh_draw_exit@PAGEOFF
    mov x17, #0
    bl _runtime_hle_switch
    ldp x0, x1, [sp, #88]
    ldp q0, q1, [sp, #240]
    ldp x29, x30, [sp]
    add sp, sp, #0x110
    ret

/* The four instructions the hook replaced (register saves of the original prologue), then the rest of it. */
    .p2align 2
acnh_draw_body:
    stp x28, x27, [sp, #-96]!
    stp x26, x25, [sp, #16]
    stp x24, x23, [sp, #32]
    stp x22, x21, [sp, #48]
    adrp x16, _acnh_draw_resume@PAGE
    ldr x16, [x16, _acnh_draw_resume@PAGEOFF]   // main + 0x1bf40e0
    br x16
