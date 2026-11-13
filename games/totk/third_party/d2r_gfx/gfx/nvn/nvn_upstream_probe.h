/* Regression fixtures use production lookup, retirement and table publication. */
static int upstream_resident(id object) {
    pthread_mutex_lock(&g_res_mu);int found=[g_res_objects containsObject:object];pthread_mutex_unlock(&g_res_mu);return found;
}
static void upstream_finish(void) {
    op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);mtl_sub_commit();mtl_finish();
}
static int upstream_probe(void) {
    int checked=0;mtl_init();g_upstream_negative=getenv("OOT_UPSTREAM_NEGATIVE")?atoi(getenv("OOT_UPSTREAM_NEGATIVE")):0;
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead|MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture=[g_dev newTextureWithDescriptor:d];res_add(texture);
    uint8_t pixels[64];for(int i=0;i<64;i++)pixels[i]=(uint8_t)(i*3+7);
    [texture replaceRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0 withBytes:pixels bytesPerRow:16];
    // A base texture has no parent. Missing current storage must not equal that nil parent.
    tex_t missing={0};mtl_reg_tex(47,&missing,NULL);registry_lock();
    treg_t *r=&g_treg[47];r->view=(void *)CFBridgingRetain(texture);r->view_parent=NULL;r->view_ser=0;r->dead_pending=1;
    id result=tex_by_id(47);registry_unlock();
    if(result)MDIE("upstream: missing current texture retained stale view");checked++;
    mtl_reg_tex(47,NULL,NULL);
    // Package release, independent serial release and a parent held by a view.
    serial_put(0xfffe1001,texture);serial_put(0xfffe1002,texture);
    @autoreleasepool {NvPkg *package=[NvPkg new];package->tex=texture;package->owned=1;package=nil;}
    upstream_finish();upstream_finish();
    if(!upstream_resident(texture))MDIE("upstream: package retired shared texture");checked++;
    serial_del(0xfffe1001);upstream_finish();upstream_finish();
    if(!upstream_resident(texture))MDIE("upstream: first serial retired shared texture");checked++;
    id<MTLTexture> view=[texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    if(!view||view.parentTexture!=texture)MDIE("upstream: expected real parent view");
    serial_put(0xfffe1003,view);serial_del(0xfffe1002);upstream_finish();upstream_finish();
    if(!upstream_resident(texture))MDIE("upstream: view failed to hold parent residency");checked++;
    reup_key key=reup_key_of(d);if(!reup_offer(texture,&key))MDIE("upstream: recycle fixture");
    upstream_finish();upstream_finish();
    if(reup_take(&key)==texture)MDIE("upstream: recycled texture held by a serial");checked++;
    id<MTLBuffer> rb=probe_buf(256);op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);
    enc_b_copy_texture_buffer(blit(),view,0,0,MTLOriginMake(0,0,0),MTLSizeMake(4,4,1),rb,0,16,64);
    mtl_sub_commit();mtl_finish();
    for(int i=0;i<64;i++){if(((uint8_t *)rb.contents)[i]!=pixels[i])MDIE("upstream: held texture GPU pixels %d",i);checked++;}
    serial_del(0xfffe1003);view=nil;upstream_finish();upstream_finish();
    if(upstream_resident(texture))MDIE("upstream: last holder kept retired texture resident");checked++;
    // Remove our recycle fixture: it was deliberately also retired by the package.
    os_unfair_lock_lock(&g_reup_lk);
    for(int i=0;i<g_nreup;i++)if(g_reup[i].tex==(__bridge void *)texture){CFRelease(g_reup[i].tex);g_reup[i--]=g_reup[--g_nreup];}
    os_unfair_lock_unlock(&g_reup_lk);res_remove(rb);

    // The table initially sees one missing storage. Adding an unrelated pool
    // revisits only that entry; adding its real storage publishes a fresh table.
    size_t page=getpagesize();void *mem=NULL,*other=NULL;
    if(posix_memalign(&mem,page,page)||posix_memalign(&other,page,page))MDIE("upstream: pool fixture");
    memset(mem,53,page);memset(other,0,page);
    tex_t t={0};t.serial=0xfffe2001;t.mem=mem;
    t.d=(nvn_texdesc){.width=4,.height=4,.depth=1,.levels=1,.target=1,.format=0x25,.swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};nvn_tex_layout(&t.d,&t.L);
    pool_t pool={.mem=mem,.size=page,.flags=0x24};t.pool=&pool;
    mtl_reg_tex(57,&t,NULL);int oldbase=S.tex_base,oldn=S.tex_pool_n;S.tex_base=0;S.tex_pool_n=128;
    nvn_rbind binding={.kind=NVN_RB_TEXP,.dtt=MTLTextureType2D};
    mtl_sub_begin();mtl_exec((const uint8_t *)&end);registry_lock();
    uint64_t first=descriptor_pool_gpu(&binding);uint64_t before=g_descriptor_pool_scanned;registry_unlock();
    mtl_pool_add(other,page);registry_lock();uint64_t same=descriptor_pool_gpu(&binding);registry_unlock();
    if(first!=same||g_descriptor_pool_scanned-before!=(pool_incremental()?1:128))MDIE("upstream: unrelated pool change rebuilt full descriptor table");checked++;
    mtl_pool_add(mem,page);nvn_dirty_add_range(mem,page);registry_lock();uint64_t ready=descriptor_pool_gpu(&binding);registry_unlock();
    if(ready==first)MDIE("upstream: new storage did not refresh missing descriptor");checked++;
    registry_lock();id<MTLTexture> current=tex_by_id(57);registry_unlock();
    if(!current)MDIE("upstream: descriptor has no current texture");
    rb=probe_buf(256);enc_b_copy_texture_buffer(blit(),current,0,0,MTLOriginMake(0,0,0),MTLSizeMake(4,4,1),rb,0,16,64);
    mtl_sub_commit();mtl_finish();
    for(int i=0;i<64;i++){if(((uint8_t *)rb.contents)[i]!=53)MDIE("upstream: revived descriptor GPU pixels %d",i);checked++;}
    mtl_reg_tex(57,NULL,NULL);mtl_tex_release(&t);mtl_pool_del(mem);mtl_pool_del(other);nvn_dirty_remove_range(mem);
    upstream_finish();upstream_finish();res_remove(rb);free(mem);free(other);S.tex_base=oldbase;S.tex_pool_n=oldn;
    // Hold the first compile until all workers joined the same in-flight job.
    for(int invalid=0;invalid<2;invalid++){
        NSString *source=invalid?@"invalid shader fixture":@"#include <metal_stdlib>\nusing namespace metal;\nkernel void upstream_shared(device uint *out [[buffer(0)]]) { out[0]=73u; }\n";
        g_variant_probe_gate=dispatch_semaphore_create(0);atomic_store(&g_variant_probe_entered,0);
        dispatch_group_t group=dispatch_group_create();NSMutableArray *answers=[NSMutableArray array],*errors=[NSMutableArray array];
        uint64_t shared_before=atomic_load(&g_variant_shared);
        for(int i=0;i<8;i++)dispatch_group_async(group,dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{@autoreleasepool{
            NSError *error=nil;id lib=variant_library(source,&error);
            @synchronized(answers){[answers addObject:lib?:NSNull.null];[errors addObject:error?:NSNull.null];}
        }});
        uint64_t until=clock_gettime_nsec_np(CLOCK_UPTIME_RAW)+5*NSEC_PER_SEC;
        while(atomic_load(&g_variant_probe_entered)!=8&&clock_gettime_nsec_np(CLOCK_UPTIME_RAW)<until)usleep(100);
        if(atomic_load(&g_variant_probe_entered)!=8)MDIE("upstream: compilation workers did not join");
        dispatch_semaphore_signal(g_variant_probe_gate);
        if(dispatch_group_wait(group,dispatch_time(DISPATCH_TIME_NOW,30*NSEC_PER_SEC)))MDIE("upstream: shared compile timed out");
        g_variant_probe_gate=nil;
        if(atomic_load(&g_variant_shared)-shared_before!=7)MDIE("upstream: duplicate compilation");checked++;
        for(int i=0;i<8;i++){
            if(answers[i]!=answers[0]||errors[i]!=errors[0]||(invalid?(errors[i]==NSNull.null):(answers[i]==NSNull.null)))MDIE("upstream: shared compile result/error mismatch");checked++;
        }
    }
    // Archive construction must not depend on runtime argument binding support.
    NSError *archive_error=nil;
    id<MTLLibrary> archive_lib=totk_msl_library(@"#include <metal_stdlib>\nusing namespace metal;\nkernel void archive_fixture(device uint *out [[buffer(0)]]) {out[0]=73u;}\n",&archive_error);
    NSMutableDictionary *meta=[@{@"chain":@DMSH_CHAIN,@"key":@"archive-fixture",@"stage":@"cs",
        @"msl_entry_kind":@"kernel",@"entry":@"archive_fixture",@"threadgroup_size":@[@1,@1,@1],
        @"route":@"B",@"bindings":@[@{@"kind":@"tex",@"msl_name":@"unmapped_array",@"msl_type":@"array<texture3d<float>, 32>",@"arg_id":@0,@"reg":NSNull.null}]} mutableCopy];
    nvn_refl rejected;char reflection_error[160];
    if(!nvn_refl_from_json(meta,&rejected,reflection_error,sizeof reflection_error))MDIE("upstream: runtime must still reject unsupported binding");checked++;
    NvShader *archive_shader=compute_archive_shader(archive_lib,meta,@"archive-fixture");
    if(!archive_shader||!cs_new(archive_shader,&archive_error))MDIE("upstream: binding-independent compute archive");checked++;
    if(compute_archive_shader(archive_lib,meta,@"wrong-key"))MDIE("upstream: archive key mismatch");checked++;
    for(NSString *field in @[@"chain",@"stage",@"entry",@"msl_entry_kind"]){
        id original=meta[field];meta[field]=@"invalid";
        if(compute_archive_shader(archive_lib,meta,@"archive-fixture"))MDIE("upstream: invalid archive metadata %s",field.UTF8String);checked++;meta[field]=original;
    }
    for(NSArray *tg in @[@[@0,@1,@1],@[@1024,@2,@1],@[@1.5,@1,@1]]){
        meta[@"threadgroup_size"]=tg;if(compute_archive_shader(archive_lib,meta,@"archive-fixture"))MDIE("upstream: invalid archive threadgroup");checked++;
    }
    MLOG("[upstream-probe] %d checks: nil views, serial/parent residency, recycling, incremental pools, GPU pixels",checked);
    return checked;
}
