/* GPU indirect-argument preparation, before a run of render commands.
 * Reference: seedds/nxnative dm/gfx/nvn/nvn_mtl.m mdx_run (MIT).
 * Both count and firstTask belong to one immutable GPU-produced snapshot.
 * Native ordering is separate from snapshot validity: a barrier does not
 * change bytes. Actual writes, CPU dirtiness and mapping changes invalidate.
 */
#define MESH_BATCH_CAP 256
typedef struct {uint64_t source,grid,stamp,hostwrite;} mesh_batch_entry;
static mesh_batch_entry g_mb[MESH_BATCH_CAP];static unsigned g_mb_n;
static uint64_t g_mb_arena,g_mb_pool,g_mb_runs,g_mb_jobs,g_mb_hits,g_mb_splits,g_mb_invalid,g_mb_fallback;
static int g_mb_negative; // GPU probe only: 1 drops write invalidation, 2 binds the live firstTask.
static _Atomic uint64_t g_mb_verified;
static int mesh_batch_on(void) {
    int v=atomic_load(&g_mesh_batch);
    if(v<0){const char *e=getenv("OOT_MESH_BATCH");v=!TARGET_OS_IPHONE&&e&&atoi(e)!=0;atomic_store(&g_mesh_batch,v);}
    return v&&!TARGET_OS_IPHONE&&g_oot_bar_mode==1;
}
static void mesh_batch_write(uint64_t lo,uint64_t size) {
    if(g_mb_negative==1||!g_mb_n)return;
    if(!size||size>UINT64_MAX-lo){g_mb_invalid+=g_mb_n;g_mb_n=0;return;}
    unsigned kept=0;for(unsigned i=0;i<g_mb_n;i++){
        mesh_batch_entry e=g_mb[i];if(e.source<lo+size&&lo<e.source+8){g_mb_invalid++;continue;}g_mb[kept++]=e;
    }g_mb_n=kept;
}
static void mesh_batch_context(void) {
    uint64_t pool=atomic_load_explicit(&g_pool_gen,memory_order_acquire);
    if(!mesh_batch_on()||g_mb_arena!=g_arena_epoch||g_mb_pool!=pool||totk_vpool_aliases_used()){
        g_mb_n=0;g_mb_arena=g_arena_epoch;g_mb_pool=pool;
    }
    if(atomic_exchange(&g_mesh_batch_dump,0))MLOG("[mesh-batch] F%llu enabled=%d native=%d runs=%llu jobs=%llu hits=%llu splits=%llu invalid=%llu fallback=%llu active=%u verified=%llu",(unsigned long long)atomic_load(&g_cur_frame),mesh_batch_on(),g_oot_bar_mode,(unsigned long long)g_mb_runs,(unsigned long long)g_mb_jobs,(unsigned long long)g_mb_hits,(unsigned long long)g_mb_splits,(unsigned long long)g_mb_invalid,(unsigned long long)g_mb_fallback,g_mb_n,(unsigned long long)atomic_load(&g_mb_verified));
}
static mesh_batch_entry *mesh_batch_find(uint64_t source) {
    for(unsigned i=0;i<g_mb_n;i++)if(g_mb[i].source==source){
        mesh_batch_entry *e=&g_mb[i];
        if(e->grid&&e->hostwrite==atomic_load_explicit(&g_pool_input_hostwrite,memory_order_acquire)&&nvn_dirty_tracked((void *)(uintptr_t)source,8)&&!nvn_dirty_since((void *)(uintptr_t)source,8,e->stamp))return e;
        e->grid=0;return NULL;
    }return NULL;
}
static int mesh_batch_eligible(uint64_t source) {
    return source&&source<=UINT64_MAX-8&&!totk_vpool_aliases_used()&&nvn_dirty_tracked((void *)(uintptr_t)source,8)&&mtl_addr_known(source,8);
}
static void mesh_batch_written(NvProg *pr) {
    if(!g_mb_n)return;
    if(!pr){mesh_batch_write(0,UINT64_MAX);return;}
    if(R.has_rt){for(int i=0;i<R.rt.n;i++)if(R.rt.color[i])mesh_batch_write((uintptr_t)R.rt.color[i]->mem,R.rt.color[i]->L.size);if(R.rt.depth)mesh_batch_write((uintptr_t)R.rt.depth->mem,R.rt.depth->L.size);}
    // Unknown shader addresses and image aliases invalidate the whole batch.
    // This deliberately keeps the existing conservative reflection contract.
    for(int st=0;st<6;st++)if(pool_shader_writes(pr->sh[st])){mesh_batch_write(0,UINT64_MAX);return;}
}
static void mesh_batch_op(unsigned op,const void *a) {
    if(!g_mb_n)return;
    if(op==OP_UPDATE_UB||op==OP_CLEAR_BUF){uint64_t lo,n;memcpy(&lo,a,8);memcpy(&n,(const uint8_t *)a+8,8);mesh_batch_write(lo,n);}
    else if(op==OP_COPY_B2B){uint64_t lo,n;memcpy(&lo,(const uint8_t *)a+8,8);memcpy(&n,(const uint8_t *)a+16,8);mesh_batch_write(lo,n);}
    else if(op==OP_DISPATCH||op==OP_COPY_T2B||op==OP_COPY_B2T||op==OP_COPY_T2T||op==OP_CLEAR_TEX||op==OP_CLEAR_COLOR||op==OP_CLEAR_DS||op==OP_FENCE_SYNC)mesh_batch_write(0,UINT64_MAX);
}
static id<MTLComputePipelineState> g_mb_ps;static id<MTL4ArgumentTable> g_mb_at;
static void mesh_batch_pipeline(void) {
    if(g_mb_ps)return;
    NSString *source=@"#include <metal_stdlib>\nusing namespace metal;struct Job{ulong source;ulong output;};kernel void mesh_grid_batch(const device Job*jobs[[buffer(0)]],uint i[[thread_position_in_grid]]){Job j=jobs[i];if(!j.source)return;const device uint*r=reinterpret_cast<const device uint*>(j.source);uint count=r[0],first=r[1];*reinterpret_cast<device uint4*>(j.output)=uint4(count,1,1,first);}";
    NSError *error=nil;id<MTLLibrary> lib=totk_msl_library(source,&error);if(!lib)MDIE("mesh batch library: %s",error.localizedDescription.UTF8String);
    g_mb_ps=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"mesh_grid_batch"]error:&error];
    MTL4ArgumentTableDescriptor *d=[MTL4ArgumentTableDescriptor new];d.maxBufferBindCount=1;g_mb_at=[g_dev newArgumentTableWithDescriptor:d error:&error];
    if(!g_mb_ps||!g_mb_at)MDIE("mesh batch pipeline: %s",error.localizedDescription.UTF8String);
    MLOG("[mesh-batch] created mesh_grid_batch; immutable count/firstTask; native barrier policy required");
}
static void mesh_batch_add(uint64_t source,uint64_t *sources,unsigned *n) {
    if(*n==MESH_BATCH_CAP||!mesh_batch_eligible(source)||mesh_batch_find(source))return;
    for(unsigned i=0;i<*n;i++)if(sources[i]==source)return;
    sources[(*n)++]=source;
}
static void mesh_batch_prepare(uint64_t current) {
    uint64_t sources[MESH_BATCH_CAP];unsigned n=0;mesh_batch_add(current,sources,&n);
    if(g_exec_next&&g_ret_n<=8){fz_cursor c={.p=g_exec_next,.ret_n=g_ret_n};memcpy(c.ret,g_ret,(size_t)g_ret_n*sizeof *g_ret);
        for(unsigned guard=0;guard<200000&&n<MESH_BATCH_CAP;guard++){
            const uint8_t *p;op_hdr h;const void *a;if(!fz_next(&c,&p,&h,&a))break;
            if(h.size<8||(h.size&7)||h.op>=OP_COUNT_)break;
            if(h.op==OP_DISPATCH||h.op==OP_COPY_B2B||h.op==OP_CLEAR_BUF||h.op==OP_COPY_T2B||h.op==OP_COPY_B2T||h.op==OP_COPY_T2T||h.op==OP_CLEAR_TEX||h.op==OP_FENCE_SYNC)break;
            if(h.op==OP_UPDATE_UB){uint64_t lo,size;memcpy(&lo,a,8);memcpy(&size,(const uint8_t *)a+8,8);int overlap=!size||size>UINT64_MAX-lo;for(unsigned k=0;k<n&&!overlap;k++)overlap=sources[k]<lo+size&&lo<sources[k]+8;if(overlap)break;}
            if(h.op==OP_DRAW&&h.size>=8+sizeof(p_draw)){const p_draw *draw=a;if(draw->prim==0x40&&draw->ind)mesh_batch_add(draw->ind,sources,&n);}
        }
    }
    if(!n)return;mesh_batch_pipeline();
    if(g_mb_n+n>MESH_BATCH_CAP){g_mb_n=0;n=1;sources[0]=current;}
    if(g_enc){g_mb_splits++;rp_end();}blit_close();
    typedef struct {uint64_t source,output;} job;
    unsigned padded=(n+63u)&~63u;id<MTLBuffer> buffer;size_t at=arena_alloc((size_t)padded*sizeof(job)+(size_t)n*16,&buffer);
    job *jobs=(job *)((uint8_t *)buffer.contents+at);memset(jobs,0,(size_t)padded*sizeof(job));uint64_t out=buffer.gpuAddress+at+(uint64_t)padded*sizeof(job);unsigned issued=0;
    for(unsigned k=0;k<n;k++){
        uint64_t address=sources[k],stamp=nvn_dirty_now();nvn_dirty_protect((void *)(uintptr_t)address,8);upd_guard(address,address+8);
        void *source;uint64_t offset,gpu;if(!mtl_gpu_addr(address,8,&source,&offset,&gpu))continue;
        jobs[k]=(job){gpu,out+(uint64_t)k*16};unsigned slot=g_mb_n;for(unsigned j=0;j<g_mb_n;j++)if(g_mb[j].source==address){slot=j;break;}
        if(slot==g_mb_n)g_mb_n++;g_mb[slot]=(mesh_batch_entry){address,jobs[k].output,stamp,atomic_load_explicit(&g_pool_input_hostwrite,memory_order_acquire)};issued++;
    }
    blit_close();id<MTL4ComputeCommandEncoder> encoder=enc_c_begin(NULL);[encoder setLabel:@"OOT mesh_grid_batch"];
    [g_mb_at setAddress:buffer.gpuAddress+at atIndex:0];[encoder setArgumentTable:g_mb_at];[encoder setComputePipelineState:g_mb_ps];
    [encoder dispatchThreadgroups:MTLSizeMake(padded/64,1,1)threadsPerThreadgroup:MTLSizeMake(64,1,1)];enc_c_end(encoder);args_dirty_all();g_mb_runs++;g_mb_jobs+=issued;
}
static void mesh_batch_verify(uint64_t source,uint64_t grid,uint64_t first) {
    if(!atomic_load(&g_mesh_batch_check))return;
    void *live;uint64_t offset,gpu;if(!mtl_gpu_addr(source,8,&live,&offset,&gpu))MDIE("mesh batch verification lost live input");
    for(NvArena *a in g_sub_arenas)if(grid>=a->gpu&&grid-a->gpu<=a->used&&16<=a->used-(grid-a->gpu)){
        id<MTLBuffer> capture;size_t at=arena_alloc(32,&capture);size_t go=(size_t)(grid-a->gpu);
        enc_b_copy_buffer(blit(),(__bridge id<MTLBuffer>)live,offset,capture,at,8);
        enc_b_copy_buffer(blit(),a->buf,go,capture,at+8,16);
        if(first==grid+12)enc_b_copy_buffer(blit(),a->buf,go+12,capture,at+24,4);
        else if(first==gpu+4)enc_b_copy_buffer(blit(),(__bridge id<MTLBuffer>)live,offset+4,capture,at+24,4);
        else MDIE("mesh batch verification lost bound firstTask");
        uint64_t frame=atomic_load(&g_cur_frame);
        [g_commit_blocks addObject:^{const uint32_t *v=(const uint32_t *)((const uint8_t *)capture.contents+at);
            if(v[0]!=v[2]||v[1]!=v[5]||v[1]!=v[6]||v[3]!=1||v[4]!=1)MDIE("mesh batch GPU input mismatch F%llu source=%#llx live=%u/%u grid=%u/%u/%u/%u bound-first=%u",(unsigned long long)frame,(unsigned long long)source,v[0],v[1],v[2],v[3],v[4],v[5],v[6]);atomic_fetch_add(&g_mb_verified,1);
        }];return;
    }MDIE("mesh batch verification lost snapshot arena");
}
static uint64_t mesh_batch_bound(mesh_batch_entry *e,uint64_t *first) {
    uint64_t grid=e->grid,source=e->source;*first=grid+12;
    if(g_mb_negative==2){void *b;uint64_t off,gpu;if(mtl_gpu_addr(source,8,&b,&off,&gpu))*first=gpu+4;}
    mesh_batch_verify(source,grid,*first);return grid;
}
static uint64_t mesh_batch_grid(uint64_t source,uint64_t *first) {
    mesh_batch_context();if(!mesh_batch_on()||!mesh_batch_eligible(source)){g_mb_fallback++;return 0;}
    mesh_batch_entry *e=mesh_batch_find(source);if(e){g_mb_hits++;return mesh_batch_bound(e,first);}
    mesh_batch_prepare(source);e=mesh_batch_find(source);if(!e){g_mb_fallback++;return 0;}
    return mesh_batch_bound(e,first);
}
