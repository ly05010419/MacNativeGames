/* Exercise the real runtime/threads.c pause barrier & resume behavior (both TotK and BotW). */
#include "../games/totk/runtime/threads.c"
#include <assert.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <unistd.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL %s\n", message); exit(2); }
uint64_t runtime_now_ns(void) { mach_timebase_info_data_t t; mach_timebase_info(&t); return (uint64_t)((__uint128_t)mach_absolute_time() * t.numer / t.denom); }
uint64_t runtime_hle_tsd_off;
int runtime_guest_code_pc(uint64_t pc) { (void)pc; return 0; }
pthread_key_t runtime_keys[8];

static thr_t worker;
static _Atomic int worker_ready, worker_stop;
static _Atomic uint64_t worker_loops;

static void *worker_fn(void *unused) {
    (void)unused;
    pthread_setspecific(g_keythr, &worker);
    pthread_mutex_lock(&G);
    core_take(&worker);
    atomic_store(&worker_ready, 1);
    pthread_mutex_unlock(&G);
    while (!atomic_load(&worker_stop)) {
        runtime_core_checkpoint();
        atomic_fetch_add(&worker_loops, 1);
        usleep(200);
    }
    pthread_mutex_lock(&G);
    core_put(&worker);
    pthread_mutex_unlock(&G);
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    int mode = atoi(argv[1]);
    unsetenv("TOTK_FREE_CORES");
    if (mode == 1) setenv("TOTK_ASYNC_PREEMPT", "0", 1);
    else unsetenv("TOTK_ASYNC_PREEMPT");
    pthread_key_t reserved;
    assert(!pthread_key_create(&reserved, NULL));
    assert(!pthread_key_create(&g_keythr, NULL));

    if (mode == 0) {
        assert(!runtime_threads_is_paused());
        assert(runtime_threads_pause(50000000ull) == 0);
        assert(runtime_threads_is_paused() == 1);
        assert(runtime_threads_pause(50000000ull) == 0);
        runtime_threads_resume();
        assert(runtime_threads_is_paused() == 0);
        runtime_threads_resume();

        worker = (thr_t){.handle = 0x1101, .cur_core = -1, .mask = 1, .core = 0, .prio = 44};
        pthread_cond_init(&worker.cv, NULL);
        assert(!pthread_create(&worker.pt, NULL, worker_fn, NULL));
        while (!atomic_load(&worker_ready)) usleep(100);

        assert(runtime_threads_pause(1000000000ull) == 0);
        assert(runtime_threads_is_paused() == 1 && worker.cur_core == -1);
        uint64_t paused_at = atomic_load(&worker_loops);
        usleep(20000);
        assert(atomic_load(&worker_loops) == paused_at);

        runtime_threads_resume();
        assert(runtime_threads_is_paused() == 0);
        for (int i = 0; i < 500 && atomic_load(&worker_loops) == paused_at; i++) usleep(1000);
        assert(atomic_load(&worker_loops) > paused_at);
        atomic_store(&worker_stop, 1);
        assert(!pthread_join(worker.pt, NULL));
    } else {
        /* With TOTK_ASYNC_PREEMPT=0, timeout must roll back cleanly without raising unhandled SIGUSR2. */
        worker = (thr_t){.handle = 0x1102, .cur_core = -1, .mask = 1, .core = 0, .prio = 44, .pt = pthread_self()};
        pthread_cond_init(&worker.cv, NULL);
        pthread_mutex_lock(&G); core_take(&worker); pthread_mutex_unlock(&G);
        assert(runtime_threads_pause(30000000ull) == -1);
        assert(!runtime_threads_is_paused() && !g_pause_requested && worker.cur_core == 0);
        pthread_mutex_lock(&G); core_put(&worker); pthread_mutex_unlock(&G);
    }
    puts(mode == 0 ? "pass: threads pause checkpoint barrier and resume" : "pass: threads pause timeout rollback without async preempt");
}
