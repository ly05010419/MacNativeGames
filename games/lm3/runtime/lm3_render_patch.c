#include "lm3_render_patch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LM3_RENDER_PATCH_TEST
#include "game_version.h"
#endif

/* 1.4.0 only: original main build 79E5950FFA85ACF6. The engine chooses a
   docked/handheld size at startup and on its operation-mode notification. Its
   own callback writes renderer+c128/c12c, then 45d5a0..45d5c4 rebuilds the
   graphics configuration/crop via 7c1890, 7ccf60 and b7e3d0. The framework's
   first initialization (78c2xx) and default configuration (78e1xx) must agree
   before it asks NVN for texture storage. The window factory reads the latter
   config's +74/+7c into all three colour descriptors and their shared format
   descriptor (78d80c/824, 78d980/98c). Its original 7cb150 -> 7cb3f0 -> b81ef0
   path queries storage size, allocates that size/alignment and initializes
   each NVN texture; no existing 1080p suballocation is enlarged in place.
   Original window-resource resize exclusions, recursive rebuild ordering and
   GPU waits remain unchanged. Runtime size changes are not supported.

   DRS ends by clamping to [5/6, 1] at 45df48..45df54 and writing 17cec88;
   replace only its result with 1. The shared 5/6 and 1080 float constants also
   have unrelated users and stay unmodified. All original instruction guards,
   build identity and the code-cave zeros are checked before any write. */
