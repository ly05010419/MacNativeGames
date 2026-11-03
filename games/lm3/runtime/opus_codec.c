/* nn::codec hardware Opus decoder, implemented with libopus on the host (the hwopus service is a
   hardware-accelerated libopus). Input frames carry Nintendo's 8-byte big-endian header
   {payload size, final range}; the decoder's final range is checked against it. A header range of 0 is not a
   recorded value: none of the 280,723 packets in 1.4.3's English voice streams has it, while a fan dub's encoder
   writes 0 into every packet (all 186,758 of the PKG PT-BR dub, which decode without error). Those are not checked. */
#include "runtime.h"
#include <opus.h>
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define R_INVALID 0x1fcd1u   /* codec: invalid argument (module 111) */
/* A fixed-capacity host table could terminate a running game when another
 * stream opened. Allocate one stable node per live decoder and free it at
 * Finalize; growing the registry must not move a decoder another thread uses.
 * Registry -> decoder is the lock order. A caller acquires the decoder lock
 * before dropping the registry lock, so Finalize cannot free a borrowed node.
 * Different streams can still decode in parallel. */
typedef struct Decoder {
    const void *guest;
    OpusDecoder *dec;
    int rate, channels;
    struct Decoder *next;
    pthread_mutex_t use_lock;
} Decoder;
static Decoder *decoders;
static size_t decoder_count, decoder_high_water, decoder_created, decoder_finalized, decoder_reinitialized;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void decoder_stats(const char *event) { /* registry lock held */
    fprintf(stderr, "[totk-audio] Opus %s: active %zu, peak %zu, created %zu, finalized %zu, reinitialized %zu (dynamic registry)\n",
            event, decoder_count, decoder_high_water, decoder_created, decoder_finalized, decoder_reinitialized);
}
static int codec_trace(void) {
    const char *v = getenv("TOTK_CODEC_TRACE");
    return v && *v && strcmp(v, "0");
}
static Decoder *acquire(const void *guest) {
    pthread_mutex_lock(&lock);
    Decoder *d = decoders;
    while (d && d->guest != guest) d = d->next;
    if (d) pthread_mutex_lock(&d->use_lock);
    pthread_mutex_unlock(&lock);
    if (!d) runtime_fail("unknown hardware Opus decoder");
    return d;
}
static void release(Decoder *d) { pthread_mutex_unlock(&d->use_lock); }
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
    Decoder *d = calloc(1, sizeof *d);
    if (!d) runtime_fail("hardware Opus decoder allocation");
    int err = 0;
    d->dec = opus_decoder_create(rate, channels, &err);
    if (!d->dec || err != OPUS_OK) {
        if (d->dec) opus_decoder_destroy(d->dec);
        free(d); runtime_fail("libopus decoder creation");
    }
    if (pthread_mutex_init(&d->use_lock, NULL)) {
        opus_decoder_destroy(d->dec); free(d); runtime_fail("hardware Opus decoder mutex");
    }
    d->guest = guest; d->rate = rate; d->channels = channels;
    pthread_mutex_lock(&lock);
    for (Decoder *old = decoders; old; old = old->next) if (old->guest == guest) {
        /* Preserve #18: the game may initialize the same object without
         * Finalize. Replace its state, waiting for any in-flight operation,
         * rather than adding another entry for the same guest object. */
        pthread_mutex_lock(&old->use_lock);
        OpusDecoder *previous = old->dec;
        old->dec = d->dec; old->rate = rate; old->channels = channels;
        decoder_reinitialized++;
        if (decoder_reinitialized == 1)
            fprintf(stderr, "[totk] hardware Opus decoder %p initialized again without Finalize: its slot is reused (logged once)\n", guest);
        if (codec_trace()) decoder_stats("reinitialize");
        pthread_mutex_unlock(&lock);
        opus_decoder_destroy(previous); release(old);
        pthread_mutex_destroy(&d->use_lock); free(d);
        return 0;
    }
    d->next = decoders; decoders = d; decoder_count++; decoder_created++;
    int peak = decoder_count > decoder_high_water;
    if (peak) decoder_high_water = decoder_count;
    /* past the old fixed 64: at 65 and every power of two, so a leak shows in a normal log */
    if (codec_trace() || (peak && decoder_count > 64 && (decoder_count == 65 || !(decoder_count & (decoder_count - 1)))))
        decoder_stats("initialize");
    pthread_mutex_unlock(&lock);
    return 0;
}
static void finalize(void *guest) {
    pthread_mutex_lock(&lock);
    Decoder **slot = &decoders;
    while (*slot && (*slot)->guest != guest) slot = &(*slot)->next;
    Decoder *d = *slot;
    if (!d) { pthread_mutex_unlock(&lock); runtime_fail("unknown hardware Opus decoder"); }
    pthread_mutex_lock(&d->use_lock);
    *slot = d->next; decoder_count--; decoder_finalized++;
    if (codec_trace()) decoder_stats("finalize");
    pthread_mutex_unlock(&lock);
    opus_decoder_destroy(d->dec);
    release(d); pthread_mutex_destroy(&d->use_lock); free(d);
}
static int sample_rate(const void *guest) { Decoder *d = acquire(guest); int v = d->rate; release(d); return v; }
static int channel_count(const void *guest) { Decoder *d = acquire(guest); int v = d->channels; release(d); return v; }
static unsigned reset(void *guest) {
    Decoder *d = acquire(guest); int ok = opus_decoder_ctl(d->dec, OPUS_RESET_STATE); release(d);
    return ok == OPUS_OK ? 0 : R_INVALID;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned decode(void *guest, size_t *consumed, int *samples, int16_t *out, size_t out_size, const uint8_t *in, size_t in_size) {
    if (!consumed || !samples || !out || !in || in_size < 8) return R_INVALID;
    uint32_t n = be32(in), range = be32(in + 4);
    if (n > in_size - 8 || n > INT_MAX) return R_INVALID;
    Decoder *d = acquire(guest);
    size_t capacity = out_size / (sizeof(int16_t) * (size_t)d->channels);
    if (capacity > INT_MAX) { release(d); return R_INVALID; }
    int max = (int)capacity;
    dm_guest_write_prepare(out, out_size);
    int got = opus_decode(d->dec, in + 8, (opus_int32)n, out, max, 0);
    if (got < 0) { fprintf(stderr, "[totk] opus_decode: %s\n", opus_strerror(got)); release(d); return R_INVALID; }
    opus_uint32 final = 0; opus_decoder_ctl(d->dec, OPUS_GET_FINAL_RANGE(&final));
    if (n && range && final != range) { fprintf(stderr, "[totk] Opus final range %#x != stream %#x\n", final, range); runtime_fail("Opus stream integrity"); }
    *consumed = 8 + n; *samples = got; release(d); return 0;
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

/* The hwopus IPC service (Luigi's Mansion 3): SDK 7's nn::codec::HardwareOpusDecoder calls the service from inside the
   SDK, below any import an override could replace. Each IHardwareOpusDecoder session is one decoder of the table above,
   keyed by its session state. Buffers: Opus frames in an A buffer, PCM out in a B buffer (as the SDK sends them). */
#include "ipc.h"
static void hw_decoder_close(void *state) { finalize(state); free(state); }
static void hw_decoder_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 4: case 6: {   /* DecodeInterleavedOld / WithPerfOld / DecodeInterleaved(reset flag) */
        if (rq->na < 1 || rq->nb < 1) ipc_unimplemented(s, rq);
        if (rq->cmd == 6 && IPC_ARG(rq, uint8_t, 0)) reset(s->state);
        size_t consumed = 0; int samples = 0;
        rs->result = decode(s->state, &consumed, &samples, ipc_guest(rq->b[0].addr, rq->b[0].size), rq->b[0].size,
                            ipc_guest(rq->a[0].addr, rq->a[0].size), rq->a[0].size);
        uint32_t c = (uint32_t)consumed, n = (uint32_t)samples; IPC_OUT(rs, c); IPC_OUT(rs, n);
        if (rq->cmd != 0) { uint64_t perf = 0; IPC_OUT(rs, perf); }
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}
static void hw_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {   /* Initialize(sample rate, channels, work size; copy transfer memory) -> IHardwareOpusDecoder */
        int rate = IPC_ARG(rq, int32_t, 0), channels = IPC_ARG(rq, int32_t, 4);
        void *key = malloc(1); if (!key) runtime_fail("hwopus session");
        unsigned r = initialize(key, rate, channels, NULL, work_size(rate, channels, 0), 0);
        if (r) { free(key); rs->result = r; return; }
        rs->move[rs->nmove++] = runtime_ipc_new_session("IHardwareOpusDecoder", hw_decoder_handler, key, hw_decoder_close);
        return;
    }
    case 1: { uint32_t v = (uint32_t)work_size(IPC_ARG(rq, int32_t, 0), IPC_ARG(rq, int32_t, 4), 0); IPC_OUT(rs, v); return; }
    default: ipc_unimplemented(s, rq);
    }
}
__attribute__((constructor)) static void register_hwopus(void) { runtime_ipc_register("hwopus", hw_manager_handler, NULL, NULL); }
