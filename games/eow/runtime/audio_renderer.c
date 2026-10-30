/* audren:u — host audio renderer service. The SDK's nn::audio client builds RequestUpdate input
   unchanged; this service validates every section against the header, keeps server-side voice state,
   advances it in real time (one 5 ms frame per render tick, signalling the frame event) and writes the
   output status the client expects. Sample mixing to CoreAudio is done by audio_mixer.c. */
#include "runtime.h"
#include "ipc.h"
#include "audio_renderer.h"
#include "audio_mixer.h"
#include "audio_reverb.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mach/mach.h>
#include <pthread/qos.h>

#define REV(n) (0x30564552u + ((uint32_t)(n) << 24))   /* "REVn" */
#define WORK_BUFFER_BYTES 0x10000u                      /* host keeps its state in host memory */
#define BEHAVIOR_OUT 0xb0u
#define RENDER_INFO_OUT 0x10u

static int revision_number(uint32_t rev) { return (int)(rev >> 24) - '0'; }
static void need(int c, const char *m) { if (!c) { fprintf(stderr, "[totk-audio] %s\n", m); runtime_fail("audio renderer protocol"); } }

static AudioRenderer *renderer_new(const AudrenParameter *p) {
    AudioRenderer *r = calloc(1, sizeof *r);
    need(r != NULL, "renderer allocation");
    r->param = *p;
    r->revision=p->revision;
    need(p->sample_rate == 48000 || p->sample_rate == 32000, "unsupported output rate");
    need(p->voice_count > 0 && p->voice_count <= 1024 && p->sink_count > 0 && p->sink_count <= 256 && p->effect_count >= 0 && p->effect_count <= 4096 && p->submix_count >= 0 && p->submix_count < 256, "renderer counts");
    r->voices = calloc((size_t)p->voice_count, sizeof *r->voices);
    r->pools = calloc((size_t)p->effect_count + 4u * (size_t)p->voice_count, sizeof *r->pools);
    r->mixes = calloc((size_t)p->submix_count + 1, sizeof *r->mixes);
    r->sinks = calloc((size_t)p->sink_count, sizeof *r->sinks);
    r->effects = calloc((size_t)(p->effect_count ? p->effect_count : 1), sizeof *r->effects);
    r->channels = calloc((size_t)p->voice_count * 6, sizeof *r->channels);
    need(r->voices && r->pools && r->mixes && r->sinks && r->effects && r->channels, "renderer tables");
    pthread_mutex_init(&r->lock, NULL);
    r->event = runtime_event_new();
    r->state = 1;   /* stopped */
    return r;
}

/* ---------- RequestUpdate ---------- */
typedef struct { const uint8_t *p, *end; } Reader;
static const uint8_t *take(Reader *in, uint32_t n, const char *what) {
    if ((size_t)(in->end - in->p) < n) { fprintf(stderr, "[totk-audio] input section %s (%u bytes) overruns request\n", what, n); runtime_fail("audio renderer protocol"); }
    const uint8_t *p = in->p; in->p += n; return p;
}
static void voice_reset(ServerVoice *v) {
    memset(v->valid, 0, sizeof v->valid); memset(v->pending, 0, sizeof v->pending);
    v->state = SV_STOPPED; v->flush = 0; v->flush_from = 0;
    v->index = 0; v->offset = 0; v->fraction = 0; v->played = 0; v->consumed = 0; v->loops_done = 0;
    memset(&v->adpcm, 0, sizeof v->adpcm); v->adpcm.buffer = -1;
    memset(v->biquad_state, 0, sizeof v->biquad_state);
    v->ramp_valid = 0;   /* the next start mixes at its own volume; a decaying tail (v->tail) is left to finish */
}
static void update_voice(AudioRenderer *r, const AudrenVoiceIn *in) {
    need(in->id < (uint32_t)r->param.voice_count, "voice id out of range");
    ServerVoice *v = &r->voices[in->id];
    if (in->is_new) memset(v->tail, 0, sizeof v->tail);   /* a re-acquired voice: the old sound's depop has long decayed (~7 ms) */
    if (in->is_new || !in->in_use) voice_reset(v);
    for (int i = 0; i < 2; i++) if (!in->biquads[i].enable)
        memset(v->biquad_state[i], 0, sizeof v->biquad_state[i]);
    v->in = *in;
    v->use_float_biquads=0;
    if (!in->in_use) return;
    need(in->wave_buffer_index < 4, "wave buffer index");
    /* The server's play state the way the DSP keeps it (Ryujinx VoiceInfo.UpdatePlayState): Stop on a voice that is not
       stopped becomes Stopping, which the next render frame turns into Stopped after giving back every queued buffer. */
    if (in->play_state == PLAY_STARTED) v->state = SV_STARTED;
    else if (in->play_state == PLAY_PAUSED) v->state = SV_PAUSED;
    else if (v->state != SV_STOPPED) v->state = SV_STOPPING;
    for (unsigned i = 0; i < 4; i++) {   /* a new buffer waits until a render frame finds the voice playing */
        const AudrenWaveBuffer *w = &in->wave_buffers[i];
        if (!w->sent) { v->buffers[i] = *w; v->pending[i] = 1; }
    }
    v->flush += in->flush_wave_buffer_count; v->flush_from = in->wave_buffer_index;
    if (in->clear_voice_drop) v->drops = 0;
}

/* No samples (no memory, no size or an empty range): the DSP decodes nothing and counts the buffer as played when it
   reaches it, so a stream's empty last buffer does not hold up the buffers queued behind it. */
