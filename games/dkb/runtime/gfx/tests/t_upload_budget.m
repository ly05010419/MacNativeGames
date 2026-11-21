// A streaming burst inside one still-open rendering submission must not retain all
// staging until its rendering fence. Exercise the real Metal upload/retirement path.
#include "../nvn/nvn_mtl.m"
uint64_t g_t0_ns;
int dm_guest_as_contains(uintptr_t address) { (void)address; return 0; }
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

static size_t live_bytes(NSHashTable *buffers) { @autoreleasepool {
    size_t n = 0;
    for (id<MTLBuffer> b in buffers.allObjects) n += b.length;
    return n;
} }
static void upload_one(id<MTLTexture> dst, NSHashTable *buffers, uint8_t value) {
    up_begin();
    size_t off;
    id<MTLBuffer> b = up_alloc(32u << 20, &off);
    @autoreleasepool { [buffers addObject:b]; }
    [g_up_ce fillBuffer:b range:NSMakeRange(off, 256) value:value];
    up_blit_add(b, off, 256, 256, MTLSizeMake(1, 1, 1), dst, 0, 0);
    [g_up_texs addObject:dst];
}
int main(void) { @autoreleasepool {
    mtl_init();
    NSHashTable *buffers = [NSHashTable weakObjectsHashTable];
    NSMutableArray *textures = [NSMutableArray new];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [g_dev newTextureWithDescriptor:d]; res_add(target);
    mtl_sub_begin();
    MTL4RenderPassDescriptor *pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
    id encoder = [g_cb renderCommandEncoderWithDescriptor:pass];
    size_t peak = 0;
    for (int i = 0; i < 64; i++) {
        id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
        [textures addObject:t]; res_add(t);
        upload_one(t, buffers, (uint8_t)(i + 1));
        peak = MAX(peak, live_bytes(buffers));
    }
    up_flush(); rec_publish(); up_wait_zero(&g_up_pending, WS_DIAG);
    size_t idle = live_bytes(buffers);
    CHECK(peak <= (512u << 20), "streaming burst staging stays within 512 MiB");
    // Demand reuse retains a primary and one bounded overflow block per slot.
    // Everything beyond those six blocks must retire before the rendering fence.
    CHECK(idle <= (up_reuse() ? (192u << 20) : (96u << 20)),
          "completed uploads retain only the bounded slot staging cache");
    m4_end(encoder);
    uint64_t fence = mtl_sub_commit();
    CHECK(mtl_wait(fence, 5000000000ull), "rendering submission completes");
    CHECK(!mtl_last_commit_error(), "no rendering GPU error");
    uint8_t clear[4] = {0};
    [target getBytes:clear bytesPerRow:4 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
    CHECK(clear[0] == 255 && clear[1] == 0 && clear[2] == 0 && clear[3] == 255,
          "the open rendering pass survives upload commits");
    for (int i = 0; i < 64; i++) {
        uint8_t pixel[4] = {0};
        [textures[i] getBytes:pixel bytesPerRow:4 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
        for (int c = 0; c < 4; c++) CHECK(pixel[c] == i + 1, "upload pixels survive batch splitting and staging retirement");
    }
    printf("upload budget: peak %zu MiB, completed %zu MiB, %d checks, %d failed\n", peak >> 20, idle >> 20, checks, fails);
    return fails != 0;
} }
