/* Derived from nxnative nvn_ndve.inc, MIT, commit 976fbd57. Diagnostic-only adaptation. */
#include "oot_ndve_kernel.h"
// nn::ndve's movie network run by the host's kernels (dm/gfx/msl/ndve_engine.metal) instead of its translated dispatches (on by default; DM_NDVE_ENGINE=0 runs the
// game's, ab alternates with DM_AB). Included by nvn_mtl.m; shaders.md, "The NDVE movie network by the host". Bound to the library's model, not to a game: nn::ndve (a
// Nintendo library linked into main, nn::ndve::RouleauArchiveIO) reads Movie/Ndve/Ndve-Nvn2-vid_seq5.dat, a Rouleau archive whose first network is the U-Net below: its 9
// compute shaders (the keys here are their contents' hashes) and its 0x3c320 bytes of weights, the size the archive's network table states. Any game that ships that model
// runs these keys with these weights; the archive holds no layer graph (the library's code does), so the graph is written here, and the weights' offsets follow from it.
// A movie frame is 14 compute dispatches: the colour conversion 49dff010 (NV12 -> RGBA8, from the archive's NDSR pack), eleven of a U-Net over the picture packed 2x2 at
// 960x540 (16 / 32 / 64 channels, HMMA.16816.F16 convolutions; the model's shaders are compiled for that size), 5a7d69b0 (the last convolution, added to the picture, into
// an RGB10A2 image) and the resampler 4f41ccf2 (the model's 4/3, to 2560x1440). The host skips the eleven, computes the whole network at 5a7d69b0 from the picture the first
// of them read, with the weights and biases the game bound, into the image 5a7d69b0 writes, and runs its own resampler at 4f41ccf2. 49dff010 stays the game's. The kernels
// do the translated shaders' arithmetic in their order (each HMMA as dmsh_build.py's HMMA_BODY does it), so the output bits are theirs (dm/gfx/tests/test_ndve_engine.py).
// Recognition: the keys in order, the weights and biases each dispatch binds at the offsets the graph gives from the first layer's, the picture and image sizes and formats,
// the resampler's constants. Anything else (another model, the archive's second network, other bindings or sizes, a dispatch out of order, no library) leaves the game's
// dispatches running, logged once; a deviation after some dispatches were skipped turns the host's network off for the rest of the run, and that frame's movie picture is
// not updated. The intermediate tensors the skipped dispatches would have written stay unwritten: the network reads them only within the frame's sequence

enum { NDVE_N = 14, NDVE_WBYTES = 0x3c320 };
static const char k_ndve_keys[NDVE_N][17] = {"49dff010b9832251", "ba079f68ae288dba", "a8b4fb48fee0ccf8", "e2eebd7ba18fe35f", "e6be82d835fbe4d3", "9442573ee4ace011",
    "0a56002cdc7e1fce", "0a56002cdc7e1fce", "d15370a382630d4e", "e6be82d835fbe4d3", "7f7ff0f36b54d90f", "a8b4fb48fee0ccf8", "5a7d69b067a7f011", "4f41ccf24f163e0d"};

