/* The Mac side of the microphone streams (audio_input.h): one AUHAL unit on the chosen or default input device, its
   callback handing planar Float32 to dm_audio_input_process; device changes rebuild it. From seedds/nxnative c25d4011
   (PR #1822, dm/host/audio_input_mac.m, MIT, Copyright (c) 2026 seedds); ours: OOT_MIC_DEVICE, the log lines, and the
   test feed (OOT_MIC_FILE, below) in place of a device. The Mac only: the iPhone / iPad keeps no input (user 2026-11-20). */
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include "audio_input.h"
#include <AVFAudio/AVFAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { INPUT_CHANNEL_MAX = 8, INPUT_FALLBACK_MAX_FRAMES = 4096 };

typedef struct {
    AudioUnit unit;
    AudioDeviceID device;
    AudioStreamBasicDescription format;
    AudioBufferList *buffers;
    float *planes[INPUT_CHANNEL_MAX];
    UInt32 max_frames;
    int channel_count;
    char name[0x100], uid[0x100];
    _Atomic int running, failed;
    _Atomic uint32_t callbacks;
} input_device;

static input_device g_input;
static pthread_mutex_t g_input_lock = PTHREAD_MUTEX_INITIALIZER;

static AudioObjectPropertyAddress default_input_address(void) {
    return (AudioObjectPropertyAddress){ kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
}

static AudioObjectPropertyAddress devices_address(void) {
    return (AudioObjectPropertyAddress){ kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
}

static AudioDeviceID default_input_device(void) {
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof device;
    AudioObjectPropertyAddress address = default_input_address();
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device) != noErr) return kAudioObjectUnknown;
    return device;
}

static int device_string(AudioDeviceID device, AudioObjectPropertySelector selector, char out[0x100]) {
    memset(out, 0, 0x100);
    AudioObjectPropertyAddress address = { selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef value = NULL; UInt32 size = sizeof(CFStringRef);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &value) != noErr || !value) return 0;
    int ok = CFStringGetCString(value, out, 0x100, kCFStringEncodingUTF8);
    CFRelease(value);
    return ok && out[0];
}

