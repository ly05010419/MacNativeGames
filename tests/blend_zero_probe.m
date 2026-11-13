// tests/test_oot_blend_zero.py builds this: the blend-zero guard (nvn_blend_zero.h) on this Mac's GPU. Prints "ok <check>" or
// "FAIL <check>: <detail>" lines and "done <failures>". Arguments: MSL files to transform and compile (a sample of a seed).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <simd/simd.h>
#include "nvn_blend_zero.h"

static int fails;
static void expect(const char *name, int ok, NSString *detail) {
    printf("%s %s%s%s\n", ok ? "ok" : "FAIL", name, ok ? "" : ": ", ok ? "" : detail.UTF8String);
    if (!ok) fails++;
}

// A SPIRV-Cross-shaped pixel shader: colour 0 is a NaN rgb with the alpha the uniform gives (or a finite colour), colour 1
// always finite; two returns, as many translated shaders have.
static NSString *const SHADER =
    @"#include <metal_stdlib>\n#include <simd/simd.h>\n\nusing namespace metal;\n\n"
    @"struct P\n{\n    float4 v;\n};\n\n"
    @"struct main0_out\n{\n    float4 m_0 [[color(0)]];\n    float4 m_1 [[color(1)]];\n};\n\n"
    @"[[ early_fragment_tests ]] fragment main0_out main0(constant P& p [[buffer(0)]], float4 gl_FragCoord [[position]])\n{\n"
    @"    main0_out out = {};\n    float zero = p.v.x - p.v.x;\n    out.m_1 = float4(0.5);\n"
    @"    if (p.v.z > 0.5f)\n    {\n        out.m_0 = float4(0.125f, 0.25f, 0.375f, p.v.y);\n        return out;\n    }\n"
    @"    out.m_0 = float4(zero / zero, 1.0f / zero, -1.0f / zero, p.v.y);\n    return out;\n}\n";

