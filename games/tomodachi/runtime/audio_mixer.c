#include <TargetConditionals.h>
/* Audio renderer mixing stage and the IAudioDevice service.
   Per 5 ms frame (called by audio_renderer.c with the renderer lock held, before voices advance):
   voices are decoded (PCM8/16/32, float, Nintendo DSP-ADPCM) and resampled to the output rate with linear
   interpolation along exactly the sample path the voice clock consumes, scaled by voice volume and by the
   channel-resource or splitter-destination volumes into the buffers of their destination mix; submixes are
   folded into their destination in dependency order; the device sink's inputs (5.1 downmixed to stereo) go to
   CoreAudio through a ring buffer. Splitter wire formats follow the public Ryujinx description (REV11: SNDH
   header, SNDI infos with 12 reserved bytes after the ids, SNDD destinations, 16-byte padded section).
   Two per-voice Q14 biquads run after resampling, before volume/routing. 3D reverb, guest auxiliary effects and the output limiter run on each completed mix in effect order.
   Effect algorithms/layouts follow Ryujinx.Audio (MIT; third_party/licenses/Ryujinx-Audio.txt). */
#include "runtime.h"
#include "audio_mixer.h"
#include "audio_reverb.h"
#include <AudioToolbox/AudioToolbox.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FRAME 1024
#define RING_FRAMES 16384            /* stereo frames, ~340 ms at 48 kHz */
#define SPLITTER_UNUSED 0xffffffffu

typedef struct { float vol[24]; int32_t dest; uint8_t used; AudrenBiquadFloat biquads[2]; float state[2][2]; } SplitDest;
typedef struct { int32_t ndest; int32_t *ids; } SplitInfo;
typedef struct {
    Reverb3d *reverb; Reverb3dParam param; int pending;
    float *delay, detector[6], compression[6], peak[6], minimum[6]; int position[6], delay_max, channels;
} EffectState;
typedef struct Mixer {
    float *buf; int nbuf, frame;
    int *offset;                                   /* first mix buffer of each mix id */
    SplitInfo *split; int nsplit; SplitDest *dest; int ndest;
    AudioComponentInstance unit; int unit_ok;
    float ring[RING_FRAMES * 2]; _Atomic uint64_t wr, rd, underruns, overruns;
    EffectState *effect; int *effect_order; unsigned effect_reported;
    unsigned reported;                             /* one-time reports */
} Mixer;

static float device_volume = 1.0f;
static void report_once(Mixer *m, unsigned bit, const char *msg, int value) {
    if (m->reported & (1u << bit)) return;
    m->reported |= 1u << bit; fprintf(stderr, "[totk-audio] 【红】%s (%d)\n", msg, value);
}

/* ---------- CoreAudio output ---------- */
static OSStatus pull(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)flags; (void)ts; (void)bus;
    Mixer *m = ctx; float *out = io->mBuffers[0].mData;
    uint64_t rd = atomic_load(&m->rd), wr = atomic_load_explicit(&m->wr, memory_order_acquire), have = wr - rd;
    UInt32 n = frames < have ? frames : (UInt32)have;
    for (UInt32 i = 0; i < n; i++) { uint64_t k = (rd + i) % RING_FRAMES; out[2 * i] = m->ring[2 * k]; out[2 * i + 1] = m->ring[2 * k + 1]; }
    if (n < frames) { memset(out + 2 * n, 0, (frames - n) * 2 * sizeof(float)); if (wr) atomic_fetch_add(&m->underruns, 1); }
    atomic_store_explicit(&m->rd, rd + n, memory_order_release);
    return noErr;
}
static void output_open(Mixer *m, int rate) {
    if (getenv("TOTK_AUDIO_OFF")) { fprintf(stderr, "[totk-audio] TOTK_AUDIO_OFF: mixing without a CoreAudio device\n"); return; }
#if TARGET_OS_IPHONE
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0};   /* session set up by the app */
#else
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
#endif
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &m->unit)) { fprintf(stderr, "[totk-audio] 【红】no default output device: mixing without sound\n"); return; }
    AudioStreamBasicDescription f = {(Float64)rate, kAudioFormatLinearPCM, kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked, 8, 1, 8, 2, 32, 0};
    AURenderCallbackStruct cb = {pull, m};
    if (AudioUnitSetProperty(m->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) ||
        AudioUnitSetProperty(m->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) ||
        AudioUnitInitialize(m->unit) || AudioOutputUnitStart(m->unit)) {
        fprintf(stderr, "[totk-audio] 【红】CoreAudio output setup failed: mixing without sound\n"); AudioComponentInstanceDispose(m->unit); return;
    }
    m->unit_ok = 1; fprintf(stderr, "[totk-audio] CoreAudio output %d Hz stereo\n", rate);
}
static void output_push(Mixer *m, const float *lr, int frames) {
    uint64_t wr = atomic_load(&m->wr), rd = atomic_load_explicit(&m->rd, memory_order_acquire);
    if (m->unit_ok && wr + (uint64_t)frames - rd > RING_FRAMES) { atomic_fetch_add(&m->overruns, 1); return; }   /* device stalled: drop this frame */
    for (int i = 0; i < frames; i++) { uint64_t k = (wr + (uint64_t)i) % RING_FRAMES; m->ring[2 * k] = lr[2 * i]; m->ring[2 * k + 1] = lr[2 * i + 1]; }
    atomic_store_explicit(&m->wr, wr + (uint64_t)frames, memory_order_release);
    if (!m->unit_ok) atomic_store(&m->rd, wr + (uint64_t)frames);   /* no device: the ring only keeps the latest frames */
}

