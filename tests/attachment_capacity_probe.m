#import <Foundation/Foundation.h>
#include "nvn_refl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MTLRenderPassColorAttachmentDescriptor AttachmentDescriptor

// CPU descriptor stand-ins: only fields touched by the extracted registration block.
enum { MTLTextureType3D = 7, MTLPixelFormatDepth32Float_Stencil8 = 260,
       MTLLoadActionLoad = 1, MTLStoreActionUnknown = 4, NVN_STENCIL_OP_KEEP = 1 };
@protocol MTLTexture
@property NSUInteger textureType, pixelFormat;
@end
@interface Texture : NSObject <MTLTexture>
@property NSUInteger textureType, pixelFormat;
@end
@implementation Texture @end
@interface MTLRenderPassColorAttachmentDescriptor : NSObject
@property id<MTLTexture> texture;
@property NSUInteger level, slice, depthPlane, loadAction, storeAction;
@end
@implementation MTLRenderPassColorAttachmentDescriptor @end
typedef struct { uint64_t serial; } TextureSerial;
static struct {
    struct { int n; TextureSerial *color[8], *depth; } rt;
    int has_rt;
    uint8_t clear_c[8], clear_d, clear_s;
} R;
static unsigned g_sd_plain;
static uint64_t att_bytes(id<MTLTexture> texture, NSUInteger level) { return texture ? 320 : 0; }
static int g_zskip_cand_ok[8];
static void zskip_del(uint64_t serial) {}
static void zskip_clear(void) {}
static struct {
    struct { uint32_t mask; } chmask;
    struct { int dw, st, fail_f, zfail_f, zpass_f, fail_b, zfail_b, zpass_b; } ds;
    unsigned st_wmask[2];
} S;
@interface NvShader : NSObject { @public nvn_refl rf; } @end
@implementation NvShader @end
@interface NvProg : NSObject {
@public
    NvShader *sh[6];
    void *udc_sh[6];
    uint8_t udc_ok, udc_img, udc_imgw;
    uint32_t udc_cmask;
}
@end
@implementation NvProg @end

/* LEDGER */
static void register_attachments(NSArray *cas, MTLRenderPassColorAttachmentDescriptor *da,
                                 MTLRenderPassColorAttachmentDescriptor *sa) {
/* REGISTER */
}
/* NOTE_DRAW */
#define ATT_READS_MAX 8
static int g_enc;
/* COLLECT */
static const uint8_t *g_exec_cur;
static int g_hoist_n;
static int hoist_skipped(const uint8_t *op) { return 0; }
/* LOOKAHEAD */

static void check_lookahead(void) {
    /* NST */
    static const uint8_t op[2];
    const uint8_t *ops[] = {op, op + 1};
    g_sdx_op = ops; g_exec_cur = op; g_exec_seq = 1; g_sdx_ok = 1;
    memset(g_sdx_hk, 0, sizeof g_sdx_hk); g_sdx_nb = 0;
    memset(g_sdx_bst, 0, sizeof g_sdx_bst);
    assert(sdx_decide(nst));
    for (int i = 0; i < g_nsd; i++) assert(nst[i] == 5 << 2);

    // One future clear per color; two plane-specific events share the depth/stencil bucket.
    sdx_ev events[10]; unsigned count = 0;
    for (int i = 0; i < g_nsd; i++) {
        sd_att *t = &g_sd[i];
        int b = sdx_bucket(t->ser, 1);
        if (i == 0 || t->ser != g_sd[i - 1].ser) g_sdx_bst[b] = count;
        events[count++] = (sdx_ev){.seq = 1, .rtseq = 1, .ser = t->ser, .op = op + 1,
            .lv = t->lv, .sl = t->sl, .kind = t->depth == 2 ? SDX_ATT : SDX_CLEAR,
            .plane = t->depth == 2 ? 8 : t->depth};
        g_sdx_bst[b + 1] = count;
    }
    g_sdx = events;
    assert(sdx_decide(nst));
    for (int i = 0; i < g_nsd; i++) assert(nst[i] == (g_sd[i].depth == 2 ? 2 << 2 : 2));
    for (unsigned i = 0; i < count; i++) { events[i].kind = SDX_READ; events[i].why = 3; }
    assert(sdx_decide(nst));
    for (int i = 0; i < g_nsd; i++) assert(nst[i] == 3 << 2);
    g_sdx_ok = 0;
    assert(!sdx_decide(nst));
    g_sdx = NULL; g_sdx_op = NULL;
}

