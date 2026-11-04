/* audout:u — nn::audio AudioOut, which Bayonetta 3's Wwise uses for its final mix (TOTK only uses the audio renderer,
   audren:u). Commands and layouts follow the public switchbrew description ("Audio services": IAudioOutManager, IAudioOut).
   Each AudioOut is a Core Audio AudioQueue: an appended buffer's samples are copied and enqueued; when the device has
   played it, the queue's callback gives the buffer back (GetReleasedAudioOutBuffers) and signals the buffer event, so
   the game is paced by the real output. 5.1 output is mixed down to stereo (Switch order FL FR FC LFE BL BR).
   PLA_AUDIO=0 keeps the silent real-time sink (each buffer held for its duration) for tests without a sound device. */
#include "runtime.h"
#include "ipc.h"
#include <stdatomic.h>
#include <AudioToolbox/AudioToolbox.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { STATE_STARTED = 0, STATE_STOPPED = 1, FORMAT_PCM16 = 2, DEFAULT_RATE = 48000, QUEUE_MAX = 32 };
typedef struct { uint64_t next, sample_buffer, capacity, data_size, data_offset; } AudioOutBuffer;   /* the client's descriptor */
_Static_assert(sizeof(AudioOutBuffer) == 0x28, "AudioOutBuffer");
typedef struct { uint64_t tag, bytes; int prerel; } Queued;
typedef struct {
    pthread_mutex_t lock; pthread_cond_t relcv; pthread_t thread; int running, silent, disposed, freed, primed;
    uint32_t rate, channels, state, event; float volume;
    AudioQueueRef aq;
    Queued queue[QUEUE_MAX]; unsigned nqueue;          /* appended, not yet played (head first) */
    uint64_t released[QUEUE_MAX]; unsigned nreleased;  /* played, not yet collected by GetReleasedAudioOutBuffers */
    uint64_t played_samples;
} AudioOut;

static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static void need(int ok, const char *what) { if (!ok) runtime_fail(what); }

/* The head of the queue has been played: hand its tag back. Called with the lock held. */
static void release_head(AudioOut *o) {
    if (!o->nqueue) return;
    const Queued head = o->queue[0];
    memmove(o->queue, o->queue + 1, (--o->nqueue) * sizeof *o->queue);
    if (!head.prerel && o->nreleased < QUEUE_MAX) o->released[o->nreleased++] = head.tag;
    o->played_samples += head.bytes / (2ull * o->channels);
    o->primed = 1;
    pthread_cond_broadcast(&o->relcv);
}
static _Atomic long g_enq;
static void aq_trace_line(const char *what, const void *o, long enq, long played) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[aq] %-7s obj=%p t=%.3f enq=%ld played=%ld depth=%ld\n", what, o,
        (double)ts.tv_sec + (double)ts.tv_nsec / 1e9, enq, played, enq - played);
}
static int aq_trace(void) { static int on = -1; if (on < 0) on = getenv("PLA_AQ_TRACE") != NULL; return on; }
static void played(void *user, AudioQueueRef q, AudioQueueBufferRef b) {   /* Core Audio thread */
    AudioOut *o = user;
    { static _Atomic unsigned n; unsigned i = atomic_fetch_add(&n, 1);
      if (i == 1 || i % 20000 == 0) fprintf(stderr, "[aucore] played callback #%u\n", i);
      if (aq_trace()) aq_trace_line("played", (void *)o, atomic_load(&g_enq), (long)i + 1); }
    pthread_mutex_lock(&o->lock);
    int closed = o->freed;                    /* a disposed queue can still deliver; its owner has left */
    if (!closed) release_head(o);
    pthread_mutex_unlock(&o->lock);
    if (closed) { AudioQueueFreeBuffer(q, b); return; }
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
            if (now >= due) { release_head(o); due = 0; pthread_mutex_unlock(&o->lock); runtime_event_signal(o->event); continue; }
        } else due = 0;
        pthread_mutex_unlock(&o->lock);
        struct timespec ts = {0, 1000000}; nanosleep(&ts, NULL);
    }
}
/* All AudioQueue calls run on this dedicated host thread. Core Audio queues created on the runtime's guest
   threads never drain: Start returns 0, buffers sit until Pause flushes them at once (every polled released
   buffer times out and the game tears its sink down in a loop), while the same pattern on a plain host thread
   (PLA_AUDIO_SELFTEST=1) plays and returns its buffers immediately — even later in the same process while the
   game's sink is failing. The guest threads carry the runtime's TSD/signal environment; whatever Core Audio
   inherits from them wedges its IO dispatch, so the queue now lives on a host thread of its own. */

