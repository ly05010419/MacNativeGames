/* Included after the production backend: first-use pixels, not just masks. */
/* The texture-ID set of a handle table across captures (pool_input_ids): the first capture scans the table, a later one
   only the pages written since, unchanged tables are not scanned, and a host write-back starts the set again. */
static int pool_ids_probe(void) {
    int checked=0;size_t page=(size_t)getpagesize(),span=page*4;void *m=NULL;
    if(posix_memalign(&m,page,span))MDIE("pool ids probe allocation");
    uint32_t *w=m;for(size_t i=0;i<span/4;i++)w[i]=0xfffffu;   // ID 1048575: past the pool, not a handle
    w[0]=5;w[page/4]=9;
    mtl_pool_add(m,span);nvn_dirty_add_range(m,span);
    p_bind_buf u={.addr=(uintptr_t)m,.size=(uint32_t)span};
    op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);
    uint64_t full0=g_pool_ids_full,inc0=g_pool_ids_words_inc;
    pool_input *p=pool_input_get(&u,64);const uint64_t *ids=p->ids.bytes;
    if(g_pool_ids_full!=full0+1||!(ids[0]>>5&1)||!(ids[0]>>9&1)||ids[0]>>17&1)MDIE("pool ids: first capture %#llx",(unsigned long long)ids[0]);checked++;
    ((volatile uint32_t *)w)[2*page/4]=17;   // the game writes one page (write fault: its generation)
    p=pool_input_get(&u,64);ids=p->ids.bytes;
    if(g_pool_ids_full!=full0+1)MDIE("pool ids: the rescan after one written page was not incremental");checked++;
    if(g_pool_ids_words_inc-inc0!=page/4)MDIE("pool ids: scanned %llu words, wanted the written page (%zu)",(unsigned long long)(g_pool_ids_words_inc-inc0),page/4);checked++;
    if(!(ids[0]>>17&1)||!(ids[0]>>5&1)||!(ids[0]>>9&1))MDIE("pool ids: the set lost or missed an ID: %#llx",(unsigned long long)ids[0]);checked++;
    uint64_t inc1=g_pool_ids_words_inc;p=pool_input_get(&u,64);
    if(g_pool_ids_words_inc!=inc1||g_pool_ids_full!=full0+1)MDIE("pool ids: an unchanged table was scanned again");checked++;
    atomic_fetch_add(&g_pool_input_hostwrite,1);   // a host write-back marks no page
    p=pool_input_get(&u,64);
    if(g_pool_ids_full!=full0+2)MDIE("pool ids: a host write-back did not rescan the whole table");checked++;
    mtl_sub_commit();mtl_finish();nvn_dirty_remove_range(m);mtl_pool_del(m);free(m);
    return checked;
}
int mtl_probe_memory_demand(void) {
    mtl_init();int checked=raw_write_domain_probe()+upstream_probe()+pool_ids_probe(),negative=getenv("OOT_MEMORY_PROBE_NEGATIVE")?atoi(getenv("OOT_MEMORY_PROBE_NEGATIVE")):0;
    size_t page=(size_t)getpagesize();void *raw=NULL;
    if(posix_memalign(&raw,page,page*2))MDIE("memory demand probe allocation");memset(raw,0,page*2);
    int owner_a=0,owner_b=0;mtl_pool_add(raw,page);mtl_pool_add((char *)raw+page,page);
    op_hdr end={OP_END,8};mtl_sub_begin();mtl_exec((const uint8_t *)&end);
    (void)gmem_map_gpu(1);
    if(g_pool_res_n)MDIE("unaddressed raw pools were made resident");checked++;
    if(negative!=1)mtl_pool_addressed(&owner_a,raw,page);
    (void)gmem_map_gpu(1);
    if(g_pool_res_n!=1)MDIE("issued raw address missing from residency");checked++;
    uint64_t old=g_gmem_buf[1].gpuAddress;
    mtl_pool_addressed(&owner_b,(char *)raw+page,page);(void)gmem_map_gpu(1);
    if(g_pool_res_n!=2||g_gmem_buf[1].gpuAddress==old)MDIE("issued address did not invalidate raw table");checked++;
    // A wrapper replacement is not a new NVN owner (virtual remap).
    mtl_pool_del(raw);mtl_pool_add(raw,page);(void)gmem_map_gpu(0);
    if(!pidx_find(&g_pools,(uintptr_t)raw,1)->res)MDIE("remapping lost the issued address");checked++;
    mtl_pool_forget_address(&owner_a);mtl_pool_del(raw);mtl_pool_add(raw,page);(void)gmem_map_gpu(1);
    if(pidx_find(&g_pools,(uintptr_t)raw,1)->res)MDIE("reused address inherited an expired owner");checked++;
    // Overlapping owners retire independently.
    mtl_pool_addressed(&owner_a,raw,page*2);mtl_pool_forget_address(&owner_b);(void)gmem_map_gpu(1);
    if(!pidx_overlaps(&g_addressed,(uintptr_t)raw+page,(uintptr_t)raw+page*2))MDIE("overlapping owner was erased");checked++;
    mtl_sub_commit();mtl_finish();mtl_pool_forget_address(&owner_a);
    mtl_pool_del(raw);mtl_pool_del((char *)raw+page);free(raw);

    tex_t t[3]={0};pool_t pools[3]={0};void *mem[3]={0};size_t sizes[3]={0};
    for(int i=0;i<3;i++){
        t[i].serial=UINT64_C(0xfffdd100)+i;
        t[i].d=(nvn_texdesc){.width=16,.height=16,.depth=1,.levels=1,.target=1,.format=0x25,.swizzle_r=2,.swizzle_g=3,.swizzle_b=4,.swizzle_a=5};
        nvn_tex_layout(&t[i].d,&t[i].L);sizes[i]=(t[i].L.size+page-1)&~(page-1);
        if(posix_memalign(&mem[i],page,sizes[i]))MDIE("demand texture memory");memset(mem[i],37+i*50,sizes[i]);
        pools[i]=(pool_t){.mem=mem[i],.size=sizes[i],.flags=0x24};t[i].mem=mem[i];t[i].pool=&pools[i];
        mtl_pool_add(mem[i],sizes[i]);nvn_dirty_add_range(mem[i],sizes[i]);mtl_reg_tex(i+1,&t[i],NULL);
    }
    size_t handle_size=5767200,handle_span=(handle_size+page-1)&~(page-1);
    void *handles=NULL;if(posix_memalign(&handles,page,handle_span))MDIE("demand handles");memset(handles,0,handle_span);mtl_pool_add(handles,handle_span);nvn_dirty_add_range(handles,handle_span);
    void *uniform=NULL;if(posix_memalign(&uniform,page,page))MDIE("demand uniform");memset(uniform,0,page);mtl_pool_add(uniform,page);
    NvShader *sh=[NvShader new];sh->stage=5;sh->rf.nb=3;
    sh->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.reg=0,.aid=0};
    sh->rf.b[1]=(nvn_rbind){.kind=NVN_RB_TEXP,.dtt=MTLTextureType2D,.aid=1,.pool_sources=1};
    sh->rf.b[2]=(nvn_rbind){.kind=NVN_RB_CBUF,.reg=0,.aid=3};
    NSString *proof_source=@"struct Words { uint _m0[1]; };\nstruct Uniform { float4 _m0[1]; };\nstruct Args { constant Pool* textures; };\nuint _10 = as_type<uint>((*spvDescriptorSet0.cb)._m0[0].x);\nfloat _1 = as_type<float>((*spvDescriptorSet0.data)._m0[_10]);\nint _2 = as_type<int>(_1) & 1048575;\nfloat x = (&spvDescriptorSet0.textures->e[0])[min(uint(_2), spvDescriptorSet0.textures->n)].read(uint2(0)).x;";
    sh->pool_reads=nvn_pool_sources(&sh->rf,@[@{@"msl_name":@"data",@"msl_type":@"const device Words*"},@{@"msl_name":@"textures"},@{@"msl_name":@"cb",@"msl_type":@"constant Uniform*"}],proof_source);
    NSDictionary *exact_reads=sh->pool_reads;
    if(![sh->pool_reads[@1][@0] count])MDIE("exact descriptor index proof missing");checked++;
    NSError *error=nil;NSString *src=@"#include <metal_stdlib>\nusing namespace metal;\nstruct Pool { uint n; uint pad; texture2d<float,access::read> t[5]; };\nkernel void k(device const uint *h [[buffer(0)]],device const Pool &p [[buffer(1)]],device uchar4 *o [[buffer(2)]],device const uint *index [[buffer(3)]]) { o[0]=uchar4(round(p.t[h[index[0]]&1048575u].read(uint2(0))*255.0f)); }";
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:src options:nil error:&error];
    id<MTLComputePipelineState> ps=lib?[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k"] error:&error]:nil;
    if(!ps)MDIE("demand probe pipeline: %s",error.localizedDescription.UTF8String);
    id<MTLBuffer> out=probe_buf(256),writer=probe_buf(256),zero=probe_buf(256);memset(zero.contents,0,256);
    for(int step=0;step<5;step++){
        *(uint32_t *)handles=1;((uint32_t *)handles)[37]=0;mtl_sub_begin();
        if(!step){
            ((uint32_t *)handles)[0]=3;((uint32_t *)handles)[37]=1;*(uint32_t *)uniform=37;
            uint64_t commands[16]={0},*at=commands;size_t bytes=usp_bind((uint8_t *)at,5,0,(uintptr_t)uniform,16);memcpy((uint8_t *)at+bytes,&end,8);
            uint64_t handle=(uintptr_t)commands;mtl_sub_snapshot_ubos(&handle,1);usnap_wait_copies();mtl_exec((const uint8_t *)commands);
            *(uint32_t *)uniform=0; // guest rewrites after submit: draw still uses 37
        }else {sh->pool_reads=nil;mtl_exec((const uint8_t *)&end);}
        S.tex_base=0;S.tex_pool_n=4;S.ssbo[5][0]=(p_bind_buf){.addr=(uintptr_t)handles,.size=handle_size};
        void *hb=NULL;uint64_t ho=0;mtl_addr((uintptr_t)handles,4,&hb,&ho);
        if(step==1){
            *(uint32_t *)writer.contents=2;enc_b_copy_buffer(blit(),writer,0,(__bridge id<MTLBuffer>)hb,ho,4);
            if(negative!=3)usnap_drop((uintptr_t)handles,4);
        }
        if(step==2){uint32_t value=3;mtl_sub_update(&value,(uintptr_t)handles,4);}
        if(step==3){sh->rf.b[1].pool_sources=0;*(uint32_t *)handles=3;}
        if(step==4){
            sh->rf.b[1].pool_sources=1;sh->pool_reads=exact_reads;((uint32_t *)handles)[37]=3;*(uint32_t *)uniform=0;*(uint32_t *)writer.contents=37;
            S.ubo[5][0]=(p_bind_buf){.addr=(uintptr_t)uniform,.size=16};void *ub=NULL;uint64_t offset=0;mtl_addr((uintptr_t)uniform,16,&ub,&offset);
            enc_b_copy_buffer(blit(),writer,0,(__bridge id<MTLBuffer>)ub,offset,4);if(negative!=5)usnap_drop((uintptr_t)uniform,4);
        }
        prepare_shader_pools(sh);
        if(step==4){if(!(sh->pool_capture_mask&4)||*(const uint32_t *)sh->pool_capture_cpu[2]!=37)MDIE("GPU-produced uniform snapshot stale");checked++;*(uint32_t *)uniform=0;}
        if(!step){if(!g_treg[1].view||g_treg[2].view||g_treg[3].view)MDIE("demand cold draw eagerly prepared unused descriptors");checked++;
            if(!g_pool_demand_exact||!(sh->pool_capture_mask&4))MDIE("exact index did not capture its uniform");checked++;}
        if(step==1){if(!g_treg[2].view||!g_pool_demand_waits)MDIE("first GPU-produced handle was not prepared");checked++;}
        if(step>=2){if(!g_treg[3].view)MDIE("unknown consumer omitted a descriptor");checked++;}
        registry_lock();uint64_t table=descriptor_pool_gpu_need(&sh->rf.b[1],pool_demand_for(sh,&sh->rf.b[1]));registry_unlock();
        uint64_t input=sh->pool_capture_mask?sh->pool_capture_gpu[0]:((__bridge id<MTLBuffer>)hb).gpuAddress+ho;
        if(!step){
            pool_input *cached=pool_input_get(&S.ssbo[5][0],4);uint64_t before=cached->gpu;
            if(before!=input)MDIE("large descriptor snapshot failed reuse");checked++;
            ((uint32_t *)handles)[37]=2;cached=pool_input_get(&S.ssbo[5][0],4);
            if(cached->gpu==before||!(((const uint64_t *)cached->ids.bytes)[0]&4))MDIE("CPU write kept a stale descriptor snapshot");checked++;
            before=cached->gpu;tex_t host_write={0};host_write.mem=handles;host_write.L.size=handle_size;
            if(!wb_target_lock(&host_write))MDIE("descriptor writeback fixture lost its pool");
            void *token=nvn_dirty_gpu_write_begin(handles,handle_size);((uint32_t *)handles)[37]=3;nvn_dirty_gpu_write_end(token);pthread_mutex_unlock(&g_mu);
            cached=pool_input_get(&S.ssbo[5][0],4);
            if(cached->gpu==before||cached->cpu[37]!=3)MDIE("host GPU writeback kept a stale descriptor snapshot");checked++;
            if(negative==2)input=((__bridge id<MTLBuffer>)hb).gpuAddress+ho;}
        rp_end();blit_close();id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,ps);
        enc_c_args(ce,input,0);enc_c_args(ce,table,1);enc_c_args(ce,out.gpuAddress,2);
        uint64_t index_gpu=(!step||step==4)?sh->pool_capture_gpu[2]:zero.gpuAddress;
        if(!step&&negative==4){void *ub=NULL;uint64_t offset=0;mtl_addr((uintptr_t)uniform,16,&ub,&offset);index_gpu=((__bridge id<MTLBuffer>)ub).gpuAddress+offset;}
        enc_c_args(ce,index_gpu,3);
        [ce dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];enc_c_end(ce);mtl_sub_commit();mtl_finish();
        for(int c=0;c<4;c++){if(((uint8_t *)out.contents)[c]!=37+MIN(step,2)*50)MDIE("demand pixel step%d byte%d: %u",step,c,((uint8_t *)out.contents)[c]);checked++;}
        for(int i=0;i<3;i++){if(pidx_find(&g_pools,(uintptr_t)mem[i],1)->res)MDIE("texture preparation pinned its full source pool");checked++;}
    }
    /* Repeated draws use the same storage capture but different uniform
       indices. Check real GPU pixels, hit counts and submission/size misses. */
    {
        *(uint32_t *)handles=1;((uint32_t *)handles)[37]=2;
        sh->pool_reads=exact_reads;sh->rf.b[1].pool_sources=1;
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        S.tex_base=0;S.tex_pool_n=4;
        S.ssbo[5][0]=(p_bind_buf){.addr=(uintptr_t)handles,.size=handle_size};
        S.ubo[5][0]=(p_bind_buf){.addr=(uintptr_t)uniform,.size=16};
        g_pool_wait_probe_negative=negative;
        for(int draw=0;draw<6;draw++){
            if(draw==5){mtl_sub_begin();mtl_exec((const uint8_t *)&end);S.tex_pool_n=3;S.ssbo[5][0]=(p_bind_buf){.addr=(uintptr_t)handles,.size=handle_size};S.ubo[5][0]=(p_bind_buf){.addr=(uintptr_t)uniform,.size=16};}
            if(draw==4)S.tex_pool_n=3;
            uint32_t index=draw<2?37:0;mtl_sub_update(&index,(uintptr_t)uniform,4);
            if(negative==10&&draw==2){
                // Deliberately erase the prior uniform value from the key.
                pool_memo_uniform wrong={2,0,0};for(int w=0;w<POOL_MEMO_WAYS;w++)if(sh->pool_memo[1]->ent[w].valid)sh->pool_memo[1]->uniforms[w]=[NSData dataWithBytes:&wrong length:sizeof wrong];
            }
            uint64_t hits=g_pool_demand_memo;
            prepare_shader_pools(sh);
            if(negative!=10&&pool_memo_on()&&g_pool_demand_memo!=hits+(draw==1||draw==3||draw==5))MDIE("memo hit/miss draw%d",draw);checked++;   // draw 5: a new submission, same inputs and uniform (memo valid across submissions, 2026-11-08)
            if(!(sh->pool_capture_mask&4))MDIE("memo omitted uniform capture draw%d",draw);checked++;
            const uint64_t *need=pool_demand_for(sh,&sh->rf.b[1]);
            if(!need||!(need[0]&(UINT64_C(1)<<(draw<2?2:1))))MDIE("memo omitted required descriptor draw%d",draw);checked++;
            registry_lock();uint64_t table=descriptor_pool_gpu_need(&sh->rf.b[1],pool_demand_for(sh,&sh->rf.b[1]));registry_unlock();
            rp_end();blit_close();id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,ps);
            enc_c_args(ce,sh->pool_capture_gpu[0],0);enc_c_args(ce,table,1);
            enc_c_args(ce,out.gpuAddress+draw*4,2);enc_c_args(ce,sh->pool_capture_gpu[2],3);
            [ce dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];enc_c_end(ce);
            if(draw==4||draw==5){mtl_sub_commit();mtl_finish();}
        }
        for(int draw=0;draw<6;draw++)for(int c=0;c<4;c++){
            if(((uint8_t *)out.contents)[draw*4+c]!=(draw<2?87:37))MDIE("memo pixel draw%d byte%d: %u",draw,c,((uint8_t *)out.contents)[draw*4+c]);checked++;
        }
        g_pool_wait_probe_negative=0;
    }
    /* Wait for the actual prior writer, while an unrelated later submission
       remains blocked. Whole-current-chunk waiting cannot pass this check.
       The captured bytes are also consumed by the real GPU descriptor read. */
    {
        int previous=atomic_exchange(&g_pool_wait_precise,negative==7?0:1);
        g_pool_wait_probe_negative=negative;
        *(uint32_t *)handles=1;*(uint32_t *)writer.contents=2;
        nvn_dirty_protect(handles,handle_size);
        id<MTLSharedEvent> producer_hold=[g_dev newSharedEvent],unrelated_hold=[g_dev newSharedEvent];
        producer_hold.signaledValue=0;unrelated_hold.signaledValue=0;
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        void *hb=NULL;uint64_t ho=0;mtl_addr((uintptr_t)handles,4,&hb,&ho);
        enc_b_copy_buffer(blit(),writer,0,(__bridge id<MTLBuffer>)hb,ho,4);usnap_drop((uintptr_t)handles,4);
        [g_q4 waitForEvent:producer_hold value:1];uint64_t producer=mtl_sub_commit();
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        enc_b_copy_buffer(blit(),writer,0,out,128,4); // a real unrelated GPU command, not an empty-buffer callback
        [g_q4 waitForEvent:unrelated_hold value:1];uint64_t unrelated=mtl_sub_commit();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{producer_hold.signaledValue=1;});
        // Always release a broken full-wait implementation without hanging.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{unrelated_hold.signaledValue=1;});
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        S.tex_base=0;S.tex_pool_n=4;S.ssbo[5][0]=(p_bind_buf){.addr=(uintptr_t)handles,.size=handle_size};
        sh->pool_reads=nil;sh->rf.b[1].pool_sources=1;
        uint64_t prior=g_pool_wait_prior,current=g_pool_wait_current,ns0=g_pool_wait_ns;
        uint64_t needed=pool_range_write_fence(&S.ssbo[5][0]);
        if(!needed)MDIE("prior-writer dependency was lost");checked++;
        pool_inputs_wait(needed);
        if(negative!=6){
            if(g_pool_wait_prior!=prior+1||g_pool_wait_current!=current)MDIE("descriptor wait did not target its prior writer");checked++;
            if(g_ev.signaledValue<producer||g_ev.signaledValue>=unrelated||unrelated_hold.signaledValue)MDIE("descriptor wait consumed unrelated later work producer=%llu unrelated=%llu GPU-done=%llu CPU-done=%llu released=%llu wait-ms=%.3f",producer,unrelated,g_ev.signaledValue,g_done,unrelated_hold.signaledValue,(double)(g_pool_wait_ns-ns0)/1e6);checked++;
        }
        // Take the same immutable snapshot before releasing the unrelated work.
        // Later texture preparation may itself need independent GPU resources.
        (void)pool_input_get(&S.ssbo[5][0],0);
        producer_hold.signaledValue=1;unrelated_hold.signaledValue=1;
        prepare_shader_pools(sh);
        uint64_t captured=sh->pool_capture_gpu[0];
        if(!captured)MDIE("prior-writer input was not captured");checked++;
        registry_lock();uint64_t table=descriptor_pool_gpu_need(&sh->rf.b[1],pool_demand_for(sh,&sh->rf.b[1]));registry_unlock();
        producer_hold.signaledValue=1;unrelated_hold.signaledValue=1;
        rp_end();blit_close();id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,ps);
        enc_c_args(ce,captured,0);enc_c_args(ce,table,1);enc_c_args(ce,out.gpuAddress,2);enc_c_args(ce,zero.gpuAddress,3);
        [ce dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];enc_c_end(ce);mtl_sub_commit();mtl_finish();
        for(int c=0;c<4;c++){if(((uint8_t *)out.contents)[c]!=87)MDIE("prior-writer descriptor pixel byte%d: %u",c,((uint8_t *)out.contents)[c]);checked++;}
        g_pool_wait_probe_negative=0;atomic_store(&g_pool_wait_precise,previous);
    }
    /* Same logical submission: writer completes in its first chunk while
       unrelated later commands remain blocked. Preserve exact GPU bytes. */
    {
        int old=atomic_exchange(&g_writer_boundary,1),precise=atomic_exchange(&g_pool_wait_precise,1);
        g_pool_wait_probe_negative=negative;
        *(uint32_t *)handles=1;*(uint32_t *)writer.contents=2;nvn_dirty_protect(handles,handle_size);
        id<MTLSharedEvent> first=[g_dev newSharedEvent],later=[g_dev newSharedEvent];
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        void *hb=NULL;uint64_t ho=0;mtl_addr((uintptr_t)handles,4,&hb,&ho);
        enc_b_copy_buffer(blit(),writer,0,(__bridge id<MTLBuffer>)hb,ho,4);usnap_drop((uintptr_t)handles,4);
        [g_q4 waitForEvent:first value:1];g_raw_writer_needs_seal=1;writer_boundary_seal();
        if(!g_writer_ticket)MDIE("writer boundary had no ticket");checked++;
        // Rewriting the same range must discard its previous completion ticket.
        usnap_drop((uintptr_t)handles,4);
        int found=0;for(int i=0;i<g_ngw;i++)if(g_gw[i].lo==(uintptr_t)handles&&g_gw[i].fence==g_fence+1){if(g_gw[i].ready_ticket)MDIE("later duplicate write kept an old ticket");found++;}
        if(!found)MDIE("writer-boundary fixture lost its write");checked++;
        // Seal the real second write, then block only later unrelated work.
        enc_b_copy_buffer(blit(),writer,0,(__bridge id<MTLBuffer>)hb,ho,4);g_raw_writer_needs_seal=1;writer_boundary_seal();
        [g_q4 waitForEvent:later value:1];enc_b_copy_buffer(blit(),writer,0,out,128,4);uint64_t logical=mtl_sub_commit();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{first.signaledValue=1;});
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_global_queue(QOS_CLASS_DEFAULT,0),^{later.signaledValue=1;});
        mtl_sub_begin();mtl_exec((const uint8_t *)&end);
        S.tex_base=0;S.tex_pool_n=4;S.ssbo[5][0]=(p_bind_buf){.addr=(uintptr_t)handles,.size=handle_size};sh->pool_reads=nil;sh->rf.b[1].pool_sources=1;
        uint64_t waits=g_writer_waits;if(negative==8)atomic_store(&g_writer_boundary,0);
        pool_inputs_wait_range(&S.ssbo[5][0]);
        if(negative!=9){if(g_writer_waits!=waits+1||g_ev.signaledValue>=logical||later.signaledValue)MDIE("writer boundary waited for unrelated commands");checked++;}
        pool_input *captured=pool_input_get(&S.ssbo[5][0],0);uint64_t gpu=captured->gpu;
        first.signaledValue=1;later.signaledValue=1;
        prepare_shader_pools(sh);registry_lock();uint64_t table=descriptor_pool_gpu_need(&sh->rf.b[1],pool_demand_for(sh,&sh->rf.b[1]));registry_unlock();
        rp_end();blit_close();id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,ps);
        enc_c_args(ce,gpu,0);enc_c_args(ce,table,1);enc_c_args(ce,out.gpuAddress,2);enc_c_args(ce,zero.gpuAddress,3);
        [ce dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];enc_c_end(ce);mtl_sub_commit();mtl_finish();
        for(int c=0;c<4;c++){if(((uint8_t *)out.contents)[c]!=87)MDIE("writer-boundary pixel byte%d: %u",c,((uint8_t *)out.contents)[c]);checked++;}
        atomic_store(&g_writer_boundary,old);atomic_store(&g_pool_wait_precise,precise);g_pool_wait_probe_negative=0;
    }
    for(int i=0;i<3;i++){mtl_reg_tex(i+1,NULL,NULL);mtl_tex_release(&t[i]);mtl_pool_del(mem[i]);nvn_dirty_remove_range(mem[i]);free(mem[i]);}
    mtl_pool_del(handles);nvn_dirty_remove_range(handles);free(handles);mtl_pool_del(uniform);free(uniform);res_remove(out);res_remove(writer);res_remove(zero);
    return checked;
}
