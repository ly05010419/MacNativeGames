/* CPU only: TOTK_PIPELINE page state and in-place ranges (nvn_dirty.c "one frame deeper"): classification of GPU
   reads (snapshot or in place), probation, the release-time protection and the holds of CPU writes while a released
   frame still runs. A fake "completed fence" stands in for the GPU and a fake guest-thread handle for the runtime's.
   - a page seen the first time is put on probation (protected, snapshot for PROBE frames), then static;
   - a CPU write on a protected page makes it volatile (snapshot) for KEEP frames;
   - GPU writes in flight are reported by nvn_pl_gpu_written until their fence completes;
   - a write into a published in-place range of a released, unfinished submission waits until the GPU passes it; a
     write elsewhere on the same page goes through (a pass); an announced host write over the range waits as well;
     a host thread (handle 0) is not held; neither a write into a range of a submission not released or completed;
   - nvn_pl_reprotect protects again a page of a released range that a CPU write unprotected before the release, and
     every page of the ranges that are not static;
   - TOTK_PIPELINE=2 (dry run) counts the write and does not wait; a watched page is recorded on the first read and
     restored, and a write to a watched page the tracker keeps read-only is not retried forever (the RWATCH hang).
   TOTK_PROBE_NEGATIVE=1: the fence never completes; the held write gives up after PL_HOLD_MS (a timeout) instead of
   hanging, which is the deadlock guard. */
#define NVN_DIRTY_NO_HANDLE_DEFAULT 1
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_dirty.c"
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>

static int (*g_fault)(void *, int, void *);
static void (*g_prep)(void *, size_t);
void dm_fault_hook_register(int (*fn)(void *, int, void *)) { g_fault = fn; }
void dm_guest_write_prep_register(void (*fn)(void *, size_t)) { g_prep = fn; }
static _Thread_local uint32_t t_handle;
uint32_t runtime_thread_current_handle(void) { return t_handle; }
static void on_signal(int sig, siginfo_t *si, void *uc) {
    int is_write = (int)((((ucontext_t *)uc)->uc_mcontext->__es.__esr >> 6) & 1);
    if (g_fault && g_fault(si->si_addr, is_write, uc)) return;
    signal(sig, SIG_DFL);
}
static _Atomic uint64_t g_done_fence;
static uint64_t done_fn(void) { return atomic_load(&g_done_fence); }
static uint64_t now_ms(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000ull; }
static int g_fail, g_checks;
#define EXPECT(name, want, got) do { long long w_ = (long long)(want), g_ = (long long)(got); g_checks++; \
    printf("%-70s expected %-6lld got %-6lld %s\n", name, w_, g_, w_ == g_ ? "ok" : "FAIL"); if (w_ != g_) g_fail++; } while (0)