// The host's network: its kernels (ndve_engine.metal), its tensors (f16 NHWC with a zero border), one layer table
typedef struct { uint32_t W, H, tiles_x, tiles_y; } ndve_args;
enum { NT_X0, NT_A, NT_S16, NT_B32, NT_S32, NT_E32, NT_C64, NT_D64, NT_N };
static const int k_ndve_t[NT_N][3] = {{960, 540, 16}, {960, 540, 16}, {960, 540, 16}, {480, 270, 32}, {480, 270, 32}, {480, 270, 32}, {240, 135, 64}, {240, 135, 64}};
enum { NK_PACK, NK_C16, NK_C32, NK_C64, NK_D16, NK_D32, NK_U64, NK_U32, NK_OUT, NK_LUMA, NK_RS, NK_CMP, NK_N };
static const char *k_ndve_kern[NK_N] = {"pack_input", "conv3s_16_16", "conv3s_32_32", "conv3s_64_64", "down_16_32", "down_32_64", "up_64_32", "up_32_16", "out_16", "rs_luma", "resample", "cmp_img"};
static const int k_ndve_ry[NK_N] = {0, 4, 2, 1, 2, 1, 1, 2, 4, 0, 0, 0};
// each kernel's weights: taps, input channels, output channels (w[tap][k][n] halves), then N biases
static const int k_ndve_shape[NK_N][3] = {{0, 0, 0}, {9, 16, 16}, {9, 32, 32}, {9, 64, 64}, {4, 16, 32}, {4, 32, 64}, {4, 64, 32}, {4, 32, 16}, {9, 16, 16}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
typedef struct { int k, in, out, skip, layer; } ndve_layer;   // layer: the game's dispatch it stands for (its position in k_ndve_keys)
static const ndve_layer k_ndve_l[] = {
    {NK_PACK, -1, NT_X0, -1, 0}, {NK_C16, NT_X0, NT_A, -1, 1}, {NK_C16, NT_A, NT_S16, -1, 2}, {NK_D16, NT_S16, NT_B32, -1, 3}, {NK_C32, NT_B32, NT_S32, -1, 4},
    {NK_D32, NT_S32, NT_C64, -1, 5}, {NK_C64, NT_C64, NT_D64, -1, 6}, {NK_C64, NT_D64, NT_C64, -1, 7}, {NK_U64, NT_C64, NT_B32, NT_S32, 8}, {NK_C32, NT_B32, NT_E32, -1, 9},
    {NK_U32, NT_E32, NT_A, NT_S16, 10}, {NK_C16, NT_A, NT_X0, -1, 11}, {NK_OUT, NT_X0, -1, -1, 12}};
enum { NDVE_NL = sizeof k_ndve_l / sizeof k_ndve_l[0] };
// Each network dispatch's weights and biases (storage buffer registers 2 / 3), bytes from the first layer's weights: the model's layers in order, each one's weights then its
// biases, each padded to 256 bytes but the last biases, to the end of the model's 0x3c320 bytes (ndve_offsets checks it)
static uint32_t g_ndve_w[NDVE_N], g_ndve_b[NDVE_N];
static int ndve_offsets(void) {
    uint32_t o = 0;
    for (int i = 1; i < NDVE_NL; i++) {
        const int *sh = k_ndve_shape[k_ndve_l[i].k]; const int p = k_ndve_l[i].layer;
        g_ndve_w[p] = o; o += ((uint32_t)(sh[0] * sh[1] * sh[2] * 2) + 255) & ~255u;
        g_ndve_b[p] = o; o += i == NDVE_NL - 1 ? (uint32_t)sh[2] * 2 : ((uint32_t)sh[2] * 2 + 255) & ~255u;
    }
    return o == NDVE_WBYTES;
}
static struct {
    _Atomic int state;   // 0 not started, 1 building (background), 2 ready, -1 failed
    id<MTLComputePipelineState> ps[NK_N];
    id<MTLBuffer> t[NT_N], args, luma;
    id<MTL4ArgumentTable> at;
    int pos;             // the next dispatch of the sequence expected (0 = none taken), 13 = 4f41ccf2 next
    int off;             // permanently off after a deviation in the middle of a sequence
    uint64_t w0;         // guest address of the first layer's weights in this sequence
    int pic;             // texture ID of the picture the first layer read (the engine's input)
    uint64_t frames, skipped, rs_frames;
    uint64_t wrs;        // GPU address of the resampler's weights this frame (storage buffer register 2 of 4f41ccf2)
    // DM_NDVE_ENGINE=check: the engine's network and resampler into images of their own beside the game's, compared texel by texel (ndve_compare)
    id<MTLTexture> shadow, shadow_rs, game_out, game_rs;   // the engine's network output and resampled picture; the game's, to compare once its dispatch has run
    id<MTLBuffer> cnt;   // uint32: network texels that differ, the largest difference; the same for the resampler
    uint64_t chk_net, chk_rs;   // frames compared
} g_nd;
static int ndve_mode(void) { const char *e = getenv("OOT_NDVE_COMPARE"); return e && !strcmp(e,"replace"); }   // explicit opt-in only
// DM_NDVE_ENGINE=check: the game's dispatches run, and the engine computes the same frame beside them into images of its own, compared with the game's (logged every 300 frames)
static int ndve_check(void) { static int c = -1; if (c < 0) { const char *e = getenv("OOT_NDVE_COMPARE"); c = e && !strcmp(e, "1"); if (c) MLOG("OOT_NDVE_COMPARE=1: the game's movie network runs, and the host's beside it, compared"); } return c; }
static int ndve_index(NvShader *cs, int from) { for (int i = from; i < NDVE_N; i++) if (!strncmp(cs->key, k_ndve_keys[i], 16)) return i; return -1; }
// Pipelines and tensors, made once in the background; the game's dispatches run until they are ready
static void ndve_build(void) {
    int z = 0; if (!atomic_compare_exchange_strong(&g_nd.state, &z, 1)) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        if (!ndve_offsets()) { MLOG("[RED] NDVE engine: the layer table does not add up to the model's weights; the game's network runs"); g_nd.state = -1; return; }
        NSError *e = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:@(k_ndve_source) options:nil error:&e];
        if (!lib) { MLOG("[RED] NDVE engine: no library (%s); the game's network runs", e.localizedDescription.UTF8String); g_nd.state = -1; return; }
        for (int k = 0; k < NK_N; k++) {
            id<MTLFunction> f = [lib newFunctionWithName:@(k_ndve_kern[k])];
            g_nd.ps[k] = f ? [g_dev newComputePipelineStateWithFunction:f error:&e] : nil;
            if (!g_nd.ps[k]) { MLOG("[RED] NDVE engine: no pipeline for %s; the game's network runs", k_ndve_kern[k]); g_nd.state = -1; return; }
        }
        id<MTLCommandBuffer> cb = [[g_dev newCommandQueue] commandBuffer]; id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        for (int i = 0; i < NT_N; i++) {   // private, zeroed once: the borders stay zero (the kernels write interiors only)
            size_t n = (size_t)(k_ndve_t[i][0] + 2) * (k_ndve_t[i][1] + 2 + 16) * k_ndve_t[i][2] * 2;
            g_nd.t[i] = [g_dev newBufferWithLength:n options:MTLResourceStorageModePrivate];
            [be fillBuffer:g_nd.t[i] range:NSMakeRange(0, n) value:0];
        }
        [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
        for (int i = 0; i < NT_N; i++) res_add(g_nd.t[i]);
        g_nd.luma = [g_dev newBufferWithLength:1920 * 1080 * 2 options:MTLResourceStorageModePrivate]; res_add(g_nd.luma);
        g_nd.args = [g_dev newBufferWithLength:256 * NDVE_NL options:MTLResourceStorageModeShared]; res_add(g_nd.args);
        for (int i = 0; i < NDVE_NL; i++) {
            const ndve_layer *L = &k_ndve_l[i]; const int *T = k_ndve_t[L->in >= 0 ? L->in : NT_X0]; ndve_args a = {960, 540, 0, 0};
            if (L->k != NK_PACK) {
                const int down = L->k == NK_D16 || L->k == NK_D32;   // a.W, a.H: the output's size, the input's for the transposed convolutions
                a.W = (uint32_t)(down ? T[0] / 2 : T[0]); a.H = (uint32_t)(down ? T[1] / 2 : T[1]);
                a.tiles_x = a.W / 8; a.tiles_y = (a.H + k_ndve_ry[L->k] - 1) / k_ndve_ry[L->k];
            }
            memcpy((uint8_t *)g_nd.args.contents + 256 * i, &a, sizeof a);
        }
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 6; ad.maxTextureBindCount = 3;
        g_nd.at = [g_dev newArgumentTableWithDescriptor:ad error:&e];
        if (!g_nd.at) { MLOG("[RED] NDVE engine: no argument table; the game's network runs"); g_nd.state = -1; return; }
        g_nd.state = 2;
        MLOG("NDVE engine ready: host kernels available for opt-in movie network testing");
    } });
}
static int ndve_fail(NvShader *cs, const char *why) {
    if (g_nd.pos > 1 && !ndve_check()) { g_nd.off = 1; MLOG("[RED] NDVE engine: %s at %s after %d of the network's dispatches were skipped; the engine is off from now on (this frame's movie picture is not updated)", why, cs->key, g_nd.pos - 1); }
    else { static int once; if (!once++) MLOG("NDVE engine: %s at %s; the game's network runs (logged once)", why, cs->key); }
    g_nd.pos = 0; return 0;
}
// The image (img) or texture (tex) binding of a stage-5 reflection entry, or nil; *id_out (when given) its texture ID
static id<MTLTexture> ndve_tex_id(NvShader *cs, int kind, int reg, int *id_out) {
    for (int i = 0; i < cs->rf.nb; i++) {
        const nvn_rbind *b = &cs->rf.b[i];
        if (b->kind != kind || b->reg != reg) continue;
        int tid = netbp_tid(reg, kind == NVN_RB_IMG);
        if (id_out) *id_out = tid;
        return netbp_tex(tid, kind == NVN_RB_IMG);
    }
    return nil;
}
static id<MTLTexture> ndve_tex(NvShader *cs, int kind, int reg) { return ndve_tex_id(cs, kind, reg, NULL); }
// The network: 13 kernels in one compute encoder, ordered by barriers; input = the picture 5a7d69b0 reads, output = the image it writes
static void ndve_encode(id<MTLTexture> rgba, id<MTLTexture> out, uint64_t wgpu) {
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)enc_c_begin(NULL);   // waits for the work before it (queue barrier)
    [ce setArgumentTable:g_nd.at];
    id<MTL4ArgumentTable> at = g_nd.at;
    for (int i = 0; i < NDVE_NL; i++) {
        const ndve_layer *L = &k_ndve_l[i];
        if (i) [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:bar_vis()];
        [ce setComputePipelineState:g_nd.ps[L->k]];
        [at setAddress:g_nd.args.gpuAddress + 256 * i atIndex:4];
        const ndve_args *a = (const ndve_args *)((uint8_t *)g_nd.args.contents + 256 * i);
        if (L->k == NK_PACK) {
            [at setTexture:rgba.gpuResourceID atIndex:0]; [at setAddress:g_nd.t[L->out].gpuAddress atIndex:1];
            [ce dispatchThreadgroups:MTLSizeMake((960 + 31) / 32, (540 + 3) / 4, 1) threadsPerThreadgroup:MTLSizeMake(32, 4, 1)];
            continue;
        }
        [at setAddress:g_nd.t[L->in].gpuAddress atIndex:0];
        if (L->out >= 0) [at setAddress:g_nd.t[L->out].gpuAddress atIndex:1];
        [at setAddress:wgpu + g_ndve_w[L->layer] atIndex:2]; [at setAddress:wgpu + g_ndve_b[L->layer] atIndex:3];
        if (L->skip >= 0) [at setAddress:g_nd.t[L->skip].gpuAddress atIndex:5];
        if (L->k == NK_OUT) { [at setTexture:rgba.gpuResourceID atIndex:0]; [at setTexture:out.gpuResourceID atIndex:1]; }
        const int staged = L->k == NK_C16 || L->k == NK_C32 || L->k == NK_C64;   // conv3s_*: a threadgroup = 32 x RY outputs
        uint32_t groups = staged ? (a->W + 31) / 32 * a->tiles_y : (a->tiles_x * a->tiles_y + 3) / 4;
        [ce dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
    }
    [ce barrierAfterStages:MTLStageDispatch | MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:bar_vis()];   // what follows (4f41ccf2) waits for the image
    enc_c_end(ce);
    args_dirty_all();
}
// The resampler 4f41ccf2: luma pre-pass, then one SIMD-group an 8x8 output block
static int ndve_resample(NvShader *cs, id<MTLTexture> into) {
    id<MTLTexture> pic = ndve_tex(cs, NVN_RB_TEX, 0), dith = ndve_tex(cs, NVN_RB_TEX, 3), out = ndve_tex(cs, NVN_RB_IMG, 1);
    const int32_t *c = (const int32_t *)S.push[1];
    if (!pic || !dith || !out || pic.width != 1920 || pic.height != 1080 || pic.pixelFormat != MTLPixelFormatRGB10A2Unorm || dith.width != 512 || dith.height != 512 ||
        out.width != 2560 || out.height != 1440 || out.pixelFormat != MTLPixelFormatRGBA8Unorm || out.textureType != MTLTextureType2D ||
        c[0] || c[1] || c[2] != 1919 || c[3] != 1079 || c[4] || c[5] || c[6] != 2559 || c[7] != 1439 || c[20] != 1) {
        static int once; if (!once++) MLOG("NDVE engine: the resampler's bindings or constants are not the model's (2560x1440 RGBA8 from 1920x1080, dither on); the game's resampler runs (logged once)");
        return 0;
    }
    __strong id<MTLBuffer> cb = nil; size_t co = arena_alloc(256, &cb); memcpy((uint8_t *)cb.contents + co, S.push[1], 96);
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)enc_c_begin(NULL);
    id<MTL4ArgumentTable> at = g_nd.at; [ce setArgumentTable:at];
    [ce setComputePipelineState:g_nd.ps[NK_LUMA]]; [at setTexture:pic.gpuResourceID atIndex:0]; [at setAddress:g_nd.luma.gpuAddress atIndex:1];
    [ce dispatchThreadgroups:MTLSizeMake(60, 135, 1) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
    [ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:bar_vis()];
    [ce setComputePipelineState:g_nd.ps[NK_RS]];
    [at setAddress:g_nd.luma.gpuAddress atIndex:0]; [at setAddress:g_nd.wrs atIndex:2]; [at setAddress:cb.gpuAddress + co atIndex:3];
    if (into) g_nd.game_rs = out;
    [at setTexture:pic.gpuResourceID atIndex:0]; [at setTexture:dith.gpuResourceID atIndex:1]; [at setTexture:(into ? into : out).gpuResourceID atIndex:2];
    [ce dispatchThreadgroups:MTLSizeMake(80, 180, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
    [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:bar_vis()];
    enc_c_end(ce);
    args_dirty_all();
    return 1;
}
// DM_NDVE_ENGINE=check: an image of the engine's own like the game's, made once
static id<MTLTexture> ndve_shadow(id<MTLTexture> like) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:like.pixelFormat width:like.width height:like.height mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d]; res_add(t); return t;
}
// DM_NDVE_ENGINE=check: the game's image a against the engine's b, into counters slot (0 network, 1 resampler); every 300 frames the totals so far
static void ndve_compare(id<MTLTexture> a, id<MTLTexture> b, int slot) {
    if (!g_nd.cnt) { g_nd.cnt = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; memset(g_nd.cnt.contents, 0, 16); res_add(g_nd.cnt); }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    id<MTL4ComputeCommandEncoder> ce = (id<MTL4ComputeCommandEncoder>)enc_c_begin(NULL);
    id<MTL4ArgumentTable> at = g_nd.at; [ce setArgumentTable:at];
    [ce setComputePipelineState:g_nd.ps[NK_CMP]]; [at setTexture:a.gpuResourceID atIndex:0]; [at setTexture:b.gpuResourceID atIndex:1]; [at setAddress:g_nd.cnt.gpuAddress + 8 * slot atIndex:1];
    [ce dispatchThreadgroups:MTLSizeMake((a.width + 15) / 16, (a.height + 15) / 16, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [ce barrierAfterStages:MTLStageDispatch beforeQueueStages:MTLStageAll visibilityOptions:bar_vis()];
    enc_c_end(ce);
    args_dirty_all();
    uint64_t *n = slot ? &g_nd.chk_rs : &g_nd.chk_net;
    if (++*n % 300 == 0) {
        const uint32_t *c = (const uint32_t *)g_nd.cnt.contents;
        MLOG("%sNDVE engine check, %s, %llu frames compared: %u texels of the engine's %lux%lu differ from the game's (largest difference %u/1023)", c[2 * slot] ? "[RED] " : "",
             slot ? "resampler" : "network", (unsigned long long)*n, c[2 * slot], (unsigned long)a.width, (unsigned long)a.height, c[2 * slot + 1]);
    }
}
// 1 when the dispatch was taken (skipped or replaced by the engine), 0 to run it
static int ndve_engine_dispatch(NvShader *cs, const p_dispatch *dp) {
    const int chk = ndve_check();
    if ((!ndve_mode() && !chk) || g_nd.off) return 0;
    int i = ndve_index(cs, 0); if (i < 0) return 0;
    if (g_nd.state == 0) ndve_build();
    if (!chk && !ndve_mode()) return 0;   // off unless the isolated test explicitly selects replace
    if (i == 0) {   // the colour conversion runs; a new frame's sequence may start. Under check, the game's resampled picture of the last frame is there now
        if (chk && g_nd.game_rs && g_nd.state == 2) ndve_compare(g_nd.game_rs, g_nd.shadow_rs, 1);
        g_nd.game_rs = nil; g_nd.pos = 0; return 0;
    }
    if (i == NDVE_N - 1) {   // the resampler: by the engine right after the engine's network, else the game's
        int take = g_nd.pos == 13;
        g_nd.pos = 0;
        if (!take) return 0;
        if (chk && g_nd.game_out) { ndve_compare(g_nd.game_out, g_nd.shadow, 0); g_nd.game_out = nil; }   // the game's network has run: its image against the engine's
        const p_bind_buf *w = &S.ssbo[5][2]; void *wb = NULL; uint64_t wo = 0;
        if (!w->addr || w->size < 0x3000 || !mtl_gpu_addr(w->addr, 0x3000, &wb, &wo, &g_nd.wrs)) { static int once; if (!once++) MLOG("NDVE engine: the resampler's weights are not bound; the game's resampler runs"); return 0; }
        if (chk && !g_nd.shadow_rs) { id<MTLTexture> o = ndve_tex(cs, NVN_RB_IMG, 1); if (o) g_nd.shadow_rs = ndve_shadow(o); }
        if (!ndve_resample(cs, chk ? g_nd.shadow_rs : nil)) return 0;
        if (!g_nd.rs_frames++) MLOG("NDVE engine: first movie frame resampled by the engine");
        return !chk;
    }
    // dispatch i of the sequence: the keys repeat (a8b4, e6be, 0a56 twice), so position decides
    if (g_nd.pos == 0) { if (i != 1) return 0; if (g_nd.state != 2) return 0; }
    else if (ndve_index(cs, g_nd.pos) != g_nd.pos) return ndve_fail(cs, "a dispatch out of the network's order");
    const int p = g_nd.pos ? g_nd.pos : 1;
    const p_bind_buf *w = &S.ssbo[5][2], *b = &S.ssbo[5][3];
    if (p == 1) {   // the first layer: its weights place the others, its picture is the engine's input
        if (!ndve_tex_id(cs, NVN_RB_TEX, 1, &g_nd.pic)) return ndve_fail(cs, "the first layer reads no picture");
        g_nd.w0 = w->addr;
    }
    if (w->addr != g_nd.w0 + g_ndve_w[p] || b->addr != g_nd.w0 + g_ndve_b[p] || !w->size || !b->size) return ndve_fail(cs, "weights or biases not where the model's layers put them");
    if (p < 12) { g_nd.pos = p + 1; g_nd.skipped += !chk; return !chk; }
    // 5a7d69b0: the whole network now
    int rgba_id = 0;
    id<MTLTexture> rgba = ndve_tex_id(cs, NVN_RB_TEX, 4, &rgba_id), out = ndve_tex(cs, NVN_RB_IMG, 0);
    void *wb = NULL; uint64_t wo = 0, wgpu = 0;
    if (!rgba || !out || rgba.width != 1920 || rgba.height != 1080 || out.width != 1920 || out.height != 1080 || rgba.pixelFormat != MTLPixelFormatRGBA8Unorm ||
        out.pixelFormat != MTLPixelFormatRGB10A2Unorm || rgba.textureType != MTLTextureType2D || out.textureType != MTLTextureType2D)
        return ndve_fail(cs, "the picture or the image is not the model's 1920x1080 RGBA8 / RGB10A2 pair");
    if (rgba_id != g_nd.pic) return ndve_fail(cs, "the picture added to the network's output is not the one its first layer read");
    if (!mtl_gpu_addr(g_nd.w0, NDVE_WBYTES, &wb, &wo, &wgpu)) return ndve_fail(cs, "the weight table is in no memory pool");
    if (chk && !g_nd.shadow) g_nd.shadow = ndve_shadow(out);
    ndve_encode(rgba, chk ? g_nd.shadow : out, wgpu);
    if (chk) g_nd.game_out = out;   // compared once the game's 5a7d69b0 has written it, at 4f41ccf2
    g_nd.pos = 13;   // the resampler next
    if (!g_nd.frames++) MLOG("NDVE engine: first movie frame by the engine (%llu dispatches skipped)", (unsigned long long)g_nd.skipped);
    (void)dp; return !chk;
}
