// Initial ASTC image data exists before registration in a CPU_UNCACHED pool.
// Every mip must sample its literal color without a flush or a later CPU write.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label,c) do {checks++;if(!(c)){failed++;fprintf(stderr,"FAIL: %s\n",label);}}while(0)
static void finish(void) { CHECK("submission completes",mtl_wait(mtl_sub_commit(),5000000000ull)); CHECK("GPU has no error",!mtl_last_commit_error()); }
static void run(int format, uint64_t serial) {
    void *mem=NULL; if(posix_memalign(&mem,getpagesize(),65536))abort(); memset(mem,0,65536);
    pool_t pool={.mem=mem,.size=65536,.flags=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(mem,65536);
    tex_t t={.magic=MAGIC('T','E','X',0),.serial=serial,.pool=&pool,.mem=mem};
    t.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=format,.width=12,.height=12,.depth=1,.levels=3,.samples=1};
    nvn_tex_layout(&t.d,&t.L);
    const uint16_t colors[3][4]={{0,65535,0,65535},{65535,0,0,65535},{0,0,65535,65535}};
    for(int lv=0;lv<3;lv++){
        unsigned w=12u>>lv,blocks=(w+5)/6;uint8_t linear[64];
        for(unsigned i=0;i<blocks*blocks;i++){static const uint8_t head[8]={0xfc,0xfd,0xff,0xff,0xff,0xff,0xff,0xff};memcpy(linear+i*16,head,8);memcpy(linear+i*16+8,colors[lv],8);}
        CHECK("fixture swizzle succeeds",!nvn_swizzle(&t.d,lv,0,linear,blocks*16,mem));
    }
    mtl_sub_begin();pool.flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    id<MTLTexture> control=[g_dev newTextureWithDescriptor:tex_desc(&t,0)];res_add(control);
    CHECK("CPU_CACHED storage still requires a flush",!tex_init_fill(&t,control));finish();
    pool.flags=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    mtl_tex_init(&t);mtl_sub_begin();id<MTLTexture> image=tex_mtl(&t);
    CHECK("ASTC texture resolves",image!=nil);finish();
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;kernel void read_mips(texture2d<float> t[[texture(0)]],device float4 *o[[buffer(0)]],uint lv[[thread_position_in_grid]]){constexpr sampler s(coord::normalized,filter::nearest,mip_filter::nearest);o[lv]=t.sample(s,float2(.5),level(lv));}" options:nil error:&error];
    id<MTLComputePipelineState> ps=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"read_mips"] error:&error];CHECK("mip probe compiles",ps!=nil);
    id<MTLBuffer> result=[g_dev newBufferWithLength:48 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue=[g_dev newCommandQueue];id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> ce=[cb computeCommandEncoder];
    [ce setComputePipelineState:ps];[ce setTexture:image atIndex:0];[ce setBuffer:result offset:0 atIndex:0];[ce dispatchThreadgroups:MTLSizeMake(3,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[ce endEncoding];[cb commit];[cb waitUntilCompleted];
    CHECK("mip readback completes",cb.status==MTLCommandBufferStatusCompleted);
    float *out=result.contents;for(int lv=0;lv<3;lv++)for(int c=0;c<4;c++)CHECK("literal ASTC mip color",fabsf(out[lv*4+c]-(colors[lv][c]?1.f:0.f))<.001f);
    // A later resolution must reuse the existing texture, rather than upload
    // the now-modified CPU bytes over content that may have been GPU-written.
    dm_guest_write_prepare(mem,t.L.size);memset(mem,0,t.L.size);mtl_sub_begin();CHECK("initial fill happens only once",tex_mtl(&t)==image);finish();
    mtl_pool_del(mem);mtl_drain();free(mem);
}
int main(void){@autoreleasepool{mtl_init();run(NVN_FORMAT_RGBA_ASTC_6x6,9750);run(NVN_FORMAT_RGBA_ASTC_6x6_SRGB,9751);printf("t_initial_uncached: %d checks, %d failed\n",checks,failed);return failed!=0;}}
