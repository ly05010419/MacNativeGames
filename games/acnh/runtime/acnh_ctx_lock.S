/* Animal Crossing: New Horizons only -- per-context lock around main+0x1be5b30 (x0 = context), installed by
   acnh_ctx_lock.c unless ACNH_CTX_LOCK=0. The function runs on the game's stack as before (its frame 0x110 bytes deeper);
   the lock and unlock run on the thread's host stack through runtime_hle_switch.
   main+0x1be5b30 is rewritten to "ldr x16, #8; br x16; .quad _acnh_ctx_hook", so x30 is the game caller's return. */
.text

    .globl _acnh_ctx_hook
    .p2align 2
_acnh_ctx_hook:
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
    adrp x16, _acnh_ctx_lock@PAGE         // acnh_ctx_lock(context)
    add x16, x16, _acnh_ctx_lock@PAGEOFF
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
    bl acnh_ctx_body                      // the original function; it returns here
    stp x0, x1, [sp, #88]
    stp q0, q1, [sp, #240]
    ldr x0, [sp, #16]                     // acnh_ctx_unlock(context)
    adrp x16, _acnh_ctx_unlock@PAGE
    add x16, x16, _acnh_ctx_unlock@PAGEOFF
    mov x17, #0
    bl _runtime_hle_switch
    ldp x0, x1, [sp, #88]
    ldp q0, q1, [sp, #240]
    ldp x29, x30, [sp]
    add sp, sp, #0x110
    ret

/* The four instructions the hook replaced (the original prologue's stack setup), then the rest of it. */
    .p2align 2
acnh_ctx_body:
    sub sp, sp, #160
    str x27, [sp, #64]
    stp x26, x25, [sp, #80]
    stp x24, x23, [sp, #96]
    adrp x16, _acnh_ctx_resume@PAGE
    ldr x16, [x16, _acnh_ctx_resume@PAGEOFF]    // main + 0x1be5b40
    br x16
