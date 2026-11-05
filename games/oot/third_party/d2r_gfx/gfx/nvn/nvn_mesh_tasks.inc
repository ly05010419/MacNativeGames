/* NVN2 task/mesh: original control metadata -> native Metal object/mesh stages.
 * API/layout reference: ChiefSZ13/nxnative ee9b7765, MIT. Pipeline creation is
 * synchronous here: every requested draw must execute with its complete stages. */
static struct { int on; NvShader *ts,*ms,*fs; uint32_t first,count; uint64_t first_gpu,indirect_gpu; } g_mtk;
static nvn_hmap g_mtk_cache={.klen=sizeof(pso_key)+sizeof(void *)};
#include "nvn_mesh_capture.inc"
/* Diagnostic only: copy the selected draw's storage inputs on the same GPU
 * timeline, after their producers and before this draw. A CPU memcpy while
 * encoding observes older contents when an earlier dispatch has not run yet.
 * Capture each selected fragment program once; normal rendering is unchanged.
 * OOT_MESH_INPUT_CAPTURE=<external directory>, OOT_MESH_INPUT_CAPTURE_FS=<prefix,...>.
 */
static void mtk_capture_storage(NvProg *pr) {
    static int initialized;static NSString *directory;static const char *prefixes;static NSMutableDictionary<NSString *,NSNumber *> *done;
    if(!initialized){initialized=1;const char *path=getenv("OOT_MESH_INPUT_CAPTURE");prefixes=getenv("OOT_MESH_INPUT_CAPTURE_FS");
        if(path&&*path&&prefixes&&*prefixes){directory=@(path);done=[NSMutableDictionary dictionary];}}
    NvShader *fs=pr->sh[1];if(!directory||!fs)return;
    NSString *key=@(fs->key);uint64_t frame=atomic_load(&g_cur_frame);NSNumber *previous=done[key];
    if(!diag_shader_prefix_csv_matches(fs->key,prefixes)||(previous&&(!passdump()||previous.unsignedLongLongValue==frame)))return;done[key]=@(frame);
    NSString *stem=[NSString stringWithFormat:@"%@-F%llu",key,frame];
    NSMutableArray *records=[NSMutableArray array];
    for(int st=1;st<5;st++){
        NvShader *sh=pr->sh[st];if(!sh)continue;upd_guard_stage(sh,st);
        unsigned captured=0;
        for(int i=0;i<sh->rf.nb;i++){
            const nvn_rbind *b=&sh->rf.b[i];int reg=b->reg;
            if(b->kind!=NVN_RB_SBUFP||reg<0||reg>=16||(captured&(1u<<reg)))continue;captured|=1u<<reg;
            const p_bind_buf *u=&S.ssbo[st][reg];if(!u->addr||!u->size)continue;
            size_t size=(size_t)MIN(u->size,UINT64_C(64)<<20);void *source;uint64_t offset,gpu;
            if(!mtl_gpu_addr(u->addr,size,&source,&offset,&gpu)){MLOG("[mesh-input] unmapped stage %d storage %d",st,reg);continue;}
            id<MTLBuffer> capture=[g_dev newBufferWithLength:size options:MTLResourceStorageModeShared];
            if(!capture){MLOG("[mesh-input] cannot allocate %zu bytes",size);continue;}res_add(capture);
            enc_b_copy_buffer(blit_buffer((__bridge id<MTLBuffer>)source,offset,capture,0,size),(__bridge id<MTLBuffer>)source,offset,capture,0,size);
            NSString *name=[NSString stringWithFormat:@"%@-st%d-sb%d.bin",stem,st,reg];NSString *path=[directory stringByAppendingPathComponent:name];
            [records addObject:@{@"shader":@(sh->key),@"stage":@(st),@"register":@(reg),@"address":[NSString stringWithFormat:@"%#llx",u->addr],
                @"bound_size":@(u->size),@"captured_size":@(size),@"file":name}];
            [g_commit_blocks addObject:^{BOOL ok=[[NSData dataWithBytes:capture.contents length:size]writeToFile:path atomically:YES];
                MLOG("[mesh-input] GPU snapshot %s: %s (%zu bytes)",ok?"saved":"FAILED",path.UTF8String,size);}];
            defer_obj_release(capture);
        }
    }
    NSDictionary *manifest=@{@"fragment":key,@"frame":@(atomic_load(&g_cur_frame)),@"first_task":@(g_mtk.first),@"task_count":@(g_mtk.count),
        @"timing":@"GPU copies before selected draw; files written after submission completion",@"storage":records};
    NSString *path=[directory stringByAppendingPathComponent:[stem stringByAppendingString:@".json"]];
    [g_commit_blocks addObject:^{NSData *data=[NSJSONSerialization dataWithJSONObject:manifest options:NSJSONWritingPrettyPrinted error:nil];
        if(![data writeToFile:path atomically:YES])MLOG("[mesh-input] FAILED manifest %s",path.UTF8String);}];
}
/* Match the actual mesh vertex/primitive outputs to fragment inputs. Unwritten
 * varyings follow the existing raster path's zero initialization, but a mesh
 * function must be extended through dm_vertex_t and set_vertex, never main0_out. */
