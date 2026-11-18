// NVNformat → Metal 像素格式 + 分量重映射。纯 C，不依赖 Metal 头（数值取自 SDK MTLPixelFormat.h / MTLTexture.h，iOS 26.5 SDK）。
// NVNformat → Metal pixel format + component remapping. Pure C, no dependency on Metal headers (values taken from the SDK's MTLPixelFormat.h / MTLTexture.h, iOS 26.5 SDK).
#pragma once
#include <stdint.h>

// MTLTextureSwizzle 数值
// MTLTextureSwizzle values
enum { NVN_SWZ_ZERO = 0, NVN_SWZ_ONE = 1, NVN_SWZ_R = 2, NVN_SWZ_G = 3, NVN_SWZ_B = 4, NVN_SWZ_A = 5 };

// 返回：1 = 精确对应（数据原样上传）；
//       2 = 替代（精度 / 位序 / 读法不同：上传需转换或着色器需改读法，每个格式的办法见 nvn_fmt_mtl.c 表内注释）；
//       0 = Metal 无对应（*mtl 写 0；处理办法见表内注释，主要是 3 分量格式上传时扩成 4 分量）。
// swz 为 r/g/b/a 四个输出分量各取哪个源分量（MTLTextureSwizzleChannels 顺序）；可为 NULL。
// Returns: 1 = exact match (data uploaded as is);
//          2 = substitute (precision / bit order / read mode differs: upload needs conversion or the shader must read differently; per-format handling is in the table comments in nvn_fmt_mtl.c);
//          0 = no Metal counterpart (*mtl is set to 0; handling is in the table comments, mainly expanding 3-component formats to 4 components on upload).
// swz gives, for each of the four output components r/g/b/a, which source component to take (MTLTextureSwizzleChannels order); may be NULL.
int nvn_fmt_to_mtl(int nvn_format, uint32_t *mtl_pixel_format, uint8_t swz[4]);