static const uint8_t lm3_v140_build_id[32] = {
    0x79,0xe5,0x95,0x0f,0xfa,0x85,0xac,0xf6,
    0x3e,0x28,0xc9,0xae,0xc0,0x51,0xea,0xc2,
    0x7d,0x6f,0x7f,0x8d,0,0,0,0,0,0,0,0,0,0,0,0
};
enum { LM3_RENDER_STUB = 0x1045fa0, LM3_RENDER_STUB_BYTES = 12 };
typedef enum {
    WIDTH32, HEIGHT32, WIDTH64_PAIR, HEIGHT64_LOW, HEIGHT64_HIGH,
    HEIGHT_FROM_W12, HEIGHT_FROM_W9, NOP, SCALE_ONE, ONE_VARIANT,
    GPU_POOL_BUDGET, APP_HEAP_BUDGET, APP_HEAP_AVAILABLE, GUARD_ONLY
} RenderValue;
typedef struct { uint32_t offset, original; RenderValue value; unsigned reg; } RenderSite;
static const RenderSite lm3_render_sites[] = {
    /* Startup renderer and operation-mode callback: both modes use the user's
       explicit startup size; changing handheld mode cannot silently undo it. */
    {0x458564,0x5280a00a,WIDTH32,10},
    {0x458568,0x32190fe8,WIDTH32,8},
    {0x45856c,0x52808709,HEIGHT32,9},
    {0x458570,0x52805a0c,HEIGHT32,12},
    {0xb7a91c,0x32190fe9,WIDTH32,9},
    {0xb7a920,0x5280a00a,WIDTH32,10},
    {0xb7a924,0x5280870b,HEIGHT32,11},
    {0xb7a928,0x52805a0c,HEIGHT32,12},
    /* First graphics configuration: retain the aspect calculation, replacing
       the shared 1080 float loads by a conversion of the selected height. */
    {0x78c2a0,0x5280870c,HEIGHT32,12},
    {0x78c2a8,0xb00061ec,NOP,0},
    {0x78c2ac,0xbd44d981,HEIGHT_FROM_W12,0},
    {0x78c304,0x52808709,HEIGHT32,9},
    {0x78c30c,0xb00061e9,NOP,0},
    {0x78c310,0xbd44d921,HEIGHT_FROM_W9,0},
    {0x78c328,0x32190fe8,WIDTH32,8},
    /* 1440p consumes more of the fixed GPU pool [2], including the floor
       texture allocator's fixed 0xd700200-byte block. config+30 flows through
       78c4d4 -> stack+58 -> b7ed10 -> poolarray[2]; b7ed10 allocates exactly
       that capacity from the application heap and exposes it to NVN. Grow
       this pool and the fixed nninitStartup heap by the same 512 MiB. */
    {0x78c374,0x52940009,GUARD_ONLY,0},
    {0x78c378,0x72a7e749,GPU_POOL_BUDGET,0},
    {0x78c37c,0xa902a728,GUARD_ONLY,0},
    {0x78c4d4,0xa942ab29,GUARD_ONLY,0},
    {0x78c500,0xa9052be0,GUARD_ONLY,0},
    {0x78c528,0x940fc9fa,GUARD_ONLY,0},
    {0x78c534,0xf9000914,GUARD_ONLY,0},
    {0xb5f698,0x52b77c14,APP_HEAP_BUDGET,0},
    {0xb5f6d8,0xf117791f,APP_HEAP_AVAILABLE,0},
    {0xb5f6e4,0xaa1403e0,GUARD_ONLY,0},
    {0xb5f6e8,0x94139566,GUARD_ONLY,0},
    {0xb5f6f0,0xaa1403e1,GUARD_ONLY,0},
    {0xb5f6f4,0x94139567,GUARD_ONLY,0},
    /* Actual default configuration used by this game's window factory.
       x8 = main+1bd2f10 = default config+50. Set both repeated width/height
       pairs before the three output resources request their own storage. */
    {0x78e168,0x9000a228,GUARD_ONLY,0},
    {0x78e16c,0x913c4108,GUARD_ONLY,0},
    {0x78e174,0xb2190fea,WIDTH64_PAIR,10},
    {0x78e178,0xd280870b,HEIGHT64_LOW,11},
    {0x78e184,0xf802410a,GUARD_ONLY,0},
    {0x78e188,0xf2c0870b,HEIGHT64_HIGH,11},
    {0x78e18c,0xf801c10a,GUARD_ONLY,0},
    {0x78e1a8,0xf802c10b,GUARD_ONLY,0},
    {0x78e1ac,0xf801410b,GUARD_ONLY,0},
    {0x45df48,0x1e2d6920,SCALE_ONE,0},
    {0x45df4c,0x1e2e1001,GUARD_ONLY,0},
    {0x45df50,0x1e217800,GUARD_ONLY,0},
    {0x45df54,0xbd0c8900,GUARD_ONLY,0},
    /* With scene scale fixed at one, retain only the full-size descriptor.
       b81af0 normally fills a fixed-capacity array with every integer DRS
       size down to 5/6. A later larger canvas would overrun the array that
       was allocated for the initial size. Its normal full-size creation,
       count, cleanup and texture-pool registration remain intact. */
    {0xb81c40,0xd63f0100,GUARD_ONLY,0},
    {0xb81c44,0x6b1702bf,ONE_VARIANT,0},
    {0xb81ce4,0x6d4823e9,GUARD_ONLY,0},
    /* The unmodified output factory consumes this one config for all three
       images, before their storage-size query/allocation. Guard the consumers
       and calls as well as the config initializer; none are rewritten. */
    {0x78d80c,0x7940eb28,GUARD_ONLY,0},
    {0x78d824,0xb9407f29,GUARD_ONLY,0},
    {0x78d980,0xb9407729,GUARD_ONLY,0},
    {0x78d98c,0xb9407f29,GUARD_ONLY,0},
    {0x78da00,0x940fca48,GUARD_ONLY,0},
    {0x78da44,0x9400f5c3,GUARD_ONLY,0},
    {0x78da64,0x9400f5bb,GUARD_ONLY,0},
    {0x78da7c,0x9400f5b5,GUARD_ONLY,0},
    /* Establish that this is the expected size propagation/crop path too. */
    {0x458610,0x52982508,GUARD_ONLY,0},
    {0x458614,0xb8286840,GUARD_ONLY,0},
    {0x458618,0x52982588,GUARD_ONLY,0},
    {0x45861c,0xb8286841,GUARD_ONLY,0}
};

