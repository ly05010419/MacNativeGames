/* SPDX-License-Identifier: MIT
 * Conditional protocol test, not a managed-exception integration test.
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void tmnt_graphics_owner_gate(unsigned, uint64_t *);
void tmnt_graphics_owner_mark_caught_return(void);
void tmnt_graphics_owner_finish_caught_return(void);
void tmnt_graphics_owner_abandon_on_rethrow(void);
void tmnt_graphics_owner_thread_exit(void);
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static uint32_t count;
static int set_done, request_abandon, mark_done, allow_finish, abandon_done, presenter_done, finish, away_seen;
static int rethrow_mode;
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "%s\n", s); abort(); }
int runtime_core_away(void) {
    pthread_mutex_lock(&mu);
    away_seen = 1; pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return 1;
}
void runtime_core_back(int *away) { assert(*away == 1); }

static void *writer(void *unused) {
    (void)unused;
    uint64_t r[31] = {0}; r[8] = (uintptr_t)&count; r[0] = 1;
    tmnt_graphics_owner_gate(0x90, r); count = 1;
    tmnt_graphics_owner_gate(0x95, r);
    tmnt_graphics_owner_finish_caught_return(); /* normal return has no mark */
    pthread_mutex_lock(&mu);
    set_done = 1; pthread_cond_broadcast(&cv);
    while (!request_abandon) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    if (rethrow_mode) {
        tmnt_graphics_owner_abandon_on_rethrow();
        tmnt_graphics_owner_abandon_on_rethrow(); /* idempotent */
    } else {
        tmnt_graphics_owner_mark_caught_return();
        pthread_mutex_lock(&mu);
        mark_done = 1; pthread_cond_broadcast(&cv);
        while (!allow_finish) pthread_cond_wait(&cv, &mu);
        pthread_mutex_unlock(&mu);
        /* Simulate the game's common flag store before the finish gate. */
        tmnt_graphics_owner_finish_caught_return();
        tmnt_graphics_owner_finish_caught_return(); /* idempotent */
    }
    pthread_mutex_lock(&mu);
    abandon_done = 1; pthread_cond_broadcast(&cv);
    while (!finish) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    /* Later legitimate cleanup and reuse still work. */
    r[9] = 0; tmnt_graphics_owner_gate(0x96, r);
    count = 0; tmnt_graphics_owner_gate(0x91, r);
    tmnt_graphics_owner_gate(0x90, r); count = 1;
    tmnt_graphics_owner_gate(0x95, r);
    tmnt_graphics_owner_gate(0x96, r); count = 0;
    tmnt_graphics_owner_gate(0x91, r);
    tmnt_graphics_owner_thread_exit();
    return NULL;
}

static void *presenter(void *unused) {
    (void)unused;
    uint64_t r[31] = {0}; r[8] = (uintptr_t)&count;
    tmnt_graphics_owner_gate(0x92, r);
    r[9] = count; /* Preserve the original managed nonzero-count check. */
    assert(r[9] == 1);
    tmnt_graphics_owner_gate(0x93, r);
    pthread_mutex_lock(&mu);
    presenter_done = 1; pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void expect_still_blocked(void) {
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_nsec += 100000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    int rc = 0;
    while (!presenter_done && rc == 0) rc = pthread_cond_timedwait(&cv, &mu, &deadline);
    assert(rc == ETIMEDOUT && !presenter_done);
}

int main(int argc, char **argv) {
    rethrow_mode = argc > 1 && argv[1][0] == 'r';
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, writer, NULL));
    pthread_mutex_lock(&mu);
    while (!set_done) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    assert(!pthread_create(&b, NULL, presenter, NULL));
    pthread_mutex_lock(&mu);
    while (!away_seen) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    tmnt_graphics_owner_finish_caught_return(); /* no mark on this thread */
    tmnt_graphics_owner_abandon_on_rethrow();   /* wrong owner */
    pthread_mutex_lock(&mu);
    expect_still_blocked();
    request_abandon = 1; pthread_cond_broadcast(&cv);
    if (!rethrow_mode) {
        while (!mark_done) pthread_cond_wait(&cv, &mu);
        expect_still_blocked(); /* marking alone may not wake Present */
        allow_finish = 1; pthread_cond_broadcast(&cv);
    }
    while (!abandon_done || !presenter_done) pthread_cond_wait(&cv, &mu);
    finish = 1; pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    assert(!pthread_join(a, NULL)); assert(!pthread_join(b, NULL));
    assert(count == 0);
    puts(rethrow_mode ? "synthetic rethrow: owner-only release and original error passed" :
                        "synthetic caught return: no early wake, owner-only release and reuse passed");
}
