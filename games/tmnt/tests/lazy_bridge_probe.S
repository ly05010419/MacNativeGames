.text
.globl _test_lazy_invoke
.p2align 2
_test_lazy_invoke:
    mov x16, x2
    b _runtime_lazy_bind_entry