static AudioDeviceID input_device_for_uid(const char *uid) {
    if (!uid || !*uid) return kAudioObjectUnknown;
    CFStringRef value = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    if (!value) return kAudioObjectUnknown;
    AudioObjectPropertyAddress address = {kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof device;
    OSStatus status = AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, sizeof(CFStringRef), &value, &size, &device);
    CFRelease(value);
    if (status != noErr || device == kAudioObjectUnknown) return kAudioObjectUnknown;
    AudioObjectPropertyAddress input = {kAudioDevicePropertyStreams, kAudioDevicePropertyScopeInput, kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyDataSize(device, &input, 0, NULL, &size) == noErr && size ? device : kAudioObjectUnknown;
}

static AudioDeviceID selected_input_device(int *uses_default, int *fell_back) {
    const char *uid = getenv("OOT_MIC_DEVICE");
    *uses_default = !uid || !*uid || !strcmp(uid, "default");
    *fell_back = 0;
    if (*uses_default) return default_input_device();
    AudioDeviceID selected = input_device_for_uid(uid);
    if (selected != kAudioObjectUnknown) return selected;
    *fell_back = 1;
    return default_input_device();
}

/* The test feed (OOT_MIC_FILE): tone:<hz>, a sine at -20 dBFS, or a raw 48 kHz mono PCM16 file, looped; 480 frames every
   10 ms on a timer, handed to dm_audio_input_process as a device's callback would. It needs no device and no permission
   (probe-audio, test runs that play into the ocarina without a microphone); a player never sets it. */
static dispatch_source_t g_feed_timer;
static dispatch_queue_t g_feed_queue;
static int16_t *g_feed_pcm;
static size_t g_feed_frames, g_feed_at;
static double g_feed_hz, g_feed_phase;
static const char *feed_spec(void) { const char *f = getenv("OOT_MIC_FILE"); return f && *f ? f : NULL; }
static int feed_load_locked(void) {
    if (g_feed_pcm || g_feed_hz > 0) return 1;
    const char *spec = feed_spec();
    if (!spec) return 0;
    if (!strncmp(spec, "tone:", 5)) {
        double hz = atof(spec + 5);
        if (!(hz >= 20.0 && hz <= 20000.0)) { fprintf(stderr, "[ns2-audio] 【红】audio input: OOT_MIC_FILE=%s: a tone is 20 to 20000 Hz\n", spec); return 0; }
        g_feed_hz = hz; g_feed_phase = 0;
    } else {
        FILE *f = fopen(spec, "rb");
        long n = -1;
        if (f && !fseek(f, 0, SEEK_END)) n = ftell(f);
        if (f && n >= 2 && n <= (64L << 20) && !fseek(f, 0, SEEK_SET) && (g_feed_pcm = malloc((size_t)n)) && fread(g_feed_pcm, 1, (size_t)n, f) == (size_t)n) {
            g_feed_frames = (size_t)n / 2; g_feed_at = 0;
        } else { free(g_feed_pcm); g_feed_pcm = NULL; }
        if (f) fclose(f);
        if (!g_feed_pcm) { fprintf(stderr, "[ns2-audio] 【红】audio input: OOT_MIC_FILE=%s: not a readable PCM16 file of 2 B to 64 MB\n", spec); return 0; }
    }
    fprintf(stderr, "[ns2-audio] audio input: test feed %s instead of a device\n", spec);
    return 1;
}
static void feed_tick(void) {
    enum { N = 480 };
    float plane[N]; const float *planes[1] = {plane};
    for (int i = 0; i < N; i++) {
        if (g_feed_hz > 0) {
            plane[i] = 0.1f * (float)sin(g_feed_phase);
            g_feed_phase += 2.0 * M_PI * g_feed_hz / DM_AUDIO_INPUT_RATE;
            if (g_feed_phase >= 2.0 * M_PI) g_feed_phase -= 2.0 * M_PI;
        } else {
            plane[i] = g_feed_pcm[g_feed_at] / 32768.0f;
            if (++g_feed_at >= g_feed_frames) g_feed_at = 0;
        }
    }
    dm_audio_input_process(planes, 1, N, DM_AUDIO_INPUT_RATE);
}
static int feed_start_locked(void) {
    if (g_feed_timer) return 0;
    if (!g_feed_queue) g_feed_queue = dispatch_queue_create("org.macnativegames.audio-input.feed", DISPATCH_QUEUE_SERIAL);
    g_feed_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_feed_queue);
    if (!g_feed_timer) return -1;
    dispatch_source_set_timer(g_feed_timer, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), 10 * NSEC_PER_MSEC, NSEC_PER_MSEC);
    dispatch_source_set_event_handler(g_feed_timer, ^{ feed_tick(); });
    dispatch_resume(g_feed_timer);
    return 0;
}
static void feed_close_locked(void) {
    if (!g_feed_timer) return;
    dispatch_source_cancel(g_feed_timer);
    dispatch_sync(g_feed_queue, ^{});   // a tick under way finishes before the streams it feeds can go
    g_feed_timer = nil;
}

int dm_audio_input_platform_device_available(void) {
    if (feed_spec()) return 1;
    if (AVAudioApplication.sharedInstance.recordPermission != AVAudioApplicationRecordPermissionGranted) return 0;
    int uses_default, fell_back;
    return selected_input_device(&uses_default, &fell_back) != kAudioObjectUnknown;
}

int dm_audio_input_platform_failed(void) { return !feed_spec() && atomic_load_explicit(&g_input.failed, memory_order_acquire); }

int dm_audio_input_platform_route_changed(void) {
    if (feed_spec()) return 0;
    int uses_default, fell_back;
    pthread_mutex_lock(&g_input_lock);
    int changed = !g_input.unit || dm_audio_input_platform_failed() || g_input.device != selected_input_device(&uses_default, &fell_back);
    pthread_mutex_unlock(&g_input_lock);
    return changed;
}

static void input_cleanup_locked(void) {
    if (!g_input.unit) return;
    if (atomic_exchange_explicit(&g_input.running, 0, memory_order_acq_rel)) AudioOutputUnitStop(g_input.unit);
    while (atomic_load_explicit(&g_input.callbacks, memory_order_acquire)) sched_yield();
    AudioUnitUninitialize(g_input.unit);
    AudioComponentInstanceDispose(g_input.unit);
    for (int i = 0; i < INPUT_CHANNEL_MAX; i++) { free(g_input.planes[i]); g_input.planes[i] = NULL; }
    free(g_input.buffers); g_input.buffers = NULL;
    g_input.unit = NULL; g_input.device = kAudioObjectUnknown; g_input.channel_count = 0; g_input.max_frames = 0;
    memset(g_input.name, 0, sizeof g_input.name); memset(g_input.uid, 0, sizeof g_input.uid);
    memset(&g_input.format, 0, sizeof g_input.format);
    atomic_store_explicit(&g_input.failed, 0, memory_order_release);
}

