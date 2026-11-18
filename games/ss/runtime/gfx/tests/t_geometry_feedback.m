// End-to-end GS feedback replay, including pause/resume and raster discard.
#define main geometry_fixture_main
#include "t_geometry.m"
#undef main
static void feedback_replay(int discard) {
    id<MTLLibrary> l=library(@"#include <metal_stdlib>\nusing namespace metal;\n"
      "struct C{device float*p;uint4 info;};vertex void capture(uint v[[vertex_id]],constant C&c[[buffer(13)]]){c.p[(v-c.info.x)*144]=float(v);}\n"
      "vertex float4 unused(){return float4(0,0,0,1);}\n"
      "struct G{device const float*in[[id(0)]];device float4*out[[id(1)]];device uint*ix[[id(2)]];constant uint4*info[[id(3)]];device const uint*map[[id(4)]];};\n"
      "kernel void geometry(constant G&g[[buffer(0)]],uint3 t[[thread_position_in_grid]]){if(t.x>=g.info[1].x)return;float x=g.in[g.map[t.x]*144]*.8-.9;float2 d[3]={float2(0,0),float2(.6,0),float2(0,.6)};for(uint k=0;k<3;k++){uint v=t.x*3+k;g.out[v]=float4(x+d[k].x,-.8+d[k].y,0,1);g.ix[t.x*4+k]=v;}g.ix[t.x*4+3]=0xffffffffu;}\n"
      "struct V{device const float4*p[[id(0)]];};vertex float4 reader(uint v[[vertex_id]],constant V&a[[buffer(0)]]){return a.p[v];}\n"
      "struct T{uint4 offset,args;device const uint*order;uint first,count,base_instance,pad;ulong reserved;};\n"
      "vertex void feedback(uint v[[vertex_id]],constant V&a[[buffer(0)]],device float4*out[[buffer(8)]],constant T&t[[buffer(14)]]){if(v<t.args.x)out[t.offset.x/16+v]=a.p[t.order[v]];}\n"
      "fragment float4 red(){return float4(1,0,0,1);}\n");
    int helpers[]={0,1,2,3,4},rh[]={1};NvProg *pr=[NvProg new];
    pr->sh[0]=shader(l,@"unused",0,NULL,0);pr->sh[0]->geo_fn=[l newFunctionWithName:@"capture"];
    pr->sh[1]=shader(l,@"red",1,NULL,0);NvShader *gs=pr->sh[2]=shader(l,@"geometry",2,helpers,5);
    strcpy(gs->key,"000000000000feed_b");gs->geo_vertices=3;gs->geo_invocations=1;gs->geo_output=7;gs->geo_stride=4;gs->geo_index_stride=4;
    NvShader *reader=shader(l,@"reader",0,rh,1);reader->tf_fn=[l newFunctionWithName:@"feedback"];reader->tf_stride[0]=16;
    g_shcache=[NSMutableDictionary dictionary];g_shcache[@"000000000000feed_bg0"]=gs;g_shcache[@"000000000000feed_bgv"]=reader;
    static prog_t pg[2];pg[discard].serial=9960+discard;serial_put(pg[discard].serial,pr);
    uint8_t *cmd;posix_memalign((void**)&cmd,getpagesize(),65536);memset(cmd,0,65536);mtl_pool_add(cmd,65536);size_t used=0;
    #define EMIT(OP,X) do{size_t z=(8+sizeof(X)+7)&~7ull;op_hdr h={OP,(uint32_t)z};memcpy(cmd+used,&h,8);memcpy(cmd+used+8,&(X),sizeof(X));used+=z;}while(0)
    tex_t target={.magic=MAGIC('T','E','X',0),.serial=9970+discard,.d={.target=NVN_TEXTURE_TARGET_2D,.format=NVN_FORMAT_RGBA8,.width=32,.height=32,.depth=1,.levels=1,.samples=1}};
    p_set_rt rt={.n=1};rt.color[0]=&target;EMIT(OP_SET_RT,rt);p_viewports vp={.n=1};vp.v[0][2]=vp.v[0][3]=32;EMIT(OP_VIEWPORTS,vp);
    p_clear_color cl={.mask=15,.rgba={0,0,0,1}};EMIT(OP_CLEAR_COLOR,cl);p_bind_program bp={&pg[discard],7};EMIT(OP_BIND_PROGRAM,bp);
    p_rast_discard rd={.on=discard};EMIT(OP_RAST_DISCARD,rd);
    p_tf_bind bind={.index=0,.addr=(uint64_t)(cmd+32768),.size=4096};EMIT(OP_TF_BIND,bind);
    p_tf_state state={.what=0,.ctl=(uint64_t)(cmd+8192)};EMIT(OP_TF_STATE,state);
    p_draw d={.prim=0,.count=2,.instances=1};EMIT(OP_DRAW,d);
    state.what=2;EMIT(OP_TF_STATE,state);d.first=4;d.count=1;EMIT(OP_DRAW,d); // Paused output must not be captured.
    state.what=3;EMIT(OP_TF_STATE,state);d.first=2;d.count=2;EMIT(OP_DRAW,d);
    state.what=1;state.ctl=(uint64_t)(cmd+8256);EMIT(OP_TF_STATE,state);op_hdr end={OP_END,8};memcpy(cmd+used,&end,8);
    #undef EMIT
    state_reset();memset(&R,0,sizeof R);mtl_sub_begin();uint64_t h=(uint64_t)cmd;mtl_sub_index(&h,1);mtl_exec(cmd);
    id<MTLBuffer> rb=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    [blit() copyFromTexture:tex_mtl(&target) sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(32,32,1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:128 destinationBytesPerImage:4096];finish();
    uint32_t *pause=(uint32_t*)(cmd+8192),*done=(uint32_t*)(cmd+8256);
    EXPECT("GS pause saves six vertices",6,pause[4]);EXPECT("GS resume appends six vertices",12,done[4]);EXPECT("GS feedback byte offset",192,done[0]);
    float *out=(float*)(cmd+32768);const float corner[3][2]={{0,0},{.6f,0},{0,.6f}};
    for(int input=0;input<4;input++)for(int v=0;v<3;v++) {
        float *p=out+(input*3+v)*4;
        EXPECT("GS feedback x follows ordered emitted vertices",1,fabsf(p[0]-(input*.8f-.9f+corner[v][0]))<1e-5f);
        EXPECT("GS feedback y follows ordered emitted vertices",1,fabsf(p[1]-(-.8f+corner[v][1]))<1e-5f);
        EXPECT("GS feedback z",1,p[2]==0);EXPECT("GS feedback w",1,p[3]==1);
    }
    EXPECT("paused GS draw and append canary",0,((uint32_t*)out)[48]);
    int red=0;uint8_t *px=rb.contents;for(int i=0;i<1024;i++)red+=px[4*i]>200;
    EXPECT("GS feedback preserves raster discard",!discard,red>0);
    mtl_tex_release(&target);mtl_pool_del(cmd);mtl_drain();free(cmd);
}
int main(void){@autoreleasepool{mtl_init();feedback_replay(0);feedback_replay(1);printf("t_geometry_feedback: %d checks, %d failed\n",checks,failed);return failed!=0;}}
