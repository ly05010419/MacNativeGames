// TOTK: system load sampled for the bottom-right line (user, 2026-10-16 night). A background thread reads every 0.5 s:
// - per-core CPU load: host_processor_info(PROCESSOR_CPU_LOAD_INFO) tick deltas; performance / efficiency cores from the
//   device tree's "cluster-type" (E cores have the lowest logical ids on Apple silicon; that is the fallback);
// - the GPU's own utilisation: IOAccelerator PerformanceStatistics "Device Utilization %" (what Activity Monitor's GPU
//   history shows; our keep-alive fills the GPU's idle gaps, so it reads near 100 % while the game runs);
// - the game's GPU share: the union of our command buffers' GPU intervals (sysload_note_gpu) per wall time.
// It never touches the game threads; reading the device tree / IOKit happens on its own thread.
#include "totk_sysload.h"
#include <TargetConditionals.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#if TARGET_OS_OSX
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#endif

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static sysload_info g_cur;
static int g_have;
static sysload_busy g_busy;   // under g_mu (GPU completion handlers)

int sysload_enabled(void) {
    static int on = -1;
#if TARGET_OS_IPHONE
    const int def = 0;   // no IOKit on iOS; the phone shows only the FPS counter
#else
    const int def = 1;
#endif
    if (on < 0) { const char *e = getenv("TOTK_LOAD_OVERLAY"); on = e ? strcmp(e, "0") != 0 : def; }
    return on;
}

double sysload_busy_add(sysload_busy *b, double s, double e) {
    if (!(e > s)) return 0;
    double from = s > b->last_end ? s : b->last_end, add = e > from ? e - from : 0;
    if (e > b->last_end) b->last_end = e;
    b->busy += add;
    return add;
}
void sysload_note_gpu(double s, double e) { pthread_mutex_lock(&g_mu); sysload_busy_add(&g_busy, s, e); pthread_mutex_unlock(&g_mu); }

void sysload_text(char *out, size_t n, const sysload_info *in) {
    size_t o = 0;
    #define PUT(...) do { int k = snprintf(out + o, o < n ? n - o : 0, __VA_ARGS__); if (k > 0) o += (size_t)k; } while (0)
    /* the GPU first, then the CPU cores (user 2026-10-25); only the game's own GPU share (user 2026-10-30: the system's
       utilisation reads near 100 % because our keep-alive fills the idle gaps, so it said nothing about the game) */
    if (in->gpu_game >= 0) PUT("GPU %d%%", in->gpu_game); else PUT("GPU --");
    PUT("  P"); for (int i = 0; i < in->np; i++) PUT(" %2d%%", in->p[i]);   // every value with % (user, 2026-10-16 night)
    PUT("  E"); for (int i = 0; i < in->ne; i++) PUT(" %2d%%", in->e[i]);
    if (in->thread_max >= 0) PUT("  T %d%%", in->thread_max);   // the busiest thread (user, 2026-10-16 night)
    #undef PUT
    if (n) out[n - 1 < o ? n - 1 : o] = 0;
}

int sysload_get(sysload_info *out) {
    pthread_mutex_lock(&g_mu); int have = g_have; if (have) *out = g_cur; pthread_mutex_unlock(&g_mu);
    return have;
}

static double now_s(void) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;   // the clock of CACurrentMediaTime and GPUStartTime
}

/* 'P' or 'E' per logical CPU id. */
static void classify(int ncpu, char *kind) {
    int ne = 0; size_t sz = sizeof ne;
    if (sysctlbyname("hw.perflevel1.logicalcpu", &ne, &sz, NULL, 0) || ne < 0 || ne > ncpu) ne = 0;   // perflevel1 = efficiency
    for (int i = 0; i < ncpu; i++) kind[i] = i < ne ? 'E' : 'P';
#if TARGET_OS_OSX
    io_registry_entry_t cpus = IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/cpus");
    io_iterator_t it = 0;
    if (cpus && IORegistryEntryGetChildIterator(cpus, kIODeviceTreePlane, &it) == KERN_SUCCESS) {
        io_object_t c;
        while ((c = IOIteratorNext(it))) {
            CFTypeRef id = IORegistryEntryCreateCFProperty(c, CFSTR("logical-cpu-id"), kCFAllocatorDefault, 0);
            CFTypeRef ct = IORegistryEntryCreateCFProperty(c, CFSTR("cluster-type"), kCFAllocatorDefault, 0);
            int lid = -1;
            if (id && CFGetTypeID(id) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)id, kCFNumberIntType, &lid);
            if (lid >= 0 && lid < ncpu && ct && CFGetTypeID(ct) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)ct) > 0) {
                char k = (char)CFDataGetBytePtr((CFDataRef)ct)[0]; if (k == 'P' || k == 'E') kind[lid] = k;
            }
            if (id) CFRelease(id);
            if (ct) CFRelease(ct);
            IOObjectRelease(c);
        }
        IOObjectRelease(it);
    }
    if (cpus) IOObjectRelease(cpus);
#endif
}

