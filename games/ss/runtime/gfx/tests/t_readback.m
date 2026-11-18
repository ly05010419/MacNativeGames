// NVN readback contract: a pending clear must reach both GPU buffer copies and CPU reads.
// Expected 8x4 RGBA values are literal bytes, independent of the Metal replay's state.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int cases, failed;
#define EXPECT(n,w,v) do { int a=(w), b=(v); cases++; printf("%s expected %d -> got %d %s\n",n,a,b,a==b?"PASS":"FAIL"); failed+=a!=b; } while (0)

// Observe the real readback buffer without retaining it. The caller's outer pool stays open,
// as on a long-lived guest thread; ReadTexels must drain its own temporary Metal objects.
@interface ReadbackDevice : NSObject
@property(nonatomic, strong) id<MTLDevice> device;
@property(nonatomic, weak) id<MTLBuffer> lastBuffer;
@property(nonatomic) NSUInteger allocations;
@property(nonatomic) NSUInteger observedLength;   // 0 observes every buffer; otherwise only the readback's exact size.
@end
@implementation ReadbackDevice
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    id<MTLBuffer> buffer = [_device newBufferWithLength:length options:options];
    if (!_observedLength || length == _observedLength) { _lastBuffer = buffer; _allocations++; }
    return buffer;
}
- (id)forwardingTargetForSelector:(SEL)selector { (void)selector; return _device; }
@end

@interface ReadbackCompletion : NSObject {
@public
    const NVNsync *sync;
    const NVNtexture *texture;
    const NVNevent *event;
    NVNcopyRegion region;
    dispatch_semaphore_t feedbackEntered, feedbackRelease, waiterStarted, waiterDone, poolRelease, waiterExited;
    _Atomic int gateTimedOut;
    NVNsyncWaitResult waitResult;
    int badPixels;
    uint32_t eventValue;
}
@end
@implementation ReadbackCompletion @end

static void readback_wait_for(const char *name, dispatch_semaphore_t semaphore) {
    int ok = dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 5000000000ll)) == 0;
    EXPECT(name, 1, ok);
    if (!ok) exit(1);   // Never join a stuck waiter or leave a failed gate hanging the test process.
}
static void readback_feedback_idle(void) {
    dispatch_semaphore_t idle = dispatch_semaphore_create(0);
    dispatch_async(g_feedback_q, ^{ dispatch_semaphore_signal(idle); });
    readback_wait_for("readback feedback queue reached its bounded checkpoint", idle);
}
static int readback_buffer_gone(ReadbackDevice *device) { @autoreleasepool {
    // A weak property's getter can itself autorelease the object; do not prolong a live observation in the test's outer pool.
    return device.lastBuffer == nil;
} }
static void *readback_guest_waiter(void *arg) {
    ReadbackCompletion *c = (__bridge ReadbackCompletion *)arg;
    @autoreleasepool {
        dispatch_semaphore_signal(c->waiterStarted);
        c->waitResult = h_nvnSyncWait(c->sync, 5000000000ull);
        c->badPixels = 32; c->eventValue = 0;
        if (c->waitResult == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || c->waitResult == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED) {
            static const uint8_t expected[] = {51, 102, 153, 255};
            uint8_t cpu[128]; memset(cpu, 0xEE, sizeof cpu);
            h_nvnTextureReadTexels(c->texture, NULL, &c->region, cpu);
            c->badPixels = 0;
            for (int i = 0; i < 32; i++) c->badPixels += memcmp(cpu + i * 4, expected, 4) != 0;
            c->eventValue = h_nvnEventGetValue(c->event);
        }
        dispatch_semaphore_signal(c->waiterDone);
        // Keep this same guest pool alive through callback delivery, deferred retirement and the weak observation.
        if (dispatch_semaphore_wait(c->poolRelease, dispatch_time(DISPATCH_TIME_NOW, 20000000000ll))) {
            fprintf(stderr, "readback guest pool was not released within 20 seconds\n"); _Exit(1);
        }
    }
    dispatch_semaphore_signal(c->waiterExited);
    return NULL;
}

