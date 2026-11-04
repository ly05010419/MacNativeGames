/* probe-gpu-pipeline-archive, part two (2026-11-04): a stored pipeline that no longer loads from its file is written
   again (par_repair, totk_pipeline_archive_files.h). Two pipelines under one key stand for a host change that keeps the
   key and changes the pipeline: the first is stored, the second misses its file, is compiled, and must be written to a
   new file that wins when the folder is read again. Render and compute; a late miss naming a file the key no longer names
   drops nothing; a compile that fails drops nothing; a key is repaired once a session; of two files with one key the newest
   wins, then the larger name. Included by nvn_mtl.m after pso_new / cs_new; run by mtl_probe_pipeline_archive. */
static NSString *par_probe_name(NSString *key) {
    pthread_mutex_lock(&g_par_mu); NSString *n = g_par_where[key]; pthread_mutex_unlock(&g_par_mu);
    return n;
}
static void par_probe_reload(void) {
    pthread_mutex_lock(&g_par_mu); NSUInteger files = 0; par_load_dir(&files); pthread_mutex_unlock(&g_par_mu);
}
static NSUInteger par_probe_files(void) {
    NSUInteger n = 0;
    for (NSString *name in [NSFileManager.defaultManager contentsOfDirectoryAtPath:par_dir() error:nil]) n += [name hasSuffix:@".keys"];
    return n;
}
static void par_probe_date(NSString *name, NSTimeInterval at) {
    NSDictionary *a = @{NSFileModificationDate: [NSDate dateWithTimeIntervalSince1970:at]};
    for (NSString *path in @[par_file(name), par_keys_file(name)])
        if (![NSFileManager.defaultManager setAttributes:a ofItemAtPath:path error:nil]) MDIE("pipeline archive repair probe: cannot date %s", path.UTF8String);
}
static void par_probe_copy(NSString *from, NSString *to) {
    NSFileManager *fm = NSFileManager.defaultManager;
    if (![fm copyItemAtPath:par_file(from) toPath:par_file(to) error:nil] || ![fm copyItemAtPath:par_keys_file(from) toPath:par_keys_file(to) error:nil])
        MDIE("pipeline archive repair probe: cannot copy %s to %s", from.UTF8String, to.UTF8String);
}
static void par_probe_loads_render(MTLRenderPipelineDescriptor *d, NSString *key, const char *what) {
    id<MTLBinaryArchive> a = par_archive_for(g_dev, key);
    if (!a) MDIE("pipeline archive repair probe: %s: no file for its key", what);
    d.binaryArchives = @[a]; NSError *e = nil;
    id ps = [g_dev newRenderPipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&e];
    d.binaryArchives = nil;
    if (!ps) MDIE("pipeline archive repair probe: %s: not in the file its key names (%s): %s", what, par_probe_name(key).UTF8String, e.localizedDescription.UTF8String);
}
static int par_probe_repair(void) {
    NSFileManager *fm = NSFileManager.defaultManager;
    for (NSString *name in [fm contentsOfDirectoryAtPath:par_dir() error:nil])
        if (![name isEqualToString:@"identity.json"]) [fm removeItemAtPath:[par_dir() stringByAppendingPathComponent:name] error:nil];
    par_probe_reload();
    int checks = 0, owip = par_write_in_place; par_write_in_place = 1;   // this thread writes as the warm-up's do
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p [[position]]; };\n"
        "struct W { float4 p [[position]]; float4 c [[user(tint)]]; };\n"
        "vertex V vmain(uint i [[vertex_id]]) { return {float4(float(i & 1), float(i >> 1), 0, 1)}; }\n"
        "fragment float4 fmain(V in [[stage_in]]) { return float4(in.p.x * 0.001, 0.25, 0.5, 1); }\n"
        "fragment float4 fbad(W in [[stage_in]]) { return in.c; }\n"   // reads what vmain never writes: no pipeline
        "kernel void k1(device uint *o [[buffer(0)]]) { o[0] = 1; }\n"
        "kernel void k2(device uint *o [[buffer(0)]]) { o[0] = 2; }\n" options:nil error:&error];
    if (!lib) MDIE("pipeline archive repair probe library: %s", error.localizedDescription.UTF8String);
    MTLRenderPipelineDescriptor *(^desc)(NSString *, MTLColorWriteMask) = ^MTLRenderPipelineDescriptor *(NSString *f, MTLColorWriteMask m) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"vmain"]; d.fragmentFunction = [lib newFunctionWithName:f];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; d.colorAttachments[0].writeMask = m; return d;
    };
    MTLRenderPipelineDescriptor *dA = desc(@"fmain", MTLColorWriteMaskAll), *dB = desc(@"fmain", MTLColorWriteMaskRed), *bad = desc(@"fbad", MTLColorWriteMaskAll);
    NSString *K = par_hash("probe-repair-render", 19), *K2 = par_hash("probe-repair-fail", 17);

    /* Render: A stored, then B under its key misses A's file, is compiled and written to a new file. */
    if (!pso_new(dA, K, &error)) MDIE("pipeline archive repair probe: pipeline A: %s", error.localizedDescription.UTF8String);
    par_flush_thread();
    NSString *fileA = par_probe_name(K);
    if (!fileA) MDIE("pipeline archive repair probe: pipeline A not stored");
    if (!pso_new(dB, K, &error)) MDIE("pipeline archive repair probe: pipeline B: %s", error.localizedDescription.UTF8String);
    par_flush_thread();
    NSString *fileB = par_probe_name(K);
    if (!fileB || [fileB isEqualToString:fileA]) MDIE("pipeline archive repair probe: B missed A's file and was not written again (the key still names %s)", fileA.UTF8String);
    if (![fm fileExistsAtPath:par_keys_file(fileA)] || ![fm fileExistsAtPath:par_file(fileA)]) MDIE("pipeline archive repair probe: B's file replaced A's (%s), which holds other keys in a real batch", fileA.UTF8String);
    checks += 3;
    /* A late miss from a thread that looked in A's file before the repair leaves the repair. */
    if (par_repair(K, fileA) || ![par_probe_name(K) isEqualToString:fileB]) MDIE("pipeline archive repair probe: a late miss of the old file undid the repair");
    checks++;
    /* The next start: the newer file wins, and B loads from it without compiling. */
    par_probe_reload();
    if (![par_probe_name(K) isEqualToString:fileB]) MDIE("pipeline archive repair probe: after reading the folder the key names %s, not the newer %s", par_probe_name(K).UTF8String, fileB.UTF8String);
    par_probe_loads_render(dB, K, "pipeline B after the repair");
    checks += 2;
    /* Once a session: A again misses B's file and is compiled, but nothing is written. */
    NSUInteger before = par_probe_files();
    if (!pso_new(dA, K, &error)) MDIE("pipeline archive repair probe: pipeline A again: %s", error.localizedDescription.UTF8String);
    par_flush_thread();
    if (par_probe_files() != before || ![par_probe_name(K) isEqualToString:fileB]) MDIE("pipeline archive repair probe: a key was repaired twice in one session");
    checks++;

    /* A compile that fails drops nothing and keeps the key's one repair. */
    if (!pso_new(dA, K2, &error)) MDIE("pipeline archive repair probe: pipeline A under the second key: %s", error.localizedDescription.UTF8String);
    par_flush_thread();
    NSString *fileA2 = par_probe_name(K2);
    if (pso_new(bad, K2, &error)) MDIE("pipeline archive repair probe: the broken pipeline compiled");
    par_flush_thread();
    if (![par_probe_name(K2) isEqualToString:fileA2]) MDIE("pipeline archive repair probe: a failed compile dropped its key");
    if (!pso_new(dB, K2, &error)) MDIE("pipeline archive repair probe: pipeline B under the second key: %s", error.localizedDescription.UTF8String);
    par_flush_thread();
    if (!par_probe_name(K2) || [par_probe_name(K2) isEqualToString:fileA2]) MDIE("pipeline archive repair probe: after a failed compile the key was not repaired");
    checks += 2;

    /* Compute: the same with two kernels under one key. */
    NvShader *c1 = [NvShader new], *c2 = [NvShader new];
    c1->fn = [lib newFunctionWithName:@"k1"]; c2->fn = [lib newFunctionWithName:@"k2"];
    strlcpy(c1->key, "probe-repair-kernel", sizeof c1->key); strlcpy(c2->key, "probe-repair-kernel", sizeof c2->key);
    NSString *KC = cs_archive_key(c1);
    if (!cs_new(c1, &error)) MDIE("pipeline archive repair probe: kernel 1: %s", error.localizedDescription.UTF8String);
    par_flush_thread();
    NSString *fileC1 = par_probe_name(KC);
    if (!cs_new(c2, &error)) MDIE("pipeline archive repair probe: kernel 2: %s", error.localizedDescription.UTF8String);
    par_flush_thread();
    NSString *fileC2 = par_probe_name(KC);
    if (!fileC1 || !fileC2 || [fileC2 isEqualToString:fileC1]) MDIE("pipeline archive repair probe: kernel 2 missed kernel 1's file and was not written again");
    par_probe_reload();
    {
        id<MTLBinaryArchive> a = par_archive_for(g_dev, KC);
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new]; d.computeFunction = c2->fn; d.binaryArchives = a ? @[a] : @[];
        if (!a || ![g_dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionFailOnBinaryArchiveMiss reflection:nil error:&error])
            MDIE("pipeline archive repair probe: kernel 2 not in the newer file after reading the folder");
    }
    checks += 2;

    /* Two files with one key: the newest wins, then the larger name; not the folder's listing order. */
    par_probe_copy(fileB, @"aaa-dup"); par_probe_copy(fileB, @"zzz-dup");
    par_probe_date(fileA, 500); par_probe_date(fileB, 1000); par_probe_date(@"zzz-dup", 2000); par_probe_date(@"aaa-dup", 3000);   // A's file holds the key too
    par_probe_reload();
    if (![par_probe_name(K) isEqualToString:@"aaa-dup"]) MDIE("pipeline archive repair probe: of four files the key names %s, not the newest", par_probe_name(K).UTF8String);
    for (NSString *n in @[fileA, fileB, @"aaa-dup", @"zzz-dup"]) par_probe_date(n, 4000);
    par_probe_reload();
    if (![par_probe_name(K) isEqualToString:@"zzz-dup"]) MDIE("pipeline archive repair probe: of four files of one age the key names %s, not the larger name", par_probe_name(K).UTF8String);
    checks += 2;

    /* A miss reported for a file the key no longer names, without a repair this session (the key moved to a newer file
       when the folder was read again, or an unreadable file was dropped and the key written anew): nothing is dropped. */
    NSString *K4 = par_hash("probe-repair-moved", 18);
    if (!pso_new(dA, K4, &error)) MDIE("pipeline archive repair probe: pipeline A under the fourth key: %s", error.localizedDescription.UTF8String);
    par_flush_thread();
    NSString *fileK4 = par_probe_name(K4);
    par_probe_copy(fileK4, @"k4-moved"); par_probe_date(fileK4, 5000); par_probe_date(@"k4-moved", 6000);
    par_probe_reload();
    if (![par_probe_name(K4) isEqualToString:@"k4-moved"]) MDIE("pipeline archive repair probe: the fourth key did not move to the newer file");
    if (par_repair(K4, fileK4) || ![par_probe_name(K4) isEqualToString:@"k4-moved"]) MDIE("pipeline archive repair probe: a miss of a file the key no longer names dropped it");
    checks += 2;

    par_write_in_place = owip;
    for (NSString *name in [fm contentsOfDirectoryAtPath:par_dir() error:nil])
        if (![name isEqualToString:@"identity.json"]) [fm removeItemAtPath:[par_dir() stringByAppendingPathComponent:name] error:nil];
    par_probe_reload();
    return checks;
}
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
