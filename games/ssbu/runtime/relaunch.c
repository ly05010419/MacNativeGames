/* Starting the running game again, as the console does when a game asks to launch itself (Q74, 2026-10-24).
   Super Smash Bros. Ultimate does that after its language option changes: nn::applet::RequestToLaunchApplication with
   its own ApplicationId (applet_host.c). Before this the SDK reached for an applet object the host does not have and
   read address 0 (SIGSEGV at sdk+0x804c8).

   The game starts again as a new process with the same program, arguments and environment, and this one ends. A first
   version replaced this process in place (execv, same pid): AppKit reported "Task policy set failed: 4" at the new
   image's start, and the shader warm-up behind the purple start picture took 98.6 s to 16,384 items instead of 16.0 s
   (play-20261024-1823.log). As a new process neither happens: no such line, 6.0 s to 16,384 items, warm-up done in
   5.8 s (play-20261024-1927.log). Why macOS slowed the in-place image is not measured; its nice value was no
   difference (5 in the new process too). Only stdin, stdout and stderr go along (the log continues), every other descriptor
   stays behind; the game-slot lock is held until this process ends, so the new one waits for it. ssbu-play.sh follows
   the new pid from the "started ... as pid" line.

   The save has to be on disk first: the game commits before it asks, and the host waits while runtime_fs_save_busy
   says otherwise (60 s, then it stops loudly instead of throwing a half-written save away). The diagnostic command file
   (TOTK_INPUT_FILE) is not run a second time: TOTK_INPUT_SKIP tells the new process how much of it was already there
   (input.c). */
#include "runtime.h"
#include <errno.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;
char **runtime_main_argv;   /* loader.c main */

_Noreturn void runtime_relaunch(const char *why) {
    if (!runtime_main_argv) runtime_fail("restart before main kept its arguments");
    for (int i = 0; runtime_fs_save_busy(); i++) {
        if (i == 0) fprintf(stderr, "[totk] restart: waiting for the save to be written\n");
        if (i >= 1200) runtime_fail("restart: the game is still writing its save after 60 s");
        usleep(50000);
    }
    const char *in = getenv("TOTK_INPUT_FILE");
    struct stat st;
    if (in && *in && !stat(in, &st)) {
        char n[32];
        snprintf(n, sizeof n, "%lld", (long long)st.st_size);
        setenv("TOTK_INPUT_SKIP", n, 1);
    }
    char exe[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size)) runtime_fail("restart: executable path");
    /* In the app (Q80: the launcher names SSBU_APP_BUNDLE) the game starts again as the app, the way the player starts it:
       a small shell waits until this process has ended (the launcher would otherwise find it still holding the game
       slot and only bring it to the front) and then opens the app; the launcher sets everything up again, the Picture
       menu's size included. A bare binary would run without the app's name and icon. */
    const char *bundle = getenv("SSBU_APP_BUNDLE");
    char pid_text[16];
    snprintf(pid_text, sizeof pid_text, "%d", (int)getpid());
    char *const via_app[] = {"/bin/sh", "-c", "while kill -0 \"$0\" 2>/dev/null; do sleep 0.1; done; exec /usr/bin/open -n \"$1\"",
                             pid_text, (char *)bundle, NULL};
    fflush(NULL);
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t files;
    if (posix_spawnattr_init(&attr) || posix_spawn_file_actions_init(&files)) runtime_fail("restart: spawn setup");
    if (posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT)) runtime_fail("restart: spawn flags");
    for (int fd = 0; fd <= 2; fd++)
        if (posix_spawn_file_actions_addinherit_np(&files, fd)) runtime_fail("restart: keeping stdin/stdout/stderr");
    pid_t child;
    if (bundle && *bundle) {
        int e = posix_spawn(&child, "/bin/sh", &files, &attr, via_app, environ);
        if (e) {
            fprintf(stderr, "[totk] restart: starting the app again failed: %s\n", strerror(e));
            runtime_fail("restart (posix_spawn /bin/sh)");
        }
        fprintf(stderr, "[totk] restart (%s): the app %s opens again once pid %d has ended (waiter pid %d)\n", why, bundle,
                (int)getpid(), (int)child);
        fflush(NULL);
        _exit(0);
    }
    int e = posix_spawn(&child, exe, &files, &attr, runtime_main_argv, environ);
    if (e) {
        fprintf(stderr, "[totk] restart: starting %s failed: %s\n", exe, strerror(e));
        runtime_fail("restart (posix_spawn)");
    }
    fprintf(stderr, "[totk] restart (%s): started %s again as pid %d; pid %d ends\n", why, exe, (int)child, (int)getpid());
    fflush(NULL);
    _exit(0);
}
