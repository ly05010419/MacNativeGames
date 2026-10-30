// A high-priority same-core notification thread must yield its guest core while
// its HLE blocks on the host condition variable, then resume notification delivery.
#include "../dm.h"
#include <assert.h>
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>
#include "../hle_thread.c"
static int observed_wait(pthread_cond_t *, pthread_mutex_t *);
#define pthread_cond_wait observed_wait
#ifndef HLE_OE_SOURCE
#define HLE_OE_SOURCE "../hle_oe.c"
#endif
#include HLE_OE_SOURCE
#undef pthread_cond_wait
static _Atomic int waiting, resumed, finish;
static uint32_t message;
static thr_t *notifier;
static int observed_wait(pthread_cond_t *cv, pthread_mutex_t *mu) {
    if (cv == &g_mq_cond) atomic_fetch_add(&waiting, 1);
    return pthread_cond_wait(cv, mu);
}
void dm_audio_suspend(int on) { (void)on; }
void guest_clock_freeze_for(int reason, int on) { (void)reason; (void)on; }
void dm_gfx_suspend(int on) { (void)on; }
static void timeout(int sig) { (void)sig; const char message[] = "[RED] notification scheduler test timed out\n"; write(2, message, sizeof message - 1); _exit(99); }
static void notify(uint64_t unused) {
    (void)unused;
    // Exercise the real gate's deferred QoS update after a priority change before StartThread.
    uint64_t regs[32] = {0}; thr_svc_enter(regs, 0); thr_svc_leave();
    pthread_mutex_lock(&G);
    assert(cur()->host_work == &dm_host_work && cur()->mport == pthread_mach_thread_np(pthread_self()));
    assert(!atomic_load(&cur()->qos_pending));
    pthread_mutex_unlock(&G);
    message=h_oe_PopMsg();
    atomic_store(&resumed, 1);
    while (!atomic_load(&finish)) sched_yield();
}
// The real thread_main registers the pthread and waits for StartThread. Only
// the assembly transition to a guest stack is replaced by this C entry point.
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) {
    (void)sp;
    ((void (*)(uint64_t))entry)(arg);
    uint64_t r[32] = {0}; svc_thread(0x0A, r);
    abort();
}
static void until_waiting(int count) {
    while (atomic_load(&waiting) < count) sched_yield();
    // observed_wait publishes while holding the queue mutex: acquiring it
    // here guarantees the notifier has entered pthread_cond_wait.
    pthread_mutex_lock(&g_mq_lock); pthread_mutex_unlock(&g_mq_lock);
    pthread_mutex_lock(&G);
    assert(!thr_runnable(notifier));
    assert(notifier->nhand == 0);
    pthread_mutex_unlock(&G);
}
int main(int argc, char **argv) {
    (void)argv;
    signal(SIGALRM,timeout);alarm(10);
    unsetenv("DM_OE_SCRIPT");
    setenv("DM_PREEMPT", argc > 1 ? "1" : "0", 1);
    svc_thread_main_init();
    thr_t *self=cur();self->mask=1;self->core=0;self->prio=44;
    uint64_t r[32] = {0, (uint64_t)notify, 0, 0, 28, 0};
    assert(svc_thread(0x08, r) && r[0] == 0);
    uint32_t handle = (uint32_t)r[1];
    pthread_mutex_lock(&G); notifier = h_get(handle, O_THREAD); pthread_mutex_unlock(&G);
    r[0] = handle; r[1] = 26;
    assert(svc_thread(0x0D, r) && r[0] == 0);
    r[1] = handle;
    assert(svc_thread(0x0C, r) && r[0] == 0 && r[1] == 26);
    r[0] = handle;
    // Unpatched PopNotificationMessage strands this StartThread handoff: the
    // notifier sleeps in the HLE while still counted as runnable on core 0.
    assert(svc_thread(0x09, r) && r[0] == 0);
    until_waiting(1);
    dm_yield_to_core_peers(0);
    dm_yield_to_core_peers(1);
    // A wake without a message must wait again without leaking native-wait
    // depth or restoring the handoff that ended at the first blocking boundary.
    pthread_mutex_lock(&g_mq_lock);
    pthread_cond_broadcast(&g_mq_cond);
    pthread_mutex_unlock(&g_mq_lock);
    until_waiting(2);
    dm_yield_to_core_peers(0);
    dm_yield_to_core_peers(1);
    dm_focus_set(2);
    while (!atomic_load(&resumed)) sched_yield();
    pthread_mutex_lock(&G);
    assert(notifier->native_wait == 0 && thr_runnable(notifier));
    assert(notifier->nhand == 0);
    pthread_mutex_unlock(&G);
    atomic_store(&finish, 1);
    r[1] = (uint64_t)&handle; r[2] = 1; r[3] = (uint64_t)-1;
    assert(svc_thread(0x18, r) && r[0] == 0);
    assert(message==15);
    r[0] = handle; assert(svc_thread(0x16, r) && r[0] == 0);
    puts("real thread registration, deferred QoS, start/exit, native wait and notification delivery passed");
    return 0;
}
