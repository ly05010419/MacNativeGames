// Stencil-only views of depth-stencil textures (PR #106): Metal allows an X32_Stencil8 view only of a texture with the format-view usage, and its API validation layer aborts the
// process at the first view of one made without it. Run under MTL_DEBUG_LAYER=1: for both NVN depth-stencil formats (Depth32Float_Stencil8 in Metal), a texture is filled with a
// per-pixel depth and stencil, read through a stencil-mode view, a texture built in stencil mode and the depth-stencil copy draw, and the copy's destination is read back
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
enum { W = 8, H = 8 };
static float depth_at(int x, int y) { return (float)(x + y * W) / (W * H); }
static uint8_t stencil_at(int x, int y) { return (uint8_t)(x * 7 + y * 13 + 1); }
// Each plane of a Depth32Float_Stencil8 texture goes through a buffer with the plane's blit option (classic queue, waited on)
static void plane_io(id<MTLTexture> t, int stencil, int to_tex, void *bytes) {
    NSUInteger bpp = stencil ? 1 : 4;
    id<MTLBuffer> b = [g_dev newBufferWithLength:W * H * bpp options:MTLResourceStorageModeShared];
    if (to_tex) memcpy(b.contents, bytes, W * H * bpp);
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    MTLBlitOption o = stencil ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil;
    if (to_tex) [e copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:W * bpp sourceBytesPerImage:W * H * bpp sourceSize:MTLSizeMake(W, H, 1) toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0) options:o];
    else [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:W * bpp destinationBytesPerImage:W * H * bpp options:o];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (!to_tex) memcpy(bytes, b.contents, W * H * bpp);
}
int main(void) { @autoreleasepool {
    mtl_init();
    const int formats[] = {NVN_FORMAT_DEPTH24_STENCIL8, NVN_FORMAT_DEPTH32F_STENCIL8};
    for (int i = 0; i < 2; i++) {
        tex_t s = {.magic=MAGIC('T','E','X',0), .serial=99501u + 4 * i};
        s.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=formats[i], .width=W, .height=H, .depth=1, .levels=1, .samples=1};
        nvn_tex_layout(&s.d, &s.L);
        tex_t sb = s, d = s; sb.serial++; sb.stencil = 1; d.serial += 2;   // sb: the same storage built in stencil mode; d: the copy's destination
        mtl_tex_init(&s); mtl_tex_init(&sb); mtl_tex_init(&d);
        id<MTLTexture> base = tex_mtl(&s), dst = tex_mtl(&d);
        CHECK(base && base.pixelFormat == MTLPixelFormatDepth32Float_Stencil8, "the depth-stencil texture is Depth32Float_Stencil8");
        CHECK(base.usage & MTLTextureUsagePixelFormatView, "a depth-stencil texture is made with the format-view usage");
        if (!base || !dst) continue;
        float dz[W * H]; uint8_t st[W * H];
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { dz[x + y * W] = depth_at(x, y); st[x + y * W] = stencil_at(x, y); }
        plane_io(base, 0, 1, dz); plane_io(base, 1, 1, st);

        // A stencil-mode view, and the texture built in stencil mode, are X32_Stencil8 views of the depth-stencil storage itself (no upgraded copy)
        view_t v = {.set_dsmode=1, .dsmode=NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL};
        mtl_reg_tex(800 + 2 * i, &s, &v); mtl_reg_tex(801 + 2 * i, &sb, NULL);
        pthread_mutex_lock(&g_rmu); id<MTLTexture> sv = tex_by_id(800 + 2 * i), bv = tex_by_id(801 + 2 * i); pthread_mutex_unlock(&g_rmu);
        CHECK(sv.pixelFormat == MTLPixelFormatX32_Stencil8 && sv.parentTexture == base, "a stencil-mode view reads the stencil of the texture's own storage");
        CHECK(bv.pixelFormat == MTLPixelFormatX32_Stencil8 && bv.parentTexture == base, "a texture built in stencil mode reads the stencil of the same storage");
        CHECK(tex_get(s.serial) == base, "the depth-stencil texture is not replaced");

        // The depth-stencil copy draw reads the source's stencil through its own X32_Stencil8 view and writes it with the depth
        mtl_sub_begin();
        NVNcopyRegion r = {.width=W, .height=H, .depth=1};
        CHECK(copy_t2t_draw(base, dst, 0, 0, 0, 0, &r, &r, 0, 0), "the depth-stencil copy is drawn");
        mtl_sub_commit(); mtl_drain();
        float dz2[W * H]; uint8_t st2[W * H];
        plane_io(dst, 0, 0, dz2); plane_io(dst, 1, 0, st2);
        CHECK(!memcmp(dz, dz2, sizeof dz), "the copy carries every depth value");
        CHECK(!memcmp(st, st2, sizeof st), "the copy carries every stencil value, read through the stencil-only view");
    }
    printf("stencil views: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
