/* GPU indirect-argument preparation, before a run of render commands.
 * Reference: seedds/nxnative dm/gfx/nvn/nvn_mtl.m mdx_run (MIT).
 * Both count and firstTask belong to one immutable GPU-produced snapshot.
 * Native ordering is separate from snapshot validity: a barrier does not
 * change bytes. Actual writes, CPU dirtiness and mapping changes invalidate.
 *
 * Inline updates seen ahead (2026-11-08): the game writes most indirect arguments with an inline uniform update right
 * before the draw that reads them. The look-ahead used to stop at an update of a collected source and the update's
 * replay invalidated every entry it touched, so the draw after it re-batched inside its render pass (ct3: 3.7 of the
 * 5.3 batches a frame split an open pass; every big 1536x864 pass started behind a compute encoder, which kept it
 * from overlapping the previous pass). Now an update seen in the window that covers a source's whole 8 bytes supplies
 * the value: the grid is written on the CPU from the update's own bytes (the kernel skips the job), the entry remembers
 * the update (applied), and the update's replay keeps it (an entry still holding an older value gets the new one as
 * "next" and switches when the update replays). A partial overlap or a second update of one source keeps the old
 * behaviour (that source waits for its draw). */
#define MESH_BATCH_CAP 256
#define MESH_BATCH_UPD_CAP 1024   // inline updates a look-ahead window may pass (mb3: 477 windows a game ended at 128 before reaching the mesh draws behind them)
typedef struct {uint64_t source,grid,stamp,hostwrite;const void *applied;uint64_t next_grid;const void *next_applied;uint32_t cnt,first,next_cnt,next_first;uint8_t cpu;} mesh_batch_entry;
static mesh_batch_entry g_mb[MESH_BATCH_CAP];static unsigned g_mb_n;
static uint64_t g_mb_arena,g_mb_pool,g_mb_runs,g_mb_jobs,g_mb_hits,g_mb_splits,g_mb_invalid,g_mb_fallback;
static uint64_t g_mb_cpu,g_mb_promoted,g_mb_kept_upd,g_mb_partial,g_mb_second,g_mb_updfull;   // pre-applied updates: jobs filled on the CPU, entries switched / kept at the update's replay, sources left to their draw
static uint64_t g_mb_inv_upd,g_mb_inv_op,g_mb_inv_post,g_mb_inv_written,g_mb_inv_dirty,g_mb_last_report;      // why entries went away: inline update, copy / clear / dispatch op, event or counter write, shader or target write, CPU dirtiness or write-back
static int g_mb_negative; // GPU probe only: 1 drops write invalidation, 2 binds the live firstTask.
static _Atomic uint64_t g_mb_verified;
static int mesh_batch_on(void) {
    int v=atomic_load(&g_mesh_batch);
    if(v<0){const char *e=getenv("OOT_MESH_BATCH");v=!TARGET_OS_IPHONE&&!(e&&!atoi(e));atomic_store(&g_mesh_batch,v); /* Mac default on since 2026-11-08; OOT_MESH_BATCH=0 turns it off */ }
    return v&&!TARGET_OS_IPHONE&&g_oot_bar_mode==1;
}
/* A write of [lo, lo+size) reaches the GPU timeline: entries overlapping it go, except the ones whose value is this very
   update (a == the update's payload; pre-applied in mesh_batch_prepare): those stay, or switch to their next value. */
