// TOTK: publishing pipeline archive files when several processes share one pipelines/ folder (the game, the build's
// pipeline-archive mode, a second copy), and reading it back. No Metal: the folder and the lock file are arguments, so
// tests/test_pipeline_archive_publish.py checks it with real processes. Used by totk_pipeline_archive.h.
/* A batch is two files: <name>.metalarchive and its key list <name>.keys, written after it; a file without its pair is a
   write cut short. Until 2026-11-04 a writer named its batch from its first key, wrote <name>.metalarchive.tmp and renamed
   it over <name>: two writers with one name shared the temp file and replaced each other's files, a failed write removed
   the name another writer had published, and a start that read the folder while a writer was between its two files
   removed the half as a write cut short.
   Now:
   - temp files are reserved (mkstemps), so no writer, and no remnant of an earlier run, shares one;
   - a file is published with renamex_np(RENAME_EXCL), which never replaces a file: a name taken goes to <name>-2, -3, ...;
   - a writer removes only its own temp files and what it published itself;
   - the lock file <cache>/pipelines.lock (beside pipelines/, so wiping the folder never replaces it) is held shared by
     each publication on its own open file description (flock locks per description: a thread that finishes cannot
     release another's), from its temp files until both files are published;
   - removing what writes cut short, and wiping the folder for another identity, take the lock exclusively without
     waiting: while anyone publishes, nothing is removed (the half-published batch is complete by the next start).
   Without the lock nothing is published (no unguarded files) and nothing is removed. The lock file is never removed. */
#pragma once
#import <Foundation/Foundation.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>

static int par_pub_lock(NSString *lock, int op) {   // a new open of the lock file holding op (LOCK_SH, LOCK_EX | LOCK_NB), or -1
    if (!lock) return -1;
    int fd = open(lock.fileSystemRepresentation, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    int r; while ((r = flock(fd, op)) < 0 && errno == EINTR) {}
    if (r) { close(fd); return -1; }
    return fd;
}
static void par_pub_unlock(int fd) { if (fd >= 0) close(fd); }   // closing this description releases its lock only

static NSString *par_pub_temp(NSString *dir) {   // a reserved, empty temp file in dir, or nil
    char path[PATH_MAX];
    if (snprintf(path, sizeof path, "%s/tmp-XXXXXXXX.tmp", dir.fileSystemRepresentation) >= (int)sizeof path) return nil;
    int fd = mkstemps(path, 4);
    if (fd < 0) return nil;
    close(fd);
    return @(path);
}

/* Publishes one batch under base (or base-2, -3, ... when taken): write(temp) fills the archive file, keys its key list.
   Returns the name published, or nil with nothing of this writer left in dir. between (tests only, else NULL) runs after
   the archive file is published and before its key list; NO from it abandons the batch. */
static NSString *par_pub_batch(NSString *dir, NSString *lock, NSString *base, BOOL (^write)(NSString *temp), NSString *keys,
                               BOOL (^between)(NSString *name)) {
    int fd = par_pub_lock(lock, LOCK_SH);
    if (fd < 0) return nil;
    NSString *ta = par_pub_temp(dir), *tl = par_pub_temp(dir), *name = nil, *archive = nil;
    BOOL ok = ta && tl && write(ta) && [keys writeToFile:tl atomically:NO encoding:NSUTF8StringEncoding error:nil];
    for (int i = 1; ok && !name && i < 1000; i++) {
        NSString *n = i == 1 ? base : [base stringByAppendingFormat:@"-%d", i];
        NSString *a = [dir stringByAppendingPathComponent:[n stringByAppendingPathExtension:@"metalarchive"]];
        NSString *l = [dir stringByAppendingPathComponent:[n stringByAppendingPathExtension:@"keys"]];
        if (renamex_np(ta.fileSystemRepresentation, a.fileSystemRepresentation, RENAME_EXCL)) { ok = errno == EEXIST; continue; }
        archive = a;
        if (between && !between(n)) { ok = NO; break; }
        if (renamex_np(tl.fileSystemRepresentation, l.fileSystemRepresentation, RENAME_EXCL) == 0) { name = n; break; }
        ok = errno == EEXIST;   // a key list left without its file: put the archive back and try the next name
        if (ok && renamex_np(a.fileSystemRepresentation, ta.fileSystemRepresentation, RENAME_EXCL)) ok = NO;
        if (ok) archive = nil;
    }
    if (!name) {   // only this writer's files
        if (archive) unlink(archive.fileSystemRepresentation);
        if (ta) unlink(ta.fileSystemRepresentation);
        if (tl) unlink(tl.fileSystemRepresentation);
    }
    par_pub_unlock(fd);
    return name;
}

/* The folder's complete batches, oldest first (the key list's modification time, then the name: a later batch wins a
   key both list). With clean, files that are no complete batch (writes cut short, temp files) are removed, but only
   when no one publishes (the exclusive lock, not waited for); keep: names never removed (identity.json). */
static NSArray<NSString *> *par_pub_scan(NSString *dir, NSString *lock, BOOL clean, NSSet<NSString *> *keep) {
    NSFileManager *fm = NSFileManager.defaultManager;
    int fd = clean ? par_pub_lock(lock, LOCK_EX | LOCK_NB) : -1;   // before listing: a batch seen half-published while
    NSArray<NSString *> *names = [fm contentsOfDirectoryAtPath:dir error:nil] ?: @[];   // unlocked may be complete by now
    NSSet<NSString *> *present = [NSSet setWithArray:names];
    NSMutableArray<NSString *> *bases = [NSMutableArray array], *junk = [NSMutableArray array];
    NSMutableDictionary<NSString *, NSDate *> *when = [NSMutableDictionary dictionary];
    for (NSString *name in names) {
        NSString *base = name.stringByDeletingPathExtension, *ext = name.pathExtension;
        if ([ext isEqualToString:@"keys"] && [present containsObject:[base stringByAppendingPathExtension:@"metalarchive"]]) {
            [bases addObject:base];
            when[base] = [[fm attributesOfItemAtPath:[dir stringByAppendingPathComponent:name] error:nil] fileModificationDate] ?: NSDate.distantPast;
        } else if (!([ext isEqualToString:@"metalarchive"] && [present containsObject:[base stringByAppendingPathExtension:@"keys"]]) && ![keep containsObject:name])
            [junk addObject:name];
    }
    if (fd >= 0) for (NSString *name in junk) [fm removeItemAtPath:[dir stringByAppendingPathComponent:name] error:nil];
    par_pub_unlock(fd);
    [bases sortUsingComparator:^NSComparisonResult(NSString *x, NSString *y) {
        NSComparisonResult r = [when[x] compare:when[y]];
        return r != NSOrderedSame ? r : [x compare:y];
    }];
    return bases;
}

/* Removes dir (its files are another identity's) unless someone publishes there now: YES when it is gone. */
static BOOL par_pub_wipe(NSString *dir, NSString *lock) {
    int fd = par_pub_lock(lock, LOCK_EX | LOCK_NB);
    if (fd < 0) return NO;
    [NSFileManager.defaultManager removeItemAtPath:dir error:nil];
    BOOL gone = ![NSFileManager.defaultManager fileExistsAtPath:dir];
    par_pub_unlock(fd);
    return gone;
}
