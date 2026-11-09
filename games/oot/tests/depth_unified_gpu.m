// Can one translation serve a slot whether a colour or a depth texture is bound?
// Compares, on the GPU and through an argument buffer as the game binds resources:
//   ref: what the depth variant does today (depth2d declaration, totk_depth_fetch, totk_nearest)
//   uni: the default variant's code (texture2d declaration, spvDepthCast for compares) with the same depth texture bound,
//        once as a plain texture and once through a (R,0,0,1) swizzle view, with a nearest / no-mip / clamp sampler.
// Build: clang -fobjc-arc -framework Metal -framework Foundation depth_unify_test.m -o depth_unify_test
// Run with MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 as well.
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

enum { W = 8, H = 8, MODES = 13 };
static const char *mode_name[MODES] = {"sample-implicit", "sample-lod0", "sample-lod1", "sample-offset", "read-lod0", "read-lod1", "gather",
                                       "compare", "gather-compare", "array-sample", "array-read", "array-gather", "array-compare"};

static NSString *msl = @R"MSL(
#include <metal_stdlib>
using namespace metal;
template <typename T> static inline depth2d<T> spvDepthCast(texture2d<T> t) { return reinterpret_cast<thread const depth2d<T> &>(t); }
template <typename T> static inline depth2d_array<T> spvDepthCast(texture2d_array<T> t) { return reinterpret_cast<thread const depth2d_array<T> &>(t); }
static inline float4 totk_depth_fetch(float d) { return float4(d, 0.0, 0.0, 1.0); }
constexpr sampler totk_nearest(filter::nearest, mip_filter::none, address::clamp_to_edge);
struct RefSet { depth2d<float> t [[id(0)]]; depth2d_array<float> a [[id(1)]]; sampler s [[id(2)]]; sampler c [[id(3)]]; };
struct UniSet { texture2d<float> t [[id(0)]]; texture2d_array<float> a [[id(1)]]; sampler s [[id(2)]]; sampler c [[id(3)]]; };
struct Vary { float4 p [[position]]; };
vertex Vary vs(uint i [[vertex_id]]) { Vary o; float2 u = float2((i << 1) & 2, i & 2); o.p = float4(u * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
struct Fill { float mip, slice; };
struct DepthOut { float d [[depth(any)]]; };
fragment DepthOut fill(Vary in [[stage_in]], constant Fill &f [[buffer(0)]]) {
    DepthOut o; float2 q = floor(in.p.xy); o.d = fract((q.x * 17 + q.y * 31 + f.mip * 7 + f.slice * 13 + 1) / 97.0); return o; }
#ifdef FRAG
#define IMPLICIT(FETCH, S) case 0: return FETCH(set.t.sample(S, uv));
#else
#define IMPLICIT(FETCH, S) case 0: return float4(0);
#endif
#define BODY(SET, FETCH, NEAR, GATHER, GATHER_A, CAST)                                                                  \
static float4 eval(constant SET &set, float2 pos, int mode) {                                                           \
    float2 uv = (pos + 0.3) / 8.0; uint2 ip = uint2(pos) / 2; float r = fract(pos.x * 0.13 + pos.y * 0.07);              \
    switch (mode) {                                                                                                     \
    IMPLICIT(FETCH, NEAR)                                                                                               \
    case 1: return FETCH(set.t.sample(NEAR, uv, level(0.0)));                                                           \
    case 2: return FETCH(set.t.sample(NEAR, uv, level(1.0)));                                                           \
    case 3: return FETCH(set.t.sample(NEAR, uv, level(0.0), int2(1, 0)));                                               \
    case 4: return FETCH(set.t.read(uint2(pos), 0));                                                                    \
    case 5: return FETCH(set.t.read(ip, 1));                                                                            \
    case 6: return GATHER(set.t, uv);                                                                                   \
    case 7: return float4(CAST(set.t).sample_compare(set.c, uv, r, level(0.0)));                                        \
    case 8: return CAST(set.t).gather_compare(set.c, uv, r);                                                            \
    case 9: return FETCH(set.a.sample(NEAR, uv, 1, level(0.0)));                                                        \
    case 10: return FETCH(set.a.read(uint2(pos), 1, 0));                                                                \
    case 11: return GATHER_A(set.a, uv);                                                                                \
    default: return float4(CAST(set.a).sample_compare(set.c, uv, 1, r, level(0.0)));                                    \
    } }
#define REF_FETCH totk_depth_fetch
#define UNI_FETCH
#define REF_G(t, uv) t.gather(set.s, uv)
#define REF_GA(t, uv) t.gather(set.s, uv, 1)
#define UNI_G(t, uv) t.gather(set.s, uv, int2(0), component::x)
#define UNI_GA(t, uv) t.gather(set.s, uv, 1, int2(0), component::x)
#define NOCAST(t) t
namespace ref { BODY(RefSet, REF_FETCH, totk_nearest, REF_G, REF_GA, NOCAST) }
namespace uni { BODY(UniSet, UNI_FETCH, set.s, UNI_G, UNI_GA, spvDepthCast) }
#ifdef FRAG
fragment float4 ref_fn(Vary in [[stage_in]], constant RefSet &set [[buffer(0)]], constant int &mode [[buffer(1)]]) { return ref::eval(set, in.p.xy, mode); }
fragment float4 uni_fn(Vary in [[stage_in]], constant UniSet &set [[buffer(0)]], constant int &mode [[buffer(1)]]) { return uni::eval(set, in.p.xy, mode); }
#else
kernel void ref_fn(uint2 id [[thread_position_in_grid]], constant RefSet &set [[buffer(0)]], constant int &mode [[buffer(1)]], device float4 *o [[buffer(2)]]) { o[id.y * 8 + id.x] = ref::eval(set, float2(id) + 0.5, mode); }
kernel void uni_fn(uint2 id [[thread_position_in_grid]], constant UniSet &set [[buffer(0)]], constant int &mode [[buffer(1)]], device float4 *o [[buffer(2)]]) { o[id.y * 8 + id.x] = uni::eval(set, float2(id) + 0.5, mode); }
#endif
)MSL";

static id<MTLDevice> dev; static id<MTLCommandQueue> queue; static id<MTLLibrary> frag, comp; static int problems;

static void die_if(id o, NSError *e, const char *what) { if (!o) { printf("FAILED %s: %s\n", what, e.description.UTF8String); exit(2); } }
static void finish(id<MTLCommandBuffer> c, const char *what) {
    [c commit]; [c waitUntilCompleted];
    if (c.status != MTLCommandBufferStatusCompleted) { printf("COMMAND BUFFER ERROR (%s): %s\n", what, c.error.description.UTF8String); problems++; } }

static id<MTLTexture> depth_tex(MTLPixelFormat f, MTLTextureType type, int slices) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor new]; d.textureType = type; d.pixelFormat = f; d.width = W; d.height = H; d.arrayLength = slices; d.mipmapLevelCount = 2;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate; return [dev newTextureWithDescriptor:d]; }

