//          0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
// Thread and synchronization SVCs of the Horizon kernel, implemented natively: each guest thread = one host pthread, running on the guest stack given by the SDK.
// The SDK's nn::os runs as is; only the "kernel" underneath it is replaced by the host:
//   Threads    0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D priority 0x0E/0x0F core mask
//              0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   Locks      0x1A ArbitrateLock 0x1B ArbitrateUnlock (tag word = owner handle | 0x40000000 has waiters)
//   Condition  0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   Address    0x34 WaitForAddress 0x35 SignalToAddress
//   Events     0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// Semantics follow the public Horizon kernel behavior (KConditionVariable / KAddressArbiter / KSynchronization of Atmosphère mesosphère).
// All waits hang on one global lock G, with one condition variable per thread; correctness first, performance to be measured later.
#include "dm.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <mach/mach.h>
#include <pthread/qos.h>
#include <dlfcn.h>
#include <stdatomic.h>

#define R_TIMEOUT     0xEA01u   // Kernel module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_OUT_OF_RES  0xCE01u   // 103
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W };
// Service-layer SystemEvent bindings (see hle_bind_system_event): guest SystemEventType address → the write-end handle of its kernel event.
// An entry lives as long as that handle: CloseHandle on it removes the entry (se_unbind), so a SystemEvent destroyed and created again at the same address gets a new kernel event.
// Entries used to stay forever: the second bind returned early, the new event was never created and signals went to the closed handle (or to whatever event reused its number).
// Lock order: G, then g_se_lock (nothing takes G while holding g_se_lock)
static struct { void *ev; uint32_t wh; } g_se[512];
static int g_nse;
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(void *ev) {   // caller holds g_se_lock
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
static void se_unbind(uint32_t wh) {   // caller holds G
    pthread_mutex_lock(&g_se_lock);
    for (int i = 0; i < g_nse; ) if (g_se[i].wh == wh) g_se[i] = g_se[--g_nse]; else i++;
    pthread_mutex_unlock(&g_se_lock);
}
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns; uint32_t born_tick, exit_tick;   // Time of creation / exit and the game tick at that time (for the state dump: did the job worker exit before the stall)
    int started, exited, cancel;
    pthread_t pt; pthread_cond_t cv;
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    _Atomic int sleeping;      // inside SleepThread's timed sleep (not runnable; used by dm_yield_to_core_peers)
    int yield_pending;         // this SVC woke a higher-priority thread pinned to this core: let it run before the SVC returns (svc_thread)
    _Atomic int *host_work;    // points at the thread's dm_host_work (registered when the thread starts)
    int host_owned;            // a guest thread the host created itself (dm_guest_thread_spawn): it waits in host code and is not one of the game's threads
    uint64_t wseq;             // Sequence number of wait registration: at equal priority first come, first woken (same order as the Horizon kernel's KThread wait tree)
    struct thr *next;          // Wait chain
    struct thr *all;           // All-threads chain (for the state dump)
    int last_svc; uint64_t last_lr;
    int closed;                // Handle closed: the object is freed only when this holds together with exited (Horizon: the thread object goes away with the last reference)
    uint64_t nsvc;             // SVC count (for the state dump)
    uint64_t nyield, nyield_seen, nbackoff, nbackoff_seen;   // Yields (SleepThread 0/-1/-2) and those the back-off turned into short sleeps, with both counts at the last power report (dm_yield_top)
    uint64_t nspin, nspin_seen, spin_site; const char *spin_mod;   // Spin wait passes (dm_spin_wait; those that yield count in nyield too) and where the last one was
    uint64_t srun_svc, srun_t0, spin_ret;     // Current spin wait: nsvc at its passes, when it began, when the last pass returned
    uint64_t cpu_seen_us;                     // CPU time (µs) at the last power report (dm_thread_cpu)
    uint64_t yrun_svc, yrun_t0, yrun_sig;     // Current run of yields: nsvc of the last yield, when the run began, and the caller's x19–x28 at the last yield (a run lasts while no other SVC comes between two yields and those registers stay the same, thr_yield)
    mach_port_t mport;         // Host thread port (to get CPU time)
    _Atomic int qos_pending;   // SetThreadPriority from another thread changed prio: the thread applies the new class at its next SVC (only a thread can set its own QoS)
    uint32_t qos;
    // Pausing (SetThreadActivity, used by IL2CPP's garbage collector to stop the world): pause_req = requested, paused = stopped; when stopped inside an SVC the context comes from the gate frame svc_regs (registered on SVC entry),
    // when stopped in guest code the signal handler saves it into ctx_*; pcv lets the requester wait for "stopped" and the paused thread wait for "resume"
    _Atomic int pause_req; int paused;
    uint64_t *_Atomic svc_regs; uint64_t svc_pc, svc_x18;   // svc_x18: the guest's x18 at SVC entry (its thread slot; the gate frame holds the Apple platform register)
    // SVC exit without G: bit 0 = inside an SVC, bit 1 = a pause was requested. The exit clears bit 0 with one compare-and-swap when bit 1 is clear, and takes G only otherwise;
    // the requester sets bit 1 and treats the thread as stopped only if bit 0 was set, so a thread it counts as stopped in an SVC cannot leave the SVC without G (svc_regs stays valid for GetThreadContext3)
    _Atomic int svc_state;
    const uint32_t *whs; int whn;   // WaitSynchronization's handle array (guest memory, valid while the thread waits there): a signal wakes only waiters that hold the signalled object
    int ctx_ok; uint64_t ctx_x[29], ctx_fp, ctx_lr, ctx_sp, ctx_pc; uint32_t ctx_pstate, ctx_fpcr, ctx_fpsr; __uint128_t ctx_v[32];
    pthread_cond_t pcv;
} thr_t;
static thr_t *g_all;
extern __thread _Atomic int dm_host_work;   // declared in hle.h too; defined in hle_nvn.c
static _Atomic uint64_t g_preempt_n, g_preempt_ns, g_preempt_cap;   // same-core preemption yields: count, total wait, cap hits (svc_thread)

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
static pthread_key_t g_keythr;
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          // All waiting threads (lock / condition / address / synchronization)

static uint32_t h_new(int type, void *obj) {     // Caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("handle table full");
}
static void *h_peek(uint32_t h, int type) {      // Lookup only, no report
    if (h == H_CUR_THREAD && type == O_THREAD) return pthread_getspecific(g_keythr);
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // Reports if not found (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("invalid handle %#x (wanted type %d)", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) die("thread SVC called from a non-guest thread");
    return t;
}
// Let higher-priority threads of the same core run first: Horizon schedules per core, and when a signal from one thread wakes a higher-priority thread pinned to the same core, that thread preempts at once and runs until it blocks again before the signalling thread continues.
// Some games rely on this as synchronization: in Metroid Dread's movie player the main thread (priority 44, core 0) posts "tear down the decoder" to the dispatcher and worker threads (priority 30, core 0 only), then frees the object without waiting
// and builds the next one in the same memory; on the host the threads truly run in parallel, the dispatcher still had the old holders linked and the worker still had the old object queued, so SignalEvent hit a null MultiWait and the worker read a freed object.
// svc_thread calls it after an SVC that woke such a thread: it waits until every started, unexited, not host-created thread allowed only on this core with a higher priority is waiting or sleeping, for at most max_us; returns −1 on timeout
void dm_thread_mark_host(void) { thr_t *t = pthread_getspecific(g_keythr); if (t) t->host_owned = 1; }
// Whether a thread is really running: besides guest waits / timed sleeps it may be blocked in host code (an HLE call waiting on a host condition variable, the GPU, a file), which has no guest wait kind; only the kernel's thread state tells
static int thr_running(const thr_t *t) {
    if (t->wkind || t->sleeping || !t->mport || (t->host_work && *t->host_work)) return 0;
    thread_basic_info_data_t bi; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
    if (thread_info(t->mport, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) != KERN_SUCCESS) return 0;
    return bi.run_state == TH_STATE_RUNNING;
}
int dm_yield_to_core_peers(unsigned max_us) {
    thr_t *self = pthread_getspecific(g_keythr); if (!self) return 0;
    uint64_t bit = 1ull << (self->core & 63), t0 = now_ns(); int waited = 0;
    for (;;) {
        thr_t *busy = NULL;
        pthread_mutex_lock(&G);
        for (thr_t *t = g_all; t && !busy; t = t->all)
            if (t != self && t->started && !t->exited && !t->host_owned && t->mask == bit && t->prio < self->prio && thr_running(t)) busy = t;
        uint32_t bh = busy ? busy->handle : 0; int bp = busy ? busy->prio : 0; uint64_t blr = busy ? busy->last_lr : 0; int bsvc = busy ? busy->last_svc : 0;
        pthread_mutex_unlock(&G);
        if (!busy) return waited;
        if (now_ns() - t0 > (uint64_t)max_us * 1000) {
            static _Atomic int nlog; if (nlog++ < 8) { module_t *m = module_of((void *)blr);   // the first few cap hits: who waits, who keeps running
                LOG("same-core preemption yield hit its cap: thread %#x (priority %d core %d) waits for %#x (priority %d, last svc %#x lr=%s+%#llx)", self->handle, self->prio, self->core, bh, bp, bsvc, m ? m->name : "?", (unsigned long long)(m ? blr - (uint64_t)m->base : blr)); }
            return -1;
        }
        { struct timespec ts = {0, 50000}; nanosleep(&ts, 0); } waited = 1;
    }
}

