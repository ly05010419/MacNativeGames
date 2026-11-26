#pragma once
/* A lock with explicit per-thread ownership depth. All entry/exit paths
   for this lock must use these helpers and the same thread-local depth counter.
   A failed trylock can never distinguish nesting from another thread's ownership.
   TOTK: an os_unfair_lock, not a pthread mutex: its waiters lend their priority to the owner. The game's recording
   threads (priority 46) take it to register textures and were preempted while holding it; the submit thread (97)
   then waited 1.8 ms a frame in a shrine (System Trace, TOTK 1.4.3 on the Mac, 2026-10-21). */
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
