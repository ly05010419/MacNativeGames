/* The Update page's update (Mac; included by app/launcher.m; user 2026-10-31: "点击按钮 然后给用户更新 更新不了 就提示错误
   让用户自己更新代码 重新编译"). Nothing happens by itself: the settings panel's Update page (runtime/update_tab.h) starts
   the launcher with --update-check when it opens, and with --update-install when the player presses its Update button.
   The check looks in the checkout that made the app (Info.plist MNGSourceRepo, build_app.record_update_source), fetches
   upstream and lists what upstream's main has that the app lacks, without changing the checkout. The install, once the
   game has closed, brings main up to upstream with a fast-forward and puts the new runtime into the app with
   update_runtime.py (--seed when the translation chain changed), as a player would by hand (docs/install.md "Update the
   app"). A checkout that is not plainly upstream's (another branch, uncommitted changes, commits upstream lacks), one
   that cannot be found or reached, or an update that fails, is left to the player: the page and the launcher say why and
   how to update the code and build again by hand.
   What happened goes to update/status.json in the game's data folder (the page reads it) and the updater's output to
   update/update.log. An agent's test session (TOTK_SLOT_OWNER other than user) never updates. */
#pragma once
#import <Foundation/Foundation.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <unistd.h>

enum { MNG_UPDATE_SHOWN = 30 };   /* commit titles kept for the page */
#define MNG_UPDATE_FETCH_SECONDS 15

/* The game's data folder (the folder of TOTK_CONTROLS, where the runtime keeps its settings). */
static inline NSString *mng_update_data(void) {
    const char *controls = getenv("TOTK_CONTROLS");
    return controls && *controls ? @(controls).stringByDeletingLastPathComponent : nil;
}
/* An agent's test session never touches a checkout. */
static inline BOOL mng_update_held(void) {
    const char *owner = getenv("TOTK_SLOT_OWNER");
    return owner && *owner && strcmp(owner, "user");
}