// A texture in a sparse virtual pool takes its content from what the pool's page table maps (nvn_sparse_copy), unmapped pages as 0. ReadTexels reads it there on whatever thread asks:
// Tears of the Kingdom reads such textures on its own threads (bake::ModuleMgr), and resolving the host texture there uploaded it outside the submit thread (up_owner_check aborted, #471)
typedef struct { const NVNtexture *t; const NVNcopyRegion *r; uint8_t *dst; } read_job;
static void *read_on_thread(void *p) { read_job *j = p; h_nvnTextureReadTexels(j->t, NULL, j->r, j->dst); return NULL; }
static void sparse_reads(const NVNcopyRegion *region, const uint8_t expected[4]) {
    size_t len = 4 * (size_t)getpagesize();
    uint8_t *phys = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    pool_t physical, sparse; pool_builder_t pb = {.mem=phys, .size=len, .flags=0x24};
    EXPECT("physical pool init", 1, h_nvnMemoryPoolInitialize((NVNmemoryPool *)&physical, (const NVNmemoryPoolBuilder *)&pb));
    pb = (pool_builder_t){.size=DM_GAME_APP_POOL + 4 * len, .flags=NVN_MEMORY_POOL_FLAGS_VIRTUAL};   // larger than the application pool: sparse
    EXPECT("sparse pool init", 1, h_nvnMemoryPoolInitialize((NVNmemoryPool *)&sparse, (const NVNmemoryPoolBuilder *)&pb));
    map_req map = {.phys=(NVNmemoryPool *)&physical, .size=len};   // [0, len) mapped, the rest not
    EXPECT("sparse mapping", 1, h_nvnMemoryPoolMapVirtual((NVNmemoryPool *)&sparse, 1, (const NVNmappingRequest *)&map));
    EXPECT("pool reads through its page table", 1, nvn_sparse_pool(&sparse));
    for (size_t i = 0; i < len / 4; i++) memcpy(phys + i * 4, expected, 4);   // every texel the same, whatever the block-linear order
    NVNtextureBuilder tb; NVNtexture mapped, unmapped; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 8);
    h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&sparse, 0); EXPECT("mapped sparse texture init", 1, h_nvnTextureInitialize(&mapped, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&sparse, 2 * len); EXPECT("unmapped sparse texture init", 1, h_nvnTextureInitialize(&unmapped, &tb));
    static const uint8_t zero[4];
    for (int other = 1; other >= 0; other--) for (int m = 0; m < 2; m++) {   // a game thread first (the texture not resolved yet), then the submit thread (this one: mtl_sub_begin ran here)
        uint8_t cpu[128]; memset(cpu, 0xEE, sizeof cpu); read_job j = {m ? &unmapped : &mapped, region, cpu};
        if (other) { pthread_t th; pthread_create(&th, NULL, read_on_thread, &j); pthread_join(th, NULL); } else read_on_thread(&j);
        int bad = 0; for (int i = 0; i < 32; i++) bad += memcmp(cpu + i * 4, m ? zero : expected, 4) != 0;
        char name[128]; snprintf(name, sizeof name, "sparse texture, %s pages, ReadTexels on %s thread, 32 literal pixels", m ? "unmapped" : "mapped", other ? "a game" : "the submit");
        EXPECT(name, 0, bad);
    }
    h_nvnTextureFinalize(&unmapped); h_nvnTextureFinalize(&mapped);
    h_nvnMemoryPoolFinalize((NVNmemoryPool *)&sparse); h_nvnMemoryPoolFinalize((NVNmemoryPool *)&physical); munmap(phys, len);
}

