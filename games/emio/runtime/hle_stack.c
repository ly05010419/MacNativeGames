/* Host functions handed to guest code are wrapped so they run on the guest thread's host stack (hle_stack.S).
   Diagnostics (TOTK_INPUT_FILE "hletrace <0|1|2>"): while tracing, every wrapped call is counted per function; in mode 2
   the first call of each function since tracing began is also logged with the time and the calling guest thread (so a
   window that starts on an idle screen shows, in order, what the game starts calling after an input); "hletrace 0"
   prints the functions called since tracing began, most called first. 2026-10-17 this is how a hang with every thread idle is narrowed to the last
   host call the game made (the game over "retry" that never reloads). */
#include "runtime.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HLE_THUNKS 4096
#define HLE_THUNK_BYTES 16
extern char runtime_hle_thunks[];
void *runtime_hle_targets[HLE_THUNKS];
uint64_t runtime_hle_tsd_off;
int runtime_hle_trace;                                   /* read by hle_stack.S on every wrapped call */
static const char *names[HLE_THUNKS];
static _Atomic uint64_t counts[HLE_THUNKS];
static uint64_t since[HLE_THUNKS];
static _Atomic unsigned char seen[HLE_THUNKS];
static uint64_t trace_t0;
static unsigned used;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void runtime_hle_stack_init(uint64_t tsd_offset) { runtime_hle_tsd_off = tsd_offset; }

/* Same host function -> same wrapper, so pointers the game compares stay consistent. The first name given is kept
   (the import or NVN entry the wrapper was made for; diagnostics only). */
void *runtime_hle_wrap_named(void *fn, const char *name) {
    if (!fn) return NULL;
    pthread_mutex_lock(&lock);
    unsigned i = 0;
    while (i < used && runtime_hle_targets[i] != fn) i++;
    if (i == used) {
        if (used == HLE_THUNKS) runtime_fail("host-stack wrapper table full");
        __atomic_store_n(&runtime_hle_targets[used++], fn, __ATOMIC_RELEASE);
    }
    if (!names[i] && name) names[i] = strdup(name);
    pthread_mutex_unlock(&lock);
    return runtime_hle_thunks + (size_t)i * HLE_THUNK_BYTES;
}
void *runtime_hle_wrap(void *fn) { return runtime_hle_wrap_named(fn, NULL); }

/* A readable name: the demangled import, else the host symbol. */
static void name_of(unsigned i, char *out, size_t cap) {
    const char *n = i < HLE_THUNKS ? names[i] : NULL;
    if (n) {
        typedef char *(*demangle_t)(const char *, char *, size_t *, int *);
        static demangle_t demangle; static int looked;
        if (!looked) { demangle = (demangle_t)dlsym(RTLD_DEFAULT, "__cxa_demangle"); looked = 1; }
        int status = -1; char *d = demangle && n[0] == '_' && n[1] == 'Z' ? demangle(n, NULL, NULL, &status) : NULL;
        snprintf(out, cap, "%s", d && !status ? d : n); free(d); return;
    }
    Dl_info info; void *fn = i < HLE_THUNKS ? runtime_hle_targets[i] : NULL;
    if (fn && dladdr(fn, &info) && info.dli_sname) snprintf(out, cap, "%s (host)", info.dli_sname);
    else snprintf(out, cap, "wrapper %u -> %p", i, fn);
}
const char *runtime_hle_raw_name(unsigned i) { return i < HLE_THUNKS ? names[i] : NULL; }
void runtime_hle_describe(unsigned i, char *out, size_t cap) { name_of(i, out, cap); }

/* hle_stack.S calls this before each wrapped call while tracing (on the host stack, arguments kept). */
void runtime_hle_note(uint64_t i) {
    if (i >= HLE_THUNKS) return;
    atomic_fetch_add_explicit(&counts[i], 1, memory_order_relaxed);
    runtime_thread_note_hle((unsigned)i);
    if (runtime_hle_trace != 2 || atomic_exchange_explicit(&seen[i], 1, memory_order_relaxed)) return;
    char text[256]; name_of((unsigned)i, text, sizeof text);
    fprintf(stderr, "[hle] %8.3f h=%#x %s\n", (runtime_now_ns() - trace_t0) / 1e9, runtime_thread_current_handle(), text);
}

typedef struct { uint64_t n; unsigned i; } hit_t;
static int by_count(const void *a, const void *b) {
    const hit_t *x = a, *y = b; return x->n < y->n ? 1 : x->n > y->n ? -1 : (int)x->i - (int)y->i;
}
void runtime_hle_trace_set(int mode) {
    if (mode < 0 || mode > 2) runtime_fail("hletrace takes 0, 1 or 2");
    if (mode) {
        for (unsigned i = 0; i < HLE_THUNKS; i++) since[i] = atomic_load_explicit(&counts[i], memory_order_relaxed);
        for (unsigned i = 0; i < HLE_THUNKS; i++) atomic_store_explicit(&seen[i], 0, memory_order_relaxed);
        trace_t0 = runtime_now_ns();
        __atomic_store_n(&runtime_hle_trace, mode, __ATOMIC_RELEASE);
        fprintf(stderr, "[hle] tracing on: counting every host call%s\n", mode == 2 ? ", logging the first call of each function" : "");
        return;
    }
    __atomic_store_n(&runtime_hle_trace, 0, __ATOMIC_RELEASE);
    static hit_t hits[HLE_THUNKS]; unsigned k = 0; uint64_t total = 0;
    for (unsigned i = 0; i < HLE_THUNKS; i++) {
        uint64_t d = atomic_load_explicit(&counts[i], memory_order_relaxed) - since[i];
        if (d) { hits[k].n = d; hits[k].i = i; k++; total += d; }
    }
    qsort(hits, k, sizeof *hits, by_count);
    fprintf(stderr, "[hle] tracing off after %.1f s: %llu calls to %u functions\n", (runtime_now_ns() - trace_t0) / 1e9, (unsigned long long)total, k);
    for (unsigned j = 0; j < k; j++) {
        char text[256]; name_of(hits[j].i, text, sizeof text);
        fprintf(stderr, "[hle] %10llu  %s\n", (unsigned long long)hits[j].n, text);
    }
}

/* Called on a guest thread's own host stack before it enters guest code; top must leave room for the caller's frame. */
void runtime_hle_stack_set(uintptr_t top) {
    uintptr_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp));
    if (!runtime_hle_tsd_off) runtime_fail("host-stack slot not initialised");
    *(volatile uintptr_t *)(tp + runtime_hle_tsd_off) = top & ~(uintptr_t)15;
}