static int render_error(char *error, size_t n, const char *message, unsigned at) {
    if (error && n) {
        if (at) snprintf(error,n,"%s at main+0x%x",message,at);
        else snprintf(error,n,"%s",message);
    }
    return 0;
}
static uint32_t render_word(const uint8_t *image, unsigned offset) {
    uint32_t word; memcpy(&word,image+offset,sizeof word); return word;
}
static void render_put(uint8_t *image, unsigned offset, uint32_t word) {
    memcpy(image+offset,&word,sizeof word);
}
static uint32_t render_branch(unsigned from, unsigned to) {
    return 0x14000000u | ((uint32_t)(((int64_t)to-from)/4) & 0x03ffffffu);
}
static int render_size_valid(unsigned w, unsigned h) {
    return (w==1280&&h==720)||(w==1600&&h==900)||
           (w==1920&&h==1080)||(w==2560&&h==1440);
}
uint64_t lm3_render_extra_bytes_for_size(unsigned w, unsigned h) {
    return w==2560 && h==1440 ? 0x20000000ULL : 0;
}

int lm3_render_apply_image(uint8_t *image, size_t image_size,
                           const uint8_t build_id[32], unsigned width,
                           unsigned height, char *error, size_t error_size) {
    if (!image || !build_id || memcmp(build_id,lm3_v140_build_id,32))
        return render_error(error,error_size,"LM3 render patch requires main 1.4.0 build 79E5950FFA85ACF6",0);
    if (!render_size_valid(width,height))
        return render_error(error,error_size,"unsupported LM3 render size",0);
    uint64_t extra=lm3_render_extra_bytes_for_size(width,height);
    if (image_size<LM3_RENDER_STUB+LM3_RENDER_STUB_BYTES)
        return render_error(error,error_size,"LM3 render image too small",0);
    for (size_t i=0;i<sizeof lm3_render_sites/sizeof *lm3_render_sites;i++) {
        const RenderSite *s=&lm3_render_sites[i];
        if (s->offset>image_size-4 || render_word(image,s->offset)!=s->original)
            return render_error(error,error_size,"LM3 render instruction differs",s->offset);
    }
    for (unsigned i=0;i<LM3_RENDER_STUB_BYTES;i++)
        if (image[LM3_RENDER_STUB+i])
            return render_error(error,error_size,"LM3 render text padding already in use",LM3_RENDER_STUB+i);
    /* No writes precede the complete validation above. The stub uses only x10,
       exactly the register defined by the replaced instruction; no scratch
       register, flags, stack or x18 is touched. */
    render_put(image,LM3_RENDER_STUB,0xd280000au|(width<<5));
    render_put(image,LM3_RENDER_STUB+4,0xf2c0000au|(width<<5));
    render_put(image,LM3_RENDER_STUB+8,render_branch(LM3_RENDER_STUB+8,0x78e178));
    for (size_t i=0;i<sizeof lm3_render_sites/sizeof *lm3_render_sites;i++) {
        const RenderSite *s=&lm3_render_sites[i]; uint32_t word=s->original;
        switch (s->value) {
            case WIDTH32: word=0x52800000u|(width<<5)|s->reg; break;
            case HEIGHT32: word=0x52800000u|(height<<5)|s->reg; break;
            case WIDTH64_PAIR: word=render_branch(s->offset,LM3_RENDER_STUB); break;
            case HEIGHT64_LOW: word=0xd2800000u|(height<<5)|s->reg; break;
            case HEIGHT64_HIGH: word=0xf2c00000u|(height<<5)|s->reg; break;
            case HEIGHT_FROM_W12: word=0x1e230181u; break; /* ucvtf s1,w12 */
            case HEIGHT_FROM_W9: word=0x1e230121u; break;  /* ucvtf s1,w9 */
            case NOP: word=0xd503201fu; break;
            case SCALE_ONE: word=0x1e2e1000u; break;       /* fmov s0,#1 */
            case ONE_VARIANT: word=render_branch(s->offset,0xb81ce4); break;
            case GPU_POOL_BUDGET:
                if (extra) word=0x72a00009u|((unsigned)((0x3f3aa000ULL+extra)>>16)<<5);
                break;
            case APP_HEAP_BUDGET:
                if (extra) word=0x52a00014u|((unsigned)((0xbbe00000ULL+extra)>>16)<<5);
                break;
            case APP_HEAP_AVAILABLE:
                /* QueryMemoryInfo's available size is compared in 2 MiB
                   units. The original strict-greater check needs heap/2MiB-1. */
                if (extra) word=0xf100011fu|((unsigned)(((0xbbe00000ULL+extra)>>21)-1)<<10);
                break;
            case GUARD_ONLY: continue;
        }
        render_put(image,s->offset,word);
    }
    if (error && error_size) *error=0;
    return 1;
}

