#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
/* Shared by the engine parameter and its settings UI. Metal's 2D texture
   extent is at most 16384 on the Apple GPU families this backend supports. */
static inline int ns2_parse_resolution(const char *s,unsigned *w,unsigned *h) {
    if(!s)return 0;
    unsigned v[2]={0,0};
    for(unsigned k=0;k<2;k++) {
        if(*s<'0'||*s>'9')return 0;
        while(*s>='0'&&*s<='9') {
            v[k]=v[k]*10+(unsigned)(*s++-'0');
            if(v[k]>16384)return 0;
        }
        if(!v[k])return 0;
        if(k==0) { if(*s++!='x')return 0; }
        else if(*s)return 0;
    }
    *w=v[0];*h=v[1];return 1;
}

/* One-word settings files end in a newline; environment/UI input stays strict. */
static inline int ns2_parse_saved_resolution(const char *s,unsigned *w,unsigned *h) {
    if(!s)return 0;
    size_t n=strlen(s);while(n && (s[n-1]=='\n'||s[n-1]=='\r'))n--;
    if(n>=32)return 0;
    char word[32];memcpy(word,s,n);word[n]=0;
    return ns2_parse_resolution(word,w,h);
}

/* Compare rounded pixels before fitting: float 32/9 is slightly above its
   exact value, but a 3840x1080 canvas still fits the 3840-pixel capacity. */
static inline int ns2_aspect_size(float ratio,unsigned height,unsigned max_w,unsigned max_h,unsigned *w,unsigned *h) {
    if(!(ratio>=0.5f&&ratio<=8)||!height||!max_w||!max_h||max_w>16384||max_h>16384)return 0;
    if(height>max_h)height=max_h;
    unsigned width=(unsigned)(height*(double)ratio+0.5);
    if(width>max_w){
        height=(unsigned)((max_w+0.5)/(double)ratio);
        width=(unsigned)(height*(double)ratio+0.5);
        if(width>max_w&&height){height--;width=(unsigned)(height*(double)ratio+0.5);}
    }
    if(!width||!height)return 0;
    *w=width;*h=height;return 1;
}

/* main+0x5399ec copies 0x108 bytes starting AFTER the 8-byte vtable
   of gsys's 0x110-byte creation argument. Keep its flags and tail intact. */
typedef struct {
    uint32_t flags;
    struct { uint32_t w,h; } extent[24];
    struct { float x,y; } scale[6];
    uint8_t tail[20];
} bananza_gsys_sizes;
_Static_assert(sizeof(bananza_gsys_sizes)==0x108,"Bananza gsys copy size");
_Static_assert(offsetof(bananza_gsys_sizes,extent)==4,"Bananza extents after copied flags");
_Static_assert(offsetof(bananza_gsys_sizes,scale)==0xc4,"Bananza copied scale offset");
static inline int bananza_set_gsys_sizes(void *buffer,size_t n,unsigned w,unsigned h) {
    if(n!=sizeof(bananza_gsys_sizes))return 0;
    bananza_gsys_sizes *p=buffer;
    if(p->extent[0].w!=w||p->extent[0].h!=h)return 0;
    for(unsigned k=0;k<24;k++){p->extent[k].w=w;p->extent[k].h=h;}
    for(unsigned k=0;k<6;k++){p->scale[k].x=1;p->scale[k].y=1;}
    return 1;
}

/* The 0x11d-byte graphics creation payload omits its vtable. At +0x20
   is the DynamicTextureAllocator's primary pool (full argument +0x28),
   initialized to 200 MiB by main+0x53995c/64. The native constructor
   main+0x71566c -> +0x717b3c forwards it to the allocator; crash-state
   verification found manager+0x2a70 = 0xc800000 and a null fallback heap.
   The adjacent 32 MiB field is the fallback block size, not the primary pool.
   Keep the native 1080p primary budget per pixel, rounded to 64 KiB.
   No texture descriptors, fallback parameters or effect switches change. */
static inline uint64_t bananza_render_pool_bytes(unsigned w,unsigned h) {
    const uint64_t native=200ULL<<20, pixels=(uint64_t)w*h, base=1920ULL*1080;
    if(pixels<=base)return native;
    return ((native*pixels+base-1)/base+65535)&~65535ULL;
}
static inline int bananza_set_render_pool(void *buffer,size_t n,unsigned w,unsigned h) {
    if(n!=0x11d)return 0;
    uint64_t pool;
    memcpy(&pool,(uint8_t *)buffer+0x20,sizeof pool);
    if(pool!=(200ULL<<20))return 0;
    pool=bananza_render_pool_bytes(w,h);
    memcpy((uint8_t *)buffer+0x20,&pool,sizeof pool);
    return 1;
}
