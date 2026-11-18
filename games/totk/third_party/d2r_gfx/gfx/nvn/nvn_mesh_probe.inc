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
/* Empty and nonempty mesh groups have identical required storage writes.
   Exercise the production variant/PSO selection, pixels and those side effects
   in A/B/A order. A zero primitive count must not suppress the guest body. */
static int mesh_empty_export_probe(void) {
    int checked=0,saved=atomic_load(&g_mesh_empty_export);NSError *error=nil;
    NSString *src=@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct dm_vertex_t { float4 position [[position]];\nfloat4 a1 [[user(locn1)]];\n};\n"
        "using dm_mesh_t=metal::mesh<dm_vertex_t,void,3,1,metal::topology::triangle>;\n"
        "static inline void dm_stage_body(device uint *p,uint tg){p[tg]=0x12340000u+tg;}\n"
        "[[mesh]] void main0(dm_mesh_t dm_out,device uint *oot_results [[buffer(27)]],constant uint *dm_first [[buffer(29)]],uint dm_lid [[thread_position_in_threadgroup]],uint dm_tg [[threadgroup_position_in_grid]]){\n"
        "if(dm_lid==0u)dm_stage_body(oot_results,dm_tg);\nthreadgroup_barrier(mem_flags::mem_threadgroup);\n"
        "uint np=min(dm_first[0]?(dm_tg&1u):0u,1u);\nif(dm_lid==0u)dm_out.set_primitive_count(np);\n"
        "for(uint n=dm_lid;n<3u;n+=32u){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};dm_vertex_t v;v.position=float4(p[n],0.5,1);v.a1=float4(.25,.5,.75,1);dm_out.set_vertex(n,v);}\n"
        "for(uint n=dm_lid;n<np*3u;n+=32u)dm_out.set_index(n,n);}\n";
    if(getenv("OOT_MESH_EXPORT_PROBE_NEGATIVE"))src=[src stringByReplacingOccurrencesOfString:@"if(dm_lid==0u)dm_stage_body" withString:@"if(dm_lid==0u&&dm_first[0]&&(dm_tg&1u))dm_stage_body"];
    id<MTLLibrary> lib=totk_msl_library(src,&error);if(!lib)MDIE("empty mesh fixture: %s",error.localizedDescription.UTF8String);
    NvShader *ms=[NvShader new];ms->src=src;ms->fn=[lib newFunctionWithName:@"main0"];ms->stage=4;ms->mesh_kind=2;ms->mesh_notask=1;ms->mesh_threads=32;strcpy(ms->key,"probe-empty-mesh");
    NSString *fsrc=@"#include <metal_stdlib>\nusing namespace metal;\nstruct main0_in\n{\nfloat4 a1 [[user(locn1)]];\n};\nfragment float4 main0(main0_in v [[stage_in]]){return v.a1;}\n";
    id<MTLLibrary> fl=totk_msl_library(fsrc,&error);if(!fl)MDIE("empty mesh fragment: %s",error.localizedDescription.UTF8String);
    NvShader *fs=[NvShader new];fs->src=fsrc;fs->fn=[fl newFunctionWithName:@"main0"];fs->stage=1;strcpy(fs->key,"probe-empty-mesh-fragment");
    NvProg *raster=[NvProg new];raster->sh[1]=fs;
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> color=[g_dev newTextureWithDescriptor:td];res_add(color);id<MTLBuffer> rb=probe_buf(1024),side=probe_buf(32*4);
    for(int mode=0;mode<3;mode++)for(int mixed=0;mixed<2;mixed++) {
        atomic_store(&g_mesh_empty_export,mode==1);memset(side.contents,0,side.length);
        g_mtk=(__typeof__(g_mtk)){.on=1,.ms=ms,.fs=fs,.first=mixed,.count=32};
        id<MTLFunction> variant=mtk_empty_export_fn(ms->fn,ms);
        if((variant!=ms->fn)!=(mode==1))MDIE("empty mesh probe did not switch production variant");checked++;
        id<MTLRenderPipelineState> ps=mtk_pso(raster);
        mtl_sub_begin();MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];rp.colorAttachments[0].texture=color;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.renderTargetWidth=rp.renderTargetHeight=16;
        if(getenv("OOT_MESH_GPU_FILE"))pt_mark(g_cb,'R',rp);
        id<MTL4RenderCommandEncoder> en=enc_r_begin(rp);enc_r_pso(en,ps);enc_r_viewport(en,(MTLViewport){0,0,16,16,0,1});enc_r_scissor(en,(MTLScissorRect){0,0,16,16});enc_r_vbuf(en,27,side.gpuAddress);mtk_encode(en);enc_r_end(en);
        enc_b_copy_texture_buffer(blit(),color,0,0,MTLOriginMake(0,0,0),MTLSizeMake(16,16,1),rb,0,64,1024);mtl_sub_commit();mtl_finish();
        for(int i=0;i<32;i++){if(((uint32_t *)side.contents)[i]!=(0x12340000u+i))MDIE("empty mesh lost guest side effect group%d",i);checked++;}
        const int rgba[4]={64,128,191,255};for(int i=0;i<1024;i++){int want=mixed?rgba[i%4]:0;if(abs(((uint8_t *)rb.contents)[i]-want)>1)MDIE("empty mesh mode%d mixed%d pixel byte%d: %u != %d",mode,mixed,i,((uint8_t *)rb.contents)[i],want);checked++;}
    }
    g_mtk.on=0;atomic_store(&g_mesh_empty_export,saved);defer_obj_release(color);defer_obj_release(rb);defer_obj_release(side);return checked;
}
/* Device stage memory (MeshStage.cs, mtk_scratch_gpu): a threadgroup's slot stays its own while it runs, however many
   threadgroups the GPU keeps alive and however few slots there are. 20,000 threadgroups share 16 slots of 64 words: each
   fills its slice with its own mark, re-reads it eight times (time for a neighbour to overlap) and counts foreign words;
   the slot is released as the generated wrapper does (barrier, then lane 0 clears the flag), and every flag must be clear
   at the end. OOT_MESH_SLOT_PROBE_NEGATIVE=1 takes slots by the counter alone, as a ring without flags would: overlaps
   must be counted, so the probe must fail. */
