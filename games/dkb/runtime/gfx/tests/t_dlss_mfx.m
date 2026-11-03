// NVN2's DLSS on MetalFX (nvn_mtl.m dlss_eval) with Metal encoding on its own thread and without: MetalFX's temporal scaler encodes into the replay's real command buffer
// (rec_cb_block), never the recording's stand-in, and fills a 1920x1080 output from a flat 960x540 colour; two evaluations through the same scaler before the submission ends
// keep each one's own output
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static tex_t texture(uint64_t serial, unsigned w, unsigned h, int format) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .off = serial << 24};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1, .flags = NVN_TEXTURE_FLAGS_IMAGE};
    nvn_tex_layout(&t.d, &t.L); mtl_tex_init(&t); return t;
}
static void fill(id<MTLTexture> t, MTLClearColor c, int depth) {
    id<MTLCommandBuffer> cb = q_cb(); MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (depth) { rp.depthAttachment.texture = t; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore; rp.depthAttachment.clearDepth = 0; }
    else { rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = c; }
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb commit]; [cb waitUntilCompleted];
}
static int flat(id<MTLTexture> t, const float want[3]) {   // the middle half of the picture (the scaler's edges may differ), within HDR rounding
    NSUInteger n = t.width * t.height; id<MTLBuffer> b = [g_dev newBufferWithLength:n * 8 options:MTLResourceStorageModeShared];
    if (!b) { fprintf(stderr, "readback buffer did not allocate\n"); return 0; }
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(t.width, t.height, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:t.width * 8 destinationBytesPerImage:n * 8];
    [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const __fp16 *p = b.contents;
    for (NSUInteger y = t.height / 4; y < t.height * 3 / 4; y++) for (NSUInteger x = t.width / 4; x < t.width * 3 / 4; x++)
        for (int c = 0; c < 3; c++) if (!(fabsf((float)p[4 * (y * t.width + x) + c] - want[c]) <= 0.03f)) {
            fprintf(stderr, "texel %lu,%lu channel %d: %g, expected %g\n", (unsigned long)x, (unsigned long)y, c, (float)p[4 * (y * t.width + x) + c], want[c]); return 0; }
    return 1;
}
int main(void) { @autoreleasepool {
    mtl_init();
    tex_t col = texture(90, 960, 540, NVN_FORMAT_RGBA16F), col2 = texture(91, 960, 540, NVN_FORMAT_RGBA16F), dep = texture(92, 960, 540, NVN_FORMAT_DEPTH32F),
          mot = texture(93, 960, 540, NVN_FORMAT_RG16F), out = texture(94, 1920, 1080, NVN_FORMAT_RGBA16F), out2 = texture(95, 1920, 1080, NVN_FORMAT_RGBA16F);
    const float a[3] = {0.5f, 0.25f, 0.75f}, b[3] = {0.125f, 0.625f, 0.25f};
    fill(tex_mtl(&col), MTLClearColorMake(a[0], a[1], a[2], 1), 0); fill(tex_mtl(&col2), MTLClearColorMake(b[0], b[1], b[2], 1), 0);
    fill(tex_mtl(&dep), MTLClearColorMake(0, 0, 0, 0), 1); fill(tex_mtl(&mot), MTLClearColorMake(0, 0, 0, 0), 0);
    p_dlss c = {.tex = {&col, &dep, &mot, &out}, .net = 1, .iw = 960, .ih = 540, .ow = 1920, .oh = 1080, .mv = {1, 1}, .pre_exposure = 1, .flags = DLSS_F_DEPTH_INVERTED, .reset = 1};
    p_dlss c2 = c; c2.tex[DLSS_COLOR] = &col2; c2.tex[DLSS_OUTPUT] = &out2;
    for (int frame = 0; frame < 4; frame++) {   // the temporal scaler settles over a few frames of the same input
        mtl_sub_begin(); dlss_eval(&c); dlss_eval(&c2);
        CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the submission completes");
        c.reset = c2.reset = 0;
    }
    CHECK(g_dlss[0].sc != nil, "MetalFX made a temporal scaler for these formats");
    CHECK(flat(tex_mtl(&out), a), "the first evaluation's output holds its own colour");
    CHECK(flat(tex_mtl(&out2), b), "the second evaluation through the same scaler holds its own");
    printf("t_dlss_mfx (encoding thread %s): %d checks, %d failed\n", rec_on() ? "on" : "off", checks, failed);
    return failed != 0;
}}