void audio_mixer_open(AudioRenderer *r) {
    Mixer *m = calloc(1, sizeof *m);
    if (!m) runtime_fail("audio mixer allocation");
    m->frame = r->param.sample_count; m->nbuf = r->param.mix_buffer_count;
    if (m->frame <= 0 || m->frame > MAX_FRAME || m->nbuf <= 0 || m->nbuf > 512) runtime_fail("audio mixer frame / buffer count");
    m->buf = calloc((size_t)m->nbuf * (size_t)m->frame, sizeof(float));
    m->offset = calloc((size_t)r->param.submix_count + 1, sizeof(int));
    m->nsplit = r->param.splitter_count; m->ndest = r->param.splitter_destination_count;
    m->split = calloc((size_t)(m->nsplit ? m->nsplit : 1), sizeof *m->split); m->dest = calloc((size_t)(m->ndest ? m->ndest : 1), sizeof *m->dest);
    if (!m->buf || !m->offset || !m->split || !m->dest) runtime_fail("audio mixer tables");
    m->effect = calloc((size_t)(r->param.effect_count ? r->param.effect_count : 1), sizeof *m->effect);
    m->effect_order = calloc((size_t)(r->param.effect_count ? r->param.effect_count : 1), sizeof *m->effect_order);
    if (!m->effect || !m->effect_order) runtime_fail("audio effect tables");
    r->mixer = m;
    output_open(m, r->param.sample_rate);
}
void audio_mixer_close(AudioRenderer *r) {
    Mixer *m = r->mixer; if (!m) return;
    if (m->unit_ok) { AudioOutputUnitStop(m->unit); AudioUnitUninitialize(m->unit); AudioComponentInstanceDispose(m->unit); }
    for (int i = 0; i < m->nsplit; i++) free(m->split[i].ids);
    for (int i = 0; i < r->param.effect_count; i++) { audio_reverb_free(m->effect[i].reverb); free(m->effect[i].delay); }
    free(m->effect); free(m->effect_order);
    free(m->buf); free(m->offset); free(m->split); free(m->dest); free(m); r->mixer = NULL;
}

/* ---------- routing input: splitters and mixes ---------- */
static void splitters_update(AudioRenderer *r, const uint8_t *p, uint32_t n) {
    Mixer *m = r->mixer;
    if (!n) return;
    uint32_t hdr[3];
    if (n < 0x20) runtime_fail("audio splitter section header");
    memcpy(hdr, p, 12);
    if (hdr[0] != 0x48444e53u) runtime_fail("audio splitter header magic (SNDH)");
    uint32_t ni = hdr[1], nd = hdr[2], at = 0x20;
    { static int dumped; if (!dumped++ && getenv("TOTK_AUDIO_DUMP")) { FILE *f = fopen("analysis/audio-splitter-section.bin", "wb"); if (f) { fwrite(p, 1, n, f); fclose(f); }
        fprintf(stderr, "[totk-audio] splitter section %u bytes, header infos %u destinations %u\n", n, ni, nd); } }
    for (uint32_t i = 0; i < ni; i++) {
        uint32_t h[4]; if (at + 16 > n) runtime_fail("audio splitter info overruns section");
        memcpy(h, p + at, 16); at += 16;
        if (h[0] != 0x49444e53u || h[1] >= (uint32_t)m->nsplit || h[3] > (uint32_t)m->ndest || at + 4u * h[3] > n) {
            FILE *f = fopen("analysis/audio-splitter-section-bad.bin", "wb"); if (f) { fwrite(p, 1, n, f); fclose(f); }
            fprintf(stderr, "[totk-audio] splitter info %u at %#x: %08x %08x %08x %08x (section %u bytes, infos %u, destinations %u)\n", i, at - 16, h[0], h[1], h[2], h[3], n, ni, nd);
            runtime_fail("audio splitter info (SNDI)");
        }
        SplitInfo *s = &m->split[h[1]]; free(s->ids);
        s->ndest = (int32_t)h[3]; s->ids = calloc(h[3] ? h[3] : 1, sizeof(int32_t));
        if (!s->ids) runtime_fail("audio splitter destinations");
        memcpy(s->ids, p + at, 4u * h[3]); at += 4u * h[3] + 12;   /* 12 reserved bytes follow the ids (measured, TOTK REV11) */
        if (at > n) runtime_fail("audio splitter info padding overruns section");
        for (uint32_t k = 0; k < h[3]; k++) if (s->ids[k] < 0 || s->ids[k] >= m->ndest) runtime_fail("audio splitter destination id");
    }
    for (uint32_t i = 0; i < nd; i++) {
        unsigned rev=(r->revision>>24)-'0',stride=rev>=15?0xa8:rev>=12?0x90:0x70;
        uint8_t e[0xa8]={0}; if (at>n||stride>n-at) runtime_fail("audio splitter destination overruns section");
        memcpy(e, p + at, stride); at += stride;
        uint32_t magic, id; int32_t dst; memcpy(&magic, e, 4); memcpy(&id, e + 4, 4); memcpy(&dst, e + 0x68, 4);
        if (magic != 0x44444e53u || id >= (uint32_t)m->ndest) runtime_fail("audio splitter destination (SNDD)");
        SplitDest *d = &m->dest[id]; memcpy(d->vol, e + 8, sizeof d->vol); d->dest = dst;
        d->used=e[rev>=15?0x9c:rev>=12?0x84:0x6c];
        for(unsigned f=0;f<2;f++){
            AudrenBiquadFloat b={0};
            if(rev>=15)memcpy(&b,e+0x6c+f*sizeof b,sizeof b);
            else if(rev>=12){AudrenBiquad q;memcpy(&q,e+0x6c+f*sizeof q,sizeof q);b.enable=q.enable;for(unsigned c=0;c<3;c++)b.numerator[c]=q.numerator[c]/16384.0f;for(unsigned c=0;c<2;c++)b.denominator[c]=q.denominator[c]/16384.0f;}
            if(b.enable){for(unsigned c=0;c<3;c++)if(!isfinite(b.numerator[c]))runtime_fail("audio splitter nonfinite numerator");for(unsigned c=0;c<2;c++)if(!isfinite(b.denominator[c]))runtime_fail("audio splitter nonfinite denominator");}
            if(!b.enable||!d->biquads[f].enable||!d->used)memset(d->state[f],0,sizeof d->state[f]);
            d->biquads[f]=b;
        }
    }
    if (at <= n && n - at < 16) at = n;   /* the section is padded to 16 bytes; the padding is not cleared by the game (measured) */
    if (at != n) { fprintf(stderr, "[totk-audio] splitter section %u bytes, parsed %u (infos %u, destinations %u)\n", n, at, ni, nd); runtime_fail("audio splitter section size"); }
}
void audio_mixer_update_routing(AudioRenderer *r, const uint8_t *splitters, uint32_t splitter_bytes, const uint8_t *mixes, uint32_t mix_bytes, int mix_count) {
    splitters_update(r, splitters, splitter_bytes);
    if (mix_bytes == (uint32_t)mix_count * sizeof(AudrenMixIn)) { memcpy(r->mixes, mixes, mix_bytes); return; }
    /* MixInParameterDirtyOnly: 0x20-byte header {u32 magic 0xCAFEBABE, u32 count, pad} followed by the dirty mixes */
    if (mix_bytes >= 0x20 && (mix_bytes - 0x20) % sizeof(AudrenMixIn) == 0) {
        uint32_t magic, n; memcpy(&magic, mixes, 4); memcpy(&n, mixes + 4, 4);
        if (magic != 0xcafebabeu || n * sizeof(AudrenMixIn) != mix_bytes - 0x20) runtime_fail("audio dirty mix header");
        for (uint32_t i = 0; i < n; i++) {
            AudrenMixIn m; memcpy(&m, mixes + 0x20 + i * sizeof m, sizeof m);
            if (m.mix_id < 0 || m.mix_id >= mix_count) runtime_fail("audio mix id");
            r->mixes[m.mix_id] = m;
        }
        return;
    }
    fprintf(stderr, "[totk-audio] mix section %u bytes for %d mixes\n", mix_bytes, mix_count);
    runtime_fail("audio mix section size");
}

