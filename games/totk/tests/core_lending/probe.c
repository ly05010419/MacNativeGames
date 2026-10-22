/* CPU only, against the real runtime/threads.c built for Breath of the Wild (GAME_BOTW=1): core lending
   (BOTW_CORE_TIMESLICE_MS). A priority-44 thread pinned to core 1 polls in guest code without any SVC, as 1.9.0's job
   workers poll their sead::JobQueue; then a priority-46 thread pinned to core 1 wants that core and ends the poll when it
   runs. Two cases:
   - lend: the poll is where the game allows lending (runtime_game_spin_lendable). Expected with BOTW_CORE_TIMESLICE_MS=2:
     the priority-46 thread runs within 500 ms, and the poller gets its core back and exits.
   - hold: the same poll where the game does not allow lending. Expected: the priority-46 thread does not run within
     500 ms (the host then ends the poll).
   Without BOTW_CORE_TIMESLICE_MS neither runs within 500 ms: Horizon's rule, a worse priority never preempts.
   The poll counts as top-level guest code through the host-stack slot the preemption signal checks (runtime_hle_tsd_off).
   Prints one JSON line: {"lend": <ran within 500 ms>, "hold": <ran within 500 ms>}. */
#include "../../runtime/runtime.h"
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
void runtime_thread_init(void) {}
uint64_t runtime_now_ns(void) { mach_timebase_info_data_t t; mach_timebase_info(&t); return (uint64_t)((__uint128_t)mach_absolute_time() * t.numer / t.denom); }
void *runtime_symbol(const char *name) { (void)name; runtime_fail("unexpected guest symbol"); }
uint64_t runtime_hle_tsd_off;                                   /* hle_stack.c: set while guest code runs at the top level */
int runtime_guest_code_pc(uint64_t pc) { (void)pc; return 1; }  /* loader.c */
static pthread_key_t g_slot;                                    /* the host-stack slot: 1 = polling, 2 = polling where lending is allowed */
int runtime_game_spin_lendable(uint64_t pc, const uint64_t *x) { (void)pc; (void)x; return pthread_getspecific(g_slot) == (void *)2; }
static void need(int c, const char *m) { if (!c) runtime_fail(m); }
static void kernel(unsigned id, uint64_t *r) { need(svc_thread((int)id, r) == 1, "service dispatch"); }

static atomic_int g_mode, g_polling, g_stop;
static void poller(uint64_t arg) {
    (void)arg; pthread_setspecific(g_slot, (void *)(intptr_t)atomic_load(&g_mode)); atomic_store(&g_polling, 1);
    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {}
    pthread_setspecific(g_slot, NULL); uint64_t r[31] = {0}; kernel(0x0a, r);
}
static void lender(uint64_t arg) { (void)arg; atomic_store_explicit(&g_stop, 1, memory_order_release); uint64_t r[31] = {0}; kernel(0x0a, r); }
static uint32_t start(void (*entry)(uint64_t), int prio, int core) {
    uint8_t *stack = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); need(stack != MAP_FAILED, "guest stack");
    uint64_t r[31] = {0}; r[1] = (uintptr_t)entry; r[3] = (uintptr_t)stack + (1 << 20); r[4] = (uint64_t)prio; r[5] = (uint64_t)core;
    kernel(8, r); need(!r[0], "CreateThread"); uint32_t h = (uint32_t)r[1];
    r[0] = h; kernel(9, r); need(!r[0], "StartThread"); return h;
}
static int exited_within(uint32_t h, uint64_t ns) {
    uint64_t r[31] = {0}; r[1] = (uintptr_t)&h; r[2] = 1; r[3] = ns; kernel(0x18, r);
    need(r[0] == 0 || r[0] == 0xea01, "wait result"); return r[0] == 0;
}
static int ran_beside_poll(int mode) {
    atomic_store(&g_mode, mode); atomic_store(&g_polling, 0); atomic_store(&g_stop, 0);
    uint32_t p = start(poller, 44, 1);
    while (!atomic_load(&g_polling)) usleep(100);
    uint32_t l = start(lender, 46, 1);
    int ran = exited_within(l, 500000000);
    atomic_store_explicit(&g_stop, 1, memory_order_release);   /* not lent: the host ends the poll, then the lender runs */
    need(exited_within(l, 2000000000) && exited_within(p, 2000000000), "both threads exited");
    uint64_t r[31] = {p}; kernel(0x16, r); r[0] = l; kernel(0x16, r);
    return ran;
}
int main(void) {
    svc_thread_main_init();
    need(!pthread_key_create(&g_slot, NULL) && !pthread_setspecific(g_slot, (void *)7), "host-stack slot");
    runtime_hle_tsd_off = g_slot * sizeof(void *);
    uintptr_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    need(*(uintptr_t *)(tp + runtime_hle_tsd_off) == 7, "the slot is where the preemption signal reads it");
    pthread_setspecific(g_slot, NULL);
    int lend = ran_beside_poll(2), hold = ran_beside_poll(1);
    printf("{\"lend\":%s,\"hold\":%s}\n", lend ? "true" : "false", hold ? "true" : "false");
    return 0;
}
