/* Render-target heap recycling, adapted to our storage owners from
   seedds/nxnative 7f502220 (MIT; LICENSE.nxnative). No live alias is moved.
   A heap enters this cache only after the last texture/view is destroyed;
   deferred texture ownership has already protected its final GPU use. */
enum { RT_HEAP_CAP = 256u << 20, RT_HEAP_SLOTS = 64 };
static const uint64_t RT_HEAP_IDLE_NS = 3000000000ull;
typedef struct { void *heap; uint64_t bytes, since; } rt_heap_spare;
static pthread_mutex_t g_rt_heap_mu = PTHREAD_MUTEX_INITIALIZER;
static rt_heap_spare g_rt_heaps[RT_HEAP_SLOTS];
static unsigned g_rt_heap_n;
static uint64_t g_rt_heap_bytes, g_rt_heap_made, g_rt_heap_reused, g_rt_heap_freed;
static _Atomic int g_rt_heap_on = -1, g_rt_heap_timer;
static int rt_heap_on(void) {
    int on = atomic_load(&g_rt_heap_on);
    if (on < 0) { const char *e = getenv("TOTK_HEAP_REUSE"); int value = e && atoi(e), expected = -1;
        atomic_compare_exchange_strong(&g_rt_heap_on, &expected, value); on = atomic_load(&g_rt_heap_on); }
    return on;
}
static void rt_heap_schedule(uint64_t ns) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_rt_heap_timer, &expected, 1)) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, ns), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        atomic_store(&g_rt_heap_timer, 0); retire_idle_kick();
    });
}
static void rt_heap_drop(void *object) { res_remove((__bridge id<MTLResource>)object); CFRelease(object); }
static void rt_heap_trim_at(uint64_t now, int all) {
    void *drop[RT_HEAP_SLOTS]; unsigned nd = 0; uint64_t next = RT_HEAP_IDLE_NS;
    pthread_mutex_lock(&g_rt_heap_mu);
    for (unsigned i = 0; i < g_rt_heap_n;) {
        rt_heap_spare *e = &g_rt_heaps[i]; uint64_t age = now >= e->since ? now - e->since : 0;
        if (all || age >= RT_HEAP_IDLE_NS) { drop[nd++] = e->heap; g_rt_heap_bytes -= e->bytes; *e = g_rt_heaps[--g_rt_heap_n]; }
        else { if (RT_HEAP_IDLE_NS - age < next) next = RT_HEAP_IDLE_NS - age; i++; }
    }
    g_rt_heap_freed += nd; unsigned remaining = g_rt_heap_n;
    pthread_mutex_unlock(&g_rt_heap_mu);
    for (unsigned i = 0; i < nd; i++) rt_heap_drop(drop[i]);
    if (remaining) rt_heap_schedule(next);
}
static int rt_heap_put(id<MTLHeap> heap) {
    if (!rt_heap_on() || heap.size > RT_HEAP_CAP) return 0;
    void *drop[RT_HEAP_SLOTS]; unsigned nd = 0;
    pthread_mutex_lock(&g_rt_heap_mu);
    while (g_rt_heap_n && (g_rt_heap_n == RT_HEAP_SLOTS || g_rt_heap_bytes + heap.size > RT_HEAP_CAP)) {
        unsigned oldest = 0;
        for (unsigned i = 1; i < g_rt_heap_n; i++) if (g_rt_heaps[i].since < g_rt_heaps[oldest].since) oldest = i;
        drop[nd++] = g_rt_heaps[oldest].heap; g_rt_heap_bytes -= g_rt_heaps[oldest].bytes;
        g_rt_heaps[oldest] = g_rt_heaps[--g_rt_heap_n];
    }
    g_rt_heaps[g_rt_heap_n++] = (rt_heap_spare){(void *)CFBridgingRetain(heap),heap.size,clock_gettime_nsec_np(CLOCK_UPTIME_RAW)};
    g_rt_heap_bytes += heap.size; g_rt_heap_freed += nd;
    pthread_mutex_unlock(&g_rt_heap_mu);
    for (unsigned i = 0; i < nd; i++) rt_heap_drop(drop[i]);
    rt_heap_schedule(RT_HEAP_IDLE_NS); return 1;
}
static id<MTLHeap> rt_heap_take(MTLTextureDescriptor *d, uint64_t bytes, int exact) {
    void *got = NULL;
    pthread_mutex_lock(&g_rt_heap_mu); unsigned best = g_rt_heap_n;
    for (unsigned i = 0; i < g_rt_heap_n; i++) {
        rt_heap_spare *e = &g_rt_heaps[i]; id<MTLHeap> h = (__bridge id<MTLHeap>)e->heap;
        if (e->bytes < bytes || e->bytes - bytes > (exact ? 0 : bytes / 8) ||
            h.storageMode != d.storageMode || h.cpuCacheMode != d.cpuCacheMode ||
            (best < g_rt_heap_n && e->bytes >= g_rt_heaps[best].bytes)) continue;
        best = i;
    }
    if (best < g_rt_heap_n) { got = g_rt_heaps[best].heap; g_rt_heap_bytes -= g_rt_heaps[best].bytes;
        g_rt_heaps[best] = g_rt_heaps[--g_rt_heap_n]; g_rt_heap_reused++; }
    pthread_mutex_unlock(&g_rt_heap_mu);
    return got ? CFBridgingRelease(got) : nil;
}
@interface NvTargetHeapOwner : NSObject { @public id<MTLHeap> heap; } @end
@implementation NvTargetHeapOwner
- (void)dealloc { if (!rt_heap_put(heap)) res_remove((id<MTLResource>)heap); }
@end
static char g_rt_heap_owner_key;
static id<MTLTexture> rt_heap_texture(MTLTextureDescriptor *d) {
    if (!rt_heap_on()) return nil;
    MTLSizeAndAlign need = [g_dev heapTextureSizeAndAlignWithDescriptor:d]; if (!need.size) return nil;
    id<MTLHeap> h = rt_heap_take(d, need.size, 0);
    if (!h) { MTLHeapDescriptor *hd = [MTLHeapDescriptor new]; hd.type = MTLHeapTypePlacement; hd.size = need.size;
        hd.storageMode = d.storageMode; hd.cpuCacheMode = d.cpuCacheMode; hd.hazardTrackingMode = MTLHazardTrackingModeUntracked;
        h = [g_dev newHeapWithDescriptor:hd]; if (h) { pthread_mutex_lock(&g_rt_heap_mu); g_rt_heap_made++; pthread_mutex_unlock(&g_rt_heap_mu); } }
    if (h) res_add((id<MTLResource>)h); // heap textures themselves are skipped by res_add
    id<MTLTexture> t = h ? [h newTextureWithDescriptor:d offset:0] : nil;
    if (!t) { if (h && !rt_heap_put(h)) res_remove((id<MTLResource>)h); return nil; }
    NvTargetHeapOwner *owner = [NvTargetHeapOwner new]; owner->heap = h;
    objc_setAssociatedObject(t, &g_rt_heap_owner_key, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return t;
}
