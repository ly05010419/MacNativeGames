// Partial depth/stencil clears preserve every pixel and stencil bit outside their mask.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

int main(void) { @autoreleasepool {
    mtl_init();
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void read_planes(texture2d<uint,access::read> s [[texture(0)]], depth2d<float,access::read> d [[texture(1)]], device uint2 *o [[buffer(0)]], uint2 p [[thread_position_in_grid]]) { o[p.y*8+p.x]=uint2(s.read(p).r, as_type<uint>(d.read(p))); }\n" options:nil error:&error];
    id<MTLComputePipelineState> reader = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"read_planes"] error:&error];
    CHECK(reader != nil, "synthetic plane reader compiles");
    if (!reader) return 1;
    id<MTLCommandQueue> queue = [g_dev newCommandQueue];
    struct { int mid; p_scissor sc; int depth, mask; const char *label; } cases[] = {
        {0, {2,1,3,2}, 1, 0, "outside-pass scissored depth clear"},
        {0, {2,1,3,2}, 0, 255, "outside-pass scissored stencil clear"},
        {0, {0,0,8,8}, 0, 15, "outside-pass masked stencil clear"},
        {1, {0,0,8,8}, 0, 15, "inside-pass masked stencil clear"},
        {1, {2,1,3,2}, 1, 15, "inside-pass scissored depth/stencil clear"},
        {0, {2,1,0,2}, 1, 255, "empty scissor writes nothing"},
        {0, {-1,-1,3,3}, 1, 15, "negative scissor origin is clamped"},
        {0, {0,0,8,8}, 1, 255, "full clear remains a load action"},
    };
    for (unsigned i=0; i<sizeof cases/sizeof *cases; i++) {
        tex_t t = {.magic=MAGIC('T','E','X',0), .serial=99401+i};
        t.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_DEPTH32F_STENCIL8, .width=8, .height=8, .depth=1, .levels=1, .samples=1};
        MTLTextureDescriptor *descriptor = tex_desc(&t, 0); descriptor.usage |= MTLTextureUsagePixelFormatView;
        id<MTLTexture> base = [g_dev newTextureWithDescriptor:descriptor];
        CHECK(base != nil, "synthetic depth/stencil target allocates");
        if (!base) continue;
        MTLRenderPassDescriptor *init = [MTLRenderPassDescriptor renderPassDescriptor];
        init.depthAttachment.texture = init.stencilAttachment.texture = base;
        init.depthAttachment.loadAction = init.stencilAttachment.loadAction = MTLLoadActionClear;
        init.depthAttachment.storeAction = init.stencilAttachment.storeAction = MTLStoreActionStore;
        init.depthAttachment.clearDepth = .75; init.stencilAttachment.clearStencil = 0xa5;
        id<MTLCommandBuffer> cb = [queue commandBuffer]; [[cb renderCommandEncoderWithDescriptor:init] endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted, "literal initial planes reach the GPU");
        res_add(base); mtl_tex_init(&t); serial_put(t.serial, base);
        state_reset(); memset(&R,0,sizeof R); mtl_sub_begin();
        R.has_rt=1; R.rt.depth=&t; R.has_sc=1; R.sc=cases[i].sc;
        if (cases[i].mid) CHECK(rp_begin() != nil, "preexisting render pass opens");
        struct { op_hdr head; p_clear_ds clear; op_hdr end; } ops = {0};
        ops.head=(op_hdr){OP_CLEAR_DS, sizeof(op_hdr)+sizeof(p_clear_ds)};
        ops.clear=(p_clear_ds){.depth=.25, .depth_mask=cases[i].depth, .stencil=0x3c, .stencil_mask=cases[i].mask};
        ops.end=(op_hdr){OP_END,sizeof(op_hdr)};
        mtl_exec((const uint8_t *)&ops); mtl_sub_commit(); mtl_drain();
        CHECK(!mtl_last_commit_error(), "production clear completes without GPU error");
        id<MTLTexture> stencil = [base newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
        id<MTLBuffer> result = [g_dev newBufferWithLength:64*8 options:MTLResourceStorageModeShared];
        cb=[queue commandBuffer]; id<MTLComputeCommandEncoder> e=[cb computeCommandEncoder];
        [e setComputePipelineState:reader]; [e setTexture:stencil atIndex:0]; [e setTexture:base atIndex:1]; [e setBuffer:result offset:0 atIndex:0];
        [e dispatchThreads:MTLSizeMake(8,8,1) threadsPerThreadgroup:MTLSizeMake(8,8,1)]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
        CHECK(cb.status == MTLCommandBufferStatusCompleted, "both resulting planes are read on the GPU");
        uint32_t *pixels=result.contents; int valid=1;
        for (int y=0;y<8;y++) for (int x=0;x<8;x++) {
            p_scissor sc=cases[i].sc; int inside=x>=sc.x && x<sc.x+sc.w && y>=sc.y && y<sc.y+sc.h;
            unsigned mask=inside?cases[i].mask:0, want_s=(0xa5&~mask)|(0x3c&mask);
            uint32_t want_d=inside&&cases[i].depth?0x3e800000:0x3f400000;
            valid &= pixels[2*(y*8+x)]==want_s && pixels[2*(y*8+x)+1]==want_d;
        }
        CHECK(valid,cases[i].label);
        printf("case %u: %s %s\n",i,cases[i].label,valid?"PASS":"FAIL");
        mtl_tex_release(&t);
    }
    printf("partial depth/stencil clears: %d checks, %d failed\n",checks,failed);
    return failed!=0;
} }
