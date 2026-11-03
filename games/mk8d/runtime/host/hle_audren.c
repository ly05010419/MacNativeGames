// nn::audio AudioRenderer (audren) -- in-process rendering: use the complete renderer the game's SDK ships with, no mixer of our own.
// Newer SDKs link the renderer server side (nn::audio::server: InfoUpdater / CommandGenerator ...) and the CPU version of the DSP command processing (nn::audio::dsp::CommandListProcessor:
// ADPCM / PCM decoding, resampling, mixing, reverb / delay / biquad / limiter / compressor ...) into the application, for the "CPU rendering + manual execution" mode (AudioRendererParameter
// +0x2c renderingDevice = 1 CPU, +0x30 executionMode = 1 manual). In this mode OpenAudioRenderer goes through the in-process DFC manager (CreateAudioRendererManagerByDfc),
// RequestUpdate is parsed in-process, and rendering happens on the CPU of whichever thread calls nn::audio::ExecuteAudioRendererRendering, which then signals the game's renderer event.
// The host does three things:
//   1. Takes over GetAudioRendererWorkBufferSize / OpenAudioRenderer: preserves the DSP service's work-buffer requirement, then runs the SDK's CPU + manual renderer.
//      The local SDK uses a different command format whose storage can exceed the service's budget; that backend storage is supplied by the host. Explicit CPU renderers keep the SDK contract;
//   2. A render thread (a guest thread started by the host): renders one frame whenever the output hands back a 5 ms buffer, so pacing follows the sound card clock;
//   3. Takes over AudioRendererImpl::ExecuteAudioRendererRendering (reached through the PLT): generates commands and runs CommandListProcessor like the original,
//      then finds the DeviceSink command in the command list (a no-op for the CPU processor) and clamps its input mix buffers (int32 holding 16-bit range) to int16 stereo for AudioOut (hle_audio.c).
// Command list format (SDK 16): header +0x04 command count, +0x08 mix buffer address, +0x10 buffer count, +0x14 samples per frame, +0x18 sample rate, commands from +0x20;
//   each command +0x00 magic 0xcafebabe, +0x04 enabled, +0x05 command ID (DeviceSink = 0x13), +0x06 size; DeviceSink +0x128 upsampler buffer, +0x130 input count, +0x134 input indices (s16).
#include "hle.h"
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size);
void dm_guest_thread_join(void *tt);
void *dm_pcmout_open(int ch);
void dm_pcmout_close(void *k);
void dm_pcmout_start(void *k);
int dm_pcmout_append(void *k, void *b);
void *dm_pcmout_released(void *k);
void dm_pcmout_wait(void *k, int ms);

// P_SIZE: bytes of the game's AudioRendererParameter copied before the two fields are changed. SDK 16's struct is 0x38 bytes; SDK 8's (Link's Awakening) has one more word,
// its revision at +0x38: copying only 0x38 left the revision uninitialized and the SDK rejected it (CheckValidRevision, Result 0x499). The SDK reads only its own size of the copy
enum { P_DEVICE = 0x2c, P_MODE = 0x30, P_SIZE = 0x40, WORK_PAGE = 0x1000, FRAME_MAX = 1024, NBUF = 4 };
static void *gsym(const char *n) { void *p = guest_sym(n); if (!p) die("audren: missing SDK symbol %s", n); return p; }
static void cpu_manual(uint8_t *dst, const uint8_t *src) { memcpy(dst, src, P_SIZE); *(int32_t *)(dst + P_DEVICE) = 1; *(int32_t *)(dst + P_MODE) = 1; }

static size_t ar_align(size_t n, size_t alignment) { return (n + alignment - 1) & ~(alignment - 1); }
// Horizon's renderer service budget, through renderer revision 15. Its command/state representation is independent of the application's SDK: an older client still uses the
// service's compact commands. The SDK's local server instead sizes its own CPU command structures, so forwarding its size inflates the guest's service requirement.
// Derived from the service's sizing and revision gates; checked against its isolated machine-code execution. No firmware code is needed at runtime. Unknown revisions retain
// the SDK requirement. SDK 8 keeps the public parameter revision at +0x38, while SDK 9+ uses +0x34 (the copied parameter holds both).
static size_t ar_service_work_size(const uint8_t *p) {
    uint32_t revision = *(const uint32_t *)(p + 0x34);
    if ((revision & 0xffffff) != 0x564552) revision = *(const uint32_t *)(p + 0x38);
    if ((revision & 0xffffff) != 0x564552 || revision < 0x31564552 || revision > 0x3f564552) return 0;
    unsigned r = (revision >> 24) - 0x30;
    size_t n[11];
    for (int i = 0; i < 11; i++) {
        if (i == 8) { n[i] = p[0x20]; continue; }   // voice-drop is a byte; the following public-struct padding is not initialized
        int32_t v = ((const int32_t *)p)[i];
        if (v < 0 || v > 0x100000) return 0;   // bound arithmetic even if the caller supplied an invalid parameter
        n[i] = (size_t)v;
    }
    size_t mix = n[2], sub = n[3], voices = n[4], sinks = n[5], effects = n[6], perf = n[7], splitters = n[9], dest = n[10], mixes = sub + 1;
    size_t size = ar_align(((sinks + sub) * 0x3c0 + n[1] * 4) * (mix + 6), 64);
    size += ar_align(mix * 4, 64) + sub * 0x400 + mixes * 0x940 + ar_align(mixes * 8, 16);
    size += voices * 0x1d0 + ar_align(voices * (r >= 15 ? 0x238 : 0x220), 16) + ar_align(voices * 8, 16);
    if (r >= 2) {
        size_t graph = mixes * mixes * 4 + mixes * 12 + 2 * ar_align(mixes, 64) / 8 + ar_align(mixes * mixes, 64) / 8;
        size += ar_align(graph, 16);
        size += splitters * 0x20 + ar_align(dest * (r >= 12 ? (r >= 15 ? 0x130 : 8) : 0xe0), 16);
        if (r < 5) size += ar_align(dest * 4, 16);
    }
    size += (effects + voices * 4) * 0x20 + 0x50;
    if (r >= 12 && splitters && dest) size += ar_align(dest * (r >= 14 ? 0xe0 : 0x80), 64);
    size = ar_align(size + (r >= 9 ? effects * 0x80 : 0), 64);
    size += (sinks + sub) * 0x280 + effects * (r >= 9 ? 0x530 : 0x4b0) + sinks * 0x170 + voices * 0x100 + 0x40;
    if (perf) {
        size_t frame = (sub + voices + sinks + effects + 1) * (r >= 5 ? 0x18 : 0x10) + (r >= 5 ? 0x990 : 0x658);
        size += ar_align(frame * (perf + 1), 64) + 0xc0;
    }
    size_t commands = 0x18000;
    if (r >= 5) commands = effects * 0x540 + sub * 0x3f24 + sinks * 0x130 + dest * 0x360
        + voices * (splitters * 0x50 + (r >= 8 ? 0x284 : 0x250)) + (voices + sub + effects + sinks + 0x65) * 0x28 + 0x2c4;
    return ar_align(size + commands + 0x7e, WORK_PAGE);
}

