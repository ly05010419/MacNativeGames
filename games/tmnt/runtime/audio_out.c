/* nn::audio::AudioOut for Super Smash Bros. Ultimate 13.0.2: the game mixes its sound itself and hands 16-bit PCM buffers
   to an AudioOut (audout:u on a console); it opens no AudioRenderer. The host answers the AudioOut API in-process and plays
   the buffers through a CoreAudio output of its own, in the order they were appended. A buffer is released once its last
   frame went to the device; the game's buffer event (bound at open) is signaled for every release.
   Layouts from the SDK (8.2.1): AudioOutParameter {u32 sample rate, u16 channel count, u16 0xCAFE}, 0 meaning the default;
   AudioOutBuffer {+8 data pointer, +0x10 buffer size, +0x18 data size} (SetAudioOutBufferInfo, sdk+0x80f80, stays native).
   TOTK_AUDIO_OFF: buffers are released at the real-time pace without a device. */
#include "runtime.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { RATE = 48000, MAX_OUTS = 4, MAX_QUEUED = 64 };
typedef struct {
    void *guest;                 /* the game's AudioOut object */
    void *event;                 /* its buffer event (nn::os::SystemEvent, bound with hle_bind_system_event) */
    unsigned channels;           /* 2 or 6 (downmixed to stereo) */
    AudioComponentInstance unit; int unit_ok, running;
    pthread_mutex_t lock;
    uint8_t *queued[MAX_QUEUED]; unsigned qhead, qtail; uint64_t played;   /* played: bytes of queued[qhead] already sent */
    uint8_t *released[MAX_QUEUED]; unsigned rhead, rtail;
    _Atomic uint64_t frames_out, underruns, releases;
    pthread_t pacer; int pacer_on;
} Out;
static Out g_outs[MAX_OUTS];
static pthread_mutex_t g_table = PTHREAD_MUTEX_INITIALIZER;

static Out *find(const void *guest) {
    for (int i = 0; i < MAX_OUTS; i++) if (g_outs[i].guest == guest) return &g_outs[i];
    return NULL;
}
static Out *need_out(const void *guest) { Out *o = find(guest); if (!o) runtime_fail("AudioOut used without OpenDefaultAudioOut"); return o; }
static const int16_t *data_of(const uint8_t *b, uint64_t *size) { const int16_t *p; memcpy(&p, b + 8, 8); memcpy(size, b + 0x18, 8); return p; }

