/* Breath of the Wild 1.9.0's scene compose f7ee6fe1 (tone mapping) and its HDR twin (totk_hdr_compose.h,
   botw_hdr_compose_source), drawn on the GPU at one pixel. argv[1]: the compose MSL from the player's shader seed,
   argv[2] (optional): Tears of the Kingdom 1.4.3's compose 0ac9a549, which the BotW edit must refuse.
   Checks: the twin with HDR off draws what the game's shader draws; with HDR on, a dark grey keeps the game's value,
   a bright grey goes above SDR white, saturated colours keep the unfolded colour chain's values (the hue and purity
   donor, whose matrices are folded in totk_hdr_compose.h), and the bloom slider scales the bloom texture, not the scene. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#define TOTK_HDR_FRONTEND 1   /* the parameters' initial values, no frontend */
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/totk_hdr.h"
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/totk_hdr_compose.h"

static int failures;
static void check(int ok, const char *what) { printf("%s: %s\n", ok ? "pass" : "FAIL", what); if (!ok) failures++; }

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static NSString *const vertexSource = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; float4 m_31 [[user(locn0)]]; };\n"
    "vertex V vs(uint i [[vertex_id]]) { float2 u = float2((i << 1) & 2, i & 2); V o; o.p = float4(u * 2 - 1, 0, 1); o.m_31 = float4(u * 0.5, 0, 0); return o; }\n";

static id<MTLTexture> pixel(float r, float g, float b) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
    id<MTLTexture> t = [dev newTextureWithDescriptor:d];
    float v[4] = {r, g, b, 1};
    [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:v bytesPerRow:sizeof v];
    return t;
}

/* One pixel of the compose: scene and bloom colours, the game's constants (c1: luma weights and grade weight, c3: the
   saturation grade), and the HDR uniform when the function is the twin. */
