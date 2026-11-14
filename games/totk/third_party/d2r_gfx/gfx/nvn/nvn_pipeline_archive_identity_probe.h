/* probe-gpu-pipeline-archive, part two. */
/* The folder's identity is written only under the shared lock: with the lock file unopenable (a folder in its place),
   par_open_identity writes no identity.json and keeps the archive off (0); once it opens again, it writes it (1). */
static int par_probe_identity_lock(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *stamp = [par_dir() stringByAppendingPathComponent:@"identity.json"];
    [fm removeItemAtPath:par_dir() error:nil]; [fm removeItemAtPath:par_lock_file() error:nil];
    if (![fm createDirectoryAtPath:par_lock_file() withIntermediateDirectories:YES attributes:nil error:nil]) MDIE("pipeline archive identity probe: cannot block the lock file");
    int opened = par_open_identity(g_dev), written = [fm fileExistsAtPath:stamp];
    [fm removeItemAtPath:par_lock_file() error:nil];
    if (opened || written) MDIE("pipeline archive identity probe: without the lock the folder was opened (%d) or identity.json written (%d)", opened, written);
    if (!par_open_identity(g_dev) || ![fm fileExistsAtPath:stamp]) MDIE("pipeline archive identity probe: with the lock back identity.json was not written");
    return 2;
}

static int par_probe_fill_preload(void) {
    NSMutableSet<NSString *> *saved_have = g_par_have;
    int saved_on = g_par_on, saved_valid = g_par_valid, saved_recheck = g_par_recheck;
    g_par_have = [NSMutableSet set]; g_par_on = g_par_valid = 1; g_par_recheck = 0;
    pso_skey sk = {0}; memcpy(sk.vs, "probe_vs_b", 11); memcpy(sk.fs, "probe_fs_b", 11);
    sk.fs_stage = 1; sk.k.samples = 1;
    char hex[sizeof sk * 2 + 1]; pso_skey_hex(&sk, hex);
    NSString *r = [NSString stringWithFormat:@"R\t%s", hex], *c = @"C\tprobe_cs_b";
    NSString *b = [NSString stringWithFormat:@"B\t%s\t-\t1:1:0,0,0,0;", hex];
    NSString *t = [NSString stringWithFormat:@"T\t%s\t1:0;", hex];
    [g_par_have addObject:par_hash(&sk, sizeof sk)];
    const char *ck = "probe_cs_b"; [g_par_have addObject:[@"c" stringByAppendingString:par_hash(ck, strlen(ck))]];
    for (NSString *variant in @[@"B:-\t1:1:0,0,0,0;", @"T:1:0;"]) {
        pso_skey vk = sk; vk.fz = 1;
        NSMutableData *key = [NSMutableData dataWithBytes:&vk length:sizeof vk]; [key appendData:[variant dataUsingEncoding:NSUTF8StringEncoding]];
        [g_par_have addObject:par_hash(key.bytes, key.length)];
    }
    NSArray<NSString *> *hits = @[r, c, b, t];
    NSMutableArray<NSString *> *missing = [NSMutableArray arrayWithArray:@[@"R\tbroken", @"C\t", @"R", @"X\tunknown"]];
    [missing addObject:[NSString stringWithFormat:@"M\t-\t%s", hex]];   // mesh fill keeps its existing preload path
    [missing addObject:@"C\ttranslated_later_b"];
    [missing addObject:[b stringByReplacingOccurrencesOfString:@"1:1:0,0,0,0;" withString:@"1:1:1,0,0,0;"]];
    [missing addObject:[t stringByReplacingOccurrencesOfString:@"1:0;" withString:@"1:1;"]];
    [missing addObject:[NSString stringWithFormat:@"B\t%s\t-", hex]];
    [missing addObject:[NSString stringWithFormat:@"T\t%s", hex]];
    sk.k.dfmt = MTLPixelFormatDepth32Float; pso_skey_hex(&sk, hex); [missing addObject:[NSString stringWithFormat:@"R\t%s", hex]];
    sk.k.dfmt = 0; sk.fz = 1; pso_skey_hex(&sk, hex); [missing addObject:[NSString stringWithFormat:@"R\t%s", hex]];
    sk.fz = 0; sk.merged = 1; pso_skey_hex(&sk, hex); [missing addObject:[NSString stringWithFormat:@"T\t%s\t1:0;", hex]];
    int checks = 0;
    for (NSString *line in hits) {
        if (!fill_row_have([line componentsSeparatedByString:@"\t"])) MDIE("pipeline archive preload probe: prepared row missed");
        checks++;
    }
    for (NSString *line in missing) {
        if (fill_row_have([line componentsSeparatedByString:@"\t"])) MDIE("pipeline archive preload probe: missing or invalid row hit");
        checks++;
    }
    NSMutableArray<NSString *> *mixed = [NSMutableArray arrayWithArray:hits]; [mixed addObjectsFromArray:missing];
    if (![fill_rows_to_load(mixed) isEqualToArray:missing]) MDIE("pipeline archive preload probe: mixed rows lost or reordered");
    checks++;
    NSMutableDictionary<NSString *, NvShader *> *dict = [NSMutableDictionary dictionary]; fill_counts fc = {0};
    for (NSString *line in hits) fill_row(line, dict, &fc);
    if (dict.count || atomic_load(&fc.nhave) != hits.count || atomic_load(&fc.nr) || atomic_load(&fc.nc) || atomic_load(&fc.nm) || atomic_load(&fc.nfail))
        MDIE("pipeline archive preload probe: prepared rows loaded shaders or changed fill counts");
    checks++;
    g_par_on = 0;
    if (fill_rows_to_load(mixed) != mixed) MDIE("pipeline archive preload probe: archive-disabled behavior changed");
    checks++; g_par_on = 1; g_par_valid = 0;
    if (fill_rows_to_load(mixed) != mixed) MDIE("pipeline archive preload probe: invalid archive behavior changed");
    checks++; g_par_valid = 1; g_par_recheck = 1;
    if (fill_rows_to_load(mixed) != mixed) MDIE("pipeline archive preload probe: packaging recheck behavior changed");
    checks++;
    for (NSString *line in hits) {
        if (fill_row_have([line componentsSeparatedByString:@"\t"])) MDIE("pipeline archive preload probe: packaging recheck skipped a prepared row");
        checks++;
    }
    g_par_recheck = 0;
    [g_par_have removeAllObjects];
    if (![fill_rows_to_load(mixed) isEqualToArray:mixed]) MDIE("pipeline archive preload probe: discarded archive still skipped rows");
    checks++;
    g_par_have = saved_have; g_par_on = saved_on; g_par_valid = saved_valid; g_par_recheck = saved_recheck;
    return checks;
}
