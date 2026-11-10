// frag_z_snap_gpu: dm_zsnap (tools/totk_xlate/FragZSnap.cs) on the GPU, against the depth the rasterizer stores.
// After seedds/nxnative's t_zsnap.m (MIT). Build and run (outside the checkout):
//   clang -fobjc-arc -O2 -framework Metal -framework Foundation -o <work>/frag_z_snap_gpu frag_z_snap_gpu.m && <work>/frag_z_snap_gpu
// Pass 1 draws a surface depth-only (LESS_EQUAL, D32F cleared to 1). Pass 2 draws the same vertices and indices; its
// fragment reads pass 1's depth at its own pixel and reports whether its z is above the stored depth:
//   plain    gl_FragCoord.z as it is. On an M4 some pixels are one ulp above (5,035 of 230,080 for a flat quad,
//            2026-11-10); the count is printed, not checked, since an M6 stores the fragment's z exactly (nxnative).
//   snapped  through dm_zsnap: no pixel may be above its own surface's stored depth.
//   farther  the same surface 4e-6 farther in depth, through dm_zsnap: every pixel must still be above.
// Four surfaces: flat in perspective, wavy, constant depth, w = 1. Exit status 1 when a check fails.
#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#include <stdio.h>
#include <math.h>
static NSString *SRC = @
"#include <metal_stdlib>\nusing namespace metal;\n"
"static inline __attribute__((always_inline)) float dm_zsnap(float z, float d, float zw)\n"
"{\n    float lo = as_type<float>(as_type<int>(z) - 1) - zw * 0x1p-20f;\n    return (z > 0.0f && d < z && d >= lo) ? d : z;\n}\n"
"struct V { float4 position [[position, invariant]]; };\n"
"struct P { float2 inv; float push; uint snap; };\n"
"vertex V vs(uint vid [[vertex_id]], const device float4* p [[buffer(0)]], constant P& u [[buffer(1)]]) { V v; v.position = p[vid]; v.position.z += u.push * v.position.w; return v; }\n"
"fragment void fs0() {}\n"
"fragment float4 fs1(V in [[stage_in]], texture2d<float> d [[texture(0)]], sampler s [[sampler(0)]], constant P& u [[buffer(0)]]) {\n"
"  float zw = fwidth(in.position.z); float st = d.sample(s, in.position.xy * u.inv, level(0.0)).x;\n"
"  float z = u.snap != 0u ? dm_zsnap(in.position.z, st, zw) : in.position.z;\n"
"  return float4(z > st ? 1.0 : 0.0, z < st ? 1.0 : 0.0, 0.0, 1.0); }\n";
typedef struct { float inv[2]; float push; uint32_t snap; } P;
int main(void) { @autoreleasepool {
    const int W = 960, H = 540, N = 48; int failed = 0;
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue]; NSError *e = nil;
    MTLCompileOptions *o = [MTLCompileOptions new]; o.mathMode = MTLMathModeSafe; o.preserveInvariance = YES;
    id<MTLLibrary> lib = [dev newLibraryWithSource:SRC options:o error:&e]; if (!lib) { printf("%s\n", e.localizedDescription.UTF8String); return 1; }
    int nv = (N + 1) * (N + 1), ni = N * N * 6; id<MTLBuffer> vb = [dev newBufferWithLength:nv * 16 options:0], ib = [dev newBufferWithLength:ni * 4 options:0];
    uint32_t *ix = ib.contents; int k = 0;
    for (int j = 0; j < N; j++) for (int i = 0; i < N; i++) { uint32_t a = j * (N + 1) + i, b = a + 1, c = a + N + 1, d = c + 1; ix[k++] = a; ix[k++] = b; ix[k++] = c; ix[k++] = c; ix[k++] = b; ix[k++] = d; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate; id<MTLTexture> depth = [dev newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatRGBA8Unorm; td.storageMode = MTLStorageModeShared; id<MTLTexture> col = [dev newTextureWithDescriptor:td];
    MTLRenderPipelineDescriptor *d0 = [MTLRenderPipelineDescriptor new]; d0.vertexFunction = [lib newFunctionWithName:@"vs"]; d0.fragmentFunction = [lib newFunctionWithName:@"fs0"]; d0.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> p0 = [dev newRenderPipelineStateWithDescriptor:d0 error:&e]; if (!p0) { printf("%s\n", e.localizedDescription.UTF8String); return 1; }
    MTLRenderPipelineDescriptor *d1 = [MTLRenderPipelineDescriptor new]; d1.vertexFunction = d0.vertexFunction; d1.fragmentFunction = [lib newFunctionWithName:@"fs1"]; d1.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> p1 = [dev newRenderPipelineStateWithDescriptor:d1 error:&e]; if (!p1) { printf("%s\n", e.localizedDescription.UTF8String); return 1; }
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionLessEqual; dd.depthWriteEnabled = YES; id<MTLDepthStencilState> ds = [dev newDepthStencilStateWithDescriptor:dd];
    id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    uint8_t *px = malloc(W * H * 4);
    static const char *const SURFACE[] = {"flat, perspective", "wavy, perspective", "constant depth", "w = 1"};
    printf("%s\n", dev.name.UTF8String);
    for (int s = 0; s < 4; s++) {
        float *p = vb.contents;
        for (int j = 0; j <= N; j++) for (int i = 0; i <= N; i++) { float u = (float)i / N, v = (float)j / N, *o4 = p + (j * (N + 1) + i) * 4;
            float zv = 2.0f + v * 30.0f, y = -1.5f + (s == 1 ? 0.02f * sinf(u * 17) * cosf(v * 13) : 0), n = 0.1f, f = 25000.0f;
            if (s < 2) { o4[0] = (u * 2 - 1) * 9.0f * 1.2f; o4[1] = y * 2.1f + zv * 0.05f; o4[2] = (f / (f - n)) * zv - (f * n) / (f - n); o4[3] = zv; }
            else { o4[0] = u * 1.6f - 0.8f; o4[1] = v * 1.6f - 0.8f; o4[2] = s == 2 ? 0.7310586f : 0.2f + 0.6f * u + 0.1f * v; o4[3] = 1; } }
        long counts[3][3];   // [plain, snapped, farther][covered, above, below]
        for (int mode = 0; mode < 3; mode++) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            MTLRenderPassDescriptor *r0 = [MTLRenderPassDescriptor renderPassDescriptor]; r0.depthAttachment.texture = depth; r0.depthAttachment.loadAction = MTLLoadActionClear; r0.depthAttachment.clearDepth = 1; r0.depthAttachment.storeAction = MTLStoreActionStore;
            P u0 = {{1.0f / W, 1.0f / H}, 0, 0}, u1 = {{1.0f / W, 1.0f / H}, mode == 2 ? 4e-6f : 0, mode != 0};
            id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:r0]; [en setRenderPipelineState:p0]; [en setDepthStencilState:ds]; [en setVertexBuffer:vb offset:0 atIndex:0]; [en setVertexBytes:&u0 length:sizeof u0 atIndex:1];
            [en drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:ni indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0]; [en endEncoding];
            MTLRenderPassDescriptor *r1 = [MTLRenderPassDescriptor renderPassDescriptor]; r1.colorAttachments[0].texture = col; r1.colorAttachments[0].loadAction = MTLLoadActionClear; r1.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); r1.colorAttachments[0].storeAction = MTLStoreActionStore;
            en = [cb renderCommandEncoderWithDescriptor:r1]; [en setRenderPipelineState:p1]; [en setVertexBuffer:vb offset:0 atIndex:0]; [en setVertexBytes:&u1 length:sizeof u1 atIndex:1];
            [en setFragmentTexture:depth atIndex:0]; [en setFragmentSamplerState:smp atIndex:0]; [en setFragmentBytes:&u1 length:sizeof u1 atIndex:0];
            [en drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:ni indexType:MTLIndexTypeUInt32 indexBuffer:ib indexBufferOffset:0]; [en endEncoding];
            [cb commit]; [cb waitUntilCompleted];
            [col getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
            long cov = 0, gt = 0, lt = 0; for (int i = 0; i < W * H; i++) if (px[i * 4 + 3]) { cov++; gt += px[i * 4] != 0; lt += px[i * 4 + 1] != 0; }
            counts[mode][0] = cov; counts[mode][1] = gt; counts[mode][2] = lt;
        }
        int ok = counts[1][0] > 1000 && counts[1][1] == 0 && counts[1][2] == 0 && counts[2][1] == counts[2][0];
        printf("%-18s covered %ld: plain above %ld below %ld | snapped above %ld below %ld | 4e-6 farther: above %ld of %ld  %s\n", SURFACE[s], counts[0][0], counts[0][1], counts[0][2],
               counts[1][1], counts[1][2], counts[2][1], counts[2][0], ok ? "ok" : "FAILED");
        failed += !ok;
    }
    printf(failed ? "FAILED\n" : "PASSED\n");
    return failed != 0; } }
