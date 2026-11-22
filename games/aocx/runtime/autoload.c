/* TOTK_AUTOLOAD=1: load the newest save without pressing through the title. Only the agents' test sessions set it
   (scripts/diag/start_user_session.sh); the desktop launcher does not (user, 2026-10-16: the title stays part of his game). The title's "继续" loads the newest save, which is also the first entry of its load list.
   At boot the game reads every slot's caption.sav; once one exists, A (key J) is pressed every 1.5 s. The presses stop
   for good when the game opens a slot's progress.sav (the load has begun), when the user presses a key or a controller
   button (they take over, e.g. to pick another save) or after 3 minutes. Without any save, or with TOTK_INPUT_SCRIPT
   (a test pressing its own keys), nothing is pressed: on the title A would otherwise start a new game.
   TOTK_AUTOLOAD_PERIOD_MS / TOTK_AUTOLOAD_LIMIT_S change the cadence and the limit (scripts/autoload_probe.c). */
#include "autoload.h"
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { AL_OFF, AL_WAIT, AL_PRESSING, AL_DONE };
enum { KEY_A = 38, PRESS_MS = 150, POLL_MS = 50 };
static _Atomic int g_state = AL_OFF;
static unsigned g_period_ms = 1500, g_limit_s = 180;

static unsigned env_uint(const char *name, unsigned def, unsigned lo, unsigned hi) {
    const char *e = getenv(name); if (!e) return def;
    char *end; unsigned long v = strtoul(e, &end, 10);
    if (end == e || *end || v < lo || v > hi) { fprintf(stderr, "[autoload] %s=%s: expected %u..%u\n", name, e, lo, hi); runtime_fail("invalid auto-load setting"); }
    return (unsigned)v;
}
__attribute__((constructor)) static void autoload_configure(void) {
    const char *e = getenv("TOTK_AUTOLOAD");
    if (!e || !strcmp(e, "0")) return;
    if (strcmp(e, "1")) runtime_fail("TOTK_AUTOLOAD takes 0 or 1");
    g_period_ms = env_uint("TOTK_AUTOLOAD_PERIOD_MS", g_period_ms, 300, 10000);
    g_limit_s = env_uint("TOTK_AUTOLOAD_LIMIT_S", g_limit_s, 1, 600);
    if (getenv("TOTK_INPUT_SCRIPT")) { fprintf(stderr, "[autoload] off: TOTK_INPUT_SCRIPT presses its own keys\n"); return; }
    atomic_store(&g_state, AL_WAIT);
    fprintf(stderr, "[autoload] on: the newest save is loaded once the game finds one (any key you press takes over)\n");
}

static void stop(int from, const char *why) {
    if (atomic_compare_exchange_strong(&g_state, &from, AL_DONE)) fprintf(stderr, "[autoload] %s\n", why);
}
static void *press_loop(void *arg) {
    (void)arg;
    uint64_t t0 = runtime_now_ns();
    for (unsigned n = 1; atomic_load(&g_state) == AL_PRESSING; n++) {
        if (runtime_now_ns() - t0 >= (uint64_t)g_limit_s * 1000000000ull) { stop(AL_PRESSING, "gave up: the load did not start (no progress.sav opened)"); break; }
        runtime_input_key_synthetic(KEY_A, 1); usleep(PRESS_MS * 1000); runtime_input_key_synthetic(KEY_A, 0);
        fprintf(stderr, "[autoload] press A #%u at %.1f s\n", n, (double)(runtime_now_ns() - t0) / 1e9);
        for (unsigned w = PRESS_MS; w < g_period_ms && atomic_load(&g_state) == AL_PRESSING; w += POLL_MS) usleep(POLL_MS * 1000);
    }
    return NULL;
}

/* "save:/slot_NN/<name>" */
static int slot_file(const char *path, const char *name) {
    static const char prefix[] = "save:/slot_";
    if (!path || strncmp(path, prefix, sizeof prefix - 1)) return 0;
    const char *slash = strchr(path + sizeof prefix - 1, '/');
    return slash && !strcmp(slash + 1, name);
}
void runtime_autoload_save_file_opened(const char *path) {
    int s = atomic_load(&g_state);
    if (s == AL_OFF || s == AL_DONE) return;
    if (slot_file(path, "progress.sav")) {
        char why[200]; snprintf(why, sizeof why, "load started (%s opened): done", path);
        stop(s, why);
    } else if (s == AL_WAIT && slot_file(path, "caption.sav")) {
        int from = AL_WAIT;
        if (!atomic_compare_exchange_strong(&g_state, &from, AL_PRESSING)) return;
        fprintf(stderr, "[autoload] found a save (%s): pressing A every %.1f s until the load starts\n", path, g_period_ms / 1000.0);
        pthread_t t; if (pthread_create(&t, NULL, press_loop, NULL)) runtime_fail("auto-load thread"); pthread_detach(t);
    }
}
void runtime_autoload_user_input(void) {
    int s = atomic_load(&g_state);
    if (s == AL_WAIT || s == AL_PRESSING) stop(s, "cancelled: the user pressed a key or button");
}
