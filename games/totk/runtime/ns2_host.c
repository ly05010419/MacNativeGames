/* Switch 2 SDK calls that go through services this host does not serve (GAME_NS2; docs/ns2-port.md). Each is what an offline
   Mac has to report: no HDR or VRR output, no Joy-Con 2 mouse sensor, no GameShare guest, a fixed graphics scale tier. The
   behaviour is ported from seedds/nxnative (MIT): commit 8dd0c98a (dm/host/hle_misc.c, hle_hid.c, hle_oe.c, hle_audren.c,
   dm/gfx/hle_vi.c) and its nv::InitCheck (dm/gfx/hle_nv.c), all found on Donkey Kong Bananza. They are matched by the
   game's imports (runtime_override, services.c), like every other host function here. */
#include "runtime.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef uint32_t Result;
#define RES_OK 0u

/* ---------- vi: HDR, VRR and extended swap intervals ----------
   The SDK checks the display against its own default display object and forwards the layer to vi2, whose layer the host
   never made (Bananza: a call through null from SetLayerNextHdrContentGamutMetadata). The host presents SDR at a fixed
   refresh rate: HdrOutputFormat 0 is SDR; paper white is BT.2408's 203 nits, read only when HDR is on. */
static Result vi_hdr_status(uint8_t *on, uint32_t *fmt, uint16_t *peak, const void *disp) {
    (void)disp; if (on) *on = 0; if (fmt) *fmt = 0; if (peak) *peak = 0; return RES_OK; }
static Result vi_paper_white(uint16_t *nits, const void *disp) { (void)disp; if (nits) *nits = 203; return RES_OK; }
static Result vi_vrr_supported(uint8_t *on, const void *disp) { (void)disp; if (on) *on = 0; return RES_OK; }
static Result vi_set_vrr(void *disp, unsigned on) { (void)disp; (void)on; return RES_OK; }
/* The extended swap interval asks for a rate between whole vsync intervals (VRR); without VRR the NVN swap interval stays. */
static Result vi_ext_swap(void *layer, int a, int b) {
    (void)layer; static atomic_int once;
    if (!atomic_exchange(&once, 1)) fprintf(stderr, "[ns2] vi::SetLayerExtendedSwapInterval %d %d: no VRR on the host, the NVN swap interval stays\n", a, b);
    return RES_OK;
}
static Result vi_ext_swap_clear(void *layer) { (void)layer; return RES_OK; }
static Result vi_hdr_metadata1(void *layer, int v) { (void)layer; (void)v; return RES_OK; }
static Result vi_hdr_metadata2(void *layer, uint32_t lo, uint32_t hi) { (void)layer; (void)lo; (void)hi; return RES_OK; }

/* ---------- audio: output volume auto-tune (read at boot by Bananza): kept and read back, off at start ---------- */
static atomic_uchar autotune;
static void audio_set_autotune(unsigned on) { atomic_store(&autotune, on & 1); }
static unsigned audio_autotune(void) { return atomic_load(&autotune); }

/* ---------- audio: the output device (the aud:u service the host does not serve; seedds/nxnative hle_audren.c, MIT) ----------
   One built-in speaker, stereo. Its volume is kept and read back, not applied: the Mac's output has the system's own. */
enum { DEVNAME = 0x100 };
static void out_devname(char *out) { memset(out, 0, DEVNAME); strlcpy(out, "AudioBuiltInSpeakerOutput", DEVNAME); }
static int audio_list_devices(char *names, int n) { if (n <= 0 || !names) return 0; out_devname(names); return 1; }
static void audio_active_device(char *name) { out_devname(name); }
static int audio_channels(void) { return 2; }
static _Atomic float device_volume = 1.0f;
static void audio_set_volume(const char *name, float v) { (void)name; atomic_store(&device_volume, v); }
static float audio_volume(const char *name) { (void)name; return atomic_load(&device_volume); }
/* 0, or 0x499 when the notification is already acquired; bound, never signaled (the output never changes). */
static Result audio_notification(void *ev) { static atomic_int acquired; if (atomic_exchange(&acquired, 1)) return 0x499; hle_bind_system_event(ev, 0); return RES_OK; }
/* SDK 22's name for it (Ocarina of Time): the same answer, its own acquisition (IAudioDevice command 17 otherwise). */
static Result audio_output_notification(void *ev) { static atomic_int acquired; if (atomic_exchange(&acquired, 1)) return 0x499; hle_bind_system_event(ev, 0); return RES_OK; }

