/* Adapted from pinned D2R Mobile host/hle_thread.c, MIT.
 * Game-specific diagnostics and optional timing accounting removed.
 * Core affinity is virtual bookkeeping; Darwin schedules pthreads. */
// Horizon 内核的线程与同步 SVC，原生实现：每个客体线程 = 一个宿主 pthread，跑在 SDK 给的客体栈上。
// SDK 的 nn::os 原样运行，只把它下面的「内核」换成宿主：
//   线程   0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D 优先级 0x0E/0x0F 核掩码
//          0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   锁     0x1A ArbitrateLock 0x1B ArbitrateUnlock（标签字 = 持有者句柄 | 0x40000000 有等待者）
//   条件   0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   地址   0x34 WaitForAddress 0x35 SignalToAddress
//   事件   0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// 语义按公开 Horizon 内核行为（Atmosphère mesosphère 的 KConditionVariable / KAddressArbiter / KSynchronization）。
// 所有等待挂在一把全局锁 G 上，每线程一个条件变量；先求正确，性能后量。
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
#include "thread_compat.h"
#include "dm_wait_stats.h"
#include "ipc.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <mach/mach.h>
#include <pthread/qos.h>
#include <dlfcn.h>
#include <TargetConditionals.h>

#define R_TIMEOUT     0xEA01u   // 内核模块 1，描述 117 | Kernel module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_OUT_OF_RES  0xCE01u   // 103
#ifdef GAME_BOTW
#define R_BAD_CORE    0xE201u   // 113 InvalidCoreId
#define R_BAD_COMBO   0xE801u   // 116 InvalidCombination
/* Core ids as Horizon reads them (Atmosphere svc::CreateThread / SetThreadCoreMask, KThread::SetCoreMask): -1 = no ideal
   core (DontCare), -2 = the process's ideal core with only that core in the mask (UseProcessValue), -3 = keep the ideal
   core (NoUpdate). A new thread's mask is its ideal core alone; the kernel starts the main thread the same way on the
   process's default core (main.npdm META +0xf: core 0 for Breath of the Wild, cores 0-2 allowed). botw.md: with mask 0x7
   for core -2 (and for the main thread), threads the Switch keeps on core 0 with the main thread ran beside it on cores 1-2. */
#define PROCESS_IDEAL_CORE 0
#define PROCESS_CORE_MASK  7u
#endif
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_SESSION, O_TMEM };   // O_SESSION / O_TMEM: IPC objects owned by ipc.c
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; uint64_t host_context[2]; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns; uint32_t born_tick, exit_tick;   // 建立 / 退出的时刻与当时的游戏拍（现场导出用：作业工人是不是在卡住前退的） | Time of creation / exit and the game tick at that time (for the state dump: did the job worker exit before the stall)
    int started, exited, cancel;
    pthread_t pt; pthread_cond_t cv;
    // 当前等待：kind 0=无 1=锁 2=条件 3=地址 4=同步对象
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    uint64_t wseq;             // 登记等待的先后序号：同优先级先到先醒（与 Horizon 内核 KThread 等待树同序） | Sequence number of wait registration: at equal priority first come, first woken (same order as the Horizon kernel's KThread wait tree)
    struct thr *next;          // 等待链 | Wait chain
    struct thr *all;           // 全部线程链（现场导出用） | All-threads chain (for the state dump)
    int last_svc; uint64_t last_lr, last_fp;
    int closed;                // 句柄已关：与 exited 同时成立才释放对象（Horizon：线程对象随最后一个引用消失） | Handle closed: the object is freed only when this holds together with exited (Horizon: the thread object goes away with the last reference)
    uint64_t nsvc;             // SVC 次数（现场导出用） | SVC count (for the state dump)
    mach_port_t mport;         // 宿主线程端口（取 CPU 时间） | Host thread port (to get CPU time)
    uint32_t qos;
    int cur_core;              // virtual core held while running guest code, -1 while blocked / not started
    int preempt;               // a higher-priority thread waits for this thread's core
    uint64_t cseq, core_since; struct thr *cnext;
    uint32_t wh[4]; int nwh;   // handles of the current WaitSynchronization (state dump)
    int last_hle;              // 1 + wrapper index of the last host call made while "hletrace" was on (state dump)
    uint64_t yrun_ns, yrun_svc;   // the current run of yields: when it started, and nsvc at its last yield (yield_backoff_ns)
    uint64_t core_wait_ns, core_held_ns, core_nwait, core_stats_since;   // time in core_take waiting for a virtual core, time holding one, waits (core_table)
    uint64_t core_held_prev, core_wait_prev, core_nwait_prev, cpu_prev_ns;   // the same, and the host thread's CPU time, at the last table
    volatile uintptr_t *hle_slot;   // TOTK: this thread's host-stack TSD slot (0 while it runs a host function), read by core waiters (totk_kick)
    volatile uint16_t *tlr16;       // TOTK: its ThreadLocalRegion (+0x100 disable_count, +0x102 interrupt_flag)
    uint64_t ap_sig_ns;             // TOTK: when a core waiter last signalled it
#ifdef GAME_BOTW
    uint64_t cwait_ns;            // when it last queued for a virtual core (starvation rule, core_beats)
    uint64_t protect_until;       // given a core by the starvation rule: no priority preemption before this
    int cqueued;                  // in g_core_wait: queued by core_take, or by wake_queue the moment it was woken
#endif
} thr_t;
static thr_t *g_all;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
static pthread_key_t g_keythr;
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          // 全部在等的线程（锁 / 条件 / 地址 / 同步） | All waiting threads (lock / condition / address / synchronization)

#ifdef GAME_BOTW
/* Handles as Horizon encodes them (Atmosphere KHandleTable): the slot in bits 0-14 and, in bits 15-29, a linear id that
   changes at every allocation, so a handle stays invalid after it is closed even when its slot is reused. Plain slot
   numbers (0x1000 + slot) let a stale handle -- a signal to or a wait on an event the game had already closed, a thread
   handle kept after the thread exited -- reach whatever object took the slot next (botw.md: Breath of the Wild reused
   thread handle 0x1029 within a minute). Bit 30 stays clear: mutex tags use it as the waiters bit. */
static uint16_t g_hlin[MAX_H];
static uint16_t g_next_lin = 1;
static uint32_t g_main_handle;
static uint32_t h_new(int type, void *obj) {     // caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) {
        uint16_t lin = g_next_lin++; if (g_next_lin > 0x7fff) g_next_lin = 1;
        g_h[i].type = type; g_h[i].obj = obj; g_hlin[i] = lin;
        return ((uint32_t)lin << 15) | (uint32_t)i;
    }
    die("句柄表满");
}
static uint64_t g_stale_handles;
static unsigned h_index(uint32_t h) {            // slot of a live handle, 0 when the value names none
    unsigned i = h & 0x7fff, lin = (h >> 15) & 0x7fff;
    if ((h >> 30) || i == 0 || i >= MAX_H || !g_h[i].type) return 0;
    if (g_hlin[i] != lin) {   // a closed handle whose slot now holds another object: the old encoding handed that object over
        if (g_stale_handles++ < 40) { thr_t *me = g_keythr ? pthread_getspecific(g_keythr) : NULL;
            fprintf(stderr, "[threads] stale handle %#x used by thread %#x: its slot now holds handle %#x (type %d)\n", h, me ? me->handle : 0, ((uint32_t)g_hlin[i] << 15) | i, g_h[i].type); }
        return 0;
    }
    return i;
}
#else
static uint32_t h_new(int type, void *obj) {     // 调用方持 G | Caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("句柄表满");
}
#endif
static void *h_peek(uint32_t h, int type) {      // 只查不报 | Lookup only, no report
    if (h == H_CUR_THREAD && type == O_THREAD) return (g_keythr ? pthread_getspecific(g_keythr) : NULL);
#ifdef GAME_BOTW
    unsigned i = h_index(h);
#else
    unsigned i = h - 0x1000;
#endif
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // 查不到即报（句柄错是真错误） | Reports if not found (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("句柄无效 %#x（要类型 %d）", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = (g_keythr ? pthread_getspecific(g_keythr) : NULL);
    if (!t) die("非客体线程调用了线程 SVC");
    return t;
}

// ---------- 等待原语（持 G 调用）----------
// ---------- Wait primitives (called holding G) ----------
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// 按内核顺序挑等待者：优先级数值小者优先，同优先级先登记者优先（持 G）。kind 1 按锁地址、2 按条件键、3 按地址匹配
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
#ifdef GAME_BOTW
static void wake_queue(thr_t *t);   /* below, with the virtual cores */
#endif
static void wake(thr_t *t, int64_t res) { wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; pthread_cond_signal(&t->cv);
#ifdef GAME_BOTW
    wake_queue(t);
#endif
}
// 挂起直到被唤醒或超时（ns<0 无限、0 立即超时）；返回 wres 或 R_TIMEOUT
// Suspend until woken or timed out (ns<0 infinite, 0 immediate timeout); returns wres or R_TIMEOUT
static int64_t block_inner(thr_t *t, int kind, int64_t ns);
// ---------- Virtual cores ----------
// Horizon runs at most one thread per core, and TOTK keys per-core data (allocators, job queues) by
// GetCurrentCoreNumber without locks. Guest threads therefore hold one of the three application cores while they
// run guest code; a core is given up only where Horizon would switch threads (waits, sleep, yield, exit, blocking
// host services) and handed to the best waiting thread (lowest priority value, then first come). A thread that a
// higher-priority waiter wants to preempt gives its core up at its next SVC. TOTK_FREE_CORES=1 disables this.
#define NCORE 3
static thr_t *g_core_owner[NCORE];
static thr_t *g_core_wait;
static uint64_t g_core_wait_all_ns, g_core_wait_all_n;   // all guest threads' waits in core_take (holding G; dm_core_wait_stats)
static int g_cores_on = -1;
static int cores_enabled(void) { if (g_cores_on < 0) g_cores_on = getenv("TOTK_FREE_CORES") ? 0 : 1; return g_cores_on; }
static int core_allowed(const thr_t *t, int c) { return (int)((t->mask >> c) & 1); }
#ifdef GAME_BOTW
/* Priority inheritance (Breath of the Wild, botw.md): a thread that owns a guest mutex runs, for the virtual cores, at
   the priority of the best thread blocked on that mutex, as Horizon raises a mutex owner. The owner is read from the
   mutex word, so it follows hand-offs. TOTK_PI_STEP=<n> (diagnostics) makes it n steps better than that waiter, and
   TOTK_STARVE=1 turns on the starvation rule below: both were tried against a hang at the Shrine of Resurrection
   (2026-10-19: the main thread, priority 44, blocked on a mutex handed to worker 0x1026, priority 45, which only runs on
   core 1, while worker 0x1009, 44, spun on core 1 waiting for the main thread), which came from the game's spin locks
   reading stale state on Apple cores (game_patches_botw.c, exclusive loads with acquire); with that fixed the game runs
   on Horizon's rules (session hz1). Holding G. */
