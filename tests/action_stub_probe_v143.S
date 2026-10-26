.text
.globl _probe_core_gate
.p2align 2
/* x0 = gate entry (main+0xf23e28), x1 = device, w2 = gate, x3 = out[4] */
_probe_core_gate:
 stp x20, x21, [sp, #-32]!
 stp x22, x30, [sp, #16]
 mov x5, x3
 mov x6, x0
 mov x20, x1
 mov w0, w2
 blr x6
 stp w21, w20, [x5]
 str w22, [x5, #8]
 mrs x7, nzcv
 str w7, [x5, #12]
 ldp x22, x30, [sp, #16]
 ldp x20, x21, [sp], #32
 ret

.globl _probe_raw_dual
.p2align 2
/* x0 = fused ldp site, x1 = device, x2 = out (w8, w26, nzcv) */
_probe_raw_dual:
 stp x21, x26, [sp, #-32]!
 stp x22, x30, [sp, #16]
 mov x5, x2
 mov x6, x0
 mov x21, x1
 blr x6
 str w8, [x5]
 str w26, [x5, #4]
 mrs x3, nzcv
 str w3, [x5, #8]
 ldp x22, x30, [sp, #16]
 ldp x21, x26, [sp], #32
 ret

.globl _probe_raw_bow
.p2align 2
/* x0 = bow load site, x1 = device, x2 = out (w8, nzcv) */
_probe_raw_bow:
 stp x9, x30, [sp, #-16]!
 mov x3, x2
 mov x9, x0
 mov x0, x1
 blr x9
 str w8, [x3]
 mrs x4, nzcv
 str w4, [x3, #4]
 ldp x9, x30, [sp], #16
 ret

.globl _probe_query
.p2align 2
/* x0 = query site, x1 = node, x2 = out (x10, x1, x19, nzcv) */
_probe_query:
 stp x19, x30, [sp, #-16]!
 mov x19, x1
 mov x4, x2
 add x1, x19, #0x30
 blr x0
 str x10, [x4]
 str x1, [x4, #8]
 str x19, [x4, #16]
 mrs x5, nzcv
 str x5, [x4, #24]
 ldp x19, x30, [sp], #16
 ret
