// TOTK: the pipeline archive (2026-10-20/21). Pipelines are compiled once into GPU code and kept in Metal binary archives
// in the translation cache; later pipeline creations take their code from them instead of compiling.
// Why: iOS warmed every prepared pipeline at start and kept them all (about 450 MB of a 3.84 GB footprint on an
// iPhone 14, whose limit is 4 GB), because a pipeline met for the first time in play compiles on the device (50-700 ms
// hitches), and the system's own shader cache is cleared by reboots and updates.
// Files of up to par_batch_size() pipelines (pipelines/<name>.metalarchive, the keys in <name>.keys), and a pipeline is looked
// up only in the file holding its key. Measured on an M4 (2026-10-21):
//  - one file holding many pipelines costs memory about quadratic in their number, never given back (32 per file 1 MB,
//    1,024 per file 777 MB: Metal clones every pipeline description into one flatbuffer when it opens the file);
//  - Metal searches every archive given to a pipeline (102 new pipelines with 142 files attached took 3.1 s);
//  - an archive opened from a file and written again keeps only what was added after opening;
//  - a one-pipeline file: 0.08 ms and about 35 KB to open, the pipeline from it 0.04 ms;
//  - every archive a pipeline is added to leaves a build folder in the user cache folder (com.apple.gpuarchiver/
//    gpuarchiver-*, about 380 KB, PersistentState and its parts) that Metal never removes, not even when the process exits:
//    432 folders in a one-minute first run, 65,536 folders and 25 GB overnight until no folder could be made any more
//    (reported by the other session, 2026-10-21). Nothing holds them open once the archive is written, so par_write
//    removes the one its own write made (par_ga_*). The folder sits under the app's bundle id when it has one
//    (iPhone 14, 2026-10-21: 239 folders left in Library/Caches/<bundle id>/com.apple.gpuarchiver while the first
//    version of this looked one level up).
// Until 2026-10-26 each pipeline had its own file, and each write took the write lock, compiled the pipeline into a new
// archive, wrote it and listed the build folders twice: the warm-up built its pipelines one at a time (23,226 in 235 s
// on the first start, 10 ms each). Now the warm-up's threads each fill their own archive and write it every par_batch_size()
// pipelines; a build folder is made per archive, when its first pipeline is added (2026-10-26), and those that appeared
// are removed once no batch is open. Writing the file is most of the cost (Metal seems to compile the archive's code
// then: 315 s of 400 s summed over the threads), so the threads are what makes it quicker. TOTK 1.4.3, 23,226
// pipelines, M-series Mac, 8 threads, an empty cache, then a second start from it (2026-10-26; the old row ran from
// another app folder, whose system Metal cache was warmer):
//   per file   first start   second start   footprint in the world (second start)
//   1 (old)    248 s         28.3 s         7,427 MB      (one thread)
//   1          109.5 s       19.3 s         7,371 MB
//   8          62.0 / 83.0   11.4 / 11.5    7,249 / 7,414 MB  (the Mac's default)
//   32         75.2 / 71.4   12.1 / 11.8    7,903 / 8,201 MB
// Breath of the Wild 1.6.0 at 8 per file: 23.4 s first start (9,224 pipelines), 3.4 s second.
// The key only finds the file: Metal checks the functions and state, so a wrong file is a miss, never a wrong pipeline.
// On everywhere; TOTK_PIPELINE_ARCHIVE=0 turns it off. Valid for one OS version, GPU and translation chain
// (pipelines/identity.json); otherwise the folder is removed. Included by nvn_mtl.m after MLOG and g_dev.
#pragma once
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <pthread.h>
#include <stdatomic.h>
#include "dmsh_chain.h"
#include "totk_shader_rt.h"

#ifndef PAR_BATCH_MAC
#define PAR_BATCH_MAC 8
#endif

static pthread_mutex_t g_par_mu = PTHREAD_MUTEX_INITIALIZER;
static NSMutableSet<NSString *> *g_par_have;                              // keys with a file
static NSMutableDictionary<NSString *, NSString *> *g_par_where;          // key -> the file (name) holding it
static NSMutableSet<NSString *> *g_par_pending;                           // keys in a batch not written yet
static NSMutableDictionary<NSString *, id<MTLBinaryArchive>> *g_par_open; // opened files by name (kept: Metal keeps their memory anyway)
static int g_par_on, g_par_valid;                                        // valid: files of this OS / GPU / chain exist
static _Atomic uint64_t g_par_written, g_par_write_failed, g_par_files, g_par_add_ns, g_par_flush_ns;
static _Atomic int par_filling;                                          // the background fill (warm_fill_archive)
static __thread int par_write_in_place;                                  // the fill's and the start-up warm-up's thread: they write at once

