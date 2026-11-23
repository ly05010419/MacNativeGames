/* audout:u — nn::audio AudioOut, which Bayonetta 3's Wwise uses for its final mix (TOTK only uses the audio renderer,
   audren:u). Commands and layouts follow the public switchbrew description ("Audio services": IAudioOutManager, IAudioOut).
   Each AudioOut is a Core Audio AudioQueue: an appended buffer's samples are copied and enqueued; when the device has
   played it, the queue's callback gives the buffer back (GetReleasedAudioOutBuffers) and signals the buffer event, so
   the game is paced by the real output. 5.1 output is mixed down to stereo (Switch order FL FR FC LFE BL BR).
   BY3_AUDIO=0 keeps the silent real-time sink (each buffer held for its duration) for tests without a sound device.

   A client that spins on GetReleasedAudioOutBuffers instead of waiting for the buffer event (明星三缺一 2013's libnx port
   asks about 50 000 times a second between emulation slices; one host core stayed at ~94 %) is given a short wait: after
   SPIN_RUN empty answers in a row, each within SPIN_GAP_NS of the last (no work between them), the next empty one waits up
   to TOTK_AUDOUT_POLL_WAIT_US (default 1000 us; 0 turns it off) for a buffer to be played and returns as soon as one is,
   with the virtual core given up meanwhile; then the count starts again. Measured on an M4 (2026-11-22): the game's core
   at 23-36 % instead of 94 % at the title, the menus and the table, the same frames at the same times, no underrun. A
   client that asks once per frame, does work between asks, or waits for the event never meets the wait: what it is
   answered and when are unchanged. The queue running dry while started is counted as an underrun and logged. */
#include "runtime.h"
#include "ipc.h"
#include <AudioToolbox/AudioToolbox.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { STATE_STARTED = 0, STATE_STOPPED = 1, FORMAT_PCM16 = 2, DEFAULT_RATE = 48000, QUEUE_MAX = 32 };
#define SPIN_GAP_NS 10000ull             /* empty answers closer than this follow each other with no work between */
#define SPIN_RUN 16                      /* that many in a row: the client spins on the queue */
#define POLL_WAIT_DEFAULT_US 1000ll      /* the longest a polling client is held (TOTK_AUDOUT_POLL_WAIT_US) */
#define UNDERRUN_LOG_NS 2000000000ull    /* underruns are logged at most this often */
int runtime_core_away(void);             /* shared/runtime/threads.c */
void runtime_core_back(int *away);
typedef struct { uint64_t next, sample_buffer, capacity, data_size, data_offset; } AudioOutBuffer;   /* the client's descriptor */
_Static_assert(sizeof(AudioOutBuffer) == 0x28, "AudioOutBuffer");
typedef struct { uint64_t tag, bytes; } Queued;
typedef struct {
    pthread_mutex_t lock; pthread_cond_t played_cv; pthread_t thread; int running, silent;
    uint32_t rate, channels, state, event; float volume;
    AudioQueueRef aq;
    Queued queue[QUEUE_MAX]; unsigned nqueue;          /* appended, not yet played (head first) */
    uint64_t released[QUEUE_MAX]; unsigned nreleased;  /* played, not yet collected by GetReleasedAudioOutBuffers */
    uint64_t played_samples;
    uint64_t last_empty_ns;                            /* when GetReleasedAudioOutBuffers last answered nothing */
    unsigned spin_run;                                 /* empty answers in a row, each within SPIN_GAP_NS of the last */
    uint64_t underruns, underruns_logged, underrun_log_ns;
} AudioOut;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void need(int ok, const char *what) { if (!ok) runtime_fail(what); }

static int64_t poll_wait_ns(void) {
    static int64_t v = -1;
    if (v < 0) { const char *e = getenv("TOTK_AUDOUT_POLL_WAIT_US"); long long us = e && *e ? atoll(e) : POLL_WAIT_DEFAULT_US; v = (us > 0 ? us : 0) * 1000; }
    return v;
}
/* Called with the lock held: the output ran dry while started (a gap in the sound), counted and logged now and then. */
static void note_underrun(AudioOut *o) {
    o->underruns++;
    uint64_t now = now_ns();
    if (o->underrun_log_ns && now - o->underrun_log_ns < UNDERRUN_LOG_NS) return;
    fprintf(stderr, "[totk-audio] AudioOut underrun: the queue ran dry (%llu so far)\n", (unsigned long long)o->underruns);
    o->underrun_log_ns = now; o->underruns_logged = o->underruns;
}
/* The head of the queue has been played: hand its tag back. Called with the lock held. by_device: the output played it
   (not a flush), so an empty queue after it is an underrun. */
