/* Fixture for tests/test_nvn_dispatch_barrier.py: compute dispatches ordered by the game's barriers (nvn_dispatch_order.h,
   included as written; cs_rangeless spliced in from nvn_mtl.m at the marker below), driven on a real Metal 4 compute encoder
   the way nvn_mtl.m drives it: host operations (blit_kind) put a barrier before themselves and note themselves, dispatches
   (disp_enc) ask dsp_barrier_before. Cases follow nxnative 547c5db6's t_dispatch_barrier (MIT). No game, no runtime.

   Env: DM_NVN_DISPATCH_BARRIER=game | full (the production switch); PROBE_RACE=1 runs the race demonstration only (game
   barriers dropped, as a game that forgot them; reported, not asserted). Output: "<n> checks, <m> failed"; exit 1 on a
   failure, 77 without a Metal 4 device. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include "nvn_refl.h"

#define MLOG(...) do { fprintf(stderr, "[probe] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#include "nvn_dispatch_order.h"

/* ---- the production classification, with the fixture's stand-ins for its types ---- */
typedef struct { uint64_t addr, size; } p_bind_buf;
static struct { p_bind_buf ubo[6][16], ssbo[6][16]; } S;
typedef struct { nvn_refl rf; } NvShader;
/* CS_RANGELESS */

/* ---- the harness ---- */
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
#define EXPECT(label, want, got) do { checks++; long long w_ = (long long)(want), g_ = (long long)(got); \
    if (w_ != g_) { failed++; fprintf(stderr, "FAIL: %s: expected %lld, got %lld\n", label, w_, g_); } } while (0)

static id<MTLDevice> dev; static id<MTL4CommandQueue> queue; static id<MTL4CommandAllocator> alloc; static id<MTLResidencySet> rset;
static id<MTL4CommandBuffer> cb; static id<MTL4ComputeCommandEncoder> enc; static id<MTL4ArgumentTable> table;
static int g_open, g_drop_game_barriers; static uint64_t g_barriers;
static id<MTLBuffer> mem; static uint32_t *word;
enum { MEM = 1 << 23 };

static void barrier(void) { [enc barrierAfterEncoderStages:MTLStageBlit | MTLStageDispatch beforeEncoderStages:MTLStageBlit | MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice]; g_barriers++; }
static void begin(void) {   // a new shared encoder: behind a full queue barrier (disp_enc / blit_kind open path)
    cb = [dev newCommandBuffer]; [cb beginCommandBufferWithAllocator:alloc]; [cb useResidencySet:rset];
    enc = [cb computeCommandEncoder]; [enc barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    [enc setArgumentTable:table]; g_open = 0; dsp_note_encoder_open();
}
static void end(const char *what) {
    [enc endEncoding]; [cb endCommandBuffer]; enc = nil;
    id<MTLSharedEvent> ev = [dev newSharedEvent]; id<MTL4CommandBuffer> list[1] = {cb};
    [queue commit:list count:1]; [queue signalEvent:ev value:1];
    CHECK([ev waitUntilSignaledValue:1 timeoutMS:10000], what); cb = nil; [alloc reset];
}
static void host_fill(uint64_t off, uint64_t n, uint8_t v) {   // blit_kind, then the fill
    if (g_open) barrier();
    dsp_note_host_op(); g_open = 1;
    [enc fillBuffer:mem range:NSMakeRange(off, n) value:v];
}
static void host_copy(uint64_t src, uint64_t dst, uint64_t n) {
    if (g_open) barrier();
    dsp_note_host_op(); g_open = 1;
    [enc copyFromBuffer:mem sourceOffset:src toBuffer:mem destinationOffset:dst size:n];
}
static void game_barrier(void) { if (!g_drop_game_barriers) dsp_note_game_barrier(); }
static void dispatch(id<MTLComputePipelineState> ps, const NvShader *sh, uint64_t off, NSUInteger groups, NSUInteger tg) {   // disp_enc, then the dispatch
    if (dsp_barrier_before(g_open, cs_rangeless(sh))) barrier();
    g_open = 1;
    [enc setComputePipelineState:ps]; [table setAddress:mem.gpuAddress + off atIndex:0];
    [enc dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
}
static NvShader one_ssbo(uint64_t addr, uint64_t size) {   // storage buffer slot 0, read-write, plus a driver segment
    NvShader sh; memset(&sh, 0, sizeof sh); sh.rf.nb = 2;
    sh.rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .rw = 1, .reg = 0};
    sh.rf.b[1] = (nvn_rbind){.kind = NVN_RB_DRV0};
    S.ssbo[5][0] = (p_bind_buf){addr, size};
    return sh;
}
static id<MTLComputePipelineState> pipeline(id<MTLLibrary> lib, NSString *name) {
    NSError *e = nil; id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:name] error:&e];
    if (!ps) { fprintf(stderr, "pipeline %s: %s\n", name.UTF8String, e.localizedDescription.UTF8String); exit(1); }
    return ps;
}