static int g_pi_step = -1;      /* default 0: Horizon's inheritance */
static int g_starve_on = -1;    /* default off: Horizon has no starvation rule */
static void sched_knobs(void) {
    if (g_pi_step < 0) { const char *e = getenv("TOTK_PI_STEP"); g_pi_step = e ? atoi(e) : 0; }
    if (g_starve_on < 0) { const char *e = getenv("TOTK_STARVE"); g_starve_on = e && *e && strcmp(e, "0"); }
}
static int eff_prio(const thr_t *o) {
    sched_knobs();
    int p = o->prio;
    for (const thr_t *w = g_wait; w; w = w->next)
        if (w->wkind == 1 && w->waddr && (__atomic_load_n((const uint32_t *)(uintptr_t)w->waddr, __ATOMIC_RELAXED) & ~WAITERS_BIT) == o->handle && w->prio - g_pi_step < p) p = w->prio - g_pi_step;
    return p;
}
/* Starvation (TOTK_STARVE=1, diagnostics): a thread that waited STARVE_NS for a core whose owner held it STARVE_NS may
   take it, and then keeps it for at least PROTECT_NS. Horizon has no such rule (only priority 59 rotates), and a lower
   priority thread taking a core breaks per-core data the game keys by core (botw.md B3). Logged when it fires. */
#define STARVE_NS 100000000ull
#define PROTECT_NS 2000000ull
static uint64_t g_starve_grants;
static int starved(const thr_t *w, uint64_t now) { sched_knobs(); return g_starve_on && w->cwait_ns && now - w->cwait_ns >= STARVE_NS; }
#else
/* Horizon's priority inheritance for Tears of the Kingdom, as BotW's eff_prio above: a thread that owns a guest mutex
   counts, for the virtual cores, at the priority of the best thread blocked on that mutex (the owner is read from the
   mutex word, so it follows hand-offs). Used only with the asynchronous preemption (TOTK_ASYNC_PREEMPT, below): without
   it that preemption stopped the game at its first "Continue" (a better thread took the core from the low-priority
   owner of a mutex it then waited for). With the preemption off the scheduling is exactly as before. Holding G. */
static int eff_prio(const thr_t *o) {
    int p = o->prio;
    for (const thr_t *w = g_wait; w; w = w->next)
        if (w->wkind == 1 && w->waddr && (__atomic_load_n((const uint32_t *)(uintptr_t)w->waddr, __ATOMIC_RELAXED) & ~WAITERS_BIT) == o->handle && w->prio < p) p = w->prio;
    return p;
}
static int totk_ap_on(void);
#endif
/* Complete waits at the grant, including BotW threads queued by wake_queue before core_take.
   Holding G; core_since is already sampled by the scheduler, so disabled collection adds no clocks. */
static void core_wait_complete(thr_t *t, uint64_t granted_ns) {
    if (!t->core_stats_since) return;
    uint64_t elapsed = granted_ns - t->core_stats_since;
    t->core_stats_since = 0;
    t->core_wait_ns += elapsed; t->core_nwait++; g_core_wait_all_ns += elapsed; g_core_wait_all_n++;
}
static void core_grant(int c) {   // holding G: give free core c to the best waiter
    thr_t *b = NULL, **bp = NULL;
#ifdef GAME_BOTW
    uint64_t now = now_ns(); int bpri = 0, bst = 0;   // starved waiters first, then by effective priority, then first come
    for (thr_t **p = &g_core_wait; *p; p = &(*p)->cnext) {
        if (!core_allowed(*p, c)) continue;
        int st = starved(*p, now), pr = eff_prio(*p);
        if (!b || st > bst || (st == bst && (pr < bpri || (pr == bpri && (*p)->cseq < b->cseq)))) { b = *p; bp = p; bpri = pr; bst = st; }
    }
    if (b && bst) { b->protect_until = now + PROTECT_NS; if (g_starve_grants++ < 20) fprintf(stderr, "[threads] core %d to thread %#x (priority %d) after %.1f ms waiting: starvation rule\n", c, b->handle, b->prio, (now - b->cwait_ns) / 1e6); }
    if (b) b->cqueued = 0;
#else
    if (totk_ap_on()) {   // by effective priority (inheritance), then first come
        int bpri = 0;
        for (thr_t **p = &g_core_wait; *p; p = &(*p)->cnext) {
            if (!core_allowed(*p, c)) continue;
            int pr = eff_prio(*p);
            if (!b || pr < bpri || (pr == bpri && (*p)->cseq < b->cseq)) { b = *p; bp = p; bpri = pr; }
        }
    } else
    for (thr_t **p = &g_core_wait; *p; p = &(*p)->cnext)
        if (core_allowed(*p, c) && (!b || (*p)->prio < b->prio || ((*p)->prio == b->prio && (*p)->cseq < b->cseq))) { b = *p; bp = p; }
#endif
    if (!b) return;
    *bp = b->cnext; b->cnext = NULL; b->cur_core = c; b->core_since = now_ns(); core_wait_complete(b, b->core_since); g_core_owner[c] = b; pthread_cond_signal(&b->cv);
}
static void core_put(thr_t *t) {   // holding G
    if (t->cur_core < 0) return;
    if (dm_core_diag_enabled() && t->core_since) t->core_held_ns += now_ns() - t->core_since;   // core_table
    int c = t->cur_core; t->cur_core = -1; t->preempt = 0; g_core_owner[c] = NULL; core_grant(c);
}
#ifdef GAME_BOTW
/* Asynchronous preemption (Breath of the Wild, botw.md). Horizon switches a core to a better thread the moment that thread
   becomes ready, wherever the running one is. BotW's job workers spin on user-mode spinlocks without any SVC, so preempting
   only at SVC entry let spinning workers keep cores that better threads waited for, forever (2026-10-19: workers 0x1009 /
   0x100a at priority 44 spun at main+0x12c0e4c for minutes while threads at 30 / 32 / 43 waited for cores 1 and 2).
   A waiter therefore signals (SIGUSR2) each owner it may preempt, again every millisecond while it waits. The owner gives
   its core up in the handler only when it was interrupted in guest code at the top level: pc in a module's original code
   (not a rewritten window or a gate in the patch area) and its host-stack slot set (not inside a host function, nor in
   guest code that a host function called). It then holds no host lock and is not in libc, so waiting in the handler is
   like a kernel switch. Otherwise it keeps the core until its next SVC or the next signal. Who may preempt is Horizon's
   rule: a strictly better priority, or at priority 59 (Horizon rotates that priority on cores 0-2 every 10 ms) a waiter
   once the owner has held the core 10 ms. TOTK_ASYNC_PREEMPT=0 leaves only the preemption at SVC entry. */
#include <string.h>
#include <sys/ucontext.h>
static int g_async_preempt = -1;
static uint64_t g_preempt_kicks, g_preempt_async;   // signals sent / cores given up in the handler (state dump)
static int core_beats(const thr_t *w, const thr_t *o, uint64_t now) {   // holding G: may waiter w take running o's core?
    if (now < o->protect_until) return 0;
    int wp = eff_prio(w), op = eff_prio(o);
    return wp < op || (wp == op && op == 59 && now - o->core_since >= 10000000ull)
        || (starved(w, now) && now - o->core_since >= STARVE_NS);
}
static int core_should_yield(const thr_t *t) {   // holding G: does a waiter that may run on t's core beat t?
    uint64_t now = now_ns();
    for (const thr_t *w = g_core_wait; w; w = w->cnext) if (core_allowed(w, t->cur_core) && core_beats(w, t, now)) return 1;
    return 0;
}
static void preempt_signal(int sig, siginfo_t *si, void *context);
static int async_preempt_on(void) {   // holding G
    if (g_async_preempt < 0) {
        const char *e = getenv("TOTK_ASYNC_PREEMPT"); g_async_preempt = !(e && !strcmp(e, "0"));
        if (g_async_preempt) {
            struct sigaction a = {0}; a.sa_sigaction = preempt_signal; a.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
            sigemptyset(&a.sa_mask);
            if (sigaction(SIGUSR2, &a, NULL)) g_async_preempt = 0;
        }
        fprintf(stderr, "[threads] asynchronous preemption of virtual cores: %s\n", g_async_preempt ? "on" : "off (TOTK_ASYNC_PREEMPT=0)");
    }
    return g_async_preempt;
}
static int core_kick(thr_t *t) {   // holding G, t waiting: mark (and signal) the owners t may preempt; returns how many
    int n = 0; uint64_t now = now_ns();
    for (int i = 0; i < NCORE; i++) {
        thr_t *o = g_core_owner[i];
        if (!o || !core_allowed(t, i) || !core_beats(t, o, now)) continue;
        __atomic_store_n(&o->preempt, 1, __ATOMIC_RELEASE); n++;
        if (async_preempt_on() && !pthread_kill(o->pt, SIGUSR2)) g_preempt_kicks++;
    }
    return n;
}
static void core_unqueue(thr_t *t) {   // holding G: out of the core wait queue, if it is in it
    if (!t->cqueued) return;
    for (thr_t **p = &g_core_wait; *p; p = &(*p)->cnext) if (*p == t) { *p = t->cnext; break; }
    t->cnext = NULL; t->cqueued = 0;
}
/* A woken thread competes for a core from the moment it is woken, as in Horizon, where it is ready at once: Horizon
   switches to it before the SVC that woke it returns when it beats the thread that woke it on that thread's core, and
   interrupts the owner of another core it beats. Here the woken host thread still has to be scheduled and reach
   core_take; until it did, the thread that woke it ran on, and a producer that hands a buffer to a better consumer on
   its own core could change the buffer before the consumer saw it. So it queues here (core_take finds it queued), the
   owners it beats are signalled, and runtime_svc's checkpoint after every SVC lets the waker give its core up.
   Holding G. */