static void release_head(AudioOut *o, int by_device) {
    if (!o->nqueue) return;
    const Queued head = o->queue[0];
    memmove(o->queue, o->queue + 1, (--o->nqueue) * sizeof *o->queue);
    if (o->nreleased < QUEUE_MAX) o->released[o->nreleased++] = head.tag;
    o->played_samples += head.bytes / (2ull * o->channels);
    if (by_device && !o->nqueue && o->state == STATE_STARTED) note_underrun(o);
    pthread_cond_broadcast(&o->played_cv);
}
static void played(void *user, AudioQueueRef q, AudioQueueBufferRef b) {   /* Core Audio thread */
    AudioOut *o = user;
    pthread_mutex_lock(&o->lock); release_head(o, 1); pthread_mutex_unlock(&o->lock);
    AudioQueueFreeBuffer(q, b);
    runtime_event_signal(o->event);
}
static void *silent_playback(void *arg) {
    AudioOut *o = arg; uint64_t due = 0;
    for (;;) {
        pthread_mutex_lock(&o->lock);
        if (!o->running) { pthread_mutex_unlock(&o->lock); return NULL; }
        uint64_t now = now_ns();
        if (o->state == STATE_STARTED && o->nqueue) {
            if (!due) due = now + o->queue[0].bytes / (2ull * o->channels) * 1000000000ull / o->rate;
            if (now >= due) { release_head(o, 1); due = 0; pthread_mutex_unlock(&o->lock); runtime_event_signal(o->event); continue; }
        } else due = 0;
        pthread_mutex_unlock(&o->lock);
        struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);
    }
}
static void open_device(AudioOut *o) {
    const char *e = getenv("BY3_AUDIO");
    o->silent = e && !strcmp(e, "0");
    if (!o->silent) {
        AudioStreamBasicDescription f = {.mSampleRate = o->rate, .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked, .mBytesPerPacket = 4, .mFramesPerPacket = 1,
            .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
        OSStatus s = AudioQueueNewOutput(&f, played, o, NULL, kCFRunLoopCommonModes, 0, &o->aq);
        if (s) { fprintf(stderr, "[totk-audio] AudioOut: no Core Audio output (%d); continuing silent\n", (int)s); o->silent = 1; }
    }
    if (o->silent) need(pthread_create(&o->thread, NULL, silent_playback, o) == 0, "AudioOut playback thread");
}
/* Interleaved PCM16 of the client's layout -> stereo PCM16 (the queue's format). */
static void to_stereo(const int16_t *in, int16_t *out, uint64_t frames, uint32_t channels) {
    for (uint64_t i = 0; i < frames; i++) {
        const int16_t *s = in + i * channels;
        if (channels == 2) { out[2 * i] = s[0]; out[2 * i + 1] = s[1]; continue; }
        if (channels == 1) { out[2 * i] = out[2 * i + 1] = s[0]; continue; }
        float c = s[2] * 0.7071f, l = s[0] + c + s[4] * 0.7071f, r = s[1] + c + s[5] * 0.7071f;   /* ITU-style 5.1 downmix, LFE left out */
        l *= 0.5f; r *= 0.5f;
        out[2 * i] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l); out[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

static const IpcBuffer *in_buffer(const IpcRequest *rq) { return rq->na && rq->a[0].size ? &rq->a[0] : rq->nx && rq->x[0].size ? &rq->x[0] : NULL; }
static const IpcBuffer *out_buffer(const IpcRequest *rq, int *pointer) {
    if (rq->nb && rq->b[0].size) { *pointer = 0; return &rq->b[0]; }
    if (rq->nc && rq->c[0].size) { *pointer = 1; return &rq->c[0]; }
    return NULL;
}
static void append(AudioOut *o, const IpcRequest *rq) {
    const IpcBuffer *b = in_buffer(rq); need(b && b->size >= sizeof(AudioOutBuffer), "AppendAudioOutBuffer descriptor");
    AudioOutBuffer d; memcpy(&d, ipc_guest(b->addr, sizeof d), sizeof d);
    uint64_t tag = IPC_ARG(rq, uint64_t, 0);
    /* The audio service plays data_size bytes from the start of the sample buffer and never reads data_offset (libnx:
       "unused"); The Witcher 3 leaves it uninitialised (0x5101400050f06 next to a 0x1000-byte buffer, 2026-11-23), so it
       is not used here either. */
    need(d.data_size <= d.capacity || !d.capacity, "AudioOut buffer bounds");
    pthread_mutex_lock(&o->lock);
    need(o->nqueue < QUEUE_MAX, "AudioOut queue full");
    o->queue[o->nqueue++] = (Queued){tag, d.data_size};
    pthread_mutex_unlock(&o->lock);
    if (o->silent) return;
    uint64_t frames = d.data_size / (2ull * o->channels);
    AudioQueueBufferRef qb;
    need(AudioQueueAllocateBuffer(o->aq, (UInt32)(frames * 4 ? frames * 4 : 4), &qb) == 0, "AudioOut queue buffer");
    to_stereo(ipc_guest(d.sample_buffer, d.data_size), qb->mAudioData, frames, o->channels);
    qb->mAudioDataByteSize = (UInt32)(frames * 4);
    if (!frames) { memset(qb->mAudioData, 0, 4); qb->mAudioDataByteSize = 4; }   /* an empty buffer still has to come back */
    need(AudioQueueEnqueueBuffer(o->aq, qb, 0, NULL) == 0, "AudioOut enqueue");
}
/* Called with the lock held, nothing released: a client spinning on the queue (SPIN_RUN empty answers in a row, each
   within SPIN_GAP_NS of the last) waits until the next buffer is played, or poll_wait_ns() at most, without its virtual
   core; then it has to spin SPIN_RUN times again before it waits again. */
static void wait_when_polled(AudioOut *o) {
    uint64_t now = now_ns(), previous = o->last_empty_ns; int64_t wait = poll_wait_ns();
    o->last_empty_ns = now;
    o->spin_run = previous && now - previous <= SPIN_GAP_NS ? o->spin_run + 1 : 0;
    if (!wait || o->spin_run < SPIN_RUN || o->state != STATE_STARTED || !o->nqueue || !o->running) return;
    o->spin_run = 0;
    pthread_mutex_unlock(&o->lock);
    int away = runtime_core_away();   /* no host lock held while the core is given up or taken back */
    pthread_mutex_lock(&o->lock);
    const uint64_t deadline = now + (uint64_t)wait;
    while (!o->nreleased && o->state == STATE_STARTED && o->nqueue && o->running) {
        uint64_t t = now_ns(); if (t >= deadline) break;
        struct timespec rel = {(time_t)((deadline - t) / 1000000000ull), (long)((deadline - t) % 1000000000ull)};
        if (pthread_cond_timedwait_relative_np(&o->played_cv, &o->lock, &rel) == ETIMEDOUT) break;
    }
    pthread_mutex_unlock(&o->lock);
    runtime_core_back(&away);
    pthread_mutex_lock(&o->lock);
    o->last_empty_ns = o->nreleased ? 0 : now_ns();
}
static void released(AudioOut *o, const IpcRequest *rq, IpcResponse *rs) {
    int pointer; const IpcBuffer *b = out_buffer(rq, &pointer); need(b != NULL, "GetReleasedAudioOutBuffers buffer");
    uint64_t *out = ipc_guest(b->addr, b->size); uint32_t n = 0;
    pthread_mutex_lock(&o->lock);
    if (!o->nreleased) wait_when_polled(o);
    else o->last_empty_ns = 0;
    while (n < o->nreleased && (n + 1) * 8 <= b->size) { out[n] = o->released[n]; n++; }
    memmove(o->released, o->released + n, (o->nreleased - n) * sizeof *o->released); o->nreleased -= n;
    pthread_mutex_unlock(&o->lock);
    if (n < b->size / 8) out[n] = 0;
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, (uint64_t)n * 8, 0};
    IPC_OUT(rs, n);
}
static void set_state(AudioOut *o, uint32_t state) {
    pthread_mutex_lock(&o->lock); o->state = state; pthread_cond_broadcast(&o->played_cv); pthread_mutex_unlock(&o->lock);
    if (o->aq) { if (state == STATE_STARTED) AudioQueueStart(o->aq, NULL); else AudioQueuePause(o->aq); }
}
static void audio_out_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioOut *o = s->state;
    switch (rq->cmd) {
    case 0: { uint32_t v = o->state; IPC_OUT(rs, v); return; }                                   /* GetAudioOutState */
    case 1: set_state(o, STATE_STARTED); return;                                                /* Start */
    case 2: set_state(o, STATE_STOPPED); return;                                                /* Stop */
    case 3: case 7: append(o, rq); return;                                                     /* AppendAudioOutBuffer(Auto) */
    case 4: rs->copy[rs->ncopy++] = runtime_event_readable(o->event); return;                   /* RegisterBufferEvent */
    case 5: case 8: released(o, rq, rs); return;                                               /* GetReleasedAudioOutBuffers(Auto) */
    case 6: {                                                                                   /* ContainsAudioOutBuffer(tag) */
        uint64_t tag = IPC_ARG(rq, uint64_t, 0); uint8_t found = 0;
        pthread_mutex_lock(&o->lock);
        for (unsigned i = 0; i < o->nqueue; i++) found |= o->queue[i].tag == tag;
        for (unsigned i = 0; i < o->nreleased; i++) found |= o->released[i] == tag;
        pthread_mutex_unlock(&o->lock); IPC_OUT(rs, found); return;
    }
    case 9: { pthread_mutex_lock(&o->lock); uint32_t v = o->nqueue; pthread_mutex_unlock(&o->lock); IPC_OUT(rs, v); return; }   /* GetAudioOutBufferCount */
    case 10: { pthread_mutex_lock(&o->lock); uint64_t v = o->played_samples; pthread_mutex_unlock(&o->lock); IPC_OUT(rs, v); return; }   /* GetAudioOutPlayedSampleCount */
    case 11: {                                                                                  /* FlushAudioOutBuffers */
        pthread_mutex_lock(&o->lock); uint8_t any = o->nqueue != 0;
        while (o->nqueue) release_head(o, 0);
        pthread_mutex_unlock(&o->lock);
        if (o->aq) AudioQueueReset(o->aq);   /* its callbacks then find nothing left to release */
        if (any) runtime_event_signal(o->event);
        IPC_OUT(rs, any); return;
    }
    case 12: o->volume = IPC_ARG(rq, float, 0); if (o->aq) AudioQueueSetParameter(o->aq, kAudioQueueParam_Volume, o->volume); return;   /* SetAudioOutVolume */
    case 13: { float v = o->volume; IPC_OUT(rs, v); return; }                                   /* GetAudioOutVolume */
    default: ipc_unimplemented(s, rq);
    }
}
static void audio_out_close(void *p) {
    AudioOut *o = p;
    pthread_mutex_lock(&o->lock); o->running = 0; pthread_cond_broadcast(&o->played_cv); pthread_mutex_unlock(&o->lock);
    if (o->aq) AudioQueueDispose(o->aq, true);
    if (o->silent) pthread_join(o->thread, NULL);
    if (o->underruns != o->underruns_logged) fprintf(stderr, "[totk-audio] AudioOut closed after %llu underruns\n", (unsigned long long)o->underruns);
    pthread_cond_destroy(&o->played_cv); pthread_mutex_destroy(&o->lock); free(o);
}

static const char device_name[] = "DeviceOut";
static void write_name(const IpcRequest *rq, IpcResponse *rs) {
    int pointer; const IpcBuffer *b = out_buffer(rq, &pointer);
    if (!b) return;
    char *out = ipc_guest(b->addr, b->size); size_t n = b->size < sizeof device_name ? b->size : sizeof device_name;
    memcpy(out, device_name, n); if (n) out[n - 1] = 0;
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, n, 0};
}
static void manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: case 2: { write_name(rq, rs); uint32_t n = 1; IPC_OUT(rs, n); return; }            /* ListAudioOuts(Auto) */
    case 1: case 3: {                                                                           /* OpenAudioOut(Auto) */
        uint32_t rate = IPC_ARG(rq, uint32_t, 0); uint16_t channels = IPC_ARG(rq, uint16_t, 4);
        if (!rate) rate = DEFAULT_RATE;
        if (!channels) channels = 2;
        if (rate != DEFAULT_RATE || (channels != 1 && channels != 2 && channels != 6)) {
            fprintf(stderr, "[totk-audio] OpenAudioOut %u Hz x%u\n", rate, channels); runtime_fail("AudioOut format not supported");
        }
        AudioOut *o = calloc(1, sizeof *o); need(o != NULL, "AudioOut allocation");
        pthread_mutex_init(&o->lock, NULL); pthread_cond_init(&o->played_cv, NULL); o->rate = rate; o->channels = channels; o->state = STATE_STOPPED; o->volume = 1.0f;
        o->event = runtime_event_new(); o->running = 1;
        open_device(o);
        write_name(rq, rs);
        uint32_t out[4] = {rate, channels, FORMAT_PCM16, STATE_STOPPED}; ipc_out(rs, out, sizeof out);
        rs->move[rs->nmove++] = runtime_ipc_new_session("IAudioOut", audio_out_handler, o, audio_out_close);
        fprintf(stderr, "[totk-audio] AudioOut open %u Hz x%u PCM16 -> %s\n", rate, channels, o->silent ? "silent sink" : "Core Audio (stereo)");
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_audio_out(void) { runtime_ipc_register("audout:u", manager_handler, NULL, NULL); }
