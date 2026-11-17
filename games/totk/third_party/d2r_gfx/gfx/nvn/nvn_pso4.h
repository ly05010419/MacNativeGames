// Ocarina of Time on the Mac: the Metal 4 pipeline archive (2026-11-17, user: "好 开始做").
// Why: the old archive (MTLBinaryArchive, totk_pipeline_archive.h) keeps every pipeline's whole description in its file and
// Metal copies them all out when it opens one, so a file can hold only a few pipelines: 8 a file on the Mac, 4,033 files in
// the App. Probe (scratch/runs/2026-11/par-probe, M4, 1,024 pipelines in one file): opened +782 MB through the old API,
// +1.0 MB through Metal 4's archive holding GPU code only (MTL4PipelineDataSetSerializerConfigurationCaptureBinaries), 1-4 ms.
// With thousands of small files on a USB drive, a start that looked up its pipelines opened 1,152 files over 20 ms each
// (390 s summed), and a draw that needed an unopened file waited for the drive. Recorded as the root fix on 2026-10-21
// (issues_lessons.md), done now, after nxnative (seedds/nxnative dm/gfx/nvn/nvn_mtl.m, docs/development.md "Pipeline bases
// and the pipeline archive"):
//  - a pipeline missing from the archive is compiled by a Metal 4 compiler bound to the current period's serializer, which
//    keeps its GPU code as it compiles (no second compile, unlike the old add); the period becomes one file
//    (pipelines-mtl4/<first key>-<count>.mtl4a and its .keys) when it holds PAR4_PERIOD_MAX pipelines (Metal's archiver
//    keeps a period's state in a 10 MB store, full near 2,000: nxnative "Metal's archiver store filled by one flush period"),
//    10 s after the last capture (play), or when asked (the fill, the packaging tool, the window closing);
//  - a serializer must not write while one of its compiles runs (that crashes Metal): each period has its own compiler and
//    serializer, a flush starts a new period for later compiles and writes the old one once its compiles have returned;
//  - a lookup asks the one file whose key list names the key (g_par_where; names "m4:<base>" here, "m4-<stem>" in the
//    App's build/pipelines), opened once, mapped (nvn_archive_map.h);
//  - what Metal 4 cannot build from our description (no fragment function with rasterization on; a function made without
//    lib_fn, nvn_pso4_fn.h) and the mesh pipelines keep the old path and the old archive, as does every key the old files
//    hold (until the App is packaged again: the packaging tool builds those through Metal 4 and the old files go).
// Off: TOTK_PSO_MTL4=0. Not for an iPad build's run (TOTK_SHADER_DEVICE_LIBS=1): device_pipelines.py reads the old files'
// descriptions. Included by nvn_mtl.m after totk_pipeline_archive.h.
#pragma once
#ifdef PSO4_BUILT
#define PAR4_PERIOD_MAX 2000
#define PAR4_FLUSH_AFTER_S 10

/* Pipelines a period holds before it is written: PAR4_PERIOD_MAX, or TOTK_PSO4_PERIOD=<1-2000> (diagnostic: smaller
   periods tell a few pipelines Metal cannot serialize from a whole kind of them). */
static NSUInteger par4_period_max(void) {
    static NSUInteger n; static dispatch_once_t once;
    dispatch_once(&once, ^{ const char *e = getenv("TOTK_PSO4_PERIOD"); long v = e && *e ? atol(e) : PAR4_PERIOD_MAX;
        n = (NSUInteger)(v < 1 ? 1 : v > PAR4_PERIOD_MAX ? PAR4_PERIOD_MAX : v); });
    return n;
}
static int par4_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_PSO_MTL4"), *dev = getenv("TOTK_SHADER_DEVICE_LIBS");
        on = par_enabled() && !(e && !strcmp(e, "0")) && !(dev && !strcmp(dev, "1"));
    }
    return on;
}
static NSString *par4_dir(void) { return [totk_rt_dir() stringByAppendingPathComponent:@"pipelines-mtl4"]; }
static NSString *par4_lock(void) { return [totk_rt_dir() stringByAppendingPathComponent:@"pipelines-mtl4.lock"]; }
static BOOL par4_name(NSString *name) { return [name hasPrefix:@"m4:"] || [name hasPrefix:@"m4-"]; }
static NSString *par4_own(NSString *base) { return [@"m4:" stringByAppendingString:base]; }
/* A file's pipelines by its name, <first key>-<count>[-n] (par_pub_batch_ext; a compute key may carry -tg<size>): the
   first all-digit part after the key, or 0. PAR4_SMALL or fewer: a small file (the rows kept before the title, packaged
   apart, nvn_pso4_build.h g_par4_compact; this device's own). */
