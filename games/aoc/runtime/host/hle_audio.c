// nn::audio AudioOut: the game (FMOD nx output layer) appends PCM buffers block by block; once played the system puts them back on the "released" queue and signals the event.
// Host: real audio output = CoreAudio AudioUnit (Mac DefaultOutput / iOS RemoteIO); silent sink = consumes buffers in real time (no sound, pacing matches the real device).
//       iOS defaults to real audio output (the product must have sound), tests that want silence set DM_AUDIO=0 explicitly; Mac defaults to the silent sink (replay / gate checks are silent), DM_AUDIO=1 for real audio output.
// The contents of AudioOut / AudioOutBuffer / AudioOutParameter are read and written only through the interfaces below; the host defines its own internal layout and only writes the front part of the structs (AudioOut: the SDK's own fields, au_info).
#include "hle.h"
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <unistd.h>

enum { FMT_PCM16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, RATE = 48000 };
enum { BK_NONE, BK_AU, BK_NULL };   // what is consuming an output's queue right now (aout.backend)

typedef struct { void *ptr; uint64_t cap, size; } gbuf;     // The first 24 bytes written into the guest AudioOutBuffer
#define QMAX 64
#define RELMAX QMAX                    // Released max = number of in-flight buffers (the game must take a buffer back before it can append again), same limit as pending
typedef struct {
    pthread_mutex_t m;
    void *event;                       // Guest SystemEvent: signaled whenever a buffer is released
    int ch;
    _Atomic int state;
    _Atomic uint32_t vol;              // SetAudioOutVolume gain as float bits (1.0 at open), applied to the samples as they are pulled
    gbuf *q[QMAX]; _Atomic uint32_t q_head, q_tail;       // Pending: written by the game side, read by the output side
    gbuf *rel[RELMAX]; _Atomic uint32_t rel_head, rel_tail; // Released: written by the output side, read by the game side
    uint64_t pos;                      // Bytes already played of the current block
    AudioUnit au;
    int backend;                       // BK_*: the running consumer. Start, Stop and the background suspend change it only under g_aos_lock and act on it rather than re-deriving it from state and g_suspended
    pthread_t null_th; _Atomic int null_run;
    dispatch_semaphore_t notify_sem;
    pthread_t notify_th; _Atomic int notify_run, notify_pending, notify_armed;
    dispatch_semaphore_t rel_sem;      // Host outputs only (dm_pcmout_open): signaled for each release, so the in-process audio renderer blocks instead of polling (dm_pcmout_wait)
    int id;                            // open sequence number (#N in the log; tells simultaneous outputs apart, e.g. Engage's Wwise and Unity's built-in FMOD)
    int nlog;                          // Start / Stop / Close lines logged so far (at most 32 per output)
    _Atomic uint64_t under_n, under_b; // underruns: times the pending queue was empty when the callback needed data / zero-filled bytes
    uint64_t st_calls, st_qsum, st_u0, st_ub0; uint32_t st_fmin, st_fmax;   // sound card callback statistics (read and written only on the callback thread)
    _Atomic uint64_t cb_n;             // device callbacks of this output (the watchdog's evidence that the device still plays it)
    uint64_t wd_n, wd_t;               // watchdog: callback count last seen and when it last moved (under g_aos_lock)
    int stalled, retry_ms; uint64_t retry_at;   // the device stopped calling back (or would not start): the silent sink stands in and the watchdog retries the AudioUnit at retry_at, backing off
} aout;

// Registry of open outputs: suspended / resumed all at once by the front-end lifecycle (going to background).
// g_aos_lock also serializes every backend change (Start / Stop / suspend / resume). Start used to mark the output started and pick its backend outside it: a suspend in between
// started a silent sink, then Start started a second one over the same single-consumer queues (or the AudioUnit beside it); a Stop racing a suspend joined a sink that never existed
#define AOMAX 8
static aout *g_aos[AOMAX];
static pthread_mutex_t g_aos_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_suspended;
static aout *ao(const void *g) { aout *a = objmap_get(g); if (!a) die("AudioOut %p was not created by the host", g); return a; }
// the call site as "module+offset"; "host" when the host itself calls (in-process audio renderer)
static const char *site(void *lr, char *buf, size_t n) {
    module_t *m = lr ? module_of(lr) : NULL;
    if (m) snprintf(buf, n, "%s+%#llx", m->name, (unsigned long long)((uint8_t *)lr - m->base)); else snprintf(buf, n, "host");
    return buf;
}
// Start / Stop / Close: at most 32 lines per output; middleware calls them only at init, suspend and resume, more means it keeps toggling
static void au_note(aout *a, const char *what, void *lr) {
    char b[64];
    if (a->nlog < 32) { a->nlog++; LOG("audio #%d %s (call site %s)%s", a->id, what, site(lr, b, sizeof b), a->nlog == 32 ? "; not logged from here on" : ""); }
}

