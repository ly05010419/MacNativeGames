// Transform feedback uses a void vertex entry. For GS output the generated
// reader gathers packed primitive indices; its draw count stays on the GPU.
enum { TF_SLOT = 8 };
static id<MTLBuffer> g_tf_state;
static id<MTLComputePipelineState> g_tf_prepare;
static void tf_state_gpu(const p_tf_state *b) {
    rp_end(); ubd_land();
    if (!g_tf_state) { g_tf_state = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared]; res_add(g_tf_state); }
    if (b->what == 0) {
        id<MTLBuffer> buf; size_t off = arena_alloc(32, &buf);
        uint32_t initial[8] = {0,0,0,0,0,1,0,0}; memcpy((uint8_t *)buf.contents + off, initial, 32);
        enc_b_copy_buffer(blit(), buf, off, g_tf_state, 0, 32);
    } else if (b->ctl) {
        void *buffer; uint64_t off, gpu;
        if (!mtl_gpu_addr(b->ctl, 32, &buffer, &off, &gpu)) { RED_ONCE(103, "transform feedback control buffer is outside a memory pool"); return; }
        if (b->what == 3) enc_b_copy_buffer(blit_buf(b->ctl,b->ctl+32,0,0), (__bridge id<MTLBuffer>)buffer, off, g_tf_state, 0, 32);
        else { enc_b_copy_buffer(blit_buf(0,0,b->ctl,b->ctl+32), g_tf_state, 0, (__bridge id<MTLBuffer>)buffer, off, 32); pend_add(b->ctl,32,4); }
    }
    g_wr_epoch++; g_mdx_epoch++;
}
static id<MTLRenderPipelineState> tf_pso(NvShader *vs) {
    pso_key k; static const uint32_t cf[8]; pso_key_fill(&k, vs, nil, cf, 0, 0, 1, 0); k.fsfn = TF_MARK;
    id<MTLRenderPipelineState> cached = hm_get_id(&g_pso, &k);
    if (cached || hm_get_id(&g_pso_fail, &k)) return cached;
    NSError *e = nil; id<MTLRenderPipelineState> ps = pso_build(pso_desc(&k, vs, nil, nil, 0), &e);
    if (!ps) { MLOG("[RED] transform feedback pipeline %s: %s", vs->key, e.localizedDescription.UTF8String); hm_put_id(&g_pso_fail, &k, NSNull.null); }
    else hm_put_id(&g_pso, &k, ps);
    return ps;
}
static uint64_t tf_prepare(NvShader *vs, uint64_t source_count, uint64_t order, uint32_t count, uint32_t primitive,
                           uint32_t first, uint32_t per_instance, uint32_t base_instance, uint64_t dst[4]) {
    if (!g_tf_state || !vs || !vs->tf_fn) { RED_ONCE(100, "transform feedback shader %s has no capture entry", vs ? vs->key : "-"); return 0; }
    nvn_tf_job j = {.order=order,.first=first,.count=per_instance,.base_instance=base_instance,
                    .state=g_tf_state.gpuAddress,.source_count=source_count,.requested=count,.primitive=primitive};
    for (int b = 0; b < 4; b++) if (vs->tf_stride[b]) {
        void *buf; uint64_t off;
        if (!S.tf[b].addr || S.tf[b].size > UINT32_MAX || !mtl_gpu_addr(S.tf[b].addr,S.tf[b].size,&buf,&off,&dst[b])) {
            RED_ONCE(102, "transform feedback buffer %d of %s is unbound or outside its memory pool", b, vs->key); return 0; }
        j.stride[b]=vs->tf_stride[b];j.capacity[b]=(uint32_t)S.tf[b].size;
    }
    if (!g_tf_prepare) {
        NSError *e=nil; id<MTLLibrary> lib=[g_dev newLibraryWithSource:@(k_nvn_tf_msl) options:nil error:&e];
        if (lib) g_tf_prepare=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"tf_prepare"] error:&e];
        if (!g_tf_prepare) { MLOG("[RED] transform feedback prepare: %s",e.localizedDescription.UTF8String); return 0; }
    }
    rp_end(); ubd_land();
    if (g_blit) { enc_b_end(g_blit);g_blit=nil; }
    id<MTLBuffer> buf;size_t off=arena_alloc(sizeof j,&buf);memcpy((uint8_t *)buf.contents+off,&j,sizeof j);
    uint64_t job=buf.gpuAddress+off;
    id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,g_tf_prepare);enc_c_args(ce,job,0);
    enc_c_dispatch(ce,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1));enc_c_end(ce);
    return job;
}
static void tf_note(NvShader *vs) {
    for(int b=0;b<4;b++)if(vs->tf_stride[b])pend_add(S.tf[b].addr,S.tf[b].size,4);
    args_dirty_all();g_enc_pso=nil;g_enc_dss=nil;g_ed.ok=0;g_wr_epoch++;g_mdx_epoch++;
}
static void tf_draw(NvProg *pr,const p_draw *dw,const p_draw_idx *di) {
    NvShader *vs=pr?pr->sh[0]:nil;
    if(!dw||dw->ind||dw->mdi_cnt||!(dw->prim==0||dw->prim==1||dw->prim==4)) {
        RED_ONCE(101,"transform feedback array capture requires a point, line or triangle list (vertex %s)",vs?vs->key:"-");return; }
    if(dw->count<=0||dw->instances<0)return;
    unsigned instances=MAX(dw->instances,1);uint64_t count=(uint64_t)dw->count*instances;
    if(count>UINT32_MAX)return;
    id<MTLRenderPipelineState> ps=vs&&vs->tf_fn?tf_pso(vs):nil;if(!ps)return;
    uint64_t dst[4]={0};uint64_t job=tf_prepare(vs,0,0,(uint32_t)count,dw->prim==0?1:dw->prim==1?2:3,
                                              dw->first,dw->count,dw->base_instance,dst);if(!job)return;
    vbuf_prepare();if(g_blit){enc_b_end(g_blit);g_blit=nil;}
    MTL4RenderPassDescriptor *rd=[MTL4RenderPassDescriptor new];rd.renderTargetWidth=rd.renderTargetHeight=rd.defaultRasterSampleCount=1;
    id<MTL4RenderCommandEncoder> en=enc_r_begin(rd);enc_r_pso(en,ps);bind_vbufs(en);vconv_bind(en,vs,1,dw,NULL);
    args_dirty_all();encode_args(en,vs,0,1);enc_r_vbuf(en,14,job);
    for(int b=0;b<4;b++)if(vs->tf_stride[b])enc_r_vbuf(en,TF_SLOT+b,dst[b]);
    enc_r_draw_instanced(en,MTLPrimitiveTypePoint,dw->first,dw->count,instances,dw->base_instance);enc_r_end(en);
    swrite_note(pr);tf_note(vs);
    PD("T %llu tf capture vs=%s first=%d count=%d instances=%u",g_stat_pass,vs->key,dw->first,dw->count,instances);
}
static void tf_geometry(NvShader *reader,uint64_t indices,uint64_t args,uint32_t primitive,uint32_t base_instance) {
    if(!reader->tf_fn){RED_ONCE(104,"geometry feedback reader %s has no capture entry",reader->key);return;}
    id<MTLRenderPipelineState> ps=tf_pso(reader);if(!ps)return;
    uint64_t dst[4]={0};uint64_t job=tf_prepare(reader,args,indices,0,primitive,0,0,base_instance,dst);if(!job)return;
    MTL4RenderPassDescriptor *rd=[MTL4RenderPassDescriptor new];rd.renderTargetWidth=rd.renderTargetHeight=rd.defaultRasterSampleCount=1;
    id<MTL4RenderCommandEncoder> en=enc_r_begin(rd);enc_r_pso(en,ps);args_dirty_all();encode_args(en,reader,0,1);enc_r_vbuf(en,14,job);
    for(int b=0;b<4;b++)if(reader->tf_stride[b])enc_r_vbuf(en,TF_SLOT+b,dst[b]);
    enc_r_draw_indirect(en,MTLPrimitiveTypePoint,job+16);enc_r_end(en);tf_note(reader);
}
