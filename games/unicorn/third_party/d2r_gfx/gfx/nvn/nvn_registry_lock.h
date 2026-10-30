#pragma once
/* A normal mutex with explicit per-thread ownership depth. All entry/exit paths
   for this mutex must use these helpers and the same thread-local depth counter.
   A failed trylock can never distinguish nesting from another thread's ownership. */
#include <pthread.h>
#include <stdlib.h>

static inline int nvn_registry_enter(pthread_mutex_t *mutex, unsigned *thread_depth) {
    int outer = *thread_depth == 0;
    if (outer && pthread_mutex_lock(mutex)) abort();
    if (++*thread_depth == 0) abort();
    return outer;
}
static inline void nvn_registry_leave(pthread_mutex_t *mutex, unsigned *thread_depth) {
    if (!*thread_depth) abort();
    if (!--*thread_depth && pthread_mutex_unlock(mutex)) abort();
}
