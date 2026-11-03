/* nn::audio AudioOut on the host (GAME_NS2; Pokemon Pokopia streams PCM through it beside its audio renderer). The game
   appends PCM16 buffers; once a buffer has played, it goes to the released queue and the open call's event is signalled,
   as the audout service does. Played through an AudioQueue (AudioToolbox) per AudioOut.
   Layouts, from seedds/nxnative dm/host/hle_audio.c (MIT): the front of the guest AudioOut is what the SDK's open fills and
   the game reads directly: +0 sample rate, +4 channel count, +8 sample format, +0xc state (1 started, 0 stopped); the
   host writes the first 24 bytes of an AudioOutBuffer {data, capacity, size}; AudioOutParameter {int32 sample rate,
   uint16 channel count}, 0 = 48 kHz / 2 channels. */
#include "runtime.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void hle_signal_system_event(void *event);
#define AO_MAX 8
#define AO_RELEASED 256
#define AO_PCM16 2

typedef struct { void *data; uint64_t cap, size; } GuestBuffer;
typedef struct {
    void *guest, *event; AudioQueueRef queue; int rate, channels, started;
    pthread_mutex_t mu; void *released[AO_RELEASED]; unsigned rhead, rcount;
} AudioOut;
static AudioOut g_ao[AO_MAX];
static pthread_mutex_t g_ao_mu = PTHREAD_MUTEX_INITIALIZER;

