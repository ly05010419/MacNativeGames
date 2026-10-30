// TOTK: BC1 -> ASTC 4x4 block transcoding for GPUs without BC (A15/A16). Most of TOTK's colour textures are BC1;
// decompressed to RGBA8 they take 8x their size, as ASTC 4x4 2x (a quarter of RGBA8).
//  - four-colour blocks (c0 > c1, 89% in TOTK): RGB direct endpoints, 2-bit weights. The BC1 palette (0, 1/3, 2/3, 1)
//    is exactly the 2-bit ASTC weight set, so these blocks are reproduced to within interpolation rounding.
//  - three-colour blocks without transparent texels: endpoints stretched by 1.5x so that (0, 1/2, 1) land on 2-bit
//    weights (exact up to endpoint rounding); if that would leave 0..255, 3-bit weights approximate the midpoint.
//  - three-colour blocks with transparent texels (index 3 of the RGBA formats): dual-plane RGBA with 1-bit weights;
//    alpha is exact (0 or 255), the opaque colours use the best two of the block's colours.
// The GPU upload path runs the same algorithm in MSL (nvn_mtl.m, k_bc1astc_msl); probe-gpu-bc1astc compares both with a
// reference BC1 decode through Metal's own ASTC decoder.
#pragma once
#include <stdint.h>

// rgba_formats: 1 for the BC1 formats with alpha (index 3 of a three-colour block is transparent black), 0 for RGB-only
// formats (opaque black). Writes one 16-byte ASTC 4x4 block.
void nvn_bc1_to_astc(const uint8_t bc1[8], uint8_t astc[16], int rgba_formats);
// Whole image: rows of BC1 blocks (src_pitch bytes apart) into tightly packed ASTC blocks (bw x bh blocks).
void nvn_bc1_to_astc_image(const uint8_t *src, uint64_t src_pitch, uint32_t bw, uint32_t bh, uint8_t *dst, int rgba_formats);
