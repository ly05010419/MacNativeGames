// Real NVN recording entry → production Metal replay; each mip of 8x6 is hand-calculated independently; with no viewport set, an explicit viewport is not used to dodge it.
// nvn_mtl.m's nvn_game_fps is a weak default and hle_nvn.c gives the strong definition; in one translation unit the weak one is renamed (as in t_present_crop.m)
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle   // both files have a static g_ft, so hle_nvn.c's is renamed too (as in t_present_crop.m)
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;   // process start time from host main.c (used by hle_nvn's per-frame @t_s); unit test stub
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int cases,failed; static prog_t colour_pg,depth_pg;
#define EXPECT(n,w,v) do { uint64_t a=(w),b=(v);cases++;printf("%s expected %llu → got %llu %s\n",n,(unsigned long long)a,(unsigned long long)b,a==b?"✓":"✗");failed+=a!=b;}while(0)
static tex_t make_tex(int w,int h,int levels,int format) {
    static uint64_t ser=90000; tex_t t={.magic=MAGIC('T', 'E', 'X', 0), .serial =ser, .off=(int64_t)(ser-90000)<<20};ser++;   // own storage offset: textures of one description on one (null) pool at one offset are one shared texture (g_alias)
   t.d.width=w;t.d.height=h;
    t.d.depth=t.d.samples=1;t.d.levels=levels;t.d.target=NVN_TEXTURE_TARGET_2D;t.d.format=format;return t;
}
typedef struct { NVNcommandBuffer cb; pool_t pool; void *mem; } recording;
static recording begin(void) {
    recording r={0};if(posix_memalign(&r.mem,getpagesize(),65536))abort();memset(r.mem,0,65536);
    r.pool.mem=r.mem;r.pool.size=65536;mtl_pool_add(r.mem,65536);
    EXPECT("NVN command buffer init",1,h_nvnCommandBufferInitialize(&r.cb,NULL));
    h_nvnCommandBufferAddCommandMemory(&r.cb,(const NVNmemoryPool *)&r.pool,0,65536);h_nvnCommandBufferBeginRecording(&r.cb);return r;
}
static void target(recording *r,const tex_t *t,int lv,int slot,const tex_t *other,int other_lv,int depth) {
    NVNtextureView view,ov;h_nvnTextureViewSetDefaults(&view);h_nvnTextureViewSetLevels(&view,lv,1);
    h_nvnTextureViewSetDefaults(&ov);h_nvnTextureViewSetLevels(&ov,other_lv,1);
    const NVNtexture *ct[2]={NULL,NULL};const NVNtextureView *cv[2]={NULL,NULL};
    if(!depth){ct[slot]=(const NVNtexture *)t;cv[slot]=&view;if(other){ct[1]=(const NVNtexture *)other;cv[1]=&ov;}}
    h_nvnCommandBufferSetRenderTargets(&r->cb,depth?0:other?2:slot+1,ct,cv,depth?(const NVNtexture *)t:NULL,depth?&view:NULL);
    // Changing the view after recording must not change the recorded level; the source keeps a snapshot of the original parameters.
    h_nvnTextureViewSetLevels(&view,0,1);
}
static void finish(recording *r) {
    NVNcommandHandle h=h_nvnCommandBufferEndRecording(&r->cb);state_reset();memset(&R,0,sizeof R);
    mtl_sub_begin();mtl_exec((const uint8_t *)h);
    int done=mtl_wait(mtl_sub_commit(),5000000000ull);EXPECT("production GPU done",1,done);if(!done)abort();EXPECT("no error in commit feedback",0,mtl_last_commit_error());
    mtl_pool_del(r->mem);free(r->mem);
}
static void seed(const tex_t *t) {
    id<MTLTexture> m=tex_mtl(t);id<MTLCommandBuffer> cb=q_cb();   // q_cb flushes the host's pending new-texture zeroing first and waits for it; a plain g_q command buffer ran before it, and the zeroing wiped the seed
    for(int l=0;l<t->d.levels;l++){MTLRenderPassDescriptor *p=[MTLRenderPassDescriptor renderPassDescriptor];
        if(t->d.format==NVN_FORMAT_RGBA8){p.colorAttachments[0].texture=m;p.colorAttachments[0].level=l;
            p.colorAttachments[0].loadAction=MTLLoadActionClear;p.colorAttachments[0].storeAction=MTLStoreActionStore;p.colorAttachments[0].clearColor=MTLClearColorMake(0,0,1,1);}
        else if(t->d.format==NVN_FORMAT_STENCIL8){p.stencilAttachment.texture=m;p.stencilAttachment.level=l;p.stencilAttachment.loadAction=MTLLoadActionClear;p.stencilAttachment.storeAction=MTLStoreActionStore;p.stencilAttachment.clearStencil=0;}
        else {p.depthAttachment.texture=m;p.depthAttachment.level=l;p.depthAttachment.loadAction=MTLLoadActionClear;p.depthAttachment.storeAction=MTLStoreActionStore;p.depthAttachment.clearDepth=.75;}
        if(t->d.format==NVN_FORMAT_DEPTH32F_STENCIL8){p.stencilAttachment.texture=m;p.stencilAttachment.level=l;p.stencilAttachment.loadAction=MTLLoadActionClear;p.stencilAttachment.storeAction=MTLStoreActionStore;p.stencilAttachment.clearStencil=0;}
        [[cb renderCommandEncoderWithDescriptor:p] endEncoding];}
    [cb commit];[cb waitUntilCompleted];EXPECT("standalone Metal seed done",4,cb.status);
}
static uint8_t *read_mip(const tex_t *t,int lv,int w,int h,int bpp) {
    NSUInteger row=(w*bpp+15)&~15u;id<MTLBuffer> buf=[g_dev newBufferWithLength:row*h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb=q_cb();id<MTLBlitCommandEncoder> b=[cb blitCommandEncoder];
    id<MTLTexture> src=tex_mtl(t);MTLBlitOption opt=t->d.format==NVN_FORMAT_DEPTH32F_STENCIL8?(bpp==1?MTLBlitOptionStencilFromDepthStencil:MTLBlitOptionDepthFromDepthStencil):MTLBlitOptionNone;
    [b copyFromTexture:src sourceSlice:0 sourceLevel:lv sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1)
      toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row*h options:opt];[b endEncoding];[cb commit];[cb waitUntilCompleted];EXPECT("per-texel readback done",4,cb.status);
    uint8_t *out=malloc(w*h*bpp);for(int y=0;y<h;y++)memcpy(out+y*w*bpp,(uint8_t *)buf.contents+y*row,w*bpp);return out;
}
static void save(const char *name,int lv,const uint8_t *out,int n) {
    const char *dir=getenv("DM_GFX_FIXTURE_OUT");if(!dir)return;char path[1024];snprintf(path,sizeof path,"%s/%s_lv%d.raw",dir,name,lv);
    FILE *f=fopen(path,"wb");if(!f||fwrite(out,1,n,f)!=n)abort();fclose(f);
}
static void colour_check(const char *name,const tex_t *t,int lv,int kind,int draw_w,int draw_h,int partial) {
    // The four level sizes are a constant table given by the problem statement; the mip helper under test is not called.
    static const int W[]={8,4,2,1},H[]={6,3,1,1};
    for(int l=0;l<4;l++){int w=W[l],h=H[l],bad=0;uint8_t *out=read_mip(t,l,w,h,4);
        for(int y=0;y<h;y++)for(int x=0;x<w;x++){uint8_t want[4]={0,0,255,255};
            int in=l==lv&&x<draw_w&&y<draw_h&&(!partial||(x>=1&&x<3&&y==1));
            if(in&&kind==1){want[0]=255;want[2]=0;} // valid NVN full clear: red
            if(in&&kind==2){int rx=5*(2*x+1)>=4*draw_w,by=5*(2*y+1)>=4*draw_h;
                want[0]=rx?0:255;want[1]=rx?255:0;want[2]=by?255:0;}
            bad+=memcmp(out+4*(y*w+x),want,4)!=0;}
        char label[128];snprintf(label,sizeof label,"%s lv%d %d texels",name,l,w*h);EXPECT(label,0,bad);save(name,l,out,w*h*4);free(out);}
}
static void colour_case(const char *name,int lv,int sc,int clear_only,int slot,int mrt) {
    static const int W[]={8,4,2,1},H[]={6,3,1,1};int w=W[lv],h=H[lv];tex_t t=make_tex(8,6,4,NVN_FORMAT_RGBA8),other=make_tex(8,6,4,NVN_FORMAT_RGBA8);
    seed(&t);if(mrt)seed(&other);recording r=begin();target(&r,&t,lv,slot,mrt?&other:NULL,1,0);
    if(mrt){w=4;h=3;}
    if(sc==1)h_nvnCommandBufferSetScissor(&r.cb,0,0,w,h);
    if(sc==2)h_nvnCommandBufferSetScissor(&r.cb,-2,-3,30,40);
    if(sc==3)h_nvnCommandBufferSetScissor(&r.cb,1,1,2,1);
    if(clear_only){float red[4]={1,0,0,1};h_nvnCommandBufferClearColor(&r.cb,slot,red,15);}
    else {h_nvnCommandBufferBindProgram(&r.cb,(const NVNprogram *)&colour_pg,3);h_nvnCommandBufferDrawArrays(&r.cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);}
    finish(&r);NSUInteger pre_w,pre_h;rt_dims(&pre_w,&pre_h);EXPECT("precheck: first target selected width upper bound",W[lv],pre_w);EXPECT("precheck: first target selected height upper bound",H[lv],pre_h);
    if(sc!=3||!clear_only){EXPECT("actual pass effective width",w,g_pass.tw);EXPECT("actual pass effective height",h,g_pass.th);}
    colour_check(name,&t,lv,clear_only?1:2,w,h,sc==3);   // a scissored clear is drawn as a triangle (clear_color_tri) and fills exactly the scissor rectangle; it used to be rejected (nothing cleared)
    if(mrt)colour_check("MRT_unwritten_attachment",&other,-1,0,0,0,0);
}
static void plane_case(const char *name,int stencil,int clear_only) {
    tex_t t=make_tex(8,6,4,stencil?NVN_FORMAT_DEPTH32F_STENCIL8:NVN_FORMAT_DEPTH32F);seed(&t);recording r=begin();target(&r,&t,1,0,NULL,0,1);
    if(clear_only){h_nvnCommandBufferSetScissor(&r.cb,0,0,4,3);h_nvnCommandBufferClearDepthStencil(&r.cb,.25,!stencil,0x5a,stencil?255:0);}
    else {ds_st ds={.dt=!stencil,.dw=!stencil,.df=NVN_DEPTH_FUNC_ALWAYS,.st=stencil,.sf_f=8,.sf_b=8,.fail_f=1,.zfail_f=1,.zpass_f=3,.fail_b=1,.zfail_b=1,.zpass_b=3};
        p_bind_state st={.kind=5};memcpy(st.bytes,&ds,sizeof ds);*REC(&r.cb,OP_BIND_STATE,p_bind_state)=st;
        if(stencil)for(int k=0;k<3;k++)*REC(&r.cb,OP_STENCIL,p_stencil)=(p_stencil){.face=3,.which=k,.value_mask=255,.mask=255,.ref=0x5a};
        h_nvnCommandBufferBindProgram(&r.cb,(const NVNprogram *)&depth_pg,1);h_nvnCommandBufferDrawArrays(&r.cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);}
    finish(&r);EXPECT("depth/stencil effective width",4,g_pass.tw);EXPECT("depth/stencil effective height",3,g_pass.th);
    static const int W[]={8,4,2,1},H[]={6,3,1,1};for(int l=0;l<4;l++){int n=W[l]*H[l],bad=0;uint8_t *b=read_mip(&t,l,W[l],H[l],stencil?1:4);
        for(int i=0;i<n;i++)if(stencil)bad+=b[i]!=(l==1?0x5a:0);else bad+=((float *)b)[i]!=(l==1?.25f:.75f);
        EXPECT("selected plane per texel / unselected levels unchanged",0,bad);save(name,l,b,n*(stencil?1:4));free(b);}
    if(stencil)for(int l=0;l<4;l++){int n=W[l]*H[l],bad=0;uint8_t *b=read_mip(&t,l,W[l],H[l],4);
        for(int i=0;i<n;i++)bad+=((float *)b)[i]!=.75f;EXPECT("stencil ops leave the depth plane unchanged",0,bad);
        char label[96];snprintf(label,sizeof label,"%s_preserved_depth",name);save(label,l,b,n*4);free(b);}
}
int main(int argc,char **argv){@autoreleasepool{
    setvbuf(stdout,NULL,_IONBF,0);int stencil_only=argc>1&&!strcmp(argv[1],"--stencil-only");setenv("DM_NVN_TBDR",stencil_only?"1":"0",1);mtl_init();NSError *e=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;struct O{float4 p[[position]];float2 uv[[user(locn0)]];};\n"
      "vertex O v(uint n[[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return {float4(p[n],.25,1),float2((p[n].x+1)/2,(1-p[n].y)/2)};}\n"
      "fragment float4 f(O o[[stage_in]]){return float4(o.uv.x<.4?1:0,o.uv.x<.4?0:1,o.uv.y<.4?0:1,1);}\n" options:nil error:&e];if(!lib){fprintf(stderr,"%s\n",e.localizedDescription.UTF8String);return 2;}
    for(int i=0;i<2;i++){NvProg *p=[NvProg new];p->sh[0]=[NvShader new];p->sh[0]->fn=[lib newFunctionWithName:@"v"];
        if(!i){p->sh[1]=[NvShader new];p->sh[1]->fn=[lib newFunctionWithName:@"f"];p->sh[1]->stage=1;}
        (i?&depth_pg:&colour_pg)->serial=80000+i;serial_put(80000+i,p);}
    if(stencil_only){plane_case("stencil_only",1,0);EXPECT("existing stencil-only mode",3,g_enc_mode);EXPECT("stencil-only attaches no depth",1,g_pass.hs&&!g_pass.hd);}
    else if(argc>1&&!strcmp(argv[1],"--default"))colour_case("default",1,0,0,0,0);
    else if(argc>1&&!strcmp(argv[1],"--oversized"))colour_case("oversized",1,2,0,0,0);
    else {for(int lv=0;lv<4;lv++){char name[64];snprintf(name,sizeof name,"bounded_L%d",lv);colour_case(name,lv,1,0,0,0);
            snprintf(name,sizeof name,"clear_L%d",lv);colour_case(name,lv,1,1,0,0);}
        colour_case("partial_draw",1,3,0,0,0);colour_case("partial_clear",1,3,1,0,0);colour_case("slot1_clear",1,1,1,1,0);
        if(argc==1){colour_case("default",1,0,0,0,0);colour_case("oversized",1,2,0,0,0);colour_case("MRT_min",0,0,0,0,1);plane_case("depth",0,0);plane_case("stencil",1,0);plane_case("depth_clear",0,1);plane_case("stencil_clear",1,1);}}
    printf("== %d checks, %d failed ==\n",cases,failed);return failed!=0;
}}