static int g_wake_queue_on = -1;   /* TOTK_WAKE_QUEUE=0: a woken thread queues only when it reaches core_take (old) */
static void wake_queue(thr_t *t) {
    if (g_wake_queue_on < 0) { const char *e = getenv("TOTK_WAKE_QUEUE"); g_wake_queue_on = !(e && !strcmp(e, "0")); fprintf(stderr, "[threads] woken threads compete for a core at once: %s\n", g_wake_queue_on ? "on" : "off (TOTK_WAKE_QUEUE=0)"); }
    if (!g_wake_queue_on || !cores_enabled() || t->cur_core >= 0 || t->cqueued || !t->started || t->exited) return;
    for (int i = 0; i < NCORE; i++) if (core_allowed(t, i) && !g_core_owner[i]) return;   /* a free core: core_take takes it */
    t->cseq = ++g_wseq; t->cnext = g_core_wait; g_core_wait = t; t->cqueued = 1; t->cwait_ns = now_ns();
    if (dm_wait_stats_enabled()) t->core_stats_since = t->cwait_ns;
    core_kick(t);
}
#endif
void runtime_describe_address(uint64_t a, char *out, size_t n);
#define CORE_DIAG_NS 4000000L
static int core_diag_on(void) { return dm_core_diag_enabled(); }
static int g_core_reports;
/* Enter/return holding G. Snapshot retained Mach ports, then drop G before suspending or formatting:
   an owner may be in host code holding an allocator/dyld lock, or may next need the scheduler lock. */
