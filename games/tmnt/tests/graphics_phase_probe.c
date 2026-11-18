/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void tmnt_graphics_owner_gate(unsigned, uint64_t *);
void tmnt_graphics_owner_thread_exit(void);
_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "%s\n", message); abort(); }

static pthread_mutex_t test_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_cv = PTHREAD_COND_INITIALIZER;
static int ready, clear_worker, away_seen;
static _Atomic int away_count, back_count;
int runtime_core_away(void) {
    atomic_fetch_add(&away_count, 1);
    pthread_mutex_lock(&test_mu); away_seen = 1; pthread_cond_broadcast(&test_cv); pthread_mutex_unlock(&test_mu);
    return 1;
}
void runtime_core_back(int *away) { assert(*away == 1); atomic_fetch_add(&back_count, 1); }

static uint32_t count;
static void *setter(void *unused) {
    (void)unused;
    uint64_t r[31] = {0}; r[8] = (uintptr_t)&count; r[0] = 1;
    tmnt_graphics_owner_gate(0x90, r); count = 1; tmnt_graphics_owner_gate(0x95, r);
    pthread_mutex_lock(&test_mu);
    ready = 1; pthread_cond_broadcast(&test_cv);
    while (!clear_worker) pthread_cond_wait(&test_cv, &test_mu);
    pthread_mutex_unlock(&test_mu);
    r[9] = 0;
    tmnt_graphics_owner_gate(0x96, r); count = 0; tmnt_graphics_owner_gate(0x91, r);
    tmnt_graphics_owner_thread_exit();
    return NULL;
}

static void *presenter(void *unused) {
    (void)unused;
    uint64_t r[31] = {0}; r[8] = (uintptr_t)&count;
    tmnt_graphics_owner_gate(0x92, r);
    r[9] = count;
    assert(r[9] == 0);
    tmnt_graphics_owner_gate(0x93, r);
    return NULL;
}

static void *exiting_setter(void *unused) {
    (void)unused;
    uint64_t r[31] = {0}; r[8] = (uintptr_t)&count; r[0] = 1;
    tmnt_graphics_owner_gate(0x90, r);
    /* Simulate abnormal guest exit before the one-instruction store. */
    tmnt_graphics_owner_thread_exit();
    return NULL;
}

static void *exiting_active_setter(void *unused) {
    (void)unused;
    uint64_t r[31] = {0}; r[8] = (uintptr_t)&count; r[0] = 1;
    tmnt_graphics_owner_gate(0x90, r); count = 1; tmnt_graphics_owner_gate(0x95, r);
    /* The guest dies without clearing the active managed target. */
    tmnt_graphics_owner_thread_exit();
    return NULL;
}

int main(void) {
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, setter, NULL));
    pthread_mutex_lock(&test_mu);
    while (!ready) pthread_cond_wait(&test_cv, &test_mu);
    pthread_mutex_unlock(&test_mu);
    assert(!pthread_create(&b, NULL, presenter, NULL));
    pthread_mutex_lock(&test_mu);
    while (!away_seen) pthread_cond_wait(&test_cv, &test_mu);
    clear_worker = 1; pthread_cond_broadcast(&test_cv);
    pthread_mutex_unlock(&test_mu);
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    assert(atomic_load(&away_count) == 1 && atomic_load(&back_count) == 1);

    uint64_t r[31] = {0}; r[8] = (uintptr_t)&count; r[0] = 1;
    tmnt_graphics_owner_gate(0x90, r); count = 1; tmnt_graphics_owner_gate(0x95, r);
    tmnt_graphics_owner_gate(0x92, r); r[9] = count; assert(r[9] == 1);
    tmnt_graphics_owner_gate(0x93, r); /* original managed check would throw */
    r[9] = 0; tmnt_graphics_owner_gate(0x96, r); count = 0; tmnt_graphics_owner_gate(0x91, r);

    assert(!pthread_create(&a, NULL, exiting_setter, NULL));
    assert(!pthread_join(a, NULL));
    tmnt_graphics_owner_gate(0x92, r); r[9] = count; assert(r[9] == 0);
    tmnt_graphics_owner_gate(0x93, r);

    assert(!pthread_create(&a, NULL, exiting_active_setter, NULL));
    assert(!pthread_join(a, NULL));
    tmnt_graphics_owner_gate(0x92, r); r[9] = count; assert(r[9] == 1);
    tmnt_graphics_owner_gate(0x93, r); /* retain the original managed error */
    count = 0;
    puts("tmnt_graphics_owner_test: overlap, original failure, and abnormal exit passed");
}
