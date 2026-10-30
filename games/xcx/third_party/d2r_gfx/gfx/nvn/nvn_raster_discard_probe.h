/* Vertex producers and consumers reuse one storage allocation between NVN barriers.
   Each consumer saves its round's data so a later producer cannot hide a race. */
static int probe_vertex_barriers(void) {
    enum { count=8192, rounds=32, words=count*(rounds+1), bytes=words*4+16384 };
    void *memory=NULL; if(posix_memalign(&memory,16384,(bytes+16383)&~16383))MDIE("vertex barrier memory");
    memset(memory,0xa5,bytes); mtl_pool_add(memory,(bytes+16383)&~16383);
    NvProg *programs[2]={[NvProg new],[NvProg new]}; prog_t packets[2]={0};
    for(int reader=0;reader<2;reader++) {
        NSString *source=[@"#include <metal_stdlib>\nusing namespace metal;\n"
          "struct A { device uint *scratch [[id(0)]]; device uint *saved [[id(1)]]; constant uint *p [[id(2)]]; };\n"
          "struct main0_out { float4 position [[position]]; };\n"
          "vertex main0_out main0(uint i [[vertex_id]], constant A &a [[buffer(0)]]) { main0_out out={};\n"
          stringByAppendingString:reader?@"a.saved[a.p[0]*8192+i]=a.scratch[i];\nreturn out;\n}\n":
                                      @"a.scratch[i]=i^(0x13579bdu+a.p[0]*0x10203u);\nreturn out;\n}\n"];
        NSError *error=nil; id<MTLLibrary> lib=[g_dev newLibraryWithSource:source options:nil error:&error];
        if(!lib)MDIE("vertex barrier shader: %s",error.localizedDescription.UTF8String);
        NvShader *vs=[NvShader new]; vs->fn=[lib newFunctionWithName:@"main0"]; vs->src=source; vs->io=@{};
        snprintf(vs->key,sizeof vs->key,"probe-vertex-barrier-%d",reader);
        vs->rf.route_b=1; vs->rf.nb=3;
        vs->rf.b[0]=(nvn_rbind){.kind=NVN_RB_SBUFP,.aid=0,.reg=0,.rw=!reader};
        vs->rf.b[1]=(nvn_rbind){.kind=NVN_RB_SBUFP,.aid=1,.reg=1,.rw=reader};
        vs->rf.b[2]=(nvn_rbind){.kind=NVN_RB_CBUF,.aid=2,.reg=0};
        vs->ae=[vs->fn newArgumentEncoderWithBufferIndex:0]; vs->arg_len=vs->ae.encodedLength;
        programs[reader]->sh[0]=vs; packets[reader].serial=0xffffffb0+reader; serial_put(packets[reader].serial,programs[reader]);
    }
    const char *negative=getenv("TOTK_PROBE_NEGATIVE"); int omit=negative && atoi(negative)==2, checked=0;
    for(int stream=0;stream<2;stream++) {
        atomic_store(&g_rs_on,stream); memset(memory,0xa5,bytes);
        uint8_t *ops=malloc(65536), *w=ops; if(!ops)MDIE("vertex barrier ops");
        p_set_rt empty={0}; w=lp_rec(w,OP_SET_RT,&empty,sizeof empty);
        p_flag discard={1}; w=lp_rec(w,OP_RASTER_DISCARD,&discard,sizeof discard);
        p_bind_buf scratch={0,0,(uintptr_t)memory,count*4}, saved={0,1,(uintptr_t)memory+count*4,count*rounds*4};
        w=lp_rec(w,OP_BIND_SSBO,&scratch,sizeof scratch); w=lp_rec(w,OP_BIND_SSBO,&saved,sizeof saved);
        for(unsigned round=0;round<rounds;round++) {
            uint32_t *param=(uint32_t *)((uint8_t *)memory+words*4+round*256); *param=round;
            p_bind_buf uniform={0,0,(uintptr_t)param,16}; w=lp_rec(w,OP_BIND_UBO,&uniform,sizeof uniform);
            for(int reader=0;reader<2;reader++) {
                w=lp_prog(w,&packets[reader]); p_draw draw={.prim=0,.count=count}; w=lp_rec(w,OP_DRAW,&draw,sizeof draw);
                int32_t barrier[2]={omit?0:reader?0x20:1,0}; w=lp_rec(w,OP_BARRIER,barrier,sizeof barrier);
            }
        }
        lp_end(w); mtl_sub_begin(); mtl_exec(ops); rp_end(); mtl_sub_commit(); mtl_finish(); free(ops);
        const uint32_t *result=(const uint32_t *)memory+count;
        for(unsigned round=0;round<rounds;round++) for(unsigned i=0;i<count;i++) {
            uint32_t want=i^(0x13579bdu+round*0x10203u), got=result[round*count+i];
            if(got!=want)MDIE("vertex barrier stream%d round%u word%u got%#x want%#x",stream,round,i,got,want);
            checked++;
        }
    }
    memset(&S,0,sizeof S); args_dirty_all(); mtl_pool_del(memory); mtl_finish(); free(memory); return checked;
}
/* Asset-free production replay regression: discard keeps vertex storage writes,
   suppresses fragments, and switching it in one encoder invalidates the PSO. */