#define PAR4_SMALL 1024
static NSUInteger par4_name_count(NSString *name) {
    NSArray<NSString *> *parts = [name componentsSeparatedByString:@"-"];
    NSCharacterSet *notDigit = NSCharacterSet.decimalDigitCharacterSet.invertedSet;
    for (NSUInteger i = 1; i < parts.count; i++)
        if (parts[i].length && parts[i].length < 8 && [parts[i] rangeOfCharacterFromSet:notDigit].location == NSNotFound) return (NSUInteger)parts[i].integerValue;
    return 0;
}

/* The folder for this OS, GPU and key format, as par_open_identity does for the old one: another identity's is removed. */
static int par4_open_identity(id<MTLDevice> dev) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *stamp_path = [par4_dir() stringByAppendingPathComponent:@"identity.json"];
    NSData *sd = [NSData dataWithContentsOfFile:stamp_path];
    NSDictionary *stamp = sd ? [NSJSONSerialization JSONObjectWithData:sd options:0 error:nil] : nil;
    if (!par_same_identity(stamp, par_identity(dev)) && [fm fileExistsAtPath:par4_dir()]) {
        if (stamp) MLOG("Metal 4 管线存档：系统版本、GPU 或存档格式变了，删掉重建");
        if (!par_pub_wipe(par4_dir(), par4_lock())) { MLOG("【红】Metal 4 管线存档：另一个进程正在写，这次不用"); return 0; }
    }
    [fm createDirectoryAtPath:par4_dir() withIntermediateDirectories:YES attributes:nil error:nil];
    int lk = par_pub_lock(par4_lock(), LOCK_SH);
    if (lk < 0) { MLOG("【红】Metal 4 管线存档：锁不了 %s（%s），这次不用", par4_lock().UTF8String, strerror(errno)); return 0; }
    [[NSJSONSerialization dataWithJSONObject:par_identity(dev) options:NSJSONWritingSortedKeys error:nil] writeToFile:stamp_path atomically:YES];
    par_pub_unlock(lk);
    return 1;
}
/* After par_load_dir, before par_load_prebuilt (this Mac's own files win over the App's): the keys of the folder's files.
   A key also in an old file goes to the newer one. Returns how many files. */
static int g_par4_ready;
static NSUInteger par4_load_dir(id<MTLDevice> dev, uint64_t *bytes) {
    *bytes = 0;
    if (!par4_on() || !par4_open_identity(dev)) return 0;
    NSFileManager *fm = NSFileManager.defaultManager; NSUInteger files = 0;
    for (NSString *base in par_pub_scan_ext(par4_dir(), par4_lock(), YES, [NSSet setWithObject:@"identity.json"], @"mtl4a")) {
        NSString *list = [NSString stringWithContentsOfFile:[par4_dir() stringByAppendingFormat:@"/%@.keys", base] encoding:NSUTF8StringEncoding error:nil];
        for (NSString *key in [list componentsSeparatedByString:@"\n"]) if (key.length) { [g_par_have addObject:key]; g_par_where[key] = par4_own(base); }
        *bytes += [[fm attributesOfItemAtPath:[par4_dir() stringByAppendingFormat:@"/%@.mtl4a", base] error:nil] fileSize]; files++;
    }
    g_par4_ready = 1;
    return files;
}

/* Lookup: the Metal 4 file holding key, or nil (no file, or the key is in an old file: *old). */
static NSMutableDictionary<NSString *, id<MTL4Archive>> *g_par4_open;   // name -> opened file (g_par_mu)
static _Atomic uint64_t g_par4_hits, g_par4_built, g_par4_old_path, g_par4_written, g_par4_files, g_par4_failed, g_par4_migrated, g_par4_lost;/* The opened file of a Metal 4 name (m4:<base> or m4-<stem>), opened once: a thread that asks while another opens it waits
   for that open (2026-11-17, mtl4-1: two threads opened the same 231 MB file at once, 802 ms each). The first open of a
   file of 2,000 pipelines from the USB drive took 667-803 ms, so the start opens them all in the background
   (par4_open_all) before a draw needs one. nil: unreadable (its keys are forgotten, so their pipelines are compiled again
   into new files). */