static void notify_release(aout *a) {
    atomic_store_explicit(&a->notify_pending, 1, memory_order_release);
    if (atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
        dispatch_semaphore_signal(a->notify_sem);
}

void dm_host_helper_qos(qos_class_t q);   // hle_thread.c: under DM_QOS_MAP a helper thread sets its own class instead of inheriting its creator's
static void *event_notify(void *p) {
    aout *a = p; dm_host_helper_qos(QOS_CLASS_USER_INTERACTIVE);   // the game's audio thread waits on the release event it signals
    for (;;) {
        dispatch_semaphore_wait(a->notify_sem, DISPATCH_TIME_FOREVER);
        if (atomic_exchange_explicit(&a->notify_pending, 0, memory_order_acq_rel) && a->event)
            hle_signal_system_event(a->event);
        atomic_store_explicit(&a->notify_armed, 0, memory_order_release);
        if (!atomic_load_explicit(&a->notify_run, memory_order_acquire)) break;
        if (atomic_load_explicit(&a->notify_pending, memory_order_acquire) &&
            atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
            dispatch_semaphore_signal(a->notify_sem);
    }
    return NULL;
}

// Take n bytes from the pending queue and hand them to the output side; once a whole block has played, move it to released and notify the ordinary thread.
static void pull(aout *a, uint8_t *dst, uint64_t n) {
    int released = 0;
    int16_t *s16 = (int16_t *)dst; uint64_t ns = n / 2;
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed);
    while (n) {
        uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
        if (qh == qt || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) {
            if (qh == qt && atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED) { atomic_fetch_add(&a->under_n, 1); atomic_fetch_add(&a->under_b, n); }
            memset(dst, 0, n); break; }
        gbuf *b = a->q[qh & (QMAX - 1)];
        uint64_t k = b->size - a->pos; if (k > n) k = n;
        memcpy(dst, (uint8_t *)b->ptr + a->pos, k);
        dst += k; n -= k; a->pos += k;
        if (a->pos >= b->size) {
            uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_relaxed);
            uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_acquire);
            if (rw - rr == RELMAX) die("AudioOut released queue overflow");
            a->rel[rw & (RELMAX - 1)] = b;
            atomic_store_explicit(&a->rel_tail, rw + 1, memory_order_release);
            qh++;
            atomic_store_explicit(&a->q_head, qh, memory_order_release);
            a->pos = 0; released = 1;
        }
    }
    uint32_t vb = atomic_load_explicit(&a->vol, memory_order_relaxed);
    if (vb != 0x3f800000u) {   // the output's volume (SetAudioOutVolume), as the audout session applies it to the PCM16 it plays; saturating
        float v; memcpy(&v, &vb, 4);
        for (uint64_t i = 0; i < ns; i++) { float x = s16[i] * v; s16[i] = (int16_t)(x > 32767.f ? 32767.f : x < -32768.f ? -32768.f : x); }
    }
    if (released) {
        if (a->event) notify_release(a);   // the notify thread only signals the guest's event: outputs without one (the host's renderer output, games that poll) need no wake-up
        if (a->rel_sem) dispatch_semaphore_signal(a->rel_sem);
    }
}