// ---------- Wait primitives (called holding G) ----------
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// Pick a waiter in kernel order: numerically lower priority first, at equal priority the earlier registered first (holding G). kind 1 matches by lock address, 2 by condition key, 3 by address
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(thr_t *t, int64_t res) {
    wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv);
    thr_t *self = pthread_getspecific(g_keythr);   // the woken thread is pinned to this core with a higher priority: on Horizon it preempts at once (see dm_yield_to_core_peers)
    if (self && self != t && !t->host_owned && t->prio < self->prio && t->mask == 1ull << (self->core & 63)) self->yield_pending = 1;
}
// Suspend until woken or timed out (ns<0 infinite, 0 immediate timeout); returns wres or R_TIMEOUT
static int64_t block(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    struct timespec dl;
    int det_wait = ns > 0 && dm_detclock_enabled();
    uint64_t guest_deadline = det_wait ? guest_ns() + (uint64_t)ns : 0;
    if (ns > 0 && !det_wait) { clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
                  if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    while (!t->woke) {
        int rc;
        if (det_wait) {
            uint64_t now = guest_ns();
            if (now >= guest_deadline) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
            (void)(guest_deadline - now);
            rc = pthread_cond_wait(&t->cv, &G);
        } else rc = ns > 0 ? pthread_cond_timedwait(&t->cv, &G, &dl) : pthread_cond_wait(&t->cv, &G);
        if (rc == ETIMEDOUT && !t->woke && (!det_wait || guest_ns() >= guest_deadline)) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

// ---------- Locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline uint64_t ld64(uint64_t a) { return __atomic_load_n((uint64_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// Make t take the lock at addr: if free, it gets it directly and is woken, otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // Still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // The kernel inserts it anew into the lock owner's wait queue: at equal priority it goes after the existing lock waiters
}

// ---------- Threads ----------
extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);   // entry.S
uint32_t (*dm_thread_tick_fn)(void);   // Gets the current game tick; registered when diag.c initializes (unit tests do not register it, the tick number is recorded as 0)
void crash_thread_init(void);
void guest_thread_init(void);   // thread.c: the single place that sets the per-thread TSD slots (gate, x18, TLS, TLR)
// Switch priority → QoS class (DM_QOS_MAP=1; off by default until a phone A/B shows it saves power without dropping frames). Otherwise every guest thread runs at user-interactive
// whatever its priority, and the performance controller keeps all of them on fast cores at high clocks. Bands: priority <= e1 (the main thread is 44) user-interactive, <= e2 user-initiated,
// above that utility; DM_QOS_EDGES=e1,e2 moves them (default 44,59). Read once in svc_thread_main_init, before any other guest thread exists
static int g_qos_map, g_qos_e1 = 44, g_qos_e2 = 59;
static void qos_map_init(void) {
    const char *e = getenv("DM_QOS_MAP"), *d = getenv("DM_QOS_EDGES"); g_qos_map = e && atoi(e);
    if (d) sscanf(d, "%d,%d", &g_qos_e1, &g_qos_e2);
    if (g_qos_map) LOG("DM_QOS_MAP=1: guest threads get QoS by priority (≤%d interactive, ≤%d user-initiated, the rest utility)", g_qos_e1, g_qos_e2);
}
int dm_qos_map_on(void) { return g_qos_map; }
static qos_class_t prio_qos(int prio) {
    if (!g_qos_map) return QOS_CLASS_USER_INTERACTIVE;
    return prio <= g_qos_e1 ? QOS_CLASS_USER_INTERACTIVE : prio <= g_qos_e2 ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY;
}
static void qos_apply(thr_t *t) {   // on the thread itself
    qos_class_t q = prio_qos(t->prio);
    if ((uint32_t)q != t->qos && !pthread_set_qos_class_self_np(q, 0)) t->qos = (uint32_t)q;
}
// A host helper thread (audio notify / silent sink, vsync, movie decode) created from a guest thread inherits that thread's class; under DM_QOS_MAP it sets its own instead
void dm_host_helper_qos(qos_class_t q) { if (g_qos_map) pthread_set_qos_class_self_np(q, 0); }
static void *thread_main(void *p) {
    thr_t *t = p;
    pthread_setspecific(g_keythr, t);
    pthread_set_qos_class_self_np(prio_qos(t->prio), 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    t->mport = pthread_mach_thread_np(pthread_self()); t->host_work = &dm_host_work;
    crash_thread_init();
    guest_thread_init();
    pthread_mutex_lock(&G); while (!t->started) pthread_cond_wait(&t->cv, &G); pthread_mutex_unlock(&G);
    dm_thread_enter(t->entry, t->arg, t->stack_top);   // Does not return: the guest must exit through ExitThread
    die("guest thread entry returned");
}
static void thr_free(thr_t *t) {   // Holding G; unlink from the all-threads chain and free
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); pthread_cond_destroy(&t->pcv); free(t);
}
static const char *thr_name(thr_t *t, char *buf, size_t n);
static void wake_sync_waiters(const void *obj);
static void thread_exit(thr_t *t) {
    t->exit_ns = now_ns(); t->exit_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
    // No logging here: the exit happens on the guest thread's own stack (some are only a dozen or so KiB), and a printf with floating point would blow the stack (real-device smoke test: SIGBUS when LibcurlResolver exits, backtrace fprintf → __dtoa). Only the time is recorded, and it is printed later in the state dump
    pthread_mutex_lock(&G);
    t->exited = 1;
    wake_sync_waiters(t);   // Synchronization waiters on this thread re-check
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    pthread_exit(NULL);
}
void dm_thread_returned(void) { die("guest thread entry function returned (Horizon requires ExitThread)"); }

// IL2CPP's Boehm garbage collector stops the world: SetThreadActivity(handle, paused) on each thread, GetThreadContext3 to read registers for roots, scan the stack, then resume. Both used to be placeholders that reported success:
// threads kept running and the context was empty, so the collector missed pointers held only in registers and freed objects still in use (finalizers releasing native resources with them). They now really stop, as on Horizon:
//   · inside an SVC (the gate frame holds every register): stopped at once, context = the gate frame; if still paused when the SVC returns, the thread waits at the exit for the resume;
//   · running guest code: send SIGUSR2; the handler saves the ucontext and waits there for the resume (guest code holds no host locks, so the handler may take G);
//   · inside host code (HLE functions etc.): the handler does not stop it; the requester resends every 100 µs until the thread is back in guest code or enters an SVC.
module_t *module_of(const void *p);
static void on_pause_sig(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t || !atomic_load(&t->pause_req) || atomic_load(&t->svc_regs)) return;
    ucontext_t *uc = uc_; const struct __darwin_arm_thread_state64 *ss = &uc->uc_mcontext->__ss;
    if (!module_of((void *)__darwin_arm_thread_state64_get_pc(*ss))) return;   // in host code: do not stop; the requester resends
    pthread_mutex_lock(&G);
    if (t->pause_req) {
        for (int i = 0; i < 29; i++) t->ctx_x[i] = ss->__x[i];
        // x18 is the Apple platform register: the rewriter keeps the guest's x18 in its thread slot (patch_nso.py K18), and compiled game code may hold an object pointer there,
        // which a collector scanning this context (IL2CPP's, through GetThreadContext3) would otherwise not see. The rewriter's x18 sequences also park two registers up to 24 bytes
        // below sp for a few instructions, so the reported sp is 32 bytes lower: the collector scans the stack upward from it, and a few extra words cost a conservative scan nothing
        t->ctx_x[18] = (uint64_t)(uintptr_t)pthread_getspecific(g_key18);
        t->ctx_fp = __darwin_arm_thread_state64_get_fp(*ss); t->ctx_lr = __darwin_arm_thread_state64_get_lr(*ss);
        t->ctx_sp = __darwin_arm_thread_state64_get_sp(*ss) - 32; t->ctx_pc = __darwin_arm_thread_state64_get_pc(*ss); t->ctx_pstate = ss->__cpsr;
        memcpy(t->ctx_v, uc->uc_mcontext->__ns.__v, sizeof t->ctx_v); t->ctx_fpcr = uc->uc_mcontext->__ns.__fpcr; t->ctx_fpsr = uc->uc_mcontext->__ns.__fpsr;
        t->ctx_ok = 1; t->paused = 1; pthread_cond_broadcast(&t->pcv);
        while (t->pause_req) pthread_cond_wait(&t->pcv, &G);
        t->paused = 0; t->ctx_ok = 0;
    }
    pthread_mutex_unlock(&G);
}
// called by the gate on SVC entry / exit (gate_dispatch in hle.c)
void thr_svc_enter(uint64_t *regs, uint64_t pc) {
    thr_t *t = pthread_getspecific(g_keythr); if (!t) return;
    t->svc_pc = pc; t->svc_x18 = (uint64_t)(uintptr_t)pthread_getspecific(g_key18); atomic_store(&t->svc_regs, regs); atomic_fetch_or(&t->svc_state, 1);
    if (atomic_load_explicit(&t->qos_pending, memory_order_relaxed) && atomic_exchange(&t->qos_pending, 0)) qos_apply(t);
    if (atomic_load(&t->pause_req)) { pthread_mutex_lock(&G); if (t->pause_req && !t->paused) { t->paused = 1; pthread_cond_broadcast(&t->pcv); } pthread_mutex_unlock(&G); }
}
void thr_svc_leave(void) {
    thr_t *t = pthread_getspecific(g_keythr); if (!t) return;
    int in = 1; if (atomic_compare_exchange_strong(&t->svc_state, &in, 0)) { atomic_store(&t->svc_regs, NULL); return; }   // no pause requested: G is not needed (every other thread's SVC takes it)
    pthread_mutex_lock(&G);
    while (t->pause_req) { t->paused = 1; pthread_cond_broadcast(&t->pcv); pthread_cond_wait(&t->pcv, &G); }
    t->paused = 0; atomic_fetch_and(&t->svc_state, ~1); atomic_store(&t->svc_regs, NULL);
    pthread_mutex_unlock(&G);
}
static void pause_install(void) {
    struct sigaction sa = {.sa_sigaction = on_pause_sig, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
}
// Horizon svc::ThreadContext (arm64, 0x320 bytes)
typedef struct { uint64_t r[29], fp, lr, sp, pc; uint32_t pstate, pad; __uint128_t v[32]; uint32_t fpcr, fpsr; uint64_t tpidr; } hz_ctx_t;
_Static_assert(sizeof(hz_ctx_t) == 0x320, "ThreadContext");
static void ctx_fill(const thr_t *t, hz_ctx_t *o) {   // holding G, t is stopped
    memset(o, 0, sizeof *o);
    const uint64_t *r = atomic_load(&t->svc_regs);
    if (r) {   // gate frame: x0..x30 at +0, q0..q31 at +256, nzcv / fpsr / fpcr at +768 / +776 / +784; the stack top before the gate = frame + 800
        for (int i = 0; i < 29; i++) o->r[i] = r[i];
        o->r[18] = t->svc_x18;   // the gate frame's x18 is the host's (see on_pause_sig)
        o->fp = r[29]; o->lr = r[30]; o->sp = (uint64_t)r + 800; o->pc = t->svc_pc; o->pstate = (uint32_t)r[96];
        memcpy(o->v, r + 32, sizeof o->v); o->fpsr = (uint32_t)r[97]; o->fpcr = (uint32_t)r[98];
    } else if (t->ctx_ok) {
        memcpy(o->r, t->ctx_x, sizeof o->r); o->fp = t->ctx_fp; o->lr = t->ctx_lr; o->sp = t->ctx_sp; o->pc = t->ctx_pc; o->pstate = t->ctx_pstate;
        memcpy(o->v, t->ctx_v, sizeof o->v); o->fpcr = t->ctx_fpcr; o->fpsr = t->ctx_fpsr;
    }
}

// Register the main thread and return its handle (passed to nnosInitialize as the main thread handle)
uint32_t svc_thread_main_init(void) {
    pthread_key_create(&g_keythr, NULL);
    qos_map_init();
    pause_install();
    thr_t *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self(); t->mport = pthread_mach_thread_np(t->pt); t->host_work = &dm_host_work;
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, t);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    return t->handle;
}

