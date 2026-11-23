// TOTK: the build folders Metal's archive builder leaves (com.apple.gpuarchiver/gpuarchiver-*, see totk_pipeline_archive.h's
// top) and the pipeline archive's batches that make them. Included by totk_pipeline_archive.h; MLOG comes from its includer.
#pragma once
#import <Foundation/Foundation.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/file.h>
#include <stdatomic.h>
#include <unistd.h>
/* The folder Metal's archive builder works in (see the top) is com.apple.gpuarchiver under a cache folder, inside a folder
   named after the bundle id when the program has one (iPhone 14: Library/Caches/<bundle id>/com.apple.gpuarchiver; a bare
   Mac binary such as build/totk-native-v143: <user cache folder>/com.apple.gpuarchiver; a packaged Mac app:
   <user cache folder>/<bundle id>/...). The first version took only confstr's user cache folder, which the iPhone app does
   not get, and removed nothing there (2026-10-21). So every candidate is watched: confstr's folder and the Caches folder,
   each with and without the bundle id. Folders under the bundle id are this app's own (own: 1); the others are shared with
   other programs. The game binaries carry a bundle id of their own (runtime/game_info.plist, linked into __TEXT,__info_plist),
   so a bare one such as the build's pipeline-archive mode works in an own folder too. */
static pthread_mutex_t g_par_wmu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_par_ga_removed;
static NSArray<NSDictionary *> *g_par_probe_ga_dirs;   // native archive probe only: isolated owned/shared folders
static NSArray<NSDictionary *> *par_ga_dirs(void) {   // @{path, own}
    if (g_par_probe_ga_dirs) return g_par_probe_ga_dirs;
    static NSArray *dirs; static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableArray *bases = [NSMutableArray array];
        char b[PATH_MAX]; if (confstr(_CS_DARWIN_USER_CACHE_DIR, b, sizeof b)) [bases addObject:@(b)];
        NSString *caches = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
        if (caches) [bases addObject:caches];
        NSString *bundle = NSBundle.mainBundle.bundleIdentifier;
        NSMutableArray *out = [NSMutableArray array]; NSMutableSet *seen = [NSMutableSet set];
        for (NSString *base in bases) for (int own = bundle.length ? 1 : 0; own >= 0; own--) {
            NSString *dir = [(own ? [base stringByAppendingPathComponent:bundle] : base) stringByAppendingPathComponent:@"com.apple.gpuarchiver"];
            dir = dir.stringByResolvingSymlinksInPath.stringByStandardizingPath;
            if (![seen containsObject:dir]) { [seen addObject:dir]; [out addObject:@{@"path": dir, @"own": @(own)}]; }
        }
        dirs = out;
    });
    return dirs;
}
static NSSet<NSString *> *par_ga_list(int own_only) {   // full paths of the build folders there now
    NSMutableSet *s = [NSMutableSet set];
    for (NSDictionary *d in par_ga_dirs()) {
        if (own_only && ![d[@"own"] boolValue]) continue;
        for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:d[@"path"] error:nil] ?: @[])
            if ([name hasPrefix:@"gpuarchiver-"]) [s addObject:[d[@"path"] stringByAppendingPathComponent:name]];
    }
    return s;
}
/* Who removes: the folders of programs without this app's bundle id are not this app's to remove, so only its own folders
   are cleaned (before 2026-11-04 every folder that appeared during a batch went, in the shared folder too, where another
   program's live folder could be among them). Several processes of the app share its own folders (the game and the build's
   pipeline-archive mode, two builds), so a folder is removed only while no process of the app has a batch open: each
   process holds a shared lock (flock LOCK_SH) on <own cache folder>/pipeline-archive.lock from the moment its first batch
   begins, before Metal makes a folder, until its last batch ends. Cleaning takes the exclusive lock without waiting; while
   another process holds the shared one it is refused and nothing is removed: the last batch to end, or the next start,
   removes them. With no batch open in this process, nothing of it needs the shared lock, so the non-atomic upgrade from
   shared to exclusive (flock releases the old lock first) leaves no live folder unguarded. Without the shared lock (no own
   folder, the file cannot be opened or locked) no batch begins: its folder would be unguarded, so the pipeline is not
   written to the archive (it is compiled as usual; a later start writes it). The file is never removed or replaced, so
   every process locks the same file; the system releases a dead process's lock. */
