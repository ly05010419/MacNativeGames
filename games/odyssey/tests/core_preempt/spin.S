/* A native ARM64 no-SVC guest loop, linked read-only; no JIT or game binary needed. */
.text
.p2align 2
.globl _probe_guest_spin, _probe_guest_end
_probe_guest_spin:
    ldar w1, [x0]
    cbz w1, _probe_guest_spin
    ret
_probe_guest_end:
