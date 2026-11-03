// Integer color attachments ignore blending; the same descriptor must remain valid on Metal.
#include "../nvn/nvn_mtl.m"

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

int main(void) { @autoreleasepool {
    mtl_init();
    NvShader *vs = [NvShader new], *fs = [NvShader new];
    fs->rf.color_known = 1; fs->rf.color_mask = 3;
    pso_key k = {0}; k.samples = 1; k.chmask = 0xff; k.blend_en = 3;
    k.blend[0].src_rgb = k.blend[0].src_a = 1;
    k.blend[1] = k.blend[0];
    MTLPixelFormat integers[] = {MTLPixelFormatR8Uint, MTLPixelFormatR8Sint, MTLPixelFormatR16Uint, MTLPixelFormatR16Sint,
        MTLPixelFormatR32Uint, MTLPixelFormatR32Sint, MTLPixelFormatRG8Uint, MTLPixelFormatRG8Sint,
        MTLPixelFormatRG16Uint, MTLPixelFormatRG16Sint, MTLPixelFormatRG32Uint, MTLPixelFormatRG32Sint,
        MTLPixelFormatRGBA8Uint, MTLPixelFormatRGBA8Sint, MTLPixelFormatRGB10A2Uint,
        MTLPixelFormatRGBA16Uint, MTLPixelFormatRGBA16Sint, MTLPixelFormatRGBA32Uint, MTLPixelFormatRGBA32Sint};
    for (unsigned i = 0; i < sizeof integers / sizeof *integers; i++) {
        k.cfmt[0] = integers[i]; k.cfmt[1] = MTLPixelFormatRGBA8Unorm;
        MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, nil, 0);
        CHECK(!d.colorAttachments[0].blendingEnabled, "integer attachment bypasses blending");
        CHECK(d.colorAttachments[0].writeMask == MTLColorWriteMaskAll, "integer output remains writable");
        CHECK(d.colorAttachments[1].blendingEnabled, "normalized attachment retains requested blending");
    }
    k.cfmt[0] = MTLPixelFormatR32Uint; k.cfmt[1] = MTLPixelFormatRGBA8Uint;
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, nil, 0);
    CHECK(!d.colorAttachments[0].blendingEnabled && !d.colorAttachments[1].blendingEnabled,
          "two integer attachments do not create an invalid pipeline");
    if (!failed) {
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 v(uint i [[vertex_id]]) { return float4(i == 1 ? 3.0 : -1.0, i == 2 ? 3.0 : -1.0, 0, 1); }\n"
            "struct O { uint4 a [[color(0)]]; uint4 b [[color(1)]]; };\n"
            "fragment O f() { return {uint4(0x12345678u), uint4(17,34,51,68)}; }\n";
        NSError *error = nil;
        id<MTLLibrary> library = [g_dev newLibraryWithSource:source options:nil error:&error];
        CHECK(library != nil, "synthetic shader compiles");
        vs->fn = [library newFunctionWithName:@"v"]; fs->fn = [library newFunctionWithName:@"f"];
        d = pso_desc(&k, vs, fs, nil, 0);
        id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
        CHECK(pipeline != nil, "real Metal accepts the corrected descriptor");
        if (pipeline) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:k.cfmt[0] width:4 height:4 mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
            id<MTLTexture> a = [g_dev newTextureWithDescriptor:td]; td.pixelFormat = k.cfmt[1];
            id<MTLTexture> b = [g_dev newTextureWithDescriptor:td];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            for (int i = 0; i < 2; i++) { rp.colorAttachments[i].texture = i ? b : a;
                rp.colorAttachments[i].loadAction = MTLLoadActionClear; rp.colorAttachments[i].storeAction = MTLStoreActionStore; }
            id<MTLCommandQueue> queue = [g_dev newCommandQueue]; id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:rp];
            [encoder setRenderPipelineState:pipeline]; [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding]; [cb commit]; [cb waitUntilCompleted];
            CHECK(cb.status == MTLCommandBufferStatusCompleted, "integer render completes");
            uint32_t pixels[16]; [a getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0];
            int ok = 1; for (int i = 0; i < 16; i++) ok &= pixels[i] == 0x12345678u;
            CHECK(ok, "R32Uint output preserves all literal bits");
            [b getBytes:pixels bytesPerRow:16 fromRegion:MTLRegionMake2D(0,0,4,4) mipmapLevel:0];
            ok = 1; for (int i = 0; i < 16; i++) ok &= pixels[i] == 0x44332211u;
            CHECK(ok, "RGBA8Uint output preserves four literal channels");
        }
    }
    printf("integer blending: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
