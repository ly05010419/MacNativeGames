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
     A texture overlapping current regions partially gets a new region covering their union and their textures move
     there (nvn_stor_migrate.h); a union replacing regions that hold many textures takes room on the side it grows
     on, so the game's per-frame layouts widen it a few times and not every few frames (nvn_alias_plan.h, 2026-11-18).
   - Exact-address heaps: a texture that does not fit (its tiled size exceeds the guest size, e.g. 1492x1492 depth)
     keeps compression and shares a heap with other layouts of its format starting at the same address.
   Other pixel formats never share pages: compressible storage changes hands between formats while stale descriptors
   still read the earlier format's content (stor_compressed, 2026-10-15 flashes), which physical aliasing would break.
   A write through one layout leaves the overlapping layouts' content undefined, as on hardware. Same-layout
   descriptors keep sharing one MTLTexture (stor_key). A heap leaves the residency set and is freed once its last
   texture is deallocated (textures retire through the deferred-release queue after the GPU work that used them).
   OOT_STOR_ALIAS=0: one private allocation per layout (the default outside Ocarina of Time, below); OOT_STOR_ALIAS=2:
   exact-address heaps only (A/B). */
#include "nvn_alias_plan.h"
typedef struct { uint64_t mem, pf, samples; } stor_alias_key;
typedef struct stor_alias_heap { void *heap; stor_alias_key key; uint64_t lo, size, generation; int live, queued, current, region; struct stor_alias_heap *all_prev, *all_next; } stor_alias_heap;
static stor_alias_heap *g_alias_all;   // every live heap, current or not (stor_alias_report: which generations hold the memory)
static void stor_alias_migrate_to(stor_alias_heap *destination);
static uint64_t g_alias_migrated, g_alias_migration_skipped;   // nvn_stor_migrate.h
/* What made the newest heap, for the migration detail line (OOT_STOR_MIGRATE_LOG=1, nvn_stor_migrate.h): the texture's
   guest range, the extent of the current regions it merged and the new region. Written under g_alias_mu. */
static struct { uint64_t lo, hi, old_lo, old_hi, nlo, nhi; int merged, region; } g_alias_grow_note;
/* On by default wherever the alias heaps are (Ocarina of Time, STOR_ALIAS_DEFAULT): a new region heap takes the live
   textures of the earlier generations it covers, so those heaps go (nvn_stor_migrate.h). Without it a few live render
   targets kept every earlier generation: same OOT save and route on an M4 (2026-11-15, memory-seven-followup
   lifetime-run vs migrate-run), alias heaps 1,197 -> 758 MiB at the end and 1,249 -> 790 MiB after walking, for live
   targets whose guest ranges come to about 683 MB; same pictures, no GPU error, the longest frame while walking 990 ->
   137 ms. OOT_STOR_MIGRATE=0 keeps every generation, as before. */
static int stor_alias_migrate_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("OOT_STOR_MIGRATE"); on = e ? atoi(e) != 0 : 1; }
    return on;
}
static nvn_hmap g_alias_by_key = {.klen = sizeof(stor_alias_key)};   // exact-address key -> current heap
static stor_alias_heap **g_alias_regions; static int g_alias_nregions, g_alias_cregions;   // current region heaps (key.mem unused)
static pthread_mutex_t g_alias_mu = PTHREAD_MUTEX_INITIALIZER;     // lock order: g_alias_mu before g_res_mu
static stor_alias_heap **g_alias_retire; static int g_alias_nretire, g_alias_cretire;
static uint64_t g_alias_heaps_made, g_alias_heaps_freed, g_alias_heaps_live, g_alias_heap_bytes, g_alias_textures, g_alias_reused, g_alias_grown,
                g_alias_region_textures, g_alias_region_merges,
                g_alias_roomed, g_alias_room_bytes;   // regions made with room for growth, and that room in all (nvn_alias_plan.h)
/* Why a texture took the exact-address path (a heap per address and format), counted so that each one is accounted for:
   [0] larger in Metal than its guest range even without compression metadata (a region placement would reach into a
   neighbour's bytes: the only case that has to be exact), [1] misaligned with no room left in its range to start at an
   aligned address, [2] the region plan could not keep its offset aligned, [3] OOT_STOR_ALIAS=2 or OOT_ALIAS_PAD=0. */
