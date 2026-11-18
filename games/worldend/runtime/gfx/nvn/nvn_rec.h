// Deferred encoding (DM_NVN_ENC_THREAD=1; included by nvn_mtl.m only): the replay's Metal encoding runs on a worker thread while the submit thread goes on translating.
// About 40% of the submit thread's time was spent inside Metal itself: encoder creation, draw calls, state setters and argument table writes (Breath of the Wild: about 2.7 of 6.5 ms a frame).
// With this switch the replay talks to stand-ins for the main command buffer, its encoders and the argument tables it rewrites per draw. Each call becomes a fixed-size record (objects retained);
// records go to the encode thread in blocks, and the encode thread makes the same calls in the same order on the real objects. Commits of the main command buffer and of upload batches, and
// event signals, are records too, so the queue sees work in exactly the order it did before. The submit thread waits for the encode thread to finish (rec_drain) at the end of each submission
// and before anything that waits for the GPU or uses another queue, so nothing outside the submission can tell the difference.
// Everything else the replay creates (textures, views, pipelines, buffers, residency) stays on the submit thread: device calls, safe from any thread. Queue commits take the residency lock,
// since the submit thread may be adding to the set at the same time.
// A selector the stand-ins do not implement stops the host with its name (forwardingTargetForSelector), so a missing one cannot silently drop work.

// ---------- Copy queue: large uniform shadows (ubp_get) are filled by a worker thread ----------
// A shadow is the bytes of a uniform range as the GPU will read them at that point of the submission (nvn_mtl.m ubp_get): memory, or the previous shadow of the range, with the inline
// updates queued since laid over it. Breath of the Wild makes about 440 a frame, 14 MB of copies, a tenth of the submit thread's time. The submit thread queues the copies in order and
// goes on; one worker makes them in the same order (a shadow may start from the previous one). The GPU reads shadows only after its command buffer is committed, and every commit first
// waits for the copies queued before it (ubc_wait, from the encode thread when Metal encoding is deferred); a CPU read of a shadow waits for its own copies. DM_NVN_UB_COPY_THREAD=0
// copies on the submit thread as before
typedef struct { void *d; const void *s; size_t n; } ubc_op;
enum { UBC_N = 8192 };
static ubc_op g_ubc[UBC_N];
static _Atomic uint64_t g_ubc_head, g_ubc_done;   // ops queued / made (monotonic)
static _Atomic int g_ubc_sleeping;
static pthread_mutex_t g_ubc_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_ubc_cv = PTHREAD_COND_INITIALIZER;
static int ubc_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_UB_COPY_THREAD"); v = !(e && !strcmp(e, "0")); } return v; }
static void *ubc_thread(void *arg) {
    (void)arg; pthread_setname_np("dm-ub-copy"); pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    for (uint64_t t = 0;;) {
        uint64_t h = atomic_load_explicit(&g_ubc_head, memory_order_acquire);
        if (t == h) {
            for (int spin = 0; spin < 2000 && t == h; spin++) h = atomic_load_explicit(&g_ubc_head, memory_order_acquire);   // a submission queues copies in bursts: stay awake a moment
            if (t == h) { pthread_mutex_lock(&g_ubc_mu); atomic_store(&g_ubc_sleeping, 1);
                while ((h = atomic_load_explicit(&g_ubc_head, memory_order_acquire)) == t) pthread_cond_wait(&g_ubc_cv, &g_ubc_mu);
                atomic_store(&g_ubc_sleeping, 0); pthread_mutex_unlock(&g_ubc_mu); }
        }
        for (; t < h; t++) { const ubc_op *o = &g_ubc[t % UBC_N]; memcpy(o->d, o->s, o->n); }
        atomic_store_explicit(&g_ubc_done, t, memory_order_release);
    }
    return NULL;
}
// Wait until the first seq ops are made
static void ubc_wait(uint64_t seq) {
    if (atomic_load_explicit(&g_ubc_done, memory_order_acquire) >= seq) return;
    while (atomic_load_explicit(&g_ubc_done, memory_order_acquire) < seq) {
        if (atomic_load(&g_ubc_sleeping)) { pthread_mutex_lock(&g_ubc_mu); pthread_cond_signal(&g_ubc_cv); pthread_mutex_unlock(&g_ubc_mu); }
        __builtin_arm_yield();
    }
}
static inline uint64_t ubc_head(void) { return atomic_load_explicit(&g_ubc_head, memory_order_relaxed); }
// Queue a copy (submit thread only); returns the count of ops that must be made for it to be done
static uint64_t ubc_push(void *d, const void *s, size_t n) {
    static int started;
    if (!started) { pthread_t th; if (pthread_create(&th, NULL, ubc_thread, NULL)) MDIE("cannot start the uniform copy thread"); pthread_detach(th); started = 1; }
    uint64_t h = atomic_load_explicit(&g_ubc_head, memory_order_relaxed);
    if (h - atomic_load_explicit(&g_ubc_done, memory_order_acquire) >= UBC_N) ubc_wait(h - UBC_N + 1);
    g_ubc[h % UBC_N] = (ubc_op){d, s, n};
    atomic_store_explicit(&g_ubc_head, h + 1, memory_order_release);
    if (atomic_load(&g_ubc_sleeping)) { pthread_mutex_lock(&g_ubc_mu); pthread_cond_signal(&g_ubc_cv); pthread_mutex_unlock(&g_ubc_mu); }
    return h + 1;
}

