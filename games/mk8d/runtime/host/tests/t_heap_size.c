// Exercise the production SVC dispatcher, including its fatal path, without game files.
#include "../dm.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int heap_mprotect(void *addr, size_t size, int prot);
static _Noreturn void heap_abort(void);
// Substitution is confined to this translation unit; production needs no test API.
#define mprotect heap_mprotect
#define abort heap_abort
#ifndef HLE_SVC_SOURCE
#define HLE_SVC_SOURCE "../hle_svc.c"
#endif
#include HLE_SVC_SOURCE
#undef abort
#undef mprotect

module_t *g_mod;
int g_nmod;
struct dm_flags g_flag;
void hlecount_svc(int num) { (void)num; }
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void guest_clock_read_wait(void) {}
uint64_t guest_ns(void) { return 0; }
uint64_t dm_entropy64(void) { return 0; }
int thr_limit_used(void) { return 0; }
int dm_ue_heap_owns(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; } // No Unreal allocator is installed in this fixture.
void dm_guest_write_prepare(void *addr, size_t size) { (void)addr; (void)size; }
// This fixture calls the SVC dispatcher directly, without rewritten guest gates.
void gate_obs_set(int id, void (*fn)(uint64_t *, int)) { (void)id; (void)fn; }

static size_t page, old_size, requested;
static int calls, injected_error, last_prot;
static volatile sig_atomic_t abort_validated;
static void *last_addr;
static size_t last_size;
static uint64_t regs[31];
static const uint64_t sentinel = 0xabcdef1234567890ull;

static int heap_mprotect(void *addr, size_t size, int prot) {
    calls++; last_addr = addr; last_size = size; last_prot = prot;
    // The production state must not be published before the host operation.
    assert(g_heap_cur == old_size && regs[0] == sentinel && regs[1] == requested);
    if (injected_error) { errno = injected_error; return -1; }
    return mprotect(addr, size, prot);
}
static int correct_call(void) {
    int grow = requested > old_size;
    return calls == 1 && last_addr == g_heap + (grow ? old_size : requested) &&
        last_size == (grow ? requested - old_size : old_size - requested) &&
        last_prot == (grow ? PROT_READ | PROT_WRITE : PROT_NONE);
}
static void aborted(int sig) { _exit(sig == SIGABRT && abort_validated ? 77 : 78); }
static _Noreturn void heap_abort(void) {
    // Observe the exact state at die(), before letting the real abort run.
    if (!injected_error || errno != injected_error || !correct_call() ||
        g_heap_cur != old_size || regs[0] != sentinel || regs[1] != requested ||
        !dm_guest_heap_readable((uint64_t)g_heap, old_size) ||
        dm_guest_heap_readable((uint64_t)g_heap, old_size + 1)) _exit(78);
    abort_validated = 1;   // An unrelated assertion abort must not pass the failure test.
    abort();
}
static void request(size_t size) {
    old_size = g_heap_cur; requested = size; calls = 0;
    memset(regs, 0, sizeof regs); regs[0] = sentinel; regs[1] = size;
    assert(svc_dispatch(0x01, regs, 0) == 0);
}
static void success(size_t size) {
    injected_error = 0;
    request(size);
    assert(g_heap_cur == size && regs[0] == 0 && regs[1] == (uint64_t)g_heap);
    assert(size == old_size ? calls == 0 : correct_call());
    assert(dm_guest_heap_readable((uint64_t)g_heap, size));
    assert(!dm_guest_heap_readable((uint64_t)g_heap, size + 1));
    if (size) { g_heap[0] = 0x5a; g_heap[size - 1] = 0xa5; }
}
static void failure(size_t size, int error) {
    fflush(NULL);
    pid_t p = fork(); assert(p >= 0);
    if (!p) {
        signal(SIGABRT, aborted); alarm(5);
        injected_error = error;
        request(size);
        _exit(79);   // Any return to the guest, even with untouched registers, is wrong.
    }
    int status; assert(waitpid(p, &status, 0) == p);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 77) {
        fprintf(stderr, "SetHeapSize failure did not preserve state and abort (status=%d)\n", status);
        exit(1);
    }
}
static void used_memory(size_t heap_size) {
    uint64_t r[31] = {0, I_UsedMem};
    assert(svc_dispatch(0x29, r, 0) == 0);
    assert(r[0] == 0 && r[1] == heap_size + DM_GAME_MEM_USED);
}
int main(void) {
    mem_init();
    page = (size_t)getpagesize(); g_heap_cap = 4 * page;
    assert(g_heap_cap < APP_POOL);
    // The reservation stays lazy; only four host pages of its heap are used.
    success(2 * page); used_memory(2 * page);   // initial growth
    success(3 * page); used_memory(3 * page);   // further growth
    success(page); used_memory(page);         // shrink
    injected_error = EACCES; errno = EACCES; request(page);   // unchanged: no mprotect despite stale errno
    assert(calls == 0 && g_heap_cur == page && regs[0] == 0 && regs[1] == (uint64_t)g_heap);
    failure(3 * page, ENOMEM); failure(3 * page, EACCES);   // failed growth
    success(3 * page);                        // successful retry from the unchanged parent state
    failure(page, ENOMEM); failure(page, EACCES);         // failed shrink
    failure(0, EACCES);                       // failed release of the whole heap
    success(0); used_memory(0);
    success(0);                              // unchanged empty heap
    success(page); success(0);               // regrowth after release
    free(g_alias_bits); free(g_alias_exposed);
    assert(munmap(g_as, RESERVE) == 0);
    puts("SetHeapSize production dispatcher: growth, shrink, unchanged, zero, failure state and fail-fast passed");
    return 0;
}
