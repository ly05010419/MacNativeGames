/* Included only by nvn_mtl.m. Encode stage (TOTK_RENDER_STREAM=1, runtime `flag render_stream 0|1`): the Metal render
   encoder of every render pass is created, fed and ended by one persistent worker thread ("B"); the replay thread ("A")
   keeps the NVN state, the resource resolution, the argument-block writes, blits, dispatches and commits, and only
   joins B (rs_sync) where it must touch the command buffer itself. 2026-10-16, replacing the 2026-10-16 morning version
   (S017) that handed the encoder over from the 16th draw of a pass, woke the worker with a condition variable at every
   pass and joined it at every pass end: measured replay 4.90 -> 5.28 ms, i.e. the waits cost more than the driver calls
   they moved (analysis/s0a-*).
   - A sees a sentinel object as `g_enc` while B owns a pass; the enc_r_* wrappers turn calls on the sentinel into packets.
   - Packets travel through the single-producer / single-consumer FIFO; draws publish, so B sees whole draws.
   - B spins while packets keep coming and sleeps on a semaphore after RS_SPIN_NS of nothing; A signals it when it
     publishes to a sleeping B (store / fence / load on both sides, so a wake-up cannot be lost).
   - rs_sync() (A) waits until B consumed everything: before any encoder A opens on the command buffer itself, before a
     command buffer is ended / committed, and at the end of the submission. It spins; B is awake by then.
   - Objects: pipeline / depth-stencil states and the pass descriptor are retained for the packet and released by B
     after use (probes build pipeline states that go away right after the pass; the game's live in caches).
   No GPU ordering, resource retirement or uniform snapshot policy changes: B only encodes what A decided, in A's order. */
#include "nvn_encode_fifo.h"
#include <TargetConditionals.h>
#include <sched.h>
#include <dispatch/dispatch.h>

enum {
    RS_PSO, RS_DSS, RS_VIEWPORT, RS_SCISSOR, RS_CULL, RS_WINDING, RS_FILL,
    RS_BIAS, RS_CLIP, RS_STENCIL, RS_BLEND, RS_VISIBILITY, RS_ADDRESS,
    RS_DRAW, RS_INSTANCED, RS_INDEXED, RS_STORE,
    RS_BEGIN,   /* u[0] = retained MTL4RenderPassDescriptor, u[1] = vertex table, u[2] = fragment table, u[3] = command buffer, u[4] = barrier at start, u[5] = visibility options */
    RS_END,     /* end the encoder (barrier at end if u[0], visibility options u[1]) */
};
enum { RS_SPIN_NS = 200000 };   /* B waits (wfe) this long on an empty queue before sleeping on the semaphore. During a replay A never pauses
                                   for that long, so B sleeps in the frame gap and wakes (~30 us) at the next submission. The waits are wfe / sev,
                                   not a hot spin: with a hot spin (2 ms yield loop) the visible-mode GPU time rose 8.9 -> 9.3 ms (analysis/rs3v-*,
                                   rs4v-* with 200 us): CPU power taken from the GPU's budget. */
