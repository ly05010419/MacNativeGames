/* GPU conversion reuse, scoped to an immutable submission arena. The source
 * is NVN's complete {taskCount, firstTask} record, not a CPU copy of its value.
 * Reference: nanotchec/nxnative PR #1 (MIT), disjoint mesh storage writes.
 * Our runtime also checks CPU dirty pages, pool generations and virtual aliases.
 */
typedef struct { uint64_t source, grid, stamp; } mesh_indirect_entry;
static mesh_indirect_entry g_mic[64];
static unsigned g_mic_n;
static uint64_t g_mic_arena, g_mic_pool, g_mic_hits, g_mic_copies, g_mic_splits;
static uint64_t g_mic_cpu, g_mic_unknown, g_mic_overlap, g_mic_untracked;
static uint64_t g_mic_op_drops[OP_COUNT_],g_mic_context_drops[3],g_mic_repeated;
static uint64_t g_mic_seen[256];static unsigned g_mic_seen_n;
static int g_mic_mode = -1;
static int g_mic_ignore_writes; // GPU probe negative control only.
static _Atomic uint64_t g_mic_verified;
static int mesh_trace_on(void) {
    static uint64_t last=UINT64_MAX;static int active;uint64_t frame=atomic_load(&g_cur_frame);
    if(frame!=last){last=frame;active=atomic_load(&g_mesh_trace_frames)>0;if(active)atomic_fetch_sub(&g_mesh_trace_frames,1);}
    return active;
}
static int mesh_indirect_mode(void) {
    int v=atomic_load(&g_mesh_reuse);
    if(v<0){const char *e=getenv("OOT_MESH_REUSE");v=e?atoi(e):0;if(v<0||v>2)v=0;atomic_store(&g_mesh_reuse,v);}
    return v;
}
static void mesh_indirect_context(void) {
    uint64_t pool=atomic_load_explicit(&g_pool_gen,memory_order_acquire);
    int mode=mesh_indirect_mode();
    if(g_mic_arena!=g_arena_epoch || g_mic_pool!=pool || mode!=g_mic_mode){
        if(g_mic_arena!=g_arena_epoch){g_mic_context_drops[0]+=g_mic_n;g_mic_seen_n=0;}
        else if(g_mic_pool!=pool)g_mic_context_drops[1]+=g_mic_n;
        else g_mic_context_drops[2]+=g_mic_n;
        g_mic_n=0;g_mic_arena=g_arena_epoch;g_mic_pool=pool;g_mic_mode=mode;
    }
}
static void mesh_indirect_write(uint64_t lo,uint64_t size) {
    if(g_mic_ignore_writes)return;
    if(!g_mic_n)return;
    if(!size||size>UINT64_MAX-lo){g_mic_unknown+=g_mic_n;g_mic_n=0;return;}
    unsigned kept=0;
    for(unsigned i=0;i<g_mic_n;i++){
        mesh_indirect_entry e=g_mic[i];
        if(e.source<lo+size && lo<e.source+8){g_mic_overlap++;continue;}
        g_mic[kept++]=e;
    }
    g_mic_n=kept;
}
static void mesh_indirect_written(NvProg *pr) {
    if(mesh_trace_on()){
        MLOG("[mesh-trace] writers F%llu pass=%llu program=%p",atomic_load(&g_cur_frame),g_stat_pass,(__bridge void *)pr);
        if(R.has_rt){
            for(int i=0;i<R.rt.n;i++)if(R.rt.color[i])MLOG("[mesh-trace] attachment c%d source=%p bytes=%llu",i,R.rt.color[i]->mem,R.rt.color[i]->L.size);
            if(R.rt.depth)MLOG("[mesh-trace] attachment depth source=%p bytes=%llu",R.rt.depth->mem,R.rt.depth->L.size);
        }
        for(int st=0;pr&&st<5;st++){NvShader *sh=pr->sh[st];if(!sh)continue;
            MLOG("[mesh-trace] shader st%d key=%s",st,sh->key);
            for(int i=0;i<sh->rf.nb;i++){const nvn_rbind *b=&sh->rf.b[i];if(!b->rw&&b->kind!=NVN_RB_GMEML)continue;
                const p_bind_buf *u=b->kind==NVN_RB_SBUFP&&b->reg<16?&S.ssbo[st][b->reg]:NULL;
                MLOG("[mesh-trace] write st%d kind=%u reg=%u source=%#llx bytes=%llu",st,b->kind,b->reg,u?u->addr:0,u?u->size:0);
            }
        }
    }
    if(!g_mic_n)return;
    if(!pr){g_mic_unknown+=g_mic_n;g_mic_n=0;return;}
    // Attachments also write guest storage, even when no shader has an SSBO.
    if(R.has_rt){
        for(int i=0;i<R.rt.n;i++)if(R.rt.color[i])
            mesh_indirect_write((uintptr_t)R.rt.color[i]->mem,R.rt.color[i]->L.size);
        if(R.rt.depth)mesh_indirect_write((uintptr_t)R.rt.depth->mem,R.rt.depth->L.size);
    }
    for(int st=0;st<5;st++){
        NvShader *sh=pr->sh[st];if(!sh)continue;
        for(int i=0;i<sh->rf.nb;i++){
            const nvn_rbind *b=&sh->rf.b[i];
            if(b->kind==NVN_RB_GMEML){g_mic_unknown+=g_mic_n;g_mic_n=0;return;}
            if(!b->rw)continue;
            if(b->kind!=NVN_RB_SBUFP || b->reg>=16){g_mic_unknown+=g_mic_n;g_mic_n=0;return;}
            const p_bind_buf *u=&S.ssbo[st][b->reg];
            if(!u->addr||!u->size){g_mic_unknown+=g_mic_n;g_mic_n=0;return;}
            mesh_indirect_write(u->addr,u->size);
        }
    }
}
// Mode 2 follows the actual write sites: draw_inner checks resolved shader
// bindings and attachments, and post checks counter/event destinations. A
// deferred draw is checked again when it is replayed with its saved bindings.
// Commands without complete write-range accounting still clear every record.
static void mesh_indirect_op(unsigned op,const void *a) {
    if(mesh_trace_on()){
        uint64_t detail=0;
        if(op==OP_BARRIER){uint32_t bits;memcpy(&bits,a,4);detail=bits;}
        else if(op==OP_DRAW)detail=((const p_draw *)a)->ind;
        MLOG("[mesh-trace] op F%llu pass=%llu op=%u detail=%#llx cached=%u",atomic_load(&g_cur_frame),g_stat_pass,op,detail,g_mic_n);
    }
    if(!g_mic_n)return;
    switch(op){
        case OP_SET_RT:case OP_COPY_STRIDE:if(mesh_indirect_mode()==2)return;break;
        case OP_UPDATE_UB:case OP_CLEAR_BUF:
            if(mesh_indirect_mode()==2){uint64_t dst,size;memcpy(&dst,a,8);memcpy(&size,(const uint8_t *)a+8,8);mesh_indirect_write(dst,size);return;}break;
        case OP_COPY_B2B:
            if(mesh_indirect_mode()==2){uint64_t dst,size;memcpy(&dst,(const uint8_t *)a+8,8);memcpy(&size,(const uint8_t *)a+16,8);mesh_indirect_write(dst,size);return;}break;
        case OP_REPORT_COUNTER:case OP_SIGNAL_EVENT:case OP_RESET_COUNTER:
        case OP_DRAW_IDX:if(mesh_indirect_mode()==2)return;break;
        case OP_DRAW:if(((const p_draw *)a)->prim==0x40||mesh_indirect_mode()==2)return;break;
        // The old conversion also supplied a render-pass boundary. Keep that
        // boundary across explicit NVN barriers until their full dependency
        // semantics have a separate in-encoder implementation.
        case OP_BARRIER:if(mesh_indirect_mode()!=2)return;break;
        case OP_END:case OP_JUMP:case OP_BEGIN:case OP_CALL:
        case OP_VIEWPORTS:case OP_SCISSOR:case OP_DEPTH_RANGE:case OP_BIND_PROGRAM:
        case OP_BIND_STATE:case OP_BIND_VATTR:case OP_BIND_VSTREAM:case OP_BIND_VBUF:
        case OP_BIND_UBO:case OP_BIND_SSBO:case OP_BIND_TEX:case OP_BIND_SAMP:case OP_BIND_IMG:
        case OP_STENCIL:case OP_POLY_OFFSET:case OP_BLEND_COLOR:case OP_DEPTH_CLAMP:
        case OP_RASTER_DISCARD:case OP_SET_TEX_POOL:case OP_SET_SAMP_POOL:
        case OP_RENDER_ENABLE:case OP_RENDER_COND:case OP_PUSH_CONST:return;
        default:break;
    }
    g_mic_op_drops[op]+=g_mic_n;g_mic_n=0;
}
// A short diagnostic window compares the source and the reused conversion on
// the GPU timeline. Its extra copies deliberately are not performance samples.
static void mesh_indirect_check(uint64_t source,id<MTLBuffer> src,uint64_t offset,uint64_t converted) {
    if(!atomic_load(&g_mesh_reuse_check))return;
    for(NvArena *a in g_sub_arenas)if(converted>=a->gpu && converted+16<=a->gpu+a->used){
        id<MTLBuffer> capture;size_t at=arena_alloc(16,&capture);
        enc_b_copy_buffer(blit(),src,offset,capture,at,8);
        enc_b_copy_buffer(blit(),a->buf,converted-a->gpu,capture,at+8,4);
        [g_commit_blocks addObject:^{
            const uint32_t *words=(const uint32_t *)((const uint8_t *)capture.contents+at);
            if(words[0]!=words[2])MDIE("mesh reuse GPU mismatch source=%#llx actual=%u cached=%u",source,words[0],words[2]);
            atomic_fetch_add(&g_mic_verified,1);
        }];return;
    }
    MDIE("mesh reuse validation lost conversion arena");
}
static uint64_t mesh_indirect_grid(uint64_t source,id<MTLBuffer> src,uint64_t offset) {
    // Inline updates may still reside in renamed buffers. Land them before
    // looking up the conversion, just as the actual indirect draw requires.
    upd_guard(source,source+8);
    mesh_indirect_context();
    unsigned seen=0;for(;seen<g_mic_seen_n;seen++)if(g_mic_seen[seen]==source)break;
    if(seen<g_mic_seen_n)g_mic_repeated++;else if(g_mic_seen_n<256)g_mic_seen[g_mic_seen_n++]=source;
    int eligible=source<=UINT64_MAX-8 && !totk_vpool_aliases_used() &&
        nvn_dirty_tracked((void *)(uintptr_t)source,8);
    int hit=-1;
    if(eligible)for(unsigned i=0;i<g_mic_n;i++)if(g_mic[i].source==source){
        if(!nvn_dirty_since((void *)(uintptr_t)source,8,g_mic[i].stamp))hit=(int)i;
        else{g_mic_cpu++;g_mic[i]=g_mic[--g_mic_n];}
        break;
    }
    if(hit>=0){g_mic_hits++;if(g_mic_mode){uint64_t grid=g_mic[hit].grid;mesh_indirect_check(source,src,offset,grid);return grid;}}
    uint64_t stamp=nvn_dirty_now();
    if(eligible)nvn_dirty_protect((void *)(uintptr_t)source,8);else g_mic_untracked++;
    id<MTLBuffer> grid;size_t at=arena_alloc(16,&grid);
    uint32_t initial[4]={0,1,1,0};memcpy((uint8_t *)grid.contents+at,initial,sizeof initial);
    p_bind_buf pb={0};pb.addr=source;pb.size=8;
    if(mesh_src_cpu_on()&&!pool_range_write_fence(&pb)){memcpy((uint8_t *)grid.contents+at,(const void *)(uintptr_t)source,4);g_mb_srccpu++;}   // OOT_MESH_SRC_CPU: the count as of the encode (upd_guard above landed renamed updates)
    else{if(g_enc && g_enc_mode!=1)g_mic_splits++;
        gpu_read_input(source,8,"mesh-count-src",'m');   // OOT_RACE_CHECK / OOT_GPU_READ_FENCE: the blit reads the source on the GPU
        enc_b_copy_buffer(blit(),src,offset,grid,at,4);g_mic_copies++;}
    uint64_t result=grid.gpuAddress+at;
    if(eligible && !nvn_dirty_since((void *)(uintptr_t)source,8,stamp)){
        if(hit>=0)g_mic[hit]=(mesh_indirect_entry){source,result,stamp};
        else if(g_mic_n<64)g_mic[g_mic_n++]=(mesh_indirect_entry){source,result,stamp};
    }
    return result;
}
static void mesh_indirect_stats(void) {
    static uint64_t last;uint64_t frame=atomic_load(&g_cur_frame);
    if(frame<last+300)return;last=frame;
    MLOG("[mesh-reuse] F%llu mode=%d hits=%llu copies=%llu splits=%llu cpu=%llu unknown=%llu overlap=%llu untracked=%llu verified=%llu",
        frame,mesh_indirect_mode(),g_mic_hits,g_mic_copies,g_mic_splits,g_mic_cpu,g_mic_unknown,g_mic_overlap,g_mic_untracked,atomic_load(&g_mic_verified));
    char line[512];size_t at=0;
    for(unsigned i=0;i<OP_COUNT_ && at<sizeof line-40;i++)if(g_mic_op_drops[i])at+=(size_t)snprintf(line+at,sizeof line-at," %u:%llu",i,g_mic_op_drops[i]);
    line[at]=0;
    MLOG("[mesh-reuse-drop] repeated=%llu arena=%llu pool=%llu mode=%llu ops%s",g_mic_repeated,g_mic_context_drops[0],g_mic_context_drops[1],g_mic_context_drops[2],line);
}
