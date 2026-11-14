// H.264 (AVC) decoding for the movie decoder (hle_movie.c), on Apple's VideoToolbox; see hle_movie_avc.c
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct mavc mavc;
mavc *mavc_new(void);
void mavc_free(mavc *a);
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n);   // SPS / PPS (Configure's csd-0 / csd-1), with or without Annex B start codes
void mavc_avcc(mavc *a, const uint8_t *p, size_t n);         // SPS / PPS from an AVCDecoderConfigurationRecord (the MP4 avcC box)
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us);   // one access unit; 0 = decoded (or nothing to decode), -1 = error
// the next picture in presentation order, as NV12 (Y stride w, then interleaved UV at w*h, stride w; a smaller picture is padded with black, a larger one cropped):
// 1 = a picture was written (its time in *pts_us), 0 = none yet; with flush every queued picture comes out (end of stream)
int mavc_take(mavc *a, int flush, uint8_t *nv12, int w, int h, int64_t *pts_us);
int mavc_queued(const mavc *a);
void mavc_set_rgba(mavc *a);   // pictures come out as 4-byte RGBA (stride w * 4) instead of NV12: the native texture mode
void mavc_drop(mavc *a);   // a flush / seek: queued pictures are discarded
