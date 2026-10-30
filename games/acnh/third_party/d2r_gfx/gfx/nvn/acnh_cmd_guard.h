/* Animal Crossing: New Horizons only -- command buffer guards, included by hle_nvn.c after live_seg_remove.
   The shared command buffer code calls the acnh_* hooks below; for every other game they are no-ops (the #else
   branch at the end), so hle_nvn.c behaves exactly as before for them.

   Why (2026-10-25, three crashes on the same game path, main+0x1bf418c -> main+0x1adaa1c -> EndRecording):
   the game keeps a command buffer per slot (main+0x1bf40d0: slot byte & 3, 0x260 apart) in memory on a thread's stack,
   and worker threads run Initialize / Begin / record / End on it. EndRecording found its segment NULL (run 1), its write
   pointer NULL (run 2) and its segment pointer garbage (run 3, 0x4349523ec22d6f91). The cause is not known yet:
   - g_seg_mu: a segment is never freed (Initialize / Finalize), moved (cmd_reserve) or flattened (EndRecording) at the
     same time, and EndRecording hands back an empty recording if its segment was released during the call;
   - recorder ownership: BeginRecording claims the command buffer for its thread until EndRecording; Initialize, Finalize
     and BeginRecording from another thread wait for that (virtual core given up), without taking over an active recording;
   - ACNH_STACKCB=1: recordings into a command buffer on another thread's stack, the moment that thread leaves the frame,
     and both threads' recent core events (diagnostics only);
   - TOTK_CB_CHECK=1: each record, BeginRecording and the flatten in EndRecording check that the host fields still make
     sense and log the first violations with every field, the thread and the game code that called (diagnostics only). */
#ifndef ACNH_CMD_GUARD_H
#define ACNH_CMD_GUARD_H
#ifdef GAME_ACNH
#include <limits.h>

static uint8_t *flatten_recording(const uint8_t *p);
static void guest_chain(const char *what);
uint32_t runtime_thread_current_handle(void);
void runtime_core_back(int *away);

// ---------- ACNH_STACKCB=1: command buffers recorded on another thread's stack (runtime/acnh_thread_diag.h) ----------
/* A recording into a command buffer that lives on another thread's stack is logged with where that thread is; a 1 ms
   watcher logs the first moment the owner's guest SP is above the command buffer (it left the frame holding it) while the
   recording is still open, and EndRecording says whether that happened. Both threads' recent core events are logged
   with it. Diagnostics only: the owner is suspended for a moment to read its PC / SP. */
