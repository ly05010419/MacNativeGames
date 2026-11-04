/* Save sync through a folder the player chooses: the same save on the Mac and on iPhone / iPad.

   The game's save folder (<profile>/save/<uid>, runtime/fs_host.c) stays the working copy. A copy of it lives in the sync
   folder as <sync>/Saves/current, with current/sync.plist naming its generation, the device that wrote it, when and the
   SHA-256 of its files:
   - at start, before the game reads a save (fs_host.c init), the folder's copy replaces the local folder when another
     device saved since this one last synced;
   - after the game commits a save (fs_host.c commit), the folder is copied to the sync folder a few seconds later.
   When the two differ at start, the one saved last wins (sync.plist's time against the newest local file, which keeps its
   time when copied); the other is kept in <profile>/cloud-backups (the last 10). Nothing is merged inside a save.
   A game that is still running when the other device uploads does not overwrite that save (the game writes its save
   folder now and then even when nobody plays): it stops uploading and its next start takes the other device's save
   (cloud-sync.plist "superseded"). Generations decide: a device whose view of the folder is out of date (a synced folder
   such as iCloud Drive can take a while to download the other device's upload) never uploads over a newer generation, and
   an older generation in the folder than this device synced is replaced by this device's save. A device that never synced
   uploads only into a folder it still finds empty at its first save in the game, not at start (a synced folder may not
   have listed a folder new to this device yet), so it cannot replace a save it has not seen. The upload also runs when
   the game quits (Mac) or goes to the background (iPhone / iPad), where the 3 s wait after a save might not come.

   <sync> is <folder>/<game name> (TOTK_CLOUD_SAVE_FOLDER), a folder the player chose: on the Mac from the app menu
   (runtime/frontend.m) or the offer at a start that has none (app/launcher.m), on iPhone / iPad from that offer, the
   Settings app or the Home Screen icon's menu (ios/Sources/main.m, a security-scoped bookmark). Any folder each device
   can see works, among them an iCloud Drive folder (also one shared between two Apple IDs); the app needs no iCloud
   container, entitlement or paid team. One folder holds both games side by side. TOTK_CLOUD_SAVE_DIR (the <sync> folder
   itself) wins over it; TOTK_CLOUD_SAVE_DIR=0 turns sync off. With neither, nothing syncs. */
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "runtime.h"
#include "game_version.h"
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#endif


static NSString *g_local, *g_state, *g_backups, *g_cloud;   // save folder, <profile>/cloud-sync.plist, backups, <cloud>/Saves
static dispatch_queue_t g_queue;
static void watch_lifecycle(void);   // upload at quit / background (below)
static int g_push_pending, g_superseded;   // an upload is waiting; another device uploaded since this one synced
static long long g_start_gen;   // the generation the start compared this device's save with (0: the folder had none)

#define LOG(...) fprintf(stderr, "[cloud-save] " __VA_ARGS__)

static NSString *cloud_root(void) {
    const char *e = getenv("TOTK_CLOUD_SAVE_DIR");
    if (e) return *e && strcmp(e, "0") ? @(e) : nil;
    const char *chosen = getenv("TOTK_CLOUD_SAVE_FOLDER");
    return chosen && *chosen ? [@(chosen) stringByAppendingPathComponent:@GAME_NAME] : nil;
}

/* The files of a save folder, sorted, without Finder / iCloud bookkeeping and sync.plist. */
static NSArray<NSString *> *save_files_all(NSString *dir, BOOL progress_only);
static NSArray<NSString *> *save_files(NSString *dir) { return save_files_all(dir, NO); }
/* What says where the game is: storage/ (CacheStorageKey.dat and the like) is rewritten while nobody plays, so it is
   copied with the save but left out of its hash and of its time. */
