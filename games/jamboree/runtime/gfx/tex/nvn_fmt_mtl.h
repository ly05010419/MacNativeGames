// NVNformat → Metal pixel format + component remapping. Pure C, no dependency on Metal headers (values taken from the SDK's MTLPixelFormat.h / MTLTexture.h, iOS 26.5 SDK).
#pragma once
#include <stdint.h>

// MTLTextureSwizzle values
enum { NVN_SWZ_ZERO = 0, NVN_SWZ_ONE = 1, NVN_SWZ_R = 2, NVN_SWZ_G = 3, NVN_SWZ_B = 4, NVN_SWZ_A = 5 };

// Returns: 1 = exact match (data uploaded as is);
//          2 = substitute (precision / bit order / read mode differs: upload needs conversion or the shader must read differently; per-format handling is in the table comments in nvn_fmt_mtl.c);
//          0 = no Metal counterpart (*mtl is set to 0; handling is in the table comments, mainly expanding 3-component formats to 4 components on upload).
// swz gives, for each of the four output components r/g/b/a, which source component to take (MTLTextureSwizzleChannels order); may be NULL.
int nvn_fmt_to_mtl(int nvn_format, uint32_t *mtl_pixel_format, uint8_t swz[4]);
