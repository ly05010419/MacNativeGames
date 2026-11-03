// Run the real watchdog in a child; replace only the external sampler with this
// executable in recorder mode. No sample process attaches to another application.
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum fault { NONE, INIT, OPEN, DUP, SPAWN, INTERRUPT, WAIT, EXIT_BAD, SIGNAL_BAD };
static enum fault fault;
static int actions_live, interrupted;

static int sample_init(posix_spawn_file_actions_t *a) {
    if (fault == INIT) return ENOMEM;
    int rc = posix_spawn_file_actions_init(a);
    if (!rc) actions_live++;
    return rc;
}
static int sample_open(posix_spawn_file_actions_t *a, int fd, const char *p, int flags, mode_t mode) {
    if (fault == OPEN) return ENOMEM;
    return posix_spawn_file_actions_addopen(a, fd, p, flags, mode);
}
static int sample_dup(posix_spawn_file_actions_t *a, int from, int to) {
    if (fault == DUP) return ENOMEM;
    return posix_spawn_file_actions_adddup2(a, from, to);
}
static int sample_destroy(posix_spawn_file_actions_t *a) {
    assert(actions_live == 1);
    actions_live--;
    return posix_spawn_file_actions_destroy(a);
}
static int sample_spawn(pid_t *pid, const char *file, const posix_spawn_file_actions_t *a,
                        const posix_spawnattr_t *attr, char *const argv[], char *const env[]) {
    assert(!strcmp(file, "/usr/bin/sample"));
    assert(!strcmp(argv[0], "/usr/bin/sample"));
    if (fault == SPAWN) return ENOENT;
    return posix_spawn(pid, "./sampler", a, attr, argv, env);
}
static pid_t sample_wait(pid_t pid, int *status, int flags) {
    if (fault == INTERRUPT && !interrupted++) { errno = EINTR; return -1; }
    pid_t rc = waitpid(pid, status, flags);
    // Reap the real fixture before simulating a failed wait, so no child is left.
    if (fault == WAIT) { errno = ECHILD; return -1; }
    return rc;
}
static _Noreturn void sample_exit(int status) {
    assert(actions_live == 0);
    _exit(status);
}
#define posix_spawn_file_actions_init sample_init
#define posix_spawn_file_actions_addopen sample_open
#define posix_spawn_file_actions_adddup2 sample_dup
#define posix_spawn_file_actions_destroy sample_destroy
#define posix_spawn sample_spawn
#define waitpid sample_wait
#define _exit sample_exit
// A regression to a shell must fail to compile, not execute the fixture's paths.
#pragma GCC poison system
#include "../crash.c"
#undef posix_spawn_file_actions_init
#undef posix_spawn_file_actions_addopen
#undef posix_spawn_file_actions_adddup2
#undef posix_spawn_file_actions_destroy
#undef posix_spawn
#undef waitpid
#undef _exit

module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }

static int recorder(int argc, char **argv) {
    FILE *f = fopen("arguments.bin", "wb");
    assert(f);
    for (int i = 0; i < argc; i++) assert(fwrite(argv[i], 1, strlen(argv[i]) + 1, f) == strlen(argv[i]) + 1);
    assert(!fclose(f));
    puts("SAMPLER_STDOUT");
    fputs("SAMPLER_STDERR\n", stderr);
    const char *injected = getenv("WATCH_TEST_FAULT");
    if (injected && !strcmp(injected, "exit")) return 7;
    if (injected && !strcmp(injected, "signal")) raise(SIGTERM);
    return 0;
}
static int checks, failures;
#define CHECK(cond, name) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: %s\n", name); } } while (0)

