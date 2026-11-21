#import "nvn_deswizzle_gpu.h"
#import "../nvn/host_msl.h"

static id<MTLComputePipelineState> g_nvn_deswz_pipeline, g_nvn_deswz_direct;

// The kernels deswz and deswz_direct: dm/gfx/msl/deswz.metal

int nvn_deswz_gpu_init(id<MTLDevice> dev) {
    if (g_nvn_deswz_pipeline) return 0;
    if (!dev) return -1;
    NSError *error = nil;
    id<MTLLibrary> library = host_msl_lib(dev, nil, "deswz", &error);   // compiled when the host is built (dm/gfx/msl/deswz.metal)
    if (!library) return -1;
    id<MTLFunction> function = [library newFunctionWithName:@"deswz"];
    if (!function) return -1;
    g_nvn_deswz_pipeline = [dev newComputePipelineStateWithFunction:function error:&error];
    id<MTLFunction> direct = [library newFunctionWithName:@"deswz_direct"];
    if (direct) g_nvn_deswz_direct = [dev newComputePipelineStateWithFunction:direct error:&error];
    return g_nvn_deswz_pipeline ? 0 : -1;
}

int nvn_deswz_gpu_direct_supported(const nvn_texdesc *d) {
    // Prefer retained compression for large images. This path targets many
    // small array layers where per-face dispatch/copy overhead dominates.
    return g_nvn_deswz_direct && d && d->format == 0x29 && (d->target == 4 || d->target == 9) &&
        !(d->flags & 0x38) && d->samples <= 1 && d->width && d->height && d->width <= 64 && d->height <= 64 && d->depth >= 32;
}
int nvn_deswz_gpu_direct_m4(id<MTL4ComputeCommandEncoder> enc, id<MTL4ArgumentTable> table,
                            id<MTLBuffer> params, size_t params_off, id<MTLBuffer> src, uint64_t src_off,
                            const nvn_texdesc *d, unsigned level, id<MTLTexture> view) {
    if (!nvn_deswz_gpu_direct_supported(d) || !enc || !table || !params || !src || !view ||
        view.pixelFormat != MTLPixelFormatRGBA16Uint || view.textureType != MTLTextureType2DArray ||
        level >= view.mipmapLevelCount || (src_off & 7) || params_off + 64 > params.length) return -1;
    nvn_texlayout layout; nvn_tex_layout(d, &layout);
    uint32_t row,rows,slices,bpb;uint64_t base;unsigned th,td;
    if (nvn_deswizzle_geom(d,level,0,&row,&rows,&slices,&base,&th,&td,&bpb) || bpb != 8 || slices != 1 || td ||
        layout.size > src.length || src_off > src.length-layout.size || layout.layers != view.arrayLength) return -1;
    struct {uint64_t src_off,base,layer_stride;uint32_t width,height,layers,level,th,gobs_x,gob2;} p = {
        src_off,base,layout.layer_size,MAX(d->width>>level,1u),MAX(d->height>>level,1u),layout.layers,level,th,MAX(nvn_tex_level_gobs(d,level),(row+63u)/64u),(uint32_t)nvn_gob_switch2};
    memcpy((uint8_t*)params.contents+params_off,&p,sizeof p);
    [enc setArgumentTable:table];[enc setComputePipelineState:g_nvn_deswz_direct];
    [table setAddress:src.gpuAddress atIndex:0];[table setAddress:params.gpuAddress+params_off atIndex:2];[table setTexture:view.gpuResourceID atIndex:0];
    [enc dispatchThreadgroups:MTLSizeMake((p.width+7)/8,(p.height+3)/4,p.layers) threadsPerThreadgroup:MTLSizeMake(8,4,1)];
    return 0;
}

int nvn_deswz_gpu_encode(id<MTLComputeCommandEncoder> enc, id<MTLBuffer> src, uint64_t src_off,
                         const nvn_texdesc *d, unsigned level, unsigned layer,
                         id<MTLBuffer> dst, uint64_t dst_off, size_t dst_pitch) {
    if (!g_nvn_deswz_pipeline || !enc || !src || !dst || !d) return -1;
    uint32_t row_bytes, rows, slices, bpb;
    uint64_t base;
    unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || dst_pitch < row_bytes) return -1;
    struct { uint64_t src_off, dst_off, base, pitch; uint32_t row_bytes, rows, slices, th, td, gobs_x, gob2; } p = {
        src_off, dst_off, base, dst_pitch, row_bytes, rows, slices, th, td, MAX(nvn_tex_level_gobs(d, level), (row_bytes + 63u) >> 6), (uint32_t)nvn_gob_switch2   // GOB columns as laid out (nvn_tex_level_gobs)
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
    struct { uint64_t src_off, dst_off, base, pitch; uint32_t row_bytes, rows, slices, th, td, gobs_x, gob2; } p = {
        src_off, dst_off, base, dst_pitch, row_bytes, rows, slices, th, td, MAX(nvn_tex_level_gobs(d, level), (row_bytes + 63u) >> 6), (uint32_t)nvn_gob_switch2   // GOB columns as laid out (nvn_tex_level_gobs)
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
