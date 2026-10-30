// Geometry assembly, GPU strip compaction, and production capture/compute/raster replay.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
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
static void pack_test(void) {
    id<MTLLibrary> l=library(@(k_nvn_geo_pack_msl)); NSError *e=nil;
    id<MTLComputePipelineState> p=[g_dev newComputePipelineStateWithFunction:[l newFunctionWithName:@"geo_pack_indices"] error:&e];
    if(!p) abort();
    uint32_t data[]={0,1,2,3,UINT32_MAX,4,5,6, UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX, 8,9,10,UINT32_MAX,11,12,13,UINT32_MAX};
    uint32_t want[]={0,1,2,2,1,3,4,5,6,8,9,10,11,12,13};
    id<MTLBuffer> b=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; res_add(b);
    for(int t=0;t<3;t++) {
        memset(b.contents,0xcc,4096); memcpy(b.contents,data,sizeof data);
        struct {uint64_t src,dst,args;uint32_t groups,stride,topology,capacity;} j={b.gpuAddress,b.gpuAddress+512,b.gpuAddress+1024,3,8,t==0?7:t==1?6:1,100};
        memcpy((char*)b.contents+2048,&j,sizeof j);
        mtl_sub_begin(); id<MTL4ComputeCommandEncoder> en=enc_c_begin(NULL); enc_c_pso(en,p);enc_c_args(en,b.gpuAddress+2048,0);
        enc_c_dispatch(en,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1));enc_c_end(en);finish();
        uint32_t *args=(uint32_t*)((char*)b.contents+1024);
        EXPECT("compacted index count",t==0?15:t==1?18:13,args[0]);
        EXPECT("indirect instance count",1,args[1]);
        if(!t) EXPECT("strip order across empty invocations",0,memcmp((char*)b.contents+512,want,sizeof want));
        EXPECT("output bound sentinel",0xcccccccc,((uint32_t*)((char*)b.contents+512))[args[0]]);
    }
}
static NvShader *shader(id<MTLLibrary> l, NSString *name, int stage, const int *helpers, int n) {
    NvShader *s=[NvShader new];s->stage=stage;s->fn=[l newFunctionWithName:name];s->rf.nb=n;
    if(n) {s->ae=[s->fn newArgumentEncoderWithBufferIndex:0];s->arg_len=s->ae.encodedLength;}
    for(int i=0;i<n;i++)s->rf.b[i]=(nvn_rbind){.kind=NVN_RB_GEOMETRY,.reg=helpers[i],.aid=i};
    return s;
}
static void replay(int mode) {
    id<MTLLibrary> l=library(@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct C {device float *dst;uint4 info;};\n"
        "vertex void capture(uint v[[vertex_id]],uint i[[instance_id]],constant C&c[[buffer(13)]]) {uint n=v-c.info.x,inst=i-c.info.z;device float*p=c.dst+(inst*c.info.y+n)*144; p[0]=float(n);p[1]=float(inst);p[2]=0;p[3]=1;}\n"
        "vertex float4 unused(uint v[[vertex_id]]) {return float4(0,0,0,1);}\n"
        "struct G{device const float*in[[id(0)]];device float4*out[[id(1)]];device uint*ix[[id(2)]];constant uint4*info[[id(3)]];device const uint*map[[id(4)]];};\n"
        "kernel void geometry(constant G&g[[buffer(0)]],uint3 t[[thread_position_in_grid]]) {if(t.x>=g.info[1].x||t.y>=g.info[0].y)return;uint k=(t.y*g.info[1].x+t.x)*2+t.z;uint v=g.map[t.x];float2 q=float2(g.in[(t.y*g.info[0].x+v)*144],g.in[(t.y*g.info[0].x+v)*144+1]);float2 a=q*0.5-0.9;float2 d[4]={float2(0),float2(.4,0),float2(0,.4),float2(.4)};for(uint n=0;n<4;n++){g.out[k*4+n]=float4(a+d[n],0,1);g.ix[k*5+n]=k*4+n;}g.ix[k*5+4]=0xffffffffu;}\n"
        "struct V{device const float4*p[[id(0)]];};vertex float4 reader(uint v[[vertex_id]],constant V&a[[buffer(0)]]){return a.p[v];}\n"
        "fragment float4 red(){return float4(1,0,0,1);}\n");
    int helpers[]={0,1,2,3,4}, rh[]={1};
    NvProg *pr=[NvProg new];pr->sh[0]=shader(l,@"unused",0,NULL,0);pr->sh[0]->geo_fn=[l newFunctionWithName:@"capture"];
    pr->sh[1]=shader(l,@"red",1,NULL,0);pr->sh[2]=shader(l,@"geometry",2,helpers,5);
    NvShader *gs=pr->sh[2];strcpy(gs->key,"0123456789abcdef_b");gs->geo_vertices=4;gs->geo_invocations=2;gs->geo_output=7;gs->geo_stride=4;gs->geo_index_stride=5;
    g_shcache=[NSMutableDictionary dictionary];g_shcache[@"0123456789abcdef_bg0"]=gs;g_shcache[@"0123456789abcdef_bgv"]=shader(l,@"reader",0,rh,1);
    static NSMutableArray *keep;if(!keep)keep=[NSMutableArray array];[keep addObject:pr];[keep addObject:g_shcache[@"0123456789abcdef_bgv"]];
    static prog_t cp[7];cp[mode].serial=8700+mode;serial_put(cp[mode].serial,pr);
    tex_t target={.magic=MAGIC('T','E','X',0),.serial=8800+mode};target.d.width=target.d.height=32;target.d.depth=target.d.samples=target.d.levels=1;target.d.target=NVN_TEXTURE_TARGET_2D;target.d.format=NVN_FORMAT_RGBA8;
    uint8_t *commands; if(posix_memalign((void**)&commands,getpagesize(),65536))abort();memset(commands,0,65536);mtl_pool_add(commands,65536);
    size_t used=0;
    #define EMIT(OP,X) do {size_t z=(8+sizeof(X)+7)&~7ull;op_hdr h={OP,(uint32_t)z};memcpy(commands+used,&h,8);memcpy(commands+used+8,&(X),sizeof(X));used+=z;}while(0)
    p_set_rt rt={.n=1};rt.color[0]=&target;EMIT(OP_SET_RT,rt);
    p_viewports vp={.n=1};vp.v[0][2]=vp.v[0][3]=32;EMIT(OP_VIEWPORTS,vp);p_scissor sc={0,0,32,32};EMIT(OP_SCISSOR,sc);
    p_clear_color cl={.index=0,.mask=15,.rgba={0,0,0,1}};EMIT(OP_CLEAR_COLOR,cl);
    p_bind_program bp={&cp[mode],7};EMIT(OP_BIND_PROGRAM,bp);
    p_draw d={.prim=0,.first=7,.count=mode==6?9000:3,.instances=2,.base_instance=5};
    if((mode>=1&&mode<=3)||mode==5) {
        int type=mode==5?1:mode-1;uint32_t ix[]={12,10,UINT32_MAX,11};
        for(int i=0;i<4;i++)memcpy(commands+8192+(i<<type),ix+i,1<<type);
        p_prim_restart restart={1,UINT32_MAX};EMIT(OP_PRIM_RESTART,restart);
        p_draw_idx di={.prim=0,.itype=type,.count=4,.instances=2,.base_vertex=-3,.base_instance=5,.ibuf=(uint64_t)(commands+8192)};
        if(mode==5){uint32_t args[]={4,2,0,(uint32_t)-3,5};memcpy(commands+16384,args,sizeof args);di.ind=(uint64_t)(commands+16384);}
        EMIT(OP_DRAW_IDX,di);
    } else {
        if(mode==4){uint32_t args[]={3,2,7,5};memcpy(commands+16384,args,sizeof args);d.ind=(uint64_t)(commands+16384);}
        EMIT(OP_DRAW,d);
    }
    op_hdr end={OP_END,8};memcpy(commands+used,&end,8);
    #undef EMIT
    state_reset();memset(&R,0,sizeof R);mtl_sub_begin();uint64_t h=(uint64_t)commands;mtl_sub_index(&h,1);mtl_exec(commands);
    id<MTLBuffer> b=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    uint32_t *args=NULL;id<MTLBuffer> argsBuffer;
    for(NvArena *a in g_sub_arenas)if(g_geo_args>=a->gpu&&g_geo_args+20<=a->gpu+a->buf.length){argsBuffer=a->buf;args=(uint32_t*)((char*)argsBuffer.contents+g_geo_args-a->gpu);}
    [blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(32,32,1) toBuffer:b destinationOffset:0 destinationBytesPerRow:128 destinationBytesPerImage:4096];finish();
    EXPECT("two GS invocations per input primitive",mode==6?216000:72,args?args[0]:0);
    int red=0;uint8_t *px=b.contents;for(int i=0;i<1024;i++)if(px[i*4]>200)red++;
    EXPECT("expanded quads rasterized",mode==6?288:216,red);
    EXPECT("each instance has independent capture and output",1,px[(24*32+4)*4]>200);
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
        uint64_t head[2]={b.gpuAddress,0};uint32_t info[4]={1,2,5,2};memcpy((char*)b.contents+3072,head,16);memcpy((char*)b.contents+3088,info,16);
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
static void discard_storage_writes(void) {
    id<MTLLibrary> l=library(@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct A {device uint* dst [[id(0)]];};\n"
        "vertex float4 raster(uint v[[vertex_id]]) {return float4(0,0,0,1);}\n"
        "vertex void discard(constant A&a[[buffer(0)]],uint v[[vertex_id]],uint i[[instance_id]]) {a.dst[(i-5)*8+v]=v*10+i;}\n"
        "vertex void feedback(constant A&a[[buffer(0)]],uint v[[vertex_id]],device uint*tf[[buffer(8)]]) {a.dst[v]=200+v;tf[v]=100+v;}\n");
    NvShader *vs=shader(l,@"raster",0,NULL,0);vs->discard_fn=[l newFunctionWithName:@"discard"];
    vs->tf_fn=[l newFunctionWithName:@"feedback"];vs->tf_stride[0]=4;
    vs->ae=[vs->discard_fn newArgumentEncoderWithBufferIndex:0];vs->arg_len=vs->ae.encodedLength;
    vs->rf.nb=1;vs->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=1,.reg=0,.aid=0};
    [g_keep addObject:vs];
    NvProg *pr=[NvProg new];pr->sh[0]=vs;
    uint32_t *out;if(posix_memalign((void**)&out,getpagesize(),65536))abort();memset(out,0xcc,65536);mtl_pool_add(out,65536);
    state_reset();memset(&R,0,sizeof R);S.rast_discard=1;
    S.ssbo[0][0]=(p_bind_buf){.stage=0,.index=0,.addr=(uint64_t)out,.size=64};
    p_draw d={.prim=0,.first=2,.count=4,.instances=2,.base_instance=5};
    mtl_sub_begin();tf_draw(pr,&d,NULL);
    EXPECT("discard storage write tracked",1,pend_hits((uint64_t)(out+2),4));finish();
    for(int i=0;i<2;i++)for(int v=0;v<8;v++)
        EXPECT("discard runs vertices without feedback",v>=2&&v<6?(uint32_t)(v*10+i+5):0xcccccccc,out[i*8+v]);
    EXPECT("discard output bound sentinel",0xcccccccc,out[16]);
    S.tf_on=1;S.tf[0]=(p_tf_bind){.index=0,.addr=(uint64_t)(out+32),.size=16};
    d.instances=1;d.base_instance=0;mtl_sub_begin();tf_draw(pr,&d,NULL);finish();
    for(int v=0;v<4;v++) {
        EXPECT("feedback selects its capture entry",102u+v,out[32+v]);
        EXPECT("feedback preserves vertex storage writes",202u+v,out[2+v]);
    }
    EXPECT("feedback byte offset",16,S.tf_off[0]);
    EXPECT("feedback output bound sentinel",0xcccccccc,out[36]);
    mtl_pool_del(out);mtl_drain();free(out);
}
int main(int argc,char **argv) {@autoreleasepool {
    setvbuf(stdout,NULL,_IONBF,0);assembly();mtl_init();pack_test();for(int mode=0;mode<7;mode++)replay(mode);
    discard_storage_writes();
    if(argc>1)translated(argv[1]);
    printf("t_geometry: %d checks, %d failed\n",checks,failed);return failed!=0;
}}