enum {
    RO_CB_NEW, RO_CB_BEGIN, RO_CB_END, RO_COMMIT, RO_COMMIT_REAL, RO_SIGNAL,
    RO_RE_BEGIN, RO_CE_BEGIN, RO_END,
    RO_BAR_QUEUE, RO_BAR_AFTER_STAGES, RO_BAR_ENCODER,
    RO_R_TABLE, RO_R_PSO, RO_R_DSS, RO_R_VIEWPORT, RO_R_SCISSOR, RO_R_CULL, RO_R_WINDING, RO_R_FILL, RO_R_BIAS, RO_R_CLIP, RO_R_SREF, RO_R_BLEND, RO_R_VIS,
    RO_R_DRAW, RO_R_DRAW_INST, RO_R_DRAW_IND, RO_R_DRAW_IDX, RO_R_DRAW_IDX_IND, RO_R_STORE_C, RO_R_STORE_D, RO_R_STORE_S,
    RO_C_TABLE, RO_C_PSO, RO_C_DISPATCH, RO_C_DISPATCH_IND, RO_C_COPY_BB, RO_C_COPY_BT, RO_C_COPY_TB, RO_C_COPY_TT, RO_C_FILL,
    RO_T_ADDR, RO_T_TEX, RO_T_SAMP,
    RO_LABEL, RO_R_TS, RO_C_TS,   // diagnostics: DM_NVN_ENC_LABEL, DM_NVN_GPUPROF
};
typedef struct { uint32_t op, nobj; uint64_t a[15]; } rec_t;   // the first nobj slots hold retained objects (0 for nil), released once the call is made
enum { REC_BLK = 256 };
typedef struct rec_blk { struct rec_blk *next; int n; rec_t r[REC_BLK]; } rec_blk;

// On by default on iOS: Breath of the Wild on an iPhone 17 Pro Max (handheld, the Great Plateau) took about 8 ms of submit thread per frame without it and 3.3–4.5 ms with it
static int g_rec = -1;
static inline int rec_on(void) {
    if (__builtin_expect(g_rec >= 0, 1)) return g_rec;
    const char *e = getenv("DM_NVN_ENC_THREAD");
#if TARGET_OS_IPHONE
    g_rec = !e || atoi(e) > 0;
#else
    g_rec = e && atoi(e) > 0;
#endif
    if (g_rec) MLOG("Metal encoding runs on its own thread (DM_NVN_ENC_THREAD=0 turns it off)");
    return g_rec;
}
static pthread_mutex_t g_rb_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_rb_work = PTHREAD_COND_INITIALIZER, g_rb_idle = PTHREAD_COND_INITIALIZER;
static rec_blk *g_rb_cur, *g_rb_head, *g_rb_tail, *g_rb_free;   // the submit thread's open block; blocks waiting for the encode thread; empty blocks
static int g_rb_busy, g_rb_started, g_rb_sleeping;              // the encode thread is working through a block; the thread exists; it waits for work