// The RGB565 family in a plain texture (not packaged, sampled with no red / blue swap: tex_by_id) is in Metal's order, red at bit 11 (bit 10 for RGB5A1), the guest's order has red at bit 0.
// Copies between such a texture and guest memory convert: a buffer → texture copy samples as the guest wrote it, a texture → buffer copy and ReadTexels give back the guest's bits, also of
// what Metal itself cleared into it. A packaged texture holds Metal's order too: uploads convert, so what Metal clears or copies into one samples right (GitHub issue #183)
static void sample2(int tid, float out[8]) {
    static id<MTLComputePipelineState> p;
    if (!p) { NSError *e = nil; id<MTLLibrary> l = [g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void k(texture2d<float, access::read> t [[texture(0)]], device float4 *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = t.read(uint2(i, 0)); }\n" options:nil error:&e];
        p = [g_dev newComputePipelineStateWithFunction:[l newFunctionWithName:@"k"] error:&e]; }
    os_unfair_lock_lock(&g_rmu); id<MTLTexture> v = tex_by_id(tid); os_unfair_lock_unlock(&g_rmu);
    id<MTLBuffer> b = [g_dev newBufferWithLength:32 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> c = q_cb(); id<MTLComputeCommandEncoder> e = [c computeCommandEncoder];
    [e setComputePipelineState:p]; [e setTexture:v atIndex:0]; [e setBuffer:b offset:0 atIndex:0]; [e dispatchThreads:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(2, 1, 1)];
    [e endEncoding]; [c commit]; [c waitUntilCompleted]; memcpy(out, b.contents, 32);
}
static void packed_copies(int format, int tid) {
    int b565 = format == NVN_FORMAT_RGB565, a1 = format == NVN_FORMAT_RGB5A1;
    uint16_t red = a1 ? 0x801f : 0x001f, blue = b565 ? 0xf800 : 0x7c00, guest[2] = {red, blue};
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 262144)) abort(); memset(mem, 0, 262144);
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=262144}; mtl_pool_add(mem, 262144); nvn_dirty_add_range(mem, 262144);
    NVNtextureBuilder tb; NVNtexture copied, cleared; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 2);
    h_nvnTextureBuilderSetHeight(&tb, 1); h_nvnTextureBuilderSetFormat(&tb, format);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 65536); EXPECT("packed texture init", 1, h_nvnTextureInitialize(&copied, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 98304); EXPECT("packed texture 2 init", 1, h_nvnTextureInitialize(&cleared, &tb));
    // Packaged textures (uploaded from their storage, NvPkg): one only sampled, one with its blue texel cleared to red, one a copy of the cleared plain texture (read back into its storage)
    NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}}; NVNtexture up, upclr, upcopy; h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &lay);
    memcpy((uint8_t *)mem + 163840, guest, 4); memcpy((uint8_t *)mem + 172032, guest, 4);   // two texels of a GOB's first row: linear
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 163840); EXPECT("packaged texture init", 1, h_nvnTextureInitialize(&up, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 172032); EXPECT("packaged texture 2 init", 1, h_nvnTextureInitialize(&upclr, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 180224); EXPECT("packaged texture 3 init", 1, h_nvnTextureInitialize(&upcopy, &tb));
    uint8_t *src = (uint8_t *)mem + 131072, *out = src + 4096, *out2 = src + 8192; memcpy(src, guest, 4); memset(out, 0xEE, 4); memset(out2, 0xEE, 4);
    NVNcommandBuffer cmd; EXPECT("command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536);
    h_nvnCommandBufferBeginRecording(&cmd);
    NVNcopyRegion region = {0, 0, 0, 2, 1, 1};
    h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)src, &copied, NULL, &region, 0);
    h_nvnCommandBufferCopyTextureToBuffer(&cmd, &copied, NULL, &region, (NVNbufferAddress)out, 0);
    float rgba[] = {1, 0, 0, 1}; h_nvnCommandBufferClearTexture(&cmd, &cleared, NULL, &region, rgba, 15);
    h_nvnCommandBufferCopyTextureToBuffer(&cmd, &cleared, NULL, &region, (NVNbufferAddress)out2, 0);
    NVNcopyRegion second = {1, 0, 0, 1, 1, 1}; h_nvnCommandBufferClearTexture(&cmd, &upclr, NULL, &second, rgba, 15);
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &cleared, NULL, &region, &upcopy, NULL, &region, 0);
    NVNsync sync; h_nvnSyncInitialize(&sync, NULL);
    h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&cmd);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec((const uint8_t *)h); mtl_sub_commit();
    int wait = h_nvnSyncWait(&sync, 5000000000ull); EXPECT("packed copies fence wait", 1, wait == 0 || wait == 1);
    char n[160]; const char *f = b565 ? "RGB565" : a1 ? "RGB5A1" : "RGB5";
    mtl_reg_tex(tid, TX(&copied), NULL); float s[8]; sample2(tid, s);
    snprintf(n, sizeof n, "%s buffer -> plain texture, sampled red then blue", f); EXPECT(n, 1, s[0] > .99f && s[2] < .01f && s[4] < .01f && s[6] > .99f);
    uint16_t got[2]; memcpy(got, out, 4); snprintf(n, sizeof n, "%s plain texture -> buffer, the guest's bits", f); EXPECT(n, 1, got[0] == red && got[1] == blue);
    memset(got, 0xEE, 4); h_nvnTextureReadTexels(&copied, NULL, &region, (uint8_t *)got); snprintf(n, sizeof n, "%s plain texture ReadTexels, the guest's bits", f); EXPECT(n, 1, got[0] == red && got[1] == blue);
    uint16_t used = b565 || a1 ? 0xffff : 0x7fff;   // RGB5's top bit is unused (Metal's BGR5A1 stores alpha 1 there)
    memcpy(got, out2, 4); snprintf(n, sizeof n, "%s cleared to red -> buffer, guest red", f); EXPECT(n, 1, (got[0] & used) == red && (got[1] & used) == red);
    memset(got, 0xEE, 4); h_nvnTextureReadTexels(&cleared, NULL, &region, (uint8_t *)got); snprintf(n, sizeof n, "%s cleared to red, ReadTexels guest red", f); EXPECT(n, 1, (got[0] & used) == red && (got[1] & used) == red);
    mtl_reg_tex(tid + 10, TX(&up), NULL); sample2(tid + 10, s);
    snprintf(n, sizeof n, "%s packaged texture, sampled red then blue", f); EXPECT(n, 1, s[0] > .99f && s[2] < .01f && s[4] < .01f && s[6] > .99f);
    mtl_reg_tex(tid + 20, TX(&upclr), NULL); sample2(tid + 20, s);
    snprintf(n, sizeof n, "%s packaged texture, its blue texel cleared to red: red twice", f); EXPECT(n, 1, s[0] > .99f && s[2] < .01f && s[4] > .99f && s[6] < .01f);
    mtl_reg_tex(tid + 30, TX(&upcopy), NULL); sample2(tid + 30, s);
    snprintf(n, sizeof n, "%s cleared plain texture -> packaged texture, sampled red", f); EXPECT(n, 1, s[0] > .99f && s[2] < .01f && s[4] > .99f && s[6] < .01f);
    memset(got, 0xEE, 4); h_nvnTextureReadTexels(&upcopy, NULL, &region, (uint8_t *)got); snprintf(n, sizeof n, "%s copy into a packaged texture, guest red in its storage", f); EXPECT(n, 1, (got[0] & used) == red && (got[1] & used) == red);
    h_nvnTextureFinalize(&upcopy); h_nvnTextureFinalize(&upclr); h_nvnTextureFinalize(&up);
    h_nvnTextureFinalize(&cleared); h_nvnTextureFinalize(&copied); nvn_dirty_remove_range(mem); mtl_pool_del(mem); free(mem);
}