static void mesh_batch_write_why(uint64_t lo,uint64_t size,const void *a,uint64_t *why) {
    if(g_mb_negative==1||!g_mb_n)return;
    if(!size||size>UINT64_MAX-lo){g_mb_invalid+=g_mb_n;*why+=g_mb_n;g_mb_n=0;return;}
    unsigned kept=0;for(unsigned i=0;i<g_mb_n;i++){
        mesh_batch_entry e=g_mb[i];
        if(e.source<lo+size&&lo<e.source+8){
            if(a&&e.next_applied==a){e.grid=e.next_grid;e.cnt=e.next_cnt;e.first=e.next_first;e.cpu=1;e.applied=a;e.next_applied=NULL;g_mb_promoted++;g_mb[kept++]=e;continue;}
            if(a&&e.applied==a){g_mb_kept_upd++;g_mb[kept++]=e;continue;}
            g_mb_invalid++;(*why)++;continue;
        }
        g_mb[kept++]=e;
    }g_mb_n=kept;
}
static void mesh_batch_write(uint64_t lo,uint64_t size){mesh_batch_write_why(lo,size,NULL,&g_mb_inv_post);}   // post() in nvn_mtl.m: event and counter writes
/* Precise invalidation (2026-11-08 night). mb1 (MetalFX default): 4.3 of 4.9 batches a frame split an open render pass,
   and of the 38 invalidations a frame none came from inline updates: 20.6 from mesh_batch_op dropping the whole batch at
   every dispatch / texture copy / clear and 17.9 from mesh_batch_written dropping it for any draw whose program has a
   storage, raw-memory or image binding (pool_shader_writes counts read-only images too). What can write a source:
   a writable storage buffer binding (its bound range, S.ssbo), a writable image (the texture's memory), raw stores
   (gmem_log: no known range -> whole batch), storage through uniform-held pointers (unknown -> whole batch), copies and
   clears (their destination's memory), render targets, event / counter writes (post), CPU writes (dirty pages). Reads
   never invalidate. The same rules as the native draw barriers' write test (oot_bar_shader_writes: rw), by range. */