static inline uint64_t rec_obj(id o) { return o ? (uint64_t)(uintptr_t)CFBridgingRetain(o) : 0; }
static inline id rec_id(uint64_t v) { return (__bridge id)(void *)(uintptr_t)v; }
static inline uint64_t rec_f(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float rec_fv(uint64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }

// ---------- The encode thread ----------
static id g_rx_cb, g_rx_enc;   // the encode thread's real command buffer and open encoder
static void rec_exec(const rec_t *r) {
    const uint64_t *a = r->a; id e = g_rx_enc;
    switch (r->op) {
    case RO_CB_NEW: g_rx_cb = [g_dev newCommandBuffer]; break;
    case RO_CB_BEGIN: [(id<MTL4CommandBuffer>)g_rx_cb beginCommandBufferWithAllocator:rec_id(a[0])]; break;
    case RO_CB_END: [(id<MTL4CommandBuffer>)g_rx_cb endCommandBuffer]; break;
    case RO_COMMIT: case RO_COMMIT_REAL: {
        if (r->op == RO_COMMIT) ubc_wait(a[2]);   // the uniform shadows this command buffer reads
        id<MTL4CommandBuffer> buffers[] = {r->op == RO_COMMIT ? g_rx_cb : rec_id(a[1])}; MTL4CommitOptions *o = rec_id(a[0]);
        pthread_mutex_lock(&g_res_mu);
        if (o) [g_q4 commit:buffers count:1 options:o]; else [g_q4 commit:buffers count:1];
        pthread_mutex_unlock(&g_res_mu);
        if (r->op == RO_COMMIT) g_rx_cb = nil;
        break; }
    case RO_SIGNAL: [g_q4 signalEvent:rec_id(a[0]) value:a[1]]; break;
    case RO_RE_BEGIN: g_rx_enc = [(id<MTL4CommandBuffer>)g_rx_cb renderCommandEncoderWithDescriptor:rec_id(a[0])]; break;
    case RO_CE_BEGIN: g_rx_enc = [(id<MTL4CommandBuffer>)g_rx_cb computeCommandEncoder]; break;
    case RO_END: [(id<MTL4CommandEncoder>)e endEncoding]; g_rx_enc = nil; break;
    case RO_BAR_QUEUE: [(id<MTL4CommandEncoder>)e barrierAfterQueueStages:a[0] beforeStages:a[1] visibilityOptions:a[2]]; break;
    case RO_BAR_AFTER_STAGES: [(id<MTL4CommandEncoder>)e barrierAfterStages:a[0] beforeQueueStages:a[1] visibilityOptions:a[2]]; break;
    case RO_BAR_ENCODER: [(id<MTL4CommandEncoder>)e barrierAfterEncoderStages:a[0] beforeEncoderStages:a[1] visibilityOptions:a[2]]; break;
    case RO_R_TABLE: [(id<MTL4RenderCommandEncoder>)e setArgumentTable:rec_id(a[0]) atStages:a[1]]; break;
    case RO_R_PSO: [(id<MTL4RenderCommandEncoder>)e setRenderPipelineState:rec_id(a[0])]; break;
    case RO_R_DSS: [(id<MTL4RenderCommandEncoder>)e setDepthStencilState:rec_id(a[0])]; break;
    case RO_R_VIEWPORT: { MTLViewport v; memcpy(&v, a, sizeof v); [(id<MTL4RenderCommandEncoder>)e setViewport:v]; break; }
    case RO_R_SCISSOR: { MTLScissorRect s; memcpy(&s, a, sizeof s); [(id<MTL4RenderCommandEncoder>)e setScissorRect:s]; break; }
    case RO_R_CULL: [(id<MTL4RenderCommandEncoder>)e setCullMode:a[0]]; break;
    case RO_R_WINDING: [(id<MTL4RenderCommandEncoder>)e setFrontFacingWinding:a[0]]; break;
    case RO_R_FILL: [(id<MTL4RenderCommandEncoder>)e setTriangleFillMode:a[0]]; break;
    case RO_R_BIAS: [(id<MTL4RenderCommandEncoder>)e setDepthBias:rec_fv(a[0]) slopeScale:rec_fv(a[1]) clamp:rec_fv(a[2])]; break;
    case RO_R_CLIP: [(id<MTL4RenderCommandEncoder>)e setDepthClipMode:a[0]]; break;
    case RO_R_SREF: [(id<MTL4RenderCommandEncoder>)e setStencilReferenceValue:(uint32_t)a[0]]; break;
    case RO_R_BLEND: [(id<MTL4RenderCommandEncoder>)e setBlendColorRed:rec_fv(a[0]) green:rec_fv(a[1]) blue:rec_fv(a[2]) alpha:rec_fv(a[3])]; break;
    case RO_R_VIS: [(id<MTL4RenderCommandEncoder>)e setVisibilityResultMode:a[0] offset:a[1]]; break;
    case RO_R_DRAW: [(id<MTL4RenderCommandEncoder>)e drawPrimitives:a[0] vertexStart:a[1] vertexCount:a[2]]; break;
    case RO_R_DRAW_INST: [(id<MTL4RenderCommandEncoder>)e drawPrimitives:a[0] vertexStart:a[1] vertexCount:a[2] instanceCount:a[3] baseInstance:a[4]]; break;
    case RO_R_DRAW_IND: [(id<MTL4RenderCommandEncoder>)e drawPrimitives:a[0] indirectBuffer:a[1]]; break;
    case RO_R_DRAW_IDX: [(id<MTL4RenderCommandEncoder>)e drawIndexedPrimitives:a[0] indexCount:a[1] indexType:a[2] indexBuffer:a[3] indexBufferLength:a[4] instanceCount:a[5] baseVertex:(NSInteger)a[6] baseInstance:a[7]]; break;
    case RO_R_DRAW_IDX_IND: [(id<MTL4RenderCommandEncoder>)e drawIndexedPrimitives:a[0] indexType:a[1] indexBuffer:a[2] indexBufferLength:a[3] indirectBuffer:a[4]]; break;
    case RO_R_STORE_C: [(id<MTL4RenderCommandEncoder>)e setColorStoreAction:a[0] atIndex:a[1]]; break;
    case RO_R_STORE_D: [(id<MTL4RenderCommandEncoder>)e setDepthStoreAction:a[0]]; break;
    case RO_R_STORE_S: [(id<MTL4RenderCommandEncoder>)e setStencilStoreAction:a[0]]; break;
    case RO_C_TABLE: [(id<MTL4ComputeCommandEncoder>)e setArgumentTable:rec_id(a[0])]; break;
    case RO_C_PSO: [(id<MTL4ComputeCommandEncoder>)e setComputePipelineState:rec_id(a[0])]; break;
    case RO_C_DISPATCH: [(id<MTL4ComputeCommandEncoder>)e dispatchThreadgroups:MTLSizeMake(a[0], a[1], a[2]) threadsPerThreadgroup:MTLSizeMake(a[3], a[4], a[5])]; break;
    case RO_C_DISPATCH_IND: [(id<MTL4ComputeCommandEncoder>)e dispatchThreadgroupsWithIndirectBuffer:a[0] threadsPerThreadgroup:MTLSizeMake(a[1], a[2], a[3])]; break;
    case RO_C_COPY_BB: [(id<MTL4ComputeCommandEncoder>)e copyFromBuffer:rec_id(a[0]) sourceOffset:a[2] toBuffer:rec_id(a[1]) destinationOffset:a[3] size:a[4]]; break;
    case RO_C_COPY_BT: [(id<MTL4ComputeCommandEncoder>)e copyFromBuffer:rec_id(a[0]) sourceOffset:a[2] sourceBytesPerRow:a[3] sourceBytesPerImage:a[4] sourceSize:MTLSizeMake(a[5], a[6], a[7])
                           toTexture:rec_id(a[1]) destinationSlice:a[8] destinationLevel:a[9] destinationOrigin:MTLOriginMake(a[10], a[11], a[12])]; break;
    case RO_C_COPY_TB: [(id<MTL4ComputeCommandEncoder>)e copyFromTexture:rec_id(a[0]) sourceSlice:a[2] sourceLevel:a[3] sourceOrigin:MTLOriginMake(a[4], a[5], a[6]) sourceSize:MTLSizeMake(a[7], a[8], a[9])
                           toBuffer:rec_id(a[1]) destinationOffset:a[10] destinationBytesPerRow:a[11] destinationBytesPerImage:a[12]]; break;
    case RO_C_COPY_TT: [(id<MTL4ComputeCommandEncoder>)e copyFromTexture:rec_id(a[0]) sourceSlice:a[2] sourceLevel:a[3] sourceOrigin:MTLOriginMake(a[4], a[5], a[6]) sourceSize:MTLSizeMake(a[7], a[8], a[9])
                           toTexture:rec_id(a[1]) destinationSlice:a[10] destinationLevel:a[11] destinationOrigin:MTLOriginMake(a[12], a[13], a[14])]; break;
    case RO_C_FILL: [(id<MTL4ComputeCommandEncoder>)e fillBuffer:rec_id(a[0]) range:NSMakeRange(a[1], a[2]) value:(uint8_t)a[3]]; break;
    case RO_T_ADDR: [(id<MTL4ArgumentTable>)rec_id(a[0]) setAddress:a[1] atIndex:a[2]]; break;
    case RO_T_TEX: [(id<MTL4ArgumentTable>)rec_id(a[0]) setTexture:(MTLResourceID){a[1]} atIndex:a[2]]; break;
    case RO_T_SAMP: [(id<MTL4ArgumentTable>)rec_id(a[0]) setSamplerState:(MTLResourceID){a[1]} atIndex:a[2]]; break;
    case RO_LABEL: [(id<MTL4CommandEncoder>)e setLabel:rec_id(a[0])]; break;
    case RO_R_TS: [(id<MTL4RenderCommandEncoder>)e writeTimestampWithGranularity:a[1] afterStage:a[2] intoHeap:rec_id(a[0]) atIndex:a[3]]; break;
    case RO_C_TS: [(id<MTL4ComputeCommandEncoder>)e writeTimestampWithGranularity:a[1] intoHeap:rec_id(a[0]) atIndex:a[2]]; break;
    default: MDIE("deferred encoding: unknown record %u", r->op);
    }
    for (uint32_t i = 0; i < r->nobj; i++) if (a[i]) CFRelease((CFTypeRef)(uintptr_t)a[i]);
}
static void *rec_thread(void *arg) {
    (void)arg; pthread_setname_np("dm-encode");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    for (;;) {
        pthread_mutex_lock(&g_rb_mu);
        while (!g_rb_head) { g_rb_sleeping = 1; pthread_cond_wait(&g_rb_work, &g_rb_mu); g_rb_sleeping = 0; }
        rec_blk *b = g_rb_head; g_rb_head = b->next; if (!g_rb_head) g_rb_tail = NULL; g_rb_busy = 1;
        pthread_mutex_unlock(&g_rb_mu);
        @autoreleasepool { for (int i = 0; i < b->n; i++) rec_exec(&b->r[i]); }
        pthread_mutex_lock(&g_rb_mu);
        b->n = 0; b->next = g_rb_free; g_rb_free = b; g_rb_busy = 0;
        if (!g_rb_head) pthread_cond_broadcast(&g_rb_idle);
        pthread_mutex_unlock(&g_rb_mu);
    }
    return NULL;
}

// ---------- The submit thread's side ----------
static void rec_publish(void) {
    if (!g_rb_cur || !g_rb_cur->n) return;
    pthread_mutex_lock(&g_rb_mu);
    g_rb_cur->next = NULL; if (g_rb_tail) g_rb_tail->next = g_rb_cur; else g_rb_head = g_rb_cur; g_rb_tail = g_rb_cur; g_rb_cur = NULL;
    if (g_rb_sleeping) pthread_cond_signal(&g_rb_work);
    pthread_mutex_unlock(&g_rb_mu);
}
static rec_t *rec_put(uint32_t op, uint32_t nobj) {
    if (g_rb_cur && g_rb_cur->n == REC_BLK) rec_publish();
    if (!g_rb_cur) {
        pthread_mutex_lock(&g_rb_mu);
        if (!g_rb_started) { pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1 << 20);
            if (pthread_create(&th, &at, rec_thread, NULL)) MDIE("deferred encoding: cannot start the encode thread"); pthread_attr_destroy(&at); g_rb_started = 1; }
        g_rb_cur = g_rb_free; if (g_rb_cur) g_rb_free = g_rb_cur->next;
        pthread_mutex_unlock(&g_rb_mu);
        if (!g_rb_cur) { g_rb_cur = malloc(sizeof *g_rb_cur); if (!g_rb_cur) MDIE("deferred encoding: out of memory"); }
        g_rb_cur->n = 0;
    }
    rec_t *r = &g_rb_cur->r[g_rb_cur->n++]; r->op = op; r->nobj = nobj; return r;
}
// Hand over what is recorded and wait until the encode thread has made every call
static void rec_drain(void) {
    if (!g_rb_started) return;
    rec_publish();
    pthread_mutex_lock(&g_rb_mu);
    while (g_rb_head || g_rb_busy) pthread_cond_wait(&g_rb_idle, &g_rb_mu);
    pthread_mutex_unlock(&g_rb_mu);
}
static __attribute__((noreturn)) void rec_missing(id o, SEL s) { MDIE("deferred encoding: %s has no stand-in for -%s (DM_NVN_ENC_THREAD=0 runs without it)", object_getClassName(o), sel_getName(s)); }

