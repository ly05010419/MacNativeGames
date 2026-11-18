// RegisterImage on a COMPRESSIBLE-only render texture: first use adds image usage, keeps the open pass's clear and every mip, and updates sampled aliases.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static void read_level(id<MTLTexture> texture, unsigned level, uint16_t *out) {
    NSUInteger n = texture.width >> level;
    id<MTLBuffer> readback = [g_dev newBufferWithLength:n * n * 2 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command = q_cb(); id<MTLBlitCommandEncoder> copy = [command blitCommandEncoder];
    [copy copyFromTexture:texture sourceSlice:0 sourceLevel:level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(n,n,1)
                toBuffer:readback destinationOffset:0 destinationBytesPerRow:n * 2 destinationBytesPerImage:n * n * 2];
    [copy endEncoding]; [command commit]; [command waitUntilCompleted];
    CHECK(command.status == MTLCommandBufferStatusCompleted, "readback completed");
    memcpy(out, readback.contents, n * n * 2);
}
static void image_case(int explicit_image) { @autoreleasepool {
    tex_t texture = {.magic=MAGIC('T','E','X',0), .serial=99501 + 10 * explicit_image, .off=(uint64_t)explicit_image << 20};
    texture.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_R16F, .flags=NVN_TEXTURE_FLAGS_COMPRESSIBLE,
                            .width=8, .height=8, .depth=1, .levels=2, .samples=1};
    nvn_tex_layout(&texture.d, &texture.L); mtl_tex_init(&texture);
    tex_t alias = texture; alias.serial++; mtl_tex_init(&alias);
    id<MTLTexture> old = tex_mtl(&texture);
    CHECK(old && !(old.usage & MTLTextureUsageShaderWrite), "ordinary render target keeps its original usage");
    CHECK(tex_mtl(&alias) == old, "another serial shares the original backing");
    // Seed the smaller mip before the production render pass clears level 0.
    id<MTLCommandBuffer> seed = q_cb(); MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = old; rp.colorAttachments[0].level = 1; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0.75,0,0,1);
    [[seed renderCommandEncoderWithDescriptor:rp] endEncoding]; [seed commit]; [seed waitUntilCompleted];
    CHECK(seed.status == MTLCommandBufferStatusCompleted, "smaller mip seeded");
    mtl_reg_tex(601, &alias, NULL);
    os_unfair_lock_lock(&g_rmu); CHECK(tex_by_id(601) == old, "sampled descriptor caches the old backing"); os_unfair_lock_unlock(&g_rmu);
    mtl_sub_begin(); memset(&R, 0, sizeof R); R.has_rt = 1; R.rt.n = 1; R.rt.color[0] = &texture;
    R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0.5,0,0,1);
    CHECK(rp_begin() != nil && g_enc != nil, "production render pass is open before image registration");
    view_t mip = {.set_levels=1, .base_level=1, .nlevels=1};
    if (explicit_image) mtl_reg_image(602, &texture, &mip); else mtl_reg_tex(602, &texture, &mip);
    CHECK(texture.d.flags == NVN_TEXTURE_FLAGS_COMPRESSIBLE, "image registration leaves the guest texture flags alone");
    NvProg *program = [NvProg new]; NvShader *shader = [NvShader new]; program->sh[5] = shader;
    shader->rf.nb = 1; shader->rf.b[0] = (nvn_rbind){.kind=NVN_RB_IMG, .dtt=MTLTextureType2D, .dpf=NVN_DPF_FLOAT, .reg=0};
    S.img[5][0] = 602; image_prepare(program, 0x20);
    os_unfair_lock_lock(&g_rmu); uint64_t read_rid; int read_real;
    id<MTLTexture> read_only = img_binding(602, &shader->rf.b[0], &read_rid, &read_real);
    CHECK(read_real && read_only && !(read_only.usage & MTLTextureUsageShaderWrite), "load-only image accepts a read-only texture");
    CHECK(tex_mtl(&texture) == old && g_enc != nil, "load-only image does not upgrade storage or end the render pass");
    os_unfair_lock_unlock(&g_rmu);
    shader->rf.b[0].rw = 1; image_prepare(program, 0x20);
    CHECK(!g_enc, "image preparation ends the previous render pass before copying it");
    id<MTLTexture> upgraded = tex_mtl(&texture);
    CHECK(upgraded != old && (upgraded.usage & MTLTextureUsageShaderWrite), "explicit image use upgrades only its backing");
    CHECK(tex_mtl(&alias) == upgraded, "sampled alias follows the upgraded backing");
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> sampled = tex_by_id(601); uint64_t rid; int real;
    id<MTLTexture> image = img_binding(602, &shader->rf.b[0], &rid, &real);
    CHECK(sampled == upgraded, "cached sampled descriptor follows the replacement");
    CHECK(real && image.width == 4 && image.parentRelativeLevel == 1 && (image.usage & MTLTextureUsageShaderWrite), "image descriptor keeps its mip range and writable backing");
    CHECK(rid == image.gpuResourceID._impl, "image argument names the real view");
    os_unfair_lock_unlock(&g_rmu);
    mtl_sub_commit(); mtl_drain();
    uint16_t pixels[64]; read_level(upgraded, 0, pixels);
    int same = 1; for (int i = 0; i < 64; i++) same &= pixels[i] == 0x3800; CHECK(same, "upgrade preserves the clear from the pass it ended");
    read_level(upgraded, 1, pixels); same = 1; for (int i = 0; i < 16; i++) same &= pixels[i] == 0x3a00; CHECK(same, "upgrade preserves the other mip");
    NSError *error = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void put(texture2d<float, access::write> t [[texture(0)]]) { t.write(float4(0.25),uint2(1,1)); }\n" options:nil error:&error];
    id<MTLComputePipelineState> pipeline = [g_dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"put"] error:&error];
    CHECK(pipeline != nil, "image writer compiled");
    id<MTLCommandBuffer> write = q_cb(); id<MTLComputeCommandEncoder> encoder = [write computeCommandEncoder];
    [encoder setComputePipelineState:pipeline]; [encoder setTexture:image atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)]; [encoder endEncoding]; [write commit]; [write waitUntilCompleted];
    CHECK(write.status == MTLCommandBufferStatusCompleted, "image write completed");
    read_level(tex_mtl(&alias), 1, pixels); same = 1; for (int i = 0; i < 16; i++) same &= pixels[i] == (i == 5 ? 0x3400 : 0x3a00);
    CHECK(same, "sampled alias sees the image write and untouched pixels");
} }
int main(void) { @autoreleasepool {
    mtl_init(); image_case(0); image_case(1);
    printf("explicit image usage: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
