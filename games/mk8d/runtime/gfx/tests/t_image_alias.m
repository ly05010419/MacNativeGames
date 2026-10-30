// Live dynamic-resolution descriptors must not multiply their NVN storage.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

static tex_t image(pool_t *pool, uint64_t serial, unsigned w, unsigned h) {
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial, .pool=pool, .mem=pool->mem};
    t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE|NVN_TEXTURE_FLAGS_IMAGE,
                      .width=w, .height=h, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&t.d, &t.L); return t;
}
static id<MTLBuffer> read_image(id<MTLTexture> t) {
    NSUInteger row=t.width*4, bytes=row*t.height;
    id<MTLBuffer> b=[g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared]; res_add(b);
    enc_b_copy_texture_buffer(blit(), t, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(t.width,t.height,1), b, 0, row, bytes);
    mtl_sub_commit(); mtl_drain(); CHECK(!mtl_last_commit_error(), "image read completes without GPU error");
    res_remove(b); mtl_sub_begin(); return b;
}
static void pixels(id<MTLTexture> t, uint32_t seed) {
    NSUInteger bytes=t.width*t.height*4;
    id<MTLBuffer> b=[g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    uint32_t *p=b.contents; for (NSUInteger i=0;i<bytes/4;i++) p[i]=seed^(uint32_t)i;
    res_add(b); enc_b_copy_buffer_texture(blit(), b, 0, t.width*4, bytes, MTLSizeMake(t.width,t.height,1), t, 0, 0, MTLOriginMake(0,0,0));
    id<MTLBuffer> out=read_image(t); CHECK(!memcmp(out.contents,b.contents,bytes), "every literal pixel survives alternating resolutions"); res_remove(b);
}
static void is_zero(id<MTLTexture> t) {
    id<MTLBuffer> b=read_image(t); uint32_t *p=b.contents; int zero=1;
    for (NSUInteger i=0;i<b.length/4;i++) zero &= p[i]==0;
    CHECK(zero, "fresh family is zero, including beyond its first-used small descriptor");
}
static void indirect_pixels(id<MTLTexture> t, uint32_t seed) {
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Args { texture2d<float,access::read> tex [[id(0)]]; };\n"
        "kernel void read_image(constant Args &a [[buffer(0)]], device uint *o [[buffer(1)]], uint2 p [[thread_position_in_grid]]) { if(p.x>=a.tex.get_width()||p.y>=a.tex.get_height())return; uint4 v=uint4(round(a.tex.read(p)*255.0f)); o[p.y*a.tex.get_width()+p.x]=v.x|(v.y<<8)|(v.z<<16)|(v.w<<24); }\n" options:nil error:&error];
    id<MTLFunction> fn=[lib newFunctionWithName:@"read_image"];
    id<MTLComputePipelineState> ps=fn?[g_dev newComputePipelineStateWithFunction:fn error:&error]:nil;
    CHECK(ps!=nil,"indirect texture reader compiles"); if (!ps) return;
    id<MTLArgumentEncoder> ae=[fn newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> args=[g_dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
    id<MTLBuffer> out=[g_dev newBufferWithLength:t.width*t.height*4 options:MTLResourceStorageModeShared];
    [ae setArgumentBuffer:args offset:0]; [ae setTexture:t atIndex:0]; res_add(args); res_add(out);
    MTL4ArgumentTableDescriptor *ad=[MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount=2;
    id<MTL4ArgumentTable> at=rec_table([g_dev newArgumentTableWithDescriptor:ad error:&error]);
    [at setAddress:args.gpuAddress atIndex:0]; [at setAddress:out.gpuAddress atIndex:1];
    id<MTL4ComputeCommandEncoder> e=enc_c_begin(nil); [e setArgumentTable:at]; enc_c_pso(e,ps);
    enc_c_dispatch(e,MTLSizeMake((t.width+7)/8,(t.height+7)/8,1),MTLSizeMake(8,8,1)); enc_c_end(e);
    mtl_sub_commit(); mtl_drain();
    CHECK(!mtl_last_commit_error(),"indirect Metal 4 read completes with only the heap resident");
    uint32_t *p=out.contents; int valid=1;
    for (NSUInteger i=0;i<out.length/4;i++) valid &= p[i]==(seed^(uint32_t)i);
    CHECK(valid,"indirect access reads every literal pixel");
    res_remove(args); res_remove(out); mtl_sub_begin();
}
static void atomic_pixel(id<MTLTexture> t, uint32_t seed) {
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void bump(texture2d<uint,access::read_write> image [[texture(0)]]) { image.atomic_fetch_add(uint2(0,0),5u); }\n" options:nil error:&error];
    id<MTLComputePipelineState> ps=lib?[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"bump"] error:&error]:nil;
    CHECK(ps!=nil,"native texture-atomic reader compiles"); if (!ps) return;
    MTL4ArgumentTableDescriptor *ad=[MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount=1;
    id<MTL4ArgumentTable> at=rec_table([g_dev newArgumentTableWithDescriptor:ad error:&error]);
    [at setTexture:t.gpuResourceID atIndex:0];
    id<MTL4ComputeCommandEncoder> e=enc_c_begin(nil); [e setArgumentTable:at]; enc_c_pso(e,ps);
    enc_c_dispatch(e,MTLSizeMake(1,1,1),MTLSizeMake(1,1,1)); enc_c_end(e);
    id<MTLBuffer> out=read_image(t); uint32_t *p=out.contents; int valid=p[0]==seed+5;
    for (NSUInteger i=1;i<out.length/4;i++) valid &= p[i]==(seed^(uint32_t)i);
    CHECK(valid,"native atomic modifies the heap-backed integer image and preserves every other pixel");
}
static void depth_planes(pool_t *pool) {
    tex_t d[2];
    for (int i=0;i<2;i++) { d[i]=image(pool,992000+i,32u<<i,32u<<i); d[i].off=12<<20; d[i].mem=pool->mem+d[i].off;
        d[i].d.format=NVN_FORMAT_DEPTH32F_STENCIL8; nvn_tex_layout(&d[i].d,&d[i].L); mtl_tex_init(&d[i]); }
    NSError *error=nil;
    id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void planes(texture2d<uint,access::read> s [[texture(0)]], depth2d<float,access::read> d [[texture(1)]], device uint2 *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { o[p.y*d.get_width()+p.x]=uint2(s.read(p).r,as_type<uint>(d.read(p))); }\n" options:nil error:&error];
    id<MTLComputePipelineState> ps=[g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"planes"] error:&error];
    CHECK(ps!=nil,"depth/stencil reader compiles"); if (!ps) return;
    for (int i=0;i<3;i++) {
        tex_t *t=&d[i%2]; id<MTLTexture> base=tex_mtl(t);
        CHECK(base.heap!=nil,"depth/stencil resolutions share physical storage");
        state_reset(); memset(&R,0,sizeof R); R.has_rt=1; R.rt.depth=t;
        struct { op_hdr h; p_clear_ds clear; op_hdr end; } ops={0};
        ops.h=(op_hdr){OP_CLEAR_DS,sizeof(op_hdr)+sizeof(p_clear_ds)};
        ops.clear=(p_clear_ds){.depth=.25,.depth_mask=1,.stencil=0x30+i,.stencil_mask=255}; ops.end=(op_hdr){OP_END,sizeof(op_hdr)};
        mtl_exec((const uint8_t *)&ops); mtl_sub_commit(); mtl_drain();
        CHECK(!mtl_last_commit_error(),"production depth/stencil clear completes");
        id<MTLTexture> stencil=[base newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
        id<MTLBuffer> out=[g_dev newBufferWithLength:base.width*base.height*8 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb=q_cb(); id<MTLComputeCommandEncoder> e=[cb computeCommandEncoder];
        [e setComputePipelineState:ps]; [e setTexture:stencil atIndex:0]; [e setTexture:base atIndex:1]; [e setBuffer:out offset:0 atIndex:0];
        [e dispatchThreads:MTLSizeMake(base.width,base.height,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        uint32_t *p=out.contents; int valid=cb.status==MTLCommandBufferStatusCompleted;
        for (NSUInteger j=0;j<out.length/8;j++) valid &= p[j*2]==0x30u+i && p[j*2+1]==0x3e800000;
        CHECK(valid,"both planes survive resolution changes and stencil views"); mtl_sub_begin();
    }
    for (int i=0;i<2;i++) mtl_tex_release(&d[i]);
    memset(&R,0,sizeof R);
}
static void separate_storage(pool_t *pool, id<MTLHeap> original) {
    pool_t other_pool=*pool; other_pool.mem+=14<<20; other_pool.size=2<<20;
    pool_t cpu_pool=*pool; cpu_pool.flags=NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED|NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE;
    tex_t t[8];
    for (int i=0;i<8;i++) {
        t[i]=image(i<2?&other_pool:i<4?&cpu_pool:pool,994000+i,(i%2+1)*64,64);
        if (i>=4) { t[i].off=8<<20; t[i].mem=pool->mem+t[i].off; }
        if (i>=6) t[i].d.format=NVN_FORMAT_R32UI;
        nvn_tex_layout(&t[i].d,&t[i].L); mtl_tex_init(&t[i]);
    }
    id<MTLTexture> p=tex_mtl(&t[0]), o=tex_mtl(&t[4]), f=tex_mtl(&t[6]);
    CHECK(p.heap && p.heap!=original,"another physical pool gets a separate heap");
    CHECK(o.heap && o.heap!=original && o.heap!=p.heap,"another offset gets a separate heap");
    CHECK(f.heap && f.heap!=o.heap && f.heap!=original,"another format cannot join an existing heap");
    CHECK(f.heap==tex_mtl(&t[7]).heap,"integer images reuse backing when Metal hides atomic usage");
    CHECK(tex_atomic_capable(f) && tex_atomic_capable(tex_mtl(&t[7])),"integer aliases retain shader-atomic capability");
    CHECK(!tex_mtl(&t[2]).heap && !tex_mtl(&t[3]).heap,"CPU-visible resolution families retain their original backing");
    pixels(p,0x18273645); pixels(o,0x29384756); pixels(f,0x3a495867);
    atomic_pixel(f,0x3a495867);
    for (int i=0;i<8;i++) mtl_tex_release(&t[i]);
    mtl_alias_drop_pool(&other_pool); mtl_alias_drop_pool(&cpu_pool);
}
int main(void) { @autoreleasepool {
    mtl_init(); __weak id<MTLHeap> weak_heap; void *mem=NULL;
    posix_memalign(&mem,16384,16<<20); memset(mem,0,16<<20);
    @autoreleasepool {
    mtl_sub_begin();
    pool_t pool={.magic=MAGIC('P','O','O','L'), .flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED|NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE, .mem=mem, .size=16<<20};
    enum { N=128 }; tex_t a[N];
    for (int i=0;i<N;i++) { a[i]=image(&pool, 990000+i, 512+i*4, 512); mtl_tex_init(&a[i]); }
    uint64_t before=g_dev.currentAllocatedSize;
    id<MTLTexture> small=tex_mtl(&a[0]);
    CHECK(small.width==512 && small.height==512,"the first descriptor retains its NVN dimensions");
    CHECK(small.heap!=nil,"live resolution family has physical backing");
    CHECK(!(small.usage&MTLTextureUsagePixelFormatView),"colour compression is not disabled by forced format-view usage");
    MTLSizeAndAlign max_size=[g_dev heapTextureSizeAndAlignWithDescriptor:tex_desc(&a[N-1],0)];
    CHECK(small.heap.size==max_size.size,"heap capacity is the exact largest declared size");
    is_zero(small);
    id<MTLTexture> big=tex_mtl(&a[N-1]); is_zero(big);
    pixels(small,0x10203040);
    for (int i=1;i<N;i++) {
        id<MTLTexture> t=tex_mtl(&a[i]);
        CHECK(t.heap==small.heap,"all live descriptors share one allocation");
        CHECK(t!=small && t.width==512u+i*4 && t.height==512,"each descriptor remains a distinct texture with its own dimensions");
    }
    // Descriptor creation must not enqueue an upload-batch zero over an earlier writer.
    id<MTLBuffer> saved=read_image(small); uint32_t *p=saved.contents; int intact=1;
    for (NSUInteger i=0;i<saved.length/4;i++) intact &= p[i]==(0x10203040u^(uint32_t)i);
    CHECK(intact,"creating aliases preserves an earlier writer");
    uint64_t delta=g_dev.currentAllocatedSize-before;
    printf("128 live image descriptors: device allocation delta %llu bytes, heap %lu bytes\n",(unsigned long long)delta,(unsigned long)small.heap.size);
    CHECK(delta<max_size.size+(12<<20),"allocation is bounded by one image plus upload/readback scratch");
    pixels(big,0x55667788); pixels(tex_mtl(&a[63]),0xa1b2c3d4); pixels(small,0x76543210);
    separate_storage(&pool,small.heap);
    indirect_pixels(small,0x76543210);   // writes to other pools, offsets and formats left it intact
    depth_planes(&pool);
    CHECK([g_res_objects containsObject:small.heap],"the parent heap is resident for indirect access");
    CHECK(![g_res_objects containsObject:small],"aliases do not each register a separate allocation");
    // Different offsets, formats, CPU-visible and sparse flags cannot join this family.
    tex_t other=image(&pool,991000,64,64); other.off=10<<20; other.mem=pool.mem+other.off; mtl_tex_init(&other);
    CHECK(!tex_mtl(&other).heap,"single-description storage stays an ordinary texture");
    tex_t sparse=image(&pool,991001,128,64); sparse.d.flags|=NVN_TEXTURE_FLAGS_SPARSE; mtl_tex_init(&sparse);
    CHECK(!tex_mtl(&sparse).heap,"sparse images stay outside physical image sharing");
    weak_heap=small.heap;
    for (int i=0;i<N;i++) mtl_tex_release(&a[i]);
    mtl_tex_release(&other); mtl_tex_release(&sparse); mtl_alias_drop_pool(&pool);
    small=nil; big=nil; saved=nil;
    mtl_sub_commit(); mtl_drain();
    // Finish a second submission: release calls are deferred to the next fence.
    mtl_sub_begin(); mtl_sub_commit(); mtl_drain();
    // GPU completion publishes the fence before its feedback handler drains deferred objects.
    dispatch_sync(g_feedback_q, ^{}); defer_drain();
    CHECK(g_image_desc.n==0,"finalization releases every recorded description");
    }
    CHECK(!weak_heap || ![g_res_objects containsObject:weak_heap],"the last texture retires its heap from residency");
    CHECK(g_image_heap_n==0,"every family heap is released after its textures and views go");
    uint64_t settled=g_dev.currentAllocatedSize;
    for (int i=0;i<8;i++) { @autoreleasepool {
        pool_t pool={.flags=NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS|NVN_MEMORY_POOL_FLAGS_GPU_CACHED|NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE,.mem=mem,.size=16<<20};
        tex_t a=image(&pool,993000+i*2,512,512), b=image(&pool,993001+i*2,1024,512);
        mtl_tex_init(&a); mtl_tex_init(&b); mtl_sub_begin();
        CHECK(tex_mtl(&a).heap!=nil,"a recreated family allocates its backing");
        mtl_tex_release(&a); mtl_tex_release(&b); mtl_alias_drop_pool(&pool);
        mtl_sub_commit(); mtl_drain(); mtl_sub_begin(); mtl_sub_commit(); mtl_drain();
        dispatch_sync(g_feedback_q,^{}); defer_drain();
    } }
    CHECK(g_image_heap_n==0,"repeated destruction leaves no heap owner");
    CHECK(g_dev.currentAllocatedSize<=settled+(3<<20),"repeated families do not retain their old physical allocations");
    free(mem);
    printf("image aliases: %d checks, %d failed\n",checks,failed); return failed!=0;
} }
