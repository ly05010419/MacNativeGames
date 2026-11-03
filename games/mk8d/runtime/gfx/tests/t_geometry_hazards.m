// Actual GPU dependency: GS writes a counter, next vertex capture reads it.
// Rebinding that memory as a uniform must retain the same ordering.
#define main geometry_fixture_main
#include "t_geometry.m"
#undef main
// Independent scalar oracle: the pre-optimization predicate examines every
// pair, including read/read pairs. The optimized write mask must agree exactly.
static __attribute__((noinline)) int access_reference(const geo_access *a,const geo_access *b) {
    if(a->unknown||b->unknown)return 1;
    for(int i=0;i<a->n;i++)for(int j=0;j<b->n;j++)
        if((a->r[i].write||b->r[j].write)&&a->r[i].lo<b->r[j].hi&&b->r[j].lo<a->r[i].hi)return 1;
    return 0;
}
static __attribute__((noinline)) int access_candidate(const geo_access *a,const geo_access *b) {return geo_access_conflict(a,b);}
static uint32_t access_random(uint32_t *s){*s^=*s<<13;*s^=*s>>17;*s^=*s<<5;return *s;}
static void access_contract(void) {
    uint32_t seed=0x91b73e42;int mismatches=0,asymmetry=0;
    for(int trial=0;trial<4096;trial++) {
        geo_access a={0},b={0};geo_access *v[2]={&a,&b};
        for(int side=0;side<2;side++) {
            unsigned n=access_random(&seed)%66;
            for(unsigned k=0;k<n;k++) {
                uint64_t lo=0x1000+(access_random(&seed)%512)*32ull;
                if((trial&7)==1&&side)lo+=1ull<<32;
                int write=(trial&7)==0?0:(access_random(&seed)%8)==0;
                geo_access_add(v[side],lo,1+access_random(&seed)%256,write);
            }
        }
        if(trial%29==0)geo_access_add(&a,UINT64_MAX-3,8,1);
        if(trial%31==0)geo_access_add(&b,0x1000,0,0);
        mismatches+=access_candidate(&a,&b)!=access_reference(&a,&b);
        asymmetry+=access_candidate(&a,&b)!=access_candidate(&b,&a);
    }
    EXPECT("4096 access sets agree with scalar dependency oracle",0,mismatches);
    EXPECT("dependency relation remains symmetric",0,asymmetry);
    geo_access a={0},b={0};for(int i=0;i<64;i++)geo_access_add(&a,0x1000+i*64,32,0);
    EXPECT("64 read ranges do not conflict",0,access_candidate(&a,&a));
    geo_access_add(&b,0x1000+63*64+32,32,1);
    EXPECT("adjacent half-open ranges stay independent",0,access_candidate(&a,&b));
    b=(geo_access){0};geo_access_add(&b,0x1000+63*64+31,32,1);
    EXPECT("one-byte overlap with range 63 is detected",1,access_candidate(&a,&b));
    geo_access_add(&a,0x1000+63*64,32,1);
    EXPECT("duplicate range promoted to write sets bit 63",1ull<<63,a.writes);
    b=(geo_access){0};geo_access_add(&b,0x1000+63*64,1,0);
    EXPECT("promoted write conflicts with later read",1,access_candidate(&a,&b));
    geo_access_add(&a,0x100000,1,0);
    EXPECT("range overflow remains conservative",1,access_candidate(&a,&(geo_access){0}));
}
static int access_benchmark(void) {
    static geo_access jobs[64];for(int i=0;i<64;i++) {
        for(int j=0;j<32;j++)geo_access_add(&jobs[i],0x1000+j*64,32,0);
        geo_access_add(&jobs[i],0x100000+i*4096,32,1);
    }
    volatile unsigned sink=0;
    for(int trial=0;trial<12;trial++) {
        double times[2];for(int turn=0;turn<2;turn++) {
            int mode=(trial&1)?1-turn:turn;
            int (*volatile f)(const geo_access*,const geo_access*)=mode?access_candidate:access_reference;
            uint64_t t=bp_now();unsigned sum=0;
            for(unsigned k=0;k<60000;k++){unsigned i=k%64,j=(i+1+(k/64)%63)%64;sum+=f(&jobs[i],&jobs[j]);}
            times[mode]=(bp_now()-t)/1e6;sink+=sum;
        }
        printf("ACCESS_BENCH %d reference_ms %.6f candidate_ms %.6f\n",trial,times[0],times[1]);
    }
    return sink!=0;
}
static void dependency(int uniform) {
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
    prog_t cp={.serial=9700+uniform};serial_put(cp.serial,pr);
    uint8_t *cmd;posix_memalign((void**)&cmd,getpagesize(),65536);memset(cmd,0,65536);mtl_pool_add(cmd,65536);size_t used=0;
    #define EMIT(OP,X) do{size_t z=(8+sizeof(X)+7)&~7ull;op_hdr h={OP,(uint32_t)z};memcpy(cmd+used,&h,8);memcpy(cmd+used+8,&(X),sizeof(X));used+=z;}while(0)
    tex_t target={.magic=MAGIC('T','E','X',0),.serial=9800+uniform,.d={.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=32,.height=32,.depth=1,.levels=1,.samples=1}};
    p_set_rt rt={.n=1};rt.color[0]=&target;EMIT(OP_SET_RT,rt);p_viewports vp={.n=1};vp.v[0][2]=vp.v[0][3]=32;EMIT(OP_VIEWPORTS,vp);
    p_clear_color cl={.mask=15,.rgba={0,0,0,1}};EMIT(OP_CLEAR_COLOR,cl);p_bind_program bp={&cp,7};EMIT(OP_BIND_PROGRAM,bp);
    p_bind_buf bind={.stage=0,.index=0,.addr=(uint64_t)(cmd+32768),.size=4};if(uniform){EMIT(OP_BIND_UBO,bind);}else{EMIT(OP_BIND_SSBO,bind);}bind.stage=2;EMIT(OP_BIND_SSBO,bind);
    p_draw d={.prim=0,.count=1,.instances=1};EMIT(OP_DRAW,d);EMIT(OP_DRAW,d);op_hdr end={OP_END,8};memcpy(cmd+used,&end,8);
    #undef EMIT
    state_reset();memset(&R,0,sizeof R);mtl_set_frame(100+uniform);mtl_sub_begin();uint64_t cuts=g_cuts,h=(uint64_t)cmd;mtl_sub_index(&h,1);mtl_exec(cmd);
    id<MTLBuffer> rb=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];[blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(32,32,1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:128 destinationBytesPerImage:4096];finish();
    EXPECT("next capture sees earlier GS write",2,*(uint32_t*)(cmd+32768));
    uint8_t *px=rb.contents;EXPECT("first draw covers original position",1,px[(14*32+5)*4]>200);EXPECT("dependent draw covers updated position",1,px[(14*32+18)*4]>200);
    EXPECT("dependent geometry does not join earlier run",1,g_geo_nrun);
    EXPECT("geometry boundary submits early only when enabled",cut_draws()==1&&geo_cut_on(),g_cuts>cuts);
    mtl_tex_release(&target);mtl_pool_del(cmd);mtl_drain();free(cmd);
}
int main(int argc,char **argv){@autoreleasepool{
    if(argc>1&&!strcmp(argv[1],"--access-bench"))return access_benchmark();
    access_contract();mtl_init();dependency(0);dependency(1);
    printf("t_geometry_hazards: %d checks, %d failed\n",checks,failed);return failed!=0;
}}