static volatile uint8_t *g_mem; static size_t g_pg_sz;
static _Atomic int g_t_done; static uint32_t g_t_handle;
static void *writer(void *arg) { t_handle = g_t_handle; size_t off = (size_t)(uintptr_t)arg; g_mem[off] = 99; atomic_store(&g_t_done, 1); return NULL; }
static void *host_writer(void *arg) { t_handle = g_t_handle; size_t off = (size_t)(uintptr_t)arg; g_prep((void *)(g_mem + off), 16); g_mem[off] = 77; atomic_store(&g_t_done, 1); return NULL; }
/* 1 = the thread finished within wait_ms (before the fence was completed); then the fence completes and the thread is joined */
static int run_thread(void *(*fn)(void *), size_t off, uint32_t handle, int wait_ms, uint64_t complete_fence) {
    pthread_t t; atomic_store(&g_t_done, 0); g_t_handle = handle;
    pthread_create(&t, NULL, fn, (void *)(uintptr_t)off);
    uint64_t t0 = now_ms(); while (!atomic_load(&g_t_done) && now_ms() - t0 < (uint64_t)wait_ms) usleep(1000);
    int early = atomic_load(&g_t_done);
    if (complete_fence) atomic_store(&g_done_fence, complete_fence);
    pthread_join(t, NULL);
    return early;
}
static int page_writable(size_t off) {
    vm_address_t a = (vm_address_t)(g_mem + off); vm_size_t sz = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    if (vm_region_64(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) return -1;
    return (info.protection & VM_PROT_WRITE) ? 2 : (info.protection & VM_PROT_READ) ? 1 : 0;
}

static void publish1(uint64_t fence, uintptr_t lo, uintptr_t hi) { nvn_pl_publish(fence, &lo, &hi, 1, 0); }
static void publishw(uint64_t fence, uintptr_t lo, uintptr_t hi) { nvn_pl_publish(fence, &lo, &hi, 1, 1); }
int main(int argc, char **argv) {
    int dry = argc > 1 && !strcmp(argv[1], "dry"), negative = getenv("TOTK_PROBE_NEGATIVE") && atoi(getenv("TOTK_PROBE_NEGATIVE"));
    setenv("TOTK_PIPELINE", dry ? "2" : "1", 1);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_signal; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    g_pg_sz = (size_t)getpagesize();
    g_mem = mmap(NULL, 16 * g_pg_sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    nvn_dirty_add_range((const void *)g_mem, 16 * g_pg_sz);
    nvn_pl_set_done(done_fn);
    EXPECT("mode", dry ? 2 : 1, nvn_pl_mode());
    const uint32_t KEEP = 8, PROBE = 3; uint32_t fr = 100; nvn_pl_set_frame(fr);
    uintptr_t M = (uintptr_t)g_mem; size_t P = g_pg_sz;
    atomic_store(&g_done_fence, 10);
    if (negative) {   // released fence 11 never completes: the held write must give up (timeout), not hang
        publish1(11, M + 9 * P, M + 9 * P + 256);
        nvn_dirty_protect((const void *)(M + 9 * P), P);
        nvn_pl_set_released(11, 10);
        uint64_t t0 = now_ms(); t_handle = 7; g_mem[9 * P] = 5; uint64_t dt = now_ms() - t0;
        uint64_t v[NVN_PLS_N]; nvn_pl_stats(v);
        EXPECT("negative: the held write timed out (1 timeout)", dry ? 0 : 1, v[NVN_PLS_TIMEOUT]);
        EXPECT("negative: it waited about PL_HOLD_MS (>= 400 ms)", dry ? 0 : 1, dt >= 400);
        printf("%d checks, %d failed\n", g_checks, g_fail); return g_fail != 0;
    }
    EXPECT("first sight: snapshot (probation)", 1, nvn_pl_classify((const void *)g_mem, 64, KEEP, PROBE, 1, NULL));
    EXPECT("first sight: page write-protected", 1, page_writable(0));
    nvn_pl_set_frame(fr + PROBE - 1);
    EXPECT("still on probation", 1, nvn_pl_classify((const void *)g_mem, 64, KEEP, PROBE, 1, NULL));
    nvn_pl_set_frame(fr + PROBE);
    EXPECT("probation over without writes: static", 0, nvn_pl_classify((const void *)g_mem, 64, KEEP, PROBE, 1, NULL));
    EXPECT("first=0: an unseen page is static", 0, nvn_pl_classify((const void *)(g_mem + 5 * P), 64, KEEP, PROBE, 0, NULL));
    fr += PROBE; nvn_pl_set_frame(fr);
    EXPECT("write with nothing in flight finishes at once", 1, run_thread(writer, 8, 7, 200, 0));
    EXPECT("written page: snapshot (volatile)", 1, nvn_pl_classify((const void *)g_mem, 64, KEEP, PROBE, 1, NULL));
    nvn_pl_set_frame(fr + KEEP - 1);
    EXPECT("volatile until KEEP frames", 1, nvn_pl_classify((const void *)g_mem, 64, KEEP, PROBE, 1, NULL));
    nvn_pl_set_frame(fr + KEEP);
    EXPECT("KEEP expired: probation again (snapshot)", 1, nvn_pl_classify((const void *)g_mem, 64, KEEP, PROBE, 1, NULL));
    EXPECT("probation again: page protected", 1, page_writable(0));
    fr += KEEP; nvn_pl_set_frame(fr);
    nvn_pl_gpu_write((const void *)(g_mem + 2 * P), 32, 11);
    EXPECT("GPU write in flight (fence 11, done 10)", 1, nvn_pl_gpu_written((const void *)(g_mem + 2 * P), 64, 10));
    EXPECT("GPU write completed (done 11)", 0, nvn_pl_gpu_written((const void *)(g_mem + 2 * P), 64, 11));
    EXPECT("GPU write: another page untouched", 0, nvn_pl_gpu_written((const void *)(g_mem + 6 * P), 64, 10));
    // page 3: bytes [256, 512) read in place by fence 11, released at 11 (done 10)
    publish1(11, M + 3 * P + 256, M + 3 * P + 512);
    nvn_dirty_protect((const void *)(M + 3 * P), P);
    nvn_pl_set_released(11, 10);
    EXPECT("write elsewhere on the page: goes through (pass)", 1, run_thread(writer, 3 * P + 4096, 7, 150, 0));
    { uint64_t v[NVN_PLS_N]; nvn_pl_stats(v); EXPECT("counted as a pass", 1, v[NVN_PLS_PASS]); }
    nvn_dirty_protect((const void *)(M + 3 * P), P);
    EXPECT(dry ? "dry: write into the range not held" : "write into the range held until fence 11", dry ? 1 : 0, run_thread(writer, 3 * P + 300, 7, 150, 11));
    { uint64_t v[NVN_PLS_N]; nvn_pl_stats(v); EXPECT(dry ? "dry: counted as would-hold" : "counted as a hold", 1, dry ? v[NVN_PLS_WOULD] : v[NVN_PLS_HOLD]); }
    EXPECT("written through: byte stored", 99, g_mem[3 * P + 300]);
    EXPECT("held page is volatile now", 1, nvn_pl_classify((const void *)(g_mem + 3 * P), 64, KEEP, PROBE, 1, NULL));
    // fence 12 released (done 11), fence 13 published but not released: a write into 13's range only is not held
    publish1(12, M + 4 * P, M + 4 * P + 64); publish1(13, M + 6 * P, M + 6 * P + 64);
    nvn_dirty_protect((const void *)(M + 4 * P), P); nvn_dirty_protect((const void *)(M + 6 * P), P);
    nvn_pl_set_released(12, 11);
    EXPECT("range of an unreleased submission: write not held", 1, run_thread(writer, 6 * P, 7, 150, 0));
    EXPECT("host thread (handle 0): write not held", 1, run_thread(writer, 4 * P, 0, 150, 0));
    // page 8: announced host write over 12's range: held until 12
    publish1(12, M + 8 * P, M + 8 * P + 64);
    nvn_dirty_protect((const void *)(M + 8 * P), P);
    EXPECT(dry ? "dry: announced host write not held" : "announced host write held until 12", dry ? 1 : 0, run_thread(host_writer, 8 * P, 7, 150, 12));
    EXPECT("announced host write stored", 77, g_mem[8 * P]);
    // completed: done 12, a write into 12's range is not held
    nvn_dirty_protect((const void *)(M + 8 * P), P);
    EXPECT("completed submission: write not held", 1, run_thread(writer, 8 * P + 8, 7, 150, 0));
    // reprotection at the release of 14 (done 13): a page of its range unprotected before the release, and a non-static range
    atomic_store(&g_done_fence, 13);
    publish1(14, M + 11 * P, M + 11 * P + 64);
    nvn_dirty_protect((const void *)(M + 11 * P), P);
    nvn_pl_reprotect(13, 13, NULL, NULL, 0);   // consume the ring so far
    g_mem[11 * P] = 1;   // a CPU write before the release (fence 14 not yet released: not held)
    EXPECT("page writable before the release", 2, page_writable(11 * P));
    uintptr_t nlo = M + 10 * P, nhi = M + 10 * P + 32;   // never protected, not static
    EXPECT("release protects both pages again", 2, nvn_pl_reprotect(13, 14, &nlo, &nhi, 1));
    EXPECT("unprotected page of the range read-only again", 1, page_writable(11 * P));
    EXPECT("page of the non-static range read-only", 1, page_writable(10 * P));
    { nvn_pl_hold_rec h[8]; int n = nvn_pl_holds(h, 8); EXPECT("hold records kept", 1, n >= 2); }
    {   // a write that meets only a GPU write range of the released frame goes through (TOTK_PIPELINE_WWHOLD=1 holds it)
        atomic_store(&g_done_fence, 15); publishw(16, M + 15 * P, M + 15 * P + 256); publish1(16, M + 15 * P + 1024, M + 15 * P + 1100);
        nvn_dirty_protect((const void *)(M + 15 * P), P); nvn_pl_set_released(16, 15);
        EXPECT("write over a GPU write range only: not held", 1, run_thread(writer, 15 * P + 64, 7, 150, 0));
        uint64_t v[NVN_PLS_N]; nvn_pl_stats(v); EXPECT("counted", 1, v[NVN_PLS_WWPASS]);
        nvn_dirty_protect((const void *)(M + 15 * P), P);
        EXPECT(dry ? "dry: write over a read range not held" : "write over a GPU read range: held", dry ? 1 : 0, run_thread(writer, 15 * P + 1040, 7, 150, 16));
    }
    {   // a page whose other part the CPU writes after every release is left writable after 4 passes (TOTK_PIPELINE_PASSKEEP)
        publish1(15, M + 14 * P, M + 14 * P + 64); nvn_pl_set_released(15, 14); atomic_store(&g_done_fence, 14);
        int prot = 0;
        for (int i = 0; i < 4; i++) { nvn_pl_protect_span((const void *)(M + 14 * P), 64); prot += page_writable(14 * P) == 1; t_handle = 7; g_mem[14 * P + 8192] = (uint8_t)i; }
        EXPECT("protected again after each of the first 4 passes", 4, prot);
        nvn_pl_protect_span((const void *)(M + 14 * P), 64);
        EXPECT("then left writable", 2, page_writable(14 * P));
        uint64_t v[NVN_PLS_N]; nvn_pl_stats(v); EXPECT("counted", 1, v[NVN_PLS_PASSKEEP]);
    }
    if (dry) {   // TOTK_PIPELINE_RWATCH
        uintptr_t pg = M + 12 * P, pg2 = M + 13 * P; uintptr_t two[2] = {pg2, pg};
        nvn_dirty_protect((const void *)pg2, P);   // the tracker wants page 13 read-only
        EXPECT("watch: two pages", 2, nvn_pl_watch(two, 2));
        EXPECT("watched page inaccessible", 0, page_writable(12 * P));
        volatile uint8_t x = g_mem[12 * P]; (void)x;
        uint64_t v[NVN_PLS_N]; nvn_pl_stats(v);
        EXPECT("watched read recorded", 1, v[NVN_PLS_WATCH_READ]);
        EXPECT("page accessible again (tracker: writable)", 2, page_writable(12 * P));
        g_mem[13 * P] = 4;   // a write to a watched page the tracker keeps read-only: restored to read-only, then the tracker's fault
        EXPECT("write to a watched read-only page finishes", 4, g_mem[13 * P]);
        EXPECT("watch over", 0, nvn_pl_watch(NULL, 0));
    }
    {   // range lookups through the sorted index: 3000 ranges added in reverse order, every third removed, random addresses
        int N = 3000; uint8_t *m = mmap(NULL, P * 3 * N, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); int bad = 0;
        for (int i = 0; i < N; i++) nvn_dirty_add_range(m + (size_t)(N - 1 - i) * 3 * P, 2 * P);
        for (int i = 0; i < N; i += 3) nvn_dirty_remove_range(m + (size_t)i * 3 * P);
        for (int k = 0; k < 20000; k++) {
            size_t off = (size_t)arc4random_uniform((uint32_t)(3 * N)) * P + arc4random_uniform((uint32_t)P); range_t *r = find((uintptr_t)m + off);
            size_t idx = off / (3 * P); int live = off % (3 * P) < 2 * P && idx % 3 != 0;
            bad += !!r != live || (r && ((uintptr_t)m + off < r->base || (uintptr_t)m + off >= r->end));
        }
        EXPECT("indexed range lookups agree (20000 random addresses)", 0, bad);
    }
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail != 0;
}