static NSMutableSet<NSString *> *g_par4_opening;   // g_par_mu
static pthread_cond_t g_par4_opened_cv = PTHREAD_COND_INITIALIZER;
static id<MTL4Archive> par4_open_name(id<MTLDevice> dev, NSString *name) {
    pthread_mutex_lock(&g_par_mu);
    if (!g_par4_open) g_par4_open = [NSMutableDictionary dictionary];
    if (!g_par4_opening) g_par4_opening = [NSMutableSet set];
    while ([g_par4_opening containsObject:name]) pthread_cond_wait(&g_par4_opened_cv, &g_par_mu);
    id<MTL4Archive> a = g_par4_open[name];
    int pre = [g_par_pre containsObject:name];
    NSString *path = a ? nil : pre ? [g_par_pre_base stringByAppendingFormat:@"%@.mtl4a", name]
                                   : [par4_dir() stringByAppendingFormat:@"/%@.mtl4a", [name substringFromIndex:3]];
    if (path) [g_par4_opening addObject:name];
    pthread_mutex_unlock(&g_par_mu);
    if (!path) return a;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    NSError *e = nil; arch_map_begin(); id<MTL4Archive> opened = [dev newArchiveWithURL:[NSURL fileURLWithPath:path] error:&e]; arch_map_end();
    uint64_t ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    atomic_fetch_add(&g_par_opens, 1); atomic_fetch_add(&g_par_open_ns, ns);
    struct stat st; long long kb = stat(path.fileSystemRepresentation, &st) ? -1 : (long long)st.st_size / 1024;
    MLOG("Metal 4 管线存档：打开 %s（%lld KB）%.1f ms%s", path.lastPathComponent.UTF8String, kb, ns / 1e6, opened ? "" : "，打不开");
    pthread_mutex_lock(&g_par_mu);
    if (opened) g_par4_open[name] = opened;
    else {
        for (NSString *k in [g_par_where allKeysForObject:name]) { [g_par_have removeObject:k]; [g_par_where removeObjectForKey:k]; }
        if (pre) [g_par_pre removeObject:name];
        if (atomic_fetch_add(&g_par4_failed, 1) < 4) MLOG("【红】Metal 4 管线存档：%s 打不开（%s），里面的管线重编", path.UTF8String, e.localizedDescription.UTF8String ?: "?");
    }
    [g_par4_opening removeObject:name]; pthread_cond_broadcast(&g_par4_opened_cv);
    pthread_mutex_unlock(&g_par_mu);
    return opened;
}
/* Once, after the archive's index is read (par_open_dir): every Metal 4 file opened in the background, several at once.
   Mapped files: the footprint stays (probe: +1.0 MB a file of 1,024 pipelines). */