// A six-channel output (front-left front-right center LFE back-left back-right) goes to the device as stereo, downmixed here with the audio renderer's default Q15 coefficients (hle_audren.c):
// Core Audio's own 6 → 2 mapping keeps only the front pair, so voices on the center (Luigi's Mansion 3) and the back channels were silent. The queue still takes all six guest channels,
// in blocks through a stack buffer (no allocation in the real-time callback)
static int device_channels(const aout *a) { return a->ch == 6 ? 2 : a->ch; }
static void pull_device(aout *a, uint8_t *dst, uint64_t bytes) {
    if (a->ch != 6) { pull(a, dst, bytes); return; }
    enum { BLOCK = 128, CF = 32768, CC = 23167, CL = 8224, CB = 23167 };
    int16_t scratch[BLOCK * 6], *out = (int16_t *)dst;
    uint64_t frames = bytes / (2 * sizeof *out);
    while (frames) {
        uint64_t n = frames < BLOCK ? frames : BLOCK;
        pull(a, (uint8_t *)scratch, n * 6 * sizeof *scratch);
        for (uint64_t i = 0; i < n; i++) {
            const int16_t *p = scratch + 6 * i;
            int64_t c = (int64_t)p[2] * CC + (int64_t)p[3] * CL;
            int64_t l = ((int64_t)p[0] * CF + c + (int64_t)p[4] * CB) >> 15;
            int64_t r = ((int64_t)p[1] * CF + c + (int64_t)p[5] * CB) >> 15;
            *out++ = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            *out++ = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        frames -= n;
    }
}

static _Atomic uint64_t g_render_calls;   // Cumulative sound card callbacks (lifecycle verification: must keep growing after resume)
uint64_t dm_audio_render_calls(void) { return atomic_load_explicit(&g_render_calls, memory_order_relaxed); }
static OSStatus au_render(void *ctx, AudioUnitRenderActionFlags *f, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *bl) {
    (void)f; (void)ts; (void)bus; (void)frames;
    aout *a = ctx;
    atomic_fetch_add_explicit(&g_render_calls, 1, memory_order_relaxed); atomic_fetch_add_explicit(&a->cb_n, 1, memory_order_relaxed);
    {
        // Every 2000 callbacks of each output, log one line: frames requested per call, pending blocks at callback start, underruns (check this line on a new device; docs/agents/03-bringup.md "Crackling on iOS only").
        // Kept per output: with two outputs open, combined numbers mislead (one fully fed and one fed by nobody look like "every other callback underruns")
        uint32_t qd = atomic_load(&a->q_tail) - atomic_load(&a->q_head);
        if (!a->st_calls || frames < a->st_fmin) a->st_fmin = frames; if (frames > a->st_fmax) a->st_fmax = frames; a->st_qsum += qd;
        if (++a->st_calls % 2000 == 0) { uint64_t un = atomic_load(&a->under_n), ubb = atomic_load(&a->under_b);
            LOG("audio #%d device callbacks %llu: %u–%u frames each, %.1f blocks queued on average at callback; in these 2000, %llu underruns, %.1f ms zero-padded", a->id, (unsigned long long)a->st_calls, a->st_fmin, a->st_fmax, a->st_qsum / 2000.0,
                (unsigned long long)(un - a->st_u0), (ubb - a->st_ub0) / (2.0 * a->ch) / (RATE / 1000.0)); a->st_fmin = ~0u; a->st_fmax = 0; a->st_qsum = 0; a->st_u0 = un; a->st_ub0 = ubb; } }
    pull_device(a, bl->mBuffers[0].mData, bl->mBuffers[0].mDataByteSize);
    { static int mute = -1; if (mute < 0) { const char *e = getenv("DM_AUDIO_MUTE"); mute = e && atoi(e); }   // diagnostic: the whole real-output path (AudioUnit callback pacing, background take-over) but with the output silenced (lifecycle tests on the Mac without noise)
      if (mute) memset(bl->mBuffers[0].mData, 0, bl->mBuffers[0].mDataByteSize); }
    return noErr;
}
static OSStatus au_create(aout *a) {
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output,
#if TARGET_OS_IPHONE
                                   .componentSubType = kAudioUnitSubType_RemoteIO,
#else
                                   .componentSubType = kAudioUnitSubType_DefaultOutput,
#endif
                                   .componentManufacturer = kAudioUnitManufacturer_Apple};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    a->au = NULL; if (!c || AudioComponentInstanceNew(c, &a->au)) { a->au = NULL; return -1; }
    unsigned ch = (unsigned)device_channels(a);
    AudioStreamBasicDescription f = {.mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
                                     .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                     .mBytesPerPacket = 2u * ch, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * ch,
                                     .mChannelsPerFrame = ch, .mBitsPerChannel = 16};
    AURenderCallbackStruct cb = {au_render, a};
#if !TARGET_OS_IPHONE
    { const char *e = getenv("DM_DBG_AU_FRAMES"); if (e) {   // diagnostic: on the Mac, make the sound card callback ask N frames each time (1024 = the iOS default, reproduces the crackling)
        UInt32 fr = (UInt32)atoi(e); OSStatus st = AudioUnitSetProperty(a->au, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &fr, sizeof fr);
        LOG("audio device buffer frame count set to %u: status %d", fr, (int)st); } }
#endif
    OSStatus st = AudioUnitSetProperty(a->au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    if (!st) st = AudioUnitSetProperty(a->au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (!st) st = AudioUnitInitialize(a->au);
    if (st) { AudioComponentInstanceDispose(a->au); a->au = NULL; }
    return st;
}
// OpenAudioOut runs on the game's thread, and creating the AudioUnit takes about 24 KB of stack (measured on the Mac: component lookup, instance, Initialize), more than some game threads have
// left (Super Mario 3D World's save thread has 8 KB in all; Pokémon: Let's Go overflowed here at startup, PR #36). So it runs on a host thread, once per AudioOut. Not HLE_FORCE_BIG:
// the open hooks log the game's call site through __builtin_return_address(0), which under the big-stack entry would be the trampoline
static void *au_create_th(void *a) { return (void *)(intptr_t)au_create(a); }
static void au_open(aout *a) {
    pthread_t t; void *r = NULL;
    if (pthread_create(&t, NULL, au_create_th, a) || pthread_join(t, &r)) die("AudioUnit setup: cannot start its thread");
    OSStatus st = (OSStatus)(intptr_t)r; if (st) die("AudioUnit setup failed (status %d)", (int)st);
}
// Silent sink: consumes in real time at 48 kHz every 5 ms. When more than 100 ms behind (iOS suspended the whole process in the background, SIGSTOP on the Mac), restart the count from now and do not make up the missed time --
// making it up would pull hundreds of times in one go (seconds' worth), releasing all of the game's buffers and logging hundreds of underruns; a suspended console does not play that stretch either
static void *null_sink(void *p) {
    aout *a = p; uint8_t scratch[RATE / 200 * 2 * 8]; dm_host_helper_qos(QOS_CLASS_USER_INTERACTIVE);   // paces the game's audio in real time
    uint64_t bytes = (uint64_t)RATE / 200 * 2 * a->ch, t = now_ns();
    while (atomic_load_explicit(&a->null_run, memory_order_acquire)) {
        t += 5000000; uint64_t now = now_ns();
        if (t > now) usleep((useconds_t)((t - now) / 1000)); else if (now - t > 100000000) t = now;
        pull(a, scratch, bytes);
    }
    return NULL;
}
// Backend changes; callers hold g_aos_lock. pull() takes no lock, so joining a sink under it cannot deadlock
static void null_start(aout *a) { atomic_store_explicit(&a->null_run, 1, memory_order_release); pthread_create(&a->null_th, NULL, null_sink, a); a->backend = BK_NULL; }
static void null_stop(aout *a) { atomic_store_explicit(&a->null_run, 0, memory_order_release); pthread_join(a->null_th, NULL); a->backend = BK_NONE; }
static void backend_stop(aout *a) {
    if (a->backend == BK_AU) { AudioOutputUnitStop(a->au); a->backend = BK_NONE; }
    else if (a->backend == BK_NULL) null_stop(a);
}
static int real_audio(void) { const char *e = getenv("DM_AUDIO"); return e ? atoi(e) != 0 : TARGET_OS_IPHONE; }
// Start the AudioUnit of an output whose backend is BK_NONE (callers hold g_aos_lock). When the device refuses (the session is held by a call, or is being reset), the silent sink stands in and the watchdog retries
static void au_go(aout *a, uint64_t now) {
    OSStatus st = a->au ? AudioOutputUnitStart(a->au) : -1;
    a->wd_n = atomic_load_explicit(&a->cb_n, memory_order_relaxed); a->wd_t = now;
    if (!st) { a->backend = BK_AU; a->stalled = 0; return; }
    null_start(a);
    if (!a->stalled) { a->stalled = 1; a->retry_ms = 1000; LOG("[RED] audio #%d: the AudioUnit did not start (status %d): the silent sink stands in, retrying", a->id, (int)st); }
    else if (a->retry_ms < 8000) a->retry_ms *= 2;
    a->retry_at = now + (uint64_t)a->retry_ms * 1000000;
}
// Watchdog (real output only): an AudioUnit that is started while the app is in focus must keep calling back. iOS stops calling back without telling the AudioUnit when another audio session takes over
// (Siri, an alarm, a call) or the media services reset; the output's buffers were then never released, and a game whose audio thread waits for them (Wwise's poll-only output, FMOD) stayed silent for good.
// After 200 ms without a callback the silent sink takes over (the game's audio keeps its real-time pace) and the AudioUnit is retried after 1, 2, 4, 8 s. A late wake-up of the watchdog itself (the whole process was stopped) only restarts the count.
// The time is read under g_aos_lock: au_go stamps wd_t under it too, and a watchdog time read before the lock was older than the stamp of an AudioUnit the return from the background had just
// started (DRAGON QUEST VII Reimagined on an iPhone: "has not called back for 18446744073662 ms", a restart of a running AudioUnit)
enum { WD_TICK_MS = 50, WD_STALL_MS = 200 };
static void *au_watchdog(void *p) {
    (void)p; uint64_t last = now_ns();
    for (;;) {
        usleep(WD_TICK_MS * 1000);
        pthread_mutex_lock(&g_aos_lock);
        uint64_t t = now_ns(), gap = t - last; last = t;
        for (int i = 0; i < AOMAX; i++) {
            aout *a = g_aos[i]; if (!a || !a->au) continue;
            uint64_t n = atomic_load_explicit(&a->cb_n, memory_order_relaxed);
            if (a->backend != BK_AU || g_suspended || gap > 4ull * WD_TICK_MS * 1000000 || n != a->wd_n) {
                if (a->backend == BK_AU && n != a->wd_n) a->retry_ms = 0;   // callbacks seen: a later stall starts the back-off from 1 s again
                a->wd_n = n; a->wd_t = t;
            } else if (t - a->wd_t > WD_STALL_MS * 1000000ull) {
                LOG("[RED] audio #%d: the device has not called back for %.0f ms while playing: the silent sink stands in, retrying the AudioUnit", a->id, (t - a->wd_t) / 1e6);
                AudioOutputUnitStop(a->au); a->backend = BK_NONE; null_start(a);
                a->stalled = 1; a->retry_ms = a->retry_ms ? (a->retry_ms < 8000 ? a->retry_ms * 2 : 8000) : 1000; a->retry_at = t + (uint64_t)a->retry_ms * 1000000;
            }
            if (a->stalled && a->backend == BK_NULL && !g_suspended && t >= a->retry_at) {
                null_stop(a); au_go(a, t);
                if (a->backend == BK_AU) LOG("audio #%d: AudioUnit restarted", a->id);
            }
        }
        pthread_mutex_unlock(&g_aos_lock);
    }
    return NULL;
}
static void au_watchdog_start(void) { pthread_t t; if (!pthread_create(&t, NULL, au_watchdog, NULL)) pthread_detach(t); }

// AudioOutParameter = { int32 sampleRate; uint16 channelCount; uint16 _r; ... }, 0 = system default (48 kHz / 2 channels)
HLE_FORCE("_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", "audio output parameter defaults", void, h_au_InitParam, uint8_t *p) { memset(p, 0, 8); }
// AudioOutInfo = { char name[256] }
HLE_FORCE("_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", "audio output device list; 1 on the host", int, h_au_List, char *info, int n) {
    if (n < 1) return 0;
    memset(info, 0, 256); strcpy(info, "DeviceOut"); return 1;
}
// The front of the guest AudioOut as the SDK's OpenAudioOut fills it from the service's answer: sample rate, channel count, sample format, state (+0 / +4 / +8 / +0xc, the same in SDK 8, 12, 18 and 20;
// its GetAudioOutSampleRate / ChannelCount / SampleFormat are single loads from there); CloseAudioOut sets the state to stopped and leaves the rest. Code outside the SDK reads these fields too:
// middleware that checks the first word before it stops and closes its output skipped the close while the host left it zero, freed the PCM buffers and the host's consumer read freed memory (Luigi's Mansion 3, PR #109)
static void au_info(void *g, int ch, int state) { uint32_t *f = g; f[0] = RATE; f[1] = (uint32_t)ch; f[2] = FMT_PCM16; f[3] = (uint32_t)state; }
static Result au_open_at(void *g, void *ev, const char *name, const uint8_t *param, void *lr) {
    static _Atomic int nopen;
    int ch = *(const uint16_t *)(param + 4); int rate = *(const int32_t *)param;
    if (!ch) ch = 2;
    if (rate && rate != RATE) die("AudioOut sample rate %d not supported (48 kHz only)", rate);
    aout *a = calloc(1, sizeof *a);
    pthread_mutex_init(&a->m, NULL); a->event = ev; a->ch = ch;
    atomic_init(&a->state, STATE_STOPPED); atomic_init(&a->vol, 0x3f800000u);
    atomic_init(&a->q_head, 0); atomic_init(&a->q_tail, 0);
    atomic_init(&a->rel_head, 0); atomic_init(&a->rel_tail, 0);
    atomic_init(&a->null_run, 0); atomic_init(&a->notify_run, 1);
    atomic_init(&a->notify_pending, 0); atomic_init(&a->notify_armed, 0);
    a->notify_sem = dispatch_semaphore_create(0); a->id = atomic_fetch_add(&nopen, 1);
    pthread_create(&a->notify_th, NULL, event_notify, a);
    if (ev) hle_bind_system_event(ev, 1);   // Buffer release event: bind a kernel event on the guest thread (auto-clear), afterwards signaled by the audio thread
    objmap_put(g, a);
    pthread_mutex_lock(&g_aos_lock);
    int slot = -1; for (int i = 0; i < AOMAX; i++) if (!g_aos[i]) { slot = i; break; }
    if (slot < 0) die("more than %d AudioOuts open at once", AOMAX);
    g_aos[slot] = a;
    pthread_mutex_unlock(&g_aos_lock);
    char sb[64];
    LOG("audio #%d OpenAudioOut %s channels %d %s%s (call site %s)", a->id, name ? name : "(default)", ch, real_audio() ? "real output" : "silent sink", ev ? "" : ", no release event (the game polls)", site(lr, sb, sizeof sb));
    if (real_audio()) { static pthread_once_t wd = PTHREAD_ONCE_INIT; au_open(a); pthread_once(&wd, au_watchdog_start); }
    au_info(g, ch, STATE_STOPPED);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventEPKcRKNS0_17AudioOutParameterE", "connect to the audout service; host AudioUnit / silent sink",
          Result, h_au_Open, void *g, void *ev, const char *name, const uint8_t *param) { return au_open_at(g, ev, name, param, __builtin_return_address(0)); }
// The two default-device overloads (Engage, SDK 12): as OpenAudioOut with the default device name; the overload without an event binds no release event (the game polls GetReleasedAudioOutBuffer)
HLE_FORCE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", "connect to the audout service (default device); same as OpenAudioOut",
          Result, h_au_OpenDef, void *g, void *ev, const uint8_t *param) { return au_open_at(g, ev, NULL, param, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", "connect to the audout service (default device, no event); same as OpenAudioOut",
          Result, h_au_OpenDef0, void *g, const uint8_t *param) { return au_open_at(g, NULL, NULL, param, __builtin_return_address(0)); }
static Result au_start_at(void *g, void *lr) {
    aout *a = ao(g);
    pthread_mutex_lock(&g_aos_lock);
    int was = atomic_exchange_explicit(&a->state, STATE_STARTED, memory_order_acq_rel);
    au_note(a, was == STATE_STARTED ? "Start (already playing)" : g_suspended ? "Start (app in the background: the silent sink takes over for now)" : "Start", lr);
    if (was != STATE_STARTED) {
        if (a->au && !g_suspended) { a->stalled = 0; au_go(a, now_ns()); }
        else null_start(a);
    }
    pthread_mutex_unlock(&g_aos_lock);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", "start playback", Result, h_au_Start, void *g) { return au_start_at(g, __builtin_return_address(0)); }
static void au_stop_at(void *g, void *lr) {   // lr NULL: the stop inside Close, not logged separately
    aout *a = ao(g);
    pthread_mutex_lock(&g_aos_lock);
    int was = atomic_exchange_explicit(&a->state, STATE_STOPPED, memory_order_acq_rel);
    if (lr) au_note(a, was == STATE_STARTED ? "Stop" : "Stop (already stopped)", lr);
    backend_stop(a);   // the AudioUnit, or the silent sink (including one that took over during suspension)
    pthread_mutex_unlock(&g_aos_lock);
}
HLE_FORCE("_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", "stop playback", void, h_au_Stop, void *g) { au_stop_at(g, __builtin_return_address(0)); }
static void au_close(void *g, void *lr) {
    au_note(ao(g), "Close", lr);
    au_stop_at(g, NULL);
    aout *a = objmap_del(g);
    atomic_store_explicit(&a->notify_run, 0, memory_order_release);
    dispatch_semaphore_signal(a->notify_sem);
    pthread_join(a->notify_th, NULL);
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) if (g_aos[i] == a) g_aos[i] = NULL;
    pthread_mutex_unlock(&g_aos_lock);
    if (a->au) { AudioUnitUninitialize(a->au); AudioComponentInstanceDispose(a->au); }
    dispatch_release(a->notify_sem);
    if (a->rel_sem) dispatch_release(a->rel_sem);
    pthread_mutex_destroy(&a->m); int ch = a->ch; free(a);
    au_info(g, ch, STATE_STOPPED);
}
HLE_FORCE("_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", "close the audio output", void, h_au_Close, void *g) { au_close(g, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", "playback state", int, h_au_State, const void *g) { return atomic_load_explicit(&ao(g)->state, memory_order_acquire); }
// Pokémon Legends: Z-A (SDK 19): Wwise's sink wakeup thread (main+0x30faae8) asks after every release event and marks the sink starved at 0; the SDK's own version read the audout session the host never made
HLE_FORCE("_ZN2nn5audio22GetAudioOutBufferCountEPKNS0_8AudioOutE", "buffers appended and not yet played to the end (the one playing included)", unsigned, h_au_BufCount, const void *g) {
    aout *a = ao(g);
    return atomic_load_explicit(&a->q_tail, memory_order_acquire) - atomic_load_explicit(&a->q_head, memory_order_acquire);
}
HLE_FORCE("_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", "sample rate", int, h_au_Rate, const void *g) { (void)ao(g); return RATE; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", "channel count", int, h_au_Ch, const void *g) { return ao(g)->ch; }
HLE_FORCE("_ZN2nn5audio15GetAudioOutNameEPKNS0_8AudioOutE", "device name; the host has only the default device", const char *, h_au_Name, const void *g) { (void)g; return "DeviceOut"; }
HLE_FORCE("_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", "sample format; PCM16 only", int, h_au_Fmt, const void *g) { (void)ao(g); return FMT_PCM16; }
// STEINS;GATE RE:BOOT (SDK 20) sets its movie sound's output volume right after opening it; the SDK's versions call the audout session the host never made (a null read at sdk+0x1f2314)
HLE_FORCE("_ZN2nn5audio17SetAudioOutVolumeEPNS0_8AudioOutEf", "output volume; a gain on the samples the host plays", void, h_au_SetVol, void *g, float v) {
    if (!(v >= 0.f)) v = 0.f; uint32_t b; memcpy(&b, &v, 4); atomic_store_explicit(&ao(g)->vol, b, memory_order_relaxed);
}
HLE_FORCE("_ZN2nn5audio17GetAudioOutVolumeEPKNS0_8AudioOutE", "output volume", float, h_au_GetVol, const void *g) {
    uint32_t b = atomic_load_explicit(&ao(g)->vol, memory_order_relaxed); float v; memcpy(&v, &b, 4); return v;
}

HLE_FORCE("_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", "buffer description; written into the first 24 bytes of the guest struct", void, h_au_SetBuf, gbuf *b, void *p, uint64_t cap, uint64_t size) {
    b->ptr = p; b->cap = cap; b->size = size;
}
HLE_FORCE("_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", "buffer data pointer", void *, h_au_BufPtr, const gbuf *b) { return b->ptr; }
HLE_FORCE("_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", "buffer data size", uint64_t, h_au_BufSize, const gbuf *b) { return b->size; }
HLE_FORCE("_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", "append a buffer to play", unsigned, h_au_Append, void *g, gbuf *b) {
    aout *a = ao(g);
    // Diagnostic switch DM_AUDIO_DUMP=<file>: every PCM block the game appends is written out as is (16-bit, 48 kHz, channel count per the OpenAudioOut log, usually 6). Used to judge "did a given sound play or not":
    // cross-correlate the original game's audio against it (this is how the tier-one drop sound was verified). The file is opened only when the variable is set; when unset this costs only one extra cached check
    { static FILE *df; static int on = -1; if (on < 0) { const char *e = getenv("DM_AUDIO_DUMP"); on = e != NULL; if (on && !(df = fopen(e, "wb"))) LOG("[RED] DM_AUDIO_DUMP cannot open %s", e); }
      if (df && b && b->ptr && b->size) fwrite(b->ptr, 1, (size_t)b->size, df); }
    pthread_mutex_lock(&a->m);
    uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_relaxed);
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_acquire);
    int ok = qt - qh < QMAX;
    if (ok) { a->q[qt & (QMAX - 1)] = b; atomic_store_explicit(&a->q_tail, qt + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return ok;      // bool: false when the queue is full (the real device has a limit too)
}
HLE_FORCE("_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", "get a played buffer; NULL if none", gbuf *, h_au_Released, void *g) {
    aout *a = ao(g); gbuf *b = NULL;
    pthread_mutex_lock(&a->m);
    uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_relaxed);
    uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_acquire);
    if (rr != rw) { b = a->rel[rr & (RELMAX - 1)]; atomic_store_explicit(&a->rel_head, rr + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return b;
}

// Audio input (a microphone): the host has none. SDL3's Switch audio driver lists the input devices when its audio starts (Absolum); the SDK's versions reach the audin service.
// No device is listed and an open fails, so the calls that need an open AudioIn are never reached; they still answer without touching the guest object, in case a game ignores the open's result.
// The buffer descriptor functions (SetAudioInBufferInfo, GetAudioInBufferDataPointer / Size) only fill and read the game's own struct and run in the sdk
HLE_FORCE("_ZN2nn5audio12ListAudioInsEPNS0_11AudioInInfoEi", "audio input device list; the host has no microphone: 0 entries", int, h_ai_List, void *info, int n) { (void)info; (void)n; return 0; }
static Result ai_open(const char *name, void *lr) {
    static _Atomic int once; char sb[64];
    if (!atomic_exchange(&once, 1)) LOG("audio input %s: the host has no microphone, the open fails (call site %s)", name ? name : "(default)", site(lr, sb, sizeof sb));
    return RES_FAIL;
}
HLE_FORCE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPNS_2os11SystemEventEPKcRKNS0_16AudioInParameterE", "connect to the audin service; no microphone on the host: fails",
          Result, h_ai_Open, void *g, void *ev, const char *name, const void *param) { (void)g; (void)ev; (void)param; return ai_open(name, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPKcRKNS0_16AudioInParameterE", "connect to the audin service (no buffer event: Patrick's Parabox imports this one); no microphone on the host: fails",
          Result, h_ai_OpenNoEv, void *g, const char *name, const void *param) { (void)g; (void)param; return ai_open(name, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn5audio18OpenDefaultAudioInEPNS0_7AudioInEPNS_2os11SystemEventERKNS0_16AudioInParameterE", "connect to the audin service (default device); no microphone on the host: fails",
          Result, h_ai_OpenDef, void *g, void *ev, const void *param) { (void)g; (void)ev; (void)param; return ai_open(NULL, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn5audio12StartAudioInEPNS0_7AudioInE", "start recording; never opened on the host: fails", Result, h_ai_Start, void *g) { (void)g; return RES_FAIL; }
HLE_FORCE("_ZN2nn5audio11StopAudioInEPNS0_7AudioInE", "stop recording; never opened on the host", void, h_ai_Stop, void *g) { (void)g; }
HLE_FORCE("_ZN2nn5audio12CloseAudioInEPNS0_7AudioInE", "close the audio input; never opened on the host", void, h_ai_Close, void *g) { (void)g; }
HLE_FORCE("_ZN2nn5audio19AppendAudioInBufferEPNS0_7AudioInEPNS0_13AudioInBufferE", "append a recording buffer; never opened on the host: refused", unsigned, h_ai_Append, void *g, void *b) { (void)g; (void)b; return 0; }
HLE_FORCE("_ZN2nn5audio24GetReleasedAudioInBufferEPNS0_7AudioInE", "get a recorded buffer; never opened on the host: none", void *, h_ai_Released, void *g) { (void)g; return NULL; }
HLE_FORCE("_ZN2nn5audio20GetAudioInSampleRateEPKNS0_7AudioInE", "recording sample rate", int, h_ai_Rate, const void *g) { (void)g; return RATE; }
HLE_FORCE("_ZN2nn5audio22GetAudioInChannelCountEPKNS0_7AudioInE", "recording channel count", int, h_ai_Ch, const void *g) { (void)g; return 2; }
HLE_FORCE("_ZN2nn5audio22GetAudioInSampleFormatEPKNS0_7AudioInE", "recording sample format", int, h_ai_Fmt, const void *g) { (void)g; return FMT_PCM16; }

// Host-internal PCM output (used by the in-process audio renderer, see hle_audren.c): the same AudioOut machinery (real output / silent sink / background take-over), keyed by a host-allocated dummy object
void *dm_pcmout_open(int ch) {
    void *key = calloc(1, 64); uint8_t param[8] = {0}; *(uint16_t *)(param + 4) = (uint16_t)ch;
    if (au_open_at(key, NULL, "AudioRenderer", param, NULL)) die("host PCM output cannot be opened");
    ao(key)->rel_sem = dispatch_semaphore_create(0);
    return key;
}
// Wait until the output releases a block, at most ms milliseconds (spurious returns are fine: the caller checks dm_pcmout_released again)
void dm_pcmout_wait(void *k, int ms) { dispatch_semaphore_wait(ao(k)->rel_sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * 1000000)); }
void dm_pcmout_start(void *k) { au_start_at(k, NULL); }
void dm_pcmout_close(void *k) { au_close(k, NULL); free(k); }   // stop, take it out of the registry (frees its AOMAX slot), free it and its key
int dm_pcmout_append(void *k, void *b) { return (int)h_au_Append(k, b); }
void *dm_pcmout_released(void *k) { return h_au_Released(k); }

// Front-end lifecycle / audio session interruption: suspend real audio output, with the silent sink taking over real-time consumption meanwhile (pacing unbroken); switch back to the AudioUnit on resume.
// The front end calls with 1 on going to background / interruption began, and with 0 on returning to foreground / interruption ended (shouldResume).
void dm_audio_suspend(int on) {
    pthread_mutex_lock(&g_aos_lock);
    if (g_suspended != on) {
        g_suspended = on; int failed = 0;
        for (int i = 0; i < AOMAX; i++) {
            aout *a = g_aos[i];
            if (!a || !a->au) continue;
            // While suspended the silent sink consumes in real time instead: otherwise buffers are no longer released, the FMOD output thread waits forever and the game's audio pacing stalls (same for an incoming call interruption)
            if (on) { if (a->backend == BK_AU) { AudioOutputUnitStop(a->au); null_start(a); } }
            else if (a->backend == BK_NULL) {
                // The session may still be held by someone else (back in the game with a call not yet hung up): not fatal, the silent sink keeps covering and the watchdog retries
                null_stop(a); a->stalled = 0; au_go(a, now_ns()); failed |= a->backend != BK_AU; }
        }
        LOG("audio %s", on ? "suspended (background / interruption)" : failed ? "resumed, an AudioUnit still refuses to start (retrying)" : "resumed (foreground / interruption ended)");
    }
    pthread_mutex_unlock(&g_aos_lock);
}
// The media services were reset (iOS): every AudioUnit is invalid. Create each output's unit again and restart the ones that were playing; the front end has set the audio session up again first
void dm_audio_reset(void) {
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) {
        aout *a = g_aos[i]; if (!a || !a->au) continue;
        int playing = a->backend == BK_AU;
        if (playing) { AudioOutputUnitStop(a->au); a->backend = BK_NONE; }
        AudioUnitUninitialize(a->au); AudioComponentInstanceDispose(a->au);
        OSStatus st = au_create(a);
        if (st) LOG("[RED] audio #%d: no AudioUnit after the media services reset (status %d): this output stays silent", a->id, (int)st);
        if (playing) { if (a->au && !g_suspended) { a->stalled = 0; au_go(a, now_ns()); } else null_start(a); }
    }
    LOG("audio outputs set up again after the media services reset");
    pthread_mutex_unlock(&g_aos_lock);
}
