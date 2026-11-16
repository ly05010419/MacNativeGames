#include <stdint.h>

typedef uint64_t (*Resolve)(uint64_t);
Resolve runtime_lazy_bind_target;
static uint64_t expected_slot;
extern uint64_t test_lazy_invoke(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                 uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                 double, double, double, double, double, double, double, double);
static uint64_t target(uint64_t a, uint64_t b, uint64_t slot, uint64_t d, uint64_t e,
                       uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j,
                       double x, double y, double z, double p, double q, double r, double s, double t) {
    return a == 1 && b == 2 && slot == expected_slot && d == 4 && e == 5 && f == 6 &&
           g == 7 && h == 8 && i == 9 && j == 10 && x == 1.25 && y == 2.5 && z == 3.5 && p == 4.5 &&
           q == 5.5 && r == 6.5 && s == 7.5 && t == 8.5 ? 0x1234 : 0;
}
static uint64_t resolve(uint64_t slot) {
    if (slot != expected_slot) return 0;
    __asm__ volatile("mov x1, #0\n\tmov x2, #0\n\tmov x3, #0\n\tmov x4, #0\n\t"
                     "mov x5, #0\n\tmov x6, #0\n\tmov x7, #0\n\tmov x8, #0\n\t"
                     "fmov d0, xzr\n\tfmov d1, xzr\n\tfmov d2, xzr\n\tfmov d3, xzr\n\t"
                     "fmov d4, xzr\n\tfmov d5, xzr\n\tfmov d6, xzr\n\tfmov d7, xzr"
                     : : : "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8",
                             "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "memory");
    return (uintptr_t)&target;
}
int main(void) {
    expected_slot = 0x12345678;
    runtime_lazy_bind_target = resolve;
    return test_lazy_invoke(1, 2, expected_slot, 4, 5, 6, 7, 8, 9, 10,
                            1.25, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5) != 0x1234;
}