uint32_t acnh_stack_owner(uint64_t addr);
int acnh_thread_sample(uint32_t h, uint64_t *pc, uint64_t *gsp, int *core);
void acnh_thread_ring_dump(uint32_t h, const char *tag);
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);
int runtime_current_core(void);
static int acnh_scb_on(void) { static int on = -1; if (on < 0) on = getenv("ACNH_STACKCB") != NULL; return on; }
#define ACNH_SCB_MAX 64
static struct { const void *cb; uint32_t owner, rec; uint64_t t0, sp0, left_ns; int used; } g_scb[ACNH_SCB_MAX];
static pthread_mutex_t g_scb_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_scb_n, g_scb_left;
static void acnh_scb_where(uint64_t pc, char *out, size_t n) {
    const char *name; uint64_t off;
    if (runtime_module_offset(pc, &name, &off)) snprintf(out, n, "%s+%#llx", name, (unsigned long long)off); else snprintf(out, n, "host %#llx", (unsigned long long)pc);
}
static uint64_t acnh_scb_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void acnh_scb_left(int i, const char *who) {   /* g_scb_mu held; the owner's SP is above the command buffer */
    uint64_t pc = 0, sp = 0; int core = -1; acnh_thread_sample(g_scb[i].owner, &pc, &sp, &core);
    char at[64]; acnh_scb_where(pc, at, sizeof at); uint64_t n = atomic_fetch_add(&g_scb_left, 1);
    if (n < 20) {
        LOG("【红】[stackcb] %s: thread %#x left the frame holding cb=%p %.3f ms after thread %#x began recording into it, still open; owner now at %s sp %#llx core %d (sp at Begin %#llx)",
            who, g_scb[i].owner, g_scb[i].cb, (double)(acnh_scb_now() - g_scb[i].t0) / 1e6, g_scb[i].rec, at, (unsigned long long)sp, core, (unsigned long long)g_scb[i].sp0);
        acnh_thread_ring_dump(g_scb[i].owner, "owner"); acnh_thread_ring_dump(g_scb[i].rec, "recorder");
    }
}
static void *acnh_scb_watch(void *arg) {
    (void)arg;
    for (;;) {
        struct timespec d = {0, 1000000}; nanosleep(&d, NULL);
        pthread_mutex_lock(&g_scb_mu);
        for (int i = 0; i < ACNH_SCB_MAX; i++) if (g_scb[i].used && !g_scb[i].left_ns) {
            uint64_t pc, sp; int core;
            if (acnh_thread_sample(g_scb[i].owner, &pc, &sp, &core) && sp > (uint64_t)(uintptr_t)g_scb[i].cb) { g_scb[i].left_ns = acnh_scb_now(); acnh_scb_left(i, "watcher"); }
        }
        pthread_mutex_unlock(&g_scb_mu);
    }
    return NULL;
}
static void acnh_scb_foreign(const void *cb, const char *what);   /* below the recorder table */
static void acnh_scb_start(void) { pthread_t w; if (!pthread_create(&w, NULL, acnh_scb_watch, NULL)) pthread_detach(w); }
static void acnh_scb_begin(const void *cb) {   /* BeginRecording, after the claim */
    if (!acnh_scb_on()) return;
    uint32_t me = runtime_thread_current_handle(), owner = acnh_stack_owner((uint64_t)(uintptr_t)cb);
    if (!owner || owner == me) return;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, acnh_scb_start);
    uint64_t pc = 0, sp = 0; int core = -1; acnh_thread_sample(owner, &pc, &sp, &core);
    uint64_t n = atomic_fetch_add(&g_scb_n, 1);
    if (n < 40 || n % 1000 == 0) { char at[64]; acnh_scb_where(pc, at, sizeof at);
        LOG("[stackcb] Begin #%llu: thread %#x (core %d) records into cb=%p on the stack of thread %#x, now at %s sp %#llx core %d%s; owner left the frame %llu times so far",
            (unsigned long long)n, me, runtime_current_core(), cb, owner, at, (unsigned long long)sp, core, sp > (uint64_t)(uintptr_t)cb ? " -- ALREADY ABOVE IT" : "", (unsigned long long)atomic_load(&g_scb_left)); }
    pthread_mutex_lock(&g_scb_mu);
    int i = 0; while (i < ACNH_SCB_MAX && g_scb[i].used) i++;
    if (i < ACNH_SCB_MAX) g_scb[i] = (__typeof__(g_scb[0])){cb, owner, me, acnh_scb_now(), sp, 0, 1};
    pthread_mutex_unlock(&g_scb_mu);
}
static void acnh_scb_end(const void *cb) {   /* EndRecording, before the recording is flattened */
    if (!acnh_scb_on()) return;
    acnh_scb_foreign(cb, "End");
    uint32_t me = runtime_thread_current_handle();
    pthread_mutex_lock(&g_scb_mu);
    for (int i = 0; i < ACNH_SCB_MAX; i++) if (g_scb[i].used && g_scb[i].cb == cb && g_scb[i].rec == me) {
        uint64_t pc, sp; int core;
        if (g_scb[i].left_ns) { static _Atomic int said; if (atomic_fetch_add(&said, 1) < 20)
            LOG("【红】[stackcb] End: thread %#x ends cb=%p %.3f ms after its owner %#x left that frame", me, cb, (double)(acnh_scb_now() - g_scb[i].left_ns) / 1e6, g_scb[i].owner); }
        else if (acnh_thread_sample(g_scb[i].owner, &pc, &sp, &core) && sp > (uint64_t)(uintptr_t)cb) { g_scb[i].left_ns = acnh_scb_now(); acnh_scb_left(i, "End"); }
        g_scb[i].used = 0; break;
    }
    pthread_mutex_unlock(&g_scb_mu);
}
static void acnh_scb_bad(const void *cb) {   /* TOTK_CB_CHECK found the command buffer overwritten */
    if (!acnh_scb_on()) return;
    uint32_t owner = acnh_stack_owner((uint64_t)(uintptr_t)cb);
    LOG("[stackcb] the overwritten cb=%p is on the stack of thread %#x", cb, owner);
    if (owner) acnh_thread_ring_dump(owner, "owner");
    acnh_thread_ring_dump(runtime_thread_current_handle(), "recorder");
}

