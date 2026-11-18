// Big-stack entry (hle.h HLE_FORCE_BIG): a take-over function runs directly on the stack of the guest thread that calls it, and the stack a game gives a thread can be small --
// the save thread of Super Mario 3D World + Bowser's Fury has only 8 KB (CreateThread stack top 0x…9fe0, crash 0x21a0 below it); when it calls fs::MountSaveData,
// the host's mount path (two 1 KB-class path buffers in h_fs_MountSave + two 1100-byte ones in mount_ex + snprintf) ran past the stack bottom, SIGBUS in __vfprintf.
// On hardware this call is one IPC that needs a few hundred bytes of stack; the host's file system take-overs size their buffers by PATH_MAX, and one file open takes 6-7 KB (-fstack-usage: OpenFile 1.3 KB -> host_path_ex 1.9 KB -> path_component 3.2 KB).
// How: what gets registered is a static stub (one per take-over function, which loads the implementation's address into x16 and jumps to dm_bigstack_enter). The entry looks at the current sp: inside the guest address block (a guest thread stack)
// it switches to this thread's host stack (512 KB, mapped on first use, with a guard page), calls the implementation with x0-x8 / d0-d7 untouched, and switches back after it returns; outside the guest block (a host thread, or already on the host stack) it jumps straight through.
// The implementation's own return address is then the instruction after the entry's call, not the game's call site: the entry keeps the game's return address beside the saved sp at the top of the host stack,
// and dm_bigstack_caller (hle.h) gives it back, so take-overs that log their call site (audio Start / Stop / Close) name the game's code on either path.
// Only register arguments are carried over: functions with more than 8 integer / 8 floating-point arguments, or variadic ones reading from the stack, cannot use it.
// iOS does not allow generating executable code at run time, so the stubs are generated at compile time by the macro, not written at run time as DM_HLECOUNT does
#include <pthread.h>
#include <stdint.h>
#include <sys/mman.h>
#include "dm.h"

int dm_guest_as_contains(uintptr_t a);   // hle_svc.c

#define BS_SIZE (512u << 10)
#define BS_GUARD 16384u
static pthread_key_t g_bs_key;
static pthread_once_t g_bs_once = PTHREAD_ONCE_INIT;
static void bs_free(void *top) { if (top) munmap((uint8_t *)top - BS_SIZE - BS_GUARD, BS_SIZE + BS_GUARD); }
static void bs_init(void) { if (pthread_key_create(&g_bs_key, bs_free)) die("big stack pthread_key_create"); }

// sp inside the guest address block -> the top of this thread's host stack (16-byte aligned); otherwise 0 (no switch)
uintptr_t dm_bigstack_top(uintptr_t sp);
uintptr_t dm_bigstack_top(uintptr_t sp) {
    if (!dm_guest_as_contains(sp)) return 0;
    pthread_once(&g_bs_once, bs_init);
    uint8_t *top = pthread_getspecific(g_bs_key);
    if (!top) {
        uint8_t *m = mmap(NULL, BS_SIZE + BS_GUARD, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (m == MAP_FAILED) { LOG("[RED] big stack mapping failed, staying on the guest stack this time"); return 0; }
        mprotect(m, BS_GUARD, PROT_NONE);   // guard page: running off the host stack itself crashes here instead of quietly corrupting another mapping
        top = m + BS_GUARD + BS_SIZE;
        pthread_setspecific(g_bs_key, top);
    }
    return (uintptr_t)top;
}

// x16 = implementation. Frame (on the original stack, 0xa0 bytes): +0x00 x0-x8, x16, +0x50 d0-d7, +0x90 x29 / x30. After the switch the original sp and the game's return address are stored at the new stack top; the sp is restored when the implementation returns
__asm__(
    ".text\n"
    ".p2align 2\n"
    ".globl _dm_bigstack_enter\n"
    ".private_extern _dm_bigstack_enter\n"
    "_dm_bigstack_enter:\n"
    "    sub sp, sp, #0xa0\n"
    "    stp x29, x30, [sp, #0x90]\n"
    "    add x29, sp, #0x90\n"
    "    stp x0, x1, [sp, #0x00]\n"
    "    stp x2, x3, [sp, #0x10]\n"
    "    stp x4, x5, [sp, #0x20]\n"
    "    stp x6, x7, [sp, #0x30]\n"
    "    stp x8, x16, [sp, #0x40]\n"
    "    stp d0, d1, [sp, #0x50]\n"
    "    stp d2, d3, [sp, #0x60]\n"
    "    stp d4, d5, [sp, #0x70]\n"
    "    stp d6, d7, [sp, #0x80]\n"
    "    mov x0, sp\n"
    "    bl _dm_bigstack_top\n"
    "    mov x17, x0\n"
    "    ldp x0, x1, [sp, #0x00]\n"
    "    ldp x2, x3, [sp, #0x10]\n"
    "    ldp x4, x5, [sp, #0x20]\n"
    "    ldp x6, x7, [sp, #0x30]\n"
    "    ldp x8, x16, [sp, #0x40]\n"
    "    ldp d0, d1, [sp, #0x50]\n"
    "    ldp d2, d3, [sp, #0x60]\n"
    "    ldp d4, d5, [sp, #0x70]\n"
    "    ldp d6, d7, [sp, #0x80]\n"
    "    cbz x17, 1f\n"
    "    mov x9, sp\n"
    "    ldr x10, [x9, #0x98]\n"
    "    mov sp, x17\n"
    "    stp x9, x10, [sp, #-16]!\n"
    "    blr x16\n"
    ".globl _dm_bigstack_ret\n"
    ".private_extern _dm_bigstack_ret\n"
    "_dm_bigstack_ret:\n"
    "    ldr x9, [sp]\n"
    "    mov sp, x9\n"
    "    ldp x29, x30, [sp, #0x90]\n"
    "    add sp, sp, #0xa0\n"
    "    ret\n"
    "1:\n"
    "    ldp x29, x30, [sp, #0x90]\n"
    "    add sp, sp, #0xa0\n"
    "    br x16\n");

// The return address inside an implementation that came through the switch is _dm_bigstack_ret; the game's own is in the slot the entry stored below this thread's host stack top. Only the outermost
// take-over of a thread switches (a nested one already runs off the guest block and is jumped to directly), so that slot belongs to the call in progress
void *dm_bigstack_caller(void *ra) {
    extern const char dm_bigstack_ret[];
    void **top = ra == (void *)dm_bigstack_ret ? pthread_getspecific(g_bs_key) : NULL;   // set: a switch happened on this thread
    return top ? top[-1] : ra;
}