/* Effect parameters arrive once per IPC update, while the DSP may process multiple frames. */
void audio_mixer_update_effects(AudioRenderer *r) {
    Mixer *m = r->mixer;
    for (int i=0;i<r->param.effect_count;i++) {
        const AudrenEffectIn *e=&r->effects[i]; EffectState *s=&m->effect[i];
        if(e->type!=5 || e->is_new) { audio_reverb_free(s->reverb); s->reverb=NULL; s->pending=1; }
        if(e->type==7) {
            AudrenLimiterParam p; memcpy(&p,e->specific,sizeof p);
            if(e->is_new || !s->delay || p.status==0 || p.channels!=s->channels || p.delay_max!=s->delay_max) {
                free(s->delay);s->delay=NULL;s->delay_max=p.delay_max;s->channels=p.channels;
                memset(s->detector,0,sizeof s->detector);memset(s->position,0,sizeof s->position);
                for(int c=0;c<6;c++)s->compression[c]=s->minimum[c]=1;
                memset(s->peak,0,sizeof s->peak);
                if(e->enabled) {
                    if(p.channels<1 || p.channels>6 || p.delay_max<1 || p.delay_max>192000)runtime_fail("limiter delay dimensions");
                    s->delay=calloc((size_t)p.channels*p.delay_max,sizeof(float));if(!s->delay)runtime_fail("limiter delay allocation");
                }
            }
            if(p.statistics_reset) { memset(s->peak,0,sizeof s->peak); for(int c=0;c<6;c++)s->minimum[c]=1; }
        } else if(s->delay) { free(s->delay);s->delay=NULL; }
        if(e->type!=5) continue;
        Reverb3dParam p; memcpy(&p,e->specific,sizeof p);
        if(s->reverb && p.sample_rate!=s->param.sample_rate) { audio_reverb_free(s->reverb); s->reverb=NULL; }
        if(memcmp(&p,&s->param,72) || p.status<2) s->pending=1;
        s->param=p;
    }
}

/* ---------- voice decoding along the voice clock's sample path ---------- */
typedef struct { ServerVoice *v; unsigned index; uint64_t offset; int32_t loops; uint8_t valid[4]; int ended; int16_t h1, h2; int32_t abuf; uint64_t aat; } Walk;
static int fmt_bytes(int f) { return f == FMT_PCM8 ? 1 : f == FMT_PCM16 ? 2 : f == FMT_PCM32 || f == FMT_PCMF32 ? 4 : 0; }
/* DSP-ADPCM: 8-byte frames of a predictor/scale byte and 14 4-bit samples; the history runs across samples. */
static int16_t adpcm_next(Walk *w, const AudrenWaveBuffer *b, const int16_t *coef, uint64_t i) {
    const uint8_t *d = (const uint8_t *)(uintptr_t)b->address;
    uint64_t frame = i / 14, nib = i % 14;
    if ((frame + 1) * 8 > b->size) return 0;
    uint8_t ps = d[frame * 8]; int scale = 1 << (ps & 15), pred = (ps >> 4) & 7;
    int n = (d[frame * 8 + 1 + nib / 2] >> ((nib & 1) ? 0 : 4)) & 15; if (n >= 8) n -= 16;
    int32_t s = ((n * scale) << 11) + 1024 + coef[pred * 2] * w->h1 + coef[pred * 2 + 1] * w->h2;
    s >>= 11; if (s > 32767) s = 32767; if (s < -32768) s = -32768;
    w->h2 = w->h1; w->h1 = (int16_t)s; return (int16_t)s;
}
static float sample_at(Walk *w, int ch) {
    if (w->ended || !w->valid[w->index]) return 0;
    ServerVoice *v = w->v; const AudrenWaveBuffer *b = &v->buffers[w->index];
    uint64_t i = (uint64_t)b->start + w->offset; int f = v->in.sample_format, cc = (int)v->in.channel_count;
    if (f == FMT_ADPCM) {
        if (cc != 1 || !v->in.extra_address || v->in.extra_size < 32) return 0;
        const int16_t *coef = (const int16_t *)(uintptr_t)v->in.extra_address;
        if (w->abuf != (int32_t)w->index || w->aat > i + 1) {   /* (re)start this buffer: from its context, else from the frame boundary with carried history */
            w->abuf = (int32_t)w->index;
            if (b->context_address && b->context_size >= 6) { const int16_t *c = (const int16_t *)(uintptr_t)b->context_address; w->h1 = c[1]; w->h2 = c[2]; w->aat = (uint64_t)b->start; }
            else w->aat = (uint64_t)b->start - (uint64_t)b->start % 14;
            if (i < w->aat) w->aat = i;
        }
        while (w->aat <= i) adpcm_next(w, b, coef, w->aat++);
        return w->h1 / 32768.0f;
    }
    int bytes = fmt_bytes(f); if (!bytes || ch >= cc) return 0;
    uint64_t at = (i * (uint64_t)cc + (uint64_t)ch) * (uint64_t)bytes;
    if (at + (uint64_t)bytes > b->size) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)b->address + at;
    switch (f) {
    case FMT_PCM8: return (int8_t)p[0] / 128.0f;
    case FMT_PCM16: { int16_t s; memcpy(&s, p, 2); return s / 32768.0f; }
    case FMT_PCM32: { int32_t s; memcpy(&s, p, 4); return (float)(s / 2147483648.0); }
    default: { float s; memcpy(&s, p, 4); return s; }
    }
}
static void walk_skip_empty(Walk *w) {   /* the same as skip_empty in audio_renderer.c: no samples, played on arrival */
    while (!w->ended && w->valid[w->index] && audio_buffer_empty(&w->v->buffers[w->index])) {
        const AudrenWaveBuffer *b = &w->v->buffers[w->index];
        if (b->loop && b->loop_count < 0) break;
        w->valid[w->index] = 0; w->offset = 0; w->loops = 0; w->index = (w->index + 1) & 3;
        if (b->end_of_stream) w->ended = 1;
    }
}
static void walk_step(Walk *w) {   /* the same transitions as advance_voice in audio_renderer.c */
    if (w->ended || !w->valid[w->index]) { w->ended = 1; return; }
    const AudrenWaveBuffer *b = &w->v->buffers[w->index];
    uint64_t length = (uint64_t)(b->end - b->start);
    if (++w->offset < length) return;
    if (b->loop && (b->loop_count < 0 || w->loops < b->loop_count)) { w->loops++; w->offset = b->loop_start > b->start && b->loop_start < b->end ? (uint64_t)(b->loop_start - b->start) : 0; return; }
    w->valid[w->index] = 0; w->offset = 0; w->loops = 0; w->index = (w->index + 1) & 3;
    if (b->end_of_stream) w->ended = 1;
    walk_skip_empty(w);
}
/* Decodes `frames` output samples per channel into out[c][k]; the renderer advances the voice afterwards by the same amount. */
static void voice_render(AudioRenderer *r, ServerVoice *v, float out[6][MAX_FRAME], int frames) {
    const double step = v->in.sample_rate * (v->in.pitch > 0 ? v->in.pitch : 1.0) / r->param.sample_rate;
    Walk w = {v, v->index, v->offset, v->loops_done, {0}, 0, v->adpcm.history[0], v->adpcm.history[1], v->adpcm.buffer, v->adpcm.at};
    memcpy(w.valid, v->valid, sizeof w.valid);
    walk_skip_empty(&w);
    int cc = (int)v->in.channel_count; double frac = v->fraction;
    for (int k = 0; k < frames; k++) {
        Walk next = w; walk_step(&next);
        for (int c = 0; c < cc; c++) { float s0 = sample_at(&w, c), s1 = sample_at(&next, c); out[c][k] = (float)(s0 + (s1 - s0) * frac); }
        frac += step;
        while (frac >= 1.0) { frac -= 1.0; walk_step(&w); }
    }
    v->adpcm.history[0] = w.h1; v->adpcm.history[1] = w.h2; v->adpcm.buffer = w.abuf; v->adpcm.at = w.aat;
}

