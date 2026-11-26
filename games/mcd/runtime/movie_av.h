/* Buffer-mode host codecs. Configuration bytes are avcC / MPEG-4 ESDS. */
#ifndef MOVIE_AV_H
#define MOVIE_AV_H
#include <stddef.h>
#include <stdint.h>
typedef struct MovieAV MovieAV;
MovieAV *movie_av_create(int type, int width, int height, int rate, int channels,
                         const uint8_t *config, size_t config_size);
void movie_av_destroy(MovieAV *codec);
int movie_av_flush(MovieAV *codec);
/* Returns decoded byte count, zero for a packet with no output, -1 on error. */
int movie_av_decode(MovieAV *codec, const uint8_t *packet, size_t size, int64_t pts,
                    uint8_t *output, size_t capacity);
#endif
