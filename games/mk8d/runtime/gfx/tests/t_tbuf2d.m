// A texture buffer read through a slot the translator made a 2D read of wrapped 1D fetches (InstEmitTexture.cs nxnative: x & 0x3FFF, x >> 14): nvn_mtl.m tbuf_as_2d binds a linear
// 2D texture of 16384-texel rows (nvn_tbuf_rows) over the same buffer bytes. Checks, for element counts from 1 to 1,048,576, pool offsets at the linear-texture alignment and two
// formats, that every element read through the alias at the wrapped address equals the texture_buffer view's read and the memory itself; and that a one-row 2D texture (how the
// host creates a 1D texture) reads the same at x & 0x3FFF, x >> 14 as at (x, 0)
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
#include "../nvn/nvn_tex.h"

static const char *k_msl =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "kernel void ku(texture_buffer<uint> tb [[texture(0)]], texture2d<uint> t2 [[texture(1)]], device uint4 *o [[buffer(0)]], constant uint &n [[buffer(1)]], uint i [[thread_position_in_grid]])\n"
    "{ if (i >= n) return; int x = int(i); o[2 * i] = tb.read(i); o[2 * i + 1] = t2.read(uint2(int2(x & 16383, int(uint(x) >> 14u))), 0); }\n"
    "kernel void kf(texture_buffer<float> tb [[texture(0)]], texture2d<float> t2 [[texture(1)]], device uint4 *o [[buffer(0)]], constant uint &n [[buffer(1)]], uint i [[thread_position_in_grid]])\n"
    "{ if (i >= n) return; int x = int(i); o[2 * i] = as_type<uint4>(tb.read(i)); o[2 * i + 1] = as_type<uint4>(t2.read(uint2(int2(x & 16383, int(uint(x) >> 14u))), 0)); }\n"
    "kernel void k1(texture2d<uint> t2 [[texture(1)]], device uint4 *o [[buffer(0)]], constant uint &n [[buffer(1)]], uint i [[thread_position_in_grid]])\n"
    "{ if (i >= n) return; int x = int(i); o[2 * i] = t2.read(uint2(i, 0), 0); o[2 * i + 1] = t2.read(uint2(int2(x & 16383, int(uint(x) >> 14u))), 0); }\n";

static int g_n, g_fail;
static void check(const char *what, int ok, const char *detail) { g_n++; if (!ok) g_fail++; printf("%-70s %s %s\n", what, ok ? "✓" : "✗", ok ? "" : detail); }

int main(void) {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(k_msl) options:nil error:&err];
    if (!lib) { printf("MSL compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLComputePipelineState> pu = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"ku"] error:&err], pf = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"kf"] error:&err],
                                p1 = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"k1"] error:&err];
    id<MTLCommandQueue> q = [dev newCommandQueue];
    const uint32_t counts[] = { 1, 1000, 16384, 16385, 40000, 1048576 };
    const struct { MTLPixelFormat pf; uint32_t bpb; const char *name; } fm[] = { { MTLPixelFormatR32Uint, 4, "R32Uint" }, { MTLPixelFormatRGBA32Float, 16, "RGBA32Float" } };
    for (unsigned fi = 0; fi < 2; fi++) for (unsigned ci = 0; ci < sizeof counts / sizeof *counts; ci++) for (int oi = 0; oi < 3; oi++) {
        uint32_t n = counts[ci], bpb = fm[fi].bpb, w, h, row; NSUInteger al = [dev minimumLinearTextureAlignmentForPixelFormat:fm[fi].pf], off = (NSUInteger)oi * al;
        if (!nvn_tbuf_rows(n, bpb, (uint32_t)al, &w, &h, &row)) { check("rows", 0, "nvn_tbuf_rows refused"); continue; }
        size_t len = off + (size_t)row * h + 4096;   // as a memory pool: more bytes follow the buffer texture
        id<MTLBuffer> b = [dev newBufferWithLength:len options:MTLResourceStorageModeShared];
        uint32_t *words = (uint32_t *)((uint8_t *)b.contents + off); for (size_t i = 0; i < (size_t)n * bpb / 4; i++) words[i] = (uint32_t)(i * 2654435761u ^ (i >> 7));
        MTLTextureDescriptor *td = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:fm[fi].pf width:n resourceOptions:b.resourceOptions usage:MTLTextureUsageShaderRead];
        id<MTLTexture> tb = [b newTextureWithDescriptor:td offset:off bytesPerRow:((size_t)n * bpb + al - 1) / al * al];
        MTLTextureDescriptor *d2 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fm[fi].pf width:w height:h mipmapped:NO]; d2.resourceOptions = b.resourceOptions; d2.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t2 = [b newTextureWithDescriptor:d2 offset:off bytesPerRow:row];
        char what[128]; snprintf(what, sizeof what, "%s %u elements at offset %lu: %ux%u rows of %u bytes", fm[fi].name, n, (unsigned long)off, w, h, row);
        if (!tb || !t2) { check(what, 0, "texture not created"); continue; }
        id<MTLBuffer> o = [dev newBufferWithLength:(size_t)n * 32 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:fi ? pf : pu]; [ce setTexture:tb atIndex:0]; [ce setTexture:t2 atIndex:1]; [ce setBuffer:o offset:0 atIndex:0]; [ce setBytes:&n length:4 atIndex:1];
        [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const uint32_t *r = o.contents; int bad = -1;
        for (uint32_t i = 0; i < n && bad < 0; i++) for (int c = 0; c < 4; c++) {
            uint32_t mem = c < (int)(bpb / 4) ? words[i * (bpb / 4) + c] : (c == 3 ? (fi ? 0x3f800000u : 1u) : 0u);   // missing components of a one-channel read: 0, 0, 1
            if (r[8 * i + c] != r[8 * i + 4 + c] || r[8 * i + c] != mem) { bad = (int)i; break; } }
        char det[128] = ""; if (bad >= 0) snprintf(det, sizeof det, "element %d: buffer view %#x, rows %#x, memory %#x", bad, r[8 * bad], r[8 * bad + 4], words[bad * (bpb / 4)]);
        check(what, bad < 0, det);
    }
    {   // a 1D texture as the host makes it (one row at most 16384 wide) reads the same at the wrapped address
        uint32_t n = 16384; MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:n height:1 mipmapped:NO]; d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [dev newTextureWithDescriptor:d]; uint32_t *v = malloc(n * 4); for (uint32_t i = 0; i < n; i++) v[i] = i * 7919u + 3;
        [t replaceRegion:MTLRegionMake2D(0, 0, n, 1) mipmapLevel:0 withBytes:v bytesPerRow:n * 4];
        id<MTLBuffer> o = [dev newBufferWithLength:(size_t)n * 32 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:p1]; [ce setTexture:t atIndex:1]; [ce setBuffer:o offset:0 atIndex:0]; [ce setBytes:&n length:4 atIndex:1];
        [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)]; [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const uint32_t *r = o.contents; int bad = -1; for (uint32_t i = 0; i < n && bad < 0; i++) if (r[8 * i] != r[8 * i + 4] || r[8 * i] != v[i]) bad = (int)i;
        check("a 16384-wide one-row 2D texture (a 1D texture) reads the same at the wrapped address", bad < 0, "differs");
        free(v);
    }
    printf("== %d checks, %d failed ==\n", g_n, g_fail);
    return g_fail != 0;
}