static void run_case(const char *path, enum fault inject, const char *name) {
    assert(unlink("arguments.bin") == 0 || errno == ENOENT);
    assert(unlink("injected-marker") == 0 || errno == ENOENT);
    int logs[2]; assert(!pipe(logs));
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(logs[0]);
        assert(dup2(logs[1], STDOUT_FILENO) == STDOUT_FILENO);
        assert(dup2(logs[1], STDERR_FILENO) == STDERR_FILENO);
        close(logs[1]);
        fault = inject;
        if (path) assert(!setenv("DM_WATCH_SAMPLE", path, 1));
        else assert(!unsetenv("DM_WATCH_SAMPLE"));
        // Normal sampling also covers a recorder launched without fault controls.
        if (inject == NONE) assert(!unsetenv("WATCH_TEST_FAULT"));
        else assert(!setenv("WATCH_TEST_FAULT", inject == EXIT_BAD ? "exit" : inject == SIGNAL_BAD ? "signal" : "none", 1));
        g_watch_stall = 1;
        atomic_store(&g_watch_last_ns, 0);
        watchdog_main(NULL);
        _exit(99);
    }
    close(logs[1]);
    char output[16384]; size_t used = 0; ssize_t n;
    while ((n = read(logs[0], output + used, sizeof output - 1 - used)) > 0) used += (size_t)n;
    assert(n == 0); output[used] = 0; close(logs[0]);
    int status; assert(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGALRM, "watchdog retains its exit status and frees spawn actions");
    CHECK(!strstr(output, "SAMPLER_STDOUT") && !strstr(output, "SAMPLER_STDERR"), "sampler stdout and stderr stay redirected");
    int succeeds = path && (inject == NONE || inject == INTERRUPT);
    CHECK((strstr(output, "thread stacks written to") != NULL) == succeeds, name);
    CHECK((strstr(output, "sampling failed") != NULL) == (path && !succeeds), "sampling failures are reported");
    CHECK(access("injected-marker", F_OK) == -1 && errno == ENOENT, "output path cannot execute shell syntax");
    FILE *f = fopen("arguments.bin", "rb");
    int launched = path && (inject == NONE || inject >= INTERRUPT);
    CHECK((f != NULL) == launched, "sampler launches only after setup succeeds");
    if (f) {
        char args[8192]; size_t size = fread(args, 1, sizeof args, f); assert(!ferror(f)); fclose(f);
        char pid[32]; snprintf(pid, sizeof pid, "%d", child);
        const char *expected[] = {NULL, pid, "1", "-f", path};
        size_t offset = 0;
        for (int i = 0; i < 5; i++) {
            size_t end = offset;
            while (end < size && args[end]) end++;
            CHECK(end < size && (!expected[i] || !strcmp(args + offset, expected[i])), "sampler receives the exact PID, duration, flag and path");
            offset = end + 1;
        }
        CHECK(offset == size, "sampler receives exactly five arguments");
    }
    printf("%s\n", name);
}
int main(int argc, char **argv) {
    if (argc > 1) return recorder(argc, argv);
    char executable[PATH_MAX]; assert(realpath(argv[0], executable));
    char directory[] = "/tmp/dm-watch-sample-XXXXXX";
    assert(mkdtemp(directory)); assert(!chdir(directory));
    assert(!symlink(executable, "sampler"));
    run_case(NULL, NONE, "unset output path skips sampling");
    run_case("", NONE, "empty output path is still passed");
    run_case("/tmp/ordinary sample.txt", NONE, "spaces in an output path");
    run_case("/tmp/John's Mac/hang.txt", NONE, "apostrophe in an output path");
    run_case("sample'; printf injected > injected-marker; #", NONE, "shell syntax is a literal output path");
    run_case("/tmp/line\n\"$(`echo bad`)\\*?;\xc3\xa9.txt", NONE, "newlines, metacharacters and UTF-8 path bytes");
    char long_path[1601]; memset(long_path, 'x', sizeof long_path - 1); long_path[sizeof long_path - 1] = 0;
    run_case(long_path, NONE, "output arguments are never truncated");
    run_case("result", INIT, "spawn action allocation failure");
    run_case("result", OPEN, "stdout redirection setup failure");
    run_case("result", DUP, "stderr redirection setup failure");
    run_case("result", SPAWN, "sampler launch failure");
    run_case("result", INTERRUPT, "interrupted wait resumes until sampler completion");
    run_case("result", WAIT, "wait failure is reported");
    run_case("result", EXIT_BAD, "nonzero sampler status is reported");
    run_case("result", SIGNAL_BAD, "sampler signal is reported");
    assert(unlink("arguments.bin") == 0 || errno == ENOENT);
    assert(unlink("injected-marker") == 0 || errno == ENOENT);
    assert(!unlink("sampler")); assert(!chdir("/")); assert(!rmdir(directory));
    printf("watchdog sample: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
