/* Independent golden metadata: original SDK CPU queries for uncompressed and
 * BC3-sRGB boundary cases. No game bytes are included. The old rounded-up
 * heuristic mismatches 16 of these 40 cases, including the 160x160 shrine pad. */
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
typedef struct { unsigned format,width,height,levels,h; uint64_t size; } Case;
static const Case cases[] = {
    {0x25,40,1,6,0,4608},
    {0x25,40,8,6,0,4608},
    {0x25,40,9,6,0,6144},
    {0x25,40,10,6,0,6144},
    {0x25,40,11,6,1,6144},
    {0x25,40,16,6,1,6144},
    {0x25,40,17,6,1,9216},
    {0x25,40,21,6,1,10240},
    {0x25,40,22,6,2,10240},
    {0x25,40,32,6,2,10240},
    {0x25,40,33,6,2,16384},
    {0x25,40,40,6,2,18944},
    {0x25,40,42,6,2,18944},
    {0x25,40,43,6,3,18944},
    {0x25,40,64,7,3,19456},
    {0x25,40,65,7,3,31744},
    {0x25,40,85,7,3,37376},
    {0x25,40,86,7,4,37376},
    {0x25,40,128,8,4,37888},
    {0x25,40,129,8,4,62464},
    {0x48,160,4,8,0,12288},
    {0x48,160,32,8,0,12288},
    {0x48,160,36,8,0,17408},
    {0x48,160,40,8,0,17408},
    {0x48,160,44,8,1,17408},
    {0x48,160,64,8,1,17408},
    {0x48,160,68,8,1,30208},
    {0x48,160,84,8,1,30208},
    {0x48,160,88,8,2,30208},
    {0x48,160,128,8,2,30208},
    {0x48,160,132,8,2,57344},
    {0x48,160,160,8,2,57344},
    {0x48,160,168,8,2,57344},
    {0x48,160,172,8,3,57344},
    {0x48,160,256,9,3,57856},
    {0x48,160,260,9,3,112128},
    {0x48,160,340,9,3,113152},
    {0x48,160,344,9,4,113152},
    {0x48,160,512,10,4,113664},
    {0x48,160,516,10,4,222208},
};
static void check(int ok,const char *message) { if(!ok){fprintf(stderr,"FAIL: %s\n",message);exit(1);} }
/* Values the original NVN driver gives (digitalhivehub ran TOTK's driver under emulation, nxnative PR #11 t_tex section G):
 * cube array depth counts layer-faces, packaged byte 0 = block height (low nibble) and block depth (high nibble),
 * linear sizes count pixel rows also for BC. pkg is byte 0 + 1 (0 = automatic), as nvn_texdesc stores it. */
