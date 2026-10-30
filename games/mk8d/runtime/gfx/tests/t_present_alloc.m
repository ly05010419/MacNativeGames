// Display-link presents with the GPU held back: a present must not reset a command allocator whose own present has not run yet. Real Metal 4 queue and offscreen
// drawables; the link's callback is called by hand (no window, no game files). Two cases: a dropped frame (two presents in flight) and one frame per tick (three in flight).
#include "../nvn/nvn_mtl.m"
@interface FakeLinkUpdate : NSObject
@property id<CAMetalDrawable> drawable;
@property CFTimeInterval targetPresentationTimestamp;
@end
@implementation FakeLinkUpdate
@end
static CAMetalLayer *g_test_layer;
void *front_metal_layer(void) { return (__bridge void *)g_test_layer; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(name, ok) do { int pass = !!(ok); checks++; failed += !pass; printf("%s: %s\n", name, pass ? "PASS" : "FAIL"); } while (0)
static tex_t source(unsigned serial, uint8_t left, uint8_t right) {   // 8x8 grey, the left four columns one value and the right four another
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d]; uint8_t bytes[256];
    for (int i = 0; i < 64; i++) { uint8_t v = i % 8 < 4 ? left : right; bytes[4 * i] = bytes[4 * i + 1] = bytes[4 * i + 2] = v; bytes[4 * i + 3] = 255; }
    [mt replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0 withBytes:bytes bytesPerRow:32];
    tex_t t = {.magic=MAGIC('T','E','X',0), .serial=serial};
    t.d.width = t.d.height = 8; t.d.depth = t.d.levels = t.d.samples = 1;
    t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    serial_put(t.serial, mt); res_add(mt); res_commit(); return t;
}
static CAMetalLayer *offscreen_layer(void) {
    CAMetalLayer *l = [CAMetalLayer layer]; l.device = g_dev; l.pixelFormat = MTLPixelFormatBGRA8Unorm; l.framebufferOnly = NO; l.drawableSize = CGSizeMake(8, 8); return l;
}
// Every present gets a drawable of its own layer. With all drawables from one offscreen layer, 1 of 100 runs under CPU load got ticket 1's IOSurface back from
// nextDrawable while the gate still held ticket 1's present, and ticket 4 then drew over the picture this test reads back
static NSMutableArray *g_layers;
static id<CAMetalDrawable> drawable(void) {
    CAMetalLayer *l = offscreen_layer(); [g_q4 addResidencySet:l.residencySet]; [g_layers addObject:l]; return [l nextDrawable];
}
static NvMetalDisplayLinkDelegate *g_delegate;
static id<CAMetalDrawable> tick_with(id<CAMetalDrawable> dr) {   // one display-link callback (it does not look at the link)
    FakeLinkUpdate *u = [FakeLinkUpdate new]; u.drawable = dr; u.targetPresentationTimestamp = CACurrentMediaTime() + 0.016;
    [g_delegate metalDisplayLink:(CAMetalDisplayLink *)(id)u needsUpdate:(CAMetalDisplayLinkUpdate *)u];
    return dr;
}
static id<CAMetalDrawable> tick(void) { return tick_with(drawable()); }
// The tick that lands on a used allocator runs on its own thread, so the test sees whether it waits; its drawable is taken before the thread starts
static id<CAMetalDrawable> g_held; static dispatch_semaphore_t g_held_done;
static void *held_tick(void *arg) { (void)arg; @autoreleasepool { tick_with(g_held); } dispatch_semaphore_signal(g_held_done); return NULL; }
static void reuse_case(id<CAMetalDrawable> dr, id<MTLSharedEvent> gate, uint64_t open, uint64_t ticket, int inflight, uint64_t old) {
    char name[160];
    g_held = dr; g_held_done = dispatch_semaphore_create(0); pthread_t th; pthread_create(&th, NULL, held_tick, NULL);
    int returned = dispatch_semaphore_wait(g_held_done, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC)) == 0;
    pthread_mutex_lock(&g_pq_mu); int inside = g_pq_read == ticket && g_pq_inflight == inflight; pthread_mutex_unlock(&g_pq_mu);
    snprintf(name, sizeof name, "ticket %llu reached present_now", (unsigned long long)ticket); CHECK(name, returned || inside);
    snprintf(name, sizeof name, "allocator %llu is not reused while ticket %llu is on the GPU", (unsigned long long)(ticket % 3), (unsigned long long)old); CHECK(name, !returned);
    gate.signaledValue = open;
    if (!returned) returned = dispatch_semaphore_wait(g_held_done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) == 0;
    snprintf(name, sizeof name, "ticket %llu is presented once the GPU gets to ticket %llu", (unsigned long long)ticket, (unsigned long long)old); CHECK(name, returned);
    pthread_join(th, NULL);
    pthread_mutex_lock(&g_pq_mu); struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 5;
    while (g_pq_inflight && pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT) {}
    int left = g_pq_inflight; uint64_t done = g_pq_done; pthread_mutex_unlock(&g_pq_mu);
    CHECK("every present completed", left == 0 && done == ticket);
}
static int bad_pixels(id<MTLTexture> t, uint8_t left, uint8_t right) {
    id<MTLBuffer> out = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [g_q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(8, 8, 1)
        toBuffer:out destinationOffset:0 destinationBytesPerRow:32 destinationBytesPerImage:256];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    const uint8_t *p = out.contents; int bad = 0;
    for (int i = 0; i < 64; i++) { uint8_t v = i % 8 < 4 ? left : right; bad += p[4 * i] != v || p[4 * i + 1] != v || p[4 * i + 2] != v; }
    if (bad) { printf("first row:"); for (int i = 0; i < 8; i++) printf(" %u", p[4 * i]); printf(" (want %u x4, %u x4)\n", left, right); }
    return bad;
}
int main(void) { @autoreleasepool {
    alarm(30); setbuf(stdout, NULL);
    setenv("DM_PRESENT_HZ", "0", 1);
    mtl_init();
    g_test_layer = offscreen_layer(); g_layers = [NSMutableArray new];
    // Display-link mode without a link thread: link_sync sees a started link that is not there yet, the present thread leaves the queue to the callback
    g_dm_present_link = 1; g_link_started = 1; g_link_owns_layer = 1; g_delegate = [NvMetalDisplayLinkDelegate new];
    tex_t a = source(91001, 50, 150), b = source(91002, 100, 100), c = source(91003, 200, 200);
    static const int left_half[4] = {0, 0, 4, 8};
    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    // Every callback moves g_pq_done to ticket - 1, so it is no guard for the allocator: the presents below wait only if the allocator's own completion is checked.
    // On main both cases fail the reuse check, and the earlier ticket's drawable reads back black (0 in every pixel): its command buffer was encoded again before the GPU ran it

    // A dropped frame: ticket 1 on allocator 1, then 3 (2 dropped) on allocator 0, then 4 on allocator 1 again with two presents in flight
    [g_q4 waitForEvent:gate value:1];   // nothing committed to the Metal 4 queue from here runs until the gate opens
    present_enqueue(&a, 1. / 60, NULL, 0);
    id<CAMetalDrawable> first = tick();
    present_enqueue(&b, 1. / 60, NULL, 0); present_enqueue(&b, 1. / 60, NULL, 0); present_enqueue(&c, 1. / 60, left_half, 0);
    tick();
    printf("after the drop: read %llu, done %llu, in flight %d\n", (unsigned long long)g_pq_read, (unsigned long long)g_pq_done, g_pq_inflight);
    CHECK("ticket 1 has not completed", g_pq_inflight == 2);
    reuse_case(drawable(), gate, 1, 4, 2, 1);
    int bad = bad_pixels(first.texture, 50, 150); printf("ticket 1 drawable: %d of 64 pixels wrong\n", bad);
    CHECK("ticket 1 shows its own picture", bad == 0);

    // One frame per tick, nothing dropped: tickets 5, 6 and 7 on allocators 2, 0 and 1, then 8 on allocator 2 again with three presents in flight
    [g_q4 waitForEvent:gate value:2];
    present_enqueue(&a, 1. / 60, NULL, 0); id<CAMetalDrawable> fifth = tick();
    present_enqueue(&b, 1. / 60, NULL, 0); tick();
    present_enqueue(&b, 1. / 60, NULL, 0); tick();
    present_enqueue(&c, 1. / 60, left_half, 0);
    printf("one per tick: read %llu, done %llu, in flight %d, dropped %llu\n", (unsigned long long)g_pq_read, (unsigned long long)g_pq_done, g_pq_inflight, (unsigned long long)g_link_dropped);
    CHECK("tickets 5-7 have not completed", g_pq_inflight == 3 && g_link_dropped == 1);
    reuse_case(drawable(), gate, 2, 8, 3, 5);
    CHECK("no frame dropped after ticket 2", g_link_dropped == 1);
    bad = bad_pixels(fifth.texture, 50, 150); printf("ticket 5 drawable: %d of 64 pixels wrong\n", bad);
    CHECK("ticket 5 shows its own picture", bad == 0);
    printf("present allocators: %d checks, %d failed\n", checks, failed); return failed != 0;
} }