static int g_par_batches;        // batches open in this process (g_par_wmu)
static int g_par_lock_fd = -1;   // g_par_wmu; kept open
static NSString *g_par_lock_at;  // its path: the probe's folders first, then the real ones (no batch open in between)
static int par_flock(int fd, int op) { int r; while ((r = flock(fd, op)) < 0 && errno == EINTR) {} return r; }
static int par_ga_lock_fd(void) {   // g_par_wmu held; -1 without an own folder or when the file cannot be opened
    NSString *path = nil;
    for (NSDictionary *d in par_ga_dirs()) if ([d[@"own"] boolValue]) { path = [[d[@"path"] stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"pipeline-archive.lock"]; break; }
    if (g_par_lock_fd >= 0 && ![g_par_lock_at isEqualToString:path]) { close(g_par_lock_fd); g_par_lock_fd = -1; }
    if (g_par_lock_fd < 0 && path) {
        [NSFileManager.defaultManager createDirectoryAtPath:path.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
        g_par_lock_fd = open(path.fileSystemRepresentation, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        g_par_lock_at = path;
        if (g_par_lock_fd < 0) { static _Atomic int said; if (!atomic_fetch_add(&said, 1)) MLOG("[pipeline-archive] cannot open %s (%s): Metal's build folders are not removed", path.UTF8String, strerror(errno)); }
    }
    return g_par_lock_fd;
}
/* g_par_wmu held. Removes every build folder in the own folders if no batch is open in this process or another; how many,
   or -1 when another process has a batch open, this one has, or the lock failed. */
static long par_ga_clean_locked(void) {
    int fd = g_par_batches ? -1 : par_ga_lock_fd();
    if (fd < 0) return -1;
    if (par_flock(fd, LOCK_EX | LOCK_NB)) { par_flock(fd, LOCK_UN); return -1; }
    long n = 0;
    for (NSString *path in par_ga_list(1)) {
        NSError *e = nil;
        if ([NSFileManager.defaultManager removeItemAtPath:path error:&e]) { n++; atomic_fetch_add(&g_par_ga_removed, 1); }
        else { static _Atomic int said; if (atomic_fetch_add(&said, 1) < 4) MLOG("【红】管线存档：删不掉 Metal 留下的工作目录 %s（%s）", path.UTF8String, e.localizedDescription.UTF8String); }
    }
    par_flock(fd, LOCK_UN);
    return n;
}
static void par_ga_cleanup_startup(void) {   // what earlier runs left; skipped while a batch is open here or elsewhere
    pthread_mutex_lock(&g_par_wmu);
    long n = par_ga_clean_locked();
    pthread_mutex_unlock(&g_par_wmu);
    NSMutableArray *where = [NSMutableArray array];
    for (NSDictionary *d in par_ga_dirs()) [where addObject:[NSString stringWithFormat:@"%@ (%@)", d[@"path"], [d[@"own"] boolValue] ? @"this app's, cleaned" : @"shared, never cleaned"]];
    if (n < 0) MLOG("[pipeline-archive] Metal's build folders: %s; none removed at start (a batch is open in this or another process of the app, or no own folder)",
                    [where componentsJoinedByString:@", "].UTF8String);
    else MLOG("[pipeline-archive] Metal's build folders: %s; %ld left by earlier runs removed at start", [where componentsJoinedByString:@", "].UTF8String, n);
}
static int par_batch_begin(void) {   // before the batch's archive makes its build folder; 0: no batch, write nothing
    pthread_mutex_lock(&g_par_wmu);
    int ok = 1;
    if (g_par_batches == 0) {   // the process's first open batch takes the shared lock; later ones run under it
        int fd = par_ga_lock_fd();
        ok = fd >= 0 && par_flock(fd, LOCK_SH) == 0;
        if (!ok) { static _Atomic int said; if (!atomic_fetch_add(&said, 1)) MLOG("[pipeline-archive] no shared lock on %s (%s): pipelines are not written to the archive while it fails",
                                                                                   g_par_lock_at ? g_par_lock_at.UTF8String : "(no own cache folder: no bundle id)", fd >= 0 ? strerror(errno) : "cannot open"); }
    }
    if (ok) g_par_batches++;
    pthread_mutex_unlock(&g_par_wmu);
    return ok;
}
static void par_batch_end(void) {   // the batch's archive is released: the folders go once no batch of the app is open
    pthread_mutex_lock(&g_par_wmu);
    if (--g_par_batches == 0) par_ga_clean_locked();   // which also releases the shared lock
    pthread_mutex_unlock(&g_par_wmu);
}
