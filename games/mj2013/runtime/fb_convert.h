/* The picture the game queues (libnx framebuffer.c: block-linear, 16-GOB blocks, one of Android's pixel formats) as
   RGBA8 rows for the window and screenshots. Block-linear addressing is the shared nvn_bl_offset (gfx/tex). */
#pragma once
#include <stdint.h>
#include <stddef.h>
enum { FB_RGBA_8888 = 1, FB_RGBX_8888 = 2, FB_RGB_565 = 4, FB_BGRA_8888 = 5, FB_RGBA_4444 = 7 };
/* Bytes per pixel of a pixel format, 0 when not supported. */
unsigned fb_bytes_per_pixel(uint32_t format);
/* src: the queued buffer (pitch bytes per row, block_height_log2, or linear when block_linear is 0); dst: width*height
   RGBA8 pixels, alpha 255 where the format has none. Returns 0, or -1 for a format it does not know. */
int fb_to_rgba(const uint8_t *src, uint32_t width, uint32_t height, uint32_t pitch, int block_linear, unsigned block_height_log2,
               uint32_t format, uint8_t *dst);
