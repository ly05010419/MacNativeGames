#pragma once
#include <stddef.h>
#include <stdint.h>
#import <Metal/Metal.h>
#include "nvn_deswizzle.h"

int nvn_deswz_gpu_init(id<MTLDevice> dev);
int nvn_deswz_gpu_encode(id<MTLComputeCommandEncoder> enc, id<MTLBuffer> src, uint64_t src_off,
                         const nvn_texdesc *d, unsigned level, unsigned layer,
                         id<MTLBuffer> dst, uint64_t dst_off, size_t dst_pitch);
int nvn_deswz_gpu_encode_m4(id<MTL4ComputeCommandEncoder> enc, id<MTL4ArgumentTable> table,
                            id<MTLBuffer> params, size_t params_off, id<MTLBuffer> src, uint64_t src_off,
                            const nvn_texdesc *d, unsigned level, unsigned layer,
                            id<MTLBuffer> dst, uint64_t dst_off, size_t dst_pitch);

/* Block-linear pool <-> linear CPU texel region, in format block/byte coordinates.
   Return -1 before encoding for an unsupported/out-of-bounds request. The caller
   must wait for GPU completion before reading the linear buffer or reusing the pool. */
int nvn_texels_gpu_encode(id<MTLComputeCommandEncoder> enc, id<MTLBuffer> pool, uint64_t pool_off,
                          const nvn_texdesc *d, unsigned level, unsigned layer,
                          uint32_t x, uint32_t y, uint32_t z, uint32_t width_bytes, uint32_t height_blocks, uint32_t depth,
                          id<MTLBuffer> linear, uint64_t linear_off, size_t pitch, size_t image_pitch, int to_linear);