/* Pipelines per file (see the top): TOTK_PIPELINE_ARCHIVE_BATCH=<1-256>, 8 on the Mac. iOS keeps one per file: it opens a
   file when a draw first needs one of its pipelines, so a bigger file would bring the others' descriptions into memory
   too, and the iPhone plays near its limit (not measured there). */
static int par_batch_size(void) {
    static int n; static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("TOTK_PIPELINE_ARCHIVE_BATCH");
        n = e && *e ? atoi(e) : TARGET_OS_IPHONE ? 1 : PAR_BATCH_MAC;
        if (n < 1) n = 1; if (n > 256) n = 256;
    });
    return n;
}
static int par_enabled(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("TOTK_PIPELINE_ARCHIVE"); on = !(e && !strcmp(e, "0")); }
    return on;
}
// The first run without valid archives: fill them in the background without keeping the pipelines (iOS; the full
// warm-up and the archive together passed the 4 GB limit). TOTK_PIPELINE_ARCHIVE_FILL=1 does the same on a Mac.
static int par_fill_light(void) {
#if TARGET_OS_IPHONE
    return 1;
#else
    const char *e = getenv("TOTK_PIPELINE_ARCHIVE_FILL"); return e && !strcmp(e, "1");
#endif
}
static NSString *par_dir(void) { return [totk_rt_dir() stringByAppendingPathComponent:@"pipelines"]; }
static NSString *par_file(NSString *name) { return [par_dir() stringByAppendingFormat:@"/%@.metalarchive", name]; }
static NSString *par_keys_file(NSString *name) { return [par_dir() stringByAppendingFormat:@"/%@.keys", name]; }
// 5 (2026-10-29): border and tile-read pipelines are keyed with their variant (nvn_mtl.m pso_archive_key); files of 4 held
// some under the plain pipeline's key, which then missed in every session.
#define PAR_SCHEMA 5
static NSDictionary *par_identity(id<MTLDevice> dev) {
    return @{@"schema": @PAR_SCHEMA, @"chain": @DMSH_CHAIN, @"gpu": dev.name ?: @"?",
             @"os": NSProcessInfo.processInfo.operatingSystemVersionString ?: @"?"};
}
static NSString *par_hash(const void *bytes, size_t n) {
    uint8_t d[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(bytes, (CC_LONG)n, d);
    NSMutableString *s = [NSMutableString stringWithCapacity:40];
    for (int i = 0; i < 20; i++) [s appendFormat:@"%02x", d[i]];   // 160 bits name a file
    return s;
}
/* The folder Metal's archive builder works in (see the top) is com.apple.gpuarchiver under a cache folder, inside a folder
   named after the bundle id when the program has one (iPhone 14: Library/Caches/<bundle id>/com.apple.gpuarchiver; a bare
   Mac binary such as build/totk-native-v143: <user cache folder>/com.apple.gpuarchiver; a packaged Mac app:
   <user cache folder>/<bundle id>/...). The first version took only confstr's user cache folder, which the iPhone app does
   not get, and removed nothing there (2026-10-21). So every candidate is watched: confstr's folder and the Caches folder,
   each with and without the bundle id. Folders under the bundle id are this app's own (own: 1); the others are shared with
   other programs. Removed: the folders that appeared while this program's batches were open (par_batch_begin / _end). */
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
static void par_ga_remove_new(NSSet<NSString *> *before) {
    NSMutableSet<NSString *> *made = [par_ga_list(0) mutableCopy]; [made minusSet:before];
    for (NSString *path in made) {
        NSError *e = nil;
        if ([NSFileManager.defaultManager removeItemAtPath:path error:&e]) atomic_fetch_add(&g_par_ga_removed, 1);
        else { static _Atomic int said; if (atomic_fetch_add(&said, 1) < 4) MLOG("【红】管线存档：删不掉 Metal 留下的工作目录 %s（%s）", path.UTF8String, e.localizedDescription.UTF8String); }
    }
}
static int g_par_startup_cleaned;   // g_par_wmu; startup deletion must never run again while batches are open
static void par_ga_cleanup_startup(void) {
    pthread_mutex_lock(&g_par_wmu);
    if (g_par_startup_cleaned) { pthread_mutex_unlock(&g_par_wmu); return; }
    NSUInteger n = 0;
    for (NSString *path in par_ga_list(1)) if ([NSFileManager.defaultManager removeItemAtPath:path error:nil]) n++;
    g_par_startup_cleaned = 1;
    pthread_mutex_unlock(&g_par_wmu);
    NSMutableArray *where = [NSMutableArray array];
    for (NSDictionary *d in par_ga_dirs()) [where addObject:[NSString stringWithFormat:@"%@（%@）", d[@"path"], [d[@"own"] boolValue] ? @"本 App 的" : @"共用"]];
    MLOG("管线存档：Metal 的工作目录查这些：%s；启动时删掉以前留下的 %lu 个", [where componentsJoinedByString:@"，"].UTF8String, (unsigned long)n);
}
/* The keys of the files in the folder (g_par_have, g_par_where); removes what a write cut short left. Returns the files' bytes. */
static uint64_t par_load_dir(NSUInteger *files) {
    NSFileManager *fm = NSFileManager.defaultManager;
    g_par_have = [NSMutableSet set]; g_par_where = [NSMutableDictionary dictionary]; g_par_pending = [NSMutableSet set];
    g_par_open = [NSMutableDictionary dictionary]; uint64_t bytes = 0; *files = 0;
    NSArray<NSString *> *names = [fm contentsOfDirectoryAtPath:par_dir() error:nil] ?: @[];
    NSSet<NSString *> *present = [NSSet setWithArray:names];
    for (NSString *name in names) {   // a file counts with its key list, written after it; the rest is a write cut short
        NSString *base = name.stringByDeletingPathExtension, *ext = name.pathExtension;
        if ([ext isEqualToString:@"keys"] && [present containsObject:[base stringByAppendingPathExtension:@"metalarchive"]]) {
            NSString *list = [NSString stringWithContentsOfFile:par_keys_file(base) encoding:NSUTF8StringEncoding error:nil];
            for (NSString *key in [list componentsSeparatedByString:@"\n"]) if (key.length) { [g_par_have addObject:key]; g_par_where[key] = base; }
            bytes += [[fm attributesOfItemAtPath:par_file(base) error:nil] fileSize]; ++*files;
        } else if ([ext isEqualToString:@"metalarchive"] ? ![present containsObject:[base stringByAppendingPathExtension:@"keys"]] : ![name isEqualToString:@"identity.json"])
            [fm removeItemAtPath:[par_dir() stringByAppendingPathComponent:name] error:nil];
    }
    return bytes;
}
/* Pipelines the Mac compiled for this device's GPU while packaging (2026-10-29; ios/tools/device_pipelines.py, run by
   build_ipa.py): flat in the seed folder, since a pushed seed is one stream of files (install_device.py write_pack):
   pipelines-<GPU architecture>.json (schema, chain, arch), .tsv (key, file name) and pipelines-<arch>-<name>.metalarchive.
   Read where they are. A first start then takes the lists' pipelines from them and fills nothing, as a second start
   would (Breath of the Wild 1.6.0, iPhone 14: 12,453 pipelines filled in 567.8 s on the device). Only for the translation
   chain and the architecture they name (MTLDevice.architecture); the device's own files win. TOTK_PIPELINE_PREBUILT=0
   ignores them. Called under par_open_dir's once, after par_load_dir. Returns how many keys they added. */
static NSString *g_par_pre_base;              // <seed>/pipelines-<arch>-
static NSMutableSet<NSString *> *g_par_pre;   // their file names (g_par_mu)
static _Atomic uint64_t g_par_pre_hit, g_par_pre_miss;
static __thread int par_last_pre;             // par_archive_for's archive was one of them
static NSString *par_pre_file(NSString *name) { return [g_par_pre_base stringByAppendingFormat:@"%@.metalarchive", name]; }
static NSUInteger par_load_prebuilt(id<MTLDevice> dev) {
    g_par_pre = [NSMutableSet set];
    const char *off = getenv("TOTK_PIPELINE_PREBUILT"), *seed = getenv("TOTK_SHADER_SEED_DIR");
    NSString *arch = dev.architecture.name;
    if ((off && !strcmp(off, "0")) || !seed || !*seed || !arch.length) return 0;
    NSString *base = [@(seed) stringByAppendingPathComponent:[@"pipelines-" stringByAppendingString:arch]];
    NSData *jd = [NSData dataWithContentsOfFile:[base stringByAppendingString:@".json"]];
    NSDictionary *stamp = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    if (![stamp isKindOfClass:NSDictionary.class]) {
        NSMutableArray *other = [NSMutableArray array];
        for (NSString *n in [NSFileManager.defaultManager contentsOfDirectoryAtPath:@(seed) error:nil] ?: @[])
            if ([n hasPrefix:@"pipelines-"] && [n hasSuffix:@".json"]) [other addObject:[[n substringFromIndex:10] stringByDeletingPathExtension]];
        if (other.count) MLOG("管线存档：App 带的 Mac 编好的管线是给 %s 的，这台设备的 GPU 是 %s，首次启动照常在本机填充",
                              [other componentsJoinedByString:@"、"].UTF8String, arch.UTF8String);
        return 0;
    }
    if (![stamp[@"schema"] isEqual:@PAR_SCHEMA] || ![stamp[@"chain"] isEqual:@DMSH_CHAIN] || ![stamp[@"arch"] isEqual:arch]) {
        MLOG("【红】管线存档：App 带的 Mac 编好的管线不对（格式 %s / 翻译链 %s / GPU %s，本机 %d / %s / %s），不用", [[stamp[@"schema"] description] UTF8String],
             [[stamp[@"chain"] description] UTF8String], [[stamp[@"arch"] description] UTF8String], PAR_SCHEMA, DMSH_CHAIN, arch.UTF8String);
        return 0;
    }
    NSString *index = [NSString stringWithContentsOfFile:[base stringByAppendingString:@".tsv"] encoding:NSUTF8StringEncoding error:nil];
    NSUInteger n = 0;
    for (NSString *line in [index componentsSeparatedByString:@"\n"]) {
        NSArray<NSString *> *c = [line componentsSeparatedByString:@"\t"];
        if (c.count != 2 || !c[0].length || !c[1].length || [c[1] containsString:@"/"] || g_par_where[c[0]]) continue;
        [g_par_have addObject:c[0]]; g_par_where[c[0]] = c[1]; [g_par_pre addObject:c[1]]; n++;
    }
    g_par_pre_base = [base stringByAppendingString:@"-"];
    return n;
}
// Once: the folder of this OS version, GPU and chain, and the keys it has files for.
static void par_open_dir(id<MTLDevice> dev) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (!par_enabled()) return;
        NSFileManager *fm = NSFileManager.defaultManager;
        /* Only earlier runs' owned folders may be removed. Finish before dispatch_once publishes the archive to any
           writer: batches no longer hold g_par_wmu while Metal uses their build folders. Shared folders stay intact. */
        par_ga_cleanup_startup();
        NSString *stamp_path = [par_dir() stringByAppendingPathComponent:@"identity.json"];
        NSData *sd = [NSData dataWithContentsOfFile:stamp_path];
        NSDictionary *stamp = sd ? [NSJSONSerialization JSONObjectWithData:sd options:0 error:nil] : nil;
        [fm removeItemAtPath:[totk_rt_dir() stringByAppendingPathComponent:@"pipelines.metalarchive"] error:nil];   // layouts of earlier tests
        [fm removeItemAtPath:[totk_rt_dir() stringByAppendingPathComponent:@"pipelines.json"] error:nil];
        if (![stamp isEqual:par_identity(dev)] && [fm fileExistsAtPath:par_dir()]) {
            if (stamp) MLOG("管线存档：系统版本、GPU 或翻译链变了，删掉重建");
            [fm removeItemAtPath:par_dir() error:nil];
        }
        [fm createDirectoryAtPath:par_dir() withIntermediateDirectories:YES attributes:nil error:nil];
        [[NSJSONSerialization dataWithJSONObject:par_identity(dev) options:NSJSONWritingSortedKeys error:nil] writeToFile:stamp_path atomically:YES];
        NSUInteger files = 0; uint64_t bytes = par_load_dir(&files), own = g_par_have.count, pre = par_load_prebuilt(dev);
        g_par_on = 1; g_par_valid = g_par_have.count > 0;
        if (pre) MLOG("管线存档：App 带了 Mac 为这台设备的 GPU（%s）编好的 %llu 条管线，首次启动不用填充；本机自己的 %llu 条", dev.architecture.name.UTF8String,
                      (unsigned long long)pre, (unsigned long long)own);
        if (g_par_valid && own) MLOG("管线存档：%llu 条管线在 %lu 个文件里（共 %.1f MB），用到哪个才打开哪个", (unsigned long long)own, (unsigned long)files, bytes / 1048576.0);
        else if (!g_par_valid) MLOG("管线存档：还没有，这次%s", par_fill_light() ? "在后台把管线清单收进存档（不常驻）" : "边玩边收（游戏空闲时在后台写）");
    });
}
// The archive with this pipeline's code, or nil (no file yet; off).
static id<MTLBinaryArchive> par_archive_for(id<MTLDevice> dev, NSString *key) {
    par_open_dir(dev);
    if (!g_par_on || !key) return nil;
    pthread_mutex_lock(&g_par_mu);
    NSString *name = g_par_where[key];
    int pre = name && [g_par_pre containsObject:name];
    id<MTLBinaryArchive> a = name ? g_par_open[name] : nil;
    if (!a && name) {
        MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new]; d.url = [NSURL fileURLWithPath:pre ? par_pre_file(name) : par_file(name)];
        NSError *e = nil; a = [dev newBinaryArchiveWithDescriptor:d error:&e];
        if (a) g_par_open[name] = a;
        else {   // unreadable: its pipelines are compiled again and written to new files (the app's own files stay)
            for (NSString *k in [g_par_where allKeysForObject:name]) { [g_par_have removeObject:k]; [g_par_where removeObjectForKey:k]; }
            if (pre) [g_par_pre removeObject:name];
            else { [NSFileManager.defaultManager removeItemAtPath:par_file(name) error:nil]; [NSFileManager.defaultManager removeItemAtPath:par_keys_file(name) error:nil]; }
        }
    }
    pthread_mutex_unlock(&g_par_mu);
    par_last_pre = a && pre;
    return a;
}
/* A pipeline from par_archive_for's file was found there (hit) or not: for the files the app brought (par_load_prebuilt). */
static void par_pre_note(int hit) {
    if (par_last_pre) atomic_fetch_add(hit ? &g_par_pre_hit : &g_par_pre_miss, 1);
}
/* Before a fill: if this system's Metal took less than half of what it looked up in the app's files (an iOS update whose
   compiler no longer takes their code; a wrong architecture is never loaded), they are dropped, so the fill builds those
   pipelines on the device and writes them into its own files, as without them. */
