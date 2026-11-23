/* The headphone route of spatial audio (macOS; audio_route.h): the mixer's 5.1 float mix through
   AVSampleBufferAudioRenderer + AVSampleBufferRenderSynchronizer. A Core Audio unit is not eligible for the system's
   Spatial Audio; this renderer is, for multichannel audio with a channel layout (WWDC21 10265; nxnative issue #2650 measured it
   with TotK on AirPods Pro: positions right, head tracking, the Sound menu active). No entitlement is needed.

   A feeder thread (not the real-time thread) keeps the renderer about TOTK_SPATIAL_LEAD_MS (default 150; #2650: 100 and
   80 stutter) ahead of the synchronizer's clock: it reads 5-ms blocks from the mixer's ring (the read callback zero-fills
   an underrun, so the timeline never breaks), wraps each in a CMSampleBuffer with the MPEG 5.1 A layout (L R C LFE Ls Rs
   = the Switch's FL FR C LFE RL RR) and enqueues it. The synchronizer is anchored once; a late feeder or an automatic
   flush (output configuration change) moves the timestamps forward instead of re-anchoring (re-anchoring made start-up
   unlistenable in #2650). The renderer follows the default output device by itself (audioOutputDeviceUniqueID nil). */
#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio_spatial.h"

@interface TOTKSystemSpatial : NSObject {
@public
    AVSampleBufferAudioRenderer *renderer;
    AVSampleBufferRenderSynchronizer *sync;
    id flushObserver;
    CMAudioFormatDescriptionRef format;
    int rate, channels; unsigned block; int64_t leadFrames; float gain;
    AudioSpatialRead read; void *ctx;
    float *buf;
    int64_t enqueued;                  /* frames enqueued so far = the next block's timestamp, in frames */
    _Atomic int stop, flushed;
    pthread_t thread; int threadStarted;
    unsigned long long late, flushes, failures, blocks;
}
@end
@implementation TOTKSystemSpatial
@end

int audio_system_spatial_available(void) {
    if (@available(macOS 12.0, *)) return NSClassFromString(@"AVSampleBufferAudioRenderer") != nil;
    return 0;
}

static int enqueue_block(TOTKSystemSpatial *s, const float *data, unsigned frames) {
    size_t bytes = (size_t)frames * (size_t)s->channels * sizeof(float);
    CMBlockBufferRef block = NULL; CMSampleBufferRef sample = NULL;
    OSStatus err = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, NULL, bytes, kCFAllocatorDefault, NULL, 0, bytes,
                                                      kCMBlockBufferAssureMemoryNowFlag, &block);
    if (!err) err = CMBlockBufferReplaceDataBytes(data, block, 0, bytes);
    if (!err) err = CMAudioSampleBufferCreateReadyWithPacketDescriptions(kCFAllocatorDefault, block, s->format, (CMItemCount)frames,
                                                                         CMTimeMake(s->enqueued, s->rate), NULL, &sample);
    if (!err) { [s->renderer enqueueSampleBuffer:sample]; s->enqueued += frames; s->blocks++; }
    if (sample) CFRelease(sample);
    if (block) CFRelease(block);
    return err == noErr;
}
static int64_t clock_frames(TOTKSystemSpatial *s) {
    CMTime t = [s->sync currentTime];
    if (!CMTIME_IS_NUMERIC(t)) return 0;
    return (int64_t)(CMTimeGetSeconds(t) * s->rate);
}
/* (Re)creates the renderer and anchors the synchronizer at 0 after one lead of silence: at the start, and after the
   renderer failed (an output it cannot play any more). */