static nvn_encode_fifo g_rs_fifo;
static _Atomic int g_rs_on = -1;
static _Atomic int g_rs_sleeping;          /* B announced it is about to sleep (seq_cst handshake with the publisher) */
static dispatch_semaphore_t g_rs_sem;
static int g_rs_created, g_rs_configured, g_rs_active;
static id g_rs_sentinel;                   /* what A sees as the encoder while B owns the pass */
static uint64_t g_rs_passes, g_rs_packets, g_rs_syncs, g_rs_sync_ns, g_rs_sync_max_ns, g_rs_full_waits, g_rs_full_ns, g_rs_subs;
static _Atomic uint64_t g_rs_b_sleeps, g_rs_b_passes, g_rs_b_busy_ns;   /* busy: from the first packet after an idle spell to the next idle spell */
static int g_rs_probe_negative;
static int tbdr_on(void);
static int enc_label_on(void);
int mfx_enabled(void);
static int rs_enabled(void) {
    int v = atomic_load_explicit(&g_rs_on, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("TOTK_RENDER_STREAM"); int expected = -1;
        /* Mac default on (2026-10-16: replay -18%, frame -3..5%, 30-minute soak clean); iOS off (A15: two performance cores). */
        atomic_compare_exchange_strong(&g_rs_on, &expected, e ? !strcmp(e, "1") : !TARGET_OS_IPHONE);
        v = atomic_load_explicit(&g_rs_on, memory_order_relaxed);
    }
    return v;
}
static inline uint64_t rs_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void rs_apply(id<MTL4RenderCommandEncoder> *enc, id<MTL4ArgumentTable> *vt, id<MTL4ArgumentTable> *ft, const nvn_encode_packet *p) {
    const uint64_t *u = p->u; id<MTL4RenderCommandEncoder> e = *enc;
    if (g_rs_probe_negative && (p->op == RS_DRAW || p->op == RS_INSTANCED || p->op == RS_INDEXED)) return;
    if (p->op != RS_BEGIN && !e) MDIE("render stream packet %llu without an encoder", (unsigned long long)p->op);
    switch (p->op) {
        case RS_BEGIN: {
            if (e) MDIE("render stream: pass begun while the previous one is open");
            MTL4RenderPassDescriptor *d = (__bridge MTL4RenderPassDescriptor *)(void *)(uintptr_t)u[0];
            id<MTL4CommandBuffer> cb = (__bridge id<MTL4CommandBuffer>)(void *)(uintptr_t)u[3];
            *vt = (__bridge id<MTL4ArgumentTable>)(void *)(uintptr_t)u[1]; *ft = (__bridge id<MTL4ArgumentTable>)(void *)(uintptr_t)u[2];
            e = [cb renderCommandEncoderWithDescriptor:d];
            if (!e) MDIE("render stream: renderCommandEncoderWithDescriptor failed");
            if (u[4] == 2) {   /* TOTK_BARRIER_NARROW: copies / compute order before every stage; earlier render work only before our fragment stage (the vertex stage overlaps the previous pass's fragment work) */
                [e barrierAfterQueueStages:MTLStageBlit | MTLStageDispatch beforeStages:MTLStageVertex | MTLStageFragment visibilityOptions:(MTL4VisibilityOptions)u[5]];
                [e barrierAfterQueueStages:MTLStageVertex | MTLStageFragment beforeStages:MTLStageFragment visibilityOptions:(MTL4VisibilityOptions)u[5]];
            } else if (u[4]) [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:(MTL4VisibilityOptions)u[5]];
            [e setArgumentTable:*vt atStages:MTLRenderStageVertex]; [e setArgumentTable:*ft atStages:MTLRenderStageFragment];
            CFRelease((CFTypeRef)(uintptr_t)u[0]);
            *enc = e; atomic_fetch_add_explicit(&g_rs_b_passes, 1, memory_order_relaxed);
            break; }
        case RS_END:
            if (u[0] == 2) [e barrierAfterStages:MTLStageVertex | MTLStageFragment beforeQueueStages:MTLStageFragment | MTLStageBlit | MTLStageDispatch visibilityOptions:(MTL4VisibilityOptions)u[1]];   /* TOTK_BARRIER_NARROW: later vertex stages do not wait */
            else if (u[0]) [e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:(MTL4VisibilityOptions)u[1]];
            [e endEncoding]; *enc = nil; break;
        case RS_PSO: [e setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)(void *)(uintptr_t)u[0]]; if (u[0]) CFRelease((CFTypeRef)(uintptr_t)u[0]); break;
        case RS_DSS: [e setDepthStencilState:(__bridge id<MTLDepthStencilState>)(void *)(uintptr_t)u[0]]; if (u[0]) CFRelease((CFTypeRef)(uintptr_t)u[0]); break;
        case RS_VIEWPORT: { MTLViewport v; memcpy(&v, u, sizeof v); [e setViewport:v]; break; }
        case RS_SCISSOR: { MTLScissorRect v; memcpy(&v, u, sizeof v); [e setScissorRect:v]; break; }
        case RS_CULL: [e setCullMode:(MTLCullMode)u[0]]; break;
        case RS_WINDING: [e setFrontFacingWinding:(MTLWinding)u[0]]; break;
        case RS_FILL: [e setTriangleFillMode:(MTLTriangleFillMode)u[0]]; break;
        case RS_BIAS: { float v[3]; memcpy(v, u, sizeof v); [e setDepthBias:v[0] slopeScale:v[1] clamp:v[2]]; break; }
        case RS_CLIP: [e setDepthClipMode:(MTLDepthClipMode)u[0]]; break;
        case RS_STENCIL: [e setStencilReferenceValue:(uint32_t)u[0]]; break;
        case RS_BLEND: { float v[4]; memcpy(v, u, sizeof v); [e setBlendColorRed:v[0] green:v[1] blue:v[2] alpha:v[3]]; break; }
        case RS_VISIBILITY: [e setVisibilityResultMode:(MTLVisibilityResultMode)u[0] offset:(NSUInteger)u[1]]; break;
        case RS_ADDRESS: [(u[0] ? *ft : *vt) setAddress:u[2] atIndex:(NSUInteger)u[1]]; break;
        case RS_DRAW: [e drawPrimitives:(MTLPrimitiveType)u[0] vertexStart:(NSUInteger)u[1] vertexCount:(NSUInteger)u[2]]; break;
        case RS_INSTANCED: [e drawPrimitives:(MTLPrimitiveType)u[0] vertexStart:(NSUInteger)u[1] vertexCount:(NSUInteger)u[2] instanceCount:(NSUInteger)u[3] baseInstance:(NSUInteger)u[4]]; break;
        case RS_INDEXED: [e drawIndexedPrimitives:(MTLPrimitiveType)u[0] indexCount:(NSUInteger)u[1] indexType:(MTLIndexType)u[2] indexBuffer:u[3] indexBufferLength:(NSUInteger)u[4] instanceCount:(NSUInteger)u[5] baseVertex:(NSInteger)u[6] baseInstance:(NSUInteger)u[7]]; break;
        case RS_STORE:
            if (u[0] == 1) [e setDepthStoreAction:(MTLStoreAction)u[1]];
            else if (u[0] == 2) [e setStencilStoreAction:(MTLStoreAction)u[1]];
            else [e setColorStoreAction:(MTLStoreAction)u[1] atIndex:(NSUInteger)u[2]];
            break;
        default: MDIE("render stream invalid operation %llu", (unsigned long long)p->op);
    }
}
static void *rs_worker(void *unused) {
    (void)unused;
    pthread_setname_np("TOTK encode stage");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    id<MTL4RenderCommandEncoder> enc = nil; id<MTL4ArgumentTable> vt = nil, ft = nil;
    uint64_t idle_since = 0, busy_since = 0;
    for (;;) {
        const nvn_encode_packet *p = nvn_encode_fifo_peek(&g_rs_fifo);
        if (!p) {
            uint64_t now = rs_now();
            if (busy_since) { atomic_fetch_add_explicit(&g_rs_b_busy_ns, now - busy_since, memory_order_relaxed); busy_since = 0; }
            if (!idle_since) idle_since = now;
            if (now - idle_since < RS_SPIN_NS) { __builtin_arm_wfe(); continue; }   /* low-power wait; A does sev after every publish */
            atomic_store_explicit(&g_rs_sleeping, 1, memory_order_seq_cst);
            atomic_thread_fence(memory_order_seq_cst);
            if (!nvn_encode_fifo_peek(&g_rs_fifo)) { dispatch_semaphore_wait(g_rs_sem, DISPATCH_TIME_FOREVER); atomic_fetch_add_explicit(&g_rs_b_sleeps, 1, memory_order_relaxed); }
            atomic_store_explicit(&g_rs_sleeping, 0, memory_order_seq_cst);
            idle_since = 0; continue;
        }
        if (!busy_since) busy_since = rs_now();
        idle_since = 0;
        @autoreleasepool { rs_apply(&enc, &vt, &ft, p); }
        nvn_encode_fifo_consume(&g_rs_fifo);
        __builtin_arm_sev();
    }
    return NULL;
}
/* Publish what A queued and wake B if it went to sleep (seq_cst store / fence / load pairs with B's sleep handshake). */
static inline void rs_publish(void) {
    nvn_encode_fifo_publish(&g_rs_fifo);
    atomic_thread_fence(memory_order_seq_cst);
    __builtin_arm_sev();
    if (atomic_load_explicit(&g_rs_sleeping, memory_order_seq_cst)) dispatch_semaphore_signal(g_rs_sem);
}
static void rs_put(nvn_encode_packet p, int publish) {
    if (!nvn_encode_fifo_push(&g_rs_fifo, &p)) {   /* ring full: B is behind; wait for it (counted: this is A time lost to B) */
        uint64_t t0 = rs_now(); g_rs_full_waits++;
        do { rs_publish(); __builtin_arm_wfe(); } while (!nvn_encode_fifo_push(&g_rs_fifo, &p));
        g_rs_full_ns += rs_now() - t0;
    }
    if (publish) rs_publish();
    g_rs_packets++;
}
static inline void rs_object(unsigned op, id object) { rs_put((nvn_encode_packet){.op = op, .u = {(uint64_t)(uintptr_t)(object ? CFBridgingRetain(object) : NULL)}}, 0); }
static void rs_copy(unsigned op, const void *data, size_t size) {
    nvn_encode_packet p = {.op = op};
    if (size > sizeof p.u) MDIE("render stream payload too large");
    memcpy(p.u, data, size); rs_put(p, 0);
}
/* A: wait until B has consumed everything queued so far. */
static void rs_sync(void) {
    if (!g_rs_created) return;
    if (g_rs_active) MDIE("render stream: sync inside a streamed pass");
    rs_publish();
    uint64_t t0 = rs_now(), produced = g_rs_fifo.produced;
    while (atomic_load_explicit(&g_rs_fifo.consumed, memory_order_acquire) != produced) {
        __builtin_arm_wfe();
        if ((rs_now() - t0) > 2000000000ull) MDIE("render stream: the encode stage did not drain in 2 s");
    }
    uint64_t dt = rs_now() - t0; g_rs_syncs++; g_rs_sync_ns += dt; if (dt > g_rs_sync_max_ns) g_rs_sync_max_ns = dt;
    if (totk_tl_on()) totk_tl(TL_RS_JOIN, dt, g_rs_syncs);
}
/* Whether the next render pass is encoded by B. Diagnostics that encode on the command buffer themselves, the TBDR
   rewrites and MetalFX stay on the replay thread. */
