/* Publishing pipeline archive batches when processes share one pipelines/ folder (totk_pipeline_archive_publish.h): no
   GPU, game or player data. Run by tests/test_pipeline_archive_publish.py; every case leaves its checks to this program.
     publish <dir> same     two processes publish under one name at once: each keeps its own pair
     publish <dir> midload  a start reads the folder while a batch is half published: the batch survives, complete
     publish <dir> fail     a writer that fails, before or after publishing its archive, removes only its own files
     publish <dir> stale    a start with no writer removes temp files and halves; complete batches and identity.json stay
     publish <dir> threads  two threads publish; one finishes: the other's half batch is still guarded
     publish <dir> wipe     wiping the folder for another identity waits for a publication; the lock file stays the same */
#include <stdio.h>
#include <stdlib.h>
#include "../../third_party/d2r_gfx/gfx/nvn/totk_pipeline_archive_publish.h"
#include <pthread.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char **environ;
static NSString *g_dir, *g_lock;
#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)

static void say(int fd, char c) { if (write(fd, &c, 1) != 1) FAIL("pipe write"); }
static void hear(int fd, char want) {
    char c = 0; alarm(20);
    if (read(fd, &c, 1) != 1 || c != want) FAIL("expected '%c' from the other process, got '%c'", want, c ? c : '-');
    alarm(0);
}
static NSString *path(NSString *name) { return [g_dir stringByAppendingPathComponent:name]; }
static int there(NSString *name) { return [NSFileManager.defaultManager fileExistsAtPath:path(name)]; }
static NSString *text(NSString *name) { return [NSString stringWithContentsOfFile:path(name) encoding:NSUTF8StringEncoding error:nil]; }
static BOOL (^bytes(NSString *what))(NSString *) {
    return ^BOOL(NSString *temp) { return [what writeToFile:temp atomically:NO encoding:NSUTF8StringEncoding error:nil]; };
}
static NSString *publish(NSString *base, NSString *content, NSString *keys, BOOL (^between)(NSString *)) {
    return par_pub_batch(g_dir, g_lock, base, bytes(content), keys, between);
}
/* Every complete batch pairs the archive and key list its writer wrote (content X with keys kX), nothing else is left. */
static NSArray<NSString *> *check_folder(NSUInteger want) {
    NSArray<NSString *> *bases = par_pub_scan(g_dir, g_lock, NO, nil);
    if (bases.count != want) FAIL("%lu complete batches, want %lu: %s", (unsigned long)bases.count, (unsigned long)want, bases.description.UTF8String);
    for (NSString *b in bases) {
        NSString *a = text([b stringByAppendingPathExtension:@"metalarchive"]), *k = text([b stringByAppendingPathExtension:@"keys"]);
        if (![k isEqualToString:[@"k" stringByAppendingString:a ?: @"?"]]) FAIL("batch %s pairs archive %s with keys %s", b.UTF8String, a.UTF8String, k.UTF8String);
    }
    NSUInteger files = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:g_dir error:nil] count];
    if (files != 2 * want) FAIL("%lu files in the folder, want %lu", (unsigned long)files, (unsigned long)(2 * want));
    return bases;
}
static pid_t spawn(const char *self, const char *role, int *to, int *from) {
    int a[2], b[2];
    if (pipe(a) || pipe(b)) FAIL("pipe");
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, a[0], 3); posix_spawn_file_actions_adddup2(&fa, b[1], 4);
    char *argv[] = {(char *)self, (char *)g_dir.stringByDeletingLastPathComponent.fileSystemRepresentation, (char *)role, NULL};
    pid_t pid;
    if (posix_spawn(&pid, self, &fa, NULL, argv, environ)) FAIL("spawn the second process");
    close(a[0]); close(b[1]); *to = a[1]; *from = b[0];
    return pid;
}
static void reap(pid_t pid) {
    int status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) FAIL("the second process failed (%d)", status);
}

/* The second process's parts, told what to do through fd 3 and answering on fd 4. */
static void peer(const char *role) {
    if (!strcmp(role, "peer-same")) {   // publishes the same name while the first holds it half published
        hear(3, 'g'); if (!publish(@"x-1", @"B", @"kB", ^BOOL(NSString *n) { (void)n; return YES; })) FAIL("the second writer published nothing");
        say(4, 'n');
    } else if (!strcmp(role, "peer-scan")) {   // a start reading the folder, with its clean-up
        hear(3, 's'); par_pub_scan(g_dir, g_lock, YES, nil); say(4, 'd');
    } else if (!strcmp(role, "peer-wipe")) {
        struct stat before, after;
        hear(3, 'w');
        if (stat(g_lock.fileSystemRepresentation, &before)) FAIL("no lock file");
        if (par_pub_wipe(g_dir, g_lock) || ![NSFileManager.defaultManager fileExistsAtPath:g_dir]) FAIL("the folder was wiped while a batch was being published");
        say(4, 'd'); hear(3, 'e');
        if (!par_pub_wipe(g_dir, g_lock) || [NSFileManager.defaultManager fileExistsAtPath:g_dir]) FAIL("the folder was not wiped once no one published");
        if (stat(g_lock.fileSystemRepresentation, &after) || after.st_ino != before.st_ino) FAIL("the lock file was replaced");
        say(4, 'd');
    } else FAIL("unknown role %s", role);
    exit(0);
}

