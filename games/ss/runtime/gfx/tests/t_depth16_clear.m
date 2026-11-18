// Both production Depth16Unorm clear paths must store the same depth as a fragment. Use NVN's float inputs, including half-bin boundaries and their neighboring floats.
// Expectations come from the GPU's fragment writes; count raw load-clear differences for comparison without requiring a particular GPU's unadjusted conversion.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int production_clears(id<MTLTexture> attachment, id<MTLTexture> texture, const float *depth, int n) {
    tex_t targets[2] = {0};
    id<MTLTexture> textures[2] = {attachment, texture};
    for (int k = 0; k < 2; k++) {
        targets[k].magic = MAGIC('T','E','X',0); targets[k].serial = 99501 + k;
        targets[k].d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D_ARRAY, .format=NVN_FORMAT_DEPTH16,
            .width=(int)attachment.width, .height=(int)attachment.height, .depth=n, .levels=1, .samples=1};
        res_add(textures[k]); mtl_tex_init(&targets[k]); serial_put(targets[k].serial, textures[k]);
    }
    typedef struct {
        op_hdr rt_header; p_set_rt rt;
        op_hdr ds_header; p_clear_ds ds;
        op_hdr tex_header; p_clear_tex tex;
    } clear_ops;
    clear_ops *ops = calloc(1, (size_t)n * sizeof(*ops) + sizeof(op_hdr));
    if (!ops) return 1;
    for (int i = 0; i < n; i++) {
        ops[i].rt_header = (op_hdr){OP_SET_RT, sizeof(op_hdr) + sizeof(p_set_rt)};
        ops[i].rt = (p_set_rt){.depth=&targets[0], .has_dview=1, .dview={.set_layers=1, .min_layer=i}};
        ops[i].ds_header = (op_hdr){OP_CLEAR_DS, sizeof(op_hdr) + sizeof(p_clear_ds)};
        ops[i].ds = (p_clear_ds){.depth=depth[i], .depth_mask=1};
        ops[i].tex_header = (op_hdr){OP_CLEAR_TEX, sizeof(op_hdr) + sizeof(p_clear_tex)};
        ops[i].tex = (p_clear_tex){.tex=&targets[1], .mask=15,
            .r={0, 0, i, (int)texture.width, (int)texture.height, 1}};
        ops[i].tex.c.f[0] = depth[i];
    }
    *(op_hdr *)(ops + n) = (op_hdr){OP_END, sizeof(op_hdr)};
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    mtl_exec((const uint8_t *)ops); mtl_sub_commit(); mtl_drain();
    int failed = mtl_last_commit_error() != 0;
    free(ops);
    for (int k = 0; k < 2; k++) mtl_tex_release(&targets[k]);
    return failed;
}

int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev) { puts("t_depth16_clear: no Metal device, skipped"); return 0; }
    mtl_init(); dev = g_dev;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; };\n"
        "vertex V vs(uint i [[vertex_id]]) { float2 q = float2((i << 1) & 2, i & 2); return { float4(q * 2 - 1, 0, 1) }; }\n"
        "struct F { float d [[depth(any)]]; };\n"
        "fragment F fs(constant float &d [[buffer(0)]]) { return { d }; }\n" options:nil error:&err];
    if (!lib) { printf("t_depth16_clear: shader compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"]; pd.depthAttachmentPixelFormat = MTLPixelFormatDepth16Unorm;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> ds = [dev newDepthStencilStateWithDescriptor:dd];
    if (!ps || !ds) { printf("t_depth16_clear: pipeline failed: %s\n", err.localizedDescription.UTF8String); return 1; }
    enum { N = 1000 + 6 + 12 * 3, W = 8, H = 8 };
    float depth[N];
    for (int i = 0; i < 1000; i++) depth[i] = i / 999.f;
    const float extra[6] = {0.5f, 0.25f, 0.75f, 1.f / 3, 2.f / 3, 32767.5f / 65535};
    for (int i = 0; i < 6; i++) depth[1000 + i] = extra[i];
    const int bins[12] = {0, 1, 2, 3, 127, 128, 255, 256, 32766, 32767, 32768, 65534};
    for (int i = 0; i < 12; i++) {
        float d = (float)((bins[i] + 0.5) / 65535.0);
        depth[1006 + 3 * i] = nextafterf(d, 0);
        depth[1007 + 3 * i] = d;
        depth[1008 + 3 * i] = nextafterf(d, 1);
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth16Unorm width:W height:H mipmapped:NO];
    td.textureType = MTLTextureType2DArray; td.arrayLength = N; td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex[4] = {[dev newTextureWithDescriptor:td], [dev newTextureWithDescriptor:td], [dev newTextureWithDescriptor:td], [dev newTextureWithDescriptor:td]};   // fragment, ClearDepthStencil, raw clear, ClearTexture
    id<MTLBuffer> out = [dev newBufferWithLength:(size_t)4 * N * W * H * 2 options:MTLResourceStorageModeShared];
    if (!tex[0] || !tex[1] || !tex[2] || !tex[3] || !out) { puts("t_depth16_clear: resource allocation failed"); return 1; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    id<MTLCommandBuffer> cb = [q commandBuffer];
    for (int i = 0; i < N; i++) for (int k = 0; k < 3; k += 2) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = tex[k]; rp.depthAttachment.slice = (NSUInteger)i; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.clearDepth = k == 0 ? 1.0 : depth[i];
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        if (k == 0) { float d = (float)depth[i]; [e setRenderPipelineState:ps]; [e setDepthStencilState:ds]; [e setFragmentBytes:&d length:4 atIndex:0]; [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; }
        [e endEncoding];
    }
    [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { printf("t_depth16_clear: reference command buffer failed: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
    if (production_clears(tex[1], tex[3], depth, N)) { puts("t_depth16_clear: production clear failed"); return 1; }
    cb = [q commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    for (int k = 0; k < 4; k++) for (int i = 0; i < N; i++)
        [b copyFromTexture:tex[k] sourceSlice:(NSUInteger)i sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                  toBuffer:out destinationOffset:((size_t)k * N + i) * W * H * 2 destinationBytesPerRow:W * 2 destinationBytesPerImage:W * H * 2];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.error) { printf("t_depth16_clear: command buffer failed: %s\n", cb.error.localizedDescription.UTF8String); return 1; }
    const uint16_t *v = out.contents;
    int fail[2] = {0}, raw_off = 0;
    for (int i = 0; i < N; i++) {
        const uint16_t *f = v + (size_t)i * W * H, *c = v + ((size_t)N + i) * W * H, *r = v + ((size_t)2 * N + i) * W * H;
        const uint16_t *t = v + ((size_t)3 * N + i) * W * H;
        int bad[2] = {0}, roff = 0;
        for (int p = 0; p < W * H; p++) { bad[0] |= c[p] != f[p]; bad[1] |= t[p] != f[p]; roff |= r[p] != f[p]; }
        raw_off += roff;
        if ((bad[0] || bad[1]) && fail[0] + fail[1] < 10)
            printf("  depth %.9f: fragment %u, ClearDepthStencil %u, ClearTexture %u (raw clear %u)\n", depth[i], f[0], c[0], t[0], r[0]);
        for (int k = 0; k < 2; k++) fail[k] += bad[k];
    }
    printf("t_depth16_clear: %d depths, ClearDepthStencil differs from the fragment in %d, ClearTexture in %d, raw clear in %d (0.5: fragment %u, raw clear %u)\n",
           N, fail[0], fail[1], raw_off, v[(size_t)1000 * W * H], v[((size_t)2 * N + 1000) * W * H]);
    return fail[0] != 0 || fail[1] != 0;
} }