static int rs_stream_pass(void) {
    if (!rs_enabled() || pt_active() || tbdr_on() || enc_label_on() || mfx_enabled()) return 0;
    if (!g_rs_configured) {
        const char *negative = getenv("TOTK_PROBE_NEGATIVE");
        g_rs_probe_negative = negative && atoi(negative) == 17;
        g_rs_sentinel = [NSObject new];
        g_rs_sem = dispatch_semaphore_create(0);
        g_rs_configured = 1;
    }
    if (!g_rs_created) {
        nvn_encode_fifo_reset(&g_rs_fifo);
        pthread_t thread; pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr, 1 << 20);
        if (pthread_create(&thread, &attr, rs_worker, NULL)) MDIE("render stream worker creation failed");
        pthread_detach(thread); g_rs_created = 1;
        MLOG("[nvn] 编码线程：渲染通道的 Metal 编码在独立线程上（TOTK_RENDER_STREAM=1；flag render_stream 0 关）");
    }
    return 1;
}
/* A: hand a pass to B. Returns the sentinel the replay uses as its encoder. */
static id rs_pass_begin(MTL4RenderPassDescriptor *d, id<MTL4ArgumentTable> vt, id<MTL4ArgumentTable> ft, id<MTL4CommandBuffer> cb, int bar_start, uint64_t vis) {
    if (g_rs_active) MDIE("render stream: pass begun while another streamed pass is open");
    g_rs_active = 1; g_rs_passes++;
    rs_put((nvn_encode_packet){.op = RS_BEGIN, .u = {(uint64_t)(uintptr_t)CFBridgingRetain(d), (uint64_t)(uintptr_t)(__bridge void *)vt, (uint64_t)(uintptr_t)(__bridge void *)ft,
                                                     (uint64_t)(uintptr_t)(__bridge void *)cb, (uint64_t)bar_start, vis}}, 1);
    return g_rs_sentinel;
}
static int rs_using(id e) {
    if (!g_rs_active) return 0;
    if (e != g_rs_sentinel) MDIE("render stream encoder ownership mismatch");
    return 1;
}
static void rs_pass_end(id e, int bar_end, uint64_t vis) {
    if (!rs_using(e)) MDIE("render stream: ending a pass it does not own");
    rs_put((nvn_encode_packet){.op = RS_END, .u = {(uint64_t)bar_end, vis}}, 1);
    g_rs_active = 0;
}
/* End of a submission: B drained; a statistics line every 1800 submissions. */
static void rs_sub_end(void) {
    if (!g_rs_created) return;
    rs_sync();
    if (++g_rs_subs % 1800 == 0) {
        static uint64_t last_busy, last_full, last_packets, last_sync;
        uint64_t busy = atomic_load(&g_rs_b_busy_ns);
        MLOG("[nvn] 编码线程 至提交 %llu：通道 %llu，包 %llu（近 1800 次提交每次 %.0f 个），等待 %llu 次 / 平均 %.1f µs / 最长 %.1f µs / 近 1800 次每次 %.3f ms，"
             "环满 %llu 次 / 近 1800 次每次等 %.3f ms，线程入睡 %llu 次，编码线程忙 近 1800 次每次 %.3f ms",
             (unsigned long long)g_rs_subs, (unsigned long long)g_rs_passes, (unsigned long long)g_rs_packets, (double)(g_rs_packets - last_packets) / 1800.0,
             (unsigned long long)g_rs_syncs, g_rs_syncs ? (double)g_rs_sync_ns / (double)g_rs_syncs / 1e3 : 0.0, (double)g_rs_sync_max_ns / 1e3,
             (double)(g_rs_sync_ns - last_sync) / 1800.0 / 1e6, (unsigned long long)g_rs_full_waits, (double)(g_rs_full_ns - last_full) / 1800.0 / 1e6,
             (unsigned long long)atomic_load(&g_rs_b_sleeps), (double)(busy - last_busy) / 1800.0 / 1e6);
        last_busy = busy; last_full = g_rs_full_ns; last_packets = g_rs_packets; last_sync = g_rs_sync_ns;
        g_rs_sync_max_ns = 0;
    }
}