int main(void) { @autoreleasepool {
    TextureSerial serials[9];
    for (int i = 0; i < 9; i++) serials[i].serial = 100 + i;
    NvProg *pr = [NvProg new]; pr->sh[1] = [NvShader new];
    pr->sh[1]->rf.color_mask = 0xff;
    Texture *texture = [Texture new];
    texture.pixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    int cases = 0;
    for (int colors = 0; colors <= 8; colors++) for (int ds = 0; ds < 4; ds++) {
        NSMutableArray *cas = [NSMutableArray new];
        memset(&R, 0, sizeof R); memset(&S, 0, sizeof S);
        R.has_rt = 1; R.rt.n = colors;
        if (ds) R.rt.depth = &serials[8];
        for (int i = 0; i < 8; i++) {
            MTLRenderPassColorAttachmentDescriptor *c = [MTLRenderPassColorAttachmentDescriptor new];
            if (i < colors) { c.texture = texture; R.rt.color[i] = &serials[i]; }
            c.level = 2; c.slice = 3; c.depthPlane = 4;
            c.loadAction = MTLLoadActionLoad; R.clear_c[i] = i & 1;
            [cas addObject:c];
        }
        MTLRenderPassColorAttachmentDescriptor *da = [MTLRenderPassColorAttachmentDescriptor new];
        MTLRenderPassColorAttachmentDescriptor *sa = [MTLRenderPassColorAttachmentDescriptor new];
        da.texture = ds & 1 ? texture : nil; sa.texture = ds & 2 ? texture : nil;
        da.level = sa.level = 2; da.slice = sa.slice = 3;
        da.loadAction = MTLLoadActionLoad; sa.loadAction = 0; R.clear_s = 1;
        texture.textureType = colors & 1 ? MTLTextureType3D : 2;
        register_attachments(cas, da, sa);
        assert(g_nsd == colors + !!(ds & 1) + !!(ds & 2));
        for (int i = 0; i < colors; i++) {
            sd_att *t = &g_sd[i];
            assert(t->ser == serials[i].serial && t->idx == i && t->depth == 0);
            assert(t->lv == 2 && t->sl == (colors & 1 ? 4 : 3));
            assert(t->load && t->wrote == (i & 1) && t->bytes == 320);
            assert(((MTLRenderPassColorAttachmentDescriptor *)cas[i]).storeAction == MTLStoreActionUnknown);
        }
        int k = colors;
        if (ds & 1) {
            assert(g_sd[k].depth == 1 && g_sd[k].idx == -1 && g_sd[k].ser == serials[8].serial);
            assert(g_sd[k].bytes == (ds == 3 ? 256 : 320) && g_sd[k].load && !g_sd[k].wrote);
            k++;
        }
        if (ds & 2) {
            assert(g_sd[k].depth == 2 && g_sd[k].idx == -1 && g_sd[k].ser == serials[8].serial);
            assert(g_sd[k].bytes == 64 && !g_sd[k].load && g_sd[k].wrote);
        }
        S.ds.dw = S.ds.st = 1; S.st_wmask[0] = 0xff;
        udc_note_draw(pr);
        for (int i = 0; i < g_nsd; i++) assert(g_sd[i].wrote);
        check_lookahead();
        for (int has_rt = 0; has_rt <= 1; has_rt++) for (g_enc = 0; g_enc <= 1; g_enc++) {
            R.has_rt = has_rt;
            uint64_t observed[19];
            int n = draw_att_reads(pr, observed), current = has_rt ? colors + !!ds : 0;
            assert(n == current + (g_enc ? g_nsd : 0));
            for (int i = 0; i < current; i++) assert(observed[i] == serials[i < colors ? i : 8].serial);
            if (g_enc) for (int i = 0; i < g_nsd; i++) assert(observed[current + i] == g_sd[i].ser);
            if (colors == 8 && ds == 3 && has_rt && g_enc) assert(n == 19);
        }
        cases++;
    }
    assert(cases == 36);
    puts("36 attachment shapes passed: registration, UDC, lookahead and collector boundaries");
    return 0;
} }
