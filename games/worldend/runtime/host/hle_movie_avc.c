// H.264 (AVC) decoding for the movie decoder (hle_movie.c), on Apple's VideoToolbox (the hardware decoder on the Mac and the iPhone).
// Super Mario Odyssey's 44 movies are MP4 / H.264 (the hint videos in its tutorial boxes, the kingdom travel videos): 41 High or Main profile with B-frames, 3 Baseline.
// The SDK's demuxer hands over one access unit per packet with Annex B start codes; SPS / PPS come in Configure's csd-0 / csd-1 (as with Android's MediaCodec) and may also appear in the stream.
// VideoToolbox wants a format description built from SPS / PPS and NAL units with 4-byte length prefixes. Decoding is synchronous and yields pictures in decode order; MediaCodec yields them
// in presentation order, so pictures wait in a queue sorted by presentation time and leave it once more than `depth` are queued (4 when the profile has B-frames, 0 for Baseline), or all at the end of the stream
#include "hle.h"
#include <VideoToolbox/VideoToolbox.h>
#include <arm_neon.h>
#include "hle_movie_avc.h"

#define QMAX 16
struct mavc {
    uint8_t sps[512], pps[512]; int nsps, npps, dirty;
    CMVideoFormatDescriptionRef fd; VTDecompressionSessionRef vs;
    struct { CVPixelBufferRef pb; int64_t pts; } q[QMAX]; int nq, depth;
    uint8_t *au; size_t aucap;   // length-prefixed copy of the access unit being decoded
    int red, rgba;               // rgba: pictures as 4-byte RGBA (native texture mode, Monster Hunter Rise) instead of NV12
};

mavc *mavc_new(void) { mavc *a = calloc(1, sizeof *a); if (!a) die("movie: AVC decoder allocation failed"); return a; }
static void close_session(mavc *a) {
    if (a->vs) { VTDecompressionSessionWaitForAsynchronousFrames(a->vs); VTDecompressionSessionInvalidate(a->vs); CFRelease(a->vs); a->vs = NULL; }
    if (a->fd) { CFRelease(a->fd); a->fd = NULL; }
}
void mavc_drop(mavc *a) { for (int i = 0; i < a->nq; i++) CVPixelBufferRelease(a->q[i].pb); a->nq = 0; }
void mavc_free(mavc *a) { if (!a) return; close_session(a); mavc_drop(a); free(a->au); free(a); }
int mavc_queued(const mavc *a) { return a->nq; }

static void keep_ps(mavc *a, const uint8_t *nal, size_t n) {   // a parameter set NAL unit (without start code)
    int t = nal[0] & 31; uint8_t *dst = t == 7 ? a->sps : a->pps; int *dn = t == 7 ? &a->nsps : &a->npps;
    if (n > sizeof a->sps) { if (!a->red++) LOG("[RED] movie: AVC parameter set of %zu bytes, too long", n); return; }
    if ((size_t)*dn == n && !memcmp(dst, nal, n)) return;
    memcpy(dst, nal, n); *dn = (int)n; a->dirty = 1;
    if (t == 7 && n > 1) a->depth = nal[1] == 66 ? 0 : 4;   // profile_idc: Baseline has no B-frames
}
// walk the NAL units of p (Annex B start codes, or else 4-byte lengths); fn gets each unit without its prefix. Zero bytes before a start code (the first byte of a 4-byte
// start code, trailing_zero_8bits) are not part of the unit
static size_t next_sc(const uint8_t *p, size_t n, size_t i) { for (; i + 3 <= n; i++) if (!p[i] && !p[i + 1] && p[i + 2] == 1) return i; return n; }
static void each_nal(const uint8_t *p, size_t n, void (*fn)(void *, const uint8_t *, size_t), void *ctx) {
    size_t s = next_sc(p, n, 0);
    if (s < n && s <= 1) {
        while (s < n) {
            size_t b = s + 3, e = next_sc(p, n, b), end = e;
            while (end > b && !p[end - 1]) end--;
            if (end > b) fn(ctx, p + b, end - b);
            s = e;
        }
    } else for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)p[i] << 24 | (size_t)p[i + 1] << 16 | (size_t)p[i + 2] << 8 | p[i + 3]; i += 4;
        if (!len || len > n - i) break;
        fn(ctx, p + i, len); i += len;
    }
}
static void ps_only(void *ctx, const uint8_t *nal, size_t n) { int t = nal[0] & 31; if (t == 7 || t == 8) keep_ps(ctx, nal, n); }
void mavc_param_sets(mavc *a, const uint8_t *p, size_t n) { each_nal(p, n, ps_only, a); }
// AVCDecoderConfigurationRecord: version, profile, compatibility, level, NAL length size - 1, SPS count (low 5 bits) and 16-bit-length SPS, PPS count and 16-bit-length PPS
void mavc_avcc(mavc *a, const uint8_t *p, size_t n) {
    if (n < 7 || p[0] != 1) { if (!a->red++) LOG("[RED] movie: unrecognized avcC record (%zu bytes, version %d)", n, n ? p[0] : -1); return; }
    size_t i = 5;
    for (int set = 0; set < 2 && i < n; set++) {
        int cnt = set ? p[i] : p[i] & 31; i++;
        for (int k = 0; k < cnt && i + 2 <= n; k++) { size_t len = (size_t)p[i] << 8 | p[i + 1]; i += 2; if (len > n - i) return; if (len) keep_ps(a, p + i, len); i += len; }
    }
}