static void core_wait_report(thr_t *w, uint64_t since) {
    if (g_core_reports++ >= 400) return;
    struct { mach_port_t port; uint32_t handle; int prio; uint64_t mask, held; int svc; } owners[NCORE] = {0};
    uint64_t now = now_ns(), mask = w->mask; uint32_t handle = w->handle; int prio = w->prio;
    for (int i=0;i<NCORE;i++) {
        thr_t *o = g_core_owner[i]; if (!o) continue;
        owners[i].handle=o->handle; owners[i].prio=o->prio; owners[i].mask=o->mask;
        owners[i].held=o->core_since ? now-o->core_since : 0; owners[i].svc=o->last_svc;
        if (o->mport && mach_port_mod_refs(mach_task_self(), o->mport, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS) owners[i].port=o->mport;
    }
    pthread_mutex_unlock(&G);
    char pc[NCORE][96], lr[NCORE][96];
    for (int i=0;i<NCORE;i++) {
        strcpy(pc[i], "?"); strcpy(lr[i], "?"); mach_port_t port=owners[i].port;
        if (!port) continue;
        arm_thread_state64_t state; mach_msg_type_number_t count=ARM_THREAD_STATE64_COUNT; int ok=0;
        if (thread_suspend(port)==KERN_SUCCESS) {
            ok=thread_get_state(port, ARM_THREAD_STATE64, (thread_state_t)&state, &count)==KERN_SUCCESS;
            thread_resume(port);
        }
        mach_port_deallocate(mach_task_self(), port);
        if (ok) { runtime_describe_address(arm_thread_state64_get_pc(state), pc[i], sizeof pc[i]);
                  runtime_describe_address(arm_thread_state64_get_lr(state), lr[i], sizeof lr[i]); }
    }
    fprintf(stderr, "[core-wait] thread %#x (priority %d, mask %#llx) waited %.1f ms:", handle, prio, (unsigned long long)mask, (now-since)/1e6);
    for (int i=0;i<NCORE;i++) {
        if (!owners[i].handle) { fprintf(stderr, " core %d free;", i); continue; }
        fprintf(stderr, " core %d%s: %#x (priority %d, mask %#llx, held %.1f ms, last svc %#x) at %s <- %s;", i,
            (mask>>i&1) ? "" : " (not in mask)", owners[i].handle, owners[i].prio, (unsigned long long)owners[i].mask,
            owners[i].held/1e6, owners[i].svc, pc[i], lr[i]);
    }
    fputc('\n',stderr);
    pthread_mutex_lock(&G);
}
#ifndef GAME_BOTW
/* Asynchronous preemption for Tears of the Kingdom, as Breath of the Wild has (above, botw.md). Horizon
   gives a core to a strictly better thread the moment that thread is ready; here a better waiter only marked the owner,
   which gave the core up at its next SVC. Of the long core waits (2026-10-23) 58% were a better thread waiting for an
   owner running game code (median 4.5 ms). A waiter now signals (SIGUSR2) each owner it beats, again every millisecond
   while it waits, and the owner gives its core up in the handler only when interrupted in the game's own code at the
   top level (pc in a module's original code, host-stack slot set: no host lock held, not in libc), as in BotW. Who may
   preempt: a strictly better priority. Off by default (preemption at SVC entry only, as before); TOTK_ASYNC_PREEMPT=1 turns it on. */
#include <string.h>
#include <sys/ucontext.h>
extern uint64_t runtime_hle_tsd_off;   // hle_stack.c
int runtime_guest_code_pc(uint64_t pc);   // loader.c
static thr_t *me_or_null(void);
static void core_take(thr_t *t);
static int g_totk_ap = -1;
static uint64_t g_ap_kicks, g_ap_taken, g_ap_refused, g_ap_deferred;   // signals sent / cores given up in the handler / owner outside game code / owner with preemption disabled (SVC 0x36 later)
extern pthread_key_t runtime_keys[8];   // loader.c: [2] holds the game's thread-local region (its tpidrro)
static void totk_preempt_signal(int sig, siginfo_t *si, void *context);
static int totk_ap_on(void) {   // holding G
    if (g_totk_ap < 0) {
        /* On by default on the Mac since 2026-10-25 (user's choice; two short 1.4.3 runs at Lookout Landing: dropped
           refreshes 1.42% -> 0.84%, frame-time p95 15.2 -> 14.3 ms, too short to be conclusive; the author measured
           2.61% -> 0.93%). TOTK_ASYNC_PREEMPT=0 turns it off: the first thing to try when a hang, crash or new stutter
           shows up. Off on iPhone / iPad until it has run there (=1 turns it on). */
        const char *e = getenv("TOTK_ASYNC_PREEMPT");
#if TARGET_OS_IPHONE
        g_totk_ap = e && !strcmp(e, "1");
#else
        g_totk_ap = !(e && !strcmp(e, "0"));
#endif
        if (g_totk_ap) {
            struct sigaction a = {0}; a.sa_sigaction = totk_preempt_signal; a.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
            sigemptyset(&a.sa_mask);
            if (sigaction(SIGUSR2, &a, NULL)) g_totk_ap = 0;
        }
        fprintf(stderr, "[threads] asynchronous preemption of virtual cores (Horizon's rule): %s\n", g_totk_ap ? "on (TOTK_ASYNC_PREEMPT=0 turns it off)" : "off (TOTK_ASYNC_PREEMPT=1 turns it on)");
    }
    return g_totk_ap;
}
static int totk_should_yield(const thr_t *t) {   // holding G: does a better waiter that may run on t's core wait?
    if (t->cur_core < 0) return 0;
    int op = eff_prio(t);   // an owner that holds a mutex a better thread waits for is not preempted by worse ones
    for (const thr_t *w = g_core_wait; w; w = w->cnext) if (core_allowed(w, t->cur_core) && eff_prio(w) < op) return 1;
    return 0;
}
/* The owner's state is read before it is signalled (: 35% of 2 million signals reached an owner in a host
   function and changed nothing): in a host function (its host-stack slot is 0) it gives the core up at its next SVC
   anyway, so it is only marked; with preemption disabled (disable_count) the interrupt flag is set for it, as the
   kernel does, and the switch comes at its SVC 0x36. Only an owner in game code is signalled, and the same owner again
   at most every AP_RESIGNAL_NS. The handler still checks both itself (the owner may have moved meanwhile). */
#define AP_RESIGNAL_NS 2000000ull
static uint64_t g_ap_skip_host, g_ap_skip_disabled, g_ap_skip_recent;
static void totk_note_self(thr_t *t) {   // the calling thread, once its slots are set (guest_thread_init): for totk_kick
    if (t->hle_slot || !runtime_hle_tsd_off) return;
    void *tlr = pthread_getspecific(runtime_keys[2]); if (!tlr) return;
    uintptr_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    t->tlr16 = tlr; __atomic_store_n(&t->hle_slot, (volatile uintptr_t *)(tp + runtime_hle_tsd_off), __ATOMIC_RELEASE);
}
static int totk_kick(thr_t *t) {   // holding G, t waiting: mark (and signal) the owners t beats; returns how many
    int n = 0; uint64_t now = 0;
    for (int i = 0; i < NCORE; i++) {
        thr_t *o = g_core_owner[i];
        if (!o || !core_allowed(t, i)) continue;
        if (totk_ap_on() ? eff_prio(o) <= eff_prio(t) : o->prio <= t->prio) continue;   // off: the raw priorities, exactly as before
        __atomic_store_n(&o->preempt, 1, __ATOMIC_RELEASE); n++;
        if (!totk_ap_on()) continue;
        volatile uintptr_t *slot = __atomic_load_n(&o->hle_slot, __ATOMIC_ACQUIRE);
        if (slot && !*slot) { g_ap_skip_host++; continue; }   // in a host function: marked, it yields at its next SVC
        if (slot && o->tlr16 && o->tlr16[0x100 / 2]) { o->tlr16[0x102 / 2] = 1; g_ap_skip_disabled++; continue; }   // preemption disabled: SVC 0x36 switches
        if (!now) now = now_ns();
        if (o->ap_sig_ns && now - o->ap_sig_ns < AP_RESIGNAL_NS) { g_ap_skip_recent++; continue; }
        if (!pthread_kill(o->pt, SIGUSR2)) { g_ap_kicks++; o->ap_sig_ns = now; }
    }
    return n;
}
static void totk_preempt_signal(int sig, siginfo_t *si, void *context) {
    (void)sig; (void)si;
    thr_t *t = me_or_null();
    if (!t || !__atomic_load_n(&t->preempt, __ATOMIC_ACQUIRE) || t->cur_core < 0) return;
    uint64_t pc = (uint64_t)((ucontext_t *)context)->uc_mcontext->__ss.__pc;
    uintptr_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    if (!runtime_hle_tsd_off || !*(volatile uintptr_t *)(tp + runtime_hle_tsd_off) || !runtime_guest_code_pc(pc)) {
        __atomic_fetch_add(&g_ap_refused, 1, __ATOMIC_RELAXED); return; }   // in a host function: its next SVC gives the core up
    /* Horizon's preemption disable (ThreadLocalRegion +0x100 disable_count, +0x102 interrupt_flag): the SDK's spin locks
       (sdk+0x1277e0 and siblings) set disable_count while they hold or try the lock word, and after clearing it call
       SynchronizePreemptionState (SVC 0x36) when interrupt_flag is set. The kernel does not preempt such a thread; it
       sets interrupt_flag and the switch happens at that SVC. Preempting it anyway made the game stop at its first
       "Continue" (2026-10-24): the lock holder lost its core to a better thread that then spun with wfe on the lock it
       held, on the same core. The TLR is the game's tpidrro, which the loader keeps in runtime_keys[2]. */
    { volatile uint16_t *tlr = *(uint16_t *volatile *)(tp + (uintptr_t)runtime_keys[2] * 8);
      if (tlr && tlr[0x100 / 2]) { tlr[0x102 / 2] = 1; __atomic_fetch_add(&g_ap_deferred, 1, __ATOMIC_RELAXED); return; } }
    int saved = errno;
    pthread_mutex_lock(&G);
    if (t->cur_core >= 0 && totk_should_yield(t)) { core_put(t); core_take(t); g_ap_taken++; }
    else t->preempt = 0;
    pthread_mutex_unlock(&G);
    errno = saved;
}
#endif
static void core_take(thr_t *t) {   // holding G; waits until a core in t's mask is handed over
    if (!cores_enabled() || t->cur_core >= 0) return;
    if (!(t->mask & ((1u << NCORE) - 1))) die("thread %#x core mask %#llx has no application core", t->handle, (unsigned long long)t->mask);
    int c = t->core >= 0 && t->core < NCORE && core_allowed(t, t->core) && !g_core_owner[t->core] ? t->core : -1;
    for (int i = 0; c < 0 && i < NCORE; i++) if (core_allowed(t, i) && !g_core_owner[i]) c = i;
    uint64_t w0 = c < 0 && dm_wait_stats_enabled() ? now_ns() : 0;
    if (w0 && !t->core_stats_since) t->core_stats_since = w0;
#ifdef GAME_BOTW
    if (c >= 0) { core_unqueue(t); t->cur_core = c; t->core_since = now_ns(); core_wait_complete(t, t->core_since); g_core_owner[c] = t; return; }
    if (!t->cqueued) { t->cseq = ++g_wseq; t->cnext = g_core_wait; g_core_wait = t; t->cqueued = 1; t->cwait_ns = now_ns(); }
    int reported = 0;
    while (t->cur_core < 0) {   // kicked owners are signalled again every millisecond; otherwise look again every 20 ms (starvation, inheritance)
        struct timespec again = {0, core_kick(t) ? 1000000 : 20000000};
        pthread_cond_timedwait_relative_np(&t->cv, &G, &again);
        if (!reported && t->cur_core < 0 && t->cwait_ns && now_ns() - t->cwait_ns >= STARVE_NS) { if (core_diag_on()) core_wait_report(t, t->cwait_ns); reported = 1; }
    }
    t->cwait_ns = 0;
#else
    if (c >= 0) { t->cur_core = c; t->core_since = now_ns(); g_core_owner[c] = t; return; }
    t->cseq = ++g_wseq; t->cnext = g_core_wait; g_core_wait = t;
    int reported = 0;
    while (t->cur_core < 0) {   // the owners it beats are marked (and, with the asynchronous preemption, signalled again every millisecond)
        int kick = totk_kick(t) && totk_ap_on();
        if (!kick && (reported || !core_diag_on())) { pthread_cond_wait(&t->cv, &G); continue; }
        struct timespec again = {0, kick ? 1000000 : CORE_DIAG_NS};   // TOTK_CORE_DIAG: one look at the owners once a wait gets long
        pthread_cond_timedwait_relative_np(&t->cv, &G, &again);
        if (!reported && core_diag_on() && t->cur_core < 0 && now_ns() - w0 >= CORE_DIAG_NS) { core_wait_report(t, w0); reported = 1; }
    }
#endif
}
static thr_t *me_or_null(void) { return g_keythr ? pthread_getspecific(g_keythr) : NULL; }
#ifdef GAME_BOTW
extern uint64_t runtime_hle_tsd_off;                 // hle_stack.c: host-stack slot, cleared while a host function runs
int runtime_guest_code_pc(uint64_t pc);              // loader.c
static void preempt_signal(int sig, siginfo_t *si, void *context) {
    (void)sig; (void)si;
    thr_t *t = me_or_null();
    if (!t || !__atomic_load_n(&t->preempt, __ATOMIC_ACQUIRE) || t->cur_core < 0) return;
    uint64_t pc = (uint64_t)((ucontext_t *)context)->uc_mcontext->__ss.__pc;
    uintptr_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    if (!runtime_hle_tsd_off || !*(volatile uintptr_t *)(tp + runtime_hle_tsd_off) || !runtime_guest_code_pc(pc)) return;
    int saved = errno;
    pthread_mutex_lock(&G);
    if (t->cur_core >= 0 && core_should_yield(t)) { core_put(t); core_take(t); g_preempt_async++; }
    else t->preempt = 0;
    pthread_mutex_unlock(&G);
    errno = saved;
}
#endif
void runtime_thread_note_hle(unsigned i) { thr_t *t = me_or_null(); if (t) t->last_hle = (int)i + 1; }   // hle_stack.c, while tracing
uint32_t runtime_thread_current_handle(void) { thr_t *t = me_or_null(); return t ? t->handle : 0; }
// Host services that wait for other threads or the GPU give the core up around the wait (no host locks held).
int runtime_core_away(void) {
    thr_t *t = me_or_null(); if (!t || t->cur_core < 0) return 0;
    pthread_mutex_lock(&G); core_put(t); pthread_mutex_unlock(&G); return 1;
}
void runtime_core_back(int *away) {
    thr_t *t = me_or_null(); if (!*away || !t) return;
    pthread_mutex_lock(&G); core_take(t); pthread_mutex_unlock(&G);
}
/* A pass of a gated spin wait (patch_nso.py "Spin waits", registered builds only): a guest loop that polls memory until
   another thread changes it. On the Switch it fills only its own core; here it holds a virtual core that the thread it
   waits for, or a thread that one waits for, may need (Breath of the Wild 1.9.0's job queue, issue #480: workers at
   priority 44 polled until the main thread took its item, while priority 45-48 threads waited for their cores). A wait
   is a run of passes through the same loop with no SVC and at most SPIN_GAP_NS between them. Once a run has lasted
   TOTK_SPIN_HANDOFF_US (default 50 us) and a thread is queued for a virtual core, each pass gives the core to the waiter,
   whatever its priority, yields the host CPU and takes a core again (the job-queue hook of PR #621 did that on every
   pass of that one loop). Shorter waits (a lock held for a moment) keep the core: handing it over there made the next
   pass wait for a lower-priority thread's next SVC. TOTK_SPIN=0 keeps the core always (the gate's own cost only, A/B). */
#define SPIN_GAP_NS 20000ull
static _Atomic uint64_t g_spin_passes, g_spin_handoffs;
static int spin_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_SPIN"); v = !(e && !strcmp(e, "0")); } return v; }
static uint64_t spin_handoff_ns(void) { static int64_t v = -1; if (v < 0) { const char *e = getenv("TOTK_SPIN_HANDOFF_US"); v = (e ? atoll(e) : 50) * 1000; } return (uint64_t)v; }
static __thread uint64_t sw_head, sw_t0, sw_last, sw_nsvc;
void runtime_spin_wait(unsigned module, uint64_t head) {
    atomic_fetch_add_explicit(&g_spin_passes, 1, memory_order_relaxed);
    if (!spin_on()) return;
    thr_t *t = me_or_null(); if (!t || t->cur_core < 0) return;
    uint64_t now = now_ns(), key = head ^ ((uint64_t)module << 56);
    if (key != sw_head || t->nsvc != sw_nsvc || now - sw_last > SPIN_GAP_NS) { sw_head = key; sw_t0 = now; sw_nsvc = t->nsvc; }
    sw_last = now;
    if (now - sw_t0 < spin_handoff_ns() || !__atomic_load_n(&g_core_wait, __ATOMIC_RELAXED)) return;   // a racy look: a waiter missed now is seen at the next pass
    atomic_fetch_add_explicit(&g_spin_handoffs, 1, memory_order_relaxed);
    int away = runtime_core_away();
    sched_yield();
    runtime_core_back(&away);
    sw_last = now_ns();
}
uint64_t runtime_spin_passes(void) { return atomic_load_explicit(&g_spin_passes, memory_order_relaxed); }
// Preemption point at SVC entry: a higher-priority thread waits for this core.
void runtime_core_checkpoint(void) {
    thr_t *t = me_or_null();
#ifdef GAME_BOTW
    /* Breath of the Wild (botw.md): the preempt flag is set when a waiter queues up, but a waiter's mask or priority can
       change while it waits; so at every SVC a core owner also looks at the queue itself (core_should_yield: Horizon's
       rule, see core_beats). */
    if (!t || t->cur_core < 0 || (!t->preempt && !__atomic_load_n(&g_core_wait, __ATOMIC_RELAXED))) return;
    pthread_mutex_lock(&G);
    if (t->cur_core >= 0 && core_should_yield(t)) { core_put(t); core_take(t); }
    else t->preempt = 0;
    pthread_mutex_unlock(&G);
    return;
#else
    if (t && !t->hle_slot) totk_note_self(t);   // once, at its first SVC (its slots are set by then)
#endif
    if (!t || !t->preempt) return;
    pthread_mutex_lock(&G); t->preempt = 0; core_put(t); core_take(t); pthread_mutex_unlock(&G);
}
int runtime_current_core(void) { thr_t *t = me_or_null(); return t ? (t->cur_core >= 0 ? t->cur_core : t->core) : -1; }

static int64_t block(thr_t *t,int kind,int64_t ns) {
    if (ns == 0) return block_inner(t, kind, ns);
    core_put(t); int64_t r = block_inner(t, kind, ns); core_take(t); return r;
}
static int64_t block_inner(thr_t *t, int kind, int64_t ns) {
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

// ---------- 锁 ----------
// ---------- Locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

#ifdef GAME_BOTW
/* A thread's lock tag is its own handle (the SDK reads it from the ThreadType at TLR+0x1f8). Any other tag means two
   threads can believe they hold the same mutex (a tag of 0 reads as "free"): logged (holding G). */
static uint64_t g_bad_tags;
static void tag_check(const thr_t *t, uint32_t tag, const char *what, uint64_t addr) {
    if (tag == t->handle) return;
    if (g_bad_tags++ < 40) fprintf(stderr, "[threads] %s by thread %#x with tag %#x (not its handle) on %#llx\n", what, t->handle, tag, (unsigned long long)addr);
}
#endif
// 释放 addr 处的锁：交给等在它上面的第一个线程，或清零（持 G）
// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// 让 t 去拿 addr 处的锁：空闲就直接拿到并唤醒，否则置等待者位并转为锁等待（持 G）
// Make t take the lock at addr: if free, it gets it directly and is woken, otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // 仍在 g_wait 里，改为锁等待 | Still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // 内核把它新插进锁持有者的等待队列：同优先级排在已有锁等待者之后 | The kernel inserts it anew into the lock owner's wait queue: at equal priority it goes after the existing lock waiters
}

// ---------- 线程 ----------
// ---------- Threads ----------
extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp,uint64_t *host_context);
extern _Noreturn void runtime_thread_leave(const uint64_t *host_context,void *arg,void(*fn)(void *));   // entry.S
uint32_t (*dm_thread_tick_fn)(void);   // 取当前游戏拍；diag.c 初始化时登记（单测不登记，拍号记 0） | Gets the current game tick; registered when diag.c initializes (unit tests do not register it, the tick number is recorded as 0)
void crash_thread_init(void);
void guest_thread_init(void);   // thread.c：每线程 TSD 槽（门、x18、TLS、TLR）唯一设置点 | thread.c: the single place that sets the per-thread TSD slots (gate, x18, TLS, TLR)
__attribute__((weak)) void runtime_hle_stack_set(uintptr_t top) { (void)top; }   // standalone thread probe: no host-stack switch
static void *thread_main(void *p) {
    thr_t *t = p;
    pthread_setspecific(g_keythr, t);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    t->mport = pthread_mach_thread_np(pthread_self());

    guest_thread_init();
    pthread_mutex_lock(&G); while (!t->started) pthread_cond_wait(&t->cv, &G); core_take(t); pthread_mutex_unlock(&G);
    { volatile char marker = 0; runtime_hle_stack_set((uintptr_t)&marker - 1024); }   // host services of this thread run below this frame
    dm_thread_enter(t->entry, t->arg, t->stack_top,t->host_context);   // 不返回：客体须经 ExitThread 退出 | Does not return: the guest must exit through ExitThread
    die("客体线程入口返回");
}
static void thr_free(thr_t *t) {   // 持 G；从全部线程链摘下并释放 | Holding G; unlink from the all-threads chain and free
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); free(t);
}

