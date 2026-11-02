// Exercise the production dispatcher with real backing, including protection
// failures and concurrent operations. No alternate allocator implementation.
#include "../dm.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int memory_mprotect(void *addr, size_t size, int prot);
static _Noreturn void memory_abort(void);
static int memory_getpagesize(void) {
#ifdef TEST_PAGE_16K
    assert(getpagesize() <= 0x4000);
    return 0x4000;
#else
    return getpagesize();
#endif
}
#define getpagesize memory_getpagesize
#define mprotect memory_mprotect
#define abort memory_abort
#include "../hle_svc.c"
#undef abort
#undef mprotect
#undef getpagesize

static module_t test_modules[2];
module_t *g_mod = test_modules;
int g_nmod;
struct dm_flags g_flag;
void hlecount_svc(int num) { (void)num; }
int svc_thread(int num, uint64_t *regs) { (void)num; (void)regs; return 0; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void guest_clock_read_wait(void) {}
uint64_t guest_ns(void) { return 0; }
uint64_t dm_entropy64(void) { return 0; }
int thr_limit_used(void) { return 0; }
int dm_ue_heap_owns(uint64_t a, uint64_t n) { (void)a; (void)n; return 0; } // No Unreal allocator is installed in this fixture.
// This fixture calls the SVC dispatcher directly, without rewritten guest gates.
void gate_obs_set(int id, void (*fn)(uint64_t *, int)) { (void)id; (void)fn; }
module_t *module_of(const void *p) {
    uintptr_t a = (uintptr_t)p;
    for (int i = 0; i < g_nmod; ++i) {
        uintptr_t b = (uintptr_t)g_mod[i].base;
        if (a >= b && a - b < g_mod[i].size) return &g_mod[i];
        if (g_mod[i].patch_off < 0 && a >= b + g_mod[i].patch_off && a < b) return &g_mod[i];
    }
    return NULL;
}

#ifdef TEST_REAL_DIRTY
#include "../../gfx/nvn/nvn_dirty.h"
static void (*write_prepare)(void *, size_t);
static int (*fault_hook)(void *, int, void *);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { fault_hook = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { write_prepare = fn; }
static void memory_fault(int sig, siginfo_t *info, void *ctx) {
    if (fault_hook && fault_hook(info->si_addr, 1, ctx)) return;
    _exit(128 + sig);
}
#endif
static _Atomic unsigned prepares, protects;
static uintptr_t readonly_base;
static size_t readonly_size;
void dm_guest_write_prepare(void *addr, size_t size) {
    atomic_fetch_add(&prepares, 1);
#ifdef TEST_REAL_DIRTY
    if (write_prepare) write_prepare(addr, size);
#endif
    if (readonly_size && (uintptr_t)addr < readonly_base + readonly_size && readonly_base < (uintptr_t)addr + size) {
        assert(!mprotect((void *)readonly_base, readonly_size, PROT_READ | PROT_WRITE));
        readonly_size = 0;
    }
}

static int fail_protect;
static uint64_t *failure_regs, failure_input[3];
static int64_t failure_mapped;
static size_t failure_heap;
static int failure_nmap;
static unsigned failure_prepares;
static int memory_mprotect(void *addr, size_t size, int prot) {
    size_t pg = memory_getpagesize();
    assert(!((uintptr_t)addr & (pg - 1)) && !(size & (pg - 1)) && size);
    atomic_fetch_add(&protects, 1);
    if (fail_protect) { errno = EACCES; return -1; }
    return mprotect(addr, size, prot);
}
static _Noreturn void memory_abort(void) {
    // Only the production fatal path reaches this wrapper; assertion failures
    // cannot pass these checks. No guest return or metadata publication allowed.
    if (!fail_protect || errno != EACCES || !failure_regs ||
        memcmp(failure_regs, failure_input, sizeof failure_input) ||
        g_alias_mapped != failure_mapped || g_heap_cur != failure_heap ||
        g_nmap != failure_nmap || prepares != failure_prepares) _exit(78);
    _exit(77);
}
enum { P = 0x1000, BAD = 0xd401, OOM = 0xd001, LIMIT = 0x10801 };
static uintptr_t alias, stack, heap;
static uint64_t call(unsigned svc, uint64_t x0, uint64_t x1, uint64_t x2) {
    uint64_t r[31] = {x0, x1, x2}; svc_dispatch(svc, r, 0); return r[0];
}
static uint64_t pages(int commit, uintptr_t a, size_t n) { return call(commit ? 0x2c : 0x2d, a, n, 0); }
static uint64_t resize(size_t n) { return call(1, 0, n, 0); }
static uint64_t map(uintptr_t d, uintptr_t s, size_t n) { return call(4, d, s, n); }
static uint64_t unmap(uintptr_t d, uintptr_t s, size_t n) { return call(5, d, s, n); }
static uint64_t info(unsigned id) {
    uint64_t r[31] = {0, id}; svc_dispatch(0x29, r, 0); assert(!r[0]); return r[1];
}
typedef struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, reserved; } memory_info;
static memory_info query(uintptr_t a) {
    uint8_t out[57]; memset(out, 0xa7, sizeof out);
    uint64_t r[31] = {(uintptr_t)(out + 1), 0, a}; svc_dispatch(6, r, 0);
    assert(!r[0] && !r[1] && out[0] == 0xa7);
    for (unsigned i = 41; i < sizeof out; ++i) assert(out[i] == 0xa7);
    memory_info m; memcpy(&m, out + 1, sizeof m);
    assert(!m.ipc && !m.device && !m.reserved && m.size && a >= m.base && a - m.base < m.size);
    return m;
}
static void expect(uintptr_t a, uintptr_t base, size_t size, unsigned state, unsigned attr, unsigned perm) {
    memory_info m = query(a);
    assert(m.base == base && m.size == size && m.state == state && m.attr == attr && m.perm == perm);
}
static void fatal(unsigned svc, uint64_t x0, uint64_t x1, uint64_t x2) {
    fflush(NULL); pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        alarm(5);
        uint64_t r[31] = {x0, x1, x2}; failure_regs = r;
        memcpy(failure_input, r, sizeof failure_input);
        failure_mapped = g_alias_mapped; failure_heap = g_heap_cur;
        failure_nmap = g_nmap; failure_prepares = prepares; fail_protect = 1;
        svc_dispatch(svc, r, 0); _exit(79);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 77);
}

