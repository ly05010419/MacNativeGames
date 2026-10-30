// Invariance between a vertex shader's two pipelines: the hybrid draws a pair with its pulling variant (<key>_bp, dmsh_pull.py) until the pipeline for the draw's vertex layout
// is built, so the same geometry can be drawn by one in a pass and by the other in the next (a depth prepass, then the colour pass with depth test equal; a shadow map, then the
// lookup). A vertex shader in the translator's shape ([[position, invariant]], compiled with preserve invariance as the chain does) goes through dmsh_pull.variant; 2,000 random
// triangles (half positions, snorm normals) are drawn into depth by one pipeline and then with depth test EQUAL by the other, both ways round: every covered pixel must pass
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
static int checks, failed;
#define EXPECT(name, want, got) do { uint64_t w = (want), g = (got); checks++; if (w != g) { fprintf(stderr, "%s: expected %llu, got %llu\n", name, (unsigned long long)w, (unsigned long long)g); failed++; } } while (0)
static NSString *k_vs =
@"#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\n"
"struct main0_out\n{\n    float4 out_c [[user(locn0)]];\n    float4 gl_Position [[position, invariant]];\n};\n\n"
"struct main0_in\n{\n    float4 m_0 [[attribute(0)]];\n    float4 m_1 [[attribute(1)]];\n};\n\n"
"vertex main0_out main0(main0_in in [[stage_in]], constant float4x4& u [[buffer(0)]])\n{\n    main0_out out = {};\n"
"    float4 p = u * float4(in.m_0.xyz, 1.0);\n    p.xyz = fma(in.m_1.xyz, float3(0.0137), p.xyz);\n    p.z = fma(p.z, 0.5, 0.25 * p.w);\n"
"    out.gl_Position = p;\n    out.out_c = in.m_1;\n    return out;\n}\n";
static NSString *k_fs = @"#include <metal_stdlib>\nusing namespace metal;\nstruct I { float4 c [[user(locn0)]]; };\nfragment float4 fs(I in [[stage_in]]) { return in.c * 0.5 + 0.5; }\n";
typedef struct { uint64_t base; uint32_t stride, divisor; } dm_vstream_c;
typedef struct { dm_vstream_c s[16]; uint32_t attr[16]; } dm_vlayout_c;
int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue]; NSError *e = nil;
    char dir[] = "/tmp/t_pull_inv.XXXXXX"; if (!mkdtemp(dir)) return 1;
    NSString *in = [@(dir) stringByAppendingPathComponent:@"v.metal"], *out = [@(dir) stringByAppendingPathComponent:@"vp.metal"];
    [k_vs writeToFile:in atomically:NO encoding:NSUTF8StringEncoding error:nil];
    char cmd[2048]; snprintf(cmd, sizeof cmd, "python3 -c \"import sys; sys.path.insert(0, '../tools'); import dmsh_pull; v, i = dmsh_pull.variant(open(sys.argv[1]).read()); open(sys.argv[2], 'w').write(v)\" '%s' '%s'", in.UTF8String, out.UTF8String);
    if (system(cmd)) { fprintf(stderr, "dmsh_pull.variant failed\n"); return 1; }
    NSString *pull = [NSString stringWithContentsOfFile:out encoding:NSUTF8StringEncoding error:nil];
    [NSFileManager.defaultManager removeItemAtPath:@(dir) error:nil];
    MTLCompileOptions *o = [MTLCompileOptions new]; o.preserveInvariance = YES;   // the chain's -fpreserve-invariance
    id<MTLLibrary> lv = [dev newLibraryWithSource:k_vs options:o error:&e], lp = [dev newLibraryWithSource:pull options:o error:&e], lf = [dev newLibraryWithSource:k_fs options:o error:&e];
    if (!lv || !lp || !lf) { fprintf(stderr, "compile: %s\n", e.localizedDescription.UTF8String); return 1; }
    enum { NT = 2000, NV = NT * 3, W = 256 };
    // vertex data: attribute 0 RGBA16F positions at offset 0, attribute 1 RGBA8 snorm at offset 8, stride 12
    id<MTLBuffer> vb = [dev newBufferWithLength:NV * 12 options:MTLResourceStorageModeShared]; uint8_t *p = vb.contents; srand(9);
    for (int v = 0; v < NV; v++) { _Float16 h[4]; for (int c = 0; c < 3; c++) h[c] = (_Float16)((float)(rand() % 20000) / 10000 - 1); h[3] = 1; memcpy(p + v * 12, h, 8);
        for (int c = 0; c < 4; c++) p[v * 12 + 8 + c] = (uint8_t)rand(); }
    float M[16] = { 0.93f, 0.11f, -0.07f, 0, -0.13f, 0.91f, 0.21f, 0, 0.05f, -0.19f, 0.83f, 0, 0.013f, -0.021f, 0.31f, 1 };
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatHalf4; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 16;
    vd.attributes[1].format = MTLVertexFormatChar4Normalized; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 16; vd.layouts[16].stride = 12;
    dm_vlayout_c L; memset(&L, 0, sizeof L); L.s[0].base = vb.gpuAddress; L.s[0].stride = 12; L.attr[0] = 0x29; L.attr[1] = 0x26 | 8u << 8;
    id<MTLBuffer> lay = [dev newBufferWithBytes:&L length:sizeof L options:MTLResourceStorageModeShared];
    id<MTLRenderPipelineState> ps[2];
    for (int k = 0; k < 2; k++) {
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; d.vertexFunction = [(k ? lp : lv) newFunctionWithName:@"main0"]; d.fragmentFunction = [lf newFunctionWithName:@"fs"];
        if (!k) d.vertexDescriptor = vd;
        d.colorAttachments[0].pixelFormat = MTLPixelFormatR32Float; d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        ps[k] = [dev newRenderPipelineStateWithDescriptor:d error:&e]; if (!ps[k]) { fprintf(stderr, "pipeline: %s\n", e.localizedDescription.UTF8String); return 1; }
    }
    MTLDepthStencilDescriptor *dl = [MTLDepthStencilDescriptor new]; dl.depthCompareFunction = MTLCompareFunctionLess; dl.depthWriteEnabled = YES;
    MTLDepthStencilDescriptor *de = [MTLDepthStencilDescriptor new]; de.depthCompareFunction = MTLCompareFunctionEqual; de.depthWriteEnabled = NO;
    id<MTLDepthStencilState> sl = [dev newDepthStencilStateWithDescriptor:dl], se = [dev newDepthStencilStateWithDescriptor:de];
    for (int first = 0; first < 2; first++) {   // first: the pipeline that draws the depth prepass (0 vertex descriptor, 1 pulling)
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:W height:W mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> c0 = [dev newTextureWithDescriptor:td], c1 = [dev newTextureWithDescriptor:td];
        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:W height:W mipmapped:NO]; dd.usage = MTLTextureUsageRenderTarget; dd.storageMode = MTLStorageModePrivate;
        id<MTLTexture> depth = [dev newTextureWithDescriptor:dd];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        for (int pass = 0; pass < 2; pass++) {
            int k = pass == 0 ? first : !first;
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
            rp.colorAttachments[0].texture = pass ? c1 : c0; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.depthAttachment.texture = depth; rp.depthAttachment.loadAction = pass ? MTLLoadActionLoad : MTLLoadActionClear; rp.depthAttachment.clearDepth = 1; rp.depthAttachment.storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
            [en setRenderPipelineState:ps[k]]; [en setDepthStencilState:pass ? se : sl]; [en setVertexBytes:M length:64 atIndex:0];
            if (k) { [en setVertexBuffer:lay offset:0 atIndex:16]; [en useResource:vb usage:MTLResourceUsageRead stages:MTLRenderStageVertex]; } else [en setVertexBuffer:vb offset:0 atIndex:16];
            [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:NV]; [en endEncoding];
        }
        [cb commit]; [cb waitUntilCompleted];
        EXPECT("no GPU error", 0, cb.error != nil);
        float *a = malloc(W * W * 4), *b = malloc(W * W * 4);
        [c0 getBytes:a bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, W) mipmapLevel:0]; [c1 getBytes:b bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, W) mipmapLevel:0];
        int covered = 0, passed = 0; for (int i = 0; i < W * W; i++) { covered += a[i] != 0; passed += a[i] != 0 && b[i] != 0; }
        char name[120]; snprintf(name, sizeof name, "%s prepass, %s with depth equal: covered pixels that pass (of %d)", first ? "pulling" : "vertex descriptor", first ? "vertex descriptor" : "pulling", covered);
        EXPECT(name, covered, passed);
        checks++; if (covered < W * W / 2) { fprintf(stderr, "only %d of %d pixels covered: the test draws too little\n", covered, W * W); failed++; }
        free(a); free(b);
    }
    printf("t_pull_inv: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
