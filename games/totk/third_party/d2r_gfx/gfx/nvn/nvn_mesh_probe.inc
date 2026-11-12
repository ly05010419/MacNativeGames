/* Regression for mesh/fragment linkage through the production descriptor and
 * Metal 4 encoder FIFO. Fixtures are independent of any game's shaders. */
static int mesh_indirect_probe(void) {
    const size_t bytes=32768;uint32_t *guest=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    if(guest==MAP_FAILED)MDIE("mesh indirect fixture mmap");
    mtl_pool_add(guest,bytes);nvn_dirty_add_range(guest,bytes);
    void *raw;uint64_t offset,gpu;
    if(!mtl_gpu_addr((uintptr_t)guest,bytes,&raw,&offset,&gpu))MDIE("mesh fixture mapping");
    id<MTLBuffer> src=(__bridge id<MTLBuffer>)raw,rb=probe_buf(512);
    uint32_t expected[16][4];__block unsigned n=0;int checked=0,old=atomic_load(&g_mesh_reuse);
    const char *negative=getenv("OOT_MESH_REUSE_NEGATIVE");int neg=negative?atoi(negative):0;
    g_mic_ignore_writes=neg==1;
    atomic_store(&g_mesh_reuse,1);mtl_sub_begin();
    void (^capture)(uint64_t)=^(uint64_t address){
        for(NvArena *a in g_sub_arenas)if(address>=a->gpu && address+16<=a->gpu+a->used){
            enc_b_copy_buffer(blit(),a->buf,address-a->gpu,rb,n*16,16);return;
        }
        MDIE("mesh fixture lost conversion arena");
    };
    uint32_t records[4]={3,17,0,39};mtl_sub_update(records,(uintptr_t)guest,sizeof records);
    uint64_t a=mesh_indirect_grid((uintptr_t)guest,src,offset);
    capture(a);memcpy(expected[n++],(uint32_t[]){3,1,1,0},16);
    uint64_t b=mesh_indirect_grid((uintptr_t)(guest+2),src,offset+8);
    capture(b);memcpy(expected[n++],(uint32_t[]){0,1,1,0},16);
    if(mesh_indirect_grid((uintptr_t)guest,src,offset)!=a)MDIE("unchanged indirect record not reused");checked++;
    atomic_store(&g_mesh_reuse,2);atomic_store(&g_mesh_reuse_check,1);a=mesh_indirect_grid((uintptr_t)guest,src,offset);
    mesh_indirect_op(OP_SET_RT,NULL);
    if(mesh_indirect_grid((uintptr_t)guest,src,offset)!=a)MDIE("state-only target change dropped record");checked++;
    uint64_t update[2]={(uintptr_t)(guest+16),4};mesh_indirect_op(OP_UPDATE_UB,update);
    if(mesh_indirect_grid((uintptr_t)guest,src,offset)!=a)MDIE("disjoint inline update dropped record");checked++;
    update[0]=(uintptr_t)(guest+1);mesh_indirect_op(OP_UPDATE_UB,update);
    if(mesh_indirect_grid((uintptr_t)guest,src,offset)==a)MDIE("overlapping inline update retained record");checked++;
    a=mesh_indirect_grid((uintptr_t)guest,src,offset);b=mesh_indirect_grid((uintptr_t)(guest+2),src,offset+8);
    NvProg *pr=[NvProg new];pr->sh[4]=[NvShader new];pr->sh[4]->rf.nb=1;
    pr->sh[4]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=1,.reg=0};
    S.ssbo[4][0]=(p_bind_buf){.addr=(uintptr_t)(guest+16),.size=4};mesh_indirect_written(pr);
    if(mesh_indirect_grid((uintptr_t)guest,src,offset)!=a)MDIE("disjoint mesh write dropped record");checked++;
    S.ssbo[4][0]=(p_bind_buf){.addr=(uintptr_t)(guest+1),.size=4};mesh_indirect_written(pr);
    if(mesh_indirect_grid((uintptr_t)(guest+2),src,offset+8)!=b)MDIE("disjoint second record dropped");checked++;
    if(mesh_indirect_grid((uintptr_t)guest,src,offset)==a)MDIE("firstTask overlap failed to invalidate");checked++;
    uint32_t replacement=7;mtl_sub_update(&replacement,(uintptr_t)guest,4);
    a=mesh_indirect_grid((uintptr_t)guest,src,offset);capture(a);memcpy(expected[n++],(uint32_t[]){7,1,1,0},16);
    S.ssbo[4][0].size=0;mesh_indirect_written(pr);
    if(g_mic_n)MDIE("unsized shader write retained records");checked++;
    a=mesh_indirect_grid((uintptr_t)guest,src,offset);
    pr->sh[4]->rf.b[0].kind=NVN_RB_GMEML;pr->sh[4]->rf.b[0].rw=0;mesh_indirect_written(pr);
    if(g_mic_n)MDIE("global shader write retained records");checked++;
    a=mesh_indirect_grid((uintptr_t)guest,src,offset);mesh_indirect_op(OP_DISPATCH,NULL);
    if(g_mic_n)MDIE("compute command retained mesh records");checked++;
    a=mesh_indirect_grid((uintptr_t)guest,src,offset);
    mesh_indirect_op(OP_REPORT_COUNTER,NULL);mtl_sub_counter((uintptr_t)(guest+16),0);
    if(mesh_indirect_grid((uintptr_t)guest,src,offset)!=a)MDIE("disjoint counter dropped record");checked++;
    // The second half of the 16-byte counter record must count as a write too.
    b=mesh_indirect_grid((uintptr_t)(guest+2),src,offset+8);
    mtl_sub_counter((uintptr_t)guest,0);
    if(g_mic_n)MDIE("counter timestamp overlap retained record");checked++;
    a=mesh_indirect_grid((uintptr_t)guest,src,offset);
    mtl_sub_event(guest+16,0,42);
    if(mesh_indirect_grid((uintptr_t)guest,src,offset)!=a)MDIE("disjoint event dropped record");checked++;
    mtl_sub_event(guest+1,0,42);
    if(g_mic_n)MDIE("overlapping event retained record");checked++;
    a=mesh_indirect_grid((uintptr_t)guest,src,offset);
    pr->sh[4]->rf.nb=0;mesh_indirect_op(OP_DRAW_IDX,NULL);mesh_indirect_written(pr);
    if(mesh_indirect_grid((uintptr_t)guest,src,offset)!=a)MDIE("read-only indexed draw dropped record");checked++;
    mesh_indirect_op(OP_BARRIER,NULL);
    if(g_mic_n)MDIE("explicit barrier retained conversion pass boundary");checked++;
    a=mesh_indirect_grid((uintptr_t)guest,src,offset);
    tex_t target={0};target.mem=(void *)(guest+1);target.L.size=4;
    __typeof__(R) saved_rt=R;R.has_rt=1;R.rt.n=1;R.rt.color[0]=&target;R.rt.depth=NULL;
    mesh_indirect_written(pr);R=saved_rt;
    if(g_mic_n)MDIE("overlapping render target retained record");checked++;
    a=mesh_indirect_grid((uintptr_t)guest,src,offset);mesh_indirect_written(nil);
    if(g_mic_n)MDIE("unresolved draw program retained record");checked++;
    if(neg==2){
        // Deliberately retain a conversion across an actual GPU overwrite.
        // The timeline check must see the new source and reject the old grid.
        mesh_indirect_grid((uintptr_t)guest,src,offset);g_mic_ignore_writes=1;
        uint32_t wrong=13;mtl_sub_update(&wrong,(uintptr_t)guest,4);
        mesh_indirect_grid((uintptr_t)guest,src,offset);g_mic_ignore_writes=0;
    }
    mtl_sub_commit();mtl_finish();
    atomic_store(&g_mesh_reuse_check,0);
    for(unsigned i=0;i<n;i++)for(unsigned j=0;j<4;j++){
        if(((uint32_t *)rb.contents)[i*4+j]!=expected[i][j])MDIE("GPU mesh conversion %u word %u mismatch",i,j);checked++;
    }
    mtl_sub_begin();mesh_indirect_context();if(g_mic_n)MDIE("mesh conversion survived arena replacement");checked++;
    a=mesh_indirect_grid((uintptr_t)guest,src,offset);mtl_sub_commit();mtl_finish();
    extern void dm_guest_write_prepare(void *,size_t);
    dm_guest_write_prepare(guest,4);guest[0]=9;
    // Check the real dirty-page tracker before a new arena masks this change.
    if(!g_mic_n||!nvn_dirty_since(guest,8,g_mic[0].stamp))MDIE("CPU overwrite not tracked");checked++;
    mtl_sub_begin();a=mesh_indirect_grid((uintptr_t)guest,src,offset);
    atomic_fetch_add_explicit(&g_pool_gen,1,memory_order_release);mesh_indirect_context();
    if(g_mic_n)MDIE("pool replacement retained mesh records");checked++;
    mtl_sub_commit();mtl_finish();atomic_store(&g_mesh_reuse,old);
    memset(&S.ssbo[4][0],0,sizeof S.ssbo[4][0]);g_mic_n=0;
    nvn_dirty_remove_range(guest);mtl_pool_del(guest);munmap(guest,bytes);defer_obj_release(rb);
    MLOG("mesh indirect GPU conversion/reuse/invalidation: %d checks",checked);return checked;
}
int mtl_probe_mesh(void) {
    mtl_init();NSError *error=nil;int checked=0;
    checked+=mesh_indirect_probe();
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct dm_vertex_t { float4 position [[position]];\nfloat4 a1 [[user(locn1)]];\n};\n"
        "struct dm_prim_t {\nfloat4 p3 [[user(locn3)]];\n};\n"
        "using dm_mesh_t=metal::mesh<dm_vertex_t,dm_prim_t,3,1,metal::topology::triangle>;\n"
        "[[mesh]] void main0(dm_mesh_t dm_out,uint lid [[thread_position_in_threadgroup]]) {\n"
        "if(lid<3){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};dm_vertex_t v;v.position=float4(p[lid],0.5,1);v.a1=float4(0.25);dm_out.set_vertex(lid,v);dm_out.set_index(lid,lid);}\n"
        "if(lid==0){dm_prim_t p;p.p3=float4(0.75);dm_out.set_primitive(0,p);dm_out.set_primitive_count(1);}}\n";
    id<MTLLibrary> library=totk_msl_library(source,&error);if(!library)MDIE("mesh probe library: %s",error.localizedDescription.UTF8String);
    NvShader *ms=[NvShader new];ms->fn=[library newFunctionWithName:@"main0"];ms->src=source;ms->stage=4;ms->mesh_kind=2;ms->mesh_notask=1;ms->mesh_threads=32;strcpy(ms->key,"probe-mesh");
    NSDictionary *outputs=msl_varyings(ms);if(outputs.count!=2||!outputs[@1]||!outputs[@3])MDIE("mesh probe did not identify vertex and primitive outputs");
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
    td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget;id<MTLTexture> color=[g_dev newTextureWithDescriptor:td];res_add(color);id<MTLBuffer> readback=probe_buf(1024);
    memset(&S.color,0,sizeof S.color);S.chmask.mask=15;S.ms.a2c=0;memset(g_pass.cfmt,0,sizeof g_pass.cfmt);g_pass.cfmt[0]=MTLPixelFormatRGBA8Unorm;g_pass.dfmt=g_pass.sfmt=0;g_pass.samples=1;
    NSMutableArray *keep=[NSMutableArray arrayWithObject:ms];
    for(int mode=0;mode<3;mode++) {
        NSString *extra=mode==0?@"":mode==1?@"float4 a2 [[user(locn2)]];\n":@"uint4 a2 [[user(locn2),flat]];\n";
        NSString *fsrc=[NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\nstruct main0_in\n{\nfloat4 a1 [[user(locn1)]];\n%@float4 p3 [[user(locn3),flat]];\n};\nfragment float4 main0(main0_in in [[stage_in]]) { return float4(in.a1.x,%@,in.p3.x,1); }",extra,mode==0?@"0.0":@"float(in.a2.x)"];
        id<MTLLibrary> fl=totk_msl_library(fsrc,&error);if(!fl)MDIE("mesh probe fragment: %s",error.localizedDescription.UTF8String);
        NvShader *fs=[NvShader new];fs->src=fsrc;fs->stage=1;fs->fn=[fl newFunctionWithName:@"main0"];snprintf(fs->key,sizeof fs->key,"probe-mesh-fragment-%d",mode);[keep addObject:fs];
        g_mtk=(__typeof__(g_mtk)){.on=1,.ms=ms,.count=1};NvProg *raster=[NvProg new];raster->sh[1]=fs;
        id<MTLFunction> linked=mtk_mesh_fn(ms,fs);if(mode==0&&linked!=ms->fn)MDIE("mesh probe replaced an already complete interface");
        id<MTLRenderPipelineState> pipeline=mtk_pso(raster);
        mtl_sub_begin();MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];rp.colorAttachments[0].texture=color;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.renderTargetWidth=rp.renderTargetHeight=16;
        id<MTL4RenderCommandEncoder> en=enc_r_begin(rp);enc_r_pso(en,pipeline);enc_r_viewport(en,(MTLViewport){0,0,16,16,0,1});enc_r_scissor(en,(MTLScissorRect){0,0,16,16});
        mtk_encode(en);enc_r_end(en);enc_b_copy_texture_buffer(blit(),color,0,0,MTLOriginMake(0,0,0),MTLSizeMake(16,16,1),readback,0,64,1024);mtl_sub_commit();mtl_finish();
        const uint8_t *pixels=readback.contents;for(int p=0;p<256;p++){const uint8_t *v=pixels+4*p;if(abs((int)v[0]-64)>1||v[1]!=0||abs((int)v[2]-191)>1||v[3]!=255)MDIE("mesh probe mode %d pixel %d: %u %u %u %u",mode,p,v[0],v[1],v[2],v[3]);checked++;}
    }
    g_mtk.on=0;defer_obj_release(color);defer_obj_release(readback);
    // Optional original-program corpus, supplied externally. This exercises the
    // production linkage/pipeline builder without replaying an opening movie.
    const char *manifest=getenv("OOT_MESH_PIPELINE_CASES");
    if(manifest) {
        NSData *raw=[NSData dataWithContentsOfFile:@(manifest)];NSArray *cases=raw?[NSJSONSerialization JSONObjectWithData:raw options:0 error:&error]:nil;
        if(![cases isKindOfClass:NSArray.class])MDIE("invalid mesh pipeline fixture manifest");
        NSMutableDictionary *loaded=[NSMutableDictionary dictionary];
        NvShader *(^load)(NSDictionary *)=^NvShader *(NSDictionary *entry){
            if(!entry)return nil;NSString *path=entry[@"source"];NvShader *sh=loaded[path];if(sh)return sh;
            NSError *err=nil;NSString *src=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&err];
            NSData *json=[NSData dataWithContentsOfFile:entry[@"reflection"]];NSDictionary *rf=json?[NSJSONSerialization JSONObjectWithData:json options:0 error:&err]:nil;
            id<MTLLibrary> lib=src?totk_msl_library(src,&err):nil;if(!lib||!rf)MDIE("mesh pipeline fixture %s: %s",path.UTF8String,err.localizedDescription.UTF8String);
            sh=[NvShader new];sh->fn=[lib newFunctionWithName:@"main0"];sh->src=src;sh->stage=[entry[@"stage"]intValue];sh->writes_depth=[src containsString:@"[[depth("];
            NSDictionary *mesh=rf[@"mesh_stage"];
            if(mesh){sh->mesh_kind=[mesh[@"kind"]isEqualToString:@"ts"]?1:2;sh->mesh_threads=[mesh[@"threads"]intValue];sh->mesh_payload=[mesh[@"payload"]intValue];sh->mesh_notask=![mesh[@"task"]boolValue];}
            strlcpy(sh->key,path.lastPathComponent.UTF8String,sizeof sh->key);loaded[path]=sh;return sh;
        };
        unsigned count=0;
        for(NSDictionary *fixture in cases){@autoreleasepool{
            NvShader *mesh=load(fixture[@"mesh"]),*task=load(fixture[@"task"]),*fs=load(fixture[@"fragment"]);
            if(!mesh||mesh->mesh_kind!=2||(task&&task->mesh_kind!=1))MDIE("bad mesh pipeline fixture stages");
            g_mtk=(__typeof__(g_mtk)){.on=1,.ms=mesh,.ts=task};NvProg *raster=[NvProg new];raster->sh[1]=fs;
            for(int depth=0;depth<2;depth++){
                for(int a=0;a<8;a++)g_pass.cfmt[a]=MTLPixelFormatRGBA8Unorm;
                S.chmask.mask=0xffffffff;g_pass.dfmt=depth?MTLPixelFormatDepth32Float:0;
                if(!mtk_pso(raster))MDIE("mesh pipeline fixture failed");checked++;
            }
            if(++count%50==0)MLOG("mesh pipeline fixtures: %u complete",count);
        }}
        g_mtk.on=0;MLOG("mesh pipeline fixtures: %u complete, %lu unique shaders",count,(unsigned long)loaded.count);
    }
    return checked;
}