// A real GPU copy into packaged storage retires its shared readback buffer on whichever completion owner wins.
// Gate the submission's feedback, not its GPU event: FAST_WAKE=1 must do the CPU effects on the guest waiter before
// feedback can run; FAST_WAKE=0 exercises the real callback owner instead. Neither path drains the guest's outer pool.
static void mirror_completion(void) {
    int early = fast_wake();
    printf("mirror completion: %s wins\n", early ? "guest waiter" : "GPU feedback");
    readback_feedback_idle();
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 262144)) abort(); memset(mem, 0, 262144);
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=262144}; mtl_pool_add(mem, 262144); nvn_dirty_add_range(mem, 262144);
    NVNtextureBuilder tb; NVNtexture source, packaged; h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetWidth(&tb, 8); h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 131072);
    EXPECT("completion source texture init", 1, h_nvnTextureInitialize(&source, &tb));
    NVNpackagedTextureLayout layout = {{1, 0, 0, 0, 7, 0, 1, 0}};
    h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &layout);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 163840);
    EXPECT("completion packaged texture init", 1, h_nvnTextureInitialize(&packaged, &tb));
    memset((uint8_t *)mem + 163840, 0xEE, h_nvnTextureGetStorageSize(&packaged));
    NVNeventBuilder eb; NVNevent event; h_nvnEventBuilderSetDefaults(&eb);
    h_nvnEventBuilderSetStorage(&eb, (const NVNmemoryPool *)&pool, 196608);
    EXPECT("completion event init", 1, h_nvnEventInitialize(&event, &eb));
    NVNsync sync; h_nvnSyncInitialize(&sync, NULL);
    NVNcommandBuffer cmd; EXPECT("completion command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536); h_nvnCommandBufferBeginRecording(&cmd);
    NVNcopyRegion region = {0, 0, 0, 8, 4, 1}; float color[] = {0.2f, 0.4f, 0.6f, 1.f};
    h_nvnCommandBufferClearTexture(&cmd, &source, NULL, &region, color, 15);
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &source, NULL, &region, &packaged, NULL, &region, 0);
    h_nvnCommandBufferSignalEvent(&cmd, &event, NVN_EVENT_SIGNAL_MODE_ADD, NVN_EVENT_SIGNAL_LOCATION_BOTTOM, 0, 47);
    h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle handle = h_nvnCommandBufferEndRecording(&cmd);
    ReadbackCompletion *c = [ReadbackCompletion new]; c->sync = &sync; c->texture = &packaged; c->event = &event; c->region = region;
    c->feedbackEntered = dispatch_semaphore_create(0); c->feedbackRelease = dispatch_semaphore_create(0);
    c->waiterStarted = dispatch_semaphore_create(0); c->waiterDone = dispatch_semaphore_create(0);
    c->poolRelease = dispatch_semaphore_create(0); c->waiterExited = dispatch_semaphore_create(0);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    ReadbackDevice *device = [ReadbackDevice new]; device.device = g_dev; device.observedLength = 128; g_dev = (id)device;
    mtl_exec((const uint8_t *)handle);
    g_dev = device.device;
    [g_commit_blocks addObject:^{
        dispatch_semaphore_signal(c->feedbackEntered);
        if (dispatch_semaphore_wait(c->feedbackRelease, dispatch_time(DISPATCH_TIME_NOW, 10000000000ll))) atomic_store(&c->gateTimedOut, 1);
    }];
    mtl_sub_commit();
    readback_wait_for("real mirror feedback reached its gate", c->feedbackEntered);
    EXPECT("mirror copy allocated one real 128-byte readback buffer", 1, device.allocations);
    EXPECT("GPU completion alone does not publish the pending fence", NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED, h_nvnSyncWait(&sync, 0));
    EXPECT("pending fence has not applied its event effect", 0, h_nvnEventGetValue(&event));
    int changed = 0;
    for (int i = 0; i < h_nvnTextureGetStorageSize(&packaged); i++) changed += ((uint8_t *)mem)[163840 + i] != 0xEE;
    EXPECT("pending fence has not landed mirror bytes in guest storage", 0, changed);
    pthread_t thread; int created = pthread_create(&thread, NULL, readback_guest_waiter, (__bridge void *)c) == 0;
    EXPECT("long-lived readback guest waiter started", 1, created);
    if (!created) { dispatch_semaphore_signal(c->feedbackRelease); exit(1); }
    readback_wait_for("guest waiter entered SyncWait", c->waiterStarted);
    int completed = 0;
    if (!early) {
        completed = dispatch_semaphore_wait(c->waiterDone, dispatch_time(DISPATCH_TIME_NOW, 50000000ll)) == 0;
        EXPECT("callback-owned waiter cannot return while feedback is gated", 0, completed);
        dispatch_semaphore_signal(c->feedbackRelease);
    }
    if (!completed) readback_wait_for(early ? "fast-wake guest completed before feedback was released" : "callback-owned guest completed after feedback was released", c->waiterDone);
    EXPECT("mirror completion guest fence wait succeeded", 1, c->waitResult == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || c->waitResult == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED);
    EXPECT("mirror bytes are in guest storage at successful wait return, 32 literal pixels", 0, c->badPixels);
    EXPECT("event effect is visible at successful guest wait return", 47, c->eventValue);
    if (early) dispatch_semaphore_signal(c->feedbackRelease);
    readback_feedback_idle();
    EXPECT("mirror feedback gate did not time out", 0, atomic_load(&c->gateTimedOut));
    EXPECT("late feedback does not apply the event effect twice", 47, h_nvnEventGetValue(&event));
    // A subsequent real submission drains completed retirements and resets the command allocator that held the copy.
    mtl_sub_begin(); uint64_t retirement = mtl_sub_commit();
    EXPECT("mirror retirement submission completed", 1, mtl_wait_at(retirement, 5000000000ull, WS_FINISH));
    readback_feedback_idle();
    EXPECT("mirror retirement GPU success", 0, mtl_last_commit_error());
    EXPECT("mirror completion releases its readback buffer before the guest caller's pool drains", 1, readback_buffer_gone(device));
    dispatch_semaphore_signal(c->poolRelease);
    readback_wait_for("guest waiter drained its outer pool and exited", c->waiterExited); pthread_join(thread, NULL);
    EXPECT("mirror readback buffer is gone after the guest outer pool drains", 1, readback_buffer_gone(device));
    h_nvnCommandBufferFinalize(&cmd); h_nvnSyncFinalize(&sync); h_nvnEventFinalize(&event);
    h_nvnTextureFinalize(&packaged); h_nvnTextureFinalize(&source);
    nvn_dirty_remove_range(mem); mtl_pool_del(mem); free(mem);
}

