/* The SVC dispatcher of this port: threads and synchronisation (shared/runtime/threads.c), IPC (shared/runtime/ipc.c),
   memory (svc_memory.c), and the few left (process exit, break, debug output). An unknown SVC stops the game with its
   number and caller. */
#include "hb.h"
#include <stdatomic.h>
#include <unistd.h>

void hb_services_init(void) {
    static int done; if (done) return; done = 1;
    hb_system_register(); hb_applet_register(); hb_hid_register(); hb_fs_register(); hb_display_register();
    /* audout:u registers itself (shared/runtime/audio_out.c). */
}

/* HB_TRACE_IPC=1: every request's handle and command id (CMIF, domain or not), read before ipc.c answers it. */
static int hb_trace_ipc(void) { static int v = -1; if (v < 0) v = getenv("HB_TRACE_IPC") || getenv("HB_IPC_STATS"); return v; }
/* HB_IPC_STATS=1: requests per (handle, command) counted, the busiest printed every 2 s (cheap enough not to change timing). */
#define STATS 512
static _Atomic uint64_t stat_count[STATS]; static _Atomic uint32_t stat_key[STATS];
static void stat_note(uint32_t key) {
    for (unsigned i = key * 2654435761u % STATS, n = 0; n < STATS; i = (i + 1) % STATS, n++) {
        uint32_t k = atomic_load(&stat_key[i]);
        if (k == key || (!k && atomic_compare_exchange_strong(&stat_key[i], &k, key)) || k == key) { atomic_fetch_add(&stat_count[i], 1); return; }
    }
}
static void *stat_thread(void *unused) {
    (void)unused;
    for (;;) {
        sleep(2); uint64_t top[5] = {0}; uint32_t key[5] = {0};
        for (unsigned i = 0; i < STATS; i++) { uint64_t c = atomic_exchange(&stat_count[i], 0); uint32_t k = atomic_load(&stat_key[i]);
            for (unsigned j = 0; j < 5; j++) if (c > top[j]) { memmove(top + j + 1, top + j, (4 - j) * 8); memmove(key + j + 1, key + j, (4 - j) * 4); top[j] = c; key[j] = k; break; } }
        fprintf(stderr, "[hb-ipc] 2 s:"); for (unsigned j = 0; j < 5 && top[j]; j++) fprintf(stderr, " %#x/%u x%llu", key[j] >> 16, key[j] & 0xffff, (unsigned long long)top[j]); fputc('\n', stderr);
    }
    return NULL;
}
static void hb_trace_request(uint32_t handle) {
    const uint32_t *m = pthread_getspecific(runtime_keys[2]); if (!m) return;
    unsigned type = m[0] & 0xffff, nx = (m[0] >> 16) & 15, nabw = ((m[0] >> 20) & 15) + ((m[0] >> 24) & 15) + ((m[0] >> 28) & 15);
    const uint32_t *p = m + 2;
    if (m[1] >> 31) { uint32_t h = *p++; p += (h & 1) * 2 + ((h >> 1) & 15) + ((h >> 5) & 15); }
    p += nx * 2 + nabw * 3;
    const uint8_t *payload = (const uint8_t *)m + ((((const uint8_t *)p - (const uint8_t *)m) + 15) & ~15);
    uint32_t h[4]; memcpy(h, payload, 16);
    uint32_t object = 0;
    if (h[0] != 0x49434653u) { memcpy(h, payload + 16, 16); object = ((const uint32_t *)payload)[1]; }
    static int stats = -1; if (stats < 0) { stats = getenv("HB_IPC_STATS") != NULL; if (stats) { pthread_t t; pthread_create(&t, NULL, stat_thread, NULL); pthread_detach(t); } }
    if (stats) { stat_note((handle & 0xfff) << 20 | object << 16 | (h[2] & 0xffff)); return; }
    if (object) fprintf(stderr, "[hb-ipc] handle %#x type %u domain object %u cmd %u\n", handle, type, object, h[2]);
    else fprintf(stderr, "[hb-ipc] handle %#x type %u cmd %u\n", handle, type, h[2]);
}
uint64_t runtime_svc(unsigned num, uint64_t *r, const char *module, uint64_t offset) {
    static _Atomic int trace = -1; int enabled = atomic_load_explicit(&trace, memory_order_relaxed);
    if (enabled < 0) { enabled = getenv("TOTK_TRACE_SVC") != NULL; atomic_store_explicit(&trace, enabled, memory_order_relaxed); }
    if (enabled) fprintf(stderr, "[totk] SVC 0x%x at %s+0x%llx x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n", num, module, (unsigned long long)offset,
                         (unsigned long long)r[0], (unsigned long long)r[1], (unsigned long long)r[2], (unsigned long long)r[3]);
    if (num == 0x21 && hb_trace_ipc()) hb_trace_request((uint32_t)r[0]);
    if (svc_thread((int)num, r)) return r[0];
    if (runtime_ipc_svc(num, r)) return r[0];
    if (svc_memory(num, r)) return r[0];
    switch (num) {
    case 0x07:                                                   /* ExitProcess */
        fprintf(stderr, "[totk] the game exited (svcExitProcess)\n"); fflush(stderr);
        runtime_front_request_close(); _exit(0);
    case 0x26: {                                                 /* Break(reason, x1 info, x2 size) */
        uint32_t result = 0; if (r[1] && r[2] >= 4) memcpy(&result, (void *)(uintptr_t)r[1], 4);
        fprintf(stderr, "[totk] guest Break reason=%#llx result=%#x (module %u, description %u)\n", (unsigned long long)r[0], result, result & 0x1ff, (result >> 9) & 0x1fff);
        runtime_trace_guest(r[29], r[30]);
        if (r[0] & 0x80000000ULL) { r[0] = 0; return 0; }      /* a notification only */
        runtime_fail("the game stopped itself (svcBreak, libnx diagAbortWithResult)");
    }
    case 0x27: {                                                 /* OutputDebugString(x0 text, x1 size) */
        size_t n = r[1]; if (n > 65536) n = 65536;
        fprintf(stderr, "[guest] "); fwrite((void *)(uintptr_t)r[0], 1, n, stderr); if (n && ((char *)(uintptr_t)r[0])[n - 1] != '\n') fputc('\n', stderr);
        r[0] = 0; return 0;
    }
    case 0x28:                                                   /* ReturnFromException: no exception was delivered */
        runtime_fail("svcReturnFromException without an exception");
    default:
        fprintf(stderr, "[totk] unimplemented SVC 0x%x at %s+%#llx\n", num, module, (unsigned long long)offset);
        runtime_trace_guest(r[29], r[30]); runtime_fail("system call not implemented");
    }
}
