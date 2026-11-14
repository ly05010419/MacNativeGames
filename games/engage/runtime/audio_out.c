/* nn::audio AudioOut on the host: the game appends PCM16 buffers and they are played through an AudioQueue (AudioToolbox),
   one queue per AudioOut. A played buffer goes to the released queue and the open call's SystemEvent is signalled, exactly
   as the console's audout service does; GetReleasedAudioOutBuffer hands the same guest buffer back, so the game can reuse
   it. Engage imports the whole AudioOut family (subsdk0 imports 19 nn::audio entries, the AudioOut set among them), and none
   of them was hosted, so those calls went to the SDK, which needs the audout:u session and the guest AudioOut header the
   service fills -- no audout:u session is ever opened in this port's logs.

   Layouts, from the upstream host (dist/work/eng_src/dm/host/hle_audio.c) and games/totk/runtime/ns2_audio_out.c: the front
   of the guest AudioOut is what the SDK's open fills and the game reads directly -- +0 sample rate, +4 channel count,
   +8 sample format (PCM16 = 2), +0xc state (1 started, 0 stopped); the host writes the first 24 bytes of an AudioOutBuffer
   {data, capacity, size}; AudioOutParameter {int32 sample rate, uint16 channel count}, 0 = 48 kHz / 2 channels. Code outside
   the SDK reads those fields too, so they are filled here even though the game also asks through the getters below.

   Measured 2026-11-13, and this is the state this file is in: with the table below bound (the log shows all 18 names bound at
   load), Engage called NONE of these entries during 65 s of play -- boot, intro movie, title/menu -- and never opened
   audout:u; the only [totk-audio] lines were the 18 bind lines. This file is the right host for an AudioOut client, but it is
   not what makes Engage silent: the guest never asks for audio output at all. runtime/audio_renderer.c's request diary shows
   the audren:u sessions are device queries (cmd 4 GetAudioDeviceServiceWithRevisionInfo + one IAudioDevice command), and the
   client entry points (OpenAudioRenderer / InitializeAudioRendererParameter / OpenDefaultAudioOut ...) are only ever *bound*,
   never called, so neither this file nor the renderer service is entered. Do not read this file as "sound fixed" until a call
   to it shows up in the log.

   Not hosted here, and deliberately: ListAudioDeviceName, GetActiveAudioDeviceName, SetAudioDeviceOutputVolume,
   AcquireAudioDeviceNotificationForOutput, GetActiveAudioDeviceChannelCountForOutput. The SDK's own device client already
   reaches this port's IAudioDevice service (runtime/audio_mixer.c) and returns sane values, so hosting them would be a second
   implementation of something that works. (An earlier version did host them and the log then showed 0 audren:u sessions
   instead of 22; that looked like a broken renderer path but the sessions were the device queries themselves.) */
#include "runtime.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void hle_signal_system_event(void *event);   /* runtime/threads.c: raise a guest nn::os system event */
#define AO_MAX 8                 /* concurrent AudioOuts */
#define AO_RELEASED 256          /* buffers awaiting GetReleasedAudioOutBuffer */
#define AO_FORMAT_PCM16 2        /* nn::audio::SampleFormat */
#define AO_FORMAT_PCM8 1
#define AO_RATE 48000

typedef struct { void *data; uint64_t cap, size; } GuestBuffer;
typedef struct {
    void *guest, *event;
    AudioQueueRef queue;
    int rate, channels, started;
    pthread_mutex_t mu;
    void *released[AO_RELEASED];
    unsigned rhead, rcount;
    uint64_t appended, released_total;
} AudioOut;
static AudioOut g_ao[AO_MAX];
static pthread_mutex_t g_ao_mu = PTHREAD_MUTEX_INITIALIZER;

/* Every entry below is called by the game with the AudioOut the open returned; an unknown pointer means the host never
   opened it, which is a bug in the wiring and must not be papered over. */
static AudioOut *ao_of(const void *guest) {
    for (int i = 0; i < AO_MAX; i++) if (g_ao[i].guest == guest) return &g_ao[i];
    runtime_fail("AudioOut not opened by the host");
}

