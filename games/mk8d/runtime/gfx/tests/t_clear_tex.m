// Texture clears preserve channels and pixels outside their explicit mask and region.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)

int main(void) { @autoreleasepool {
    CHECK(setenv("DM_NVN_PASSDUMP", "0", 1) == 0, "enable pass dump for frame zero");
    mtl_init(); id<MTLCommandQueue> queue=[g_dev newCommandQueue];
    CHECK(passdump(), "null clear runs with pass-dump diagnostics enabled");
    clear_tex(&(p_clear_tex){0});   // A missing texture must also be safe in pass-dump diagnostics.
    struct { int mask, x, y, w, h, layer, level, ui; const char *label; } cases[]={
        {8,0,0,8,8,0,0,0,"alpha-only clear preserves RGB"},
        {5,2,1,3,2,0,0,0,"masked rectangle preserves other pixels and channels"},
        {15,2,1,3,2,0,0,0,"whole-channel rectangle"},
        {8,0,0,4,4,1,1,0,"mip and array layer selection"},
        {8,0,0,8,8,0,0,1,"integer alpha clear preserves exact values"},
        {0,0,0,8,8,0,0,0,"empty channel mask writes nothing"},
        {15,0,0,8,8,0,0,0,"whole clear keeps load-action path"},
    };
    for(unsigned i=0;i<sizeof cases/sizeof *cases;i++) {
        tex_t t={.magic=MAGIC('T','E','X',0),.serial=99601+i};
        t.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D_ARRAY,.format=cases[i].ui?NVN_FORMAT_RGBA8UI:NVN_FORMAT_RGBA8,.width=8,.height=8,.depth=2,.levels=2,.samples=1};
        MTLTextureDescriptor *d=tex_desc(&t,0); d.usage|=MTLTextureUsageRenderTarget;
        id<MTLTexture> base=[g_dev newTextureWithDescriptor:d]; CHECK(base!=nil,"target allocates"); if(!base)continue;
        for(int layer=0;layer<2;layer++)for(int lv=0;lv<2;lv++) {
            MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture=base;rp.colorAttachments[0].slice=layer;rp.colorAttachments[0].level=lv;
            rp.colorAttachments[0].loadAction=MTLLoadActionClear;rp.colorAttachments[0].storeAction=MTLStoreActionStore;
            rp.colorAttachments[0].clearColor=cases[i].ui?MTLClearColorMake(17,34,51,68):MTLClearColorMake(17./255,34./255,51./255,68./255);
            id<MTLCommandBuffer> cb=[queue commandBuffer];[[cb renderCommandEncoderWithDescriptor:rp] endEncoding];[cb commit];[cb waitUntilCompleted];
        }
        res_add(base);mtl_tex_init(&t);serial_put(t.serial,base);
        state_reset();memset(&R,0,sizeof R);mtl_sub_begin();
        // ClearTexture uses its region, independently of the current draw scissor.
        R.has_sc=1;R.sc=(p_scissor){0,0,1,1};
        struct { op_hdr head; p_clear_tex clear; op_hdr end; } ops={0};
        ops.head=(op_hdr){OP_CLEAR_TEX,sizeof(op_hdr)+sizeof(p_clear_tex)};
        ops.clear=(p_clear_tex){.tex=&t,.has_view=1,.is_uint=cases[i].ui,.mask=cases[i].mask,
            .r={cases[i].x,cases[i].y,0,cases[i].w,cases[i].h,1}};
        ops.clear.view=(view_t){.set_levels=1,.base_level=cases[i].level,.set_layers=1,.min_layer=cases[i].layer};
        for(int ch=0;ch<4;ch++)if(cases[i].ui)ops.clear.c.u[ch]=201+ch;else ops.clear.c.f[ch]=(201.+ch)/255;
        ops.end=(op_hdr){OP_END,sizeof(op_hdr)};
        mtl_exec((const uint8_t *)&ops);mtl_sub_commit();mtl_drain();
        CHECK(!mtl_last_commit_error(),"production texture clear completes");
        int valid=1;
        for(int layer=0;layer<2;layer++)for(int lv=0;lv<2;lv++) {
            int size=8>>lv;id<MTLBuffer> out=[g_dev newBufferWithLength:256*size options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLBlitCommandEncoder> en=[cb blitCommandEncoder];
            [en copyFromTexture:base sourceSlice:layer sourceLevel:lv sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(size,size,1) toBuffer:out destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256*size];
            [en endEncoding];[cb commit];[cb waitUntilCompleted];CHECK(cb.status==MTLCommandBufferStatusCompleted,"readback completes");
            uint8_t *p=out.contents;
            for(int y=0;y<size;y++)for(int x=0;x<size;x++)for(int ch=0;ch<4;ch++) {
                int hit=layer==cases[i].layer&&lv==cases[i].level&&x>=cases[i].x&&x<cases[i].x+cases[i].w&&y>=cases[i].y&&y<cases[i].y+cases[i].h&&(cases[i].mask&(1<<ch));
                valid&=p[y*256+x*4+ch]==(hit?201+ch:17*(ch+1));
            }
        }
        CHECK(valid,cases[i].label);printf("%s: %s\n",cases[i].label,valid?"PASS":"FAIL");mtl_tex_release(&t);
    }
    printf("texture clears: %d checks, %d failed\n",checks,failed);return failed!=0;
} }