static void alias_lifecycle(void) {
    assert(!pages(1, alias + 4 * P, 4 * P));
    memset((void *)(alias + 4 * P), 0x53, 4 * P);
    assert(!pages(1, alias + 6 * P, 4 * P));
    assert(g_alias_mapped == 6 * P);
    expect(alias + 7 * P, alias + 4 * P, 6 * P, 5, 0, 3);
    expect((uintptr_t)g_as, (uintptr_t)g_as, alias + 4 * P - (uintptr_t)g_as, 0, 0, 0);
    expect(stack, alias + 10 * P, heap - alias - 10 * P, 0, 0, 0);
    for (size_t i = 0; i < 4 * P; ++i) assert(((uint8_t *)alias)[4 * P + i] == 0x53);
    for (size_t i = 8 * P; i < 10 * P; ++i) assert(((uint8_t *)alias)[i] == 0);
    unsigned before = protects;
    assert(!pages(1, alias + 4 * P, 6 * P) && protects == before);
    assert(!pages(0, alias + 5 * P, P));
    expect(alias + 5 * P, alias + 5 * P, P, 0, 0, 0);
    expect(alias + 6 * P, alias + 6 * P, 4 * P, 5, 0, 3);
    assert(*(uint8_t *)(alias + 4 * P) == 0x53 && *(uint8_t *)(alias + 6 * P) == 0x53);
    assert(!pages(1, alias + 5 * P, P));
    for (size_t i = 0; i < P; ++i) assert(((uint8_t *)alias)[5 * P + i] == 0);
    assert(*(uint8_t *)(alias + 4 * P) == 0x53);
    assert(!pages(0, alias, 16 * P) && !pages(0, alias, 16 * P) && !g_alias_mapped);
    // First commit of a logical sibling after its host page was writable must
    // clear retained bytes, even though that logical page was never committed.
    size_t pg = memory_getpagesize();
    assert(!pages(1, alias, P));
    if (pg > P) {
        memset((void *)(alias + P), 0xa9, P);
        assert(!pages(1, alias + P, P));
        for (size_t i = 0; i < P; ++i) assert(((uint8_t *)alias)[P + i] == 0);
    }
    assert(!pages(0, alias, pg));
    assert(pages(1, alias, 0) == BAD && pages(1, alias + 1, P) == BAD);
    assert(pages(1, alias, P + 1) == BAD && pages(1, alias - P, P) == BAD);
    assert(pages(1, UINTPTR_MAX - P + 1, 2 * P) == BAD);
    assert(pages(1, alias + g_alias_cap - P, 2 * P) == BAD);
}

