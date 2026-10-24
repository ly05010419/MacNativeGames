/* Copied into checked 1.0.0 text padding before RX sealing. A sequence-checked
   16-source table supplies edge, held and release. Retry concurrent Apply; the
   device object is read, never modified. ADRP and final branches are relocated. */
.section __TEXT,__const
.p2align 2
.globl _semantic_action_code, _semantic_action_code_end
.globl _semantic_action_data_adrp, _semantic_action_resume
_semantic_action_code:
 stp x8, x9, [sp, #-96]!
 stp x10, x11, [sp, #16]
 stp x12, x13, [sp, #32]
 stp x14, x15, [sp, #48]
 stp x16, x17, [sp, #64]
 mrs x14, nzcv
 str x14, [sp, #80]
 str x20, [sp, #88]
_semantic_action_data_adrp:
 adrp x16, 0
 add x16, x16, #64
Laction_snapshot:
 ldr x20, [sp, #88]
 ldar w15, [x16]
 tbnz w15, #0, Laction_snapshot
 add x16, x16, #8
 ldr w8, [x20, #8]
 ldr w9, [x20, #0x114]
 ldr w10, [x20, #12]
 mov w22, wzr
 mov w21, wzr
 mov w20, wzr
 adr x12, Laction_raw_masks
 mov w17, #16
Laction_loop:
 ldr w11, [x12], #4
 ldrh w13, [x16], #2
 tst w8, w11
 csel w14, w13, wzr, ne
 orr w22, w22, w14
 tst w9, w11
 csel w14, w13, wzr, ne
 orr w21, w21, w14
 tst w10, w11
 csel w14, w13, wzr, ne
 orr w20, w20, w14
 subs w17, w17, #1
 b.ne Laction_loop
 sub x16, x16, #40
 dmb ishld
 ldar w13, [x16]
 cmp w13, w15
 b.ne Laction_snapshot
 and w22, w22, #0x3fff
 /* Crouch/scope are edge actions. Attack and ability also consume releases. */
 and w21, w21, #0xfff
 ubfx w20, w20, #3, #2
 ldr x14, [sp, #80]
 msr nzcv, x14
 ldp x10, x11, [sp, #16]
 ldp x12, x13, [sp, #32]
 ldp x14, x15, [sp, #48]
 ldp x16, x17, [sp, #64]
 ldp x8, x9, [sp], #96
_semantic_action_resume:
 b _semantic_action_resume
Laction_raw_masks:
 .long 1, 2, 8, 16, 0x2000, 0x4000, 4, 32
 .long 0x10000, 0x20000, 0x40000, 0x80000, 0x80, 0x40, 0xc00, 0x1200
_semantic_action_code_end:

/* 1.4.3: the original entry loads and gate-filters the edge, held and release
   masks into w10, w9 and w8, then zeroes w21 before the tbnz chain. This stub
   takes those gated masks (w10 edge, w9 held, w8 release), builds w21 edge,
   w20 held and w22 release, and resumes at the original continuation. The
   gated masks are saved so a concurrent table update can retry the snapshot. */
.p2align 2
.globl _semantic_action_v143_code, _semantic_action_v143_code_end
.globl _semantic_action_v143_data_adrp, _semantic_action_v143_resume
_semantic_action_v143_code:
 stp x8, x9, [sp, #-112]!
 stp x10, x11, [sp, #16]
 stp x12, x13, [sp, #32]
 stp x14, x15, [sp, #48]
 stp x16, x17, [sp, #64]
 mrs x14, nzcv
 str x14, [sp, #80]
 str x20, [sp, #88]
 str w10, [sp, #96]
 str w9, [sp, #100]
 str w8, [sp, #104]
_semantic_action_v143_data_adrp:
 adrp x16, 0
 add x16, x16, #64
Laction143_snapshot:
 ldar w15, [x16]
 tbnz w15, #0, Laction143_snapshot
 add x16, x16, #8
 ldp w10, w9, [sp, #96]
 ldr w8, [sp, #104]
 mov w11, w10
 mov w12, w9
 mov w13, w8
 mov w22, wzr
 mov w21, wzr
 mov w20, wzr
 adr x14, Laction143_raw_masks
 mov w17, #16
Laction143_loop:
 ldr w10, [x14], #4
 ldrh w9, [x16], #2
 tst w11, w10
 csel w8, w9, wzr, ne
 orr w21, w21, w8
 tst w12, w10
 csel w8, w9, wzr, ne
 orr w20, w20, w8
 tst w13, w10
 csel w8, w9, wzr, ne
 orr w22, w22, w8
 subs w17, w17, #1
 b.ne Laction143_loop
 sub x16, x16, #40
 dmb ishld
 ldar w10, [x16]
 cmp w10, w15
 b.ne Laction143_snapshot
 and w21, w21, #0x3fff
 and w20, w20, #0xfff
 ubfx w22, w22, #3, #2
 ldr x14, [sp, #80]
 msr nzcv, x14
 ldp x10, x11, [sp, #16]
 ldp x12, x13, [sp, #32]
 ldp x14, x15, [sp, #48]
 ldp x16, x17, [sp, #64]
 ldp x8, x9, [sp], #112
_semantic_action_v143_resume:
 b _semantic_action_v143_resume
Laction143_raw_masks:
 .long 1, 2, 8, 16, 0x2000, 0x4000, 4, 32
 .long 0x10000, 0x20000, 0x40000, 0x80000, 0x80, 0x40, 0xc00, 0x1200
_semantic_action_v143_code_end:

/* Gameplay UI requests use raw-shaped masks, unlike the operation dispatcher.
   Translate all 16 sources; preserve unrelated bits. Ordinary menu readers never call
   this translator. w8 is the only output. Registers and NZCV are preserved. */
.p2align 2
.globl _semantic_raw_code, _semantic_raw_code_end, _semantic_raw_data_adrp
_semantic_raw_code:
 stp x9, x10, [sp, #-96]!
 stp x11, x12, [sp, #16]
 stp x13, x14, [sp, #32]
 stp x15, x16, [sp, #48]
 stp x17, x30, [sp, #64]
 mrs x14, nzcv
 str x14, [sp, #80]
_semantic_raw_data_adrp:
 adrp x16, 0
 add x16, x16, #64
Lraw_snapshot:
 ldar w10, [x16]
 tbnz w10, #0, Lraw_snapshot
 add x16, x16, #8
 mov w11, #0x7eff
 movk w11, #0xf, lsl #16
 bic w15, w8, w11
 adr x12, Lraw_target_masks
 mov w17, #1
Lraw_loop:
 ldr w11, [x12, x17, lsl #2]
 ldrh w13, [x16], #2
 clz w13, w13
 mov w14, #32
 sub w13, w14, w13
 ldr w13, [x12, x13, lsl #2]
 tst w8, w11
 csel w14, w13, wzr, ne
 orr w15, w15, w14
 add w17, w17, #1
 cmp w17, #17
 b.ne Lraw_loop
 sub x16, x16, #40
 dmb ishld
 ldar w13, [x16]
 cmp w13, w10
 b.ne Lraw_snapshot
 mov w8, w15
 ldr x14, [sp, #80]
 msr nzcv, x14
 ldp x11, x12, [sp, #16]
 ldp x13, x14, [sp, #32]
 ldp x15, x16, [sp, #48]
 ldp x17, x30, [sp, #64]
 ldp x9, x10, [sp], #96
 ret
Lraw_target_masks:
 .long 0, 1, 2, 8, 16, 0x2000, 0x4000, 4, 32
 .long 0x10000, 0x20000, 0x40000, 0x80000, 0x80, 0x40, 0xc00, 0x1200
_semantic_raw_code_end:

/* The generic UI query node publishes five button masks. Invoke the native
   module-specific adapter after the loads, preserving its live GPRs and flags.
   SIMD values cannot be live here: the preceding virtual call clobbers them. */
.p2align 3
.globl _semantic_query_code, _semantic_query_code_end
.globl _semantic_query_callback, _semantic_query_resume, _semantic_query_original_load
_semantic_query_code:
 stp x0, x1, [sp, #-176]!
 stp x2, x3, [sp, #16]
 stp x4, x5, [sp, #32]
 stp x6, x7, [sp, #48]
 stp x8, x9, [sp, #64]
 stp x10, x11, [sp, #80]
 stp x12, x13, [sp, #96]
 stp x14, x15, [sp, #112]
 stp x16, x17, [sp, #128]
 stp x18, x30, [sp, #144]
 mrs x0, nzcv
 str x0, [sp, #160]
 mov x0, x19
 ldr x16, _semantic_query_callback
 blr x16
 ldr x0, [sp, #160]
 msr nzcv, x0
 ldp x2, x3, [sp, #16]
 ldp x4, x5, [sp, #32]
 ldp x6, x7, [sp, #48]
 ldp x8, x9, [sp, #64]
 ldp x10, x11, [sp, #80]
 ldp x12, x13, [sp, #96]
 ldp x14, x15, [sp, #112]
 ldp x16, x17, [sp, #128]
 ldp x18, x30, [sp, #144]
 ldp x0, x1, [sp], #176
_semantic_query_original_load:
 ldr x9, [x21, #0x20]
_semantic_query_resume:
 b _semantic_query_resume
.p2align 3
_semantic_query_callback:
 .quad 0
_semantic_query_code_end:
