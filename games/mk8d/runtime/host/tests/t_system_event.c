// Service-event lifetimes through real binding / CloseHandle SVCs and the vi
// hook. Hold worker startup and advance its fallback clock explicitly so event
// destruction and address / handle reuse always happen before the first tick.
#include "../hle_thread.c"
#include <assert.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void *(*jobs[32])(void *);
static void *args[32];
static int njobs, sleeps, observed_signal;
static uint64_t clock_ns;
typedef struct { uint32_t wh, rh; } test_event;
static test_event *observe;
static void destroy_event(test_event *ev);
static int test_create(pthread_t *th, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
    (void)attr; assert(njobs < 32); *th = (pthread_t)(uintptr_t)(njobs + 1);
    jobs[njobs] = fn; args[njobs++] = arg; return 0;
}
static int test_detach(pthread_t th) { (void)th; return 0; }
static int test_sleep(const struct timespec *ts, struct timespec *left) {
    (void)left;
    // Bound an old worker that incorrectly follows a replacement event: inspect
    // its first tick, then destroy the replacement so even that worker can exit.
    if (sleeps++ && observe && observe->wh) {
        pthread_mutex_lock(&G); observed_signal |= ((event_t *)h_get(observe->wh, O_EVENT_W))->signaled; pthread_mutex_unlock(&G);
        destroy_event(observe);
    }
    assert(sleeps < 4);
    clock_ns += (uint64_t)ts->tv_sec * 1000000000ull + ts->tv_nsec;
    return 0;
}
#define pthread_create test_create
#define pthread_detach test_detach
#define nanosleep test_sleep
#include "../hle_sdk16.c"
#undef pthread_create
#undef pthread_detach
#undef nanosleep
void dm_applet_enter(const char *name) { (void)name; }   // hle_oe.c: the shop stand-ins hold the game out of focus
void dm_applet_leave(void) {}

struct dm_flags g_flag;
module_t *g_mod;
int g_nmod;
uint64_t g_t0_ns;
__thread _Atomic int dm_host_work;
pthread_key_t g_key18;
void crash_thread_init(void) {}
void guest_thread_init(void) {}
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) { (void)entry; (void)arg; (void)sp; abort(); }
uint64_t now_ns(void) { return clock_ns; }
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
int dm_sdk_major(void) { return 20; }
int dm_main_imports(const char *prefix) { (void)prefix; return 1; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
uint32_t hle_hid_connected_npad(void) { return 0; }
static uint32_t create_event(void *p, int mode, int interprocess) {
    (void)mode; assert(interprocess);
    uint64_t r[32] = {0}; assert(svc_thread(0x45, r) && !r[0]);
    test_event *ev = p; ev->wh = r[1]; ev->rh = r[2]; return 0;
}
static uint32_t writable_event(const void *p) { return ((const test_event *)p)->wh; }
void *guest_sym(const char *name) {
    if (!strcmp(name, "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb")) return create_event;
    if (!strcmp(name, "_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE")) return writable_event;
    return NULL;
}
static void destroy_event(test_event *ev) {
    uint64_t r[32] = {ev->wh}; assert(ev->wh && svc_thread(0x16, r) && !r[0]);
    r[0] = ev->rh; assert(svc_thread(0x16, r) && !r[0]);
    ev->wh = ev->rh = 0;
}
static int signalled(test_event *ev) {
    pthread_mutex_lock(&G); int signalled = ((event_t *)h_get(ev->wh, O_EVENT_W))->signaled; pthread_mutex_unlock(&G);
    return signalled;
}
static void run_job(int i) {
    void *guest = pthread_getspecific(g_keythr); pthread_setspecific(g_keythr, NULL);
    sleeps = 0; jobs[i](args[i]);
    pthread_setspecific(g_keythr, guest);
}
static void duplicate(void) {
    test_event ev = {0};
    assert(!h_vi_VsyncEvent(&ev, NULL)); assert(!h_vi_VsyncEvent(&ev, NULL));
    assert(njobs == 1); destroy_event(&ev); run_job(0);
}
static void capacity(void) {
    test_event ev[9] = {0};
    for (unsigned i = 0; i < 9; i++) { assert(!h_vi_VsyncEvent(&ev[i], NULL)); destroy_event(&ev[i]); }
    for (int i = 0; i < njobs; i++) run_job(i);
    assert(g_nse == 0);
}
static void reuse(void) {
    test_event ev = {0}; assert(!h_vi_VsyncEvent(&ev, NULL));
    uint32_t old_wh = ev.wh; destroy_event(&ev); hle_bind_system_event(&ev, 0);
    assert(ev.wh == old_wh);   // Force reuse of both guest storage and the kernel handle number.
    observe = &ev; run_job(0);
    assert(!observed_signal && (!ev.wh || !signalled(&ev)));
    if (ev.wh) destroy_event(&ev);
}
static void rebind(void) {
    test_event ev = {0}; assert(!h_vi_VsyncEvent(&ev, NULL)); destroy_event(&ev);
    assert(!h_vi_VsyncEvent(&ev, NULL));
    // The replacement vi registration must still tick after the old lifetime
    // retires, including when it uses exactly the same storage and handles.
    observe = &ev;
    for (int i = 0; i < njobs; i++) run_job(i);
    assert(observed_signal);
}
static void host_signal(void) {
    test_event ev = {0}; hle_bind_system_event(&ev, 0);
    void *guest = pthread_getspecific(g_keythr); pthread_setspecific(g_keythr, NULL);
    hle_signal_system_event(&ev); assert(signalled(&ev));
    destroy_event(&ev); hle_signal_system_event(&ev); hle_signal_system_event(&ev);
    assert(g_nse == 0); pthread_setspecific(g_keythr, guest);
    // Guest callers retain lazy creation for service events not yet bound.
    hle_signal_system_event(&ev); assert(signalled(&ev)); destroy_event(&ev);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct rlimit lim = {0, 0}; setrlimit(RLIMIT_CORE, &lim);
    void (*tests[])(void) = {duplicate, capacity, reuse, rebind, host_signal};
    const char *names[] = {"duplicate vi registration", "rapid vi event replacement", "storage and handle reuse by another service", "vi rebind at reused storage", "host skip and guest lazy binding"};
    int failed = 0;
    for (unsigned i = 0; i < sizeof tests / sizeof *tests; i++) {
        pid_t pid = fork(); assert(pid >= 0);
        if (!pid) { alarm(5); setenv("DM_VSYNC_HZ", "60", 1); svc_thread_main_init(); tests[i](); _exit(0); }
        int status; assert(waitpid(pid, &status, 0) == pid);
        int ok = WIFEXITED(status) && !WEXITSTATUS(status); failed |= !ok;
        printf("%s: %s\n", ok ? "PASS" : "FAIL", names[i]);
    }
    return failed;
}