static uint64_t g_main_yield_n, g_main_yield_ns;   // Written only by the guest main thread; the diagnostic line reads it on the same thread
void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = g_main_yield_n; *ns = g_main_yield_ns; }
uint32_t dm_guest_work_qos(unsigned *count) {
    uint32_t qos = QOS_CLASS_UNSPECIFIED; unsigned n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->handle == 0x1001) continue;
        if (!n) qos = t->qos; else if (qos != t->qos) qos = UINT32_MAX;
        n++;
    }
    pthread_mutex_unlock(&G);
    if (count) *count = n;
    return qos;
}

// ---------- Synchronization objects ----------
static int sig_obj(uint32_t h, int *bad) {    // 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
// A synchronization object became signaled (an event, or a thread that exited): wake the WaitSynchronization waiters whose handle set holds it, so they re-check (holding G).
// It used to wake every such waiter on every signal; each then re-checked its handles and went back to sleep, a wake-up per waiting thread per signal
static void wake_sync_waiters(const void *obj) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next;
        if (w->wkind == 4) { int hit = !w->whs;
            for (int i = 0; i < w->whn && !hit; i++) { unsigned k = w->whs[i] - 0x1000; hit = k > 0 && k < MAX_H && g_h[k].obj == obj; }
            if (hit) wake(w, -1); }
        w = n; }
}

static int64_t now_mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
void dm_guest_clock_advanced(void) {
    if (!dm_detclock_enabled()) return;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind) pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&G);
}