static void par4_open_all(id<MTLDevice> dev) {
    if (!par4_on()) return;
    pthread_mutex_lock(&g_par_mu);
    NSMutableSet<NSString *> *names = [NSMutableSet set];
    for (NSString *n in g_par_where.objectEnumerator) if (par4_name(n)) [names addObject:n];
    pthread_mutex_unlock(&g_par_mu);
    /* this device's own files first (its warm-up set, par4_note_warm), then the App's: a warm-up lookup in an own file does
       not wait behind the App's 4.7 GB being read */
    NSArray<NSString *> *own = [names.allObjects filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"SELF BEGINSWITH 'm4:'"]];
    /* then the App's small files (the packaging puts the rows kept before the title in small files of their own,
       g_par4_compact), then its large ones: each step before the next, so the warm-up's files are not read beside 4.7 GB */
    NSMutableArray<NSString *> *small = [NSMutableArray array], *large = [NSMutableArray array];
    for (NSString *n in names) if (![n hasPrefix:@"m4:"]) { NSUInteger c = par4_name_count([n substringFromIndex:3]); [(c && c <= PAR4_SMALL ? small : large) addObject:n]; }
    if (!own.count && !small.count && !large.count) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        uint64_t t0 = bp_now();
        dispatch_apply(own.count, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(size_t i) { par4_open_name(dev, own[i]); });
        uint64_t t1 = bp_now();
        dispatch_apply(small.count, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(size_t i) { par4_open_name(dev, small[i]); });
        uint64_t t2 = bp_now();
        dispatch_apply(large.count, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(size_t i) { par4_open_name(dev, large[i]); });
        MLOG("Metal 4 管线存档：后台打开了本机 %lu 个文件（%.1f s）、App 的小文件 %lu 个（%.1f s）、大文件 %lu 个（%.1f s）",
             (unsigned long)own.count, (t1 - t0) / 1e9, (unsigned long)small.count, (t2 - t1) / 1e9, (unsigned long)large.count, (bp_now() - t2) / 1e9);
    });
}
static id<MTL4Archive> par4_archive_for(id<MTLDevice> dev, NSString *key, int *old) {
    *old = 0;
    par_open_dir(dev);
    if (!g_par_on || !key) return nil;
    pthread_mutex_lock(&g_par_mu);
    if (g_par_recheck) { if (!g_par_used) g_par_used = [NSMutableSet set]; [g_par_used addObject:key]; }
    NSString *name = g_par_where[key];
    if (name && !par4_name(name)) { *old = 1; pthread_mutex_unlock(&g_par_mu); return nil; }
    int pre = name && [g_par_pre containsObject:name];
    pthread_mutex_unlock(&g_par_mu);
    id<MTL4Archive> a = name ? par4_open_name(dev, name) : nil;
    par_last_pre = a && pre;
    return a;
}

/* A flush period: its compiler captures what it compiles into its serializer. busy: compiles under way in it. */
@interface Par4Period : NSObject { @public id<MTL4PipelineDataSetSerializer> ser; id<MTL4Compiler> comp; dispatch_group_t busy;
    NSMutableArray<NSString *> *keys; NSMutableArray *descs; NSUInteger captured; int batch; }   // descs: each key's Metal 4 description (par4_resplit)
@end
@implementation Par4Period
@end
static pthread_mutex_t g_par4_mu = PTHREAD_MUTEX_INITIALIZER;
static Par4Period *g_par4_period;          // g_par4_mu
static int g_par4_capture_off;             // g_par4_mu: capturing failed in a new period, so it stays off this run
static uint64_t g_par4_last_capture;       // g_par4_mu: bp_now() of the last capture (the timer's flush)
static int g_par4_timer;                   // g_par4_mu: a timer is set
static dispatch_queue_t par4_queue(void) {
    static dispatch_queue_t q; static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("dm.nvn.pso4-write", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)); });
    return q;
}
static id<MTL4Compiler> par4_plain(id<MTLDevice> dev) {   // no serializer: pipelines without a key, and a refused capture
    static id<MTL4Compiler> c; static dispatch_once_t once;
    dispatch_once(&once, ^{ NSError *e = nil; c = [dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
        if (!c) MLOG("【红】Metal 4 编译器建不了（%s），管线走旧路", e.localizedDescription.UTF8String ?: "?"); });
    return c;
}
/* A new period: its own serializer and the compiler bound to it, or nil (no batch lock, or Metal refused). */
static Par4Period *par4_new_period(id<MTLDevice> dev) {
    Par4Period *p = [Par4Period new]; p->busy = dispatch_group_create(); p->keys = [NSMutableArray array]; p->descs = [NSMutableArray array];
    p->batch = par_batch_begin();   // Metal's build folder of this serializer is removed once it is written (totk_pipeline_archive_folders.h)
    MTL4PipelineDataSetSerializerDescriptor *sd = [MTL4PipelineDataSetSerializerDescriptor new];
    sd.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;   // descriptions too: no file is written (nxnative)
    p->ser = p->batch ? [dev newPipelineDataSetSerializerWithDescriptor:sd] : nil;
    MTL4CompilerDescriptor *cd = [MTL4CompilerDescriptor new]; cd.pipelineDataSetSerializer = p->ser;
    NSError *e = nil; p->comp = p->ser ? [dev newCompilerWithDescriptor:cd error:&e] : nil;
    if (p->comp) return p;
    if (p->batch) par_batch_end();
    MLOG("【红】Metal 4 管线存档：建不了捕获用的编译器（%s）", e.localizedDescription.UTF8String ?: "no batch lock");
    return nil;
}
static void par4_release(Par4Period *p) { p->comp = nil; p->ser = nil; p->descs = nil; if (p->batch) par_batch_end(); p->batch = 0; }
/* The period a compile goes into (its busy group entered), or nil: no capturing (off, or the batch lock is not held). */
static Par4Period *par4_enter(id<MTLDevice> dev) {
    pthread_mutex_lock(&g_par4_mu);
    Par4Period *p = g_par4_period;
    if (!p && !g_par4_capture_off) {
        p = par4_new_period(dev);
        if (!p) { g_par4_capture_off = 1; MLOG("【红】Metal 4 管线存档：这次新编的管线不存"); }
        else g_par4_period = p;
    }
    if (p) dispatch_group_enter(p->busy);
    pthread_mutex_unlock(&g_par4_mu);
    return p;
}
static void par4_write(Par4Period *p);
static void par4_timer_fire(void);
static void par4_leave(Par4Period *p, NSString *key, id desc4, BOOL captured) {
    BOOL full = NO, arm = NO;
    pthread_mutex_lock(&g_par4_mu);
    if (captured) {
        [p->keys addObject:key]; [p->descs addObject:desc4]; g_par4_last_capture = bp_now();
        if (++p->captured >= par4_period_max() && g_par4_period == p) { g_par4_period = nil; full = YES; }
        else if (!g_par4_timer) arm = g_par4_timer = 1;
    }
    pthread_mutex_unlock(&g_par4_mu);
    dispatch_group_leave(p->busy);
    if (full) dispatch_async(par4_queue(), ^{ par4_write(p); });
    if (arm) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, PAR4_FLUSH_AFTER_S * NSEC_PER_SEC), par4_queue(), ^{ par4_timer_fire(); });
}
/* A capture the period's compiler refused (its store full, or the compiler service): the period is written as it is; one
   refused in a period that captured nothing stops capturing for the run (nxnative). */
