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
#include <stdatomic.h>
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

/* No input capture is opened: an empty virtual-device list is valid offline. */
static int list_virtual_input(void *names, int capacity) { (void)names; (void)capacity; return 0; }

/* No microphone on the host: list_virtual_input lists no device, so the game never opens an input stream. The SDK forwards
   these calls to the stream's Impl pointer, null for a stream that never opened, and the Impl's first read is [Impl+0x340];
   only GetInputStreamState answers a null Impl itself. The Legend of Zelda: Ocarina of Time purges its two microphone streams
   when they are not started (main+0x336ee7c, when a song is played; seedds/nxnative issue 1525, pull request 1527, MIT).
   Every input stream call the game imports answers here, without touching the guest object. */
#define INPUT_STREAM_CLOSED 0
#define INPUT_STREAM_UNAVAILABLE 0x199u   /* the failure open_out returns */
typedef struct { int64_t value[3]; } InputStreamStatistics;   /* returned through x8: InputStreamImpl::GetStatistics zeroes 24 bytes */
#define INPUT_STREAM_CALL(name) do { static _Atomic int said; if (!atomic_exchange(&said, 1)) fprintf(stderr, "[ns2-audio] input stream %s: no microphone, answered for a stream that never opened\n", name); } while (0)
static uint32_t input_open(void *stream, void *buffer, size_t size, const void *device, const void *format, int frames) {
    (void)stream; (void)buffer; (void)size; (void)device; (void)format; (void)frames; INPUT_STREAM_CALL("Open"); return INPUT_STREAM_UNAVAILABLE;
}
static uint32_t input_start(void *stream) { (void)stream; INPUT_STREAM_CALL("Start"); return INPUT_STREAM_UNAVAILABLE; }
static void input_stop(void *stream) { (void)stream; INPUT_STREAM_CALL("Stop"); }
static void input_close(void *stream) { (void)stream; INPUT_STREAM_CALL("Close"); }
static void input_purge(void *stream) { (void)stream; INPUT_STREAM_CALL("Purge"); }
static int input_read(void *stream, void *buffer, int frames) { (void)stream; (void)buffer; (void)frames; INPUT_STREAM_CALL("Read"); return 0; }
static int input_state(const void *stream) { (void)stream; INPUT_STREAM_CALL("GetState"); return INPUT_STREAM_CLOSED; }
static InputStreamStatistics input_statistics(void *stream, _Bool reset) { (void)stream; (void)reset; INPUT_STREAM_CALL("GetStatistics"); return (InputStreamStatistics){{0, 0, 0}}; }
static int input_filled(const void *stream) { (void)stream; INPUT_STREAM_CALL("GetFilledFrameCount"); return 0; }

void *runtime_ns2_audio_out_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi", list_virtual_input},
        {"_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi", input_open},
        {"_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE", input_start}, {"_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE", input_stop},
        {"_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE", input_close}, {"_ZN2nn5audio16PurgeInputStreamEPNS0_15InputStreamTypeE", input_purge},
        {"_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi", input_read}, {"_ZN2nn5audio19GetInputStreamStateEPKNS0_15InputStreamTypeE", input_state},
        {"_ZN2nn5audio24GetInputStreamStatisticsEPNS0_15InputStreamTypeEb", input_statistics},
        {"_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE", input_filled},
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
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) {
        if (strstr(name, "InputStream")) fprintf(stderr, "[ns2-audio] import answered by the host: %s\n", name);
        return map[i].fn;
    }
    return NULL;
}

_Static_assert(sizeof(InputStreamStatistics) == 24, "the 24 bytes the SDK's GetStatistics zeroes through x8");
/* Calls the statistics hook as the game does, with the destination in x8, whatever C type the hook is declared with. */
static void input_statistics_through_x8(void *hook, void *stream, int64_t *destination) {
    __asm__ volatile("mov x0, %[stream]\n\tmov w1, #1\n\tmov x8, %[destination]\n\tblr %[hook]"
        : : [stream] "r"(stream), [destination] "r"(destination), [hook] "r"(hook)
        : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x30", "cc", "memory",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
}
/* probe-audio: every input stream call in the table must answer for a stream that never opened, and leave it alone. The names
   are checked against the game's real imports outside this probe (main-inventory.json) and by the "import answered" log lines. */
static void *input_stream_hook(const char *mangled) {
    void *fn = runtime_ns2_audio_out_override(mangled);
    if (!fn) { fprintf(stderr, "[ns2-audio] no hook for %s\n", mangled); runtime_fail("input stream probe: a listed call has no hook"); }
    return fn;
}
int runtime_ns2_input_stream_probe(void) {
    uint8_t stream[64] = {0}, blank[64] = {0}, pcm[16], pcm_before[16];
    memset(pcm, 0x5a, sizeof pcm); memcpy(pcm_before, pcm, sizeof pcm);
    uint32_t (*open_stream)(void *, void *, size_t, const void *, const void *, int) = input_stream_hook("_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi");
    uint32_t (*start)(void *) = input_stream_hook("_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE");
    void (*stop)(void *) = input_stream_hook("_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE");
    void (*close_stream)(void *) = input_stream_hook("_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE");
    void (*purge)(void *) = input_stream_hook("_ZN2nn5audio16PurgeInputStreamEPNS0_15InputStreamTypeE");
    int (*read_stream)(void *, void *, int) = input_stream_hook("_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi");
    int (*state)(const void *) = input_stream_hook("_ZN2nn5audio19GetInputStreamStateEPKNS0_15InputStreamTypeE");
    void *statistics = input_stream_hook("_ZN2nn5audio24GetInputStreamStatisticsEPNS0_15InputStreamTypeEb");
    int (*filled)(const void *) = input_stream_hook("_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE");
    int checked = 9;
    uint8_t device[32] = {0}, format[12] = {0};
    #define INPUT_STREAM_CHECK(ok, what) do { if (!(ok)) runtime_fail("input stream probe: " what); checked++; } while (0)
    INPUT_STREAM_CHECK(open_stream(stream, pcm, sizeof pcm, device, format, 16) != 0, "Open must fail without a microphone");
    INPUT_STREAM_CHECK(start(stream) != 0, "Start must fail on a stream that never opened");
    INPUT_STREAM_CHECK(state(stream) == INPUT_STREAM_CLOSED, "the state of a stream that never opened is closed");
    purge(stream); stop(stream); close_stream(stream);
    INPUT_STREAM_CHECK(read_stream(stream, pcm, 16) == 0, "Read returns no frames");
    INPUT_STREAM_CHECK(!memcmp(pcm, pcm_before, sizeof pcm), "Read must not write the buffer");
    INPUT_STREAM_CHECK(filled(stream) == 0, "no frames captured");
    int64_t through_x8[4] = {-1, -1, -1, 0x1234};
    input_statistics_through_x8(statistics, stream, through_x8);
    INPUT_STREAM_CHECK(!through_x8[0] && !through_x8[1] && !through_x8[2], "the statistics are zero in the destination x8 names");
    INPUT_STREAM_CHECK(through_x8[3] == 0x1234, "the statistics are 24 bytes, no more");
    INPUT_STREAM_CHECK(!memcmp(stream, blank, sizeof stream), "the guest stream object must be left alone");
    #undef INPUT_STREAM_CHECK
    return checked;
}
