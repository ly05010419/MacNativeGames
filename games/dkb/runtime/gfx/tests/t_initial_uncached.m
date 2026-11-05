// Initial ASTC image data exists before registration in a CPU_UNCACHED or CPU_CACHED pool, with no flush.
// Every mip must sample its literal color at first use. The storage stays the content: a later CPU write is
// re-uploaded, and a new texture object over rewritten storage samples the new data, not the earlier upload.
// An uncompressed 3D texture with no IMAGE / COMPRESSIBLE flag is taken from its storage the same way (Xenoblade Chronicles 3's 10x10x10
// particle steering fields, GitHub issue #1011); with IMAGE it stays a GPU texture.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(label,c) do {checks++;if(!(c)){failed++;fprintf(stderr,"FAIL: %s\n",label);}}while(0)
static void finish(void) { CHECK("submission completes",mtl_wait(mtl_sub_commit(),5000000000ull)); CHECK("GPU has no error",!mtl_last_commit_error()); }
static const uint16_t colors[4][4]={{0,65535,0,65535},{65535,0,0,65535},{0,0,65535,65535},{65535,65535,0,65535}};
static void fill(tex_t *t,void *mem,int first){   // level lv gets colors[(first+lv)%4]
    for(int lv=0;lv<3;lv++){
        unsigned w=12u>>lv,blocks=(w+5)/6;uint8_t linear[64];
        for(unsigned i=0;i<blocks*blocks;i++){static const uint8_t head[8]={0xfc,0xfd,0xff,0xff,0xff,0xff,0xff,0xff};memcpy(linear+i*16,head,8);memcpy(linear+i*16+8,colors[(first+lv)%4],8);}
        CHECK("fixture swizzle succeeds",!nvn_swizzle(&t->d,lv,0,linear,blocks*16,mem));
    }
}
static void expect(id<MTLTexture> image,int first,const char *label){
    NSError *error=nil;
    static id<MTLComputePipelineState> ps;static id<MTLCommandQueue> queue;
    if(!ps){id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;kernel void read_mips(texture2d<float> t[[texture(0)]],device float4 *o[[buffer(0)]],uint lv[[thread_position_in_grid]]){constexpr sampler s(coord::normalized,filter::nearest,mip_filter::nearest);o[lv]=t.sample(s,float2(.5),level(lv));}" options:nil error:&error];
        ps=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"read_mips"] error:&error];queue=[g_dev newCommandQueue];}
    CHECK("mip probe compiles",ps!=nil);if(!ps||!image)return;
    id<MTLBuffer> result=[g_dev newBufferWithLength:48 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> ce=[cb computeCommandEncoder];
    [ce setComputePipelineState:ps];[ce setTexture:image atIndex:0];[ce setBuffer:result offset:0 atIndex:0];[ce dispatchThreadgroups:MTLSizeMake(3,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[ce endEncoding];[cb commit];[cb waitUntilCompleted];
    CHECK("mip readback completes",cb.status==MTLCommandBufferStatusCompleted);
    float *out=result.contents;for(int lv=0;lv<3;lv++)for(int c=0;c<4;c++)CHECK(label,fabsf(out[lv*4+c]-(colors[(first+lv)%4][c]?1.f:0.f))<.001f);
}
static void run(int format, uint64_t serial, uint32_t pool_flags, int c0) {   // c0: the first fill's colour (distinct per run: a run's pool and storage may reuse an earlier run's addresses)
    void *mem=NULL; if(posix_memalign(&mem,getpagesize(),65536))abort(); memset(mem,0,65536);
    pool_t pool={.magic=MAGIC('P','O','O','L'),.mem=mem,.size=65536,.flags=pool_flags};
    mtl_pool_add(mem,65536);
    tex_t t={.magic=MAGIC('T','E','X',0),.serial=serial,.pool=&pool,.mem=mem};
    t.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D,.format=format,.width=12,.height=12,.depth=1,.levels=3,.samples=1};
    nvn_tex_layout(&t.d,&t.L);
    fill(&t,mem,c0);
    mtl_tex_init(&t);mtl_sub_begin();id<MTLTexture> image=tex_mtl(&t);
    CHECK("ASTC texture resolves",image!=nil);finish();
    expect(image,c0,"literal ASTC mip color at first use");
    // A later CPU write is the texture's content (a block-compressed texture is never rendered into, and GPU copies into it reach its storage too)
    dm_guest_write_prepare(mem,t.L.size);fill(&t,mem,c0+1);mtl_sub_begin();id<MTLTexture> again=tex_mtl(&t);finish();
    expect(again,c0+1,"a later CPU write is re-uploaded");
    // A new texture object with the same description over the rewritten storage (GitHub issue #568): its own upload, not the earlier object's texture
    dm_guest_write_prepare(mem,t.L.size);fill(&t,mem,c0+2);
    tex_t u=t;u.serial=serial+100;mtl_tex_init(&u);mtl_sub_begin();id<MTLTexture> other=tex_mtl(&u);finish();
    expect(other,c0+2,"a new object over rewritten storage samples the new data");
    mtl_tex_release(&u);mtl_tex_release(&t);
    mtl_pool_del(mem);mtl_drain();free(mem);
}
// Each slice z of a 4x4x4 RGBA8 3D texture holds the colour {z*40+1, z*40+2, z*40+3, 255}
static void run3d(uint64_t serial, uint32_t tex_flags, int want) {
    void *mem=NULL; if(posix_memalign(&mem,getpagesize(),65536))abort(); memset(mem,0,65536);
    pool_t pool={.magic=MAGIC('P','O','O','L'),.mem=mem,.size=65536,.flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED};
    mtl_pool_add(mem,65536);
    tex_t t={.magic=MAGIC('T','E','X',0),.serial=serial,.pool=&pool,.mem=mem};
    t.d=(nvn_texdesc){.target=NVN_TEXTURE_TARGET_3D,.format=NVN_FORMAT_RGBA8,.width=4,.height=4,.depth=4,.levels=1,.samples=1,.flags=tex_flags};
    nvn_tex_layout(&t.d,&t.L);
    uint8_t linear[4*4*4*4];for(int z=0;z<4;z++)for(int i=0;i<16;i++){uint8_t *p=linear+(z*16+i)*4;p[0]=z*40+1;p[1]=z*40+2;p[2]=z*40+3;p[3]=255;}
    CHECK("3D fixture swizzle succeeds",!nvn_swizzle(&t.d,0,0,linear,16,mem));
    CHECK(want?"an uncompressed 3D texture with no IMAGE flag is taken from its storage":"a 3D image keeps its GPU content",tex_cpu_content(&t)==want);
    mtl_tex_init(&t);mtl_sub_begin();id<MTLTexture> image=tex_mtl(&t);
    CHECK("3D texture resolves",image!=nil);finish();
    if(want&&image){
        NSError *error=nil;static id<MTLComputePipelineState> ps;static id<MTLCommandQueue> queue;
        if(!ps){id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;kernel void read_slices(texture3d<float,access::read> t[[texture(0)]],device float4 *o[[buffer(0)]],uint z[[thread_position_in_grid]]){o[z]=t.read(uint3(1,2,z));}" options:nil error:&error];
            ps=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"read_slices"] error:&error];queue=[g_dev newCommandQueue];}
        CHECK("3D probe compiles",ps!=nil);
        if(ps){id<MTLBuffer> result=[g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb=[queue commandBuffer];id<MTLComputeCommandEncoder> ce=[cb computeCommandEncoder];
            [ce setComputePipelineState:ps];[ce setTexture:image atIndex:0];[ce setBuffer:result offset:0 atIndex:0];[ce dispatchThreadgroups:MTLSizeMake(4,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];[ce endEncoding];[cb commit];[cb waitUntilCompleted];
            float *out=result.contents;int ok=cb.status==MTLCommandBufferStatusCompleted;
            for(int z=0;z<4;z++)for(int c=0;c<3;c++)ok&=fabsf(out[z*4+c]*255.f-(float)(z*40+1+c))<.5f;
            CHECK("every slice of the 3D texture holds the CPU's texels at first use",ok);}
    }
    mtl_tex_release(&t);mtl_pool_del(mem);mtl_drain();free(mem);
}
int main(void){@autoreleasepool{mtl_init();
    const uint32_t uncached=NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED,cached=NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
    run(NVN_FORMAT_RGBA_ASTC_6x6,9750,uncached,0);run(NVN_FORMAT_RGBA_ASTC_6x6_SRGB,9751,uncached,0);run(NVN_FORMAT_RGBA_ASTC_6x6,9752,cached,3);
    run3d(9760,0,1);run3d(9761,NVN_TEXTURE_FLAGS_IMAGE,0);
    printf("t_initial_uncached: %d checks, %d failed\n",checks,failed);return failed!=0;}}