static int mesh_device_stage_probe(void) {
    int checked=0;NSError *error=nil;const unsigned slots=16,words=64,groups=20000;int negative=getenv("OOT_MESH_SLOT_PROBE_NEGATIVE")!=NULL;
    NSString *take=negative?@"true":[NSString stringWithFormat:@"atomic_exchange_explicit((device atomic_uint*)(dm_scratch+%du+s),1u,memory_order_relaxed)==0u",MTK_SCRATCH_FLAGS];
    NSString *give=negative?@"":[NSString stringWithFormat:@"atomic_store_explicit((device atomic_uint*)(dm_scratch+%du+dm_slot_tg),0u,memory_order_relaxed);",MTK_SCRATCH_FLAGS];
    NSString *src=[NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct dm_vertex_t { float4 position [[position]]; };\nusing dm_mesh_t=metal::mesh<dm_vertex_t,void,3,1,metal::topology::triangle>;\n"
        "[[mesh]] void main0(dm_mesh_t dm_out,device atomic_uint *oot_results [[buffer(27)]],device uint* dm_scratch [[buffer(%d)]],uint dm_lid [[thread_position_in_threadgroup]],uint dm_tg [[threadgroup_position_in_grid]]){\n"
        "threadgroup uint dm_slot_tg;\n"
        "if(dm_lid==0u){ uint s; for(;;){ s=atomic_fetch_add_explicit((device atomic_uint*)dm_scratch,1u,memory_order_relaxed)&dm_scratch[1]; if(%@)break; } dm_slot_tg=s; }\n"
        "threadgroup_barrier(mem_flags::mem_threadgroup);\n"
        "device uint* mem=dm_scratch+%du+dm_slot_tg*dm_scratch[2];\nuint mark=0x5a000000u|dm_tg;\n"
        "for(uint i=dm_lid;i<%uu;i+=32u)mem[i]=mark;\nthreadgroup_barrier(mem_flags::mem_threadgroup|mem_flags::mem_device);\n"
        "uint bad=0u;for(uint k=0u;k<8u;k++){for(uint i=dm_lid;i<%uu;i+=32u)bad+=mem[i]!=mark;threadgroup_barrier(mem_flags::mem_threadgroup|mem_flags::mem_device);}\n"
        "if(bad)atomic_fetch_add_explicit(oot_results,bad,memory_order_relaxed);\n"
        "threadgroup_barrier(mem_flags::mem_threadgroup|mem_flags::mem_device);\n"
        "if(dm_lid==0u){atomic_fetch_add_explicit(oot_results+1,1u,memory_order_relaxed);%@}\n"
        "if(dm_lid==0u)dm_out.set_primitive_count(0u);}\n",MTK_SCRATCH_BUFFER,take,MTK_SCRATCH_SLOTS,words,words,give];
    id<MTLLibrary> lib=totk_msl_library(src,&error);if(!lib)MDIE("device stage fixture: %s",error.localizedDescription.UTF8String);
    NSString *fsrc=@"#include <metal_stdlib>\nusing namespace metal;\nfragment float4 main0(){return float4(1.0);}\n";
    id<MTLLibrary> fl=totk_msl_library(fsrc,&error);if(!fl)MDIE("device stage fragment: %s",error.localizedDescription.UTF8String);
    MTLMeshRenderPipelineDescriptor *d=[MTLMeshRenderPipelineDescriptor new];d.meshFunction=[lib newFunctionWithName:@"main0"];d.fragmentFunction=[fl newFunctionWithName:@"main0"];
    d.maxTotalThreadsPerMeshThreadgroup=32;d.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> ps=[g_dev newRenderPipelineStateWithMeshDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error];
    if(!ps)MDIE("device stage pipeline: %s",error.localizedDescription.UTF8String);
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> color=[g_dev newTextureWithDescriptor:td];res_add(color);
    id<MTLBuffer> results=probe_buf(16),scratch=probe_buf(((NSUInteger)MTK_SCRATCH_SLOTS+(NSUInteger)slots*words)*4);
    for(int round=0;round<3;round++){
        memset(results.contents,0,16);uint32_t *h=scratch.contents;memset(h,0,(size_t)MTK_SCRATCH_SLOTS*4);h[1]=slots-1;h[2]=words;
        mtl_sub_begin();MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];rp.colorAttachments[0].texture=color;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionDontCare;rp.renderTargetWidth=rp.renderTargetHeight=16;
        id<MTL4RenderCommandEncoder> en=enc_r_begin(rp);enc_r_pso(en,ps);enc_r_viewport(en,(MTLViewport){0,0,16,16,0,1});enc_r_scissor(en,(MTLScissorRect){0,0,16,16});
        if(rs_using(en))rs_put((nvn_encode_packet){.op=RS_MESH_TABLES,.u={0}},0);else [en setArgumentTable:g_vt atStages:MTLRenderStageMesh];   // the mesh stage's buffer table, as mtk_encode binds it
        enc_r_vbuf(en,27,results.gpuAddress);enc_r_vbuf(en,MTK_SCRATCH_BUFFER,scratch.gpuAddress);
        if(rs_using(en))rs_put((nvn_encode_packet){.op=RS_DRAW_MESH,.u={groups,1,32}},1);else [en drawMeshThreadgroups:MTLSizeMake(groups,1,1) threadsPerObjectThreadgroup:MTLSizeMake(1,1,1) threadsPerMeshThreadgroup:MTLSizeMake(32,1,1)];
        enc_r_end(en);mtl_sub_commit();mtl_finish();
        const uint32_t *r=results.contents;
        if(r[1]!=groups)MDIE("device stage probe round %d: %u of %u threadgroups finished",round,r[1],groups);checked++;
        if(r[0])MDIE("device stage probe round %d: %u words overwritten by another live threadgroup (%u slots for %u threadgroups)",round,r[0],slots,groups);checked++;
        for(unsigned s=0;s<slots;s++)if(h[MTK_SCRATCH_FLAGS+s])MDIE("device stage probe round %d: slot %u left in use",round,s);checked++;
    }
    MLOG("[mesh] device stage memory probe: %u threadgroups through %u slots, 3 rounds, no overlap, every slot released",groups,slots);
    defer_obj_release(color);defer_obj_release(results);defer_obj_release(scratch);return checked;
}
/* Packed varyings (nvn_mesh_pack.inc): a mesh in the generator's export format spreading real lanes over five attributes
   (one read flat, one without perspective) plus one input the mesh never writes; the fragment reads lanes of all six.
   Rendered with packing off and on: both must give the expected constants and identical bytes, and the packed pair must
   declare four slots (flat 1, no-perspective 1, default 2) instead of six. */