/* ---------- mixing ---------- */
static float *mixbuf(Mixer *m, int i) { return m->buf + (size_t)i * (size_t)m->frame; }
static void accumulate(Mixer *m, int buffer, const float *src, float vol, int frames) {
    if (vol == 0 || buffer < 0 || buffer >= m->nbuf) return;
    float *d = mixbuf(m, buffer); for (int k = 0; k < frames; k++) d[k] += src[k] * vol;
}
static int mix_valid(AudioRenderer *r, int32_t id) { return id >= 0 && id <= r->param.submix_count && r->mixes[id].in_use; }
static int mix_depth(AudioRenderer *r, int id) {
    int d = 0;
    for (int guard = 0; id > 0 && guard < 64; guard++, d++) {
        if (!mix_valid(r, r->mixes[id].dest_mix_id)) return d + 1;
        id = r->mixes[id].dest_mix_id;
    }
    return d;
}

/* Guest custom effects exchange int32 samples in two rings. Each ring has CPU and DSP
   0x40-byte headers followed by samples; only the DSP header's owned cursor is advanced. */
static void mix_aux(AudioRenderer *r, int mix, const AudrenEffectIn *e) {
    Mixer *m=r->mixer; AudrenAuxParam p; memcpy(&p,e->specific,sizeof p);
    if(!p.send_info || !p.return_info || !p.capacity || !p.channels) return;
    if(p.channels>24 || p.capacity>16*1024*1024)runtime_fail("auxiliary effect dimensions");
    uint8_t *send=ipc_guest(p.send_info,128+(uint64_t)p.capacity*4), *ret=ipc_guest(p.return_info,128+(uint64_t)p.capacity*4);
    uint32_t *sh=(uint32_t *)(send+64),*rh=(uint32_t *)(ret+64); int32_t *sd=(int32_t *)(send+128),*rd=(int32_t *)(ret+128);
    uint32_t wr=sh[1],read=rh[0],total=p.channels*(uint32_t)m->frame;
    for(unsigned c=0;c<p.channels;c++) {
        if(p.input[c]>=r->mixes[mix].buffer_count || p.output[c]>=r->mixes[mix].buffer_count)runtime_fail("auxiliary effect mix buffer");
        float *input=mixbuf(m,m->offset[mix]+p.input[c]),*output=mixbuf(m,m->offset[mix]+p.output[c]);
        if(!e->enabled) { if(input!=output)memmove(output,input,m->frame*sizeof(float)); continue; }
        uint64_t w=(uint64_t)wr+c*m->frame,at=(uint64_t)read+c*m->frame;
        for(int k=0;k<m->frame;k++) {
            double scaled=trunc((double)input[k]*32768); int32_t value=scaled>INT32_MAX?INT32_MAX:scaled<INT32_MIN?INT32_MIN:!isfinite(scaled)?0:(int32_t)scaled;
            input[k]=value/32768.f;
            if(w<=p.capacity)sd[(w+(unsigned)k)%p.capacity]=value;
        }
        for(int k=0;k<m->frame;k++)output[k]=at<=p.capacity?rd[(at+(unsigned)k)%p.capacity]/32768.f:0;
        if(c==p.channels-1) { if(w<=p.capacity)sh[1]=(wr+total)%p.capacity; if(at<=p.capacity)rh[0]=(read+total)%p.capacity; }
    }
    if(!e->enabled) { sh[0]=sh[1]=sh[3]=0; rh[0]=rh[1]=rh[3]=0; }
}

void audio_mixer_effect_result(AudioRenderer *r, int id, void *output, unsigned size) {
    Mixer *m=r->mixer;
    if(size>=48 && r->effects[id].type==7) { memcpy(output,m->effect[id].peak,24);memcpy((uint8_t *)output+24,m->effect[id].minimum,24); }
}
static void mix_limiter(AudioRenderer *r, int mix, int id) {
    Mixer *m=r->mixer; EffectState *s=&m->effect[id]; const AudrenEffectIn *e=&r->effects[id];
    AudrenLimiterParam p;memcpy(&p,e->specific,sizeof p);
    if(p.channels<1 || p.channels>6) { if(e->enabled)runtime_fail("limiter channel count");return; }
    if(e->enabled && (!s->delay || p.delay_min<1 || p.delay_min>p.delay_max || p.attack<0 || p.attack>1 || p.release<0 || p.release>1))runtime_fail("limiter parameters");
    for(int c=0;c<p.channels;c++) {
        if(p.input[c]>=r->mixes[mix].buffer_count || p.output[c]>=r->mixes[mix].buffer_count)runtime_fail("limiter mix buffer");
        float *input=mixbuf(m,m->offset[mix]+p.input[c]),*output=mixbuf(m,m->offset[mix]+p.output[c]);
        if(!e->enabled) { if(input!=output)memmove(output,input,m->frame*sizeof(float));continue; }
        for(int k=0;k<m->frame;k++) {
            float x=(input[k]*(32768.f/32767.f))*p.input_gain,level=fabsf(x);
            float coeff=level>s->detector[c]?p.attack:p.release;s->detector[c]+=coeff*(level-s->detector[c]);
            float attenuation=s->detector[c]>p.threshold?p.threshold/s->detector[c]:1;
            coeff=s->compression[c]>attenuation?p.attack:p.release;s->compression[c]+=coeff*(attenuation-s->compression[c]);
            int slot=c*p.delay_max+s->position[c];output[k]=s->delay[slot]*s->compression[c]*p.output_gain*(32767.f/32768.f);
            s->delay[slot]=x;s->position[c]=(s->position[c]+1)%p.delay_min;
            if(p.statistics_enabled) { s->peak[c]=fmaxf(s->peak[c],level);s->minimum[c]=fminf(s->minimum[c],s->compression[c]); }
        }
    }
}