int audio_buffer_empty(const AudrenWaveBuffer *b) { return !b->address || !b->size || b->end <= b->start; }
static void skip_empty(AudioRenderer *r, ServerVoice *v) {
    while (v->valid[v->index] && audio_buffer_empty(&v->buffers[v->index])) {
        const AudrenWaveBuffer *b = &v->buffers[v->index];
        if (b->loop && b->loop_count < 0) break;   /* an endless empty loop starves the voice, as on the DSP */
        v->valid[v->index] = 0; v->consumed++; v->offset = 0; v->loops_done = 0; v->index = (v->index + 1) & 3;
        if (r) r->empty_played++;
        if (b->end_of_stream) v->played = 0;
    }
}
static int stop_release(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("TOTK_AUDIO_STOP_RELEASE"); on = !(e && !strcmp(e, "0"));
        if (!on) fprintf(stderr, "[totk-audio] TOTK_AUDIO_STOP_RELEASE=0: a stopped voice keeps its queued wave buffers (the old behaviour, for comparison)\n");
    }
    return on;
}
/* Each render frame, before mixing (Ryujinx VoiceInfo.UpdateParametersForCommandGeneration): flush, stop, start. */
void audio_voices_prepare(AudioRenderer *r) {
    for (int k = 0; k < r->param.voice_count; k++) {
        ServerVoice *v = &r->voices[k];
        if (!v->in.in_use) continue;
        if (v->flush) {   /* FlushWaveBuffers: from the client's current buffer, the queued ones count as played */
            unsigned i = v->flush_from & 3;
            for (uint32_t n = v->flush; n; n--, i = (i + 1) & 3) {
                v->pending[i] = 0;
                if (v->valid[i]) { v->valid[i] = 0; v->consumed++; v->index = (v->index + 1) & 3; v->offset = 0; v->loops_done = 0; r->flushed++; }
            }
            v->flush = 0;
        }
        if (v->state == SV_STOPPING) {
            if (!stop_release()) { v->state = SV_STOPPED; continue; }
            for (int i = 0; i < 4; i++) {   /* every queued buffer counts as played; ones never sent to the DSP are dropped */
                if (v->valid[i]) { v->consumed++; v->index = (v->index + 1) & 3; r->stop_released++; }
                v->valid[i] = 0; v->pending[i] = 0;
            }
            v->offset = 0; v->played = 0; v->fraction = 0; v->loops_done = 0;
            memset(&v->adpcm, 0, sizeof v->adpcm); v->adpcm.buffer = -1;
            v->state = SV_STOPPED;
            continue;
        }
        if (v->state == SV_STARTED) {
            for (int i = 0; i < 4; i++) if (v->pending[i]) {
                v->pending[i] = 0; v->valid[i] = 1;
                if ((unsigned)i == v->index) { v->offset = 0; v->loops_done = 0; }
            }
            skip_empty(r, v);
        }
    }
}

static uint32_t effect_out_bytes(const AudioRenderer *r) { return revision_number(r->revision) >= 9 ? 0x90u : 0x10u; }

static void request_update(AudioRenderer *r, const uint8_t *input, uint64_t in_size, uint8_t *output, uint64_t out_size, uint64_t perf_size) {
    need(in_size >= sizeof(AudrenHeader), "update input too small");
    AudrenHeader h; memcpy(&h, input, sizeof h);
    need(h.total <= in_size, "update total size");
    Reader in = {input + sizeof h, input + h.total};
    pthread_mutex_lock(&r->lock);
    r->revision = h.revision;
    const int vc = r->param.voice_count, ec = r->param.effect_count, sc = r->param.sink_count, mc = r->param.submix_count + 1;
    const uint32_t npools = (uint32_t)ec + 4u * (uint32_t)vc;
    if (!r->logged) {
        r->logged = 1;
        fprintf(stderr, "[totk-audio] update REV%d sizes behavior=%u pools=%u voices=%u channels=%u effects=%u mixes=%u sinks=%u perf=%u splitters=%u render=%u total=%u out=%llu perf_out=%llu\n",
                revision_number(h.revision), h.behavior, h.memory_pools, h.voices, h.voice_resources, h.effects, h.mixes, h.sinks, h.performance, h.splitters, h.render_info, h.total,
                (unsigned long long)out_size, (unsigned long long)perf_size);
    }
    need(h.behavior == 0x10, "behavior section size");
    const uint8_t *behavior = take(&in, h.behavior, "behavior"); memcpy(&r->behavior_flags, behavior + 8, 8);
    need(h.memory_pools == npools * sizeof(AudrenPoolIn), "memory pool section size");
    const AudrenPoolIn *pools = (const AudrenPoolIn *)take(&in, h.memory_pools, "memory pools");
    need(h.voice_resources % sizeof(AudrenChannelIn) == 0 && h.voice_resources / sizeof(AudrenChannelIn) <= (uint32_t)vc * 6, "voice channel section size");
    const AudrenChannelIn *chans = (const AudrenChannelIn *)take(&in, h.voice_resources, "voice channels");
    int float_voices=revision_number(h.revision)>=15;
    size_t voice_stride=float_voices?sizeof(AudrenVoiceInV2):sizeof(AudrenVoiceIn);
    need(h.voices == (uint32_t)vc * voice_stride, "voice section size");
    const uint8_t *voices = take(&in, h.voices, "voices");
    need(h.effects == (uint32_t)ec * sizeof(AudrenEffectIn), "effect section size");
    const AudrenEffectIn *effects = (const AudrenEffectIn *)take(&in, h.effects, "effects");
    const uint8_t *splitters = take(&in, h.splitters, "splitters");
    const uint8_t *mixes = take(&in, h.mixes, "mixes");
    need(h.sinks == (uint32_t)sc * sizeof(AudrenSinkIn), "sink section size");
    const AudrenSinkIn *sinks = (const AudrenSinkIn *)take(&in, h.sinks, "sinks");
    take(&in, h.performance, "performance");
    take(&in, h.render_info, "render info");
    need(in.p == in.end, "unconsumed update input");

    for (uint32_t i = 0; i < h.voice_resources / sizeof(AudrenChannelIn); i++) {
        need(chans[i].id < (uint32_t)vc * 6, "channel resource id");
        r->channels[chans[i].id] = chans[i];
    }
    for (int i = 0; i < vc; i++) {
        AudrenVoiceIn v;
        AudrenVoiceInV2 v2;
        if(float_voices){
            memcpy(&v2,voices+i*voice_stride,sizeof v2);memset(&v,0,sizeof v);
            memcpy(&v,v2.prefix,sizeof v2.prefix);
            memcpy((uint8_t *)&v+0x3c,v2.tail,sizeof v2.tail);
            for(unsigned f=0;f<2;f++){
                v.biquads[f].enable=v2.biquads[f].enable;
                if(v.biquads[f].enable){
                    for(unsigned c=0;c<3;c++)need(isfinite(v2.biquads[f].numerator[c]),"nonfinite voice biquad numerator");
                    for(unsigned c=0;c<2;c++)need(isfinite(v2.biquads[f].denominator[c]),"nonfinite voice biquad denominator");
                }
            }
        }else memcpy(&v,voices+i*voice_stride,sizeof v);
        update_voice(r,&v);
        ServerVoice *server=&r->voices[v.id];server->use_float_biquads=float_voices;
        if(float_voices)memcpy(server->float_biquads,v2.biquads,sizeof v2.biquads);
    }
    for (int i = 0; i < ec; i++) r->effects[i] = effects[i];
    if (getenv("TOTK_AUDIO_DUMP")) { static unsigned dumped; unsigned active = 0; for (int i=0;i<ec;i++) if (effects[i].enabled && effects[i].type) active++;
        if (active > dumped) { dumped = active; FILE *f = fopen("analysis/audio-effects.bin", "wb"); if (f) { fwrite(effects, sizeof *effects, ec, f); fclose(f); } }
    }
    audio_mixer_update_effects(r);
    audio_mixer_update_routing(r, splitters, h.splitters, mixes, h.mixes, mc);
    for (int i = 0; i < sc; i++) r->sinks[i] = sinks[i];

    /* ---- output ---- */
    const uint32_t eo = effect_out_bytes(r);
    AudrenHeader o = {0};
    o.revision = REV(revision_number(h.revision) >= 1 ? revision_number(h.revision) : 1);
    o.memory_pools = npools * sizeof(AudrenPoolOut); o.voices = (uint32_t)vc * sizeof(AudrenVoiceOut);
    o.effects = (uint32_t)ec * eo; o.sinks = (uint32_t)sc * sizeof(AudrenSinkOut);
    o.performance = 0x10; o.behavior = BEHAVIOR_OUT; o.render_info = RENDER_INFO_OUT;
    o.total = (uint32_t)sizeof o + o.memory_pools + o.voices + o.effects + o.sinks + o.performance + o.behavior + o.render_info;
    if (o.total > out_size) { fprintf(stderr, "[totk-audio] output needs %u bytes, client gave %llu (effect out %u)\n", o.total, (unsigned long long)out_size, eo); runtime_fail("audio renderer output size"); }
    memset(output, 0, out_size);
    uint8_t *w = output; memcpy(w, &o, sizeof o); w += sizeof o;
    for (uint32_t i = 0; i < npools; i++) {
        uint32_t s = pools[i].state;
        if (s == POOL_REQUEST_ATTACH) s = POOL_ATTACHED; else if (s == POOL_REQUEST_DETACH) s = POOL_DETACHED;
        r->pools[i] = pools[i]; r->pools[i].state = s;
        AudrenPoolOut po = {s, {0}}; memcpy(w, &po, sizeof po); w += sizeof po;
    }
    for (int i = 0; i < vc; i++) {
        AudrenVoiceOut vo = {r->voices[i].played, r->voices[i].consumed, r->voices[i].drops};
        memcpy(w, &vo, sizeof vo); w += sizeof vo;
    }
    for (int i = 0; i < ec; i++) { w[0] = effects[i].enabled ? 3 : 4; if(eo>16)audio_mixer_effect_result(r,i,w+16,eo-16); w += eo; }   /* EffectStatus Enabled=3 / Disabled=4 */
    for (int i = 0; i < sc; i++) { AudrenSinkOut so={0};if(r->sinks[i].type==SINK_CIRCULAR)so.last_written_offset=audio_mixer_sink_written(r,i);memcpy(w,&so,sizeof so);w += sizeof so; }
    w += 0x10;                                            /* performance: nothing written */
    w += BEHAVIOR_OUT;                                    /* behavior: no errors */
    memcpy(w, &r->elapsed_frames, 8);
    pthread_mutex_unlock(&r->lock);
}

