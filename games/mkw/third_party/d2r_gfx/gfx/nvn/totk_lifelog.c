// TOTK diagnostic DM_NVN_LIFELOG: see totk_lifelog.h.
#include "totk_lifelog.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern uint64_t g_frame_pub;

enum { LIFE_IDS = 1 << 20, LIFE_CBS = 64, LIFE_HANDLES = 256 };
static int g_on = -1, g_fmt = -1;
static unsigned g_w, g_h;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t *g_reg;                              // id -> serial of the latest matching registration
static const void *g_cb[LIFE_CBS]; static unsigned g_ncb;
static uint64_t g_handle[LIFE_HANDLES]; static unsigned g_nhandle;

int totk_life_on(void) {
    if (g_on < 0) {
        const char *e = getenv("DM_NVN_LIFELOG"); unsigned f = 0; unsigned w = 0, h = 0;
        int n = e ? sscanf(e, "%ux%u:%x", &w, &h, &f) : 0;
        g_w = w; g_h = h; g_fmt = n == 3 ? (int)f : -1;
        if (n >= 2 && !(g_reg = calloc(LIFE_IDS, sizeof *g_reg))) { fprintf(stderr, "[life] cannot allocate the registration shadow\n"); abort(); }
        if (n >= 2) fprintf(stderr, "[life] lifecycle log on: %ux%u format %s%x\n", w, h, g_fmt < 0 ? "any " : "0x", g_fmt < 0 ? 0 : (unsigned)g_fmt);
        g_on = n >= 2;
    }
    return g_on;
}
int totk_life_match(const tex_t *t) {
    return totk_life_on() && t && t->d.width == g_w && t->d.height == g_h && (g_fmt < 0 || (int)t->d.format == g_fmt);
}
void totk_life_log(const char *fmt, ...) {
    char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    uint64_t th = 0; pthread_threadid_np(NULL, &th);
    fprintf(stderr, "[life] t=%.6f th=%llu fp=%llu %s\n", (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9,
            (unsigned long long)th, (unsigned long long)g_frame_pub, b);
}
void totk_life_reg(int id, const tex_t *t) {
    if (!totk_life_on() || id < 0 || id >= LIFE_IDS) return;
    uint64_t was = __atomic_load_n(&g_reg[id], __ATOMIC_RELAXED);
    if (totk_life_match(t)) {
        __atomic_store_n(&g_reg[id], t->serial, __ATOMIC_RELAXED);
        totk_life_log("reg id=%d ser=%llu mem=%p tex_was=%llu", id, (unsigned long long)t->serial, (void *)t->mem, (unsigned long long)was);
    } else if (was) {
        __atomic_store_n(&g_reg[id], 0, __ATOMIC_RELAXED);
        totk_life_log("reg id=%d now ser=%llu fmt=%#x %ux%u mem=%p (was matching ser=%llu)", id, t ? (unsigned long long)t->serial : 0ull,
                      t ? t->d.format : 0, t ? t->d.width : 0, t ? t->d.height : 0, t ? (void *)t->mem : NULL, (unsigned long long)was);
    }
}
uint64_t totk_life_reg_serial(int id) {
    return totk_life_on() && id >= 0 && id < LIFE_IDS ? __atomic_load_n(&g_reg[id], __ATOMIC_RELAXED) : 0;
}
void totk_life_cb_mark(const void *cb) {
    pthread_mutex_lock(&g_mu);
    unsigned i = 0; while (i < g_ncb && g_cb[i] != cb) i++;
    if (i == g_ncb && g_ncb < LIFE_CBS) g_cb[g_ncb++] = cb;
    pthread_mutex_unlock(&g_mu);
}
int totk_life_cb_take(const void *cb) {
    pthread_mutex_lock(&g_mu);
    unsigned i = 0; while (i < g_ncb && g_cb[i] != cb) i++;
    int found = i < g_ncb; if (found) g_cb[i] = g_cb[--g_ncb];
    pthread_mutex_unlock(&g_mu);
    return found;
}
void totk_life_handle_add(uint64_t h) {
    pthread_mutex_lock(&g_mu); g_handle[g_nhandle++ % LIFE_HANDLES] = h; pthread_mutex_unlock(&g_mu);
}
int totk_life_handle_has(uint64_t h) {
    pthread_mutex_lock(&g_mu);
    int found = 0; for (unsigned i = 0; i < LIFE_HANDLES && !found; i++) found = g_handle[i] == h;
    pthread_mutex_unlock(&g_mu);
    return found;
}