static NSArray<NSString *> *progress_files(NSString *dir) { return save_files_all(dir, YES); }
static NSArray<NSString *> *save_files_all(NSString *dir, BOOL progress_only) {
    NSMutableArray *out = [NSMutableArray array];
    NSDirectoryEnumerator *en = [NSFileManager.defaultManager enumeratorAtPath:dir];
    for (NSString *rel in en) {
        NSString *name = rel.lastPathComponent;
        if ([name isEqualToString:@".DS_Store"] || [rel isEqualToString:@"sync.plist"] || ([name hasPrefix:@"."] && [name hasSuffix:@".icloud"])) continue;
        if (progress_only && [rel hasPrefix:@"storage/"]) continue;
        if ([en.fileAttributes.fileType isEqualToString:NSFileTypeRegular]) [out addObject:rel];
    }
    [out sortUsingSelector:@selector(compare:)];
    return out;
}
static NSString *save_hash(NSString *dir) {
    CC_SHA256_CTX c; CC_SHA256_Init(&c);
    for (NSString *rel in progress_files(dir)) {
        NSData *d = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:rel] options:NSDataReadingMappedIfSafe error:nil];
        if (!d) return nil;   // unreadable (not downloaded yet): no hash rather than a wrong one
        const char *r = rel.UTF8String; uint64_t n = d.length;
        CC_SHA256_Update(&c, r, (CC_LONG)strlen(r) + 1); CC_SHA256_Update(&c, &n, sizeof n); CC_SHA256_Update(&c, d.bytes, (CC_LONG)d.length);
    }
    unsigned char h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(h, &c);
    NSMutableString *s = [NSMutableString string]; for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) [s appendFormat:@"%02x", h[i]];
    return s;
}
static double newest_mtime(NSString *dir) {
    double t = 0;
    for (NSString *rel in progress_files(dir)) {
        NSDate *m = [NSFileManager.defaultManager attributesOfItemAtPath:[dir stringByAppendingPathComponent:rel] error:nil].fileModificationDate;
        if (m.timeIntervalSince1970 > t) t = m.timeIntervalSince1970;
    }
    return t;
}

/* An iCloud Drive folder keeps files as placeholders until asked (iPhone / iPad always; the Mac when "Optimise Mac Storage" evicted them,
   or for a folder another Apple ID shares); ask for them and wait (at most `wait` s). A folder iCloud does not sync has
   no downloading status and passes at once. */
static BOOL ensure_downloaded(NSString *dir, double wait) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *url = [NSURL fileURLWithPath:dir];
    [fm startDownloadingUbiquitousItemAtURL:url error:nil];
    for (double waited = 0;; waited += 0.25) {
        BOOL all = YES;
        for (NSURL *u in [fm enumeratorAtURL:url includingPropertiesForKeys:@[NSURLUbiquitousItemDownloadingStatusKey] options:0 errorHandler:nil]) {
            NSString *st = nil; [u getResourceValue:&st forKey:NSURLUbiquitousItemDownloadingStatusKey error:nil];
            if (st && ![st isEqualToString:NSURLUbiquitousItemDownloadingStatusCurrent]) { all = NO; [fm startDownloadingUbiquitousItemAtURL:u error:nil]; }
        }
        if (all) return YES;
        if (waited >= wait) return NO;
        usleep(250000);
    }
}

/* Read the folder's copy while no other process writes it: NSFileCoordinator waits for iCloud Drive or the like (or
   this game's own upload) to finish a write in progress. NO when the read could not be coordinated. */
static BOOL read_coordinated(NSString *path, void (^read)(NSString *path)) {
    __block BOOL ran = NO;
    NSError *err = nil;
    [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateReadingItemAtURL:[NSURL fileURLWithPath:path] options:0 error:&err byAccessor:^(NSURL *u) {
        ran = YES; read(u.path);
    }];
    if (!ran) LOG("cannot read %s: %s\n", path.UTF8String, err ? err.localizedDescription.UTF8String : "?");
    return ran;
}

static NSDictionary *read_plist(NSString *path) { NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:path]; return [d isKindOfClass:NSDictionary.class] ? d : nil; }
static void write_state(NSNumber *gen, NSString *hash) {
    [@{@"gen": gen ?: @0, @"hash": hash ?: @""} writeToFile:g_state atomically:YES];
}
static void write_superseded(NSDictionary *state, NSNumber *cloudGen) {
    NSMutableDictionary *d = [state ?: @{} mutableCopy]; d[@"superseded"] = cloudGen;
    [d writeToFile:g_state atomically:YES];
}