static _Noreturn void thread_exit_on_host(void *argument) {
    thr_t *t=argument;
    t->exit_ns = now_ns(); t->exit_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
    // 这里不打日志：退出发生在客体线程自己的栈上（有的只有十几 KiB），带浮点的 printf 会把栈撑爆（真机冒烟：LibcurlResolver 退出时 SIGBUS，回溯 fprintf → __dtoa）。时刻只记下来，现场导出时再打
    // No logging here: the exit happens on the guest thread's own stack (some are only a dozen or so KiB), and a printf with floating point would blow the stack (real-device smoke test: SIGBUS when LibcurlResolver exits, backtrace fprintf → __dtoa). Only the time is recorded, and it is printed later in the state dump
    pthread_mutex_lock(&G);
    core_put(t);
    t->exited = 1;
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }   // 同步等待者重查 | Synchronization waiters re-check
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    pthread_exit(NULL);
}
static _Noreturn void thread_exit(thr_t *t) {
 if(t->host_context[0])runtime_thread_leave(t->host_context,t,thread_exit_on_host);
 thread_exit_on_host(t);
}
void dm_thread_returned(void) { die("客体线程入口函数返回（Horizon 要求 ExitThread）"); }

// 主线程登记，返回其句柄（交给 nnosInitialize 作主线程句柄）
// Register the main thread and return its handle (passed to nnosInitialize as the main thread handle)
uint32_t svc_thread_main_init(void) {
    pthread_key_create(&g_keythr, NULL);
    thr_t *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cv, NULL);
#ifdef GAME_BOTW
    t->started = 1; t->prio = 44; t->core = PROCESS_IDEAL_CORE; t->mask = 1u << PROCESS_IDEAL_CORE; t->pt = pthread_self(); t->mport = pthread_mach_thread_np(t->pt);
#else
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self(); t->mport = pthread_mach_thread_np(t->pt);
#endif
    t->cur_core = -1;
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; core_take(t); pthread_mutex_unlock(&G);
#ifdef GAME_BOTW
    g_main_handle = t->handle;
#endif
    pthread_setspecific(g_keythr, t);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    return t->handle;
}

static _Atomic uint64_t g_main_yield_n, g_main_yield_ns;   // 只有客体主线程写；诊断行在同一线程读 | Written only by the guest main thread; the diagnostic line reads it on the same thread
void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = g_main_yield_n; *ns = g_main_yield_ns; }
#define CORE_TABLE_NS 10000000000ull
static void core_table(void) {
    static _Atomic uint64_t last;
    if (!core_diag_on()) return;
    uint64_t now=now_ns(), previous=atomic_load_explicit(&last,memory_order_relaxed);
    if (previous && now-previous<CORE_TABLE_NS) return;
    enum { ROWS=256 };
    typedef struct { uint32_t handle; int prio, core, svc, active; uint64_t mask, held, wait, count, cpu; } row;
    row *rows=calloc(ROWS,sizeof *rows); if (!rows) return;
    pthread_mutex_lock(&G);
    now=now_ns(); previous=atomic_load_explicit(&last,memory_order_relaxed);
    if (previous && now-previous<CORE_TABLE_NS) { pthread_mutex_unlock(&G); free(rows); return; }
    double span=previous ? (now-previous)/1e6 : 0; atomic_store_explicit(&last,now,memory_order_relaxed); int n=0, truncated=0;
    for (thr_t *t=g_all;t;t=t->all) {
        uint64_t held=t->core_held_ns+(t->cur_core>=0 && t->core_since ? now-t->core_since : 0);
        uint64_t dh=held-t->core_held_prev, dw=t->core_wait_ns-t->core_wait_prev, dn=t->core_nwait-t->core_nwait_prev;
        t->core_held_prev=held; t->core_wait_prev=t->core_wait_ns; t->core_nwait_prev=t->core_nwait;
        uint64_t cpu=0; thread_basic_info_data_t bi; mach_msg_type_number_t count=THREAD_BASIC_INFO_COUNT;
        if (t->mport && thread_info(t->mport,THREAD_BASIC_INFO,(thread_info_t)&bi,&count)==KERN_SUCCESS)
            cpu=(uint64_t)(bi.user_time.seconds+bi.system_time.seconds)*1000000000ull+(uint64_t)(bi.user_time.microseconds+bi.system_time.microseconds)*1000ull;
        uint64_t dc=cpu>=t->cpu_prev_ns ? cpu-t->cpu_prev_ns : 0; t->cpu_prev_ns=cpu;
        if (span && dh<50000000ull && dw<5000000ull) continue;
        if (n==ROWS) { truncated=1; continue; }
        rows[n++]=(row){t->handle,t->prio,t->core,t->last_svc,t->cur_core>=0,t->mask,dh,dw,dn,dc};
    }
#ifndef GAME_BOTW
    uint64_t ap[7]={g_ap_kicks,__atomic_load_n(&g_ap_taken,__ATOMIC_RELAXED),__atomic_load_n(&g_ap_refused,__ATOMIC_RELAXED),
                    __atomic_load_n(&g_ap_deferred,__ATOMIC_RELAXED),g_ap_skip_host,g_ap_skip_disabled,g_ap_skip_recent};
    int ap_on=g_totk_ap==1;
#endif
    pthread_mutex_unlock(&G);
    fprintf(stderr,"[core-table] last %.0f ms; mask: bit 0-2 = virtual cores 0-2\n",span);
    { uint64_t sp = atomic_load_explicit(&g_spin_passes, memory_order_relaxed); if (sp) fprintf(stderr, "[core-table] spin-wait gate passes so far: %llu, core hand-offs %llu%s\n", (unsigned long long)sp, (unsigned long long)atomic_load_explicit(&g_spin_handoffs, memory_order_relaxed), spin_on() ? "" : " (TOTK_SPIN=0: the core is kept)"); }
#ifndef GAME_BOTW
    if (ap_on) fprintf(stderr,"[core-table] asynchronous preemption so far: %llu signals, %llu cores given up in the handler, %llu refused (host code), "
                       "%llu deferred to SVC 0x36; not signalled: %llu in a host function, %llu with preemption disabled, %llu signalled < 2 ms ago\n",
                       (unsigned long long)ap[0],(unsigned long long)ap[1],(unsigned long long)ap[2],(unsigned long long)ap[3],
                       (unsigned long long)ap[4],(unsigned long long)ap[5],(unsigned long long)ap[6]);
#endif
    for (int i=0;i<n;i++) { const row *r=&rows[i];
        fprintf(stderr,"[core-table]   thread %#x priority %d mask %#llx ideal %d: held %.0f ms (%.0f%%), cpu %.0f ms (%.0f%% of held), waited %.1f ms in %llu waits, last svc %#x%s\n",
            r->handle,r->prio,(unsigned long long)r->mask,r->core,r->held/1e6,span ? 100.0*(r->held/1e6)/span : 0,r->cpu/1e6,
            r->held ? 100.0*r->cpu/r->held : 0,r->wait/1e6,(unsigned long long)r->count,r->svc,r->active ? " (on a core now)" : "");
    }
    if (truncated) fputs("[core-table] more than 256 active rows; remaining rows omitted\n",stderr);
    free(rows);
}
/* For the frame TSV (hle_nvn.c @core_wait_ms / @core_wait_all_ms / @core_waits): the calling thread's and all guest
   threads' time waiting for a virtual core, and the number of waits. Called on the presenting thread once a frame. */
void dm_core_wait_stats(uint64_t *self_ns, uint64_t *all_ns, uint64_t *all_n) {
    core_table();
    *self_ns = *all_ns = *all_n = 0;
    if (!dm_wait_stats_enabled()) return;
    pthread_mutex_lock(&G);
    thr_t *me = me_or_null();
    *self_ns = me ? me->core_wait_ns : 0; *all_ns = g_core_wait_all_ns; *all_n = g_core_wait_all_n;
    pthread_mutex_unlock(&G);
}
uint32_t dm_guest_work_qos(unsigned *count) {
    uint32_t qos = QOS_CLASS_UNSPECIFIED; unsigned n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) {
#ifdef GAME_BOTW
        if (t->handle == g_main_handle) continue;
#else
        if (t->handle == 0x1001) continue;
#endif
        if (!n) qos = t->qos; else if (qos != t->qos) qos = UINT32_MAX;
        n++;
    }
    pthread_mutex_unlock(&G);
    if (count) *count = n;
    return qos;
}