HLE_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "renderer work buffer: DSP service budget, with extra local CPU backend storage supplied by the host",
          size_t, h_ar_WorkSize, const uint8_t *param) {
    static size_t (*orig)(const uint8_t *); if (!orig) orig = gsym("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE");
    uint8_t p[P_SIZE]; cpu_manual(p, param); size_t n = orig(p);
    size_t service = *(int32_t *)(param + P_DEVICE) == 0 ? ar_service_work_size(p) : 0;
    size_t guest_n = service ? service : n;
    { static int once; if (!once++) LOG("audren: work buffer guest %#zx, local CPU/manual renderer %#zx", guest_n, n); }
    return guest_n;
}
// ---------- rendering and output ----------
static void *g_handle[2];                    // AudioRendererHandle (renderer object, client state)
static _Atomic int g_open;
static void *g_render_tt;                    // the render thread (ThreadType from dm_guest_thread_spawn): Close joins it before the SDK tears the renderer down
static int16_t g_frame[FRAME_MAX * 2]; static int g_frame_n;   // the latest frame (stereo int16)
static _Atomic uint64_t g_frames, g_nonsilent;
enum { CQ = 4 }; static int16_t g_cq[CQ][FRAME_MAX * 2]; static int g_cq_n[CQ]; static _Atomic uint32_t g_cq_head, g_cq_tail;   // output queue of catch-up frames
static pthread_mutex_t g_cq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_cq_cv = PTHREAD_COND_INITIALIZER;   // the render thread signals each frame it takes off the queue (catch-up pacing)
static void cq_popped(void) { pthread_mutex_lock(&g_cq_mu); pthread_cond_broadcast(&g_cq_cv); pthread_mutex_unlock(&g_cq_mu); }

typedef struct { void *ptr; uint64_t cap, size; } gbuf;       // same as the front of hle_audio.c's AudioOutBuffer
static void render_main(void *arg) {
    (void)arg;
    void (*exec)(void *, void *) = gsym("_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE");
    void *out = dm_pcmout_open(2);
    static int16_t data[NBUF][FRAME_MAX * 2]; static gbuf bufs[NBUF]; gbuf *free_list[NBUF]; int nfree = NBUF;
    for (int i = 0; i < NBUF; i++) { bufs[i] = (gbuf){data[i], sizeof data[i], 0}; free_list[i] = &bufs[i]; }
    dm_pcmout_start(out);
    LOG("audren: render thread started (%d buffers in flight, paced by the output)", NBUF);
    while (atomic_load(&g_open)) {
        while (!nfree) {   // wait for the output to hand a block back (silent sink / sound card, one every 5 ms); it signals each release, so the thread sleeps until then (it used to poll every 1 ms)
            gbuf *b = dm_pcmout_released(out);
            if (b) { free_list[nfree++] = b; break; }
            dm_pcmout_wait(out, 10);
            if (!atomic_load(&g_open)) break;
        }
        if (!nfree) break;
        const int16_t *src; int got;
        uint32_t h = atomic_load(&g_cq_head);
        if (h != atomic_load(&g_cq_tail)) { src = g_cq[h % CQ]; got = g_cq_n[h % CQ]; atomic_store(&g_cq_head, h + 1); cq_popped(); }   // play queued catch-up output first
        else { g_frame_n = 0; exec(g_handle[0], g_handle[1]); src = g_frame; got = g_frame_n; }   // SDK: generate commands → CPU processing (output taken in the host takeover) → signal the game's renderer event
        int n = got ? got : 240;
        gbuf *b = free_list[--nfree];
        if (got) memcpy(b->ptr, src, (size_t)n * 4); else memset(b->ptr, 0, (size_t)n * 4);
        b->size = (uint64_t)n * 4;
        if (!dm_pcmout_append(out, b)) free_list[nfree++] = b;
    }
    dm_pcmout_close(out);   // the output would otherwise keep its sound card callback or silent sink running and hold one of the AudioOut slots after the renderer closes
    cq_popped();   // an update waiting for queue space (catch-up pacing) sees g_open cleared
}

