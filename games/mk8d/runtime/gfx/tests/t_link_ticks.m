// The acquire's waits on the display link, counted in its own ticks (mtl_link_wait_ticks) or in the game's 60 Hz vsyncs (mtl_link_wait_tick): a 120 Hz link takes two of its
// ticks per vsync, so present_interval display at DM_PRESENT_HZ=120 needs the first to reach one frame per tick. The real acquire also covers fractional rates and the game/step
// pacing branches. A controlled clock and timed wait deliver one tick per wake: a sleeping ticker can miss the acquire's real deadline on a busy CI runner
#import <Foundation/Foundation.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
static uint64_t test_now = 10000000000ull;
static unsigned test_waits, test_spurious;
static int test_silent;
static int test_clock_gettime(clockid_t clock, struct timespec *t) {
    (void)clock;
    *t = (struct timespec){.tv_sec = test_now / 1000000000ull, .tv_nsec = test_now % 1000000000ull};
    return 0;
}
static int test_pthread_cond_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
#define clock_gettime test_clock_gettime
#define pthread_cond_timedwait test_pthread_cond_timedwait
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#undef pthread_cond_timedwait
#undef clock_gettime
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
// Only the OS wait is replaced: the production wait loop, timeout calculation,
// tick history and acquire conversion all run. No other thread advances time.
static int test_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline) {
    if (cond != &g_link_cv || mutex != &g_link_mu) { fprintf(stderr, "unexpected timed wait\n"); abort(); }
    test_waits++;
    if (test_spurious) { test_spurious--; return 0; }
    uint64_t end = (uint64_t)deadline->tv_sec * 1000000000ull + deadline->tv_nsec;
    if (test_silent || test_now + 2000000ull >= end) { test_now = end; return ETIMEDOUT; }
    test_now += 2000000ull; g_link_tick_n++;
    return 0;
}
// Ticks per wait over 50 waits of the given kind (frames that take no time: each wait is the whole frame)
static double per_wait(int own, int n) {
    uint64_t t0 = g_link_tick_n;
    for (int i = 0; i < 50; i++) own ? mtl_link_wait_ticks(n, 100000000ull) : mtl_link_wait_tick(n, 100000000ull);
    return (g_link_tick_n - t0) / 50.0;
}
// Exercise the real acquire so the conversion from the requested frame rate to link ticks is covered too.
static double per_acquire(int hz, int display, int interval) {
    nvn_present_config(hz, display);
    win_t win = {.interval = interval}; int idx;
    uint64_t t0 = g_link_tick_n;
    for (int i = 0; i < 60; i++) h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx);
    return (g_link_tick_n - t0) / 60.0;
}
static void acquire_ticks(int hz, int display, int interval, double want, const char *label) {
    double got = per_acquire(hz, display, interval);
    printf("%s: %.2f ticks per acquire (expected %.2f)\n", label, got, want);
    CHECK(got == want, label);
}
int main(void) { @autoreleasepool {
    setenv("DM_RT", "0", 1); setenv("DM_PRESENT_SNAP", "0", 1);
    // Test uncapped display pacing independently of the selected game's default.
    setenv("DM_FPS_TARGET", "0", 1);
    g_dm_present_link = 2; g_link = (CAMetalDisplayLink *)[NSObject new];   // a link is "on" while the object is set; nothing calls it here
    CHECK(mtl_link_active(), "the link counts as active");
    g_link_mode = 2;
    CHECK(mtl_link_rate() == 120, "a 120 Hz link reports 120");
    double a = per_wait(0, 1), b = per_wait(1, 1), c = per_wait(1, 2);
    printf("120 Hz link: interval 1 waits %.2f ticks, one own tick %.2f, two own ticks %.2f\n", a, b, c);
    CHECK(a == 2, "a 60 Hz vsync is two ticks of a 120 Hz link");
    CHECK(b == 1, "one own tick per frame: 120 frames a second");
    CHECK(c == 2, "two own ticks");
    acquire_ticks(120, 1, 2, 1, "display 120 Hz ignores the game's interval");
    acquire_ticks(90, 1, 1, 4. / 3, "display 90 Hz preserves fractional ticks");
    acquire_ticks(75, 1, 1, 1.6, "display 75 Hz preserves fractional ticks");
    acquire_ticks(120, 0, 2, 4, "game mode keeps interval 2 in 60 Hz vsyncs");
    acquire_ticks(60, 1, 2, 2, "display 60 Hz ignores the game's interval");
    acquire_ticks(240, 1, 1, 1, "a rate above the link's waits one tick");
    setenv("DM_FPS_TARGET", "30", 1); g_present_cap = -1;
    acquire_ticks(120, 1, 1, 4, "native 30 fps cap waits four 120 Hz ticks");
    CHECK(nvn_game_fps() == 30, "native 30 fps cap configures the display link");
    setenv("DM_FPS_TARGET", "60", 1); g_present_cap = -1;
    acquire_ticks(120, 1, 1, 2, "native 60 fps cap waits two 120 Hz ticks");
    CHECK(nvn_game_fps() == 60, "native 60 fps cap configures the display link");
    setenv("DM_FPS_TARGET", "0", 1); g_present_cap = -1;
    _Alignas(8) float state[10] = {60}; dm_fps_state = (uint8_t *)state;   // include the fixed-step flag at +0x24 read by rt_period_ns
    acquire_ticks(120, 1, 1, 2, "the 60 fps frame step keeps 60 Hz vsyncs");
    state[0] = 30;
    acquire_ticks(120, 1, 1, 4, "the 30 fps frame step keeps two 60 Hz vsyncs");
    dm_fps_state = NULL;
    g_link_mode = 1;
    CHECK(mtl_link_rate() == 60, "a 60 Hz link reports 60");
    a = per_wait(0, 1);
    CHECK(a == 1, "a 60 Hz link: one tick per vsync");
    acquire_ticks(120, 1, 1, 1, "a 60 Hz link limits display 120 Hz to one tick");
    g_link_mode = 3;
    CHECK(mtl_link_rate() == 30, "the 30 Hz power-saving link reports 30");
    acquire_ticks(120, 1, 1, 1, "a 30 Hz link limits display 120 Hz to one tick");
    acquire_ticks(60, 0, 2, 1, "a 30 Hz link keeps game interval 2 at one tick");
    acquire_ticks(60, 0, 3, 2, "a 30 Hz link rounds game interval 3 up to two ticks");
    unsigned waits = test_waits; g_link_tick_n += 4;
    CHECK(mtl_link_wait_ticks(2, 10000000ull) && test_waits == waits, "already elapsed ticks need no wait");
    uint64_t tick = g_link_tick_n; waits = test_waits; test_spurious = 1;
    CHECK(mtl_link_wait_ticks(2, 10000000ull) && g_link_tick_n - tick == 2 && test_waits - waits == 3, "a spurious wake cannot finish a tick wait");
    test_silent = 1; tick = g_link_tick_n; uint64_t start = test_now;
    CHECK(!mtl_link_wait_ticks(1, 5000000ull), "a silent link times out");
    CHECK(g_link_tick_n == tick && test_now - start == 5000000ull, "timeout reaches its deadline without inventing ticks");
    test_silent = 0;
    CHECK(mtl_link_wait_ticks(2, 10000000ull) && g_link_tick_n - tick == 2, "ticks resume from the last successful wait after timeout");
    printf("t_link_ticks: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