static void mix_effects(AudioRenderer *r, int mix) {
    Mixer *m=r->mixer; int count=0;
    for(int i=0;i<r->param.effect_count;i++) if(r->effects[i].type && r->effects[i].mix_id==mix) {
        int j=count++; while(j>0 && r->effects[m->effect_order[j-1]].order>r->effects[i].order) { m->effect_order[j]=m->effect_order[j-1]; j--; } m->effect_order[j]=i;
    }
    for(int n=0;n<count;n++) {
        int id=m->effect_order[n]; const AudrenEffectIn *e=&r->effects[id]; EffectState *s=&m->effect[id];
        if(e->type==2) { mix_aux(r,mix,e); continue; }
        if(e->type==7) { mix_limiter(r,mix,id); continue; }
        if(e->type!=5) {
            if(e->enabled && e->type<32 && !(m->effect_reported&(1u<<e->type))) { m->effect_reported|=1u<<e->type; fprintf(stderr,"[totk-audio] 【红】effect type %u not implemented\n",e->type); }
            continue;
        }
        Reverb3dParam *p=&s->param; float *input[6]={0},*output[6]={0};
        if(p->channels!=1 && p->channels!=2 && p->channels!=4 && p->channels!=6) { if(e->enabled)runtime_fail("audio reverb channel count"); continue; }
        for(int c=0;c<p->channels;c++) {
            int idx=c; /* REV11+ channel resources use L,R,C,LFE,BL,BR; the reference DSP uses legacy order. */
            if(p->channels==6 && (r->revision>>24)>='0'+11) { static const int map[6]={0,1,4,5,2,3}; idx=map[c]; }
            if(p->input[idx]>=r->mixes[mix].buffer_count || p->output[idx]>=r->mixes[mix].buffer_count)runtime_fail("audio reverb buffer index");
            input[c]=mixbuf(m,m->offset[mix]+p->input[idx]); output[c]=mixbuf(m,m->offset[mix]+p->output[idx]);
        }
        if(!e->enabled) { for(int c=0;c<p->channels;c++)if(input[c]!=output[c])memmove(output[c],input[c],m->frame*sizeof(float)); continue; }
        if(!s->reverb)s->reverb=audio_reverb_create(p); else if(s->pending)audio_reverb_update(s->reverb,p);
        s->pending=0; audio_reverb_process(s->reverb,input,output,m->frame);
    }
}

