.text
.globl _probe_action_masks
.p2align 2
/* x0 = installed entry, x1 = synthetic device, x2 = results. */
_probe_action_masks:
 stp x21, x22, [sp, #-64]!
 stp x29, x30, [sp, #16]
 stp x19, x20, [sp, #32]
 mov x19, x2
 mov x20, x1
 mov x8, #8
 mov x9, #9
 mov x10, #10
 mov x11, #11
 mov x12, #12
 mov x13, #13
 mov x14, #14
 mov x15, #15
 mov x16, #16
 mov x17, #17
 cmp x1, x1
 blr x0
 stp x22, x21, [x19]
 str x20, [x19, #16]
 stp x8, x9, [x19, #32]
 stp x10, x11, [x19, #48]
 stp x12, x13, [x19, #64]
 stp x14, x15, [x19, #80]
 stp x16, x17, [x19, #96]
 mrs x8, nzcv
 str x8, [x19, #24]
 ldp x19, x20, [sp, #32]
 ldp x29, x30, [sp, #16]
 ldp x21, x22, [sp], #64
 ret

.globl _probe_gameplay_raw
.p2align 2
_probe_gameplay_raw:
 stp x19, x20, [sp, #-64]!
 stp x21, x27, [sp, #16]
 stp x22, x30, [sp, #32]
 mov x20, x0
 mov x21, x1
 mov x0, x1
 mov x22, x2
 mov x19, x3
 mov x8, #8
 mov x9, #9
 mov x27, #27
 cmp x1, x1
 blr x20
 stp x8, x9, [x22]
 str x27, [x22, #16]
 mrs x8, nzcv
 str x8, [x22, #24]
 ldp x21, x27, [sp, #16]
 ldp x22, x30, [sp, #32]
 ldp x19, x20, [sp], #64
 ret

.globl _probe_query_masks
.p2align 2
_probe_query_masks:
 stp x19, x20, [sp, #-48]!
 stp x21, x30, [sp, #16]
 mov x19, x1
 mov x20, x2
 add x21, x19, #0x30
 mov x1, #1
 mov x2, #2
 mov x3, #3
 mov x4, #4
 mov x5, #5
 mov x6, #6
 mov x7, #7
 mov x8, #8
 mov x9, #9
 mov x10, #10
 mov x11, #11
 mov x12, #12
 mov x13, #13
 mov x14, #14
 mov x15, #15
 mov x16, #16
 mov x17, #17
 cmp x1, x1
 blr x0
 stp x0, x1, [x20]
 stp x2, x3, [x20, #16]
 stp x4, x5, [x20, #32]
 stp x6, x7, [x20, #48]
 stp x8, x9, [x20, #64]
 stp x10, x11, [x20, #80]
 stp x12, x13, [x20, #96]
 stp x14, x15, [x20, #112]
 stp x16, x17, [x20, #128]
 mrs x0, nzcv
 str x0, [x20, #144]
 ldp x21, x30, [sp, #16]
 ldp x19, x20, [sp], #48
 ret