/* ---------- audio: SDK 21 streams (Ocarina of Time imports them; without these the SDK opens auddmg:u itself) ----------
   Frame accounting and interleaved formats follow seedds/nxnative dm/host/hle_audio.c, MIT.
   Each write is an AudioQueue buffer; only its completion returns room and signals the guest event.
   States: 0 not open, 1 stopped, 2 started. Input streams still report no microphone. */
#include <AudioToolbox/AudioToolbox.h>
enum { OSTREAM_MAX = 16 };
typedef struct {
    const void *key; void *event; AudioQueueRef queue;
    AudioStreamBasicDescription format; int cap, queued, state;
} OutStream;
static OutStream ostreams[OSTREAM_MAX];
/* API calls serialize lifecycle changes; callbacks only take the count lock. Never hold the count lock
   across AudioQueue calls: synchronous disposal waits for callbacks before a slot can be reused. */
static pthread_mutex_t ostream_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ostream_count_lock = PTHREAD_MUTEX_INITIALIZER;
void hle_signal_system_event(void *event);
static OutStream *os_find(const void *g) { for (int i = 0; i < OSTREAM_MAX; i++) if (ostreams[i].key == g) return &ostreams[i]; return NULL; }
static void os_played(void *user, AudioQueueRef q, AudioQueueBufferRef b) {
    OutStream *o = user; int frames = (int)(uintptr_t)b->mUserData;
    AudioQueueFreeBuffer(q, b);
    pthread_mutex_lock(&ostream_count_lock); o->queued -= frames; pthread_mutex_unlock(&ostream_count_lock);
    if (o->event) hle_signal_system_event(o->event);
}
static OSStatus os_new_queue(OutStream *o) {
    return AudioQueueNewOutput(&o->format, os_played, o, NULL, kCFRunLoopCommonModes, 0, &o->queue);
}
static void os_default_device(char *name) { memset(name, 0, DEVNAME); strlcpy(name, "AudioVirtualOutput", DEVNAME); }
/* sdk+0x224cd4..0x224cf0 copies name[0x100], reply+0x218 latency (u64) to +0x100,
   reply+0x108 channels (u32) to +0x108. The 0x228-byte IPC reply is NOT the public struct. */
static Result os_properties(uint8_t *props, const char *name) {
    if (!props || !name || strncmp(name, "AudioVirtualOutput", DEVNAME)) return 0x13201u;
    uint64_t latency = 10000000; uint32_t channels = 2;
    memset(props, 0, 0x100); strlcpy((char *)props, "AudioVirtualOutput", 0x100);
    memcpy(props + 0x100, &latency, 8); memcpy(props + 0x108, &channels, 4);
    return RES_OK;
}
static Result os_open_common(void *g, void *ev, const int32_t *fmt, int frames) {
    if (!g || !fmt || frames <= 0 || (fmt[0] != 2 && fmt[0] != 5) ||
        (fmt[1] != 1 && fmt[1] != 2 && fmt[1] != 6) || fmt[2] < 0) return 0x13201u;
    pthread_mutex_lock(&ostream_lock);
    OutStream *o = os_find(g);
    if (o) { pthread_mutex_unlock(&ostream_lock); return 0x13201u; }
    o = os_find(NULL);
    if (!o) { pthread_mutex_unlock(&ostream_lock); return 0x13201u; }
    unsigned bytes = fmt[0] == 5 ? 4 : 2;
    *o = (OutStream){.key = g, .event = ev, .cap = frames, .state = 1,
        .format = {.mSampleRate = fmt[2] ? fmt[2] : 48000, .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kLinearPCMFormatFlagIsPacked | (fmt[0] == 5 ? kLinearPCMFormatFlagIsFloat : kLinearPCMFormatFlagIsSignedInteger),
            .mBytesPerPacket = bytes * fmt[1], .mFramesPerPacket = 1, .mBytesPerFrame = bytes * fmt[1],
            .mChannelsPerFrame = fmt[1], .mBitsPerChannel = bytes * 8}};
    OSStatus st = os_new_queue(o);
    if (st) { fprintf(stderr, "[ns2] audio OutputStream [RED] AudioQueueNewOutput failed (%d)\n", (int)st); *o = (OutStream){0}; }
    else {
        if (ev) hle_bind_system_event(ev, 0);
        fprintf(stderr, "[ns2] audio OutputStream open: format %d, %d channels, %d Hz, %d frames (AudioQueue)\n", fmt[0], fmt[1], fmt[2] ? fmt[2] : 48000, frames);
    }
    pthread_mutex_unlock(&ostream_lock); return st ? 0x13201u : RES_OK;
}
static Result os_open(void *g, void *ev, void *buf, uint64_t size, const char *dev, const int32_t *fmt, int frames, int threshold, uint32_t flags) {
    (void)buf; (void)size; (void)dev; (void)threshold; (void)flags; return os_open_common(g, ev, fmt, frames); }