static void on_frame(void *ref, void *src, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
    (void)src; (void)fl; (void)dur; mavc *a = ref;
    if (st || !img) { if (!a->red++) LOG("[RED] movie: AVC decode error (VideoToolbox %d, first case)", (int)st); return; }
    if (a->nq == QMAX) { if (!a->red++) LOG("[RED] movie: AVC reorder queue full, dropping the oldest frame"); CVPixelBufferRelease(a->q[0].pb); memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]); }
    int64_t t = pts.timescale ? pts.value * 1000000 / pts.timescale : 0;
    int k = a->nq; while (k > 0 && a->q[k - 1].pts > t) { a->q[k] = a->q[k - 1]; k--; }
    a->q[k].pb = CVPixelBufferRetain(img); a->q[k].pts = t; a->nq++;
}
static int open_session(mavc *a) {
    close_session(a);
    if (!a->nsps || !a->npps) { if (!a->red++) LOG("[RED] movie: AVC has no SPS / PPS, outputting black frames"); return 0; }
    const uint8_t *ps[2] = {a->sps, a->pps}; size_t sz[2] = {(size_t)a->nsps, (size_t)a->npps};
    OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, 2, ps, sz, 4, &a->fd);
    if (st) { if (!a->red++) LOG("[RED] movie: AVC parameter sets give no format description (%d), outputting black frames", (int)st); return 0; }
    int32_t pf = a->rgba ? kCVPixelFormatType_32BGRA : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;   // NV12, video range: what the SDK's decoder hands out; BGRA (converted by VideoToolbox with the stream's colour matrix) for RGBA output
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf);
    const void *k[] = {kCVPixelBufferPixelFormatTypeKey}, *v[] = {n};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(n);
    VTDecompressionOutputCallbackRecord cb = {on_frame, a};
    st = VTDecompressionSessionCreate(NULL, a->fd, NULL, attrs, &cb, &a->vs); CFRelease(attrs);
    if (st) { if (!a->red++) LOG("[RED] movie: VideoToolbox cannot create an AVC decode session (%d), outputting black frames", (int)st); a->vs = NULL; return 0; }
    CMVideoDimensions dm = CMVideoFormatDescriptionGetDimensions(a->fd);
    LOG("[movie] AVC: VideoToolbox decoding %d×%d, profile %d, reorder depth %d", dm.width, dm.height, a->nsps > 1 ? a->sps[1] : 0, a->depth);
    a->dirty = 0; return 1;
}
typedef struct { mavc *a; size_t len; } au_ctx;
static void au_add(void *ctx, const uint8_t *nal, size_t n) {
    au_ctx *c = ctx; mavc *a = c->a; int t = nal[0] & 31;
    if (t == 7 || t == 8) { keep_ps(a, nal, n); return; }
    if (t == 9) return;   // access unit delimiter
    if (c->len + 4 + n > a->aucap) { a->aucap = (c->len + 4 + n) * 2; a->au = realloc(a->au, a->aucap); if (!a->au) die("movie: AVC buffer allocation failed"); }
    uint8_t *o = a->au + c->len; o[0] = (uint8_t)(n >> 24); o[1] = (uint8_t)(n >> 16); o[2] = (uint8_t)(n >> 8); o[3] = (uint8_t)n;
    memcpy(o + 4, nal, n); c->len += 4 + n;
}
int mavc_decode(mavc *a, const uint8_t *p, int n, int64_t pts_us) {
    au_ctx c = {a, 0}; each_nal(p, (size_t)n, au_add, &c);
    if (!c.len) return 0;
    if ((a->dirty || !a->vs) && !open_session(a)) return -1;
    CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, c.len, NULL, NULL, 0, c.len, 0, &bb);
    if (!st) st = CMBlockBufferReplaceDataBytes(a->au, bb, 0, c.len);
    CMSampleTimingInfo ti = {kCMTimeInvalid, CMTimeMake(pts_us, 1000000), kCMTimeInvalid}; size_t ssz = c.len;
    if (!st) st = CMSampleBufferCreateReady(NULL, bb, a->fd, 1, 1, &ti, 1, &ssz, &sb);
    if (!st) st = VTDecompressionSessionDecodeFrame(a->vs, sb, 0, NULL, NULL);   // flags 0: synchronous, the picture is queued before this returns
    if (sb) CFRelease(sb); if (bb) CFRelease(bb);
    if (st) { if (!a->red++) LOG("[RED] movie: AVC decode submission failed (%d, first case)", (int)st); return -1; }
    return 0;
}
void mavc_set_rgba(mavc *a) { if (!a->rgba) { a->rgba = 1; a->dirty = 1; } }
// BGRA picture → RGBA rows of w pixels (stride w * 4); a smaller picture is padded with opaque black, a larger one cropped
static void take_rgba(CVPixelBufferRef pb, uint8_t *dst, int w, int h) {
    int pw = (int)CVPixelBufferGetWidth(pb), ph = (int)CVPixelBufferGetHeight(pb), cw = pw < w ? pw : w, chh = ph < h ? ph : h;
    const uint8_t *src = CVPixelBufferGetBaseAddress(pb); size_t ss = CVPixelBufferGetBytesPerRow(pb);
    if (cw < w || chh < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    for (int r = 0; r < chh; r++) {
        const uint8_t *s = src + r * ss; uint8_t *o = dst + (size_t)r * w * 4; int x = 0;
        for (; x + 16 <= cw; x += 16) { uint8x16x4_t v = vld4q_u8(s + 4 * x); uint8x16_t t = v.val[0]; v.val[0] = v.val[2]; v.val[2] = t; vst4q_u8(o + 4 * x, v); }
        for (; x < cw; x++) { o[4 * x] = s[4 * x + 2]; o[4 * x + 1] = s[4 * x + 1]; o[4 * x + 2] = s[4 * x]; o[4 * x + 3] = 255; }
    }
}
int mavc_take(mavc *a, int flush, uint8_t *dst, int w, int h, int64_t *pts_us) {
    if (!a->nq || (!flush && a->nq <= a->depth)) return 0;
    CVPixelBufferRef pb = a->q[0].pb; if (pts_us) *pts_us = a->q[0].pts;
    memmove(a->q, a->q + 1, --a->nq * sizeof a->q[0]);
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    if (a->rgba) { take_rgba(pb, dst, w, h); CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly); CVPixelBufferRelease(pb); return 1; }
    int pw = (int)CVPixelBufferGetWidthOfPlane(pb, 0), ph = (int)CVPixelBufferGetHeightOfPlane(pb, 0);
    const uint8_t *y = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uv = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    int cw = pw < w ? pw : w, chh = ph < h ? ph : h;
    if (cw < w || chh < h) { memset(dst, 16, (size_t)w * h); memset(dst + (size_t)w * h, 128, (size_t)w * h / 2); }
    for (int r = 0; r < chh; r++) memcpy(dst + (size_t)r * w, y + r * ys, (size_t)cw);
    size_t uvb = (size_t)((cw + 1) & ~1); if (uvb > (size_t)w) uvb = (size_t)w;   // interleaved U V: one byte per luma column
    for (int r = 0; r < (chh + 1) / 2 && r < h / 2; r++) memcpy(dst + (size_t)w * h + (size_t)r * w, uv + r * uvs, uvb);
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CVPixelBufferRelease(pb);
    return 1;
}