static void par4_refused(Par4Period *p) {
    BOOL write = NO;
    pthread_mutex_lock(&g_par4_mu);
    if (g_par4_period == p) { g_par4_period = nil; write = YES; if (!p->captured) g_par4_capture_off = 1; }
    pthread_mutex_unlock(&g_par4_mu);
    if (write) dispatch_async(par4_queue(), ^{ par4_write(p); });
}
/* Writes the period's file and key list; the keys find it from now on. NO: nothing published (*why). */
static BOOL par4_publish(Par4Period *p, NSArray<NSString *> *keys, NSError **why) {
    uint64_t t0 = bp_now();
    {
        __block NSError *we = nil; id<MTL4PipelineDataSetSerializer> ser = p->ser;
        NSString *name = par_pub_batch_ext(par4_dir(), par4_lock(), [keys.firstObject stringByAppendingFormat:@"-%lu", (unsigned long)keys.count], @"mtl4a",
            ^BOOL(NSString *temp) {
                unlink(temp.fileSystemRepresentation);   // the serializer makes the file itself
                NSError *e = nil; BOOL ok = [ser serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:temp] error:&e];
                if (ok && access(temp.fileSystemRepresentation, F_OK)) { ok = NO; e = [NSError errorWithDomain:@"pso4" code:1 userInfo:@{NSLocalizedDescriptionKey: @"no file written"}]; }
                if (!ok) we = e;
                return ok;
            }, [keys componentsJoinedByString:@"\n"], NULL);
        if (name) {
            pthread_mutex_lock(&g_par_mu);
            for (NSString *k in keys) { [g_par_have addObject:k]; g_par_where[k] = par4_own(name); }
            pthread_mutex_unlock(&g_par_mu);
            atomic_fetch_add(&g_par4_files, 1);
            uint64_t n = atomic_fetch_add(&g_par4_written, keys.count) + keys.count;
            MLOG("Metal 4 管线存档：写了 %lu 条进 %s.mtl4a（%.0f ms；这次共 %llu 条）", (unsigned long)keys.count, name.UTF8String, (bp_now() - t0) / 1e6, (unsigned long long)n);
            return YES;
        }
        if (why) *why = we;
        return NO;
    }
}
static void par4_resplit(NSArray<NSString *> *keys, NSArray *descs);   // nvn_pso4_build.h
/* On par4_queue: waits for the period's compiles, then writes it. A write that fails (2026-11-17: Metal's compiler service
   dropped while serializing a period of 1,945 pipelines, XPC_ERROR_CONNECTION_INTERRUPTED; the same pipelines in periods
   of 25 all wrote) is done again in halves (par4_resplit), so one failure loses as few pipelines as it must. */