/* Replace the contents of `dst` with those of `src`, through a sibling folder so a failure leaves `dst` as it was. */
static BOOL replace_folder(NSString *src, NSString *dst) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *tmp = [dst stringByAppendingFormat:@".incoming-%d", getpid()], *old = [dst stringByAppendingFormat:@".old-%d", getpid()];
    [fm removeItemAtPath:tmp error:nil]; [fm removeItemAtPath:old error:nil];
    NSError *err = nil;
    if (![fm copyItemAtPath:src toPath:tmp error:&err]) { LOG("copy failed: %s\n", err.localizedDescription.UTF8String); [fm removeItemAtPath:tmp error:nil]; return NO; }
    [fm removeItemAtPath:[tmp stringByAppendingPathComponent:@"sync.plist"] error:nil];
    BOOL had = [fm fileExistsAtPath:dst];
    if (had && ![fm moveItemAtPath:dst toPath:old error:&err]) { LOG("move failed: %s\n", err.localizedDescription.UTF8String); [fm removeItemAtPath:tmp error:nil]; return NO; }
    if (![fm moveItemAtPath:tmp toPath:dst error:&err]) { LOG("move failed: %s\n", err.localizedDescription.UTF8String); if (had) [fm moveItemAtPath:old toPath:dst error:nil]; return NO; }
    [fm removeItemAtPath:old error:nil];
    return YES;
}
static void backup(NSString *src, NSString *what) {
    NSFileManager *fm = NSFileManager.defaultManager;
    [fm createDirectoryAtPath:g_backups withIntermediateDirectories:YES attributes:nil error:nil];
    NSDateFormatter *f = [NSDateFormatter new]; f.dateFormat = @"yyyyMMdd-HHmmss";
    NSString *to = [g_backups stringByAppendingPathComponent:[NSString stringWithFormat:@"%@-%@", [f stringFromDate:NSDate.date], what]];
    if ([fm copyItemAtPath:src toPath:to error:nil]) LOG("kept the %s save in %s\n", what.UTF8String, to.UTF8String);
    NSArray *all = [[fm contentsOfDirectoryAtPath:g_backups error:nil] sortedArrayUsingSelector:@selector(compare:)];
    for (NSUInteger i = 0; i + 10 < all.count; i++) [fm removeItemAtPath:[g_backups stringByAppendingPathComponent:all[i]] error:nil];
}

/* Write the local save to the sync folder as the next generation. */
static void push(void) {
    NSString *cur = [g_cloud stringByAppendingPathComponent:@"current"], *hash = save_hash(g_local);
    NSDictionary *state = read_plist(g_state);
    __block NSDictionary *meta = read_plist([cur stringByAppendingPathComponent:@"sync.plist"]);
    if (!hash || !save_files(g_local).count) return;
    if (g_superseded) return;
    if (!meta && state) { LOG("the folder's save is not visible right now; this one waits for the next save\n"); return; }
    if (!state) {
        /* A device that never synced uploads only into a folder that is still empty when it looks again now: a synced folder
           lists a folder that is new to this device some time after the game opens it. A save that has appeared meanwhile is not
           overwritten; the next start takes it, and keeps this device's save in cloud-backups. */
        ensure_downloaded(g_cloud, 10);
        read_coordinated(cur, ^(NSString *p) { meta = read_plist([p stringByAppendingPathComponent:@"sync.plist"]); });
        if (meta && [meta[@"gen"] longLongValue] != g_start_gen) {   // the start's own choice (this device's save is newer) goes ahead
            g_superseded = 1; write_superseded(nil, meta[@"gen"]);
            LOG("a save from %s (generation %lld) is in the folder now: this device's saves stay local, and its next start takes that one (keeping this one in cloud-backups)\n",
                [meta[@"device"] UTF8String], [meta[@"gen"] longLongValue]);
            return;
        }
    }
    if (meta && state && [meta[@"gen"] longLongValue] > [state[@"gen"] longLongValue]) {   // another device uploaded while this game ran
        g_superseded = 1; write_superseded(state, meta[@"gen"]);
        LOG("%s uploaded a newer save (generation %lld): this device's saves stay local, and its next start takes that one\n",
            [meta[@"device"] UTF8String], [meta[@"gen"] longLongValue]);
        return;
    }
    if ([hash isEqualToString:state[@"hash"]] && [meta[@"gen"] isEqual:state[@"gen"]]) return;   // nothing new on either side
    long long gen = MAX([meta[@"gen"] longLongValue], [state[@"gen"] longLongValue]) + 1;
    [NSFileManager.defaultManager createDirectoryAtPath:g_cloud withIntermediateDirectories:YES attributes:nil error:nil];
    __block BOOL ok = NO;
    NSError *cerr = nil;
    [[[NSFileCoordinator alloc] initWithFilePresenter:nil] coordinateWritingItemAtURL:[NSURL fileURLWithPath:cur] options:NSFileCoordinatorWritingForReplacing error:&cerr byAccessor:^(NSURL *u) {
        if (!replace_folder(g_local, u.path)) return;
        ok = [@{@"gen": @(gen), @"device": NSProcessInfo.processInfo.hostName ?: @"?", @"time": @(NSDate.date.timeIntervalSince1970), @"hash": hash}
                 writeToFile:[u.path stringByAppendingPathComponent:@"sync.plist"] atomically:YES];
    }];
    if (!ok) { LOG("upload failed%s%s\n", cerr ? ": " : "", cerr ? cerr.localizedDescription.UTF8String : ""); return; }
    write_state(@(gen), hash);
    LOG("saved to the sync folder (generation %lld)\n", gen);
}

