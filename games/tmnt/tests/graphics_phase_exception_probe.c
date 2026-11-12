/* SPDX-License-Identifier: MIT
 * Bounded synthetic catch/unwind probe for the actual graphics phase module.
 * The timed wait is a test watchdog, never part of the production protocol.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void tmnt_graphics_owner_gate(unsigned, uint64_t *);
void tmnt_graphics_owner_thread_exit(void);
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "%s\n", s); abort(); }

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int caught, allow_clear, away_seen, presenter_done;
static _Atomic int core_away, core_back;
static uint32_t count;

int runtime_core_away(void) {
    atomic_fetch_add(&core_away, 1);
    pthread_mutex_lock(&mu); away_seen = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    return 1;
}
void runtime_core_back(int *away) { assert(*away == 1); atomic_fetch_add(&core_back, 1); }

static void *writer(void *unused) {
    (void)unused;
    uint64_t r[31] = {0}; r[8] = (uintptr_t)&count; r[0] = 1;
    jmp_buf recovery;
    if (setjmp(recovery) == 0) {
        tmnt_graphics_owner_gate(0x90, r);
        count = 1;
        tmnt_graphics_owner_gate(0x95, r);
        longjmp(recovery, 1); /* synthetic caught managed exception */
    }
    pthread_mutex_lock(&mu);
    caught = 1; pthread_cond_broadcast(&cv);
    while (!allow_clear) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    r[9] = 0;
    tmnt_graphics_owner_gate(0x96, r);
    count = 0;
    tmnt_graphics_owner_gate(0x91, r);
    tmnt_graphics_owner_thread_exit();
    return NULL;
}

static void *presenter(void *unused) {
    (void)unused;
    uint64_t r[31] = {0}; r[8] = (uintptr_t)&count;
    tmnt_graphics_owner_gate(0x92, r);
    r[9] = count; assert(r[9] == 0);
    tmnt_graphics_owner_gate(0x93, r);
    pthread_mutex_lock(&mu); presenter_done = 1; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    return NULL;
}

int main(void) {
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, writer, NULL));
    pthread_mutex_lock(&mu);
    while (!caught) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    assert(!pthread_create(&b, NULL, presenter, NULL));
    pthread_mutex_lock(&mu);
    while (!away_seen) pthread_cond_wait(&cv, &mu);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_nsec += 100000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    int rc = 0;
    while (!presenter_done && rc == 0) rc = pthread_cond_timedwait(&cv, &mu, &deadline);
    assert(rc == ETIMEDOUT && !presenter_done);
    allow_clear = 1; pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    assert(presenter_done && atomic_load(&core_away) == 1 && atomic_load(&core_back) == 1);
    puts("synthetic caught exception: waiter blocks until explicit clear, then resumes");
}
