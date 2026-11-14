/* Offline entry point: prepare with the backend's real shader/descriptor code,
 * then compile independent pipelines in parallel. No guest modules are run. */
int mng_build_pipeline_archive(void) {
    @autoreleasepool {
        const char *manifest = getenv("DM_PSO_MANIFEST");
        if (!manifest || !*manifest) { fprintf(stderr, "[MNG] pipeline manifest is required\n"); return 2; }
        mtl_init();
        g_warm_started = 1;  /* the offline pass owns the work, not warm_start */
        shmap_load();
        NSError *error = nil;
        NSString *text = [NSString stringWithContentsOfFile:@(manifest) encoding:NSUTF8StringEncoding error:&error];
        if (!text) { fprintf(stderr, "[MNG] cannot read pipeline manifest: %s\n", error.localizedDescription.UTF8String); return 2; }
        NSMutableDictionary<NSString *, NvShader *> *shaders = [NSMutableDictionary dictionary];
        NSMutableSet *seen = [NSMutableSet set];
        NSMutableArray *jobs = [NSMutableArray array];
        unsigned invalid = 0, shader_count = 0;
        for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
            if (!line.length || [line hasPrefix:@"#"] || [seen containsObject:line]) continue;
            [seen addObject:line];
            NSArray<NSString *> *columns = [line componentsSeparatedByString:@"\t"];
            BOOL valid = NO;
            if (columns.count >= 3 && [columns[0] isEqualToString:@"S"]) {
                valid = warm_shader(shaders, columns[1], columns[2].intValue) != nil;
                shader_count += valid;
            } else if (columns.count == 2 && [columns[0] isEqualToString:@"C"]) {
                NvShader *shader = warm_shader(shaders, columns[1], 5);
                if (shader && shader->fn) { [jobs addObject:@{@"compute": shader->fn, @"row": line}]; valid = YES; }
            } else if (columns.count == 2 && [columns[0] isEqualToString:@"R"]) {
                pso_skey sk;
                if (!pso_skey_parse(columns[1].UTF8String, &sk)) {
                    NSString *vkey = warm_key(sk.vs, sizeof sk.vs), *fkey = warm_key(sk.fs, sizeof sk.fs);
                    NvShader *vs = vkey.length ? warm_shader(shaders, vkey, sk.vs_stage) : nil;
                    NvShader *fs = fkey.length ? warm_shader(shaders, fkey, sk.fs_stage) : nil;
                    id<MTLFunction> specialized = nil;
                    if (sk.fz && sk.fz_desc.kind == 1 && fs) {
                        int constants[32]; for (int i = 0; i < 32; i++) constants[i] = sk.fz_desc.fx[i];
                        specialized = bf_fn(fs, constants, sk.fz_desc.fbmask, sk.k.lmap);
                    }
                    if (vs && (!fkey.length || fs) && (!sk.fz || specialized)) {
                        pso_key key = sk.k;
                        key.vs = (__bridge void *)vs; key.fs = (__bridge void *)fs; key.fsfn = (__bridge void *)specialized;
                        MTLRenderPipelineDescriptor *descriptor = pso_desc(&key, vs, fs, specialized, sk.merged);
                        if (descriptor) { [jobs addObject:@{@"render": descriptor, @"row": line}]; valid = YES; }
                    }
                }
            }
            if (!valid) { fprintf(stderr, "[MNG] invalid/unavailable manifest row: %.160s\n", line.UTF8String); invalid++; }
        }
        if (invalid) return 2;
        __block _Atomic unsigned built = 0;
        NSUInteger workers = MIN(8u, MAX(1u, NSProcessInfo.processInfo.activeProcessorCount));
        dispatch_apply(workers, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t worker) {
            for (NSUInteger i = worker; i < jobs.count; i += workers) { @autoreleasepool {
                NSDictionary *job = jobs[i]; NSError *failure = nil;
                id state;
                if (job[@"render"]) state = mng_render_pipeline(g_dev, job[@"render"], &failure);
                else state = mng_compute_pipeline(g_dev, job[@"compute"], &failure);
                if (state) atomic_fetch_add(&built, 1);
                else fprintf(stderr, "[MNG] pipeline failed: %.160s: %s\n", [job[@"row"] UTF8String], failure.localizedDescription.UTF8String);
            } }
        });
        if (built != jobs.count || !mng_archive_flush(&error)) {
            fprintf(stderr, "[MNG] pipeline archive incomplete: %u/%lu: %s\n", built, (unsigned long)jobs.count, error.localizedDescription.UTF8String);
            return 1;
        }
        fprintf(stderr, "[MNG] manifest archive: %lu requested, %u built, %u shaders, success\n", (unsigned long)jobs.count, built, shader_count);
        return 0;
    }
}
