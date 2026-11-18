// Short timed waits as polls, through real thread SVCs on one guest thread.
// Drive the run/gap clock explicitly: a CI deschedule longer than SPIN_GAP_NS
// legitimately resets the run, so 20 ms of wall time need not include a backoff.
// Host wait deadlines still use the real clock and pthread timed waits.
#include "../dm.h"
#include <assert.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

static _Atomic uint64_t poll_clock;
static uint64_t test_now_ns(void) { return atomic_load(&poll_clock); }
static _Thread_local struct timespec wait_start;
static int test_clock_gettime(clockid_t clock, struct timespec *ts) {
    int r = clock_gettime(clock, ts);
    if (!r && clock == CLOCK_REALTIME) wait_start = *ts;
    return r;
}
static int signal_wait, waiter_ready;   // signal_wait is set before starting the signaller; waiter_ready is protected by G
static pthread_cond_t ready_cv = PTHREAD_COND_INITIALIZER;
static int test_timedwait(pthread_cond_t *cv, pthread_mutex_t *mutex, const struct timespec *deadline) {
    if (!signal_wait) return pthread_cond_timedwait(cv, mutex, deadline);
    int64_t ns = (deadline->tv_sec - wait_start.tv_sec) * 1000000000ll + deadline->tv_nsec - wait_start.tv_nsec;
    assert(ns == 250000);   // the production wait must have been lengthened, not merely counted as a backoff
    waiter_ready = 1; pthread_cond_signal(&ready_cv);
    // Only the signal case has no OS timeout: the signaller must run after this
    // waiter releases G, even when CI takes longer than 250 us to schedule it.
    // The real condition wait and SignalProcessWideKey still perform the wake.
    return pthread_cond_wait(cv, mutex);
}
#define now_ns test_now_ns
#define clock_gettime test_clock_gettime
#define pthread_cond_timedwait test_timedwait
#include "../hle_thread.c"
#undef now_ns
#undef clock_gettime
#undef pthread_cond_timedwait

static _Atomic uint32_t mutex_word, condition_key;
static uint32_t me_handle;
static uint64_t backoffs(void) { thr_t *me = pthread_getspecific(g_keythr); assert(me); return me->nbackoff; }
// One condition variable wait as the SDK's TimedWait makes it: take the lock in user mode, wait (the timeout releases it without taking it back)
static int64_t cv_wait(int64_t ns) {
    atomic_store(&mutex_word, me_handle);
    uint64_t r[32] = {(uint64_t)&mutex_word, (uint64_t)&condition_key, me_handle, (uint64_t)ns};
    svc_thread(0x1C, r);
    int64_t result = (int64_t)r[0];
    if (!result) {
        assert(atomic_load(&mutex_word) == me_handle);   // a signalled waiter owns the lock again
    }
    return result;
}
static void poll_step(int64_t wait_ns, int sleep_too, uint32_t event) {
    if (event) { uint64_t r[32] = {0, (uint64_t)&event, 1, (uint64_t)wait_ns}; svc_thread(0x18, r); assert(r[0] == R_TIMEOUT); }
    else assert(cv_wait(wait_ns) == R_TIMEOUT);
    if (sleep_too) { uint64_t r[32] = {1000}; svc_thread(0x0B, r); }
}
static void poll_run(int64_t wait_ns, int sleep_too, uint32_t event) {
    atomic_fetch_add(&poll_clock, 30000);   // guest work ends the previous run
    uint64_t b0 = backoffs();
    for (int i = 0; i < 200; i++) {   // 0 through 1.99 ms: no backoff yet
        poll_step(wait_ns, sleep_too, event);
        assert(backoffs() == b0);
        atomic_fetch_add(&poll_clock, 10000);
    }
    poll_step(wait_ns, sleep_too, event);   // exactly 2 ms: the wait (and any yield) backs off
    assert(backoffs() == b0 + 1 + sleep_too);
}
static void *signaller(void *unused) {
    (void)unused;
    pthread_mutex_lock(&G);
    while (!waiter_ready) pthread_cond_wait(&ready_cv, &G);
    assert(g_wait && g_wait->wkind == 2 && g_wait->wkey == (uint64_t)&condition_key);
    pthread_mutex_unlock(&G);
    uint64_t r[32] = {(uint64_t)&condition_key, 1}; svc_thread(0x1D, r);
    return NULL;
}
int main(void) {
    alarm(20);
    me_handle = svc_thread_main_init();
    poll_run(500, 0, 0);
    puts("PASS: 500 ns condition variable waits back off at 2 ms, not before");
    poll_run(30, 1, 0);
    puts("PASS: 30 ns waits between 1 us sleeps back off at 2 ms, not before");
    uint64_t r[32] = {0}; svc_thread(0x45, r); assert(r[0] == 0); uint32_t ev_w = (uint32_t)r[1], ev_r = (uint32_t)r[2];
    poll_run(30, 1, ev_r);
    puts("PASS: the same with WaitSynchronization");

    uint64_t b0 = backoffs();
    atomic_fetch_add(&poll_clock, 20000); poll_step(500, 0, 0);
    assert(backoffs() == b0 + 1);
    for (int i = 0; i < 200; i++) { atomic_fetch_add(&poll_clock, 20001); poll_step(500, 0, 0); }
    assert(backoffs() == b0 + 1);
    puts("PASS: a 20 us gap keeps the run; longer gaps reset it");

    poll_run(500, 0, 0);
    b0 = backoffs();
    r[0] = 0; r[1] = me_handle; svc_thread(0x0C, r); assert(r[0] == 0);
    poll_step(500, 0, 0); assert(backoffs() == b0);
    puts("PASS: an intervening non-poll SVC resets the run");

    poll_run(500, 0, 0);
    b0 = backoffs();
    signal_wait = 1;
    pthread_t s; assert(!pthread_create(&s, NULL, signaller, NULL));
    assert(cv_wait(500) == 0);
    assert(!pthread_join(s, NULL)); signal_wait = 0;
    assert(backoffs() == b0 + 1);
    poll_step(500, 0, 0); assert(backoffs() == b0 + 1);
    puts("PASS: a lengthened wait returns at the signal and the next poll starts a new run");
    r[0] = ev_w; svc_thread(0x16, r); r[0] = ev_r; svc_thread(0x16, r);
    return 0;
}
