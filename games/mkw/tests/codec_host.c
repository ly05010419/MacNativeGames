/* Standalone host API integration test; no game data. Build from games/mkw:
 * xcrun clang -std=c11 -Wall -Wextra -Werror -Iruntime \
 * -I/opt/homebrew/opt/libvpx/include tests/codec_host.c \
 * /opt/homebrew/opt/libvpx/lib/libvpx.a -o build/codec-test
 */
#include <assert.h>
#include <vpx/vp8cx.h>
#include <vpx/vpx_encoder.h>
#include "../runtime/codec_host.c"
static unsigned event_create(void *p,int mode,int ipc) {assert((mode==0||mode==1)&&!ipc);memset(p,0,0x30);return 0;}
static void event_noop(void *p) {(void)p;}
static void event_signal(void *p) {*(unsigned char*)p=1;}
static void event_clear(void *p) {*(unsigned char*)p=0;}
static _Bool event_try(void *p) {return *(unsigned char*)p!=0;}
void *runtime_symbol(const char *name) {
    if(strstr(name,"CreateSystemEvent"))return event_create;
    if(strstr(name,"SignalSystemEvent"))return event_signal;
    if(strstr(name,"ClearSystemEvent"))return event_clear;
    if(strstr(name,"TryWaitSystemEvent"))return event_try;
    return event_noop;
}
_Noreturn void runtime_fail(const char *message) {fprintf(stderr,"FAIL %s\n",message);abort();}
void dm_guest_write_prepare(void *p,size_t n) {(void)p;(void)n;}
int runtime_core_away(void) {return 0;}
void runtime_core_back(int *away) {(void)away;}
static void packet(vpx_codec_ctx_t *enc,unsigned frame,void *d,int64_t pts,int eos) {
    vpx_image_t image;assert(vpx_img_alloc(&image,VPX_IMG_FMT_I420,32,24,1));
    for(unsigned y=0;y<24;y++)memset(image.planes[0]+y*image.stride[0],64+frame,32);
    for(unsigned y=0;y<12;y++){memset(image.planes[1]+y*image.stride[1],96,16);memset(image.planes[2]+y*image.stride[2],160,16);}
    assert(vpx_codec_encode(enc,&image,frame,1,VPX_EFLAG_FORCE_KF,VPX_DL_REALTIME)==VPX_CODEC_OK);
    vpx_codec_iter_t it=NULL;const vpx_codec_cx_pkt_t *p;unsigned found=0;
    while((p=vpx_codec_get_cx_data(enc,&it)))if(p->kind==VPX_CODEC_CX_FRAME_PKT) {
        uint64_t id=0;assert(submit(&id,d,p->data.frame.buf,p->data.frame.sz,pts,eos)==0);assert(id);found++;
    }
    assert(found==1);vpx_img_free(&image);
}
typedef struct { vpx_codec_ctx_t *enc; void *decoder; } Producer;
static void *produce(void *arg) {
    Producer *p=arg;struct timespec delay={.tv_nsec=20000000};nanosleep(&delay,NULL);
    packet(p->enc,3,p->decoder,0,0);return NULL;
}
int main(void) {
    unsigned char guest[0x2a0],pg[0x18];Preference pref=preference(32,24,3,1);
    uint64_t size,align;assert(pool_size(&size,&align,&pref,4)==0);void *pixels=malloc(size);assert(pixels);
    assert(pool_init(pg,pixels,size,&pref,4,1)==0);assert(decoder_init(guest,NULL,4096,0,32,24,4,1,UINT64_C(0xfeedbeef00000003))==0);
    for(unsigned i=0;i<4;i++)assert(register_buffer(guest,i,pg,i)==0);
    unsigned index[4],unused;NativeSync sync;
    NativeSync first={0};
    for(unsigned i=0;i<4;i++){assert(acquire(&index[i],&sync,guest)==0);assert(index[i]==i&&sync.words[0]==SYNC_TAG&&sync.words[1]);if(!i)first=sync;}
    unsigned char completion[0x80];assert(sync_create(completion)==0);sync_request(completion,&first);assert(!sync_try(completion));
    assert(acquire(&unused,&sync,guest)==0x1c286f);assert(start(guest)==0);
    vpx_codec_ctx_t enc;vpx_codec_enc_cfg_t cfg;assert(vpx_codec_enc_config_default(vpx_codec_vp9_cx(),&cfg,0)==VPX_CODEC_OK);
    cfg.g_w=32;cfg.g_h=24;cfg.g_timebase.num=1;cfg.g_timebase.den=25;cfg.g_lag_in_frames=0;
    assert(vpx_codec_enc_init(&enc,vpx_codec_vp9_cx(),&cfg,0)==VPX_CODEC_OK);assert(vpx_codec_control(&enc,VP9E_SET_LOSSLESS,1)==VPX_CODEC_OK);
    packet(&enc,0,guest,1000000000,0);assert(sync_try(completion));sync_cancel(completion);assert(!sync_try(completion));sync_destroy(completion);
    packet(&enc,1,guest,1040000000,0);packet(&enc,2,guest,1080000000,1);
    uint64_t id;assert(submit(&id,guest,NULL,0,0,1)==0x1c266f);uint64_t begin=now_ns();
    for(unsigned i=0;i<4;i++) {
        uint16_t x,y,w,h;unsigned color;int64_t pts;_Bool eos;
        assert(properties(&x,&y,&w,&h,&color,&pts,&eos,guest,UINT64_C(0xfeedbeef00000000)|index[i])==0);assert(!x&&!y&&w==32&&h==24);assert(eos==(i==3));
        if(i<3) {
            assert(pts==1000000000+(int64_t)i*40000000);
            unsigned char yd[64],uv[32];memset(yd,0xcc,sizeof yd);memset(uv,0xcc,sizeof uv);
            assert(read_nv12(pg,index[i],2,4,6,8,yd,8,sizeof yd,uv,8,sizeof uv)==0);
            for(unsigned r=0;r<8;r++){for(unsigned c=0;c<6;c++)assert(yd[r*8+c]==64+i);assert(yd[r*8+6]==0xcc);}
            for(unsigned r=0;r<4;r++){for(unsigned c=0;c<6;c++)assert(uv[r*8+c]==(c%2?160:96));assert(uv[r*8+6]==0xcc);}
        }
        release_buffer(guest,index[i],sync);
    }
    assert(now_ns()-begin>=80000000);assert(flush(guest)==0);assert(acquire(&unused,&sync,guest)==0);
    Producer producer={&enc,guest};pthread_t worker;assert(!pthread_create(&worker,NULL,produce,&producer));
    begin=now_ns();
    uint16_t x,y,w,h;unsigned color;int64_t pts;_Bool eos;
    assert(properties(&x,&y,&w,&h,&color,&pts,&eos,guest,unused)==0);assert(pts==0&&!eos);
    assert(now_ns()-begin>=20000000);assert(!pthread_join(worker,NULL));
    release_buffer(guest,unused,sync);stop(guest);pool_fini(pg);decoder_fini(guest);free(pixels);
    assert(pool_size(&size,&align,&pref,2)==0);pixels=malloc(size);assert(pixels);
    assert(pool_init(pg,pixels,size,&pref,2,1)==0);assert(decoder_init_owned(guest,4096,0,32,24,25,3,3)==0);
    assert(register_buffer(guest,0,pg,0)==0);assert(register_buffer(guest,1,pg,1)==0);assert(start(guest)==0);
    assert(acquire(&index[0],&sync,guest)==0);assert(acquire(&index[1],&sync,guest)==0);
    packet(&enc,4,guest,0,0);assert(properties(&x,&y,&w,&h,&color,&pts,&eos,guest,0)==0);
    packet(&enc,5,guest,20000000,0);struct timespec delay={.tv_nsec=80000000};nanosleep(&delay,NULL);
    packet(&enc,6,guest,40000000,0); /* discard overdue output while retaining VP9 references */
    packet(&enc,7,guest,85000000,0);
    assert(properties(&x,&y,&w,&h,&color,&pts,&eos,guest,1)==0);assert(pts==85000000);
    unsigned char yd[32*24],uv[32*12];assert(read_nv12(pg,1,0,0,32,24,yd,32,sizeof yd,uv,32,sizeof uv)==0);assert(yd[0]==71);
    release_buffer(guest,1,sync);release_buffer(guest,0,sync);assert(flush(guest)==0);
    packet(&enc,8,guest,0,0);assert(acquire(&unused,&sync,guest)==0);assert(unused==1);
    assert(properties(&x,&y,&w,&h,&color,&pts,&eos,guest,unused)==0);assert(pts==0);
    release_buffer(guest,unused,sync);stop(guest);pool_fini(pg);decoder_fini(guest);free(pixels);vpx_codec_destroy(&enc);
    puts("PASS: VP9/NV12, slots/backpressure, completion events, pacing/late drop, EOS, flush, threads and teardown");
    return 0;
}
