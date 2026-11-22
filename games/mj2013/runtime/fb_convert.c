/* See fb_convert.h. The GOB order is Tegra's (Switch 1): runs of 16 bytes are contiguous inside a GOB, so a row is
   copied 16 bytes at a time from where nvn_bl_offset puts each run. */
#include "fb_convert.h"
#include "../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"
#include <string.h>

unsigned fb_bytes_per_pixel(uint32_t format) {
    switch (format) {
    case FB_RGBA_8888: case FB_RGBX_8888: case FB_BGRA_8888: return 4;
    case FB_RGB_565: case FB_RGBA_4444: return 2;
    default: return 0;
    }
}
static void pixel(uint32_t format, const uint8_t *s, uint8_t *d) {
    switch (format) {
    case FB_RGBA_8888: memcpy(d, s, 4); return;
    case FB_RGBX_8888: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; return;
    case FB_BGRA_8888: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; return;
    case FB_RGB_565: { uint16_t v = (uint16_t)(s[0] | s[1] << 8); uint8_t r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        d[0] = (uint8_t)(r << 3 | r >> 2); d[1] = (uint8_t)(g << 2 | g >> 4); d[2] = (uint8_t)(b << 3 | b >> 2); d[3] = 255; return; }
    case FB_RGBA_4444: { uint16_t v = (uint16_t)(s[0] | s[1] << 8);   /* Android RGBA_4444: R in the top nibble */
        d[0] = (uint8_t)((v >> 12) * 17); d[1] = (uint8_t)(((v >> 8) & 15) * 17); d[2] = (uint8_t)(((v >> 4) & 15) * 17); d[3] = (uint8_t)((v & 15) * 17); return; }
    }
}
int fb_to_rgba(const uint8_t *src, uint32_t width, uint32_t height, uint32_t pitch, int block_linear, unsigned block_height_log2,
               uint32_t format, uint8_t *dst) {
    unsigned bpp = fb_bytes_per_pixel(format);
    if (!bpp || (uint64_t)width * bpp > pitch) return -1;
    uint8_t row[16384]; if (pitch > sizeof row) return -1;
    uint32_t row_bytes = width * bpp;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *line = src + (uint64_t)y * pitch;
        if (block_linear) {
            for (uint32_t x = 0; x < row_bytes; x += 16) {
                uint32_t n = row_bytes - x < 16 ? row_bytes - x : 16;
                memcpy(row + x, src + nvn_bl_offset(x, y, 0, pitch, height, block_height_log2, 0), n);
            }
            line = row;
        }
        uint8_t *out = dst + (uint64_t)y * width * 4;
        if (format == FB_RGBA_8888) memcpy(out, line, (size_t)width * 4);
        else for (uint32_t x = 0; x < width; x++) pixel(format, line + x * bpp, out + x * 4);
    }
    return 0;
}
