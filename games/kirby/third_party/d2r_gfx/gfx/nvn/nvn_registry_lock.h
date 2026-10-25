#pragma once
/* A lock with explicit per-thread ownership depth. All entry/exit paths
   for this lock must use these helpers and the same thread-local depth counter.
   A failed trylock can never distinguish nesting from another thread's ownership.
   An os_unfair_lock, not a pthread mutex: its waiters lend their priority to the owner (upstream 5f5de5e: the game's
   recording threads, priority 46, were preempted while holding it and the submit thread, 97, waited 1.8 ms a frame). */
#include <os/lock.h>
#include <stdlib.h>

static inline int nvn_registry_enter(os_unfair_lock_t lock, unsigned *thread_depth) {
    int outer = *thread_depth == 0;
    if (outer) os_unfair_lock_lock(lock);
    if (++*thread_depth == 0) abort();
    return outer;
}
static inline void nvn_registry_leave(os_unfair_lock_t lock, unsigned *thread_depth) {
    if (!*thread_depth) abort();
    if (!--*thread_depth) os_unfair_lock_unlock(lock);
}