static OSStatus input_proc(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *time, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)io; (void)bus;
    input_device *d = ctx;
    atomic_fetch_add_explicit(&d->callbacks, 1, memory_order_acquire);
    if (!atomic_load_explicit(&d->running, memory_order_acquire) || atomic_load_explicit(&d->failed, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&d->callbacks, 1, memory_order_release);
        return noErr;
    }
    if (!d->buffers || frames > d->max_frames) {
        atomic_store_explicit(&d->failed, 1, memory_order_release);
        atomic_fetch_sub_explicit(&d->callbacks, 1, memory_order_release);
        return kAudio_ParamError;
    }
    for (int i = 0; i < d->channel_count; i++) d->buffers->mBuffers[i].mDataByteSize = frames * sizeof(float);
    OSStatus status = AudioUnitRender(d->unit, flags, time, 1, frames, d->buffers);
    if (status != noErr) {
        atomic_store_explicit(&d->failed, 1, memory_order_release);
        atomic_fetch_sub_explicit(&d->callbacks, 1, memory_order_release);
        return status;
    }
    const float *planes[INPUT_CHANNEL_MAX];
    for (int i = 0; i < d->channel_count; i++) planes[i] = d->planes[i];
    dm_audio_input_process(planes, d->channel_count, frames, d->format.mSampleRate);
    atomic_fetch_sub_explicit(&d->callbacks, 1, memory_order_release);
    return noErr;
}

// These listeners are process-wide: keep listening while a started stream is waiting for a device.
static void input_listen(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_queue_t queue = dispatch_queue_create("org.macnativegames.audio-input.route", DISPATCH_QUEUE_SERIAL);
        AudioObjectPropertyListenerBlock change = ^(UInt32 count, const AudioObjectPropertyAddress *addresses) {
            (void)count; (void)addresses;
            dm_audio_input_route_changed();
        };
        AudioObjectPropertyAddress addresses[] = {default_input_address(), devices_address()};
        for (size_t i = 0; i < sizeof addresses / sizeof *addresses; i++) {
            OSStatus result = AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &addresses[i], queue, change);
            if (result != noErr) fprintf(stderr, "[ns2-audio] 【红】audio input: cannot observe device changes (%d)\n", (int)result);
        }
    });
}

