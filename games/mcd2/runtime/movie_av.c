#include "movie_av.h"
#include <VideoToolbox/VideoToolbox.h>
#include <AudioToolbox/AudioToolbox.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

struct MovieAV {
    int type, w, h, channels;
    CMVideoFormatDescriptionRef format;
    VTDecompressionSessionRef video;
    CVImageBufferRef image;
    OSStatus status;
    AudioConverterRef audio;
};
static void decoded(void *ctx, void *source, OSStatus status, VTDecodeInfoFlags flags,
                    CVImageBufferRef image, CMTime pts, CMTime duration) {
    (void)source; (void)flags; (void)pts; (void)duration;
    MovieAV *c = ctx;
    c->status = status;
    if (c->image) CFRelease(c->image);
    c->image = image ? CVBufferRetain(image) : NULL;
}
static OSStatus video_session(MovieAV *c) {
    int32_t pixel = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef value = CFNumberCreate(NULL, kCFNumberSInt32Type, &pixel);
    const void *keys[] = {kCVPixelBufferPixelFormatTypeKey};
    const void *values[] = {value};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    VTDecompressionOutputCallbackRecord cb = {decoded, c};
    OSStatus status = VTDecompressionSessionCreate(NULL, c->format, NULL, attrs, &cb, &c->video);
    CFRelease(attrs); CFRelease(value);
    return status;
}
MovieAV *movie_av_create(int type, int w, int h, int rate, int channels,
                         const uint8_t *config, size_t n) {
    if (!config || !n || n > UINT32_MAX) return NULL;
    MovieAV *c = calloc(1, sizeof *c); if (!c) return NULL;
    c->type = type; c->w = w; c->h = h; c->channels = channels;
    OSStatus status = -1;
    if (type == 1) {
        // Validate every SPS/PPS length before passing parameter sets to CoreMedia.
        const uint8_t *sets[286]; size_t sizes[286], count = 0, off = 6;
        if (w <= 0 || h <= 0 || n < 7 || config[0] != 1) goto fail;
        unsigned nsps = config[5] & 31;
        if (!nsps) goto fail;
        for (int group = 0; group < 2; group++) {
            unsigned num = nsps;
            if (group) { if (off >= n) goto fail; num = config[off++]; if (!num) goto fail; }
            for (unsigned i = 0; i < num; i++) {
                if (n - off < 2) goto fail;
                size_t len = ((size_t)config[off] << 8) | config[off+1]; off += 2;
                if (!len || len > n-off || count >= 286) goto fail;
                sets[count] = config + off; sizes[count++] = len; off += len;
            }
        }
        status = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, count, sets, sizes, 4, &c->format);
        if (status) goto fail;
        status = video_session(c);
    } else if (type == 4) {
        if (rate <= 0 || channels < 1 || channels > 8) goto fail;
        AudioStreamBasicDescription in = {.mSampleRate=rate, .mFormatID=kAudioFormatMPEG4AAC,
            .mFramesPerPacket=1024, .mChannelsPerFrame=(UInt32)channels};
        AudioStreamBasicDescription out = {.mSampleRate=rate, .mFormatID=kAudioFormatLinearPCM,
            .mFormatFlags=kAudioFormatFlagIsSignedInteger|kAudioFormatFlagIsPacked,
            .mBytesPerPacket=(UInt32)channels*2, .mFramesPerPacket=1,
            .mBytesPerFrame=(UInt32)channels*2, .mChannelsPerFrame=(UInt32)channels, .mBitsPerChannel=16};
        status = AudioConverterNew(&in, &out, &c->audio);
        if (!status) status = AudioConverterSetProperty(c->audio, kAudioConverterDecompressionMagicCookie, (UInt32)n, config);
    }
    if (!status) return c;
