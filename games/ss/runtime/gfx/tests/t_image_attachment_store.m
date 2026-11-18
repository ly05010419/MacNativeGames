// A loaded color attachment and a fragment image refer to the same array texture.
// The image-only draw must survive pass end; raster outputs and clears executed in that pass still need their tile store.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)
static tex_t color = {.magic=MAGIC('T','E','X',0),.serial=99801};
static prog_t programs[3] = {{.serial=99802},{.serial=99803},{.serial=99804}};
static void seed(void) {
    for (unsigned layer=0;layer<4;layer++) {
        MTL4RenderPassDescriptor *p=[MTL4RenderPassDescriptor new];
        p.colorAttachments[0].texture=tex_mtl(&color); p.colorAttachments[0].slice=layer;
        p.colorAttachments[0].loadAction=MTLLoadActionClear;
        p.colorAttachments[0].storeAction=MTLStoreActionStore;
        p.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
        enc_r_end(enc_r_begin(p));
    }
}
static void run(int mode, int store_all) {
    int raster=mode==1 || mode==4 || mode==6;
    // Metal 4 encoders do not own pipeline lifetimes. Keep the boundary fixture's uncached pipeline alive through deferred replay and GPU completion.
    __attribute__((objc_precise_lifetime)) id<MTLRenderPipelineState> boundary_pipeline=nil;
    static unsigned frame;
    state_reset(); memset(&R,0,sizeof R); mtl_set_frame(++frame); mtl_sub_begin(); seed();
    g_udc_ab=!store_all; g_udc_ab_on=0;
    S.img[1][0]=701;
    struct { op_hdr rh; p_set_rt rt; op_hdr ph; p_bind_program pr; op_hdr dh; p_draw d; op_hdr end; } code={0};
    code.rh=(op_hdr){OP_SET_RT,sizeof(op_hdr)+sizeof(p_set_rt)}; code.rt.n=1; code.rt.color[0]=&color;
    code.ph=(op_hdr){OP_BIND_PROGRAM,sizeof(op_hdr)+sizeof(p_bind_program)}; code.pr=(p_bind_program){&programs[mode==2||mode==3?2:raster],3};
    code.dh=(op_hdr){OP_DRAW,sizeof(op_hdr)+sizeof(p_draw)}; code.d=(p_draw){NVN_DRAW_PRIMITIVE_TRIANGLES,0,3};
    code.end=(op_hdr){OP_END,sizeof(op_hdr)};
    if(mode==5) S.chmask.mask=0;
    NvProg *pr=prog_get(&programs[raster]);
    if(mode==6) pr->sh[1]->rf.color_known=0;
    if(mode==2||mode==8) { R.has_rt=1; R.rt=code.rt; R.clear_c[0]=1; R.cc[0]=MTLClearColorMake(0,1,0,1); }
    uint64_t discarded=g_sd_n_dead;
    if(mode==7) { // Exercise the store-decision boundary with a real DontCare-load pass (image-only production passes normally load).
        R.has_rt=1; R.rt=code.rt;
        MTL4RenderPassDescriptor *pd=[MTL4RenderPassDescriptor new];
        pd.colorAttachments[0].texture=tex_mtl(&color); pd.colorAttachments[0].loadAction=MTLLoadActionDontCare;
        pd.colorAttachments[0].storeAction=MTLStoreActionUnknown; g_enc=enc_r_begin(pd);
        g_sd[0]=(sd_att){.ser=color.serial,.idx=0,.bytes=256,.load=0}; g_nsd=1;
        NvProg *tail=prog_get(&programs[2]); NvShader *fs=tail?tail->sh[1]:nil;
        if(!fs||!tail->sh[0]) { CHECK(0,"DontCare-load image program resolves its two stages"); rp_end(); return; } // NOLINT(clang-analyzer-osx.cocoa.RetainCount): g_by_serial owns the NvProg prog_get returned (serial_put's CFBridgingRetain)
        MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new]; d.vertexFunction=tail->sh[0]->fn; d.fragmentFunction=fs->fn;
        d.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm; d.colorAttachments[0].writeMask=MTLColorWriteMaskNone;
        NSError *error=nil; id<MTLRenderPipelineState> ps=[g_dev newRenderPipelineStateWithDescriptor:d error:&error];
        boundary_pipeline=ps; CHECK(ps!=nil,"DontCare-load image pipeline compiles");
        enc_r_pso(g_enc,ps); enc_r_viewport(g_enc,(MTLViewport){0,0,8,8,0,1});
        CHECK(encode_args((id<MTLCommandEncoder>)g_enc,fs,1,0),"DontCare-load image arguments encoded");
        udc_note_draw(tail,0); enc_r_draw(g_enc,MTLPrimitiveTypeTriangle,0,3);
    } else mtl_exec(mode==8 ? (const uint8_t *)&code.ph : (const uint8_t *)&code); // Keep mode 8's pending clear for the first draw; another SetRenderTargets would land it alone.
    if(mode==4) { code.pr=(p_bind_program){&programs[2],3}; mtl_exec((const uint8_t *)&code.ph); }
    if(mode==6) pr->sh[1]->rf.color_known=1;
    if(mode==3) clear_color_tri(&(p_clear_color){.index=0,.mask=15,.rgba={0,1,0,1}}); // NOLINT(clang-analyzer-osx.cocoa.RetainCount): g_by_serial owns the NvProg prog_get returned into pr (serial_put's CFBridgingRetain)
    rp_end();
    if(mode==7) CHECK(g_sd_n_dead==discarded,"DontCare-load attachment keeps its store");
    if(mode==8) CHECK(g_sd_n_dead==discarded+1,"a clear landed before the draw pass does not store stale tiles over image writes");
    id<MTLBuffer> pixels=[g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]; res_add(pixels);
    for (unsigned layer=0;layer<4;layer++)
        enc_b_copy_texture_buffer(blit(),tex_mtl(&color),layer,0,MTLOriginMake(0,0,0),MTLSizeMake(8,8,1),pixels,1024*layer,128,1024);
    CHECK(mtl_wait(mtl_sub_commit(),5000000000ull),"GPU submission completes");
    CHECK(!mtl_last_commit_error(),"GPU submission has no error");
    for (unsigned layer=0;layer<4;layer++) {
        if(mode==7&&!layer) continue; // DontCare leaves the attached layer unspecified.
        uint8_t want[4]={32+layer*32,64,128,255}; if(raster&&!layer) {want[0]=255;want[1]=want[2]=0;}
        if((mode==2||mode==3)&&!layer) {want[0]=want[2]=0;want[1]=255;}
        int good=1;
        for(unsigned y=0;y<8;y++) for(unsigned x=0;x<8;x++)
            good&=!memcmp((const uint8_t *)pixels.contents+1024*layer+128*y+4*x,want,4);
        char label[160]; snprintf(label,sizeof label,"%s layer %u survives pass end (unwritten-store optimization %s)",mode==8?"separate clear and image":mode==7?"DontCare load and image":mode==4?"earlier raster and later image":mode==5?"zero-mask image":mode==6?"unknown reflection and image":mode==2?"load clear and image":mode==3?"explicit clear and image":raster?"raster and image":"image-only",layer,store_all?"off":"on");
        CHECK(good,label);
        printf("%s: first pixel %u,%u,%u,%u\n",label,((uint8_t *)pixels.contents)[1024*layer],((uint8_t *)pixels.contents)[1024*layer+1],((uint8_t *)pixels.contents)[1024*layer+2],((uint8_t *)pixels.contents)[1024*layer+3]);
    }
}
// The facts passed to store tracking must use the selected pipeline's physical target map, before draw_ resets it.
static void mapping_checks(NvProg *pr) {
    state_reset();
    uint32_t original = pr->sh[1]->rf.color_mask;
    pr->sh[1]->rf.color_mask = 4;
    S.chmask.mask = 0xf00;
    const uint8_t logical[8] = {2,255,255,255,255,255,255,255}; g_pso_lmap = logical;
    CHECK(draw_color_writes(pr) == 1, "logical output 2 writes remapped physical attachment 0");
    S.chmask.mask = 0;
    CHECK(draw_color_writes(pr) == 0, "a remapped zero channel mask still writes no color");
    g_pso_lmap = NULL; S.chmask.mask = 15; pr->sh[1]->rf.color_mask = 0; g_pso_fsfn = pr->sh[1]->fn;
    CHECK(draw_color_writes(pr) == 1, "a specialized fragment does not inherit the original empty output map");
    g_pso_fsfn = nil; pr->sh[1]->rf.color_mask = original;
    NvShader *fs=pr->sh[1]; pr->sh[1]=nil;
    CHECK(draw_color_writes(pr) == 0, "no fragment shader writes no color attachment");
    pr->sh[1]=fs;
}
int main(void) { @autoreleasepool {
    setenv("DM_NVN_TBDR","0",1); mtl_init();
    color.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D_ARRAY,.format=NVN_FORMAT_RGBA8,.width=8,.height=8,.depth=4,.levels=1,.samples=1};
    MTLTextureDescriptor *td=tex_desc(&color,0); td.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture=[g_dev newTextureWithDescriptor:td]; CHECK(texture!=nil,"array texture created"); res_add(texture); mtl_tex_init(&color); serial_put(color.serial,texture); mtl_reg_image(701,&color,NULL); // NOLINT(clang-analyzer-osx.cocoa.RetainCount): serial_put transfers a retained reference to g_by_serial; the analyzer does not model its C hash table.
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint n[[vertex_id]]){return float4(n==1?3.0:-1.0,n==2?3.0:-1.0,0,1);}\n"
        "struct A{texture2d_array<float,access::write> image[[id(0)]];};\n"
        "void stores(float4 p,constant A&a,uint first){for(uint l=first;l<4;l++)a.image.write(float4(float(32+l*32)/255.0,64.0/255.0,128.0/255.0,1),uint2(p.xy),l);}\n"
        "fragment void image_only(float4 p[[position]],constant A&a[[buffer(0)]]){stores(p,a,0);}\n"
        "fragment void image_tail(float4 p[[position]],constant A&a[[buffer(0)]]){stores(p,a,1);}\n"
        "fragment float4 both(float4 p[[position]],constant A&a[[buffer(0)]]){stores(p,a,1);return float4(1,0,0,1);}\n"
        options:nil error:&error];
    CHECK(lib!=nil,"literal image-store shaders compile"); if(!lib){fprintf(stderr,"%s\n",error.localizedDescription.UTF8String);return 1;}
    for(unsigned mode=0;mode<3;mode++) { // NOLINT(clang-analyzer-osx.cocoa.RetainCount): the preceding iteration's NvProg is retained by g_by_serial through serial_put.
        NvProg *pr=[NvProg new]; pr->sh[0]=[NvShader new]; pr->sh[0]->fn=[lib newFunctionWithName:@"v"];
        NvShader *fs=[NvShader new]; pr->sh[1]=fs; fs->stage=1; fs->fn=[lib newFunctionWithName:mode==2?@"image_tail":mode?@"both":@"image_only"];
        fs->rf.color_known=1; fs->rf.color_mask=mode==1; fs->rf.nb=1;
        fs->rf.b[0]=(nvn_rbind){.kind=NVN_RB_IMG,.aid=0,.reg=0,.rw=1,.dtt=MTLTextureType2DArray};
        fs->ae=[fs->fn newArgumentEncoderWithBufferIndex:0]; fs->arg_len=fs->ae.encodedLength;
        serial_put(programs[mode].serial,pr);
    }
    mapping_checks(prog_get(&programs[0]));
    for(int mode=0;mode<9;mode++) for(int off=0;off<2;off++) run(mode,off); // NOLINT(clang-analyzer-osx.cocoa.RetainCount): g_by_serial owns the fixture programs throughout GPU replay.
    printf("IMAGE_ATTACHMENT_STORE %d checks, %d failed\n",checks,failed); return failed!=0;
} }