static void draw(id<MTLFunction> fs, const float scene[3], const float bloom[3], const float grade[2],
                 const TotkHDRUniform *uniform, float out[4]) {
    NSError *e = nil;
    id<MTLLibrary> vl = [dev newLibraryWithSource:vertexSource options:nil error:&e];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [vl newFunctionWithName:@"vs"]; pd.fragmentFunction = fs;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
    if (!pso) { printf("FAIL: pipeline: %s\n", e.localizedDescription.UTF8String); exit(1); }
    id<MTLBuffer> c1 = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    id<MTLBuffer> c3 = [dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    float *w = c1.contents; w[0] = 0.5866f; w[1] = 0.1145f; w[2] = 2.0f / 3.0f;   /* the game's c1 (seed json) */
    float *g = c3.contents; g[0] = grade[0]; g[1] = grade[1];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.supportArgumentBuffers = YES;
    id<MTLSamplerState> sampler = [dev newSamplerStateWithDescriptor:sd];
    id<MTLTexture> t0 = pixel(scene[0], scene[1], scene[2]), t1 = pixel(bloom[0], bloom[1], bloom[2]);
    id<MTLArgumentEncoder> ae = [fs newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> args = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
    [ae setArgumentBuffer:args offset:0];
    [ae setBuffer:c1 offset:0 atIndex:0]; [ae setBuffer:c3 offset:0 atIndex:1];
    [ae setTexture:t0 atIndex:2]; [ae setSamplerState:sampler atIndex:3];
    [ae setTexture:t1 atIndex:4]; [ae setSamplerState:sampler atIndex:5];
    MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
    od.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [dev newTextureWithDescriptor:od];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor new];
    rp.colorAttachments[0].texture = target; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setRenderPipelineState:pso];
    [en useResource:c1 usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [en useResource:c3 usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [en useResource:t0 usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [en useResource:t1 usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [en setFragmentBuffer:args offset:0 atIndex:0];
    if (uniform) [en setFragmentBytes:uniform length:sizeof *uniform atIndex:30];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    [target getBytes:out bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
}

static id<MTLFunction> compile(NSString *source, const char *what) {
    NSError *e = nil;
    MTLCompileOptions *o = [MTLCompileOptions new]; o.mathMode = MTLMathModeSafe;   /* the precise:: calls, as in the game */
    id<MTLLibrary> l = [dev newLibraryWithSource:source options:o error:&e];
    id<MTLFunction> f = [l newFunctionWithName:@"main0"];
    if (!f) { printf("FAIL: %s does not compile: %s\n", what, e.localizedDescription.UTF8String); exit(1); }
    return f;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: %s f7ee6fe1.metal [0ac9a549.metal]\n", argv[0]); return 2; }
        dev = MTLCreateSystemDefaultDevice(); queue = [dev newCommandQueue];
        NSString *source = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
        NSString *twin = botw_hdr_compose_source(source);
        check(twin != nil, "the BotW 1.9.0 compose has the HDR twin's anchors");
        if (!twin) return 1;
        check(totk_hdr_compose_source(source) == nil, "the TOTK 1.4.3 edit refuses the BotW compose");
        NSString *totk = argc > 2 ? [NSString stringWithContentsOfFile:@(argv[2]) encoding:NSUTF8StringEncoding error:nil] : nil;
        if (totk) check(botw_hdr_compose_source(totk) == nil, "the BotW edit refuses the TOTK 1.4.3 compose");
        id<MTLFunction> game = compile(source, "the game's compose"), hdr = compile(twin, "the HDR twin");
        const float none[3] = {0, 0, 0}, grade[2] = {1, 0};
        const float colours[][3] = {{0.05f, 0.05f, 0.05f}, {0.9f, 0.3f, 0.1f}, {6.0f, 2.0f, 0.5f}, {0.02f, 0.4f, 1.5f}};
        TotkHDRUniform off, on;
        totk_hdr_uniform_snapshot(&off, 16, 0);
        totk_hdr_uniform_snapshot(&on, 16, 1);
        int same = 1;
        for (unsigned i = 0; i < sizeof colours / sizeof *colours; i++) {
            const float sat[2] = {0.9f, 0.35f}, bloom[3] = {0.1f, 0.2f, 0.05f};
            float a[4], b[4];
            draw(game, colours[i], bloom, sat, NULL, a); draw(hdr, colours[i], bloom, sat, &off, b);
            if (memcmp(a, b, sizeof a)) { same = 0; printf("  colour %u: game %g %g %g, twin off %g %g %g\n", i, a[0], a[1], a[2], b[0], b[1], b[2]); }
        }
        check(same, "HDR off: the twin draws exactly the game's picture");
        float game_dark[4], hdr_dark[4], game_bright[4], hdr_bright[4];
        const float dark[3] = {0.05f, 0.05f, 0.05f}, bright[3] = {4, 4, 4};
        draw(game, dark, none, grade, NULL, game_dark); draw(hdr, dark, none, grade, &on, hdr_dark);
        draw(game, bright, none, grade, NULL, game_bright); draw(hdr, bright, none, grade, &on, hdr_bright);
        printf("  grey 0.05: game %.5f, HDR %.5f; grey 4: game %.5f, HDR %.5f\n", game_dark[0], hdr_dark[0], game_bright[0], hdr_bright[0]);
        check(fabsf(game_dark[0] - hdr_dark[0]) < 2e-3f && fabsf(game_dark[1] - hdr_dark[1]) < 2e-3f, "HDR on: a dark grey keeps the game's value");
        check(game_bright[0] <= 1.0f && hdr_bright[0] > 2.0f, "HDR on: a bright grey goes above SDR white");
        /* hdr-bloom 0 takes the bloom texture out and leaves the scene: the pixel equals the scene alone */
        TotkHDRUniform nobloom = on; nobloom.v[9] = 0;
        const float bloomOnly[3] = {2, 2, 2};
        float with_bloom[4], without_bloom[4], scene_alone[4];
        draw(hdr, dark, bloomOnly, grade, &on, with_bloom);
        draw(hdr, dark, bloomOnly, grade, &nobloom, without_bloom);
        draw(hdr, dark, none, grade, &on, scene_alone);
        check(with_bloom[0] > scene_alone[0] + 0.5f && fabsf(without_bloom[0] - scene_alone[0]) < 1e-5f,
              "HDR on: the bloom setting scales the bloom texture (slot 1), not the scene");
        /* Saturated, bright colours go through the MacLeod-Boynton donor (hue, purity, the gamut radius); a grey never
           does. Expected: the unfolded chain (BT.709 -> BT.2020 -> XYZ -> LMS and back) in double precision. */
        static const struct { float in[3], want[3]; } saturated[] = {
            {{90.0f, 0.0f, 0.08f}, {22.80875f, 2.37583f, 3.42683f}},
            {{0.0f, 0.04f, 45.0f}, {0.94989f, 0.88846f, 15.55812f}},
            {{0.0f, 0.0f, 18.0f}, {0.23099f, 0.02663f, 11.51480f}},
            {{0.0f, 66.0f, 0.0f}, {10.62835f, 15.75229f, 3.44315f}},
            {{15.0f, 0.0f, 15.0f}, {11.09497f, 0.13595f, 10.17410f}},
            {{6.0f, 2.0f, 0.5f}, {3.18303f, 2.10211f, 0.79305f}},
            {{0.0f, 0.07f, 88.0f}, {1.78349f, 2.25130f, 16.91131f}},
            {{0.0f, 40.0f, 40.0f}, {6.90932f, 14.77589f, 14.48541f}},
            {{0.5f, 30.0f, 60.0f}, {8.64104f, 13.83339f, 13.93577f}}};
        int donor = 1;
        for (unsigned i = 0; i < sizeof saturated / sizeof *saturated; i++) {
            float got[4]; draw(hdr, saturated[i].in, none, grade, &on, got);
            for (int c = 0; c < 3; c++) if (!(fabsf(got[c] - saturated[i].want[c]) < 5e-3f)) {
                donor = 0; printf("  saturated %u channel %d: %.5f, expected %.5f\n", i, c, got[c], saturated[i].want[c]); }
        }
        check(donor, "HDR on: saturated colours keep the unfolded chain's values (hue and purity donor)");
    }
    printf(failures ? "probe failed\n" : "pass: BotW 1.9.0 HDR compose\n");
    return failures != 0;
}