static void ao_push_released(AudioOut *a, void *guest_buffer) {
    pthread_mutex_lock(&a->mu);
    if (a->rcount < AO_RELEASED) { a->released[(a->rhead + a->rcount) % AO_RELEASED] = guest_buffer; a->rcount++; a->released_total++; }
    else fprintf(stderr, "[totk-audio] 【红】released queue full: a buffer is lost\n");
    pthread_mutex_unlock(&a->mu);
    if (a->event) hle_signal_system_event(a->event);
    if (a->released_total % 600 == 0) fprintf(stderr, "[totk-audio] buffers played and released so far: %llu (appended %llu)\n",
                                               (unsigned long long)a->released_total, (unsigned long long)a->appended);
}

/* AudioQueue's own thread: the buffer has finished playing, so its guest buffer is free again. */
static void ao_played(void *user, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    AudioOut *a = user;
    void *guest_buffer = buffer->mUserData;
    AudioQueueFreeBuffer(queue, buffer);
    ao_push_released(a, guest_buffer);
}

static void ao_info(uint8_t *guest, int rate, int channels, int state) {
    int32_t front[4] = {rate, channels, AO_FORMAT_PCM16, state};
    memcpy(guest, front, sizeof front);
}

#define AO_ONCE(...) do { static int said; if (!said++) fprintf(stderr, "[totk-audio] " __VA_ARGS__); } while (0)

static void init_parameter(uint8_t *param) {
    memset(param, 0, 8);   /* 0 = 48 kHz / 2 channels */
    AO_ONCE("InitializeAudioOutParameter -> 0 = 48 kHz / 2 channels\n");
}

static uint32_t open_out(uint8_t *guest, void *event, const uint8_t *param) {
    int32_t rate; uint16_t channels;
    memcpy(&rate, param, 4); memcpy(&channels, param + 4, 2);
    if (!rate) rate = AO_RATE;
    if (!channels) channels = 2;
    if (channels != 1 && channels != 2 && channels != 6) {
        fprintf(stderr, "[totk-audio] 【红】AudioOut asked for %u channels\n", (unsigned)channels);
        return 0x199u;   /* ResultInvalidArgument; the game sees the open failed rather than a silent device */
    }
    pthread_mutex_lock(&g_ao_mu);
    AudioOut *a = NULL;
    for (int i = 0; i < AO_MAX && !a; i++) if (!g_ao[i].guest) a = &g_ao[i];
    if (!a) { pthread_mutex_unlock(&g_ao_mu); runtime_fail("more than 8 AudioOuts open"); }
    memset(a, 0, sizeof *a);
    a->guest = guest; a->rate = rate; a->channels = channels;
    pthread_mutex_init(&a->mu, NULL);
    pthread_mutex_unlock(&g_ao_mu);
    AudioStreamBasicDescription format = {.mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
        .mBytesPerPacket = 2u * (uint32_t)channels, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * (uint32_t)channels,
        .mChannelsPerFrame = (uint32_t)channels, .mBitsPerChannel = 16};
    OSStatus st = AudioQueueNewOutput(&format, ao_played, a, NULL, kCFRunLoopCommonModes, 0, &a->queue);
    if (st) { fprintf(stderr, "[totk-audio] 【红】AudioQueueNewOutput failed (%d): this AudioOut is silent\n", (int)st); a->queue = NULL; }
    if (event) { a->event = event; hle_bind_system_event(event, 0); }
    ao_info(guest, rate, channels, 0);
    fprintf(stderr, "[totk-audio] AudioOut opened: %d Hz x%d PCM16, %s\n", rate, channels,
            a->queue ? "AudioQueue playing" : "SILENT (no queue)");
    return 0;
}
static uint32_t open_default(uint8_t *guest, void *event, const uint8_t *param) { return open_out(guest, event, param); }
static uint32_t open_default_no_event(uint8_t *guest, const uint8_t *param) { return open_out(guest, NULL, param); }