static AudioOut *ao_of(const void *guest) {
    for (int i = 0; i < AO_MAX; i++) if (g_ao[i].guest == guest) return &g_ao[i];
    runtime_fail("AudioOut not opened by the host");
}
static void ao_played(void *user, AudioQueueRef q, AudioQueueBufferRef b) {   /* AudioQueue's thread: the buffer has played */
    AudioOut *a = user; void *guest_buffer = b->mUserData;
    AudioQueueFreeBuffer(q, b);
    pthread_mutex_lock(&a->mu);
    if (a->rcount < AO_RELEASED) { a->released[(a->rhead + a->rcount) % AO_RELEASED] = guest_buffer; a->rcount++; }
    else fprintf(stderr, "[ns2-audio] [RED] AudioOut released queue full: a buffer is lost\n");
    pthread_mutex_unlock(&a->mu);
    if (a->event) hle_signal_system_event(a->event);
}
static void init_parameter(uint8_t *p) { memset(p, 0, 8); }
static uint32_t open_out(uint8_t *guest, void *event, const uint8_t *param) {
    int32_t rate; uint16_t ch; memcpy(&rate, param, 4); memcpy(&ch, param + 4, 2);
    if (!rate) rate = 48000;
    if (!ch) ch = 2;
    if (ch != 1 && ch != 2 && ch != 6) { fprintf(stderr, "[ns2-audio] [RED] AudioOut with %u channels\n", ch); return 0x199u; }
    pthread_mutex_lock(&g_ao_mu);
    AudioOut *a = NULL; for (int i = 0; i < AO_MAX && !a; i++) if (!g_ao[i].guest) a = &g_ao[i];
    if (!a) { pthread_mutex_unlock(&g_ao_mu); runtime_fail("more than 8 AudioOuts open"); }
    memset(a, 0, sizeof *a); a->guest = guest; a->rate = rate; a->channels = ch; pthread_mutex_init(&a->mu, NULL);
    pthread_mutex_unlock(&g_ao_mu);
    AudioStreamBasicDescription f = {.mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mBytesPerPacket = 2u * ch, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * ch, .mChannelsPerFrame = ch, .mBitsPerChannel = 16};
    OSStatus st = AudioQueueNewOutput(&f, ao_played, a, NULL, kCFRunLoopCommonModes, 0, &a->queue);
    if (st) { fprintf(stderr, "[ns2-audio] [RED] AudioQueueNewOutput failed (%d): the AudioOut is silent\n", (int)st); a->queue = NULL; }
    if (event) { a->event = event; hle_bind_system_event(event, 0); }
    int32_t front[4] = {rate, ch, AO_PCM16, 0}; memcpy(guest, front, sizeof front);
    fprintf(stderr, "[ns2-audio] AudioOut %d Hz x%d opened%s\n", rate, ch, a->queue ? "" : " (silent)");
    return 0;
}
static uint32_t open_default(uint8_t *guest, void *event, const uint8_t *param) { return open_out(guest, event, param); }
static uint32_t open_default_noevent(uint8_t *guest, const uint8_t *param) { return open_out(guest, NULL, param); }
static uint32_t start_out(uint8_t *guest) {
    AudioOut *a = ao_of(guest); a->started = 1; int32_t s = 1; memcpy(guest + 0xc, &s, 4);
    if (a->queue) AudioQueueStart(a->queue, NULL);
    return 0;
}
static void stop_out(uint8_t *guest) {
    AudioOut *a = ao_of(guest); a->started = 0; int32_t s = 0; memcpy(guest + 0xc, &s, 4);
    if (a->queue) AudioQueueStop(a->queue, true);   /* immediate: queued buffers come back through ao_played */
}
static void close_out(uint8_t *guest) {
    AudioOut *a = ao_of(guest);
    if (a->queue) { AudioQueueStop(a->queue, true); AudioQueueDispose(a->queue, true); }
    pthread_mutex_lock(&g_ao_mu); a->guest = NULL; pthread_mutex_unlock(&g_ao_mu);
    int32_t s = 0; memcpy(guest + 0xc, &s, 4);
}
static void set_buffer_info(GuestBuffer *b, void *data, uint64_t cap, uint64_t size) { b->data = data; b->cap = cap; b->size = size; }
static void *buffer_data(const GuestBuffer *b) { return b->data; }
static uint64_t buffer_size(const GuestBuffer *b) { return b->size; }
static uint32_t append_buffer(uint8_t *guest, GuestBuffer *b) {
    AudioOut *a = ao_of(guest);
    AudioQueueBufferRef qb = NULL;
    if (!a->queue || !b->size || AudioQueueAllocateBuffer(a->queue, (UInt32)b->size, &qb)) {   /* silent: released at once */
        pthread_mutex_lock(&a->mu);
        if (a->rcount < AO_RELEASED) { a->released[(a->rhead + a->rcount) % AO_RELEASED] = b; a->rcount++; }
        pthread_mutex_unlock(&a->mu);
        if (a->event) hle_signal_system_event(a->event);
        return 0;
    }
    memcpy(qb->mAudioData, b->data, b->size); qb->mAudioDataByteSize = (UInt32)b->size; qb->mUserData = b;
    if (AudioQueueEnqueueBuffer(a->queue, qb, 0, NULL)) { AudioQueueFreeBuffer(a->queue, qb); return 0x199u; }
    return 0;
}
static void *released_buffer(uint8_t *guest) {
    AudioOut *a = ao_of(guest); void *b = NULL;
    pthread_mutex_lock(&a->mu);
    if (a->rcount) { b = a->released[a->rhead]; a->rhead = (a->rhead + 1) % AO_RELEASED; a->rcount--; }
    pthread_mutex_unlock(&a->mu);
    return b;
}
static int sample_rate(const uint8_t *guest) { int32_t v; memcpy(&v, guest, 4); return v; }
static int channel_count(const uint8_t *guest) { int32_t v; memcpy(&v, guest + 4, 4); return v; }
static int sample_format(const uint8_t *guest) { int32_t v; memcpy(&v, guest + 8, 4); return v; }

void *runtime_ns2_audio_out_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", init_parameter},
        {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", open_default},
        {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", open_default_noevent},
        {"_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", start_out}, {"_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", stop_out},
        {"_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", close_out},
        {"_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", set_buffer_info},
        {"_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", buffer_data},
        {"_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", buffer_size},
        {"_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", append_buffer},
        {"_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", released_buffer},
        {"_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", sample_rate},
        {"_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", channel_count},
        {"_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", sample_format},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