static Result os_open_no_flags(void *g, void *ev, void *buf, uint64_t size, const char *dev, const int32_t *fmt, int frames, int threshold) {
    (void)buf; (void)size; (void)dev; (void)threshold; return os_open_common(g, ev, fmt, frames); }
static Result os_start(void *g) {
    pthread_mutex_lock(&ostream_lock); OutStream *o = os_find(g);
    OSStatus st = o && o->queue ? AudioQueueStart(o->queue, NULL) : -1;
    if (!st) o->state = 2;
    pthread_mutex_unlock(&ostream_lock); return st ? 0x13201u : RES_OK;
}
static void os_stop(void *g) {
    pthread_mutex_lock(&ostream_lock); OutStream *o = os_find(g);
    if (o) { if (o->queue) AudioQueuePause(o->queue); o->state = 1; }
    pthread_mutex_unlock(&ostream_lock);
}
static void os_purge(void *g) {
    pthread_mutex_lock(&ostream_lock); OutStream *o = os_find(g);
    if (o) {
        /* Dispose frees pending buffers too, including those never submitted to the device while stopped.
           It waits for callbacks; recreating avoids counting any pre-purge callback against new frames. */
        if (o->queue) AudioQueueDispose(o->queue, true);
        o->queue = NULL;
        pthread_mutex_lock(&ostream_count_lock); o->queued = 0; pthread_mutex_unlock(&ostream_count_lock);
        OSStatus st = os_new_queue(o);
        if (!st && o->state == 2) st = AudioQueueStart(o->queue, NULL);
        if (st) { o->state = 1; fprintf(stderr, "[ns2] audio OutputStream [RED] purge/restart failed (%d)\n", (int)st); }
        if (o->event) hle_signal_system_event(o->event);
    }
    pthread_mutex_unlock(&ostream_lock);
}
static void os_close(void *g) {
    pthread_mutex_lock(&ostream_lock); OutStream *o = os_find(g);
    if (o) { if (o->queue) AudioQueueDispose(o->queue, true); *o = (OutStream){0}; }
    pthread_mutex_unlock(&ostream_lock);
}
static int os_state(const void *g) { pthread_mutex_lock(&ostream_lock); OutStream *o = os_find(g); int s = o ? o->state : 0; pthread_mutex_unlock(&ostream_lock); return s; }
static int os_free(const void *g) {
    pthread_mutex_lock(&ostream_lock); OutStream *o = os_find(g);
    pthread_mutex_lock(&ostream_count_lock); int n = o && o->queue ? o->cap - o->queued : 0; pthread_mutex_unlock(&ostream_count_lock);
    pthread_mutex_unlock(&ostream_lock); return n;
}
static int os_filled(const void *g) {
    pthread_mutex_lock(&ostream_lock); OutStream *o = os_find(g);
    pthread_mutex_lock(&ostream_count_lock); int n = o ? o->queued : 0; pthread_mutex_unlock(&ostream_count_lock);
    pthread_mutex_unlock(&ostream_lock); return n;
}
static int os_write(void *g, const void *data, int frames) {
    if (!data || frames <= 0) return 0;
    pthread_mutex_lock(&ostream_lock); OutStream *o = os_find(g); int n = 0;
    if (o && o->queue) {
        pthread_mutex_lock(&ostream_count_lock); int room = o->cap - o->queued; pthread_mutex_unlock(&ostream_count_lock);
        n = frames < room ? frames : room;
        uint64_t bytes = (uint64_t)n * o->format.mBytesPerFrame;
        AudioQueueBufferRef b = NULL; OSStatus st = 0;
        if (n > 0 && bytes <= UINT32_MAX && !(st = AudioQueueAllocateBuffer(o->queue, (UInt32)bytes, &b))) {
            memcpy(b->mAudioData, data, (size_t)bytes); b->mAudioDataByteSize = (UInt32)bytes; b->mUserData = (void *)(uintptr_t)n;
            pthread_mutex_lock(&ostream_count_lock); o->queued += n; pthread_mutex_unlock(&ostream_count_lock);
            st = AudioQueueEnqueueBuffer(o->queue, b, 0, NULL);
            if (st) {
                pthread_mutex_lock(&ostream_count_lock); o->queued -= n; pthread_mutex_unlock(&ostream_count_lock);
                AudioQueueFreeBuffer(o->queue, b); n = 0;
            }
        } else n = 0;
        if (st) fprintf(stderr, "[ns2] audio OutputStream [RED] write failed (%d)\n", (int)st);
    }
    pthread_mutex_unlock(&ostream_lock); return n;
}
/* Statistics are returned by value through x8, 24 bytes (nxnative: a larger result overwrote Star Fox's stack canary). */
typedef struct { int64_t a, b, c; } StreamStats;
static StreamStats os_stats(void *g, unsigned reset) { (void)g; (void)reset; return (StreamStats){0}; }
static int is_list(void *names, int n) { (void)names; (void)n; return 0; }
static Result is_open(void *g, void *buf, uint64_t size, const char *dev, const void *fmt, int frames) { (void)g; (void)buf; (void)size; (void)dev; (void)fmt; (void)frames; return 0x1u | (153u << 9); }
static void is_noop(void *g) { (void)g; }
static Result is_start(void *g) { (void)g; return 0x1u | (153u << 9); }
static int is_zero(const void *g) { (void)g; return 0; }
static int is_read(void *g, void *buf, int frames) { (void)g; (void)buf; (void)frames; return 0; }