// Both OpenAudioRenderer forms: CPU rendering + manual execution, then the host's render thread. With the event the game waits on it, which each render signals; without it the
// game paces its updates itself (Balatro: LÖVE's OpenAL Soft) and still needs the renderer to run every 5 ms: the form without an event used to start no render thread, so only the
// game's updates rendered (catch-up frames, which nothing played) and the game was silent.
// SDK 17.5.3's event form opens the renderer by calling the form without one through the PLT (sdk+0x1e489c, ROMANCE OF THE THREE KINGDOMS 8 REMAKE), which the loader binds to the
// hook below since that form has one (Balatro): the inner call started the render thread before the event form had created its event, and the outer call then logged a second
// renderer. The inner call (t_in_open) goes straight to the SDK with the host's copy of the parameter, as it did before the form without an event was hooked
static __thread int t_in_open;
static Result ar_open(void **handle, void *ev, const uint8_t *param, void *work, size_t sz) {
    static Result (*orig_ev)(void **, void *, const uint8_t *, void *, size_t), (*orig)(void **, const uint8_t *, void *, size_t);
    if (!orig_ev) orig_ev = gsym("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm");
    if (!orig) orig = gsym("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm");
    if (t_in_open) return orig(handle, param, work, sz);
    uint8_t p[P_SIZE]; cpu_manual(p, param);
    LOG("audren: OpenAudioRenderer%s rate %d samples per frame %d mix buffers %d submixes %d voices %d sinks %d effects %d splitters %d (original device %d mode %d → CPU / manual) work buffer %p+%#zx",
        ev ? "" : " (no event)", *(int32_t *)param, *(int32_t *)(param + 4), *(int32_t *)(param + 8), *(int32_t *)(param + 0xc), *(int32_t *)(param + 0x10), *(int32_t *)(param + 0x14),
        *(int32_t *)(param + 0x18), *(int32_t *)(param + 0x24), *(int32_t *)(param + P_DEVICE), *(int32_t *)(param + P_MODE), work, sz);
    // The application's buffer follows the DSP service contract; the local server's larger CPU command representation is a backend cost. Keep a full-size caller buffer
    // when it fits the backend, otherwise supply the backend storage here. The nested no-event open receives this same buffer and starts no second thread.
    void *owned = NULL;
    if (*(int32_t *)(param + P_DEVICE) == 0 && work && !((uintptr_t)work & (WORK_PAGE - 1)) && sz >= WORK_PAGE && !(sz & (WORK_PAGE - 1))) {
        size_t (*size)(const uint8_t *) = gsym("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE");
        size_t need = size(p);
        size_t service = ar_service_work_size(p), guest_need = service ? service : need;
        if (sz < guest_need) { LOG("[RED] audren: work buffer %#zx is smaller than the service requirement %#zx", sz, guest_need); return RES_FAIL; }
        if (need > sz) {
            if (posix_memalign(&owned, WORK_PAGE, need)) { LOG("[RED] audren: cannot allocate local renderer work buffer %#zx", need); return RES_FAIL; }
            memset(owned, 0, need); work = owned; sz = need;
            LOG("audren: host backs local renderer work buffer %p+%#zx", work, sz);
        }
    }
    t_in_open = 1; Result r = ev ? orig_ev(handle, ev, p, work, sz) : orig(handle, p, work, sz); t_in_open = 0;
    if (r) { free(owned); LOG("[RED] audren: SDK OpenAudioRenderer failed %#x", r); return r; }
    if (owned) objmap_put(handle[0], owned);   // Close releases it only after the render thread and the SDK have finished with it
    // The renderer event as the service gives it. With the DSP service (device 0, what the game asked for) OpenAudioRenderer attaches the service's kernel event (auto-clear,
    // AttachReadableHandleToSystemEvent); with CPU + manual it creates a local one instead (CreateSystemEvent(ev, AutoClear, false), SDK 4: sdk+0x190b6c). A signal of a local
    // auto-clear event wakes only its highest-priority waiter, a kernel event wakes every waiter (each tries ResetSignal). Donkey Kong Country: Tropical Freeze waits on it from
    // two threads: its audio thread (priority 29) every frame, and the main thread while CAudioMemoryPool::Shutdown detaches a pool as a level loads (main+0x6fa8c); the audio
    // thread took every wake and the main thread waited forever. The renderer signals the event through os::SignalSystemEvent, which takes either kind
    if (ev && *(int32_t *)(param + P_DEVICE) == 0) {
        static void (*destroy)(void *); if (!destroy) destroy = gsym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
        destroy(ev); hle_bind_system_event(ev, 1);
    }
    if (atomic_load(&g_open)) { LOG("[RED] audren: a second renderer (the host drives only one)"); return r; }
    g_handle[0] = handle[0]; g_handle[1] = handle[1]; atomic_store(&g_open, 1);
    g_render_tt = dm_guest_thread_spawn(render_main, NULL, 9, 0x10000);
    return r;
}
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "open the renderer (with event): CPU rendering + manual execution, the host starts a render thread",
          Result, h_ar_Open, void **handle, void *ev, const uint8_t *param, void *work, size_t sz) { return ar_open(handle, ev, param, work, sz); }
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm", "open the renderer (no event): CPU rendering + manual execution, the host starts a render thread",
          Result, h_ar_Open0, void **handle, const uint8_t *param, void *work, size_t sz) { return ar_open(handle, NULL, param, work, sz); }
HLE_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "close the renderer: stop the host render thread first, then hand over to the SDK", void, h_ar_Close, void *obj, void *state) {
    static void (*orig)(void *, void *); if (!orig) orig = gsym("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE");
    // Join the render thread first (it checks g_open every round and at most every 10 ms while waiting for the output): it may be inside exec(), which uses the renderer the SDK is about to free.
    // A fixed 20 ms sleep used to stand in for this, and a renderer reopened inside that window ran a second thread over the first one's static buffers
    if (obj == g_handle[0] && g_render_tt) { atomic_store(&g_open, 0); dm_guest_thread_join(g_render_tt); g_render_tt = NULL; }
    void *owned = objmap_del(obj);
    orig(obj, state);
    free(owned);
}