static void par_pre_check(void) {
    uint64_t hit = atomic_load(&g_par_pre_hit), miss = atomic_load(&g_par_pre_miss);
    if (miss < 16 || miss <= hit) return;
    pthread_mutex_lock(&g_par_mu);
    NSUInteger n = 0;
    for (NSString *k in g_par_where.allKeys) if ([g_par_pre containsObject:g_par_where[k]]) { [g_par_have removeObject:k]; [g_par_where removeObjectForKey:k]; n++; }
    for (NSString *name in g_par_pre) [g_par_open removeObjectForKey:name];
    [g_par_pre removeAllObjects];
    pthread_mutex_unlock(&g_par_mu);
    MLOG("【红】管线存档：App 带的 Mac 编好的管线这台设备的 Metal 不认（找到 %llu 条、没找到 %llu 条），丢掉这 %lu 条，改为在本机填充",
         (unsigned long long)hit, (unsigned long long)miss, (unsigned long)n);
}
/* The thread's batch: an archive being filled and its keys (par_write), written as one file by par_flush_thread. Kept in
   the thread's dictionary; every thread that writes (par_write_in_place) flushes before it does other work. Adding compiles
   the pipeline once more, through the compiler service the game uses too: 0.19 ms after the pipeline itself (12.6 ms
   cold, 2026-10-26), so only the fill and the start-up warm-up do it (par_add). */
