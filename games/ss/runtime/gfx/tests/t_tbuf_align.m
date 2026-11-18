// A texture buffer view at a buffer offset below Metal's 16-byte alignment: nvn_mtl.m tbuf_shadow binds a view of a copy of its bytes at an aligned offset instead. Checks, for every
// 4-byte offset in the first 64 bytes and three formats, that the view of the copy reads the elements at the offset, bound directly and through an argument buffer; and reports how
// a view created at the offset itself reads on this GPU (an M4 Pro reads from the 16-byte boundary below: R32Float at offset 4 gives element 0 where element 1 is expected)
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

static const char *k_msl =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct AB { texture_buffer<uint> t [[id(0)]]; };\n"
    "kernel void kd(texture_buffer<uint> t [[texture(0)]], device uint4 *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = t.read(i); }\n"
    "kernel void ka(constant AB &a [[buffer(1)]], device uint4 *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = a.t.read(i); }\n";

static int g_n, g_fail;
static void check(const char *what, int ok, const char *detail) { g_n++; if (!ok) g_fail++; printf("%-70s %s %s\n", what, ok ? "✓" : "✗", ok ? "" : detail); }

enum { N = 64 };
static id<MTLDevice> dev; static id<MTLCommandQueue> q; static id<MTLComputePipelineState> pd, pa; static id<MTLArgumentEncoder> ae;
// Reads N elements of t into o (uint4 each), directly (argbuf 0) or through an argument buffer (argbuf 1)
static void readback(id<MTLTexture> t, int argbuf, uint32_t *o) {
    id<MTLBuffer> ob = [dev newBufferWithLength:N * 16 options:MTLResourceStorageModeShared], ab = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    if (argbuf) { [ae setArgumentBuffer:ab offset:0]; [ae setTexture:t atIndex:0]; [ce setComputePipelineState:pa]; [ce setBuffer:ab offset:0 atIndex:1]; [ce useResource:t usage:MTLResourceUsageRead]; }
    else { [ce setComputePipelineState:pd]; [ce setTexture:t atIndex:0]; }
    [ce setBuffer:ob offset:0 atIndex:0];
    [ce dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    memcpy(o, ob.contents, N * 16);
}

int main(void) { @autoreleasepool {
    dev = MTLCreateSystemDefaultDevice(); NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(k_msl) options:nil error:&err];
    if (!lib) { printf("MSL compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
    pd = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"kd"] error:&err];
    id<MTLFunction> fa = [lib newFunctionWithName:@"ka"]; pa = [dev newComputePipelineStateWithFunction:fa error:&err]; ae = [fa newArgumentEncoderWithBufferIndex:1];
    q = [dev newCommandQueue];
    const struct { MTLPixelFormat pf; uint32_t bpb, comps; const char *name; } fm[] = {
        { MTLPixelFormatR32Uint, 4, 1, "R32Uint" }, { MTLPixelFormatRG32Uint, 8, 2, "RG32Uint" }, { MTLPixelFormatRG16Uint, 4, 2, "RG16Uint" } };
    size_t len = 64 + N * 16 + 64;
    id<MTLBuffer> pool = [dev newBufferWithLength:len options:MTLResourceStorageModeShared], stage = [dev newBufferWithLength:len options:MTLResourceStorageModeShared];
    uint16_t *w = pool.contents; for (size_t i = 0; i < len / 2; i++) w[i] = (uint16_t)(i * 7 + 1);   // every 16-bit word distinct
    int direct_off = 0, direct_n = 0; char note[160] = "";
    for (int f = 0; f < 3; f++) for (uint32_t off = 0; off < 64; off += 4) {
        if (off % fm[f].bpb) continue;
        NSUInteger row = (N * fm[f].bpb + 15) & ~15u;
        MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:fm[f].pf width:N resourceOptions:pool.resourceOptions usage:MTLTextureUsageShaderRead];
        memcpy(stage.contents, (uint8_t *)pool.contents + off, row);   // what tbuf_shadow does: the bytes from the offset, at an aligned offset of a staging block (0 here)
        id<MTLTexture> copy = [stage newTextureWithDescriptor:d offset:0 bytesPerRow:row], at = [pool newTextureWithDescriptor:d offset:off bytesPerRow:row];
        for (int argbuf = 0; argbuf < 2; argbuf++) {
            uint32_t got[N * 4], raw[N * 4]; int bad = -1, bad_at = -1;
            readback(copy, argbuf, got);
            for (int i = 0; i < N && bad < 0; i++) for (uint32_t c = 0; c < fm[f].comps; c++) {
                const uint8_t *e = (const uint8_t *)pool.contents + off + (size_t)i * fm[f].bpb; uint32_t want = fm[f].bpb / fm[f].comps == 4 ? ((const uint32_t *)e)[c] : ((const uint16_t *)e)[c];
                if (got[4 * i + c] != want) { bad = i; break; }
            }
            char what[96], det[96]; snprintf(what, sizeof what, "%s offset %2u %s: the aligned copy reads the elements at the offset", fm[f].name, off, argbuf ? "argument buffer" : "direct");
            snprintf(det, sizeof det, "element %d wrong", bad); check(what, bad < 0, det);
            if (off & 15) {
                readback(at, argbuf, raw);
                for (int i = 0; i < N && bad_at < 0; i++) if (memcmp(&raw[4 * i], &got[4 * i], 4 * fm[f].comps)) bad_at = i;
                direct_n++; if (bad_at >= 0) { if (!direct_off++) snprintf(note, sizeof note, "first: %s offset %u, element %d", fm[f].name, off, bad_at); }
            }
        }
    }
    printf("views created at an offset below the 16-byte alignment that read wrong on %s: %d of %d (%s)\n", dev.name.UTF8String, direct_off, direct_n, direct_off ? note : "none");
    printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}}
