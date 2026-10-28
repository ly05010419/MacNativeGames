// Bootstrap: after loading the modules, initialize the SDK runtime in the real order of nn::init::Start (sdk+0x2b0470) in the SDK, then enter nnMain.
#include "dm.h"
#include "front.h"
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
void probe_install(void); void hid_keys_init(void); void stubs_report(void); void crash_install(void); void guest_thread_init(void); uint32_t svc_thread_main_init(void);
typedef void (*fn0)(void); typedef void (*fn2)(uint64_t, uint64_t); typedef void (*fnp)(void *);
static void *req(const char *n){ void *p=guest_sym(n); if(!p) die("missing symbol %s", n); return p; }

// Game arguments: argv[0] is fixed to "game", and whatever follows "--" is appended as is (on a real device nn::init takes them from the process argument area and hands them to the SDK through SetHostArgc/Argv)
static char *g_argv[64] = {"game"}; static int g_argc = 1;

static void guest_main(void) {
    guest_thread_init();
    { void dm_oodle_serve_maybe(int); dm_oodle_serve_maybe(0); }   // DM_OODLE_SERVE tool mode: the game's own Oodle decoder over stdin / stdout (hle_oodle.c, exits when the input ends)
    uint32_t hmain = svc_thread_main_init();   // Main thread handle: x0 of nnosInitialize (stored in an sdk global, used by ThreadManagerImplByHorizon); x1 = pointer to the argument area
    // Copy Start step by step: _init_libc1 = __nnmusl_init_locks (initializes the stdio etc. locks as recursive locks; skipping it self-deadlocks on nested locking in putc) + __init_ssp.
    // The three callbacks passed in by rtld: A only sets an rtld flag (skipped), B = each module's DT_INIT (before nnMain), C = destructors (not done)
    ((fn0)req("_init_libc0"))();
    LOG(">>> nnosInitialize"); ((fn2)req("nnosInitialize"))(hmain, 0);
    ((fn0)req("_init_libc1"))();
    LOG(">>> nninitInitializeSdkModule"); ((fn0)req("nninitInitializeSdkModule"))();
    // The game arguments are in place before nninitStartup, as on hardware (nn::os takes them from the process argument area in nnosInitialize): Unreal Engine's nninitStartup
    // (Octopath Traveler 0, main+0x12afc) reads -SwitchVirtualAddressSpaceSize= and its other memory options there
    ((fnp)req("_ZN2nn2os11SetHostArgcEi"))((void *)(uint64_t)g_argc);
    ((fnp)req("_ZN2nn2os11SetHostArgvEPPc"))(g_argv);
    LOG(">>> nninitStartup"); ((fn0)req("nninitStartup"))();
    ((fn0)req("_init_libc2"))();
    run_module_inits();   // rtld callback B: each module's DT_INIT (global constructors)
    { void dm_guest_spawn_selftest(void); dm_guest_spawn_selftest(); }   // DM_GUEST_SPAWN_TEST self-test of host-created guest threads (hle_guestthread.c)
    { void dm_oodle_serve_maybe(int); dm_oodle_serve_maybe(1); }   // DM_OODLE_SERVE with a decoder that needs the game's malloc (LZ5, hle_oodle.c)
    { void dm_oodle_job_maybe(void); dm_oodle_job_maybe(); }   // DM_OODLE_JOB: decode a job file with the game's own Oodle and exit (hle_oodle.c, for the offline shader tools)
    LOG(">>> nnMain"); ((fn0)req("nnMain"))();
    LOG("nnMain returned");
}

struct dm_flags g_flag;
uint64_t g_t0_ns;
static void flags_init(void) {
    g_flag.thr = !!getenv("DM_THR"); g_flag.svc = !!getenv("DM_SVC"); g_flag.fs = !!getenv("DM_FS");
    g_flag.movie = !!getenv("DM_MOVIE"); g_flag.dbg = !!getenv("DM_DBG"); g_flag.imports = !!getenv("DM_IMPORTS");
    g_flag.bootprof = !!getenv("DM_BOOTPROF");
}

// Zero point of the boot timeline = process creation time (p_starttime from sysctl KERN_PROC_PID, wall clock) converted to the now_ns monotonic time base; same definition on Mac / iOS
static uint64_t proc_start_mono_ns(void) {
    struct kinfo_proc kp; size_t n = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    uint64_t mono = now_ns(); struct timeval tv; gettimeofday(&tv, NULL);
    if (sysctl(mib, 4, &kp, &n, NULL, 0) || !n) return mono;
    int64_t age = (int64_t)(tv.tv_sec - kp.kp_proc.p_starttime.tv_sec) * 1000000000ll + (tv.tv_usec - kp.kp_proc.p_starttime.tv_usec) * 1000ll;
    return age > 0 ? mono - (uint64_t)age : mono;
}