// Argument tables: every stand-in is made once and kept for good (rec_table), so the real table is passed without a retain
@interface DmRecTable : NSObject { @public id real; } @end
static inline void rec_table_op(uint32_t op, id t, uint64_t v, NSUInteger i) { rec_t *r = rec_put(op, 0); r->a[0] = (uint64_t)(uintptr_t)(__bridge void *)((DmRecTable *)t)->real; r->a[1] = v; r->a[2] = i; }
@implementation DmRecTable
- (void)setAddress:(uint64_t)gpu atIndex:(NSUInteger)i { rec_table_op(RO_T_ADDR, self, gpu, i); }
- (void)setTexture:(MTLResourceID)t atIndex:(NSUInteger)i { rec_table_op(RO_T_TEX, self, t._impl, i); }
- (void)setSamplerState:(MTLResourceID)t atIndex:(NSUInteger)i { rec_table_op(RO_T_SAMP, self, t._impl, i); }
- (id)forwardingTargetForSelector:(SEL)s { rec_missing(self, s); }
@end
static inline id rec_table_real(id t) { return [t isKindOfClass:[DmRecTable class]] ? ((DmRecTable *)t)->real : t; }

@interface DmRecEnc : NSObject @end
@implementation DmRecEnc
- (BOOL)conformsToProtocol:(Protocol *)p { return p == @protocol(MTL4CommandEncoder) || p == @protocol(MTL4RenderCommandEncoder) || p == @protocol(MTL4ComputeCommandEncoder) || [super conformsToProtocol:p]; }
- (id)forwardingTargetForSelector:(SEL)s { rec_missing(self, s); }
- (void)endEncoding { rec_put(RO_END, 0); }
- (void)barrierAfterQueueStages:(MTLStages)a beforeStages:(MTLStages)b visibilityOptions:(MTL4VisibilityOptions)v { rec_t *r = rec_put(RO_BAR_QUEUE, 0); r->a[0] = a; r->a[1] = b; r->a[2] = v; }
- (void)barrierAfterStages:(MTLStages)a beforeQueueStages:(MTLStages)b visibilityOptions:(MTL4VisibilityOptions)v { rec_t *r = rec_put(RO_BAR_AFTER_STAGES, 0); r->a[0] = a; r->a[1] = b; r->a[2] = v; }
- (void)barrierAfterEncoderStages:(MTLStages)a beforeEncoderStages:(MTLStages)b visibilityOptions:(MTL4VisibilityOptions)v { rec_t *r = rec_put(RO_BAR_ENCODER, 0); r->a[0] = a; r->a[1] = b; r->a[2] = v; }
// Render
- (void)setArgumentTable:(id)t atStages:(MTLRenderStages)s { id x = rec_table_real(t); int keep = x != t; rec_t *r = rec_put(RO_R_TABLE, !keep); r->a[0] = keep ? (uint64_t)(uintptr_t)(__bridge void *)x : rec_obj(x); r->a[1] = s; }
- (void)setRenderPipelineState:(id)p { rec_t *r = rec_put(RO_R_PSO, 1); r->a[0] = rec_obj(p); }
- (void)setDepthStencilState:(id)s { rec_t *r = rec_put(RO_R_DSS, 1); r->a[0] = rec_obj(s); }
- (void)setViewport:(MTLViewport)v { rec_t *r = rec_put(RO_R_VIEWPORT, 0); memcpy(r->a, &v, sizeof v); }
- (void)setScissorRect:(MTLScissorRect)s { rec_t *r = rec_put(RO_R_SCISSOR, 0); memcpy(r->a, &s, sizeof s); }
- (void)setCullMode:(MTLCullMode)m { rec_put(RO_R_CULL, 0)->a[0] = m; }
- (void)setFrontFacingWinding:(MTLWinding)w { rec_put(RO_R_WINDING, 0)->a[0] = w; }
- (void)setTriangleFillMode:(MTLTriangleFillMode)m { rec_put(RO_R_FILL, 0)->a[0] = m; }
- (void)setDepthBias:(float)u slopeScale:(float)s clamp:(float)c { rec_t *r = rec_put(RO_R_BIAS, 0); r->a[0] = rec_f(u); r->a[1] = rec_f(s); r->a[2] = rec_f(c); }
- (void)setDepthClipMode:(MTLDepthClipMode)m { rec_put(RO_R_CLIP, 0)->a[0] = m; }
- (void)setStencilReferenceValue:(uint32_t)v { rec_put(RO_R_SREF, 0)->a[0] = v; }
- (void)setBlendColorRed:(float)r_ green:(float)g blue:(float)b alpha:(float)a { rec_t *r = rec_put(RO_R_BLEND, 0); r->a[0] = rec_f(r_); r->a[1] = rec_f(g); r->a[2] = rec_f(b); r->a[3] = rec_f(a); }
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)m offset:(NSUInteger)o { rec_t *r = rec_put(RO_R_VIS, 0); r->a[0] = m; r->a[1] = o; }
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)v vertexCount:(NSUInteger)n { rec_t *r = rec_put(RO_R_DRAW, 0); r->a[0] = p; r->a[1] = v; r->a[2] = n; }
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)v vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i baseInstance:(NSUInteger)b {
    rec_t *r = rec_put(RO_R_DRAW_INST, 0); r->a[0] = p; r->a[1] = v; r->a[2] = n; r->a[3] = i; r->a[4] = b; }