// ---------- 同步对象 ----------
// ---------- Synchronization objects ----------
static int sig_obj(uint32_t h, int *bad) {    // 1=已触发（持 G） | 1 = signaled (holding G)
#ifdef GAME_BOTW
    unsigned i = h_index(h);
#else
    unsigned i = h - 0x1000;
#endif
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
// 只唤醒有对象已触发的同步等待者（等待的句柄多于记下的 4 个时照旧全唤醒；句柄失效的也唤醒，好让它返回错误）。
// 原先王国之泪每次触发都唤醒全部同步等待者：每个都要醒来、重新抢虚拟核（core_take，可能为此抢占别的线程）、查一遍句柄再睡回去，
// 一次触发就是每个等待线程一次宿主线程切换，费 CPU 和电（iPhone 上尤甚）。旷野之息从 58ac238 起就这样做（它的 wake_queue 让被唤醒者立即抢核）；
// 当时只放在 GAME_BOTW 下是为了那次提交不改王国之泪的构建，并不是王国之泪有问题。语义不变：没有触发的等待者醒来也只会再睡回去（nxnative df7badd 同理）。
// Wake only the waiters one of whose objects is now signaled (all of them when the wait has more handles than the 4 kept; also those
// holding a handle that is no longer valid, so they return the error). Tears of the Kingdom used to wake every synchronization waiter
// on every signal: each woke, competed for a virtual core again (core_take, which may preempt another thread for it), re-checked its
// handles and went back to sleep, one host thread switch per waiting thread per signal, costing CPU and power (most of all on the
// iPhone). Breath of the Wild has done this since 58ac238 (its wake_queue makes a woken thread compete for a core at once); it was
// kept under GAME_BOTW only so that commit left Tears of the Kingdom's build unchanged, not because of a problem there. Semantics are
// unchanged: a waiter none of whose objects is signaled would only have gone back to sleep (nxnative df7badd does the same).
// Holding G.
static void wake_sync_waiters(void) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next;
        if (w->wkind == 4) { int hit = w->nwh > 4, bad = 0; for (int i = 0; !hit && i < w->nwh; i++) hit = sig_obj(w->wh[i], &bad) || bad; if (hit) wake(w, -1); }
        w = n; }
}

static int64_t now_mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
void dm_guest_clock_advanced(void) {
    if (!dm_detclock_enabled()) return;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind) pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&G);
}

// 服务层 SystemEvent 的绑定（见下面 hle_bind_system_event）：客体 SystemEventType 地址 → 它那个内核事件写端的句柄。
// 绑定与写端句柄同生共死：CloseHandle 关掉写端时摘掉它（se_unbind），同一地址上销毁后重建的 SystemEvent 会得到新的内核事件。
// 原先绑定永不结束：第二次绑定见到旧项直接返回，新的内核事件根本没建，触发发给已关的句柄（王国之泪的句柄编号会复用，于是发给了占了那个编号的别的事件）。
// 例如 nfp::AttachAvailabilityChangeEvent：游戏每次用 amiibo 都可能在同一地址重建这个事件（nxnative 49ee623 同理）。锁的次序：先 G 后 g_se_lock（持 g_se_lock 时不取 G）
// Service-layer SystemEvent bindings (see hle_bind_system_event below): guest SystemEventType address -> the handle of its kernel event's
// write end. A binding lives as long as that handle: CloseHandle on the write end removes it (se_unbind), so a SystemEvent destroyed and
// created again at the same address gets a new kernel event. Bindings used to stay forever: the second bind found the old entry and
// returned, the new kernel event was never created, and signals went to the closed handle (Tears of the Kingdom reuses handle numbers,
// so to whatever event took that number). nfp::AttachAvailabilityChangeEvent, for one, may create the event again at the same address
// each time the game uses amiibo (nxnative 49ee623 found the same). Lock order: G, then g_se_lock (nothing takes G holding g_se_lock).
static struct { void *ev; uint32_t wh; } g_se[512];
static int g_nse;
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(void *ev) {   // 调用方持 g_se_lock | Caller holds g_se_lock
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
static void se_unbind(uint32_t wh) {   // 调用方持 G | Caller holds G
    pthread_mutex_lock(&g_se_lock);
    for (int i = 0; i < g_nse; ) if (g_se[i].wh == wh) g_se[i] = g_se[--g_nse]; else i++;
    pthread_mutex_unlock(&g_se_lock);
}

// 处理线程 / 同步类 SVC；返回 1 = 已处理
// Handle thread / synchronization SVCs; returns 1 = handled
/* Yield back-off (after nxnative e06a8ec / b703be2). On the Switch a yield with nothing else runnable costs nothing; here
   a guest loop that polls and yields becomes a host busy spin (on the iPhone the system logged about 1,300 wake-ups a
   second for the game). A run of yields with no other SVC between them stays plain sched_yield for its first 2 ms:
   shorter waits are handoffs between working threads, such as the main thread waiting for its ModuleSystem workers on
   the frame's critical path (nxnative started at 200 µs and 1.4.3 then dropped itself to 20 fps). After that each yield
   sleeps an eighth of the run so far, 10 to 250 µs, with the virtual core given up, so a wait ends at most an eighth late.
   Off by default: on the Mac at about 100 fps (1.2.1 field save, 300 s each) it saved 3% CPU (708 -> 685 s) but frames
   dropped steadily, 10-15 per 300 instead of 0-3. TOTK_YIELD_BACKOFF=on turns it on (250:2000); =<cap µs>[:<start µs>]
   sets the cap and the start. Returns the sleep for a yield whose run started run_ns ago, 0 for a plain yield. */
static uint64_t yield_backoff_ns(uint64_t run_ns) {
    static int64_t cap = -1, start = 2000000;
    if (cap < 0) {
        const char *e = getenv("TOTK_YIELD_BACKOFF"); int64_t c = 0;
        if (e && !strcmp(e, "on")) c = 250000;
        else if (e && *e) { c = atoll(e) * 1000; const char *colon = strchr(e, ':'); if (colon) start = atoll(colon + 1) * 1000; }
        if (c) LOG("TOTK_YIELD_BACKOFF: a run of yields backs off after %lld us, sleeping at most %lld us", (long long)(start / 1000), (long long)(c / 1000));
        cap = c;
    }
    if (!cap || run_ns < (uint64_t)start) return 0;
    uint64_t s = run_ns / 8; if (s < 10000) s = 10000;
    return s < (uint64_t)cap ? s : (uint64_t)cap;
}

int svc_thread(int num, uint64_t *regs) {
    { thr_t *me = (g_keythr ? pthread_getspecific(g_keythr) : NULL); if (me) { me->last_svc = num; me->last_lr = regs[30]; me->last_fp = regs[29]; me->nsvc++; } }

    switch (num) {
    case 0x08: {  // CreateThread(→w1 句柄; x1 入口, x2 参数, x3 栈顶, w4 优先级, w5 核) | CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        if(!regs[1]||!regs[3]||(regs[3]&15)||(int)regs[4]<28||(int)regs[4]>59||(int)regs[5]<-3||(int)regs[5]>2){regs[0]=R_BAD_STATE;return 1;}
        thr_t *t = calloc(1, sizeof *t);
        if(!t)die("thread allocation");
        t->cur_core = -1;
        pthread_cond_init(&t->cv, NULL);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
#ifdef GAME_BOTW
        { int c = (int)regs[5]; if (c == -2) c = PROCESS_IDEAL_CORE;
          if (c < 0 || c > 2) { free(t); regs[0] = R_BAD_CORE; return 1; }   // Horizon: only -2 is adjusted, other negatives are invalid here
          t->prio = (int)regs[4]; t->core = c; t->mask = 1ull << c; }
#else
        t->prio = (int)regs[4]; t->core = (int)regs[5]; t->mask = t->core >= 0 ? 1ull << t->core : 7;
#endif
        t->born_ns = now_ns(); t->born_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
        pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);   // also the host-service stack of this guest thread
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // 退出即回收宿主资源（客体经句柄等待，不 join） | Host resources are reclaimed on exit (the guest waits through the handle, no join)
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        LOG("CreateThread h=%#x entry=%#llx sp=%#llx", t->handle,t->entry,t->stack_top);
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
    case 0x0B: {  // SleepThread(x0 ns)；0/-1/-2 = 各类让出 | SleepThread(x0 ns); 0/-1/-2 = the various kinds of yield
        int64_t ns = (int64_t)regs[0];

        thr_t *self = cur();
        if (ns > 0) { pthread_mutex_lock(&G); core_put(self); pthread_mutex_unlock(&G); }
        if (ns > 0 && dm_detclock_enabled()) {
            uint64_t deadline = guest_ns() + (uint64_t)ns;
            while (guest_ns() < deadline) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
        } else if (ns > 0) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, 0); }
        else {   // 让出（0 / -1 / -2）。主线程的让出次数与耗时记账：游戏渲染任务图「等全部做完」是纯轮询 + 让出（main+0x15ee898），量它占一帧多少 | Yield (0 / -1 / -2). Count and time of the main thread's yields are recorded: the game's render task graph "wait for everything to finish" is pure polling + yield (main+0x15ee898); measure how much of a frame it takes
            thr_t *me = (g_keythr ? pthread_getspecific(g_keythr) : NULL);
            uint64_t y0 = now_ns();
            if (me && me->nsvc != me->yrun_svc + 1) me->yrun_ns = y0;   // another SVC since the last yield: a new run
            if (me) me->yrun_svc = me->nsvc;
            pthread_mutex_lock(&G);   // Horizon yield: hand the core to a waiting thread of equal or higher priority
            int give = 0; for (thr_t *w = g_core_wait; w && !give; w = w->cnext) give = me->cur_core >= 0 && core_allowed(w, me->cur_core) && w->prio <= me->prio;
            if (give) { core_put(me); core_take(me); }
            pthread_mutex_unlock(&G);
            uint64_t backoff = !give && me ? yield_backoff_ns(y0 - me->yrun_ns) : 0;
            if (backoff) {   // a long poll: sleep with the core given up, as SleepThread(ns) does
                pthread_mutex_lock(&G); core_put(me); pthread_mutex_unlock(&G);
                struct timespec ts = {0, (long)backoff}; nanosleep(&ts, 0);
                pthread_mutex_lock(&G); core_take(me); pthread_mutex_unlock(&G);
            } else if (!give) sched_yield();
#ifdef GAME_BOTW
            if (me && me->handle == g_main_handle) { g_main_yield_ns += now_ns() - y0; g_main_yield_n++; }
#else
            if (me && me->handle == 0x1001) { g_main_yield_ns += now_ns() - y0; g_main_yield_n++; }
#endif
        }
        if (ns > 0) { pthread_mutex_lock(&G); core_take(self); pthread_mutex_unlock(&G); }

        return 1;
    }
    case 0x0C: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; pthread_mutex_unlock(&G);return 1; }
    case 0x0D: { if(regs[1]<28||regs[1]>59){regs[0]=R_BAD_STATE;return 1;}pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[0], O_THREAD); if (t) t->prio = (int)regs[1]; regs[0] = t ? 0 : R_BAD_HANDLE;pthread_mutex_unlock(&G); return 1; }
    case 0x0E: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0;pthread_mutex_unlock(&G); return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 理想核, x2 掩码)：-1 = 用进程缺省、-2/-3 = 不改理想核；宿主只记账 | SetThreadCoreMask(w0, w1 ideal core, x2 mask): -1 = use the process default, -2/-3 = leave the ideal core unchanged; the host only keeps the bookkeeping
        pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
