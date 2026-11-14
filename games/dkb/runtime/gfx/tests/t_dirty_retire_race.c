// Race between "set read-only" and pool retirement in dirty-page tracking.
// Scene: thread A, in nvn_dirty_protect, has found the pool's live range but not yet changed any page state; thread B now retires the pool (nvn_dirty_remove_range:
// restore its protected pages to writable, mark the range dead). Old implementation: B finished in that window, then A set the retired storage read-only --
//   no live range owns the page any more, so a host system call reading into it fails with EFAULT and a game thread writing it faults with nobody to unprotect it.
// How it is staged: atomic_compare_exchange_strong in nvn_dirty.c is wrapped here -- before protect's first page-state exchange (right after its lookup), B is started and given 100 ms, so the order is deterministic.
// Expected values do not depend on the implementation under test: after both return, the retired pool is untracked and really writable (reading 16 bytes into it gives 16).
#include <stdatomic.h>
static void t_before_cas(void);
#undef atomic_compare_exchange_strong
#define atomic_compare_exchange_strong(object, expected, desired) \
    (t_before_cas(), __c11_atomic_compare_exchange_strong(object, expected, desired, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
#include "../nvn/nvn_dirty.c"
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }

static void *page;
static _Atomic int go, removing, removed;
static _Thread_local int in_protect;   // only the protecting thread stops at the seam; B's retirement exchanges pass straight through
static void t_before_cas(void) {
    if (!in_protect) return;
    in_protect = 0;
    atomic_store(&go, 1);
    while (!atomic_load(&removing)) usleep(100);
    usleep(100000);   // B's retirement finishes in this window unless protect's lookup holds it off
}
static void *retire(void *unused) {
    (void)unused;
    while (!atomic_load(&go)) usleep(100);
    atomic_store(&removing, 1);
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
    int fd = open("/dev/zero", O_RDONLY);
    if (fd < 0) return 2;
    ssize_t bytes = read(fd, page, 16);
    int read_error = bytes < 0 ? errno : 0;
    int untracked = !nvn_dirty_tracked(page, size);
    printf("retirement completed: %d; untracked: %d; write into retired pool: %zd bytes (expected 16), errno=%d\n",
           atomic_load(&removed), untracked, bytes, read_error);
    close(fd);
    mprotect(page, size, PROT_READ | PROT_WRITE);   // let a failing run exit cleanly instead of faulting on cleanup
    munmap(page, size);
    return bytes == 16 && atomic_load(&removed) && untracked && !in_protect ? 0 : 1;
}