static void case_same(const char *self) {
    int to, from; pid_t pid = spawn(self, "peer-same", &to, &from);
    NSString *mine = publish(@"x-1", @"A", @"kA", ^BOOL(NSString *n) { (void)n; say(to, 'g'); hear(from, 'n'); return YES; });
    reap(pid);
    if (!mine) FAIL("the first writer published nothing");
    check_folder(2);
    puts("PASS: two writers under one name keep their own batches");
}
static void case_midload(const char *self) {
    int to, from; pid_t pid = spawn(self, "peer-scan", &to, &from);
    NSString *mine = publish(@"x-1", @"A", @"kA", ^BOOL(NSString *n) {
        (void)n; say(to, 's'); hear(from, 'd');   // the other process has read and cleaned the folder meanwhile
        if (!there(@"x-1.metalarchive")) FAIL("a start removed a half-published batch");
        return YES;
    });
    reap(pid);
    if (![mine isEqualToString:@"x-1"]) FAIL("the batch was not published");
    check_folder(1);
    puts("PASS: a batch read half published survives and is complete");
}
static void case_fail(void) {
    if (!publish(@"p-1", @"A", @"kA", nil)) FAIL("the first batch was not published");
    if (par_pub_batch(g_dir, g_lock, @"p-1", ^BOOL(NSString *t) { (void)t; return NO; }, @"kB", nil)) FAIL("a failed write was published");
    if (publish(@"p-1", @"C", @"kC", ^BOOL(NSString *n) { return ![n isEqualToString:@"p-1-2"]; })) FAIL("an abandoned batch was published");
    check_folder(1);
    puts("PASS: a failed writer leaves only the other writer's batch");
}
static void case_stale(void) {
    if (!publish(@"q-1", @"A", @"kA", nil)) FAIL("the batch was not published");
    for (NSString *n in @[@"tmp-ABCDEFGH.tmp", @"y-1.metalarchive", @"z-1.keys", @"identity.json"])
        if (![@"x" writeToFile:path(n) atomically:NO encoding:NSUTF8StringEncoding error:nil]) FAIL("cannot write %s", n.UTF8String);
    par_pub_scan(g_dir, g_lock, YES, [NSSet setWithObject:@"identity.json"]);
    if (!there(@"identity.json") || there(@"tmp-ABCDEFGH.tmp") || there(@"y-1.metalarchive") || there(@"z-1.keys")) FAIL("the clean-up kept a remnant or removed identity.json");
    [NSFileManager.defaultManager removeItemAtPath:path(@"identity.json") error:nil];
    check_folder(1);
    puts("PASS: a start alone removes what writes cut short");
}
static dispatch_semaphore_t g_reached, g_go[2];
static void *thread_writer(void *arg) {
    long i = (long)arg;
    if (!publish(i ? @"t2-1" : @"t1-1", i ? @"B" : @"A", i ? @"kB" : @"kA", ^BOOL(NSString *n) {
        (void)n; dispatch_semaphore_signal(g_reached);
        return dispatch_semaphore_wait(g_go[i], dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC)) == 0;
    })) FAIL("thread %ld published nothing", i + 1);
    return NULL;
}
static void case_threads(const char *self) {
    int to, from; pid_t pid = spawn(self, "peer-scan", &to, &from);
    g_reached = dispatch_semaphore_create(0); g_go[0] = dispatch_semaphore_create(0); g_go[1] = dispatch_semaphore_create(0);
    pthread_t t[2];
    for (long i = 0; i < 2; i++) pthread_create(&t[i], NULL, thread_writer, (void *)i);
    for (int i = 0; i < 2; i++) if (dispatch_semaphore_wait(g_reached, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC))) FAIL("a thread did not reach its half");
    dispatch_semaphore_signal(g_go[0]); pthread_join(t[0], NULL);   // the first is done and has let its lock go
    say(to, 's'); hear(from, 'd');
    if (!there(@"t2-1.metalarchive")) FAIL("the other thread's half batch was removed after the first thread finished");
    dispatch_semaphore_signal(g_go[1]); pthread_join(t[1], NULL);
    reap(pid);
    check_folder(2);
    puts("PASS: one thread finishing leaves the other's batch guarded");
}
static void case_wipe(const char *self) {
    int to, from; pid_t pid = spawn(self, "peer-wipe", &to, &from);
    if (!publish(@"w-1", @"A", @"kA", ^BOOL(NSString *n) { (void)n; say(to, 'w'); hear(from, 'd'); return YES; })) FAIL("the batch was not published");
    say(to, 'e'); hear(from, 'd');
    reap(pid);
    if (![NSFileManager.defaultManager fileExistsAtPath:g_lock]) FAIL("the lock file went with the folder");
    puts("PASS: the folder is wiped only once no one publishes, the lock file stays");
}

int main(int argc, char **argv) {
    if (argc != 3) FAIL("usage: publish <dir> same|midload|fail|stale|threads|wipe");
    @autoreleasepool {
        g_dir = [@(argv[1]) stringByAppendingPathComponent:@"pipelines"];
        g_lock = [@(argv[1]) stringByAppendingPathComponent:@"pipelines.lock"];
        if (!strncmp(argv[2], "peer-", 5)) peer(argv[2]);
        if (![NSFileManager.defaultManager createDirectoryAtPath:g_dir withIntermediateDirectories:YES attributes:nil error:nil]) FAIL("no folder");
        const char *c = argv[2];
        if (!strcmp(c, "same")) case_same(argv[0]);
        else if (!strcmp(c, "midload")) case_midload(argv[0]);
        else if (!strcmp(c, "fail")) case_fail();
        else if (!strcmp(c, "stale")) case_stale();
        else if (!strcmp(c, "threads")) case_threads(argv[0]);
        else if (!strcmp(c, "wipe")) case_wipe(argv[0]);
        else FAIL("unknown case %s", c);
    }
    return 0;
}