// A texture ID outlives the game's memory pool object: the ID stays registered, the object is freed and its memory reused. What was there reads as anything, as x8 = pool->vp =
// 0x2c33398156a0e722 did in nvn_sparse_protect after 22 minutes of FINAL FANTASY RESONANCE (GitHub issue #934). Nothing may be made or refreshed from such a texture's pool or storage.
static void pool_object_reused(pool_t *o) { memset(o, 0x22, sizeof(NVNmemoryPool)); o->vp = (struct vpool *)0x2c33398156a0e722ull; }
static id<MTLTexture> by_id(int tid) { os_unfair_lock_lock(&g_rmu); id<MTLTexture> t = tex_by_id(tid); os_unfair_lock_unlock(&g_rmu); return t; }
static void stale_pools(void) {
    size_t len = 4 * (size_t)getpagesize();
    uint8_t *phys = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0); if (phys == MAP_FAILED) abort();
    NVNmemoryPool *mp = calloc(1, sizeof *mp), *sp = calloc(1, sizeof *sp), *mp2 = calloc(1, sizeof *mp2);   // the game's objects, on its heap
    pool_builder_t pb = {.mem=phys, .size=len, .flags=0x24}, vb = {.size=DM_GAME_APP_POOL + 4 * len, .flags=NVN_MEMORY_POOL_FLAGS_VIRTUAL};
    EXPECT("stale: physical pool init", 1, h_nvnMemoryPoolInitialize(mp, (const NVNmemoryPoolBuilder *)&pb));
    NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}}; NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 8);
    h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &lay);
    h_nvnTextureBuilderSetStorage(&tb, mp, 0);

    // The texture finalized and its ID left registered, its pool then finalized and the object reused; a late read has no host texture to fall back on, so it would upload (the crash)
    NVNtexture late, live, unfinalized; EXPECT("stale: late texture init", 1, h_nvnTextureInitialize(&late, &tb));
    EXPECT("stale: live texture init", 1, h_nvnTextureInitialize(&live, &tb));
    EXPECT("stale: unfinalized texture init", 1, h_nvnTextureInitialize(&unfinalized, &tb));
    mtl_reg_tex(601, (const tex_t *)&late, NULL); mtl_reg_tex(602, (const tex_t *)&live, NULL);
    id<MTLTexture> made = by_id(602); EXPECT("stale: a texture of a live pool resolves by ID", 1, made != nil);
    h_nvnTextureFinalize(&late);
    h_nvnMemoryPoolFinalize(mp); pool_object_reused((pool_t *)mp);
    EXPECT("stale: finalized texture and pool, object reused, read by ID -> empty slot", 1, by_id(601) == nil);
    // A live texture the host already made stays what it was, with no refresh from the dead pool; one it has not made stays unmade, also through its object rather than an ID
    uint64_t uploads = g_pkg_uploads, reuploads = g_pkg_reuploads;
    EXPECT("stale: live texture of a finalized pool keeps its host texture", 1, by_id(602) == made);
    EXPECT("stale: live texture of a finalized pool, through its object", 1, tex_mtl((const tex_t *)&live) == made);
    EXPECT("stale: a texture never made is not made", 1, tex_mtl((const tex_t *)&unfinalized) == nil);
    EXPECT("stale: nothing was uploaded", 1, g_pkg_uploads == uploads && g_pkg_reuploads == reuploads);

    // A new pool at the same address (a heap reuses a freed block of this size) does not bring the old pool's IDs back: their storage is still gone, until the game registers them again
    EXPECT("stale: new pool at the old object's address", 1, h_nvnMemoryPoolInitialize(mp, (const NVNmemoryPoolBuilder *)&pb));
    EXPECT("stale: the old ID stays empty over the new pool", 1, by_id(601) == nil);
    h_nvnTextureBuilderSetStorage(&tb, mp, 0); NVNtexture again; EXPECT("stale: new texture init", 1, h_nvnTextureInitialize(&again, &tb));
    mtl_reg_tex(601, (const tex_t *)&again, NULL); EXPECT("stale: registered again -> resolves", 1, by_id(601) != nil);
    h_nvnTextureFinalize(&again); h_nvnMemoryPoolFinalize(mp); memset(mp, 0, sizeof *mp);

    // The object overwritten with no Finalize at all: no gone mark can know, and its magic is what is left to tell
    EXPECT("stale: physical pool init 2", 1, h_nvnMemoryPoolInitialize(mp2, (const NVNmemoryPoolBuilder *)&pb));
    h_nvnTextureBuilderSetStorage(&tb, mp2, 0); NVNtexture unsaid; EXPECT("stale: texture init 2", 1, h_nvnTextureInitialize(&unsaid, &tb));
    mtl_reg_tex(603, (const tex_t *)&unsaid, NULL);
    pool_t saved = *(pool_t *)mp2; pool_object_reused((pool_t *)mp2);
    EXPECT("stale: pool object overwritten with no Finalize, read by ID -> empty slot", 1, by_id(603) == nil);
    *(pool_t *)mp2 = saved; h_nvnTextureFinalize(&unsaid); h_nvnMemoryPoolFinalize(mp2);

    // A sparse texture the host uploaded: its next look would go through the dead pool's page table (nvn_sparse_since), not only a first upload's protection
    EXPECT("stale: physical pool init 3", 1, h_nvnMemoryPoolInitialize(mp2, (const NVNmemoryPoolBuilder *)&pb));
    EXPECT("stale: sparse pool init", 1, h_nvnMemoryPoolInitialize(sp, (const NVNmemoryPoolBuilder *)&vb));
    map_req map = {.phys=mp2, .size=len}; EXPECT("stale: sparse mapping", 1, h_nvnMemoryPoolMapVirtual(sp, 1, (const NVNmappingRequest *)&map));
    NVNtextureBuilder sb; NVNtexture st; h_nvnTextureBuilderSetDefaults(&sb); h_nvnTextureBuilderSetWidth(&sb, 8); h_nvnTextureBuilderSetHeight(&sb, 4);
    h_nvnTextureBuilderSetFormat(&sb, NVN_FORMAT_RGBA8); h_nvnTextureBuilderSetStorage(&sb, sp, 0); EXPECT("stale: sparse texture init", 1, h_nvnTextureInitialize(&st, &sb));
    mtl_reg_tex(604, (const tex_t *)&st, NULL);
    id<MTLTexture> smade = by_id(604); EXPECT("stale: sparse texture resolves", 1, smade != nil);
    h_nvnMemoryPoolFinalize(sp); pool_object_reused((pool_t *)sp);
    EXPECT("stale: sparse texture of a finalized pool keeps its host texture", 1, by_id(604) == smade);
    EXPECT("stale: and through its object", 1, tex_mtl((const tex_t *)&st) == smade);
    mtl_reg_tex(601, NULL, NULL); mtl_reg_tex(602, NULL, NULL); mtl_reg_tex(603, NULL, NULL); mtl_reg_tex(604, NULL, NULL);
    h_nvnMemoryPoolFinalize(mp2); munmap(phys, len); free(mp); free(mp2); free(sp);
}

