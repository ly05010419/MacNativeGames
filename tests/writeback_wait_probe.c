/* The test driver inserts production wb_wait_gpu here. Use the real scheduler;
   only translation/GPU completion are stand-ins. No game data or Metal needed. */
#include "threads.c"
#include "nvn_impl.h"
#include "nvn_obj.h"
#include "totk_timeline.h"   /* WB_WAIT_GPU's timeline events (d1966c5); threads.c has the weak totk_tl_on / totk_tl */
#include <assert.h>
#include <stdatomic.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "FAIL %s\n", message); exit(2); }
uint64_t runtime_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
uint64_t runtime_hle_tsd_off;
int runtime_guest_code_pc(uint64_t pc) { (void)pc; return 0; }
pthread_key_t runtime_keys[8];
int botw_wb_staged_skip_on(void) { return 1; }   /* nvn_mtl.m's default: staged lists do not wait for the uploads */

typedef struct { BOTW_WB_ENT_FIELDS } wb_ent;
static thr_t caller, worker;
static _Atomic int completed, g_up_pending;
static int drains, waits, full_wait;
static uint64_t expected_fence;
int mtl_wait(uint64_t fence, uint64_t timeout);
int totk_wb_fence_on(void);
static void mtl_finish_gpu(void);

/* PRODUCTION_WAIT */

void nvn_x_drain(void) {
    thr_t *t = me_or_null();
    int before = t ? t->cur_core : -1;
    { NVN_BLOCKING(); assert(!t || t->cur_core == -1); }
    assert(!t || t->cur_core == before);   /* a nested guard must not retake it */
    drains++;
}
int mtl_wait(uint64_t fence, uint64_t timeout) {
    assert(fence == expected_fence && timeout == UINT64_MAX);
    thr_t *t = me_or_null();
    assert(!t || t->cur_core == -1);
    if (t) {
        /* The completion depends on a worker restricted to the caller's core.
           Holding it across the wait would prevent this worker from running. */
        assert(!pthread_join(worker.pt, NULL));
        assert(atomic_load(&completed));
    }
    waits++;
    return 1;
}
int totk_wb_fence_on(void) { return !full_wait; }
static void mtl_finish_gpu(void) { mtl_wait(expected_fence, UINT64_MAX); }

static void *complete_on_worker(void *unused) {
    (void)unused;
    assert(!pthread_setspecific(g_keythr, &worker));
    pthread_mutex_lock(&G);
    core_take(&worker);
    atomic_store(&completed, 1);
    core_put(&worker);
    pthread_mutex_unlock(&G);
    return NULL;
}

static void guest_wait(wb_ent **list, int nested) {
    atomic_store(&completed, 0);
    pthread_mutex_lock(&G); core_take(&caller); pthread_mutex_unlock(&G);
    assert(caller.cur_core == 0);
    assert(!pthread_create(&worker.pt, NULL, complete_on_worker, NULL));
    for (;;) {
        pthread_mutex_lock(&G); int queued = g_core_wait != NULL; pthread_mutex_unlock(&G);
        if (queued) break;
        usleep(100);
    }
    if (nested) {
        NVN_BLOCKING();
        wb_wait_gpu(list, 2);
        assert(caller.cur_core == -1);
    } else wb_wait_gpu(list, 2);
    assert(caller.cur_core == 0 && g_core_owner[0] == &caller);
    pthread_mutex_lock(&G); core_put(&caller); pthread_mutex_unlock(&G);
}

int main(int argc, char **argv) {
    full_wait = argc > 1 && atoi(argv[1]);
    unsetenv("TOTK_FREE_CORES"); unsetenv("TOTK_CORE_DIAG"); unsetenv("DM_NVN_TSV");
    pthread_key_t reserved;
    assert(!pthread_key_create(&reserved, NULL));
    assert(!pthread_key_create(&g_keythr, NULL));
    caller = (thr_t){.handle=0x1101, .cur_core=-1, .mask=1, .core=0, .prio=44};
    worker = (thr_t){.handle=0x1102, .cur_core=-1, .mask=1, .core=0, .prio=44};
    assert(!pthread_cond_init(&caller.cv, NULL)); assert(!pthread_cond_init(&worker.cv, NULL));
    wb_ent a = {.fence=5}, b = {.fence=9};
#ifdef GAME_BOTW
    a.stage_fence = 7; b.stage_fence = 11;
    expected_fence = 11;
#else
    expected_fence = 9;
#endif
    wb_ent *list[] = {&a, &b};
    assert(!pthread_setspecific(g_keythr, &caller));
    guest_wait(list, 0);
    guest_wait(list, 1);
    /* The translation thread and GPU probes have no guest core to give up. */
    assert(!pthread_setspecific(g_keythr, NULL));
    wb_wait_gpu(list, 2);
    assert(drains == 3 && waits == 3 && !g_core_owner[0]);
    puts("write-back waits release and restore the core; nested and host calls passed");
    return 0;
}
