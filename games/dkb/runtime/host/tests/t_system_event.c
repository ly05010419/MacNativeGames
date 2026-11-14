// Service-event lifetimes through real binding / CloseHandle SVCs and the vi
// hook. Hold worker startup and advance its fallback clock explicitly so event
// destruction and address / handle reuse always happen before the first tick.
#include "../hle_thread.c"
#include <assert.h>
#include <mach-o/getsect.h>
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
void guest_thread_set_id(uint64_t id) { (void)id; }
void guest_gate_stack(int own_stack) { (void)own_stack; }
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) { (void)entry; (void)arg; (void)sp; abort(); }
uint64_t now_ns(void) { return clock_ns; }
uint64_t guest_ns(void) { return now_ns(); }
int dm_detclock_enabled(void) { return 0; }
int dm_guest_as_contains(uintptr_t addr) { (void)addr; return 0; }
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
static void copied_read_end(void) {
    // The audio renderer keeps its own event and hands the game a copy of the read end (hle_audren.c): a signal reaches the copy, and the copy outlives the renderer's ends
    test_event ev = {0}; hle_bind_system_event(&ev, 1);
    uint32_t copy = hle_dup_readable_handle(ev.rh);
    assert(copy && copy != ev.rh && copy != ev.wh && !hle_dup_readable_handle(ev.wh));   // only a read end is copied
    assert(hle_signal_system_event_binding(hle_system_event_binding(&ev)));
    pthread_mutex_lock(&G); event_t *e = h_get(copy, O_EVENT_R); assert(e && e->signaled && e == h_get(ev.rh, O_EVENT_R) && e->refs == 3); pthread_mutex_unlock(&G);
    destroy_event(&ev); assert(g_nse == 0);
    pthread_mutex_lock(&G); assert(h_peek(copy, O_EVENT_R) == e && e->refs == 1); pthread_mutex_unlock(&G);
    uint64_t r[32] = {copy}; assert(svc_thread(0x16, r) && !r[0]);
    pthread_mutex_lock(&G); assert(!h_peek(copy, O_EVENT_R)); pthread_mutex_unlock(&G);
}
// Resolve exactly the SDK imports the service watcher calls, through the host's normal force-hook table.
static void *purchase_hook(const char *name) {
    extern const struct mach_header_64 _mh_execute_header;
    unsigned long bytes = 0;
    const hle_force_ent *entries = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    for (size_t i = 0; i < bytes / sizeof *entries; i++) if (!strcmp(entries[i].name, name)) return (void *)entries[i].fn;
    fprintf(stderr, "missing purchased-item hook: %s\n", name); abort();
}
typedef struct { void *guest; test_event *(*get)(void); test_event *event; } purchase_reader;
static void *purchase_read(void *arg) {
    purchase_reader *reader = arg;
    pthread_setspecific(g_keythr, reader->guest);
    reader->event = reader->get();
    return NULL;
}
static void purchased_event(void) {
    void (*init)(void) = purchase_hook("_ZN2nn2ec24InitializePurchasedEventEv");
    void (*init_all)(void) = purchase_hook("_ZN2nn2ec27InitializeAllPurchasedEventEv");
    void (*init_app)(const uint64_t *) = purchase_hook("_ZN2nn2ec27InitializeAllPurchasedEventERKNS_13ApplicationIdE");
    void (*finish)(void) = purchase_hook("_ZN2nn2ec22FinalizePurchasedEventEv");
    test_event *(*get)(void) = purchase_hook("_ZN2nn2ec17GetPurchasedEventEv");
    unsigned (*pop)(void *) = purchase_hook("_ZN2nn2ec20PopPurchasedItemInfoEPNS0_17PurchasedItemInfoE");
    unsigned (*pop_private)(void *) = purchase_hook("_ZN2nn2ec20PopPurchasedItemInfoEPNS0_27PurchasedItemInfoForPrivateE");
    init();
    pthread_t threads[4]; purchase_reader readers[4];
    for (unsigned i = 0; i < 4; ++i) {
        readers[i] = (purchase_reader){pthread_getspecific(g_keythr), get, NULL};
        assert(!pthread_create(&threads[i], NULL, purchase_read, &readers[i]));
    }
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(threads[i], NULL));
    test_event *ev = get();
    for (unsigned i = 0; i < 4; ++i) assert(readers[i].event == ev);
    assert(ev && g_nse == 1 && !signalled(ev));
    uint64_t regs[32] = {0, (uintptr_t)&ev->rh, 1, 0};
    assert(svc_thread(0x18, regs) && regs[0] == R_TIMEOUT);
    uint64_t marker = 0xfeedface;
    assert(!pop(&marker) && !pop_private(&marker) && marker == 0xfeedface);
    finish(); init_all(); finish(); init_app(&marker);
    assert(get() && g_nse == 1 && !signalled(get())); finish();
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct rlimit lim = {0, 0}; setrlimit(RLIMIT_CORE, &lim);
    void (*tests[])(void) = {duplicate, capacity, reuse, rebind, host_signal, copied_read_end, purchased_event};
    const char *names[] = {"duplicate vi registration", "rapid vi event replacement", "storage and handle reuse by another service", "vi rebind at reused storage", "host skip and guest lazy binding", "copied read end of a bound event", "offline purchased-item watcher"};
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
