#include "graphics_phase.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    abort();
}
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int wait_count;
static _Atomic int observed_a, observed_b;
int runtime_core_away(void) {
    pthread_mutex_lock(&mu);
    ++wait_count;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    return 1;
}
void runtime_core_back(int *away) { assert(*away == 1); }

static uint32_t a, b, c;
static void set(uint32_t *count) {
    uint64_t r[31] = {0}; r[8] = (uintptr_t)count; r[0] = 1;
    tmnt_graphics_owner_gate(0x90, r);
    *count = 1;
    tmnt_graphics_owner_gate(0x95, r);
}
static void clear(uint32_t *count) {
    uint64_t r[31] = {0}; r[8] = (uintptr_t)count; r[9] = 0;
    tmnt_graphics_owner_gate(0x96, r);
    *count = 0;
    tmnt_graphics_owner_gate(0x91, r);
}
static void *present(void *arg) {
    uint32_t *count = arg;
    uint64_t r[31] = {0}; r[8] = (uintptr_t)count;
    tmnt_graphics_owner_gate(0x92, r);
    r[9] = *count;
    tmnt_graphics_owner_gate(0x93, r);
    atomic_store(count == &a ? &observed_a : &observed_b, (int)r[9] + 1);
    return NULL;
}
static void wait_for(int expected) {
    pthread_mutex_lock(&mu);
    while (wait_count < expected) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

int main(void) {
    pthread_t pa, pb;
    set(&a);
    set(&b); /* A must remain owned when B becomes active. */
    assert(!pthread_create(&pa, NULL, present, &a));
    wait_for(1);
    assert(atomic_load(&observed_a) == 0 && a == 1 && b == 1);
    clear(&a);
    assert(!pthread_join(pa, NULL));
    assert(atomic_load(&observed_a) == 1);
    assert(!pthread_create(&pb, NULL, present, &b));
    wait_for(2);
    assert(atomic_load(&observed_b) == 0 && b == 1);
    clear(&b);
    assert(!pthread_join(pb, NULL));
    assert(atomic_load(&observed_b) == 1);
    set(&c); clear(&c); /* An inactive slot can be reused. */
    tmnt_graphics_owner_thread_exit();
    return 0;
}
