// The race between "set read-only" and pool retirement in the dirty-page tracking (games/totk/third_party/d2r_gfx/gfx/nvn/
// nvn_dirty.c), after nxnative's t_dirty_retire_race (af616f54). Thread A is in nvn_dirty_protect: it has looked the pool's
// range up but changed no page state yet; thread B then retires the pool (nvn_dirty_remove_range: protected pages back to
// writable, the range marked dead). Without the slot mutex B finishes in that window and A then sets the retired storage
// read-only with no live range to release it: a host read() into it fails with EFAULT.
// Staged without timed sleeps: atomic_compare_exchange_strong is wrapped so A stops before its first page-state exchange
// and starts B. A then tries the slot mutex: if it gets it, A does not hold it (the unfixed code), so it lets it go and
// waits until B's retirement has completed; if it does not, A holds it (the fixed code), and it waits until B has reached
// the mutex (pthread_mutex_lock is wrapped too) and so is held off. Every wait has a 10 s limit (exit 3, "timeout").
// Built by tests/test_nvn_dirty_retire_race.py with DIRTY_C naming the nvn_dirty.c to test. Prints which case it saw,
// then "ok" (exit 0) when the retired page is untracked and writable (a 16-byte read into it gives 16), else exit 1.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static void t_before_cas(void);
static void t_before_lock(pthread_mutex_t *m);
#undef atomic_compare_exchange_strong
#define atomic_compare_exchange_strong(object, expected, desired) \
    (t_before_cas(), __c11_atomic_compare_exchange_strong(object, expected, desired, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
#define pthread_mutex_lock(m) (t_before_lock(m), pthread_mutex_lock(m))
#include DIRTY_C
#undef pthread_mutex_lock
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

void dm_fault_hook_register(int (*fn)(void *, int, void *)) { (void)fn; }   // the test never faults: it reads with read()
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { (void)fn; }

static void *page;
static _Atomic int go, b_at_lock, removed;
static const char *seen = "the seam was not reached";
static _Thread_local int in_protect, is_b;   // only A stops at the seam; only B's lock call is noted

static void wait_for(_Atomic int *flag, const char *what) {
    struct timespec t0, t; clock_gettime(CLOCK_MONOTONIC, &t0);
    while (!atomic_load(flag)) {
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (t.tv_sec - t0.tv_sec >= 10) { printf("timeout waiting for %s\n", what); exit(3); }
        sched_yield();
    }
}
static void t_before_lock(pthread_mutex_t *m) { if (is_b && m == &g_slot_mu) atomic_store(&b_at_lock, 1); }
static void t_before_cas(void) {
    if (!in_protect) return;
    in_protect = 0;
    atomic_store(&go, 1);
    if (pthread_mutex_trylock(&g_slot_mu) == 0) {   // protect does not hold the mutex: B's retirement runs to its end now
        pthread_mutex_unlock(&g_slot_mu);
        wait_for(&removed, "the retirement to complete");
        seen = "the retirement completed inside protect's window";
    } else {                                         // protect holds it: B reaches the mutex and is held off
        wait_for(&b_at_lock, "the retirement to reach the slot mutex");
        seen = "the retirement was held off by the slot mutex";
    }
}
static void *retire(void *unused) {
    (void)unused;
    is_b = 1;
    wait_for(&go, "protect's seam");
    nvn_dirty_remove_range(page);
    atomic_store(&removed, 1);
    return NULL;
}
int main(void) {
    size_t size = (size_t)getpagesize();
    page = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED) return 2;
    nvn_dirty_add_range(page, size);
    pthread_t worker;
    if (pthread_create(&worker, NULL, retire, NULL)) return 2;
    in_protect = 1;
    nvn_dirty_protect(page, size);
    pthread_join(worker, NULL);
    puts(seen);
    int fd = open("/dev/zero", O_RDONLY);
    if (fd < 0) return 2;
    ssize_t n = read(fd, page, 16);
    int err = errno;
    close(fd);
    if (n == 16 && !nvn_dirty_tracked(page, size)) { puts("ok"); return 0; }
    printf("retired page: read gave %zd (errno %d), tracked %d\n", n, n < 0 ? err : 0, nvn_dirty_tracked(page, size));
    return 1;
}
