// Eight color attachments plus independently tracked depth and stencil: ten entries.
// A vertex-only producer/consumer pair must still run after attachment write tracking.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)
static uint8_t *commands; static size_t used;
static void emit(unsigned op, const void *p, size_t n) {
    size_t size=(8+n+7)&~7ull; op_hdr h={op,(uint32_t)size};
    memcpy(commands+used,&h,8); if(n)memcpy(commands+used+8,p,n); used+=size;
}
static NSData *plane(const tex_t *t, unsigned bpp, MTLBlitOption option) {
    id<MTLTexture> source = tex_mtl(t);
    CHECK(source != nil,"attachment exists for readback"); if (!source) exit(1);
    id<MTLBuffer> b=[g_dev newBufferWithLength:2048 options:MTLResourceStorageModeShared];
    CHECK(b != nil,"readback buffer allocates"); if (!b) exit(1);
    id<MTLCommandBuffer> cb=q_cb(); id<MTLBlitCommandEncoder> e=[cb blitCommandEncoder];
    [e copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1)
        toBuffer:b destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:2048 options:option];
    [e endEncoding];[cb commit];[cb waitUntilCompleted];
    CHECK(cb.status==MTLCommandBufferStatusCompleted,"attachment readback completes");
    NSMutableData *out=[NSMutableData dataWithLength:64*bpp];
    for(unsigned y=0;y<8;y++)memcpy((uint8_t *)out.mutableBytes+y*8*bpp,(uint8_t *)b.contents+y*256,8*bpp);
    return out;
}
int main(void) { @autoreleasepool {
    setenv("DM_PSO_ARCHIVE","off",1);setenv("DM_PSO_MANIFEST","off",1);setenv("DM_PSO_USER","/dev/null",1);
    setenv("DM_PSO_PREBUILD","0",1);setenv("DM_PSO_HYBRID","0",1);setenv("DM_NVN_PREDICT","0",1);
    mtl_init();
    void *memory=NULL;if(posix_memalign(&memory,getpagesize(),65536))abort();memset(memory,0,65536);mtl_pool_add(memory,65536);
    commands=memory;uint32_t *output=(uint32_t *)((uint8_t *)memory+32768);
    NSError *error=nil;id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct A{device uint* p[[id(0)]];};\n"
        "vertex float4 v(){return float4(0,0,0,1);}\n"
        "vertex void producer(uint n[[vertex_id]],constant A&a[[buffer(0)]]){a.p[n]=n+0x1200;}\n"
        "vertex void consumer(uint n[[vertex_id]],constant A&a[[buffer(0)]]){a.p[n+4096]=a.p[n];}\n" options:nil error:&error];
    CHECK(lib!=nil,"synthetic vertex shaders compile");if(!lib)return 1;
    static prog_t pg[2]={{.serial=982001},{.serial=982002}};
    // NOLINTBEGIN(clang-analyzer-osx.cocoa.RetainCount): serial_put retains each program in g_by_serial through hm_put_id's CFBridgingRetain
    for(int i=0;i<2;i++) {
        NvProg *p=[NvProg new];NvShader *s=[NvShader new];
        s->fn=fn4_note([lib newFunctionWithName:@"v"],lib,@"v",nil);
        NSString *entry=i?@"consumer":@"producer";
        s->discard_fn=fn4_note([lib newFunctionWithName:entry],lib,entry,nil);
        s->ae=[s->discard_fn newArgumentEncoderWithBufferIndex:0];
        s->rf.nb=1;s->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=1,.reg=0,.aid=0};
        p->sh[0]=s;serial_put(pg[i].serial,p);
    }
    // NOLINTEND(clang-analyzer-osx.cocoa.RetainCount)
    tex_t colors[8]={{0}},depth={.magic=MAGIC('T','E','X',0),.serial=982020,.off=16*65536};
    depth.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_DEPTH32F_STENCIL8,.width=8,.height=8,.depth=1,.levels=1,.samples=1};
    state_reset();memset(&R,0,sizeof R);
    p_set_rt rt={.n=8,.depth=&depth};
    for(int i=0;i<8;i++){colors[i]=(tex_t){.magic=MAGIC('T','E','X',0),.serial=982010+i,.off=(i+1)*65536};colors[i].d=depth.d;colors[i].d.format=NVN_FORMAT_RGBA8;rt.color[i]=&colors[i];}
    emit(OP_SET_RT,&rt,sizeof rt);
    for(int i=0;i<8;i++) {
        p_clear_color c={.index=i,.rgba={(i&1)!=0,(i&2)!=0,(i&4)!=0,1},.mask=15};emit(OP_CLEAR_COLOR,&c,sizeof c);
    }
    p_clear_ds clear={.depth=.75,.stencil=0xa5,.depth_mask=1,.stencil_mask=255};emit(OP_CLEAR_DS,&clear,sizeof clear);
    ds_st ds={.dt=1,.dw=1,.df=NVN_DEPTH_FUNC_ALWAYS,.st=1,.sf_f=NVN_STENCIL_FUNC_ALWAYS,.sf_b=NVN_STENCIL_FUNC_ALWAYS,
        .fail_f=NVN_STENCIL_OP_KEEP,.zfail_f=NVN_STENCIL_OP_KEEP,.zpass_f=NVN_STENCIL_OP_KEEP,
        .fail_b=NVN_STENCIL_OP_KEEP,.zfail_b=NVN_STENCIL_OP_KEEP,.zpass_b=NVN_STENCIL_OP_KEEP};
    p_bind_state state={.kind=5};memcpy(state.bytes,&ds,sizeof ds);emit(OP_BIND_STATE,&state,sizeof state);
    p_bind_buf buffer={.stage=0,.index=0,.addr=(uint64_t)output,.size=32768};emit(OP_BIND_SSBO,&buffer,sizeof buffer);
    p_rast_discard discard={.on=1};emit(OP_RAST_DISCARD,&discard,sizeof discard);
    p_draw draw={.prim=NVN_DRAW_PRIMITIVE_POINTS,.count=4096,.instances=1};
    p_bind_program bind={.prog=&pg[0],.stages=3};emit(OP_BIND_PROGRAM,&bind,sizeof bind);emit(OP_DRAW,&draw,sizeof draw);
    int32_t barrier=NVN_BARRIER_ORDER_PRIMITIVES_BIT;emit(OP_BARRIER,&barrier,sizeof barrier);
    bind.prog=&pg[1];emit(OP_BIND_PROGRAM,&bind,sizeof bind);emit(OP_DRAW,&draw,sizeof draw);emit(OP_END,NULL,0);
    mtl_sub_begin();uint64_t handle=(uint64_t)commands;mtl_sub_index(&handle,1);mtl_exec(commands);
    CHECK(g_nsd==10,"eight colors, depth and stencil are all tracked");
    CHECK(g_sd[8].depth==1&&g_sd[9].depth==2,"last entries retain separate depth/stencil identities");
    CHECK(g_pass.dfmt==260&&g_pass.sfmt==260,"write tracking preserves pipeline formats after both vertex draws");
    CHECK(g_pso_fail.n==0,"both vertex-only pipelines build");
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"GPU submission completes");CHECK(!mtl_last_commit_error(),"GPU reports no error");
    int good=1;for(int i=0;i<4096;i++)good&=output[i]==(uint32_t)(i+0x1200)&&output[i+4096]==(uint32_t)(i+0x1200);
    CHECK(good,"all producer and consumer vertices execute with ordered storage writes");
    for(int i=0;i<8;i++) {
        NSData *data=plane(&colors[i],4,MTLBlitOptionNone);const uint8_t *p=data.bytes;good=1;
        for(int j=0;j<64;j++)good&=p[4*j]==((i&1)?255:0)&&p[4*j+1]==((i&2)?255:0)&&p[4*j+2]==((i&4)?255:0)&&p[4*j+3]==255;
        CHECK(good,"every color attachment preserves its literal clear through vertex-only work");
    }
    NSData *d=plane(&depth,4,MTLBlitOptionDepthFromDepthStencil),*s=plane(&depth,1,MTLBlitOptionStencilFromDepthStencil);
    const float *z=d.bytes;const uint8_t *st=s.bytes;good=1;
    for(int i=0;i<64;i++)good&=z[i]==.75f&&st[i]==0xa5;
    CHECK(good,"both depth and stencil planes preserve their literal clear");
    printf("attachment capacity: %d checks, %d failed (TBDR=%d)\n",checks,failed,tbdr_on());return failed!=0;
} }