- (void)drawPrimitives:(MTLPrimitiveType)p indirectBuffer:(uint64_t)g { rec_t *r = rec_put(RO_R_DRAW_IND, 0); r->a[0] = p; r->a[1] = g; }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)n indexType:(MTLIndexType)t indexBuffer:(uint64_t)g indexBufferLength:(NSUInteger)len instanceCount:(NSUInteger)i baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi {
    rec_t *r = rec_put(RO_R_DRAW_IDX, 0); r->a[0] = p; r->a[1] = n; r->a[2] = t; r->a[3] = g; r->a[4] = len; r->a[5] = i; r->a[6] = (uint64_t)bv; r->a[7] = bi; }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexType:(MTLIndexType)t indexBuffer:(uint64_t)g indexBufferLength:(NSUInteger)len indirectBuffer:(uint64_t)ind {
    rec_t *r = rec_put(RO_R_DRAW_IDX_IND, 0); r->a[0] = p; r->a[1] = t; r->a[2] = g; r->a[3] = len; r->a[4] = ind; }
- (void)setColorStoreAction:(MTLStoreAction)a atIndex:(NSUInteger)i { rec_t *r = rec_put(RO_R_STORE_C, 0); r->a[0] = a; r->a[1] = i; }
- (void)setDepthStoreAction:(MTLStoreAction)a { rec_put(RO_R_STORE_D, 0)->a[0] = a; }
- (void)setStencilStoreAction:(MTLStoreAction)a { rec_put(RO_R_STORE_S, 0)->a[0] = a; }
// Compute and copies (Metal 4 copies are on the compute encoder)
- (void)setArgumentTable:(id)t { id x = rec_table_real(t); int keep = x != t; rec_t *r = rec_put(RO_C_TABLE, !keep); r->a[0] = keep ? (uint64_t)(uintptr_t)(__bridge void *)x : rec_obj(x); }
- (void)setComputePipelineState:(id)p { rec_t *r = rec_put(RO_C_PSO, 1); r->a[0] = rec_obj(p); }
- (void)dispatchThreadgroups:(MTLSize)n threadsPerThreadgroup:(MTLSize)t { rec_t *r = rec_put(RO_C_DISPATCH, 0); r->a[0] = n.width; r->a[1] = n.height; r->a[2] = n.depth; r->a[3] = t.width; r->a[4] = t.height; r->a[5] = t.depth; }
- (void)dispatchThreadgroupsWithIndirectBuffer:(uint64_t)g threadsPerThreadgroup:(MTLSize)t { rec_t *r = rec_put(RO_C_DISPATCH_IND, 0); r->a[0] = g; r->a[1] = t.width; r->a[2] = t.height; r->a[3] = t.depth; }
- (void)copyFromBuffer:(id)s sourceOffset:(NSUInteger)so toBuffer:(id)d destinationOffset:(NSUInteger)doff size:(NSUInteger)n {
    rec_t *r = rec_put(RO_C_COPY_BB, 2); r->a[0] = rec_obj(s); r->a[1] = rec_obj(d); r->a[2] = so; r->a[3] = doff; r->a[4] = n; }