/* ---------- hid: the Joy-Con 2 mouse sensor (JCMS) ----------
   InitializeJcms opens the hid service itself. A handle is the npad id in byte 0 and the JcmsType in byte 1 (the SDK's
   MakeJcmsHandle); GetJcmsStates returns 0 while its ring holds no samples, as here: no controller of the host has one. */
static void jcms_init(void *work, uint64_t size) { (void)work; (void)size; }
static void jcms_fini(void) {}
static uint32_t jcms_handle(const uint32_t *id, const uint32_t *type) { return (*id & 0xff) | (*type & 0xff) << 8; }
static void jcms_start_stop(const uint32_t *h) { (void)h; }
static int jcms_states(void *out, int n, const uint32_t *h) { (void)out; (void)n; (void)h; return 0; }

/* ---------- streamplay: GameShare / GameChat streaming (stpl:u) ----------
   Bananza initializes it at boot. The host streams to nobody: the library is ready, no session is open and no guest is
   connected, so every guest query has nothing to report. Opening a session is a player action and stays unhooked. */
static uint32_t stpl_mem_size(int type, int mode, int n) { (void)type; (void)mode; (void)n; return 0x1000; }   /* PokeDinner answers 0 (stpl:u command 300); the page Bananza was played with stays (Pokopia runs with either) */
static Result stpl_init(int type, int mode, void *work, size_t size) { (void)type; (void)mode; (void)work; (void)size; return RES_OK; }
static void stpl_fini(void) {}
static uint8_t stpl_event[0x80] __attribute__((aligned(16)));   /* the connection state event: bound, never signaled */
static void *stpl_conn_event(void) { static atomic_int bound; if (!atomic_exchange(&bound, 1)) hle_bind_system_event(stpl_event, 0); return stpl_event; }
static void stpl_conn_state(uint32_t *s) { *s = 0; }   /* not connected */
static _Atomic uint32_t stpl_styles, stpl_hold;
static void stpl_set_styles(uint32_t s) { stpl_styles = s; }
static uint32_t stpl_get_styles(void) { return stpl_styles; }
static void stpl_set_hold(uint32_t t) { stpl_hold = t; }
static uint32_t stpl_get_hold(void) { return stpl_hold; }
static void stpl_hid_features(uint32_t s) { (void)s; }
static uint32_t stpl_pad_style(const void *id) { (void)id; return 0; }
static int stpl_none3(void *out, int n, const void *id) { (void)out; (void)n; (void)id; return 0; }
static int stpl_none4(void *h, int n, const void *id, uint32_t style) { (void)h; (void)n; (void)id; (void)style; return 0; }
static void stpl_vib_info(uint64_t *info, const void *h) { (void)h; *info = 0; }
static uint32_t stpl_jcms_handle(const void *id, const void *type) { (void)id; (void)type; return 0; }

/* ---------- oe: the graphics scale (through am's IGraphicsScalingParametersFunctions, a session the host never opened) ----------
   The SDK maps the parameter's first word 10 / 20 / 30 to scale 0 / 1 / 2; Bananza keeps it as a draw-size tier. Which tier
   the console reports in which mode is not measured (open, as in nxnative): 0 handheld, 1 docked, following the mode.
   PokeDinner (an Eden fork) answers 10 (scale 0) in either mode and kirakira-dev/ouncenative returns 0 for Pokopia; the
   mode rule Bananza was played with stays until the console's answer is known (Pokopia runs with either). */