@interface ParBatch : NSObject { @public id<MTLBinaryArchive> archive; NSMutableArray<NSString *> *keys; }
@end
@implementation ParBatch
@end
static int g_par_batches;                  // batches open (g_par_wmu)
static NSSet<NSString *> *g_par_ga_base;   // the build folders there when the first of them began
static void par_batch_begin(void) {
    pthread_mutex_lock(&g_par_wmu);
    if (g_par_batches++ == 0) g_par_ga_base = par_ga_list(0);
    pthread_mutex_unlock(&g_par_wmu);
}
static void par_batch_end(void) {   // the batch's archive is released: its build folder goes once no batch is open
    pthread_mutex_lock(&g_par_wmu);
    if (--g_par_batches == 0) { par_ga_remove_new(g_par_ga_base); g_par_ga_base = nil; }
    pthread_mutex_unlock(&g_par_wmu);
}
static NSString *const k_par_batch = @"totk.pipeline-archive.batch";
static void par_flush_thread(void) {
    NSMutableDictionary *td = NSThread.currentThread.threadDictionary;
    if (!td[k_par_batch]) return;
    NSArray<NSString *> *keys = nil; NSString *name = nil; NSError *e = nil; BOOL ok = YES; uint64_t t0 = bp_now();
    @autoreleasepool {
        ParBatch *b = td[k_par_batch]; [td removeObjectForKey:k_par_batch];
        keys = b->keys;
        if (keys.count) {
            name = [keys.firstObject stringByAppendingFormat:@"-%lu", (unsigned long)keys.count];
            NSString *path = par_file(name), *temp = [path stringByAppendingString:@".tmp"], *list = par_keys_file(name), *ltemp = [list stringByAppendingString:@".tmp"];
            ok = [b->archive serializeToURL:[NSURL fileURLWithPath:temp] error:&e] && rename(temp.fileSystemRepresentation, path.fileSystemRepresentation) == 0
              && [[keys componentsJoinedByString:@"\n"] writeToFile:ltemp atomically:NO encoding:NSUTF8StringEncoding error:&e]
              && rename(ltemp.fileSystemRepresentation, list.fileSystemRepresentation) == 0;
            if (!ok) { unlink(temp.fileSystemRepresentation); unlink(ltemp.fileSystemRepresentation); unlink(path.fileSystemRepresentation); }
        }
        b->archive = nil;
    }
    par_batch_end();
    pthread_mutex_lock(&g_par_mu);
    for (NSString *key in keys) { [g_par_pending removeObject:key]; if (ok) { [g_par_have addObject:key]; g_par_where[key] = name; } }
    pthread_mutex_unlock(&g_par_mu);
    atomic_fetch_add(&g_par_flush_ns, bp_now() - t0);
    if (!ok) { if (atomic_fetch_add(&g_par_write_failed, 1) < 4) MLOG("【红】管线存档：写不了（%s）", e ? e.localizedDescription.UTF8String : "改名失败"); return; }
    if (!keys.count) return;
    atomic_fetch_add(&g_par_files, 1);
    uint64_t n = atomic_fetch_add(&g_par_written, keys.count) + keys.count;
    if (n / 2000 != (n - keys.count) / 2000)
        MLOG("管线存档：这次已写 %llu 条、%llu 个文件（删掉 Metal 留下的工作目录 %llu 个）", (unsigned long long)n,
             (unsigned long long)atomic_load(&g_par_files), (unsigned long long)atomic_load(&g_par_ga_removed));
}
static void par_write(id<MTLDevice> dev, NSString *key, id desc) {
    pthread_mutex_lock(&g_par_mu);
    BOOL skip = [g_par_have containsObject:key] || [g_par_pending containsObject:key];
    if (!skip) [g_par_pending addObject:key];
    pthread_mutex_unlock(&g_par_mu);
    if (skip) return;   // another variant under the same key, or in another thread's batch: keep the first
    NSMutableDictionary *td = NSThread.currentThread.threadDictionary;
    ParBatch *b = td[k_par_batch];
    NSError *e = nil; uint64_t t0 = bp_now();
    if (!b) {
        par_batch_begin();
        b = [ParBatch new]; b->keys = [NSMutableArray array];
        b->archive = [dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&e];
        td[k_par_batch] = b;   // also without an archive: par_flush_thread ends the batch
    }
    BOOL ok = b->archive && ([desc isKindOfClass:MTLRenderPipelineDescriptor.class] ? [b->archive addRenderPipelineFunctionsWithDescriptor:desc error:&e]
                                                                                  : [b->archive addComputePipelineFunctionsWithDescriptor:desc error:&e]);
    atomic_fetch_add(&g_par_add_ns, bp_now() - t0);
    if (ok) [b->keys addObject:key];
    else {
        pthread_mutex_lock(&g_par_mu); [g_par_pending removeObject:key]; pthread_mutex_unlock(&g_par_mu);
        if (atomic_fetch_add(&g_par_write_failed, 1) < 4) MLOG("【红】管线存档：加不进去（%s）", e ? e.localizedDescription.UTF8String : "建不了存档");
    }
    if (b->keys.count >= (NSUInteger)par_batch_size() || !b->archive) par_flush_thread();
}
// Called after a pipeline the archive did not have was compiled. The warm-up writes it at once; one compiled in play is in
// pso_user.tsv, so the next start's warm-up builds it and writes it then.
static void par_add(id<MTLDevice> dev, NSString *key, id desc) {
    if (g_par_on && key && par_write_in_place) par_write(dev, key, desc);
}
