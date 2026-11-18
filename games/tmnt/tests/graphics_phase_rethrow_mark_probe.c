/* Test a mark followed by rethrow cannot release a later target on return. */
#include "graphics_phase.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    abort();
}
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static uint32_t count;
static int ready, blocked, clear_now;
int runtime_core_away(void) {
    pthread_mutex_lock(&mu);
    blocked = 1; pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return 1;
}
void runtime_core_back(int *away) { assert(*away == 1); }

static void set(uint64_t r[31]) {
    r[0] = 1;
    tmnt_graphics_owner_gate(0x90, r); count = 1;
    tmnt_graphics_owner_gate(0x95, r);
}
static void clear(uint64_t r[31]) {
    r[9] = 0;
    tmnt_graphics_owner_gate(0x96, r); count = 0;
    tmnt_graphics_owner_gate(0x91, r);
}
static void *writer(void *unused) {
    (void)unused;
    uint64_t r[31] = {0}; r[8] = (uintptr_t)&count;
    set(r);
    tmnt_graphics_owner_mark_caught_return();
    tmnt_graphics_owner_abandon_on_rethrow();
    clear(r);
    set(r);
    tmnt_graphics_owner_finish_caught_return(); /* stale mark must not release */
    pthread_mutex_lock(&mu);
    ready = 1; pthread_cond_broadcast(&cv);
    while (!clear_now) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    clear(r);
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
int main(void) {
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, writer, NULL));
    pthread_mutex_lock(&mu);
    while (!ready) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    assert(!pthread_create(&b, NULL, presenter, NULL));
    pthread_mutex_lock(&mu);
    while (!blocked) pthread_cond_wait(&cv, &mu);
    assert(count == 1);
    clear_now = 1; pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    assert(!pthread_join(a, NULL) && !pthread_join(b, NULL));
    return 0;
}
