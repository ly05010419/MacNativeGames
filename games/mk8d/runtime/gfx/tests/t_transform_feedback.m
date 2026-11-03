// Production feedback replay, append/resume, overflow and GPU-count gathering.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c,label) do { checks++; if(!(c)){fails++;fprintf(stderr,"FAIL: %s\n",label);} }while(0)
static void finish(void) { CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"submission completes");CHECK(!mtl_last_commit_error(),"no GPU error"); }
static NvShader *shader(NSString *path) {
    NSError *e=nil;NSString *src=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&e];
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:src options:nil error:&e];
    if(!lib){fprintf(stderr,"%s\n",e.localizedDescription.UTF8String);abort();}
    NvShader *vs=[NvShader new];vs->fn=[lib newFunctionWithName:@"main0"];vs->tf_fn=[lib newFunctionWithName:@"main0_tf"];vs->tf_stride[0]=20;
    return vs;
}
static void state(int what,void *control) {p_tf_state s={.what=what,.ctl=(uint64_t)control};state_op(OP_TF_STATE,&s);tf_state_gpu(&s);}
static void bind(void *output,size_t size) {p_tf_bind b={.index=0,.addr=(uint64_t)output,.size=size};state_op(OP_TF_BIND,&b);}
static void record(const float *p,unsigned n) {const float want[5]={n,n+10,n+40,n+50,n+60};CHECK(!memcmp(p,want,sizeof want),"interleaved captured values and order");}
static uint8_t *commands;static size_t used;
static void emit(unsigned op,const void *p,size_t n) {size_t z=(8+n+7)&~7ull;op_hdr h={op,(uint32_t)z};memcpy(commands+used,&h,8);if(n)memcpy(commands+used+8,p,n);used+=z;}
int main(int argc,char **argv) { @autoreleasepool {
    if(argc!=2){fprintf(stderr,"pass the fixture made by test_transform_feedback.py --fixture\n");return 2;}
    setenv("DM_NVN_TBDR","0",1);mtl_init();state_reset();memset(&R,0,sizeof R);
    void *memory=NULL;if(posix_memalign(&memory,getpagesize(),65536))abort();memset(memory,0,65536);mtl_pool_add(memory,65536);
    float *output=(float *)memory;uint32_t *ctl=(uint32_t *)((uint8_t *)memory+4096),*ctl2=ctl+16,*ctl3=ctl+32;
    NvProg *pr=[NvProg new];pr->sh[0]=shader(@(argv[1]));
    p_draw dw={.prim=0,.first=2,.count=3,.instances=1};S.rast_discard=1;
    mtl_sub_begin();bind(output,100);state(0,ctl);tf_draw(pr,&dw,NULL);state(2,ctl);finish();
    CHECK(ctl[0]==60&&ctl[4]==3&&ctl[5]==1,"pause saves byte offsets and draw count");
    for(unsigned i=0;i<3;i++)record(output+5*i,i+2);
    mtl_sub_begin();state(3,ctl);dw.first=9;tf_draw(pr,&dw,NULL);state(1,ctl2);finish();
    CHECK(ctl2[0]==100&&ctl2[4]==5,"resume appends only the vertices that fit");
    record(output+15,9);record(output+20,10);CHECK(output[25]==0,"overflow leaves the next record untouched");
    // Source count is zero on the CPU until an earlier GPU copy supplies five.
    // Whole-triangle truncation accepts three, in packed-index order.
    uint32_t *indices=(uint32_t *)((uint8_t *)memory+8192),*args=indices+32,*source=args+16;
    const uint32_t order[5]={6,2,4,1,5};memcpy(indices,order,sizeof order);source[0]=5;
    NvShader *reader=shader([@(argv[1]) stringByAppendingString:@".geo"]);
    NSError *error=nil;id<MTLLibrary> drawlib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct A{device uint* p[[id(0)]];};\nvertex float4 v(uint n[[vertex_id]],constant A&a[[buffer(0)]]){return float4(0,0,0,1);}\nvertex void vd(uint n[[vertex_id]],constant A&a[[buffer(0)]]){a.p[n]=0x800+n;}\n" options:nil error:&error];
    CHECK(drawlib!=nil,"indirect-count consumer compiles");
    NvShader *consumer=[NvShader new];consumer->fn=[drawlib newFunctionWithName:@"v"];consumer->discard_fn=[drawlib newFunctionWithName:@"vd"];
    consumer->ae=[consumer->discard_fn newArgumentEncoderWithBufferIndex:0];consumer->rf.nb=1;consumer->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.rw=1,.reg=0,.aid=0};
    NvProg *drawpr=[NvProg new];drawpr->sh[0]=consumer;static prog_t pg={.serial=910601};serial_put(pg.serial,drawpr);
    tex_t target={.magic=MAGIC('T','E','X',0),.serial=910602};target.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=8,.height=8,.depth=1,.levels=1,.samples=1};
    memset(output,0,256);mtl_sub_begin();bind(output,100);state(0,ctl3);
    void *mb;uint64_t off,gpu;mtl_gpu_addr((uint64_t)args,4,&mb,&off,&gpu);
    void *sb;uint64_t so,sg;mtl_gpu_addr((uint64_t)source,4,&sb,&so,&sg);
    enc_b_copy_buffer(blit(),(__bridge id<MTLBuffer>)sb,so,(__bridge id<MTLBuffer>)mb,off,4);
    void *ib;uint64_t io,ig;mtl_gpu_addr((uint64_t)indices,sizeof order,&ib,&io,&ig);
    tf_geometry(reader,ig,gpu,3,0);state(1,ctl3);
    // Consume the saved count in the same submission, without a CPU readback.
    commands=(uint8_t *)memory+32768;uint32_t *written=(uint32_t *)((uint8_t *)memory+16384);
    p_set_rt rt={.n=1};rt.color[0]=&target;emit(OP_SET_RT,&rt,sizeof rt);
    p_bind_program bp={.prog=&pg,.stages=1};emit(OP_BIND_PROGRAM,&bp,sizeof bp);
    p_bind_buf ssbo={.stage=0,.index=0,.addr=(uint64_t)written,.size=64};emit(OP_BIND_SSBO,&ssbo,sizeof ssbo);
    p_rast_discard rd={.on=1};emit(OP_RAST_DISCARD,&rd,sizeof rd);
    p_draw draw={.prim=0,.instances=1,.ind=(uint64_t)ctl3+16};emit(OP_DRAW,&draw,sizeof draw);emit(OP_END,NULL,0);
    mtl_exec(commands);finish();
    CHECK(ctl3[0]==60&&ctl3[4]==3,"geometry uses the GPU count and keeps complete primitives");
    record(output,6);record(output+5,2);record(output+10,4);CHECK(output[15]==0,"incomplete final primitive is not captured");
    CHECK(written[0]==0x800&&written[1]==0x801&&written[2]==0x802&&written[3]==0,"saved GPU count controls the following draw");
    // The earlier single-output, three-stream skinning layout retains its
    // independent byte strides and offsets with the GPU counter implementation.
    pr->sh[0]=shader([@(argv[1]) stringByAppendingString:@".split"]);
    pr->sh[0]->tf_stride[0]=12;pr->sh[0]->tf_stride[1]=pr->sh[0]->tf_stride[2]=4;
    float *second=output+128,*third=output+160;memset(output,0,1024);
    mtl_sub_begin();bind(output,36);
    p_tf_bind b1={.index=1,.addr=(uint64_t)second,.size=12},b2={.index=2,.addr=(uint64_t)third,.size=12};state_op(OP_TF_BIND,&b1);state_op(OP_TF_BIND,&b2);
    state(0,ctl);dw.first=4;dw.count=3;tf_draw(pr,&dw,NULL);state(1,ctl);finish();
    CHECK(ctl[0]==36&&ctl[1]==12&&ctl[2]==12&&ctl[4]==3,"three streams advance by their own byte strides");
    const float position[9]={4,14,24,5,15,25,6,16,26},packed[3]={44,45,46};
    CHECK(!memcmp(output,position,sizeof position),"separate position stream");
    CHECK(!memcmp(second,packed,sizeof packed)&&!memcmp(third,packed,sizeof packed),"separate packed streams");
    // Tropical Freeze records feedback with rasterization enabled and no colour
    // writes. Exercise the command replay branch as well as the capture helper.
    state_reset();memset(&R,0,sizeof R);memset(&S.chmask,0,sizeof S.chmask);
    pr->sh[0]=shader(@(argv[1]));static prog_t tfpg={.serial=910603};serial_put(tfpg.serial,pr);
    memset(output,0,256);used=0;mtl_sub_begin();emit(OP_SET_RT,&rt,sizeof rt);
    bp.prog=&tfpg;emit(OP_BIND_PROGRAM,&bp,sizeof bp);
    p_tf_bind capture={.index=0,.addr=(uint64_t)output,.size=100};emit(OP_TF_BIND,&capture,sizeof capture);
    p_tf_state begin={.what=0,.ctl=(uint64_t)ctl},end={.what=1,.ctl=(uint64_t)ctl};
    emit(OP_TF_STATE,&begin,sizeof begin);dw.first=1;dw.count=3;emit(OP_DRAW,&dw,sizeof dw);
    emit(OP_TF_STATE,&end,sizeof end);emit(OP_END,NULL,0);mtl_exec(commands);finish();
    CHECK(ctl[0]==60&&ctl[4]==3,"rasterized draw records feedback through command replay");
    for(unsigned i=0;i<3;i++)record(output+5*i,i+1);
    printf("feedback: %d checks, %d failed\n",checks,fails);return fails!=0;
} }