// ---------- TOTK_CB_CHECK=1 diagnostics (read our own fields only, never through them) ----------
#define ACNH_CB_SEEN 4096
static struct { const cmd_t *o; uint8_t *cur; } g_acnh_cb_seen[ACNH_CB_SEEN];   /* the write pointer after our own last change */
static pthread_mutex_t g_acnh_cb_seen_mu = PTHREAD_MUTEX_INITIALIZER;
static int acnh_cb_check_on(void) { static int on = -1; if (on < 0) on = getenv("TOTK_CB_CHECK") != NULL; return on; }
static void acnh_cb_seen(const cmd_t *o) {
    if (!acnh_cb_check_on()) return;
    unsigned i = (unsigned)(((uintptr_t)o >> 5) % ACNH_CB_SEEN);
    pthread_mutex_lock(&g_acnh_cb_seen_mu); g_acnh_cb_seen[i].o = o; g_acnh_cb_seen[i].cur = o->cur; pthread_mutex_unlock(&g_acnh_cb_seen_mu);
}
static void acnh_cb_check(const cmd_t *o, const char *where) {
    if (!acnh_cb_check_on()) return;
    unsigned i = (unsigned)(((uintptr_t)o >> 5) % ACNH_CB_SEEN);
    pthread_mutex_lock(&g_acnh_cb_seen_mu); int moved = g_acnh_cb_seen[i].o == o && g_acnh_cb_seen[i].cur != o->cur; uint8_t *was = g_acnh_cb_seen[i].cur; pthread_mutex_unlock(&g_acnh_cb_seen_mu);
    if (moved) { static _Atomic int said_m; if (atomic_fetch_add(&said_m, 1) < 12) {
        LOG("【红】[cb-check] %s: the write pointer of command buffer %p moved from %p to %p outside our code (%+lld bytes), thread %#x",
            where, (const void *)o, (void *)was, (void *)o->cur, (long long)(o->cur - was), runtime_thread_current_handle());
        guest_chain("cb-check moved"); } }
    int bad = o->inline_cur != ~0ull || o->inline_end != 0 || (o->seg && (o->cur < o->seg || o->cur > o->end));
    static _Atomic int said;
    if (bad && atomic_fetch_add(&said, 1) < 12) {
        const uint64_t *w = (const uint64_t *)o; char words[sizeof(cmd_t) / 8 * 17 + 1]; size_t k = 0;   /* every host field: which ones were overwritten */
        for (size_t j = 0; j < sizeof(cmd_t) / 8; j++) k += snprintf(words + k, sizeof words - k, " %016llx", (unsigned long long)w[j]);
        LOG("【红】[cb-check] %s: command buffer %p seg %p cur %p end %p flags10 %#llx inline %#llx/%#llx recording %u, thread %#x; words%s",
            where, (const void *)o, (void *)o->seg, (void *)o->cur, (void *)o->end, (unsigned long long)o->guest_flags, (unsigned long long)o->inline_cur, (unsigned long long)o->inline_end, o->recording,
            runtime_thread_current_handle(), words);
        guest_chain("cb-check");
        acnh_scb_bad(o);
    }
}