// Host loading (independent of UIKit / Metal): read the NSO, relocate, forced take-over, probes / tick gate / keys, start prefetch. By default it runs synchronously in main;
// the iOS front end can call dm_host_preload_start(same arguments) at the very start of the App's main() so that it runs first on a background thread, in parallel with the UIKit startup chain (⓪ + ① ≈ max(UIKit chain, host loading)).
static pthread_key_t g_race_keys[512];
static unsigned g_race_n, g_race_want;
static void *keyrace_thread(void *unused);
static void host_preload(int argc, char **argv) {
    g_t0_ns = proc_start_mono_ns();
    flags_init();
    if (argc < 2) die("usage: dmhost <dir> [mod...] [-- game arguments...]");
    // Time zone: the game converts absolute time to local time along two paths, both ending in the host libc -- nn::time::ToCalendarTime (hle_time.c, localtime_r) and the guest libc's
    // localtime_r / mktime (hle_table.c wires them straight to the host). So only when the host follows the system time zone do the original game's screens (chronicle discovery time etc.) show this machine's time.
    // Only deterministic comparison (DM_FIXED_TIME) pins it to UTC: the same replay must convert to the same calendar time on this machine (UTC+8) and on the second Mac (UTC+9)
    if (getenv("DM_FIXED_TIME")) setenv("TZ", "UTC", 1);
    // Open file limit: the soft limit starts at 256 on iOS (and in a default macOS shell), while the game's file handles, the RomFS archives, save transactions and Metal caches share it. Raise it to 4096 (as far as the hard limit allows)
    struct rlimit rl; if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur < 4096) {
        rlim_t was = rl.rlim_cur; rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
        if (setrlimit(RLIMIT_NOFILE, &rl)) LOG("open file limit %llu could not be raised", (unsigned long long)was);
    }
    crash_install();
    const char *race = getenv("DM_TEST_KEYRACE");
    if (race && *race) {
        g_race_want = (unsigned)strtoul(race, NULL, 10);
        if (g_race_want > 512) g_race_want = 512;
        pthread_t th;
        if (pthread_create(&th, NULL, keyrace_thread, NULL)) die("DM_TEST_KEYRACE cannot start its thread");
        pthread_detach(th);
    }
    { void dm_term_install(void); dm_term_install(); }   // SIGTERM graceful exit (hle_lifecycle.c)
    { void crash_set_dump_hook(void (*)(void)); void svc_thread_dump(void); crash_set_dump_hook(svc_thread_dump); }   // Dump guest thread state on a crash
    const char *names[MAX_MOD]; int n = 0;
    int i = 2;
    for (; i < argc && strcmp(argv[i], "--") && n < MAX_MOD; i++) names[n++] = argv[i];
    if (i < argc && !strcmp(argv[i], "--")) for (i++; i < argc && g_argc < 63; i++) g_argv[g_argc++] = argv[i];
    else if (*DM_GAME_ARGS) {   // no "--" on the command line (the iOS app, the NSP script's Mac runs): the game profile's game_args, split at spaces
        char *s = strdup(DM_GAME_ARGS);
        for (char *t = strtok(s, " "); t && g_argc < 63; t = strtok(NULL, " ")) g_argv[g_argc++] = t;
        LOG("game arguments from the game profile: %s", DM_GAME_ARGS);
    }
    if (!n) { static const char *def[] = DM_GAME_MODULES; for (int k = 0; k < DM_GAME_NMOD && n < MAX_MOD; k++) names[n++] = def[k]; }   // default module list from the game profile's modules (Mario: main sdk)
    load_modules(argv[1], names, n);
    dm_sdk_major();   // logs the SDK version, which SDK-dependent behaviour follows (loader.c)
    uint64_t ta = now_ns();
    probe_install();   // DM_PROBE probes (for diagnostics)
    hid_keys_init();   // DM_KEYS scripted input
    uint64_t tb = now_ns();
    stubs_report();
    if (g_flag.bootprof) LOG("① breakdown (after loading): probes / keys %.1f ms, stub report %.1f ms", (tb - ta) / 1e6, (now_ns() - tb) / 1e6);
    { void loader_hash_log(void); loader_hash_log(); }   // DM_LOADHASH=1: address-independent hash of the load result (loader.c)
    { void dm_ue_malloc_install(void); dm_ue_malloc_install(); }   // Unreal's allocator on a host malloc zone, for the builds whose constructor call is cut (hle_uemalloc.c)
}
static struct { int argc; char **argv; pthread_t th; _Atomic int started; } g_pre;
static void *preload_thread(void *a) { (void)a; host_preload(g_pre.argc, g_pre.argv); return NULL; }
static void *keyrace_thread(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < g_race_want; i++) if (pthread_key_create(&g_race_keys[g_race_n++], NULL)) break;
    return NULL;
}
// Start host loading early. When to call: as one of the first things in the process (iOS: in the App's main() before entering UIApplicationMain); the environment variables loading needs must already be set
// (DM_ROMFS / DM_SAVE / DM_TEMP etc. and the cwd -- loading and the prefetch start read them); the arguments must be identical, item by item, to those of the later dm_main (a difference reports a failure and exits).
// May be called only once; if not called, loading is synchronous inside main and behavior is exactly as before this call existed. Does not depend on UIKit / Metal.
void dm_host_preload_start(int argc, char **argv) {
    if (atomic_exchange(&g_pre.started, 1)) return;
    g_pre.argc = argc; g_pre.argv = calloc((size_t)argc + 1, sizeof(char *));
    for (int i = 0; i < argc; i++) g_pre.argv[i] = strdup(argv[i]);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);
    if (pthread_create(&g_pre.th, &a, preload_thread, NULL)) die("dm_host_preload_start: cannot start the thread");
}
int main(int argc, char **argv) {
    if (getenv("DM_PRELOAD")) dm_host_preload_start(argc, argv);   // Exercise the early-start path on Mac (on iOS it is called from the App's main())
    if (atomic_load(&g_pre.started)) {
        pthread_join(g_pre.th, NULL);
        int same = argc == g_pre.argc; for (int i = 0; same && i < argc; i++) same = !strcmp(argv[i], g_pre.argv[i]);
        if (!same) die("[RED] dm_host_preload_start arguments differ from main's");
        void crash_thread_init(void); crash_thread_init();   // The signal alternate stack is per thread: loading installed it on another thread, so add it for this thread
        LOG("host loading already finished early in the background (dm_host_preload_start)");
    } else host_preload(argc, argv);
    { void dm_fps_state_apply(void); dm_fps_state_apply(); }   // frame rate and render scale into the frame step state page (loader.c): the iOS launcher sets them after the loading began
    front_run(guest_main);
    return 0;
}
