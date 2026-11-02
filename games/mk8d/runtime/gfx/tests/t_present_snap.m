// Snapshot lifecycle: real NVN acquire/present/suspend, real Metal copies, and controlled encoder/GPU delays.
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
static tex_t source(unsigned i) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d]; uint8_t bytes[256]; memset(bytes, 73 + i, sizeof bytes);
    [mt replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:bytes bytesPerRow:32];
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=90001+i};
    t.d.width = t.d.height = 8; t.d.depth = t.d.levels = t.d.samples = 1;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    serial_put(t.serial, mt); res_add(mt); res_commit(); return t;
}
static void pixels(id<MTLTexture> t, int value) {
    id<MTLBuffer> out = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(8,8,1)
        toBuffer:out destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    int bad = 0; for (int i = 0; i < 256; i++) bad += ((uint8_t *)out.contents)[i] != value;
    CHECK("snapshot has the expected pixels", bad == 0);
}
static _Atomic uint64_t gate_note;
static void gate_encoder(void) {
    pthread_mutex_lock(&g_note_mu);
    rec_t *r = rec_put(RO_NOTE, 0); r->a[0] = (uint64_t)(uintptr_t)&gate_note; r->a[1] = 1; rec_publish();
}
static dispatch_semaphore_t suspend_started, suspend_done;
static void *suspend_thread(void *arg) {
    (void)arg; @autoreleasepool {
        dispatch_semaphore_signal(suspend_started); dm_gfx_suspend(1); dispatch_semaphore_signal(suspend_done);
    } return NULL;
}
static void suspend_case(void) {
    tex_t t = source(0);
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    CHECK("prior submission completed", mtl_wait(mtl_sub_commit(), 2000000000ull));
    id<MTLSharedEvent> gpu_gate = [g_dev newSharedEvent];
    [g_q4 waitForEvent:gpu_gate value:1];
    gate_encoder();
    // The OS has stopped the display link before notifying the app to suspend.
    pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 1; pthread_mutex_unlock(&g_pq_mu);
    present_enqueue(&t, 1. / 60, NULL, 0); mtl_present_discard(1);
    suspend_started = dispatch_semaphore_create(0); suspend_done = dispatch_semaphore_create(0);
    pthread_t th; pthread_create(&th, NULL, suspend_thread, NULL);
    dispatch_semaphore_wait(suspend_started, DISPATCH_TIME_FOREVER);
    int returned = dispatch_semaphore_wait(suspend_done, dispatch_time(DISPATCH_TIME_NOW, 100000000)) == 0;
    CHECK("suspend waits for the queued snapshot commit", !returned);
    pthread_mutex_unlock(&g_note_mu); snap_commit_wait(g_snap_v);
    if (!returned) returned = dispatch_semaphore_wait(suspend_done, dispatch_time(DISPATCH_TIME_NOW, 100000000)) == 0;
    CHECK("suspend also waits for snapshot GPU completion", !returned);
    gpu_gate.signaledValue = 1;
    CHECK("snapshot completes after the GPU gate opens", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
    if (!returned) CHECK("suspend finishes after the snapshot", dispatch_semaphore_wait(suspend_done, dispatch_time(DISPATCH_TIME_NOW, 2000000000)) == 0);
    pthread_join(th, NULL); dm_gfx_suspend(0);
}
static void pacing_case(void) {
    state_reset(); memset(&R, 0, sizeof R); mtl_sub_begin();
    CHECK("prior submission completed", mtl_wait(mtl_sub_commit(), 2000000000ull));
    tex_t textures[4]; win_t win = {.ntex=4}; int indices[4];
    for (int i = 0; i < 4; i++) { textures[i] = source(i); win.tex[i] = (NVNtexture *)&textures[i]; }
    // Several acquires can precede their presents. A full snapshot ring must fall back to texture tickets.
    for (int i = 0; i < 4; i++) h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &indices[i]);
    mtl_present_test_stall(150000000); double longest = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t start = now_ns(); h_nvnQueuePresentTexture(NULL, (NVNwindow *)&win, indices[i]);
        double ms = (now_ns() - start) / 1e6; if (ms > longest) longest = ms;
        printf("present %d: %.3f ms, ticket %llu\n", i, ms, (unsigned long long)win.ticket[indices[i]]);
    }
    CHECK("present does not wait for the stalled display", longest < 50);
    CHECK("full ring uses the original texture ticket", win.ticket[indices[2]] != 0 && win.ticket[indices[3]] != 0);
    uint64_t start = now_ns(); int idx; h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx);
    double ms = (now_ns() - start) / 1e6; printf("acquire: %.3f ms\n", ms);
    CHECK("acquire waits for snapshot capacity", ms >= 50);
    h_nvnQueuePresentTexture(NULL, (NVNwindow *)&win, idx);
    CHECK("snapshot resumes after acquire frees capacity", win.ticket[idx] == 0);
    mtl_present_flush(); mtl_present_test_stall(0);
    for (int i = 0; i < 4; i++) {
        h_nvnWindowAcquireTexture((NVNwindow *)&win, NULL, &idx);
        h_nvnQueuePresentTexture(NULL, (NVNwindow *)&win, idx);
        mtl_present_flush(); snap_commit_wait(g_snap_v);
        CHECK("reused snapshot completed", [g_snap_ev waitUntilSignaledValue:g_snap_v timeoutMS:2000]);
        int slot = (g_snap_next - 1) % g_snap_n; pixels(g_snap[slot].tex, 73 + idx);
    }
}
static void pending_copy_case(void) {
    tex_t t = source(0);
    id<MTLSharedEvent> gpu_gate = [g_dev newSharedEvent]; [g_q4 waitForEvent:gpu_gate value:1];
    pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 1; pthread_mutex_unlock(&g_pq_mu);
    CHECK("first snapshot queued", present_enqueue(&t, 1. / 60, NULL, 0) == 0);
    CHECK("second snapshot queued", present_enqueue(&t, 1. / 60, NULL, 0) == 0);
    present_drop_for_suspend();   // Tickets are free, but the GPU still owns both allocators.
    uint64_t v = g_snap_v, start = now_ns(), ticket = present_enqueue(&t, 1. / 60, NULL, 0);
    CHECK("pending copy falls back without waiting", ticket != 0 && now_ns() - start < 50000000);
    CHECK("pending copy's allocator was not reused", g_snap_v == v);
    gpu_gate.signaledValue = 1; mtl_drain();
    CHECK("discarded copies finish before drain returns", g_snap_ev.signaledValue >= v);
    pixels(g_snap[0].tex, 73); pixels(g_snap[1].tex, 73);
}
int main(int argc, char **argv) { @autoreleasepool {
    alarm(15); setbuf(stdout, NULL);
    if (argc != 2) return 2;
    setenv("DM_PRESENT_SNAP", "1", 1); setenv("DM_PRESENT_HZ", "0", 1);
    mtl_init();
    if (!strcmp(argv[1], "suspend")) suspend_case();
    else if (!strcmp(argv[1], "pacing")) pacing_case();
    else if (!strcmp(argv[1], "copy")) pending_copy_case();
    else return 2;
    printf("present snapshots: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