static uint32_t start_out(uint8_t *guest) {
    AudioOut *a = ao_of(guest);
    a->started = 1;
    if (a->queue) AudioQueueStart(a->queue, NULL);
    ao_info(guest, a->rate, a->channels, 1);
    fprintf(stderr, "[totk-audio] AudioOut started (%d Hz x%d)\n", a->rate, a->channels);
    return 0;
}
static void stop_out(uint8_t *guest) {
    AudioOut *a = ao_of(guest);
    a->started = 0;
    if (a->queue) AudioQueueStop(a->queue, true);   /* immediate: the buffers still queued come back through ao_played */
    ao_info(guest, a->rate, a->channels, 0);
    fprintf(stderr, "[totk-audio] AudioOut stopped (%llu buffers played)\n", (unsigned long long)a->released_total);
}
static void close_out(uint8_t *guest) {
    AudioOut *a = ao_of(guest);
    int rate = a->rate, channels = a->channels;
    if (a->queue) { AudioQueueStop(a->queue, true); AudioQueueDispose(a->queue, true); a->queue = NULL; }
    pthread_mutex_lock(&g_ao_mu); a->guest = NULL; pthread_mutex_unlock(&g_ao_mu);
    ao_info(guest, rate, channels, 0);   /* closed: rate/channels stay as the SDK leaves them, the state says stopped */
    fprintf(stderr, "[totk-audio] AudioOut closed (%llu buffers played)\n", (unsigned long long)a->released_total);
}

static void set_buffer_info(GuestBuffer *b, void *data, uint64_t cap, uint64_t size) {
    b->data = data; b->cap = cap; b->size = size;
    AO_ONCE("SetAudioOutBufferInfo -> guest AudioOutBuffer {data, capacity, size}\n");
}
static void *buffer_data(const GuestBuffer *b) { AO_ONCE("GetAudioOutBufferDataPointer\n"); return b->data; }
static uint64_t buffer_size(const GuestBuffer *b) { AO_ONCE("GetAudioOutBufferDataSize\n"); return b->size; }

static uint32_t append_buffer(uint8_t *guest, GuestBuffer *b) {
    AudioOut *a = ao_of(guest);
    if (!b || !b->data || !b->size) return 0x199u;
    AudioQueueBufferRef qb = NULL;
    if (!a->queue || AudioQueueAllocateBuffer(a->queue, (UInt32)b->size, &qb)) {
        /* No queue (the output device failed): the buffer is returned at once with a failure, never dropped silently. */
        fprintf(stderr, "[totk-audio] 【红】AppendAudioOutBuffer with no AudioQueue: buffer of %llu bytes refused\n", (unsigned long long)b->size);
        return 0x199u;
    }
    memcpy(qb->mAudioData, b->data, (size_t)b->size);
    qb->mAudioDataByteSize = (UInt32)b->size;
    qb->mUserData = b;                                  /* the guest buffer this queue buffer stands for */
    if (AudioQueueEnqueueBuffer(a->queue, qb, 0, NULL)) { AudioQueueFreeBuffer(a->queue, qb); return 0x199u; }
    if (a->appended++ == 0) fprintf(stderr, "[totk-audio] AppendAudioOutBuffer: first buffer, %llu bytes (%d Hz x%d)\n",
                                    (unsigned long long)b->size, a->rate, a->channels);
    else if (a->appended % 600 == 0) fprintf(stderr, "[totk-audio] AppendAudioOutBuffer: %llu buffers appended (%llu bytes each)\n",
                                              (unsigned long long)a->appended, (unsigned long long)b->size);
    return 0;
}
static void *released_buffer(uint8_t *guest) {
    AudioOut *a = ao_of(guest);
    void *b = NULL;
    pthread_mutex_lock(&a->mu);
    if (a->rcount) { b = a->released[a->rhead]; a->rhead = (a->rhead + 1) % AO_RELEASED; a->rcount--; }
    pthread_mutex_unlock(&a->mu);
    return b;
}