- (void)copyFromBuffer:(id)s sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)img sourceSize:(MTLSize)z toTexture:(id)d destinationSlice:(NSUInteger)sl destinationLevel:(NSUInteger)lv destinationOrigin:(MTLOrigin)o {
    rec_t *r = rec_put(RO_C_COPY_BT, 2); r->a[0] = rec_obj(s); r->a[1] = rec_obj(d); r->a[2] = so; r->a[3] = row; r->a[4] = img; r->a[5] = z.width; r->a[6] = z.height; r->a[7] = z.depth;
    r->a[8] = sl; r->a[9] = lv; r->a[10] = o.x; r->a[11] = o.y; r->a[12] = o.z; }
- (void)copyFromTexture:(id)s sourceSlice:(NSUInteger)sl sourceLevel:(NSUInteger)lv sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)z toBuffer:(id)d destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)img {
    rec_t *r = rec_put(RO_C_COPY_TB, 2); r->a[0] = rec_obj(s); r->a[1] = rec_obj(d); r->a[2] = sl; r->a[3] = lv; r->a[4] = o.x; r->a[5] = o.y; r->a[6] = o.z; r->a[7] = z.width; r->a[8] = z.height; r->a[9] = z.depth;
    r->a[10] = doff; r->a[11] = row; r->a[12] = img; }
