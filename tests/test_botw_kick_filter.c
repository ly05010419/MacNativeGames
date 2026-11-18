/* Breath of the Wild's preemption signals (core_kick): an owner in a host function is marked but not signalled, a second
   signal is not sent while the first is unhandled, one is sent again once the handler ran or after KICK_PENDING_NS, and
   TOTK_KICK_FILTER=0 sends every one as before. Real threads.c, no game code. Build with -DGAME_BOTW=1. */
#include "../games/totk/runtime/threads.c"
#include <assert.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <unistd.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL %s\n", message); exit(2); }
uint64_t runtime_now_ns(void) { mach_timebase_info_data_t t; mach_timebase_info(&t); return (uint64_t)((__uint128_t)mach_absolute_time()*t.numer/t.denom); }
uint64_t runtime_hle_tsd_off;   // 0: the handler finds the owner outside game code and refuses, as in a host function
int runtime_guest_code_pc(uint64_t pc) { (void)pc; return 0; }
pthread_key_t runtime_keys[8];

static thr_t owner, waiter;
static volatile uintptr_t slot;   // the owner's host-stack slot: 0 while it runs a host function
static _Atomic int phase;
static void *owner_main(void *unused) {
    (void)unused; pthread_setspecific(g_keythr, &owner);
    sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &s, NULL);   // signals stay pending until phase 2
    pthread_mutex_lock(&G); core_take(&owner); pthread_mutex_unlock(&G);
    atomic_store(&phase, 1);
    while (atomic_load(&phase) < 2) usleep(100);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);   // the pending signal is handled here
    while (atomic_load(&phase) < 3) usleep(100);
    pthread_mutex_lock(&G); core_put(&owner); pthread_mutex_unlock(&G);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2); int filter = atoi(argv[1]);
    if (filter) unsetenv("TOTK_KICK_FILTER"); else setenv("TOTK_KICK_FILTER", "0", 1);
    unsetenv("TOTK_ASYNC_PREEMPT"); unsetenv("TOTK_FREE_CORES"); unsetenv("TOTK_STARVE");
    pthread_key_t reserved; assert(!pthread_key_create(&reserved, NULL)); assert(!pthread_key_create(&g_keythr, NULL));
    owner = (thr_t){.handle=0x1101, .cur_core=-1, .mask=1, .core=0, .prio=44, .hle_slot=&slot};
    waiter = (thr_t){.handle=0x1102, .cur_core=-1, .mask=1, .core=0, .prio=30};
    pthread_cond_init(&owner.cv, NULL); pthread_cond_init(&waiter.cv, NULL);
    assert(!pthread_create(&owner.pt, NULL, owner_main, NULL));
    while (atomic_load(&phase) < 1) usleep(100);

    pthread_mutex_lock(&G);
    // An owner in a host function: marked; signalled only without the filter.
    slot = 0;
    assert(core_kick(&waiter) == 1 && owner.preempt);
    if (filter) assert(g_preempt_kicks == 0 && g_kick_skip_host == 1);
    else assert(g_preempt_kicks == 1 && g_kick_skip_host == 0);
    // An owner in game code: one signal while it is unhandled (the kernel would merge a second one anyway).
    slot = 0x1000; __atomic_store_n(&owner.ap_sig_ns, 0, __ATOMIC_RELEASE);
    uint64_t sent = g_preempt_kicks;
    assert(core_kick(&waiter) == 1); assert(core_kick(&waiter) == 1);
    if (filter) assert(g_preempt_kicks == sent + 1 && g_kick_skip_pending == 1);
    else assert(g_preempt_kicks == sent + 2 && g_kick_skip_pending == 0);
    // Unhandled for longer than KICK_PENDING_NS: sent again (safety net).
    __atomic_store_n(&owner.ap_sig_ns, now_ns() - KICK_PENDING_NS - 1000000, __ATOMIC_RELEASE);
    sent = g_preempt_kicks; assert(core_kick(&waiter) == 1); assert(g_preempt_kicks == sent + 1);
    pthread_mutex_unlock(&G);

    // The owner handles its signal (and refuses: outside game code); the next kick signals it again.
    atomic_store(&phase, 2);
    for (int i = 0; __atomic_load_n(&owner.ap_sig_ns, __ATOMIC_ACQUIRE) && i < 20000; i++) usleep(100);
    assert(!__atomic_load_n(&owner.ap_sig_ns, __ATOMIC_ACQUIRE));
    assert(atomic_load(&g_preempt_refused) >= 1);
    pthread_mutex_lock(&G);
    sent = g_preempt_kicks; assert(core_kick(&waiter) == 1); assert(g_preempt_kicks == sent + 1);
    // A waiter that does not beat the owner neither marks nor signals it.
    waiter.prio = 50; owner.preempt = 0; sent = g_preempt_kicks;
    __atomic_store_n(&owner.ap_sig_ns, 0, __ATOMIC_RELEASE);
    assert(core_kick(&waiter) == 0 && !owner.preempt && g_preempt_kicks == sent);
    // "flag kick_filter" switches it while the game runs: an owner in a host function is signalled with it off.
    waiter.prio = 30; slot = 0; uint64_t host = g_kick_skip_host;
    assert(runtime_thread_flag("kick_filter", !filter) == 1 && runtime_thread_flag("other", 1) == 0);
    sent = g_preempt_kicks; assert(core_kick(&waiter) == 1);
    if (filter) assert(g_preempt_kicks == sent + 1 && g_kick_skip_host == host);   // now off
    else assert(g_preempt_kicks == sent && g_kick_skip_host == host + 1);         // now on
    pthread_mutex_unlock(&G);

    atomic_store(&phase, 3);
    assert(!pthread_join(owner.pt, NULL));
    printf("kick filter %s: %llu signals, %llu not sent in a host function, %llu not sent while pending, %llu refused\n",
           filter ? "on" : "off", (unsigned long long)g_preempt_kicks, (unsigned long long)g_kick_skip_host,
           (unsigned long long)g_kick_skip_pending, (unsigned long long)atomic_load(&g_preempt_refused));
    return 0;
}