static int mesh_pack_probe(void) {
    NSError *error=nil;int checked=0;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct dm_vertex_t { float4 position [[position, invariant]];\nfloat4 a0 [[user(locn0)]];\nfloat4 a1 [[user(locn1)]];\nfloat4 a2 [[user(locn2)]];\nfloat4 a5 [[user(locn5)]];\nfloat4 a6 [[user(locn6)]];\n};\n"
        "struct dm_prim_t {\nfloat4 p7 [[user(locn7)]];\n};\n"
        "using dm_mesh_t=metal::mesh<dm_vertex_t,dm_prim_t,3,1,metal::topology::triangle>;\n"
        "[[mesh]] void main0(dm_mesh_t dm_out, uint dm_lid [[thread_position_in_threadgroup]]) {\n"
        "threadgroup uint _46[16];\nfor(uint i=dm_lid;i<16u;i+=32u)_46[i]=as_type<uint>(float(i)*0.0625);\nthreadgroup_barrier(mem_flags::mem_threadgroup);\n"
        "if(dm_lid==0u){dm_out.set_primitive_count(1u);uint b=0u;dm_prim_t p;\np.p7=float4(as_type<float>(_46[b+11u]), 0.0, 0.0, 1.0);\ndm_out.set_primitive(0u,p);}\n"
        "for(uint n=dm_lid;n<3u;n+=32u){uint b=0u; dm_vertex_t v;\n"
        "float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
        "v.position=float4(p[n],0.5,1.0);\n"
        "v.a0=float4(as_type<float>(_46[b+1u]), 0.0, 0.0, 1.0);\n"
        "v.a1=float4(as_type<float>(_46[b+2u]), as_type<float>(_46[b+3u]), as_type<float>(_46[b+4u]), 1.0);\n"
        "v.a2=float4(as_type<float>(_46[b+5u]), 0.0, 0.0, 1.0);\n"
        "v.a5=float4(as_type<float>(_46[b+6u]), as_type<float>(_46[b+7u]), 0.0, 1.0);\n"
        "v.a6=float4(as_type<float>(_46[b+8u]), as_type<float>(_46[b+9u]), as_type<float>(_46[b+10u]), 1.0);\n"
        "dm_out.set_vertex(n,v);}\n"
        "for(uint n=dm_lid;n<3u;n+=32u)dm_out.set_index(n,n);\n}\n";
    NSString *fsrc=@"#include <metal_stdlib>\nusing namespace metal;\nstruct main0_in\n{\n    float4 m_1 [[user(locn0), flat]];\n    float4 m_2 [[user(locn1)]];\n    float4 m_3 [[user(locn2), center_no_perspective]];\n    float4 m_4 [[user(locn3)]];\n    float4 m_5 [[user(locn5)]];\n    float4 m_6 [[user(locn6)]];\n    float4 m_7 [[user(locn7), flat]];\n};\n"
        "fragment float4 main0(main0_in in [[stage_in]])\n{\n    return float4(in.m_1.x + in.m_2.y + in.m_7.x, in.m_3.x + in.m_4.x + in.m_5.y, in.m_6.z + in.m_2.w * 0.125, in.m_5.w * 0.5 + in.m_1.w * 0.25 + in.m_7.w * 0.125);\n}\n";
    id<MTLLibrary> library=totk_msl_library(source,&error);if(!library)MDIE("mesh pack probe library: %s",error.localizedDescription.UTF8String);
    NvShader *ms=[NvShader new];ms->fn=[library newFunctionWithName:@"main0"];ms->src=source;ms->stage=4;ms->mesh_kind=2;ms->mesh_notask=1;ms->mesh_threads=32;strcpy(ms->key,"probe-mesh-pack");
    id<MTLLibrary> fl=totk_msl_library(fsrc,&error);if(!fl)MDIE("mesh pack probe fragment: %s",error.localizedDescription.UTF8String);
    NvShader *fs=[NvShader new];fs->src=fsrc;fs->stage=1;fs->fn=[fl newFunctionWithName:@"main0"];strcpy(fs->key,"probe-mesh-pack-fragment");
    id<MTLFunction> packedFs=nil,packed=mtk_pack_varyings(ms,fs,mtk_mesh_fn_base(ms,fs,ms->fn),1,&packedFs);
    NSString *psrc=fn_src(packed),*fsv=fn_src(packedFs);
    if(!packedFs||!psrc||![psrc containsString:@"float4 dm_p3 [[user("]||[psrc containsString:@"dm_p4 "]||[psrc containsString:@"oot_unwritten_3"]||[psrc containsString:@"v.a1="]||![psrc containsString:@"p.p7=float4("])MDIE("mesh pack probe: expected four packed slots, no original vertex exports, the primitive output untouched");
    if(!fsv||![fsv containsString:@"float4 dm_m_4 = float4(0.0, 0.0, 0.0, 0.0);"]||![fsv containsString:@"[[user(locn0), flat]]"]||![fsv containsString:@", center_no_perspective]]"]||![fsv containsString:@"float4 m_7 [[user(locn7), flat]];"]||![fsv containsString:@"in.m_7.x"]||[fsv containsString:@"in.m_1"])MDIE("mesh pack probe: fragment variant layout");
    MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
    td.storageMode=MTLStorageModePrivate;td.usage=MTLTextureUsageRenderTarget;id<MTLTexture> color=[g_dev newTextureWithDescriptor:td];res_add(color);id<MTLBuffer> readback=probe_buf(1024);
    memset(&S.color,0,sizeof S.color);S.chmask.mask=15;S.ms.a2c=0;memset(g_pass.cfmt,0,sizeof g_pass.cfmt);g_pass.cfmt[0]=MTLPixelFormatRGBA8Unorm;g_pass.dfmt=g_pass.sfmt=0;g_pass.samples=1;
    uint8_t unpacked[1024];
    for(int on=0;on<2;on++){
        atomic_store(&g_mesh_pack,on);
        g_mtk=(__typeof__(g_mtk)){.on=1,.profile_grid=-1,.ms=ms,.count=1};NvProg *raster=[NvProg new];raster->sh[1]=fs;
        id<MTLRenderPipelineState> pipeline=mtk_pso(raster);
        mtl_sub_begin();MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];rp.colorAttachments[0].texture=color;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.renderTargetWidth=rp.renderTargetHeight=16;
        id<MTL4RenderCommandEncoder> en=enc_r_begin(rp);enc_r_pso(en,pipeline);enc_r_viewport(en,(MTLViewport){0,0,16,16,0,1});enc_r_scissor(en,(MTLScissorRect){0,0,16,16});
        mtk_encode(en);enc_r_end(en);enc_b_copy_texture_buffer(blit(),color,0,0,MTLOriginMake(0,0,0),MTLSizeMake(16,16,1),readback,0,64,1024);mtl_sub_commit();mtl_finish();
        const uint8_t *pixels=readback.contents;
        // r = 1/16 + 3/16 + 11/16, g = 5/16 + 0 + 7/16, b = 10/16 + 1/8, a = 1/2 + 1/4 + 1/8
        for(int p=0;p<256;p++){const uint8_t *v=pixels+4*p;if(abs((int)v[0]-239)>1||abs((int)v[1]-191)>1||abs((int)v[2]-191)>1||abs((int)v[3]-223)>1)MDIE("mesh pack probe pack=%d pixel %d: %u %u %u %u",on,p,v[0],v[1],v[2],v[3]);checked++;}
        if(!on)memcpy(unpacked,pixels,sizeof unpacked);else if(memcmp(unpacked,pixels,sizeof unpacked))MDIE("mesh pack probe: packed pixels differ from the unpacked pair");
    }
    atomic_store(&g_mesh_pack,-1);g_mtk.on=0;defer_obj_release(color);defer_obj_release(readback);
    MLOG("[mesh] packed varyings probe: six fragment inputs over four slots, pixels identical to the unpacked pair");
    return checked;
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
    for(int mode=0;mode<4;mode++) {
        NSString *extra=mode==0?@"":mode==1?@"float4 a2 [[user(locn2)]];\n":@"uint4 a2 [[user(locn2),flat]];\n";
        NSString *fsrc=[NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\nstruct main0_in\n{\nfloat4 a1 [[user(locn1)]];\n%@float4 p3 [[user(locn3),flat]];\n};\nfragment float4 main0(main0_in in [[stage_in]]) { return float4(in.a1.x,%@,in.p3.x,1); }",extra,mode==0?@"0.0":@"float(in.a2.x)"];
        id<MTLLibrary> fl=totk_msl_library(fsrc,&error);if(!fl)MDIE("mesh probe fragment: %s",error.localizedDescription.UTF8String);
        NvShader *fs=[NvShader new];fs->src=fsrc;fs->stage=1;fs->fn=[fl newFunctionWithName:@"main0"];snprintf(fs->key,sizeof fs->key,"probe-mesh-fragment-%d",mode);[keep addObject:fs];
        g_mtk=(__typeof__(g_mtk)){.on=1,.profile_grid=-1,.ms=ms,.count=1};NvProg *raster=[NvProg new];raster->sh[1]=fs;
        id<MTLFunction> linked=mtk_mesh_fn(ms,fs);if(mode==0&&linked!=ms->fn)MDIE("mesh probe replaced an already complete interface");
        id<MTLRenderPipelineState> pipeline=mtk_pso(raster);
        mtl_sub_begin();MTL4RenderPassDescriptor *rp=[MTL4RenderPassDescriptor new];rp.colorAttachments[0].texture=color;rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;rp.renderTargetWidth=rp.renderTargetHeight=16;
        if(mode==3){id<MTLBuffer> grid;size_t at=arena_alloc(16,&grid);uint32_t initial[4]={0,1,1,0};memcpy((uint8_t *)grid.contents+at,initial,sizeof initial);*(uint32_t *)readback.contents=1;
            enc_b_copy_buffer(blit(),readback,0,grid,at,4);blit_close();g_mtk.indirect_gpu=grid.gpuAddress+at;g_mtk.profile_grid=ms_grid_capture(g_mtk.indirect_gpu);}
        if(getenv("OOT_MESH_GPU_FILE"))pt_mark(g_cb,'R',rp);
        id<MTL4RenderCommandEncoder> en=enc_r_begin(rp);enc_r_pso(en,pipeline);enc_r_viewport(en,(MTLViewport){0,0,16,16,0,1});enc_r_scissor(en,(MTLScissorRect){0,0,16,16});
        mtk_encode(en);enc_r_end(en);enc_b_copy_texture_buffer(blit(),color,0,0,MTLOriginMake(0,0,0),MTLSizeMake(16,16,1),readback,0,64,1024);mtl_sub_commit();mtl_finish();
        const uint8_t *pixels=readback.contents;for(int p=0;p<256;p++){const uint8_t *v=pixels+4*p;if(abs((int)v[0]-64)>1||v[1]!=0||abs((int)v[2]-191)>1||v[3]!=255)MDIE("mesh probe mode %d pixel %d: %u %u %u %u",mode,p,v[0],v[1],v[2],v[3]);checked++;}
    }
    g_mtk.on=0;defer_obj_release(color);defer_obj_release(readback);
    checked+=mesh_empty_export_probe();
    checked+=mesh_device_stage_probe();
    checked+=mesh_pack_probe();
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
