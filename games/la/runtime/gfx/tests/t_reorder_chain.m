// Fixed command chain experiment: production mtl_exec, hand-written constant shaders and closed-form expectations; does not include the production reorderer.
// G full stencil block, M small view + copy, U blend, R stencil resolve; only G/M are swapped.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int fail, count, all_independent;
#define EXPECT(n,w,v) do { uint64_t a=(w), b=(v); count++; \
    printf("%s expected %llu → got %llu %s\n",n,(unsigned long long)a,(unsigned long long)b,a==b?"✓":"✗"); fail+=a!=b; } while(0)
typedef struct { uint8_t *p; size_t n; int width; } chain;
static chain make_chain(void) {
    chain c={0}; if(posix_memalign((void **)&c.p,getpagesize(),65536)) abort();
    memset(c.p,0,65536); mtl_pool_add(c.p,65536); return c;
}
static void emit(chain *c, unsigned op, const void *a, size_t n) {
    size_t sz=(8+n+7)&~7ull; if(c->n+sz>65536) abort();
    op_hdr h={op,(uint32_t)sz}; memcpy(c->p+c->n,&h,8);
    if(n) memcpy(c->p+c->n+8,a,n); c->n+=sz;
}
static void state(chain *c, int kind, const void *s, size_t n) {
    p_bind_state b={.kind=kind}; if(n>sizeof b.bytes) abort(); memcpy(b.bytes,s,n); emit(c,OP_BIND_STATE,&b,sizeof b);
}
static tex_t texture(unsigned w, unsigned levels, int depth) {
    static uint64_t ser=5000; tex_t t={.magic=MAGIC('T', 'E', 'X', 0), .serial =ser++}; t.d.width=t.d.height=w;
    t.d.depth=t.d.samples=1; t.d.levels=levels; t.d.target=NVN_TEXTURE_TARGET_2D;
    t.d.format=depth?NVN_FORMAT_DEPTH32F_STENCIL8:NVN_FORMAT_RGBA8; return t;
}
static void rt(chain *c, const tex_t *t, unsigned lv, const tex_t *ds) {
    p_set_rt r={.n=1,.depth=ds}; r.color[0]=t;
    r.has_cview[0]=1; r.cview[0].set_levels=1; r.cview[0].base_level=lv; r.cview[0].nlevels=1;
    emit(c,OP_SET_RT,&r,sizeof r);
    p_viewports v={.n=1}; v.v[0][2]=v.v[0][3]=MAX(t->d.width>>lv,1u); emit(c,OP_VIEWPORTS,&v,sizeof v);
    c->width=(int)v.v[0][2];
    // The clear's scissor covers the base size, compatible with the existing scissor_full; each draw then explicitly takes the actual mip size.
    p_scissor sc={0,0,(int)t->d.width,(int)t->d.height}; emit(c,OP_SCISSOR,&sc,sizeof sc);
}
static void clear(chain *c, const tex_t *t, unsigned lv, const tex_t *ds, int red, int green, int blue) {
    rt(c,t,lv,ds); p_clear_color b={.index=0,.mask=15,.rgba={red,green,blue,1}}; emit(c,OP_CLEAR_COLOR,&b,sizeof b);
    if(ds) { p_clear_ds d={.depth=1,.depth_mask=1,.stencil=2,.stencil_mask=255}; emit(c,OP_CLEAR_DS,&d,sizeof d); }
}
static ds_st ds_state(int stencil, int func, int op) {
    return (ds_st){0,0,NVN_DEPTH_FUNC_ALWAYS,stencil,func,func,1,1,op,1,1,op};
}
static void stencil(chain *c, int enabled, int func, int op, unsigned read, unsigned write, unsigned ref) {
    ds_st d=ds_state(enabled,func,op); state(c,5,&d,sizeof d);
    for(int k=0;k<3;k++) { p_stencil s={.face=3,.which=k,.value_mask=read,.mask=write,.ref=ref}; emit(c,OP_STENCIL,&s,sizeof s); }
}
static void blending(chain *c, int enabled) {
    color_st col={enabled,NVN_LOGIC_OP_COPY,NVN_ALPHA_FUNC_ALWAYS}; state(c,1,&col,sizeof col);
    blend_st b={0,NVN_BLEND_FUNC_SRC_ALPHA,NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA,NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ZERO,NVN_BLEND_EQUATION_ADD,NVN_BLEND_EQUATION_ADD};
    state(c,0,&b,sizeof b);
}
static prog_t pg[8];
static void draw_prog(chain *c, int p, int tid) {
    p_bind_program b={&pg[p],3}; emit(c,OP_BIND_PROGRAM,&b,sizeof b);
    if(tid) { p_bind_handles h={.stage=1,.first=0,.n=1}; h.h[0]=tid; emit(c,OP_BIND_TEX,&h,sizeof h); }
    p_scissor sc={0,0,c->width,c->width}; emit(c,OP_SCISSOR,&sc,sizeof sc);
    p_draw d={NVN_DRAW_PRIMITIVE_TRIANGLES,0,3}; emit(c,OP_DRAW,&d,sizeof d);
}
static int bind_view_range(const tex_t *t, int lv, int levels, int sl, int slices) {
    static int tid=1000; view_t v={.set_levels=1,.base_level=lv,.nlevels=levels,
        .set_layers=slices>0,.min_layer=sl,.nlayers=slices};
    mtl_reg_tex(tid,t,&v); return tid++;
}
static int bind_view(const tex_t *t, int lv) { return bind_view_range(t,lv,1,0,0); }
typedef struct { uint8_t a[256],b[256],c[256],e[256]; uint64_t query; } result;
static result execute(chain *c, const tex_t *a, const tex_t *b, const tex_t *out, const tex_t *e, uint64_t *q) {
    emit(c,OP_END,NULL,0); state_reset(); memset(&R,0,sizeof R); vis_clear(); g_vis_on=g_vis_counting=0;
    mtl_sub_begin(); mtl_exec(c->p);
    const tex_t *t[4]={a,b,out,e}; id<MTLBuffer> buf[4];
    for(int k=0;k<4;k++) { buf[k]=[g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        [blit() copyFromTexture:tex_mtl(t[k]) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1)
          toBuffer:buf[k] destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256]; }
    uint64_t f=mtl_sub_commit(); EXPECT("production submit completed",1,mtl_wait(f,5000000000ull)); EXPECT("no error in commit feedback",0,mtl_last_commit_error());
    result r={0}; uint8_t *dst[4]={r.a,r.b,r.c,r.e}; for(int k=0;k<4;k++) memcpy(dst[k],buf[k].contents,256);
    if(q) r.query=*q; mtl_pool_del(c->p); free(c->p); return r;
}
static void uniform_pixels(const char *name, const uint8_t *b, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    uint8_t want[4]={red,green,blue,alpha}; unsigned bad=0;
    for(int k=0;k<64;k++) bad+=memcmp(b+4*k,want,4)!=0; EXPECT(name,0,bad);
}
static void save_raw(const char *name, int reordered, const void *r, size_t n) {
    const char *dir=getenv("DM_GFX_FIXTURE_OUT"); if(!dir) return;
    char path[1024]; snprintf(path,sizeof path,"%s/%s_%s.raw",dir,name,reordered?"reordered":"original");
    FILE *f=fopen(path,"wb"); if(!f || fwrite(r,1,n,f)!=n) abort(); fclose(f);
}
static void save_result(const char *name, int reordered, const result *r) { save_raw(name,reordered,r,sizeof *r); }
enum { INDEPENDENT, SAME_MIP, STENCIL_INDEPENDENT, STENCIL_DEPENDENT, COPY_DEPENDENT, INTERNAL_ORDER };
static void full_block(chain *c,const tex_t *a,const tex_t *ds,int reverse) {
    rt(c,a,0,ds); blending(c,0);
    for(int i=0;i<2;i++) { int reader=reverse?i==0:i==1;
        stencil(c,1,reader?NVN_STENCIL_FUNC_NOTEQUAL:NVN_STENCIL_FUNC_ALWAYS,reader?NVN_STENCIL_OP_KEEP:NVN_STENCIL_OP_REPLACE,
                reader?0x34:0,reader?0:0x10,reader?0:0x30); draw_prog(c,reader?1:0,0); }
}
static void middle_block(chain *c,const tex_t *a,const tex_t *b,const tex_t *ds,const tex_t *d,const tex_t *e,int tid,int mode) {
    rt(c,b,0,mode==STENCIL_INDEPENDENT||mode==STENCIL_DEPENDENT?ds:NULL); blending(c,0);
    unsigned mask=mode==STENCIL_DEPENDENT?0x10:0x2;
    stencil(c,mode==STENCIL_INDEPENDENT||mode==STENCIL_DEPENDENT,NVN_STENCIL_FUNC_EQUAL,NVN_STENCIL_OP_KEEP,mask,0,mask); draw_prog(c,2,tid);
    p_copy_t2t cp={.src=mode==COPY_DEPENDENT?a:d,.dst=e,.sr={0,0,0,8,8,1},.dr={0,0,0,8,8,1}};
    emit(c,OP_COPY_T2T,&cp,sizeof cp);
}
static result render_case(int mode,int reordered) {
    tex_t a=texture(8,2,0), b=texture(8,1,0), out=texture(8,1,0), ds=texture(8,1,1), d=texture(8,1,0), e=texture(8,1,0);
    int at=bind_view(&a,mode==SAME_MIP?0:1),bt=bind_view(&b,0),af=bind_view(&a,0); chain c=make_chain();
    clear(&c,&a,0,&ds,0,0,0); clear(&c,&a,1,NULL,0,1,0); clear(&c,&b,0,NULL,0,0,0);
    clear(&c,&d,0,NULL,0,0,1); clear(&c,&e,0,NULL,0,0,0); clear(&c,&out,0,NULL,0,0,0);
    if(reordered) middle_block(&c,&a,&b,&ds,&d,&e,at,mode);
    full_block(&c,&a,&ds,reordered&&mode==INTERNAL_ORDER);
    if(!reordered) middle_block(&c,&a,&b,&ds,&d,&e,at,mode);
    rt(&c,&a,0,NULL); stencil(&c,0,8,1,255,255,0); blending(&c,1); draw_prog(&c,3,bt);
    rt(&c,&out,0,&ds); stencil(&c,1,NVN_STENCIL_FUNC_EQUAL,1,2,0,2); blending(&c,0); draw_prog(&c,2,af);
    return execute(&c,&a,&b,&out,&e,NULL);
}
static uint64_t query_case(int reordered, int mode) {
    tex_t a=texture(8,1,0),b=texture(4,1,0); chain c=make_chain(); uint64_t q[2]={UINT64_MAX,0}, type=NVN_COUNTER_TYPE_SAMPLES_PASSED;
    emit(&c,OP_RESET_COUNTER,&type,8); stencil(&c,0,8,1,255,255,0); blending(&c,0);
    if(!reordered) { clear(&c,&a,0,NULL,0,0,0); draw_prog(&c,0,0); }
    if(mode!=1) emit(&c,OP_RESET_COUNTER,&type,8);
    clear(&c,&b,0,NULL,0,0,0); draw_prog(&c,1,0);
    if(reordered) { clear(&c,&a,0,NULL,0,0,0); draw_prog(&c,0,0); }
    if(mode==2) {   // both blocks' counts are wiped by the subsequent reset before any report; only the new 4x4 draw is reported.
        emit(&c,OP_RESET_COUNTER,&type,8); clear(&c,&b,0,NULL,0,0,0); draw_prog(&c,1,0); }
    uint64_t report[2]={type,(uint64_t)q}; emit(&c,OP_REPORT_COUNTER,report,sizeof report); emit(&c,OP_END,NULL,0);
    state_reset(); memset(&R,0,sizeof R); vis_clear(); g_vis_on=g_vis_counting=0; mtl_sub_begin(); mtl_exec(c.p);
    EXPECT("query completed",1,mtl_wait(mtl_sub_commit(),5000000000ull)); EXPECT("query: no error in commit feedback",0,mtl_last_commit_error());
    const char *names[]={"query","same_range_query","later_reset_overrides_query"};
    save_raw(names[mode],reordered,q,sizeof q); mtl_pool_del(c.p); free(c.p); return q[0];
}
static void uniform_case(int reordered,uint8_t pixels[256]) {
    void *mem=NULL; if(posix_memalign(&mem,getpagesize(),getpagesize())) abort(); memset(mem,0,getpagesize());
    float red[4]={1,0,0,1},green[4]={0,1,0,1}; memcpy(mem,red,16); mtl_pool_add(mem,getpagesize());
    tex_t a=texture(8,1,0),b=texture(8,1,0),out=texture(8,1,0),e=texture(8,1,0); chain c=make_chain();
    clear(&c,&out,0,NULL,0,0,0); clear(&c,&e,0,NULL,0,0,0); stencil(&c,0,8,1,255,255,0); blending(&c,0);
    p_bind_buf ub={.stage=1,.index=0,.addr=(uint64_t)mem,.size=16}; emit(&c,OP_BIND_UBO,&ub,sizeof ub);
    if(!reordered) { clear(&c,&a,0,NULL,0,0,0); draw_prog(&c,4,0); }
    struct { uint64_t dst,n; float color[4]; } update={(uint64_t)mem,16,{0}}; memcpy(update.color,green,16); emit(&c,OP_UPDATE_UB,&update,sizeof update);
    if(reordered) { clear(&c,&a,0,NULL,0,0,0); draw_prog(&c,4,0); }
    clear(&c,&b,0,NULL,0,0,0); draw_prog(&c,4,0); result r=execute(&c,&a,&b,&out,&e,NULL); save_result("UB_write",reordered,&r); memcpy(pixels,r.a,256);
    uniform_pixels("second draw after the UB update always green",r.b,0,255,0,255); mtl_pool_del(mem); free(mem);
}
// A view's accessible range cannot be judged by base alone: the shader reads relative to the second level/second layer, and G writes that subresource.
static void slice_rt(chain *c,const tex_t *t,int lv,int sl) {
    p_set_rt r={.n=1}; r.color[0]=t; r.has_cview[0]=1;
    r.cview[0]=(view_t){.set_levels=1,.base_level=lv,.nlevels=1,.set_layers=1,.min_layer=sl,.nlayers=1};
    emit(c,OP_SET_RT,&r,sizeof r); p_viewports v={.n=1}; v.v[0][2]=v.v[0][3]=MAX(t->d.width>>lv,1u); emit(c,OP_VIEWPORTS,&v,sizeof v);
    c->width=(int)v.v[0][2]; p_scissor sc={0,0,(int)t->d.width,(int)t->d.height}; emit(c,OP_SCISSOR,&sc,sizeof sc);
}
static void slice_clear(chain *c,const tex_t *t,int lv,int sl,int green) {
    slice_rt(c,t,lv,sl); p_clear_color cc={.index=0,.mask=15,.rgba={0,green,0,1}}; emit(c,OP_CLEAR_COLOR,&cc,sizeof cc);
}
static void sample_block(chain *c,const tex_t *dst,int program,int tid) {
    rt(c,dst,0,NULL); stencil(c,0,8,1,255,255,0); blending(c,0); draw_prog(c,program,tid);
}
static result range_case(int array,int reordered) {
    tex_t a=texture(8,array?1:2,0),b=texture(8,1,0),out=texture(8,1,0),e=texture(8,1,0);
    if(array) { a.d.target=NVN_TEXTURE_TARGET_2D_ARRAY; a.d.depth=2; }
    int tid=bind_view_range(&a,0,array?1:2,0,array?2:0); chain c=make_chain();
    slice_clear(&c,&a,0,0,0); slice_clear(&c,&a,array?0:1,array?1:0,1);
    clear(&c,&b,0,NULL,0,0,0); clear(&c,&out,0,NULL,0,0,0); clear(&c,&e,0,NULL,0,0,0);
    if(reordered) sample_block(&c,&b,array?6:5,tid);
    slice_rt(&c,&a,array?0:1,array?1:0); stencil(&c,0,8,1,255,255,0); blending(&c,0); draw_prog(&c,1,0);
    if(!reordered) sample_block(&c,&b,array?6:5,tid);
    result r=execute(&c,&a,&b,&out,&e,NULL);
    NSUInteger w=array?8:4; id<MTLBuffer> buf=[g_dev newBufferWithLength:w*w*4 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb=[g_q commandBuffer]; id<MTLBlitCommandEncoder> bl=[cb blitCommandEncoder];
    [bl copyFromTexture:tex_mtl(&a) sourceSlice:array?1:0 sourceLevel:array?0:1 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,w,1)
      toBuffer:buf destinationOffset:0 destinationBytesPerRow:w*4 destinationBytesPerImage:w*w*4]; [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
    EXPECT("second subresource readback GPU done",4,cb.status); uint8_t blue[4]={0,0,255,255}; unsigned bad=0;
    for(NSUInteger i=0;i<w*w;i++) bad+=memcmp((const uint8_t *)buf.contents+4*i,blue,4)!=0;
    EXPECT("G second subresource blue per pixel",0,bad); save_raw(array?"whole_slice_write_target":"whole_mip_write_target",reordered,buf.contents,w*w*4);
    return r;
}
// Disjoint attachments can still create a dependency through an inherited program; this only demonstrates explicitly filling in the program and does not pass itself off as a full S/R snapshot.
static result state_case(int restored,int reordered) {
    tex_t a=texture(8,1,0),b=texture(8,1,0),out=texture(8,1,0),e=texture(8,1,0); chain c=make_chain();
    clear(&c,&a,0,NULL,0,0,0); clear(&c,&b,0,NULL,0,0,0); clear(&c,&out,0,NULL,0,0,0); clear(&c,&e,0,NULL,0,0,0);
    stencil(&c,0,8,1,255,255,0); blending(&c,0); p_bind_program seed={&pg[0],3}; emit(&c,OP_BIND_PROGRAM,&seed,sizeof seed);
    for(int k=0;k<2;k++) { int middle=reordered?k==0:k==1;
        rt(&c,middle?&b:&a,0,NULL);
        if(!middle) draw_prog(&c,1,0);
        else { if(restored) { p_bind_program p={&pg[1],3}; emit(&c,OP_BIND_PROGRAM,&p,sizeof p); }
            p_draw d={NVN_DRAW_PRIMITIVE_TRIANGLES,0,3}; emit(&c,OP_DRAW,&d,sizeof d); } }
    return execute(&c,&a,&b,&out,&e,NULL);
}
static result exit_state_case(int restored,int reordered) {
    tex_t a=texture(8,1,0),b=texture(8,1,0),out=texture(8,1,0),e=texture(8,1,0); chain c=make_chain();
    clear(&c,&a,0,NULL,0,0,0); clear(&c,&b,0,NULL,0,0,0); clear(&c,&out,0,NULL,0,0,0); clear(&c,&e,0,NULL,0,0,0);
    for(int k=0;k<2;k++) { int middle=reordered?k==0:k==1; sample_block(&c,middle?&b:&a,middle?1:0,0); }
    rt(&c,&out,0,NULL);
    if(restored) { p_bind_program p={&pg[1],3}; emit(&c,OP_BIND_PROGRAM,&p,sizeof p); }
    p_draw d={NVN_DRAW_PRIMITIVE_TRIANGLES,0,3}; emit(&c,OP_DRAW,&d,sizeof d);
    return execute(&c,&a,&b,&out,&e,NULL);
}
// Disjoint write masks are not enough either: the implicit read of IncrementWrap includes the low bits, and clearing the low bits changes the carry into bit1.
static result stencil_carry_case(int reordered) {
    tex_t a=texture(8,1,0),b=texture(8,1,0),out=texture(8,1,0),e=texture(8,1,0),ds=texture(8,1,1); chain c=make_chain();
    clear(&c,&a,0,&ds,0,0,0); p_clear_ds init={.depth=1,.depth_mask=1,.stencil=1,.stencil_mask=255}; emit(&c,OP_CLEAR_DS,&init,sizeof init);
    clear(&c,&b,0,NULL,0,0,0); clear(&c,&out,0,NULL,0,0,0); clear(&c,&e,0,NULL,0,0,0); blending(&c,0);
    chmask_st mask={0}; state(&c,2,&mask,sizeof mask);
    for(int k=0;k<2;k++) { int inc=reordered?k==0:k==1; rt(&c,inc?&b:&a,0,&ds);
        stencil(&c,1,NVN_STENCIL_FUNC_ALWAYS,inc?NVN_STENCIL_OP_INCR_WRAP:NVN_STENCIL_OP_ZERO,0,inc?2:1,0); draw_prog(&c,0,0); }
    mask.mask=0xffffffffu; state(&c,2,&mask,sizeof mask); rt(&c,&out,0,&ds); stencil(&c,1,NVN_STENCIL_FUNC_EQUAL,1,2,0,2); draw_prog(&c,7,0);
    return execute(&c,&a,&b,&out,&e,NULL);
}
// The G write is hidden in a real CALL/JUMP target; the jump list uses return addresses to preserve the original execution path and in-block order.
static result control_case(int jump,int reordered) {
    tex_t a=texture(8,1,0),b=texture(8,1,0),out=texture(8,1,0),e=texture(8,1,0); int tid=bind_view(&a,0);
    chain c=make_chain(),child=make_chain();
    clear(&c,&a,0,NULL,0,0,0); clear(&c,&b,0,NULL,0,0,0); clear(&c,&out,0,NULL,0,0,0); clear(&c,&e,0,NULL,0,0,0);
    if(reordered) sample_block(&c,&b,2,tid);
    uint64_t ptr=(uint64_t)child.p; emit(&c,jump?OP_JUMP:OP_CALL,&ptr,8);
    uint64_t ret=(uint64_t)(c.p+c.n); sample_block(&child,&a,1,0);
    emit(&child,jump?OP_JUMP:OP_END,jump?&ret:NULL,jump?8:0);
    if(!reordered) sample_block(&c,&b,2,tid);
    result r=execute(&c,&a,&b,&out,&e,NULL); mtl_pool_del(child.p); free(child.p); return r;
}
static void compare_pair(const char *name,const result *o,const result *r,unsigned different) {
    save_result(name,0,o); save_result(name,1,r); unsigned bad=0;
    for(int k=0;k<1024;k+=4) bad+=memcmp((const uint8_t *)o+k,(const uint8_t *)r+k,4)!=0;
    EXPECT("full range/state/control flow differing pixels (out of 256)",all_independent?0:different,bad);
}
int main(int argc,char **argv) { @autoreleasepool {
    setvbuf(stdout,NULL,_IONBF,0);
    all_independent=argc>1&&!strcmp(argv[1],"--all-independent"); setenv("DM_NVN_TBDR","0",1); mtl_init();
    NSError *err=nil; id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
      "vertex float4 v(uint n [[vertex_id]]){float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[n],0,1);}\n"
      "fragment float4 red(){return float4(1,0,0,1);} fragment float4 blue(){return float4(0,0,1,1);} fragment float4 green(){return float4(0,1,0,1);}\n"
      "struct A{texture2d<float> t [[id(0)]];}; fragment float4 readtex(constant A&a [[buffer(0)]]){return a.t.read(uint2(0));}\n"
      "fragment float4 halftex(constant A&a [[buffer(0)]]){return float4(a.t.read(uint2(0)).rgb,0.5);}\n"
      "fragment float4 readlevel1(constant A&a [[buffer(0)]]){return a.t.read(uint2(0),1);}\n"
      "struct T{texture2d_array<float> t [[id(0)]];}; fragment float4 readslice1(constant T&a [[buffer(0)]]){return a.t.read(uint2(0),1);}\n"
      "struct U{constant float4 *c [[id(0)]];}; fragment float4 ub(constant U&a [[buffer(0)]]){return *a.c;}\n" options:nil error:&err];
    if(!lib) { fprintf(stderr,"%s\n",err.localizedDescription.UTF8String); return 2; }
    NSString *fn[8]={@"red",@"blue",@"readtex",@"halftex",@"ub",@"readlevel1",@"readslice1",@"green"};
    for(int i=0;i<8;i++) { NvProg *p=[NvProg new]; p->sh[0]=[NvShader new]; p->sh[0]->fn=[lib newFunctionWithName:@"v"];
        p->sh[1]=[NvShader new]; NvShader *f=p->sh[1]; f->fn=[lib newFunctionWithName:fn[i]]; f->stage=1;
        if(i>=2&&i!=7) { f->rf.nb=1; f->rf.b[0]=(nvn_rbind){.kind=i==4?NVN_RB_CBUF:NVN_RB_TEX,.dtt=i==6?3:2}; f->ae=[f->fn newArgumentEncoderWithBufferIndex:0]; }
        pg[i].serial=4000+i; serial_put(pg[i].serial,p); }
    const char *names[]={"independent_mip_copy","same_mip_negative","stencil_disjoint_bits","stencil_shared_bits_negative","copy_reads_source_negative","in_block_order_negative"};
    for(int mode=0;mode<6;mode++) { printf("CASE %s\n",names[mode]); result o=render_case(mode,0),r=render_case(mode,1);
        save_result(names[mode],0,&o); save_result(names[mode],1,&r);
        int same=mode==SAME_MIP; uniform_pixels("original order closed-form resolve",o.c,0,same?0:128,same?255:128,128);
        uint8_t cr=mode==INTERNAL_ORDER?128:0,cg=mode==SAME_MIP||mode==STENCIL_DEPENDENT?0:128,cb=mode==INTERNAL_ORDER?0:128;
        uniform_pixels("candidate closed-form resolve",r.c,cr,cg,cb,128);
        uniform_pixels("original order independent/dependent copy result",o.e,0,0,255,255);
        uniform_pixels("candidate copy result",r.e,0,0,mode==COPY_DEPENDENT?0:255,255);
        unsigned different=0; const uint8_t *x=(const uint8_t *)&o,*y=(const uint8_t *)&r;
        for(int k=0;k<1024;k+=4) different+=memcmp(x+k,y+k,4)!=0;
        EXPECT("reorder differing pixels (4 attachments, out of 256 pixels)",all_independent?0:mode==INDEPENDENT||mode==STENCIL_INDEPENDENT?0:mode==COPY_DEPENDENT?64:mode==INTERNAL_ORDER?128:192,different);
    }
    uint64_t q0=query_case(0,0),q1=query_case(1,0); EXPECT("reset original range 4x4 samples passed",16,q0); EXPECT("across-reset negative 4x4+8x8 samples passed",80,q1);
    EXPECT("query lifetime must not count as independent",all_independent?1:0,q0==q1);
    uint8_t u0[256],u1[256]; uniform_case(0,u0); uniform_case(1,u1); uniform_pixels("UB original order first draw red",u0,255,0,0,255); uniform_pixels("across UB write first draw green",u1,0,255,0,255);
    EXPECT("UB write must not move across draws",all_independent?1:0,!memcmp(u0,u1,256));
    for(int array=0;array<2;array++) { const char *name=array?"whole_slice_range":"whole_mip_range"; printf("CASE %s\n",name);
        result o=range_case(array,0),r=range_case(array,1); uniform_pixels("range original order second subresource blue",o.b,0,0,255,255);
        uniform_pixels("range candidate second subresource green",r.b,0,255,0,255); compare_pair(name,&o,&r,64); }
    for(int restored=0;restored<2;restored++) { const char *name=restored?"entry_program_explicit_restore":"entry_program_inherits"; printf("CASE %s\n",name);
        result o=state_case(restored,0),r=state_case(restored,1); uniform_pixels("inherited original order blue",o.b,0,0,255,255);
        uniform_pixels("candidate explicitly restored blue / not restored red",r.b,restored?0:255,0,restored?255:0,255); compare_pair(name,&o,&r,restored?0:64); }
    for(int restored=0;restored<2;restored++) { const char *name=restored?"exit_program_explicit_restore":"exit_program_inherits"; printf("CASE %s\n",name);
        result o=exit_state_case(restored,0),r=exit_state_case(restored,1); uniform_pixels("following original order inherited blue",o.c,0,0,255,255);
        uniform_pixels("following candidate explicitly restored blue / not restored red",r.c,restored?0:255,0,restored?255:0,255); compare_pair(name,&o,&r,restored?0:64); }
    for(int jump=0;jump<2;jump++) { const char *name=jump?"JUMP_hidden_write":"CALL_hidden_write"; printf("CASE %s\n",name);
        result o=control_case(jump,0),r=control_case(jump,1); uniform_pixels("control original order blue",o.b,0,0,255,255);
        uniform_pixels("control candidate reads black first",r.b,0,0,0,255); compare_pair(name,&o,&r,64); }
    q0=query_case(0,1); q1=query_case(1,1); EXPECT("same epoch original order samples passed 64+16",80,q0); EXPECT("same epoch swapped order samples passed 16+64",80,q1);
    EXPECT("same epoch, two fixed draws swapped: report stays 80",1,q0==q1);
    q0=query_case(0,2); q1=query_case(1,2); EXPECT("later reset overrides the unreported count (original)",16,q0); EXPECT("later reset overrides the unreported count (candidate)",16,q1);
    EXPECT("this fixed later reset dominates the report: 16 reported before and after the swap",1,q0==q1);
    printf("CASE stencil_implicit_carry_read\n"); result co=stencil_carry_case(0),cr=stencil_carry_case(1);
    uniform_pixels("low bit cleared first gives bit1 = 0",co.c,0,0,0,255); uniform_pixels("increment carry first gives bit1 = 1",cr.c,0,255,0,255); compare_pair("stencil_implicit_carry_read",&co,&cr,64);
    printf("== %d checks, %d failed ==\n",count,fail); return fail!=0;
} }