- (void)copyFromTexture:(id)s sourceSlice:(NSUInteger)sl sourceLevel:(NSUInteger)lv sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)z toTexture:(id)d destinationSlice:(NSUInteger)dsl destinationLevel:(NSUInteger)dlv destinationOrigin:(MTLOrigin)dorg {
    rec_t *r = rec_put(RO_C_COPY_TT, 2); r->a[0] = rec_obj(s); r->a[1] = rec_obj(d); r->a[2] = sl; r->a[3] = lv; r->a[4] = o.x; r->a[5] = o.y; r->a[6] = o.z; r->a[7] = z.width; r->a[8] = z.height; r->a[9] = z.depth;
    r->a[10] = dsl; r->a[11] = dlv; r->a[12] = dorg.x; r->a[13] = dorg.y; r->a[14] = dorg.z; }
- (void)fillBuffer:(id)b range:(NSRange)rg value:(uint8_t)v { rec_t *r = rec_put(RO_C_FILL, 1); r->a[0] = rec_obj(b); r->a[1] = rg.location; r->a[2] = rg.length; r->a[3] = v; }
// Diagnostics: encoder labels (DM_NVN_ENC_LABEL) and timestamps into a counter heap (DM_NVN_GPUPROF); the render form names a stage, the compute form none
- (void)setLabel:(NSString *)l { rec_put(RO_LABEL, 1)->a[0] = rec_obj(l); }
- (void)writeTimestampWithGranularity:(MTL4TimestampGranularity)g afterStage:(MTLRenderStages)st intoHeap:(id)h atIndex:(NSUInteger)i { rec_t *r = rec_put(RO_R_TS, 1); r->a[0] = rec_obj(h); r->a[1] = g; r->a[2] = st; r->a[3] = i; }
- (void)writeTimestampWithGranularity:(MTL4TimestampGranularity)g intoHeap:(id)h atIndex:(NSUInteger)i { rec_t *r = rec_put(RO_C_TS, 1); r->a[0] = rec_obj(h); r->a[1] = g; r->a[2] = i; }
@end