static void ownership(void) {
    assert(!pages(1, alias, 16 * P));
    memset((void *)alias, 0x34, 16 * P);
    assert(!map(stack + P, alias + P, 2 * P));
    expect(alias + P, alias + P, 2 * P, 5, 1, 0);
    expect(alias, alias, P, 5, 0, 3);
    expect(alias + 3 * P, alias + 3 * P, 13 * P, 5, 0, 3);
    expect(stack + P, stack + P, 2 * P, 0xb, 0, 3);
    assert(map(stack + 8 * P, alias + 2 * P, P) == BAD);
    assert(map(stack + 2 * P, alias + 8 * P, P) == BAD);
    assert(pages(0, alias, 4 * P) == BAD);
    // SDK StandardAllocator extends committed windows across existing stacks.
    // Recommit must preserve the borrowed source and its authoritative copy.
    assert(!pages(1, alias, 4 * P));
    assert(!pages(0, alias, P));
    assert(!pages(1, alias, 4 * P)); // initializes a hole beside the borrowed source
    assert(!pages(1, alias + 15 * P, 2 * P));
    expect(alias + P, alias + P, 2 * P, 5, 1, 0);
    assert(*(uint8_t *)(alias + P) == 0x34);
    expect(UINTPTR_MAX, UINTPTR_MAX - P + 1, P, 0, 0, 0);
    assert(call(2, alias + P, P, 3) == BAD && call(3, alias + P, P, 0) == BAD);
    assert(unmap(stack + P, alias + P, P) == BAD);
    assert(unmap(stack + P, alias + 4 * P, 2 * P) == BAD);
    assert(!pages(0, alias, P) && !pages(1, alias, P));
    assert(*(uint8_t *)(alias + P) == 0x34);
    memset((void *)(stack + P), 0x76, 2 * P);
    readonly_base = alias; readonly_size = memory_getpagesize();
    assert(!mprotect((void *)readonly_base, readonly_size, PROT_READ));
    assert(!unmap(stack + P, alias + P, 2 * P) && !readonly_size);
    for (size_t i = P; i < 3 * P; ++i) assert(((uint8_t *)alias)[i] == 0x76);
    expect(alias + P, alias, 17 * P, 5, 0, 3);
    assert(!pages(0, alias, 17 * P));
    assert(map(stack, alias, P) == BAD);
    assert(map(stack + 1, alias, P) == 0xcc01 && map(stack, alias, 0) == 0xca01);
    assert(map(alias, alias, P) == 0xdc01 && map(stack, UINTPTR_MAX - P + 1, 2 * P) == BAD);
    assert(!resize(4 * 1024 * 1024));
    assert(!map(stack, heap + 3 * 1024 * 1024, P));
    assert(resize(2 * 1024 * 1024) == BAD && g_heap_cur == 4 * 1024 * 1024);
    assert(!resize(6 * 1024 * 1024));
    *(uint8_t *)stack = 0x91;
    assert(!unmap(stack, heap + 3 * 1024 * 1024, P));
    assert(*(uint8_t *)(heap + 3 * 1024 * 1024) == 0x91);
    assert(!resize(0));
}