/* A command's exit status (-1: it could not run or ran past `seconds`) and its output, stdout and stderr together. */
static inline int mng_update_command(NSString *dir, NSArray<NSString *> *argv, double seconds, NSString *log, NSString **output) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:argv[0]];
    task.arguments = [argv subarrayWithRange:NSMakeRange(1, argv.count - 1)];
    if (dir) task.currentDirectoryURL = [NSURL fileURLWithPath:dir isDirectory:YES];
    NSMutableDictionary *env = [NSProcessInfo.processInfo.environment mutableCopy];
    env[@"GIT_TERMINAL_PROMPT"] = @"0";   /* a fetch that wants a password fails instead of waiting */
    env[@"LC_ALL"] = @"C";
    task.environment = env;
    NSFileHandle *sink = nil; NSPipe *pipe = nil;
    if (log) {
        if (![NSFileManager.defaultManager fileExistsAtPath:log]) [NSFileManager.defaultManager createFileAtPath:log contents:nil attributes:nil];
        sink = [NSFileHandle fileHandleForWritingAtPath:log]; [sink seekToEndOfFile];
        task.standardOutput = sink; task.standardError = sink;
    } else {
        pipe = [NSPipe pipe]; task.standardOutput = pipe; task.standardError = pipe;
    }
    task.standardInput = [NSFileHandle fileHandleWithNullDevice];
    if (![task launchAndReturnError:nil]) return -1;
    __block BOOL late = NO;
    dispatch_source_t timer = nil;
    if (seconds > 0) {
        timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
        dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), DISPATCH_TIME_FOREVER, NSEC_PER_SEC);
        dispatch_source_set_event_handler(timer, ^{ late = YES; [task terminate]; });
        dispatch_resume(timer);
    }
    NSData *bytes = pipe ? [pipe.fileHandleForReading readDataToEndOfFile] : nil;
    [task waitUntilExit];
    if (timer) dispatch_source_cancel(timer);
    [sink closeFile];
    if (output) *output = bytes ? [[NSString alloc] initWithData:bytes encoding:NSUTF8StringEncoding] ?: @"" : @"";
    return late || task.terminationReason != NSTaskTerminationReasonExit ? -1 : task.terminationStatus;
}
static inline NSString *mng_update_git(NSString *repo, NSArray<NSString *> *args, int *status) {
    NSString *out = nil;
    int st = mng_update_command(repo, [@[@"/usr/bin/git"] arrayByAddingObjectsFromArray:args],   /* only the fetch is timed (the network) */
                                [args.firstObject isEqualToString:@"fetch"] ? MNG_UPDATE_FETCH_SECONDS : 0, nil, &out);
    if (status) *status = st;
    return [out stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

static inline void mng_update_write(NSString *folder, NSDictionary *fields) {
    NSMutableDictionary *d = [@{@"version": @1, @"at": @(NSDate.date.timeIntervalSince1970)} mutableCopy];
    [d addEntriesFromDictionary:fields];
    NSData *json = [NSJSONSerialization dataWithJSONObject:d options:NSJSONWritingPrettyPrinted error:nil];
    [json writeToFile:[folder stringByAppendingPathComponent:@"status.json"] atomically:YES];
}
/* The log from byte `from` on (this run's part). */
static inline NSString *mng_update_log_since(NSString *log, unsigned long long from) {
    NSData *all = [NSData dataWithContentsOfFile:log] ?: NSData.data;
    NSData *part = from < all.length ? [all subdataWithRange:NSMakeRange((NSUInteger)from, all.length - (NSUInteger)from)] : NSData.data;
    return [[NSString alloc] initWithData:part encoding:NSUTF8StringEncoding] ?: @"";
}
/* The last line of this run's log that says why it stopped. */
static inline NSString *mng_update_why(NSString *log, unsigned long long from) {
    NSArray<NSString *> *lines = [mng_update_log_since(log, from) componentsSeparatedByString:@"\n"];
    NSString *last = @"";
    for (NSString *line in lines.reverseObjectEnumerator) {
        NSString *t = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (!t.length) continue;
        if (!last.length) last = t;
        if ([t hasPrefix:@"ERROR"]) return t;
    }
    return last;
}

/* What upstream has for `app_commit` (the commit the app's runtime was built from), from `repo`, the checkout is not
   changed: "latest"; "behind" (it can be updated: to, commits newest first, total); "blocked" (it cannot: reason, detail,
   and the commits when known); "error" (reason, detail). Written to folder/status.json and returned. */
static inline NSDictionary *mng_update_check(NSString *folder, NSString *repo, NSString *app_commit) {
    [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    __block NSDictionary *what = @{};   /* the version upstream has and its commits the app lacks, once known */
    NSDictionary *(^end)(NSString *, NSString *, NSString *) = ^NSDictionary *(NSString *state, NSString *reason, NSString *detail) {
        NSMutableDictionary *d = [what mutableCopy];
        [d addEntriesFromDictionary:@{@"state": state, @"installed": app_commit ?: @""}];
        if (reason) { d[@"reason"] = reason; d[@"detail"] = detail ?: @""; }
        mng_update_write(folder, d);
        return d;
    };
    BOOL dir = NO;
    if (!repo.length || ![NSFileManager.defaultManager fileExistsAtPath:[repo stringByAppendingPathComponent:@"update_runtime.py"]] ||
        ![NSFileManager.defaultManager fileExistsAtPath:[repo stringByAppendingPathComponent:@".git"] isDirectory:&dir])
        return end(@"error", @"no_repo", repo);
    int st = 0;
    /* The first look into the checkout: macOS may ask the player first (a removable volume, Documents ...: Privacy &
       Security), so this waits as long as that takes (mng_update_git: no time limit but the fetch's). */
    NSString *said = mng_update_git(repo, @[@"rev-parse", @"--git-dir"], &st);
    if (st) {
        int fd = open([repo stringByAppendingPathComponent:@".git/HEAD"].fileSystemRepresentation, O_RDONLY | O_CLOEXEC), err = errno;
        if (fd >= 0) close(fd); else if (err == EPERM || err == EACCES) return end(@"error", @"no_access", repo);   /* the player said no */
        return end(@"error", @"git_failed", [said componentsSeparatedByString:@"\n"].lastObject);
    }
    mng_update_git(repo, @[@"fetch", @"--quiet", @"--no-tags"], &st);
    if (st) return end(@"error", @"fetch_failed", nil);
    /* upstream's main, whichever branch the checkout is on (that is checked below, once the commits are known) */
    NSString *newest = mng_update_git(repo, @[@"rev-parse", @"--short=7", @"main@{upstream}"], &st);
    if (st || !newest.length) return end(@"error", @"no_upstream", nil);
    if (app_commit.length && [newest hasPrefix:app_commit]) return end(@"latest", nil, nil);   /* the app runs upstream's newest */
    /* What upstream has that the app lacks: its commits, the newest first (no merges: those a player reads about). An app
       built from a commit this checkout does not have cannot be told apart. */
    BOOL known = NO;
    if (app_commit.length) { mng_update_git(repo, @[@"cat-file", @"-e", [app_commit stringByAppendingString:@"^{commit}"]], &st); known = !st; }
    NSString *range = [NSString stringWithFormat:@"%@..main@{upstream}", app_commit];
    NSString *titles = known ? mng_update_git(repo, @[@"log", @"--no-merges", @"--format=%s", [NSString stringWithFormat:@"-%d", MNG_UPDATE_SHOWN], range], &st) : @"";
    NSInteger total = known ? mng_update_git(repo, @[@"rev-list", @"--count", @"--no-merges", range], &st).integerValue : 0;
    what = @{@"from": app_commit ?: @"", @"to": newest, @"commits": titles.length ? [titles componentsSeparatedByString:@"\n"] : @[], @"total": @(total)};
    /* Only a checkout that is plainly upstream's is moved. */
    NSString *branch = mng_update_git(repo, @[@"symbolic-ref", @"--short", @"-q", @"HEAD"], &st);
    if (st || ![branch isEqualToString:@"main"]) return end(@"blocked", @"not_main", branch);
    NSString *dirty = mng_update_git(repo, @[@"status", @"--porcelain", @"--untracked-files=no"], &st);
    if (st || dirty.length) return end(@"blocked", @"local_changes", [dirty componentsSeparatedByString:@"\n"].firstObject);
    NSArray<NSString *> *counts = [mng_update_git(repo, @[@"rev-list", @"--left-right", @"--count", @"HEAD...@{upstream}"], &st)
                                   componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    if (st || counts.count < 2) return end(@"error", @"no_upstream", nil);
    if (counts.firstObject.integerValue > 0) return end(@"blocked", @"unpushed", nil);   /* commits upstream lacks: a working copy */
    return end(@"behind", nil, nil);
}

/* The Update button's update, the game closed: the check again, then the fast-forward and update_runtime.py. `installing`
   (may be nil) is told the version before update_runtime.py runs. Returns the state written: "updated", "failed", or the
   check's when there was nothing to install. */
static inline NSString *mng_update_install(NSString *folder, NSString *repo, NSString *app, NSString *app_commit, NSString *python, NSString *work,
                                           void (^installing)(NSString *to, NSInteger total)) {
    NSDictionary *checked = mng_update_check(folder, repo, app_commit);
    if (![checked[@"state"] isEqual:@"behind"]) return checked[@"state"];
    NSString *head = checked[@"to"];
    NSMutableDictionary *result = [checked mutableCopy];
    int st = 0;
    mng_update_git(repo, @[@"merge", @"--ff-only", @"--quiet", @"main@{upstream}"], &st);
    if (st) { [result addEntriesFromDictionary:@{@"state": @"failed", @"reason": @"local_changes", @"detail": @""}]; mng_update_write(folder, result); return @"failed"; }
    result[@"state"] = @"updating"; mng_update_write(folder, result);
    if (installing) installing(head, [checked[@"total"] integerValue]);
    NSString *log = [folder stringByAppendingPathComponent:@"update.log"];   /* the launcher's notes of this install come first */
    unsigned long long from = [[NSFileManager.defaultManager attributesOfItemAtPath:log error:nil] fileSize];
    NSMutableArray<NSString *> *argv = [@[python.length ? python : @"/usr/bin/env", @"update_runtime.py", app] mutableCopy];
    if (!python.length) [argv insertObject:@"python3.14" atIndex:1];
    if (work.length && [NSFileManager.defaultManager fileExistsAtPath:work]) [argv addObjectsFromArray:@[@"--work", work]];
    int done = mng_update_command(repo, argv, 0, log, NULL);
    if (done && [mng_update_log_since(log, from) rangeOfString:@"pass --seed" options:NSCaseInsensitiveSearch].location != NSNotFound)
        done = mng_update_command(repo, [argv arrayByAddingObject:@"--seed"], 0, log, NULL);   /* a new translation chain */
    if (done) [result addEntriesFromDictionary:@{@"state": @"failed", @"reason": @"update_failed", @"detail": mng_update_why(log, from)}];
    else [result addEntriesFromDictionary:@{@"state": @"updated", @"installed": head}];
    mng_update_write(folder, result);
    return result[@"state"];
}

/* One check or install at a time (a second press meanwhile finds the lock taken). -1: taken. */
static inline int mng_update_lock(NSString *folder) {
    [NSFileManager.defaultManager createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil];
    int lock = open([folder stringByAppendingPathComponent:@"lock"].fileSystemRepresentation, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB)) { close(lock); return -1; }
    return lock;
}