// Handle thread / synchronization SVCs; returns 1 = handled
static const char *thr_name(thr_t *t, char *nb, size_t n) {   // SDK ThreadType+0x1a0 = name pointer
    if (!t->arg) return t->handle == 0x1001 ? "main thread" : "?";
    uint64_t np = 0; vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), t->arg + 0x1a0, 8, (vm_address_t)&np, &got) || !np ||
        vm_read_overwrite(mach_task_self(), np, n - 1, (vm_address_t)nb, &got)) return "?";
    nb[got] = 0; return nb;
}
// All live guest threads (used by the sampler DM_SAMPLE=ms:*): fill the pthreads into out, return the count
int thr_list(pthread_t *out, int max) {
    int n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t && n < max; t = t->all) if (!t->exited && t->started) out[n++] = t->pt;
    pthread_mutex_unlock(&G);
    return n;
}
// Soak readings (DM_SOAK): handle table usage, live guest threads, total thread objects (including exited but not yet freed)
void thr_counts(int *handles, int *live, int *objs) {
    int nh = 0, nl = 0, no = 0;
    pthread_mutex_lock(&G);
    for (int i = 1; i < MAX_H; i++) nh += g_h[i].type != O_FREE;
    for (thr_t *t = g_all; t; t = t->all) { no++; nl += t->started && !t->exited; }
    pthread_mutex_unlock(&G);
    *handles = nh; *live = nl; *objs = no;
}
// The game's threads that hold a slot of the process's thread resource limit, as Horizon counts them: created (started or not) and not yet exited (GetInfo FreeThreadCount, hle_svc.c).
// Guest threads the host spawns for itself are not the game's
int thr_limit_used(void) {
    int n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) n += !t->exited && !t->host_owned;
    pthread_mutex_unlock(&G);
    return n;
}
// Get the guest thread name by pthread (used by per-thread CPU sampling DM_THRCPU, memstat.m): returns 0 if not a guest thread
int thr_name_of(pthread_t pt, char *out, size_t n) {
    int ok = 0; char nb[64];
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) if (t->pt == pt && !t->exited) { snprintf(out, n, "%#x %s", t->handle, thr_name(t, nb, sizeof nb)); ok = 1; break; }
    pthread_mutex_unlock(&G);
    return ok;
}
uint32_t thr_cur_handle(void) { thr_t *t = pthread_getspecific(g_keythr); return t ? t->handle : 0; }   // Only reads TSD, usable in a signal handler
// Find a live guest thread by thread name (used by the sampler); returns 0 if not found
int thr_find_by_name(const char *name, pthread_t *out) {
    char nb[32]; int ok = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) if (!t->exited && !strcmp(thr_name(t, nb, sizeof nb), name)) { *out = t->pt; ok = 1; break; }
    pthread_mutex_unlock(&G);
    return ok;
}
// State dump (called from crash.c on watchdog / crash; read-only, takes no lock): what each guest thread is waiting on and where its most recent SVC was issued from
// During a state dump, take the backtrace of other guest threads: suspend → read registers → walk the frame pointer chain (both guest code and gate blocks keep the x29 chain) → resume; only main / sdk frames are printed
static void thr_bt(thr_t *t) {
    if (!t->mport || t->exited || t->mport == mach_thread_self()) return;
    if (thread_suspend(t->mport) != KERN_SUCCESS) return;
    arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
        // Threads not waiting in a system call (running guest code, possibly spinning on a condition): print the callee-saved registers x19–x28, plus 128 bytes of the memory pointed to by each of x19 / x20.
        // A thread spinning on a condition is told apart by its callee-saved registers (often the object it waits on) and the memory they point to
        if (t->wkind == 0) { char rl[400]; int q = snprintf(rl, sizeof rl, "[dm]   %#x registers pc=", t->handle); uint64_t pc = __darwin_arm_thread_state64_get_pc(st); module_t *pm = module_of((void *)pc);
            q += pm ? snprintf(rl + q, sizeof rl - q, "%s+%#llx", pm->name, pc - (uint64_t)pm->base) : snprintf(rl + q, sizeof rl - q, "%#llx", pc);
            for (int i = 19; i <= 28; i++) q += snprintf(rl + q, sizeof rl - q, " x%d=%#llx", i, st.__x[i]);
            fprintf(stderr, "%s\n", rl);
            for (int i = 19; i <= 20; i++) { uint8_t raw[128]; vm_size_t g = 0; uint64_t a = st.__x[i];
                if (a < 0x100000000ull || vm_read_overwrite(mach_task_self(), a, sizeof raw, (vm_address_t)raw, &g) || g != sizeof raw) continue;
                char hx[420]; int h = 0; for (int k = 0; k < 128; k += 4) h += snprintf(hx + h, sizeof hx - h, "%08x ", *(uint32_t *)(raw + k));
                uint64_t vt = *(uint64_t *)raw; module_t *vm_ = module_of((void *)vt); char vs[64] = ""; if (vm_) snprintf(vs, sizeof vs, " (first 8 bytes = %s+%#llx)", vm_->name, vt - (uint64_t)vm_->base);
                fprintf(stderr, "[dm]   %#x x%d → 128 bytes from %#llx%s: %s\n", t->handle, i, a, vs, hx); } }
        uint64_t fp = __darwin_arm_thread_state64_get_fp(st), lr = __darwin_arm_thread_state64_get_lr(st);
        // Threads running guest code: print the 1 KiB above the stack top as is (the registers saved by the outer frames are in there, so the wait object and resource address can be recovered afterwards)
        if (t->wkind == 0 && module_of((void *)__darwin_arm_thread_state64_get_pc(st))) { uint64_t sp = __darwin_arm_thread_state64_get_sp(st), stk[128]; vm_size_t g = 0;   // Printed only if pc is inside a guest module: a thread in sleep / yield has its pc in a system library, and the stack is not worth looking at
            if (sp > 0x100000000ull && !vm_read_overwrite(mach_task_self(), sp, sizeof stk, (vm_address_t)stk, &g) && g == sizeof stk)
                for (int r = 0; r < 128; r += 8) fprintf(stderr, "[dm]   %#x stack sp+%#05x: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx\n", t->handle, r * 8,
                        stk[r], stk[r + 1], stk[r + 2], stk[r + 3], stk[r + 4], stk[r + 5], stk[r + 6], stk[r + 7]); }
        char line[1024]; int p = snprintf(line, sizeof line, "[dm]   %#x backtrace:", t->handle), nf = 0;
        for (int k = 0; k < 64 && fp && nf < 20 && p < (int)sizeof line - 40; k++) {
            module_t *m = module_of((void *)lr);
            if (m) { p += snprintf(line + p, sizeof line - p, " %s+%#llx", m->name, lr - (uint64_t)m->base); nf++; }
            uint64_t fr[2]; vm_size_t g2 = 0;
            if (vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)fr, &g2) || g2 != 16 || fr[0] <= fp) break;
            fp = fr[0]; lr = fr[1];
        }
        fprintf(stderr, "%s\n", line);
    }
    thread_resume(t->mport);
}
void svc_thread_dump(void) {
    static const char *kinds[] = {"running/not waiting", "mutex", "condvar", "address", "sync object"};
    int nh = 0; for (int i = 1; i < MAX_H; i++) nh += g_h[i].type != O_FREE;
    fprintf(stderr, "[dm] handle table use %d / %d, threads created so far %llu\n", nh, MAX_H, g_next_id - 0x51);
    fprintf(stderr, "[dm] same-core preemption yields %llu, total %.1f ms, hit the cap %llu times\n", (unsigned long long)g_preempt_n, g_preempt_ns / 1e6, (unsigned long long)g_preempt_cap);
    for (thr_t *t = g_all; t; t = t->all) {
        module_t *m = module_of((void *)t->last_lr);
        // Thread name: SDK ThreadType+0x1a0 is the name pointer (ThreadManager::SetInitialThreadNameUnsafe 0x34a2a4); x2 of CreateThread = ThreadType*
        char nb[32]; const char *nm = thr_name(t, nb, sizeof nb);
        double cpu = -1;
        if (t->mport && !t->exited) { thread_basic_info_data_t bi; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
            if (thread_info(t->mport, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) == KERN_SUCCESS)
                cpu = bi.user_time.seconds + bi.user_time.microseconds / 1e6 + bi.system_time.seconds + bi.system_time.microseconds / 1e6; }
        char own[96] = ""; if (t->wkind == 1 && t->waddr) { uint32_t w = 0; vm_size_t g2 = 0;   // 0x40000000 | Lock wait: lock word = owner handle | 0x40000000
            if (!vm_read_overwrite(mach_task_self(), t->waddr, 4, (vm_address_t)&w, &g2)) snprintf(own, sizeof own, " owner %#x", w & ~WAITERS_BIT); }
        if (t->wkind == 2 && t->waddr) { uint32_t w = 0, k = 0; vm_size_t g2 = 0;              // Condition wait: current values of the associated lock word and the condition key (the deadlock dump lacked this item)
            vm_read_overwrite(mach_task_self(), t->waddr, 4, (vm_address_t)&w, &g2); vm_read_overwrite(mach_task_self(), t->wkey, 4, (vm_address_t)&k, &g2);
            snprintf(own, sizeof own, " lock word %#x condvar key %#llx=%#x wait #%llu", w, t->wkey, k, (unsigned long long)t->wseq); }   // wait #: the order the waiters arrived in, which SignalProcessWideKey wakes them in at equal priority
        uint8_t raw[64]; vm_size_t g3 = 0;                                                   // Raw 64 bytes of the object being waited on (the structure holding the read-write lock / condition variable): to see the counts and the owner
        int have_raw = (t->wkind == 1 || t->wkind == 2 || t->wkind == 3) && t->waddr && !vm_read_overwrite(mach_task_self(), t->waddr & ~0xfull, 64, (vm_address_t)raw, &g3) && g3 == 64;
        char life[96]; uint64_t nowns = now_ns();
        if (t->exited) snprintf(life, sizeof life, " created at tick %u, exited at tick %u (%.1f s ago)", t->born_tick, t->exit_tick, (nowns - t->exit_ns) / 1e9); else if (!t->born_ns) snprintf(life, sizeof life, " present since process start"); else snprintf(life, sizeof life, " created at tick %u (%.1f s ago)", t->born_tick, (nowns - t->born_ns) / 1e9);
        fprintf(stderr, "[dm] thread %#x '%s' priority %d core %d mask %#llx CPU %.2fs svc %llu %s%s%s waits:%s addr=%#llx%s last svc %#x lr=%s+%#llx\n", t->handle, nm, t->prio, t->core, (unsigned long long)t->mask,
                cpu, t->nsvc, t->exited ? "exited" : "", t->started ? "" : "not started", life, kinds[t->wkind & 7], t->waddr, own, t->last_svc,
                m ? m->name : "?", m ? t->last_lr - (uint64_t)m->base : t->last_lr);
        if (have_raw) { char hx[200]; int q = 0; for (int i = 0; i < 64; i += 4) q += snprintf(hx + q, sizeof hx - q, "%08x ", *(uint32_t *)(raw + i));
                        fprintf(stderr, "[dm]   %#x waited-for object, 64 bytes from %#llx: %s\n", t->handle, t->waddr & ~0xfull, hx); }
        thr_bt(t);
    }
    // Threads created by the host itself (graphics present, audio, movie decoding, etc.): not in the guest thread table, enumerated as mach threads, the first 12 frames symbolized with dladdr
    thread_act_array_t ths; mach_msg_type_number_t nth = 0;
    if (task_threads(mach_task_self(), &ths, &nth) == KERN_SUCCESS) {
        for (mach_msg_type_number_t i = 0; i < nth; i++) {
            int guest = 0; for (thr_t *t = g_all; t; t = t->all) if (t->mport == ths[i]) { guest = 1; break; }
            if (!guest && ths[i] != mach_thread_self() && thread_suspend(ths[i]) == KERN_SUCCESS) {
                arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
                if (thread_get_state(ths[i], ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
                    char line[1200]; int p = snprintf(line, sizeof line, "[dm] host thread %#x:", ths[i]);
                    uint64_t fp = __darwin_arm_thread_state64_get_fp(st), pc = __darwin_arm_thread_state64_get_pc(st);
                    for (int k = 0; k < 12 && p < (int)sizeof line - 80; k++) {
                        Dl_info di; module_t *m = module_of((void *)pc);
                        if (m) p += snprintf(line + p, sizeof line - p, " %s+%#llx", m->name, pc - (uint64_t)m->base);
                        else if (dladdr((void *)pc, &di) && di.dli_sname) p += snprintf(line + p, sizeof line - p, " %s+%#lx", di.dli_sname, (unsigned long)(pc - (uint64_t)di.dli_saddr));
                        else p += snprintf(line + p, sizeof line - p, " %#llx", pc);
                        uint64_t fr[2]; vm_size_t g2 = 0;
                        if (!fp || vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)fr, &g2) || g2 != 16 || fr[0] <= fp) break;
                        fp = fr[0]; pc = fr[1];
                    }
                    fprintf(stderr, "%s\n", line);
                }
                thread_resume(ths[i]);
            }
            mach_port_deallocate(mach_task_self(), ths[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)ths, nth * sizeof *ths);
    }
}

