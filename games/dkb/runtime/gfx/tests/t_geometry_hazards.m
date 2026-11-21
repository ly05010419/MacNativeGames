// Actual GPU dependency: GS writes a counter, next vertex capture reads it.
// Rebinding that memory as a uniform must retain the same ordering.
#define main geometry_fixture_main
#include "t_geometry.m"
#undef main
// Independent scalar oracle: a job may join a run only when no pair of its
// ranges and an earlier job's ranges overlaps with at least one of them a write.
static __attribute__((noinline)) int access_reference(const geo_access *a,const geo_access *b) {
    if(a->unknown||b->unknown)return 1;
    for(int i=0;i<a->n;i++)for(int j=0;j<b->n;j++)
        if((a->r[i].write||b->r[j].write)&&a->r[i].lo<b->r[j].hi&&b->r[j].lo<a->r[i].hi)return 1;
    return 0;
}
static int run_reference(const geo_access *jobs,int n,const geo_access *b) {for(int i=0;i<n;i++)if(access_reference(&jobs[i],b))return 1;return 0;}
static geo_run_access run_u;
static int run_candidate(const geo_access *jobs,int n,const geo_access *b) {
    run_u.w.n=run_u.r.n=run_u.unknown=0;for(int i=0;i<n;i++)geo_run_note(&run_u,&jobs[i]);return geo_run_conflict(&run_u,b);
}
static uint32_t access_random(uint32_t *s){*s^=*s<<13;*s^=*s>>17;*s^=*s<<5;return *s;}
static void access_contract(void) {
    uint32_t seed=0x91b73e42;int mismatches=0,conflicts=0;static geo_access jobs[GEO_RUN];
    for(int trial=0;trial<4096;trial++) {
        int n=1+access_random(&seed)%(trial&1?4:GEO_RUN-1);memset(jobs,0,sizeof jobs);
        for(int side=0;side<=n;side++) {
            unsigned k=access_random(&seed)%(trial%5==0?66:12);
            for(unsigned q=0;q<k;q++) {
                // dense addresses make touching, nested and partly overlapping ranges common
                uint64_t lo=0x1000+(access_random(&seed)%(trial&2?4096:512))*32ull;
                if((trial&7)==1&&side==n)lo+=1ull<<32;
                int write=(trial&7)==0?0:(access_random(&seed)%(trial&4?64:8))==0;
                geo_access_add(&jobs[side],lo,1+access_random(&seed)%256,write);
            }
        }
        if(trial%29==0)geo_access_add(&jobs[access_random(&seed)%(n+1)],UINT64_MAX-3,8,1);
        if(trial%31==0)geo_access_add(&jobs[n],0x1000,0,0);
        int c=run_candidate(jobs,n,&jobs[n]);conflicts+=c;
        mismatches+=c!=run_reference(jobs,n,&jobs[n]);
    }
    EXPECT("4096 runs agree with scalar dependency oracle",0,mismatches);
    EXPECT("random runs cover both outcomes",1,conflicts>200&&conflicts<3900);
    geo_access a={0},b={0};for(int i=0;i<64;i++)geo_access_add(&a,0x1000+i*64,32,0);
    EXPECT("64 read ranges do not conflict",0,run_candidate(&a,1,&a));
    geo_access_add(&b,0x1000+63*64+32,32,1);
    EXPECT("adjacent half-open ranges stay independent",0,run_candidate(&a,1,&b));
    EXPECT("adjacent write before reads stays independent",0,run_candidate(&b,1,&a));
    b=(geo_access){0};geo_access_add(&b,0x1000+63*64+31,32,1);
    EXPECT("one-byte overlap with range 63 is detected",1,run_candidate(&a,1,&b));
    geo_access_add(&a,0x1000+63*64,32,1);
    EXPECT("duplicate range promoted to write sets bit 63",1ull<<63,a.writes);
    b=(geo_access){0};geo_access_add(&b,0x1000+63*64,1,0);
    EXPECT("promoted write conflicts with later read",1,run_candidate(&a,1,&b));
    geo_access w[2]={0};geo_access_add(&w[0],0x2000,16,1);geo_access_add(&w[1],0x2010,16,1);b=(geo_access){0};geo_access_add(&b,0x200f,2,0);
    EXPECT("read across two merged writes is detected",1,run_candidate(w,2,&b));
    b=(geo_access){0};geo_access_add(&b,0x2020,16,0);
    EXPECT("read just past merged writes stays independent",0,run_candidate(w,2,&b));
    geo_access_add(&a,0x100000,1,0);
    EXPECT("range overflow remains conservative",1,run_candidate(&a,1,&(geo_access){0}));
    EXPECT("earlier overflow remains conservative",1,run_candidate(w,2,&a));
}
// After a drain in this submission, a CPU read waits only for writes encoded after it (drw_valid)
static void drain_contract(void) {
    pend_reset(); g_sub_serial = 41; g_drain_sub = ~0ull;
    sbw_note(0x10000, 64, SBW_GPU);
    EXPECT("no drain: a write earlier in the submission is a hazard",1,geo_cpu_hazard(0x10000,4));
    drw_drained();
    EXPECT("drained: an earlier write no longer counts",0,geo_cpu_hazard(0x10000,4));
    EXPECT("the drain re-encodes bindings",1,g_args_dirty[0]&&g_args_dirty[2]);
    sbw_note(0x20000, 16, SBW_GPU);
    EXPECT("a storage write after the drain counts",1,geo_cpu_hazard(0x2000c,4));
    EXPECT("only where it wrote",0,geo_cpu_hazard(0x20010,4));
    pend_add(0x30000, 8, 2);
    EXPECT("a copy after the drain counts",1,geo_cpu_hazard(0x30004,4));
    g_wr_epoch++; g_wr_geo++;
    EXPECT("a geometry kernel's epoch keeps the list",1,drw_valid());
    g_wr_epoch++;
    EXPECT("another epoch bump (feedback) drops it",0,drw_valid());
    EXPECT("then the submission-wide sets answer",1,geo_cpu_hazard(0x10000,4));
    drw_drained(); sbw_note(0x40000, 0, SBW_GPU);
    EXPECT("a write of unknown size drops the list",0,drw_valid());
    drw_drained(); for (int i = 0; i <= DRW_N; i++) sbw_note(0x1000000 + (uint64_t)i * 0x2000, 16, SBW_GPU);
    EXPECT("an overflowing list drops it",0,drw_valid());
    drw_drained(); for (int i = 0; i < 4 * DRW_N; i++) sbw_note(0x1000000 + (uint64_t)i * 16, 16, SBW_GPU);
    EXPECT("adjacent writes merge instead of overflowing",1,drw_valid()&&g_ndrw==1);
    g_sub_serial++;
    EXPECT("a new submission has no drain",0,drw_valid());
    pend_reset();
}
static int access_benchmark(void) {
    // the shape of Donkey Kong Bananza's runs: shared reads, one private write each, no conflict
    static geo_access jobs[GEO_RUN];for(int i=0;i<GEO_RUN;i++) {
        for(int j=0;j<32;j++)geo_access_add(&jobs[i],0x1000+j*64,32,0);
        geo_access_add(&jobs[i],0x100000+i*4096,32,1);
    }
    volatile unsigned sink=0;
    for(int trial=0;trial<12;trial++) {
        double times[2];for(int turn=0;turn<2;turn++) {
            int mode=(trial&1)?1-turn:turn;
            uint64_t t=bp_now();unsigned sum=0;
            for(int k=0;k<200;k++) {   // one whole run, each job checked against the jobs before it
                if(mode){run_u.w.n=run_u.r.n=run_u.unknown=0;geo_run_note(&run_u,&jobs[0]);for(int i=1;i<GEO_RUN;i++){sum+=geo_run_conflict(&run_u,&jobs[i]);geo_run_note(&run_u,&jobs[i]);}}
                else for(int i=1;i<GEO_RUN;i++)sum+=run_reference(jobs,i,&jobs[i]);
            }
            times[mode]=(bp_now()-t)/1e6;sink+=sum;
        }
        printf("ACCESS_BENCH %d reference_ms %.6f candidate_ms %.6f\n",trial,times[0],times[1]);
    }
    return sink!=0;
}
// A stream bound with size 0 reads no guest memory, wherever it points: it is in no list of what a draw reads
static void empty_stream_contract(void) {
    NvProg *pr=[NvProg new];state_reset();
    S.vbuf[3]=(NVNbufferRange){1,0};S.vbuf[4]=(NVNbufferRange){0x1000,0};
    geo_access a=geo_access_of(pr,nil);
    EXPECT("empty streams are no unknown access",0,a.unknown);EXPECT("empty streams add no range",0,a.n);
    static csh_rng r[CSH_RN];int n=0;p_draw d={.count=1,.instances=1};
    EXPECT("a draw with empty streams can move compute past it",1,csh_draw_reads(pr,&d,NULL,r,&n));EXPECT("its list holds no empty stream",0,n);
    S.vbuf[5]=(NVNbufferRange){0x2000,64};a=geo_access_of(pr,nil);n=0;
    EXPECT("a stream with data stays a known range",1,a.n==1&&!a.unknown);
    EXPECT("a stream with data stays listed",1,csh_draw_reads(pr,&d,NULL,r,&n)&&n==1);
    state_reset();
}
enum { DEP_STORAGE, DEP_UNIFORM, RUN_EMPTY_STREAM, NORUN_UNALIGNED_STREAM };
static void dependency(int mode) {
    int uniform=mode==DEP_UNIFORM,independent=mode>=RUN_EMPTY_STREAM;   // independent: the two draws touch different memory
    id<MTLLibrary> l=library(@"#include <metal_stdlib>\nusing namespace metal;\n"
      "struct B{device uint*p[[id(0)]];};struct C{device float*dst;uint4 info;};\n"
      "vertex float4 unused(constant B&b[[buffer(0)]]){return float4(float(b.p[0]),0,0,1);}\n"
      "vertex void capture(uint v[[vertex_id]],constant B&b[[buffer(0)]],constant C&c[[buffer(13)]]){c.dst[0]=float(b.p[0]);}\n"
      "struct G{device float*in[[id(0)]];device float4*out[[id(1)]];device uint*ix[[id(2)]];constant uint4*info[[id(3)]];device uint*map[[id(4)]];device uint*p[[id(5)]];};\n"
      "kernel void geometry(constant G&g[[buffer(0)]],uint3 t[[thread_position_in_grid]]){if(t.x||t.y||t.z)return;float x=g.in[0]*.8-.8;g.p[0]=uint(g.in[0])+1;g.out[0]=float4(x,-.5,0,1);g.out[1]=float4(x+.6,-.5,0,1);g.out[2]=float4(x,.5,0,1);g.ix[0]=0;g.ix[1]=1;g.ix[2]=2;g.ix[3]=0xffffffffu;}\n"
      "struct V{device float4*p[[id(0)]];};vertex float4 reader(uint v[[vertex_id]],constant V&a[[buffer(0)]]){return a.p[v];}\nfragment float4 red(){return float4(1,0,0,1);}\n");
    int helpers[]={0,1,2,3,4},rh[]={1};NvProg *pr=[NvProg new];
    pr->sh[0]=shader(l,@"unused",0,NULL,0);NvShader *vs=pr->sh[0];vs->geo_fn=[l newFunctionWithName:@"capture"];
    vs->rf.nb=1;vs->rf.b[0]=(nvn_rbind){.kind=uniform?NVN_RB_CBUF:NVN_RB_SBUFP,.reg=0,.aid=0};
    vs->ae=[vs->geo_fn newArgumentEncoderWithBufferIndex:0];vs->arg_len=vs->ae.encodedLength;
    pr->sh[1]=shader(l,@"red",1,NULL,0);pr->sh[2]=shader(l,@"geometry",2,helpers,5);NvShader *gs=pr->sh[2];
    gs->rf.b[gs->rf.nb++]=(nvn_rbind){.kind=NVN_RB_SBUFP,.reg=0,.aid=5,.rw=1};gs->ae=[gs->fn newArgumentEncoderWithBufferIndex:0];gs->arg_len=gs->ae.encodedLength;
    strcpy(gs->key,"fedcba9876543210_b");gs->geo_vertices=3;gs->geo_invocations=1;gs->geo_output=7;gs->geo_stride=4;gs->geo_index_stride=4;
    g_shcache=[NSMutableDictionary dictionary];g_shcache[@"fedcba9876543210_bg0"]=gs;g_shcache[@"fedcba9876543210_bgv"]=shader(l,@"reader",0,rh,1);
    prog_t cp={.serial=9700+mode};serial_put(cp.serial,pr);
    uint8_t *cmd;posix_memalign((void**)&cmd,getpagesize(),65536);memset(cmd,0,65536);mtl_pool_add(cmd,65536);size_t used=0;   // NOLINT(clang-analyzer-osx.cocoa.RetainCount): g_by_serial owns pr (serial_put's CFBridgingRetain) for the life of the test, not a leak
    #define EMIT(OP,X) do{size_t z=(8+sizeof(X)+7)&~7ull;op_hdr h={OP,(uint32_t)z};memcpy(cmd+used,&h,8);memcpy(cmd+used+8,&(X),sizeof(X));used+=z;}while(0)
    tex_t target={.magic=MAGIC('T','E','X',0),.serial=9800+mode,.d={.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=32,.height=32,.depth=1,.levels=1,.samples=1}};
    p_set_rt rt={.n=1};rt.color[0]=&target;EMIT(OP_SET_RT,rt);p_viewports vp={.n=1};vp.v[0][2]=vp.v[0][3]=32;EMIT(OP_VIEWPORTS,vp);
    p_clear_color cl={.mask=15,.rgba={0,0,0,1}};EMIT(OP_CLEAR_COLOR,cl);p_bind_program bp={&cp,7};EMIT(OP_BIND_PROGRAM,bp);
    p_bind_buf bind={.stage=0,.index=0,.addr=(uint64_t)(cmd+32768),.size=4};if(uniform){EMIT(OP_BIND_UBO,bind);}else{EMIT(OP_BIND_SSBO,bind);}bind.stage=2;EMIT(OP_BIND_SSBO,bind);
    if(independent){p_bind_vbuf vb={.first=3,.n=1};vb.r[0]=mode==RUN_EMPTY_STREAM?(NVNbufferRange){1,0}:(NVNbufferRange){(uint64_t)(cmd+24577),16};EMIT(OP_BIND_VBUF,vb);}
    p_draw d={.prim=0,.count=1,.instances=1};EMIT(OP_DRAW,d);
    if(independent){bind.stage=0;bind.addr+=16;EMIT(OP_BIND_SSBO,bind);bind.stage=2;EMIT(OP_BIND_SSBO,bind);}
    EMIT(OP_DRAW,d);op_hdr end={OP_END,8};memcpy(cmd+used,&end,8);
    #undef EMIT
    state_reset();memset(&R,0,sizeof R);mtl_set_frame(100+mode);mtl_sub_begin();uint64_t cuts=g_cuts,h=(uint64_t)cmd;mtl_sub_index(&h,1);mtl_exec(cmd);
    id<MTLBuffer> rb=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];EXPECT("readback buffer",1,rb!=nil);if(!rb)abort();[blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(32,32,1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:128 destinationBytesPerImage:4096];finish();
    uint8_t *px=rb.contents;
    if(independent) {
        EXPECT("first draw wrote its own storage",1,*(uint32_t*)(cmd+32768));EXPECT("second draw wrote its own storage",1,*(uint32_t*)(cmd+32768+16));
        EXPECT("the draws cover their position",1,px[(14*32+5)*4]>200);
        EXPECT(mode==RUN_EMPTY_STREAM?"an empty stream at an unaligned address does not keep draws out of a run":"a stream with data at an unaligned address stays out of a run",mode==RUN_EMPTY_STREAM?2:1,g_geo_nrun);
    } else {
        EXPECT("next capture sees earlier GS write",2,*(uint32_t*)(cmd+32768));
        EXPECT("first draw covers original position",1,px[(14*32+5)*4]>200);EXPECT("dependent draw covers updated position",1,px[(14*32+18)*4]>200);
        EXPECT("dependent geometry does not join earlier run",1,g_geo_nrun);
        EXPECT("geometry boundary submits early only when enabled",cut_draws()==1&&geo_cut_on(),g_cuts>cuts);
    }
    mtl_tex_release(&target);mtl_pool_del(cmd);mtl_drain();free(cmd);
}
int main(int argc,char **argv){@autoreleasepool{
    if(argc>1&&!strcmp(argv[1],"--access-bench"))return access_benchmark();
    access_contract();drain_contract();mtl_init();empty_stream_contract();dependency(DEP_STORAGE);dependency(DEP_UNIFORM);dependency(RUN_EMPTY_STREAM);dependency(NORUN_UNALIGNED_STREAM);
    printf("t_geometry_hazards: %d checks, %d failed\n",checks,failed);return failed!=0;
}}
