/* Switch 2 nn::codec video buffer-mode host backend.
 * SDK: pool is 0x18 bytes, preference is 0x40 (returned through x8), VP9 is
 * 0x2a0. Acquire returns reusable image slots BEFORE they contain decoded output
 * (2c9fe0); Properties waits for completion (2ca800). Release recycles the slot.
 * Guest stack arguments occupy eight-byte slots; use full-width trailing args. */
#include "runtime.h"
#include <pthread.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

typedef struct { uint64_t words[8]; } Preference;
typedef struct { uint64_t words[2]; } NativeSync;
typedef struct Pool { void *guest; unsigned w,h,count; uint8_t *pixels; struct Pool *next; } Pool;
typedef struct { Pool *pool; unsigned image; int registered, acquired, ready, reported, eos; int64_t pts; uint64_t ticket; } Slot;
typedef struct Decoder {
    void *guest; vpx_codec_ctx_t vpx; unsigned w,h; int started,eos;
    Slot slots[8]; unsigned available[8], navailable, write_cursor;
    uint64_t serial, frames, shown, dropped, t0; int64_t pts0, last_input_pts, frame_period;
    pthread_cond_t changed; struct Decoder *next;
} Decoder;
/* All state including object lifetimes is protected by this mutex. Cond waits
 * release it, so input submission can satisfy a properties request. Finalize is
 * legal after the game's workers are joined, as in SDK (no concurrent destroy). */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Pool *pools; static Decoder *decoders;
