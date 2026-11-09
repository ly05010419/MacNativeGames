// NVN linear image storage: GPU image writes must be visible through the memory-pool CPU mapping.
// Synthetic 160x90 RGBA8_SRGB IMAGE|LINEAR textures use a valid 640-byte row pitch.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int cases, failed;
#define EXPECT(n,w,v) do { int a=(w), b=(v); cases++; printf("%s expected %d -> got %d %s\n",n,a,b,a==b?"PASS":"FAIL"); failed+=a!=b; } while (0)
static void run_case(int format) {
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 262144)) abort(); memset(mem, 0, 262144);
    pool_t pool = {.mem=mem, .size=262144, .flags=0x24}; mtl_pool_add(mem, 262144);
    NVNtextureBuilder tb; NVNtexture tex; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 160);
    h_nvnTextureBuilderSetHeight(&tb, 90); h_nvnTextureBuilderSetFormat(&tb, format);
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_LINEAR); h_nvnTextureBuilderSetStride(&tb, 640);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 65536); EXPECT("linear texture init", 1, h_nvnTextureInitialize(&tex, &tb));
    id<MTLTexture> t = tex_mtl((const tex_t *)&tex);
    printf("LINEAR format=%#x Metal=%lu minimumAlignment=%lu bufferBacked=%d row=640\n", format, (unsigned long)t.pixelFormat,
        (unsigned long)[g_dev minimumLinearTextureAlignmentForPixelFormat:t.pixelFormat], t.buffer != nil);
    EXPECT("host texture resolved", 1, t != nil); if (!t) return;
    // NVN image stores use the shader image format, without sRGB conversion. A uint image view writes literal packed bytes.
    mtl_reg_tex(1, (const tex_t *)&tex, NULL); uint64_t rid;
    mtl_sub_begin(); id<MTLTexture> image = img_fit(1, t, NVN_DPF_UINT, &rid);
    if (!image) { img_fit_upgrade(); t = tex_mtl((const tex_t *)&tex); image = img_fit(1, t, NVN_DPF_UINT, &rid); }
    mtl_wait(mtl_sub_commit(), 5000000000ull); EXPECT("production uint image view", 1, image != nil);
    NSError *err = nil; const char *src = "#include <metal_stdlib>\nusing namespace metal;\nkernel void fill(texture2d<uint,access::write> out [[texture(0)]], uint2 p [[thread_position_in_grid]]) { if(p.x<160&&p.y<90) out.write(uint4(0xff996633u,0,0,0),p); }\n";
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(src) options:nil error:&err]; if (!lib) { fprintf(stderr,"%s\n",err.description.UTF8String); abort(); }
    id<MTLComputePipelineState> ps = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill"] error:&err]; if (!ps) abort();
    id<MTLCommandBuffer> cb = q_cb(); id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder]; [en setComputePipelineState:ps]; [en setTexture:image atIndex:0];
    [en dispatchThreads:MTLSizeMake(160,90,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    EXPECT("image compute complete", MTLCommandBufferStatusCompleted, cb.status);
    h_nvnMemoryPoolInvalidateMappedRange((const NVNmemoryPool *)&pool, 65536, 640*90);
    uint8_t *map = (uint8_t *)h_nvnMemoryPoolMap((const NVNmemoryPool *)&pool) + 65536; int bad = 0;
    static const uint8_t want[] = {51,102,153,255}; for(int i=0;i<160*90;i++) bad += memcmp(map+i*4,want,4) != 0;
    EXPECT("GPU image writes -> CPU pool map, 14400 literal pixels", 0, bad);
    h_nvnTextureFinalize(&tex); mtl_pool_del(mem); free(mem);
}
static void scaled_copy_case(void) {
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 262144)) abort(); memset(mem, 0, 262144);
    pool_t pool = {.mem=mem, .size=262144, .flags=0x24}; mtl_pool_add(mem, 262144);
    NVNtextureBuilder tb; NVNtexture dst; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 160);
    h_nvnTextureBuilderSetHeight(&tb, 90); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8_SRGB);
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_IMAGE | NVN_TEXTURE_FLAGS_LINEAR); h_nvnTextureBuilderSetStride(&tb, 640);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 65536); EXPECT("scaled copy destination init", 1, h_nvnTextureInitialize(&dst, &tb));
    tex_t src = {.magic=MAGIC('T','E','X',0), .serial=900001}; src.d.width=320; src.d.height=180; src.d.depth=src.d.levels=src.d.samples=1;
    src.d.target=NVN_TEXTURE_TARGET_2D; src.d.format=NVN_FORMAT_RGBA8_SRGB;
    id<MTLTexture> source = tex_mtl(&src); id<MTLCommandBuffer> seed = q_cb();
    MTLRenderPassDescriptor *pd = [MTLRenderPassDescriptor renderPassDescriptor]; pd.colorAttachments[0].texture=source;
    pd.colorAttachments[0].loadAction=MTLLoadActionClear; pd.colorAttachments[0].storeAction=MTLStoreActionStore; pd.colorAttachments[0].clearColor=MTLClearColorMake(1,0,0,1);
    [[seed renderCommandEncoderWithDescriptor:pd] endEncoding]; [seed commit]; [seed waitUntilCompleted];
    EXPECT("scaled copy source seeded", MTLCommandBufferStatusCompleted, seed.status);
    NVNcommandBuffer cmd; EXPECT("scaled copy command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536); h_nvnCommandBufferBeginRecording(&cmd);
    NVNcopyRegion sr={0,0,0,320,180,1}, dr={0,0,0,160,90,1};
    h_nvnCommandBufferCopyTextureToTexture(&cmd, (const NVNtexture *)&src, NULL, &sr, &dst, NULL, &dr, NVN_COPY_FLAGS_LINEAR_FILTER_BIT);
    NVNcommandHandle h=h_nvnCommandBufferEndRecording(&cmd); state_reset(); memset(&R,0,sizeof R); mtl_sub_begin(); mtl_exec((const uint8_t *)h);
    EXPECT("scaled copy committed", 1, mtl_wait(mtl_sub_commit(),5000000000ull)); EXPECT("scaled copy GPU error",0,mtl_last_commit_error());
    h_nvnMemoryPoolInvalidateMappedRange((const NVNmemoryPool *)&pool,65536,640*90); uint8_t *map=(uint8_t *)h_nvnMemoryPoolMap((const NVNmemoryPool *)&pool)+65536;
    static const uint8_t want[]={255,0,0,255}; int bad=0; for(int i=0;i<160*90;i++)bad+=memcmp(map+i*4,want,4)!=0;
    EXPECT("scaled render copy -> CPU pool map, 14400 literal red pixels",0,bad);
    EXPECT("scaled destination keeps pool backing",1,tex_mtl((const tex_t *)&dst).buffer!=nil);
    h_nvnTextureFinalize(&dst); mtl_tex_release(&src); mtl_pool_del(mem); free(mem);
}
int main(void) { @autoreleasepool { mtl_init(); run_case(NVN_FORMAT_RGBA8); run_case(NVN_FORMAT_RGBA8_SRGB);
    scaled_copy_case();
    printf("LINEAR_READBACK cases=%d failed=%d\n",cases,failed); return failed != 0;
} }
