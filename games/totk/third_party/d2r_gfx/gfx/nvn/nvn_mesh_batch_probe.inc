/* Real GPU source production and consumers, independent expected uint4s. */
int mtl_probe_mesh_batch(void) {
    mtl_init();int checked=0;
    size_t bytes=32768;uint32_t *guest=mmap(NULL,bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);if(guest==MAP_FAILED)MDIE("mesh batch fixture mmap");
    mtl_pool_add(guest,bytes);nvn_dirty_add_range(guest,bytes);
    uint32_t initial[6]={3,17,0,39,5,61};memcpy(guest,initial,sizeof initial);
    void *raw;uint64_t offset,gpu;if(!mtl_gpu_addr((uintptr_t)guest,bytes,&raw,&offset,&gpu))MDIE("mesh batch fixture mapping");
    id<MTLBuffer> source=(__bridge id<MTLBuffer>)raw,rb=probe_buf(16*16),writer=probe_buf(16);
    NSString *msl=@"#include <metal_stdlib>\nusing namespace metal;kernel void consume(const device uint*grid[[buffer(0)]],const device uint*first[[buffer(1)]],device uint4*out[[buffer(2)]]){out[0]=uint4(grid[0],grid[1],grid[2],first[0]);}";
    NSError *error=nil;id<MTLLibrary> lib=totk_msl_library(msl,&error);if(!lib)MDIE("mesh batch consumer: %s",error.localizedDescription.UTF8String);
    id<MTLComputePipelineState> pipeline=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"consume"]error:&error];if(!pipeline)MDIE("mesh batch consumer pipeline");
    MTL4ArgumentTableDescriptor *d=[MTL4ArgumentTableDescriptor new];d.maxBufferBindCount=3;id<MTL4ArgumentTable> table=[g_dev newArgumentTableWithDescriptor:d error:&error];
    int old=atomic_exchange(&g_mesh_batch,1),bar=atomic_exchange(&g_oot_bar_request,1);const uint8_t *saved_next=g_exec_next;
    int negative=getenv("OOT_MESH_BATCH_NEGATIVE")?atoi(getenv("OOT_MESH_BATCH_NEGATIVE")):0;g_mb_negative=negative;
    op_hdr end={OP_END,8};struct {op_hdr h;p_draw draw;op_hdr end;} next={.h={OP_DRAW,8+sizeof(p_draw)},.draw={.prim=0x40,.ind=(uintptr_t)(guest+2),.count=1},.end={OP_END,8}};
    uint32_t expected[16][4];uint32_t (*want)[4]=expected;__block unsigned n=0;
    void (^consume)(uint64_t,uint64_t,uint32_t,uint32_t)=^(uint64_t grid,uint64_t first,uint32_t count,uint32_t start){
        rp_end();blit_close();id<MTL4ComputeCommandEncoder> encoder=enc_c_begin(NULL);[encoder setComputePipelineState:pipeline];
        [table setAddress:grid atIndex:0];[table setAddress:first atIndex:1];[table setAddress:rb.gpuAddress+(uint64_t)n*16 atIndex:2];[encoder setArgumentTable:table];
        [encoder dispatchThreads:MTLSizeMake(1,1,1)threadsPerThreadgroup:MTLSizeMake(1,1,1)];enc_c_end(encoder);
        want[n][0]=count;want[n][1]=want[n][2]=1;want[n][3]=start;n++;args_dirty_all();
    };
    mtl_sub_begin();oot_bar_sync();g_exec_next=(const uint8_t *)&next;uint64_t runs=g_mb_runs,jobs=g_mb_jobs,first=0;
    uint64_t a=mesh_batch_grid((uintptr_t)guest,&first);if(!a||(negative!=2&&first!=a+12)||g_mb_runs!=runs+1||g_mb_jobs!=jobs+2)MDIE("mesh batch did not collect future draw");checked++;
    consume(a,first,3,17);
    uint64_t b=mesh_batch_grid((uintptr_t)(guest+2),&first);if(!b||g_mb_runs!=runs+1)MDIE("mesh batch repeated a prepared conversion");checked++;
    consume(b,first,0,39);g_exec_next=NULL;
    // Ordering alone must not replace a still-valid immutable input.
    oot_bar_native(0x12);if(mesh_batch_grid((uintptr_t)guest,&first)!=a)MDIE("pure barrier invalidated batch data");checked++;
    // A real preceding GPU writer must be observed before this new capture.
    ((uint32_t *)writer.contents)[0]=7;((uint32_t *)writer.contents)[1]=93;
    enc_b_copy_buffer(blit(),writer,0,source,offset+8,8);usnap_drop((uintptr_t)(guest+2),8);
    b=mesh_batch_grid((uintptr_t)(guest+2),&first);consume(b,first,7,93);
    // Finish the capture, preserving its arena, then mutate the original CPU
    // record. Both fields consumed below still belong to the returned snapshot.
    rp_end();blit_close();dispatch_semaphore_t ready=dispatch_semaphore_create(0);mtl_sub_split_done(ready);dispatch_semaphore_wait(ready,DISPATCH_TIME_FOREVER);
    guest[1]=101;consume(a,a+12,3,17);
    uint64_t fresh=mesh_batch_grid((uintptr_t)guest,&first);consume(fresh,first,3,101);
    // Keep the returned firstTask binding while its live source changes again.
    rp_end();blit_close();mtl_sub_split_done(ready);dispatch_semaphore_wait(ready,DISPATCH_TIME_FOREVER);guest[1]=109;
    consume(fresh,first,3,101);
    // Deferred inline updates use the replay's actual write hook, not the CPU
    // page-fault path alone.
    struct {op_hdr h;uint64_t dst,size;uint32_t words[2];op_hdr end;} update={.h={OP_UPDATE_UB,32},.dst=(uintptr_t)(guest+4),.size=8,.words={11,131},.end={OP_END,8}};
    (void)mesh_batch_grid((uintptr_t)(guest+4),&first);mtl_exec((const uint8_t *)&update);
    uint64_t updated=mesh_batch_grid((uintptr_t)(guest+4),&first);consume(updated,first,11,131);
    // An inline update seen in the look-ahead, then a mesh draw of its target (2026-11-08): the grid takes the update's
    // own bytes on the CPU (no GPU read of a value not written yet), and the update's replay keeps the entry, so that
    // draw does not re-batch inside its render pass. The current source (guest+8, zeros) is still a GPU job.
    struct {op_hdr h;uint64_t dst,size;uint32_t words[2];op_hdr dh;p_draw draw;op_hdr end;} ahead={.h={OP_UPDATE_UB,32},.dst=(uintptr_t)(guest+6),.size=8,.words={13,151},.dh={OP_DRAW,8+sizeof(p_draw)},.draw={.prim=0x40,.ind=(uintptr_t)(guest+6),.count=1},.end={OP_END,8}};
    g_exec_next=(const uint8_t *)&ahead;runs=g_mb_runs;uint64_t cpu_jobs=g_mb_cpu;
    uint64_t base=mesh_batch_grid((uintptr_t)(guest+8),&first);if(!base||g_mb_runs!=runs+1||g_mb_cpu!=cpu_jobs+1)MDIE("mesh batch did not pre-apply the look-ahead update (runs %llu/%llu cpu %llu/%llu)",(unsigned long long)g_mb_runs,(unsigned long long)runs,(unsigned long long)g_mb_cpu,(unsigned long long)cpu_jobs);checked++;
    g_exec_next=NULL;consume(base,first,0,0);
    mesh_batch_op(OP_UPDATE_UB,&ahead.dst);   // the update's replay: the pre-applied entry stays
    uint64_t pre=mesh_batch_grid((uintptr_t)(guest+6),&first);if(!pre||g_mb_runs!=runs+1)MDIE("pre-applied entry lost at the update's replay");checked++;
    consume(pre,first,13,151);
    // A second update of the same source that was not seen ahead invalidates it as before.
    struct {op_hdr h;uint64_t dst,size;uint32_t words[2];op_hdr end;} again={.h={OP_UPDATE_UB,32},.dst=(uintptr_t)(guest+6),.size=8,.words={2,3},.end={OP_END,8}};
    mesh_batch_op(OP_UPDATE_UB,&again.dst);if(mesh_batch_find((uintptr_t)(guest+6)))MDIE("an unseen update kept a pre-applied entry");checked++;
    // The native barrier implementation must see the true mesh owner, even
    // though draw_inner is handed a raster-only program for these draws.
    NvShader *writing=[NvShader new];writing->rf.nb=1;writing->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=1};
    typeof(g_mtk) saved=g_mtk;g_mtk.on=1;g_mtk.ms=writing;g_mtk.ts=nil;NvProg *raster=[NvProg new];
    if(!oot_bar_shader_writes(raster))MDIE("native mesh writer owner was lost");checked++;g_mtk=saved;
    NvProg *producer=[NvProg new];producer->sh[4]=writing;mesh_batch_written(producer);
    if(negative!=1&&g_mb_n)MDIE("unknown mesh shader write kept prepared inputs");checked++;
    int mode=g_oot_bar_mode;g_oot_bar_mode=0;if(mesh_batch_grid((uintptr_t)guest,&first))MDIE("mesh batch ran without native ordering");checked++;g_oot_bar_mode=mode;
    mtl_sub_commit();mtl_finish();
    const uint32_t *out=rb.contents;for(unsigned i=0;i<n;i++)for(int k=0;k<4;k++){if(out[i*4+k]!=expected[i][k])MDIE("mesh batch GPU output row%u lane%d: %u != %u",i,k,out[i*4+k],expected[i][k]);checked++;}
    mtl_sub_begin();oot_bar_sync();uint64_t next_grid=mesh_batch_grid((uintptr_t)guest,&first);consume(next_grid,first,3,109);mtl_sub_commit();mtl_finish();
    for(int k=0;k<4;k++){if(out[(n-1)*4+k]!=expected[n-1][k])MDIE("mesh batch next arena input stale");checked++;}
    nvn_dirty_remove_range(guest);mtl_sub_begin();oot_bar_sync();if(mesh_batch_grid((uintptr_t)guest,&first))MDIE("mesh batch accepted untracked source");checked++;mtl_sub_commit();mtl_finish();
    g_exec_next=saved_next;g_mb_negative=0;g_mb_n=0;atomic_store(&g_mesh_batch,old);atomic_store(&g_oot_bar_request,bar);
    mtl_pool_del(guest);munmap(guest,bytes);defer_obj_release(rb);defer_obj_release(writer);return checked;
}