static int oe_graphics_scale(void) { return !runtime_handheld_mode(); }
static void oe_graphics_scale_notify(unsigned on) { (void)on; }

/* ---------- nv::InitCheck ----------
   It stops the game unless graphics are initialised, testing the SDK's flag that nv::InitializeGraphics sets; the host
   replaces nv::InitializeGraphics (gfx_bridge.c), so it answers from the host's own record. */
static void nv_init_check(void) { if (!runtime_graphics_initialized()) runtime_fail("nv::InitCheck: graphics not initialised (nv::InitializeGraphics was not called)"); }

/* ---------- An offline console: no data delivery, no shop, no Nintendo services (seedds/nxnative hle_offline.c, hle_misc.c) ---------- */
#define RES_FAIL 0x1u   /* any non-zero result: the game's "unavailable" path */
static void bcat_init(void) {}
static Result bcat_mount(void) { return RES_FAIL; }   /* no delivery cache */
static void bcat_unmount(void) {}
static Result ec_init(void *mem, size_t n) { (void)mem; (void)n; return RES_OK; }
static void ec_fin(void) {}
static void ec_object(void *self) { (void)self; }   /* the shop accessor and its response: no session */
static Result ec_unavailable(void *self) { (void)self; return RES_FAIL; }
static Result ec_request(void *self, void *resp, const void *uid, int method, const char *path, const void *post, int64_t timeout) {
    (void)self; (void)resp; (void)uid; (void)method; (void)path; (void)post; (void)timeout; return RES_FAIL; }
static Result ec_size(void *self, size_t *n) { (void)self; if (n) *n = 0; return RES_FAIL; }
static Result ec_get(void *self, void *buf, size_t n) { (void)self; (void)buf; (void)n; return RES_FAIL; }
static Result ec_show_product(uint64_t ns_uid, const void *user) { (void)ns_uid; (void)user; fprintf(stderr, "[ns2] eShop product page: not available offline\n"); return RES_FAIL; }
static void aoc_lost(const int *idx, int n) { (void)idx; fprintf(stderr, "[ns2] aoc::ShowAddOnContentLostError (%d entries): the error applet is not shown\n", n); }
/* Add-on content is not packed for a Switch 2 dump yet (docs/ns2-port.md, open): no volume to mount. */
static Result aoc_query(uint64_t *size, int index) { (void)index; if (size) *size = 0; return RES_OK; }
static Result aoc_mount(const char *name, int index, void *cache, size_t size) {
    (void)cache; (void)size; fprintf(stderr, "[ns2] [RED] fs::MountAddOnContent(%s, %d): add-on content is not packed yet\n", name ? name : "?", index); return 0x7d402u; }
static void album_orientation(int o) { (void)o; }   /* screenshots are not the system's here */
static Result nfp_no_device(const void *h) { (void)h; return 0x1073u; }   /* nfp::ResultNfcDeviceNotFound */
/* nn::time::ToPosixTime through the time zone service: the host's mktime (its local zone), as nxnative hle_time.c. */
typedef struct { int16_t year; int8_t month, day, hour, minute, second; } CalendarTime;
static Result to_posix(int *count, int64_t *out, int n, const CalendarTime *ct) {
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour, .tm_min = ct->minute, .tm_sec = ct->second, .tm_isdst = -1};
    time_t v = mktime(&tm);
    if (count) *count = n > 0;
    if (n > 0 && out) out[0] = (int64_t)v;
    return RES_OK;
}

/* ---------- More of the Switch 2 SDK (Pokemon Pokopia; from xFractals/pokopia-native's nxnative patch, MIT) ----------
   The Joy-Con 2 mouse (Nmouse) and its sensor: none on the host. The hid library's vibration classes (players, targets,
   converters, node connections): inert, as the game's rumble files have no host device yet. GameShare sessions: none.
   The server environment is production (0). */
static Result ok_result(void) { return RES_OK; }
static void no_op(void) {}
static uint64_t zero_word(void) { return 0; }
static Result nmouse_states(int *count) { if (count) *count = 0; return RES_OK; }
static Result zero_out64(uint64_t *out) { if (out) *out = 0; return RES_OK; }
static Result zero_out32(uint32_t *out) { if (out) *out = 0; return RES_OK; }
static Result jcms_state(uint8_t *out, const void *h) { (void)h; if (out) memset(out, 0, 32); return RES_OK; }
static Result album_fail(void) { return 0x1u; }
static void app_error(const void *arg) { (void)arg; fprintf(stderr, "[ns2] err::ShowApplicationError: the error applet is not shown\n"); }
static Result stpl_nickname(char *out) { if (out) *out = 0; return RES_OK; }
static Result stpl_open_session(uint8_t *info, const void *cfg) { (void)cfg; if (info) memset(info, 0, 0x40); return RES_OK; }

