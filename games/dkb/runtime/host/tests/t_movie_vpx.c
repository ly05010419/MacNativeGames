// Synthetic 16x16 red key frames encoded with ffmpeg's libvpx encoders; no game data.
// Include the real decoder, without registering unrelated SDK hooks in this test.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include "../hle_movie.c"
#include <assert.h>

static const uint8_t vp8[] = {
    0xf0,0x02,0x00,0x9d,0x01,0x2a,0x10,0x00,0x10,0x00,0x00,0x47,0x08,0x85,0x85,0x88,
    0x85,0x84,0x88,0x02,0x02,0x02,0x75,0xaa,0x03,0xf8,0x03,0xfa,0x02,0x0d,0x4d,0x18,
    0x00,0xfe,0xfd,0x6e,0xf3,0xff,0xe3,0x99,0x37,0x30,0xc4,0xff,0x8e,0x6d,0xff,0xf1,
    0x61,0x3c,0x0e,0x28,0xc8,0xff,0xf1,0x51,0x00,
};
static const uint8_t vp9[] = {
    0x82,0x49,0x83,0x42,0x00,0x00,0xf0,0x00,0xf6,0x00,0x38,0x24,0x1c,0x18,0x4a,0x00,
    0x00,0x30,0x60,0x00,0x00,0x10,0xbf,0xff,0xf7,0x1d,0xaf,0xff,0xff,0xff,0x5f,0xdf,
    0xff,0xff,0xff,0xf2,0x2a,0xc0,0x00,
};
static void decode(int type, int format, const uint8_t *packet, int size) {
    mdec_t d = { .type = type, .fmt = format, .w = 16, .h = 16 };
    assert(md_vpx(&d, packet, size, 0) == 1);
    const uint8_t *p = d.out[0].frm;
    if (format == MD_FMT_NV12) {
        assert(d.out[0].cap == 16 * 16 * 3 / 2);
        for (int i = 0; i < 256; i++) assert(abs(p[i] - 81) <= 3);
        for (int i = 256; i < 384; i += 2) { assert(abs(p[i] - 90) <= 3); assert(abs(p[i+1] - 240) <= 3); }
    } else {
        assert(d.out[0].cap == 16 * 16 * 4);
        for (int i = 0; i < 1024; i += 4) assert(p[i] >= 250 && p[i+1] <= 5 && p[i+2] <= 5 && p[i+3] == 255);
    }
    vpx_codec_destroy(&d.vpx);
    free(d.out[0].frm);
}
// The frame step's movie signal (dm_movie_playing): a video picture handed out raises it for MD_PLAYING_NS; an audio output does not
static uint64_t test_now = 1000000000ull;
uint64_t now_ns(void) { return test_now; }
__thread _Atomic int dm_host_work;   // hle_thread.c
void dm_host_lock(pthread_mutex_t *m) { pthread_mutex_lock(m); }
void mavc_drop(mavc *a) { (void)a; }   // hle_movie_avc.c: no H.264 here
static void playing(void) {
    static mdec_t video = { .type = MD_TYPE_VP9, .nout = MD_NOUT_VIDEO, .started = 1 }, audio = { .type = MD_TYPE_VORBIS, .nout = MD_NOUT, .started = 1 };
    pthread_mutex_init(&video.mu, NULL); pthread_mutex_init(&audio.mu, NULL);
    void *vs[2] = { NULL, &video }, *as[2] = { NULL, &audio };
    int idx, left; int64_t pts; uint32_t flags;
    assert(!dm_movie_playing());
    audio.outq[0] = 0; audio.noutq = 1;
    assert(!h_movie_AcqOut(as, &idx, &pts, &flags, &left) && idx == 0 && !dm_movie_playing());
    video.outq[0] = 3; video.noutq = 1;
    assert(!h_movie_AcqOut(vs, &idx, &pts, &flags, &left) && idx == 3 && dm_movie_playing());
    test_now += MD_PLAYING_NS - 1; assert(dm_movie_playing());
    assert(h_movie_AcqOut(vs, &idx, &pts, &flags, &left) == MD_NO_OUTPUT && dm_movie_playing());   // no picture ready: nothing handed out, the signal is not renewed
    test_now += 1; assert(!dm_movie_playing());
    puts("movie signal: a video picture raises it for half a second, audio outputs do not");
}
// A Flush (a seek, or a movie that loops) must not end the movie's Vorbis sound. A synthetic one-channel stream (both block sizes 64, all-zero residue; no game data), made as the demuxer hands it over:
// the identification and setup headers, then raw audio packets. Before the fix, stb_vorbis_flush_pushdata left the decoder searching for an Ogg page and it gave no sample again
typedef struct { unsigned char data[1024]; int bits; } vpkt;
static void vbits(vpkt *p, unsigned value, int n) { for (int i = 0; i < n; i++, p->bits++) p->data[p->bits / 8] |= ((value >> i) & 1) << (p->bits % 8); }
static void vhead(vpkt *p, int type) { vbits(p, type, 8); for (int i = 0; i < 6; i++) vbits(p, "vorbis"[i], 8); }
static void vcodebook(vpkt *p, int lookup) {
    vbits(p, 0x564342, 24); vbits(p, 1, 16); vbits(p, 1, 24);     // one dimension, one entry
    vbits(p, 0, 1); vbits(p, 0, 1); vbits(p, 0, 5);                // unordered, dense, one-bit code
    vbits(p, lookup, 4);
    if (lookup) { vbits(p, (788u << 21) | 1, 32); vbits(p, 0, 32); vbits(p, 0, 4); vbits(p, 0, 1); vbits(p, 0, 1); }   // minimum 1.0, delta 0.0, one-bit multiplicand, no sequence
}
static int vorbis_samples(mdec_t *d, vpkt *packets, int n) {
    int got = 0; for (int i = 0; i < n; i++) got += md_vorbis(d, packets[i].data, (packets[i].bits + 7) / 8, 0); return got;
}
// One channel, blocksizes as in the identification header (0x66: both 64; 0x86: 64 and 256), one mode per window size in use; packets as vaudio makes them
static void vstream(mdec_t *d, vpkt *id, vpkt *setup, int blocksizes, int modes) {
    vhead(id, 1); vbits(id, 0, 32); vbits(id, 1, 8); vbits(id, 48000, 32); for (int i = 0; i < 3; i++) vbits(id, 0, 32); vbits(id, blocksizes, 8); vbits(id, 1, 8);
    vhead(setup, 5); vbits(setup, 1, 8); vcodebook(setup, 0); vcodebook(setup, 1);
    vbits(setup, 0, 6); vbits(setup, 0, 16);                                          // one time configuration
    vbits(setup, 0, 6); vbits(setup, 1, 16); vbits(setup, 0, 5); vbits(setup, 0, 2); vbits(setup, 5, 4); vbits(setup, 0, 6);   // one floor 1, no partitions
    vbits(setup, 0, 16); vbits(setup, 0, 24); vbits(setup, 32, 24); vbits(setup, 31, 24); vbits(setup, 0, 6); vbits(setup, 0, 8);   // residue 0: partition size 32, classbook 0
    vbits(setup, 1, 3); vbits(setup, 0, 1); vbits(setup, 1, 8);                       // pass 0 uses vector codebook 1
    vbits(setup, 0, 6); vbits(setup, 0, 16); vbits(setup, 0, 1); vbits(setup, 0, 1); vbits(setup, 0, 2);   // one mapping, no coupling, reserved bits
    vbits(setup, 0, 8); vbits(setup, 0, 8); vbits(setup, 0, 8);
    vbits(setup, modes - 1, 6); for (int m = 0; m < modes; m++) { vbits(setup, m, 1); vbits(setup, 0, 16); vbits(setup, 0, 16); vbits(setup, 0, 8); }   // mode m: short (0) or long (1) window
    vbits(setup, 1, 1);
    d->vhdr[0] = id->data; d->vhdr_n[0] = (id->bits + 7) / 8; d->vhdr[1] = setup->data; d->vhdr_n[1] = (setup->bits + 7) / 8;
    pthread_mutex_init(&d->mu, NULL); pthread_mutex_init(&d->dmu, NULL);
}
static void vaudio(vpkt *p, int modes, int blockflag, int prev_long, int next_long) {   // the window flags are only in a long window's packet
    vbits(p, 0, 1);
    if (modes > 1) { vbits(p, blockflag, 1); if (blockflag) { vbits(p, prev_long, 1); vbits(p, next_long, 1); } }
    vbits(p, 1, 1); vbits(p, 255, 8); vbits(p, 255, 8); vbits(p, 0, 1); vbits(p, 0, 32);
}
static void vorbis_flush(void) {
    static vpkt id, setup, audio[8]; static mdec_t d = { .type = MD_TYPE_VORBIS, .nout = MD_NOUT, .started = 1 };
    vstream(&d, &id, &setup, 0x66, 1);
    for (int i = 0; i < 8; i++) vaudio(&audio[i], 1, 0, 0, 0);
    int before = vorbis_samples(&d, audio, 8);
    assert(before > 0);
    void *self[2] = { NULL, &d };
    assert(h_movie_Flush(self) == 0);
    assert(vorbis_samples(&d, audio, 8) == before);   // the stream starts afresh: the same samples again
    assert(h_movie_Flush(self) == 0 && h_movie_Flush(self) == 0);   // Dread and others flush twice in a row
    assert(vorbis_samples(&d, audio, 8) == before);
    puts("Vorbis movie sound: the samples come again after a Flush");
}
// At a window change each packet leaves with (previous blocksize + its blocksize) / 4 samples. With blocksizes 64 / 256, stb_vorbis gives 176 and 32 for a long window before a short one and the short one,
// where the specification has 128 and 80. Samples kept across a change do not survive a Flush
static void vorbis_window_change(void) {
    static vpkt id, setup, audio[11]; static mdec_t d = { .type = MD_TYPE_VORBIS, .nout = MD_NOUT, .started = 1 };
    static const int window[11] = { 256, 256, 256, 64, 64, 256, 256, 64, 256, 64, 256 };
    vstream(&d, &id, &setup, 0x86, 2);
    for (int i = 0; i < 11; i++) vaudio(&audio[i], 2, window[i] == 256, i && window[i - 1] == 256, i == 10 || window[i + 1] == 256);
    void *self[2] = { NULL, &d };
    for (int run = 0; run < 2; run++) {
        assert(!md_vorbis(&d, audio[0].data, (audio[0].bits + 7) / 8, 0));   // the first packet only primes the overlap
        for (int i = 1; i < 11; i++) {
            assert(md_vorbis(&d, audio[i].data, (audio[i].bits + 7) / 8, 0));
            assert(d.out[0].bytes == (size_t)(window[i - 1] + window[i]) / 4 * sizeof(int16_t));
        }
        assert(!d.hold_n);   // the last window has a long one after it: nothing lies past its centre
        assert(h_movie_Flush(self) == 0);
        for (int i = 0; i < 3; i++) md_vorbis(&d, audio[i].data, (audio[i].bits + 7) / 8, 0);
        assert(d.hold_n);    // the long window before a short one left samples behind
        assert(h_movie_Flush(self) == 0 && !d.hold_n && !d.vb_prev);
    }
    puts("Vorbis movie sound: outputs as long as the specification says at a window change");
}
// An output takes the values asked for and leaves the rest for the next one; at most what is kept
static void vorbis_split(void) {
    static mdec_t d = { .type = MD_TYPE_VORBIS, .nout = MD_NOUT, .started = 1 };
    d.hold_cap = 1472 + 128; d.hold = malloc(d.hold_cap * sizeof *d.hold); d.hold_n = d.hold_cap;
    for (size_t i = 0; i < d.hold_n; i++) d.hold[i] = (int16_t)i;
    assert(md_vorbis_emit(&d, 0, 1024) && d.out[0].bytes == 1024 * sizeof *d.hold && d.hold_n == 576);
    assert(((int16_t *)d.out[0].frm)[1023] == 1023 && d.hold[0] == 1024);
    assert(md_vorbis_emit(&d, 0, 1024) && d.out[0].bytes == 576 * sizeof *d.hold && !d.hold_n);
    assert(!md_vorbis_emit(&d, 0, 1024));
    puts("Vorbis movie sound: an output takes the length asked for and leaves the rest");
}
// SDK 1 exports these three getters without const. Later SDKs export the const overloads.
typedef struct { char bytes[16]; size_t used; } test_buffer;
static void *buffer_data(const void *buf) { return (void *)((const test_buffer *)buf)->bytes; }
static size_t buffer_size(const void *buf) { return ((const test_buffer *)buf)->used; }
static size_t buffer_capacity(const void *buf) { return sizeof ((const test_buffer *)buf)->bytes; }
static void unused_export(void) { abort(); }
static int exports_const, exports_mutable, mutable_lookups;
void *guest_sym(const char *name) {
    static const struct { const char *modern, *early; void *fn; } getters[] = {
        {"_ZNK5movie6Buffer4DataEv", "_ZN5movie6Buffer4DataEv", buffer_data},
        {"_ZNK5movie6Buffer8CapacityEv", "_ZN5movie6Buffer8CapacityEv", buffer_capacity},
        {"_ZNK5movie6Buffer4SizeEv", "_ZN5movie6Buffer4SizeEv", buffer_size},
    };
    for (size_t i = 0; i < sizeof getters / sizeof *getters; i++) {
        if (!strcmp(name, getters[i].modern)) return exports_const ? getters[i].fn : NULL;
        if (!strcmp(name, getters[i].early)) { mutable_lookups++; return exports_mutable ? getters[i].fn : NULL; }
    }
    return unused_export;  // the other required SDK methods exist but this test never calls them
}
static void movie_buffer_exports(void) {
    test_buffer buf = {.bytes = "frame", .used = 5};
    for (int sdk = 1; sdk <= 3; sdk++) {  // mutable only, const only, both
        exports_const = sdk & 2; exports_mutable = sdk & 1; mutable_lookups = 0;
        memset(&G, 0, sizeof G);
        md_syms();
        assert(G.data(&buf) == buf.bytes && G.size(&buf) == 5 && G.cap(&buf) == 16);
        assert(mutable_lookups == (exports_const ? 0 : 3));
    }
    memset(&G, 0, sizeof G);
}
int main(void) {
    movie_buffer_exports();
    playing();
    vorbis_flush();
    vorbis_window_change();
    vorbis_split();
    int w = 0, h = 0;
    assert(!vp8_size(vp8, 9, &w, &h));
    assert(vp8_size(vp8, sizeof vp8, &w, &h) && w == 16 && h == 16);
    for (int format = MD_FMT_ABGR; format <= MD_FMT_NV12; format++) {
        decode(3, format, vp9, sizeof vp9);
        decode(2, format, vp8, sizeof vp8);
    }
    puts("VP8 and VP9 key frames: NV12 and RGBA pixels match");
}