fail:
    fprintf(stderr, "[movie] host codec %d configuration failed (%d)\n", type, (int)status);
    movie_av_destroy(c); return NULL;
}
void movie_av_destroy(MovieAV *c) {
    if (!c) return;
    if (c->video) { VTDecompressionSessionWaitForAsynchronousFrames(c->video); VTDecompressionSessionInvalidate(c->video); CFRelease(c->video); }
    if (c->format) CFRelease(c->format);
    if (c->image) CFRelease(c->image);
    if (c->audio) AudioConverterDispose(c->audio);
    free(c);
}
int movie_av_flush(MovieAV *c) {
    if (!c) return -1;
    if (c->audio) return AudioConverterReset(c->audio) ? -1 : 0;
    VTDecompressionSessionWaitForAsynchronousFrames(c->video);
    VTDecompressionSessionInvalidate(c->video); CFRelease(c->video); c->video = NULL;
    if (c->image) { CFRelease(c->image); c->image = NULL; }
    return video_session(c) ? -1 : 0;
}
static size_t start_code(const uint8_t *p, size_t n, size_t at, size_t *prefix) {
    for (size_t i = at; i + 3 <= n; i++) {
        if (p[i] || p[i+1]) continue;
        if (p[i+2] == 1) { *prefix=3; return i; }
        if (i+4 <= n && !p[i+2] && p[i+3] == 1) { *prefix=4; return i; }
    }
    *prefix=0; return n;
}
typedef struct { const uint8_t *data; UInt32 size, channels; int used; AudioStreamPacketDescription packet; } AudioInput;
static OSStatus audio_input(AudioConverterRef converter, UInt32 *count, AudioBufferList *data,
                            AudioStreamPacketDescription **desc, void *ctx) {
    (void)converter; AudioInput *in=ctx;
    if (!in->size) { *count=0; return noErr; }
    if (in->used) { *count=0; return 1; } // temporarily out of packets, not end of stream
    in->used=1; *count=1;
    data->mNumberBuffers=1; data->mBuffers[0]=(AudioBuffer){in->channels,in->size,(void *)in->data};
    in->packet=(AudioStreamPacketDescription){0,0,in->size}; if (desc) *desc=&in->packet;
    return noErr;
}
int movie_av_decode(MovieAV *c, const uint8_t *p, size_t n, int64_t pts, uint8_t *out, size_t cap) {
    if (!c || (!p && n) || !out || n > UINT32_MAX || cap > INT_MAX) return -1;
    if (c->audio) {
        AudioInput input={.data=p,.size=(UInt32)n,.channels=(UInt32)c->channels};
        UInt32 frames=(UInt32)(cap/(c->channels*2));
        AudioBufferList list={.mNumberBuffers=1,.mBuffers={{(UInt32)c->channels,(UInt32)cap,out}}};
        OSStatus status=AudioConverterFillComplexBuffer(c->audio,audio_input,&input,&frames,&list,NULL);
        if (status && status != 1) return -1;
        return (int)(frames*c->channels*2);
    }
    if (!n) return 0;
    // The guest MPEG4Source converts samples to Annex B. Normalize its 3/4-byte
    // start codes to the four-byte lengths required by the format description.
    size_t prefix=0, at=start_code(p,n,0,&prefix);
    if (at != 0 || !prefix || n > (SIZE_MAX-4)/2) return -1;
    uint8_t *bytes=malloc(n*2+4); if (!bytes) return -1;
    size_t used=0;
    while (at < n) {
        size_t begin=at+prefix, nextprefix=0, next=start_code(p,n,begin,&nextprefix);
        size_t len=next-begin;
        if (!len || len > UINT32_MAX) { free(bytes); return -1; }
        bytes[used++]=(uint8_t)(len>>24);bytes[used++]=(uint8_t)(len>>16);
        bytes[used++]=(uint8_t)(len>>8);bytes[used++]=(uint8_t)len;
        memcpy(bytes+used,p+begin,len);used+=len;at=next;prefix=nextprefix;
    }
    CMBlockBufferRef block=NULL; CMSampleBufferRef sample=NULL;
    OSStatus status=CMBlockBufferCreateWithMemoryBlock(NULL,bytes,used,kCFAllocatorNull,NULL,0,used,0,&block);
    CMSampleTimingInfo timing={kCMTimeInvalid,CMTimeMake(pts,1000000),kCMTimeInvalid};
    if (!status) status=CMSampleBufferCreateReady(NULL,block,c->format,1,1,&timing,1,&used,&sample);
    if (c->image) { CFRelease(c->image); c->image=NULL; } c->status=0;
    if (!status) status=VTDecompressionSessionDecodeFrame(c->video,sample,0,NULL,NULL);
    if (!status) status=VTDecompressionSessionWaitForAsynchronousFrames(c->video);
    if (sample) CFRelease(sample);
    if (block) CFRelease(block);
    free(bytes);
    if (status || c->status) return -1;
    if (!c->image) return 0;
    CVPixelBufferRef image=c->image;
    size_t needed=(size_t)c->w*c->h*3/2;
    if (cap < needed || CVPixelBufferGetPlaneCount(image)!=2 ||
        CVPixelBufferGetWidth(image)!=(size_t)c->w || CVPixelBufferGetHeight(image)!=(size_t)c->h) return -1;
    if (CVPixelBufferLockBaseAddress(image,kCVPixelBufferLock_ReadOnly)) return -1;
    for (size_t plane=0;plane<2;plane++) {
        size_t rows=plane?(size_t)c->h/2:(size_t)c->h;
        const uint8_t *src=CVPixelBufferGetBaseAddressOfPlane(image,plane);
        size_t stride=CVPixelBufferGetBytesPerRowOfPlane(image,plane);
        for(size_t y=0;y<rows;y++) memcpy(out+(plane?(size_t)c->w*c->h:0)+y*c->w,src+y*stride,(size_t)c->w);
    }
    CVPixelBufferUnlockBaseAddress(image,kCVPixelBufferLock_ReadOnly);
    return (int)needed;
}