// ---------- segment lifetime ----------
static pthread_mutex_t g_acnh_seg_mu = PTHREAD_MUTEX_INITIALIZER;
static void acnh_seg_lock(void) { pthread_mutex_lock(&g_acnh_seg_mu); }
static void acnh_seg_unlock(void) { pthread_mutex_unlock(&g_acnh_seg_mu); }
/* cmd_reserve, lock held: a concurrent Finalize may have dropped the segment since its fast-path check -- start a fresh one then */
static size_t acnh_seg_used(const cmd_t *o) { return o->seg && o->cur >= o->seg && o->cur <= o->end ? (size_t)(o->cur - o->seg) : 0; }
/* EndRecording: the recording's copy, flattened with the segment held */
static uint8_t *acnh_flatten(cmd_t *o, const NVNcommandBuffer *c) {
    uint8_t *copy;
    acnh_scb_end(c);
    acnh_seg_lock();
    acnh_cb_check(o, "end");   /* where the third crash read a garbage segment */
    if (o->seg) copy = flatten_recording(o->seg);
    else {   /* finalized / re-initialized by another thread since the END record went in: hand back an empty recording */
        op_hdr empty[2] = {{OP_BEGIN, 8}, {OP_END, 8}};
        copy = flatten_recording((const uint8_t *)empty);
        static _Atomic int said; if (atomic_fetch_add(&said, 1) < 8) LOG("【红】[cmd] EndRecording cb=%p: its segment was released during the call; returning an empty recording", (const void *)c);
    }
    acnh_seg_unlock();
    return copy;
}

