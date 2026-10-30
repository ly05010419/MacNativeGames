#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

int main(void) {
    int failed = 0;
    failed += tex_need_pfv(MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm) != 0;
    failed += tex_need_pfv(MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm_sRGB) != 0;
    failed += tex_need_pfv(MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB) != 0;
    failed += tex_need_pfv(MTLPixelFormatBC7_RGBAUnorm, MTLPixelFormatBC7_RGBAUnorm_sRGB) != 0;
    failed += tex_need_pfv(MTLPixelFormatRGBA8Unorm, MTLPixelFormatR32Uint) != 1;
    tex_t t = {.magic=MAGIC('T', 'E', 'X', 0), .serial = 70001};
    t.d.width = 4; t.d.height = 1; t.d.depth = 1; t.d.levels = 1; t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    mtl_init();
    MTLTextureDescriptor *d = tex_desc(&t, 0);
    failed += (d.usage & MTLTextureUsagePixelFormatView) != 0;
    id<MTLTexture> base = [g_dev newTextureWithDescriptor:d];
    uint8_t bytes[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    id<MTLBuffer> src = [g_dev newBufferWithBytes:bytes length:sizeof bytes options:MTLResourceStorageModeShared];
    mtl_sub_begin();
    res_add(src); res_add(base);   // Metal 4: both ends of a copy must be in the residency set
    id fill = blit();   // copies on the main command buffer go through the host wrapper (which carries its own barrier)
    [fill copyFromBuffer:src sourceOffset:0 sourceBytesPerRow:16 sourceBytesPerImage:16 sourceSize:MTLSizeMake(4, 1, 1)
               toTexture:base destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    enc_b_end(g_blit); g_blit = nil;
    serial_put(t.serial, base);
    id<MTLTexture> upgraded = tex_upgrade_pfv(&t, base, MTLPixelFormatR32Uint);
    failed += upgraded.pixelFormat != MTLPixelFormatRGBA8Unorm;   // the upgrade keeps the original format
    failed += !(upgraded.usage & MTLTextureUsagePixelFormatView);
    failed += serial_get(t.serial) != upgraded;   // the registration has been replaced
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTLTexture> view = [upgraded newTextureViewWithPixelFormat:MTLPixelFormatR32Uint];
    id<MTLBuffer> dst = [g_dev newBufferWithLength:sizeof bytes options:MTLResourceStorageModeShared];
    res_add(dst);
    id read = blit();
    [read copyFromTexture:view sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(4, 1, 1)
                 toBuffer:dst destinationOffset:0 destinationBytesPerRow:16 destinationBytesPerImage:16];
    enc_b_end(g_blit); g_blit = nil;
    mtl_sub_commit(); mtl_drain();
    failed += memcmp(dst.contents, bytes, sizeof bytes) != 0;
    printf("pfv predicate failed=%d\n", failed);
    return failed != 0;
}
