/* Gate register preservation adapted from D2R Mobile (MIT), pinned in third_party/d2r/README.md. */
.text
    .globl _runtime_gate
    .p2align 2
_runtime_gate:
    // Switch to this thread's host stack first (see hle_stack.S): the guest stack only holds x16/x17.
    // Frame above the 800-byte register area: [sp+800] guest sp (points at saved x16/x17), [sp+808] host top or 0.
    stp x16, x17, [sp, #-16]!
    mrs x16, tpidrro_el0
    adrp x17, _runtime_hle_tsd_off@PAGE
    ldr x17, [x17, _runtime_hle_tsd_off@PAGEOFF]
    add x16, x16, x17
    ldr x17, [x16]
    cbz x17, 1f
    str xzr, [x16]
    mov x16, sp
    sub sp, x17, #16
    stp x16, x17, [sp]
    b 2f
1:  mov x16, sp
    sub sp, sp, #16
    stp x16, xzr, [sp]
2:  ldp x16, x17, [x16]
    // 开 256 字节帧，保存 x0..x30（x0..x29 成对，x30 单独），门可读改 x0..x30
    // Open a 256-byte frame and save x0..x30 (x0..x29 in pairs, x30 alone); the gate may read and modify x0..x30
    sub sp, sp, #800
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    stp q0, q1, [sp, #256]
    stp q2, q3, [sp, #288]
    stp q4, q5, [sp, #320]
    stp q6, q7, [sp, #352]
    stp q8, q9, [sp, #384]
    stp q10, q11, [sp, #416]
    stp q12, q13, [sp, #448]
    stp q14, q15, [sp, #480]
    stp q16, q17, [sp, #512]
    stp q18, q19, [sp, #544]
    stp q20, q21, [sp, #576]
    stp q22, q23, [sp, #608]
    stp q24, q25, [sp, #640]
    stp q26, q27, [sp, #672]
    stp q28, q29, [sp, #704]
    stp q30, q31, [sp, #736]
    mrs x9, nzcv
    str x9, [sp, #768]
    mrs x9, fpsr
    str x9, [sp, #776]
    mrs x9, fpcr
    str x9, [sp, #784]
    mov x0, x30              // 返回点 = 门标识 | Return point = gate identifier
    mov x1, sp               // regs
    bl _runtime_dispatch
    ldr x9, [sp, #808]          // give the host stack back to this thread
    cbz x9, 3f
    mrs x10, tpidrro_el0
    adrp x11, _runtime_hle_tsd_off@PAGE
    ldr x11, [x11, _runtime_hle_tsd_off@PAGEOFF]
    str x9, [x10, x11]
3:
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
    // 门可改任意通用寄存器：svc 出参在 x0..x8，mrs cntpct 的目标可以是任意 xN（sdk 33 处里 14 处是 x19..x23）
    // ⇒ x0..x29 全部回读（x18 是苹果平台寄存器，客体的 x18 已改写到线程槽，跳过不碰）
    // A gate may modify any general-purpose register: svc out-parameters are in x0..x8, and the target of mrs cntpct can be any xN (14 of the 33 sites in sdk are x19..x23)
    // ⇒ read back all of x0..x29 (x18 is the Apple platform register; the guest's x18 has been rewritten to the thread slot, so skip it and leave it alone)
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldr x19, [sp, #152]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    ldr x16, [sp, #800]         // back to the guest stack; final x16/x17 travel through its save slot
    ldr x17, [sp, #128]
    str x17, [x16]
    ldr x17, [sp, #136]
    str x17, [x16, #8]
    mov sp, x16
    ldp x16, x17, [sp], #16
    ret