#ifndef LM3_RENDER_PATCH_TEST
uint64_t runtime_lm3_render_extra_bytes(void) {
    if (strcmp(TOTK_GAME_DIR,"v140")) return 0;
    const char *setting=getenv("LM3_RENDER_SIZE");
    if (setting && !strcmp(setting,"0")) return 0;
    unsigned w=2560,h=1440; char tail;
    if (setting && *setting && sscanf(setting,"%ux%u%c",&w,&h,&tail)!=2)
        runtime_fail("LM3_RENDER_SIZE must be 1280x720, 1600x900, 1920x1080, 2560x1440, or 0");
    if (!render_size_valid(w,h)) runtime_fail("unsupported LM3 render size for memory reservation");
    return lm3_render_extra_bytes_for_size(w,h);
}
void runtime_lm3_render_patch(Module *m) {
    const char *setting=getenv("LM3_RENDER_SIZE");
    if (setting && !strcmp(setting,"0")) {
        fprintf(stderr,"[lm3-render] original game dimensions and dynamic resolution retained\n");
        return;
    }
    if (strcmp(TOTK_GAME_DIR,"v140")) {
        if (setting && *setting) runtime_fail("LM3_RENDER_SIZE requires the verified 1.4.0 input");
        fprintf(stderr,"[lm3-render] no render patch for this game version\n");
        return;
    }
    if (!m || strcmp(m->name,"main") || m->text_end!=0x1045f90 || m->ro_off!=0x1046000)
        runtime_fail("LM3 render patch: unexpected main layout");
    unsigned w=2560,h=1440; char tail;
    if (setting && *setting && sscanf(setting,"%ux%u%c",&w,&h,&tail)!=2)
        runtime_fail("LM3_RENDER_SIZE must be 1280x720, 1600x900, 1920x1080, 2560x1440, or 0");
    char path[256]; snprintf(path,sizeof path,"extracted/%s/exefs/main",TOTK_GAME_DIR);
    FILE *f=fopen(path,"rb"); uint8_t header[0x60];
    if (!f) runtime_fail("LM3 render patch: cannot read original main identity");
    size_t read=fread(header,1,sizeof header,f); fclose(f);
    if (read!=sizeof header || memcmp(header,"NSO0",4))
        runtime_fail("LM3 render patch: original main header invalid");
    char error[192];
    if (!lm3_render_apply_image(m->base,m->file_size,header+0x40,w,h,error,sizeof error))
        runtime_fail(error);
    fprintf(stderr,"[lm3-render] NLG startup dimensions %ux%u; scene scale fixed 1.0; main build 79E5950FFA85ACF6 (guarded, restart required)\n",w,h);
    if (lm3_render_extra_bytes_for_size(w,h))
        fprintf(stderr,"[lm3-render] native 1440p: GPU pool[2] 0x5f3aa000 and guest heap 0xdbe00000 (+512 MiB each; host application reservation must match)\n");
}
#endif
