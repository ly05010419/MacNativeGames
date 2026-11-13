/* Storage aliasing for GPU-owned textures (included by nvn_mtl.m after the residency helpers).

   An NVN texture object is a descriptor over memory-pool bytes. Live objects may describe overlapping pool ranges with
   different layouts and on hardware they read and write the same bytes: render targets cost the pool, not one
   allocation per descriptor. Ocarina of Time places its transient render targets at shifting offsets of one 305 MiB
   pool (0x300000000, flags 0x2a1): 480 live storage textures took 1816 MiB of private Metal textures while the union
   of their guest ranges was 92 MiB; it also keeps 60-86 live depth descriptors of different sizes at one address
   (816 MiB as private textures, 14 MiB on hardware). Data: claude/oot-memory-alias/runtime-alias01 and codex
   runtime17-mapping-lastuse (2026-11-06).

   Textures are therefore placed on placement heaps so that physical overlap follows guest overlap:
   - Region heaps (one pixel format and sample count): a texture whose Metal size without lossless-compression
     metadata fits its guest size goes at heap offset = its guest offset from the region base, byte for byte (Metal's
     placement alignment, 128 bytes for these targets, divides the game's 512-byte placement). It then occupies only
     its own guest range: textures overlap physically exactly when their guest ranges overlap, as the NVN descriptors
     do. Those textures are created without GPU-optimized contents: compression metadata would extend them past their
     guest range into a neighbour's bytes. Compression is lossless, so pixels are unchanged; the cost is bandwidth.
     A texture overlapping current regions partially gets a new region covering their union; the textures on the old
     regions keep those heaps until they are freed.
   - Exact-address heaps: a texture that does not fit (its tiled size exceeds the guest size, e.g. 1492x1492 depth)
     keeps compression and shares a heap with other layouts of its format starting at the same address.
   Other pixel formats never share pages: compressible storage changes hands between formats while stale descriptors
   still read the earlier format's content (stor_compressed, 2026-10-15 flashes), which physical aliasing would break.
   A write through one layout leaves the overlapping layouts' content undefined, as on hardware. Same-layout
   descriptors keep sharing one MTLTexture (stor_key). A heap leaves the residency set and is freed once its last
   texture is deallocated (textures retire through the deferred-release queue after the GPU work that used them).
   OOT_STOR_ALIAS=0: one private allocation per layout (the default outside Ocarina of Time, below); OOT_STOR_ALIAS=2:
   exact-address heaps only (A/B). */
typedef struct { uint64_t mem, pf, samples; } stor_alias_key;
typedef struct stor_alias_heap { void *heap; stor_alias_key key; uint64_t lo, size; int live, queued, current, region; } stor_alias_heap;
static nvn_hmap g_alias_by_key = {.klen = sizeof(stor_alias_key)};   // exact-address key -> current heap
static stor_alias_heap **g_alias_regions; static int g_alias_nregions, g_alias_cregions;   // current region heaps (key.mem unused)
static pthread_mutex_t g_alias_mu = PTHREAD_MUTEX_INITIALIZER;     // lock order: g_alias_mu before g_res_mu
static stor_alias_heap **g_alias_retire; static int g_alias_nretire, g_alias_cretire;
static uint64_t g_alias_heaps_made, g_alias_heaps_freed, g_alias_heaps_live, g_alias_heap_bytes, g_alias_textures, g_alias_reused, g_alias_grown,
                g_alias_region_textures, g_alias_region_merges;
static _Atomic uint64_t g_alias_uncompressed;   // region textures created without lossless compression (it would pass their guest range)

/* On by default only in Ocarina of Time, where it is measured to pay: memory peak 6.7 -> 4.4 GB (f447609e). The region
   textures go without lossless compression, and that bandwidth is what a GPU-bound game pays: Breath of the Wild 1.9,
   same save (2026-11-13, botw160-play-1028): 70.9 fps / GPU 12.52 ms a frame with it, 76.0 / 10.99 without (main 77.9 /
   11.30). The other games keep their storage as before; OOT_STOR_ALIAS=1 turns it on for a measurement. */
#ifdef GAME_OOT
#define STOR_ALIAS_DEFAULT 1
#else
#define STOR_ALIAS_DEFAULT 0
#endif
static int stor_alias_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("OOT_STOR_ALIAS"); m = e ? atoi(e) : STOR_ALIAS_DEFAULT; } return m; }
static int stor_alias_on(void) { return stor_alias_mode() != 0; }

static void stor_alias_queue_locked(stor_alias_heap *h) {
    if (h->queued || h->live) return;
    if (g_alias_nretire == g_alias_cretire) {
        g_alias_cretire = g_alias_cretire ? g_alias_cretire * 2 : 16;
        g_alias_retire = realloc(g_alias_retire, (size_t)g_alias_cretire * sizeof *g_alias_retire);
        if (!g_alias_retire) MDIE("alias heap retire list");
    }
    h->queued = 1; g_alias_retire[g_alias_nretire++] = h;
}
static void stor_alias_region_drop_locked(stor_alias_heap *h) {   // no longer current: new textures go elsewhere
    for (int i = 0; i < g_alias_nregions; i++) if (g_alias_regions[i] == h) { g_alias_regions[i] = g_alias_regions[--g_alias_nregions]; break; }
    h->current = 0; stor_alias_queue_locked(h);
}
/* Attached to every texture placed on an alias heap; its dealloc runs when the texture itself is freed. Only takes
   g_alias_mu: it may run on any thread, including inside the deferred-release drain. */