int main(void) { @autoreleasepool {
    alarm(60); if (setvbuf(stdout, NULL, _IONBF, 0)) abort();   // GPU, callback and pthread failures must not leave the check target hanging.
    mtl_init();
    void *mem = NULL; if (posix_memalign(&mem, getpagesize(), 524288)) abort(); memset(mem, 0, 524288);
    // Registered with the dirty tracker as nvnMemoryPoolInitialize does: a packaged texture in an untracked pool is re-uploaded from its storage at every use, which would discard what the GPU wrote into it
    pool_t pool = {.magic=MAGIC('P','O','O','L'), .mem=mem, .size=524288}; mtl_pool_add(mem, 524288); nvn_dirty_add_range(mem, 524288);
    NVNtextureBuilder tb; NVNtexture tex; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetWidth(&tb, 8);
    h_nvnTextureBuilderSetHeight(&tb, 4); h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 131072); EXPECT("texture init", 1, h_nvnTextureInitialize(&tex, &tb));
    NVNcommandBuffer cmd; EXPECT("command init", 1, h_nvnCommandBufferInitialize(&cmd, NULL));
    h_nvnCommandBufferAddCommandMemory(&cmd, (const NVNmemoryPool *)&pool, 0, 65536);
    h_nvnCommandBufferBeginRecording(&cmd);
    const NVNtexture *targets[] = {&tex}; const NVNtextureView *views[] = {NULL};
    h_nvnCommandBufferSetRenderTargets(&cmd, 1, targets, views, NULL, NULL);
    float color[] = {0.2f, 0.4f, 0.6f, 1.f}; h_nvnCommandBufferClearColor(&cmd, 0, color, 15);
    NVNcopyRegion region = {0, 0, 0, 8, 4, 1}; uint8_t *out = (uint8_t *)mem + 196608; memset(out, 0xEE, 256);
    h_nvnCommandBufferSetCopyRowStride(&cmd, 64);
    h_nvnCommandBufferCopyTextureToBuffer(&cmd, &tex, NULL, &region, (NVNbufferAddress)out, 0);
    // A copy from the cleared (GPU-written) texture into a packaged texture, whose storage is where the host takes its content from: the copy's bytes are read back into the storage
    // when the submission completes (mirror_readback), so ReadTexels, which reads a packaged texture's storage, sees the clear colour
    NVNpackagedTextureLayout lay = {{1, 0, 0, 0, 7, 0, 1, 0}}; NVNtexture pk; h_nvnTextureBuilderSetPackagedTextureLayout(&tb, &lay);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 163840); EXPECT("packaged texture init", 1, h_nvnTextureInitialize(&pk, &tb));
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &tex, NULL, &region, &pk, NULL, &region, 0);
    // A copy from that packaged texture while its readback is pending: its storage does not hold the content yet, so this copy is read back as well, landing after the first
    NVNtexture pk2; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 262144); EXPECT("packaged texture 2 init", 1, h_nvnTextureInitialize(&pk2, &tb));
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pk, NULL, &region, &pk2, NULL, &region, 0);
    // A buffer a copy queued in this submission writes (out → src2 on the GPU) holds stale bytes on the CPU: the copy from it into a packaged texture is read back
    uint8_t *src2 = (uint8_t *)mem + 200704; memset(src2, 0xEE, 256);
    h_nvnCommandBufferCopyBufferToBuffer(&cmd, (NVNbufferAddress)out, (NVNbufferAddress)src2, 256, 0);
    NVNtexture pk4; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 294912); EXPECT("packaged texture 4 init", 1, h_nvnTextureInitialize(&pk4, &tb));
    h_nvnCommandBufferCopyBufferToTexture(&cmd, (NVNbufferAddress)src2, &pk4, NULL, &region, 0);
    // A format-converting copy (RGBA8 → RGBA8 sRGB, the same bytes per texel) from a packaged texture the CPU wrote is drawn on the GPU: the storage takes the converted bytes by readback,
    // not the source's raw bytes. Every texel of the source is the same, whatever its block-linear order
    static const uint8_t expected[] = {51, 102, 153, 255};
    NVNtexture pk7; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 425984); EXPECT("packaged texture 7 init", 1, h_nvnTextureInitialize(&pk7, &tb));
    for (int i = 0; i < 1024; i++) memcpy((uint8_t *)mem + 425984 + i * 4, expected, 4);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8_SRGB);
    NVNtexture pk3; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 327680); EXPECT("packaged sRGB texture init", 1, h_nvnTextureInitialize(&pk3, &tb));
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pk7, NULL, &region, &pk3, NULL, &region, 0);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    // A packaged texture the GPU cleared (the clear gives its host texture render-target use) has stale storage: a copy from it is read back, not mirrored from the storage
    NVNtexture pk5, pk6; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 360448); EXPECT("packaged texture 5 init", 1, h_nvnTextureInitialize(&pk5, &tb));
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 393216); EXPECT("packaged texture 6 init", 1, h_nvnTextureInitialize(&pk6, &tb));
    float color2[] = {0.8f, 0.6f, 0.4f, 1.f}; h_nvnCommandBufferClearTexture(&cmd, &pk5, NULL, &region, color2, 15);
    h_nvnCommandBufferCopyTextureToTexture(&cmd, &pk5, NULL, &region, &pk6, NULL, &region, 0);
    // A render pass into a packaged texture writes its storage, as the hardware's would: what the pass drew is read back into the storage when the submission completes (rt_mirror_flush).
    // Tears of the Kingdom moves such textures with memcpy (its resource defragmentation) and re-creates them at the new address, which then has to carry the drawing
    NVNtexture pk8; h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 458752); EXPECT("packaged render target init", 1, h_nvnTextureInitialize(&pk8, &tb));
    const NVNtexture *targets8[] = {&pk8};
    h_nvnCommandBufferSetRenderTargets(&cmd, 1, targets8, views, NULL, NULL);
    float color3[] = {0.4f, 0.2f, 0.8f, 1.f}; h_nvnCommandBufferClearColor(&cmd, 0, color3, 15);
    NVNsync sync; h_nvnSyncInitialize(&sync, NULL);
    h_nvnCommandBufferFenceSync(&cmd, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 0);
    NVNcommandHandle h = h_nvnCommandBufferEndRecording(&cmd);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin(); mtl_exec((const uint8_t *)h); mtl_sub_commit();
    int wait = h_nvnSyncWait(&sync, 5000000000ull); EXPECT("recorded fence wait", 1, wait == 0 || wait == 1);
    EXPECT("GPU success", 0, mtl_last_commit_error());
    int bad = 0;
    for (int y=0; y<4; y++) { for (int x=0; x<8; x++) bad += memcmp(out+y*64+x*4, expected, 4) != 0;
        for (int x=32; x<64; x++) bad += out[y*64+x] != 0xEE; }
    EXPECT("pending clear -> GPU copy, literal pixels and untouched row padding", 0, bad);
    ReadbackDevice *device = [ReadbackDevice new]; device.device = g_dev; g_dev = (id)device;
    uint8_t cpu[128]; memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&tex, NULL, &region, cpu); bad = 0;
    g_dev = device.device;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected, 4) != 0;
    EXPECT("GPU clear -> CPU ReadTexels, 32 literal pixels", 0, bad);
    EXPECT("ReadTexels used the GPU readback path", 1, device.allocations);
    EXPECT("ReadTexels releases its readback buffer before the caller's pool drains", 1, device.lastBuffer == nil);
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected, 4) != 0;
    EXPECT("GPU copy into a packaged texture -> its storage (ReadTexels), 32 literal pixels", 0, bad);
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk2, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected, 4) != 0;
    EXPECT("copy from a packaged texture with a readback pending -> read back too, 32 literal pixels", 0, bad);
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk4, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected, 4) != 0;
    EXPECT("copy from a buffer a queued copy writes -> read back, 32 literal pixels", 0, bad);
    static const uint8_t expected_srgb[] = {124, 170, 203, 255};   // the clear colour sRGB-encoded (within 1: the GPU's encoding curve), not the source's 51, 102, 153
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk3, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) for (int k=0; k<4; k++) bad += abs((int)cpu[i*4+k] - expected_srgb[k]) > 1;
    EXPECT("format-converting copy from a CPU-written packaged texture -> converted bytes in its storage, 32 pixels within 1", 0, bad);
    static const uint8_t expected2[] = {204, 153, 102, 255}; memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk6, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected2, 4) != 0;
    EXPECT("copy from a packaged texture the GPU cleared -> read back, 32 literal pixels", 0, bad);
    static const uint8_t expected3[] = {102, 51, 204, 255}; memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk8, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected3, 4) != 0;
    EXPECT("render pass into a packaged texture -> its storage (ReadTexels), 32 literal pixels", 0, bad);
    uint64_t reuploads = g_pkg_reuploads; tex_mtl((const tex_t *)&pk8);
    EXPECT("its own readback is not uploaded again", 1, g_pkg_reuploads == reuploads);
    NVNtexture pk9; memcpy((uint8_t *)mem + 491520, (uint8_t *)mem + 458752, ((const tex_t *)&pk8)->L.size);
    h_nvnTextureBuilderSetStorage(&tb, (const NVNmemoryPool *)&pool, 491520); EXPECT("moved texture init", 1, h_nvnTextureInitialize(&pk9, &tb));
    memset(cpu, 0xEE, sizeof cpu); h_nvnTextureReadTexels(&pk9, NULL, &region, cpu); bad = 0;
    for (int i=0; i<32; i++) bad += memcmp(cpu+i*4, expected3, 4) != 0;
    EXPECT("storage moved with memcpy -> a texture there holds the drawing, 32 literal pixels", 0, bad);
    h_nvnTextureFinalize(&pk9); h_nvnTextureFinalize(&pk8);
    h_nvnTextureFinalize(&pk6); h_nvnTextureFinalize(&pk5); h_nvnTextureFinalize(&pk3); h_nvnTextureFinalize(&pk7); h_nvnTextureFinalize(&pk4); h_nvnTextureFinalize(&pk2);
    h_nvnTextureFinalize(&pk); h_nvnTextureFinalize(&tex); nvn_dirty_remove_range(mem); mtl_pool_del(mem); free(mem);
    sparse_reads(&region, expected);
    stale_pools();
    packed_copies(NVN_FORMAT_RGB565, 500); packed_copies(NVN_FORMAT_RGB5A1, 501); packed_copies(NVN_FORMAT_RGB5, 502);
    mirror_completion();
    printf("READBACK cases=%d failed=%d\n", cases, failed); return failed != 0;
} }