typedef struct SyncEvent { void *guest; uint64_t ticket; struct SyncEvent *next; } SyncEvent;
static SyncEvent *sync_events;
static uint64_t next_ticket;
#define SYNC_TAG UINT64_C(0x4d4b575650394359)
static unsigned (*create_event)(void *,int,int);
static void (*signal_event)(void *), (*clear_event)(void *), (*destroy_event)(void *);
static _Bool (*try_event)(void *);
static void (*init_holder)(void *,void *);
static pthread_once_t guest_once = PTHREAD_ONCE_INIT;
#define LOG(...) do { fprintf(stderr,"[codec] " __VA_ARGS__); fputc('\n',stderr); } while(0)
static void *need(const char *name) { void *p=runtime_symbol(name); if(!p)runtime_fail("codec: guest symbol missing"); return p; }
static void guest_init(void) {
    create_event=need("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    signal_event=need("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    clear_event=need("_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE");
    destroy_event=need("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
    try_event=need("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE");
    init_holder=need("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE");
}
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static Decoder *decoder(void *g) { for(Decoder *d=decoders;d;d=d->next)if(d->guest==g)return d; runtime_fail("codec: unknown decoder"); }
static Pool *pool(const void *g) { for(Pool *p=pools;p;p=p->next)if(p->guest==g)return p; runtime_fail("codec: unknown image pool"); }
static void dimensions(unsigned w,unsigned h) { if(!w||!h||w>8192||h>8192||((w|h)&1))runtime_fail("codec: invalid NV12 dimensions"); }
static uint64_t image_bytes(const Pool *p) { return (uint64_t)p->w*p->h*3/2; }
/* The SDK's syncpt wrappers dispatch through an uninitialized console driver
 * table (529520..5296cc). The game's only Create call is main+19e7414 in its
 * movie completion queue. Keep its MultiWait behavior with local SystemEvents,
 * signaled only when the acquired slot's generation has an image. */
static SyncEvent *sync_event(void *g) {
    for(SyncEvent *e=sync_events;e;e=e->next)if(e->guest==g)return e;
    runtime_fail("codec: unknown sync point event");
}
static unsigned sync_create(void *g) {
    pthread_once(&guest_once,guest_init);
    SyncEvent *e=calloc(1,sizeof *e);if(!e)runtime_fail("codec: sync event allocation");
    unsigned result=create_event(g,0,0);if(result){free(e);return result;}
    e->guest=g;pthread_mutex_lock(&lock);e->next=sync_events;sync_events=e;pthread_mutex_unlock(&lock);return 0;
}
static void sync_destroy(void *g) {
    pthread_mutex_lock(&lock);SyncEvent **at=&sync_events;while(*at&&(*at)->guest!=g)at=&(*at)->next;
    if(!*at)runtime_fail("codec: destroy sync event");SyncEvent *e=*at;*at=e->next;free(e);
    pthread_mutex_unlock(&lock);destroy_event(g);
}
static _Bool sync_try(void *g) {return try_event(g);}
static void sync_cancel(void *g) {
    pthread_mutex_lock(&lock);sync_event(g)->ticket=0;clear_event(g);pthread_mutex_unlock(&lock);
}
static void sync_holder(void *holder,void *g) {init_holder(holder,g);}
static void sync_request(void *g,const NativeSync *handle) {
    pthread_mutex_lock(&lock);SyncEvent *e=sync_event(g);clear_event(g);
    if(!handle->words[0]&&!handle->words[1]){e->ticket=0;signal_event(g);}
    else {
        if(handle->words[0]!=SYNC_TAG)runtime_fail("codec: non-codec sync point");
        e->ticket=handle->words[1];int found=0;
        for(Decoder *d=decoders;d;d=d->next)for(unsigned i=0;i<8;i++)if(d->slots[i].ticket==e->ticket) {
            found=1;if(d->slots[i].ready)signal_event(g);
        }
        if(!found)runtime_fail("codec: stale sync point");
    }
    pthread_mutex_unlock(&lock);
}
static void signal_slot(const Slot *s) {
    if(s->ticket)for(SyncEvent *e=sync_events;e;e=e->next)if(e->ticket==s->ticket)signal_event(e->guest);
}
static unsigned initialize(void) { pthread_once(&guest_once,guest_init); LOG("video initialized (libvpx %s)",vpx_codec_version_str()); return 0; }
static void finalize(void) { }
/* SDK 2c5520: format table at b1f4e8; dimensions minus one at +8. */
static Preference preference(unsigned w,unsigned h,unsigned format,unsigned mode) {
    dimensions(w,h); if(format!=3 || (mode!=1&&mode!=2)) { LOG("preference format=%u mode=%u",format,mode); runtime_fail("codec: output other than NV12"); }
    Preference p={{(uint64_t)(mode==1?1:4)<<32,(w-1)|((uint64_t)(h-1)<<16),0,0,0,0,0,0}}; return p;
}
static Preference preference_compat(unsigned w,unsigned h,unsigned format,unsigned mode) {
    Preference p=preference(w,h,format,mode);
    if(mode==2)p.words[0]=(uint64_t)7<<32; /* SDK 2c6040 */
    return p;
}
static unsigned pool_size(uint64_t *size,uint64_t *align,const Preference *pref,unsigned count) {
    unsigned w=(pref->words[1]&65535)+1,h=((pref->words[1]>>16)&65535)+1; dimensions(w,h);
    if(!count||count>8)runtime_fail("codec: image pool count");
    *size=(uint64_t)w*h*3/2*count; *align=4096; return 0;
}
static unsigned pool_init(void *g,void *mem,uint64_t size,const Preference *pref,unsigned count,unsigned mode) {
    (void)mode; uint64_t need_size,align; pool_size(&need_size,&align,pref,count);
    if(!mem||size<need_size)runtime_fail("codec: insufficient image pool memory");
    Pool *p=calloc(1,sizeof *p); if(!p)runtime_fail("codec: pool allocation");
    *p=(Pool){.guest=g,.w=(pref->words[1]&65535)+1,.h=((pref->words[1]>>16)&65535)+1,.count=count,.pixels=mem};
    memset(g,0,0x18); pthread_mutex_lock(&lock); p->next=pools;pools=p; pthread_mutex_unlock(&lock); return 0;
}
static void pool_fini(void *g) {
    pthread_mutex_lock(&lock); Pool **at=&pools;while(*at&&(*at)->guest!=g)at=&(*at)->next;
    if(!*at)runtime_fail("codec: finalize unknown pool"); Pool *p=*at;
    /* The game destroys pools after stopping/joining its movie workers, before
     * FinalizeVp9Decoder. Discard registrations without retaining freed pools. */
    for(Decoder *d=decoders;d;d=d->next)for(unsigned i=0;i<8;i++)if(d->slots[i].pool==p) {
        if(d->started&&!d->eos)runtime_fail("codec: pool finalized during active decoding");
        d->slots[i]=(Slot){0};
        for(unsigned j=0;j<d->navailable;) {
            if(d->available[j]==i)memmove(d->available+j,d->available+j+1,(--d->navailable-j)*sizeof(unsigned));
            else j++;
        }
    }
    *at=p->next;free(p);pthread_mutex_unlock(&lock);
}
static unsigned native_image(const void *g,unsigned index) { (void)g;(void)index;runtime_fail("codec: native GPU image requested from CPU NV12 pool"); }
static unsigned read_nv12(const void *g,unsigned index,unsigned x,unsigned y,unsigned w,unsigned h,void *yd,uint64_t yp,
                          uint64_t ys,void *uvd,uint64_t uvp,uint64_t uvs) {
    pthread_mutex_lock(&lock);Pool *p=pool(g);
    if(index>=p->count||!yd||!uvd||((x|y|w|h)&1)||x>p->w||w>p->w-x||y>p->h||h>p->h-y||!w||!h||yp<w||uvp<w||ys<w||uvs<w||
       h-1>(ys-w)/yp||h/2-1>(uvs-w)/uvp)runtime_fail("codec: NV12 copy bounds");
    const uint8_t *src=p->pixels+image_bytes(p)*index;
    dm_guest_write_prepare(yd,(h-1)*yp+w);dm_guest_write_prepare(uvd,(h/2-1)*uvp+w);
    for(unsigned row=0;row<h;row++)memcpy((uint8_t*)yd+row*yp,src+(y+row)*p->w+x,w);
    src+=(size_t)p->w*p->h;
    for(unsigned row=0;row<h/2;row++)memcpy((uint8_t*)uvd+row*uvp,src+(y/2+row)*p->w+x,w);
    pthread_mutex_unlock(&lock);return 0;
}
static unsigned work_size(uint64_t *size,uint64_t *align,uint64_t flags,unsigned w,unsigned h) {
    (void)flags; dimensions(w,h); *size=4096;*align=4096;return 0;
}
static void init_vpx(Decoder *d) {
    vpx_codec_dec_cfg_t cfg={.threads=4,.w=d->w,.h=d->h};
    if(vpx_codec_dec_init(&d->vpx,vpx_codec_vp9_dx(),&cfg,0)!=VPX_CODEC_OK)runtime_fail("codec: VP9 init");
}
static unsigned decoder_init(void *g,void *work,uint64_t worksize,uint64_t flags,unsigned w,unsigned h,unsigned count,unsigned input,uint64_t output) {
    (void)work;(void)worksize;(void)flags;
    output=(unsigned)output; /* guest stores only the low word of its stack slot */
    dimensions(w,h);if(output!=3) {LOG("decoder input=%u output=%llu count=%u",input,(unsigned long long)output,count);runtime_fail("codec: non NV12 decoder");}
    pthread_once(&guest_once,guest_init);Decoder *d=calloc(1,sizeof *d);if(!d)runtime_fail("codec: decoder allocation");
    d->guest=g;d->w=w;d->h=h;pthread_cond_init(&d->changed,NULL);init_vpx(d);
    memset(g,0,0x2a0);if(create_event((uint8_t*)g+8,1,0))runtime_fail("codec: submission event");
    signal_event((uint8_t*)g+8);
    pthread_mutex_lock(&lock);d->next=decoders;decoders=d;pthread_mutex_unlock(&lock);
    LOG("VP9 %p %ux%u input=%u count=%u",g,w,h,input,count);return 0;
}
static unsigned decoder_init_owned(void *g,uint64_t worksize,uint64_t flags,unsigned w,unsigned h,unsigned count,unsigned input,unsigned output) {
    return decoder_init(g,NULL,worksize,flags,w,h,count,input,output);
}
static unsigned register_buffer(void *g,unsigned slot,void *pg,unsigned image) {
    pthread_mutex_lock(&lock);Decoder *d=decoder(g);Pool *p=pool(pg);
    if(slot>=8||image>=p->count||d->slots[slot].registered||d->navailable==8||p->w!=d->w||p->h!=d->h)runtime_fail("codec: register buffer");
    d->slots[slot]=(Slot){.pool=p,.image=image,.registered=1};d->available[d->navailable++]=slot;
    pthread_mutex_unlock(&lock);return 0;
}
static unsigned start(void *g) {pthread_mutex_lock(&lock);Decoder *d=decoder(g);d->started=1;pthread_mutex_unlock(&lock);return 0;}
static void stop(void *g) {pthread_mutex_lock(&lock);Decoder *d=decoder(g);d->started=0;pthread_cond_broadcast(&d->changed);pthread_mutex_unlock(&lock);}
static unsigned flush(void *g) {
    pthread_mutex_lock(&lock);Decoder *d=decoder(g);vpx_codec_destroy(&d->vpx);init_vpx(d);
    for(unsigned i=0;i<8;i++){d->slots[i].ready=0;d->slots[i].reported=0;}
    d->eos=0;d->t0=0;d->write_cursor=d->navailable?d->available[0]:0;
    uint64_t first_ticket=UINT64_MAX;
    for(unsigned i=0;i<8;i++)if(d->slots[i].acquired&&d->slots[i].ticket<first_ticket) {
        first_ticket=d->slots[i].ticket;d->write_cursor=i;
    }
    d->frame_period=0;d->last_input_pts=0;pthread_cond_broadcast(&d->changed);signal_event((uint8_t*)g+8);pthread_mutex_unlock(&lock);return 0;
}
static void decoder_fini(void *g) {
    pthread_mutex_lock(&lock);Decoder **at=&decoders;while(*at&&(*at)->guest!=g)at=&(*at)->next;
    if(!*at)runtime_fail("codec: finalize unknown decoder");Decoder *d=*at;*at=d->next;
    LOG("finalize %p decoded=%llu presented=%llu dropped=%llu",g,(unsigned long long)d->frames,(unsigned long long)d->shown,(unsigned long long)d->dropped);
    vpx_codec_destroy(&d->vpx);pthread_cond_destroy(&d->changed);free(d);pthread_mutex_unlock(&lock);destroy_event((uint8_t*)g+8);
}
static unsigned submission_event(void **out,void *g) {*out=(uint8_t*)g+8;return 0;}
static int writable(Decoder *d) {
    for(unsigned n=0;n<8;n++){unsigned i=(d->write_cursor+n)%8;if(d->slots[i].registered&&!d->slots[i].ready){d->write_cursor=(i+1)%8;return (int)i;}}return -1;
}
static void store_frame(Decoder *d,const vpx_image_t *img,int64_t pts,int eos) {
    int i=writable(d);if(i<0)runtime_fail("codec: no writable slot");Slot *s=&d->slots[i];
    s->pts=pts;s->eos=eos;s->ready=1;s->reported=0;
    if(img) {
        if(img->fmt!=VPX_IMG_FMT_I420||img->d_w!=d->w||img->d_h!=d->h)runtime_fail("codec: unexpected VP9 image");
        uint8_t *dst=s->pool->pixels+image_bytes(s->pool)*s->image;dm_guest_write_prepare(dst,image_bytes(s->pool));
        for(unsigned y=0;y<d->h;y++)memcpy(dst+(size_t)y*d->w,img->planes[0]+(size_t)y*img->stride[0],d->w);
        dst+=(size_t)d->w*d->h;
        for(unsigned y=0;y<d->h/2;y++)for(unsigned x=0;x<d->w/2;x++) {
            dst[(size_t)y*d->w+2*x]=img->planes[1][(size_t)y*img->stride[1]+x];
            dst[(size_t)y*d->w+2*x+1]=img->planes[2][(size_t)y*img->stride[2]+x];
        }
    }
}
static unsigned submit(uint64_t *id,void *g,const void *data,uint64_t size,int64_t pts,int eos) {
    if(size>UINT_MAX)runtime_fail("codec: oversized VP9 packet");
    pthread_mutex_lock(&lock);Decoder *d=decoder(g);unsigned free_count=0;
    for(unsigned i=0;i<8;i++)free_count+=d->slots[i].registered&&!d->slots[i].ready;
    /* As in movie_host.c, keep the picture in time when the renderer is slower
     * than the movie. Replace only a completed image whose properties have not
     * been delivered; its acquired slot and completion generation stay intact.
     * The next input must be due too, and the old frame >1.5 periods late. */
    if(size&&!eos&&d->t0&&d->frame_period>0&&!free_count&&pts>=d->pts0) {
        uint64_t now=now_ns(),next_due=d->t0+(uint64_t)(pts-d->pts0);
        int oldest=-1;
        for(unsigned i=0;i<8;i++) {
            Slot *s=&d->slots[i];
            if(s->ready&&!s->reported&&!s->eos&&(oldest<0||s->pts<d->slots[oldest].pts))oldest=(int)i;
        }
        if(oldest>=0&&now>=next_due) {
            Slot *s=&d->slots[oldest];uint64_t due=d->t0+(s->pts>d->pts0?(uint64_t)(s->pts-d->pts0):0);
            if(now>due&&now-due>(uint64_t)d->frame_period*3/2) {
                s->ready=0;d->write_cursor=(unsigned)oldest;free_count++;d->dropped++;
            }
        }
    }
    if(free_count<(unsigned)((size?1:0)+(eos?1:0))) {clear_event((uint8_t*)g+8);pthread_mutex_unlock(&lock);return 0x1c266f;}
    if(size) {
        if(pts>d->last_input_pts)d->frame_period=pts-d->last_input_pts;
        d->last_input_pts=pts;
        if(vpx_codec_decode(&d->vpx,data,(unsigned)size,NULL,0)!=VPX_CODEC_OK) {LOG("decode: %s",vpx_codec_error(&d->vpx));runtime_fail("codec: VP9 decode");}
        vpx_codec_iter_t it=NULL;const vpx_image_t *img;
        while((img=vpx_codec_get_frame(&d->vpx,&it))) {
            d->frames++;
            /* Some callers feed only one image ahead. Decode overdue packets
             * for their VP9 reference state, but keep their output slot free so
             * input can catch up instead of forcing one stale image per present. */
            if(!eos&&d->t0&&d->frame_period>0&&pts>=d->pts0) {
                uint64_t due=d->t0+(uint64_t)(pts-d->pts0),now=now_ns();
                if(now>due&&now-due>(uint64_t)d->frame_period*3/2){d->dropped++;continue;}
            }
            store_frame(d,img,pts,0);
        }
    }
    if(eos&&!d->eos){store_frame(d,NULL,pts,1);d->eos=1;LOG("EOS %p frames=%llu",g,(unsigned long long)d->frames);}
    for(unsigned i=0;i<8;i++)if(d->slots[i].ready)signal_slot(&d->slots[i]);
    *id=++d->serial;pthread_cond_broadcast(&d->changed);signal_event((uint8_t*)g+8);pthread_mutex_unlock(&lock);return 0;
}
static unsigned acquire(unsigned *index,NativeSync *sync,void *g) {
    pthread_mutex_lock(&lock);Decoder *d=decoder(g);
    if(!d->navailable){pthread_mutex_unlock(&lock);return 0x1c286f;}
    *index=d->available[0];memmove(d->available,d->available+1,--d->navailable*sizeof(unsigned));
    Slot *s=&d->slots[*index];s->acquired=1;s->ticket=++next_ticket;
    *sync=(NativeSync){{SYNC_TAG,s->ticket}};pthread_mutex_unlock(&lock);return 0;
}
static unsigned properties(uint16_t *x,uint16_t *y,uint16_t *w,uint16_t *h,unsigned *color,int64_t *pts,_Bool *eos,void *g,uint64_t index) {
    index=(unsigned)index;
    /* Submission may run on the same virtual core. Never retain it while the
     * host waits for input or for a presentation deadline. Reacquire after the
     * host mutex is released, so a guest holding a core can enter this backend. */
    int away=runtime_core_away();
    pthread_mutex_lock(&lock);Decoder *d=decoder(g);if(index>=8||!d->slots[index].registered)runtime_fail("codec: properties slot");Slot *s=&d->slots[index];
    while(!s->ready)pthread_cond_wait(&d->changed,&lock);
    if(!s->reported&&!s->eos) {
        uint64_t now=now_ns();if(!d->t0){d->t0=now;d->pts0=s->pts;}
        uint64_t due=d->t0+(s->pts>d->pts0?(uint64_t)(s->pts-d->pts0):0);
        while(now<due) {
            struct timespec delay={.tv_sec=(time_t)((due-now)/1000000000),.tv_nsec=(long)((due-now)%1000000000)};
            pthread_mutex_unlock(&lock);nanosleep(&delay,NULL);pthread_mutex_lock(&lock);now=now_ns();
        }
        d->shown++;if(d->shown<=4||d->shown%120==0)LOG("present %p frame=%llu pts_ns=%lld elapsed_ms=%.3f dropped=%llu",g,(unsigned long long)d->shown,(long long)s->pts,(now-d->t0)/1e6,(unsigned long long)d->dropped);
    }
    s->reported=1;*x=0;*y=0;*w=d->w;*h=d->h;*color=0;*pts=s->pts;*eos=s->eos;pthread_mutex_unlock(&lock);
    runtime_core_back(&away);return 0;
}
static void release_buffer(void *g,unsigned index,NativeSync sync) {
    (void)sync;pthread_mutex_lock(&lock);Decoder *d=decoder(g);
    if(index>=8||!d->slots[index].acquired||d->navailable>=8)runtime_fail("codec: release slot");
    d->slots[index].ready=d->slots[index].reported=d->slots[index].acquired=0;
    d->available[d->navailable++]=index;signal_event((uint8_t*)g+8);pthread_cond_broadcast(&d->changed);pthread_mutex_unlock(&lock);
}
static void unsupported(void) {runtime_fail("codec: H264/AV1/VP8 host decoder not implemented");}
void *runtime_video_codec_override(const char *name) {
    static const struct { const char *name; void *fn; } map[]={
        {"_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", sync_create},
        {"_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", sync_destroy},
        {"_ZN2nn6syncpt3sdk21TryWaitSyncPointEventEPKNS1_18SyncPointEventTypeE", sync_try},
        {"_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE", sync_request},
        {"_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", sync_cancel},
        {"_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE", sync_holder},
        {"_ZN2nn5codec5video10InitializeEv", initialize},
        {"_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", native_image},
        {"_ZN2nn5codec5video8FinalizeEv", finalize},
        {"_ZN2nn5codec3sdk34MakeDecodeImagePreferenceForCompatEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", preference_compat},
        {"_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_9CodecModeE", preference},
        {"_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", pool_size},
        {"_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEjNS0_9CodecModeE", pool_init},
        {"_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", pool_fini},
        {"_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", read_nv12},
        {"_ZN2nn5codec30CalculateH264DecoderWorkBufferEPmS1_mtt", unsupported},
        {"_ZN2nn5codec29CalculateAv1DecoderWorkBufferEPmS1_mtt", unsupported},
        {"_ZN2nn5codec29CalculateVp8DecoderWorkBufferEPmS1_mtt", unsupported},
        {"_ZN2nn5codec29CalculateVp9DecoderWorkBufferEPmS1_mtt", work_size},
        {"_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatEPKvm", unsupported},
        {"_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", unsupported},
        {"_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", unsupported},
        {"_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", decoder_init},
        {"_ZN2nn5codec21InitializeH264DecoderEPNS0_15H264DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatEPKvm", unsupported},
        {"_ZN2nn5codec20InitializeAv1DecoderEPNS0_14Av1DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", unsupported},
        {"_ZN2nn5codec20InitializeVp8DecoderEPNS0_14Vp8DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", unsupported},
        {"_ZN2nn5codec20InitializeVp9DecoderEPNS0_14Vp9DecoderTypeEmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatE", decoder_init_owned},
        {"_ZN2nn5codec19FinalizeH264DecoderEPNS0_15H264DecoderTypeE", unsupported},
        {"_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE", unsupported},
        {"_ZN2nn5codec18FinalizeVp8DecoderEPNS0_14Vp8DecoderTypeE", unsupported},
        {"_ZN2nn5codec18FinalizeVp9DecoderEPNS0_14Vp9DecoderTypeE", decoder_fini},
        {"_ZN2nn5codec25RegisterH264DecoderBufferEPNS0_15H264DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", unsupported},
        {"_ZN2nn5codec24RegisterAv1DecoderBufferEPNS0_14Av1DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", unsupported},
        {"_ZN2nn5codec24RegisterVp8DecoderBufferEPNS0_14Vp8DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", unsupported},
        {"_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", register_buffer},
        {"_ZN2nn5codec16StartH264DecoderEPNS0_15H264DecoderTypeE", unsupported},
        {"_ZN2nn5codec15StartAv1DecoderEPNS0_14Av1DecoderTypeE", unsupported},
        {"_ZN2nn5codec15StartVp8DecoderEPNS0_14Vp8DecoderTypeE", unsupported},
        {"_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE", start},
        {"_ZN2nn5codec15StopH264DecoderEPNS0_15H264DecoderTypeE", unsupported},
        {"_ZN2nn5codec14StopAv1DecoderEPNS0_14Av1DecoderTypeE", unsupported},
        {"_ZN2nn5codec14StopVp8DecoderEPNS0_14Vp8DecoderTypeE", unsupported},
        {"_ZN2nn5codec14StopVp9DecoderEPNS0_14Vp9DecoderTypeE", stop},
        {"_ZN2nn5codec16FlushH264DecoderEPNS0_15H264DecoderTypeE", unsupported},
        {"_ZN2nn5codec15FlushAv1DecoderEPNS0_14Av1DecoderTypeE", unsupported},
        {"_ZN2nn5codec15FlushVp8DecoderEPNS0_14Vp8DecoderTypeE", unsupported},
        {"_ZN2nn5codec15FlushVp9DecoderEPNS0_14Vp9DecoderTypeE", flush},
        {"_ZN2nn5codec34GetH264DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_15H264DecoderTypeE", unsupported},
        {"_ZN2nn5codec33GetAv1DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Av1DecoderTypeE", unsupported},
        {"_ZN2nn5codec33GetVp8DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Vp8DecoderTypeE", unsupported},
        {"_ZN2nn5codec33GetVp9DecoderSubmissionReadyEventEPPNS_2os15SystemEventTypeEPNS0_14Vp9DecoderTypeE", submission_event},
        {"_ZN2nn5codec28SubmitH264DecoderInputBufferEPmPNS0_15H264DecoderTypeEPKvmNS_8TimeSpanEb", unsupported},
        {"_ZN2nn5codec27SubmitAv1DecoderInputBufferEPmPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb", unsupported},
        {"_ZN2nn5codec27SubmitVp8DecoderInputBufferEPmPNS0_14Vp8DecoderTypeEPKvmNS_8TimeSpanEb", unsupported},
        {"_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb", submit},
        {"_ZN2nn5codec30AcquireH264DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_15H264DecoderTypeE", unsupported},
        {"_ZN2nn5codec29AcquireAv1DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Av1DecoderTypeE", unsupported},
        {"_ZN2nn5codec29AcquireVp8DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp8DecoderTypeE", unsupported},
        {"_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE", acquire},
        {"_ZN2nn5codec30ReleaseH264DecoderOutputBufferEPNS0_15H264DecoderTypeEjNS_2gs16NativeSyncHandleE", unsupported},
        {"_ZN2nn5codec29ReleaseAv1DecoderOutputBufferEPNS0_14Av1DecoderTypeEjNS_2gs16NativeSyncHandleE", unsupported},
        {"_ZN2nn5codec29ReleaseVp8DecoderOutputBufferEPNS0_14Vp8DecoderTypeEjNS_2gs16NativeSyncHandleE", unsupported},
        {"_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE", release_buffer},
        {"_ZN2nn5codec36GetH264DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_5movie10ColorSpaceEPNS_8TimeSpanEPbPNS0_15H264DecoderTypeEj", unsupported},
        {"_ZN2nn5codec35GetAv1DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_5movie10ColorSpaceEPNS_8TimeSpanEPbPNS0_14Av1DecoderTypeEj", unsupported},
        {"_ZN2nn5codec35GetVp8DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_5movie10ColorSpaceEPNS_8TimeSpanEPbPNS0_14Vp8DecoderTypeEj", unsupported},
        {"_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_5movie10ColorSpaceEPNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj", properties},
    };
    for(size_t i=0;i<sizeof map/sizeof *map;i++)if(!strcmp(name,map[i].name))return map[i].fn;
    return NULL;
}
