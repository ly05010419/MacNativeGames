/* Independent GPU check of nvn_fbread.h (ground decals reading their own G-buffer). No game data. A fragment function in
   SPIRV-Cross form samples the colour attachment it writes, at its own pixel, and adds 0.25; four draws of it overlap in one
   render pass whose attachment memory holds 0. Rewritten, each draw must see the one before it (1.0 everywhere, as on a GPU
   that renders straight to memory). The original function is the negative control: it reads memory, so it cannot reach
   1.0. A sample one texel to the side must still read memory: draw only the even columns so the sampled odd columns stay
   zero, independent of when a tile is stored. A texture used by anything but plain sample calls must be refused.
   nvn_fbdep.h (text only): a texture read only at positions that depend on another texture read (refraction) is told from
   one read at the fragment's own position, from a mix of both and from a read() use.
   Run alone through the shared game slot. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "nvn_fbread.h"
#include "nvn_fbdep.h"
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

/* The same form with a second texture (a normal map, ids 2 and 3) and a body: nvn_fbdep_dependent only reads the text. */
static NSString *refract(NSString *body) {
    return [NSString stringWithFormat:@"#include <metal_stdlib>\n\nusing namespace metal;\n\n"
        "struct spvDescriptorSetBuffer0\n{\n    texture2d<float> fp_t_tcb_8 [[id(0)]];\n    sampler fp_t_tcb_8Smplr [[id(1)]];\n"
        "    texture2d<float> fp_t_tcb_80 [[id(2)]];\n    sampler fp_t_tcb_80Smplr [[id(3)]];\n};\n\n"
        "fragment main0_out main0(main0_in in [[stage_in]], constant spvDescriptorSetBuffer0& spvDescriptorSet0 [[buffer(0)]])\n{\n"
        "    main0_out out = {};\n"
        "    float2 _1 = float2(in.m_41.x / in.m_41.w, in.m_41.y / in.m_41.w);\n"
        "    float4 _3 = spvDescriptorSet0.fp_t_tcb_80.sample(spvDescriptorSet0.fp_t_tcb_80Smplr, _1);\n"
        "    float _4 = _3.x;\n"
        "    float2 _5;\n"
        "    if (_1.x == 0.5)\n    {\n        _5 = _1;\n    }\n"
        "    _5.x = fma(_4, 0.01, _1.x);\n"
        "    float2 _6 = select(_5, float2(0.0), isnan(_5));\n"
        "    float4 _2 = spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, _6, level(0.0)).xyzw;\n"
        "%@"
        "    out.m_50 = _2 + float4(0.25);\n"
        "    return out;\n}\n", body];
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
    // nvn_fbdep.h: only a texture whose every use is a sample at a position that depends on a texture read is dependent
    check(nvn_fbdep_dependent(refract(@""), 0) == 1, "a sample at a position offset by another texture's value is dependent");
    check(nvn_fbdep_dependent(refract(@""), 2) == 0, "the normal map itself is read at the fragment's position");
    check(nvn_fbdep_dependent(decal(@"", @""), 0) == 0, "a decal reads at its own position");
    check(nvn_fbdep_dependent(refract(@"    float4 _7 = spvDescriptorSet0.fp_t_tcb_8.sample(spvDescriptorSet0.fp_t_tcb_8Smplr, _1);\n"), 0) == 0, "one plain sample among dependent ones is not dependent");
    check(nvn_fbdep_dependent(refract(@"    float4 _7 = spvDescriptorSet0.fp_t_tcb_8.read(uint2(_5));\n"), 0) == 0, "a read() use is not dependent");
    check(nvn_fbdep_dependent(refract(@"    float4 _7 = spvDescriptorSet0.fp_t_tcb_80.sample(spvDescriptorSet0.fp_t_tcb_80Smplr, _1, level(_4));\n"), 2) == 0, "a level that depends on a texture read does not make the position dependent");
    check(nvn_fbdep_dependent(refract(@""), 1) == 0 && nvn_fbdep_dependent(refract(@""), 9) == 0, "a sampler or an unknown id is not dependent");
    printf("{\"case\":\"dependent-read classifier (nvn_fbdep.h)\",\"checks\":7,\"pass\":true,\"game_data\":false}\n");
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