#ifdef GAME_BOTW
        if (t) { int c = (int)regs[1]; uint64_t m = regs[2]; uint32_t r = 0;
                 if (c == -2) { c = PROCESS_IDEAL_CORE; m = 1ull << c; }
                 else if ((m | PROCESS_CORE_MASK) != PROCESS_CORE_MASK) r = R_BAD_CORE;
                 else if (!m) r = R_BAD_COMBO;
                 else if (c >= 0 && c <= 2) { if (!((m >> c) & 1)) r = R_BAD_COMBO; }
                 else if (c == -3) { c = t->core; if (c >= 0 && !((m >> c) & 1)) r = R_BAD_COMBO; }
                 else if (c != -1) r = R_BAD_CORE;
                 if (!r) { t->core = c; t->mask = m;
                           if (t == cur() && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) { core_put(t); core_take(t); } }   // migrate off a core no longer allowed
                 regs[0] = r; pthread_mutex_unlock(&G); return 1; }
#else
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; else if (c == -1) t->core = 0; if (regs[2]) t->mask = regs[2];
                 if (t == cur() && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) { core_put(t); core_take(t); } }   // migrate off a core no longer allowed
#endif
        regs[0] = t ? 0 : R_BAD_HANDLE;pthread_mutex_unlock(&G); return 1;
    }
    case 0x10: { thr_t *t = cur(); regs[0] = (uint64_t)(uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core); return 1; }   // GetCurrentProcessorNumber
    case 0x25: { pthread_mutex_lock(&G);thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0;pthread_mutex_unlock(&G); return 1; }

    case 0x1A: {  // ArbitrateLock(w0 持有者句柄, x1 地址, w2 本线程标签) | ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        pthread_mutex_lock(&G);
#ifdef GAME_BOTW
        tag_check(t, (uint32_t)regs[2], "ArbitrateLock", addr);
#endif
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2];
#ifdef GAME_BOTW
            for (thr_t *o = g_core_wait; o; o = o->cnext) if (o->handle == (uint32_t)regs[0]) { pthread_cond_signal(&o->cv); break; }   // an owner waiting for a core looks again (eff_prio)
#endif
            r = block(t, 1, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: pthread_mutex_lock(&G); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 锁地址, x1 条件键, w2 标签, x3 超时) | WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        pthread_mutex_lock(&G);
#ifdef GAME_BOTW
        tag_check(t, (uint32_t)regs[2], "WaitProcessWideKeyAtomic", addr);
#endif
        // 顺序要紧：先置条件键 = 1，再放互斥锁。SDK 的 InternalConditionVariable::Signal 在用户态先读条件键、为 0 就不发 svc 0x1D；
        // 用户态抢互斥锁也不经过 G。原先先 lock_handoff 后置键：放锁后、置键前，别的线程可在用户态抢到锁、改完条件、读到键 0 就不唤醒，
        // 本线程随后挂上去永远等不到（并行负载下资源表读写锁 main+0x320e5e0 卡死：读者计数 0、锁空闲、写者条件键 1、写者仍在等）。
        // 挂进等待链在 block 里、仍持 G：svc 0x1D 要取 G，不会落在这之前
        // The order matters: first set the condition key = 1, then release the mutex. The SDK's InternalConditionVariable::Signal first reads the condition key in user mode, and does not issue svc 0x1D if it is 0;
        // grabbing the mutex in user mode does not go through G either. It used to do lock_handoff first and set the key after: after the release and before the key was set, another thread could grab the lock in user mode, change the condition, read key 0 and not wake anyone,
        // and this thread would then link itself in and wait forever (under parallel load the resource table read-write lock main+0x320e5e0 deadlocked: reader count 0, lock free, writer condition key 1, writer still waiting).
        // Linking into the wait chain happens inside block, still holding G: svc 0x1D has to take G, so it cannot land before this
#ifdef DM_TEST_HOOKS
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }   // 单测：紧跟放锁之后、挂链之前 | Unit test: right after releasing the lock, before linking into the wait chain
#else
#define CV_TEST_HOOK()
#endif
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr); CV_TEST_HOOK();
#else   // 仅单测 t_condvar 的负对照：还原修复前「先放锁、后置键」的顺序 | Negative control for the unit test t_condvar only: restores the pre-fix order "release the lock first, set the key after"
        lock_handoff(addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
#endif
        int64_t r = block(t, 2, (int64_t)regs[3]);   // 超时：不重新拿锁（用户态自己拿） | Timeout: the lock is not re-acquired (user mode takes it itself)
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 条件键, w1 个数，<=0 全部) | SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        pthread_mutex_lock(&G);
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // 按内核顺序；转为锁等待或被唤醒后不再是 kind 2 | In kernel order; once turned into a lock wait or woken it is no longer kind 2
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 地址, w1 类型, w2 值, x3 超时)：0 小于则等、1 小于则减一再等、2 相等则等 | WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        pthread_mutex_lock(&G);
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, (int64_t)regs[3]); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 地址, w1 类型, w2 值, w3 个数)：0 唤醒、1 相等则加一并唤醒、2 相等则按等待数改值并唤醒 | SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify the value by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        pthread_mutex_lock(&G);
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                // 全部唤醒（个数 <= 0）且有等待者时减 2，不是减 1（mesosphère KAddressArbiter::SignalAndModifyByWaitingCountIfEqual，Ryujinx、yuzu 一致；nxnative d47f899 在 Engage 上查到）
                // Waking all (count <= 0) with waiters subtracts 2, not 1 (mesosphère KAddressArbiter::SignalAndModifyByWaitingCountIfEqual; Ryujinx and yuzu agree; found by nxnative d47f899 in Engage)
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

    case 0x45: {  // CreateEvent(→w1 写端, w2 读端) | CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(写端) / ClearEvent(任一端) / ResetSignal(读端，未触发报错) | SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        pthread_mutex_lock(&G);
        // SignalEvent 只认写端；ClearEvent 两端都认；ResetSignal 只认读端
        // SignalEvent accepts only the write end; ClearEvent accepts both ends; ResetSignal accepts only the read end
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("事件 SVC %#x 句柄无效 %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 下标; x1 句柄数组, w2 个数, x3 超时) | WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        if(n<0||n>64||(!hs&&n)){regs[0]=R_BAD_STATE;return 1;}
        int64_t deadline = ns > 0 ? (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns()) + ns : 0;
        pthread_mutex_lock(&G);
        t->nwh = n; for (int i = 0; i < n && i < 4; i++) t->wh[i] = hs[i];
        int64_t r; int idx = -1;
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns());
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            if (block(t, 4, left) == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 线程) | CancelSynchronization(w0 thread)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {  // CloseHandle(w0)：事件两端都关才释放；线程句柄关闭后槽位回收，对象在线程也退出后释放 | CloseHandle(w0): an event is freed only when both ends are closed; after a thread handle is closed the slot is recycled, and the object is freed once the thread has also exited
        pthread_mutex_lock(&G);
#ifdef GAME_BOTW
        unsigned i = h_index((uint32_t)regs[0]);int valid=0;
#else
        unsigned i = (uint32_t)regs[0] - 0x1000;int valid=0;
#endif
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            if (g_h[i].type == O_EVENT_W) se_unbind((uint32_t)regs[0]);   // 绑定随写端结束 | the binding ends with its write end
            valid=1;event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && (g_h[i].type == O_SESSION || g_h[i].type == O_TMEM)) {
            valid=1;void *o = g_h[i].obj; int session = g_h[i].type == O_SESSION;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            pthread_mutex_unlock(&G);
            if (session) runtime_ipc_session_closed(o); else runtime_ipc_tmem_closed(o);
            pthread_mutex_lock(&G);
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            valid=1;thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        }
        pthread_mutex_unlock(&G);
        regs[0] = valid?0:R_BAD_HANDLE; return 1;
    }
    }
    return 0;
}