static uint64_t g_alias_exact_why[4], g_alias_exact_bytes[4];
static uint64_t g_alias_padded, g_alias_pad_failed, g_alias_rebased;   // region textures started past a misaligned guest address / ones no region took (stor_alias_texture)
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
/* OOT_STOR_TRACE=<file>: every texture the alias heaps were asked for (P) and its release (D), to try placement rules
   offline on what the game really asks for (2026-11-18). A texture keeps its number through a migration.
   P <frame> <number> <metal format> <samples> <guest address hex> <region size> <region alignment> <whole size> <guest size> <1 placed on a region heap>
   D <frame> <number>
   Region size 0: the texture cannot go on a region heap (its Metal size passes its guest range, or the address is not
   aligned). Off by default; the file is flushed every 2 s from a utility queue. */
uint64_t dm_nvn_frame_count(void);
static FILE *g_alias_trace; static _Atomic int g_alias_trace_state; static _Atomic uint64_t g_alias_trace_numbers;
static int alias_trace_on(void) {
    int s = atomic_load(&g_alias_trace_state);
    if (s) return s > 0;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("OOT_STOR_TRACE");
        g_alias_trace = e && *e ? fopen(e, "w") : NULL;
        if (g_alias_trace) {
            setvbuf(g_alias_trace, NULL, _IOFBF, 1 << 20);
            dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
            dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), 2 * NSEC_PER_SEC, NSEC_PER_SEC / 2);
            dispatch_source_set_event_handler(timer, ^{ fflush(g_alias_trace); });
            dispatch_resume(timer);
            static dispatch_source_t keep; keep = timer;
        }
        atomic_store(&g_alias_trace_state, g_alias_trace ? 1 : -1);
    });
    return atomic_load(&g_alias_trace_state) > 0;
}
@interface NvAliasTrack : NSObject { @public stor_alias_heap *h; uint64_t number; int moved; } @end   // number, moved: the trace's
@implementation NvAliasTrack
- (void)dealloc {
    pthread_mutex_lock(&g_alias_mu); h->live--; stor_alias_queue_locked(h); pthread_mutex_unlock(&g_alias_mu);
    if (number && !moved && alias_trace_on()) fprintf(g_alias_trace, "D %llu %llu\n", (unsigned long long)dm_nvn_frame_count(), (unsigned long long)number);
}
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
        if (h->all_prev) h->all_prev->all_next = h->all_next; else g_alias_all = h->all_next;
        if (h->all_next) h->all_next->all_prev = h->all_prev;
        g_alias_heaps_freed++; g_alias_heaps_live--; g_alias_heap_bytes -= h->size;
    }
    g_alias_nretire = 0;
    pthread_mutex_unlock(&g_alias_mu);
    for (int i = 0; i < nd; i++) { if (!rt_heap_put((__bridge id<MTLHeap>)dead[i]->heap)) res_remove((__bridge id<MTLResource>)dead[i]->heap); CFRelease(dead[i]->heap); free(dead[i]); }
    free(dead);
}