static const float *splitter_filter(SplitDest *d,const float *input,float *output,int frames){
    if(!d->biquads[0].enable&&!d->biquads[1].enable)return input;
    memcpy(output,input,(size_t)frames*sizeof *output);
    for(unsigned f=0;f<2;f++)if(d->biquads[f].enable){
        const AudrenBiquadFloat *b=&d->biquads[f];float *z=d->state[f];
        for(int k=0;k<frames;k++){
            float x=output[k],y=b->numerator[0]*x+z[0];
            z[0]=b->numerator[1]*x+b->denominator[0]*y+z[1];
            z[1]=b->numerator[2]*x+b->denominator[1]*y;output[k]=y;
        }
    }
    return output;
}
void audio_mixer_render(AudioRenderer *r, double seconds) {
    (void)seconds;
    Mixer *m = r->mixer; if (!m) return;
    const int frames = m->frame, mc = r->param.submix_count + 1;
    memset(m->buf, 0, (size_t)m->nbuf * (size_t)frames * sizeof(float));
    int at = 0;
    for (int i = 0; i < mc; i++) { m->offset[i] = at; if (r->mixes[i].in_use) at += r->mixes[i].buffer_count; }
    if (at > m->nbuf) { fprintf(stderr, "[totk-audio] mixes use %d buffers, renderer has %d\n", at, m->nbuf); runtime_fail("audio mix buffer layout"); }

    static float vs[6][MAX_FRAME],filtered[MAX_FRAME];
    for (int i = 0; i < r->param.voice_count; i++) {
        ServerVoice *v = &r->voices[i];
        if (!v->in.in_use || v->state != SV_STARTED || !v->in.sample_rate || !v->valid[v->index]) continue;
        int cc = (int)v->in.channel_count; if (cc < 1 || cc > 6) { report_once(m, 2, "voice channel count unsupported", cc); continue; }
        voice_render(r, v, vs, frames);
        for (int f = 0; f < 2; f++) if (v->in.biquads[f].enable) {
            const AudrenBiquad *b = &v->in.biquads[f];
            const AudrenBiquadFloat *bf=&v->float_biquads[f];
            const float a0 = v->use_float_biquads?bf->numerator[0]:b->numerator[0]/16384.0f;
            const float a1 = v->use_float_biquads?bf->numerator[1]:b->numerator[1]/16384.0f;
            const float a2 = v->use_float_biquads?bf->numerator[2]:b->numerator[2]/16384.0f;
            const float b1 = v->use_float_biquads?bf->denominator[0]:b->denominator[0]/16384.0f;
            const float b2 = v->use_float_biquads?bf->denominator[1]:b->denominator[1]/16384.0f;
            for (int c = 0; c < cc; c++) {
                float *z = v->biquad_state[f][c];
                for (int k = 0; k < frames; k++) {
                    float x = vs[c][k], y = a0 * x + z[0];
                    z[0] = a1 * x + b1 * y + z[1];
                    z[1] = a2 * x + b2 * y;
                    vs[c][k] = y;
                }
            }
        }
        if (v->in.splitter_id != SPLITTER_UNUSED && v->in.splitter_id < (uint32_t)m->nsplit) {
            const SplitInfo *s = &m->split[v->in.splitter_id];
            for (int c = 0; c < cc; c++) for (int k = c; k < s->ndest; k += cc) {
                SplitDest *d = &m->dest[s->ids[k]];
                if (!d->used || !mix_valid(r, d->dest)) continue;
                const float *source=splitter_filter(d,vs[c],filtered,frames);
                for (int b = 0; b < r->mixes[d->dest].buffer_count && b < 24; b++) accumulate(m, m->offset[d->dest] + b, source, d->vol[b] * v->in.volume, frames);
            }
        } else if (mix_valid(r, v->in.mix_id)) {
            for (int c = 0; c < cc; c++) {
                int32_t id = v->in.channel_ids[c]; if (id < 0 || id >= r->param.voice_count * 6) continue;
                const AudrenChannelIn *ch = &r->channels[id];
                for (int b = 0; b < r->mixes[v->in.mix_id].buffer_count && b < 24; b++) accumulate(m, m->offset[v->in.mix_id] + b, vs[c], ch->mix[b] * v->in.volume, frames);
            }
        }
    }
    /* submixes: deepest first, so every mix is complete before it is folded into its destination */
    int order[256], n = 0, depth[256];
    for (int i = 1; i < mc && n < 256; i++) if (r->mixes[i].in_use) { depth[n] = mix_depth(r, i); order[n++] = i; }
    for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) if (depth[b] > depth[a]) { int t = order[a]; order[a] = order[b]; order[b] = t; t = depth[a]; depth[a] = depth[b]; depth[b] = t; }
    for (int a = 0; a < n; a++) {
        mix_effects(r, order[a]);
        const AudrenMixIn *x = &r->mixes[order[a]]; int src = m->offset[order[a]];
        if (x->dest_splitter_id != SPLITTER_UNUSED && x->dest_splitter_id < (uint32_t)m->nsplit) {
            const SplitInfo *s = &m->split[x->dest_splitter_id];
            for (int c = 0; c < x->buffer_count && c < 24; c++) for (int k = c; k < s->ndest; k += x->buffer_count) {
                SplitDest *d = &m->dest[s->ids[k]];
                if (!d->used || !mix_valid(r, d->dest)) continue;
                const float *source=splitter_filter(d,mixbuf(m,src+c),filtered,frames);
                for (int b = 0; b < r->mixes[d->dest].buffer_count && b < 24; b++) accumulate(m, m->offset[d->dest] + b, source, d->vol[b] * x->volume, frames);
            }
        } else if (mix_valid(r, x->dest_mix_id)) {
            for (int c = 0; c < x->buffer_count && c < 24; c++)
                for (int b = 0; b < r->mixes[x->dest_mix_id].buffer_count && b < 24; b++) accumulate(m, m->offset[x->dest_mix_id] + b, mixbuf(m, src + c), x->mix[c][b] * x->volume, frames);
        }
    }
    if (r->mixes[0].in_use) mix_effects(r, 0);
    /* device sink */
    const AudrenSinkIn *sink = NULL;
    for (int i = 0; i < r->param.sink_count; i++) if (r->sinks[i].in_use) {
        if (r->sinks[i].type == SINK_DEVICE && !sink) sink = &r->sinks[i];
        else if (r->sinks[i].type == SINK_CIRCULAR) report_once(m, 4, "circular-buffer sink not implemented (sink)", i);
    }
    static float lr[MAX_FRAME * 2];
    memset(lr, 0, sizeof(float) * 2 * (size_t)frames);
    { static int sink_logged; if (sink && !sink_logged) { sink_logged = 1;
        fprintf(stderr, "[totk-audio] device sink: %u input(s), downmix %d, output volume %.2f\n",
                sink->input_count, sink->downmix_enabled, device_volume); } }
    if (sink && sink->input_count >= 1 && sink->input_count <= 6) {
        const float gain = device_volume * (r->mixes[0].in_use ? r->mixes[0].volume : 1.0f);
        const float *ch[6] = {0};
        for (uint32_t c = 0; c < sink->input_count; c++) if (sink->inputs[c] >= 0 && sink->inputs[c] < m->nbuf) ch[c] = mixbuf(m, sink->inputs[c]);
        for (int k = 0; k < frames; k++) {
            float l, rr;
            if (sink->input_count >= 6) {   /* FL FR C LFE RL RR -> stereo */
                float ff = sink->downmix_enabled ? sink->downmix[0] : 1.0f, cf = sink->downmix_enabled ? sink->downmix[1] : 0.7071f, sf = sink->downmix_enabled ? sink->downmix[2] : 0.7071f;
                float C = ch[2] ? ch[2][k] : 0;
                l = ff * (ch[0] ? ch[0][k] : 0) + cf * C + sf * (ch[4] ? ch[4][k] : 0);
                rr = ff * (ch[1] ? ch[1][k] : 0) + cf * C + sf * (ch[5] ? ch[5][k] : 0);
            } else if (sink->input_count >= 2) { l = ch[0] ? ch[0][k] : 0; rr = ch[1] ? ch[1][k] : 0; }
            else l = rr = ch[0] ? ch[0][k] : 0;
            l *= gain; rr *= gain;
            lr[2 * k] = l > 1 ? 1 : l < -1 ? -1 : l; lr[2 * k + 1] = rr > 1 ? 1 : rr < -1 ? -1 : rr;
        }
    } else if (!sink) {
        /* 游戏在第一次 RequestUpdate 之后才配 device sink，启动头几帧没有 sink 是正常的；持续 1 s 没有才报红。
           The game configures its device sink after the first RequestUpdate, so no sink in the first few frames is
           normal; only report if it is still missing after one second. */
        static unsigned miss; if (++miss == 200) report_once(m, 5, "no device sink after 1 s (sink_count)", r->param.sink_count);
    }
    { static double sum, peak; static uint64_t count, frames_total;   /* level report every ~5 s: evidence that sound is produced */
      for (int k = 0; k < 2 * frames; k++) { double x = lr[k]; sum += x * x; if (fabs(x) > peak) peak = fabs(x); }
      count += 2 * (uint64_t)frames; frames_total += (uint64_t)frames;
      if (frames_total >= (uint64_t)r->param.sample_rate * 5) {
          fprintf(stderr, "[totk-audio] output level: rms %.4f peak %.4f over %.1f s, underruns %llu, overruns %llu\n", sqrt(sum / (double)count), peak,
                  frames_total / (double)r->param.sample_rate, (unsigned long long)atomic_load(&m->underruns), (unsigned long long)atomic_load(&m->overruns));
          sum = peak = 0; count = frames_total = 0;
      } }
    output_push(m, lr, frames);
}