static int input_prepare_locked(void) {
    if (g_input.unit) return !atomic_load_explicit(&g_input.failed, memory_order_acquire);
    if (atomic_load_explicit(&g_input.failed, memory_order_acquire)) return 0;
    int uses_default = 0, fell_back = 0;
    AudioDeviceID device = selected_input_device(&uses_default, &fell_back);
    if (device == kAudioObjectUnknown || AVAudioApplication.sharedInstance.recordPermission != AVAudioApplicationRecordPermissionGranted) return 0;
    if (fell_back) fprintf(stderr, "[ns2-audio] audio input: selected UID is unavailable; falling back to the system default input\n");
    AudioComponentDescription desc = { kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent component = AudioComponentFindNext(NULL, &desc);
    if (!component) return 0;
    AudioUnit unit = NULL;
    OSStatus status = AudioComponentInstanceNew(component, &unit);
    UInt32 enabled = 1, disabled = 0;
    if (status == noErr) status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enabled, sizeof enabled);
    if (status == noErr) status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &disabled, sizeof disabled);
    if (status == noErr) status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof device);

    AudioStreamBasicDescription device_format = {0};
    UInt32 size = sizeof device_format;
    if (status == noErr) status = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &device_format, &size);
    if (status != noErr || device_format.mChannelsPerFrame < 1 || device_format.mChannelsPerFrame > INPUT_CHANNEL_MAX ||
        device_format.mSampleRate < 8000.0 || device_format.mSampleRate > 192000.0) {
        if (unit) AudioComponentInstanceDispose(unit);
        return 0;
    }

    AudioStreamBasicDescription client = {0};
    client.mSampleRate = device_format.mSampleRate;  // AUHAL does not rate-convert microphone input; the core resampler does.
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
    client.mBytesPerPacket = sizeof(float); client.mFramesPerPacket = 1; client.mBytesPerFrame = sizeof(float);
    client.mChannelsPerFrame = device_format.mChannelsPerFrame; client.mBitsPerChannel = 32;
    status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &client, sizeof client);
    AURenderCallbackStruct callback = { input_proc, &g_input };
    if (status == noErr) status = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &callback, sizeof callback);
    if (status == noErr) status = AudioUnitInitialize(unit);
    UInt32 max_frames = 0; size = sizeof max_frames;
    if (status == noErr) status = AudioUnitGetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, &size);
    if (status != noErr) {
        if (unit) { AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); }
        return 0;
    }
    if (!max_frames) max_frames = INPUT_FALLBACK_MAX_FRAMES;
    UInt32 channels = client.mChannelsPerFrame;
    AudioBufferList *buffers = calloc(1, sizeof(AudioBufferList) + (channels - 1) * sizeof(AudioBuffer));
    if (!buffers) { AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); return 0; }
    buffers->mNumberBuffers = channels;
    float *planes[INPUT_CHANNEL_MAX] = {0};
    for (UInt32 i = 0; i < channels; i++) {
        planes[i] = calloc(max_frames, sizeof(float));
        if (!planes[i]) { status = memFullErr; break; }
        buffers->mBuffers[i].mNumberChannels = 1;
        buffers->mBuffers[i].mDataByteSize = max_frames * sizeof(float);
        buffers->mBuffers[i].mData = planes[i];
    }
    if (status != noErr) {
        for (int i = 0; i < INPUT_CHANNEL_MAX; i++) free(planes[i]);
        free(buffers); AudioUnitUninitialize(unit); AudioComponentInstanceDispose(unit); return 0;
    }

    g_input.unit = unit; g_input.device = device; g_input.format = client; g_input.buffers = buffers;
    g_input.max_frames = max_frames; g_input.channel_count = (int)channels;
    if (!device_string(device, kAudioObjectPropertyName, g_input.name)) strlcpy(g_input.name, "Unknown input device", sizeof g_input.name);
    if (!device_string(device, kAudioDevicePropertyDeviceUID, g_input.uid)) strlcpy(g_input.uid, "unknown", sizeof g_input.uid);
    fprintf(stderr, "[ns2-audio] audio input: opened device=\"%s\" uid=\"%s\" rate=%.0f channels=%u%s\n", g_input.name, g_input.uid,
            client.mSampleRate, (unsigned)channels, uses_default ? " (system default)" : fell_back ? " (fallback)" : " (selected)");
    for (int i = 0; i < INPUT_CHANNEL_MAX; i++) g_input.planes[i] = planes[i];
    atomic_store_explicit(&g_input.failed, 0, memory_order_release);
    return 1;
}

int dm_audio_input_platform_prepare(void) {
    if (feed_spec()) { pthread_mutex_lock(&g_input_lock); int fed = feed_load_locked(); pthread_mutex_unlock(&g_input_lock); return fed ? 0 : -1; }
    input_listen();
    pthread_mutex_lock(&g_input_lock);
    int ok = input_prepare_locked();
    pthread_mutex_unlock(&g_input_lock);
    return ok ? 0 : -1;
}

int dm_audio_input_platform_start(void) {
    if (feed_spec()) { pthread_mutex_lock(&g_input_lock); int r = feed_load_locked() ? feed_start_locked() : -1; pthread_mutex_unlock(&g_input_lock); return r; }
    pthread_mutex_lock(&g_input_lock);
    int ok = g_input.unit && !atomic_load_explicit(&g_input.failed, memory_order_acquire);
    if (ok && !atomic_load_explicit(&g_input.running, memory_order_relaxed)) {
        atomic_store_explicit(&g_input.running, 1, memory_order_release);
        OSStatus status = AudioOutputUnitStart(g_input.unit);
        if (status != noErr) {
            atomic_store_explicit(&g_input.running, 0, memory_order_release);
            atomic_store_explicit(&g_input.failed, 1, memory_order_release);
            fprintf(stderr, "[ns2-audio] 【红】audio input: AUHAL start failed (%d)\n", (int)status);
            ok = 0;
        }
    }
    pthread_mutex_unlock(&g_input_lock);
    return ok ? 0 : -1;
}

void dm_audio_input_platform_close(void) {
    pthread_mutex_lock(&g_input_lock);
    feed_close_locked();
    input_cleanup_locked();
    pthread_mutex_unlock(&g_input_lock);
}