static void fill_tex(id<MTLTexture> t) {
    NSError *e; MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
    p.vertexFunction = [frag newFunctionWithName:@"vs"]; p.fragmentFunction = [frag newFunctionWithName:@"fill"]; p.depthAttachmentPixelFormat = t.pixelFormat;
    BOOL st = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8; if (st) p.stencilAttachmentPixelFormat = t.pixelFormat;
    id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:p error:&e]; die_if(ps, e, "fill pipeline");
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = YES;
    id<MTLDepthStencilState> ds = [dev newDepthStencilStateWithDescriptor:dd];
    for (NSUInteger s = 0; s < t.arrayLength; s++) for (NSUInteger m = 0; m < t.mipmapLevelCount; m++) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = t; rp.depthAttachment.level = m; rp.depthAttachment.slice = s; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (st) { rp.stencilAttachment.texture = t; rp.stencilAttachment.level = m; rp.stencilAttachment.slice = s; rp.stencilAttachment.loadAction = MTLLoadActionClear; rp.stencilAttachment.storeAction = MTLStoreActionStore; }
        id<MTLCommandBuffer> c = [queue commandBuffer]; id<MTLRenderCommandEncoder> r = [c renderCommandEncoderWithDescriptor:rp];
        [r setRenderPipelineState:ps]; [r setDepthStencilState:ds]; float f[2] = {(float)m, (float)s}; [r setFragmentBytes:f length:8 atIndex:0];
        [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [r endEncoding]; finish(c, "fill");
    } }

static id<MTLSamplerState> sampler(BOOL compare, BOOL game) {
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new]; d.supportArgumentBuffers = YES;
    d.sAddressMode = d.tAddressMode = game ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    d.minFilter = d.magFilter = game || compare ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = game ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped;
    if (compare) d.compareFunction = MTLCompareFunctionLessEqual;
    return [dev newSamplerStateWithDescriptor:d]; }

