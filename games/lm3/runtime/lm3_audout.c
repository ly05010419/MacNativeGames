/* The audout:u IPC service (Luigi's Mansion 3 opens it through nn::audio::OpenDefaultAudioOut): the game appends PCM16
   buffers, a Core Audio output unit plays them in order, and each fully played buffer is given back through
   GetReleasedAudioOutBuffers with the buffer event signalled, as the console's audio output does. The SDK client
   keeps its own objects (AudioOut, SystemEvent); this side only sees IPC. Layouts follow the public audout description
   (AudioOutBuffer: next, sample buffer, capacity, data size, data offset; configuration: rate, channels, format, state). */
#include "runtime.h"
#include "ipc.h"
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { OUT_STARTED = 0, OUT_STOPPED = 1, FORMAT_PCM16 = 2, MAX_BUFFERS = 32 };
typedef struct { uint64_t next, samples, capacity, size, offset; } AudioOutBuffer;
typedef struct { uint64_t tag; const int16_t *data; uint64_t frames, played; } Queued;
typedef struct {
    pthread_mutex_t lock;
    uint32_t rate, channels, state, event;
    Queued queue[MAX_BUFFERS]; unsigned nqueue;
    uint64_t released[MAX_BUFFERS]; unsigned nreleased;
    uint64_t played_frames; float volume;
    AudioComponentInstance unit; int unit_ok;
} AudioOut;