// DeviceSink input mix buffers → int16 stereo. The command ID is 0x13 in SDK 16; SDK 20 inserted a command before it, making it 0x14 (written at sdk+0x233f44); SDK 8 (Link's Awakening)
//   has four fewer commands before it, making it 0x10. The layout is the same in all three: 0x148 bytes, +0x20 device name ("MainAudioOut"), +0x128 upsample buffer, +0x130 input count,
//   +0x134 input indices. render_once sets the ID by layout; when the first full command list has no command with that ID, its one 0x148-byte command with 1 to 6 inputs is the sink
//   (without this, Link's Awakening rendered every frame and played silence: the music and sound effects never reached the output, only the movies' own AudioOut did)
// SDK 6 (Fire Emblem: Three Houses, CommandListProcessor::Process at sdk+0xaaa00) lays out the command header differently: +0x04 size (s16), +0x06 command ID (s16), +0x10 enabled (u8),
//   where SDK 8–20 have +0x04 enabled (u8), +0x05 ID (u8), +0x06 size (s16); DeviceSink is ID 16 there (its case in Process is a no-op). Walked with the newer header, the first command's ID
//   was taken as its size, the second "command" had no magic and the game was silent. The first list that walks in a supported layout decides the header; an incomplete startup list decides nothing
// SDK 21 (Rhythm Heaven Groove, CommandListProcessor::Process at sdk+0x24fd50) has no magic: +0x00 enabled (bit 0), +0x01 command ID (u8), +0x02 size (s16), +0x04 estimated time, +0x08 node ID,
//   and every command body sits 8 bytes earlier. DeviceSink is still ID 0x14 but 0x140 bytes (written at sdk+0x23a7b0): +0x18 device name, +0x120 upsample buffer, +0x128 input count, +0x12c input
//   indices. Walked as SDK 20, the first command's enabled / ID / size bytes read as a magic of 0x181901 and the game was silent. With no magic, that walk must stay inside the generated list
// SDK 4.4.0 (Mario Kart 8 Deluxe, GenerateDeviceSinkCommand at sdk+0x19e690) has the SDK 6 header, and its DeviceSink is ID 0xf and 0x130 bytes: +0x14 device name, +0x118 upsample buffer,
//   +0x120 input count, +0x124 input indices as bytes (s8). No 0x148-byte command, so the first full command list's one 0x130-byte command with 1 to 6 inputs is the sink (g_sink4)
static int g_sink_id = 0x13, g_hdr6, g_hdr21, g_sink4;
// Probing the wrong layout can advance by an unaligned size: decode wire fields without typed loads.
static int cmd_size(const uint8_t *q) { int16_t n; memcpy(&n, q + (g_hdr21 ? 2 : g_hdr6 ? 4 : 6), sizeof n); return n; }
static int cmd_id(const uint8_t *q) { int16_t id; if (g_hdr21) return q[1]; if (g_hdr6) { memcpy(&id, q + 6, sizeof id); return id; } return q[5]; }
static int cmd_on(const uint8_t *q) { return g_hdr21 ? q[0] & 1 : g_hdr6 ? q[0x10] : q[4]; }
static int cmd_ok(const uint8_t *q, const uint8_t *end) {
    size_t header = g_hdr21 ? 0x10 : g_hdr6 ? 0x11 : 8;
    if (q > end || (size_t)(end - q) < header) return 0;
    if (!g_hdr21) { uint32_t magic; memcpy(&magic, q, sizeof magic); if (magic != 0xcafebabeu) return 0; }
    int n = cmd_size(q);
    return n >= (int)header && (size_t)n <= (size_t)(end - q);
}
static int sink_body(void) { return g_hdr21 ? -8 : 0; }   // where DeviceSink's fields sit relative to SDK 8–20
static int cmd_walks(const uint8_t *cl, uint32_t ncmd, const uint8_t *end) {
    const uint8_t *q = cl + 0x20;
    for (uint32_t i = 0; i < ncmd; i++) { if (!cmd_ok(q, end)) return 0; q += cmd_size(q); }
    return 1;
}
// A renderer at 32 kHz (DRAGON QUEST VII Reimagined: 160 samples per 5 ms frame) has its DeviceSink upsampled to the output's 48 kHz by the SDK's upsampler, which AudioRenderSystem::
//   SetUpRenderResourceSet allocates only for the DSP (SDK 19: sdk+0x21eb6c, sample rate 0x7d00 and rendering device +0x48 == 0). With the host's CPU rendering there is no
//   UpsamplerManager, and the sink's command generation (GenerateSinkCommands, sdk+0x225a80) called Allocate on null and crashed in its mutex. The takeover below answers "no upsampler"
//   for a null manager (the generator then skips the upsample command, sdk+0x225b40), and the host upsamples the stereo downmix 3:2 itself: a windowed sinc (Blackman, 16 taps, cutoff 0.9 of
//   the input's Nyquist) in three phases, evaluated 8 input samples late so every tap is known, with the last 15 samples kept across frames
HLE_FORCE("_ZN2nn5audio6server16UpsamplerManager8AllocateEv", "upsampler for a DeviceSink below 48 kHz: none with CPU rendering (no manager), the host resamples its output",
          void *, h_up_Allocate, void *mgr) {
    static void *(*orig)(void *); if (!orig) orig = gsym("_ZN2nn5audio6server16UpsamplerManager8AllocateEv");
    return mgr ? orig(mgr) : NULL;
}
enum { RS_T = 16 };
static uint32_t up_3_2(const int32_t *in, uint32_t n, int32_t *out) {
    static float h[3][RS_T]; static int32_t buf[2 * (RS_T - 1 + FRAME_MAX)]; static uint32_t pos;   // pos: the next output's position in thirds of an input sample, from the frame start
    if (!h[0][RS_T / 2 - 1]) {
        for (int p = 0; p < 3; p++) {
            double sum = 0, w[RS_T];
            for (int k = 0; k < RS_T; k++) {
                double x = p / 3.0 + RS_T / 2 - 1 - k, fc = 0.9, s = x == 0 ? fc : sin(M_PI * fc * x) / (M_PI * x);
                double t = (x + RS_T / 2) / RS_T, win = 0.42 - 0.5 * cos(2 * M_PI * t) + 0.08 * cos(4 * M_PI * t);
                w[k] = s * win; sum += w[k];
            }
            for (int k = 0; k < RS_T; k++) h[p][k] = (float)(w[k] / sum);
        }
    }
    memcpy(buf + 2 * (RS_T - 1), in, (size_t)n * 8);   // the frame after the last RS_T - 1 samples of the one before (copied first: out may be in)
    uint32_t m = 0;
    for (; pos < 3 * n; pos += 2, m++) {
        const int32_t *x = buf + 2 * (pos / 3); const float *c = h[pos % 3]; float l = 0, r = 0;
        for (int k = 0; k < RS_T; k++) { l += c[k] * (float)x[2 * k]; r += c[k] * (float)x[2 * k + 1]; }
        out[2 * m] = (int32_t)lrintf(l); out[2 * m + 1] = (int32_t)lrintf(r);
    }
    pos -= 3 * n;
    memmove(buf, buf + 2 * n, (RS_T - 1) * 8);
    return m;
}
static int capture(const uint8_t *cl, size_t len, int16_t *dst) {
    if (len < 0x20) return 0;
    uint32_t ncmd = *(const uint32_t *)(cl + 4), samples = *(const uint32_t *)(cl + 0x14), rate = *(const uint32_t *)(cl + 0x18);
    const int32_t *mix = *(const int32_t *const *)(cl + 8); const uint8_t *cend = cl + len;
    static int logged, hist_logged, layout_done;
    if (!layout_done && ncmd) {   // the header layout, from the first list that walks in one (SDK 21's first lists have 4 commands or fewer, which the old layout would report as malformed)
        // A list generated before the game's first update need not walk in any layout: Mario Kart 8 Deluxe's (SDK 4.4.0) first list, made by the render thread before the first
        // RequestUpdate, ends in a zeroed command (first word 0). Taken as the layout, it left the SDK 6 headers read in the newer layout and the game silent; such a list is skipped
        if (cmd_walks(cl, ncmd, cend)) layout_done = 1;
        else { g_hdr6 = 1; if (cmd_walks(cl, ncmd, cend)) { layout_done = 1; g_sink_id = 0x10; LOG("audren: command headers in the SDK 6 layout (+4 size, +6 command ID, +0x10 enabled), DeviceSink is ID 0x10"); } else g_hdr6 = 0; }
        if (!layout_done) { g_hdr21 = 1; if (cmd_walks(cl, ncmd, cend)) { layout_done = 1; LOG("audren: command headers in the SDK 21 layout (no magic: +0 enabled, +1 command ID, +2 size), DeviceSink 0x140 bytes"); } else g_hdr21 = 0; }
        if (!layout_done) { static int skipped; if (!skipped++) LOG("audren: command list %u commands long walks in no header layout (generated before the first update?): silent frame, the layout is taken from a later list", ncmd); return 0; }
    }
    if (!layout_done) return 0;
    if (!hist_logged && ncmd > 4) {   // first full command list: command ID histogram and the sink ID check
        hist_logged = 1;
        int hist[64] = {0}, cand = -1, ncand = 0, cand4 = -1, ncand4 = 0, sink_sz = 0x148 + sink_body(); const uint8_t *q = cl + 0x20; char line[512]; size_t o = 0;
        for (uint32_t i = 0; i < ncmd && cmd_ok(q, cend); i++) {
            int sz = cmd_size(q); hist[cmd_id(q) & 63]++;
            if (sz == sink_sz && *(const uint32_t *)(q + 0x130 + sink_body()) - 1 < 6) { cand = cmd_id(q); ncand++; }
            if (g_hdr6 && sz == 0x130 && *(const uint32_t *)(q + 0x120) - 1 < 6) { cand4 = cmd_id(q); ncand4++; }
            q += sz;
        }
        for (int k = 0; k < 64; k++) if (hist[k]) o += snprintf(line + o, sizeof line - o, " %d×%d", k, hist[k]);
        LOG("audren: first-frame commands %u, by command ID: %s", ncmd, line);
        if (!hist[g_sink_id & 63] && ncand == 1) { LOG("audren: no command with ID %#x in the command list; the only 0x148-byte command is ID %#x, taking it as DeviceSink", g_sink_id, cand); g_sink_id = cand; }
        else if (!hist[g_sink_id & 63] && !ncand && ncand4 == 1) { LOG("audren: no command with ID %#x in the command list; the only 0x130-byte command is ID %#x, taking it as the SDK 4 DeviceSink", g_sink_id, cand4); g_sink_id = cand4; g_sink4 = 1; }
        else if (!hist[g_sink_id & 63]) LOG("[RED] audren: no DeviceSink (ID %#x) in the command list, %d commands of %#x bytes: the game will have no sound", g_sink_id, ncand, sink_sz);
    }
    const uint8_t *c = cl + 0x20;
    for (uint32_t i = 0; i < ncmd; i++) {
        if (!cmd_ok(c, cend)) { if (!logged++) LOG("[RED] audren: command %u in the command list is malformed (offset %#zx, list %#zx bytes)", i, (size_t)(c - cl), len); return 0; }
        int size = cmd_size(c);
        if (cmd_id(c) == g_sink_id && cmd_on(c)) {
            const uint8_t *b = c + sink_body();
            uint32_t nin = *(const uint32_t *)(b + (g_sink4 ? 0x120 : 0x130)); int16_t in[6];
            for (uint32_t j = 0; j < 6; j++) in[j] = g_sink4 ? ((const int8_t *)(b + 0x124))[j] : ((const int16_t *)(b + 0x134))[j];
            const int32_t *up = *(const int32_t *const *)(b + (g_sink4 ? 0x118 : 0x128));
            if (!logged++) LOG("audren: DeviceSink inputs %u (%d %d …) rate %u samples per frame %u mix buffer %p upsample buffer %p%s", nin, nin ? in[0] : -1, nin > 1 ? in[1] : -1, rate, samples, (void *)mix, (void *)up,
                               rate == 32000 ? " (32 kHz: resampled to 48 kHz on the host)" : "");
            if (!nin || samples > (rate == 48000 ? FRAME_MAX : FRAME_MAX * 2 / 3 - 1)) return 0;
            if (rate != 48000 && rate != 32000) { static int once; if (!once++) LOG("[RED] audren: renderer sample rate %u (the SDK takes 32000 or 48000): no sound", rate); return 0; }
            // Input buffers: the mix buffers the DeviceSink inputs name, at the renderer's rate (with CPU rendering the SDK makes no upsampler, below)
            const int32_t *ch[6]; uint32_t k = nin > 6 ? 6 : nin;
            for (uint32_t j = 0; j < k; j++) ch[j] = mix + (size_t)in[j] * samples;
            // Channel order front-left front-right center LFE back-left back-right; 6 → 2 with the system default downmix coefficients (front 1.0, center 0.707, LFE 0.251, back 0.707, Q15)
            enum { CF = 32768, CC = 23167, CL = 8224, CB = 23167 };
            static int32_t st[FRAME_MAX * 2];   // the stereo downmix before clamping (capture runs under g_render_mu)
            for (uint32_t s = 0; s < samples; s++) {
                int32_t l, r;
                if (k >= 6) { int64_t c = (int64_t)ch[2][s] * CC + (int64_t)ch[3][s] * CL;
                              l = (int32_t)(((int64_t)ch[0][s] * CF + c + (int64_t)ch[4][s] * CB) >> 15); r = (int32_t)(((int64_t)ch[1][s] * CF + c + (int64_t)ch[5][s] * CB) >> 15); }
                else { l = ch[0][s]; r = k > 1 ? ch[1][s] : l; }
                st[2 * s] = l; st[2 * s + 1] = r;
            }
            uint32_t nout = rate == 48000 ? samples : up_3_2(st, samples, st);
            uint64_t loud = 0;
            for (uint32_t s = 0; s < 2 * nout; s++) {
                int32_t v = st[s]; v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                dst[s] = (int16_t)v; loud |= (uint64_t)v;
            }
            atomic_fetch_add(&g_frames, 1); if (loud) atomic_fetch_add(&g_nonsilent, 1);
            uint64_t f = atomic_load(&g_frames); if (f == 200 || f % 12000 == 0) LOG("audren: rendered %llu frames, %llu of them not silent", (unsigned long long)f, (unsigned long long)atomic_load(&g_nonsilent));
            return (int)nout;
        }
        c += size;
    }
    return 0;
}
// Render one frame (generate commands + CPU processing); dst = output (stereo int16, *nout samples). The two callers (render thread, catch-up) are serialized by a host lock: they share one command buffer
static pthread_mutex_t g_render_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_updates, g_renders, g_catchups;
// SDK 20's AudioRenderSystem (The Legend of Zelda: Breath of the Wild 1.9.0) has another layout: the member functions for mode / device / command buffer are inlined (sdk+0x22a70c ExecuteAudioRendererRendering:
//   +0x48 rendering device and +0x4c execution mode, both must be 1; +0xf8 / +0x100 command buffer start / length), GenerateCommand takes a Span<std::byte> and returns a Span (x0 start, x1 length: the generated command list),
//   and the mutex UpdateImpl holds is at +0x2d0 (+0x1f0 in SDK 16). The layout is recognized by the presence of the Span version of GenerateCommand
typedef struct { uint8_t *p; size_t n; } span_t;
// The update lock before SDK 19 is the one AudioRenderSystem::Update takes at its start, read off the SDK's own code: nn::os::LockMutex(MutexType*) at +0x1f8 in SDK 3.5.1 (OCTOPATH TRAVELER), +0x1d0 in SDK 4.4.0 (Mario Kart 8 Deluxe)
//   and at +0x1d8 in SDK 6.4–8.2, SdkMutexType::Lock at +0x1d8 in SDK 10.4–11.4, +0x1f0 in 13.3–16.2, +0x1f8 in 17.5 (the sdk builds of 24 games on hand; 19 and 20 inline it).
//   The SDK 16 layout (SdkMutexType at +0x1f0) taken for all of them wrote a lock word into another field and excluded nothing. The prologue copies `this`, adds the offset into a register
//   and moves it to x0 before the first bl, a PLT stub whose GOT slot the loader bound to one of the two functions; anything else keeps the SDK 16 layout
static void *plt_target(const uint32_t *pc) {   // adrp x16, page; ldr x17, [x16, #off]; …; br x17 → the function the GOT slot holds; else the address itself
    uint32_t a = pc[0], l = pc[1];
    if ((a & 0x9f00001fu) != 0x90000010u || (l & 0xffc003ffu) != 0xf9400211u) return (void *)pc;
    int64_t imm = (int64_t)((uint64_t)((((a >> 5) & 0x7ffff) << 2) | ((a >> 29) & 3)) << 43) >> 31;   // immhi:immlo, sign-extended, × 4 KB
    uint64_t page = ((uint64_t)pc & ~0xfffull) + (uint64_t)imm;
    return *(void **)(page + ((l >> 10) & 0xfff) * 8);
}
static int update_lock(size_t *off, int *os_mutex) {
    const uint32_t *f = guest_sym("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm");
    void *lock_mutex = guest_sym("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"), *sdk_lock = guest_sym("_ZN2nn2os12SdkMutexType4LockEv");
    if (!f) return 0;
    uint32_t self = 1u << 0; int64_t add[32]; for (int r = 0; r < 32; r++) add[r] = -1;
    int64_t x0 = -1;
    for (int i = 0; i < 24; i++) {
        uint32_t w = f[i]; int rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
        if ((w & 0xffe0ffe0u) == 0xaa0003e0u) {   // mov xd, xm
            if (self >> rm & 1) self |= 1u << rd;
            if (rd == 0) x0 = add[rm];
            else add[rd] = add[rm];
        } else if ((w & 0xffc00000u) == 0x91000000u && rn != 31 && (self >> rn & 1)) {   // add xd, x<this>, #imm12
            add[rd] = (w >> 10) & 0xfff; if (rd == 0) x0 = add[rd];
        } else if ((w & 0xfc000000u) == 0x94000000u) {   // the first bl
            void *t = plt_target(f + i + (int32_t)(w << 6) / 64);
            if (x0 < 0 || !t || (t != lock_mutex && t != sdk_lock)) return 0;
            *off = (size_t)x0; *os_mutex = t == lock_mutex; return 1;
        }
    }
    return 0;
}
static Result render_once(uint8_t *sys, int16_t *dst, int *nout) {
    static int (*mode)(void *), (*device)(void *); static uint8_t *(*cbuf)(void *); static size_t (*cbsz)(void *), (*gen)(void *, void *, size_t);
    static span_t (*gen20)(void *, void *, size_t); static size_t mu_off;
    static void (*tmax)(void *, uint32_t), (*setup)(void *, const void *, size_t), (*process)(void *), (*lock)(void *), (*unlock)(void *);
    if (!process) {
        gen20 =guest_sym("_ZN2nn5audio6server17AudioRenderSystem15GenerateCommandENS_4SpanISt4byteEE");
        if (!gen20) {
            mode = gsym("_ZNK2nn5audio6server17AudioRenderSystem16GetExecutionModeEv"); device = gsym("_ZNK2nn5audio6server17AudioRenderSystem18GetRenderingDeviceEv");
            cbuf = gsym("_ZNK2nn5audio6server17AudioRenderSystem26GetCommandBufferWorkBufferEv"); cbsz = gsym("_ZNK2nn5audio6server17AudioRenderSystem30GetCommandBufferWorkBufferSizeEv");
            gen = gsym("_ZN2nn5audio6server17AudioRenderSystem15GenerateCommandEPvm");
        }
        mu_off = gen20 ? 0x2d0 : 0x1f0; g_sink_id = gen20 ? 0x14 : 0x13;
        int os_mutex = 0;
        // SdkMutexType is looked up only when the SDK uses it: SDK 3.5.1 has none (OCTOPATH TRAVELER: LockMutex at +0x1f8)
        if (!gen20 && update_lock(&mu_off, &os_mutex) && os_mutex) { lock = gsym("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"); unlock = gsym("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE"); }
        else { lock = gsym("_ZN2nn2os12SdkMutexType4LockEv"); unlock = gsym("_ZN2nn2os12SdkMutexType6UnlockEv"); }
        LOG("audren: AudioRenderSystem in the SDK %s layout (update lock %s at +%#zx)", gen20 ? "20" : "16", os_mutex ? "nn::os::MutexType" : "SdkMutexType", mu_off);
        tmax = gsym("_ZN2nn5audio3dsp20CommandListProcessor17SetProcessTimeMaxEj"); setup = gsym("_ZN2nn5audio3dsp20CommandListProcessor5SetupEPKvm");
        process = gsym("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv");
    }
    int md = gen20 ? *(int32_t *)(sys + 0x4c) : mode(sys), dv = gen20 ? *(int32_t *)(sys + 0x48) : device(sys);
    if (md != 1 || dv != 1) return 0x40299;   // same as the original: manual rendering needs CPU + manual mode
    pthread_mutex_lock(&g_render_mu);
    uint64_t t0 = now_ns();
    // Command generation must exclude the game thread's RequestUpdate (AudioRenderSystem::Update, which holds the mutex at mu_off): the original system thread (SendCommandToDsp) generates under the same lock. Processing (the CPU DSP) stays outside the lock, as in the original.
    uint8_t *cl; size_t n;
    lock(sys + mu_off);
    if (gen20) { span_t sp = gen20(sys, *(uint8_t **)(sys + 0xf8), *(size_t *)(sys + 0x100)); cl = sp.p; n = sp.n; }
    else { cl = cbuf(sys); n = gen(sys, cl, cbsz(sys)); }
    unlock(sys + mu_off);
    atomic_fetch_add(&g_renders, 1);
    uint64_t proc[16] __attribute__((aligned(16))) = {0};   // CommandListProcessor (0x58 bytes; 0x60 in SDK 21, whose Process reads +0x58)
    tmax(proc, 0xffffffffu); setup(proc, cl, n); process(proc);
    if (dst) *nout = capture(cl, n, dst);
    // render time per frame (budget: 5 ms per frame): mean / max every 2000 frames
    static uint64_t sum, mx, cnt; uint64_t d = now_ns() - t0; sum += d; if (d > mx) mx = d;
    if (++cnt == 2000) { LOG("audren: render time mean %.2f ms / max %.2f ms (5 ms budget per frame, last 2000 frames); catch-up renders so far %llu", sum / 2000 / 1e6, mx / 1e6, (unsigned long long)atomic_load(&g_catchups)); sum = mx = cnt = 0; }
    pthread_mutex_unlock(&g_render_mu);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio6server24AudioRendererManagerImpl17AudioRendererImpl29ExecuteAudioRendererRenderingEv", "render one frame manually: SDK generates commands + CPU processing, the host takes the DeviceSink output",
          Result, h_ar_ImplExec, uint8_t *impl) { return render_once(impl + 0x60, g_frame, &g_frame_n); }   // the AudioRenderSystem sits at +0x60 inside AudioRendererImpl
