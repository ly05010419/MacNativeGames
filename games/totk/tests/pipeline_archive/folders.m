/* The pipeline archive's cleanup of Metal's build folders (totk_pipeline_archive_folders.h), on folders of its own:
   no GPU, game or player data. Run by tests/test_pipeline_archive_folders.py.
     folders <dir> shared   a folder another program makes in the shared cache during a batch survives the batch's end
     folders <dir> peers    two processes of the app, kept in step through pipes: neither one's startup nor its batch's
                            end removes the other's live folder, and the last batch to end removes both
     folders <dir> nolock   the lock file cannot be opened: no batch begins (so Metal makes no unguarded folder) and
                            nothing is left held, so a cleanup once the file opens again removes what was left
     folders <dir> noown    no own cache folder (no bundle id): no batch begins */
#include <stdio.h>
#include <stdlib.h>
#define MLOG(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
#include "../../third_party/d2r_gfx/gfx/nvn/totk_pipeline_archive_folders.h"
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;
static NSString *g_owned, *g_shared;
#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

static void fixture(const char *dir) {
    NSString *d = @(dir);
    g_owned = [d stringByAppendingPathComponent:@"own/com.apple.gpuarchiver"];
    g_shared = [d stringByAppendingPathComponent:@"shared/com.apple.gpuarchiver"];
    g_par_probe_ga_dirs = @[@{@"path": g_owned, @"own": @YES}, @{@"path": g_shared, @"own": @NO}];
}
static NSString *make(NSString *in, NSString *name) {
    NSString *path = [in stringByAppendingPathComponent:name];
    if (![NSFileManager.defaultManager createDirectoryAtPath:path withIntermediateDirectories:YES attributes:nil error:nil]) FAIL("cannot make %s", path.UTF8String);
    return path;
}
static int there(NSString *path) { return [NSFileManager.defaultManager fileExistsAtPath:path]; }
static void say(int fd, char c) { if (write(fd, &c, 1) != 1) FAIL("pipe write"); }
static void hear(int fd, char want) {
    char c = 0;
    alarm(20);
    if (read(fd, &c, 1) != 1 || c != want) FAIL("expected '%c' from the other process, got '%c'", want, c ? c : '-');
    alarm(0);
}

static void shared_case(void) {
    NSString *unrelated = make(g_owned, @"keep-this-entry");
    par_ga_cleanup_startup();
    if (!par_batch_begin()) FAIL("no batch began");
    NSString *mine = make(g_owned, @"gpuarchiver-mine"), *other = make(g_shared, @"gpuarchiver-other");
    par_batch_end();
    if (there(mine)) FAIL("the batch's own folder is left");
    if (!there(other)) FAIL("a shared folder made during the batch was removed");
    if (!there(unrelated)) FAIL("an entry that is no build folder was removed");
    puts("PASS: shared folder made during a batch kept, own folder removed");
}

static void nolock_case(const char *dir) {
    NSString *lock = make([@(dir) stringByAppendingPathComponent:@"own"], @"pipeline-archive.lock");   // a folder: open() fails
    if (par_batch_begin()) FAIL("par_batch_begin said the batch began");
    if (g_par_batches) FAIL("a batch began without the shared lock (%d open)", g_par_batches);
    if (![NSFileManager.defaultManager removeItemAtPath:lock error:nil]) FAIL("cannot remove %s", lock.UTF8String);
    NSString *old = make(g_owned, @"gpuarchiver-old");
    par_ga_cleanup_startup();
    if (there(old)) FAIL("once the lock file opens, the cleanup did not run (a batch or lock left over)");
    puts("PASS: without the lock no batch begins, nothing is left held");
}

static void noown_case(void) {
    g_par_probe_ga_dirs = @[@{@"path": g_shared, @"own": @NO}];
    if (par_batch_begin()) FAIL("par_batch_begin said the batch began");
    if (g_par_batches) FAIL("a batch began without an own cache folder (%d open)", g_par_batches);
    puts("PASS: without an own cache folder no batch begins");
}

static void peer(int in, int out) {   // the second process: its startup, a batch held open, then its end
    hear(in, 's');
    par_ga_cleanup_startup();
    if (!par_batch_begin()) FAIL("no batch began");
    make(g_owned, @"gpuarchiver-peer");
    say(out, 'r');
    hear(in, 'e');
    par_batch_end();
    say(out, 'd');
    exit(0);
}

static void peers_case(const char *self, const char *dir) {
    int to[2], from[2];
    if (pipe(to) || pipe(from)) FAIL("pipe");
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, to[0], 3); posix_spawn_file_actions_adddup2(&fa, from[1], 4);
    char *argv[] = {(char *)self, (char *)dir, "peer", NULL};
    pid_t pid;
    if (posix_spawn(&pid, self, &fa, NULL, argv, environ)) FAIL("spawn the second process");
    close(to[0]); close(from[1]);
    par_ga_cleanup_startup();
    if (!par_batch_begin()) FAIL("no batch began");   // this process's batch is open before the other one starts
    NSString *mine = make(g_owned, @"gpuarchiver-first"), *its = [g_owned stringByAppendingPathComponent:@"gpuarchiver-peer"];
    say(to[1], 's'); hear(from[0], 'r');
    if (!there(mine)) FAIL("the second process's startup removed the first one's live folder");
    if (!there(its)) FAIL("the second process's folder is missing");
    par_batch_end();   // the other's batch is still open: nothing may go
    if (!there(its)) FAIL("the first process's batch end removed the second one's live folder");
    if (!there(mine)) FAIL("the first process removed a folder while the second had a batch open");
    say(to[1], 'e'); hear(from[0], 'd');
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) FAIL("the second process failed (%d)", status);
    if (there(mine) || there(its)) FAIL("the last batch to end left build folders (first %d, second %d)", there(mine), there(its));
    puts("PASS: two processes keep each other's live folders, the last batch to end removes both");
}

int main(int argc, char **argv) {
    if (argc != 3) FAIL("usage: folders <dir> shared|peers");
    @autoreleasepool {
        fixture(argv[1]);
        if (!strcmp(argv[2], "peer")) peer(3, 4);
        else if (!strcmp(argv[2], "shared")) shared_case();
        else if (!strcmp(argv[2], "peers")) peers_case(argv[0], argv[1]);
        else if (!strcmp(argv[2], "nolock")) nolock_case(argv[1]);
        else if (!strcmp(argv[2], "noown")) noown_case();
        else FAIL("unknown case %s", argv[2]);
    }
    return 0;
}
