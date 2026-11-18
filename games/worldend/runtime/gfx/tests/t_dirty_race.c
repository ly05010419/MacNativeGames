// Ordering race between "set read-only" and "unprotect before write" in dirty-page tracking (originally a test of the host this one started as, stubs adapted to this repository's tracking table).
// Scene: thread A, in nvn_dirty_protect, has already recorded the page state as "protected" while its mprotect has not landed yet; thread B now arrives with prep_write (the unprotect done before the host writes into guest memory,
// for example the movie decoder thread writing a frame into texture storage, or a disk read into a guest buffer). Old implementation: B sets the state back to "none", then A's mprotect lands => the page is read-only while its state is "none" --
//   when the host reads a file into it again: prep_write sees state "none" and skips, the system call does not go through the page-fault handler and fails with EFAULT; when a game thread writes directly: the page-fault handler only returns "re-execute" and spins forever.
// How it is staged: the mprotect in nvn_dirty.c is replaced by t_mprotect here -- before the call with which A sets read-only lands, B is let in and given 100 ms, so the order is deterministic.
// Expected values do not depend on the implementation under test: after B's prep_write returns, this page must really be writable (reading 16 bytes into it gives 16; writing one byte directly does not hang and reads back as written),
// and this write must be recorded as dirty (nvn_dirty_since returns 1 for a stamp taken before the page was set read-only).
#define mprotect t_mprotect
#include "../nvn/nvn_dirty.c"
#undef mprotect
int mprotect(void *, size_t, int);
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-58s expected %-4lld → got %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

static uint8_t *g_page; static _Atomic int g_armed, g_go, g_b_done;
static void *thread_b(void *arg) { (void)arg; while (!atomic_load(&g_go)) usleep(200); prep_write(g_page, 16); atomic_store(&g_b_done, 1); return NULL; }
int t_mprotect(void *a, size_t n, int prot) {
    if (prot == PROT_READ && atomic_exchange(&g_armed, 0)) { atomic_store(&g_go, 1); usleep(100000); }   // Before A's mprotect lands: let B in and wait until it runs to the end or gets stuck waiting
    return mprotect(a, n, prot);
}

int main(void) {
    crash_install();
    size_t pg = (size_t)getpagesize();
    g_page = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_page == MAP_FAILED) { perror("mmap"); return 2; }
    nvn_dirty_add_range(g_page, 2 * pg);
    uint64_t stamp = nvn_dirty_now();
    pthread_t tb; pthread_create(&tb, NULL, thread_b, NULL);
    atomic_store(&g_armed, 1);
    nvn_dirty_protect(g_page, 2 * pg);                       // A: set the state -> (B cuts in) -> mprotect lands
    for (int i = 0; i < 2000 && !atomic_load(&g_b_done); i++) usleep(1000);
    EXPECT("B's unprotect-before-write returns within 2 seconds", 1, atomic_load(&g_b_done));
    if (!atomic_load(&g_b_done)) { printf("t_dirty_race: B did not return, skipping the remaining checks\n"); return 1; }
    pthread_join(tb, NULL);
    int fd = open("/dev/zero", O_RDONLY); g_page[pg] = 0;     // Nobody has unprotected the second page, so this write goes through the page-fault handler (control: the normal path works)
    ssize_t r = read(fd, g_page, 16);                        // The host write that B is about to make: a system call writing straight into the page
    EXPECT("file read into the page after B unprotects it: bytes read = the 16 requested", 16, r);
    if (r == 16) { ((volatile uint8_t *)g_page)[1] = 0x5a; EXPECT("direct write to the page does not hang and reads back as written", 0x5a, g_page[1]); }
    else { printf("  (read failed errno=%d; the page is read-only while its state is 'none', a direct write would spin in the page-fault handler, so this check is skipped)\n", errno); g_fail++; }
    EXPECT("this write is recorded as dirty (against a stamp from before set-read-only)", 1, nvn_dirty_since(g_page, 16, stamp));
    close(fd);
    printf("t_dirty_race: %s\n", g_fail ? "FAILED" : "all passed"); return g_fail != 0;
}