#if TARGET_OS_OSX
static io_object_t g_accel;
static int gpu_util(void) {
    if (!g_accel) {
        io_iterator_t it;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return -1;
        io_object_t s;
        while ((s = IOIteratorNext(it))) {
            CFTypeRef ps = IORegistryEntryCreateCFProperty(s, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
            int ok = ps && CFGetTypeID(ps) == CFDictionaryGetTypeID() && CFDictionaryContainsKey((CFDictionaryRef)ps, CFSTR("Device Utilization %"));
            if (ps) CFRelease(ps);
            if (ok && !g_accel) g_accel = s; else IOObjectRelease(s);
        }
        IOObjectRelease(it);
        if (!g_accel) return -1;
    }
    int v = -1;
    CFTypeRef ps = IORegistryEntryCreateCFProperty(g_accel, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
    if (ps && CFGetTypeID(ps) == CFDictionaryGetTypeID()) {
        CFTypeRef n = CFDictionaryGetValue((CFDictionaryRef)ps, CFSTR("Device Utilization %"));
        if (n && CFGetTypeID(n) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)n, kCFNumberIntType, &v);
    }
    if (ps) CFRelease(ps);
    return v < 0 ? -1 : v > 100 ? 100 : v;
}
#else
static int gpu_util(void) { return -1; }
#endif

/* The busiest thread of this process over the last interval: CPU time (user + system) per thread from thread_info, kept by
   thread id between samples; % of one core. Threads seen for the first time are skipped (no earlier time). */
enum { MAX_THREADS = 512 };
typedef struct { uint64_t id, us; } thread_time;
static thread_time g_tt[MAX_THREADS]; static int g_ntt;
static int busiest_thread(double wall_us, char name[32]) {
    thread_act_array_t th = NULL; mach_msg_type_number_t nth = 0;
    if (task_threads(mach_task_self(), &th, &nth) != KERN_SUCCESS || !th) return -1;
    static thread_time cur[MAX_THREADS]; int ncur = 0; double best = -1; name[0] = 0;
    for (mach_msg_type_number_t i = 0; i < nth; i++) {
        thread_identifier_info_data_t idi; mach_msg_type_number_t c1 = THREAD_IDENTIFIER_INFO_COUNT;
        thread_basic_info_data_t bi; mach_msg_type_number_t c2 = THREAD_BASIC_INFO_COUNT;
        if (thread_info(th[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&idi, &c1) == KERN_SUCCESS &&
            thread_info(th[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c2) == KERN_SUCCESS && ncur < MAX_THREADS) {
            uint64_t us = (uint64_t)bi.user_time.seconds * 1000000 + (uint64_t)bi.user_time.microseconds +
                          (uint64_t)bi.system_time.seconds * 1000000 + (uint64_t)bi.system_time.microseconds;
            cur[ncur++] = (thread_time){idi.thread_id, us};
            for (int k = 0; k < g_ntt; k++) if (g_tt[k].id == idi.thread_id) {
                double pct = us >= g_tt[k].us && wall_us > 0 ? (us - g_tt[k].us) / wall_us * 100 : 0;
                if (pct > best) { best = pct; pthread_t pt = pthread_from_mach_thread_np(th[i]); name[0] = 0; if (pt) pthread_getname_np(pt, name, 32); if (!name[0]) snprintf(name, 32, "thread %llu", (unsigned long long)idi.thread_id); }
                break;
            }
        }
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)th, nth * sizeof *th);
    memcpy(g_tt, cur, (size_t)ncur * sizeof *cur); g_ntt = ncur;
    return best < 0 ? -1 : best > 100 ? 100 : (int)(best + 0.5);
}

/* Ticks (user + system + nice, total) per CPU; returns the CPU count, 0 on failure. */
static int cpu_ticks(uint64_t busy[SYSLOAD_MAX_CORES], uint64_t total[SYSLOAD_MAX_CORES]) {
    static host_t host; if (!host) host = mach_host_self();
    natural_t n = 0; processor_info_array_t info = NULL; mach_msg_type_number_t cnt = 0;
    if (host_processor_info(host, PROCESSOR_CPU_LOAD_INFO, &n, &info, &cnt) != KERN_SUCCESS || !info) return 0;
    int nc = (int)n < SYSLOAD_MAX_CORES ? (int)n : SYSLOAD_MAX_CORES;
    for (int i = 0; i < nc; i++) {
        const integer_t *t = info + i * CPU_STATE_MAX;
        busy[i] = (uint64_t)(uint32_t)t[CPU_STATE_USER] + (uint32_t)t[CPU_STATE_SYSTEM] + (uint32_t)t[CPU_STATE_NICE];
        total[i] = busy[i] + (uint32_t)t[CPU_STATE_IDLE];
    }
    vm_deallocate(mach_task_self(), (vm_address_t)info, cnt * sizeof(integer_t));
    return nc;
}

static void *sampler(void *arg) {
    (void)arg;
    pthread_setname_np("totk-sysload");
    uint64_t b0[SYSLOAD_MAX_CORES], t0[SYSLOAD_MAX_CORES], b1[SYSLOAD_MAX_CORES], t1[SYSLOAD_MAX_CORES];
    int ncpu = cpu_ticks(b0, t0); char kind[SYSLOAD_MAX_CORES]; classify(ncpu, kind);
    char tname[32]; (void)busiest_thread(0, tname);   // the first thread times
    double w0 = now_s(), g0; pthread_mutex_lock(&g_mu); g0 = g_busy.busy; pthread_mutex_unlock(&g_mu);
    for (unsigned k = 1;; k++) {
        usleep(500000);
        int nc = cpu_ticks(b1, t1);
        sysload_info s = {0}; s.gpu_sys = gpu_util();
        double w1 = now_s(), g1; pthread_mutex_lock(&g_mu); g1 = g_busy.busy; pthread_mutex_unlock(&g_mu);
        s.thread_max = busiest_thread((w1 - w0) * 1e6, s.thread_name);
        double gp = w1 > w0 ? (g1 - g0) / (w1 - w0) * 100 : -1;
        s.gpu_game = gp < 0 ? -1 : gp > 100 ? 100 : (int)(gp + 0.5);
        for (int i = 0; i < nc && i < ncpu; i++) {
            uint64_t db = b1[i] - b0[i], dt = t1[i] - t0[i]; int v = dt ? (int)((db * 100 + dt / 2) / dt) : 0;
            if (kind[i] == 'P' && s.np < SYSLOAD_MAX_CORES) s.p[s.np++] = v; else if (s.ne < SYSLOAD_MAX_CORES) s.e[s.ne++] = v;
        }
        pthread_mutex_lock(&g_mu); g_cur = s; g_have = nc > 0; pthread_mutex_unlock(&g_mu);
        memcpy(b0, b1, sizeof b0); memcpy(t0, t1, sizeof t0); w0 = w1; g0 = g1;
        if (k % 60 == 0) { char t[128]; sysload_text(t, sizeof t, &s); fprintf(stderr, "[totk-gfx] [负载] %s（T 是最忙的线程：%s；括号里是游戏自己的 GPU 工作，不算保活）\n", t, s.thread_name); }   // every 30 s
    }
    return NULL;
}
static void start_once(void) { pthread_t th; if (!pthread_create(&th, NULL, sampler, NULL)) pthread_detach(th); }
void sysload_start(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, start_once); }
