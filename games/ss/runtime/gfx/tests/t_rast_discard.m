// Discard suppresses fragments while array, indexed and instanced vertex writes still execute.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c, label) do { checks++; if (!(c)) { fails++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)
static uint8_t *cmd; static size_t used;
static void emit(unsigned op, const void *p, size_t n) {
    size_t z = (8 + n + 7) & ~7ull; op_hdr h = {op, (uint32_t)z};
    memcpy(cmd + used, &h, 8); if (n) memcpy(cmd + used + 8, p, n); used += z;
}
static void discard(int on) { p_rast_discard d = {.on=on}; emit(OP_RAST_DISCARD,&d,sizeof d); }
static void clear_blue(void) { p_clear_color c = {.rgba={0,0,1,1},.mask=15}; emit(OP_CLEAR_COLOR,&c,sizeof c); }
static void execute(void) {
    emit(OP_END,NULL,0); mtl_sub_begin(); uint64_t handle=(uint64_t)cmd; mtl_sub_index(&handle,1); mtl_exec(cmd);
    uint64_t fence=mtl_sub_commit(); CHECK(mtl_wait(fence,5000000000ull),"GPU submission completes");
    CHECK(!mtl_last_commit_error(),"GPU reports no error"); used=0;
}
static void pixels(const tex_t *t, int red) {
    mtl_sub_begin(); id<MTLBuffer> b=[g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared]; res_add(b);
    enc_b_copy_texture_buffer(blit(),tex_mtl(t),0,0,MTLOriginMake(0,0,0),MTLSizeMake(8,8,1),b,0,32,256);
    uint64_t fence=mtl_sub_commit(); CHECK(mtl_wait(fence,5000000000ull),"readback completes");
    uint8_t *p=b.contents; int good=1;
    for(int i=0;i<64;i++) good &= p[i*4]==(red?255:0) && p[i*4+1]==0 && p[i*4+2]==(red?0:255) && p[i*4+3]==255;
    CHECK(good,red?"normal draw rasterizes every pixel red":"discard leaves every pixel at the blue clear");
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_TBDR","0",1); mtl_init();
    void *memory=NULL; if(posix_memalign(&memory,getpagesize(),65536))abort(); memset(memory,0,65536); mtl_pool_add(memory,65536);
    cmd=memory; uint32_t *output=(uint32_t *)((uint8_t *)memory+32768);
    uint32_t *indices=(uint32_t *)((uint8_t *)memory+36864); indices[0]=7;indices[1]=8;indices[2]=9;
    NSError *error=nil; id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct A{device uint* p[[id(0)]];};\n"
        "vertex float4 v(uint n[[vertex_id]],uint i[[instance_id]],constant A&a[[buffer(0)]]){a.p[n+16*i]=0x500+n+16*i;float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[n%3],0,1);}\n"
        "vertex void vd(uint n[[vertex_id]],uint i[[instance_id]],constant A&a[[buffer(0)]]){a.p[n+16*i]=0x500+n+16*i;}\n"
        "vertex float4 p(uint n[[vertex_id]]){return float4(0,0,0,1);}\n"
        "vertex float4 c(uint n[[vertex_id]]){return float4(0,0,0,1);}\n"
        "vertex void pd(uint n[[vertex_id]],constant A&a[[buffer(0)]]){a.p[n]=n+0x1200;}\n"
        "vertex void cd(uint n[[vertex_id]],constant A&a[[buffer(0)]]){a.p[n+4096]=a.p[n];}\n"
        "fragment float4 f(){return float4(1,0,0,1);}\n"
        "fragment float4 fw(constant A&a[[buffer(0)]]){a.p[8192]=1;return float4(1,0,0,1);}\n" options:nil error:&error];
    if(!lib){fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 1;}
    static prog_t pg={.serial=910501}; NvProg *program=[NvProg new];
    NvShader *vs=[NvShader new];vs->fn=[lib newFunctionWithName:@"v"];vs->ae=[vs->fn newArgumentEncoderWithBufferIndex:0];
    vs->discard_fn=[lib newFunctionWithName:@"vd"];
    vs->rf.nb=1;vs->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=1,.reg=0,.aid=0};
    program->sh[0]=vs;program->sh[1]=[NvShader new];program->sh[1]->fn=[lib newFunctionWithName:@"f"];program->sh[1]->stage=1;serial_put(pg.serial,program);
    tex_t t={.magic=MAGIC('T','E','X',0),.serial=910502};t.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=8,.height=8,.depth=1,.levels=1,.samples=1};
    state_reset();memset(&R,0,sizeof R);
    p_set_rt rt={.n=1};rt.color[0]=&t;emit(OP_SET_RT,&rt,sizeof rt);
    p_bind_program bp={.prog=&pg,.stages=3};emit(OP_BIND_PROGRAM,&bp,sizeof bp);
    p_bind_buf buf={.stage=0,.index=0,.addr=(uint64_t)output,.size=256};emit(OP_BIND_SSBO,&buf,sizeof buf);
    ds_st ds={.df=NVN_DEPTH_FUNC_ALWAYS};p_bind_state bs={.kind=5};memcpy(bs.bytes,&ds,sizeof ds);emit(OP_BIND_STATE,&bs,sizeof bs);
    clear_blue();execute();
    p_scissor sc={0};emit(OP_SCISSOR,&sc,sizeof sc);
    poly_st poly={.cull=NVN_FACE_FRONT_AND_BACK};bs.kind=4;memcpy(bs.bytes,&poly,sizeof poly);emit(OP_BIND_STATE,&bs,sizeof bs);
    discard(1);p_draw arrays={.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.first=4,.count=3,.instances=1};emit(OP_DRAW,&arrays,sizeof arrays);
    p_draw_idx indexed={.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.itype=2,.count=3,.ibuf=(uint64_t)indices,.instances=2};emit(OP_DRAW_IDX,&indexed,sizeof indexed);
    execute();pixels(&t,0);
    CHECK(output[4]==0x504 && output[5]==0x505 && output[6]==0x506,"discarded array draw writes its three vertex IDs");
    CHECK(output[7]==0x507 && output[9]==0x509 && output[23]==0x517 && output[25]==0x519,"discarded indexed draw writes both instances");
    sc.w=sc.h=8;emit(OP_SCISSOR,&sc,sizeof sc);poly.cull=0;memcpy(bs.bytes,&poly,sizeof poly);emit(OP_BIND_STATE,&bs,sizeof bs);
    discard(0);arrays.first=0;emit(OP_DRAW,&arrays,sizeof arrays);execute();pixels(&t,1);
    clear_blue();discard(1);emit(OP_DRAW,&arrays,sizeof arrays);execute();pixels(&t,0);
    // ORDER_PRIMITIVES must make one vertex shader's storage writes visible
    // to the next vertex shader, even with rasterization disabled throughout.
    static prog_t pp={.serial=910503},cp={.serial=910504};NvProg *producers[2]={[NvProg new],[NvProg new]};
    for(int i=0;i<2;i++){NvShader *s=[NvShader new];s->fn=[lib newFunctionWithName:i?@"c":@"p"];s->discard_fn=[lib newFunctionWithName:i?@"cd":@"pd"];
        s->ae=[s->discard_fn newArgumentEncoderWithBufferIndex:0];s->rf.nb=1;s->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=1,.reg=0,.aid=0};producers[i]->sh[0]=s;}
    serial_put(pp.serial,producers[0]);serial_put(cp.serial,producers[1]);
    buf.size=32768;emit(OP_BIND_SSBO,&buf,sizeof buf);bp.prog=&pp;emit(OP_BIND_PROGRAM,&bp,sizeof bp);
    arrays=(p_draw){.prim=0,.count=4096,.instances=1};emit(OP_DRAW,&arrays,sizeof arrays);
    int32_t barrier=NVN_BARRIER_ORDER_PRIMITIVES_BIT;emit(OP_BARRIER,&barrier,sizeof barrier);
    bp.prog=&cp;emit(OP_BIND_PROGRAM,&bp,sizeof bp);emit(OP_DRAW,&arrays,sizeof arrays);execute();
    int ordered=1;for(int i=0;i<4096;i++)ordered&=output[i+4096]==(uint32_t)(i+0x1200);
    CHECK(ordered,"primitive barrier orders vertex storage writes before the consuming draw");
    // The barrier also orders the other way: a later draw's storage writes come after the pass's earlier draws (Xenoblade Chronicles 2 rewrites shared bone matrix slots
    // with a vertex-only draw after ORDER_PRIMITIVES). A barrier from the vertex stage inside the pass is enough when the earlier draws read memory only in their vertex stage or
    // the barrier orders only primitives; ORDER_FRAGMENTS after a draw that reads in its fragment stage ends the pass; a draw without writes, or a write with no barrier, changes nothing
    static prog_t rd={.serial=910506},rf={.serial=910507},fwp={.serial=910508};NvProg *reader=[NvProg new],*freader=[NvProg new],*fwriter=[NvProg new];
    reader->sh[0]=[NvShader new];reader->sh[0]->fn=[lib newFunctionWithName:@"c"];reader->sh[1]=program->sh[1];serial_put(rd.serial,reader);
    freader->sh[0]=reader->sh[0];freader->sh[1]=[NvShader new];freader->sh[1]->fn=program->sh[1]->fn;freader->sh[1]->stage=1;
    freader->sh[1]->rf.nb=1;freader->sh[1]->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=0,.reg=5,.aid=0};serial_put(rf.serial,freader);   // only its binding table matters here
    fwriter->sh[0]=reader->sh[0];NvShader *fws=[NvShader new];fws->fn=[lib newFunctionWithName:@"fw"];fws->stage=1;fws->ae=[fws->fn newArgumentEncoderWithBufferIndex:0];
    fws->rf.nb=1;fws->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=1,.reg=0,.aid=0};fwriter->sh[1]=fws;serial_put(fwp.serial,fwriter);
    p_bind_buf fbuf=buf;fbuf.stage=1;emit(OP_BIND_SSBO,&fbuf,sizeof fbuf);
    arrays=(p_draw){.prim=NVN_DRAW_PRIMITIVE_TRIANGLES,.count=3,.instances=1};p_draw writes=(p_draw){.prim=0,.count=4096,.instances=1};
    int32_t frag=NVN_BARRIER_ORDER_FRAGMENTS_BIT;
    const struct { prog_t *first; int32_t *bar; int then; unsigned passes, vbar; const char *what; } cases[]={   // then: 0 a draw without writes, 1 a vertex writer, 2 a fragment writer
        {&rd,&barrier,1,1,1,"a vertex-stage write after a primitive barrier stays in the pass behind a vertex barrier"},
        {&rd,&barrier,0,1,0,"a draw without writes after a primitive barrier changes nothing"},
        {&rd,NULL,1,1,0,"a storage write with no barrier changes nothing"},
        {&rf,&barrier,1,1,1,"a primitive barrier after a draw that reads in its fragment stage stays in the pass (fragments are not ordered)"},
        {&rf,&frag,1,2,0,"a fragment barrier after a draw that reads in its fragment stage ends the pass before a write"},
        {&rd,&frag,2,1,1,"a fragment-stage write after a fragment barrier and vertex-only reads stays in the pass behind a vertex-to-fragment barrier"}};
    for(int c=0;c<6;c++){
        discard(0);bp.prog=cases[c].first;emit(OP_BIND_PROGRAM,&bp,sizeof bp);emit(OP_DRAW,&arrays,sizeof arrays);
        if(cases[c].bar)emit(OP_BARRIER,cases[c].bar,4);
        if(cases[c].then==0)emit(OP_DRAW,&arrays,sizeof arrays);
        else if(cases[c].then==1){discard(1);bp.prog=&pp;emit(OP_BIND_PROGRAM,&bp,sizeof bp);emit(OP_DRAW,&writes,sizeof writes);}
        else{bp.prog=&fwp;emit(OP_BIND_PROGRAM,&bp,sizeof bp);emit(OP_DRAW,&arrays,sizeof arrays);}
        uint64_t before=g_stat_pass,vbar=g_order_vbar;execute();
        CHECK(g_stat_pass-before==cases[c].passes&&g_order_vbar-vbar==cases[c].vbar,cases[c].what);
    }
    printf("rasterizer discard: %d checks, %d failed\n",checks,fails);return fails!=0;
} }