int mtl_probe_raster_discard(void) {
    mtl_init(); setenv("TOTK_GPU_MIDCLEAR","1",1); int checked = 0;
    void *mem = NULL; if (posix_memalign(&mem,16384,16384)) MDIE("discard probe pool");
    memset(mem,0xa5,16384); mtl_pool_add(mem,16384);
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
      "struct A { device uint *values [[id(0)]]; };\n"
      "struct main0_out { float4 p [[position]]; };\n"
      "fragment float4 fs() { return float4(1,0,0,1); }\n"
      "vertex main0_out main0(uint i [[vertex_id]], constant A &a [[buffer(0)]])\n"
      "{ main0_out out = {}; float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
      "a.values[i]=0xabc000u+i; out.p=float4(p[i],0,1);\nreturn out;\n}\n";
    NSError *error = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:source options:nil error:&error];
    if (!lib) MDIE("discard probe shader: %s",error.localizedDescription.UTF8String);
    NvShader *vs = [NvShader new], *fs = [NvShader new];
    vs->fn = [lib newFunctionWithName:@"main0"]; vs->src = source; strcpy(vs->key,"probe-discard-vs"); vs->io = @{};
    vs->rf.route_b = 1; vs->rf.nb = 1; vs->rf.b[0] = (nvn_rbind){.kind=NVN_RB_SBUFP,.aid=0,.reg=0,.rw=1};
    vs->ae = [vs->fn newArgumentEncoderWithBufferIndex:0]; vs->arg_len = vs->ae.encodedLength;
    fs->fn = [lib newFunctionWithName:@"fs"]; fs->stage = 1; fs->io = @{}; strcpy(fs->key,"probe-discard-fs");
    NvProg *programs[2] = {[NvProg new],[NvProg new]}; prog_t packets[2] = {0};
    for (int i=0;i<2;i++) { programs[i]->sh[0]=vs; if (!i) programs[i]->sh[1]=fs;
        packets[i].serial=0xffffffa0+i; serial_put(packets[i].serial,programs[i]); }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:32 height:8 mipmapped:NO];
    td.storageMode=MTLStorageModePrivate; td.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> texture=[g_dev newTextureWithDescriptor:td]; res_add(texture); id<MTLBuffer> rb=probe_buf(1024);
    tex_t target={0}; target.serial=0xffffffa2; target.d.format=0x25; target.d.width=32; target.d.height=8;
    target.d.depth=target.d.levels=target.d.samples=1; target.d.target=1; serial_put(target.serial,texture);
    const char *neg=getenv("TOTK_PROBE_NEGATIVE"); int negative=neg?atoi(neg):0;
    for (int repeat=0;repeat<2;repeat++) for (int scenario=0;scenario<6;scenario++) {
        memset(mem,0xa5,16384); uint8_t ops[4096]; uint8_t *w=ops;
        if (scenario!=5) { w=lp_rt(w,&target,0,0); w=lp_sc(w,0,0,32,8); w=lp_clear(w,0,0,0,1); }
        else { p_set_rt empty={0}; w=lp_rec(w,OP_SET_RT,&empty,sizeof empty); }
        w=lp_prog(w,&packets[scenario==3]); lp_end(w);
        mtl_sub_begin(); mtl_exec(ops); S.chmask.mask=15; g_pso_dirty=1;
        for (int phase=0;phase<4;phase++) {
            int discard=scenario>=3 || !(phase&1);
            p_flag flag={discard}; state_op(OP_RASTER_DISCARD,&flag);
            flag.value=scenario!=4; state_op(OP_RENDER_ENABLE,&flag);
            p_bind_buf storage={0,0,(uintptr_t)mem+phase*64,64}; state_op(OP_BIND_SSBO,&storage);
            S.poly.cull=discard && scenario==1?NVN_FACE_FRONT_AND_BACK:NVN_FACE_NONE;
            R.has_sc=1; R.sc=(p_scissor){phase*8,0,discard && scenario==2?0:8,8};
            p_draw d={.prim=4,.count=3};
            /* Negative control recreates the original lost vertex work. */
            if (!(negative==1 && discard)) draw_inner(&d,NULL);
        }
        rp_end();
        if (scenario!=5) enc_b_copy_texture_buffer(blit(),texture,0,0,MTLOriginMake(0,0,0),MTLSizeMake(32,8,1),rb,0,128,1024);
        mtl_sub_commit(); mtl_finish();
        for (int phase=0;phase<4;phase++) for (int i=0;i<16;i++) {
            uint32_t want=scenario!=4 && i<3?0xabc000u+(unsigned)i:0xa5a5a5a5u;
            uint32_t got=((uint32_t *)mem)[phase*16+i];
            if (got!=want) MDIE("discard storage scenario%d repeat%d phase%d word%d got%#x want%#x",scenario,repeat,phase,i,got,want);
            checked++;
        }
        if (scenario!=5) for (int i=0;i<1024;i++) {
            int channel=i%4, stripe=(i/4)%32/8;
            int want=channel==3?255:channel==0 && scenario<3 && (stripe&1)?255:0;
            if (((uint8_t *)rb.contents)[i]!=want) MDIE("discard pixels scenario%d repeat%d byte%d",scenario,repeat,i);
            checked++;
        }
    }
    memset(&S,0,sizeof S); args_dirty_all(); mtl_pool_del(mem); mtl_finish(); free(mem);
    res_remove(texture); res_remove(rb); return checked+probe_vertex_barriers();
}
