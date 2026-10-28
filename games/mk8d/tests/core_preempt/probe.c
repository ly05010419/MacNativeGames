/* Actual virtual-core contention, signal delivery and guest-PC guard from threads.c. */
#include "../../runtime/threads.c"
#include <assert.h>
#include <mach/mach_time.h>
#include <unistd.h>

extern void probe_guest_spin(_Atomic int *done), probe_guest_end(void);
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "FAIL %s\n", s); exit(2); }
uint64_t runtime_now_ns(void) { mach_timebase_info_data_t t; mach_timebase_info(&t); return (uint64_t)((__uint128_t)mach_absolute_time()*t.numer/t.denom); }
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off) {
    if (pc < (uintptr_t)probe_guest_spin || pc >= (uintptr_t)probe_guest_end) return 0;
    *name="probe"; *off=pc-(uintptr_t)probe_guest_spin; return 1;
}
static thr_t owner, waiter;
static _Atomic int ready, done, host_locked, release_host;
static uint64_t acquired_ns;
static int host_guard;
static void *own_core(void *unused) {
    (void)unused; pthread_setspecific(g_keythr, &owner);
    pthread_mutex_lock(&G); core_take(&owner); pthread_mutex_unlock(&G);
    if (host_guard) {
        pthread_mutex_lock(&G); atomic_store(&host_locked, 1);
        /* The signal handler must return while interrupted in host code holding G. */
        while (!atomic_load(&release_host)) usleep(100);
        pthread_mutex_unlock(&G);
    }
    atomic_store(&ready, 1); probe_guest_spin(&done);
    pthread_mutex_lock(&G); core_put(&owner); pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, NULL); return NULL;
}
static void *wait_core(void *unused) {
    (void)unused; pthread_setspecific(g_keythr, &waiter);
    pthread_mutex_lock(&G); core_take(&waiter);
    acquired_ns=now_ns(); atomic_store(&done, 1); core_put(&waiter); pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, NULL); return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2); host_guard = !strcmp(argv[1], "host");
    int starve = !strcmp(argv[1], "starve");
    unsetenv("TOTK_FREE_CORES");
    /* Production uses a nonzero TLS key (the loader creates keys first). */
    pthread_key_t reserved; assert(!pthread_key_create(&reserved, NULL));
    assert(!pthread_key_create(&g_keythr, NULL));
    owner = (thr_t){.cur_core=-1, .mask=1, .core=0, .prio=44};
    waiter = (thr_t){.cur_core=-1, .mask=1, .core=0, .prio=starve?45:43};
    pthread_cond_init(&owner.cv,NULL); pthread_cond_init(&waiter.cv,NULL);
    core_watch_start(); assert(!pthread_create(&owner.pt,NULL,own_core,NULL));
    if (host_guard) {
        while (!atomic_load(&host_locked)) usleep(100);
        owner.preempt=1; owner.kicked=1;
        assert(!pthread_kill(owner.pt, CORE_PREEMPT_SIGNAL));
        usleep(10000); atomic_store(&release_host,1);
    }
    while (!atomic_load(&ready)) usleep(100);
    uint64_t start=now_ns(); assert(!pthread_create(&waiter.pt,NULL,wait_core,NULL));
    assert(!pthread_join(waiter.pt,NULL)); assert(!pthread_join(owner.pt,NULL));
    assert(acquired_ns >= start && acquired_ns-start < 1000000000);
    if (starve) assert(acquired_ns-start >= CORE_STARVE_NS);
    printf("%s: no-SVC owner yielded in %.2f ms\n",argv[1],(acquired_ns-start)/1e6);
    return 0;
}
