#import "nvn_deswizzle_gpu.h"

static id<MTLComputePipelineState> g_nvn_deswz_pipeline;

static const char *const k_nvn_deswz_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct P { ulong src_off, dst_off, base, pitch; uint row_bytes, rows, slices, th, td, gobs_x; };\n"
    "kernel void deswz(device const uchar *src [[buffer(0)]], device uchar *dst [[buffer(1)]], constant P& p [[buffer(2)]], uint3 id [[thread_position_in_grid]]) {\n"
    " uint bh = 1u << p.th, bd = 1u << p.td, gobs_x = p.gobs_x, blocks_y = (p.rows + (8u * bh) - 1u) >> (3u + p.th);\n"
    " uint tid = id.x & 31u, gob_x = id.x >> 5, gob_y = id.y, z = id.z; uint xseg = ((tid >> 4) << 1) | ((tid >> 1) & 1u), y = (gob_y << 3) | (((tid >> 2) & 3u) << 1) | (tid & 1u), x = (gob_x << 6) | (xseg << 4);\n"
    " if (x >= p.row_bytes || y >= p.rows || z >= p.slices) return;\n"
    " uint n = min(16u, p.row_bytes - x); ulong block = 512ul * bh * bd; ulong gob_in_block = ((ulong)(z & (bd - 1u)) << p.th) + (gob_y & (bh - 1u)); ulong so = p.src_off + p.base + ((ulong)(z >> p.td) * gobs_x * blocks_y + (gob_y >> p.th) * gobs_x + gob_x) * block + gob_in_block * 512ul + (ulong)tid * 16ul; ulong doff = p.dst_off + ((ulong)z * p.rows + y) * p.pitch + x;\n"
    " if (n == 16u && ((so | doff) & 15ul) == 0ul) *(device uint4 *)(dst + doff) = *(device const uint4 *)(src + so);\n"
    " else for (uint i = 0; i < n; i++) dst[doff + i] = src[so + i];\n"
    "}\n";

int nvn_deswz_gpu_init(id<MTLDevice> dev) {
    if (g_nvn_deswz_pipeline) return 0;
    if (!dev) return -1;
    NSError *error = nil;
    id<MTLLibrary> library = [dev newLibraryWithSource:@(k_nvn_deswz_msl) options:nil error:&error];
    if (!library) return -1;
    id<MTLFunction> function = [library newFunctionWithName:@"deswz"];
    if (!function) return -1;
    g_nvn_deswz_pipeline = [dev newComputePipelineStateWithFunction:function error:&error];
    return g_nvn_deswz_pipeline ? 0 : -1;
}

int nvn_deswz_gpu_encode(id<MTLComputeCommandEncoder> enc, id<MTLBuffer> src, uint64_t src_off,
                         const nvn_texdesc *d, unsigned level, unsigned layer,
                         id<MTLBuffer> dst, uint64_t dst_off, size_t dst_pitch) {
    if (!g_nvn_deswz_pipeline || !enc || !src || !dst || !d) return -1;
    uint32_t row_bytes, rows, slices, bpb;
    uint64_t base;
    unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || dst_pitch < row_bytes) return -1;
    struct { uint64_t src_off, dst_off, base, pitch; uint32_t row_bytes, rows, slices, th, td, gobs_x; } p = {
        src_off, dst_off, base, dst_pitch, row_bytes, rows, slices, th, td, MAX(nvn_tex_level_gobs(d, level), (row_bytes + 63u) >> 6)   // GOB columns as laid out (nvn_tex_level_gobs)
    };
    [enc setComputePipelineState:g_nvn_deswz_pipeline];
    [enc setBuffer:src offset:0 atIndex:0];
    [enc setBuffer:dst offset:0 atIndex:1];
    [enc setBytes:&p length:sizeof p atIndex:2];
    NSUInteger gobs_x = (row_bytes + 63u) >> 6;
    NSUInteger gobs_y = (rows + 7u) >> 3;
    [enc dispatchThreadgroups:MTLSizeMake(gobs_x, gobs_y, slices) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    return 0;
}

int nvn_deswz_gpu_encode_m4(id<MTL4ComputeCommandEncoder> enc, id<MTL4ArgumentTable> table,
                            id<MTLBuffer> params, size_t params_off, id<MTLBuffer> src, uint64_t src_off,
                            const nvn_texdesc *d, unsigned level, unsigned layer,
                            id<MTLBuffer> dst, uint64_t dst_off, size_t dst_pitch) {
    if (!g_nvn_deswz_pipeline || !enc || !table || !params || !src || !dst || !d) return -1;
    uint32_t row_bytes, rows, slices, bpb;
    uint64_t base;
    unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || dst_pitch < row_bytes) return -1;
    struct { uint64_t src_off, dst_off, base, pitch; uint32_t row_bytes, rows, slices, th, td, gobs_x; } p = {
        src_off, dst_off, base, dst_pitch, row_bytes, rows, slices, th, td, MAX(nvn_tex_level_gobs(d, level), (row_bytes + 63u) >> 6)   // GOB columns as laid out (nvn_tex_level_gobs)
    };
    memcpy((uint8_t *)params.contents + params_off, &p, sizeof p);
    [enc setArgumentTable:table];
    [enc setComputePipelineState:g_nvn_deswz_pipeline];
    [table setAddress:src.gpuAddress atIndex:0];
    [table setAddress:dst.gpuAddress atIndex:1];
    [table setAddress:params.gpuAddress + params_off atIndex:2];
    NSUInteger gobs_x = (row_bytes + 63u) >> 6;
    NSUInteger gobs_y = (rows + 7u) >> 3;
    [enc dispatchThreadgroups:MTLSizeMake(gobs_x, gobs_y, slices) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    return 0;
}