/* The getters the game's own code and its middleware use; the SDK's versions are single loads from the guest header. */
static int sample_rate(const uint8_t *guest) { int32_t v; memcpy(&v, guest, 4); AO_ONCE("GetAudioOutSampleRate -> %d\n", v); return v; }
static int channel_count(const uint8_t *guest) { int32_t v; memcpy(&v, guest + 4, 4); AO_ONCE("GetAudioOutChannelCount -> %d\n", v); return v; }
static int sample_format(const uint8_t *guest) { int32_t v; memcpy(&v, guest + 8, 4); AO_ONCE("GetAudioOutSampleFormat -> %d\n", v); return v; }
static int out_state(const uint8_t *guest) { int32_t v; memcpy(&v, guest + 0xc, 4); AO_ONCE("GetAudioOutState -> %d\n", v); return v; }
/* Frames played so far, from the queue's own clock: the sample time is in frames at the queue's sample rate. */
static uint64_t played_sample_count(const uint8_t *guest) {
    AudioOut *a = ao_of(guest);
    AudioTimeStamp ts; memset(&ts, 0, sizeof ts);
    if (!a->queue || AudioQueueGetCurrentTime(a->queue, NULL, &ts, NULL)) return 0;
    uint64_t played = (uint64_t)(ts.mSampleTime > 0 ? ts.mSampleTime : 0);
    AO_ONCE("GetAudioOutPlayedSampleCount -> %llu frames\n", (unsigned long long)played);
    return played;
}
/* nn::audio::SampleFormat: PCM8 = 1, PCM16 = 2. Anything else has no integral byte size here and is reported as 0 (with one
   log line), rather than a number the game would size its buffers by. */
static uint32_t sample_byte_size(uint32_t format) {
    if (format == AO_FORMAT_PCM16) { AO_ONCE("GetSampleByteSize(PCM16) -> 2\n"); return 2; }
    if (format == AO_FORMAT_PCM8) { AO_ONCE("GetSampleByteSize(PCM8) -> 1\n"); return 1; }
    static int said; if (!said++) fprintf(stderr, "[totk-audio] 【红】GetSampleByteSize: format %u has no byte size here\n", format);
    return 0;
}
/* Device enumeration: this host plays through the Mac's default output, so there is exactly one output device. The guest
   structs are AudioOutInfo / AudioDeviceName = char name[256]. */
static int list_audio_outs(char *info, int max) {
    static int said; if (!said++) fprintf(stderr, "[totk-audio] ListAudioOuts -> 1 device (the Mac's default output)\n");
    if (!info || max < 1) return 0;
    memset(info, 0, 256);
    memcpy(info, "DeviceOut", 9);
    return 1;
}
void *runtime_audio_out_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", (void *)init_parameter},
        {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", (void *)open_default},
        {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", (void *)open_default_no_event},
        {"_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", (void *)start_out},
        {"_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", (void *)stop_out},
        {"_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", (void *)close_out},
        {"_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", (void *)set_buffer_info},
        {"_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", (void *)buffer_data},
        {"_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", (void *)buffer_size},
        {"_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", (void *)append_buffer},
        {"_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", (void *)released_buffer},
        {"_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", (void *)out_state},
        {"_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", (void *)sample_rate},
        {"_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", (void *)channel_count},
        {"_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", (void *)sample_format},
        {"_ZN2nn5audio28GetAudioOutPlayedSampleCountEPKNS0_8AudioOutE", (void *)played_sample_count},
        {"_ZN2nn5audio17GetSampleByteSizeENS0_12SampleFormatE", (void *)sample_byte_size},
        {"_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", (void *)list_audio_outs}};
    static unsigned char bound[sizeof map / sizeof *map];
    for (unsigned i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(name, map[i].name)) {
            /* Load time: each module that imports this name binds to the host pointer here instead of the SDK's, so this
               line is the record of what the guest actually handed over (once per name per run). */
            if (!bound[i]) { bound[i] = 1; fprintf(stderr, "[totk-audio] host bound %s\n", name); }
            return map[i].fn;
        }
    return NULL;
}