/* ---------- Render clock: one frame every sample_count / sample_rate seconds ---------- */
static void advance_voice(AudioRenderer *r, ServerVoice *v, double out_seconds) {
    if (!v->in.in_use || v->state != SV_STARTED || !v->in.sample_rate) return;
    double want = v->fraction + out_seconds * v->in.sample_rate * (v->in.pitch > 0 ? v->in.pitch : 1.0);
    uint64_t samples = (uint64_t)want; v->fraction = want - (double)samples;
    while (samples && v->valid[v->index]) {
        AudrenWaveBuffer *b = &v->buffers[v->index];
        uint64_t length = (uint64_t)(b->end - b->start), left = length - v->offset;
        uint64_t step = samples < left ? samples : left;
        v->offset += step; v->played += step; samples -= step;
        if (v->offset < length) break;
        if (b->loop && (b->loop_count < 0 || v->loops_done < b->loop_count)) { v->loops_done++; v->offset = b->loop_start > b->start && b->loop_start < b->end ? (uint64_t)(b->loop_start - b->start) : 0; continue; }
        v->valid[v->index] = 0; v->consumed++; v->offset = 0; v->loops_done = 0; v->index = (v->index + 1) & 3;
        /* End of stream resets the played count and the frame goes on into the next queued buffer, as on the DSP.
           Echoes of Wisdom marks every 14336-sample block of its streamed music this way; stopping there left up to a
           frame of silence at each block: a click every 0.3 s in all music. */
        if (b->end_of_stream) v->played = 0;
        skip_empty(r, v);
    }
}
static void *render_thread(void *p) {
    AudioRenderer *r = p;
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE,0);
    const double frame = (double)r->param.sample_count / r->param.sample_rate;
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    while (__atomic_load_n(&r->running, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&r->lock);
        if (r->state == 0) {
            audio_voices_prepare(r);
            audio_mixer_render(r, frame);
            for (int i = 0; i < r->param.voice_count; i++) advance_voice(r, &r->voices[i], frame);
            r->elapsed_frames++;
            if (r->stop_released + r->empty_played + r->flushed != r->logged_released && r->elapsed_frames % 1000 == 0) {
                r->logged_released = r->stop_released + r->empty_played + r->flushed;
                fprintf(stderr, "[totk-audio] wave buffers given back: %llu by stopped voices, %llu without samples, %llu flushed (since start)\n",
                        (unsigned long long)r->stop_released, (unsigned long long)r->empty_played, (unsigned long long)r->flushed);
            }
        }
        pthread_mutex_unlock(&r->lock);
        runtime_event_signal(r->event);
        uint64_t ns = (uint64_t)(frame * 1e9);
        next.tv_nsec += (long)ns; while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t wait = (int64_t)(next.tv_sec - now.tv_sec) * 1000000000LL + (next.tv_nsec - now.tv_nsec);
        if (wait > 0) { struct timespec d = {wait / 1000000000LL, wait % 1000000000LL}; nanosleep(&d, NULL); }
        else if (wait < -100000000LL) next = now;   /* fell far behind (debugger, sleep): resynchronise */
    }
    return NULL;
}

