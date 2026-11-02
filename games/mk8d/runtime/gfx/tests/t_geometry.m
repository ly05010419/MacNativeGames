// Geometry assembly, GPU strip compaction, and production capture/compute/raster replay.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t check_want=(want), check_got=(got); checks++; if(check_want!=check_got) { fprintf(stderr,"%s: expected %llu, got %llu\n",name,(unsigned long long)check_want,(unsigned long long)check_got); failed++; } } while(0)
static void finish(void) {
    EXPECT("submission completed",1,mtl_wait(mtl_sub_commit(),5000000000ull));
    EXPECT("GPU error",0,mtl_last_commit_error());
}
static id<MTLLibrary> library(NSString *source) {
    NSError *e=nil; id<MTLLibrary> l=[g_dev newLibraryWithSource:source options:nil error:&e];
    if(!l) { fprintf(stderr,"%s\n",e.localizedDescription.UTF8String); abort(); } return l;
}
static void assembly(void) {
    uint32_t a[]={10,11,12,13,UINT32_MAX,20,21,22}, out[32];
    uint32_t want[]={10,11,12,12,11,13,20,21,22};
    EXPECT("strip count",9,nvn_geo_assemble(5,a,8,1,UINT32_MAX,NULL,0));
    EXPECT("strip write",9,nvn_geo_assemble(5,a,8,1,UINT32_MAX,out,32));
    EXPECT("strip winding and restart",0,memcmp(out,want,sizeof want));
    for(int i=0;i<32;i++) out[i]=123;
    EXPECT("short output rejected",SIZE_MAX,nvn_geo_assemble(5,a,8,1,UINT32_MAX,out,8));
    for(int i=0;i<32;i++) EXPECT("short output untouched",123,out[i]);
    uint32_t fan[]={10,11,12,10,12,13};
    EXPECT("fan",6,nvn_geo_assemble(6,a,4,0,0,out,32));
    EXPECT("fan order",0,memcmp(out,fan,sizeof fan));
    uint32_t loop[]={10,11,11,12,12,10};
    EXPECT("loop",6,nvn_geo_assemble(2,a,3,0,0,out,32));
    EXPECT("loop closure",0,memcmp(out,loop,sizeof loop));
    EXPECT("line adjacency",8,nvn_geo_assemble(11,a,5,0,0,out,32));
    EXPECT("adjacent second edge",11,out[4]);
    EXPECT("triangle adjacency",6,nvn_geo_assemble(12,a,8,0,0,out,32));
    EXPECT("unsupported",SIZE_MAX,nvn_geo_assemble(99,a,8,0,0,NULL,0));
}
static void capture_indices(void) {
    uint32_t order[]={229,105,UINT32_MAX,101,164,105,102,229},want[]={101,102,105,164,229},out[8];
    memset(out,0xcc,sizeof out);
    EXPECT("sparse duplicate capture count",5,geo_capture_unique(order,8,1,UINT32_MAX,100,130,out,8));
    EXPECT("ascending capture order",0,memcmp(out,want,sizeof want));
    EXPECT("capture output canary",0xcccccccc,out[5]);
    memset(out,0xcc,sizeof out);
    EXPECT("short capture destination rejected",SIZE_MAX,geo_capture_unique(order,8,1,UINT32_MAX,100,130,out,4));
    for(int i=0;i<8;i++)EXPECT("short capture destination untouched",0xcccccccc,out[i]);
    uint32_t wide[]={UINT32_MAX,0,5,UINT32_MAX},wide_want[]={0,5,UINT32_MAX};
    EXPECT("wide-span sorted fallback",3,geo_capture_unique(wide,4,0,0,0,(uint64_t)UINT32_MAX+1,out,8));
    EXPECT("fallback capture order",0,memcmp(out,wide_want,sizeof wide_want));
    uint32_t restart[]={17,17};memset(out,0xcc,sizeof out);
    EXPECT("restart-only capture",0,geo_capture_unique(restart,2,1,17,0,1,out,8));
    EXPECT("restart-only output canary",0xcccccccc,out[0]);
    uint32_t invalid[]={9,11};
    EXPECT("capture below declared span rejected",SIZE_MAX,geo_capture_unique(invalid,2,0,0,10,2,out,8));
    EXPECT("capture above declared span rejected",SIZE_MAX,geo_capture_unique(invalid,2,0,0,9,2,out,8));
    EXPECT("capture address overflow rejected",SIZE_MAX,geo_capture_unique(invalid,2,0,0,UINT32_MAX,2,out,8));
    EXPECT("invalid capture output canary",0xcccccccc,out[0]);
}
static void cpu_read_proofs(void) {
    // Independent hazards from each production tracker, including its unknown
    // cases. Fake addresses are queried only; no CPU/GPU memory is dereferenced.
    const uint64_t address=0x40000000;pend_reset();memset(g_gw,0,sizeof g_gw);g_gw_all=0;state_reset();
    EXPECT("clean CPU range",0,geo_cpu_hazard(address,64));
    for(int kind=1;kind<=3;kind++) {
        pend_reset();pend_add(address+16,16,kind);
        EXPECT("queued update copy or fill overlap",1,geo_cpu_hazard(address,64));
        EXPECT("unrelated queued write",0,geo_cpu_hazard(address+4096,64));
    }
    pend_reset();sbw_note(address,64,SBW_GPU);
    EXPECT("current shader write overlap",1,geo_cpu_hazard(address+32,4));
    EXPECT("unrelated current shader write",0,geo_cpu_hazard(address+4096,64));
    pend_reset();sbw_note(address,16,SBW_REPORT);
    EXPECT("deferred CPU counter report is not current GPU data",0,geo_cpu_hazard(address,64));
    pend_reset();g_pend_over=1;EXPECT("unknown pending writes",1,geo_cpu_hazard(address,64));
    pend_reset();g_sbw_over=1;EXPECT("unknown storage writes",1,geo_cpu_hazard(address,64));
    pend_reset();gw_put(address,address+64,g_done+1,g_done);
    EXPECT("previous submission live write",1,geo_cpu_hazard(address,64));
    EXPECT("unrelated previous live write",0,geo_cpu_hazard(address+4096,64));
    memset(g_gw,0,sizeof g_gw);gw_put(address,address+64,g_done,g_done);
    EXPECT("completed previous write",0,geo_cpu_hazard(address,64));
    g_gw_all=g_done+1;EXPECT("previous write overflow",1,geo_cpu_hazard(address,64));g_gw_all=0;
    EXPECT("CPU range overflow",1,geo_cpu_hazard(UINT64_MAX-3,8));
    EXPECT("empty CPU range",0,geo_cpu_hazard(0,0));
    NvShader *vs=[NvShader new];vs->rf.nv=1;vs->rf.v[0].nvn=0;vs->rf.v[0].ct=NVN_CT_FLOAT;
    S.nattr=1;S.attr[0].stream=0;S.attr[0].fmt=0x0a;S.vbuf[0]=(NVNbufferRange){address,64};
    pend_add(address,64,2);
    EXPECT("GPU-only native vertex stream does not need CPU snapshot",0,geo_conversion_hazard(vs));
    S.attr[0].fmt=0x51;
    EXPECT("converted vertex stream waits for queued write",1,geo_conversion_hazard(vs));
    pend_reset();EXPECT("clean converted stream",0,geo_conversion_hazard(vs));
    sbw_note(address,64,SBW_GPU);EXPECT("converted stream current GPU write",1,geo_conversion_hazard(vs));
    pend_reset();gw_put(address,address+64,g_done+1,g_done);
    EXPECT("converted stream prior GPU write",1,geo_conversion_hazard(vs));
    memset(g_gw,0,sizeof g_gw);S.attr[0].stream=1;S.vbuf[1]=(NVNbufferRange){address+4096,64};pend_add(address,64,2);
    EXPECT("conversion checks its actual stream",0,geo_conversion_hazard(vs));
    pend_reset();g_sbw_over=1;EXPECT("converted stream unknown GPU write",1,geo_conversion_hazard(vs));
    pend_reset();memset(g_gw,0,sizeof g_gw);state_reset();
}
static void index_cache_contract(void) {
    if(!geo_ix_on())return;
    uint32_t raw[]={10,11,12},map[]={0,1,2},ids[]={10,11,12};
    geo_ix_key key={.address=(uint64_t)raw,.count=3,.type=2,.prim=4};
    geo_ix_put(&key,raw,sizeof raw,10,12,3,map,3,ids,3);
    geo_ix_ent *e=geo_ix_get(&key,raw,sizeof raw);EXPECT("unchanged indices reuse preparation",1,e!=NULL);
    if(e){EXPECT("cached primitive order",0,memcmp(e->data+sizeof raw,map,sizeof map));EXPECT("cached unique capture IDs",0,memcmp(e->data+sizeof raw+sizeof map,ids,sizeof ids));}
    geo_ix_key other=key;other.prim=5;EXPECT("topology is part of cache key",1,geo_ix_get(&other,raw,sizeof raw)==NULL);
    other=key;other.restart=1;EXPECT("restart enable is part of cache key",1,geo_ix_get(&other,raw,sizeof raw)==NULL);
    other=key;other.restart_index=11;EXPECT("restart marker is part of cache key",1,geo_ix_get(&other,raw,sizeof raw)==NULL);
    other=key;other.type=1;EXPECT("index size is part of cache key",1,geo_ix_get(&other,raw,sizeof raw)==NULL);
    other=key;other.count=2;EXPECT("index count is part of cache key",1,geo_ix_get(&other,raw,sizeof raw)==NULL);
    raw[1]=99;EXPECT("CPU edit invalidates exact-byte reuse",1,geo_ix_get(&key,raw,sizeof raw)==NULL);
    for(unsigned i=0;i<GEO_IX_ENTRIES+2;i++){other=key;other.address=i+1;geo_ix_put(&other,raw,sizeof raw,10,99,3,map,3,ids,3);}
    EXPECT("entry budget is bounded",GEO_IX_ENTRIES,g_geo_ix.n);EXPECT("byte budget is bounded",1,g_geo_ix_bytes<=GEO_IX_BYTES);
    other=key;other.address=1;EXPECT("oldest preparation is evicted",1,geo_ix_get(&other,raw,sizeof raw)==NULL);
    other.address=GEO_IX_ENTRIES+2;EXPECT("newest preparation is retained",1,geo_ix_get(&other,raw,sizeof raw)!=NULL);
    while(g_geo_ix_last)geo_ix_drop(g_geo_ix_last);EXPECT("eviction releases all accounted bytes",0,g_geo_ix_bytes);
}
static void pack_test(void) {
    id<MTLLibrary> l=library(@(k_nvn_geo_pack_msl)); NSError *e=nil;
    id<MTLComputePipelineState> p[3]; const char *fn[3]={"geo_pack_count","geo_pack_scan","geo_pack_write"};
    for(int i=0;i<3;i++) { p[i]=[g_dev newComputePipelineStateWithFunction:[l newFunctionWithName:@(fn[i])] error:&e]; if(!p[i]) abort(); }
    uint32_t data[]={0,1,2,3,UINT32_MAX,4,5,6, UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX, 8,9,10,UINT32_MAX,11,12,13,UINT32_MAX};
    uint32_t want[]={0,1,2,2,1,3,4,5,6,8,9,10,11,12,13};
    id<MTLBuffer> b=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; res_add(b);
    for(int t=0;t<3;t++) {
        memset(b.contents,0xcc,4096); memcpy(b.contents,data,sizeof data);
        nvn_geo_pack_job j={b.gpuAddress,b.gpuAddress+512,b.gpuAddress+1024,3,8,t==0?7:t==1?6:1,100,b.gpuAddress+3072};
        memcpy((char*)b.contents+2048,&j,sizeof j);
        mtl_sub_begin();
        for(int i=0;i<3;i++) { id<MTL4ComputeCommandEncoder> en=enc_c_begin(NULL); enc_c_pso(en,p[i]);enc_c_args(en,b.gpuAddress+2048,0);
            enc_c_dispatch(en,MTLSizeMake(1,1,1),MTLSizeMake(i==1?128:64,1,1));enc_c_end(en); }
        finish();
        uint32_t *args=(uint32_t*)((char*)b.contents+1024);
        EXPECT("compacted index count",t==0?15:t==1?18:13,args[0]);
        EXPECT("indirect instance count",1,args[1]);
        if(!t) EXPECT("strip order across empty invocations",0,memcmp((char*)b.contents+512,want,sizeof want));
        EXPECT("output bound sentinel",0xcccccccc,((uint32_t*)((char*)b.contents+512))[args[0]]);
    }
    // Five scan blocks, including a partial final block, empty invocations,
    // restarts inside a group and both full and truncated output capacities.
    enum { GROUPS=257, STRIDE=8 };
    id<MTLBuffer> large=[g_dev newBufferWithLength:98304 options:MTLResourceStorageModeShared];res_add(large);
    for(int t=0;t<3;t++)for(int limited=0;limited<2;limited++) {
        memset(large.contents,0xcc,large.length);
        uint32_t *src=large.contents,reference[GROUPS*STRIDE*3];size_t n=0;
        for(unsigned g=0;g<GROUPS;g++) {
            for(unsigned i=0;i<STRIDE;i++)src[g*STRIDE+i]=i<g%8&&i!=3?g*STRIDE+i:UINT32_MAX;
            size_t k=nvn_geo_assemble(t==0?5:t==1?3:0,src+g*STRIDE,STRIDE,1,UINT32_MAX,reference+n,GROUPS*STRIDE*3-n);
            if(k==SIZE_MAX)abort();n+=k;
        }
        unsigned capacity=limited?37:(unsigned)n+10,width=t==0?3:t==1?2:1;
        unsigned expected=n<=capacity?(unsigned)n:capacity-capacity%width;
        nvn_geo_pack_job j={large.gpuAddress,large.gpuAddress+16384,large.gpuAddress+65536,GROUPS,STRIDE,t==0?7:t==1?6:1,capacity,large.gpuAddress+73728};
        memcpy((char*)large.contents+81920,&j,sizeof j);mtl_sub_begin();
        for(int i=0;i<3;i++) {id<MTL4ComputeCommandEncoder> en=enc_c_begin(NULL);enc_c_pso(en,p[i]);enc_c_args(en,large.gpuAddress+81920,0);
            enc_c_dispatch(en,MTLSizeMake(i==1?1:(GROUPS+63)/64,1,1),MTLSizeMake(i==1?128:64,1,1));enc_c_end(en);}
        finish();uint32_t *args=(uint32_t*)((char*)large.contents+65536),*dst=(uint32_t*)((char*)large.contents+16384);
        EXPECT("blocked scan count and capacity",expected,args[0]);
        EXPECT("blocked scan preserves hardware index order",0,memcmp(dst,reference,expected*4));
        EXPECT("blocked scan does not overrun output",0xcccccccc,dst[expected]);
        EXPECT("partial block counts stay within scratch",0xcccccccc,((uint32_t*)((char*)large.contents+73728))[GROUPS+(GROUPS+63)/64]);
    }
}
static void fast_pack_test(void) {
    id<MTLLibrary> lib=library(@(k_nvn_geo_pack_msl));NSError *error=nil;
    id<MTLComputePipelineState> small=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"geo_pack_small"] error:&error];
    id<MTLComputePipelineState> scan=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"geo_pack_scan"] error:&error];
    EXPECT("fused compaction pipeline",1,small!=nil);EXPECT("parallel scan pipeline",1,scan!=nil);if(!small||!scan)abort();
    id<MTLBuffer> b=[g_dev newBufferWithLength:131072 options:MTLResourceStorageModeShared];res_add(b);
    const unsigned sizes[]={0,1,31,32,63,64,65,127,128,129,257,1023,1024};
    for(unsigned test=0;test<sizeof sizes/sizeof *sizes;test++)for(int kind=0;kind<3;kind++)for(int limited=0;limited<2;limited++) {
        unsigned groups=sizes[test],width=kind==0?1:kind==1?2:3,topology=kind==0?1:kind==1?6:7;
        memset(b.contents,0xcc,b.length);uint32_t *src=b.contents,reference[24576];size_t n=0;
        for(unsigned g=0;g<groups;g++) {
            for(unsigned k=0;k<8;k++)src[g*8+k]=k<g%9&&k!=3?g*8+k:UINT32_MAX;
            n+=nvn_geo_assemble(kind==0?0:kind==1?3:5,src+g*8,8,1,UINT32_MAX,reference+n,24576-n);
        }
        unsigned capacity=limited?37:(unsigned)n+11,expected=MIN((unsigned)n,capacity-capacity%width);
        nvn_geo_pack_job job={b.gpuAddress,b.gpuAddress+32768,b.gpuAddress+98304,groups,8,topology,capacity,b.gpuAddress+100352};
        memcpy((char*)b.contents+102400,&job,sizeof job);mtl_sub_begin();id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,small);enc_c_args(ce,b.gpuAddress+102400,0);enc_c_dispatch(ce,MTLSizeMake(1,1,1),MTLSizeMake(128,1,1));enc_c_end(ce);finish();
        uint32_t *dst=(uint32_t*)((char*)b.contents+32768),*args=(uint32_t*)((char*)b.contents+98304);
        EXPECT("fused count includes complete primitives only",expected,args[0]);EXPECT("fused order matches CPU assembly",0,memcmp(dst,reference,expected*4));
        EXPECT("fused output bound sentinel",0xcccccccc,dst[expected]);EXPECT("fused indirect instance count",1,args[1]);
        EXPECT("fused indirect offsets stay zero",0,args[2]|args[3]|args[4]);EXPECT("fused compaction does not use global scratch",0xcccccccc,*(uint32_t*)((char*)b.contents+100352));
    }
    // More block totals than GPU lanes, with uneven chunks and clipping.
    enum { BLOCKS=16387,GROUPS=BLOCKS*64-19 };
    id<MTLBuffer> big=[g_dev newBufferWithLength:(GROUPS+BLOCKS+128)*4u options:MTLResourceStorageModeShared];res_add(big);
    for(int kind=0;kind<3;kind++)for(int limited=0;limited<2;limited++) {
        memset(big.contents,0xcc,big.length);uint32_t *tot=(uint32_t*)big.contents+GROUPS;uint32_t reference[BLOCKS],sum=0;
        for(unsigned k=0;k<BLOCKS;k++){reference[k]=sum;tot[k]=(k%17)*(kind+1);sum+=tot[k];}
        nvn_geo_pack_job job={0,0,b.gpuAddress+98304,GROUPS,8,kind==0?1:kind==1?6:7,limited?37:sum+7,big.gpuAddress};memcpy((char*)b.contents+102400,&job,sizeof job);
        mtl_sub_begin();id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,scan);enc_c_args(ce,b.gpuAddress+102400,0);enc_c_dispatch(ce,MTLSizeMake(1,1,1),MTLSizeMake(128,1,1));enc_c_end(ce);finish();
        EXPECT("parallel chunk prefix matches literal CPU scan",0,memcmp(tot,reference,sizeof reference));
        EXPECT("parallel scan bound sentinel",0xcccccccc,tot[BLOCKS]);unsigned w=kind+1;EXPECT("parallel scan indirect count",limited?37-37%w:sum,*(uint32_t*)((char*)b.contents+98304));
    }
}