static id<MTLFunction> mtk_mesh_fn(NvShader *ms,NvShader *fs) {
    if(!fs||!sh_has_src(fs))return ms->fn;
    os_unfair_lock_lock(&g_vpatch_lock); // io is shared with background raster pipeline compilation
    NSDictionary *inputs=msl_varyings(fs),*outputs=msl_varyings(ms);
    NSMutableString *recipe=[NSMutableString stringWithString:@"mesh;"];
    NSMutableString *fields=[NSMutableString string];
    for(NSNumber *location in [inputs.allKeys sortedArrayUsingSelector:@selector(compare:)])if(!outputs[location]) {
        [recipe appendFormat:@"%@:%@;",location,inputs[location]];
        [fields appendFormat:@"    %@ oot_unwritten_%@ [[user(locn%@)]];\n",inputs[location],location,location];
    }
    if(!fields.length){os_unfair_lock_unlock(&g_vpatch_lock);return ms->fn;}
    id<MTLFunction> function=ms->vpatch[recipe];os_unfair_lock_unlock(&g_vpatch_lock);
    if(function)return function;
    NSString *source=sh_src(ms),*head=@"struct dm_vertex_t {";
    NSRange declaration=[source rangeOfString:head];
    if(declaration.location==NSNotFound||[source rangeOfString:@"dm_vertex_t v;"].location==NSNotFound)MDIE("mesh shader %s has no vertex output declaration",ms->key);
    source=[source stringByReplacingCharactersInRange:declaration withString:[head stringByAppendingFormat:@"\n%@",fields]];
    source=[source stringByReplacingOccurrencesOfString:@"dm_vertex_t v;" withString:@"dm_vertex_t v = {};"];
    NSError *error=nil;id<MTLLibrary> library=variant_library(source,&error);
    function=library?[library newFunctionWithName:ms->fn.name]:nil;
    if(!function)MDIE("mesh shader %s output variant: %s",ms->key,error.localizedDescription.UTF8String);
    fn_note_src(function,source);
    os_unfair_lock_lock(&g_vpatch_lock);
    if(!ms->vpatch)ms->vpatch=[NSMutableDictionary dictionary];
    if(!ms->vpatch[recipe])ms->vpatch[recipe]=function;function=ms->vpatch[recipe];
    os_unfair_lock_unlock(&g_vpatch_lock);return function;
}
static id<MTLRenderPipelineState> mtk_pso(NvProg *raster) {
    NvShader *ts=g_mtk.ts,*ms=g_mtk.ms,*fs=raster->sh[1];
    struct { pso_key p; void *task; } key; memset(&key,0,sizeof key);
    pso_key_fill(&key.p,ms,fs,g_pass.cfmt,g_pass.dfmt,g_pass.sfmt,g_pass.samples,0);
    memset(key.p.attr,0,sizeof key.p.attr);memset(key.p.stream,0,sizeof key.p.stream);
    key.task=(__bridge void *)(ts?ts->fn:nil);
    id<MTLRenderPipelineState> ps=hm_get_id(&g_mtk_cache,&key);if(ps)return ps;
    MTLRenderPipelineDescriptor *d=pso_desc(&key.p,ms,fs,nil,0);
    MTLMeshRenderPipelineDescriptor *m=[MTLMeshRenderPipelineDescriptor new];
    m.meshFunction=mtk_geo_function(ms,fs,mtk_mesh_fn(ms,fs));m.fragmentFunction=d.fragmentFunction;m.maxTotalThreadsPerMeshThreadgroup=ms->mesh_threads;
    if(ts){m.objectFunction=ts->fn;m.payloadMemoryLength=(ts->mesh_payload+15u)&~15u;m.maxTotalThreadsPerObjectThreadgroup=ts->mesh_threads;}
    m.rasterSampleCount=d.rasterSampleCount;m.alphaToCoverageEnabled=d.alphaToCoverageEnabled;
    for(int i=0;i<8;i++)m.colorAttachments[i]=d.colorAttachments[i];
    m.depthAttachmentPixelFormat=d.depthAttachmentPixelFormat;m.stencilAttachmentPixelFormat=d.stencilAttachmentPixelFormat;
    NSError *error=nil;ps=[g_dev newRenderPipelineStateWithMeshDescriptor:m options:MTLPipelineOptionNone reflection:nil error:&error];
    if(!ps)MDIE("task/mesh pipeline %s / %s: %s",ts?ts->key:"-",ms->key,error.localizedDescription.UTF8String);
    hm_put_id(&g_mtk_cache,&key,ps);MLOG("[mesh] compiled task %s mesh %s fragment %s",ts?ts->key:"-",ms->key,fs?fs->key:"-");return ps;
}
static void mtk_encode(id<MTL4RenderCommandEncoder> en) {
    NvShader *ts=g_mtk.ts,*ms=g_mtk.ms;
    if(rs_using(en))rs_put((nvn_encode_packet){.op=RS_MESH_TABLES,.u={!!ts}},0);
    else {[en setArgumentTable:g_vt atStages:MTLRenderStageMesh];if(ts)[en setArgumentTable:g_ot atStages:MTLRenderStageObject];}
    uint64_t first=g_mtk.first_gpu;
    if(!first){id<MTLBuffer>b;size_t o=arena_alloc(16,&b);memcpy((uint8_t *)b.contents+o,&g_mtk.first,4);first=b.gpuAddress+o;}
    if(ts){g_args_dirty[3]=1;encode_args((id<MTLCommandEncoder>)en,ts,3,0);
        if(rs_using(en))rs_put((nvn_encode_packet){.op=RS_OBJECT_ADDRESS,.u={29,first}},0);else[g_ot setAddress:first atIndex:29];}
    g_args_dirty[4]=1;encode_args((id<MTLCommandEncoder>)en,ms,4,1);enc_r_vbuf(en,29,first);
    if(mtk_geo_selected(g_mtk.fs))enc_r_vbuf(en,30,g_mtk_geometry?g_mtk_geometry.gpuAddress:zero_bank_gpu());
    if(g_mtk.indirect_gpu){
        if(rs_using(en))rs_put((nvn_encode_packet){.op=RS_DRAW_MESH_INDIRECT,.u={g_mtk.indirect_gpu,ts?ts->mesh_threads:1,ms->mesh_threads}},1);
        else [en drawMeshThreadgroupsWithIndirectBuffer:g_mtk.indirect_gpu threadsPerObjectThreadgroup:MTLSizeMake(ts?ts->mesh_threads:1,1,1) threadsPerMeshThreadgroup:MTLSizeMake(ms->mesh_threads,1,1)];
    }
    else if(rs_using(en))rs_put((nvn_encode_packet){.op=RS_DRAW_MESH,.u={g_mtk.count,ts?ts->mesh_threads:1,ms->mesh_threads}},1);
    else [en drawMeshThreadgroups:MTLSizeMake(g_mtk.count,1,1) threadsPerObjectThreadgroup:MTLSizeMake(ts?ts->mesh_threads:1,1,1) threadsPerMeshThreadgroup:MTLSizeMake(ms->mesh_threads,1,1)];
}
static void mesh_tasks_draw(const p_draw *dw) {
    if(!S.prog||dw->count<=0)return;
    NvProg *pr=prog_get(S.prog);if(!pr)MDIE("mesh draw has no program");
    if(prog_fit(pr)<0)MDIE("mesh shader translation is unavailable");
    NvShader *ts=pr->sh[3],*ms=pr->sh[4];
    if(ts&&ts->mesh_kind!=1)MDIE("task stage is not a task shader");
    if(!ms||ms->mesh_kind!=2||!ms->fn||(ts?!ts->fn:!ms->mesh_notask))MDIE("DrawMeshTasks without complete task/mesh stages");
    upd_guard_stage(ts,3);upd_guard_stage(ms,4);
    prepare_shader_pools(ts);prepare_shader_pools(ms);
    prepare_shader_images(ts,3);prepare_shader_images(ms,4);
    NvProg *raster=pr->geo_lean_ras;
    if(!raster||raster->sh[1]!=pr->sh[1]||raster->sh[0]){raster=[NvProg new];raster->sh[1]=pr->sh[1];pr->geo_lean_ras=raster;}
    g_mtk.on=1;g_mtk.ts=ts;g_mtk.ms=ms;g_mtk.fs=pr->sh[1];g_mtk.first=(uint32_t)dw->first;g_mtk.count=(uint32_t)dw->count;
    g_mtk.first_gpu=g_mtk.indirect_gpu=0;
    if(dw->ind){
        /* NV_mesh_shader indirect ABI: taskCount, firstTask. Convert the one
           dimensional grid on the GPU timeline; the first-task field is bound
           directly, so a prior compute producer never needs a CPU rendezvous. */
        void *src;uint64_t off,gpu;upd_guard(dw->ind,dw->ind+8);
        if(!mtl_gpu_addr(dw->ind,8,&src,&off,&gpu))MDIE("mesh indirect arguments are not mapped");
        use_rec("buf",dw->ind,8,0,NULL);
        g_mtk.indirect_gpu=mesh_indirect_grid(dw->ind,(__bridge id<MTLBuffer>)src,off);g_mtk.first_gpu=gpu+4;
        static int read;static const char *dump;static int left=16;
        if(!read){read=1;dump=getenv("OOT_MESH_INDIRECT_DUMP");}
        if(dump&&left-->0){id<MTLBuffer> capture;size_t ca=arena_alloc(16,&capture);
            enc_b_copy_buffer(blit(),(__bridge id<MTLBuffer>)src,off,capture,ca,8);
            NSString *path=[NSString stringWithFormat:@"%s-%d.bin",dump,left];
            [g_commit_blocks addObject:^{[[NSData dataWithBytes:(uint8_t *)capture.contents+ca length:8]writeToFile:path atomically:YES];}];
        }
    }
    mtk_geo_prepare(ms,pr->sh[1]);mtk_capture_storage(pr);
    g_geo_ras.pr=raster;g_pso_dirty=1;
    p_draw tri={.prim=4,.count=3,.instances=1};draw_inner(&tri,NULL);
    g_geo_ras.pr=nil;g_mtk.on=0;args_dirty_all();g_pso_dirty=1;
    mesh_indirect_written(pr);mesh_indirect_stats();
    static int once;if(!once++)MLOG("[mesh] first native draw: %u workgroups from %u",g_mtk.count,g_mtk.first);
}
