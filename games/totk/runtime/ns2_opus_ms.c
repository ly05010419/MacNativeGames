/* nn::codec Opus multistream decoders (hardware and software forms; GAME_NS2, Pokemon Pokopia), with libopus's multistream
   decoder on the host, as opus_codec.c does the single-stream hardware decoder. A packet carries the same 8-byte big-endian
   header {payload size, final range} (Ryujinx's hwopus service reads it for both kinds); the final range is not checked
   here (a multistream decoder reports the last stream's). The game's work buffer is reserved but unused: the host keeps
   the state, keyed by the guest decoder object. */
#include "runtime.h"
#include <opus.h>
#include <opus_multistream.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R_INVALID 0x1fcd1u   /* codec: invalid argument (module 111) */
typedef struct MsDecoder { const void *guest; OpusMSDecoder *dec; int rate, channels, streams, coupled; struct MsDecoder *next; } MsDecoder;
static MsDecoder *decoders;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;   /* held for every call: multistream decoders are few */

static MsDecoder *find(const void *guest) { MsDecoder *d = decoders; while (d && d->guest != guest) d = d->next; return d; }
static int valid(int rate, int ch, int streams, int coupled) {
    return (rate == 48000 || rate == 24000 || rate == 16000 || rate == 12000 || rate == 8000) && ch >= 1 && ch <= 255 && streams >= 1 && coupled >= 0 && coupled <= streams && streams + coupled <= 255;
}
static size_t work_size4(int rate, int ch, int streams, int coupled) {
    if (!valid(rate, ch, streams, coupled)) return 0;
    return (size_t)opus_multistream_decoder_get_size(streams, coupled) + 0x1000;
}
static size_t work_size5(int rate, int ch, int streams, int coupled, int option) { (void)option; return work_size4(rate, ch, streams, coupled); }
static unsigned init(void *guest, int rate, int ch, int streams, int coupled, const uint8_t *mapping, void *work, size_t size) {
    (void)work;
    if (!guest || !mapping || !valid(rate, ch, streams, coupled) || size < work_size4(rate, ch, streams, coupled)) return R_INVALID;
    int err = 0; OpusMSDecoder *dec = opus_multistream_decoder_create(rate, ch, streams, coupled, mapping, &err);
    if (!dec || err != OPUS_OK) { fprintf(stderr, "[ns2-opus] [RED] multistream decoder %d Hz x%d (%d streams, %d coupled): %s\n", rate, ch, streams, coupled, opus_strerror(err)); return R_INVALID; }
    pthread_mutex_lock(&lock);
    MsDecoder *d = find(guest);
    if (d) opus_multistream_decoder_destroy(d->dec);
    else { d = calloc(1, sizeof *d); if (!d) runtime_fail("Opus multistream decoder memory"); d->guest = guest; d->next = decoders; decoders = d; }
    d->dec = dec; d->rate = rate; d->channels = ch; d->streams = streams; d->coupled = coupled;
    pthread_mutex_unlock(&lock);
    static int once; if (!once++) fprintf(stderr, "[ns2-opus] multistream decoder %d Hz x%d (%d streams, %d coupled) on libopus\n", rate, ch, streams, coupled);
    return 0;
}
static unsigned init_sw(void *guest, int rate, int ch, int streams, int coupled, const uint8_t *mapping, void *work, size_t size) { return init(guest, rate, ch, streams, coupled, mapping, work, size); }
static unsigned init_hw(void *guest, int rate, int ch, int streams, int coupled, const uint8_t *mapping, void *work, size_t size, int option) { (void)option; return init(guest, rate, ch, streams, coupled, mapping, work, size); }
static void fin(void *guest) {
    pthread_mutex_lock(&lock);
    for (MsDecoder **pp = &decoders; *pp; pp = &(*pp)->next) if ((*pp)->guest == guest) { MsDecoder *d = *pp; *pp = d->next; opus_multistream_decoder_destroy(d->dec); free(d); break; }
    pthread_mutex_unlock(&lock);
}
static unsigned reset(void *guest) {
    pthread_mutex_lock(&lock); MsDecoder *d = find(guest); int ok = d && opus_multistream_decoder_ctl(d->dec, OPUS_RESET_STATE) == OPUS_OK; pthread_mutex_unlock(&lock);
    return ok ? 0 : R_INVALID;
}
#define GETTER(fn, field) static int fn(const void *guest) { pthread_mutex_lock(&lock); MsDecoder *d = find(guest); int v = d ? d->field : 0; pthread_mutex_unlock(&lock); return v; }
GETTER(get_rate, rate) GETTER(get_channels, channels) GETTER(get_streams, streams) GETTER(get_coupled, coupled)
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned decode(void *guest, size_t *consumed, int *samples, int16_t *out, size_t out_size, const uint8_t *in, size_t in_size) {
    if (!consumed || !samples || !out || !in || in_size < 8) return R_INVALID;
    uint32_t n = be32(in);
    if (n > in_size - 8 || n > INT_MAX) return R_INVALID;
    pthread_mutex_lock(&lock);
    MsDecoder *d = find(guest);
    if (!d) { pthread_mutex_unlock(&lock); runtime_fail("unknown Opus multistream decoder"); }
    size_t capacity = out_size / (sizeof(int16_t) * (size_t)d->channels);
    if (capacity > INT_MAX) { pthread_mutex_unlock(&lock); return R_INVALID; }
    dm_guest_write_prepare(out, out_size);
    int got = opus_multistream_decode(d->dec, in + 8, (opus_int32)n, out, (int)capacity, 0);
    pthread_mutex_unlock(&lock);
    if (got < 0) { static int k; if (k++ < 4) fprintf(stderr, "[ns2-opus] [RED] opus_multistream_decode: %s\n", opus_strerror(got)); return R_INVALID; }
    *consumed = 8 + n; *samples = got; return 0;
}