/* Probe hook: the latest mixed stereo frames. */
int audio_mixer_read(AudioRenderer *r, float *lr, int frames) {
    Mixer *m = r->mixer; uint64_t wr = atomic_load(&m->wr); if ((uint64_t)frames > wr || frames > RING_FRAMES) return 0;
    for (int i = 0; i < frames; i++) { uint64_t k = (wr - (uint64_t)frames + (uint64_t)i) % RING_FRAMES; lr[2 * i] = m->ring[2 * k]; lr[2 * i + 1] = m->ring[2 * k + 1]; }
    return frames;
}

/* ---------- IAudioDevice: one stereo output device ---------- */
static const char device_name[] = "AudioTvOutput";
static void put_name(const IpcBuffer *b, IpcResponse *rs, int pointer) {
    if (!b->size) return;
    char *dst = ipc_guest(b->addr, b->size); size_t n = b->size < sizeof device_name ? b->size : sizeof device_name;
    memset(dst, 0, b->size); memcpy(dst, device_name, n);
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, b->size, b->index};
}
static IpcBuffer out_buffer(const IpcRequest *rq, int *pointer) {
    *pointer = !(rq->nb && rq->b[0].size) && rq->nc && rq->c[0].size;
    return *pointer ? rq->c[0] : rq->nb ? rq->b[0] : (IpcBuffer){0};
}
void audio_device_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    int pointer;
    switch (rq->cmd) {
    case 0: case 6: { IpcBuffer b = out_buffer(rq, &pointer); int32_t n = b.size >= 0x100 ? 1 : 0; if (n) put_name(&b, rs, pointer); IPC_OUT(rs, n); return; }   /* ListAudioDeviceName(Auto) */
    case 1: case 7: { float v = IPC_ARG(rq, float, 0); if (v != device_volume) fprintf(stderr, "[totk-audio] output volume: %.2f\n", v); device_volume = v; return; }   /* SetAudioDeviceOutputVolume(Auto) */
    case 2: case 8: IPC_OUT(rs, device_volume); return;                                              /* GetAudioDeviceOutputVolume(Auto) */
    case 3: case 10: { IpcBuffer b = out_buffer(rq, &pointer); put_name(&b, rs, pointer); return; }  /* GetActiveAudioDeviceName(Auto) */
    case 5: { int32_t n = 2; IPC_OUT(rs, n); return; }                                               /* GetActiveChannelCount */
    case 13: { IpcBuffer b = out_buffer(rq, &pointer); put_name(&b, rs, pointer); return; }          /* GetActiveAudioOutputDeviceName [13.0.0+] */
    case 14: { IpcBuffer b = out_buffer(rq, &pointer); int32_t n = b.size >= 0x100 ? 1 : 0; if (n) put_name(&b, rs, pointer); IPC_OUT(rs, n); return; }   /* ListAudioOutputDeviceName [13.0.0+] */
    case 4: case 11: case 12: { static uint32_t ev; if (!ev) ev = runtime_event_new(); rs->copy[rs->ncopy++] = runtime_event_readable(ev); return; }   /* device change events: never signalled, the device does not change */
    default: ipc_unimplemented(s, rq);
    }
}
