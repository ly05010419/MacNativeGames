/* The JIT's cache maintenance as the game does it (libnx armDCacheFlush on the writable view, armICacheInvalidate on the
   executable view, both stepping by the line sizes CTR_EL0 reports) on memory mapped twice the way svc_memory.c maps code
   memory: with the CTR_EL0 value loader.c gives the guest (HOST_CTR_EL0), freshly written code must run. 128-byte steps
   left stale instructions on an M4 (2026-11-22). */
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <signal.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
static sigjmp_buf jb; static void trap(int s) { siglongjmp(jb, s); }
int main(void) {
    const uint64_t ctr = HOST_CTR_EL0;
    const uintptr_t dline = 4u << ((ctr >> 16) & 15), iline = 4u << (ctr & 15);
    signal(SIGILL, trap); signal(SIGBUS, trap); signal(SIGSEGV, trap);
    mach_vm_address_t backing = 0, res = 0; vm_prot_t cur, max;
    mach_vm_allocate(mach_task_self(), &backing, 0x10000, VM_FLAGS_ANYWHERE);
    mach_vm_allocate(mach_task_self(), &res, 0x40000, VM_FLAGS_ANYWHERE);
    mach_vm_address_t rw = res, rx = res + 0x20000;
    if (mach_vm_remap(mach_task_self(), &rw, 0x10000, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), backing, FALSE, &cur, &max, VM_INHERIT_NONE) ||
        mach_vm_remap(mach_task_self(), &rx, 0x10000, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), backing, FALSE, &cur, &max, VM_INHERIT_NONE) ||
        mprotect((void *)rx, 0x10000, PROT_READ | PROT_EXEC)) { printf("FAIL: dual mapping\n"); return 1; }
    unsigned bad = 0;
    for (unsigned i = 0; i < 4000; i++) {
        uintptr_t off = (i * 72) % 0x8000;
        uint32_t code[2] = {0xd2800000u | ((i & 0xffff) << 5), 0xd65f03c0u};   /* mov x0, #i ; ret */
        memcpy((void *)(rw + off), code, 8);
        for (uintptr_t a = (rw + off) & ~(dline - 1); a < rw + off + 8; a += dline) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        __asm__ volatile("dsb sy");
        for (uintptr_t a = (rx + off) & ~(iline - 1); a < rx + off + 8; a += iline) __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
        __asm__ volatile("dsb sy\n isb");
        if (sigsetjmp(jb, 1)) { bad++; continue; }
        if (((long (*)(void))(rx + off))() != (long)(i & 0xffff)) bad++;
    }
    printf("lines %lu/%lu stale %u\n", (unsigned long)dline, (unsigned long)iline, bad);
    printf(bad ? "jit FAILED\n" : "jit ok\n"); return bad != 0;
}
