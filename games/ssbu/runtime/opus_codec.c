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
    struct Session *session;
} Decoder;

/* Super Smash Bros. Ultimate (SDK 8.2.1) also uses the C++ class nn::codec::HardwareOpusDecoder, whose methods read the
   HardwareOpusDecoderType inside it directly instead of calling the functions replaced here (23.10., disassembly of the
   SDK): IsInitialized tests +0 (the hwopus session object), GetSampleRate / GetChannelCount read +0x18 / +0x1c, Reset
   only sets the byte at +0x28 (the next decode resets), and Finalize calls the session object's virtual slot 1 (release)
   itself and clears the type. Its Finalize read the empty +0 and crashed (sdk+0x120e84). So initialize fills the type as
   the SDK's does, with a host session object at +0 whose release finalizes the host decoder; a call to any other slot
   stops with its number, and decode honours the reset byte. Layout: +0 session, +8 work buffer, +0x10 its size, +0x18
   rate, +0x1c channels, +0x20 processing time, +0x28 reset pending. */
typedef struct Session { void *const *vtable; const void *guest; } Session;
static void finalize(void *guest);
static void session_release(Session *s) { finalize((void *)s->guest); }
#define SESSION_SLOT(n) static void session_slot##n(Session *s) { (void)s; fprintf(stderr, "[totk] hwopus session object: virtual slot %d called\n", n); runtime_fail("hwopus session call not implemented"); }
SESSION_SLOT(0) SESSION_SLOT(2) SESSION_SLOT(3) SESSION_SLOT(4) SESSION_SLOT(5) SESSION_SLOT(6) SESSION_SLOT(7) SESSION_SLOT(8) SESSION_SLOT(9)
SESSION_SLOT(10) SESSION_SLOT(11) SESSION_SLOT(12) SESSION_SLOT(13) SESSION_SLOT(14) SESSION_SLOT(15)
static void *session_vtable[16];
static pthread_once_t session_once = PTHREAD_ONCE_INIT;
static void session_init(void) {
    void *fn[16] = {session_slot0, session_release, session_slot2, session_slot3, session_slot4, session_slot5, session_slot6, session_slot7,
                    session_slot8, session_slot9, session_slot10, session_slot11, session_slot12, session_slot13, session_slot14, session_slot15};
    for (int i = 0; i < 16; i++) session_vtable[i] = runtime_hle_wrap_named(fn[i], i == 1 ? "hwopus session release" : "hwopus session slot");
}
enum { TYPE_SESSION = 0, TYPE_WORK = 8, TYPE_WORK_SIZE = 0x10, TYPE_RATE = 0x18, TYPE_CHANNELS = 0x1c, TYPE_TIME = 0x20, TYPE_RESET = 0x28, TYPE_SIZE = 0x30 };
static void type_fill(void *guest, Session *s, void *work, size_t size, int rate, int channels) {
    uint8_t *t = guest; dm_guest_write_prepare(t, TYPE_SIZE); memset(t, 0, TYPE_SIZE);
    void *p = s; memcpy(t + TYPE_SESSION, &p, 8); memcpy(t + TYPE_WORK, &work, 8); uint64_t n = size; memcpy(t + TYPE_WORK_SIZE, &n, 8);
    int32_t r = rate, c = channels; memcpy(t + TYPE_RATE, &r, 4); memcpy(t + TYPE_CHANNELS, &c, 4);
}
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
/* SDK 8.2.1 (Super Smash Bros. Ultimate) has the two-argument form; the others match the later SDKs' names. */
static size_t work_size2(int rate, int channels) { return work_size(rate, channels, 0); }
static unsigned initialize(void *guest, int rate, int channels, void *work, size_t size, int option) {
    (void)option;
    if (!guest || !valid_format(rate, channels) || size < work_size(rate, channels, 0)) return R_INVALID;
    pthread_once(&session_once, session_init);
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
        type_fill(guest, old->session, work, size, rate, channels);
        pthread_mutex_unlock(&lock);
        opus_decoder_destroy(previous); release(old);
        pthread_mutex_destroy(&d->use_lock); free(d);
        return 0;
    }
    if (!(d->session = malloc(sizeof *d->session))) runtime_fail("hardware Opus session object");
    *d->session = (Session){session_vtable, guest};
    type_fill(guest, d->session, work, size, rate, channels);
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
    dm_guest_write_prepare(guest, TYPE_SIZE); memset(guest, 0, TYPE_SIZE);   /* as the SDK's Finalize leaves it */
    free(d->session);
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
    uint8_t *t = guest;
    if (t[TYPE_RESET]) { opus_decoder_ctl(d->dec, OPUS_RESET_STATE); dm_guest_write_prepare(t + TYPE_RESET, 1); t[TYPE_RESET] = 0; }   /* the class's Reset */
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
        {"_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEii", work_size2},
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