void *runtime_ns2_opus_ms_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn5codec39GetOpusMultiStreamDecoderWorkBufferSizeEiiii", work_size4},
        {"_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiiii", work_size5},
        {"_ZN2nn5codec32InitializeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeEiiiiPKhPvm", init_sw},
        {"_ZN2nn5codec40InitializeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeEiiiiPKhPvmi", init_hw},
        {"_ZN2nn5codec30FinalizeOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE", fin},
        {"_ZN2nn5codec38FinalizeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeE", fin},
        {"_ZN2nn5codec27ResetOpusMultiStreamDecoderEPNS0_26OpusMultiStreamDecoderTypeE", reset},
        {"_ZN2nn5codec35ResetHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeE", reset},
        {"_ZN2nn5codec35GetOpusMultiStreamDecoderSampleRateEPKNS0_26OpusMultiStreamDecoderTypeE", get_rate},
        {"_ZN2nn5codec43GetHardwareOpusMultiStreamDecoderSampleRateEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", get_rate},
        {"_ZN2nn5codec37GetOpusMultiStreamDecoderChannelCountEPKNS0_26OpusMultiStreamDecoderTypeE", get_channels},
        {"_ZN2nn5codec45GetHardwareOpusMultiStreamDecoderChannelCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", get_channels},
        {"_ZN2nn5codec41GetOpusMultiStreamDecoderTotalStreamCountEPKNS0_26OpusMultiStreamDecoderTypeE", get_streams},
        {"_ZN2nn5codec49GetHardwareOpusMultiStreamDecoderTotalStreamCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", get_streams},
        {"_ZN2nn5codec42GetOpusMultiStreamDecoderStereoStreamCountEPKNS0_26OpusMultiStreamDecoderTypeE", get_coupled},
        {"_ZN2nn5codec50GetHardwareOpusMultiStreamDecoderStereoStreamCountEPKNS0_34HardwareOpusMultiStreamDecoderTypeE", get_coupled},
        {"_ZN2nn5codec32DecodeOpusMultiStreamInterleavedEPNS0_26OpusMultiStreamDecoderTypeEPmPiPsmPKvm", decode},
        {"_ZN2nn5codec44DecodeOpusMultiStreamInterleavedWithHardwareEPNS0_34HardwareOpusMultiStreamDecoderTypeEPmPiPsmPKvm", decode},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