void dm_audio_input_platform_info(char name[0x100], char uid[0x100], double *sample_rate, int *channels) {
    if (feed_spec()) {
        if (name) strlcpy(name, "test feed", 0x100);
        if (uid) strlcpy(uid, feed_spec(), 0x100);
        if (sample_rate) *sample_rate = DM_AUDIO_INPUT_RATE;
        if (channels) *channels = 1;
        return;
    }
    pthread_mutex_lock(&g_input_lock);
    if (name) strlcpy(name, g_input.name[0] ? g_input.name : "unknown", 0x100);
    if (uid) strlcpy(uid, g_input.uid[0] ? g_input.uid : "unknown", 0x100);
    if (sample_rate) *sample_rate = g_input.format.mSampleRate;
    if (channels) *channels = g_input.channel_count;
    pthread_mutex_unlock(&g_input_lock);
}

/* The settings panel's 陶笛 page (user 2026-11-20: "这几首歌放在 设置里 我点击 就播放"): a song's notes made here as sines
   (-22 dBFS before the stream's gain, 10 ms fades) and handed to the game's started streams 480 frames every 10 ms, as a
   microphone would, while the capture's own frames are held back (dm_audio_input_inject_begin). Mac only. */
static dispatch_source_t g_song_timer;
static float *g_song;
static size_t g_song_frames, g_song_at;
static _Atomic int g_song_playing;
int dm_audio_input_playing(void) { return atomic_load(&g_song_playing); }
static void song_tick(void) {
    enum { N = 480 };
    size_t n = g_song_frames - g_song_at < N ? g_song_frames - g_song_at : N;
    if (n) dm_audio_input_inject(g_song + g_song_at, (uint32_t)n);
    g_song_at += n;
    if (g_song_at < g_song_frames) return;
    dispatch_source_cancel(g_song_timer);   // the cancel handler ends it, on this queue
}
int dm_audio_input_play_notes(const double *hz, int count, double note_s, double gap_s) {
    if (!hz || count <= 0 || count > 64 || !(note_s > 0.05 && note_s < 3) || !(gap_s >= 0 && gap_s < 3)) return -1;
    if (!dm_audio_input_listening()) { fprintf(stderr, "[ns2-audio] audio input: song not played: the game is not listening (take out the ocarina)\n"); return 0; }
    int idle = 0;
    if (!atomic_compare_exchange_strong(&g_song_playing, &idle, 1)) return -1;
    size_t note = (size_t)(note_s * DM_AUDIO_INPUT_RATE), gap = (size_t)(gap_s * DM_AUDIO_INPUT_RATE), fade = DM_AUDIO_INPUT_RATE / 100;
    size_t total = (size_t)count * (note + gap) + DM_AUDIO_INPUT_RATE / 2;   // half a second of silence after the last note
    float *pcm = calloc(total, sizeof *pcm);
    if (!pcm) { atomic_store(&g_song_playing, 0); return -1; }
    for (int k = 0; k < count; k++) {
        float *at = pcm + (size_t)k * (note + gap);
        for (size_t i = 0; i < note; i++) {
            double env = fmin(1.0, fmin((double)i / fade, (double)(note - i) / fade));
            at[i] = (float)(0.08 * env * sin(2.0 * M_PI * hz[k] * (double)i / DM_AUDIO_INPUT_RATE));
        }
    }
    static dispatch_queue_t queue; static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("org.macnativegames.audio-input.song", DISPATCH_QUEUE_SERIAL); });
    dispatch_async(queue, ^{
        free(g_song); g_song = pcm; g_song_frames = total; g_song_at = 0;
        dm_audio_input_inject_begin();
        fprintf(stderr, "[ns2-audio] audio input: playing a song from the settings panel, %d notes, %.1f s\n", count, (double)total / DM_AUDIO_INPUT_RATE);
        g_song_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
        dispatch_source_set_timer(g_song_timer, dispatch_time(DISPATCH_TIME_NOW, 0), 10 * NSEC_PER_MSEC, NSEC_PER_MSEC);
        dispatch_source_set_event_handler(g_song_timer, ^{ song_tick(); });
        dispatch_source_set_cancel_handler(g_song_timer, ^{
            dm_audio_input_inject_end(); g_song_timer = nil; atomic_store(&g_song_playing, 0);
            fprintf(stderr, "[ns2-audio] audio input: the song from the settings panel ended\n");
        });
        dispatch_resume(g_song_timer);
    });
    return 1;
}
#endif
