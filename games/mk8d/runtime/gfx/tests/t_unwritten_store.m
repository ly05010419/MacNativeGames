// Read back all three attachment planes after production replay, with lookahead disabled.
// Compare store elision against an interleaved store-all control and literal pixel values.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
static unsigned width=8, height=8, repeats=1;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static tex_t color = {.magic=MAGIC('T','E','X',0), .serial=99501};
static tex_t depth = {.magic=MAGIC('T','E','X',0), .serial=99502};
static prog_t program = {.serial=99503};
static void seed(void) {
    MTL4RenderPassDescriptor *p=[MTL4RenderPassDescriptor new];
    p.colorAttachments[0].texture=tex_mtl(&color);
    p.colorAttachments[0].loadAction=MTLLoadActionClear;
    p.colorAttachments[0].storeAction=MTLStoreActionStore;
    p.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
    p.depthAttachment.texture=p.stencilAttachment.texture=tex_mtl(&depth);
    p.depthAttachment.loadAction=p.stencilAttachment.loadAction=MTLLoadActionClear;
    p.depthAttachment.storeAction=p.stencilAttachment.storeAction=MTLStoreActionStore;
    p.depthAttachment.clearDepth=.75; p.stencilAttachment.clearStencil=0x2d;
    enc_r_end(enc_r_begin(p));
}
typedef struct { uint8_t color[256]; float depth[64]; uint8_t stencil[64]; uint64_t saved; double gpu_ms, wall_ms; } result;
static result run(int enabled, int mode) {
    static uint64_t frame; uint64_t started=bp_now(); mtl_set_frame(++frame);
    g_udc_ab=enabled; g_udc_ab_on=0;
    state_reset(); memset(&R,0,sizeof R); mtl_sub_begin(); seed();
    S.ds=(ds_st){.dt=1,.dw=mode==1,.df=NVN_DEPTH_FUNC_LESS,.st=mode==2,
        .sf_f=NVN_STENCIL_FUNC_ALWAYS,.sf_b=NVN_STENCIL_FUNC_ALWAYS,
        .fail_f=NVN_STENCIL_OP_KEEP,.zfail_f=NVN_STENCIL_OP_KEEP,.zpass_f=NVN_STENCIL_OP_REPLACE,
        .fail_b=NVN_STENCIL_OP_KEEP,.zfail_b=NVN_STENCIL_OP_KEEP,.zpass_b=NVN_STENCIL_OP_REPLACE};
    S.st_ref[0]=S.st_ref[1]=0x36;
    S.st_vmask[0]=S.st_vmask[1]=S.st_wmask[0]=S.st_wmask[1]=255;
    struct { op_hdr rt_h; p_set_rt rt; op_hdr pr_h; p_bind_program pr; op_hdr draw_h; p_draw draw; op_hdr end; } code={0};
    code.rt_h=(op_hdr){OP_SET_RT,sizeof(op_hdr)+sizeof(p_set_rt)};
    code.rt=(p_set_rt){.n=1,.depth=&depth}; code.rt.color[0]=&color;
    code.pr_h=(op_hdr){OP_BIND_PROGRAM,sizeof(op_hdr)+sizeof(p_bind_program)};
    code.pr=(p_bind_program){&program,3};
    code.draw_h=(op_hdr){OP_DRAW,sizeof(op_hdr)+sizeof(p_draw)};
    code.draw=(p_draw){NVN_DRAW_PRIMITIVE_TRIANGLES,0,3}; code.end=(op_hdr){OP_END,sizeof(op_hdr)};
    uint64_t before=g_udc_saved_bytes;
    if (mode==3) { // A pending clear changes the backing planes even when the later draw does not.
        R.has_rt=1; R.rt=code.rt;
        R.clear_d=R.clear_s=1; R.cd=.625; R.cs=0x63;
    }
    for (unsigned i=0;i<repeats;i++) {
    mtl_exec((const uint8_t *)&code);
    if (mode>=4) { // Unknown writable aliases must force stores, even with read-only depth state.
        NvProg *alias=[NvProg new]; alias->sh[1]=[NvShader new];
        alias->sh[1]->rf.color_mask=1; alias->sh[1]->rf.nb=1;
        int kinds[]={NVN_RB_IMG,NVN_RB_SBUFP,NVN_RB_SBUFU};
        alias->sh[1]->rf.b[0]=(nvn_rbind){.kind=kinds[mode-4],.rw=1}; udc_note_draw(alias);
    }
    rp_end();
    }
    id<MTLBuffer> b[3]; b[0]=[g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    b[1]=[g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    b[2]=[g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    [blit() copyFromTexture:tex_mtl(&color) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:b[0] destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    [blit() copyFromTexture:tex_mtl(&depth) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:b[1] destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256 options:MTLBlitOptionDepthFromDepthStencil];
    [blit() copyFromTexture:tex_mtl(&depth) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1) toBuffer:b[2] destinationOffset:0 destinationBytesPerRow:8 destinationBytesPerImage:64 options:MTLBlitOptionStencilFromDepthStencil];
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"GPU submission completes");
    CHECK(!mtl_last_commit_error(),"GPU submission has no error");
    result r={0}; memcpy(r.color,b[0].contents,256); memcpy(r.depth,b[1].contents,256); memcpy(r.stencil,b[2].contents,64);
    r.saved=g_udc_saved_bytes-before; r.gpu_ms=mtl_gpu_ms(frame); r.wall_ms=(bp_now()-started)/1e6; return r;
}
int main(int argc,char **argv) { @autoreleasepool {
    int bench=argc>1&&!strcmp(argv[1],"--bench"); if(bench) { width=1920; height=1080; repeats=32; }
    setenv("DM_NVN_TBDR","0",1); mtl_init(); CHECK(!tbdr_on(),"lookahead stays disabled");
    color.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=width,.height=height,.depth=1,.levels=1,.samples=1};
    depth.d=color.d; depth.d.format=NVN_FORMAT_DEPTH32F_STENCIL8;
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;vertex float4 v(uint n[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[n],.5,1);}\nfragment float4 f(){return float4(1,0,0,1);}" options:nil error:&error];
    CHECK(lib!=nil,"literal test shaders compile"); if (!lib) return 1;
    NvProg *pr=[NvProg new]; pr->sh[0]=[NvShader new]; pr->sh[1]=[NvShader new];
    pr->sh[0]->fn=[lib newFunctionWithName:@"v"]; pr->sh[1]->fn=[lib newFunctionWithName:@"f"];
    pr->sh[1]->stage=1; pr->sh[1]->rf.color_mask=1; serial_put(program.serial,pr);
    for (int case_id=0;case_id<(bench?12:7);case_id++) {
        int mode=bench?0:case_id;
        result off,on;
        if(case_id&1) { on=run(1,mode); off=run(0,mode); }
        else { off=run(0,mode); on=run(1,mode); }
        CHECK(!memcmp(off.color,on.color,256)&&!memcmp(off.depth,on.depth,256)&&!memcmp(off.stencil,on.stencil,64),"store-all and elided planes match byte for byte");
        int good=1; for(int i=0;i<64;i++) {
            uint8_t red[4]={255,0,0,255}; good &= !memcmp(on.color+4*i,red,4);
            good &= on.depth[i]==(mode==1?.5f:mode==3?.625f:.75f);
            good &= on.stencil[i]==(mode==2?0x36:mode==3?0x63:0x2d);
        }
        CHECK(good,"all 64 pixels match hand-computed color/depth/stencil");
        CHECK(off.saved==0,"disabled control always stores");
        uint64_t want=(uint64_t)width*height*repeats*(mode==0?5:mode==1?1:mode==2?4:0);
        CHECK(on.saved==want,"only provably unchanged planes skip their store");
        printf("mode %d: saved %llu bytes, expected %llu\n",mode,(unsigned long long)on.saved,(unsigned long long)want);
        if(bench) printf("BENCH trial %d off_gpu_ms %.6f on_gpu_ms %.6f off_wall_ms %.6f on_wall_ms %.6f saved_mb %.3f\n",case_id,off.gpu_ms,on.gpu_ms,off.wall_ms,on.wall_ms,on.saved/1e6);
    }
    printf("UNWRITTEN_STORE %d checks, %d failed\n",checks,failed); return failed!=0;
} }