static void open_device(AudioOut *o);
static void to_stereo(const int16_t *in, int16_t *out, uint64_t frames, uint32_t channels);
static int g_aqw_on;
static pthread_mutex_t g_aqw_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_aqw_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_aqw_thread;
enum { AQ_OPEN = 1, AQ_ENQUEUE, AQ_START, AQ_PAUSE, AQ_DISPOSE, AQ_RESET, AQ_VOLUME };
typedef struct AqJob {
    int kind, ack;                       /* ack: au_core_close waits for its DISPOSE to have run */
    AudioOut *o; void *pcm; uint64_t bytes;
    struct AqJob *next;
} AqJob;
static AqJob *g_aqw_head, *g_aqw_tail;

static void aq_post(AudioOut *o, int kind, void *pcm, uint64_t bytes, int ack) {
    AqJob *j = calloc(1, sizeof *j);
    need(j != NULL, "AudioOut job allocation");
    *j = (AqJob){.kind = kind, .ack = ack, .o = o, .pcm = pcm, .bytes = bytes};
    pthread_mutex_lock(&g_aqw_lock);
    if (g_aqw_tail) g_aqw_tail->next = j; else g_aqw_head = j;
    g_aqw_tail = j;
    pthread_cond_signal(&g_aqw_cv);
    pthread_mutex_unlock(&g_aqw_lock);
}

static void aq_worker_enqueue(AudioOut *o, void *pcm, uint64_t bytes) {
    if (!pcm || !bytes || !o->aq) { free(pcm); return; }
    uint64_t frames = bytes / (2ull * o->channels);
    AudioQueueBufferRef qb;
    OSStatus sa = AudioQueueAllocateBuffer(o->aq, (UInt32)(frames ? frames * 4 : 4), &qb);
    if (sa) { fprintf(stderr, "[aucore] AllocateBuffer failed %d\n", (int)sa); free(pcm); return; }
    to_stereo(pcm, qb->mAudioData, frames, o->channels);
    qb->mAudioDataByteSize = (UInt32)(frames * 4 ? frames * 4 : 4);
    if (!frames) memset(qb->mAudioData, 0, 4);
    free(pcm);
    long enq = atomic_fetch_add(&g_enq, 1) + 1;
    OSStatus se = AudioQueueEnqueueBuffer(o->aq, qb, 0, NULL);
    if (aq_trace() && (enq % 128 == 0)) aq_trace_line("enqueue", (void *)o, enq, 0);
    if (se) fprintf(stderr, "[aucore] EnqueueBuffer failed: %d\n", (int)se);
}

static void *aq_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_aqw_lock);
        while (!g_aqw_head) pthread_cond_wait(&g_aqw_cv, &g_aqw_lock);
        AqJob *j = g_aqw_head;
        g_aqw_head = j->next; if (!g_aqw_head) g_aqw_tail = NULL;
        pthread_mutex_unlock(&g_aqw_lock);
        AudioOut *o = j->o;
        switch (j->kind) {
        case AQ_OPEN: open_device(o); break;
        case AQ_ENQUEUE: aq_worker_enqueue(o, j->pcm, j->bytes); break;
        case AQ_START: case AQ_PAUSE: {
            int start = j->kind == AQ_START;
            if (o->aq) {
                OSStatus s = start ? AudioQueueStart(o->aq, NULL) : AudioQueuePause(o->aq);
                if (s) fprintf(stderr, "[aucore] AudioQueue%s failed: %d\n", start ? "Start" : "Pause", (int)s);
            }
            break; }
        case AQ_DISPOSE:
            if (o->aq) { AudioQueueDispose(o->aq, true); o->aq = NULL; }
            break;
        case AQ_RESET: if (o->aq) AudioQueueReset(o->aq); break;
        case AQ_VOLUME: if (o->aq) AudioQueueSetParameter(o->aq, kAudioQueueParam_Volume, j->o->volume); break;
        }
        int ack = j->ack;
        free(j);
        if (ack) { pthread_mutex_lock(&o->lock); o->disposed = 1; pthread_cond_broadcast(&o->relcv); pthread_mutex_unlock(&o->lock); }
    }
    return NULL;
}
__attribute__((constructor)) static void aq_worker_start(void) {
    if (pthread_create(&g_aqw_thread, NULL, aq_worker, NULL) == 0) g_aqw_on = 1;
}