// ---------- one recorder per command buffer ----------
#define ACNH_REC_INITIAL_CAP 256
static struct rec_owner { const void *cb; pthread_t owner; uint32_t handle; } *g_acnh_rec;
static int g_acnh_rec_n, g_acnh_rec_cap;
static pthread_mutex_t g_acnh_rec_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_acnh_rec_cv = PTHREAD_COND_INITIALIZER;
static int acnh_rec_find(const void *cb) { for (int i = 0; i < g_acnh_rec_n; i++) if (g_acnh_rec[i].cb == cb) return i; return -1; }
static int acnh_rec_other(int i) { return i >= 0 && !pthread_equal(g_acnh_rec[i].owner, pthread_self()); }
static void acnh_rec_drop(int i) { g_acnh_rec[i] = g_acnh_rec[--g_acnh_rec_n]; pthread_cond_broadcast(&g_acnh_rec_cv); }
static void acnh_rec_reserve(void) {
    if (g_acnh_rec_n < g_acnh_rec_cap) return;
    if (g_acnh_rec_cap > INT_MAX / 2) { LOG("[cmd] recorder ownership table capacity overflow"); abort(); }
    int cap = g_acnh_rec_cap ? g_acnh_rec_cap * 2 : ACNH_REC_INITIAL_CAP;
    void *p = realloc(g_acnh_rec, (size_t)cap * sizeof *g_acnh_rec);
    if (!p) { LOG("[cmd] cannot grow recorder ownership table"); abort(); }
    g_acnh_rec = p;
    g_acnh_rec_cap = cap;
}
/* g_acnh_rec_mu held (dropped while waiting): the command buffer's entry, owned by this thread, or -1 */
static int acnh_rec_wait_locked(const void *cb, const char *what) {
    int i = acnh_rec_find(cb);
    if (!acnh_rec_other(i)) return i;
    uint32_t them = g_acnh_rec[i].handle; uint64_t t0 = mach_absolute_time();
    while (acnh_rec_other(i)) {
        pthread_mutex_unlock(&g_acnh_rec_mu);
        int away = runtime_core_away();
        pthread_mutex_lock(&g_acnh_rec_mu);
        while (acnh_rec_other(i = acnh_rec_find(cb))) pthread_cond_wait(&g_acnh_rec_cv, &g_acnh_rec_mu);
        pthread_mutex_unlock(&g_acnh_rec_mu);
        runtime_core_back(&away);   /* no host lock held while taking a core back */
        pthread_mutex_lock(&g_acnh_rec_mu);
        i = acnh_rec_find(cb);
    }
    static _Atomic int said; if (atomic_fetch_add(&said, 1) < 16) {
        mach_timebase_info_data_t tb; mach_timebase_info(&tb);
        LOG("[cmd] %s cb=%p by thread %#x waited %.3f ms for thread %#x to end its recording",
            what, cb, runtime_thread_current_handle(), (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6, them);
    }
    return i;
}
static void acnh_rec_claim(const void *cb) {   /* BeginRecording */
    pthread_mutex_lock(&g_acnh_rec_mu);
    int i = acnh_rec_wait_locked(cb, "BeginRecording");
    if (i < 0) { acnh_rec_reserve(); i = g_acnh_rec_n++; }
    g_acnh_rec[i] = (struct rec_owner){cb, pthread_self(), runtime_thread_current_handle()};
    pthread_mutex_unlock(&g_acnh_rec_mu);
    acnh_scb_begin(cb);
}
static void acnh_rec_release(const void *cb) {   /* EndRecording */
    pthread_mutex_lock(&g_acnh_rec_mu);
    int i = acnh_rec_find(cb); if (i >= 0 && !acnh_rec_other(i)) acnh_rec_drop(i);
    pthread_mutex_unlock(&g_acnh_rec_mu);
}

/* ACNH_STACKCB: End or Finalize by a thread whose stack does not hold the command buffer -- where the stack's owner is,
   which thread holds the recording, and both threads' recent core events. */
static void acnh_scb_foreign(const void *cb, const char *what) {
    if (!acnh_scb_on()) return;
    uint32_t me = runtime_thread_current_handle(), owner = acnh_stack_owner((uint64_t)(uintptr_t)cb);
    if (!owner || owner == me) return;
    pthread_mutex_lock(&g_acnh_rec_mu); int r = acnh_rec_find(cb); uint32_t rec = r >= 0 ? g_acnh_rec[r].handle : 0; pthread_mutex_unlock(&g_acnh_rec_mu);
    static _Atomic uint64_t n_all; uint64_t n = atomic_fetch_add(&n_all, 1);
    if (n >= 40 && n % 1000) return;
    uint64_t pc = 0, sp = 0; int core = -1; acnh_thread_sample(owner, &pc, &sp, &core);
    char at[64]; acnh_scb_where(pc, at, sizeof at);
    LOG("%s[stackcb] %s #%llu: thread %#x (core %d) on cb=%p in the stack of thread %#x, which is now at %s sp %#llx core %d (%s); the recording is held by %s%#x",
        rec && rec != me ? "【红】" : "", what, (unsigned long long)n, me, runtime_current_core(), cb, owner, at, (unsigned long long)sp, core,
        sp > (uint64_t)(uintptr_t)cb ? "above it: left that frame" : "still below it: the frame is live", rec ? "thread " : "nobody ", rec);
    if (n < 20) { acnh_thread_ring_dump(owner, "stack owner"); acnh_thread_ring_dump(me, what); }
}

// ---------- Initialize / Finalize ----------
/* Waits for another thread's recording, forgets this thread's, then holds the segment lock until acnh_teardown_end. */
static void acnh_teardown_begin(const NVNcommandBuffer *c, const char *what) {
    if (c && !strcmp(what, "Finalize")) acnh_scb_foreign(c, what);
    if (c) { pthread_mutex_lock(&g_acnh_rec_mu); int i = acnh_rec_wait_locked(c, what); if (i >= 0) acnh_rec_drop(i); pthread_mutex_unlock(&g_acnh_rec_mu); }
    acnh_seg_lock();
    const cmd_t *o = (const cmd_t *)c;
    if (o && o->magic == MAGIC('C', 'M', 'D', 'B') && o->recording) {
        static _Atomic int said; if (atomic_fetch_add(&said, 1) < 8) { LOG("【红】[cmd] %s on command buffer %p while it is recording", what, (const void *)o); guest_chain(what); }
    }
}
static void acnh_teardown_end(void) { acnh_seg_unlock(); }

#else   /* every other game: the hooks do nothing */
static inline void acnh_cb_seen(const cmd_t *o) { (void)o; }
static inline void acnh_cb_check(const cmd_t *o, const char *where) { (void)o; (void)where; }
static inline void acnh_seg_lock(void) {}
static inline void acnh_seg_unlock(void) {}
static inline size_t acnh_seg_used(const cmd_t *o) { return o->seg ? (size_t)(o->cur - o->seg) : 0; }
#define acnh_flatten(o, c) flatten_recording((o)->seg)
static inline void acnh_rec_claim(const void *cb) { (void)cb; }
static inline void acnh_rec_release(const void *cb) { (void)cb; }
static inline void acnh_teardown_begin(const NVNcommandBuffer *c, const char *what) { (void)c; (void)what; }
static inline void acnh_teardown_end(void) {}
#endif
#endif