// SDK 20 (The Legend of Zelda: Breath of the Wild 1.9.0): the public ExecuteAudioRendererRendering (sdk+0x221e90) calls the AudioRendererImpl version through the vtable (sdk+0x22ca88, just x0 += 0x60 and a jump through the PLT to here),
// so the takeover above is never reached. The original generates commands here without the update lock; with the render thread on the original and the game thread's RequestUpdate rendering catch-up frames ("nothing rendered since the last update"),
// both sides wrote the same command buffer at once and CommandListProcessor::Process sometimes read a half-written command list and crashed (once at 60 s and once at 134 s in the shrine). Taken over here, so rendering goes through the host's locked render as well
HLE_FORCE("_ZN2nn5audio6server17AudioRenderSystem29ExecuteAudioRendererRenderingEv", "render one frame manually (SDK 20, reached through the PLT): same as above, through the host's locked rendering",
          Result, h_ars_Exec, uint8_t *sys) { return render_once(sys, g_frame, &g_frame_n); }

// At least one rendered frame between two RequestUpdates. An effect's "needs initialization" is set only by the update in which the game flags it as new (e.g. LightLimiterInfo::Update: isNew → parameter status 0);
// the next update overwrites it with the game's value; if no commands were generated in between, the DSP never receives the initialization and ApplyLightLimiterEffect later reads a null pointer. The real renderer produces a frame every 5 ms on its own
// and the game waits on the renderer event, so normally it is one update per frame; but at start-up the game updates twice in a row (0.04 ms apart), and with real audio output the sound card opens slowly and the first frame comes late, leaving no frame in between.
// Here, when nothing has been rendered since the last update, one catch-up frame is rendered on the current thread first; its output is queued and played by the render thread in place of its next render.
// Catch-up frames keep real time: when the queue is full the update waits for the render thread to play one (at most 50 ms). Square Enix's sound driver (DRAGON QUEST XI S,
// SQEX::Sd::Driver::Core::CoreAudioOut, main+0x442c5c0) refills its voices in a loop: append a wave buffer, RequestUpdate, take the next released buffer, and it waits on the renderer
// event only when none is released. Every update there renders a catch-up frame, which releases the buffer just appended, so the loop never waits: discarding catch-up output when the
// queue was full ran it at 11,000 updates a second on one core, the game's sound about 50 times faster than the output. Waiting keeps the queue full instead: the render thread then
// only plays queued frames, and the driver's own updates render every frame. A queued frame does not signal the renderer event: the SDK signals it in each render, and a game woken by a
// queued frame updated with nothing rendered since, which rendered the next catch-up frame, and so on (Big Brain Academy: Brain vs. Brain, one catch-up per update instead of 37 a run)
HLE_FORCE("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm", "renderer update: if nothing was rendered since the last update, render a catch-up frame first (so effects get initialized), then hand over to the SDK",
          Result, h_ars_Update, uint8_t *sys, void *o, size_t on, void *p, size_t pn, const void *in, size_t inn) {
    static Result (*orig)(void *, void *, size_t, void *, size_t, const void *, size_t); if (!orig) orig = gsym("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm");
    static uint64_t last_r = ~0ull;
    uint64_t u = atomic_fetch_add(&g_updates, 1) + 1, r = atomic_load(&g_renders);
    if (last_r != ~0ull && r == last_r) {
        uint64_t c = atomic_fetch_add(&g_catchups, 1) + 1;
        if (c <= 3) LOG("audren: no frame rendered before update %llu, rendering a catch-up frame first (%llu so far)", (unsigned long long)u, (unsigned long long)c);
        uint32_t t = atomic_load(&g_cq_tail);
        if (t - atomic_load(&g_cq_head) >= CQ) {   // queue full: wait for the render thread to play one (every 5 ms), so catch-up frames keep real time; at most 50 ms (an output that stopped)
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 50000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
            pthread_mutex_lock(&g_cq_mu);
            while (atomic_load(&g_open) && t - atomic_load(&g_cq_head) >= CQ && !pthread_cond_timedwait(&g_cq_cv, &g_cq_mu, &dl)) {}
            pthread_mutex_unlock(&g_cq_mu);
        }
        if (t - atomic_load(&g_cq_head) < CQ) { render_once(sys, g_cq[t % CQ], &g_cq_n[t % CQ]); atomic_store(&g_cq_tail, t + 1); }
        else render_once(sys, NULL, NULL);   // still full (the output stopped): render anyway, output discarded
    }
    static int tr = -1; if (tr < 0) tr = getenv("DM_AUDREN_TRACE") != NULL;
    if (tr && u < 200) LOG("audren update #%llu t=%.3f ms, %llu frames rendered since the last update", (unsigned long long)u, now_ns() / 1e6, (unsigned long long)(atomic_load(&g_renders) - (last_r == ~0ull ? 0 : last_r)));
    Result res = orig(sys, o, on, p, pn, in, inn);
    last_r = atomic_load(&g_renders);
    return res;
}

