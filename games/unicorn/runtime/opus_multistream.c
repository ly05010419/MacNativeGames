/* 圣兽之王的音乐使用多流 Opus；由 libopus 解码，保持原有包头和最终状态校验。 */
#include "runtime.h"
#include <opus_multistream.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R_INVALID 0x1fcd1u
typedef struct MultiDecoder {
    const void *guest;
    OpusMSDecoder *decoder;
    int rate, channels, streams, coupled;
    uint64_t elapsed;
    pthread_mutex_t use_lock;
    struct MultiDecoder *next;
} MultiDecoder;
static MultiDecoder *decoders;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

static MultiDecoder *acquire(const void *guest) {
    pthread_mutex_lock(&registry_lock);
    MultiDecoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) pthread_mutex_lock(&d->use_lock);
    pthread_mutex_unlock(&registry_lock);
    if (!d) runtime_fail("unknown multistream Opus decoder");
    return d;
}

static size_t work_size(int rate, int channels, int streams, int coupled, int option) {
    (void)option;
    if ((rate != 8000 && rate != 12000 && rate != 16000 && rate != 24000 && rate != 48000) ||
        channels < 1 || channels > 255 || streams < 1 || coupled < 0 || coupled > streams || streams + coupled > 255)
        return 0;
    int size = opus_multistream_decoder_get_size(streams, coupled);
    /* 原 SDK 把该返回值向上对齐到 4 KiB；游戏用它管理整页工作缓冲。 */
    return size > 0 ? ((size_t)size + 0x1fff) & ~(size_t)0xfff : 0;
}

static unsigned initialize(void *guest, int rate, int channels, int streams, int coupled,
                           const unsigned char *mapping, void *work, size_t size, int option) {
    (void)work;
    size_t needed = work_size(rate, channels, streams, coupled, option);
    if (!guest || !mapping || !needed || size < needed) return R_INVALID;
    int error;
    OpusMSDecoder *decoder = opus_multistream_decoder_create(rate, channels, streams, coupled, mapping, &error);
    if (!decoder || error != OPUS_OK) return R_INVALID;
    pthread_mutex_lock(&registry_lock);
    MultiDecoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) {
        pthread_mutex_lock(&d->use_lock);
        opus_multistream_decoder_destroy(d->decoder);
    } else {
        d = calloc(1, sizeof *d);
        if (!d) runtime_fail("multistream Opus decoder allocation");
        if (pthread_mutex_init(&d->use_lock, NULL)) runtime_fail("multistream Opus decoder mutex");
        pthread_mutex_lock(&d->use_lock);
        d->guest = guest; d->next = decoders; decoders = d;
    }
    d->decoder = decoder; d->rate = rate; d->channels = channels; d->streams = streams; d->coupled = coupled; d->elapsed = 0;
    pthread_mutex_unlock(&registry_lock);
    pthread_mutex_unlock(&d->use_lock);
    fprintf(stderr, "[unicorn-audio] Opus %d Hz, %d channels, %d streams (%d coupled)\n", rate, channels, streams, coupled);
    return 0;
}

static void finalize(void *guest) {
    pthread_mutex_lock(&registry_lock);
    MultiDecoder **slot = &decoders;
    while (*slot && (*slot)->guest != guest) slot = &(*slot)->next;
    MultiDecoder *d = *slot;
    if (!d) runtime_fail("unknown multistream Opus decoder");
    pthread_mutex_lock(&d->use_lock);
    *slot = d->next;
    pthread_mutex_unlock(&registry_lock);
    opus_multistream_decoder_destroy(d->decoder);
    pthread_mutex_unlock(&d->use_lock);
    pthread_mutex_destroy(&d->use_lock);
    free(d);
}

static int sample_rate(const void *guest) { MultiDecoder *d = acquire(guest); int v = d->rate; pthread_mutex_unlock(&d->use_lock); return v; }
static int channels(const void *guest) { MultiDecoder *d = acquire(guest); int v = d->channels; pthread_mutex_unlock(&d->use_lock); return v; }
static int streams(const void *guest) { MultiDecoder *d = acquire(guest); int v = d->streams; pthread_mutex_unlock(&d->use_lock); return v; }
static int coupled(const void *guest) { MultiDecoder *d = acquire(guest); int v = d->coupled; pthread_mutex_unlock(&d->use_lock); return v; }
static uint64_t elapsed(const void *guest) { MultiDecoder *d = acquire(guest); uint64_t v = d->elapsed; pthread_mutex_unlock(&d->use_lock); return v; }
static unsigned reset(void *guest) {
    MultiDecoder *d = acquire(guest);
    int result = opus_multistream_decoder_ctl(d->decoder, OPUS_RESET_STATE);
    pthread_mutex_unlock(&d->use_lock);
    return result == OPUS_OK ? 0 : R_INVALID;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned decode(void *guest, size_t *consumed, int *samples, int16_t *out, size_t out_size, const uint8_t *in, size_t in_size) {
    if (!consumed || !samples || !out || !in || in_size < 8) return R_INVALID;
    uint32_t length = be32(in), range = be32(in + 4);
    if (length > in_size - 8 || length > INT_MAX) return R_INVALID;
    MultiDecoder *d = acquire(guest);
    size_t capacity = out_size / (sizeof *out * (size_t)d->channels);
    if (capacity > INT_MAX) { pthread_mutex_unlock(&d->use_lock); return R_INVALID; }
    dm_guest_write_prepare(out, out_size);
    uint64_t start = runtime_now_ns();
    int got = opus_multistream_decode(d->decoder, in + 8, (opus_int32)length, out, (int)capacity, 0);
    d->elapsed = runtime_now_ns() - start;
    if (got < 0) { fprintf(stderr, "[unicorn-audio] Opus: %s\n", opus_strerror(got)); pthread_mutex_unlock(&d->use_lock); return R_INVALID; }
    opus_uint32 actual;
    int result = opus_multistream_decoder_ctl(d->decoder, OPUS_GET_FINAL_RANGE(&actual));
    if (result != OPUS_OK || (length && range && actual != range)) runtime_fail("multistream Opus final range mismatch");
    *consumed = length + 8; *samples = got;
    pthread_mutex_unlock(&d->use_lock);
    return 0;
}

void *runtime_multistream_override(const char *name) {
    static const struct { const char *name; void *function; } entries[] = {
        {"_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiiii", work_size},
        {"_ZN2nn5codec40InitializeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeEiiiiPKhPvmi", initialize},
        {"_ZN2nn5codec38FinalizeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeE", finalize},
        {"_ZN2nn5codec43GetHardwareOpusMultiStreamDecoderSampleRateEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", sample_rate},
        {"_ZN2nn5codec45GetHardwareOpusMultiStreamDecoderChannelCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", channels},
        {"_ZN2nn5codec49GetHardwareOpusMultiStreamDecoderTotalStreamCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", streams},
        {"_ZN2nn5codec50GetHardwareOpusMultiStreamDecoderStereoStreamCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", coupled},
        {"_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderProcessingTimeEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", elapsed},
        {"_ZN2nn5codec35ResetHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeE", reset},
        {"_ZN2nn5codec44DecodeOpusMultiStreamInterleavedWithHardwareEPNS0_34HardwareOpusMultiStreamDecoderTypeEPmPiPsmPKvm", decode},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].function;
    return NULL;
}
