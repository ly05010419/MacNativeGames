/* nn::codec hardware Opus decoder, implemented with libopus on the host (the hwopus service is a
   hardware-accelerated libopus). Input frames carry Nintendo's 8-byte big-endian header
   {payload size, final range}; the decoder's final range is checked against it. */
#include "runtime.h"
#include <opus.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define MAX_DECODERS 256
#define R_INVALID 0x1fcd1u   /* codec: invalid argument (module 111) */
typedef struct { const void *guest; OpusDecoder *dec; int rate, channels; } Decoder;
static Decoder decoders[MAX_DECODERS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static Decoder *find(const void *guest) {
    pthread_mutex_lock(&lock); Decoder *d = NULL;
    for (int i = 0; i < MAX_DECODERS; i++) if (decoders[i].dec && decoders[i].guest == guest) d = &decoders[i];
    pthread_mutex_unlock(&lock);
    if (!d) runtime_fail("unknown hardware Opus decoder");
    return d;
}
static int valid_format(int rate, int channels) {
    return (rate == 48000 || rate == 24000 || rate == 16000 || rate == 12000 || rate == 8000) && (channels == 1 || channels == 2);
}
static size_t work_size(int rate, int channels, int option) {
    (void)option; if (!valid_format(rate, channels)) return 0;
    return (size_t)opus_decoder_get_size(channels) + 0x1000;   /* host keeps the state; the game's buffer is reserved but unused */
}
static unsigned initialize(void *guest, int rate, int channels, void *work, size_t size, int option) {
    (void)work; (void)option;
    if (!guest || !valid_format(rate, channels) || size < work_size(rate, channels, 0)) return R_INVALID;
    int err = 0; OpusDecoder *dec = opus_decoder_create(rate, channels, &err);
    if (!dec || err != OPUS_OK) runtime_fail("libopus decoder creation");
    pthread_mutex_lock(&lock);
    /* the game may re-initialize a decoder object without finalizing it: reuse its slot */
    for (int i = 0; i < MAX_DECODERS; i++) if (decoders[i].dec && decoders[i].guest == guest) {
        opus_decoder_destroy(decoders[i].dec); decoders[i] = (Decoder){guest, dec, rate, channels}; pthread_mutex_unlock(&lock); return 0; }
    for (int i = 0; i < MAX_DECODERS; i++) if (!decoders[i].dec) { decoders[i] = (Decoder){guest, dec, rate, channels}; pthread_mutex_unlock(&lock); return 0; }
    pthread_mutex_unlock(&lock); runtime_fail("hardware Opus decoder table full");
}
static void finalize(void *guest) {
    Decoder *d = find(guest); pthread_mutex_lock(&lock); opus_decoder_destroy(d->dec); *d = (Decoder){0}; pthread_mutex_unlock(&lock);
}
static int sample_rate(const void *guest) { return find(guest)->rate; }
static int channel_count(const void *guest) { return find(guest)->channels; }
static unsigned reset(void *guest) { return opus_decoder_ctl(find(guest)->dec, OPUS_RESET_STATE) == OPUS_OK ? 0 : R_INVALID; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned decode(void *guest, size_t *consumed, int *samples, int16_t *out, size_t out_size, const uint8_t *in, size_t in_size) {
    Decoder *d = find(guest);
    if (!consumed || !samples || !out || !in || in_size < 8) return R_INVALID;
    uint32_t n = be32(in), range = be32(in + 4);
    if (n > in_size - 8) return R_INVALID;
    int max = (int)(out_size / (sizeof(int16_t) * (size_t)d->channels));
    dm_guest_write_prepare(out, out_size);
    int got = opus_decode(d->dec, in + 8, (opus_int32)n, out, max, 0);
    if (got < 0) { fprintf(stderr, "[totk] opus_decode: %s\n", opus_strerror(got)); return R_INVALID; }
    opus_uint32 final = 0; opus_decoder_ctl(d->dec, OPUS_GET_FINAL_RANGE(&final));
    if (n && final != range) { fprintf(stderr, "[totk] Opus final range %#x != stream %#x\n", final, range); runtime_fail("Opus stream integrity"); }
    *consumed = 8 + n; *samples = got; return 0;
}

void *runtime_codec_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii", work_size},
        {"_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi", initialize},
        {"_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", finalize},
        {"_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE", sample_rate},
        {"_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE", channel_count},
        {"_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", reset},
        {"_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", decode},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
