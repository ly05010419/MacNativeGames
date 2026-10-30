/* Exercise the real scheduler's collection and snapshot, with no game or guest code execution. */
#include "../games/totk/runtime/threads.c"
#include <assert.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <unistd.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL %s\n", message); exit(2); }
static _Atomic uint64_t clock_reads;
uint64_t runtime_now_ns(void) { atomic_fetch_add(&clock_reads,1); mach_timebase_info_data_t t; mach_timebase_info(&t); return (uint64_t)((__uint128_t)mach_absolute_time()*t.numer/t.denom); }
uint64_t runtime_hle_tsd_off;   // both games' asynchronous preemption reads these (TOTK's since PR #488)
int runtime_guest_code_pc(uint64_t pc) { (void)pc; return 0; }
pthread_key_t runtime_keys[8];
static thr_t owner, waiter;
static _Atomic int finished;
static uint64_t own_wait;
static void *wait_for_core(void *unused) {
    (void)unused; pthread_setspecific(g_keythr, &waiter);
    pthread_mutex_lock(&G); core_take(&waiter); core_put(&waiter); pthread_mutex_unlock(&G);
    uint64_t all, n; dm_core_wait_stats(&own_wait, &all, &n);
    atomic_store(&finished, 1); return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2); int on = atoi(argv[1]);
    unsetenv("TOTK_FREE_CORES");
    if (on==2) setenv("TOTK_CORE_DIAG","1",1); else unsetenv("TOTK_CORE_DIAG");
    if (on) setenv("DM_NVN_TSV", "/unused", 1); else unsetenv("DM_NVN_TSV");
    pthread_key_t reserved; assert(!pthread_key_create(&reserved, NULL)); assert(!pthread_key_create(&g_keythr, NULL));
    owner = (thr_t){.handle=0x1101, .cur_core=-1, .mask=1, .core=0, .prio=44, .mport=pthread_mach_thread_np(pthread_self())};
    waiter = (thr_t){.handle=0x1102, .cur_core=-1, .mask=1, .core=0, .prio=44};
    pthread_cond_init(&owner.cv,NULL); pthread_cond_init(&waiter.cv,NULL);
    pthread_mutex_lock(&G); core_take(&owner); pthread_mutex_unlock(&G);
    assert(!pthread_create(&waiter.pt, NULL, wait_for_core, NULL));
    for (;;) { pthread_mutex_lock(&G); int queued = g_core_wait != NULL; pthread_mutex_unlock(&G); if (queued) break; usleep(100); }
    if (on==2) usleep(120000);  // exercise owner sampling too, including BotW's 100 ms diagnostic threshold
    // While a waiter is sleeping, snapshots must remain consistent and monotone.
    uint64_t prev_ns=0, prev_n=0;
    for (int i=0;i<1000;i++) { uint64_t self, all, n; dm_core_wait_stats(&self,&all,&n); assert(all>=prev_ns && n>=prev_n); prev_ns=all; prev_n=n; }
    pthread_mutex_lock(&G); core_put(&owner); pthread_mutex_unlock(&G);
    while (!atomic_load(&finished)) { uint64_t self,all,n; dm_core_wait_stats(&self,&all,&n); assert(all>=prev_ns && n>=prev_n); prev_ns=all; prev_n=n; }
    assert(!pthread_join(waiter.pt, NULL));
    thread_basic_info_data_t info; mach_msg_type_number_t info_count=THREAD_BASIC_INFO_COUNT;
    assert(thread_info(owner.mport,THREAD_BASIC_INFO,(thread_info_t)&info,&info_count)==KERN_SUCCESS);
    uint64_t self,all,n; dm_core_wait_stats(&self,&all,&n);
    if (on) assert(n==1 && all>0 && own_wait==all);
    else {
        assert(!self && !all && !n && !own_wait);
        // Disabled read takes neither G nor a clock, including from a caller already holding G.
        uint64_t before=atomic_load(&clock_reads);
        pthread_mutex_lock(&G); dm_core_wait_stats(&self,&all,&n); pthread_mutex_unlock(&G);
        assert(atomic_load(&clock_reads)==before);
        assert(!g_core_wait_all_ns && !waiter.core_wait_ns && !owner.core_held_ns);
    }
#ifdef GAME_BOTW
    // A wake can queue and grant before the host thread reaches core_take. Count that wait exactly once too.
    pthread_mutex_lock(&G);
    core_take(&owner); waiter.started=1;
    wake_queue(&waiter); assert(waiter.cqueued);
    core_put(&owner); assert(waiter.cur_core==0);
    core_take(&waiter); core_put(&waiter);
    pthread_mutex_unlock(&G);
    dm_core_wait_stats(&self,&all,&n);
    assert(on ? n==2 && all>=own_wait : !n && !all);
#endif
    puts(on ? "enabled snapshots and both-game collection passed" : "disabled collection and lock-free read passed");
    return 0;
}