static void open_device(AudioOut *o) {
    const char *e = getenv("PLA_AUDIO");
    o->silent = e && !strcmp(e, "0");
    if (!o->silent) {
        AudioStreamBasicDescription f = {.mSampleRate = o->rate, .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked, .mBytesPerPacket = 4, .mFramesPerPacket = 1,
            .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
        /* NULL run loop: the queue's internal thread dispatches the played callback (in-process selftest
           PLA_AUDIO_SELFTEST=1 plays 4 buffers and returns them while started). The earlier symptom "callbacks
           only ever arrive after Pause" was the GAME stopping the sink microseconds after Start — the buffers
           had not played yet; once GetReleasedAudioOutBuffer blocks (au_core_released_wait) the queue drains
           while started, as it does here. */
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
    need(d.data_offset + d.data_size <= d.capacity || !d.capacity, "AudioOut buffer bounds");
    pthread_mutex_lock(&o->lock);
    need(o->nqueue < QUEUE_MAX, "AudioOut queue full");
    o->queue[o->nqueue++] = (Queued){tag, d.data_size, 0};
    pthread_mutex_unlock(&o->lock);
    if (o->silent) return;
    uint64_t bytes = d.data_size;
    if (!bytes) { bytes = 4; }
    void *pcm = malloc(bytes); need(pcm != NULL, "AudioOut PCM copy");
    memcpy(pcm, ipc_guest(d.sample_buffer + d.data_offset, d.data_size), d.data_size);   /* the guest may reuse the buffer */
    aq_post(o, AQ_ENQUEUE, pcm, bytes, 0);
}
static void released(AudioOut *o, const IpcRequest *rq, IpcResponse *rs) {
    int pointer; const IpcBuffer *b = out_buffer(rq, &pointer); need(b != NULL, "GetReleasedAudioOutBuffers buffer");
    uint64_t *out = ipc_guest(b->addr, b->size); uint32_t n = 0;
    pthread_mutex_lock(&o->lock);
    while (n < o->nreleased && (n + 1) * 8 <= b->size) { out[n] = o->released[n]; n++; }
    memmove(o->released, o->released + n, (o->nreleased - n) * sizeof *o->released); o->nreleased -= n;
    pthread_mutex_unlock(&o->lock);
    if (n < b->size / 8) out[n] = 0;
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, (uint64_t)n * 8, 0};
    IPC_OUT(rs, n);
}
static void set_state(AudioOut *o, uint32_t state) {
    pthread_mutex_lock(&o->lock); o->state = state; if (state == STATE_STARTED) o->primed = 0; pthread_mutex_unlock(&o->lock);
    if (!o->silent) aq_post(o, state == STATE_STARTED ? AQ_START : AQ_PAUSE, NULL, 0, 0);
}
/* PLA_AUDOUT_TRACE=1: the audout:u command sequence the guest actually issues (diagnostic). */
static void audout_trace(const char *svc, uint32_t cmd) {
    if (!getenv("PLA_AUDOUT_TRACE")) return;
    static _Atomic unsigned n[2];
    unsigned i = atomic_fetch_add(&n[svc[0] == 'm'], 1);
    if (i < 64 || i % 200 == 0) fprintf(stderr, "[audout] %s cmd=%u #%u\n", svc, cmd, i);
}
static void audio_out_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioOut *o = s->state;
    audout_trace("out", rq->cmd);
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
        while (o->nqueue) release_head(o);
        pthread_mutex_unlock(&o->lock);
        if (!o->silent) aq_post(o, AQ_RESET, NULL, 0, 0);   /* its callbacks then find nothing left to release */
        if (any) runtime_event_signal(o->event);
        IPC_OUT(rs, any); return;
    }
    case 12: o->volume = IPC_ARG(rq, float, 0); if (!o->silent) aq_post(o, AQ_VOLUME, NULL, 0, 0); return;   /* SetAudioOutVolume */
    case 13: { float v = o->volume; IPC_OUT(rs, v); return; }                                   /* GetAudioOutVolume */
    default: ipc_unimplemented(s, rq);
    }
}
static void audio_out_close(void *p) {
    AudioOut *o = p;
    pthread_mutex_lock(&o->lock); o->running = 0; pthread_mutex_unlock(&o->lock);
    if (!o->silent) { aq_post(o, AQ_DISPOSE, NULL, 0, 1);
        pthread_mutex_lock(&o->lock);
        for (int i = 0; !o->disposed && i < 2000; i++) { struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 1000000;
            if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
            if (pthread_cond_timedwait(&o->relcv, &o->lock, &dl) == ETIMEDOUT && !o->disposed) continue; break; }
        pthread_mutex_unlock(&o->lock); }
    if (o->silent) pthread_join(o->thread, NULL);
    /* The AudioOut struct is deliberately not freed: AudioQueueDispose(true) can still deliver a played callback
       for a buffer that was in flight (observed: the callback then read the freed struct's event handle as 0 and
       the runtime died on it). The struct is ~1 KiB and PLA rebuilds its sink on every engine suspend/resume
       cycle, so the leak is bounded and the callbacks always touch valid memory. */
    pthread_mutex_lock(&o->lock); o->freed = 1; pthread_mutex_unlock(&o->lock);
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
    audout_trace("mgr", rq->cmd);
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
        pthread_mutex_init(&o->lock, NULL); pthread_cond_init(&o->relcv, NULL); o->rate = rate; o->channels = channels; o->state = STATE_STOPPED; o->volume = 1.0f;
        o->event = runtime_event_new(); o->running = 1;
        if (g_aqw_on) aq_post(o, AQ_OPEN, NULL, 0, 0); else open_device(o);
        write_name(rq, rs);
        uint32_t out[4] = {rate, channels, FORMAT_PCM16, STATE_STOPPED}; ipc_out(rs, out, sizeof out);
        rs->move[rs->nmove++] = runtime_ipc_new_session("IAudioOut", audio_out_handler, o, audio_out_close);
        fprintf(stderr, "[totk-audio] AudioOut open %u Hz x%u PCM16 -> %s\n", rate, channels, o->silent ? "silent sink" : "Core Audio (stereo)");
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

/* Host API for the SDK-level AudioOut hooks (runtime/audio_sdk.c): the same core the audout:u service uses. */
static _Atomic int g_live;
static int live_trace(void) { static int on = -1; if (on < 0) on = getenv("PLA_AUDOUT_LIVE") != NULL; return on; }
void *au_core_open(uint32_t rate, uint32_t channels) {
    AudioOut *o = calloc(1, sizeof *o); need(o != NULL, "AudioOut allocation");
    pthread_mutex_init(&o->lock, NULL); pthread_cond_init(&o->relcv, NULL); o->rate = rate; o->channels = channels; o->state = STATE_STOPPED; o->volume = 1.0f;
    o->event = runtime_event_new(); o->running = 1;
    if (g_aqw_on) aq_post(o, AQ_OPEN, NULL, 0, 0); else open_device(o);
    fprintf(stderr, "[totk-audio] AudioOut open %u Hz x%u PCM16 (SDK API)\n", rate, channels);
    if (live_trace()) fprintf(stderr, "[live] open obj=%p live=%d\n", (void *)o, atomic_fetch_add(&g_live, 1) + 1);
    return o;
}
uint32_t au_core_channels(void *p) { return ((AudioOut *)p)->channels; }
uint32_t au_core_state(void *p) { return ((AudioOut *)p)->state; }
void au_core_start_stop(void *p, int start) {
    { static _Atomic unsigned n; unsigned i = atomic_fetch_add(&n, 1);
      if (i < 24 || i % 512 == 0) fprintf(stderr, "[aucore] start_stop(%d)\n", start); }
    set_state(p, start ? STATE_STARTED : STATE_STOPPED);
}
/* Enqueue PCM16 of the client's layout; tag is the guest's AudioOutBuffer pointer. Returns 0 when the queue is full. */
int au_core_append(void *p, uint64_t tag, const void *pcm, uint64_t bytes) {
    AudioOut *o = p; if (!pcm || !bytes) return 0;
    {   /* PLA_AUDIO_PCM_LOG=1 (diagnostic): is the game sending real audio or silence? */
        const char *e = getenv("PLA_AUDIO_PCM_LOG");
        if (e) {
            static _Atomic unsigned nonzero, total; static _Atomic uint64_t bytes_in;
            const uint16_t *w = pcm; unsigned nz = 0;
            for (uint64_t i = 0; i + 2 <= bytes; i += 2) nz += w[i >> 1] != 0;
            if (nz) atomic_fetch_add(&nonzero, 1);
            unsigned t = atomic_fetch_add(&total, 1) + 1;
            atomic_fetch_add(&bytes_in, bytes);
            if (t <= 24 || t % 600 == 0) fprintf(stderr, "[pcmfed] append #%u bytes=%llu nonzero=%u/%u\n",
                t, (unsigned long long)atomic_load(&bytes_in), atomic_load(&nonzero), t);
        }
    }
    pthread_mutex_lock(&o->lock);
    if (o->nqueue >= QUEUE_MAX) { pthread_mutex_unlock(&o->lock); return 0; }
    o->queue[o->nqueue++] = (Queued){tag, bytes, 0};
    if (!o->silent) {   /* the queue lives on the worker; copy first, the caller's memory is guest memory */
        void *copy = malloc(bytes);
        if (!copy) { pthread_mutex_unlock(&o->lock); return 0; }
        memcpy(copy, pcm, bytes);
        aq_post(o, AQ_ENQUEUE, copy, bytes, 0);
    }
    /* PLA_AUDIO_LEAD=n: hand the tag back as soon as its samples are copied while the device is still n or more
       buffers behind, so the game never waits for the queue to drain. The game has four buffers: pacing it on
       "played" alone stalled it every 85 ms (queue depth fell to 0 each burst), which is audible as crackle. */
    {   static int lead = -2;
        if (lead == -2) { const char *e = getenv("PLA_AUDIO_LEAD"); lead = e ? atoi(e) : 6; }
        if (lead > 0 && o->nqueue <= (unsigned)lead && o->nreleased < QUEUE_MAX) {
            o->queue[o->nqueue - 1].prerel = 1;
            o->released[o->nreleased++] = tag;
            pthread_cond_broadcast(&o->relcv);
            runtime_event_signal(o->event);
        } }
    { static _Atomic unsigned n; unsigned i = atomic_fetch_add(&n, 1);
      if (i < 24 || i % 512 == 0 || live_trace()) fprintf(stderr, "[aucore] append obj=%p tag=%#llx bytes=%llu nqueue=%u state=%u silent=%d\n",
          (void *)o, (unsigned long long)tag, (unsigned long long)bytes, o->nqueue, o->state, o->silent); }
    pthread_mutex_unlock(&o->lock);
    return 1;
}
/* Pop one played buffer's tag (0 when none). */
uint64_t au_core_released(void *p) {
    AudioOut *o = p; uint64_t tag = 0;
    pthread_mutex_lock(&o->lock);
    if (o->nreleased) { tag = o->released[0]; memmove(o->released, o->released + 1, (--o->nreleased) * sizeof *o->released); }
    pthread_mutex_unlock(&o->lock);
    if (tag) runtime_event_signal(o->event);
    return tag;
}
/* Pop one played buffer's tag. While the sink is priming (buffers queued, none played yet) wait up to ms for
   the first release — PLA probes a fresh sink with two immediate polls and tears it down when both come back
   empty, which rebuilt the sink ~17 times a second and exhausted the handle table on notification events. In
   steady state releases accumulate faster than the game polls, so the wait never engages (ms is ignored then). */
uint64_t au_core_released_wait(void *p, unsigned ms) {
    AudioOut *o = p; uint64_t tag = 0; int got = 0;
    pthread_mutex_lock(&o->lock);
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += ms / 1000; dl.tv_nsec += (ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
    /* Wait whenever the device is running and buffers are queued but none is released yet: the game polls
       Released right after collecting one, so the next buffer needs its full 21 ms to play; answering "none"
       immediately makes the game tear the sink down before it ever renders a buffer (the sink was rebuilt
       ~10x/s and the mix path was never entered at all). */
    if (!(o->state == STATE_STARTED && o->nqueue && !o->nreleased)) {
        if (o->nreleased) { tag = o->released[0]; memmove(o->released, o->released + 1, (--o->nreleased) * sizeof *o->released); }
        pthread_mutex_unlock(&o->lock);
        if (tag) runtime_event_signal(o->event);
        return tag;
    }
    while (!o->nreleased && !got) {
        if (pthread_cond_timedwait(&o->relcv, &o->lock, &dl) == ETIMEDOUT) break;
        got = o->nreleased != 0;
    }
    if (o->nreleased) { tag = o->released[0]; memmove(o->released, o->released + 1, (--o->nreleased) * sizeof *o->released); }
    pthread_mutex_unlock(&o->lock);
    if (tag) runtime_event_signal(o->event);
    return tag;
}
void au_core_close(void *p) { audio_out_close(p); }

/* PLA_AUDIO_SELFTEST=1 (diagnostic): run the bare AudioQueue pattern (NULL run loop, enqueue 4 x 4096, start,
   wait 3 s) on a plain host thread at boot, to separate the process environment from the game's call context. */
void *au_core_open(uint32_t rate, uint32_t channels);
int au_core_append(void *p, uint64_t tag, const void *pcm, uint64_t bytes);
void au_core_start_stop(void *p, int start);
uint64_t au_core_released_wait(void *p, unsigned ms);
void au_core_close(void *p);
static int g_selftest_cb;
static void played_test(void *user, AudioQueueRef q, AudioQueueBufferRef b) {
    (void)user; g_selftest_cb++; fprintf(stderr, "[aqself] callback #%d\n", g_selftest_cb); AudioQueueFreeBuffer(q, b);
}
static void *aq_selftest(void *arg) {
    (void)arg;
    AudioStreamBasicDescription f = {.mSampleRate = 48000, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked, .mBytesPerPacket = 4,
        .mFramesPerPacket = 1, .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16};
    AudioQueueRef q;
    OSStatus s = AudioQueueNewOutput(&f, played_test, NULL, NULL, kCFRunLoopCommonModes, 0, &q);
    fprintf(stderr, "[aqself] NewOutput %d\n", (int)s);
    for (int i = 0; i < 4; i++) {
        AudioQueueBufferRef b; AudioQueueAllocateBuffer(q, 4096, &b);
        memset(b->mAudioData, 0, 4096); b->mAudioDataByteSize = 4096;
        AudioQueueEnqueueBuffer(q, b, 0, NULL);
    }
    fprintf(stderr, "[aqself] start %d\n", (int)AudioQueueStart(q, NULL));
    sleep(3);
    fprintf(stderr, "[aqself] callbacks while started: %d\n", g_selftest_cb);
    return NULL;
}
static void *aq_selftest_delayed(void *arg) {
    const char *d = getenv("PLA_AUDIO_SELFTEST_DELAY");
    if (d) sleep(atoi(d));
    return aq_selftest(arg);
}
/* PLA_AUDIO_SELFTEST2=1: the game's own core path (au_core_open -> append x4 -> start -> blocking released
   -> stop -> close) driven from a plain host thread at boot. */
static void *au_core_selftest(void *arg) {
    (void)arg;
    void *o = au_core_open(48000, 2);
    fprintf(stderr, "[aqself2] core %p\n", o);
    for (int i = 0; i < 4; i++) {
        static int16_t z[1024];                       /* 4096 bytes of silence, 48k stereo */
        int r = au_core_append(o, 0x100 + i, z, sizeof z);
        fprintf(stderr, "[aqself2] append %d -> %d\n", i, r);
    }
    au_core_start_stop(o, 1);
    fprintf(stderr, "[aqself2] started\n");
    for (int i = 0; i < 3; i++) {
        uint64_t t = au_core_released_wait(o, 800);
        fprintf(stderr, "[aqself2] released_wait %d -> %#llx\n", i, (unsigned long long)t);
    }
    au_core_start_stop(o, 0);
    au_core_close(o);
    fprintf(stderr, "[aqself2] done\n");
    return NULL;
}
void audio_selftest_install(void) {
    if (getenv("PLA_AUDIO_SELFTEST2")) { pthread_t th; pthread_create(&th, NULL, au_core_selftest, NULL); pthread_detach(th); return; }
    pthread_t th; pthread_create(&th, NULL, aq_selftest_delayed, NULL); pthread_detach(th);
}

__attribute__((constructor)) static void register_audio_out(void) { runtime_ipc_register("audout:u", manager_handler, NULL, NULL); }