static void module_sources(void) {
    uint8_t *b = mmap(NULL, 0x20000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(b != MAP_FAILED);
    g_mod[0] = (module_t){.base=b, .size=0x20000, .text_end=0x2800, .ro_off=0x2800,
        .data_off=0x3000, .patch_off=0x1e000, .key_page_off=0x8000, .key_page_size=0x4000, .loaded=1};
    g_nmod = 1;
    assert(!mprotect(b, 0x4000, PROT_READ));
    assert(!mprotect(b + 0x8000, 0x4000, PROT_READ));
    assert(!mprotect(b + 0x1c000, 0x4000, PROT_READ));
    assert(map(stack, (uintptr_t)b + 0x3000, P) == BAD);
    assert(map(stack, (uintptr_t)b + 0x7000, 2 * P) == BAD);
    assert(map(stack, (uintptr_t)b + 0x1b000, 2 * P) == BAD);
    assert(!map(stack, (uintptr_t)b + 0x4000, P));
    expect((uintptr_t)b + 0x4000, (uintptr_t)b + 0x4000, P, 4, 1, 0);
    expect((uintptr_t)b + 0x8000, (uintptr_t)b + 0x8000, 0x4000, 4, 0, 1);
    *(uint8_t *)stack = 0x62;
    assert(!unmap(stack, (uintptr_t)b + 0x4000, P) && b[0x4000] == 0x62);
    g_mod[0].nro = 1;
    assert(!map(stack, (uintptr_t)b + 0x4000, P));
    assert(!svc_nro_deactivate(&g_mod[0]) && g_mod[0].loaded);
    assert(!unmap(stack, (uintptr_t)b + 0x4000, P));
    assert(svc_nro_deactivate(&g_mod[0]) && !g_mod[0].loaded);
    assert(map(stack, (uintptr_t)b + 0x4000, P) == BAD);
    g_nmod = 0; memset(g_mod, 0, sizeof test_modules); assert(!munmap(b, 0x20000));
}

// Apple has no pthread_barrier. The gate makes races deterministic at entry,
// without assumptions about which operation wins the memory-state lock.
typedef struct { pthread_mutex_t mu; pthread_cond_t cv; unsigned waiting; } gate;
static void gate_wait(gate *g) {
    assert(!pthread_mutex_lock(&g->mu));
    if (++g->waiting == 2) assert(!pthread_cond_broadcast(&g->cv));
    else while (g->waiting < 2) assert(!pthread_cond_wait(&g->cv, &g->mu));
    assert(!pthread_mutex_unlock(&g->mu));
}
typedef struct { gate *start; unsigned svc; uint64_t a, b, c, result; } task;
static void *compete(void *arg) {
    task *t = arg; gate_wait(t->start); t->result = call(t->svc, t->a, t->b, t->c); return NULL;
}
static void race(task *a, task *b) {
    gate g = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0};
    a->start = b->start = &g; pthread_t ta, tb;
    assert(!pthread_create(&ta, NULL, compete, a) && !pthread_create(&tb, NULL, compete, b));
    assert(!pthread_join(ta, NULL) && !pthread_join(tb, NULL));
    assert(!pthread_cond_destroy(&g.cv) && !pthread_mutex_destroy(&g.mu));
}
static void concurrency(void) {
    for (unsigned i = 0; i < 40; ++i) {
        task h = {.svc=1, .b=8 * 1024 * 1024}, p = {.svc=0x2c, .a=alias, .b=8 * 1024 * 1024};
        race(&h, &p);
        if (DM_GAME_MEM_MAP_LIMIT) assert((!h.result && p.result == LIMIT) || (h.result == LIMIT && !p.result));
        else assert(!h.result && !p.result);
        assert(info(I_UsedMem) == DM_GAME_MEM_USED + g_heap_cur + (DM_GAME_MEM_MAP_LIMIT ? g_alias_mapped : 0));
        assert(!pages(0, alias, p.b) && !resize(0));
        assert(!pages(1, alias, P));
        task m = {.svc=4, .a=stack, .b=alias, .c=P}, d = {.svc=0x2d, .a=alias, .b=P};
        race(&m, &d);
        assert((!m.result && d.result == BAD) || (m.result == BAD && !d.result));
        if (!m.result) assert(!unmap(stack, alias, P));
        assert(!pages(0, alias, P));
        assert(!resize(4 * 1024 * 1024));
        m.b = heap + 3 * 1024 * 1024; h.b = 2 * 1024 * 1024;
        race(&m, &h);
        assert((!m.result && h.result == BAD) || (m.result == BAD && !h.result));
        if (!m.result) assert(!unmap(stack, m.b, P));
        assert(!resize(0));
        assert(!pages(1, alias, 2 * P));
        m.b = alias;
        task other = {.svc=4, .a=stack + 0x4000, .b=alias, .c=P};
        race(&m, &other);
        assert((!m.result && other.result == BAD) || (m.result == BAD && !other.result));
        assert(!unmap(m.result ? other.a : m.a, alias, P));
        other.a = stack; other.b = alias + P;
        race(&m, &other);
        assert((!m.result && other.result == BAD) || (m.result == BAD && !other.result));
        assert(!unmap(stack, m.result ? other.b : m.b, P));
        assert(!pages(0, alias, 2 * P));
    }
}
static void fragmented_queries(void) {
    // Independent byte-sized model checks word boundaries and free runs across
    // the alias partition. Preserve every live page's marker on overlapping maps.
    enum { N = 160 };
    unsigned char live[N] = {0}; uint32_t rng = 0x13579b;
    for (unsigned step = 0; step < 1000; ++step) {
        rng = rng * 1664525 + 1013904223; unsigned first = rng % N;
        rng = rng * 1664525 + 1013904223; unsigned n = 1 + rng % (N - first);
        int commit = (rng >> 20) & 1;
        assert(!pages(commit, alias + first * P, n * P));
        for (unsigned i = first; i < first + n; ++i) {
            if (commit) {
                assert(*(uint8_t *)(alias + i * P) == (live[i] ? 0x57 : 0));
                *(uint8_t *)(alias + i * P) = 0x57;
            }
            live[i] = commit;
        }
        unsigned total = 0;
        for (unsigned i = 0; i < N; ++i) {
            total += live[i];
            unsigned lo = i, hi = i + 1;
            while (lo && live[lo - 1] == live[i]) --lo;
            while (hi < N && live[hi] == live[i]) ++hi;
            uintptr_t b = !live[i] && !lo ? (uintptr_t)g_as : alias + lo * P;
            uintptr_t e = !live[i] && hi == N ? heap : alias + hi * P;
            expect(alias + i * P, b, e - b, live[i] ? 5 : 0, 0, live[i] ? 3 : 0);
        }
        assert(g_alias_mapped == total * P);
    }
    assert(!pages(0, alias, N * P));
}
static void lazy_commit(void) {
    size_t n = DM_GAME_APP_POOL - DM_GAME_MEM_USED, pg = memory_getpagesize();
    char *resident = malloc(n / pg); assert(resident);
    assert(!pages(1, alias, n));
    assert(!mincore((void *)alias, n, resident));
    size_t count = 0;
    for (size_t i = 0; i < n / pg; ++i) count += !!(resident[i] & MINCORE_INCORE);
    assert(count < 32); // metadata scans must not fault the anonymous backing in
    ((uint8_t *)alias)[0] = 0x98;
    assert(!pages(0, alias, pg) && !pages(1, alias, pg));
    for (size_t i = 0; i < pg; ++i) assert(((uint8_t *)alias)[i] == 0);
    assert(!pages(0, alias, n)); free(resident);
}
static void metadata_limit(void) {
    assert(!pages(1, alias, (MAX_MAP + 1) * P));
    for (unsigned i = 0; i < MAX_MAP; ++i) assert(!map(stack + i * P, alias + i * P, P));
    unsigned before = protects;
    assert(map(stack + MAX_MAP * P, alias + MAX_MAP * P, P) == 0xce01);
    assert(protects == before && g_nmap == MAX_MAP);
    expect(alias + MAX_MAP * P, alias + MAX_MAP * P, P, 5, 0, 3);
    // Releasing one destination makes its metadata available again.
    assert(!unmap(stack + 123 * P, alias + 123 * P, P));
    assert(!map(stack + 123 * P, alias + MAX_MAP * P, P));
    assert(protects == before + 1);
    for (unsigned i = 0; i < MAX_MAP; ++i)
        assert(!unmap(stack + i * P, alias + (i == 123 ? MAX_MAP : i) * P, P));
    assert(!pages(0, alias, (MAX_MAP + 1) * P));
}
#ifdef TEST_REAL_DIRTY
static _Atomic int dirty_done;
static void *protect_worker(void *unused) {
    (void)unused;
    while (!atomic_load(&dirty_done)) nvn_dirty_protect((void *)alias, P);
    return NULL;
}
static void dirty_integration(void) {
    struct sigaction sa = {.sa_sigaction=memory_fault, .sa_flags=SA_SIGINFO};
    sigemptyset(&sa.sa_mask);
    assert(!sigaction(SIGBUS, &sa, NULL) && !sigaction(SIGSEGV, &sa, NULL));
    size_t pg = memory_getpagesize(); assert(pg > P);
    assert(!pages(1, alias, 2 * pg));
    nvn_dirty_add_range((void *)alias, P);
    nvn_dirty_add_range((void *)(alias + P), P);
    *(uint8_t *)alias = 0x29;
    for (unsigned i = 0; i < 20; ++i) {
        nvn_dirty_protect((void *)alias, P);
        nvn_dirty_protect((void *)(alias + P), P);
        uint64_t stamp = nvn_dirty_now();
        assert(!pages(0, alias + P, P) && !pages(1, alias + P, P));
        assert(nvn_dirty_since((void *)alias, P, stamp));
        assert(nvn_dirty_since((void *)(alias + P), P, stamp));
        assert(*(uint8_t *)alias == 0x29 && *(uint8_t *)(alias + P) == 0);
        assert(!map(stack, alias, P));
        *(uint8_t *)stack = 0x29;
        nvn_dirty_protect((void *)alias, P); stamp = nvn_dirty_now();
        assert(!unmap(stack, alias, P));
        assert(nvn_dirty_since((void *)alias, P, stamp));
    }
    pthread_t thread; assert(!pthread_create(&thread, NULL, protect_worker, NULL));
    for (unsigned i = 0; i < 1000; ++i) {
        assert(!pages(0, alias + P, P) && !pages(1, alias + P, P));
        assert(!map(stack, alias, P)); *(uint8_t *)stack = 0x29;
        assert(!unmap(stack, alias, P));
        assert(*(uint8_t *)alias == 0x29 && *(uint8_t *)(alias + P) == 0);
    }
    atomic_store(&dirty_done, 1); assert(!pthread_join(thread, NULL));
    nvn_dirty_remove_range((void *)alias); nvn_dirty_remove_range((void *)(alias + P));
    assert(!pages(0, alias, 2 * pg));
    puts("real GPU dirty tracking: shared-page recommit, copyback and concurrent protection passed");
}
#endif
static void failures(void) {
    fatal(0x2c, alias, 0x4000, 0);
    assert(!pages(1, alias, 0x4000));
    fatal(0x2d, alias, 0x4000, 0);
    fatal(4, stack, alias, P);
    assert(!pages(0, alias, 0x4000));
    fatal(1, 0, 0x200000, 0);
    assert(!resize(0x200000)); fatal(1, 0, 0, 0); assert(!resize(0));
}
// Exercise the real host helper through SDK-shaped test entry points. Its stack
// allocation must reach MapMemory as guest backing and outlive SDK copyback.
#include "../hle_guestthread.c"
static void (*helper_entry)(void *);
static void *helper_arg, *helper_stack;
static size_t helper_size;
static int helper_started, helper_destroyed, helper_freed;
void dm_thread_mark_host(void) {}
uint32_t thr_cur_handle(void) { return 0x1001; }
static void *helper_malloc(size_t n) {
    assert(n < 0x200000); assert(!resize(0x200000));
    return (void *)(heap + 32); // unaligned allocation, like SDK malloc
}
static void helper_free(void *p) {
    assert(p == (void *)(heap + 32) && helper_destroyed);
    assert(*(uint8_t *)helper_stack == 0x87); // DestroyThread already copied back
    helper_freed = 1; assert(!resize(0));
}
static int helper_create(void *tt, void (*fn)(void *), void *arg, void *source, size_t n, int prio) {
    (void)tt; (void)prio; helper_entry = fn; helper_arg = arg; helper_stack = source; helper_size = n;
    return (int)map(stack, (uintptr_t)source, n);
}
static void helper_start(void *tt) { (void)tt; helper_entry(helper_arg); helper_started = 1; }
static void helper_wait(void *tt) { (void)tt; assert(helper_started); }
static void helper_destroy(void *tt) {
    (void)tt; assert(!unmap(stack, (uintptr_t)helper_stack, helper_size)); helper_destroyed = 1;
}
void *guest_sym(const char *name) {
    if (!strcmp(name, "malloc")) return helper_malloc;
    if (!strcmp(name, "free")) return helper_free;
    if (!strcmp(name, "_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi")) return helper_create;
    if (!strcmp(name, "_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE")) return helper_start;
    if (!strcmp(name, "_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE")) return helper_wait;
    if (!strcmp(name, "_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE")) return helper_destroy;
    assert(0); return NULL;
}
static void helper_body(void *arg) { assert(arg == &helper_started); *(uint8_t *)stack = 0x87; }
static void guest_helper(void) {
    void *tt = dm_guest_thread_spawn(helper_body, &helper_started, 9, 0x10000);
    assert(helper_started && !helper_destroyed && !helper_freed);
    assert(resize(0) == BAD);
    dm_guest_thread_join(tt); assert(helper_freed && !g_nmap && !g_heap_cur);
}
static void *init_worker(void *arg) { *(uint64_t *)arg = info(I_AliasAddr); return NULL; }
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--heap-probe")) {
        mem_init();
        assert(resize(g_heap_cap + 0x200000) == OOM);
        g_nmod = 1; g_mod[0].size = 0xa00000;
        assert(resize(0x800000) == LIMIT);
        return 0;
    }
    pthread_t threads[8]; uint64_t addresses[8];
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&threads[i], NULL, init_worker, &addresses[i]));
    for (unsigned i = 0; i < 8; ++i) { assert(!pthread_join(threads[i], NULL)); assert(addresses[i] == addresses[0]); }
    alias = addresses[0]; stack = info(I_StackAddr); heap = info(I_HeapAddr);
    lazy_commit(); alias_lifecycle(); ownership(); module_sources(); failures(); concurrency(); fragmented_queries();
    guest_helper();
    if (!DM_GAME_MEM_MAP_LIMIT) metadata_limit();
#ifdef TEST_REAL_DIRTY
    dirty_integration();
#endif
    assert(!g_nmap && !g_alias_mapped && !g_heap_cur);
    assert(info(I_UsedMem) == DM_GAME_MEM_USED);
    // Offline NRO copies are not extra guest memory: the game allocates their buffers in its heap. Ordinary modules' large bss still counts.
    g_nmod = 2;
    g_mod[0] = (module_t){.size=0x10000000, .file_image_len=0x100000, .patch_off=-0x2000000};
    g_mod[1] = (module_t){.size=0x160000000, .file_image_len=0x100000, .patch_off=-0x2000000, .nro=1};
    assert(info(I_UsedMem) == DM_GAME_MEM_USED + 0xff00000);
    assert(info(I_UsedNonSys) == DM_GAME_MEM_USED + 0xff00000);
    assert(image_bytes() == 0x10000000);
    g_nmod = 0;
    free(g_alias_bits); free(g_alias_exposed); assert(!munmap(g_as, RESERVE));
    printf("memory SVCs passed (map limit=%d, host page=%d)\n", DM_GAME_MEM_MAP_LIMIT, memory_getpagesize());
    return 0;
}