// An approximation of same-core preemption: when an SVC woke a higher-priority thread pinned to this core, wait before the SVC returns until those threads (and every other higher-priority thread of this core) block, for at most 5 ms.
// Metroid Dread's movie player relies on it: the main thread posts "unlink the holders" to the dispatcher thread (priority 30, core 0 only) and leaves messages for the worker thread, then frees the object and builds a new one in its place;
// on hardware the dispatcher / worker preempt the main thread as soon as it signals and finish first, while on the host, without the wait, old holders got re-initialized and the worker read freed objects (crashes in SignalEvent on a null MultiWait and at main+0x225338).
// On per the game profile's core_preempt (off by default: Mario's loader threads would wait for busy high-priority decompression threads, pushing its first frame from 0.6 s to 2.9 s); DM_PREEMPT=0 / 1 overrides the profile. Counts are in the thread state dump
static int svc_thread_impl(int num, uint64_t *regs);
// Yield back-off: on the Switch a yield with nothing else runnable costs nothing, but a guest loop that polls the tick and yields (Three Houses' frame limiter main+0x5be430 waited out each 33 ms frame this way:
// 228,105 yields per frame on an iPhone, a P-core at full clock; a per-build cut in its patches.py now makes it sleep) becomes a host busy spin. A run of yields with no other SVC between them (and no progress, thr_yield) is let through as plain sched_yield for its first 2 ms;
// after that each yield sleeps an eighth of the run so far, between 10 and 250 µs (a 23 ms limiter wait: about 90 sleeps instead of 200k yields, ending at most 250 µs late; idle workers polling for jobs the same).
// The back-off used to start after 200 µs: that also caught Tears of the Kingdom's main thread waiting for its ModuleSystem workers (about 350 yields a frame, runs of a few hundred µs on the
// frame's critical path). A sleep that overran the moment the workers finished pushed frames past 33 ms, and the game then drops itself to present interval 3 (20 fps) for a while:
// Mac frame p95 34.5 → 41 ms with 7 drops per 300 frames; on the iPhone the threads also looked idle to the performance controller, which kept them on efficiency cores at 1.5 GHz.
// Waits under 2 ms are handoffs between threads that do work; waits beyond it are limiters and idle polls. Returns the sleep for a yield whose run started `run_ns` ago (0 = plain yield).
// DM_YIELD_BACKOFF=0 turns it off; DM_YIELD_BACKOFF=<cap µs>[:<start µs>] sets the cap and the run length after which it starts (default 250:2000)
static uint64_t yield_backoff_ns(uint64_t run_ns) {
    static int64_t cap = -1, start = 2000000;
    if (cap < 0) { const char *e = getenv("DM_YIELD_BACKOFF"); cap = 250000;
        if (e && *e) { cap = atoll(e) * 1000; const char *c = strchr(e, ':'); if (c) start = atoll(c + 1) * 1000; }
        if (!cap) LOG("DM_YIELD_BACKOFF=0: no back-off on repeated yields (plain sched_yield spin)"); else if (e && *e) LOG("DM_YIELD_BACKOFF: back off after %lld µs of repeated yields, sleeping at most %lld µs at a time", (long long)(start / 1000), (long long)(cap / 1000)); }
    if (!cap || run_ns < (uint64_t)start) return 0;
    uint64_t s = run_ns / 8; if (s < 10000) s = 10000;
    return s < (uint64_t)cap ? s : (uint64_t)cap;
}
// sched_yield, or a short sleep once the wait has lasted run_ns past the back-off's start
static void yield_for(thr_t *me, uint64_t run_ns) {
    uint64_t bo = yield_backoff_ns(run_ns);
    if (bo) { me->nbackoff++; me->sleeping = 1; struct timespec ts = {0, (long)bo}; nanosleep(&ts, 0); me->sleeping = 0; }
    else sched_yield();
}
// One yield SVC of the calling thread (me may be NULL); regs = the guest registers at the SVC.
// A run of yields is a wait only while the loop makes no progress: it also starts again when the caller's callee-saved registers x19–x28 differ from the last yield's. The SDK's
// YieldThread reaches the SVC through tail branches only (SDK 20: nn::os::YieldThread → ThreadManagerImplByHorizon::YieldThread, mov x0,#-1 → the svc stub), so they are the loop's own:
// a poll keeps its pointers and deadline there (Three Houses' limiter: x19 points at the last frame end, x23 is the target, the time passed is in x8; Galaxy 2's Hagi sleep poll keeps
// its state behind x19), while a loop that yields between steps of work moves its counters, and such a yield returns at once, with no host call, as one with no other thread of its
// priority to run does on the Switch. Animal Crossing: New Horizons' SaveProc builds a new island in grid walks that yield once per cell (main+0x157d020, cell x and y in w21 / w20):
// with the back-off each yield slept 250 µs, about 2,600 cells a second, and the screen stayed black with the loading icon for 77 s after the opening movie (now about 10 s).
// Breath of the Wild's PlacementMgr and RadarMgr yield once per item of their batches too (a list pointer and a count move; RadarMgr's batch is 94,263 items, which the back-off
// stretched to about 30 s): standing outdoors on the Mac, CPU energy 0.35–0.74 → 0.21–0.34 W at 60 fps, CPU 76–83% → 96–106% of a core, frame interval p99 19 → 20–21 ms.
// With sched_yield on each such yield (about 200,000 a second) it was 108–121% of a core and 0.38–0.70 W
static void thr_yield(thr_t *me, const uint64_t *regs) {
    if (!me) { sched_yield(); return; }
    uint64_t y0 = now_ns(), sig = 0xcbf29ce484222325ull;
    for (int i = 19; i <= 28; i++) sig = (sig ^ regs[i]) * 0x100000001b3ull;
    int moved = me->yrun_svc + 1 == me->nsvc && me->yrun_sig != sig;   // the loop moved on since its last yield
    me->nyield++; if (me->yrun_svc + 1 != me->nsvc || moved) me->yrun_t0 = y0; me->yrun_svc = me->nsvc; me->yrun_sig = sig;
    if (!moved) yield_for(me, y0 - me->yrun_t0);
    if (me->handle == 0x1001) { g_main_yield_ns += now_ns() - y0; g_main_yield_n++; }
}
// Spin waits (patch_nso.py "Spin waits"): a guest loop that polls memory with no SVC, which the rewriter has made go through a spin gate on every pass after the first. On the Switch
// such a poll costs nothing while another thread of its core has work (a higher-priority one preempts it); here it takes a host core from the threads doing the work: Paper Mario:
// The Thousand-Year Door's three job workers (main+0xa8e680) spun at 94% of a core each in Rogueport on an iPhone, which ran at 24 fps. The gate alone slows the poll down, and on the
// Mac that was the whole gain (the workers' CPU fell about tenfold, see docs/development.md): most waits are short handoffs between workers. A wait (passes with no SVC between them
// and at most SPIN_GAP_NS of guest work between two) therefore polls on for its first DM_SPIN_POLL µs (2000, the yield back-off's start), and then sleeps as a run of yields does
// (yield_backoff_ns, DM_YIELD_BACKOFF), which leaves the core to the threads doing the work. A sched_yield on every pass instead (DM_SPIN_POLL=0) raised the Mac's frame p95 by 2-5 ms,
// as a yield there hands the core to other processes. DM_SPIN=0: no sleeps either. gate = the gate's address, site = the loop head (module offset)
#define SPIN_GAP_NS 20000
void dm_spin_wait(uint64_t gate, uint64_t site) {
    static int on = -1; static uint64_t poll_ns;
    if (on < 0) { const char *e = getenv("DM_SPIN"), *p = getenv("DM_SPIN_POLL"); poll_ns = (p && *p ? strtoull(p, 0, 10) : 2000) * 1000; on = !(e && *e && atoi(e) == 0);
                  if (!on) LOG("DM_SPIN=0: spin waits do not yield or sleep"); else if (p && *p) LOG("DM_SPIN_POLL: spin waits poll for %llu µs, then yield", (unsigned long long)(poll_ns / 1000)); }
    thr_t *me = pthread_getspecific(g_keythr);
    if (!me) return;
    uint64_t now = now_ns();
    if (me->srun_svc != me->nsvc || now - me->spin_ret > SPIN_GAP_NS) me->srun_t0 = now;   // a new wait
    me->srun_svc = me->nsvc; me->nspin++;
    if (site != me->spin_site || !me->spin_mod) { const module_t *m = module_of((void *)gate); me->spin_mod = m ? m->name : "?"; me->spin_site = site; }
    if (on && now - me->srun_t0 >= poll_ns) { me->nyield++; yield_for(me, now - me->srun_t0); }
    me->spin_ret = now_ns();
}
// The threads that yielded or passed through spin waits most since the last call (the power line every 300 frames, hle_nvn.c): "handle name N (back-off sleeps M)", at most two,
// N the yields (spin wait passes that yielded among them), with the spin wait passes and the last one's site ("; spin passes K, last at main+0x…")
void dm_yield_top(char *out, size_t n) {
    thr_t *top[2] = {0}; uint64_t d[2] = {0}, b[2] = {0}, s[2] = {0}, r[2] = {0};
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) {
        uint64_t y = t->nyield - t->nyield_seen, bo = t->nbackoff - t->nbackoff_seen, sp = t->nspin - t->nspin_seen, k = y + sp;
        t->nyield_seen = t->nyield; t->nbackoff_seen = t->nbackoff; t->nspin_seen = t->nspin;
        if (k > r[0]) { top[1] = top[0]; d[1] = d[0]; b[1] = b[0]; s[1] = s[0]; r[1] = r[0]; top[0] = t; d[0] = y; b[0] = bo; s[0] = sp; r[0] = k; }
        else if (k > r[1]) { top[1] = t; d[1] = y; b[1] = bo; s[1] = sp; r[1] = k; }
    }
    size_t o = 0; char nb[32];
    for (int i = 0; i < 2 && top[i] && o < n; i++) {
        o += snprintf(out + o, n - o, "%s%#x '%s' %llu times (back-off sleeps %llu", i ? ", " : "", top[i]->handle, thr_name(top[i], nb, sizeof nb), (unsigned long long)d[i], (unsigned long long)b[i]);
        if (o < n && s[i]) o += snprintf(out + o, n - o, "; spin passes %llu, last at %s+%#llx", (unsigned long long)s[i], top[i]->spin_mod, (unsigned long long)top[i]->spin_site);
        if (o < n) o += snprintf(out + o, n - o, ")");
    }
    pthread_mutex_unlock(&G);
    if (!o && n) snprintf(out, n, "none");
}
// CPU time of the guest threads since the last call, by Switch priority band (<= 44: the main thread's level and above; 45-59; >= 60) and the three busiest threads with their priority.
// For the power line (hle_nvn.c power_line): it shows which threads a priority-to-QoS mapping would move and how much CPU they use. secs = the window length
void dm_thread_cpu(char *out, size_t n, double secs) {
    thr_t *top[3] = {0}; double d[3] = {0}, band[3] = {0};
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) {
        if (!t->mport || t->exited) continue;
        thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
        if (thread_info(t->mport, THREAD_BASIC_INFO, (thread_info_t)&bi, &c) != KERN_SUCCESS) continue;
        uint64_t us = (uint64_t)(bi.user_time.seconds + bi.system_time.seconds) * 1000000u + (uint64_t)(bi.user_time.microseconds + bi.system_time.microseconds);
        double ms = t->cpu_seen_us && us > t->cpu_seen_us ? (us - t->cpu_seen_us) / 1e3 / secs : 0; t->cpu_seen_us = us;   // ms per second; a thread's first report only takes its baseline
        band[t->prio <= 44 ? 0 : t->prio < 60 ? 1 : 2] += ms;
        for (int i = 0; i < 3; i++) if (ms > d[i]) { for (int j = 2; j > i; j--) { top[j] = top[j - 1]; d[j] = d[j - 1]; } top[i] = t; d[i] = ms; break; }
    }
    size_t o = (size_t)snprintf(out, n, "by guest priority ≤44 %.0f / 45–59 %.0f / ≥60 %.0f ms/s; top: ", band[0], band[1], band[2]); char nb[32];
    for (int i = 0; i < 3 && top[i] && o < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%s%#x '%s' priority %d %.0f ms/s%s", i ? ", " : "", top[i]->handle, thr_name(top[i], nb, sizeof nb), top[i]->prio, d[i], top[i]->host_owned ? " (host-created)" : "");
    if (!top[0] && o < n) snprintf(out + o, n - o, "none");
    pthread_mutex_unlock(&G);
}
// Sleep until dl (host monotonic ns) on time: macOS nanosleep / mach_wait_until carry timer slack; measured, 33 ms sleeps take about 36.4 ms, 5 ms take 6.1 ms, 1 ms take 1.26 ms (the same at any QoS).
// Metroid Dread's sound thread sleeps "33 ms minus this round's work" (main+0xf95b0), and on the host its period became 36.9 ms; cutscenes render a frame whenever the audio play position that thread updates moves, so they ran at about 27 fps (30 on the Switch).
// Each step sleeps only 3/4 of the remaining time (the slack is at most about 25%, so it never oversleeps), looping until within 20 µs: a 33 ms sleep takes about 4 system calls with an error of tens of microseconds, and nothing spins
static void sleep_precise(uint64_t dl) {
    for (int i = 0; i < 16; i++) {
        uint64_t now = now_ns(); if (now + 20000 >= dl) return;
        uint64_t rem = dl - now, s = rem < 100000 ? rem : rem - rem / 4;
        struct timespec ts = {(time_t)(s / 1000000000u), (long)(s % 1000000000u)}; nanosleep(&ts, 0);
    }
}
// A guest thread sleeps ns nanoseconds the way SleepThread does: on the guest clock under the deterministic clock, else on time (sleep_precise). Also for host hooks that stand for a
// call which blocks on hardware (hle_net.c: Poll / Select with nothing to wait on)
void dm_guest_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    if (dm_detclock_enabled()) {
        uint64_t deadline = guest_ns() + (uint64_t)ns;
        while (guest_ns() < deadline) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
        return;
    }
    thr_t *me = pthread_getspecific(g_keythr); if (me) me->sleeping = 1;
    sleep_precise(now_ns() + (uint64_t)ns);
    if (me) me->sleeping = 0;
}
int svc_thread(int num, uint64_t *regs) {
    int r = svc_thread_impl(num, regs);
    thr_t *me = pthread_getspecific(g_keythr);
    static int on = -1; if (on < 0) { const char *e = getenv("DM_PREEMPT"); on = e ? atoi(e) != 0 : DM_GAME_CORE_PREEMPT; }
    // Not only "whom did this SVC wake": when a higher-priority thread of the same core is already running (busy, not waiting, at the time of the signal), on hardware the calling thread does not get the CPU at all.
    // Metroid Dread: with the dispatcher thread busy, the main thread's "unlink the holders" request woke nobody, the object was freed right away, and the dispatcher later walked the freed holders in WaitAny and crashed
    if (me && on) {
        uint64_t t0 = now_ns(); int w = dm_yield_to_core_peers(5000);
        if (w) { if (w < 0) g_preempt_cap++; g_preempt_n++; g_preempt_ns += now_ns() - t0; }
    }
    if (me) me->yield_pending = 0;
    return r;
}
static int svc_thread_impl(int num, uint64_t *regs) {
    { thr_t *me = pthread_getspecific(g_keythr); if (me) { me->last_svc = num; me->last_lr = regs[30]; me->nsvc++; } }
    if (g_flag.thr && num != 0x0B) { thr_t *me = pthread_getspecific(g_keythr); LOG("[%#x] svc %#x x0=%#llx x1=%#llx x2=%#llx x3=%#llx", me ? me->handle : 0, num, regs[0], regs[1], regs[2], regs[3]); }
    switch (num) {
    case 0x08: {  // CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        thr_t *t = calloc(1, sizeof *t);
        pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        // Core -2 (IdealCoreUseProcessValue) = the process's default ideal core (0 for this game), with an affinity mask of just that core - as in the Horizon kernel. It used to be recorded as -2 and GetCurrentProcessorNumber answered -2;
        //   SMBW's font engine picks its per-core engine slot by the current core number and crashed indexing with -2 (main +0x107dbc0, SequenceBackgroundWorker thread)
        t->prio = (int)regs[4]; t->core = (int)regs[5] >= 0 ? (int)regs[5] : 0; t->mask = 1ull << t->core;
        t->born_ns = now_ns(); t->born_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
        pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // Host resources are reclaimed on exit (the guest waits through the handle, no join)
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        if (g_flag.thr) LOG("CreateThread h=%#x entry=%#llx arg=%#llx sp=%#llx prio=%d core=%d", t->handle, t->entry, t->arg, t->stack_top, t->prio, t->core);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: thread_exit(cur()); return 1;
    case 0x0B: {  // SleepThread(x0 ns); 0/-1/-2 = the various kinds of yield
        int64_t ns = (int64_t)regs[0];
        if ((ns > 0 && dm_detclock_enabled()) || ns >= 20000) dm_guest_sleep_ns(ns);
        // Yield (0 / -1 / -2), and sleeps shorter than 20 µs, which sleep_precise would return from at once: Super Mario Galaxy's Hagi polls with SleepThread(100 ns) (main+0x517c),
        // which kept a core at 100% (1.4 billion calls in 200 s); as yields they get the back-off below.
        // Count and time of the main thread's yields are recorded: a render task graph's "wait for everything to finish" is often pure polling + yield; measure how much of a frame it takes
        else thr_yield(pthread_getspecific(g_keythr), regs);
        return 1;
    }
    case 0x0C: { thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; return 1; }
    case 0x0D: { thr_t *t = h_get((uint32_t)regs[0], O_THREAD);   // SetThreadPriority; under DM_QOS_MAP the class follows (now on the calling thread, else at the target's next SVC)
        if (t) { t->prio = (int)regs[1]; if (g_qos_map) { if (t == cur()) qos_apply(t); else atomic_store(&t->qos_pending, 1); } }
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1; }
    case 0x0E: { thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0; return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 ideal core, x2 mask): -2 = use the process default (ideal core 0, mask of just that core), -1 = don't care, -3 = leave the ideal core unchanged (as in the Horizon kernel); the host only keeps the bookkeeping
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; if (c == -2) { t->core = 0; t->mask = 1; } else if (regs[2]) t->mask = regs[2];
            // The ideal core is not in the new mask (-2/-3 keep the ideal core but pin the thread to other cores): as in Horizon, move the ideal core to the lowest core in the mask.
            // Otherwise GetCurrentProcessorNumber reports core 0 even for threads pinned to cores 1 / 2, and a job system with per-core queues (SMBW's BaseProcExecutor) takes core 0's work while nobody drains the queues of cores 1 / 2
            if (t->mask && (t->core < 0 || !(t->mask >> t->core & 1))) t->core = __builtin_ctzll(t->mask); }
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x10: { thr_t *t = cur(); int c = t->core;   // The current core number is always 0..2 (never negative); a thread with no ideal core reports the lowest core its mask allows, not core 0 outside it
                 regs[0] = (uint64_t)(uint32_t)(c >= 0 ? c : t->mask ? __builtin_ctzll(t->mask) : 0); return 1; }
    case 0x32: {  // SetThreadActivity(w0 thread, w1 0 runnable / 1 paused): see "Pausing threads" above
        thr_t *me = cur(); int act = (int)regs[1];
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD); int64_t r = 0;
        if (!t) r = R_BAD_HANDLE;
        else if (t == me) r = R_BAD_STATE;
        else if (act == 1) {
            atomic_store(&t->pause_req, 1);
            int st = atomic_fetch_or(&t->svc_state, 2);
            if (!t->started || t->exited || (st & 1)) t->paused = 1;
            uint64_t t0 = now_ns(); int warned = 0;
            while (!t->paused && !t->exited) {
                pthread_mutex_unlock(&G); pthread_kill(t->pt, SIGUSR2); pthread_mutex_lock(&G);
                if (t->paused || t->exited) break;
                struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 100000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&t->pcv, &G, &dl);
                if (!warned && now_ns() - t0 > 1000000000ull) { warned = 1; LOG("[RED] thread %#x still not suspended after more than 1 second (stuck in host code?)", t->handle); }
            }
        } else { atomic_store(&t->pause_req, 0); atomic_fetch_and(&t->svc_state, ~2); pthread_cond_broadcast(&t->pcv); }
        pthread_mutex_unlock(&G);
        { static int n; if (n++ < 4) LOG("SetThreadActivity %#x → %s (%s)", (uint32_t)regs[0], act ? "pause" : "resume", r ? "failed" : "ok"); }
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x33: {  // GetThreadContext3(x0 out ThreadContext*, w1 thread): only for stopped threads
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[1], O_THREAD); int64_t r = 0;
        if (!t) r = R_BAD_HANDLE;
        else if (!t->paused) r = R_BAD_STATE;
        else ctx_fill(t, (hz_ctx_t *)regs[0]);
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x25: { thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0; return 1; }

    case 0x1A: {  // ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        pthread_mutex_lock(&G);
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2];
            // Under DM_QOS_MAP the owner runs at least at this thread's class until the lock is handed over (the Horizon kernel lends the waiter's priority the same way): a user-interactive
            // waiter is never held up by a utility-class owner on a slow core
            thr_t *o = g_qos_map ? h_peek((uint32_t)regs[0], O_THREAD) : NULL;
            pthread_override_t ov = o && o->pt && !o->exited && t->qos > o->qos ? pthread_override_qos_class_start_np(o->pt, (qos_class_t)t->qos, 0) : NULL;
            r = block(t, 1, -1);
            if (ov) pthread_override_qos_class_end_np(ov); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: pthread_mutex_lock(&G); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        pthread_mutex_lock(&G);
        // The order matters: first set the condition key = 1, then release the mutex. The SDK's InternalConditionVariable::Signal first reads the condition key in user mode, and does not issue svc 0x1D if it is 0;
        // grabbing the mutex in user mode does not go through G either. It used to do lock_handoff first and set the key after: after the release and before the key was set, another thread could grab the lock in user mode, change the condition, read key 0 and not wake anyone,
        // and this thread would then link itself in and wait forever (under parallel load the resource table read-write lock main+0x320e5e0 deadlocked: reader count 0, lock free, writer condition key 1, writer still waiting).
        // Linking into the wait chain happens inside block, still holding G: svc 0x1D has to take G, so it cannot land before this
#ifdef DM_TEST_HOOKS
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }   // Unit test: right after releasing the lock, before linking into the wait chain
#else
#define CV_TEST_HOOK()
#endif
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr); CV_TEST_HOOK();
#else   // Negative control for the unit test t_condvar only: restores the pre-fix order "release the lock first, set the key after"
        lock_handoff(addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
#endif
        int64_t r = block(t, 2, (int64_t)regs[3]);   // Timeout: the lock is not re-acquired (user mode takes it itself)
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        pthread_mutex_lock(&G);
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // In kernel order; once turned into a lock wait or woken it is no longer kind 2
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 address, w1 type, x2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal, 3 wait if the 64-bit value is equal
        // (Horizon 18; SDK 19's nn::os::WaitForAddress(const long*, long) uses it, which before fell into the 32-bit less-than test)
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        pthread_mutex_lock(&G);
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 3 ? ld64(addr) == regs[2] : type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, (int64_t)regs[3]); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify the value by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        pthread_mutex_lock(&G);
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                // Waking all (count <= 0) with waiters subtracts 2, not 1 (mesosphere KAddressArbiter::SignalAndModifyByWaitingCountIfEqual; Ryujinx and yuzu agree).
                // It used to subtract 1: Unity's (Engage) job fences decide from this value whether jobs are done, waiters were let through early, and the particle geometry job disagreed with the main thread's counts and wrote out of bounds (indices into texture objects, writes to a null pointer).
                if (cnt <= 0) nv = nw > 0 ? val - 2 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0;
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x45: {  // CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        pthread_mutex_lock(&G);
        // SignalEvent accepts only the write end; ClearEvent accepts both ends; ResetSignal accepts only the read end
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("event SVC %#x invalid handle %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(e); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        int64_t deadline = ns > 0 ? (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns()) + ns : 0;
        pthread_mutex_lock(&G);
        int64_t r; int idx = -1;
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns());
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            t->whs = hs; t->whn = n; int64_t br = block(t, 4, left); t->whs = NULL; t->whn = 0;
            if (br == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 thread)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {  // CloseHandle(w0): an event is freed only when both ends are closed; after a thread handle is closed the slot is recycled, and the object is freed once the thread has also exited
        pthread_mutex_lock(&G);
        unsigned i = (uint32_t)regs[0] - 0x1000;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            if (g_h[i].type == O_EVENT_W) se_unbind((uint32_t)regs[0]);
            event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        }
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }
    }
    return 0;
}