@interface NvAliasTrack : NSObject { @public stor_alias_heap *h; } @end
@implementation NvAliasTrack
- (void)dealloc { pthread_mutex_lock(&g_alias_mu); h->live--; stor_alias_queue_locked(h); pthread_mutex_unlock(&g_alias_mu); }
@end
static char g_alias_track_key;

/* Submit thread, before the residency set is committed: heaps without textures leave it and are freed. */
static void stor_alias_collect(void) {
    pthread_mutex_lock(&g_alias_mu);
    if (!g_alias_nretire) { pthread_mutex_unlock(&g_alias_mu); return; }
    stor_alias_heap **dead = malloc((size_t)g_alias_nretire * sizeof *dead); int nd = 0;
    if (!dead) MDIE("alias heap collect");
    for (int i = 0; i < g_alias_nretire; i++) {
        stor_alias_heap *h = g_alias_retire[i]; h->queued = 0;
        if (h->live) continue;   // a texture was placed on it again meanwhile
        if (h->current) {
            if (h->region) { for (int j = 0; j < g_alias_nregions; j++) if (g_alias_regions[j] == h) { g_alias_regions[j] = g_alias_regions[--g_alias_nregions]; break; } }
            else if (nvn_hm_get(&g_alias_by_key, &h->key) == h) nvn_hm_del(&g_alias_by_key, &h->key);
        }
        dead[nd++] = h;
        g_alias_heaps_freed++; g_alias_heaps_live--; g_alias_heap_bytes -= h->size;
    }
    g_alias_nretire = 0;
    pthread_mutex_unlock(&g_alias_mu);
    for (int i = 0; i < nd; i++) { res_remove((__bridge id<MTLResource>)dead[i]->heap); CFRelease(dead[i]->heap); free(dead[i]); }
    free(dead);
}

