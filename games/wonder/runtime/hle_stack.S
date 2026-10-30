/* Host-stack switch for host implementations called from guest code.
   Horizon services run on kernel/sysmodule stacks, but the host functions that replace them would otherwise run
   on the calling guest thread's stack, and TOTK creates threads with stacks as small as 4 KiB. Each guest thread
   has a host stack top in a Darwin TSD slot (offset _runtime_hle_tsd_off from TPIDRRO_EL0); the slot is cleared
   while that stack is in use, so nested guest->host calls made from it stay on it. */
.text

/* x16 = host function, x17 = its wrapper index. Called at a guest call site: x0..x8 and v0..v7 carry arguments,
   x9..x17 are free. No host replacement takes stack-passed arguments (checked against all NVN prototypes and host
   overrides). While _runtime_hle_trace is set (TOTK_INPUT_FILE "hletrace", diagnostics) every call is first counted by
   _runtime_hle_note on the host stack, with the argument registers kept. */
    .globl _runtime_hle_switch
    .p2align 2
_runtime_hle_switch:
    mrs x9, tpidrro_el0
    adrp x10, _runtime_hle_tsd_off@PAGE
    ldr x10, [x10, _runtime_hle_tsd_off@PAGEOFF]
    ldr x11, [x9, x10]
    cbz x11, 1f
    str xzr, [x9, x10]
    mov x12, sp
    sub sp, x11, #32
    stp x29, x30, [sp]          // frame record links the host frames back to the guest caller
    stp x12, x11, [sp, #16]
    mov x29, sp
    adrp x13, _runtime_hle_trace@PAGE
    ldr w13, [x13, _runtime_hle_trace@PAGEOFF]
    cbnz w13, 2f
3:  blr x16
    ldp x12, x11, [sp, #16]
    ldp x29, x30, [sp]
    mov sp, x12
    mrs x9, tpidrro_el0
    adrp x10, _runtime_hle_tsd_off@PAGE
    ldr x10, [x10, _runtime_hle_tsd_off@PAGEOFF]
    str x11, [x9, x10]
    ret
1:  adrp x13, _runtime_hle_trace@PAGE      // already on a host stack (nested call, or a host thread)
    ldr w13, [x13, _runtime_hle_trace@PAGEOFF]
    cbnz w13, 4f
    br x16
2:  bl hle_note_kept                        // x30 is in the frame record and is reloaded from it after the call
    b 3b
4:  stp x29, x30, [sp, #-16]!
    mov x29, sp
    bl hle_note_kept
    ldp x29, x30, [sp], #16
    br x16

/* _runtime_hle_note(x17) with x0..x8, x16 and q0..q7 kept. */
    .p2align 2
hle_note_kept:
    stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp x8, x16, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    mov x0, x17
    bl _runtime_hle_note
    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldp x8, x16, [sp, #80]
    ldp x6, x7, [sp, #64]
    ldp x4, x5, [sp, #48]
    ldp x2, x3, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #224
    ret

/* x17 = wrapper index: x16 = _runtime_hle_targets[x17], then the switch. */
    .p2align 2
hle_dispatch:
    adrp x16, _runtime_hle_targets@PAGE
    add x16, x16, _runtime_hle_targets@PAGEOFF
    ldr x16, [x16, x17, lsl #3]
    b _runtime_hle_switch

/* Fixed wrapper thunks (16 bytes each): thunk i enters the dispatch with x17 = i. */
    .globl _runtime_hle_thunks
    .p2align 4
_runtime_hle_thunks:
.set hle_i, 0
.rept 4096
    mov x17, #hle_i
    b hle_dispatch
    brk #0xb5
    brk #0xb5
.set hle_i, hle_i+1
.endr