// ---------- SystemEvent of the service layer ----------
// Real device: the service hands the read end of a kernel event to the client, and the guest waits on it through SDK nn::os. Host: on a guest thread, call the SDK's own
// CreateSystemEvent(inter-process=true), and the SDK creates the kernel event through SVC 0x45; the write-end handle is recorded, after which any thread (including the host audio thread) can signal it.
// The binding table (g_se) sits with the handle table at the top: CloseHandle unbinds through it
void hle_bind_system_event(void *ev, int autoclear) {      // Must be called on a guest thread (it has to execute SDK code)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) return;
    if (!pthread_getspecific(g_keythr)) die("hle_bind_system_event must be called on a guest thread");
    static uint32_t (*create)(void *, int, int); static uint32_t (*getw)(const void *);
    if (!create) { create = guest_sym("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
                   getw = guest_sym("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
    uint32_t r = create(ev, autoclear, 1);
    if (r) die("CreateSystemEvent failed %#x", r);
    wh = getw(ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent binding table full");
    g_se[g_nse].ev = ev; g_se[g_nse++].wh = wh;
    pthread_mutex_unlock(&g_se_lock);
}
void hle_signal_system_event(void *ev) {   // An unbound one is bound on the spot as manual-clear (can only happen on a guest thread)
    // Lookup and signal under one hold of G: CloseHandle (which unbinds) also runs under G, so the handle cannot be closed and its number reused in between
    for (int bound = 0; ; bound = 1) {
        pthread_mutex_lock(&G);
        pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
        if (wh) {
            event_t *e = h_get(wh, O_EVENT_W);
            if (!e) die("SystemEvent write-end handle invalid %#x", wh);
            e->signaled = 1; wake_sync_waiters(e);
            pthread_mutex_unlock(&G);
            return;
        }
        pthread_mutex_unlock(&G);
        if (bound) die("SystemEvent %p still not found after binding", ev);
        hle_bind_system_event(ev, 0);   // guest code (SVCs take G): outside the lock
    }
}
// Signal only a bound one, from any thread; 0 when it is not bound (never bound, or the game destroyed it). For state changes the front end reports (hle_hid.c: a controller
// connected or removed), where binding on the spot is impossible off a guest thread and a destroyed event must not be bound again
int hle_signal_bound_system_event(void *ev) {
    pthread_mutex_lock(&G);
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    event_t *e = wh ? h_get(wh, O_EVENT_W) : NULL;
    if (e) { e->signaled = 1; wake_sync_waiters(e); }
    pthread_mutex_unlock(&G);
    return e != NULL;
}
void hle_clear_system_event(void *ev) {   // Clear a bound service-layer SystemEvent (any thread), as the service does when its condition ends; an unbound one has nothing to clear
    pthread_mutex_lock(&G);
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) {
        event_t *e = h_get(wh, O_EVENT_W);
        if (!e) die("SystemEvent write-end handle invalid %#x", wh);
        e->signaled = 0;
    }
    pthread_mutex_unlock(&G);
}