static void run(BOOL fragment_stage, NSString *name, id<MTLTexture> t, id<MTLTexture> a, id<MTLSamplerState> s, id<MTLSamplerState> cs, int mode, float *out) {
    NSError *e; id<MTLBuffer> ab = [dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    MTLResourceID *ids = (MTLResourceID *)ab.contents; ids[0] = t.gpuResourceID; ids[1] = a.gpuResourceID; ids[2] = s.gpuResourceID; ids[3] = cs.gpuResourceID;
    id<MTLCommandBuffer> c = [queue commandBuffer];
    if (fragment_stage) {
        MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new]; p.vertexFunction = [frag newFunctionWithName:@"vs"]; p.fragmentFunction = [frag newFunctionWithName:name];
        p.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float; id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:p error:&e]; die_if(ps, e, "pipeline");
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:W height:H mipmapped:NO]; td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor]; rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> r = [c renderCommandEncoderWithDescriptor:rp]; [r setRenderPipelineState:ps];
        [r setFragmentBuffer:ab offset:0 atIndex:0]; [r setFragmentBytes:&mode length:4 atIndex:1];
        [r useResource:t usage:MTLResourceUsageRead stages:MTLRenderStageFragment]; [r useResource:a usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
        [r drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [r endEncoding]; finish(c, "draw");
        [rt getBytes:out bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    } else {
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[comp newFunctionWithName:name] error:&e]; die_if(ps, e, "compute pipeline");
        id<MTLBuffer> ob = [dev newBufferWithLength:W * H * 16 options:MTLResourceStorageModeShared];
        id<MTLComputeCommandEncoder> r = [c computeCommandEncoder]; [r setComputePipelineState:ps]; [r setBuffer:ab offset:0 atIndex:0]; [r setBytes:&mode length:4 atIndex:1]; [r setBuffer:ob offset:0 atIndex:2];
        [r useResource:t usage:MTLResourceUsageRead]; [r useResource:a usage:MTLResourceUsageRead];
        [r dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; [r endEncoding]; finish(c, "dispatch"); memcpy(out, ob.contents, W * H * 16);
    } }

int main(void) { @autoreleasepool {
    dev = MTLCreateSystemDefaultDevice(); queue = [dev newCommandQueue]; NSError *e;
    printf("device %s, argument buffers tier %d\n", dev.name.UTF8String, (int)dev.argumentBuffersSupport + 1);
    MTLCompileOptions *fo = [MTLCompileOptions new]; fo.preprocessorMacros = @{@"FRAG": @1}; frag = [dev newLibraryWithSource:msl options:fo error:&e]; die_if(frag, e, "fragment library");
    comp = [dev newLibraryWithSource:msl options:[MTLCompileOptions new] error:&e]; die_if(comp, e, "compute library");
    id<MTLSamplerState> near = sampler(NO, NO), game = sampler(NO, YES), cs = sampler(YES, NO);
    MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleOne);
    struct { MTLPixelFormat f; const char *n; } fmts[] = {{MTLPixelFormatDepth32Float, "Depth32Float"}, {MTLPixelFormatDepth16Unorm, "Depth16Unorm"}, {MTLPixelFormatDepth32Float_Stencil8, "Depth32Float_Stencil8"}};
    long total = 0;
    for (int i = 0; i < 3; i++) {
        id<MTLTexture> t = depth_tex(fmts[i].f, MTLTextureType2D, 1), a = depth_tex(fmts[i].f, MTLTextureType2DArray, 2); fill_tex(t); fill_tex(a);
        id<MTLTexture> vt = [t newTextureViewWithPixelFormat:t.pixelFormat textureType:t.textureType levels:NSMakeRange(0, 2) slices:NSMakeRange(0, 1) swizzle:sw];
        id<MTLTexture> va = [a newTextureViewWithPixelFormat:a.pixelFormat textureType:a.textureType levels:NSMakeRange(0, 2) slices:NSMakeRange(0, 2) swizzle:sw];
        if (!vt || !va) { printf("%s: swizzle view not created\n", fmts[i].n); problems++; continue; }
        for (int stage = 0; stage < 2; stage++) for (int m = stage; m < MODES; m++) {   // the compute stage has no implicit LOD
            float rf[W * H * 4], pl[W * H * 4], sv[W * H * 4];
            run(!stage, @"ref_fn", t, a, near, cs, m, rf); run(!stage, @"uni_fn", t, a, near, cs, m, pl); run(!stage, @"uni_fn", vt, va, near, cs, m, sv);
            BOOL vec = m == 6 || m == 8 || m == 11;   // gathers return four depths; the others (d,0,0,1) or (d,d,d,1)
            int xbad = 0, wbad = 0; float x0 = 0, y0 = 0;
            for (int p = 0; p < W * H; p++) {
                if (memcmp(&rf[p * 4], &pl[p * 4], vec ? 16 : 4)) { xbad++; x0 = rf[p * 4]; y0 = pl[p * 4]; }
                if (memcmp(&rf[p * 4], &sv[p * 4], 16)) wbad++; }
            total += W * H * 2;
            printf("%-22s %-8s %-16s plain view .x mismatches %2d/%d | (R,0,0,1) view whole-vector mismatches %2d/%d | ref (%.5f %.5f %.5f %.5f) plain (%.5f %.5f %.5f %.5f)\n", fmts[i].n, stage ? "compute" : "fragment", mode_name[m], xbad, W * H, wbad, W * H,
                   rf[0], rf[1], rf[2], rf[3], pl[0], pl[1], pl[2], pl[3]);
            if (xbad) { printf("  first plain mismatch: ref %.6f uni %.6f\n", x0, y0); problems++; }
            if (wbad) problems++;
        }
        float rf[W * H * 4], tri[W * H * 4]; run(YES, @"ref_fn", t, a, near, cs, 0, rf); run(YES, @"uni_fn", t, a, game, cs, 0, tri);
        int d = 0; for (int p = 0; p < W * H; p++) d += rf[p * 4] != tri[p * 4];
        printf("%-22s (information) the game's trilinear/repeat sampler left in place: %d/%d pixels differ from today's point read\n", fmts[i].n, d, W * H);
    }
    printf("%s: %ld comparisons, %d problems\n", problems ? "NOT PASSED" : "PASSED", total, problems);
    return problems ? 1 : 0; } }