/* g_alias_mu held: a new current heap of `size` bytes. */
static stor_alias_heap *stor_alias_heap_new_locked(MTLTextureDescriptor *d, stor_alias_key k, uint64_t lo, uint64_t size, int region) {
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement; hd.storageMode = MTLStorageModePrivate; hd.cpuCacheMode = d.cpuCacheMode;
    hd.hazardTrackingMode = MTLHazardTrackingModeUntracked;   // MTL4 queues order work by explicit barriers, as for device textures
    hd.size = size;
    id<MTLHeap> heap = rt_heap_on() ? rt_heap_take(d, size, 1) : nil;
    if (!heap) heap = [g_dev newHeapWithDescriptor:hd];
    if (!heap) { RED_ONCE(200, "alias heap of %llu bytes not created", (unsigned long long)size); return NULL; }
    stor_alias_heap *h = calloc(1, sizeof *h); if (!h) MDIE("alias heap entry");
    h->heap = (void *)CFBridgingRetain(heap); h->key = k; h->lo = lo; h->size = heap.size; h->current = 1; h->region = region;
    g_alias_heaps_made++; g_alias_heaps_live++; g_alias_heap_bytes += h->size;
    h->generation = g_alias_heaps_made;
    h->all_next = g_alias_all; if (g_alias_all) g_alias_all->all_prev = h; g_alias_all = h;
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
/* Region path: [lo, lo + size) is the texture's own guest range prefix. nil when it cannot be placed.
   A texture no region holds replaces the regions it touches by one heap over their union (their textures move,
   nvn_stor_migrate.h); nvn_alias_plan.h gives that heap room on the side it grows on when the regions hold many
   textures (2026-11-18: with the exact union Ocarina of Time replaced a 59 MiB region 3-7 frames in a row, 4.2 GiB of
   copies each). OOT_ALIAS_ROOM=0: the exact union, as before. */
static int stor_alias_room(void) { static int on = -1; if (on < 0) { const char *e = getenv("OOT_ALIAS_ROOM"); on = e ? atoi(e) != 0 : 1; } return on; }
static alias_span *g_alias_spans, *g_alias_others; static int g_alias_cspans;   // scratch under g_alias_mu
static uint64_t g_alias_want_base;   // set when the plan's base left the texture's offset misaligned: the base it would have had (under g_alias_mu)
static id<MTLTexture> stor_alias_region_locked(MTLTextureDescriptor *u, stor_alias_key k, uint64_t lo, uint64_t size, uint64_t align) {
    uint64_t hi = lo + size; int n = 0, m = 0;
    if (g_alias_cspans < g_alias_nregions) {
        g_alias_cspans = g_alias_nregions + 32;
        g_alias_spans = realloc(g_alias_spans, (size_t)g_alias_cspans * sizeof *g_alias_spans);
        g_alias_others = realloc(g_alias_others, (size_t)g_alias_cspans * sizeof *g_alias_others);
        if (!g_alias_spans || !g_alias_others) MDIE("alias region spans");
    }
    for (int i = 0; i < g_alias_nregions; i++) {
        stor_alias_heap *r = g_alias_regions[i];
        if (r->key.pf == k.pf && r->key.samples == k.samples) {
            if (r->lo <= lo && hi <= r->lo + r->size && (lo - r->lo) % align == 0) { g_alias_reused++; g_alias_region_textures++; return stor_alias_place_locked(r, u, lo - r->lo); }
            g_alias_spans[n++] = (alias_span){r->lo, r->size, (uint64_t)r->live, r->generation};
        } else g_alias_others[m++] = (alias_span){r->lo, r->size, (uint64_t)r->live, r->generation};
    }
    alias_plan plan = alias_region_plan(g_alias_spans, n, g_alias_others, m, lo, hi, stor_alias_room());
    uint64_t nlo = plan.lo, nhi = plan.hi, old_lo = UINT64_MAX, old_hi = 0; int merged = 0;
    if ((lo - nlo) % align) { g_alias_want_base = nlo; return nil; }   // the region's base must keep this texture's guest offset aligned
    for (int i = 0; i < g_alias_nregions; ) {   // the regions the texture touches; the plan's room stops short of the others
        stor_alias_heap *r = g_alias_regions[i];
        if (r->key.pf == k.pf && r->key.samples == k.samples && r->lo < hi && lo < r->lo + r->size) {
            old_lo = MIN(old_lo, r->lo); old_hi = MAX(old_hi, r->lo + r->size); stor_alias_region_drop_locked(r); merged++; } else i++;
    }
    if (merged) { g_alias_grown++; g_alias_region_merges += (uint64_t)merged; }
    if (merged) { uint64_t exact = MAX(hi, old_hi) - MIN(lo, old_lo); if (nhi - nlo > exact) { g_alias_roomed++; g_alias_room_bytes += nhi - nlo - exact; } }
    g_alias_grow_note.lo = lo; g_alias_grow_note.hi = hi; g_alias_grow_note.nlo = nlo; g_alias_grow_note.nhi = nhi;
    g_alias_grow_note.old_lo = merged ? old_lo : lo; g_alias_grow_note.old_hi = merged ? old_hi : lo; g_alias_grow_note.merged = merged; g_alias_grow_note.region = 1;
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
    if (h && h->size < sa.size) {
        g_alias_grow_note.lo = k.mem; g_alias_grow_note.hi = k.mem + sa.size; g_alias_grow_note.nlo = k.mem; g_alias_grow_note.nhi = k.mem + sa.size;
        g_alias_grow_note.old_lo = k.mem; g_alias_grow_note.old_hi = k.mem + h->size; g_alias_grow_note.merged = 1; g_alias_grow_note.region = 0;
        h->current = 0; nvn_hm_del(&g_alias_by_key, &k); stor_alias_queue_locked(h); h = NULL; g_alias_grown++; }
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
    id<MTLTexture> mt = nil; uint64_t before = g_alias_heaps_made;
    MTLSizeAndAlign sa = [g_dev heapTextureSizeAndAlignWithDescriptor:d];
    if (!sa.size) return nil;
    uint64_t trace_size = 0, trace_align = 0; int trace_region = 0, exact_why = 3;
    if (stor_alias_mode() != 2) {
        /* Keep lossless compression when the compressed texture already fits its guest range; drop it only when its
           metadata would reach past the guest range (e.g. 1536x864: 5570560 bytes compressed, 5505024 guest). */
        MTLTextureDescriptor *u = d; MTLSizeAndAlign su = sa;
        static int plain = -1;   // OOT_ALIAS_UNCOMPRESSED=1 (diagnostic): no region texture keeps lossless compression
        if (plain < 0) { const char *e = getenv("OOT_ALIAS_UNCOMPRESSED"); plain = e && atoi(e) != 0; }
        if (su.size > t->L.size || plain) { u = [d copy]; u.allowGPUOptimizedContents = NO; su = [g_dev heapTextureSizeAndAlignWithDescriptor:u]; }
        /* A guest address that is not a multiple of Metal's placement alignment (2026-11-24: 2,048 bytes for the larger
           targets, the game places at 512): the texture starts at the next multiple when it still ends inside its own
           guest range. It then lies in a part of that range, so textures whose guest ranges are apart stay apart, and
           ones that overlap may share bytes, as before. Without it such a texture took the exact-address path: a heap
           of its own per address, and the game moves these targets a few hundred bytes at a time. Ocarina of Time in
           VR at 2048x2208 per eye: 153 of 188 exact-address textures were of this kind, 2,064 MiB of heaps for guest
           ranges whose union is about 170 MiB. OOT_ALIAS_PAD=0: the exact-address path for them, as before. */
        static int pad_on = -1; if (pad_on < 0) { const char *e = getenv("OOT_ALIAS_PAD"); pad_on = !(e && !atoi(e)); }
        uint64_t rlo = k.mem, pad = su.align && rlo % su.align ? su.align - rlo % su.align : 0;
        int why = !su.size || su.size > t->L.size ? 0 : pad && !pad_on ? 3 : pad && su.size + pad > t->L.size ? 1 : 2;
        if (pad && pad_on && su.size && su.size + pad <= t->L.size) rlo += pad; else pad = 0;
        if (su.size && su.size <= t->L.size && su.align && rlo % su.align == 0) {
            trace_size = su.size; trace_align = su.align;
            pthread_mutex_lock(&g_alias_mu);
            g_alias_want_base = UINT64_MAX; mt = stor_alias_region_locked(u, k, rlo, su.size, su.align);
            if (!mt && g_alias_want_base != UINT64_MAX && pad_on) {
                /* The regions it touches start at an address this texture's alignment does not divide (their own
                   textures need less): start where the offset from that base is aligned instead, if that still ends
                   inside the guest range. The plan sees the same regions, so it answers with the same base. */
                uint64_t delta = (g_alias_want_base % su.align + su.align - k.mem % su.align) % su.align;
                if (su.size + delta <= t->L.size) { rlo = k.mem + delta; pad = delta; mt = stor_alias_region_locked(u, k, rlo, su.size, su.align); if (mt) g_alias_rebased++; }
            }
            if (mt && pad) g_alias_padded++; else if (pad) g_alias_pad_failed++;
            pthread_mutex_unlock(&g_alias_mu);
            if (mt) { if (u != d) g_alias_uncompressed++; trace_region = 1; goto placed; }
        }
        exact_why = why;
    }
    pthread_mutex_lock(&g_alias_mu); mt = stor_alias_exact_locked(d, k, sa); if (mt) { g_alias_exact_why[exact_why]++; g_alias_exact_bytes[exact_why] += sa.size; } pthread_mutex_unlock(&g_alias_mu);
placed:
    if (mt && alias_trace_on()) {
        NvAliasTrack *track = objc_getAssociatedObject(mt, &g_alias_track_key);
        if (track) {
            track->number = atomic_fetch_add(&g_alias_trace_numbers, 1) + 1;
            fprintf(g_alias_trace, "P %llu %llu %llu %llu %llx %llu %llu %llu %llu %d\n", (unsigned long long)dm_nvn_frame_count(), (unsigned long long)track->number,
                    (unsigned long long)k.pf, (unsigned long long)k.samples, (unsigned long long)k.mem, (unsigned long long)trace_size, (unsigned long long)trace_align,
                    (unsigned long long)sa.size, (unsigned long long)t->L.size, trace_region);
        }
    }
    if (mt && stor_alias_migrate_on() && g_alias_heaps_made != before) {
        NvAliasTrack *track = objc_getAssociatedObject(mt, &g_alias_track_key);
        if (track) stor_alias_migrate_to(track->h);
    }
    return mt;
}

static void stor_alias_report(void) {
    pthread_mutex_lock(&g_alias_mu);
    MLOG("[storage-alias] heaps live %llu (%llu MiB; %d current regions), made %llu, freed %llu, grown %llu (region merges %llu, with room %llu / %llu MiB of room in all); textures placed %llu (in regions %llu, uncompressed %llu, past a misaligned address %llu / not placed so %llu, at an offset aligned to the region %llu), onto an existing heap %llu",
         (unsigned long long)g_alias_heaps_live, (unsigned long long)(g_alias_heap_bytes >> 20), g_alias_nregions, (unsigned long long)g_alias_heaps_made,
         (unsigned long long)g_alias_heaps_freed, (unsigned long long)g_alias_grown, (unsigned long long)g_alias_region_merges,
         (unsigned long long)g_alias_roomed, (unsigned long long)(g_alias_room_bytes >> 20), (unsigned long long)g_alias_textures,
         (unsigned long long)g_alias_region_textures, (unsigned long long)atomic_load(&g_alias_uncompressed), (unsigned long long)g_alias_padded, (unsigned long long)g_alias_pad_failed, (unsigned long long)g_alias_rebased, (unsigned long long)g_alias_reused);
    MLOG("[storage-alias] exact-address placements so far, by reason: larger than the guest range %llu (%llu MiB placed), misaligned with no room %llu (%llu MiB), region plan refused %llu (%llu MiB), switched off %llu (%llu MiB)",
         (unsigned long long)g_alias_exact_why[0], (unsigned long long)(g_alias_exact_bytes[0] >> 20), (unsigned long long)g_alias_exact_why[1], (unsigned long long)(g_alias_exact_bytes[1] >> 20),
         (unsigned long long)g_alias_exact_why[2], (unsigned long long)(g_alias_exact_bytes[2] >> 20), (unsigned long long)g_alias_exact_why[3], (unsigned long long)(g_alias_exact_bytes[3] >> 20));
    /* Which heaps hold the memory (2026-11-15: 1,197 MiB of heaps for render targets whose guest ranges are 73 MB): the
       current ones and the earlier generations a few live textures keep, with their textures. Counting only. */
    uint64_t n[2] = {0}, bytes[2] = {0}, tex[2] = {0}, few[2] = {0}, few_b[2] = {0};
    for (stor_alias_heap *h = g_alias_all; h; h = h->all_next) {
        int c = h->current != 0; n[c]++; bytes[c] += h->size; tex[c] += (uint64_t)h->live;
        if (h->live <= 2) { few[c]++; few_b[c] += h->size; }
    }
    MLOG("[storage-alias] by generation: current %llu heaps / %llu MiB / %llu textures; earlier %llu heaps / %llu MiB / %llu textures "
         "(heaps kept by at most 2 textures: current %llu / %llu MiB, earlier %llu / %llu MiB); migration moved %llu, skipped %llu",
         (unsigned long long)n[1], (unsigned long long)(bytes[1] >> 20), (unsigned long long)tex[1], (unsigned long long)n[0], (unsigned long long)(bytes[0] >> 20),
         (unsigned long long)tex[0], (unsigned long long)few[1], (unsigned long long)(few_b[1] >> 20), (unsigned long long)few[0], (unsigned long long)(few_b[0] >> 20),
         (unsigned long long)g_alias_migrated, (unsigned long long)g_alias_migration_skipped);
    pthread_mutex_unlock(&g_alias_mu);
}
