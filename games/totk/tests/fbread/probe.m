/* Independent GPU check of nvn_fbread.h (ground decals reading their own G-buffer). No game data. A fragment function in
   SPIRV-Cross form samples the colour attachment it writes, at its own pixel, and adds 0.25; four draws of it overlap in one
   render pass whose attachment memory holds 0. Rewritten, each draw must see the one before it (1.0 everywhere, as on a GPU
   that renders straight to memory). The original function is the negative control: it reads memory, so it cannot reach
   1.0. A sample one texel to the side must still read memory: draw only the even columns so the sampled odd columns stay
   zero, independent of when a tile is stored. A texture used by anything but plain sample calls must be refused.
   Run alone through the shared game slot. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "nvn_fbread.h"
#include <stdio.h>
#include <stdlib.h>

static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); exit(1); }
}

/* The shape SPIRV-Cross gives the game's decals: an argument buffer, a combined sampler, the screen position as a varying. */
static NSString *decal(NSString *extra_param, NSString *extra_use) {
    return [NSString stringWithFormat:@"#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\n"
        "struct spvDescriptorSetBuffer0\n{\n    texture2d<float> fp_t_tcb_8 [[id(0)]];\n    sampler fp_t_tcb_8Smplr [[id(1)]];\n};\n\n"
        "struct main0_out\n{\n    float4 m_50 [[color(0)]];\n};\n\n"
        "struct main0_in\n{\n    float4 m_41 [[user(locn0)]];\n};\n\n"
        "fragment main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]]%@)\n{\n"
        "    main0_out out = {};\n"
        "    float2 _1 = float2(in.m_41.x / in.m_41.w, in.m_41.y / in.m_41.w);\n"
        "    float4 _2 = spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, select(_1, float2(0.0), isnan(_1))).xyzw;\n"
        "%@"
        "    out.m_50 = _2 + float4(0.25);\n"
        "    return out;\n}\n", extra_param, extra_use];
}

static NSString *vertex_src = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; float4 s [[user(locn0)]]; };\n"
    // a triangle covering the target; the varying is the texture coordinate of the pixel, shifted by `shift` texels
    "vertex V vs(uint i [[vertex_id]], constant float2& shift [[buffer(0)]]) {\n"
    "  float2 q = float2((i << 1) & 2, i & 2); V o; o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "  o.s = float4(q + shift, 0, 1); return o; }\n";

enum { W = 256, H = 192 };

int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); check(dev != nil, "Metal device");
    const int aid[] = {0}, att[] = {0};
    // a texture used by a read() as well must be refused; so must one with no plain sample
    check(nvn_fbread_src(decal(@"", @"    float4 _3 = spvDescriptorSet0.fp_t_tcb_8.read(uint2(0));\n"), @"main0", aid, att, 1) == nil, "a read() use is refused");
    check(nvn_fbread_src(decal(@"", @""), @"main1", aid, att, 1) == nil, "a missing entry point is refused");
    NSError *e = nil;
    id<MTLLibrary> vlib = [dev newLibraryWithSource:vertex_src options:nil error:&e]; check(vlib != nil, "vertex function");
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:W height:H mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [dev newTextureWithDescriptor:td];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.supportArgumentBuffers = YES;   // nearest, as the decals' G-buffer samplers
    id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
    id<MTLBuffer> args = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    ((MTLResourceID *)args.contents)[0] = target.gpuResourceID; ((MTLResourceID *)args.contents)[1] = smp.gpuResourceID;
    id<MTLBuffer> read = [dev newBufferWithLength:W * H * 8 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    check(target && smp && args && read && q, "test resources");
    /* case: name, rewritten?, [[position]] already a parameter?, shift in texels, expected value everywhere (negative: must not be) */
    struct { const char *name; int rewrite, has_pos; float shift; float want; int negative; } cases[] = {
        {"original (negative control)", 0, 0, 0, 1.0f, 1},
        {"rewritten", 1, 0, 0, 1.0f, 0},
        {"rewritten, gl_FragCoord already a parameter", 1, 1, 0, 1.0f, 0},
        {"rewritten, sample an untouched neighbouring column", 1, 1, 1, 0.25f, 0},
    };
    unsigned failed = 0;
    for (unsigned c = 0; c < sizeof cases / sizeof *cases; c++) { @autoreleasepool {
        NSString *src = decal(cases[c].has_pos ? @", float4 gl_FragCoord [[position]]" : @"",
                              cases[c].shift ? @"    if (uint(gl_FragCoord.x) & 1u) discard_fragment();\n" : @"");
        if (cases[c].rewrite) { src = nvn_fbread_src(src, @"main0", aid, att, 1); check(src != nil, "rewrite of the decal form"); }
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&e];
        if (!lib) fprintf(stderr, "%s\n", e.localizedDescription.UTF8String);
        check(lib != nil, "fragment function");
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [vlib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"main0"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e]; check(ps != nil, "pipeline");
        id<MTLCommandBuffer> cb = [q commandBuffer];
        MTLRenderPassDescriptor *zero = [MTLRenderPassDescriptor renderPassDescriptor];   // memory holds 0 before the decal pass
        zero.colorAttachments[0].texture = target; zero.colorAttachments[0].loadAction = MTLLoadActionClear;
        zero.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); zero.colorAttachments[0].storeAction = MTLStoreActionStore;
        [[cb renderCommandEncoderWithDescriptor:zero] endEncoding];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target; rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps]; [en useResource:target usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
        float shift[2] = {cases[c].shift / W, 0}; [en setVertexBytes:shift length:sizeof shift atIndex:0];
        [en setFragmentBuffer:args offset:0 atIndex:0];
        for (int d = 0; d < 4; d++) [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [en endEncoding];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                  toBuffer:read destinationOffset:0 destinationBytesPerRow:W * 8 destinationBytesPerImage:W * H * 8];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        check(cb.status != MTLCommandBufferStatusError, "command buffer");
        const __fp16 *px = read.contents; unsigned match = 0;
        for (unsigned y = 0; y < H; y++) for (unsigned x = 0; x < W; x++) {
            float want = cases[c].shift && (x & 1) ? 0 : cases[c].want;
            match += (float)px[(y * W + x) * 4] == want;
        }
        unsigned total = W * H; int ok = cases[c].negative ? match < total : match == total;
        printf("{\"case\":\"%s\",\"pixels\":%u,\"matching_expected\":%u,\"pass\":%s,\"game_data\":false}\n", cases[c].name, total, match, ok ? "true" : "false");
        failed += !ok;
    }}
    check(failed == 0, "decals must see the decals before them only through the rewrite, and only at their own pixel");
    return 0;
}}
