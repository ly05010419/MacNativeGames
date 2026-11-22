/* The framebuffer conversion (runtime/fb_convert.c) against libnx's own swizzler: a picture made linear, swizzled the way
   libnx's framebuffer.c does (_convertToBlocklinear / _convertGobTo16Bx2, copied below from switchbrew/libnx, ISC), must
   come back unchanged; the pixel formats a libnx framebuffer can have convert to RGBA8. */
#include "../runtime/fb_convert.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned __int128 u128;
static void gob16bx2(uint8_t *outgob, const uint8_t *ingob, uint32_t stride) {
    for (uint32_t i = 0; i < 32; i++) {
        const uint32_t y = ((i >> 1) & 0x06) | (i & 0x01), x = ((i << 3) & 0x10) | ((i << 1) & 0x20);
        memcpy(outgob, ingob + y * stride + x, 16); outgob += 16;
    }
}
static void to_blocklinear(uint8_t *out, const uint8_t *in, uint32_t stride, uint32_t height, uint32_t bh_log2) {
    const uint32_t bh_gobs = 1u << bh_log2, bh_px = 8u << bh_log2, wb = stride >> 6, hb = (height + bh_px - 1) >> (3 + bh_log2);
    uint8_t *g = out;
    for (uint32_t by = 0; by < hb; by++) for (uint32_t bx = 0; bx < wb; bx++) for (uint32_t gy = 0; gy < bh_gobs; gy++) {
        const uint32_t x = bx * 64, y = by * bh_px + gy * 8;
        if (y < height) gob16bx2(g, in + y * stride + x, stride);
        g += 512;
    }
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)
static void roundtrip(uint32_t w, uint32_t h, uint32_t bh_log2) {
    uint32_t pitch = (w * 4 + 63) & ~63u, bh_px = 8u << bh_log2, ha = (h + bh_px - 1) & ~(bh_px - 1);
    uint8_t *lin = calloc(pitch, ha), *bl = calloc(pitch, ha), *out = malloc((size_t)w * h * 4);
    srand(w * 31 + h);
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < pitch; x++) lin[y * pitch + x] = (uint8_t)rand();
    to_blocklinear(bl, lin, pitch, h, bh_log2);
    CHECK(fb_to_rgba(bl, w, h, pitch, 1, bh_log2, FB_RGBA_8888, out) == 0, "convert %ux%u", w, h);
    int same = 1; for (uint32_t y = 0; y < h && same; y++) same = !memcmp(out + (size_t)y * w * 4, lin + (size_t)y * pitch, (size_t)w * 4);
    CHECK(same, "block-linear %ux%u (block height 2^%u) does not come back as libnx wrote it", w, h, bh_log2);
    CHECK(fb_to_rgba(lin, w, h, pitch, 0, 0, FB_RGBA_8888, out) == 0 && !memcmp(out, lin, (size_t)w * 4), "linear rows");
    free(lin); free(bl); free(out);
}
int main(void) {
    roundtrip(1280, 720, 4);   /* the game's framebuffer */
    roundtrip(1920, 1080, 4);
    roundtrip(800, 600, 4);
    roundtrip(100, 37, 2);
    uint8_t out[4];
    uint8_t rgbx[4] = {10, 20, 30, 0}; fb_to_rgba(rgbx, 1, 1, 64, 0, 0, FB_RGBX_8888, out); CHECK(out[0] == 10 && out[1] == 20 && out[2] == 30 && out[3] == 255, "RGBX");
    uint8_t bgra[4] = {10, 20, 30, 40}; fb_to_rgba(bgra, 1, 1, 64, 0, 0, FB_BGRA_8888, out); CHECK(out[0] == 30 && out[1] == 20 && out[2] == 10 && out[3] == 40, "BGRA");
    uint8_t rgb565[2] = {0x1f, 0xf8}; fb_to_rgba(rgb565, 1, 1, 64, 0, 0, FB_RGB_565, out); CHECK(out[0] == 255 && out[1] == 0 && out[2] == 255 && out[3] == 255, "RGB565 %u %u %u", out[0], out[1], out[2]);
    CHECK(fb_to_rgba(rgbx, 1, 1, 64, 0, 0, 3, out) == -1, "RGB_888 is refused (libnx: does not work)");
    CHECK(fb_bytes_per_pixel(FB_RGBA_4444) == 2 && fb_bytes_per_pixel(FB_RGBA_8888) == 4, "bytes per pixel");
    printf(fails ? "fb FAILED\n" : "fb ok\n"); return fails != 0;
}
