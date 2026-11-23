/* The ports' packaging mode (Animal Crossing, Super Mario Odyssey): Tears of the Kingdom's `<host> <rewritten>
   pipeline-archive <list>...` (its nvn_mtl.m fill_row / warm_fill_archive / mtl_pipeline_archive_tool), run by the shared
   iPhone / iPad build (games/totk/ios/tools/device_pipelines.py make, called by build_ipa.py and build_port_ipa.py):
   every R (render) and C (compute) row of the lists is made from the device seed's own libraries (TOTK_SHADER_DEVICE_LIBS:
   the seed's .metallib files, so the archive names the very libraries the device loads), one pipeline per file in
   TOTK_SHADER_CACHE/pipelines with its keys (<name>.keys), and device_pipelines.py compiles those for the device GPU with
   metal-tt into the seed (pipelines-<arch>-*, par_load_prebuilt). Rows the ports' lists carry that Tears of the Kingdom's
   do not need here (border, tile-read and mesh rows, its Metal 4 archive, the Mac app's own packaging) are not made.
   Included by the port's nvn_mtl.m after the warm-up's helpers (warm_shader, warm_load_rows, warm_threads, pso_desc,
   pso_skey_parse); totk_pipeline_archive_port.h before. */
#pragma once
static struct { uint64_t render, compute, have, missing, failed; double seconds; } g_fill_last;   // the last fill
typedef struct { _Atomic uint64_t nr, nc, nfail, nhave, nmissing; } fill_counts;
static os_unfair_lock g_fill_dict_lock = OS_UNFAIR_LOCK_INIT;
/* A shader of a fill row: from the block's dictionary (warm_load_rows loaded it), else loaded here; the dictionary is
   shared by the fill's threads (Tears of the Kingdom's warm_shader locks it, the ports' does not). */
static NvShader *fill_shader(NSMutableDictionary<NSString *, NvShader *> *dict, NSString *key, int stage) {
    os_unfair_lock_lock(&g_fill_dict_lock); NvShader *sh = dict[key]; os_unfair_lock_unlock(&g_fill_dict_lock);
    if (sh || warm_failed(key)) return sh;
    sh = warm_shader_load(key, stage);
    if (!sh) { warm_fail(key); return nil; }
    os_unfair_lock_lock(&g_fill_dict_lock); dict[key] = sh; os_unfair_lock_unlock(&g_fill_dict_lock);
    return sh;
}
/* One row of a pipeline list into the archive: made from its shaders, written (par_add on this thread), dropped. A row
   naming a shader the seed does not have is counted apart (missing): the lists are the Mac's, which may hold variants it
   translated after the seed was made; the device makes such a pipeline when it meets it. */