/* Called with o->lock held: the front buffer is done. */
static void release_front(Out *o) {
    uint8_t *b = o->queued[o->qhead % MAX_QUEUED]; o->qhead++; o->played = 0;
    if (o->rtail - o->rhead < MAX_QUEUED) o->released[o->rtail++ % MAX_QUEUED] = b;
    atomic_fetch_add(&o->releases, 1);
}
/* Fill `frames` stereo float frames from the queued buffers; returns how many came from the game. */
static UInt32 fill(Out *o, float *out, UInt32 frames) {
    UInt32 done = 0; unsigned released = 0;
    pthread_mutex_lock(&o->lock);
    while (done < frames && o->running && o->qhead != o->qtail) {
        uint64_t size; const int16_t *pcm = data_of(o->queued[o->qhead % MAX_QUEUED], &size);
        uint64_t frame_bytes = 2ull * o->channels, avail = (size - o->played) / frame_bytes;
        if (!pcm || !avail) { release_front(o); released++; continue; }
        UInt32 n = frames - done < avail ? frames - done : (UInt32)avail;
        const int16_t *s = (const int16_t *)((const uint8_t *)pcm + o->played);
        /* SSBU_AUDIO_DUMP=<path> (diagnostics): the game's PCM exactly as it goes to the device, raw 16-bit, 48 kHz, the
           output's channel count, to measure what the player hears (ton-pruefen: record the output before any counter). */
        { static FILE *dump; static int tried; if (!tried) { tried = 1; const char *e = getenv("SSBU_AUDIO_DUMP"); if (e) dump = fopen(e, "wb"); }
          if (dump) fwrite(s, 2 * o->channels, n, dump); }
        for (UInt32 i = 0; i < n; i++, s += o->channels) {
            float l, r;
            if (o->channels == 6) {   /* FL FR C LFE BL BR -> stereo */
                l = s[0] + 0.707f * s[2] + 0.707f * s[4]; r = s[1] + 0.707f * s[2] + 0.707f * s[5];
                l *= 1.0f / 2.414f; r *= 1.0f / 2.414f;
            } else { l = s[0]; r = s[1]; }
            out[2 * (done + i)] = l / 32768.0f; out[2 * (done + i) + 1] = r / 32768.0f;
        }
        done += n; o->played += (uint64_t)n * frame_bytes;
        if (o->played >= size - size % frame_bytes) { release_front(o); released++; }
    }
    pthread_mutex_unlock(&o->lock);
    if (done < frames) { memset(out + 2 * done, 0, (frames - done) * 2 * sizeof(float)); if (o->running) atomic_fetch_add(&o->underruns, 1); }
    atomic_fetch_add(&o->frames_out, done);
    for (unsigned i = 0; i < released; i++) hle_signal_system_event(o->event);
    return done;
}
static OSStatus pull(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *data) {
    (void)flags; (void)ts; (void)bus;
    fill(ref, data->mBuffers[0].mData, frames);
    return noErr;
}
/* Without a device the buffers are still consumed at 48 kHz, so the game's audio thread keeps its pace. */
static void *pacer(void *arg) {
    Out *o = arg; float scratch[2 * 240];
    while (o->pacer_on) { fill(o, scratch, 240); usleep(5000); }
    return NULL;
}
static void output_open(Out *o) {
    if (getenv("TOTK_AUDIO_OFF")) { o->pacer_on = 1; pthread_create(&o->pacer, NULL, pacer, o); fprintf(stderr, "[ssbu-audio] TOTK_AUDIO_OFF: AudioOut paced without a device\n"); return; }
#if TARGET_OS_IPHONE
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0};
#else
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
#endif
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &o->unit)) { fprintf(stderr, "[ssbu-audio] no default output device: paced without sound\n"); o->pacer_on = 1; pthread_create(&o->pacer, NULL, pacer, o); return; }
    AudioStreamBasicDescription f = {RATE, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AURenderCallbackStruct cb = {pull, o};
    if (AudioUnitSetProperty(o->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(o->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(o->unit) || AudioOutputUnitStart(o->unit)) {
        fprintf(stderr, "[ssbu-audio] CoreAudio output setup failed: paced without sound\n"); AudioComponentInstanceDispose(o->unit);
        o->pacer_on = 1; pthread_create(&o->pacer, NULL, pacer, o); return;
    }
    o->unit_ok = 1;
}

static uint32_t open_default(void *guest, void *event, const uint8_t *param) {
    if (!guest || !event || !param) runtime_fail("OpenDefaultAudioOut arguments");
    uint32_t rate; uint16_t channels; memcpy(&rate, param, 4); memcpy(&channels, param + 4, 2);
    if (rate && rate != RATE) { fprintf(stderr, "[ssbu-audio] AudioOut at %u Hz\n", rate); runtime_fail("AudioOut sample rate other than 48 kHz"); }
    if (channels && channels != 2 && channels != 6) runtime_fail("AudioOut channel count other than 2 or 6");
    pthread_mutex_lock(&g_table);
    Out *o = find(NULL); if (!o) runtime_fail("more than four AudioOuts");
    memset(o, 0, sizeof *o); o->guest = guest; o->event = event; o->channels = channels ? channels : 2;
    pthread_mutex_init(&o->lock, NULL);
    pthread_mutex_unlock(&g_table);
    hle_bind_system_event(event, 1);   /* the game's thread: binding runs SDK code */
    output_open(o);
    fprintf(stderr, "[ssbu-audio] AudioOut opened: 48000 Hz, %u channel(s)%s, %s\n", o->channels, o->channels == 6 ? " downmixed to stereo" : "",
            o->unit_ok ? "CoreAudio output" : "no device");
    return 0;
}
static uint32_t start(void *guest) { Out *o = need_out(guest); pthread_mutex_lock(&o->lock); o->running = 1; pthread_mutex_unlock(&o->lock); return 0; }
static void stop(void *guest) { Out *o = need_out(guest); pthread_mutex_lock(&o->lock); o->running = 0; pthread_mutex_unlock(&o->lock); }
static void close_out(void *guest) {
    Out *o = need_out(guest);
    if (o->unit_ok) { AudioOutputUnitStop(o->unit); AudioUnitUninitialize(o->unit); AudioComponentInstanceDispose(o->unit); o->unit_ok = 0; }
    if (o->pacer_on) { o->pacer_on = 0; pthread_join(o->pacer, NULL); }
    fprintf(stderr, "[ssbu-audio] AudioOut closed: %llu frames played, %llu buffers released, %llu underruns\n",
            (unsigned long long)o->frames_out, (unsigned long long)o->releases, (unsigned long long)o->underruns);
    pthread_mutex_lock(&g_table); o->guest = NULL; pthread_mutex_unlock(&g_table);
}
static uint8_t append(void *guest, uint8_t *buffer) {
    Out *o = need_out(guest); if (!buffer) runtime_fail("AppendAudioOutBuffer null buffer");
    pthread_mutex_lock(&o->lock);
    int ok = o->qtail - o->qhead < MAX_QUEUED;
    if (ok) o->queued[o->qtail++ % MAX_QUEUED] = buffer;
    pthread_mutex_unlock(&o->lock);
    return (uint8_t)ok;
}
static void *released(void *guest) {
    Out *o = need_out(guest); void *b = NULL;
    pthread_mutex_lock(&o->lock); if (o->rhead != o->rtail) b = o->released[o->rhead++ % MAX_QUEUED]; pthread_mutex_unlock(&o->lock);
    return b;
}
static uint32_t sample_rate(const void *guest) { (void)need_out(guest); return RATE; }
static uint32_t channel_count(const void *guest) { return need_out(guest)->channels; }
static int sample_format(const void *guest) { (void)need_out(guest); return 2; }   /* SampleFormat_PcmInt16 */

void *runtime_audio_out_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", open_default},
        {"_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", start}, {"_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", stop},
        {"_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", close_out},
        {"_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", append},
        {"_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", released},
        {"_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", sample_rate},
        {"_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", channel_count},
        {"_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", sample_format},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