// Output device: one built-in speaker, stereo (the audio device service still goes through IPC; the host answers directly)
enum { DEVNAME = 0x100 };
static void out_devname(char *out) { memset(out, 0, DEVNAME); strlcpy(out, "AudioBuiltInSpeakerOutput", DEVNAME); }
HLE_FORCE("_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", "output device list: 1 built-in speaker", int, h_ad_List, char *names, int n) { if (n <= 0 || !names) return 0; out_devname(names); return 1; }
HLE_FORCE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", "active output device: built-in speaker", void, h_ad_Active, char *name) { out_devname(name); }
// The SDK 12 names (Engage): the same service, the same answers
HLE_FORCE("_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi", "output device list (SDK 12 name): 1 built-in speaker", int, h_ad_List12, char *names, int n) { return h_ad_List(names, n); }
HLE_FORCE("_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", "active output device (SDK 12 name): built-in speaker", void, h_ad_Active12, char *name) { out_devname(name); }
HLE_FORCE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", "output channel count: 2", int, h_ad_Channels, void) { return 2; }
// Device volume: kept and read back (DRAGON QUEST VII Reimagined reads it at boot, main+0xee8edc), not applied: the host's output has the system's own volume
static _Atomic float g_dev_vol = 1.0f;
HLE_FORCE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", "device volume; kept for the getter, not applied by the host", void, h_ad_SetVol, const char *name, float v) { (void)name; atomic_store(&g_dev_vol, v); }
HLE_FORCE("_ZN2nn5audio26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameE", "device volume: what the game last set (1.0 at start)", float, h_ad_GetVol, const char *name) { (void)name; return atomic_load(&g_dev_vol); }
HLE_FORCE("_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", "output device change notification: bound, never signaled", void, h_ad_Notify, void *ev) { hle_bind_system_event(ev, 0); }
// SDK 7 names of the two above (Super Mario Odyssey): the device-switch event and the active channel count
HLE_FORCE("_ZN2nn5audio36AcquireAudioDeviceSwitchNotificationEPNS_2os11SystemEventE", "output device switch notification (SDK 7 name): bound, never signaled", void, h_ad_Notify7, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn5audio21GetActiveChannelCountEv", "output channel count (SDK 7 name): 2", int, h_ad_Channels7, void) { return 2; }