static void fill_row(NSString *line, NSMutableDictionary<NSString *, NvShader *> *dict, fill_counts *fc) {
    NSArray<NSString *> *c = [line componentsSeparatedByString:@"\t"];
    id ps = nil;
    if ([c[0] isEqualToString:@"R"] && c.count >= 2) {
        pso_skey sk; if (pso_skey_parse(c[1].UTF8String, &sk) || sk.fz) return;
        pso_skey ak = sk; memset(&ak.fz_desc, 0, sizeof ak.fz_desc);
        pthread_mutex_lock(&g_par_mu); BOOL have = [g_par_have containsObject:par_hash(&ak, sizeof ak)]; pthread_mutex_unlock(&g_par_mu);
        if (have && !g_par_recheck) { atomic_fetch_add(&fc->nhave, 1); return; }   // packaging makes it against the archive
        NSString *vkey = warm_key(sk.vs, sizeof sk.vs), *fkey = warm_key(sk.fs, sizeof sk.fs);
        if (!warm_available(vkey) || !warm_available(fkey)) { atomic_fetch_add(&fc->nmissing, 1); return; }
        NvShader *vs = vkey.length ? fill_shader(dict, vkey, sk.vs_stage) : nil, *fs = fkey.length ? fill_shader(dict, fkey, sk.fs_stage) : nil;
        if (!vs || (fkey.length && !fs)) { atomic_fetch_add(&fc->nfail, 1); return; }
        pso_key k = sk.k; k.vs = (__bridge void *)vs; k.fs = (__bridge void *)fs; k.fsfn = NULL;
        NSError *er = nil; ps = pso_new(pso_desc(&k, vs, fs, nil, sk.merged), pso_archive_key(&k, vs, fs, sk.merged), &er);
        if (ps) atomic_fetch_add(&fc->nr, 1);
        else MLOG("【红】管线存档：渲染管线建不了 %s：%s", c[1].UTF8String, er.localizedDescription.UTF8String ?: "?");
    } else if ([c[0] isEqualToString:@"C"] && c.count >= 2 && [c[1] length]) {
        const char *ck = c[1].UTF8String;
        pthread_mutex_lock(&g_par_mu); BOOL have = [g_par_have containsObject:[@"c" stringByAppendingString:par_hash(ck, strlen(ck))]]; pthread_mutex_unlock(&g_par_mu);
        if (have && !g_par_recheck) { atomic_fetch_add(&fc->nhave, 1); return; }
        if (!warm_available(c[1])) { atomic_fetch_add(&fc->nmissing, 1); return; }
        NvShader *cs = fill_shader(dict, c[1], 5); if (!cs) { atomic_fetch_add(&fc->nfail, 1); return; }
        NSError *er = nil; ps = cs_new(cs, &er);
        if (ps) atomic_fetch_add(&fc->nc, 1);
        else MLOG("【红】管线存档：计算管线建不了 %s：%s", ck, er.localizedDescription.UTF8String ?: "?");
    } else return;
    if (!ps) atomic_fetch_add(&fc->nfail, 1);
}
/* The lists' rows, sorted (a render row's key begins with its vertex then its fragment shader, so one shader pair's rows
   fall into one block and its libraries load once), a block at a time: its shaders on warm_threads() threads, then its
   pipelines on as many, each thread with its own archive batch (par_write_in_place, par_flush_thread), as Tears of the
   Kingdom's warm_fill_archive. Waited for: done is signalled at the end. */
static void warm_fill_archive(NSArray<NSString *> *paths, dispatch_semaphore_t done) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        uint64_t t0 = bp_now(); NSMutableSet<NSString *> *seen = [NSMutableSet set];
        NSMutableArray<NSString *> *todo = [NSMutableArray array];
        for (NSString *path in paths) @autoreleasepool {
            for (NSString *line in [[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil] componentsSeparatedByString:@"\n"]) {
                if (line.length < 2 || [line characterAtIndex:1] != '\t' || [seen containsObject:line]) continue;
                unichar t = [line characterAtIndex:0];
                if (t != 'R' && t != 'C') continue;
                [seen addObject:line]; [todo addObject:line];
            }
        }
        seen = nil;
        [todo sortUsingSelector:@selector(compare:)];
        atomic_store(&par_filling, 1);
        size_t workers = (size_t)warm_threads();
        fill_counts *fc = calloc(1, sizeof *fc); if (!fc) MDIE("管线存档：内存不足");
        NSMutableDictionary<NSString *, NvShader *> *dict = [NSMutableDictionary dictionary];
        MLOG("管线存档：填充开始（%lu 份管线清单 %lu 条，%zu 个线程，每条建完写成自己的文件就丢）", (unsigned long)paths.count, (unsigned long)todo.count, workers);
        NSUInteger rows = todo.count;
        for (NSUInteger at = 0; at < rows; at += WARM_BLOCK) @autoreleasepool {
            NSArray<NSString *> *block = [todo subarrayWithRange:NSMakeRange(at, MIN((NSUInteger)WARM_BLOCK, rows - at))];
            warm_load_rows(block, dict);
            __block _Atomic size_t next = 0;
            dispatch_apply(workers, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t w) { (void)w;
                int owip = par_write_in_place; par_write_in_place = 1;   // dispatch_apply may run a share on the calling thread
                for (;;) @autoreleasepool {
                    size_t i = atomic_fetch_add(&next, 1);
                    if (i >= block.count) break;
                    fill_row(block[i], dict, fc);
                }
                par_flush_thread();   // each share's own batch (per thread)
                par_write_in_place = owip;
            });
            os_unfair_lock_lock(&g_fill_dict_lock); [dict removeAllObjects]; os_unfair_lock_unlock(&g_fill_dict_lock);
            MLOG("[pipeline-archive] %lu / %lu pipelines checked or built", (unsigned long)(at + block.count), (unsigned long)rows);
        }
        atomic_store(&par_filling, 0);
        g_fill_last.render = atomic_load(&fc->nr); g_fill_last.compute = atomic_load(&fc->nc); g_fill_last.have = atomic_load(&fc->nhave);
        g_fill_last.missing = atomic_load(&fc->nmissing); g_fill_last.failed = atomic_load(&fc->nfail); g_fill_last.seconds = (bp_now() - t0) / 1e9;
        free(fc);
        MLOG("管线存档：填充完成，新建渲染管线 %llu 条、计算管线 %llu 条，存档里已有 %llu 条，种子里没有着色器 %llu 条，失败 %llu 条，用时 %.1f s",
             g_fill_last.render, g_fill_last.compute, g_fill_last.have, g_fill_last.missing, g_fill_last.failed, g_fill_last.seconds);
        dispatch_semaphore_signal(done);   // after the line: the JSON report follows it on its own line
    });
}
#if !TARGET_OS_IPHONE
/* `<host> <rewritten> pipeline-archive <list>...` with TOTK_SHADER_CACHE=<folder> TOTK_SHADER_SEED_DIR=<device seed>
   (runtime/loader.c): the pipelines of the lists from the device seed's libraries, one file each in <folder>/pipelines
   (TOTK_PIPELINE_ARCHIVE_BATCH=1: device_pipelines.py groups them itself), files already there kept and checked against
   this runtime (TOTK_PAR_RECHECK=1, device_pipelines.py mac_cache). Prints what it made as JSON. Runs no game code.
   Exit 1 only when the archive could not be written: a row that could not be made is the device's to make. */