/* g_alias_mu held: a new current heap of `size` bytes. */
static stor_alias_heap *stor_alias_heap_new_locked(MTLTextureDescriptor *d, stor_alias_key k, uint64_t lo, uint64_t size, int region) {
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement; hd.storageMode = MTLStorageModePrivate; hd.cpuCacheMode = d.cpuCacheMode;
    hd.hazardTrackingMode = MTLHazardTrackingModeUntracked;   // MTL4 queues order work by explicit barriers, as for device textures
    hd.size = size;
    id<MTLHeap> heap = [g_dev newHeapWithDescriptor:hd];
    if (!heap) { RED_ONCE(200, "alias heap of %llu bytes not created", (unsigned long long)size); return NULL; }
    stor_alias_heap *h = calloc(1, sizeof *h); if (!h) MDIE("alias heap entry");
    h->heap = (void *)CFBridgingRetain(heap); h->key = k; h->lo = lo; h->size = heap.size; h->current = 1; h->region = region;
    g_alias_heaps_made++; g_alias_heaps_live++; g_alias_heap_bytes += h->size;
    res_add((__bridge id<MTLResource>)h->heap);   // the heap is the allocation the residency set holds, not its textures
    return h;
}
/* g_alias_mu held: place d at `offset` of h and tie the heap's lifetime to the texture. */
static id<MTLTexture> stor_alias_place_locked(stor_alias_heap *h, MTLTextureDescriptor *d, uint64_t offset) {
    id<MTLTexture> mt = [(__bridge id<MTLHeap>)h->heap newTextureWithDescriptor:d offset:offset];
    if (!mt) { stor_alias_queue_locked(h); RED_ONCE(201, "alias heap texture %lux%lu format %lu not placed at %llu", (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long long)offset); return nil; }
    NvAliasTrack *tr = [NvAliasTrack new]; tr->h = h; h->live++; g_alias_textures++;
    objc_setAssociatedObject(mt, &g_alias_track_key, tr, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return mt;
}
/* Region path: [lo, lo + size) is the texture's own guest range prefix. nil when it cannot be placed. */
static id<MTLTexture> stor_alias_region_locked(MTLTextureDescriptor *u, stor_alias_key k, uint64_t lo, uint64_t size, uint64_t align) {
    uint64_t hi = lo + size;
    for (int i = 0; i < g_alias_nregions; i++) {
        stor_alias_heap *r = g_alias_regions[i];
        if (r->key.pf == k.pf && r->key.samples == k.samples && r->lo <= lo && hi <= r->lo + r->size && (lo - r->lo) % align == 0) {
            g_alias_reused++; g_alias_region_textures++; return stor_alias_place_locked(r, u, lo - r->lo);
        }
    }
    uint64_t nlo = lo, nhi = hi; int merged = 0;
    for (int i = 0; i < g_alias_nregions; i++) {
        stor_alias_heap *r = g_alias_regions[i];
        if (r->key.pf == k.pf && r->key.samples == k.samples && r->lo < hi && lo < r->lo + r->size) { nlo = MIN(nlo, r->lo); nhi = MAX(nhi, r->lo + r->size); }
    }
    if ((lo - nlo) % align) return nil;   // the union's base must keep this texture's guest offset aligned
    for (int i = 0; i < g_alias_nregions; ) {
        stor_alias_heap *r = g_alias_regions[i];
        if (r->key.pf == k.pf && r->key.samples == k.samples && r->lo < nhi && nlo < r->lo + r->size) { stor_alias_region_drop_locked(r); merged++; } else i++;
    }
    if (merged) { g_alias_grown++; g_alias_region_merges += (uint64_t)merged; }
    stor_alias_key rk = k; rk.mem = 0;
    stor_alias_heap *h = stor_alias_heap_new_locked(u, rk, nlo, nhi - nlo, 1);
    if (!h) return nil;
    if (g_alias_nregions == g_alias_cregions) {
        g_alias_cregions = g_alias_cregions ? g_alias_cregions * 2 : 32;
        g_alias_regions = realloc(g_alias_regions, (size_t)g_alias_cregions * sizeof *g_alias_regions);
        if (!g_alias_regions) MDIE("alias region list");
    }
    g_alias_regions[g_alias_nregions++] = h;
    g_alias_region_textures++;
    return stor_alias_place_locked(h, u, lo - nlo);
}
/* Exact-address path: layouts of one format starting at one address share offset 0 of one heap. */
static id<MTLTexture> stor_alias_exact_locked(MTLTextureDescriptor *d, stor_alias_key k, MTLSizeAndAlign sa) {
    stor_alias_heap *h = nvn_hm_get(&g_alias_by_key, &k);
    if (h && h->size < sa.size) { h->current = 0; nvn_hm_del(&g_alias_by_key, &k); stor_alias_queue_locked(h); h = NULL; g_alias_grown++; }
    if (!h) {
        h = stor_alias_heap_new_locked(d, k, k.mem, (sa.size + sa.align - 1) / sa.align * sa.align, 0);
        if (!h) return nil;
        nvn_hm_put(&g_alias_by_key, &h->key, h);
    } else g_alias_reused++;
    return stor_alias_place_locked(h, d, 0);
}

/* A new private texture for t placed on an alias heap of its storage, or nil (the caller allocates it on the device). */
static id<MTLTexture> stor_alias_texture(const tex_t *t, MTLTextureDescriptor *d) {
    if (!stor_alias_on() || !t->mem || !t->L.size || d.storageMode != MTLStorageModePrivate || d.textureType == MTLTextureTypeTextureBuffer) return nil;
    stor_alias_key k = {(uint64_t)(uintptr_t)t->mem, (uint64_t)d.pixelFormat, d.sampleCount ? (uint64_t)d.sampleCount : 1};
    id<MTLTexture> mt = nil;
    MTLSizeAndAlign sa = [g_dev heapTextureSizeAndAlignWithDescriptor:d];
    if (!sa.size) return nil;
    if (stor_alias_mode() != 2) {
        /* Keep lossless compression when the compressed texture already fits its guest range; drop it only when its
           metadata would reach past the guest range (e.g. 1536x864: 5570560 bytes compressed, 5505024 guest). */
        MTLTextureDescriptor *u = d; MTLSizeAndAlign su = sa;
        if (su.size > t->L.size) { u = [d copy]; u.allowGPUOptimizedContents = NO; su = [g_dev heapTextureSizeAndAlignWithDescriptor:u]; }
        if (su.size && su.size <= t->L.size && su.align && k.mem % su.align == 0) {
            pthread_mutex_lock(&g_alias_mu); mt = stor_alias_region_locked(u, k, k.mem, su.size, su.align); pthread_mutex_unlock(&g_alias_mu);
            if (mt) { if (u != d) g_alias_uncompressed++; return mt; }
        }
    }
    pthread_mutex_lock(&g_alias_mu); mt = stor_alias_exact_locked(d, k, sa); pthread_mutex_unlock(&g_alias_mu);
    return mt;
}

static void stor_alias_report(void) {
    pthread_mutex_lock(&g_alias_mu);
    MLOG("[storage-alias] heaps live %llu (%llu MiB; %d current regions), made %llu, freed %llu, grown %llu (region merges %llu); textures placed %llu (in regions %llu, uncompressed %llu), onto an existing heap %llu",
         (unsigned long long)g_alias_heaps_live, (unsigned long long)(g_alias_heap_bytes >> 20), g_alias_nregions, (unsigned long long)g_alias_heaps_made,
         (unsigned long long)g_alias_heaps_freed, (unsigned long long)g_alias_grown, (unsigned long long)g_alias_region_merges, (unsigned long long)g_alias_textures,
         (unsigned long long)g_alias_region_textures, (unsigned long long)atomic_load(&g_alias_uncompressed), (unsigned long long)g_alias_reused);
    pthread_mutex_unlock(&g_alias_mu);
}
