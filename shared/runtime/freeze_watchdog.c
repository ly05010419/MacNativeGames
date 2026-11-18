/* Freeze watchdog (task 0k, after smo-recomp's nvn_vk_watchdog.c): when the game stops producing frames, write what is
   needed to see why, and nothing else. Off unless ODYSSEY_FREEZE_WATCHDOG is set: "1" = 5 s without a frame, "N" = N s.
   It never stops, resumes or changes the game; it only logs.

   Once the game has presented its first frame, a thread looks every 500 ms at the frames the game submitted
   (dm_nvn_frame_count, hle_nvn.c) and the frames actually presented (mtl_present_count, nvn_mtl.m). When neither has moved
   for the threshold it logs one "[freeze-watchdog]" line and dumps, once per freeze (at most 3 per run):
   - the guest threads, core owners and lock owners (runtime_dump_threads, threads.c), to stderr;
   - the GPU wait state (mtl_wait_state_dump, nvn_mtl.m), to stderr;
   - every host thread's stack: /usr/bin/sample for 3 s into <dir>/freeze-<pid>-<n>.txt, where <dir> is
     ODYSSEY_FREEZE_DIR, else freeze-dumps next to TOTK_PROFILE_DIR, else ./freeze-dumps.
   When frames move again it logs how long the freeze lasted. The hooks are weak, so a target without them (the headless
   probe) still links; a missing hook is skipped. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
__attribute__((weak)) uint64_t dm_nvn_frame_count(void);
__attribute__((weak)) uint64_t mtl_present_count(void);
__attribute__((weak)) void runtime_dump_threads(void);
__attribute__((weak)) void mtl_wait_state_dump(void);

enum { FW_MAX_DUMPS = 3 };
static double g_threshold_s;
static char g_dir[PATH_MAX];

static double fw_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9; }
static uint64_t fw_frames(void) { return dm_nvn_frame_count ? dm_nvn_frame_count() : 0; }
static uint64_t fw_presents(void) { return mtl_present_count ? mtl_present_count() : 0; }

static pid_t fw_sample(int n) {   /* every host thread's stack, by Apple's sampler, in its own process */
    mkdir(g_dir, 0755);
    char path[PATH_MAX], pid[16];
    snprintf(path, sizeof path, "%s/freeze-%d-%d.txt", g_dir, (int)getpid(), n);
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    char *argv[] = {"/usr/bin/sample", pid, "3", "-file", path, "-mayDie", NULL};
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t at; posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK);
    sigset_t none; sigemptyset(&none); posix_spawnattr_setsigmask(&at, &none);
    pid_t child = 0;
    int err = posix_spawn(&child, argv[0], &fa, &at, argv, environ);
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
    if (err) { fprintf(stderr, "[freeze-watchdog] could not start sample: %s\n", strerror(err)); return 0; }
    fprintf(stderr, "[freeze-watchdog] host thread stacks (3 s sample) -> %s\n", path);
    return child;
}

static void *fw_thread(void *unused) {
    (void)unused;
    pthread_setname_np("freeze-watchdog");
    uint64_t frames = fw_frames(), presents = fw_presents();
    double moved = fw_now(), frozen_at = 0;
    int dumps = 0; pid_t child = 0;
    for (;;) {
        usleep(500000);
        if (child && waitpid(child, NULL, WNOHANG) == child) child = 0;
        uint64_t f = fw_frames(), p = fw_presents();
        double now = fw_now();
        if (!f && !p) { moved = now; continue; }   /* not armed before the first frame (startup, shader warm-up) */
        if (f != frames || p != presents) {
            if (frozen_at) fprintf(stderr, "[freeze-watchdog] frames moved again after %.1f s (game frame %llu, present %llu)\n",
                                   now - moved, (unsigned long long)f, (unsigned long long)p);
            frames = f; presents = p; moved = now; frozen_at = 0;
            continue;
        }
        if (frozen_at || now - moved < g_threshold_s) continue;
        frozen_at = now;
        if (dumps >= FW_MAX_DUMPS) {
            fprintf(stderr, "[freeze-watchdog] no frame for %.1f s (game frame %llu, present %llu); dump limit reached\n",
                    now - moved, (unsigned long long)f, (unsigned long long)p);
            continue;
        }
        dumps++;
        fprintf(stderr, "[freeze-watchdog] no frame for %.1f s (game frame %llu, present %llu); dump %d of %d\n",
                now - moved, (unsigned long long)f, (unsigned long long)p, dumps, FW_MAX_DUMPS);
        if (!child) child = fw_sample(dumps);
        if (runtime_dump_threads) runtime_dump_threads();
        if (mtl_wait_state_dump) mtl_wait_state_dump();
    }
    return NULL;
}

__attribute__((constructor)) static void fw_start(void) {
    const char *e = getenv("ODYSSEY_FREEZE_WATCHDOG");
    if (!e || !*e || !strcmp(e, "0")) return;
    double s = atof(e); g_threshold_s = s > 1 ? s : 5;
    const char *dir = getenv("ODYSSEY_FREEZE_DIR"), *profile = getenv("TOTK_PROFILE_DIR");
    if (dir && *dir) snprintf(g_dir, sizeof g_dir, "%s", dir);
    else if (profile && *profile) snprintf(g_dir, sizeof g_dir, "%s/../freeze-dumps", profile);
    else snprintf(g_dir, sizeof g_dir, "freeze-dumps");
    pthread_t t;
    if (pthread_create(&t, NULL, fw_thread, NULL)) { fprintf(stderr, "[freeze-watchdog] could not start: %s\n", strerror(errno)); return; }
    pthread_detach(t);
    fprintf(stderr, "[freeze-watchdog] on: logs and dumps after %.0f s without a frame (dumps in %s); never changes the game\n", g_threshold_s, g_dir);
}