int mtl_pipeline_archive_tool(int argc, char **argv) {
    if (argc < 1 || !getenv("TOTK_SHADER_CACHE") || !getenv("TOTK_SHADER_SEED_DIR"))
        MDIE("pipeline-archive: TOTK_SHADER_CACHE=<folder> TOTK_SHADER_SEED_DIR=<device seed> <host> <rewritten> pipeline-archive <list>...");
    setenv("TOTK_SHADER_DEVICE_LIBS", "1", 1); setenv("TOTK_PIPELINE_ARCHIVE_BATCH", "1", 1);
    setenv("TOTK_PIPELINE_PREBUILT", "0", 1); unsetenv("TOTK_PIPELINE_ARCHIVE");
    mtl_init();
    par_open_dir(g_dev);
    if (!g_par_on) MDIE("pipeline-archive: the pipeline archive is off");
    NSMutableArray<NSString *> *lists = [NSMutableArray array];
    for (int i = 0; i < argc; i++) {
        if (![NSFileManager.defaultManager isReadableFileAtPath:@(argv[i])]) MDIE("pipeline-archive: cannot read %s", argv[i]);
        [lists addObject:@(argv[i])];
    }
    const char *recheck = getenv("TOTK_PAR_RECHECK");
    g_par_recheck = recheck && !strcmp(recheck, "1");
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    warm_fill_archive(lists, done); dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (g_par_recheck) {
        NSUInteger pruned = par_prune_superseded();
        MLOG("管线存档：按这个运行时核对，%llu 条和存档里的不一样，重新编进新文件；删掉被完全取代或清单里已没有的旧文件 %lu 个",
             (unsigned long long)atomic_load(&g_par_stale), (unsigned long)pruned);
    }
    printf("{\"render\": %llu, \"compute\": %llu, \"kept\": %llu, \"missing\": %llu, \"failed\": %llu, \"seconds\": %.1f, \"write_failed\": %llu, \"arch\": \"%s\"}\n",
           g_fill_last.render, g_fill_last.compute, g_fill_last.have, g_fill_last.missing, g_fill_last.failed, g_fill_last.seconds,
           (unsigned long long)atomic_load(&g_par_write_failed), g_dev.architecture.name.UTF8String ?: "");
    fflush(stdout);
    return atomic_load(&g_par_write_failed) ? 1 : 0;
}
#endif