/* ---------- IAudioRenderer ---------- */
static IpcBuffer pick(const IpcBuffer *a, unsigned na, const IpcBuffer *b, unsigned nb, unsigned i) {
    IpcBuffer x = i < na ? a[i] : (IpcBuffer){0}, y = i < nb ? b[i] : (IpcBuffer){0};
    return x.size ? x : y;
}
static void renderer_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioRenderer *r = s->state;
    switch (rq->cmd) {
    case 0: { int32_t v = r->param.sample_rate; IPC_OUT(rs, v); return; }
    case 1: { int32_t v = r->param.sample_count; IPC_OUT(rs, v); return; }
    case 2: { int32_t v = r->param.mix_buffer_count; IPC_OUT(rs, v); return; }
    case 3: { uint32_t v = r->state; IPC_OUT(rs, v); return; }
    case 4: case 10: {   /* RequestUpdate / RequestUpdateAuto: out(output), out(performance), in(input) */
        IpcBuffer in = pick(rq->a, rq->na, rq->x, rq->nx, 0);
        IpcBuffer out = pick(rq->b, rq->nb, rq->c, rq->nc, 0), perf = pick(rq->b, rq->nb, rq->c, rq->nc, 1);
        if (!r->logged) fprintf(stderr, "[totk-audio] RequestUpdate cmd %u in=%#llx+%#llx out=%#llx+%#llx perf=%#llx+%#llx (a%u b%u x%u c%u)\n", rq->cmd,
            (unsigned long long)in.addr, (unsigned long long)in.size, (unsigned long long)out.addr, (unsigned long long)out.size, (unsigned long long)perf.addr, (unsigned long long)perf.size, rq->na, rq->nb, rq->nx, rq->nc);
        if (getenv("TOTK_AUDIO_PROBE") && in.addr < (1ull << 32)) {
            for (uint64_t hi = 0; hi < 0x100; hi++) { uint64_t a = (hi << 32) | in.addr; vm_size_t n = 0; uint32_t w = 0;
                if (vm_read_overwrite(mach_task_self(), a, 4, (vm_address_t)&w, &n) == KERN_SUCCESS && (w & 0xffffff) == 0x564552) fprintf(stderr, "[totk-audio] probe: header at %#llx\n", (unsigned long long)a); }
            runtime_fail("probe done");
        }
        need(in.size && out.size, "RequestUpdate buffers");
        request_update(r, ipc_guest(in.addr, in.size), in.size, ipc_guest(out.addr, out.size), out.size, perf.size);
        for (unsigned i = 0; i < rq->nc && rq->cmd == 10; i++) if (rq->c[i].size && (i >= rq->nb || !rq->b[i].size)) rs->xout[rs->nxout++] = (IpcBuffer){rq->c[i].addr, rq->c[i].size, i};
        return;
    }
    case 5: pthread_mutex_lock(&r->lock); r->state = 0;audio_mixer_set_running(r,1); pthread_mutex_unlock(&r->lock); return;
    case 6: pthread_mutex_lock(&r->lock); r->state = 1;audio_mixer_set_running(r,0); pthread_mutex_unlock(&r->lock); return;
    case 7: rs->copy[rs->ncopy++] = runtime_event_readable(r->event); return;
    case 8: r->time_limit = IPC_ARG(rq, uint32_t, 0); return;
    case 9: { uint32_t v = r->time_limit ? r->time_limit : 100; IPC_OUT(rs, v); return; }
    default: ipc_unimplemented(s, rq);
    }
}
static void renderer_close(void *p) {
    AudioRenderer *r = p;
    __atomic_store_n(&r->running, 0, __ATOMIC_RELEASE); pthread_join(r->thread, NULL);
    audio_mixer_close(r);
    free(r->voices); free(r->pools); free(r->mixes); free(r->sinks); free(r->effects); free(r->channels); free(r);
}

/* ---------- IAudioRendererManager ---------- */
static void manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {   /* OpenAudioRenderer(param, pad, work size, aruid; copy tmem, process) */
        AudrenParameter p = IPC_ARG(rq, AudrenParameter, 0);
        need(rq->ncopy >= 1, "OpenAudioRenderer handles");
        AudioRenderer *r = renderer_new(&p);
        r->running = 1; audio_mixer_open(r);
        need(pthread_create(&r->thread, NULL, render_thread, r) == 0, "render thread");
        fprintf(stderr, "[totk-audio] renderer open %d Hz x%d, voices %d effects %d submixes %d sinks %d splitters %d/%d REV%d\n", p.sample_rate, p.sample_count,
                p.voice_count, p.effect_count, p.submix_count, p.sink_count, p.splitter_count, p.splitter_destination_count, revision_number(p.revision));
        rs->move[rs->nmove++] = runtime_ipc_new_session("IAudioRenderer", renderer_handler, r, renderer_close);
        return;
    }
    case 1: { uint64_t v = WORK_BUFFER_BYTES; IPC_OUT(rs, v); return; }
    case 2: case 4: rs->move[rs->nmove++] = runtime_ipc_new_session("IAudioDevice", audio_device_handler, NULL, NULL); return;
    default: ipc_unimplemented(s, rq);
    }
}

void runtime_audio_register(void) { runtime_ipc_register("audren:u", manager_handler, NULL, NULL); }
__attribute__((constructor)) static void register_audio(void) { runtime_audio_register(); }

/* ---------- make probe-audio: mixer checks on constructed renderer state (no game, no audio device) ---------- */
int audio_mixer_read(AudioRenderer *r, float *lr, int frames);
static int near(float a, float b) { return fabsf(a - b) <= 1e-4f; }
/* Complete REV15 wire request -> parser -> float voice/splitter filters -> sink.
 * Expected samples use separate double-precision recurrences, including state
 * across two requests. Coefficients deliberately cannot be represented in Q14. */