static void par4_write(Par4Period *p) {
    dispatch_group_wait(p->busy, DISPATCH_TIME_FOREVER);
    pthread_mutex_lock(&g_par4_mu); NSArray<NSString *> *keys = [p->keys copy]; NSArray *descs = [p->descs copy]; pthread_mutex_unlock(&g_par4_mu);
    NSError *why = nil;
    BOOL ok = !keys.count || par4_publish(p, keys, &why);
    par4_release(p);
    if (ok) return;
    if (atomic_fetch_add(&g_par4_failed, 1) < 64) MLOG("【红】Metal 4 管线存档：%lu 条写不了（%s），对半拆开再存", (unsigned long)keys.count, why ? why.localizedDescription.UTF8String : "no lock, or publishing failed");
    par4_resplit(keys, descs);
}
static void par4_timer_fire(void) {   // on par4_queue
    Par4Period *p = nil; int again = 0;
    pthread_mutex_lock(&g_par4_mu);
    if (bp_now() - g_par4_last_capture < (uint64_t)PAR4_FLUSH_AFTER_S * 1000000000ull) again = 1;   // still capturing (a warm-up)
    else { g_par4_timer = 0; p = g_par4_period; g_par4_period = nil; }
    pthread_mutex_unlock(&g_par4_mu);
    if (again) { dispatch_after(dispatch_time(DISPATCH_TIME_NOW, PAR4_FLUSH_AFTER_S * NSEC_PER_SEC), par4_queue(), ^{ par4_timer_fire(); }); return; }
    if (p) par4_write(p);
}
/* Writes what is captured now; wait: until written (up to timeout_s). */
static void par4_flush(int wait, double timeout_s) {
    if (!par4_on()) return;
    pthread_mutex_lock(&g_par4_mu); Par4Period *p = g_par4_period; g_par4_period = nil; pthread_mutex_unlock(&g_par4_mu);
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(par4_queue(), ^{ if (p) par4_write(p); dispatch_semaphore_signal(done); });
    if (wait && dispatch_semaphore_wait(done, timeout_s > 0 ? dispatch_time(DISPATCH_TIME_NOW, (int64_t)(timeout_s * NSEC_PER_SEC)) : DISPATCH_TIME_FOREVER))
        MLOG("【红】Metal 4 管线存档：等写完超过 %.0f s，不等了", timeout_s);
}
/* Packaging (g_par_recheck): files none of whose keys they still hold, or that this run did not use, go (par_prune_superseded). */
static NSUInteger par4_prune_superseded(void) {
    if (!par4_on()) return 0;
    NSFileManager *fm = NSFileManager.defaultManager; NSUInteger removed = 0;
    for (NSString *name in [fm contentsOfDirectoryAtPath:par4_dir() error:nil] ?: @[]) {
        if (![name hasSuffix:@".keys"]) continue;
        NSString *base = name.stringByDeletingPathExtension, *own = par4_own(base);
        NSString *list = [NSString stringWithContentsOfFile:[par4_dir() stringByAppendingPathComponent:name] encoding:NSUTF8StringEncoding error:nil];
        BOOL holds = NO;
        pthread_mutex_lock(&g_par_mu);
        for (NSString *key in [list componentsSeparatedByString:@"\n"]) if (key.length && [g_par_where[key] isEqualToString:own] && (!g_par_used || [g_par_used containsObject:key])) { holds = YES; break; }
        pthread_mutex_unlock(&g_par_mu);
        if (holds || !list) continue;
        [fm removeItemAtPath:[par4_dir() stringByAppendingFormat:@"/%@.mtl4a", base] error:nil];
        [fm removeItemAtPath:[par4_dir() stringByAppendingPathComponent:name] error:nil]; removed++;
    }
    return removed;
}
#endif