typedef struct { const char *what; int target,format,flags; unsigned w,h,depth,levels,stride; int pkg; uint64_t size,layer,off1; unsigned align,th,td; } Driver;
static const Driver driver[] = {
    {"cube array RGBA8 4x4 depth 6 (TOTK)",        9,0x25,0,    4,4,6,1,0,0,        3072,512,0,512,0,0},
    {"cube array RGBA8 64x64 depth 12, 7 levels",  9,0x25,0,    64,64,12,7,0,0,     294912,24576,16384,512,3,0},
    {"minimal RG8 260x260, 9 levels (TOTK)",       1,0x0d,0x200,260,260,1,9,0,0,    214528,214528,152064,512,0,0},
    {"minimal RGB565 320x320 (TOTK)",              1,0x3c,0x200,320,320,1,1,0,0,    204800,204800,0,512,0,0},
    {"minimal RGBA8 64x128 (keeps automatic)",     1,0x25,0x200,64,128,1,1,0,0,     32768,32768,0,512,4,0},
    {"minimal R8 8x1920, 11 levels",               1,0x01,0x200,8,1920,1,11,0,0,    246784,246784,122880,512,0,0},
    {"minimal 3D RGBA8 64x64x8",                   2,0x25,0x200,64,64,8,1,0,0,      131072,131072,0,512,0,3},
    {"minimal 3D RGBA8 64x64x9",                   2,0x25,0x200,64,64,9,1,0,0,      147456,147456,0,512,0,0},
    {"packaged 3D BC6H 64^3, 7 levels, 0x40 (TOTK)",2,0x49,0,   64,64,64,7,0,0x41,  163328,163328,131072,512,0,4},
    {"packaged 3D RGBA8 32^3, 0x00",               2,0x25,0,    32,32,32,1,0,0x01,  131072,131072,0,512,0,0},
    {"packaged 3D RGBA8 32^3, 0x12",               2,0x25,0,    32,32,32,1,0,0x13,  131072,131072,0,512,2,1},
    {"packaged 2D RGBA8 64x64, 0x13",              1,0x25,0,    64,64,1,1,0,0x14,   16384,16384,0,512,3,0},
    {"linear RT RGBA8 260x260 stride 1152 (TOTK)", 1,0x25,0x40, 260,260,1,1,1152,0, 299520,299520,0,128,0,0},
    {"linear RT RGBA8 1x1 stride 128 (TOTK)",      1,0x25,0x40, 1,1,1,1,128,0,      128,128,0,128,0,0},
    {"linear BC1 64x64 stride 128",                1,0x42,0x10, 64,64,1,1,128,0,    8192,8192,0,32,0,0},
};
int main(void) {
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        const Case *c=&cases[i];
        nvn_texdesc d={.target=1,.format=(int)c->format,.samples=1,.width=c->width,.height=c->height,.depth=1,.levels=c->levels};
        nvn_texlayout l;nvn_tex_layout(&d,&l);
        if(l.tileH!=c->h || l.size!=c->size){fprintf(stderr,"case %u format=%#x %ux%u: H=%u expected=%u size=%llu expected=%llu\n",i,c->format,c->width,c->height,l.tileH,c->h,(unsigned long long)l.size,(unsigned long long)c->size);return 1;}
    }
    nvn_texdesc pad={.target=1,.format=0x48,.samples=1,.width=160,.height=160,.depth=1,.levels=8};
    unsigned h,d;nvn_tex_level_tiles(&pad,0,&h,&d);
    check(h==2 && d==0,"160x160 BC3 base mip uses four-GOB blocks");
    check(nvn_bl_offset(320,0,0,640,40,h,d)==10240,"right-half material block comes from the correct GOB column");
    check(nvn_bl_offset(624,39,0,640,40,h,d)==39408,"last real BC3 block precedes padding");
    pad.pkg_block_h=4;nvn_tex_level_tiles(&pad,0,&h,&d);
    check(h==3,"explicit packaged H3 overrides automatic H2");
    check(nvn_bl_offset(320,0,0,640,40,h,d)==20480,"explicit layout preserves its own addressing");
    for(unsigned i=0;i<sizeof(driver)/sizeof(driver[0]);i++) {
        const Driver *c=&driver[i];
        nvn_texdesc d={.target=c->target,.format=c->format,.flags=c->flags,.width=c->w,.height=c->h,.depth=c->depth,.levels=c->levels,.stride=c->stride,.pkg_block_h=c->pkg};
        nvn_texlayout l;nvn_tex_layout(&d,&l);
        uint64_t off1=c->levels>1?nvn_tex_level_offset(&d,1):0;
        if(l.size!=c->size||l.layer_size!=c->layer||off1!=c->off1||l.align!=c->align||l.tileH!=c->th||l.tileD!=c->td){
            fprintf(stderr,"driver case %s: size %llu layer %llu level1 %llu align %u block %u/%u, driver %llu %llu %llu %u %u/%u\n",c->what,
                    (unsigned long long)l.size,(unsigned long long)l.layer_size,(unsigned long long)off1,l.align,l.tileH,l.tileD,
                    (unsigned long long)c->size,(unsigned long long)c->layer,(unsigned long long)c->off1,c->align,c->th,c->td);return 1;}
    }
    puts("PASS: 40 original-SDK layout goldens, 15 original-driver shapes, shrine BC3 block addresses, explicit-layout override.");
    return 0;
}