static int probe_rev15(void){
    AudrenParameter p={.sample_rate=48000,.sample_count=240,.mix_buffer_count=2,.voice_count=1,.sink_count=1,.splitter_count=1,.splitter_destination_count=1,.revision=REV(15)};
    AudioRenderer *r=renderer_new(&p);audio_mixer_open(r);
    static float pcm[480];for(unsigned k=0;k<480;k++)pcm[k]=(float)(0.3*sin(k*0.071)+(k%37==0?0.1:0));
    AudrenHeader h={.revision=REV(15),.behavior=16,.memory_pools=4*sizeof(AudrenPoolIn),.voice_resources=sizeof(AudrenChannelIn),.voices=sizeof(AudrenVoiceInV2),.splitters=0x40+0xa8,.mixes=sizeof(AudrenMixIn),.sinks=sizeof(AudrenSinkIn),.performance=16};
    h.total=sizeof h+h.behavior+h.memory_pools+h.voice_resources+h.voices+h.splitters+h.mixes+h.sinks+h.performance;
    uint8_t *input=calloc(1,h.total),output[4096];need(input!=NULL,"REV15 probe allocation");
    AudrenVoiceIn v={.id=0,.is_new=1,.in_use=1,.play_state=PLAY_STARTED,.sample_format=FMT_PCMF32,.sample_rate=48000,.channel_count=1,.pitch=1,.volume=1,.mix_id=0x7fffffff,.splitter_id=0};
    v.wave_buffers[0]=(AudrenWaveBuffer){.address=(uintptr_t)pcm,.size=sizeof pcm,.end=480};
    for(unsigned k=1;k<4;k++)v.wave_buffers[k].sent=1;
    AudrenBiquadFloat voice={.enable=1,.numerator={0.1234567f,0.0712345f,0},.denominator={0.25f,0}};
    AudrenBiquadFloat splitter={.enable=1,.numerator={0.7654321f,0,0},.denominator={0.125f,0}};
    double vx=0,vy=0,sy=0;int checked=0;
    for(unsigned block=0;block<2;block++){
        uint8_t *at=input;memcpy(at,&h,sizeof h);at+=sizeof h+h.behavior+h.memory_pools+h.voice_resources;
        AudrenVoiceInV2 wire={0};memcpy(wire.prefix,&v,sizeof wire.prefix);memcpy(wire.tail,(uint8_t *)&v+0x3c,sizeof wire.tail);wire.biquads[0]=voice;
        memcpy(at,&wire,sizeof wire);at+=sizeof wire;
        uint32_t sh[3]={0x48444e53,1,1},si[5]={0x49444e53,0,48000,1,0},magic=0x44444e53;
        memcpy(at,sh,12);memcpy(at+0x20,si,sizeof si);
        uint8_t *dest=at+0x40;memcpy(dest,&magic,4);float volumes[24]={1,0.5f};memcpy(dest+8,volumes,sizeof volumes);memcpy(dest+0x6c,&splitter,sizeof splitter);dest[0x9c]=1;
        at+=h.splitters;
        AudrenMixIn mix={.volume=1,.sample_rate=48000,.buffer_count=2,.in_use=1,.mix_id=0,.dest_mix_id=0x7fffffff,.dest_splitter_id=0xffffffff};memcpy(at,&mix,sizeof mix);at+=sizeof mix;
        AudrenSinkIn sink={.type=SINK_DEVICE,.in_use=1,.input_count=2,.inputs={0,1}};memcpy(at,&sink,sizeof sink);
        request_update(r,input,h.total,output,sizeof output,0);
        AudrenHeader reply;memcpy(&reply,output,sizeof reply);need(reply.revision==REV(15)&&reply.voices==sizeof(AudrenVoiceOut)&&reply.total<=sizeof output,"REV15 probe response");
        if(getenv("TOTK_AUDIO_REV15_NEGATIVE"))r->voices[0].use_float_biquads=0;
        audio_voices_prepare(r);audio_mixer_render(r,240.0/48000);advance_voice(r,&r->voices[0],240.0/48000);
        float samples[480];need(audio_mixer_read(r,samples,240)==240,"REV15 probe ring");
        for(unsigned k=0;k<240;k++){
            double x=pcm[block*240+k],y=voice.numerator[0]*x+voice.numerator[1]*vx+voice.denominator[0]*vy;vx=x;vy=y;
            double z=splitter.numerator[0]*y+splitter.denominator[0]*sy;sy=z;
            need(fabs(samples[2*k]-z)<1e-6&&fabs(samples[2*k+1]-z*0.5)<1e-6,"audio probe: REV15 float voice and splitter samples");checked+=2;
        }
        v.is_new=0;v.wave_buffers[0].sent=1;
    }
    free(input);audio_mixer_close(r);free(r->voices);free(r->pools);free(r->mixes);free(r->sinks);free(r->effects);free(r->channels);free(r);
    return checked;
}
extern int g_audio_probe_flat;   /* audio_mixer.c */
void runtime_audio_probe(void) {
    g_audio_probe_flat = 1;   /* the checks below compare decode / resample / mix values: no volume ramps or depop tails */
    setenv("TOTK_AUDIO_OFF", "1", 1);
    const int neg = getenv("TOTK_PROBE_NEGATIVE") != NULL;
    AudrenParameter p = {48000, 240, 8, 1, 4, 1, 2, 0, 0, 0, 0, 0, 1, 2, 0, REV(11)};
    AudioRenderer *r = renderer_new(&p); audio_mixer_open(r);
    int checked = probe_rev15();
    /* final mix 0: buffers 0-1; submix 1: buffers 2-3 -> mix 0 with a swap matrix at volume 0.5 */
    r->mixes[0] = (AudrenMixIn){.volume = 1, .sample_rate = 48000, .buffer_count = 2, .in_use = 1, .mix_id = 0, .dest_mix_id = 0x7fffffff, .dest_splitter_id = 0xffffffffu};
    r->mixes[1] = (AudrenMixIn){.volume = 0.5f, .sample_rate = 48000, .buffer_count = 2, .in_use = 1, .mix_id = 1, .dest_mix_id = 0, .dest_splitter_id = 0xffffffffu};
    r->mixes[1].mix[0][1] = 1; r->mixes[1].mix[1][0] = 1;
    r->sinks[0] = (AudrenSinkIn){.type = SINK_DEVICE, .in_use = 1, .input_count = 2, .inputs = {0, 1}};
    /* 1. PCM16 sine through a channel resource: L 0.5, R 0.125 */
    static int16_t pcm[4800]; for (int i = 0; i < 4800; i++) pcm[i] = (int16_t)(16000 * sin(2 * M_PI * 1000 * i / 48000.0));
    AudrenVoiceIn v = {.id = 0, .node_id = 0, .is_new = 1, .in_use = 1, .play_state = PLAY_STARTED, .sample_format = FMT_PCM16, .sample_rate = 48000, .channel_count = 1,
                       .pitch = 1, .volume = 0.5f, .mix_id = 0, .splitter_id = 0xffffffffu, .channel_ids = {0}};
    v.wave_buffers[0] = (AudrenWaveBuffer){.address = (uint64_t)(uintptr_t)pcm, .size = sizeof pcm, .start = 0, .end = 4800, .loop = 1, .loop_count = -1};
    r->channels[0].mix[0] = 1; r->channels[0].mix[1] = neg ? 0.3f : 0.25f;
    update_voice(r, &v);
    const double frame = 240.0 / 48000;
    float lr[480];
    for (int f = 0; f < 3; f++) { audio_voices_prepare(r); audio_mixer_render(r, frame); advance_voice(r, &r->voices[0], frame); }
    if (audio_mixer_read(r, lr, 240) != 240) runtime_fail("audio probe ring");
    for (int k = 0; k < 240; k++) { float s = pcm[480 + k] / 32768.0f;
        if (!near(lr[2 * k], 0.5f * s) || !near(lr[2 * k + 1], 0.125f * s)) { fprintf(stderr, "pcm frame sample %d: %f %f want %f %f\n", k, lr[2 * k], lr[2 * k + 1], 0.5f * s, 0.125f * s); runtime_fail("audio probe: PCM16 mix"); } checked += 2; }
    /* 2. pitch 0.5: linear interpolation between neighbouring samples */
    v.is_new = 1; v.pitch = 0.5f; update_voice(r, &v);
    audio_voices_prepare(r); audio_mixer_render(r, frame); advance_voice(r, &r->voices[0], frame); audio_mixer_read(r, lr, 240);
    for (int k = 0; k < 240; k++) { int i = k / 2; float s = (k & 1) ? (pcm[i] + pcm[i + 1]) / 65536.0f : pcm[i] / 32768.0f;
        if (!near(lr[2 * k], 0.5f * s)) { fprintf(stderr, "pitch sample %d: %f want %f\n", k, lr[2 * k], 0.5f * s); runtime_fail("audio probe: resampling"); } checked++; }
    /* 3. DSP-ADPCM, coefficients {1.0, 0} for predictor 0 (an integrator), scale 2^2 */
    static uint8_t adpcm[8 * 20]; static int16_t coef[16]; coef[0] = 2048;
    int16_t want[280]; int32_t h = 0;
    for (int fr = 0; fr < 20; fr++) { adpcm[fr * 8] = 0x02; for (int j = 0; j < 7; j++) adpcm[fr * 8 + 1 + j] = (uint8_t)(((j + fr) & 7) << 4 | ((15 - j) & 15));
        for (int j = 0; j < 14; j++) { int n = (adpcm[fr * 8 + 1 + j / 2] >> ((j & 1) ? 0 : 4)) & 15; if (n >= 8) n -= 16; h += n * 4; if (h > 32767) h = 32767; if (h < -32768) h = -32768; want[fr * 14 + j] = (int16_t)h; } }
    AudrenVoiceIn a = v; a.is_new = 1; a.pitch = 1; a.sample_format = FMT_ADPCM; a.extra_address = (uint64_t)(uintptr_t)coef; a.extra_size = sizeof coef;
    a.wave_buffers[0] = (AudrenWaveBuffer){.address = (uint64_t)(uintptr_t)adpcm, .size = sizeof adpcm, .start = 0, .end = 280};
    update_voice(r, &a);
    audio_voices_prepare(r); audio_mixer_render(r, frame); advance_voice(r, &r->voices[0], frame); audio_mixer_read(r, lr, 240);
    for (int k = 0; k < 240; k++) { float s = want[k] / 32768.0f;
        if (!near(lr[2 * k], 0.5f * s)) { fprintf(stderr, "adpcm sample %d: %f want %f\n", k, lr[2 * k], 0.5f * s); runtime_fail("audio probe: ADPCM"); } checked++; }
    /* 4. splitter: voice -> destination 0 (submix 1, volumes 1 / 0.5) -> mix 1 swapped into mix 0 at 0.5 */
    a.in_use = 0; update_voice(r, &a);
    uint8_t split[0x20 + 0x20 + 0x70] = {0}; uint32_t h0[3] = {0x48444e53u, 1, 1}, h1[5] = {0x49444e53u, 0, 48000, 1, 0}; memcpy(split, h0, 12); memcpy(split + 0x20, h1, 20);   /* info: 16 + 4 ids + 12 reserved */
    uint8_t *d = split + 0x40; uint32_t dm = 0x44444e53u, did = 0; int32_t dst = 1; float vol[24] = {1, 0.5f}; memcpy(d, &dm, 4); memcpy(d + 4, &did, 4); memcpy(d + 8, vol, sizeof vol); memcpy(d + 0x68, &dst, 4); d[0x6c] = 1;
    uint8_t mixes[2 * sizeof(AudrenMixIn)]; memcpy(mixes, r->mixes, sizeof mixes);
    audio_mixer_update_routing(r, split, sizeof split, mixes, sizeof mixes, 2);
    AudrenVoiceIn sv = v; sv.id = 1; sv.is_new = 1; sv.pitch = 1; sv.mix_id = 0x7fffffff; sv.splitter_id = 0; update_voice(r, &sv);
    audio_voices_prepare(r); audio_mixer_render(r, frame); advance_voice(r, &r->voices[1], frame); audio_mixer_read(r, lr, 240);
    for (int k = 0; k < 240; k++) { float s = pcm[k] / 32768.0f * 0.5f * 0.5f;   /* voice 0.5, mix 1 volume 0.5; buffer 2 (x1) -> R, buffer 3 (x0.5) -> L */
        if (!near(lr[2 * k], 0.5f * s) || !near(lr[2 * k + 1], s)) { fprintf(stderr, "splitter sample %d: %f %f want %f %f\n", k, lr[2 * k], lr[2 * k + 1], 0.5f * s, s); runtime_fail("audio probe: splitter routing"); } checked += 2; }
    /* 5. Two cascaded Q14 filters, independent stereo channels and state across frames.
       The oracle uses double-precision direct form I; production uses float transposed form II. */
    sv.in_use = 0; update_voice(r, &sv);
    static int16_t stereo[240 * 6 * 2];
    for (int k = 0; k < 240 * 6; k++) {
        stereo[2 * k] = (int16_t)(7000 * sin(k * 0.043) + (k % 71 == 0 ? 4000 : 0));
        stereo[2 * k + 1] = (int16_t)(5000 * cos(k * 0.071) - (k % 53 == 0 ? 3000 : 0));
    }
    AudrenBiquad filters[2] = {{.enable = 1, .numerator = {8192, 4096, 2048}, .denominator = {6144, -2048}},
                              {.enable = 1, .numerator = {12288, -4096, 1024}, .denominator = {4096, -1024}}};
    v.is_new = 1; v.pitch = 1; v.volume = 1; v.channel_count = 2; v.channel_ids[1] = 1;
    v.wave_buffers[0] = (AudrenWaveBuffer){.address = (uint64_t)(uintptr_t)stereo, .size = sizeof stereo, .end = 240 * 6};
    memset(r->channels, 0, 2 * sizeof *r->channels); r->channels[0].mix[0] = 1; r->channels[1].mix[1] = 1;
    double history[2][2][4] = {{{0}}}; int cursor = 0;
    for (int block = 0; block < 6; block++) {
        filters[0].enable = block != 2;
        if (block == 2) memset(history[0], 0, sizeof history[0]);
        if (block == 4) { v.is_new = 1; v.wave_buffers[0].sent = 0; cursor = 0; memset(history, 0, sizeof history); }
        memcpy(v.biquads, filters, sizeof filters);
        if (getenv("TOTK_AUDIO_BIQUAD_NEGATIVE")) v.biquads[0].enable = v.biquads[1].enable = 0;
        update_voice(r, &v);
        audio_voices_prepare(r); audio_mixer_render(r, frame); advance_voice(r, &r->voices[0], frame); audio_mixer_read(r, lr, 240);
        for (int k = 0; k < 240; k++) for (int c = 0; c < 2; c++) {
            double x = stereo[2 * (cursor + k) + c] / 32768.0;
            for (int f = 0; f < 2; f++) if (filters[f].enable) {
                double *h = history[f][c]; const AudrenBiquad *b = &filters[f];
                double y = (b->numerator[0] * x + b->numerator[1] * h[0] + b->numerator[2] * h[1]
                          + b->denominator[0] * h[2] + b->denominator[1] * h[3]) / 16384.0;
                h[1] = h[0]; h[0] = x; h[3] = h[2]; h[2] = y; x = y;
            }
            if (!near(lr[2 * k + c], (float)x)) {
                fprintf(stderr, "biquad block %d sample %d channel %d: %.8f want %.8f\n", block, k, c, lr[2 * k + c], x);
                runtime_fail("audio probe: biquad state/routing");
            }
            checked++;
        }
        cursor += 240; v.is_new = 0; v.wave_buffers[0].sent = 1;
    }
    /* 6. Ordered submix effects, distinct output buffers, disabled bypass and downstream routing. */
    memset(v.biquads, 0, sizeof v.biquads); v.mix_id=1; v.is_new=1; v.wave_buffers[0].sent=0;
    r->mixes[1].buffer_count=4; memset(r->mixes[1].mix,0,sizeof r->mixes[1].mix);
    r->mixes[1].mix[2][1]=1; r->mixes[1].mix[3][0]=1;
    Reverb3dParam rp={.input={0,1},.output={2,3},.channel_max=6,.channels=2,.sample_rate=48000,
        .room_hf=-2000,.hf_reference=4000,.decay_time=.8f,.hf_decay_ratio=.7f,.room_gain=-20000,
        .diffusion=50,.density=70,.dry_gain=.25f,.status=2};
    r->effects[0]=(AudrenEffectIn){.type=5,.is_new=1,.enabled=1,.mix_id=1,.order=1};
    r->effects[1]=(AudrenEffectIn){.type=5,.is_new=1,.enabled=1,.mix_id=1,.order=0};
    memcpy(r->effects[0].specific,&rp,sizeof rp); rp.output[0]=0;rp.output[1]=1;rp.dry_gain=.5f;memcpy(r->effects[1].specific,&rp,sizeof rp);
    for(int block=0;block<2;block++) {
        update_voice(r,&v); r->effects[0].enabled=block==0;
        if(getenv("TOTK_AUDIO_REVERB_NEGATIVE"))r->effects[0].order=-1;
        audio_mixer_update_effects(r);audio_voices_prepare(r);audio_mixer_render(r,frame);audio_mixer_read(r,lr,240);
        float gain=block==0?.0625f:.25f;
        for(int k=0;k<240;k++)for(int c=0;c<2;c++){float want=stereo[2*k+1-c]/32768.f*gain;
            if(!near(lr[2*k+c],want))runtime_fail("audio probe: reverb effect order/bypass/routing");checked++;}
    }
    /* Real wet response across IPC updates: the DSP kernel is independently compared to Ryujinx by probe-reverb. */
    r->effects[1].type=0; r->effects[0].enabled=1;r->effects[0].is_new=1;r->effects[0].order=0;
    rp.room_gain=-800;rp.reverb_gain=500;rp.dry_gain=.1f;rp.output[0]=2;rp.output[1]=3;
    memcpy(r->effects[0].specific,&rp,sizeof rp); Reverb3d *ref=audio_reverb_create(&rp);
    float input[2][240],output[2][240],*ip[6]={input[0],input[1]},*op[6]={output[0],output[1]};
    for(int block=0;block<80;block++){
        v.is_new=1;v.wave_buffers[0].sent=0;update_voice(r,&v);
        for(int c=0;c<2;c++)for(int k=0;k<240;k++)input[c][k]=stereo[2*k+c]/32768.f;
        audio_reverb_process(ref,ip,op,240);audio_mixer_update_effects(r);audio_voices_prepare(r);audio_mixer_render(r,frame);audio_mixer_read(r,lr,240);
        for(int c=0;c<2;c++)for(int k=0;k<240;k++){if(!near(lr[2*k+c],output[1-c][k]*.5f))runtime_fail("audio probe: reverb continuity in mix graph");checked++;}
        r->effects[0].is_new=0;
    }
    audio_reverb_free(ref);
    /* 7. Auxiliary int32 exchange: stereo wrap, DSP-owned cursors, CPU metadata and disabled passthrough. */
    uint32_t send_ring[32+960],return_ring[32+960];for(int i=0;i<32+960;i++){send_ring[i]=0x12345678;return_ring[i]=(uint32_t)(int32_t)(i-500);}
    send_ring[17]=return_ring[16]=720;send_ring[18]=return_ring[18]=7;
    AudrenAuxParam ap={.input={0,1},.output={2,3},.channels=2,.sample_rate=48000,.capacity=960,.channel_max=2,
        .send_info=(uint64_t)(uintptr_t)send_ring,.return_info=(uint64_t)(uintptr_t)return_ring};
    r->effects[0]=(AudrenEffectIn){.type=2,.enabled=1,.mix_id=1};memcpy(r->effects[0].specific,&ap,sizeof ap);
    audio_mixer_update_effects(r);update_voice(r,&v);audio_voices_prepare(r);audio_mixer_render(r,frame);audio_mixer_read(r,lr,240);
    for(int c=0;c<2;c++)for(int k=0;k<240;k++){
        int idx=(720+c*240+k)%960,want_sample=idx+32-500;
        if(!near(lr[2*k+1-c],want_sample/65536.f) || (int32_t)send_ring[32+idx]!=stereo[2*k+c])runtime_fail("audio probe: auxiliary sample/wrap/routing");checked+=2;
    }
    if(send_ring[17]!=240 || return_ring[16]!=240 || send_ring[0]!=0x12345678 || send_ring[18]!=7)runtime_fail("audio probe: auxiliary cursors");checked+=4;
    r->effects[0].enabled=0;update_voice(r,&v);audio_voices_prepare(r);audio_mixer_render(r,frame);audio_mixer_read(r,lr,240);
    for(int c=0;c<2;c++)for(int k=0;k<240;k++){if(!near(lr[2*k+1-c],stereo[2*k+c]/65536.f))runtime_fail("audio probe: auxiliary bypass");checked++;}
    if(send_ring[16]||send_ring[17]||send_ring[19]||return_ring[16]||return_ring[17]||return_ring[19]||send_ring[18]!=7)runtime_fail("audio probe: auxiliary reset");checked+=7;
    /* 8. Limiter zero-state delay and hard threshold. Independent expected rectangular response. */
    r->mixes[1].in_use=0;v.mix_id=0;v.sample_format=FMT_PCMF32;static float limiter_input[480];
    for(int k=0;k<240;k++){limiter_input[2*k]=.75f;limiter_input[2*k+1]=-.125f;}
    v.wave_buffers[0]=(AudrenWaveBuffer){.address=(uint64_t)(uintptr_t)limiter_input,.size=sizeof limiter_input,.end=240};
    AudrenLimiterParam lp={.input={0,1},.output={0,1},.channel_max=6,.channels=2,.sample_rate=48000,
        .attack=1,.release=1,.threshold=.5f,.input_gain=1,.output_gain=1,.delay_min=9,.delay_max=13,.status=2,.statistics_enabled=1};
    r->effects[0]=(AudrenEffectIn){.type=7,.enabled=1,.is_new=1,.mix_id=0};memcpy(r->effects[0].specific,&lp,sizeof lp);
    update_voice(r,&v);audio_mixer_update_effects(r);audio_voices_prepare(r);audio_mixer_render(r,frame);audio_mixer_read(r,lr,240);
    for(int c=0;c<2;c++)for(int k=0;k<240;k++){float expected=k<9?0:c==0?.5f*(32767.f/32768.f):-.125f;
        if(!near(lr[2*k+c],expected))runtime_fail("audio probe: limiter delay/threshold");checked++;}
    float statistics[12];audio_mixer_effect_result(r,0,statistics,sizeof statistics);
    if(!near(statistics[0],.75f*32768.f/32767.f)||!near(statistics[6],.5f/(.75f*32768.f/32767.f)))runtime_fail("audio probe: limiter statistics");checked+=2;
    const char *fixture=getenv("TOTK_LIMITER_REFERENCE");
    if(fixture){FILE *f=fopen(fixture,"rb");if(!f)runtime_fail("limiter reference fixture");lp.attack=.2f;lp.release=.02f;lp.threshold=.5f;lp.input_gain=1.3f;lp.output_gain=.8f;
        for(int block=0;block<150;block++){
            if(block==70){lp.threshold=.3f;lp.attack=.4f;}lp.statistics_reset=block==50;
            r->effects[0].is_new=block==0||block==120;r->effects[0].enabled=block<100||block>=110;
            memcpy(r->effects[0].specific,&lp,sizeof lp);
            for(int c=0;c<2;c++)for(int k=0;k<240;k++){int t=block*240+k;limiter_input[2*k+c]=((t*17+c*31)%257-128)/128.f*(block%7==0?1.8f:.3f);}
            v.is_new=1;v.wave_buffers[0].sent=0;update_voice(r,&v);audio_mixer_update_effects(r);audio_voices_prepare(r);audio_mixer_render(r,frame);audio_mixer_read(r,lr,240);
            audio_mixer_effect_result(r,0,statistics,sizeof statistics);
            for(int k=0;k<492;k++){float want;need(fread(&want,4,1,f)==1,"limiter reference length");float got=k<480?lr[k]:statistics[k-480];
                if(getenv("TOTK_LIMITER_NEGATIVE") && k<480)got=limiter_input[k];
                if(!isfinite(got)||fabsf(got-want)>2e-5f){fprintf(stderr,"limiter block %d index %d got %.9g want %.9g\n",block,k,got,want);runtime_fail("limiter independent reference");}checked++;
            }
        }need(fgetc(f)==EOF,"limiter reference trailing data");fclose(f);
    }
    audio_mixer_close(r);
    /* 7. Voice lifetime as the DSP keeps it (Ryujinx VoiceInfo / DataSourceHelper): a buffer without samples counts as
       played when it is reached, so the one behind it still plays; Stop gives back every queued buffer on the next render
       frame and starts the voice over; buffers sent while stopped wait for Start; FlushWaveBuffers counts the queued
       ones from the client's current buffer as played. 2026-10-17 the game over "retry" waited on stopped voices. */
    {
        static int16_t a7[480], b7[480]; for (int k = 0; k < 480; k++) { a7[k] = (int16_t)(k * 50); b7[k] = (int16_t)(-k * 50); }
        AudrenVoiceIn x = {.id = 2, .is_new = 1, .in_use = 1, .play_state = PLAY_STARTED, .sample_format = FMT_PCM16, .sample_rate = 48000,
                           .channel_count = 1, .pitch = 1, .volume = 1, .mix_id = 0x7fffffff, .splitter_id = 0xffffffffu, .channel_ids = {2}};
        x.wave_buffers[0] = (AudrenWaveBuffer){.address = (uint64_t)(uintptr_t)a7, .size = sizeof a7, .end = 480};
        x.wave_buffers[1] = (AudrenWaveBuffer){.address = 0, .size = 0, .end = 0};   /* nothing to decode */
        x.wave_buffers[2] = (AudrenWaveBuffer){.address = (uint64_t)(uintptr_t)b7, .size = sizeof b7, .end = 480};
        x.wave_buffers[3].sent = 1;
        ServerVoice *sv7 = &r->voices[2];
        update_voice(r, &x);
        for (int f = 0; f < 3; f++) { audio_voices_prepare(r); audio_mixer_render(r, frame); advance_voice(r, sv7, frame); }
        if (sv7->consumed != 2 || sv7->index != 2 || sv7->played != 720 || sv7->offset != 240) {
            fprintf(stderr, "empty buffer: consumed %u index %u played %llu offset %llu, want 2 2 720 240\n", sv7->consumed, sv7->index, (unsigned long long)sv7->played, (unsigned long long)sv7->offset);
            runtime_fail("audio probe: a buffer without samples holds up the voice");
        }
        checked += 4;
        x.is_new = 0; for (int i = 0; i < 4; i++) x.wave_buffers[i].sent = 1;
        x.play_state = PLAY_STOPPED; update_voice(r, &x);
        if (sv7->consumed != 2) runtime_fail("audio probe: Stop gave buffers back before a render frame");
        audio_voices_prepare(r); audio_mixer_render(r, frame); advance_voice(r, sv7, frame);
        if (sv7->consumed != 3 || sv7->played != 0 || sv7->offset != 0 || sv7->valid[0] | sv7->valid[1] | sv7->valid[2] | sv7->valid[3] || sv7->state != SV_STOPPED) {
            fprintf(stderr, "stop: consumed %u played %llu offset %llu state %u, want 3 0 0 stopped\n", sv7->consumed, (unsigned long long)sv7->played, (unsigned long long)sv7->offset, sv7->state);
            runtime_fail("audio probe: a stopped voice keeps its queued wave buffers");
        }
        checked += 5;
        x.wave_buffers[3] = (AudrenWaveBuffer){.address = (uint64_t)(uintptr_t)a7, .size = sizeof a7, .end = 480};   /* sent while stopped */
        update_voice(r, &x); audio_voices_prepare(r); audio_mixer_render(r, frame); advance_voice(r, sv7, frame);
        if (sv7->valid[3] || !sv7->pending[3] || sv7->consumed != 3 || sv7->played != 0) runtime_fail("audio probe: a buffer sent while stopped played");
        x.wave_buffers[3].sent = 1; x.wave_buffers[0] = (AudrenWaveBuffer){.address = (uint64_t)(uintptr_t)b7, .size = sizeof b7, .end = 480};
        x.play_state = PLAY_STARTED; update_voice(r, &x); audio_voices_prepare(r);
        if (!sv7->valid[3] || !sv7->valid[0] || sv7->index != 3) runtime_fail("audio probe: Start did not queue the waiting buffers");
        checked += 2;
        x.wave_buffers[0].sent = 1; x.flush_wave_buffer_count = 2; x.wave_buffer_index = 3; update_voice(r, &x);
        audio_voices_prepare(r); audio_mixer_render(r, frame); advance_voice(r, sv7, frame);
        if (sv7->consumed != 5 || sv7->valid[3] || sv7->valid[0] || sv7->played != 0) {
            fprintf(stderr, "flush: consumed %u valid %u%u played %llu, want 5 00 0\n", sv7->consumed, sv7->valid[3], sv7->valid[0], (unsigned long long)sv7->played);
            runtime_fail("audio probe: FlushWaveBuffers");
        }
        checked += 3;
        x.flush_wave_buffer_count = 0; x.in_use = 0; update_voice(r, &x);
    }
    printf("{\"status\":\"pass\",\"probe\":\"probe-audio\",\"checked\":%d,\"scope\":\"REV15 wire parsing, native float voice/splitter biquads and consecutive request state; PCM16 channel mix, pitch interpolation, DSP-ADPCM decode, splitter to submix to final mix, device sink, stereo cascaded biquads, ordered submix reverb, auxiliary ring exchange, output limiter, bypass, frame continuity and reset\"}\n", checked);
}