static void pack_dependency_test(uint32_t groups) {
    // A GPU producer and all three production compaction dispatches share one
    // encoder. Compare every output and offset with an independent CPU list.
    NSString *source=[@(k_nvn_geo_pack_msl) stringByAppendingString:@"\nstruct E{device uint*raw;uint groups;};\n"
        "kernel void produce(constant E&e[[buffer(0)]],uint g[[thread_position_in_grid]]){if(g>=e.groups)return;"
        "for(uint i=0;i<8;i++)e.raw[g*8+i]=(g%5&&i<4)?g*4+i:0xffffffffu;}\n"];
    id<MTLLibrary> lib=library(source);NSError *error=nil;id<MTLComputePipelineState> pack[3];
    const char *names[]={"geo_pack_count","geo_pack_scan","geo_pack_write"};
    for(int i=0;i<3;i++){pack[i]=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@(names[i])] error:&error];if(!pack[i])abort();}
    id<MTLComputePipelineState> producer=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"produce"] error:&error];if(!producer)abort();
    size_t raw_bytes=(size_t)groups*32,packed_off=(raw_bytes+259)&~255ull,packed_bytes=(size_t)groups*24;
    size_t count_off=(packed_off+packed_bytes+259)&~255ull,args_off=(count_off+((size_t)groups+(groups+63)/64)*4+259)&~255ull,job_off=args_off+256,emit_off=job_off+256;
    id<MTLBuffer> b=[g_dev newBufferWithLength:emit_off+256 options:MTLResourceStorageModeShared];res_add(b);
    const uint32_t topology[]={1,6,7},width[]={1,2,3},per[]={4,6,6},order[3][6]={{0,1,2,3},{0,1,1,2,2,3},{0,1,2,2,1,3}};
    uint32_t *want=malloc(packed_bytes?packed_bytes:4),*offsets=malloc(groups?(size_t)groups*4:4);if(!want||!offsets)abort();
    for(int t=0;t<3;t++) {
        uint32_t n=0;for(uint32_t g=0;g<groups;g++){offsets[g]=n;if(g%5)for(uint32_t k=0;k<per[t];k++)want[n++]=g*4+order[t][k];}
        uint32_t caps[]={0,1,2,3,5,6,n?n-1:0,n};int first=groups>257?7:0;
        for(int c=first;c<8;c++) {
            uint32_t capacity=caps[c],written=n<=capacity?n:capacity-capacity%width[t];
            memset(b.contents,0xcc,b.length);
            nvn_geo_pack_job job={b.gpuAddress,b.gpuAddress+packed_off,b.gpuAddress+args_off,groups,8,topology[t],capacity,b.gpuAddress+count_off};
            memcpy((char*)b.contents+job_off,&job,sizeof job);uint64_t emit[]={b.gpuAddress,groups};memcpy((char*)b.contents+emit_off,emit,sizeof emit);
            mtl_sub_begin();id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,producer);enc_c_args(ce,b.gpuAddress+emit_off,0);
            enc_c_dispatch(ce,MTLSizeMake((groups+63)/64,1,1),MTLSizeMake(64,1,1));
            geo_pack_encode(ce,pack,b.gpuAddress+job_off,groups);enc_c_end(ce);finish();
            uint32_t *raw=b.contents,*packed=(uint32_t*)((char*)b.contents+packed_off),*counts=(uint32_t*)((char*)b.contents+count_off),*args=(uint32_t*)((char*)b.contents+args_off);
            EXPECT("parallel compaction count",written,args[0]);EXPECT("parallel compaction instance count",1,args[1]);
            EXPECT("parallel compaction complete ordered output",0,memcmp(packed,want,(size_t)written*4));
            int mismatch=0;for(uint32_t g=0;g<groups;g++)mismatch+=counts[g]+counts[groups+g/64]!=offsets[g];
            EXPECT("parallel prefix offsets for every group",0,mismatch);
            EXPECT("GPU producer allocation canary",0xcccccccc,raw[(size_t)groups*8]);
            EXPECT("parallel compaction output canary",0xcccccccc,packed[written]);
            EXPECT("parallel prefix allocation canary",0xcccccccc,counts[groups+(groups+63)/64]);
            EXPECT("parallel indirect args canary",0xcccccccc,args[5]);
        }
    }
    free(want);free(offsets);
}
static void scan_edges(void) {
    id<MTLLibrary> lib=library(@(k_nvn_geo_pack_msl));NSError *error=nil;
    id<MTLComputePipelineState> scan=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"geo_pack_scan"] error:&error];if(!scan)abort();
    id<MTLBuffer> b=[g_dev newBufferWithLength:1048576 options:MTLResourceStorageModeShared];res_add(b);
    const uint32_t sizes[]={0,1,2,31,32,33,63,64,65,255,256,257,1023},threads[]={1,32,192,256};uint32_t want[1023];
    for(int sz=0;sz<13;sz++)for(int nt=0;nt<4;nt++) {
        uint32_t blocks=sizes[sz],groups=blocks*64,sum=0;memset(b.contents,0xcc,b.length);uint32_t *counts=b.contents;
        for(uint32_t g=0;g<blocks;g++){want[g]=sum;counts[groups+g]=g%5?g%4:0;sum+=counts[groups+g];}
        nvn_geo_pack_job job={0,0,b.gpuAddress+524288,groups,0,7,37,b.gpuAddress};memcpy((char*)b.contents+524544,&job,sizeof job);
        mtl_sub_begin();id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,scan);enc_c_args(ce,b.gpuAddress+524544,0);
        enc_c_dispatch(ce,MTLSizeMake(1,1,1),MTLSizeMake(threads[nt],1,1));enc_c_end(ce);finish();
        uint32_t *args=(uint32_t*)((char*)b.contents+524288);
        EXPECT("scan edge complete offsets",0,memcmp(counts+groups,want,(size_t)blocks*4));
        EXPECT("scan edge capacity count",sum<=37?sum:36,args[0]);EXPECT("scan edge counts canary",0xcccccccc,counts[groups+blocks]);
        EXPECT("scan edge args canary",0xcccccccc,args[5]);
    }
    // Large invalid totals must not wrap back into the writable output range.
    memset(b.contents,0xcc,b.length);uint32_t *counts=b.contents;counts[192]=UINT32_MAX-5;counts[193]=10;counts[194]=100;
    nvn_geo_pack_job job={0,0,b.gpuAddress+524288,192,0,7,64,b.gpuAddress};memcpy((char*)b.contents+524544,&job,sizeof job);
    mtl_sub_begin();id<MTL4ComputeCommandEncoder> ce=enc_c_begin(NULL);enc_c_pso(ce,scan);enc_c_args(ce,b.gpuAddress+524544,0);
    enc_c_dispatch(ce,MTLSizeMake(1,1,1),MTLSizeMake(256,1,1));enc_c_end(ce);finish();
    uint32_t *args=(uint32_t*)((char*)b.contents+524288);
    EXPECT("overflow prefix first",0,counts[192]);EXPECT("overflow prefix second",UINT32_MAX-5,counts[193]);EXPECT("overflow prefix saturates",UINT32_MAX,counts[194]);
    EXPECT("overflow total stays capacity bounded",63,args[0]);EXPECT("overflow scan counts canary",0xcccccccc,counts[195]);
    // Exercise the production write bound with an offset whose addition used
    // to wrap. All output bytes must remain untouched.
    id<MTLComputePipelineState> write=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"geo_pack_write"] error:&error];if(!write)abort();
    uint32_t *raw=(uint32_t*)((char*)b.contents+4096);raw[0]=0;raw[1]=1;raw[2]=2;counts[0]=UINT32_MAX-1;counts[1]=0;
    job=(nvn_geo_pack_job){b.gpuAddress+4096,b.gpuAddress+8192,b.gpuAddress+524288,1,3,7,8,b.gpuAddress};memcpy((char*)b.contents+524544,&job,sizeof job);
    mtl_sub_begin();ce=enc_c_begin(NULL);enc_c_pso(ce,write);enc_c_args(ce,b.gpuAddress+524544,0);
    enc_c_dispatch(ce,MTLSizeMake(1,1,1),MTLSizeMake(64,1,1));enc_c_end(ce);finish();
    uint32_t *output=(uint32_t*)((char*)b.contents+8192);for(int i=0;i<9;i++)EXPECT("overflow offset never writes output",0xcccccccc,output[i]);
}
static void passthrough_bounds(NSArray *layouts) {
    // The synthetic emitter uses the translator's topology-specific bound;
    // allocations and compaction consume metadata from output_layout(). Old
    // header-only metadata overwrites both canaries and compacts the wrong span.
    const uint32_t vertices[]={1,2,2,3,3};const int groups=3;
    EXPECT("five passthrough topology layouts",5,layouts.count);if(layouts.count!=5)return;
    id<MTLLibrary> packlib=library(@(k_nvn_geo_pack_msl));NSError *error=nil;
    id<MTLComputePipelineState> pack[3];const char *names[]={"geo_pack_count","geo_pack_scan","geo_pack_write"};
    for(int i=0;i<3;i++){pack[i]=[g_dev newComputePipelineStateWithFunction:[packlib newFunctionWithName:@(names[i])] error:&error];if(!pack[i])abort();}
    id<MTLBuffer> b=[g_dev newBufferWithLength:32768 options:MTLResourceStorageModeShared];res_add(b);
    for(int t=0;t<5;t++) {
        NSDictionary *layout=layouts[t];uint32_t bound=[layout[@"vertices"] unsignedIntValue],stride=[layout[@"index_stride"] unsignedIntValue],topology=[layout[@"output"] unsignedIntValue];
        if(bound>100||stride>200){EXPECT("bounded fixture layout",1,0);continue;}
        id<MTLLibrary> emitlib=library([NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct A{device uint*raw;device float4*vertices;};\n"
            "kernel void emit(constant A&a[[buffer(0)]],uint g[[thread_position_in_grid]]){const uint nv=%u;"
            "for(uint v=0;v<nv;v++){a.raw[g*nv*2+v]=g*nv+v;a.vertices[g*nv+v]=float4(float(g),float(v),0,1);}"
            "for(uint v=nv;v<nv*2;v++)a.raw[g*nv*2+v]=0xffffffffu;}\n",vertices[t]]);
        id<MTLComputePipelineState> emit=[g_dev newComputePipelineStateWithFunction:[emitlib newFunctionWithName:@"emit"] error:&error];if(!emit)abort();
        memset(b.contents,0xcc,b.length);uint64_t emitargs[]={b.gpuAddress,b.gpuAddress+16384};memcpy((char*)b.contents+28672,emitargs,sizeof emitargs);
        nvn_geo_pack_job job={b.gpuAddress,b.gpuAddress+4096,b.gpuAddress+8192,groups,stride,topology,100,b.gpuAddress+12288};memcpy((char*)b.contents+28928,&job,sizeof job);
        mtl_sub_begin();id<MTL4ComputeCommandEncoder> en=enc_c_begin(NULL);enc_c_pso(en,emit);enc_c_args(en,b.gpuAddress+28672,0);
        enc_c_dispatch(en,MTLSizeMake(groups,1,1),MTLSizeMake(1,1,1));
        geo_pack_encode(en,pack,b.gpuAddress+28928,groups);enc_c_end(en);finish();
        uint32_t *raw=b.contents,*output=(uint32_t*)((char*)b.contents+16384),*indices=(uint32_t*)((char*)b.contents+4096),*args=(uint32_t*)((char*)b.contents+8192);
        EXPECT("passthrough raw allocation canary",0xcccccccc,raw[groups*stride]);
        EXPECT("passthrough vertex allocation canary",0xcccccccc,output[groups*bound*4]);
        EXPECT("passthrough compacted count",groups*vertices[t],args[0]);
        for(unsigned i=0;i<groups*vertices[t];i++)EXPECT("passthrough compacted order",i,indices[i]);
        EXPECT("passthrough compacted allocation canary",0xcccccccc,indices[groups*vertices[t]]);
    }
}
static NvShader *shader(id<MTLLibrary> l, NSString *name, int stage, const int *helpers, int n) {
    NvShader *s=[NvShader new];s->stage=stage;s->fn=[l newFunctionWithName:name];s->rf.nb=n;
    // Synthetic functions must have distinct content identities too: pipeline caches
    // key on shader content, not the transient MTLLibrary object address.
    static uint64_t identity=0x7000000000000000ull;snprintf(s->key,sizeof s->key,"%016llx_b",(unsigned long long)++identity);
    if(n) {s->ae=[s->fn newArgumentEncoderWithBufferIndex:0];s->arg_len=s->ae.encodedLength;}
    for(int i=0;i<n;i++)s->rf.b[i]=(nvn_rbind){.kind=NVN_RB_GEOMETRY,.reg=helpers[i],.aid=i};
    return s;
}
static void storage_buffers(void) {
    // Exercise the production JSON conversion and argument encoder: geometry
    // keeps NVN stage 2 even though this function runs as a Metal kernel.
    id<MTLLibrary> l=library(@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct A {constant uint*driver[[id(0)]];device uint*output[[id(1)]];};\n"
        "kernel void table(constant A&a[[buffer(0)]]) {for(uint i=0;i<3;i++){a.output[i]=a.driver[0x104+i];a.output[3+i]=a.driver[0x10c+i];}}\n");
    int helpers[]={0,0};NvShader *s=shader(l,@"table",2,helpers,2);char error[160]={0};
    NSDictionary *json=@{@"route":@"B",@"bindings":@[
        @{@"kind":@"drvcb",@"bank":@0,@"arg_id":@0,@"msl_name":@"gp_c0",@"msl_type":@"constant uint*"},
        @{@"kind":@"sbuf",@"reg":@2,@"arg_id":@1,@"msl_name":@"gp_s1",@"msl_type":@"device uint*",@"rw":@YES}]};
    EXPECT("geometry storage reflection",0,nvn_refl_from_json(json,&s->rf,error,sizeof error));
    EXPECT("geometry storage write flag",1,s->rf.b[1].rw);
    NSDictionary *readonly=@{@"route":@"B",@"bindings":@[
        @{@"kind":@"sbuf",@"reg":@2,@"arg_id":@0,@"msl_name":@"gp_s1",@"msl_type":@"device uint*",@"rw":@NO}]};
    nvn_refl rr;EXPECT("explicit read-only reflection",0,nvn_refl_from_json(readonly,&rr,error,sizeof error));
    EXPECT("explicit read-only flag overrides device qualifier",0,rr.b[0].rw);
    uint8_t *data;if(posix_memalign((void**)&data,getpagesize(),12288))abort();memset(data,0xcc,12288);mtl_pool_add(data,12288);
    state_reset();S.ssbo[2][0]=(p_bind_buf){.stage=2,.index=0,.addr=(uint64_t)data,.size=123};
    S.ssbo[2][2]=(p_bind_buf){.stage=2,.index=2,.addr=(uint64_t)(data+4096),.size=64};
    S.ssbo[5][0]=(p_bind_buf){.stage=5,.index=0,.addr=(uint64_t)(data+8192),.size=99};
    NSError *e=nil;id<MTLComputePipelineState> ps=[g_dev newComputePipelineStateWithFunction:s->fn error:&e];if(!ps)abort();
    mtl_sub_begin();id<MTL4ComputeCommandEncoder> en=enc_c_begin(NULL);args_dirty_all();encode_args(en,s,2,0);enc_c_pso(en,ps);
    EXPECT("geometry storage write tracked",1,sbw_hits((uint64_t)(data+4096),(uint64_t)(data+4096)+24));
    enc_c_dispatch(en,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1));enc_c_end(en);finish();
    uint64_t row0=0,row2=0;uint32_t *out=(uint32_t*)(data+4096);memcpy(&row0,out,8);memcpy(&row2,out+3,8);
    EXPECT("geometry c0 row 0 guest address",(uint64_t)data,row0);EXPECT("geometry c0 row 0 size",123,out[2]);
    EXPECT("geometry c0 row 2 guest address",(uint64_t)(data+4096),row2);EXPECT("geometry c0 row 2 size",64,out[5]);
    mtl_pool_del(data);mtl_drain();free(data);
}
static void passthrough_kernels(const char *directory) {
    // Actual EXIT-only Maxwell passthrough shaders, compiled by dmsh_xlate.
    // Positions and all unmodified generic attributes must survive compute
    // lowering, with adjacency removed and instances kept independent.
    const uint32_t widths[]={1,2,4,3,6},vertices[]={1,2,2,3,3};
    const uint32_t source_vertex[5][3]={{0},{0,1},{1,2},{0,1,2},{0,2,4}};
    const uint32_t primitives=3,instances=2,groups=primitives*instances;
    id<MTLLibrary> packlib=library(@(k_nvn_geo_pack_msl));NSError *error=nil;
    id<MTLComputePipelineState> pack[3];const char *names[]={"geo_pack_count","geo_pack_scan","geo_pack_write"};
    for(int i=0;i<3;i++){pack[i]=[g_dev newComputePipelineStateWithFunction:[packlib newFunctionWithName:@(names[i])] error:&error];if(!pack[i])abort();}
    for(int t=0;t<5;t++) {
        fprintf(stderr,"compiled passthrough topology %d\n",t);
        NSString *stem=[@(directory) stringByAppendingPathComponent:[NSString stringWithFormat:@"pass%d",t]];
        NSData *data=[NSData dataWithContentsOfFile:[stem stringByAppendingString:@".json"]];
        NSDictionary *json=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:&error]:nil;
        id<MTLLibrary> lib=[g_dev newLibraryWithURL:[NSURL fileURLWithPath:[stem stringByAppendingString:@".macosx.metallib"]] error:&error];
        EXPECT("compiled passthrough fixture loaded",1,json&&lib);if(!json||!lib)continue;
        NvShader *s=shader(lib,@"main0",2,NULL,0);char message[160]={0};
        EXPECT("passthrough reflection",0,nvn_refl_from_json(json,&s->rf,message,sizeof message));
        s->ae=[s->fn newArgumentEncoderWithBufferIndex:0];s->arg_len=s->ae.encodedLength;
        NSDictionary *layout=json[@"geometry"];
        uint32_t bound=[layout[@"vertices"] unsignedIntValue],stride=[layout[@"stride"] unsignedIntValue],index_stride=[layout[@"index_stride"] unsignedIntValue];
        EXPECT("passthrough topology bound",vertices[t],bound);EXPECT("passthrough complete generic map",133,stride);
        if(bound!=vertices[t]||stride!=133||index_stride!=vertices[t]*2)continue;
        id<MTLComputePipelineState> pipeline=[g_dev newComputePipelineStateWithFunction:s->fn error:&error];
        if(!pipeline){fprintf(stderr,"passthrough topology %d: %s\n",t,error.localizedDescription.UTF8String);abort();}
        id<MTLBuffer> b=[g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];res_add(b);memset(b.contents,0xcc,b.length);
        const uint32_t count=primitives*widths[t];float *in=b.contents;
        for(unsigned i=0;i<count*instances;i++) {
            float *p=in+i*144;memset(p,0,144*4);
            p[0]=i+.25f;p[1]=i+.5f;p[2]=i+.75f;p[3]=1;p[4]=1;
            for(unsigned c=0;c<128;c++)p[16+c]=i*128+c+.125f;
        }
        uint32_t vi[8]={count,instances,0,0,primitives,0,0,0};memcpy((char*)b.contents+24576,vi,sizeof vi);
        uint32_t *map=(uint32_t*)((char*)b.contents+24832);for(unsigned v=0;v<count;v++)map[v]=v;
        state_reset();g_geo_bind[0]=b.gpuAddress;g_geo_bind[1]=b.gpuAddress+32768;g_geo_bind[2]=b.gpuAddress+49152;
        g_geo_bind[3]=b.gpuAddress+24576;g_geo_bind[4]=b.gpuAddress+24832;
        nvn_geo_pack_job job={g_geo_bind[2],b.gpuAddress+50176,b.gpuAddress+53248,groups,index_stride,[layout[@"output"] unsignedIntValue],100,b.gpuAddress+53504};
        memcpy((char*)b.contents+54016,&job,sizeof job);
        mtl_sub_begin();id<MTL4ComputeCommandEncoder> en=enc_c_begin(NULL);args_dirty_all();encode_args(en,s,2,0);enc_c_pso(en,pipeline);
        enc_c_dispatch(en,MTLSizeMake(1,instances,1),MTLSizeMake(32,1,1));
        geo_pack_encode(en,pack,b.gpuAddress+54016,groups);enc_c_end(en);finish();
        float *out=(float*)((char*)b.contents+32768);uint32_t *raw=(uint32_t*)((char*)b.contents+49152),*packed=(uint32_t*)((char*)b.contents+50176),*args=(uint32_t*)((char*)b.contents+53248);
        for(unsigned g=0;g<groups;g++) {
            for(unsigned v=0;v<vertices[t];v++) {
                float *q=in+((g/primitives)*count+(g%primitives)*widths[t]+source_vertex[t][v])*144,*p=out+(g*bound+v)*stride;
                for(unsigned c=0;c<4;c++)EXPECT("passthrough position",1,p[c]==q[c]);
                for(unsigned c=0;c<128;c++)EXPECT("passthrough generic attribute",1,p[5+c]==q[16+c]);
                EXPECT("passthrough raw index",g*bound+v,raw[g*index_stride+v]);
                EXPECT("passthrough compacted index",g*bound+v,packed[g*bound+v]);
            }
            for(unsigned v=bound;v<index_stride;v++)EXPECT("passthrough restart padding",UINT32_MAX,raw[g*index_stride+v]);
        }
        EXPECT("passthrough output canary",0xcccccccc,((uint32_t*)out)[groups*bound*stride]);
        EXPECT("passthrough index canary",0xcccccccc,raw[groups*index_stride]);
        EXPECT("passthrough compacted count",groups*bound,args[0]);
        EXPECT("passthrough compacted canary",0xcccccccc,packed[groups*bound]);
    }
}
static void replay(int mode) {
    int kind=mode<7?mode:mode>=16?5:3;
    int conversion=mode==12||mode==13,native_stream=mode==11;
    id<MTLLibrary> l=library(@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct C {device float *dst;uint4 info;};\n"
        "vertex void capture(uint v[[vertex_id]],uint i[[instance_id]],constant C&c[[buffer(13)]]) {uint n=v-c.info.x,inst=i-c.info.z;device float*p=c.dst+(inst*c.info.y+n)*144; p[0]=float(n);p[1]=float(inst);p[2]=0;p[3]=1;}\n"
        "vertex float4 unused(uint v[[vertex_id]]) {return float4(0,0,0,1);}\n"
        "struct G{device const float*in[[id(0)]];device float4*out[[id(1)]];device uint*ix[[id(2)]];constant uint4*info[[id(3)]];device const uint*map[[id(4)]];};\n"
        "kernel void geometry(constant G&g[[buffer(0)]],uint3 t[[thread_position_in_grid]]) {if(t.x>=g.info[1].x||t.y>=g.info[0].y)return;uint k=(t.y*g.info[1].x+t.x)*2+t.z;uint v=g.map[t.x];float2 q=float2(g.in[(t.y*g.info[0].x+v)*144],g.in[(t.y*g.info[0].x+v)*144+1]);float2 a=q*0.5-0.9;float2 d[4]={float2(0),float2(.4,0),float2(0,.4),float2(.4)};for(uint n=0;n<4;n++){g.out[k*4+n]=float4(a+d[n],0,1);g.ix[k*5+n]=k*4+n;}g.ix[k*5+4]=0xffffffffu;}\n"
        "struct V{device const float4*p[[id(0)]];};vertex float4 reader(uint v[[vertex_id]],constant V&a[[buffer(0)]]){return a.p[v];}\n"
        "fragment float4 red(){return float4(1,0,0,1);}\n"
        "kernel void index_write(device uint*p[[buffer(0)]]){p[0]=12;p[1]=10;p[2]=0xffffffffu;p[3]=11;}\n");
    int helpers[]={0,1,2,3,4}, rh[]={1};
    NvProg *pr=[NvProg new];pr->sh[0]=shader(l,@"unused",0,NULL,0);pr->sh[0]->geo_fn=[l newFunctionWithName:@"capture"];
    pr->sh[1]=shader(l,@"red",1,NULL,0);pr->sh[2]=shader(l,@"geometry",2,helpers,5);
    if(conversion||native_stream){pr->sh[0]->rf.nv=1;pr->sh[0]->rf.v[0].ct=NVN_CT_FLOAT;pr->sh[0]->rf.v[0].nvn=pr->sh[0]->rf.v[0].loc=0;}
    NvShader *gs=pr->sh[2];strcpy(gs->key,"0123456789abcdef_b");gs->geo_vertices=4;gs->geo_invocations=2;gs->geo_output=7;gs->geo_stride=4;gs->geo_index_stride=5;
    g_shcache=[NSMutableDictionary dictionary];g_shcache[@"0123456789abcdef_bg0"]=gs;g_shcache[@"0123456789abcdef_bgv"]=shader(l,@"reader",0,rh,1);
    static NSMutableArray *keep;if(!keep)keep=[NSMutableArray array];[keep addObject:pr];[keep addObject:g_shcache[@"0123456789abcdef_bgv"]];
    static prog_t cp[18];cp[mode].serial=8700+mode;serial_put(cp[mode].serial,pr);
    tex_t target={.magic=MAGIC('T','E','X',0),.serial=8800+mode};target.d.width=target.d.height=32;target.d.depth=target.d.samples=target.d.levels=1;target.d.target=NVN_TEXTURE_TARGET_2D;target.d.format=NVN_FORMAT_RGBA8;
    uint8_t *commands; if(posix_memalign((void**)&commands,getpagesize(),65536))abort();memset(commands,0,65536);mtl_pool_add(commands,65536);
    size_t used=0;
    #define EMIT(OP,X) do {size_t z=(8+sizeof(X)+7)&~7ull;op_hdr h={OP,(uint32_t)z};memcpy(commands+used,&h,8);memcpy(commands+used+8,&(X),sizeof(X));used+=z;}while(0)
    p_set_rt rt={.n=1};rt.color[0]=&target;EMIT(OP_SET_RT,rt);
    p_viewports vp={.n=1};vp.v[0][2]=vp.v[0][3]=32;EMIT(OP_VIEWPORTS,vp);p_scissor sc={0,0,32,32};EMIT(OP_SCISSOR,sc);
    p_clear_color cl={.index=0,.mask=15,.rgba={0,0,0,1}};EMIT(OP_CLEAR_COLOR,cl);
    p_bind_program bp={&cp[mode],7};EMIT(OP_BIND_PROGRAM,bp);
    if(conversion||native_stream) {
        p_bind_vattr va={.n=1};va_st attribute={.fmt=conversion?0x51:0x0a};memcpy(va.st,&attribute,4);EMIT(OP_BIND_VATTR,va);
        p_bind_vstream stream={.n=1};stream.st[0].stride=conversion?1:4;EMIT(OP_BIND_VSTREAM,stream);
        p_bind_vbuf vb={.n=1};vb.r[0]=(NVNbufferRange){(uint64_t)(commands+24576),64};EMIT(OP_BIND_VBUF,vb);
        memset(commands+24576,conversion?17:0,64);
    }
    p_draw d={.prim=0,.first=7,.count=kind==6?9000:3,.instances=2,.base_instance=5};
    if((kind>=1&&kind<=3)||kind==5) {
        int type=kind==5?1:kind-1;uint32_t ix[]={12,10,UINT32_MAX,11};
        for(int i=0;i<4;i++)memcpy(commands+8192+(i<<type),ix+i,1<<type);
        p_prim_restart restart={1,UINT32_MAX};EMIT(OP_PRIM_RESTART,restart);
        p_draw_idx di={.prim=0,.itype=type,.count=4,.instances=2,.base_vertex=-3,.base_instance=5,.ibuf=(uint64_t)(commands+8192)};
        if(kind==5){uint32_t args[]={4,2,mode==17?16:0,(uint32_t)-3,5};memcpy(commands+16384,args,sizeof args);di.ind=(uint64_t)(commands+16384);if(mode==17)di.ibuf-=32;}
        if(mode==7||mode==10||mode==11||mode==13||mode==16||mode==17) {
            uint64_t destination=(uint64_t)(commands+(mode==10?32768:mode==11||mode==13?24576:mode==16?16384:8192));
            uint64_t n=mode==11||mode==13?64:mode==16?20:(uint64_t)4<<type;
            memcpy(commands+12288,(void*)(uintptr_t)destination,n);
            if(mode!=10)memset((void*)(uintptr_t)destination,mode==13?3:0,n);
            struct{uint64_t source,destination,bytes;}copy={(uint64_t)(commands+12288),destination,n};EMIT(OP_COPY_B2B,copy);
        }
        if(mode==8||mode==9)memset(commands+8192,0,16);
        EMIT(OP_DRAW_IDX,di);
    } else {
        if(kind==4){uint32_t args[]={3,2,7,5};memcpy(commands+16384,args,sizeof args);d.ind=(uint64_t)(commands+16384);}
        EMIT(OP_DRAW,d);
    }
    op_hdr end={OP_END,8};memcpy(commands+used,&end,8);
    #undef EMIT
    id<MTLComputePipelineState> writer;
    if(mode==8||mode==9){NSError *error=nil;writer=[g_dev newComputePipelineStateWithFunction:[l newFunctionWithName:@"index_write"] error:&error];if(!writer)abort();}
    id<MTLSharedEvent> gate;
    if(mode==9) {
        // Hold a previous submission live until this draw reaches its hazard
        // check; a completed prior write correctly needs no second wait.
        gate=[g_dev newSharedEvent];rec_drain();[g_q4 waitForEvent:gate value:1];
        mtl_sub_begin();id<MTL4ComputeCommandEncoder> en=enc_c_begin(NULL);enc_c_pso(en,writer);
        uint64_t gpu,offset;void *buffer;mtl_gpu_addr((uint64_t)(commands+8192),16,&buffer,&offset,&gpu);enc_c_args(en,gpu,0);
        enc_c_dispatch(en,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1));enc_c_end(en);sbw_note((uint64_t)(commands+8192),16,SBW_GPU);
        mtl_sub_commit();rec_drain();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,500000000),dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{gate.signaledValue=1;});
    }
    state_reset();memset(&R,0,sizeof R);mtl_sub_begin();uint64_t waits=g_mdi_v;
    if(mode==8) {
        id<MTL4ComputeCommandEncoder> en=enc_c_begin(NULL);enc_c_pso(en,writer);
        uint64_t gpu,offset;void *buffer;mtl_gpu_addr((uint64_t)(commands+8192),16,&buffer,&offset,&gpu);enc_c_args(en,gpu,0);
        enc_c_dispatch(en,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1));enc_c_end(en);sbw_note((uint64_t)(commands+8192),16,SBW_GPU);
    }
    if(mode==14)g_sbw_over=1;
    if(mode==15)g_pend_over=1;
    uint64_t h=(uint64_t)commands;mtl_sub_index(&h,1);mtl_exec(commands);
    char wait_name[80];snprintf(wait_name,sizeof wait_name,"geometry CPU snapshot wait mode %d",mode);
    EXPECT(wait_name,mode==7||mode==8||mode==9||mode==13||mode>=14,g_mdi_v-waits);
    if(conversion) {
        float first=-1;for(int i=0;i<64;i++)if(g_vconv[i].serial==g_sub_serial&&g_vconv[i].addr==(uint64_t)(commands+24576))
            for(NvArena *a in g_sub_arenas)if(g_vconv[i].gpu>=a->gpu&&g_vconv[i].gpu+4<=a->gpu+a->buf.length)memcpy(&first,(char*)a->buf.contents+g_vconv[i].gpu-a->gpu,4);
        EXPECT("CPU conversion sees the queued GPU write",1,first==17);
    }
    id<MTLBuffer> b=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];res_add(b);
    uint32_t *args=NULL;id<MTLBuffer> argsBuffer;
    for(NvArena *a in g_sub_arenas)if(g_geo_args>=a->gpu&&g_geo_args+20<=a->gpu+a->buf.length){argsBuffer=a->buf;args=(uint32_t*)((char*)argsBuffer.contents+g_geo_args-a->gpu);}
    [blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(32,32,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:128 destinationBytesPerImage:4096];finish();
    EXPECT("two GS invocations per input primitive",kind==6?216000:72,args?args[0]:0);
    int red=0;uint8_t *px=b.contents;for(int i=0;i<1024;i++)if(px[i*4]>200)red++;
    EXPECT("expanded quads rasterized",kind==6?288:216,red);
    EXPECT("each instance has independent capture and output",1,px[(24*32+4)*4]>200);
    res_remove(b);
    mtl_tex_release(&target);
    mtl_pool_del(commands);mtl_drain();free(commands);
}
// Optional real-game fixtures stay outside the repository. The supplied directory
// is a dmsh_build result for the current chain, containing these three GS keys.
static void replay_local(int mode) {
    id<MTLLibrary> l=library(@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct C {device float *dst;uint4 info;};\n"
        "vertex void capture(uint v[[vertex_id]],uint i[[instance_id]],constant C&c[[buffer(13)]]) {uint n=v-c.info.x,inst=i-c.info.z;device float*p=c.dst+(inst*c.info.y+n)*144; p[0]=float(n);p[1]=float(inst);p[2]=0;p[3]=1;}\n"
        "vertex float4 unused(uint v[[vertex_id]]) {return float4(0,0,0,1);}\n"
        "struct G{device const float*in[[id(0)]];device float4*out[[id(1)]];device uint*ix[[id(2)]];constant uint4*info[[id(3)]];device const uint*map[[id(4)]];device uint*ssbo[[id(5)]];};\n"
        "kernel void geometry(constant G&g[[buffer(0)]],uint3 t[[thread_position_in_grid]]) {if(t.x>=g.info[1].x||t.y>=g.info[0].y)return;if(!t.x&&!t.y&&!t.z)g.ssbo[0]=uint(g.in[0]*4+1);uint k=(t.y*g.info[1].x+t.x)*2+t.z;uint v=g.map[t.x];float2 q=float2(g.in[(t.y*g.info[0].x+v)*144],g.in[(t.y*g.info[0].x+v)*144+1]);float2 a=q*0.5-0.9;float2 d[4]={float2(0),float2(.4,0),float2(0,.4),float2(.4)};for(uint n=0;n<4;n++){g.out[k*4+n]=float4(a+d[n],0,1);g.ix[k*5+n]=k*4+n;}g.ix[k*5+4]=0xffffffffu;}\n"
        "struct V{device const float4*p[[id(0)]];};vertex float4 reader(uint v[[vertex_id]],constant V&a[[buffer(0)]]){return a.p[v];}\n"
        "vertex float4 cover(uint v[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[v],.5,1);}\n"
        "fragment float4 red(){return float4(1,0,0,1);}\n");
    int helpers[]={0,1,2,3,4}, rh[]={1};
    NvProg *pr=[NvProg new];pr->sh[0]=shader(l,@"unused",0,NULL,0);pr->sh[0]->geo_fn=[l newFunctionWithName:@"capture"];
    pr->sh[1]=shader(l,@"red",1,NULL,0);pr->sh[2]=shader(l,@"geometry",2,helpers,5);
    NvShader *gs=pr->sh[2];gs->rf.b[gs->rf.nb++]=(nvn_rbind){.kind=NVN_RB_SBUFP,.reg=0,.aid=5,.rw=1};gs->ae=[gs->fn newArgumentEncoderWithBufferIndex:0];gs->arg_len=gs->ae.encodedLength;
    strcpy(gs->key,"123456789abcdef0_b");gs->geo_vertices=4;gs->geo_invocations=2;gs->geo_output=7;gs->geo_stride=4;gs->geo_index_stride=5;
    g_shcache=[NSMutableDictionary dictionary];g_shcache[@"123456789abcdef0_bg0"]=gs;g_shcache[@"123456789abcdef0_bgv"]=shader(l,@"reader",0,rh,1);
    static NSMutableArray *keep;if(!keep)keep=[NSMutableArray array];[keep addObject:pr];[keep addObject:g_shcache[@"123456789abcdef0_bgv"]];
    static prog_t cp[18];cp[mode].serial=18700+mode;serial_put(cp[mode].serial,pr);
    tex_t target={.magic=MAGIC('T','E','X',0),.serial=18800+mode};target.d.width=target.d.height=32;target.d.depth=target.d.samples=target.d.levels=1;target.d.target=NVN_TEXTURE_TARGET_2D;target.d.format=NVN_FORMAT_RGBA8;
    tex_t depth=target;depth.serial=18900+mode;depth.d.format=NVN_FORMAT_DEPTH32F;
    if(mode==14)depth.d.width=64;
    uint8_t *commands; if(posix_memalign((void**)&commands,getpagesize(),65536))abort();memset(commands,0,65536);mtl_pool_add(commands,65536);
    size_t used=0;
    #define EMIT(OP,X) do {size_t z=(8+sizeof(X)+7)&~7ull;op_hdr h={OP,(uint32_t)z};memcpy(commands+used,&h,8);memcpy(commands+used+8,&(X),sizeof(X));used+=z;}while(0)
    p_set_rt rt={.n=1};rt.color[0]=&target;if(mode>=11)rt.depth=&depth;EMIT(OP_SET_RT,rt);
    p_viewports vp={.n=1};vp.v[0][2]=vp.v[0][3]=32;EMIT(OP_VIEWPORTS,vp);p_scissor sc={0,0,32,32};EMIT(OP_SCISSOR,sc);
    p_clear_color cl={.index=0,.mask=15,.rgba={0,0,0,1}};EMIT(OP_CLEAR_COLOR,cl);
    if(mode>=11) {
        p_clear_ds clear={.depth=1,.depth_mask=1};EMIT(OP_CLEAR_DS,clear);
        ds_st ds={.dt=1,.dw=1,.df=NVN_DEPTH_FUNC_LESS};p_bind_state state={.kind=5};memcpy(state.bytes,&ds,sizeof ds);EMIT(OP_BIND_STATE,state);
        chmask_st mask={.mask=mode==15?1:0};state.kind=2;memcpy(state.bytes,&mask,sizeof mask);EMIT(OP_BIND_STATE,state);
    }
    p_bind_program bp={&cp[mode],7};EMIT(OP_BIND_PROGRAM,bp);
    p_bind_buf storage={.stage=2,.index=0,.addr=(uint64_t)(commands+24576),.size=4};EMIT(OP_BIND_SSBO,storage);
    p_draw d={.prim=0,.first=7,.count=mode==6?9000:3,.instances=2,.base_instance=5};
    if((mode>=1&&mode<=3)||mode==5||mode>=7) {
        int type=mode>=7||mode==5?1:mode-1;uint32_t ix[]={mode==10?80:12,10,UINT32_MAX,11,mode==10?80:12,10};
        int index_count=mode>=9?6:4;
        for(int i=0;i<index_count;i++)memcpy(commands+8192+(i<<type),ix+i,1<<type);
        if(mode==7||mode==8) {
            memcpy(commands+32768,commands+8192,8); memset(commands+8192,0,8);
            if(mode==7) { struct {uint64_t src,dst,n;} copy={(uint64_t)(commands+32768),(uint64_t)(commands+8192),8};EMIT(OP_COPY_B2B,copy); }
        }
        p_prim_restart restart={1,UINT32_MAX};EMIT(OP_PRIM_RESTART,restart);
        p_draw_idx di={.prim=0,.itype=type,.count=index_count,.instances=2,.base_vertex=-3,.base_instance=5,.ibuf=(uint64_t)(commands+8192)};
        if(mode==5){uint32_t args[]={4,2,0,(uint32_t)-3,5};memcpy(commands+16384,args,sizeof args);di.ind=(uint64_t)(commands+16384);}
        if(mode==16||mode==17){
            p_bind_buf sb0={.stage=2,.index=0,.addr=(uint64_t)(commands+24576),.size=4},sb1={.stage=2,.index=0,.addr=(uint64_t)(commands+24580),.size=4};
            uint64_t query=NVN_COUNTER_TYPE_SAMPLES_PASSED;
            if(mode==17)EMIT(OP_RESET_COUNTER,query);
            EMIT(OP_BIND_SSBO,sb0);di.count=mode==17?6:2;EMIT(OP_DRAW_IDX,di);
            if(mode==17){
                struct{uint64_t type,addr;} report={query,(uint64_t)(commands+30000)};EMIT(OP_REPORT_COUNTER,report);
                struct{uint64_t addr;uint32_t mode,value;} event={(uint64_t)(commands+30100),0,77};EMIT(OP_SIGNAL_EVENT,event);
                EMIT(OP_RESET_COUNTER,query);
            }
            EMIT(OP_BIND_SSBO,sb1);if(mode==16)di.ibuf=(uint64_t)(commands+8192+(1<<type));di.count=mode==17?6:3;EMIT(OP_DRAW_IDX,di);
            if(mode==17){struct{uint64_t type,addr;} report={query,(uint64_t)(commands+30016)};EMIT(OP_REPORT_COUNTER,report);}
        }
        else EMIT(OP_DRAW_IDX,di);
    } else {
        if(mode==4){uint32_t args[]={3,2,7,5};memcpy(commands+16384,args,sizeof args);d.ind=(uint64_t)(commands+16384);}
        EMIT(OP_DRAW,d);
    }
    if(mode==12||mode==16||mode==17) {
        NvProg *plain=[NvProg new];plain->sh[0]=shader(l,@"cover",0,NULL,0);plain->sh[1]=pr->sh[1];[keep addObject:plain];
        static prog_t pc={.serial=19000};serial_put(pc.serial,plain);p_bind_program bp2={&pc,7};EMIT(OP_BIND_PROGRAM,bp2);
        chmask_st mask={.mask=15};p_bind_state st={.kind=2};memcpy(st.bytes,&mask,sizeof mask);EMIT(OP_BIND_STATE,st);
        p_draw cover={.prim=4,.count=3,.instances=1};EMIT(OP_DRAW,cover);
    }
    op_hdr end={OP_END,8};memcpy(commands+used,&end,8);
    #undef EMIT
    state_reset();memset(&R,0,sizeof R);
    if(mode==8) { mtl_sub_begin();mtl_sub_copy((uint64_t)(commands+32768),(uint64_t)(commands+8192),8);mtl_sub_commit(); }
    uint64_t waits=g_mdi_waits,omits=g_geo_color_omits,passes=g_stat_pass;
    uint64_t small_before=g_geo_pack_small,large_before=g_geo_pack_large;
    int full=g_geo_full_colors;if(mode>=11)g_geo_full_colors=mode==13;
    mtl_sub_begin();uint64_t h=(uint64_t)commands;mtl_sub_index(&h,1);mtl_exec(commands);
    if(mode==0||mode==6){int use_small=mode==0&&!geo_pack_legacy();EXPECT("production draw selects bounded fused path",use_small,g_geo_pack_small-small_before);EXPECT("production large draw retains multi-stage path",!use_small,g_geo_pack_large-large_before);}
    if(mode>=11)EXPECT("only bounded masked geometry omits color",mode>=16?2:mode==11||mode==12?1:0,g_geo_color_omits-omits);
    if(mode>=16) { const char *policy=getenv("DM_NVN_GEOMETRY_WAIT"); if(!(policy&&!strcmp(policy,"always"))) { int split=mode==17&&!geo_query_run();EXPECT("geometry run batches consecutive draws",split?1:2,g_geo_nrun); EXPECT("geometry run shares depth pass before cover pass",split?4:3,g_stat_pass-passes); } }
    g_geo_full_colors=full;
    if((mode>=1&&mode<=3)||mode>=9) { const char *policy=getenv("DM_NVN_GEOMETRY_WAIT"); EXPECT("static index reads avoid GPU drain",policy&&!strcmp(policy,"always")?(mode>=16?2:1):0,g_mdi_waits-waits); }
    if(mode==7) EXPECT("GPU-copied indices retain drain",1,g_mdi_waits-waits);
    id<MTLBuffer> b=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];res_add(b);
    uint32_t *args=NULL;id<MTLBuffer> argsBuffer;
    for(NvArena *a in g_sub_arenas)if(g_geo_args>=a->gpu&&g_geo_args+20<=a->gpu+a->buf.length){argsBuffer=a->buf;args=(uint32_t*)((char*)argsBuffer.contents+g_geo_args-a->gpu);}
    [blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(32,32,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:128 destinationBytesPerImage:4096];
    id<MTLBuffer> db;
    if(mode>=11){db=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];res_add(db);[blit() copyFromTexture:tex_mtl(&depth) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(32,32,1) toBuffer:db destinationOffset:0 destinationBytesPerRow:128 destinationBytesPerImage:4096];}finish();
    EXPECT("two GS invocations per input primitive",mode==6?216000:mode==16?48:mode>=9?120:72,args?args[0]:0);
    int red=0;uint8_t *px=b.contents;for(int i=0;i<1024;i++)if(px[i*4]>200)red++;
    EXPECT("expanded quads rasterized",mode==6?288:mode==10?144:mode==12||mode>=16?808:mode>=11&&mode!=15?0:216,red);
    EXPECT("each instance has independent capture and output",mode>=11&&mode!=15?0:1,px[(24*32+4)*4]>200);
    if(mode>=11){int near=0;float *z=db.contents;for(int i=0;i<1024;i++)near+=z[i]<.25f;EXPECT("masked geometry retains exact depth coverage",216,near);}
    if(mode==16){EXPECT("first draw in geometry run writes its own stage-2 SSBO",1,*(uint32_t*)(commands+24576));EXPECT("second draw in geometry run writes its own stage-2 SSBO",1,*(uint32_t*)(commands+24580));}
    if(mode==17){EXPECT("first geometry query counts only visible raster samples",216,*(uint64_t*)(commands+30000));EXPECT("reset query excludes preceding draw and rejects hidden geometry",0,*(uint64_t*)(commands+30016));EXPECT("query event retains completion value",77,*(uint32_t*)(commands+30100));}
    if(mode==9)for(int step=0;step<3;step++){
        // Same recorded draw: unchanged, then CPU-edited, then restored through
        // a GPU buffer copy. The literal pixel coverage differs on the edit.
        uint64_t hits=g_geo_ix_hits,misses=g_geo_ix_misses,waits0=g_mdi_waits;
        if(step==1){uint16_t value=80;dm_guest_write_prepare(commands+8192,12);memcpy(commands+8192,&value,2);memcpy(commands+8200,&value,2);}
        if(step==2){uint16_t original[]={12,10,65535,11,12,10};dm_guest_write_prepare(commands+32768,12);memcpy(commands+32768,original,12);}
        state_reset();memset(&R,0,sizeof R);mtl_sub_begin();if(step==2)mtl_sub_copy((uint64_t)(commands+32768),(uint64_t)(commands+8192),12);
        mtl_sub_index(&h,1);mtl_exec(commands);
        [blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(32,32,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:128 destinationBytesPerImage:4096];finish();
        int count=0;uint8_t *p=b.contents;for(int i=0;i<1024;i++)count+=p[i*4]>200;
        EXPECT("warm/edit/GPU-copy geometry pixel oracle",step==1?144:216,count);
        if(geo_ix_on()){EXPECT("only unchanged source reuses cached preparation",step==0?1:0,g_geo_ix_hits-hits);EXPECT("edits rebuild prepared indices",step==0?0:1,g_geo_ix_misses-misses);}
        const char *policy=getenv("DM_NVN_GEOMETRY_WAIT");EXPECT("GPU source write still drains before cache validation",step==2|| (policy&&!strcmp(policy,"always"))?1:0,g_mdi_waits-waits0);
    }
    res_remove(b);if(db)res_remove(db);
    mtl_tex_release(&target);if(mode>=11)mtl_tex_release(&depth);
    mtl_pool_del(commands);mtl_drain();free(commands);
}
// Optional real-game fixtures stay outside the repository. The supplied directory
// is a dmsh_build result for the current chain, containing these three GS keys.
static void translated(const char *directory) {
    g_msl_dir=@(directory);g_shcache=[NSMutableDictionary dictionary];
    const char *keys[]={"1b378c5c014e3a32","d57cdb168fdf6927","2341c1be4fcbf690"};
    float *uniforms;if(posix_memalign((void**)&uniforms,getpagesize(),65536))abort();memset(uniforms,0,65536);mtl_pool_add(uniforms,65536);
    NvShader *vs=shader_get(@"ba2512f7f6973541_b",0);
    EXPECT("real capture vertex loaded",1,vs&&vs->geo_fn);
    if(vs&&vs->geo_fn) {
        for(int v=0;v<3;v++)for(int c=0;c<7;c++)uniforms[v*7+c]=(v*7+c)*.03125f;
        id<MTLBuffer> b=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];res_add(b);memset(b.contents,0xcc,4096);
        nvn_geo_capture capture = {b.gpuAddress, 0, {1, 2, 5, 2}, GEO_CAPTURE_WORDS, {0}};
        for (int k = 0; k < GEO_CAPTURE_WORDS; k++) capture.slot[k] = (int16_t)k;
        memcpy((char *)b.contents + 3072, &capture, sizeof capture);
        state_reset();S.ssbo[0][0]=(p_bind_buf){.stage=0,.index=0,.addr=(uint64_t)uniforms,.size=65536};
        mtl_sub_begin();MTL4RenderPassDescriptor *d=[MTL4RenderPassDescriptor new];d.renderTargetWidth=d.renderTargetHeight=d.defaultRasterSampleCount=1;
        id<MTL4RenderCommandEncoder> en=enc_r_begin(d);enc_r_pso(en,geo_capture_pso(vs));args_dirty_all();encode_args(en,vs,0,1);enc_r_vbuf(en,13,b.gpuAddress+3072);
        enc_r_draw_instanced(en,MTLPrimitiveTypePoint,1,2,2,5);enc_r_end(en);finish();
        for(int inst=0;inst<2;inst++)for(int v=0;v<2;v++) {
            float *p=(float*)b.contents+(inst*2+v)*144,*q=uniforms+(v+1)*7;
            EXPECT("capture vertex ID",1,p[0]==q[0]*2-1);EXPECT("capture y",1,p[1]==1-q[1]*2);
            EXPECT("capture retains guest clip depth",1,p[2]==0);EXPECT("capture w",1,p[3]==1);
            EXPECT("capture loc0",1,p[16]==q[2]);for(int c=0;c<4;c++)EXPECT("capture loc1",1,p[20+c]==q[c+3]);
            EXPECT("missing varying default w",1,p[27]==1);
        }
        EXPECT("capture range guard",0xcccccccc,((uint32_t*)b.contents)[4*144]);
    }
    for(int test=0;test<3;test++) {
        NvShader *s=shader_get([NSString stringWithFormat:@"%s_bg%d",keys[test],test==2?3:0],2);
        EXPECT("translated geometry loaded",1,s!=nil);if(!s)continue;
        NSError *e=nil;id<MTLComputePipelineState> ps=[g_dev newComputePipelineStateWithFunction:s->fn error:&e];if(!ps){fprintf(stderr,"%s\n",e.localizedDescription.UTF8String);abort();}
        id<MTLBuffer> b=[g_dev newBufferWithLength:32768 options:MTLResourceStorageModeShared];res_add(b);memset(b.contents,0xcc,32768);
        // Two primitives and two instances: vertex count and primitive count
        // differ for triangles, exposing either stride being used in its place.
        int width=test==2?3:1, vertices=width*2;float *in=b.contents;
        memset(in,0,vertices*2*144*4);
        for(int inst=0;inst<2;inst++)for(int v=0;v<vertices;v++) {
            float *p=in+(inst*vertices+v)*144;p[0]=(v%3==1?1:0)+v/3*3+inst*8;p[1]=v%3==2?1:0;p[2]=.25f;p[3]=1;
            p[16]=p[20]=.4f;p[17]=p[21]=.5f;p[18]=p[22]=.6f;p[19]=p[23]=1;
        }
        uniforms[0]=uniforms[1]=1;uniforms[2]=0;uniforms[3]=0;uniforms[4]=.1f;uniforms[5]=.2f;
        uint32_t vi[8]={(uint32_t)vertices,2,0,0,2,0,0,0};memcpy((char*)b.contents+8192,vi,sizeof vi);
        uint32_t *map=(uint32_t*)((char*)b.contents+8448);for(int v=0;v<vertices;v++)map[v]=v;
        state_reset();S.ubo[2][0]=(p_bind_buf){.stage=2,.index=0,.addr=(uint64_t)uniforms,.size=65536};
        g_geo_bind[0]=b.gpuAddress;g_geo_bind[1]=b.gpuAddress+12288;g_geo_bind[2]=b.gpuAddress+16384;g_geo_bind[3]=b.gpuAddress+8192;g_geo_bind[4]=b.gpuAddress+8448;
        mtl_sub_begin();id<MTL4ComputeCommandEncoder> en=enc_c_begin(NULL);args_dirty_all();encode_args(en,s,2,0);enc_c_pso(en,ps);enc_c_dispatch(en,MTLSizeMake(1,2,1),MTLSizeMake(32,1,1));enc_c_end(en);finish();
        float *out=(float*)((char*)b.contents+12288);uint32_t *ix=(uint32_t*)((char*)b.contents+16384);
        for(int group=0;group<4;group++) {
            for(unsigned v=0;v<s->geo_vertices;v++) {
                EXPECT("emission index and instance stride",group*s->geo_vertices+v,ix[group*s->geo_index_stride+v]);
                float *p=out+(group*s->geo_vertices+v)*s->geo_stride;
                int iv=(group/2)*vertices+(group%2)*width+(test==2?v:0);float *q=in+iv*144;
                float x=q[0]+(test==2?0:(v&1)?.3f:-.3f),y=q[1]+(test==2?0:v<2?.6f:-.6f);
                EXPECT("emitted x",1,fabsf(p[0]-x)<1e-5f);EXPECT("emitted y",1,fabsf(p[1]-y)<1e-5f);
                EXPECT("emitted z",1,fabsf(p[2]-.25f)<1e-5f);EXPECT("emitted w",1,p[3]==1);
                EXPECT("default point size",1,p[4]==1);
                if(test==2) {for(int c=0;c<3;c++) {float want=c==v?(v==0?sqrtf(.5f):1):0;EXPECT("wireframe edge distance",1,fabsf(p[9+c]-want)<1e-5f);}}
            }
            for(unsigned v=s->geo_vertices;v<s->geo_index_stride;v++)EXPECT("restart and unused index padding",UINT32_MAX,ix[group*s->geo_index_stride+v]);
        }
        EXPECT("output bound sentinel",0xcccccccc,((uint32_t*)out)[4*s->geo_vertices*s->geo_stride]);
        EXPECT("index bound sentinel",0xcccccccc,ix[4*s->geo_index_stride]);
    }
    mtl_pool_del(uniforms);mtl_drain();free(uniforms);
}
int main(int argc,char **argv) {@autoreleasepool {

    setvbuf(stdout,NULL,_IONBF,0);assembly();capture_indices();index_cache_contract();mtl_init();storage_buffers();cpu_read_proofs();pack_test();fast_pack_test();
    scan_edges();pack_dependency_test(3);pack_dependency_test(257);pack_dependency_test(129032);for(int mode=0;mode<18;mode++)replay(mode);for(int mode=0;mode<18;mode++)replay_local(mode);
    if(argc>2&&!strcmp(argv[1],"--passthrough-layouts")) {
        NSData *data=[NSData dataWithContentsOfFile:@(argv[2])];NSArray *layouts=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
        passthrough_bounds(layouts);
    } else if(argc>2&&!strcmp(argv[1],"--passthrough-kernels"))passthrough_kernels(argv[2]);
    else if(argc>1)translated(argv[1]);
    printf("t_geometry: %d checks, %d failed\n",checks,failed);return failed!=0;
}}
