/* Adapted platform AV1 decoder from seedds/nxnative ee9b7765, MIT. Integrated into our NS2 codec ownership/dispatch. */
// AV1 decoding on VideoToolbox or dav1d (hle_movie_av1.c), for the nn::codec decoders of the Switch 2's SDK (hle_codec_video.c)
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct mav1 mav1;
// What one decode gave: an NV12 picture of w x h in the decoder's memory, valid until the next call, its matrix as vpx_color_space_t's values (BT.601 = 1, BT.709 = 2, BT.2020 = 5)
typedef struct { const uint8_t *y, *uv; size_t ystride, uvstride; int w, h, matrix, full_range; } mav1_picture;

mav1 *mav1_new(void);
void mav1_free(mav1 *a);
void mav1_reset(mav1 *a);   // a new stream starts at the next packet with a sequence header
// One temporal unit (OBUs with their sizes). 1 and *out set: a picture; 0: nothing to show (no sequence header seen yet, a unit without a shown frame); -1: the decoder refused it
int mav1_decode(mav1 *a, const uint8_t *data, size_t n, mav1_picture *out);
/* Working ahead (dav1d without hardware AV1, DM_AV1_FRAME_DELAY > 1): mav1_send takes a temporal unit with its timestamp,
   mav1_receive gives the pictures ready so far one by one (1, with the picture's own timestamp; 0: none now), valid until
   the next call; mav1_drain at end of stream makes every picture still in the decoder ready. Up to mav1_pipeline_depth()
   pictures can come out of one send or drain. */
int mav1_pipelined(const mav1 *a);
int mav1_pipeline_depth(void);
int mav1_send(mav1 *a, const uint8_t *data, size_t n, int64_t ts);
void mav1_drain(mav1 *a);
int mav1_drained(const mav1 *a);
int mav1_receive(mav1 *a, mav1_picture *out, int64_t *ts);
const char *mav1_backend(const mav1 *a);   // "VideoToolbox" (its hardware decoder: Apple M3 and A17 Pro on) or "dav1d"
// Exposed for its test: fills the 4 + n bytes of an av1C record from a sequence header OBU (with its header and size); 0 when the OBU cannot be read
int mav1_av1c(const uint8_t *obu, size_t n, uint8_t c[4], int *w, int *h);
