// A clear covers every layer of the bound render target view, as NVN's does, not only the layer a pass that is not layered attaches.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { W = 4, OLD_D = 0x3f400000, NEW_D = 0x3e800000, OLD_C = 0x11223344, NEW_C = 0x80808080 };   // depth .75 → .25; RGBA8 → 0.5 in every channel

int main(void) { @autoreleasepool {
    mtl_init();
    id<MTLCommandQueue> queue = [g_dev newCommandQueue];
    // depth: a cube (6 faces) or a 2D array of colour; view: none, or min_layer / nlayers (0: the rest)
    struct { int depth, target, layers, view, min_layer, nlayers; const char *label; } cases[] = {
        {1, NVN_TEXTURE_TARGET_CUBEMAP, 6, 0, 0, 0, "depth clear of a cube without a view clears all six faces"},
        {1, NVN_TEXTURE_TARGET_CUBEMAP, 6, 1, 2, 1, "depth clear through a one-face view clears that face only"},
        {0, NVN_TEXTURE_TARGET_2D_ARRAY, 4, 0, 0, 0, "colour clear of a 2D array without a view clears every layer"},
        {0, NVN_TEXTURE_TARGET_2D_ARRAY, 4, 1, 1, 0, "colour clear through a view from layer 1 on clears layers 1-3"},
        {0, NVN_TEXTURE_TARGET_2D_ARRAY, 4, 1, 1, 2, "colour clear through a two-layer view clears layers 1-2"},
        {0, NVN_TEXTURE_TARGET_2D, 1, 0, 0, 0, "colour clear of a 2D target stays one layer"},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        tex_t t = {.magic = MAGIC('T','E','X',0), .serial = 99501 + i};
        t.d = (nvn_texdesc){.target = cases[i].target, .format = cases[i].depth ? NVN_FORMAT_DEPTH32F_STENCIL8 : NVN_FORMAT_RGBA8, .width = W, .height = W,
                            .depth = cases[i].target == NVN_TEXTURE_TARGET_2D_ARRAY ? cases[i].layers : 1, .levels = 1, .samples = 1};
        MTLTextureDescriptor *descriptor = tex_desc(&t, 0);
        id<MTLTexture> base = [g_dev newTextureWithDescriptor:descriptor];
        CHECK(base != nil && base.arrayLength == (NSUInteger)cases[i].layers, "synthetic layered target allocates");
        if (!base) continue;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        for (int s = 0; s < cases[i].layers; s++) {   // every layer starts at the old value
            MTLRenderPassDescriptor *init = [MTLRenderPassDescriptor renderPassDescriptor];
            if (cases[i].depth) { init.depthAttachment.texture = base; init.depthAttachment.slice = s; init.depthAttachment.loadAction = MTLLoadActionClear;
                init.depthAttachment.storeAction = MTLStoreActionStore; init.depthAttachment.clearDepth = .75; }
            else { init.colorAttachments[0].texture = base; init.colorAttachments[0].slice = s; init.colorAttachments[0].loadAction = MTLLoadActionClear;
                init.colorAttachments[0].storeAction = MTLStoreActionStore; init.colorAttachments[0].clearColor = MTLClearColorMake(0x44 / 255., 0x33 / 255., 0x22 / 255., 0x11 / 255.); }
            [[cb renderCommandEncoderWithDescriptor:init] endEncoding];
        }
        [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted, "initial layers reach the GPU");
        res_add(base); mtl_tex_init(&t); serial_put(t.serial, base);
        state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
        view_t v = {0}; if (cases[i].view) { v.set_layers = 1; v.min_layer = (uint16_t)cases[i].min_layer; v.nlayers = (uint16_t)cases[i].nlayers; }
        R.has_rt = 1;
        if (cases[i].depth) { R.rt.depth = &t; R.rt.dview = v; R.rt.has_dview = (uint8_t)cases[i].view; }
        else { R.rt.n = 1; R.rt.color[0] = &t; R.rt.cview[0] = v; R.rt.has_cview[0] = (uint8_t)cases[i].view; }
        struct { op_hdr head; union { p_clear_ds ds; p_clear_color c; } clear; op_hdr end; } ops = {0};
        if (cases[i].depth) { ops.head = (op_hdr){OP_CLEAR_DS, sizeof(op_hdr) + sizeof(p_clear_ds)}; ops.clear.ds = (p_clear_ds){.depth = .25, .depth_mask = 1}; }
        else { ops.head = (op_hdr){OP_CLEAR_COLOR, sizeof(op_hdr) + sizeof(p_clear_color)}; ops.clear.c = (p_clear_color){.index = 0, .mask = 0xf, .rgba = {.5f, .5f, .5f, .5f}}; }
        ((op_hdr *)((uint8_t *)&ops + ops.head.size))->op = OP_END; ((op_hdr *)((uint8_t *)&ops + ops.head.size))->size = sizeof(op_hdr);
        mtl_exec((const uint8_t *)&ops);
        mtl_sub_commit(); mtl_drain();
        CHECK(!mtl_last_commit_error(), "production clear completes without GPU error");
        id<MTLBuffer> result = [g_dev newBufferWithLength:W * W * 4 * cases[i].layers options:MTLResourceStorageModeShared];
        cb = [queue commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        for (int s = 0; s < cases[i].layers; s++)
            [b copyFromTexture:base sourceSlice:s sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, W, 1) toBuffer:result destinationOffset:(NSUInteger)(W * W * 4 * s)
                destinationBytesPerRow:W * 4 destinationBytesPerImage:W * W * 4 options:cases[i].depth ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted, "every layer is read back");
        const uint32_t *px = result.contents; int valid = 1;
        int lo = cases[i].view ? cases[i].min_layer : 0, hi = cases[i].view && cases[i].nlayers ? lo + cases[i].nlayers : cases[i].layers;
        for (int s = 0; s < cases[i].layers; s++) {
            uint32_t want = s >= lo && s < hi ? (cases[i].depth ? NEW_D : NEW_C) : (cases[i].depth ? OLD_D : OLD_C);
            for (int k = 0; k < W * W; k++) if (px[s * W * W + k] != want) { valid = 0; fprintf(stderr, "case %u layer %d texel %d: %#x, want %#x\n", i, s, k, px[s * W * W + k], want); break; }
        }
        CHECK(valid, cases[i].label);
        printf("case %u: %s %s\n", i, cases[i].label, valid ? "PASS" : "FAIL");
        mtl_tex_release(&t);
    }
    printf("layered clears: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