static uint64_t g_mb_wr_rt,g_mb_wr_sbuf,g_mb_wr_img,g_mb_wr_whole,g_mb_op_dispatch,g_mb_op_tex,g_mb_op_buf,g_mb_op_fence;
static uint64_t g_mb_prefetch,g_mb_split_inval,g_mb_split_miss;   // batches made before a pass opened; splits whose source had been invalidated / had never been collected
static uint64_t g_mb_end[9];   // why a look-ahead window ended: 0 stream end, 1 call depth, 2 corrupt op, 3 dispatch / copy / clear op, 4 update of a collected source, 5 update table full, 6 source cap or guard, 7 no look-ahead (g_exec_next unset or call depth), 8 an event wait not yet ready
static int g_mb_last_why = -1; static uint64_t g_mb_last_pass;   // passdump: the latest window's end reason and the pass it ran in (why a later draw's source was never collected)
static int mtk_owners(NvShader **ts,NvShader **ms);   // nvn_mesh_tasks.inc (included later): the current mesh draw's task / mesh shaders, 0 for other draws
static int mesh_batch_tex_range(const tex_t *t,uint64_t *lo,uint64_t *n){if(!t||!t->mem||!t->L.size)return 0;*lo=(uintptr_t)t->mem;*n=t->L.size;return 1;}
static void mesh_batch_whole(uint64_t *why){g_mb_wr_whole++;mesh_batch_write_why(0,UINT64_MAX,NULL,why);}
/* The writes of one shader stage, by its bindings. st: the NVN stage whose S.ssbo / S.img tables it is bound through. */
static void mesh_batch_shader_writes(NvShader *sh,int st,uint64_t *why) {
    if(!sh||!g_mb_n)return;
    for(int i=0;i<sh->rf.nb&&g_mb_n;i++){const nvn_rbind *b=&sh->rf.b[i];
        if(b->kind==NVN_RB_GMEML){mesh_batch_whole(why);return;}   // raw stores through pointers the shader loaded: anywhere
        if(!b->rw)continue;
        if(b->kind==NVN_RB_SBUFP&&b->reg<16){const p_bind_buf *u=&S.ssbo[st][b->reg];if(u->addr&&u->size){g_mb_wr_sbuf++;mesh_batch_write_why(u->addr,u->size,NULL,why);continue;}}
        if(b->kind==NVN_RB_IMG&&b->reg<32){int tid=(int)(S.img[st][b->reg]&0xfffff);uint64_t lo,n;tex_t t;int have=0;
            registry_lock();if(tid&&tid<g_ntreg){t=g_treg[tid].t;have=1;}registry_unlock();   // the table moves when it grows (9280ef3f): a copy under the lock
            if(have&&mesh_batch_tex_range(&t,&lo,&n)){g_mb_wr_img++;mesh_batch_write_why(lo,n,NULL,why);continue;}}
        mesh_batch_whole(why);return;   // storage through a uniform-held pointer, an unbound slot or an unknown image: no range
    }
}
static void mesh_batch_context(void) {
    uint64_t pool=atomic_load_explicit(&g_pool_gen,memory_order_acquire);
    if(!mesh_batch_on()||g_mb_arena!=g_arena_epoch||g_mb_pool!=pool||totk_vpool_aliases_used()){
        g_mb_n=0;g_mb_arena=g_arena_epoch;g_mb_pool=pool;
    }
    if(atomic_exchange(&g_mesh_batch_dump,0))MLOG("[mesh-batch] F%llu enabled=%d native=%d runs=%llu jobs=%llu hits=%llu splits=%llu invalid=%llu (update %llu op %llu gpu-write %llu written %llu dirty %llu) fallback=%llu active=%u verified=%llu; pre-applied: cpu-jobs=%llu promoted=%llu kept=%llu partial=%llu second=%llu window-full=%llu",
        (unsigned long long)atomic_load(&g_cur_frame),mesh_batch_on(),g_oot_bar_mode,(unsigned long long)g_mb_runs,(unsigned long long)g_mb_jobs,(unsigned long long)g_mb_hits,(unsigned long long)g_mb_splits,(unsigned long long)g_mb_invalid,
        (unsigned long long)g_mb_inv_upd,(unsigned long long)g_mb_inv_op,(unsigned long long)g_mb_inv_post,(unsigned long long)g_mb_inv_written,(unsigned long long)g_mb_inv_dirty,(unsigned long long)g_mb_fallback,g_mb_n,(unsigned long long)atomic_load(&g_mb_verified),
        (unsigned long long)g_mb_cpu,(unsigned long long)g_mb_promoted,(unsigned long long)g_mb_kept_upd,(unsigned long long)g_mb_partial,(unsigned long long)g_mb_second,(unsigned long long)g_mb_updfull);
    if(atomic_load(&g_mesh_batch_dump)==0&&g_mb_runs&&g_mb_runs%2000==0&&g_mb_runs!=g_mb_last_report){g_mb_last_report=g_mb_runs;
        MLOG("[mesh-batch] writers: targets %llu storage-ranges %llu image-ranges %llu whole %llu; ops: dispatch %llu texture %llu buffer %llu fence %llu (runs %llu splits %llu: invalidated %llu never-collected %llu; prefetched %llu); windows ended: stream-end %llu depth %llu corrupt %llu op %llu overlap %llu updates-full %llu cap %llu none %llu wait %llu",
        (unsigned long long)g_mb_wr_rt,(unsigned long long)g_mb_wr_sbuf,(unsigned long long)g_mb_wr_img,(unsigned long long)g_mb_wr_whole,(unsigned long long)g_mb_op_dispatch,(unsigned long long)g_mb_op_tex,(unsigned long long)g_mb_op_buf,(unsigned long long)g_mb_op_fence,(unsigned long long)g_mb_runs,(unsigned long long)g_mb_splits,
        (unsigned long long)g_mb_split_inval,(unsigned long long)g_mb_split_miss,(unsigned long long)g_mb_prefetch,
        (unsigned long long)g_mb_end[0],(unsigned long long)g_mb_end[1],(unsigned long long)g_mb_end[2],(unsigned long long)g_mb_end[3],(unsigned long long)g_mb_end[4],(unsigned long long)g_mb_end[5],(unsigned long long)g_mb_end[6],(unsigned long long)g_mb_end[7],(unsigned long long)g_mb_end[8]);}
}
static mesh_batch_entry *mesh_batch_find(uint64_t source) {
    for(unsigned i=0;i<g_mb_n;i++)if(g_mb[i].source==source){
        mesh_batch_entry *e=&g_mb[i];
        if(e->grid&&e->hostwrite==atomic_load_explicit(&g_pool_input_hostwrite,memory_order_acquire)&&nvn_dirty_tracked((void *)(uintptr_t)source,8)&&!nvn_dirty_since((void *)(uintptr_t)source,8,e->stamp))return e;
        if(e->grid)g_mb_inv_dirty++;
        e->grid=0;e->next_applied=NULL;return NULL;
    }return NULL;
}
static int mesh_batch_eligible(uint64_t source) {
    return source&&source<=UINT64_MAX-8&&!totk_vpool_aliases_used()&&nvn_dirty_tracked((void *)(uintptr_t)source,8)&&mtl_addr_known(source,8);
}
/* A draw: its render targets and the writes of its stages (the real task / mesh owners too: draw_inner is handed a
   raster-only program for mesh draws, g_mtk holds them, stages 3 and 4). */
