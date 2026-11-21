// Big-stack entry (hle_bigstack.c, hle.h HLE_FORCE_BIG): a take-over called on a small guest stack runs on the thread's host stack with its register arguments intact, and dm_bigstack_caller
// gives back the game's call site both after a switch and straight through off the guest block (the audio Start / Stop / Close log it, PR #721)
#include "../hle.h"
#include <stdio.h>
#include <sys/mman.h>

static uintptr_t g_lo, g_hi;   // the fake guest stack: 16 KB above a guard page
int dm_guest_as_contains(uintptr_t a) { return a >= g_lo && a < g_hi; }

static uintptr_t g_fp;
static void *g_site;
// 64 KB of locals: four times the fake guest stack, so running on it would fault in the guard page
HLE_FORCE_BIG("_ZN4test5probeEmmd", "unit test", uint64_t, probe, uint64_t a, uint64_t b, double d) {
    volatile char big[64 << 10];
    big[0] = 1; big[sizeof big - 1] = 2;
    g_fp = (uintptr_t)__builtin_frame_address(0);
    g_site = dm_bigstack_caller(__builtin_return_address(0));
    return a * 3 + b + (uint64_t)d + (uint64_t)(big[0] + big[sizeof big - 1]);
}

// call_on(a, b, d, top, fn): calls fn(a, b, d) with sp = top (0: on this stack); _call_on_site is the return address the call leaves, the "game's call site"
uint64_t call_on(uint64_t a, uint64_t b, double d, uintptr_t top, const void *fn);
extern const char call_on_site[];
__asm__(".text\n.p2align 2\n.globl _call_on\n_call_on:\n"
        "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n"
        "    cbz x2, 1f\n    mov sp, x2\n"
        "1:  blr x3\n"
        ".globl _call_on_site\n_call_on_site:\n"
        "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n");

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

int main(void) {
    size_t guard = 16384, size = 16384;
    uint8_t *m = mmap(NULL, guard + size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    mprotect(m, guard, PROT_NONE);
    g_lo = (uintptr_t)m; g_hi = (uintptr_t)m + guard + size;

    // on the guest stack: switched to the host stack, arguments kept, call site given back
    uint64_t r = call_on(5, 7, 2.0, g_hi, probe_bs);
    CHECK(r == 5 * 3 + 7 + 2 + 3, "arguments carried over through the switch");
    CHECK(g_fp && !dm_guest_as_contains(g_fp), "the implementation ran off the guest stack");
    CHECK(g_site == (void *)call_on_site, "dm_bigstack_caller gives the call site back after a switch");

    // a second call on the same thread reuses the thread's host stack
    g_site = NULL; r = call_on(1, 1, 1.0, g_hi, probe_bs);
    CHECK(r == 3 + 1 + 1 + 3 && g_site == (void *)call_on_site, "second switched call");

    // off the guest block: jumped to straight, the return address is already the caller's
    g_site = NULL; r = call_on(2, 0, 0.0, 0, probe_bs);
    CHECK(r == 6 + 3, "arguments on the direct path");
    CHECK(g_site == (void *)call_on_site, "dm_bigstack_caller passes the call site through on the direct path");

    printf("big-stack entry: %d/%d checks %s\n", checks - fails, checks, fails ? "✗" : "✓");
    return fails != 0;
}
