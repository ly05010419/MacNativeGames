/* D2R Mobile entry.S thread entry, MIT. */
.text
    .globl _dm_thread_enter
    .p2align 2
_dm_thread_enter:
    mov x4, sp
    stp x4, x29, [x3]
    mov sp, x2
    mov x29, #0
    mov x16, x0
    mov x0, x1
    adr x30, 1f
    br x16
1:  bl _dm_thread_returned
    brk #0

// Restore the owning pthread stack before signaling completion / pthread_exit.
.globl _runtime_thread_leave
.p2align 2
_runtime_thread_leave:
    ldp x4, x29, [x0]
    mov sp, x4
    mov x0, x1
    br x2