void *runtime_ns2_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", vi_hdr_status},
        {"_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", vi_paper_white},
        {"_ZN2nn2vi24QueryDisplayVrrSupportedEPbPKNS0_7DisplayE", vi_vrr_supported},
        {"_ZN2nn2vi23SetDisplayVrrCapabilityEPNS0_7DisplayEb", vi_set_vrr},
        {"_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", vi_ext_swap},
        {"_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", vi_ext_swap_clear},
        {"_ZN2nn2vi35SetLayerNextHdrContentGamutMetadataEPNS0_5LayerENS0_15HdrContentGamutE", vi_hdr_metadata1},
        {"_ZN2nn2vi39SetLayerNextHdrToSdrToneMappingMetadataEPNS0_5LayerENS0_14HdrToneMappingE", vi_hdr_metadata1},
        {"_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", vi_hdr_metadata2},
        {"_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", vi_hdr_metadata2},
        {"_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb", audio_set_autotune},
        {"_ZN2nn5audio39IsDeviceOutputVolumeAutoTuneModeEnabledEv", audio_autotune},
        {"_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", audio_list_devices},
        {"_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", audio_active_device},
        {"_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", audio_channels},
        {"_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", audio_set_volume},
        {"_ZN2nn5audio26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameE", audio_volume},
        {"_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", audio_notification},
        {"_ZN2nn5audio36AcquireAudioOutputDeviceNotificationEPNS_2os11SystemEventE", audio_output_notification},
        {"_ZN2nn5audio29GetDefaultVirtualOutputDeviceEPNS0_15AudioDeviceNameE", os_default_device},
        {"_ZN2nn5audio32GetVirtualOutputDevicePropertiesEPNS0_23VirtualDevicePropertiesERKNS0_15AudioDeviceNameE", os_properties},
        {"_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE", os_open},
        {"_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEii", os_open_no_flags},
        {"_ZN2nn5audio17StartOutputStreamEPNS0_16OutputStreamTypeE", os_start},
        {"_ZN2nn5audio16StopOutputStreamEPNS0_16OutputStreamTypeE", os_stop},
        {"_ZN2nn5audio17PurgeOutputStreamEPNS0_16OutputStreamTypeE", os_purge},
        {"_ZN2nn5audio17CloseOutputStreamEPNS0_16OutputStreamTypeE", os_close},
        {"_ZN2nn5audio20GetOutputStreamStateEPKNS0_16OutputStreamTypeE", os_state},
        {"_ZN2nn5audio29GetOutputStreamFreeFrameCountEPKNS0_16OutputStreamTypeE", os_free},
        {"_ZN2nn5audio31GetOutputStreamFilledFrameCountEPKNS0_16OutputStreamTypeE", os_filled},
        {"_ZN2nn5audio17WriteOutputStreamEPNS0_16OutputStreamTypeEPKvi", os_write},
        {"_ZN2nn5audio25GetOutputStreamStatisticsEPNS0_16OutputStreamTypeEb", os_stats},
        {"_ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi", is_list},
        {"_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi", is_open},
        {"_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE", is_noop},
        {"_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE", is_start},
        {"_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE", is_noop},
        {"_ZN2nn5audio16PurgeInputStreamEPNS0_15InputStreamTypeE", is_noop},
        {"_ZN2nn5audio19GetInputStreamStateEPKNS0_15InputStreamTypeE", is_zero},
        {"_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE", is_zero},
        {"_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi", is_read},
        {"_ZN2nn5audio24GetInputStreamStatisticsEPNS0_15InputStreamTypeEb", os_stats},
        {"_ZN2nn3hid14InitializeJcmsEPvm", jcms_init},
        {"_ZN2nn3hid12FinalizeJcmsEv", jcms_fini},
        {"_ZN2nn3hid13GetJcmsHandleERKjRKNS0_8JcmsTypeE", jcms_handle},
        {"_ZN2nn3hid9StartJcmsERKNS0_10JcmsHandleE", jcms_start_stop},
        {"_ZN2nn3hid8StopJcmsERKNS0_10JcmsHandleE", jcms_start_stop},
        {"_ZN2nn3hid13GetJcmsStatesEPNS0_9JcmsStateEiRKNS0_10JcmsHandleE", jcms_states},
        {"_ZN2nn10streamplay21GetRequiredMemorySizeENS0_14StreamPlayTypeENS0_13StreamingModeEi", stpl_mem_size},
        {"_ZN2nn10streamplay10InitializeENS0_14StreamPlayTypeENS0_13StreamingModeEPvm", stpl_init},
        {"_ZN2nn10streamplay8FinalizeEv", stpl_fini},
        {"_ZN2nn10streamplay30GetConnectionStateChangedEventEv", stpl_conn_event},
        {"_ZN2nn10streamplay18GetConnectionStateEPNS0_15ConnectionStateE", stpl_conn_state},
        {"_ZN2nn10streamplay29SetSupportedGuestNpadStyleSetENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", stpl_set_styles},
        {"_ZN2nn10streamplay29GetSupportedGuestNpadStyleSetEv", stpl_get_styles},
        {"_ZN2nn10streamplay23SetGuestNpadJoyHoldTypeENS_3hid15NpadJoyHoldTypeE", stpl_set_hold},
        {"_ZN2nn10streamplay23GetGuestNpadJoyHoldTypeEv", stpl_get_hold},
        {"_ZN2nn10streamplay25SetSupportedHidFeatureSetENS_4util10BitFlagSetILi32ENS0_13HidFeatureTagEEE", stpl_hid_features},
        {"_ZN2nn10streamplay19GetGuestPadStyleSetERKNS0_10GuestPadIdE", stpl_pad_style},
        {"_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadFullKeyStateEEEiRKNS0_10GuestPadIdE", stpl_none3},
        {"_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid17NpadHandheldStateEEEiRKNS0_10GuestPadIdE", stpl_none3},
        {"_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadJoyDualStateEEEiRKNS0_10GuestPadIdE", stpl_none3},
        {"_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid17NpadJoyRightStateEEEiRKNS0_10GuestPadIdE", stpl_none3},
        {"_ZN2nn10streamplay17GetGuestPadStatesEPNS0_13GuestPadStateINS_3hid16NpadJoyLeftStateEEEiRKNS0_10GuestPadIdE", stpl_none3},
        {"_ZN2nn10streamplay28GetGuestSixAxisSensorHandlesEPNS0_24GuestSixAxisSensorHandleEiRKNS0_10GuestPadIdENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", stpl_none4},
        {"_ZN2nn10streamplay27GetGuestSixAxisSensorStatesEPNS0_23GuestSixAxisSensorStateEiRKNS0_24GuestSixAxisSensorHandleE", stpl_none3},
        {"_ZN2nn10streamplay30GetGuestVibrationDeviceHandlesEPNS0_26GuestVibrationDeviceHandleEiRKNS0_10GuestPadIdENS_4util10BitFlagSetILi32ENS_3hid12NpadStyleTagEEE", stpl_none4},
        {"_ZN2nn10streamplay27GetGuestVibrationDeviceInfoEPNS_3hid19VibrationDeviceInfoERKNS0_26GuestVibrationDeviceHandleE", stpl_vib_info},
        {"_ZN2nn10streamplay18GetGuestJcmsHandleERKNS0_10GuestPadIdERKNS_3hid8JcmsTypeE", stpl_jcms_handle},
        {"_ZN2nn10streamplay18GetGuestJcmsStatesEPNS0_14GuestJcmsStateEiRKNS0_15GuestJcmsHandleE", stpl_none3},
        {"_ZN2nn2oe23GetCurrentGraphicsScaleEv", oe_graphics_scale},
        {"_ZN2nn2oe42SetGraphicsScaleChangedNotificationEnabledEb", oe_graphics_scale_notify},
        {"_ZN2nv9InitCheckEv", nv_init_check},
        {"_ZN2nn2oe24GetServerEnvironmentTypeEv", zero_word},
        /* Parental controls' free communication (before online play): allowed; the display at exit; the album (screenshots go
           nowhere); the application error applet (logged). */
        {"_ZN2nn4pctl25TryBeginFreeCommunicationEb", ok_result}, {"_ZN2nn4pctl20EndFreeCommunicationEv", no_op},
        {"_ZN2nn2vi8FinalizeEv", no_op}, {"_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", no_op}, {"_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", no_op},
        {"_ZN2nn5album10InitializeEv", ok_result}, {"_ZN2nn5album8FinalizeEv", no_op}, {"_ZN2nn5album14SaveScreenshotEPKvmNS0_9ImageSizeENS0_17AlbumReportOptionE", album_fail},
        {"_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", app_error},
        {"_ZN2nn10streamplay12CloseSessionEv", ok_result}, {"_ZN2nn10streamplay14StartStreamingEv", ok_result},
        {"_ZN2nn10streamplay24CreateIndividualDisplaysEi", ok_result}, {"_ZN2nn10streamplay21GetNativeWindowHandleENS0_7GuestIdE", zero_word},
        {"_ZN2nn10streamplay21WriteGuestAudioStreamENS0_7GuestIdEPKvi", ok_result},
        {"_ZN2nn10streamplay13GetGuestNsaIdEPNS_7account23NetworkServiceAccountIdENS0_7GuestIdE", zero_out64},
        {"_ZN2nn10streamplay16GetGuestNicknameEPNS_7account8NicknameENS0_7GuestIdE", stpl_nickname},
        {"_ZN2nn10streamplay31OpenSessionWithMatchmakeSupportEPNS0_11SessionInfoERKNS0_15MatchmakeConfigE", stpl_open_session},
        {"_ZN2nn3hid16InitializeNmouseEPvmii", ok_result}, {"_ZN2nn3hid14FinalizeNmouseEv", no_op},
        {"_ZN2nn3hid16GetNmouseHandlesEPNS0_12NmouseHandleEij", zero_word},
        {"_ZN2nn3hid21GetMergedNmouseHandleEPNS0_12NmouseHandleE", zero_out64},
        {"_ZN2nn3hid15GetNmouseStatesEPiPNS0_11NmouseStateEiNS0_12NmouseHandleE", nmouse_states},
        {"_ZN2nn3hid16GetNmouseTypeSetEPNS_4util10BitFlagSetILi32ENS0_10NmouseTypeEEENS0_12NmouseHandleE", zero_out32},
        {"_ZN2nn3hid26SetMergedNmouseSourceTypesENS_4util10BitFlagSetILi32ENS0_10NmouseTypeEEE", no_op},
        {"_ZN2nn3hid32SetNmouseButtonConfigJoyLeftJcmsERKNS0_29NmouseButtonConfigJoyLeftJcmsE", no_op},
        {"_ZN2nn3hid33SetNmouseButtonConfigJoyRightJcmsERKNS0_30NmouseButtonConfigJoyRightJcmsE", no_op},
        {"_ZN2nn3hid12GetJcmsStateEPNS0_9JcmsStateERKNS0_10JcmsHandleE", jcms_state},
        /* No nn::hid vibration graph classes (VibrationNode, VibrationTarget, VibrationPlayer, ...): the SDK's own code runs.
           Stubbing them (2026-11-03, for Pokopia) emptied their constructors, so a game class derived from VibrationNode
           had no vtable and Donkey Kong Bananza called garbage from VibrationNode::Update about 20 s in (main+0x2ee06c). */
        {"_ZN2nn4bcat10InitializeEv", bcat_init}, {"_ZN2nn4bcat25MountDeliveryCacheStorageEv", bcat_mount},
        {"_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", bcat_unmount},
        {"_ZN2nn2ec33InitializeForShopServiceAccessorsEPvm", ec_init}, {"_ZN2nn2ec31FinalizeForShopServiceAccessorsEv", ec_fin},
        {"_ZN2nn2ec19ShopServiceAccessorC1Ev", ec_object}, {"_ZN2nn2ec19ShopServiceAccessorD1Ev", ec_object},
        {"_ZN2nn2ec19ShopServiceAccessor10InitializeERKNS0_11ShopServiceE", ec_unavailable},
        {"_ZN2nn2ec19ShopServiceAccessor7RequestEPNS1_13AsyncResponseERKNS_7account3UidENS0_11ShopService6MethodEPKcRKNS1_8PostDataENS_8TimeSpanE", ec_request},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponseC1Ev", ec_object}, {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponseD1Ev", ec_object},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7GetSizeEPm", ec_size},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse3GetEPvm", ec_get},
        {"_ZN2nn2ec22ShowShopProductDetailsENS0_5NsUidERKNS_7account10UserHandleE", ec_show_product},
        {"_ZN2nn3aoc25ShowAddOnContentLostErrorEPKii", aoc_lost},
        {"_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi", aoc_query}, {"_ZN2nn2fs17MountAddOnContentEPKciPvm", aoc_mount},
        {"_ZN2nn2oe24SetAlbumImageOrientationENS_5album16ImageOrientationE", album_orientation},
        {"_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeE", to_posix},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
