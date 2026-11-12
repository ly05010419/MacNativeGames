#ifndef TMNT_NVN_WINDOW_SERIAL_SET_H
#define TMNT_NVN_WINDOW_SERIAL_SET_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

enum { NVN_WINDOW_SERIAL_CAPACITY = 128 };
typedef struct {
    _Atomic uint64_t serial[NVN_WINDOW_SERIAL_CAPACITY];
    _Atomic int count;
    pthread_mutex_t mu;
} nvn_window_serial_set;

#define NVN_WINDOW_SERIAL_SET_INIT { .mu = PTHREAD_MUTEX_INITIALIZER }

/* Serial numbers are never reused. Published entries remain valid until the
   host exits; release/acquire on count publishes each slot to render threads. */
static inline int nvn_window_serial_contains(const nvn_window_serial_set *set, uint64_t serial) {
    if (!serial) return 0;
    int n = atomic_load_explicit(&set->count, memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (atomic_load_explicit(&set->serial[i], memory_order_relaxed) == serial) return 1;
    return 0;
}

/* Returns 1 for a new serial, 0 for a duplicate/zero, -1 if full. */
static inline int nvn_window_serial_add(nvn_window_serial_set *set, uint64_t serial) {
    if (!serial) return 0;
    pthread_mutex_lock(&set->mu);
    int n = atomic_load_explicit(&set->count, memory_order_relaxed);
    for (int i = 0; i < n; i++)
        if (atomic_load_explicit(&set->serial[i], memory_order_relaxed) == serial) {
            pthread_mutex_unlock(&set->mu);
            return 0;
        }
    if (n == NVN_WINDOW_SERIAL_CAPACITY) {
        pthread_mutex_unlock(&set->mu);
        return -1;
    }
    atomic_store_explicit(&set->serial[n], serial, memory_order_relaxed);
    atomic_store_explicit(&set->count, n + 1, memory_order_release);
    pthread_mutex_unlock(&set->mu);
    return 1;
}

#endif