/* fs_host.c init: before the game reads a save. */
void runtime_cloud_save_start(const char *save_dir, const char *profile_dir) {
    @autoreleasepool {
        NSString *root = cloud_root();
        if (!root) { LOG("off (no sync folder)\n"); return; }
        /* A save folder that already lives in the sync folder (an older setup linked the profile into iCloud Drive) would
           be copied into itself: the game keeps it as it is. */
        NSString *localReal = @(save_dir).stringByResolvingSymlinksInPath, *rootReal = root.stringByResolvingSymlinksInPath;
        if ([[localReal stringByAppendingString:@"/"] hasPrefix:[rootReal stringByAppendingString:@"/"]] ||
            [[rootReal stringByAppendingString:@"/"] hasPrefix:[localReal stringByAppendingString:@"/"]]) {
            LOG("off: the save folder %s is inside the sync folder %s (keep the saves out of it)\n", localReal.UTF8String, rootReal.UTF8String);
            return;
        }
        g_local = @(save_dir); g_cloud = [root stringByAppendingPathComponent:@"Saves"];
        g_state = [@(profile_dir) stringByAppendingPathComponent:@"cloud-sync.plist"];
        g_backups = [@(profile_dir) stringByAppendingPathComponent:@"cloud-backups"];
        g_queue = dispatch_queue_create("totk.cloud-save", DISPATCH_QUEUE_SERIAL);
        watch_lifecycle();
        runtime_boot_status("Save sync", "Syncing saves");
        NSString *cur = [g_cloud stringByAppendingPathComponent:@"current"];
        NSDictionary *state = read_plist(g_state);
        ensure_downloaded(g_cloud, 30);
        __block NSDictionary *meta = nil;
        read_coordinated(cur, ^(NSString *p) { meta = read_plist([p stringByAppendingPathComponent:@"sync.plist"]); });
        g_start_gen = [meta[@"gen"] longLongValue];
        NSString *local = save_hash(g_local);
        BOOL localSaves = save_files(g_local).count > 0;
        if (!meta) {
            /* A device that synced before but sees nothing has not learned of the folder's files yet: it uploads on its
               next save, after the files have arrived, instead of now. */
            if (state) { LOG("no save visible in the folder yet; this device's save is used\n"); return; }
            /* Nor does a device that never synced upload now: a synced folder may not be listed yet. Its first save in
               the game uploads, after push looks again (or the next start, if a save has arrived by then). */
            LOG("no save in the folder yet%s\n", localSaves ? "; this device's save goes up at its first save in the game, if the folder is still empty then" : ""); return;
        }
        if ([meta[@"hash"] isEqualToString:local]) { write_state(meta[@"gen"], local); LOG("in sync (generation %lld)\n", [meta[@"gen"] longLongValue]); return; }
        long long cloudGen = [meta[@"gen"] longLongValue], ownGen = [state[@"gen"] longLongValue];
        BOOL localChanged = localSaves && !(state && [state[@"hash"] isEqualToString:local]);
        if (state && cloudGen == ownGen) { LOG("the folder has this device's last upload (generation %lld)\n", cloudGen); return; }   // new progress goes up with the next save
        if (state && cloudGen < ownGen) {
            /* An older generation than this device synced: another device wrote from an out-of-date view of the folder. This
               device's save is the newer one. */
            LOG("the folder has generation %lld, older than this device's %lld: uploading this device's save\n", cloudGen, ownGen);
            if (localSaves) push();
            return;
        }
        /* Another device uploaded since. Its save wins unless this device also saved since its last sync and later (the
           last session saw that upload and stopped: then it wins whatever this one wrote after it). */
        BOOL superseded = state[@"superseded"] && cloudGen >= [state[@"superseded"] longLongValue];
        double cloudTime = [meta[@"time"] doubleValue], localTime = localSaves ? newest_mtime(g_local) : 0;
        if (localChanged && !superseded && localTime > cloudTime) { LOG("both saved; this device's save is newer\n"); push(); return; }
        if (!ensure_downloaded(cur, 60)) { LOG("the folder's save did not download in time; this device's save is used\n"); return; }
        /* Checked and copied in one coordinated read, so an upload arriving meanwhile cannot slip between the two. */
        read_coordinated(cur, ^(NSString *p) {
            NSDictionary *m = read_plist([p stringByAppendingPathComponent:@"sync.plist"]);
            if (![m[@"gen"] isEqual:meta[@"gen"]] || ![save_hash(p) isEqualToString:m[@"hash"]]) { LOG("the folder's save is incomplete (still syncing); this device's save is used\n"); return; }
            if (localSaves) backup(g_local, @"local");
            if (replace_folder(p, g_local)) { write_state(m[@"gen"], m[@"hash"]); LOG("loaded the save from %s (generation %lld)\n", [m[@"device"] UTF8String], [m[@"gen"] longLongValue]); }
        });
    }
}