@interface DmRecCB : NSObject @end
@implementation DmRecCB
- (id)forwardingTargetForSelector:(SEL)s { rec_missing(self, s); }
- (void)beginCommandBufferWithAllocator:(id)a { rec_put(RO_CB_BEGIN, 1)->a[0] = rec_obj(a); }
- (void)endCommandBuffer { rec_put(RO_CB_END, 0); }
- (id)renderCommandEncoderWithDescriptor:(MTL4RenderPassDescriptor *)d { rec_put(RO_RE_BEGIN, 1)->a[0] = rec_obj([d copy]); return [DmRecEnc new]; }   // copied: the replay may reuse or change the descriptor afterwards
- (id)computeCommandEncoder { rec_put(RO_CE_BEGIN, 0); return [DmRecEnc new]; }
@end

// The replay's entry points
static id rec_cb_new(void) { if (!rec_on()) return [g_dev newCommandBuffer]; rec_put(RO_CB_NEW, 0); return [DmRecCB new]; }
static id rec_table(id t) { if (!rec_on() || !t) return t; DmRecTable *s = [DmRecTable new]; s->real = t; return s; }
// The replay's main command buffer: committed once the uniform shadows queued so far are made (ubc_wait)
static void rec_commit(id cb, MTL4CommitOptions *o) {
    if ([cb isKindOfClass:[DmRecCB class]]) { rec_t *r = rec_put(RO_COMMIT, 1); r->a[0] = rec_obj(o); r->a[2] = ubc_head(); return; }
    ubc_wait(ubc_head());
    id<MTL4CommandBuffer> buffers[] = {cb};
    if (o) [g_q4 commit:buffers count:1 options:o]; else [g_q4 commit:buffers count:1];
}
// An upload batch: its place in the queue is after what is recorded so far (it reads no uniform shadow)
static void rec_commit_upload(id cb, MTL4CommitOptions *o) {
    if (rec_on() && g_rb_started) { rec_t *r = rec_put(RO_COMMIT_REAL, 2); r->a[0] = rec_obj(o); r->a[1] = rec_obj(cb); return; }
    id<MTL4CommandBuffer> buffers[] = {cb};
    if (o) [g_q4 commit:buffers count:1 options:o]; else [g_q4 commit:buffers count:1];
}
static void rec_signal(id ev, uint64_t v) { if (rec_on() && g_rb_started) { rec_t *r = rec_put(RO_SIGNAL, 1); r->a[0] = rec_obj(ev); r->a[1] = v; } else [g_q4 signalEvent:ev value:v]; }