static OSStatus pull(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)flags; (void)ts; (void)bus;
    AudioOut *o = ctx; float *out = io->mBuffers[0].mData; int signal = 0;
    memset(out, 0, (size_t)frames * 2 * sizeof(float));
    pthread_mutex_lock(&o->lock);
    for (UInt32 f = 0; f < frames && o->state == OUT_STARTED && o->nqueue; f++) {
        Queued *q = &o->queue[0];
        const int16_t *s = q->data + q->played * o->channels;
        float l = s[0] / 32768.f, r = o->channels > 1 ? s[1] / 32768.f : l;
        if (o->channels == 6) {   /* 5.1 (FL FR C LFE SL SR) to stereo: centre and surrounds at -3 dB, LFE left out */
            l += 0.7071f * (s[2] + s[4]) / 32768.f; r += 0.7071f * (s[2] + s[5]) / 32768.f;
            l = l > 1.f ? 1.f : l < -1.f ? -1.f : l; r = r > 1.f ? 1.f : r < -1.f ? -1.f : r;
        }
        out[2 * f] = l * o->volume; out[2 * f + 1] = r * o->volume;
        o->played_frames++;
        if (++q->played >= q->frames) {
            if (o->nreleased < MAX_BUFFERS) o->released[o->nreleased++] = q->tag;
            memmove(o->queue, o->queue + 1, --o->nqueue * sizeof *o->queue); signal = 1;
        }
    }
    pthread_mutex_unlock(&o->lock);
    if (signal) runtime_event_signal(o->event);
    return noErr;
}
static void unit_open(AudioOut *o) {
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &o->unit)) { fprintf(stderr, "[lm3-audout] 【红】no default output device: buffers are released without sound\n"); return; }
    AudioStreamBasicDescription f = {(Float64)o->rate, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AURenderCallbackStruct cb = {pull, o};
    if (AudioUnitSetProperty(o->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(o->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(o->unit) || AudioOutputUnitStart(o->unit)) {
        fprintf(stderr, "[lm3-audout] 【红】Core Audio output setup failed\n"); AudioComponentInstanceDispose(o->unit); return;
    }
    o->unit_ok = 1;
}
static void out_close(void *p) {
    AudioOut *o = p;
    if (o->unit_ok) { AudioOutputUnitStop(o->unit); AudioUnitUninitialize(o->unit); AudioComponentInstanceDispose(o->unit); }
    pthread_mutex_destroy(&o->lock); free(o);
}
/* Input in an A buffer or an "Auto" X buffer; output in a B buffer or an Auto C buffer (written in place, then listed). */
static const IpcBuffer *in_buffer(const IpcRequest *rq) { return rq->na && rq->a[0].size ? &rq->a[0] : rq->nx ? &rq->x[0] : NULL; }
static IpcBuffer out_buffer(const IpcRequest *rq, IpcResponse *rs) {
    if (rq->nb && rq->b[0].size) return rq->b[0];
    if (!rq->nc) runtime_fail("audout: no output buffer");
    rs->xout[rs->nxout++] = (IpcBuffer){rq->c[0].addr, rq->c[0].size, 0}; return rq->c[0];
}
static void out_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioOut *o = s->state;
    switch (rq->cmd) {
    case 0: { uint32_t v = o->state; IPC_OUT(rs, v); return; }
    case 1: pthread_mutex_lock(&o->lock); o->state = OUT_STARTED; pthread_mutex_unlock(&o->lock); return;
    case 2: pthread_mutex_lock(&o->lock); o->state = OUT_STOPPED;   /* Stop releases what is still queued */
        while (o->nqueue && o->nreleased < MAX_BUFFERS) o->released[o->nreleased++] = o->queue[--o->nqueue].tag;
        o->nqueue = 0; pthread_mutex_unlock(&o->lock); runtime_event_signal(o->event); return;
    case 3: case 7: {   /* AppendAudioOutBuffer(tag; AudioOutBuffer) */
        const IpcBuffer *b = in_buffer(rq); uint64_t tag = IPC_ARG(rq, uint64_t, 0);
        if (!b || b->size < sizeof(AudioOutBuffer)) runtime_fail("audout: append without a buffer descriptor");
        AudioOutBuffer d; memcpy(&d, ipc_guest(b->addr, sizeof d), sizeof d);
        uint64_t frames = d.size / (2 * o->channels);
        pthread_mutex_lock(&o->lock);
        if (o->nqueue == MAX_BUFFERS) { pthread_mutex_unlock(&o->lock); rs->result = 0x0a99; return; }   /* audio: buffer count full */
        o->queue[o->nqueue++] = (Queued){tag, ipc_guest(d.samples + d.offset, d.size), frames, 0};
        if (!frames && o->nreleased < MAX_BUFFERS) { o->released[o->nreleased++] = tag; o->nqueue--; }
        pthread_mutex_unlock(&o->lock);
        if (!frames) runtime_event_signal(o->event);
        return;
    }
    case 4: rs->copy[rs->ncopy++] = runtime_event_readable(o->event); return;
    case 5: case 8: {   /* GetReleasedAudioOutBuffers -> tags, count */
        IpcBuffer b = out_buffer(rq, rs); uint64_t *tags = ipc_guest(b.addr, b.size); uint32_t n = 0;
        pthread_mutex_lock(&o->lock);
        for (; n < o->nreleased && (n + 1) * sizeof *tags <= b.size; n++) tags[n] = o->released[n];
        memmove(o->released, o->released + n, (o->nreleased - n) * sizeof *o->released); o->nreleased -= n;
        if (n * sizeof *tags < b.size) tags[n] = 0;
        pthread_mutex_unlock(&o->lock);
        IPC_OUT(rs, n); return;
    }
    case 6: { uint64_t tag = IPC_ARG(rq, uint64_t, 0); uint8_t found = 0; pthread_mutex_lock(&o->lock);
        for (unsigned i = 0; i < o->nqueue; i++) found |= o->queue[i].tag == tag; pthread_mutex_unlock(&o->lock); IPC_OUT(rs, found); return; }
    case 9: { pthread_mutex_lock(&o->lock); uint32_t n = o->nqueue; pthread_mutex_unlock(&o->lock); IPC_OUT(rs, n); return; }
    case 10: { pthread_mutex_lock(&o->lock); uint64_t n = o->played_frames; pthread_mutex_unlock(&o->lock); IPC_OUT(rs, n); return; }
    case 12: o->volume = IPC_ARG(rq, float, 0); return;
    case 13: IPC_OUT(rs, o->volume); return;
    default: ipc_unimplemented(s, rq);
    }
}
static void manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    static const char name[0x100] = "DeviceOut";
    switch (rq->cmd) {
    case 0: case 2: { IpcBuffer b = out_buffer(rq, rs); uint32_t n = b.size >= sizeof name; if (n) memcpy(ipc_guest(b.addr, sizeof name), name, sizeof name); IPC_OUT(rs, n); return; }
    case 1: case 3: {   /* OpenAudioOut(rate, channels, aruid; device name in, name out; process) -> configuration, IAudioOut */
        uint32_t rate = IPC_ARG(rq, uint32_t, 0), channels = IPC_ARG(rq, uint16_t, 4);
        if (!rate) rate = 48000;
        if (!channels) channels = 2;
        if (rate != 48000 || (channels != 1 && channels != 2 && channels != 6)) { fprintf(stderr, "[lm3-audout] %u Hz x%u\n", rate, channels); runtime_fail("audout: unsupported output configuration"); }
        IpcBuffer b = out_buffer(rq, rs); if (b.size >= sizeof name) memcpy(ipc_guest(b.addr, sizeof name), name, sizeof name);
        AudioOut *o = calloc(1, sizeof *o); if (!o) runtime_fail("audout allocation");
        pthread_mutex_init(&o->lock, NULL); o->rate = rate; o->channels = channels; o->state = OUT_STOPPED; o->volume = 1.f; o->event = runtime_event_new();
        unit_open(o);
        struct { uint32_t rate, channels, format, state; } cfg = {rate, channels, FORMAT_PCM16, OUT_STOPPED}; IPC_OUT(rs, cfg);
        rs->move[rs->nmove++] = runtime_ipc_new_session("IAudioOut", out_handler, o, out_close);
        fprintf(stderr, "[lm3-audout] output opened: %u Hz x%u PCM16\n", rate, channels);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}
__attribute__((constructor)) static void register_audout(void) { runtime_ipc_register("audout:u", manager_handler, NULL, NULL); }