/* An upload that is still waiting, now (the game quits or goes to the background). `any`: also a save written since this
   device's last upload with none waiting (push leaves it when nothing changed). */
static void flush(int any) {
    if (!g_queue) return;
    if (!__atomic_load_n(&g_push_pending, __ATOMIC_SEQ_CST) && !(any && read_plist(g_state))) return;
    dispatch_sync(g_queue, ^{
        if (!__atomic_exchange_n(&g_push_pending, 0, __ATOMIC_SEQ_CST) && !any) return;
        for (int i = 0; i < 50 && runtime_fs_save_busy(); i++) usleep(100000);   // a save being written: up to 5 s
        @autoreleasepool { push(); }
    });
}
static void flush_at_exit(void) { flush(0); }
/* The Mac game ends with _exit when its window closes (frontend.m), so flush_at_exit does not run then: a save made just
   before quitting (its upload waits for 3 quiet seconds) went nowhere, and the next start, seeing the folder at this device's
   last generation, left it local too. */
void runtime_cloud_save_quit(void) { flush(1); }
static void watch_lifecycle(void) {
    atexit(flush_at_exit);
#if TARGET_OS_IPHONE
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            (void)n;
            __block UIBackgroundTaskIdentifier task = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"save sync" expirationHandler:^{
                [UIApplication.sharedApplication endBackgroundTask:task]; task = UIBackgroundTaskInvalid; }];
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
                flush(0);
                dispatch_async(dispatch_get_main_queue(), ^{ if (task != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:task]; task = UIBackgroundTaskInvalid; });
            });
        }];
    });
#endif
}

/* fs_host.c commit: the game finished writing a save. Upload once it has been quiet for a moment. */
static void push_when_quiet(void) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), g_queue, ^{
        if (!__atomic_load_n(&g_push_pending, __ATOMIC_SEQ_CST)) return;   // flushed meanwhile
        if (runtime_fs_save_busy()) { push_when_quiet(); return; }
        __atomic_store_n(&g_push_pending, 0, __ATOMIC_SEQ_CST);
        @autoreleasepool { push(); }
    });
}
void runtime_cloud_save_changed(void) {
    if (!g_queue || __atomic_exchange_n(&g_push_pending, 1, __ATOMIC_SEQ_CST)) return;
    push_when_quiet();
}