// ---------- 服务层的 SystemEvent ----------
// 真机：服务把内核事件的读端交给客户端，客体经 SDK nn::os 等待。宿主：在客体线程上调 SDK 自己的
// CreateSystemEvent(跨进程=true)，SDK 经 SVC 0x45 建出内核事件；记下写端句柄，之后任何线程（含宿主音频线程）都可触发。
// ---------- SystemEvent of the service layer ----------
// Real device: the service hands the read end of a kernel event to the client, and the guest waits on it through SDK nn::os. Host: on a guest thread, call the SDK's own
// CreateSystemEvent(inter-process=true), and the SDK creates the kernel event through SVC 0x45; the write-end handle is recorded, after which any thread (including the host audio thread) can signal it.
// 绑定表 g_se 在 svc_thread 之前：CloseHandle 经它解绑 | The binding table g_se sits before svc_thread: CloseHandle unbinds through it
void hle_bind_system_event(void *ev, int autoclear) {      // 必须在客体线程上调（要执行 SDK 代码） | Must be called on a guest thread (it has to execute SDK code)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) return;
    if (!(g_keythr ? pthread_getspecific(g_keythr) : NULL)) die("hle_bind_system_event 须在客体线程上调用");
    static uint32_t (*create)(void *, int, int); static uint32_t (*getw)(const void *);
    if (!create) { create = guest_sym("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
                   getw = guest_sym("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
    uint32_t r = create(ev, autoclear, 1);
    if (r) die("CreateSystemEvent 失败 %#x", r);
    wh = getw(ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent 绑定表满");
    g_se[g_nse].ev = ev; g_se[g_nse++].wh = wh;
    pthread_mutex_unlock(&g_se_lock);
}
// 未绑定的在客体线程上按手动清除即时绑定；宿主线程（显示链接的垂直同步回调等）上未绑定说明游戏已销毁了它，这次触发没有对象可发，跳过。
// 查找与触发在同一次持 G 内完成：CloseHandle（会解绑）也在 G 下，句柄不会在两者之间被关掉、编号被别的事件占去
// An unbound one is bound on the spot as manual-clear on a guest thread; on a host thread (the display link's vsync callback and the
// like) unbound means the game destroyed it, and this signal has nothing to go to: skipped. The lookup and the signal happen under one
// hold of G: CloseHandle (which unbinds) also runs under G, so the handle cannot be closed and its number taken by another event between them
static uint64_t g_se_skipped;
void hle_signal_system_event(void *ev) {
    for (int bound = 0; ; bound = 1) {
        pthread_mutex_lock(&G);
        pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
        if (wh) {
            event_t *e = h_get(wh, O_EVENT_W);
            if (!e) die("SystemEvent 写端句柄失效 %#x", wh);
            e->signaled = 1; wake_sync_waiters();
            pthread_mutex_unlock(&G);
            return;
        }
        int guest = (g_keythr ? pthread_getspecific(g_keythr) : NULL) != NULL;
        if (!guest && g_se_skipped++ < 8) LOG("SystemEvent %p 未绑定（游戏已销毁），宿主线程的触发跳过 | not bound (destroyed by the game): signal from a host thread skipped", ev);
        pthread_mutex_unlock(&G);
        if (!guest) return;
        if (bound) die("SystemEvent %p 绑定后仍查不到 | still not bound after binding", ev);
        hle_bind_system_event(ev, 0);   // 执行客体代码（其 SVC 要取 G）：不能持 G | runs guest code (its SVCs take G): not under G
    }
}

// ---------- IPC support (ipc.c): sessions, transfer memory and service-owned events ----------
// Weak fallbacks keep standalone thread tests linkable; the runtime links the real ipc.c versions.
__attribute__((weak)) void runtime_ipc_session_closed(void *s) { (void)s; die("IPC session closed without ipc.c"); }
__attribute__((weak)) void runtime_ipc_tmem_closed(void *t) { (void)t; die("transfer memory closed without ipc.c"); }
uint32_t runtime_handle_new(int session, void *obj) { pthread_mutex_lock(&G); uint32_t h = h_new(session ? O_SESSION : O_TMEM, obj); pthread_mutex_unlock(&G); return h; }
void *runtime_handle_get(uint32_t h, int session) { pthread_mutex_lock(&G); void *o = h_peek(h, session ? O_SESSION : O_TMEM); pthread_mutex_unlock(&G); return o; }
// Service-side event: returns the writable handle kept by the service.
uint32_t runtime_event_new(void) {
    event_t *e = calloc(1, sizeof *e); if (!e) die("event allocation"); e->refs = 1;
    pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e); pthread_mutex_unlock(&G); return hw;
}
// Copy-handle transfer of the readable end: each call gives the client its own handle to the same event.
uint32_t runtime_event_readable(uint32_t hw) {
    pthread_mutex_lock(&G); event_t *e = h_get(hw, O_EVENT_W); if (!e) die("service event handle %#x", hw);
    e->refs++; uint32_t hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G); return hr;
}
void runtime_event_signal(uint32_t hw) {
    pthread_mutex_lock(&G); event_t *e = h_get(hw, O_EVENT_W); if (!e) die("service event handle %#x", hw);
    e->signaled = 1; wake_sync_waiters(); pthread_mutex_unlock(&G);
}

// ---------- Diagnostics: TOTK_DUMP_AFTER=<seconds> prints every guest thread's wait state and guest return chain ----------
#include "mach_vm_compat.h"
#include <unistd.h>
__attribute__((weak)) void runtime_describe_address(uint64_t a, char *out, size_t n) { snprintf(out, n, "%#llx", (unsigned long long)a); }   // loader.c names modules
__attribute__((weak)) uint64_t runtime_account_lock_owner(void) { return 0; }   // account.c is not linked into the thread probe
__attribute__((weak)) void runtime_hle_describe(unsigned i, char *out, size_t n) { snprintf(out, n, "wrapper %u", i); }   // hle_stack.c names it
static void dump_chain(uint64_t lr, uint64_t fp) {
    char where[96]; runtime_describe_address(lr, where, sizeof where); fprintf(stderr, "    %s", where);
    for (int i = 0; i < 10 && fp && !(fp & 15); i++) {
        uint64_t f[2]; mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), fp, 16, (mach_vm_address_t)(uintptr_t)f, &got) != KERN_SUCCESS || got != 16) break;
        runtime_describe_address(f[1], where, sizeof where); fprintf(stderr, " <- %s", where);
        if (f[0] == fp) break; fp = f[0];   // frames may move between the guest and host stacks
    }
    fputc('\n', stderr);
}
static void dump_threads(unsigned secs);
static void *dump_after(void *arg) { unsigned secs = (unsigned)(uintptr_t)arg; sleep(secs); dump_threads(secs); return NULL; }
/* TOTK_LISTCHECK=<heap-relative object offset>: watch the two intrusive lists at +0x48/+0x60 of one game allocator
   object (seen corrupted after the startup logo) and dump all threads the moment a node's next link is null. */
uint64_t runtime_heap_base(void) __attribute__((weak));
uint64_t runtime_heap_base(void) { return 0; }
static void *list_check(void *arg) {
    uint64_t off = (uint64_t)(uintptr_t)arg;
    for (;;) {
        usleep(200);
        uint64_t base = runtime_heap_base(); if (!base) continue;
        const uint64_t obj = base + off;
        for (int l = 0; l < 2; l++) {
            uint64_t sentinel = obj + (l ? 0x60 : 0x48), head[2]; mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), sentinel, 16, (mach_vm_address_t)(uintptr_t)head, &got) != KERN_SUCCESS) continue;
            uint64_t node = head[1];
            for (int n = 0; n < 100000 && node && node != sentinel; n++) {
                uint64_t link[2];
                if (mach_vm_read_overwrite(mach_task_self(), node, 16, (mach_vm_address_t)(uintptr_t)link, &got) != KERN_SUCCESS) break;
                if (!link[1]) {
                    usleep(50); uint64_t again[2];
                    if (mach_vm_read_overwrite(mach_task_self(), node, 16, (mach_vm_address_t)(uintptr_t)again, &got) == KERN_SUCCESS && !again[1]) {
                        fprintf(stderr, "[totk-listcheck] list +%#x node %#llx has null next (prev %#llx)\n", l ? 0x60 : 0x48, (unsigned long long)node, (unsigned long long)again[0]);
                        dump_threads(0); return NULL;
                    }
                    break;
                }
                node = link[1];
            }
        }
    }
}
static void dump_threads(unsigned secs) {
    int locked = 0; for (int i = 0; i < 100 && !(locked = pthread_mutex_trylock(&G) == 0); i++) usleep(10000);
    fprintf(stderr, "[totk-dump] global thread lock %s\n", locked ? "acquired" : "HELD BY ANOTHER THREAD (dumping without it)");
    fprintf(stderr, "[totk-dump] guest threads after %u s\n", secs);
    { fprintf(stderr, "[totk-dump] account lock owner host tid %llu\n", (unsigned long long)runtime_account_lock_owner()); }
    for (int c = 0; c < NCORE; c++) fprintf(stderr, "[totk-dump] core %d owner %#x for %.1f ms\n", c, g_core_owner[c] ? g_core_owner[c]->handle : 0,
                                            g_core_owner[c] ? (now_ns() - g_core_owner[c]->core_since) / 1e6 : 0.0);
    for (thr_t *w = g_core_wait; w; w = w->cnext) fprintf(stderr, "[totk-dump] core waiter %#x prio %d mask %#llx ideal %d\n", w->handle, w->prio, (unsigned long long)w->mask, w->core);
#ifdef GAME_BOTW
    fprintf(stderr, "[totk-dump] preemption signals %llu, cores given up in the signal handler %llu, starvation grants %llu\n",
            (unsigned long long)g_preempt_kicks, (unsigned long long)g_preempt_async, (unsigned long long)g_starve_grants);
    for (thr_t *w = g_core_wait; w; w = w->cnext) if (eff_prio(w) != w->prio) fprintf(stderr, "[totk-dump] core waiter %#x runs at %d (owns a mutex a better thread waits for)\n", w->handle, eff_prio(w));
#endif
    thr_t *run[64]; int nrun = 0;
    for (thr_t *t = g_all; t; t = t->all) {
        uint32_t word = 0; if (t->wkind == 1 && t->waddr) memcpy(&word, (const void *)(uintptr_t)t->waddr, 4);
        uint64_t htid = 0; if (t->pt) pthread_threadid_np(t->pt, &htid);
        fprintf(stderr, "[totk-dump] tid=%llu ", (unsigned long long)htid);
        fprintf(stderr, "core=%d mask=%#llx ", t->cur_core, (unsigned long long)t->mask);
        fprintf(stderr, "h=%#x prio=%d %s wait=%d addr=%#llx key=%#llx tag=%#x word=%#x last_svc=%#x nsvc=%llu\n", t->handle, t->prio,
                t->exited ? "exited" : t->started ? "run" : "new", t->wkind, (unsigned long long)t->waddr, (unsigned long long)t->wkey, t->wtag, word, t->last_svc, (unsigned long long)t->nsvc);
        if (t->wkind == 4) { fprintf(stderr, "    waiting on %d handle(s):", t->nwh); for (int i = 0; i < t->nwh && i < 4; i++) fprintf(stderr, " %#x", t->wh[i]); fputc('\n', stderr); }
        if (t->last_hle) { char nm[256]; runtime_hle_describe((unsigned)t->last_hle - 1, nm, sizeof nm); fprintf(stderr, "    last host call while tracing: %s\n", nm); }
        dump_chain(t->last_lr, t->last_fp);
        if (!t->wkind && !t->exited && t->mport && nrun < 64) run[nrun++] = t;
    }
    if (locked) pthread_mutex_unlock(&G);
    for (int i = 0; i < nrun; i++) { thr_t *t = run[i];
#ifdef GAME_BOTW
            if (t->mport == pthread_mach_thread_np(pthread_self())) continue;   /* dumping from a crashing guest thread: never suspend ourselves */
#endif
            fprintf(stderr, "[totk-dump] live h=%#x\n", t->handle);
            for (int k = 0; k < 3; k++) {
                arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
                thread_suspend(t->mport);
                kern_return_t kr = thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt);
                thread_resume(t->mport);
                if (kr != KERN_SUCCESS) break;
                char a[96], b[96]; runtime_describe_address(arm_thread_state64_get_pc(st), a, sizeof a); runtime_describe_address(arm_thread_state64_get_lr(st), b, sizeof b);
                fprintf(stderr, "    live pc=%s lr=%s x0=%#llx x1=%#llx x8=%#llx\n", a, b, (unsigned long long)st.__x[0], (unsigned long long)st.__x[1], (unsigned long long)st.__x[8]);
                dump_chain(arm_thread_state64_get_lr(st), arm_thread_state64_get_fp(st));
                usleep(20000);
            }
    }
}
void runtime_dump_threads(void) { dump_threads(0); }   // TOTK_INPUT_FILE "threads"
__attribute__((constructor)) static void dump_init(void) {
    const char *c = getenv("TOTK_LISTCHECK");
    if (c) { pthread_t th; pthread_create(&th, NULL, list_check, (void *)(uintptr_t)strtoull(c, NULL, 0)); pthread_detach(th); }
    const char *e = getenv("TOTK_DUMP_AFTER"); if (!e) return;
    pthread_t th; pthread_create(&th, NULL, dump_after, (void *)(uintptr_t)strtoul(e, NULL, 10)); pthread_detach(th);
}
