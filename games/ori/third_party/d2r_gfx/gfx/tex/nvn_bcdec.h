// 不支持 BC 压缩格式的显卡（A15 / A16 等，苹果 GPU 族 Apple8 及以下）：BC 贴图在 CPU 上解压成不压缩格式。
// 游戏贴图里的 BC3 由老机型补充包离线换成 ASTC（dm/tools/astc_pack.py）；剩下的 BC（精灵图里的 BC3、少量 BC1 / BC4 / BC5 / BC6H）走这里。
// GPUs without BC compressed format support (A15 / A16 etc., Apple GPU family Apple8 and below): BC textures are decompressed on the CPU into uncompressed formats.
// BC3 in game textures is converted offline to ASTC by the Legacy Device Pack (dm/tools/astc_pack.py); the remaining BC (BC3 in sprites, a few BC1 / BC4 / BC5 / BC6H) goes through here.
#pragma once
#include <stddef.h>
#include <stdint.h>

// NVN 格式是 BC → 返回 1，*mtl_pf = 解压后的 Metal 像素格式、*bpp = 解压后每像素字节；不是 BC 返回 0
// NVN format is BC → returns 1, *mtl_pf = Metal pixel format after decompression, *bpp = bytes per pixel after decompression; returns 0 if not BC
int nvn_bc_unpacked(uint32_t nvn_fmt, uint32_t *mtl_pf, int *bpp);
// 解一片 w×h 像素：src 按块行排（每块行 pitch 字节），dst 每像素行 dpitch 字节。成功 0
// Decode a w×h pixel region: src is laid out by block rows (pitch bytes per block row), dst has dpitch bytes per pixel row. Returns 0 on success
int nvn_bc_decode(uint32_t nvn_fmt, const uint8_t *src, size_t pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dpitch);
