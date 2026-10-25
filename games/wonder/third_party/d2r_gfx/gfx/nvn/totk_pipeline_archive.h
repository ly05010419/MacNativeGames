// TOTK: the pipeline archive (2026-10-20/21). Pipelines are compiled once into GPU code and kept in Metal binary archives
// in the translation cache; later pipeline creations take their code from them instead of compiling.
// Why: iOS warmed every prepared pipeline at start and kept them all (about 450 MB of a 3.84 GB footprint on an
// iPhone 14, whose limit is 4 GB), because a pipeline met for the first time in play compiles on the device (50-700 ms
// hitches), and the system's own shader cache is cleared by reboots and updates.
// One file per pipeline, named by the pipeline's key (pipelines/<key>.metalarchive), and a pipeline is looked up only in
// its own file. Measured on an M4 (2026-10-21):
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

static pthread_mutex_t g_par_mu = PTHREAD_MUTEX_INITIALIZER;
static NSMutableSet<NSString *> *g_par_have;                              // keys with a file
static NSMutableDictionary<NSString *, id<MTLBinaryArchive>> *g_par_open; // opened (kept: Metal keeps their memory anyway)
static int g_par_on, g_par_valid;                                        // valid: files of this OS / GPU / chain exist
static _Atomic uint64_t g_par_written, g_par_write_failed;
static _Atomic int par_filling;                                          // the background fill (warm_fill_archive)
static __thread int par_write_in_place;                                  // the fill's and the start-up warm-up's thread: they write at once

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
static NSString *par_file(NSString *key) { return [par_dir() stringByAppendingFormat:@"/%@.metalarchive", key]; }
static NSDictionary *par_identity(id<MTLDevice> dev) {
    return @{@"schema": @3, @"chain": @DMSH_CHAIN, @"gpu": dev.name ?: @"?",
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
   other programs. par_write removes only folders that appeared while it wrote, and writes one at a time (g_par_wmu), so a
   folder that appears then is its own. */
static pthread_mutex_t g_par_wmu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_par_ga_removed;
static NSArray<NSDictionary *> *par_ga_dirs(void) {   // @{path, own}
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
// Once: the folder of this OS version, GPU and chain, and the keys it has files for.
static void par_open_dir(id<MTLDevice> dev) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (!par_enabled()) return;
        NSFileManager *fm = NSFileManager.defaultManager;
        /* The app's own folders (under its bundle id) hold only build folders left by earlier runs (a crash in a write;
           before par_ga_remove_new, every write). Removed in the background, holding the write lock so no write is under
           way. Shared folders (a bare binary's) keep everything but what par_write itself made. */
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            pthread_mutex_lock(&g_par_wmu); NSUInteger n = 0;
            for (NSString *path in par_ga_list(1)) if ([NSFileManager.defaultManager removeItemAtPath:path error:nil]) n++;
            pthread_mutex_unlock(&g_par_wmu);
            NSMutableArray *where = [NSMutableArray array];
            for (NSDictionary *d in par_ga_dirs()) [where addObject:[NSString stringWithFormat:@"%@（%@）", d[@"path"], [d[@"own"] boolValue] ? @"本 App 的" : @"共用"]];
            MLOG("管线存档：Metal 的工作目录查这些：%s；启动时删掉以前留下的 %lu 个", [where componentsJoinedByString:@"，"].UTF8String, (unsigned long)n);
        });
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
        g_par_have = [NSMutableSet set]; g_par_open = [NSMutableDictionary dictionary]; uint64_t bytes = 0;
        for (NSString *name in [fm contentsOfDirectoryAtPath:par_dir() error:nil] ?: @[]) if ([name hasSuffix:@".metalarchive"]) {
            [g_par_have addObject:name.stringByDeletingPathExtension];
            bytes += [[fm attributesOfItemAtPath:[par_dir() stringByAppendingPathComponent:name] error:nil] fileSize];
        }
        g_par_on = 1; g_par_valid = g_par_have.count > 0;
        if (g_par_valid) MLOG("管线存档：%lu 条管线各有一个文件（共 %.1f MB），用到哪条才打开哪条", (unsigned long)g_par_have.count, bytes / 1048576.0);
        else MLOG("管线存档：还没有，这次%s", par_fill_light() ? "在后台把管线清单收进存档（不常驻）" : "边玩边收（游戏空闲时在后台写）");
    });
}
// The archive with this pipeline's code, or nil (no file yet; off).
static id<MTLBinaryArchive> par_archive_for(id<MTLDevice> dev, NSString *key) {
    par_open_dir(dev);
    if (!g_par_on || !key) return nil;
    pthread_mutex_lock(&g_par_mu);
    id<MTLBinaryArchive> a = g_par_open[key];
    if (!a && [g_par_have containsObject:key]) {
        MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new]; d.url = [NSURL fileURLWithPath:par_file(key)];
        NSError *e = nil; a = [dev newBinaryArchiveWithDescriptor:d error:&e];
        if (a) g_par_open[key] = a;
        else { [g_par_have removeObject:key]; [NSFileManager.defaultManager removeItemAtPath:par_file(key) error:nil]; }
    }
    pthread_mutex_unlock(&g_par_mu);
    return a;
}
// One pipeline as its own file (a temporary name, then renamed). Adding compiles the pipeline once more, through the
// compiler service the game uses too, so only the fill and the start-up warm-up do it (par_add).
static void par_write(id<MTLDevice> dev, NSString *key, id desc) {
    pthread_mutex_lock(&g_par_mu); BOOL have = [g_par_have containsObject:key]; pthread_mutex_unlock(&g_par_mu);
    if (have) return;   // another variant under the same key: keep the first
    NSError *e = nil; BOOL ok;
    NSString *path = par_file(key), *temp = [path stringByAppendingString:@".tmp"];
    pthread_mutex_lock(&g_par_wmu);
    NSSet<NSString *> *before = par_ga_list(0);
    @autoreleasepool {
        id<MTLBinaryArchive> a = [dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&e];
        ok = a && ([desc isKindOfClass:MTLRenderPipelineDescriptor.class] ? [a addRenderPipelineFunctionsWithDescriptor:desc error:&e]
                                                                          : [a addComputePipelineFunctionsWithDescriptor:desc error:&e]);
        if (ok) ok = [a serializeToURL:[NSURL fileURLWithPath:temp] error:&e] && rename(temp.fileSystemRepresentation, path.fileSystemRepresentation) == 0;
    }
    par_ga_remove_new(before);
    pthread_mutex_unlock(&g_par_wmu);
    if (!ok) { unlink(temp.fileSystemRepresentation);
               if (atomic_fetch_add(&g_par_write_failed, 1) < 4) MLOG("【红】管线存档：写不了（%s）", e ? e.localizedDescription.UTF8String : "改名失败"); return; }
    pthread_mutex_lock(&g_par_mu); [g_par_have addObject:key]; pthread_mutex_unlock(&g_par_mu);
    uint64_t n = atomic_fetch_add(&g_par_written, 1) + 1;
    if (n % 500 == 0) MLOG("管线存档：这次已写 %llu 条（删掉 Metal 留下的工作目录 %llu 个）", (unsigned long long)n, (unsigned long long)atomic_load(&g_par_ga_removed));
}
// Called after a pipeline the archive did not have was compiled. The warm-up writes it at once; one compiled in play is in
// pso_user.tsv, so the next start's warm-up builds it and writes it then.
static void par_add(id<MTLDevice> dev, NSString *key, id desc) {
    if (g_par_on && key && par_write_in_place) par_write(dev, key, desc);
}
