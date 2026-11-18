/* One-frame-chain timeline (see totk_timeline.h). */
#include "totk_timeline.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { uint64_t t, a, b; uint32_t kind, tid; uint64_t frame; } tl_ev;
enum { TL_CAP = 1 << 20 };
static tl_ev *g_ev;
static _Atomic uint64_t g_n, g_commit, g_frame;
static _Atomic unsigned g_writers;
static _Atomic int g_state = -1;   /* -1 unread, 0 off, 1 fixed window waiting, 2 recording, 3 closed, 4 trigger waiting, 5 publishing */
static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static uint64_t g_first, g_count;
static int g_gpu_only;
static char g_path[1024];

static void tl_init(void) {
    const char *e = getenv("TOTK_TIMELINE");
    int st = 0;
    if (e && *e) {
        g_gpu_only = getenv("TOTK_TIMELINE_GPU_ONLY") && atoi(getenv("TOTK_TIMELINE_GPU_ONLY"));
        const char *c1 = strrchr(e, ':');
        const char *c0 = c1 && c1 > e ? c1 - 1 : NULL;
        while (c0 && c0 > e && *c0 != ':') c0--;
        if (c1 && c1 > e && !strcmp(c1 + 1, "trigger") && (size_t)(c1 - e) < sizeof g_path) {
            memcpy(g_path, e, (size_t)(c1 - e)); g_path[c1 - e] = 0;
            g_ev = calloc(TL_CAP, sizeof *g_ev);
            if (g_ev) st = 4;
        } else if (c1 && c0 && c0 > e && *c0 == ':' && (size_t)(c0 - e) < sizeof g_path) {
            memcpy(g_path, e, (size_t)(c0 - e)); g_path[c0 - e] = 0;
            g_first = strtoull(c0 + 1, NULL, 10); g_count = strtoull(c1 + 1, NULL, 10);
            g_ev = calloc(TL_CAP, sizeof *g_ev);
            if (g_ev && g_count && g_count <= UINT64_MAX - 30 && g_first <= UINT64_MAX - g_count - 30) st = 1;
        }
        if (!st) fprintf(stderr, "[timeline] TOTK_TIMELINE=%s 读不懂或无法分配（要 <文件>:<首帧>:<帧数> 或 <文件>:trigger），不记录\n", e);
        else if (st == 4) fprintf(stderr, "[timeline] 等待 pass_gpu 触发 → %s\n", g_path);
        else fprintf(stderr, "[timeline] 记录第 %llu 帧起 %llu 帧的时间线到 %s\n", (unsigned long long)g_first, (unsigned long long)g_count, g_path);
    }
    atomic_store(&g_state, st);
}
int totk_tl_on(void) {
    int s = atomic_load(&g_state);
    if (s < 0) { pthread_once(&g_init, tl_init); s = atomic_load(&g_state); }
    return s == 2;
}
int totk_tl_trigger(uint64_t first, uint64_t count) {
    totk_tl_on();
    if (!count || count > UINT64_MAX - 30 || first > UINT64_MAX - count - 30) return 0;
    int want = 4;
    if (!atomic_compare_exchange_strong(&g_state, &want, 5)) return 0;
    g_first = first; g_count = count;
    /* Begin at the request's submit boundary so the first sampled submission
       cannot precede its timeline. Only this thread publishes the range. */
    atomic_store(&g_state, 2);
    fprintf(stderr, "[timeline] triggered first=%llu count=%llu now_ns=%llu → %s\n",
            (unsigned long long)first, (unsigned long long)count,
            (unsigned long long)clock_gettime_nsec_np(CLOCK_UPTIME_RAW), g_path);
    return 1;
}
void totk_tl_at_frame(int kind, uint64_t t_ns, uint64_t frame, uint64_t a, uint64_t b) {
    if (!totk_tl_on()) return;
    if (g_gpu_only && kind >= TL_GW_IN && kind <= TL_CW_OUT) return;
    atomic_fetch_add(&g_writers, 1);
    if (atomic_load(&g_state) != 2) { atomic_fetch_sub(&g_writers, 1); return; }
    uint64_t i = atomic_fetch_add_explicit(&g_n, 1, memory_order_relaxed);
    if (i < TL_CAP) {
        uint64_t tid; pthread_threadid_np(NULL, &tid);
        g_ev[i] = (tl_ev){t_ns, a, b, (uint32_t)kind, (uint32_t)tid, frame};
    }
    atomic_fetch_sub(&g_writers, 1);
}
void totk_tl_at(int kind, uint64_t t_ns, uint64_t a, uint64_t b) {
    totk_tl_at_frame(kind, t_ns, atomic_load_explicit(&g_frame, memory_order_relaxed), a, b);
}
void totk_tl(int kind, uint64_t a, uint64_t b) {
    if (!totk_tl_on()) return;
    totk_tl_at(kind, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), a, b);
}
uint64_t totk_tl_commit_id(void) { return atomic_fetch_add(&g_commit, 1) + 1; }

static void tl_write(void) {
    /* Closing may race a Metal feedback writer which already reserved a row.
       Never serialize that row before the writer has filled it. */
    while (atomic_load(&g_writers)) sched_yield();
    FILE *f = fopen(g_path, "w");
    if (!f) { fprintf(stderr, "[timeline] 写不了 %s\n", g_path); return; }
    uint64_t reserved = atomic_load(&g_n), n = reserved; if (n > TL_CAP) n = TL_CAP;
    fprintf(f, "# t_ns\tkind\ttid\tframe\ta\tb\n");
    fprintf(f, "# first=%llu count=%llu dropped=%llu\n", (unsigned long long)g_first,
            (unsigned long long)g_count, (unsigned long long)(reserved - n));
    static const char *names[] = {"?", "sub_in", "sub_out", "rep_begin", "rep_end", "commit", "gpu", "wait_in", "wait_out",
                                  "acq_in", "acq_out", "pres_in", "pres_out", "finish_in", "finish_out", "fence", "rs_join",
                                  "pass_begin", "pass_end", "gw_in", "gw_out", "gwake", "cw_in", "cw_out", "wb_in", "wb_out", "wb_step"};
    for (uint64_t i = 0; i < n; i++) {
        const tl_ev *e = &g_ev[i];
        fprintf(f, "%llu\t%s\t%u\t%llu\t%llu\t%llu\n", (unsigned long long)e->t, e->kind < sizeof names / sizeof *names ? names[e->kind] : "?",
                e->tid, (unsigned long long)e->frame, (unsigned long long)e->a, (unsigned long long)e->b);
    }
    fclose(f);
    fprintf(stderr, "[timeline] 写出 %llu 个事件到 %s\n", (unsigned long long)n, g_path);
}
void totk_tl_frame(uint64_t presents) {
    totk_tl_on();
    atomic_store(&g_frame, presents);
    int s = atomic_load(&g_state);
    if (s == 1 && presents >= g_first) atomic_store(&g_state, 2);
    /* GPU feedback of the last frames arrives after their presents: keep recording 30 more frames, then write */
    else if (s == 2 && presents >= g_first + g_count + 30) {
        int want = 2;
        if (atomic_compare_exchange_strong(&g_state, &want, 3)) tl_write();
    }
}
