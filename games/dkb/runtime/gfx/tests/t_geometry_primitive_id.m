// Real translated Maxwell PrimitiveID through production splitting and feedback.
#define main geometry_fixture_main
#include "t_geometry.m"
#undef main
static uint32_t source_index(unsigned primitive, unsigned corner, unsigned width) {
    return 5 + (primitive / GEO_RUN_PRIMS) * 400000 + (primitive % GEO_RUN_PRIMS) * width + corner;
}
static int run_case(NSString *directory, unsigned topology, unsigned invocations, unsigned instances, unsigned primitives) {
    const unsigned prim[]={0,1,10,4,12},width[]={1,2,4,3,6},vertices[]={1,2,2,3,3};
    const unsigned source[5][3]={{0},{0,1},{1,2},{0,1,2},{0,2,4}};
    NSString *stem=[directory stringByAppendingPathComponent:[NSString stringWithFormat:@"pid%u_%u",topology,invocations]];
    NSError *error=nil;NSDictionary *meta=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[stem stringByAppendingString:@".json"]] options:0 error:&error];
    id<MTLLibrary> lib=[g_dev newLibraryWithURL:[NSURL fileURLWithPath:[stem stringByAppendingString:@".metallib"]] error:&error];
    if(!meta||!lib){fprintf(stderr,"fixture missing: %s\n",error.description.UTF8String);return 1;}
    NvShader *gs=shader(lib,@"main0",2,NULL,0);char message[160]={0};
    if(nvn_refl_from_json(meta,&gs->rf,message,sizeof message)){fprintf(stderr,"%s\n",message);return 1;}
    gs->ae=[gs->fn newArgumentEncoderWithBufferIndex:0];gs->arg_len=gs->ae.encodedLength;
    NSDictionary *layout=meta[@"geometry"];gs->geo_vertices=[layout[@"vertices"] unsignedIntValue];gs->geo_invocations=invocations;
    gs->geo_output=[layout[@"output"] unsignedIntValue];gs->geo_stride=[layout[@"stride"] unsignedIntValue];gs->geo_index_stride=[layout[@"index_stride"] unsignedIntValue];
    strcpy(gs->key,"000000000000c001_b");
    id<MTLLibrary> vertex=[g_dev newLibraryWithURL:[NSURL fileURLWithPath:[directory stringByAppendingPathComponent:@"vertex.metallib"]] error:&error];
    NvProg *pr=[NvProg new];pr->sh[2]=gs;pr->sh[0]=shader(vertex,@"main0",0,NULL,0);pr->sh[0]->geo_fn=[vertex newFunctionWithName:@"main0_gs"];
    NSString *readerSource=[NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\nstruct V{device const uint*p[[id(0)]];};\nvertex float4 reader(uint v[[vertex_id]],constant V&a[[buffer(0)]]){return as_type<float4>(uint4(a.p[v*%uu],0,0,0x3f800000u));}\nstruct T{uint4 offset,args;device const uint*order;uint first,count,base_instance,pad;ulong reserved;};\nvertex void feedback(uint v[[vertex_id]],constant V&a[[buffer(0)]],device uint4*out[[buffer(8)]],constant T&t[[buffer(14)]]){if(v<t.args.x){uint n=t.order[v]*%uu;out[t.offset.x/16+v]=uint4(a.p[n+5],a.p[n+6],a.p[n+7],a.p[n+8]);}}\n",gs->geo_stride,gs->geo_stride];
    id<MTLLibrary> rl=library(readerSource);int rh[]={1};NvShader *reader=shader(rl,@"reader",0,rh,1);reader->tf_fn=[rl newFunctionWithName:@"feedback"];reader->tf_stride[0]=16;
    g_shcache=[NSMutableDictionary dictionary];g_shcache[@"000000000000c001_bg0"]=gs;g_shcache[@"000000000000c001_bg1"]=gs;g_shcache[@"000000000000c001_bg2"]=gs;g_shcache[@"000000000000c001_bg3"]=gs;g_shcache[@"000000000000c001_bg4"]=gs;g_shcache[@"000000000000c001_bgv"]=reader;
    size_t indexBytes=(size_t)primitives*width[topology]*4,records=(size_t)primitives*vertices[topology]*instances*invocations;
    size_t outputOffset=(indexBytes+4095)&~(size_t)4095,total=outputOffset+records*16+8192;
    uint8_t *memory;posix_memalign((void**)&memory,getpagesize(),total);memset(memory,0xcc,total);mtl_pool_add(memory,total);
    uint32_t *indices=(uint32_t*)memory;for(unsigned p=0;p<primitives;p++)for(unsigned k=0;k<width[topology];k++)indices[p*width[topology]+k]=source_index(p,k,width[topology]);
    uint32_t *output=(uint32_t*)(memory+outputOffset),*control=(uint32_t*)(memory+total-4096);
    state_reset();memset(&R,0,sizeof R);S.rast_discard=1;mtl_sub_begin();
    p_tf_bind bind={.index=0,.addr=(uint64_t)output,.size=(records+1)*16};state_op(OP_TF_BIND,&bind);
    p_tf_state begin={.what=0};state_op(OP_TF_STATE,&begin);tf_state_gpu(&begin);
    p_draw_idx draw={.prim=prim[topology],.itype=2,.count=(int32_t)(primitives*width[topology]),.instances=(int32_t)instances,.base_vertex=-5,.base_instance=7,.ibuf=(uint64_t)indices};
    geo_draw(pr,NULL,&draw,0);
    p_tf_state end={.what=1,.ctl=(uint64_t)control};state_op(OP_TF_STATE,&end);tf_state_gpu(&end);finish();
    int bad=control[4]!=records||control[0]!=records*16;
    for(unsigned i=0;i<instances&&!bad;i++)for(unsigned p=0;p<primitives&&!bad;p++)for(unsigned j=0;j<invocations&&!bad;j++)for(unsigned k=0;k<vertices[topology];k++) {
        size_t n=(((size_t)i*primitives+p)*invocations+j)*vertices[topology]+k;uint32_t v=source_index(p,source[topology][k],width[topology])-5;
        uint32_t want[]={p+1,v,7+i,v^(7+i)};
        if(memcmp(output+n*4,want,sizeof want)){fprintf(stderr,"PrimitiveID topology %u instances %u invocations %u primitives %u, primitive %u instance %u invocation %u corner %u: got %u/%u/%u/%u expected %u/%u/%u/%u\n",topology,instances,invocations,primitives,p,i,j,k,output[n*4],output[n*4+1],output[n*4+2],output[n*4+3],want[0],want[1],want[2],want[3]);bad=1;break;}
    }
    if(output[records*4]!=0xccccccccu)bad=1;
    if(bad)fprintf(stderr,"feedback count %u bytes %u expected %zu/%zu; canary %#x\n",control[4],control[0],records,records*16,output[records*4]);
    mtl_pool_del(memory);mtl_drain();free(memory);return bad;
}
int main(int argc,char **argv){@autoreleasepool{
    if(argc!=2)return 2;mtl_init();unsigned cases=0;
    for(unsigned t=0;t<5;t++)for(unsigned inv=1;inv<=3;inv+=2)for(unsigned instances=1;instances<=2;instances++)for(unsigned n=1;n<=8199;n+=8198){if(run_case(@(argv[1]),t,inv,instances,n))return 1;cases++;}
    printf("compute split PrimitiveID: %u GPU cases passed\n",cases);return failed!=0;
}}