static int start_renderer(TOTKSystemSpatial *s) {
    if (s->renderer) {
        if (s->flushObserver) [NSNotificationCenter.defaultCenter removeObserver:s->flushObserver];
        s->flushObserver = nil;
        [s->sync setRate:0];
        [s->renderer flush];
        [s->sync removeRenderer:s->renderer atTime:kCMTimeInvalid completionHandler:nil];
        s->renderer = nil;
    }
    s->renderer = [[AVSampleBufferAudioRenderer alloc] init];
    if (!s->renderer) return 0;
    if (@available(macOS 12.0, *)) s->renderer.allowedAudioSpatializationFormats = AVAudioSpatializationFormatMonoStereoAndMultichannel;
    [s->sync addRenderer:s->renderer];
    __unsafe_unretained TOTKSystemSpatial *weak = s;   /* the observer is removed before s goes away */
    s->flushObserver = [NSNotificationCenter.defaultCenter addObserverForName:AVSampleBufferAudioRendererWasFlushedAutomaticallyNotification
        object:s->renderer queue:nil usingBlock:^(NSNotification *note) { (void)note; atomic_store(&weak->flushed, 1); }];
    s->enqueued = 0;
    memset(s->buf, 0, (size_t)s->block * (size_t)s->channels * sizeof(float));
    while (s->enqueued < s->leadFrames) if (!enqueue_block(s, s->buf, s->block)) return 0;
    [s->sync setRate:1.0 time:kCMTimeZero];
    return 1;
}
static void *feeder(void *p) {
    TOTKSystemSpatial *s = (__bridge TOTKSystemSpatial *)p;
    pthread_setname_np("totk-audio-spatial");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    const struct timespec tick = {0, 2000000};   /* 2 ms */
    while (!atomic_load(&s->stop)) {
        @autoreleasepool {
            if (s->renderer.status == AVQueuedSampleBufferRenderingStatusFailed) {
                s->failures++;
                fprintf(stderr, "[totk-audio] spatial renderer failed (%s): starting it again (%llu)\n",
                        s->renderer.error.localizedDescription.UTF8String ?: "?", s->failures);
                if (s->failures > 20 || !start_renderer(s)) { fprintf(stderr, "[totk-audio] 【红】spatial renderer gave up: no sound\n"); break; }
            }
            int64_t now = clock_frames(s);
            /* After an automatic flush (the header's advice: go on from the timebase's current time) or a late feeder,
               the next block starts one lead minus one block ahead of the clock: the gap plays as silence, the anchor
               stays, and the ring is read at the device's pace again at once (no burst of reads that would underrun it). */
            if (atomic_exchange(&s->flushed, 0)) { s->flushes++; s->enqueued = now + s->leadFrames - s->block; }
            else if (s->enqueued < now) { s->late++; s->enqueued = now + s->leadFrames - s->block; }
            int n = 0;
            while (s->enqueued - now < s->leadFrames && s->renderer.isReadyForMoreMediaData && n++ < 64) {
                unsigned got = s->read(s->ctx, s->buf, s->block);
                (void)got;
                if (s->gain != 1.0f) {
                    size_t k = (size_t)s->block * (size_t)s->channels;
                    for (size_t i = 0; i < k; i++) { float x = s->buf[i] * s->gain; s->buf[i] = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x; }
                }
                if (!enqueue_block(s, s->buf, s->block)) break;
            }
        }
        nanosleep(&tick, NULL);
    }
    return NULL;
}

void *audio_system_spatial_open(int rate, int channels, unsigned block, int lead_ms, float gain, AudioSpatialRead read, void *ctx) {
    if (!audio_system_spatial_available() || channels != 6 || rate <= 0 || !block || !read) return NULL;
    @autoreleasepool {
        TOTKSystemSpatial *s = [TOTKSystemSpatial new];
        s->rate = rate; s->channels = channels; s->block = block; s->gain = gain; s->read = read; s->ctx = ctx;
        s->leadFrames = (int64_t)rate * lead_ms / 1000;
        s->buf = calloc((size_t)block * (size_t)channels, sizeof(float));
        if (!s->buf) return NULL;
        AudioStreamBasicDescription asbd = {
            .mSampleRate = rate, .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
            .mBytesPerPacket = (UInt32)(4 * channels), .mFramesPerPacket = 1, .mBytesPerFrame = (UInt32)(4 * channels),
            .mChannelsPerFrame = (UInt32)channels, .mBitsPerChannel = 32,
        };
        AudioChannelLayout layout = {.mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_5_1_A};
        if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &asbd, sizeof layout, &layout, 0, NULL, NULL, &s->format) != noErr) {
            free(s->buf); return NULL;
        }
        s->sync = [[AVSampleBufferRenderSynchronizer alloc] init];
        if (!s->sync || !start_renderer(s)) {
            fprintf(stderr, "[totk-audio] 【红】spatial renderer could not start\n");
            if (s->renderer) { [s->sync setRate:0]; [s->renderer flush]; }
            if (s->flushObserver) [NSNotificationCenter.defaultCenter removeObserver:s->flushObserver];
            CFRelease(s->format); free(s->buf); return NULL;
        }
        void *handle = (void *)CFBridgingRetain(s);
        if (pthread_create(&s->thread, NULL, feeder, handle)) {
            [s->sync setRate:0]; [s->renderer flush];
            [NSNotificationCenter.defaultCenter removeObserver:s->flushObserver];
            CFRelease(s->format); free(s->buf); CFBridgingRelease(handle); return NULL;
        }
        s->threadStarted = 1;
        return handle;
    }
}

void audio_system_spatial_close(void *handle) {
    if (!handle) return;
    @autoreleasepool {
        TOTKSystemSpatial *s = CFBridgingRelease(handle);
        atomic_store(&s->stop, 1);
        if (s->threadStarted) pthread_join(s->thread, NULL);
        if (s->flushObserver) [NSNotificationCenter.defaultCenter removeObserver:s->flushObserver];
        s->flushObserver = nil;
        [s->sync setRate:0];
        if (s->renderer) {
            [s->renderer flush];
            [s->sync removeRenderer:s->renderer atTime:kCMTimeInvalid completionHandler:nil];
        }
        fprintf(stderr, "[totk-audio] spatial renderer closed: %llu blocks, %llu late, %llu automatic flushes, %llu failures\n",
                s->blocks, s->late, s->flushes, s->failures);
        s->renderer = nil; s->sync = nil;
        if (s->format) CFRelease(s->format);
        s->format = NULL;
        free(s->buf); s->buf = NULL;
    }
}
#endif