int main(void) { @autoreleasepool {
    dev = MTLCreateSystemDefaultDevice(); if (!dev || ![dev supportsFamily:MTLGPUFamilyMetal4]) return 77;
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void chain(device uint *b [[buffer(0)]], uint p [[thread_position_in_grid]]) { if (p == 0) b[0] = b[0] * 3u + 1u; }\n"
        "kernel void slot(device uint *b [[buffer(0)]], uint p [[thread_position_in_grid]]) { if (p == 0) b[0] = b[1] + 1u; }\n"
        "kernel void k1(device uint *b [[buffer(0)]], uint i [[thread_position_in_grid]]) { b[i] += i + 1u; }\n"
        "kernel void k2(device uint *b [[buffer(0)]], uint i [[thread_position_in_grid]]) { b[i + (1u << 20)] += b[i]; }\n" options:nil error:&e];
    if (!lib) { fprintf(stderr, "kernels: %s\n", e.localizedDescription.UTF8String); return 1; }
    id<MTLComputePipelineState> chain = pipeline(lib, @"chain"), slot = pipeline(lib, @"slot"), k1 = pipeline(lib, @"k1"), k2 = pipeline(lib, @"k2");
    queue = [dev newMTL4CommandQueue]; alloc = [dev newCommandAllocator];
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; table = [dev newArgumentTableWithDescriptor:ad error:&e];
    mem = [dev newBufferWithLength:MEM options:MTLResourceStorageModeShared]; word = mem.contents; memset(word, 0, MEM);
    rset = [dev newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&e]; [rset addAllocation:mem]; [rset commit];
    if (!queue || !alloc || !table || !mem || !rset) { fprintf(stderr, "Metal 4 objects: %s\n", e.localizedDescription.UTF8String); return 1; }
    int game = dbar_game(); uint64_t base = 0x100000000ull;   // guest addresses the classification sees

    if (getenv("PROBE_RACE")) {   // 16 rounds of K1 (s0[i] += i + 1) then K2 (s1[i] += s0[i]) over 2^20 words, with the game's barriers dropped
        enum { RN = 1 << 20, R = 16 }; g_drop_game_barriers = 1;
        NvShader sh = one_ssbo(base, (uint64_t)RN * 8);
        begin(); for (int r = 0; r < R; r++) { game_barrier(); dispatch(k1, &sh, 0, RN / 64, 64); game_barrier(); dispatch(k2, &sh, 0, RN / 64, 64); }
        end("the race run completes");
        uint64_t bad = 0; for (uint32_t i = 0; i < RN; i++) bad += word[RN + i] != (i + 1u) * (uint32_t)(R * (R + 1) / 2) || word[i] != (i + 1u) * R;
        printf("race (%s, game barriers dropped): %llu of %d elements wrong, %llu barriers\n", game ? "game" : "full", (unsigned long long)bad, RN, (unsigned long long)g_barriers);
        return 0;
    }

    // 0. Which bindings have a range (cs_rangeless)
    { NvShader sh = one_ssbo(base, 64); CHECK(!cs_rangeless(&sh), "a sized storage buffer and a driver segment have a range");
      S.ssbo[5][0].size = 0; CHECK(cs_rangeless(&sh), "a storage buffer bound with no size has none");
      S.ssbo[5][0] = (p_bind_buf){0, 0}; CHECK(!cs_rangeless(&sh), "an unbound slot reaches no memory");
      sh = one_ssbo(base, 64); sh.rf.b[1] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = NVN_PUSH_BANK}; CHECK(!cs_rangeless(&sh), "push constants are no guest memory");
      sh.rf.b[1] = (nvn_rbind){.kind = NVN_RB_CBUF, .reg = 17}; CHECK(cs_rangeless(&sh), "a uniform bank past 15 has no range");
      const int none[] = {NVN_RB_TEX, NVN_RB_IMG, NVN_RB_TBUF, NVN_RB_SAMP, NVN_RB_SBUFU, NVN_RB_TEXP, NVN_RB_SAMPP, NVN_RB_GMEM, NVN_RB_GMEML, NVN_RB_GEO};
      for (unsigned i = 0; i < sizeof none / sizeof *none; i++) { sh.rf.b[1] = (nvn_rbind){.kind = (uint8_t)none[i]}; char l[64]; snprintf(l, sizeof l, "binding kind %d has no range", none[i]); CHECK(cs_rangeless(&sh), l); } }

    enum { N = 64 };
    // 1. A chain on one word, each dispatch behind the game's barrier: the sequential result in both modes
    { word[0] = 5; uint32_t want = 5; for (int i = 0; i < N; i++) want = want * 3u + 1u;
      NvShader sh = one_ssbo(base, 64);
      begin(); uint64_t b0 = g_barriers, g0 = g_dbar_game;
      for (int i = 0; i < N; i++) { game_barrier(); dispatch(chain, &sh, 0, 1, 1); }
      EXPECT("a barrier before every dispatch the game ordered", N - 1, g_barriers - b0);
      if (game) EXPECT("counted as the game's own", N - 1, g_dbar_game - g0);
      end("the chain completes"); EXPECT("the chain runs in order", want, word[0]); }

    // 2. Dispatches into slots of their own, no game barrier: none in game mode, one each in full mode
    { for (int i = 0; i < N; i++) { word[16 * i] = 0; word[16 * i + 1] = (uint32_t)i; }
      begin(); uint64_t b0 = g_barriers, f0 = g_dbar_free;
      for (int i = 0; i < N; i++) { NvShader sh = one_ssbo(base + 64 * i, 64); dispatch(slot, &sh, 64 * i, 1, 1); }
      EXPECT("barriers between dispatches the game did not order", game ? 0 : N - 1, g_barriers - b0);
      if (game) EXPECT("counted as none", N, g_dbar_free - f0);
      end("the slots complete"); int ok = 1; for (int i = 0; i < N; i++) ok &= word[16 * i] == (uint32_t)i + 1;
      CHECK(ok, "every slot's dispatch ran"); }

    // 3. A fill, then a dispatch that reads it: ordered after the host operation in both modes
    { word[0] = 0; NvShader sh = one_ssbo(base, 64);
      begin(); dispatch(slot, &sh, 2048, 1, 1); uint64_t b0 = g_barriers, h0 = g_dbar_host;
      host_fill(0, 4, 7); dispatch(chain, &sh, 0, 1, 1);
      EXPECT("a barrier before the fill and one between it and the dispatch", 2, g_barriers - b0);
      if (game) EXPECT("counted as after a host operation", 1, g_dbar_host - h0);
      end("the fill and its dispatch complete"); EXPECT("the dispatch sees the fill", 0x07070707u * 3u + 1u, word[0]); }

    // 4. A dispatch, then a copy of what it wrote: the copy is ordered after it in both modes
    { word[0] = 2; word[512] = 0; NvShader sh = one_ssbo(base, 64);
      begin(); dispatch(chain, &sh, 0, 1, 1); uint64_t b0 = g_barriers; host_copy(0, 2048, 4);
      EXPECT("a barrier between a dispatch and the copy that reads what it wrote", 1, g_barriers - b0);
      end("the dispatch and its copy complete"); EXPECT("the copy sees the dispatch", 2 * 3 + 1, word[512]); }

    // 5. A binding whose reach is no range (no size): the full order on both sides, in both modes
    { word[0] = 1; NvShader sh = one_ssbo(base, 0);
      begin(); dispatch(slot, &sh, 2048, 1, 1); uint64_t b0 = g_barriers, u0 = g_dbar_full, h0 = g_dbar_host;
      dispatch(chain, &sh, 0, 1, 1); dispatch(chain, &sh, 0, 1, 1); S.ssbo[5][0].size = 64; dispatch(chain, &sh, 0, 1, 1);
      EXPECT("barriers around dispatches with no range", 3, g_barriers - b0);
      if (game) { EXPECT("counted as no range", 2, g_dbar_full - u0); EXPECT("the next one counted as after unknown work", 1, g_dbar_host - h0); }
      end("the rangeless dispatches complete"); EXPECT("they run in order", ((1 * 3 + 1) * 3 + 1) * 3 + 1, word[0]); }

    // 6. A new encoder starts behind its own queue barrier: a host operation in the previous one orders nothing in it
    { NvShader sh = one_ssbo(base + 4096, 64);
      begin(); host_fill(4096, 4, 0); end("the first encoder completes");
      begin(); uint64_t b0 = g_barriers; dispatch(slot, &sh, 4096, 1, 1); dispatch(slot, &sh, 8192, 1, 1);
      EXPECT("no encoder barrier from a closed encoder's host operation", game ? 0 : 1, g_barriers - b0);
      end("the second encoder completes"); }

    printf("dispatch barrier probe (%s): %d checks, %d failed\n", game ? "game" : "full", checks, failed);
    return failed != 0;
} }
