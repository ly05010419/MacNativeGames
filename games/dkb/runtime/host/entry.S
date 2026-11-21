// Common entries + static stub tables.
// 0) unimpl_stubs: named stubs for unimplemented imports, the k-th = movz x9,#k; b unimpl_entry (static code; iOS does not allow runtime generation)
// 1) unimpl_entry: stub entry for unimplemented imports, x9 = stub number; save x0-x8+lr and call unimpl_c(idx, regs).
// 2) dm_gate_stubs + dm_gate: the rewriter replaces svc / cntpct reads with a gate block that pushes the guest's x29 / x30 and calls the address in the thread's KGATE slot, which is
//    this thread's stub (thread.c guest_gate_stack). The stub passes the top of the thread's gate stack to dm_gate, which saves x0-x30 there, calls gate_dispatch(ret, regs) on that stack,
//    writes the registers back and returns. Horizon's kernel never writes a thread's stack during an SVC; the gate stack keeps it so.
    .text
    .globl _unimpl_stubs
    .p2align 3
_unimpl_stubs:
    .set k, 0
    .rept 4096                // = hle.c MAX_STUB
    movz x9, #k
    b _unimpl_entry
    .set k, k + 1
    .endr

    .globl _unimpl_entry
    .p2align 2
_unimpl_entry:
    stp x29, x30, [sp, #-96]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    mov x0, x9
    add x1, sp, #16
    bl _unimpl_c
    ldp x29, x30, [sp], #96
    ret

// The gate stacks' tops, one per gate slot (thread.c guest_gate_stack). Page-aligned, so that stub k reaches its entry with adrp and an immediate offset alone
    .globl _dm_gate_top
    .zerofill __DATA,__bss,_dm_gate_top,8192,12      // 8 × 1024 (= thread.c GATE_SLOTS)

// Stub k (16 bytes) serves gate slot k: x29 = the return point (the gate block holds the guest's x29 at [sp] and reloads it after the gate), x30 = the slot's gate stack top
    .globl _dm_gate_stubs
    .p2align 4
_dm_gate_stubs:
    .set k, 0
    .rept 1024                // = thread.c GATE_SLOTS
    mov x29, x30
    adrp x30, _dm_gate_top@PAGE
    ldr x30, [x30, #(8 * k)]
    b _dm_gate
    .set k, k + 1
    .endr

    .globl _dm_gate
    .p2align 2
_dm_gate:
    // The register frame (800 bytes) goes at the top of the gate stack, not below the guest's sp: on Horizon an SVC leaves the thread's stack as it was, including the words below sp.
    // A collector that records a thread's sp before a blocking call and then scans from there finds pointers its callees left below their returned frames: Streets of Rage 4's BRUTE
    // garbage collector parks a thread in Thread.Sleep with the caller's x20 only in such a word (main+0x2ff4c80's frame), which the old frame here overwrote, so a live array was freed.
    // Layout as before: x0..x30 at +0 (x30 = the return point), q0..q31 at +256, nzcv / fpsr / fpcr at +768 / +776 / +784, and at +792 the gate block's sp
    // (the guest's x29 / x30 the block pushed; the guest's stack follows at +16)
    sub x30, x30, #800
    stp x0, x1, [x30, #0]
    stp x2, x3, [x30, #16]
    stp x4, x5, [x30, #32]
    stp x6, x7, [x30, #48]
    stp x8, x9, [x30, #64]
    stp x10, x11, [x30, #80]
    stp x12, x13, [x30, #96]
    stp x14, x15, [x30, #112]
    stp x16, x17, [x30, #128]
    stp x18, x19, [x30, #144]
    stp x20, x21, [x30, #160]
    stp x22, x23, [x30, #176]
    stp x24, x25, [x30, #192]
    stp x26, x27, [x30, #208]
    str x28, [x30, #224]
    stp q0, q1, [x30, #256]
    stp q2, q3, [x30, #288]
    stp q4, q5, [x30, #320]
    stp q6, q7, [x30, #352]
    stp q8, q9, [x30, #384]
    stp q10, q11, [x30, #416]
    stp q12, q13, [x30, #448]
    stp q14, q15, [x30, #480]
    stp q16, q17, [x30, #512]
    stp q18, q19, [x30, #544]
    stp q20, q21, [x30, #576]
    stp q22, q23, [x30, #608]
    stp q24, q25, [x30, #640]
    stp q26, q27, [x30, #672]
    stp q28, q29, [x30, #704]
    stp q30, q31, [x30, #736]
    mrs x9, nzcv
    str x9, [x30, #768]
    mrs x9, fpsr
    str x9, [x30, #776]
    mrs x9, fpcr
    str x9, [x30, #784]
    ldr x9, [sp]              // the guest's x29 (the crash backtrace walks the x29 chain from here)
    stp x9, x29, [x30, #232]  // x29, then x30 = the return point
    mov x9, sp
    str x9, [x30, #792]
    // A gate entered on its own gate stack would overwrite the frame of the gate below it: none is (gate handlers do not run guest code); stop if one ever is
    add x10, x30, #800
    cmp x9, x10
    b.hs 1f
    sub x10, x10, #0x100000   // = thread.c GATE_STACK_SIZE
    cmp x9, x10
    b.lo 1f
    mov sp, x30
    mov x0, x9
    bl _dm_gate_nested
1:  mov sp, x30
    ldr x29, [sp, #232]
    ldr x0, [sp, #240]        // Return point = gate identifier
    mov x1, sp                // regs
    bl _gate_dispatch
    ldr x9, [sp, #784]
    msr fpcr, x9
    ldr x9, [sp, #776]
    msr fpsr, x9
    ldr x9, [sp, #768]
    msr nzcv, x9
    ldp q0, q1, [sp, #256]
    ldp q2, q3, [sp, #288]
    ldp q4, q5, [sp, #320]
    ldp q6, q7, [sp, #352]
    ldp q8, q9, [sp, #384]
    ldp q10, q11, [sp, #416]
    ldp q12, q13, [sp, #448]
    ldp q14, q15, [sp, #480]
    ldp q16, q17, [sp, #512]
    ldp q18, q19, [sp, #544]
    ldp q20, q21, [sp, #576]
    ldp q22, q23, [sp, #608]
    ldp q24, q25, [sp, #640]
    ldp q26, q27, [sp, #672]
    ldp q28, q29, [sp, #704]
    ldp q30, q31, [sp, #736]
    // A gate may modify any general-purpose register: svc out-parameters are in x0..x8, and the target of mrs cntpct can be any xN (14 of the 33 sites in sdk are x19..x23)
    // ⇒ read back all of x0..x28 (x18 is the Apple platform register; the guest's x18 has been rewritten to the thread slot, so skip it and leave it alone).
    // x29 / x30 are the gate block's to restore (from the words it pushed): here they carry the block's sp and the return point
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldr x19, [sp, #152]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldr x28, [sp, #224]
    ldr x30, [sp, #240]
    ldr x29, [sp, #792]
    mov sp, x29
    ret

// 3) dm_thread_enter(entry, arg, sp): guest thread start. Switch to the guest stack given by the SDK and call entry with x0 = argument;
//    Horizon threads must exit via ExitThread; returning from entry is reported as an error.
    .globl _dm_thread_enter
    .p2align 2
_dm_thread_enter:
    mov sp, x2
    mov x29, #0
    mov x16, x0
    mov x0, x1
    adr x30, 1f
    br x16
1:  bl _dm_thread_returned
    brk #0
