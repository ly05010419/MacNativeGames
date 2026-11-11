// Synthetic SPS prefixes and pixel buffers, no game files. Exercise the real
// parameter-set selection, output queue and end-of-stream drain without requiring a hardware codec.
// Include the real decoder, without registering unrelated SDK hooks in this test.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_movie.c"
#include <VideoToolbox/VideoToolbox.h>
static OSStatus synthetic_decode(VTDecompressionSessionRef, CMSampleBufferRef, VTDecodeFrameFlags, void *, VTDecodeInfoFlags *);
#define VTDecompressionSessionDecodeFrame synthetic_decode
#include "../hle_movie_avc.c"
#undef VTDecompressionSessionDecodeFrame
#include <assert.h>

uint64_t now_ns(void) { return 1; }
__thread _Atomic int dm_host_work;   // hle_thread.c
void dm_host_lock(pthread_mutex_t *m) { pthread_mutex_lock(m); }
void dm_host_helper_qos(qos_class_t q) { (void)q; }

typedef struct { uint8_t p[512]; unsigned bit; } writer;
static void put(writer *w, unsigned v, unsigned n) {
    while (n--) { w->p[w->bit / 8] |= ((v >> n) & 1) << (7 - w->bit % 8); w->bit++; }
}
static void ue(writer *w, unsigned v) {
    unsigned n = 0, x = ++v;
    while (x >>= 1) n++;
    put(w, 0, n); put(w, v, n + 1);
}
static size_t sps(writer *w, unsigned profile, unsigned poc, unsigned chroma, int scaling) {
    *w = (writer){0}; put(w, 0x67, 8); put(w, profile, 8); put(w, 0, 8); put(w, 41, 8); ue(w, 0);
    if (profile == 100 || profile == 244) {
        ue(w, chroma); if (chroma == 3) put(w, 0, 1);
        ue(w, 0); ue(w, 0); put(w, 0, 1); put(w, scaling, 1);
        if (scaling) for (unsigned i = 0; i < (chroma == 3 ? 12u : 8u); i++) {
            put(w, i == 0, 1);
            if (!i) ue(w, 16); // delta_scale = -8, nextScale = 0: default matrix
        }
    }
    ue(w, 0); ue(w, poc); put(w, 1, 1); // prefix ends here; rbsp stop bit
    return (w->bit + 7) / 8;
}
static void queue_picture(mavc *a) {
    CVPixelBufferRef pb = NULL;
    assert(!CVPixelBufferCreate(NULL, 16, 16, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, NULL, &pb));
    CVPixelBufferLockBaseAddress(pb, 0);
    for (int i = 0; i < 2; i++) memset(CVPixelBufferGetBaseAddressOfPlane(pb, i), i ? 128 : 16,
                                      CVPixelBufferGetBytesPerRowOfPlane(pb, i) * CVPixelBufferGetHeightOfPlane(pb, i));
    CVPixelBufferUnlockBaseAddress(pb, 0);
    on_frame(a, NULL, 0, 0, pb, CMTimeMake(0, 1000000), kCMTimeInvalid);
    CVPixelBufferRelease(pb);
}
// Replace only decompression: the fixture's NAL headers are synthetic, not
// encoded media. Production still parses the access unit, builds its sample,
// passes its frame refcon, queues the callback and copies the output picture.
static OSStatus synthetic_decode(VTDecompressionSessionRef session, CMSampleBufferRef sample, VTDecodeFrameFlags flags,
                                void *source, VTDecodeInfoFlags *info) {
    assert(!flags && !info);   // the production callback must remain synchronous
    uint8_t bytes[64]; CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    size_t n = CMBlockBufferGetDataLength(block);
    assert(n <= sizeof bytes && !CMBlockBufferCopyDataBytes(block, 0, n, bytes));
    int marker = -1;
    for (size_t i = 0; i + 4 <= n;) {
        size_t len = (size_t)bytes[i] << 24 | (size_t)bytes[i + 1] << 16 | (size_t)bytes[i + 2] << 8 | bytes[i + 3]; i += 4;
        assert(len && len <= n - i);
        int type = bytes[i] & 31;
        if (type >= 1 && type <= 5) {
            assert(len == 2 && (marker < 0 || marker == bytes[i + 1]));
            marker = bytes[i + 1];   // multiple slices still produce one picture
        }
        i += len;
    }
    if (marker < 0) return noErr;
    CVPixelBufferRef pb = NULL;
    assert(!CVPixelBufferCreate(NULL, 16, 16, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, NULL, &pb));
    CVPixelBufferLockBaseAddress(pb, 0);
    for (int i = 0; i < 2; i++) memset(CVPixelBufferGetBaseAddressOfPlane(pb, i), i ? 128 : marker,
                                      CVPixelBufferGetBytesPerRowOfPlane(pb, i) * CVPixelBufferGetHeightOfPlane(pb, i));
    CVPixelBufferUnlockBaseAddress(pb, 0);
    on_frame((mavc *)session, source, noErr, 0, pb, CMSampleBufferGetPresentationTimeStamp(sample), kCMTimeInvalid);
    CVPixelBufferRelease(pb);
    return noErr;
}
static mavc *synthetic_avc(void) {
    mavc *a = mavc_new(); a->depth = 4;
    assert(!CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_H264, 16, 16, NULL, &a->fd));
    a->vs = (VTDecompressionSessionRef)a;   // test-only sentinel; no hardware session
    return a;
}
static void synthetic_free(mavc *a) { a->vs = NULL; mavc_free(a); }
static void submit_picture(mavc *a, int annex_b, int idr, int marker, int64_t pts) {
    uint8_t au[12]; size_t n = 0;
    for (int slice = 0; slice < (idr ? 2 : 1); slice++) {
        if (annex_b) { au[n++] = 0; au[n++] = 0; au[n++] = 1; }
        else { au[n++] = 0; au[n++] = 0; au[n++] = 0; au[n++] = 2; }
        au[n++] = idr ? 0x65 : 0x41; au[n++] = (uint8_t)marker;
    }
    assert(!mavc_decode(a, au, (int)n, pts));
}
static void loop_order(int annex_b) {
    static const int decode[] = {0, 3, 1, 2, 6, 4, 5, 9, 7, 8, 11, 10};
    mavc *a = synthetic_avc(); uint8_t out[16 * 16 * 3 / 2]; int64_t pts; int got = 0;
    for (int loop = 0; loop < 3; loop++) for (int i = 0; i < 12; i++) {
        submit_picture(a, annex_b, !i, 16 + loop * 12 + decode[i], decode[i] * 1000);
        // md_avc takes at most one picture for each input packet.
        if (mavc_take(a, 0, out, 16, 16, &pts)) {
            assert(pts == (got % 12) * 1000 && out[0] == 16 + got); got++;
        }
    }
    while (mavc_take(a, 1, out, 16, 16, &pts)) {
        assert(pts == (got % 12) * 1000 && out[0] == 16 + got); got++;
    }
    assert(got == 36 && !mavc_queued(a));
    synthetic_free(a);
}
static void sequence_boundaries(void) {
    mavc *a = synthetic_avc(); uint8_t out[16 * 16 * 3 / 2]; int64_t pts;
    submit_picture(a, 1, 1, 90, 9000);
    submit_picture(a, 1, 0, 60, 6000);
    assert(!mavc_take(a, 0, out, 16, 16, &pts)); // a backward PTS alone is not a boundary
    const uint8_t sei[] = {0, 0, 1, 6, 128};
    assert(!mavc_decode(a, sei, sizeof sei, 0));
    assert(mavc_queued(a) == 2 && !mavc_take(a, 0, out, 16, 16, &pts));
    submit_picture(a, 1, 1, 16, 0);
    // Even a previous sequence shorter than the reorder depth is now complete.
    assert(mavc_take(a, 0, out, 16, 16, &pts) && pts == 6000 && out[0] == 60);
    assert(mavc_take(a, 0, out, 16, 16, &pts) && pts == 9000 && out[0] == 90);
    assert(!mavc_take(a, 0, out, 16, 16, &pts)); // the new sequence still needs lookahead
    submit_picture(a, 0, 0, 17, 1000);
    assert(mavc_queued(a) == 2);
    mavc_drop(a); // Flush discards a populated queue, unlike the EOS drain
    assert(!mavc_queued(a) && !mavc_take(a, 1, out, 16, 16, &pts));
    submit_picture(a, 0, 1, 32, -1000);
    assert(!mavc_take(a, 0, out, 16, 16, &pts));
    assert(mavc_take(a, 1, out, 16, 16, &pts) && pts == -1000 && out[0] == 32);
    assert(!mavc_queued(a));
    a->depth = 0; // Baseline / POC type 2 still returns every picture immediately
    for (int i = 0; i < 3; i++) {
        submit_picture(a, 0, 1, 40 + i, 0);
        assert(mavc_take(a, 0, out, 16, 16, &pts) && pts == 0 && out[0] == 40 + i);
        assert(!mavc_queued(a));
    }
    synthetic_free(a);
}
// End of stream with three pictures held for reordering (POC type 0): an end-of-stream packet without data, nout output slots of which the client
// returns each one it takes. Every picture comes out without the end-of-stream flag, then one output without a picture carries it with the packet's time
static void drain(int nout) {
    static mdec_t d; d = (mdec_t){ .type = MD_TYPE_AVC, .fmt = MD_FMT_NV12, .nout = nout, .started = 1, .w = 16, .h = 16 };
    pthread_mutex_init(&d.mu, NULL); pthread_mutex_init(&d.dmu, NULL); pthread_mutex_init(&d.wmu, NULL); pthread_cond_init(&d.wcv, NULL);
    assert(!pthread_create(&d.wk, NULL, md_worker, &d));
    md_avc_open(&d); writer w; size_t n = sps(&w, 100, 0, 1, 0); keep_ps(d.avc, w.p, n);
    for (int i = 0; i < 3; i++) queue_picture(d.avc);
    d.pend[0].in = 0; d.pend[0].pts = 777; d.pend[0].flags = 4; d.npend = 1; d.ninfree = MD_NIN - 1;
    md_pump(&d);
    void *self[2] = { NULL, &d }; int got = 0, idx, left; int64_t pts; uint32_t flags;
    for (;;) {   // the client returns each output it takes, so the next one is always ready
        assert(!h_movie_AcqOut(self, &idx, &pts, &flags, &left));
        if (++got < 4) assert(d.out[idx].pic && pts == 0 && !(flags & 4));
        else { assert(!d.out[idx].pic && pts == 777 && flags == 4 && !d.avc_drain && !mavc_queued(d.avc) && !left); break; }
        assert(md_rel(&d, idx));
    }
    md_job(&d, MDJ_QUIT); pthread_join(d.wk, NULL);
    for (int i = 0; i < MD_NOUT; i++) free(d.out[i].frm);
}
// Native-texture slots (md_rendered): one written into the client's texture stays taken, so no packet is decoded into it before the client's own release can come (GitHub
// issue #700). A client that releases what it rendered keeps every slot until its release; one that has not yet gets the earlier slot back at its next GetOutputBuffer
static void take(mdec_t *d, int i) { d->out[i].used = d->out[i].cli = 1; }
static int rendered(mdec_t *d, int i) { pthread_mutex_lock(&d->mu); int n = md_rendered(d, i); pthread_mutex_unlock(&d->mu); return n; }
static void tex_slots(void) {
    static mdec_t d; d = (mdec_t){ .type = MD_TYPE_AVC, .mode = MD_MODE_TEX, .nout = 4, .started = 1 };
    pthread_mutex_init(&d.mu, NULL); pthread_mutex_init(&d.dmu, NULL);
    take(&d, 0); assert(!rendered(&d, 0) && d.out[0].used && d.out[0].rnd);
    take(&d, 1); assert(rendered(&d, 1) == 1 && !d.out[0].used && !d.out[0].cli && d.out[1].rnd);   // never released: slot 0 returns at the next GetOutputBuffer
    assert(md_rel(&d, 1) && d.tex_rel && !d.out[1].used);   // the client releases a rendered slot itself
    take(&d, 2); assert(!rendered(&d, 2));
    take(&d, 3); assert(!rendered(&d, 3) && d.out[2].used && d.out[2].rnd);   // now its slots wait for its release
    take(&d, 0); assert(md_rel(&d, 0) && d.tex_rel && !d.out[0].used);   // a frame dropped without GetOutputBuffer
    assert(md_rel(&d, 2) && md_rel(&d, 3) && !md_rel(&d, 3));   // a second release changes nothing
    take(&d, 1); md_reset(&d, 0); assert(!d.out[1].used && !d.out[1].rnd && d.tex_rel);
}
int main(void) {
    loop_order(1); loop_order(0);
    puts("AVC loops: all 36 pictures ordered through packet parsing and callback refcons, with both NAL prefix formats");
    sequence_boundaries();
    puts("AVC boundaries: short sequences drain, non-IDR timestamps and SEI retain lookahead, Flush discards, zero-depth output stays immediate");
    tex_slots();
    puts("native texture: a rendered slot waits for the client's release, or for its next GetOutputBuffer while it has never released one");
    writer w;
    for (unsigned profile = 100; profile <= 244; profile += 144) for (unsigned chroma = 0; chroma <= 3; chroma++) {
        for (int scaling = 0; scaling < 2; scaling++) {
            size_t n = sps(&w, profile, 2, chroma, scaling);
            assert(avc_reorder_depth(w.p, n) == 0);
            for (size_t cut = 0; cut < n; cut++) assert(avc_reorder_depth(w.p, cut) == 4);
        }
    }
    for (unsigned poc = 0; poc <= 1; poc++) {
        size_t n = sps(&w, 100, poc, 1, 0);
        assert(avc_reorder_depth(w.p, n) == 4);
    }
    size_t n = sps(&w, 66, 0, 1, 0); assert(avc_reorder_depth(w.p, n) == 0);
    memset(w.p, 0, sizeof w.p); w.p[0] = 0x67; w.p[1] = 100;
    assert(avc_reorder_depth(w.p, sizeof w.p) == 4); // unterminated Exp-Golomb code
    assert(avc_reorder_depth(w.p, sizeof w.p + 1) == 4);

    mavc *a = mavc_new(); uint8_t out[16 * 16 * 3 / 2]; int64_t pts = -1;
    n = sps(&w, 100, 2, 1, 0); keep_ps(a, w.p, n); queue_picture(a);
    assert(a->depth == 0 && mavc_take(a, 0, out, 16, 16, &pts) == 1 && pts == 0 && !mavc_queued(a));
    assert(out[0] == 16 && out[256] == 128);
    n = sps(&w, 100, 0, 1, 0); keep_ps(a, w.p, n); queue_picture(a);
    assert(a->depth == 4 && !mavc_take(a, 0, out, 16, 16, &pts) && mavc_queued(a) == 1);
    assert(mavc_take(a, 1, out, 16, 16, &pts) == 1 && !mavc_queued(a));
    mavc_free(a);
    puts("AVC: POC type 2 returns the final picture without EOS; other POC modes retain reordering and EOS drain");
    drain(MD_NOUT_VIDEO); drain(2);
    puts("AVC end of stream: every queued picture leaves without the flag, then an output without a picture carries it, also when the client holds the slots");
}