static simd_float4 run(id<MTLDevice> dev, id<MTLLibrary> lib, MTLBlendFactor src, MTLBlendFactor dst, float alpha, int finite, uint32_t word) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    id<MTLLibrary> vl = [dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nvertex float4 vs(uint i [[vertex_id]]) { float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; return float4(p[i], 0, 1); }\n" options:nil error:nil];
    d.vertexFunction = [vl newFunctionWithName:@"vs"]; d.fragmentFunction = [lib newFunctionWithName:@"main0"];
    for (int i = 0; i < 2; i++) {
        d.colorAttachments[i].pixelFormat = MTLPixelFormatRGBA16Float;
        d.colorAttachments[i].blendingEnabled = i == 0;
        d.colorAttachments[i].sourceRGBBlendFactor = src; d.colorAttachments[i].destinationRGBBlendFactor = dst;
        d.colorAttachments[i].sourceAlphaBlendFactor = MTLBlendFactorOne; d.colorAttachments[i].destinationAlphaBlendFactor = MTLBlendFactorZero;
    }
    NSError *e = nil; id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) { printf("FAIL pipeline: %s\n", e.description.UTF8String); exit(1); }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> t0 = [dev newTextureWithDescriptor:td], t1 = [dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
    rp.colorAttachments[0].texture = t0; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1);
    rp.colorAttachments[1].texture = t1; rp.colorAttachments[1].loadAction = MTLLoadActionClear;
    id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:ps];
    float v[4] = {0, alpha, finite ? 1.0f : 0.0f, 0}; [en setFragmentBytes:v length:sizeof v atIndex:0];
    uint32_t w[4] = {word, 0, 0, 0}; [en setFragmentBytes:w length:sizeof w atIndex:BLEND_ZERO_BUFFER];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
    __fp16 px[4]; [t0 getBytes:px bytesPerRow:4 * 8 fromRegion:MTLRegionMake2D(1, 1, 1, 1) mipmapLevel:0];
    return (simd_float4){px[0], px[1], px[2], px[3]};
}
static int finite3(simd_float4 c) { return isfinite(c.x) && isfinite(c.y) && isfinite(c.z); }
static int near(simd_float4 c, float r, float g, float b) { return fabsf(c.x - r) < 1e-3f && fabsf(c.y - g) < 1e-3f && fabsf(c.z - b) < 1e-3f; }
static NSString *show(simd_float4 c) { return [NSString stringWithFormat:@"(%g, %g, %g, %g)", c.x, c.y, c.z, c.w]; }

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        // the bits of an output by its blend state
        expect("bits: SRC_ALPHA over a float target is 1", blend_zero_bits(1, 1, 5, 6, 1) == 1, @"");
        expect("bits: ONE_MINUS_SRC_ALPHA is 2", blend_zero_bits(1, 1, 6, 5, 1) == 2, @"");
        expect("bits: blending off, a non-float target, MIN / MAX give 0",
               !blend_zero_bits(0, 1, 5, 6, 1) && !blend_zero_bits(1, 0, 5, 6, 1) && !blend_zero_bits(1, 1, 5, 6, 4) && !blend_zero_bits(1, 1, 5, 6, 5), @"");
        expect("bits: a destination factor reading the source colour gives 0",
               !blend_zero_bits(1, 1, 5, 3, 1) && !blend_zero_bits(1, 1, 5, 4, 1) && !blend_zero_bits(1, 1, 5, 0x10, 1), @"");
        expect("bits: other source factors give 0", !blend_zero_bits(1, 1, 2, 6, 1) && !blend_zero_bits(1, 1, 1, 6, 1), @"");

        int guarded = 0; NSString *msl = msl_blend_zero(SHADER, &guarded);
        expect("transform: both returns guarded", guarded == 2, [NSString stringWithFormat:@"%d", guarded]);
        expect("transform: the word is main0's last parameter", [msl containsString:@"float4 gl_FragCoord [[position]], constant uint& dm_blend [[buffer(28)]])"], msl);
        expect("transform: the helpers stand before the entry's attribute", [msl rangeOfString:@"dm_bz(T c"].location < [msl rangeOfString:@"[[ early_fragment_tests ]]"].location, @"");
        int again = 0; msl_blend_zero(msl, &again);
        expect("transform: a guarded shader is left as it is", again == 0, @"");
        int none = 0; msl_blend_zero(@"struct main0_out\n{\n    uint4 m_0 [[color(0)]];\n};\n\nfragment main0_out main0()\n{\n    main0_out out = {};\n    return out;\n}\n", &none);
        expect("transform: no float colour output, no guard", none == 0, @"");
        int bare = 0; NSString *b0 = msl_blend_zero(@"struct main0_out\n{\n    float4 m_0 [[color(0)]];\n};\n\nfragment main0_out main0()\n{\n    main0_out out = {};\n    out.m_0 = float4(1.0);\n    return out;\n}\n", &bare);
        expect("transform: an entry without parameters takes the word as its only one", bare == 1 && [b0 containsString:@"main0(constant uint& dm_blend [[buffer(28)]])"], b0);
        int odd = 0; msl_blend_zero([SHADER stringByReplacingOccurrencesOfString:@"        return out;\n" withString:@"        return (out);\n"], &odd);
        expect("transform: a return of another shape leaves the shader unguarded", odd == 0, @"");

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&e];
        expect("transform: the guarded MSL compiles", lib != nil, e.description ?: @"");
        if (!lib) { printf("done %d\n", fails); return 1; }
        // what Apple's blender does without the guard: recorded, the guard is what the checks below need
        simd_float4 raw = run(dev, lib, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, 0, 0, 0);
        printf("note Apple's blender, NaN / Inf rgb at alpha 0 under SRC_ALPHA: %s\n", show(raw).UTF8String);
        simd_float4 a = run(dev, lib, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, 0, 0, 1);
        expect("GPU: SRC_ALPHA at alpha 0 keeps the target (word 1)", near(a, 0.25f, 0.5f, 0.75f), show(a));
        simd_float4 b = run(dev, lib, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorSourceAlpha, 1, 0, 2);
        expect("GPU: ONE_MINUS_SRC_ALPHA at alpha 1 keeps the target (word 2)", near(b, 0.25f, 0.5f, 0.75f), show(b));
        simd_float4 c = run(dev, lib, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, 0.5f, 1, 1);
        expect("GPU: a finite colour is blended as before", near(c, 0.125f * 0.5f + 0.25f * 0.5f, 0.25f * 0.5f + 0.5f * 0.5f, 0.375f * 0.5f + 0.75f * 0.5f), show(c));
        simd_float4 d = run(dev, lib, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, 0.5f, 0, 1);
        expect("GPU: a weight that is not zero keeps the NaN (as NVIDIA's blender does)", !finite3(d), show(d));

        // a seed's shaders: transformed and compiled
        int compiled = 0, seen = 0, kept = 0;
        for (int i = 1; i < argc; i++) {
            NSString *src = [NSString stringWithContentsOfFile:@(argv[i]) encoding:NSUTF8StringEncoding error:nil];
            if (!src) continue;
            seen++; int g = 0; NSString *t = msl_blend_zero(src, &g);
            if (!g) { kept++; continue; }
            NSError *ce = nil; MTLCompileOptions *o = [MTLCompileOptions new]; o.languageVersion = MTLLanguageVersion3_2;
            if ([dev newLibraryWithSource:t options:o error:&ce]) compiled++;
            else printf("FAIL compile %s: %s\n", argv[i], [[ce.description substringToIndex:MIN(ce.description.length, 400)] UTF8String]), fails++;
        }
        if (seen) printf("ok seed sample: %d shaders, %d guarded and compiled, %d left unguarded\n", seen, compiled, kept);
        printf("done %d\n", fails);
        return fails != 0;
    }
}