static void mesh_batch_written(NvProg *pr) {
    if(!g_mb_n)return;
    if(!pr){mesh_batch_whole(&g_mb_inv_written);return;}
    if(R.has_rt){uint64_t lo,n;
        for(int i=0;i<R.rt.n&&g_mb_n;i++)if(mesh_batch_tex_range(R.rt.color[i],&lo,&n)){g_mb_wr_rt++;mesh_batch_write_why(lo,n,NULL,&g_mb_inv_written);}
        if(g_mb_n&&mesh_batch_tex_range(R.rt.depth,&lo,&n)){g_mb_wr_rt++;mesh_batch_write_why(lo,n,NULL,&g_mb_inv_written);}}
    for(int st=0;st<6&&g_mb_n;st++)mesh_batch_shader_writes(pr->sh[st],st,&g_mb_inv_written);
    NvShader *ts,*ms;if(g_mb_n&&mtk_owners(&ts,&ms)){mesh_batch_shader_writes(ts,3,&g_mb_inv_written);mesh_batch_shader_writes(ms,4,&g_mb_inv_written);}
}
static void mesh_batch_op(unsigned op,const void *a) {
    if(!g_mb_n)return;
    if(op==OP_UPDATE_UB){uint64_t lo,n;memcpy(&lo,a,8);memcpy(&n,(const uint8_t *)a+8,8);mesh_batch_write_why(lo,n,a,&g_mb_inv_upd);}
    else if(op==OP_CLEAR_BUF){g_mb_op_buf++;uint64_t lo,n;memcpy(&lo,a,8);memcpy(&n,(const uint8_t *)a+8,8);mesh_batch_write_why(lo,n,NULL,&g_mb_inv_op);}
    else if(op==OP_COPY_B2B){g_mb_op_buf++;uint64_t lo,n;memcpy(&lo,(const uint8_t *)a+8,8);memcpy(&n,(const uint8_t *)a+16,8);mesh_batch_write_why(lo,n,NULL,&g_mb_inv_op);}
    else if(op==OP_DISPATCH){g_mb_op_dispatch++;const prog_t *cp=S.cprog?S.cprog:S.prog;   // the compute program's writes, by its bindings (stage 5)
        if(!cp){mesh_batch_whole(&g_mb_inv_op);return;}
        g_prog_for_draw=1;NvProg *pr=prog_get(cp);g_prog_for_draw=0;
        if(!pr||pr->missing||!pr->sh[5])mesh_batch_whole(&g_mb_inv_op);else mesh_batch_shader_writes(pr->sh[5],5,&g_mb_inv_op);}
    else if(op==OP_COPY_B2T||op==OP_COPY_T2T||op==OP_CLEAR_TEX){g_mb_op_tex++;const tex_t *t=op==OP_COPY_B2T?((const p_copy_b2t *)a)->tex:op==OP_COPY_T2T?((const p_copy_t2t *)a)->dst:((const p_clear_tex *)a)->tex;uint64_t lo,n;
        if(mesh_batch_tex_range(t,&lo,&n))mesh_batch_write_why(lo,n,NULL,&g_mb_inv_op);else mesh_batch_whole(&g_mb_inv_op);}
    else if(op==OP_CLEAR_COLOR){g_mb_op_tex++;const p_clear_color *c=a;uint64_t lo,n;
        if(R.has_rt&&c->index>=0&&c->index<R.rt.n&&mesh_batch_tex_range(R.rt.color[c->index],&lo,&n))mesh_batch_write_why(lo,n,NULL,&g_mb_inv_op);else mesh_batch_whole(&g_mb_inv_op);}
    else if(op==OP_CLEAR_DS){g_mb_op_tex++;uint64_t lo,n;
        if(R.has_rt&&mesh_batch_tex_range(R.rt.depth,&lo,&n))mesh_batch_write_why(lo,n,NULL,&g_mb_inv_op);else if(!(R.has_rt&&!R.rt.depth))mesh_batch_whole(&g_mb_inv_op);}   // no depth attached: the clear writes nothing
    else if(op==OP_COPY_T2B){g_mb_op_tex++;mesh_batch_whole(&g_mb_inv_op);}   // destination buffer: the bytes depend on the region and format, not computed here
    else if(op==OP_FENCE_SYNC){g_mb_op_fence++;mesh_batch_whole(&g_mb_inv_op);}
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
typedef struct {uint64_t lo,n;const void *a;} mb_upd;                       // an inline update seen in the look-ahead window: range and its op payload
typedef struct {const void *a;uint32_t cnt,first;uint8_t cpu,next;} mb_fill;   // per collected source: the update that supplies its value (cpu), or a GPU read (cpu 0); next: an older value is still valid
/* Collects one mesh draw's source for this batch. Returns 0 when the window must end here. */
static int mesh_batch_collect(uint64_t source,uint64_t *sources,mb_fill *fill,unsigned *n,const mb_upd *upd,unsigned nu) {
    if(*n==MESH_BATCH_CAP||!mesh_batch_eligible(source)){
        PD("Z %llu mb-collect reject src=%#llx cap=%d tracked=%d known=%d aliases=%d",g_stat_pass,(unsigned long long)source,*n==MESH_BATCH_CAP,
           source?nvn_dirty_tracked((void *)(uintptr_t)source,8):-1,source?mtl_addr_known(source,8):-1,totk_vpool_aliases_used());
        return 1;}
    for(unsigned i=0;i<*n;i++)if(sources[i]==source)return 1;
    const mb_upd *u=NULL;int partial=0;   // the last update seen that writes this source
    for(unsigned i=0;i<nu;i++){const mb_upd *x=&upd[i];if(x->lo<source+8&&source<x->lo+x->n){if(x->lo<=source&&source+8<=x->lo+x->n){u=x;partial=0;}else{u=NULL;partial=1;}}}
    if(partial){g_mb_partial++;return 1;}   // its draw re-batches after the update, as before
    mesh_batch_entry *e=mesh_batch_find(source);
    if(!u){if(e)return 1;sources[*n]=source;fill[*n]=(mb_fill){0};(*n)++;return 1;}
    if(e&&e->next_applied){g_mb_second++;return 0;}   // one pending value per source
    uint32_t w[2];memcpy(w,(const uint8_t *)u->a+16+(size_t)(source-u->lo),8);
    sources[*n]=source;fill[*n]=(mb_fill){u->a,w[0],w[1],1,e!=NULL};(*n)++;return 1;
}
static void mesh_batch_prepare(uint64_t current) {
    int prev_why=g_mb_last_why;uint64_t prev_pass=g_mb_last_pass;   // the window before this one (passdump: why a split's source was never collected)
    uint64_t sources[MESH_BATCH_CAP];mb_fill fill[MESH_BATCH_CAP];unsigned n=0;
    mb_upd upd[MESH_BATCH_UPD_CAP];unsigned nu=0;
    if(mesh_batch_eligible(current)&&!mesh_batch_find(current)){sources[n]=current;fill[n]=(mb_fill){0};n++;}
    if(g_exec_next&&g_ret_n<=8){fz_cursor c={.p=g_exec_next,.ret_n=g_ret_n,.thru_waits=1};memcpy(c.ret,g_ret,(size_t)g_ret_n*sizeof *g_ret);int why=6;   // thru_waits: an event wait whose producer is already replayed does not end the window (the replay does not stop there either)
        for(unsigned guard=0;guard<200000&&n<MESH_BATCH_CAP;guard++){
            const uint8_t *p;op_hdr h;const void *a;if(!fz_next(&c,&p,&h,&a)){why=c.bad==2?8:c.bad?1:0;break;}
            if(h.size<8||(h.size&7)||h.op>=OP_COUNT_){why=2;break;}
            if(h.op==OP_DISPATCH||h.op==OP_COPY_B2B||h.op==OP_CLEAR_BUF||h.op==OP_COPY_T2B||h.op==OP_COPY_B2T||h.op==OP_COPY_T2T||h.op==OP_CLEAR_TEX||h.op==OP_FENCE_SYNC){why=3;break;}
            if(h.op==OP_UPDATE_UB){uint64_t lo,size;memcpy(&lo,a,8);memcpy(&size,(const uint8_t *)a+8,8);int overlap=!size||size>UINT64_MAX-lo;for(unsigned k=0;k<n&&!overlap;k++)overlap=sources[k]<lo+size&&lo<sources[k]+8;if(overlap){why=4;break;}
                if(h.size<16+size||nu==MESH_BATCH_UPD_CAP){g_mb_updfull++;why=5;break;}upd[nu++]=(mb_upd){lo,size,a};}
            if(h.op==OP_DRAW&&h.size>=8+sizeof(p_draw)){const p_draw *draw=a;if(draw->prim==0x40&&draw->ind&&!mesh_batch_collect(draw->ind,sources,fill,&n,upd,nu)){why=4;break;}}
        }
        g_mb_end[why]++;g_mb_last_why=why;g_mb_last_pass=g_stat_pass;
    } else {g_mb_end[7]++;g_mb_last_why=7;g_mb_last_pass=g_stat_pass;}
    if(!n)return;mesh_batch_pipeline();
    if(g_mb_n+n>MESH_BATCH_CAP){g_mb_n=0;if(!current)return;n=1;sources[0]=current;fill[0]=(mb_fill){0};}
    if(g_enc){int had=0;for(unsigned j=0;j<g_mb_n&&!had;j++)had=g_mb[j].source==current;if(had)g_mb_split_inval++;else g_mb_split_miss++;g_mb_splits++;
        PD("Z %llu mesh-batch split src=%#llx %s table=%u prev-window: pass=%llu end=%d (0 stream 3 op 7 none) this-window: end=%d new=%u",g_stat_pass,(unsigned long long)current,had?"invalidated":"never-collected",g_mb_n,(unsigned long long)prev_pass,prev_why,g_mb_last_why,n);
        rp_end();}blit_close();
    typedef struct {uint64_t source,output;} job;
    unsigned padded=(n+63u)&~63u;id<MTLBuffer> buffer;size_t at=arena_alloc((size_t)padded*sizeof(job)+(size_t)n*16,&buffer);
    job *jobs=(job *)((uint8_t *)buffer.contents+at);memset(jobs,0,(size_t)padded*sizeof(job));uint64_t out=buffer.gpuAddress+at+(uint64_t)padded*sizeof(job);unsigned issued=0,gpu_jobs=0;
    uint32_t *outs=(uint32_t *)((uint8_t *)buffer.contents+at+(size_t)padded*sizeof(job));
    uint64_t hostwrite=atomic_load_explicit(&g_pool_input_hostwrite,memory_order_acquire);
    for(unsigned k=0;k<n;k++){
        uint64_t address=sources[k],stamp=nvn_dirty_now();nvn_dirty_protect((void *)(uintptr_t)address,8);
        unsigned slot=g_mb_n;for(unsigned j=0;j<g_mb_n;j++)if(g_mb[j].source==address){slot=j;break;}
        if(!fill[k].cpu&&mesh_src_cpu_on()){upd_guard(address,address+8);p_bind_buf pb={0};pb.addr=address;pb.size=8;   // OOT_MESH_SRC_CPU: the count as of the encode when no GPU write is pending on it
            if(!pool_range_write_fence(&pb)){uint32_t w[2];memcpy(w,(const void *)(uintptr_t)address,8);fill[k]=(mb_fill){NULL,w[0],w[1],1,0};g_mb_srccpu++;}}
        if(fill[k].cpu){   // the value is the update's bytes: written here, the kernel skips the job (source 0)
            outs[k*4]=fill[k].cnt;outs[k*4+1]=1;outs[k*4+2]=1;outs[k*4+3]=fill[k].first;jobs[k]=(job){0,out+(uint64_t)k*16};g_mb_cpu++;issued++;
            if(fill[k].next&&slot<g_mb_n){mesh_batch_entry *e=&g_mb[slot];e->next_grid=out+(uint64_t)k*16;e->next_cnt=fill[k].cnt;e->next_first=fill[k].first;e->next_applied=fill[k].a;continue;}
            if(slot==g_mb_n)g_mb_n++;g_mb[slot]=(mesh_batch_entry){.source=address,.grid=out+(uint64_t)k*16,.stamp=stamp,.hostwrite=hostwrite,.applied=fill[k].a,.cnt=fill[k].cnt,.first=fill[k].first,.cpu=1};continue;
        }
        upd_guard(address,address+8);
        void *source;uint64_t offset,gpu;if(!mtl_gpu_addr(address,8,&source,&offset,&gpu))continue;
        gpu_read_input(address,8,"mesh-batch-src",'m');   // OOT_RACE_CHECK / OOT_GPU_READ_FENCE: the batch kernel reads the source on the GPU
        jobs[k]=(job){gpu,out+(uint64_t)k*16};
        if(slot==g_mb_n)g_mb_n++;g_mb[slot]=(mesh_batch_entry){.source=address,.grid=jobs[k].output,.stamp=stamp,.hostwrite=hostwrite};issued++;gpu_jobs++;
    }
    if(gpu_jobs){
        blit_close();id<MTL4ComputeCommandEncoder> encoder=enc_c_begin(NULL);[encoder setLabel:@"OOT mesh_grid_batch"];
        [g_mb_at setAddress:buffer.gpuAddress+at atIndex:0];[encoder setArgumentTable:g_mb_at];[encoder setComputePipelineState:g_mb_ps];
        [encoder dispatchThreadgroups:MTLSizeMake(padded/64,1,1)threadsPerThreadgroup:MTLSizeMake(64,1,1)];enc_c_end(encoder);args_dirty_all();
    }
    g_mb_runs++;g_mb_jobs+=issued;
}
static void mesh_batch_verify(const mesh_batch_entry *e,uint64_t first) {
    if(!atomic_load(&g_mesh_batch_check))return;
    uint64_t grid=e->grid,source=e->source;
    void *live;uint64_t offset,gpu;if(!mtl_gpu_addr(source,8,&live,&offset,&gpu))MDIE("mesh batch verification lost live input");
    for(NvArena *a in g_sub_arenas)if(grid>=a->gpu&&grid-a->gpu<=a->used&&16<=a->used-(grid-a->gpu)){
        id<MTLBuffer> capture;size_t at=arena_alloc(32,&capture);size_t go=(size_t)(grid-a->gpu);
        if(e->cpu){uint32_t *c=(uint32_t *)((uint8_t *)capture.contents+at);c[0]=e->cnt;c[1]=e->first;}   // the expected value is the update's, memory may not hold it yet
        else enc_b_copy_buffer(blit(),(__bridge id<MTLBuffer>)live,offset,capture,at,8);
        enc_b_copy_buffer(blit(),a->buf,go,capture,at+8,16);
        if(first==grid+12)enc_b_copy_buffer(blit(),a->buf,go+12,capture,at+24,4);
        else if(first==gpu+4)enc_b_copy_buffer(blit(),(__bridge id<MTLBuffer>)live,offset+4,capture,at+24,4);
        else MDIE("mesh batch verification lost bound firstTask");
        uint64_t frame=atomic_load(&g_cur_frame);int cpu=e->cpu;
        [g_commit_blocks addObject:^{const uint32_t *v=(const uint32_t *)((const uint8_t *)capture.contents+at);
            if(v[0]!=v[2]||v[1]!=v[5]||v[1]!=v[6]||v[3]!=1||v[4]!=1)MDIE("mesh batch GPU input mismatch F%llu source=%#llx %s=%u/%u grid=%u/%u/%u/%u bound-first=%u",(unsigned long long)frame,(unsigned long long)source,cpu?"update":"live",v[0],v[1],v[2],v[3],v[4],v[5],v[6]);atomic_fetch_add(&g_mb_verified,1);
        }];return;
    }MDIE("mesh batch verification lost snapshot arena");
}
static uint64_t mesh_batch_bound(mesh_batch_entry *e,uint64_t *first) {
    uint64_t grid=e->grid,source=e->source;*first=grid+12;
    if(g_mb_negative==2){void *b;uint64_t off,gpu;if(mtl_gpu_addr(source,8,&b,&off,&gpu))*first=gpu+4;}
    mesh_batch_verify(e,*first);return grid;
}
static uint64_t mesh_batch_grid(uint64_t source,uint64_t *first) {
    mesh_batch_context();if(!mesh_batch_on()||!mesh_batch_eligible(source)){g_mb_fallback++;return 0;}
    mesh_batch_entry *e=mesh_batch_find(source);if(e){g_mb_hits++;return mesh_batch_bound(e,first);}
    mesh_batch_prepare(source);e=mesh_batch_find(source);if(!e){g_mb_fallback++;return 0;}
    return mesh_batch_bound(e,first);
}
/* Before a render pass opens (draw_inner, g_enc nil): the mesh draws ahead of this draw, up to the next dispatch / copy /
   clear, get their grids now, so the pass is not split for them later. mb2: 1.65 of the 2.2 batches a frame still split
   an open pass; a pass opened by ordinary draws met its first mesh draw with no batch. Nothing to collect: no encoder. */
static void mesh_batch_prefetch(void) {
    if(g_enc||!g_exec_next)return;
    mesh_batch_context();if(!mesh_batch_on())return;
    uint64_t runs=g_mb_runs;mesh_batch_prepare(0);if(g_mb_runs!=runs)g_mb_prefetch++;
}
