// libvpx pictures to the layouts games read (hle_movie.c), shared with the Switch 2 decoder API (hle_codec_video.c)
#pragma once
#include <stdint.h>
#include "vpx/vpx_image.h"

// I420 → NV12 at dst: the Y plane (stride w, h rows), then the interleaved UV plane (stride w, h / 2 rows); a smaller picture is padded with black, a larger one cropped
void md_nv12(const vpx_image_t *im, uint8_t *dst, int w, int h);
// I420 → RGBA (stride 4 w) by the colour space and range the picture declares; a smaller picture is padded with black, a larger one cropped
void md_rgba(const vpx_image_t *im, uint8_t *dst, int w, int h);
