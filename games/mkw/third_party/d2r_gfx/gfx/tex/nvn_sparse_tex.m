// Sparse textures backed by Metal 4 placement-sparse textures (nvn_sparse_tex.h).
// The guest's sparse layout (nvn_tex.c) and Metal's agree tile for tile: a tiled level is laid out in whole 64 KB tiles (block height = the tile's height,
// the GOB columns padded to whole tiles), so tile (x, y) of level l in layer z is the 64 KB at layer z + level offset + (y * tiles per row + x) * 64 KB,
// a block-linear image of its own; the levels from the first one smaller than a tile (in width or height) on form the guest's mip tail, packed after
// the tiled levels of each layer. Metal's placement-sparse texture has the same 64 KB tile shapes for 64 KB pages; its mip tail can start later (it
// keeps a level that is a tile wide but less than a tile high as tiles), so the guest's tail maps and fills Metal's tiles of those levels and its tail.
#import "nvn_sparse_tex.h"
#include "nvn_deswizzle.h"
#include "../nvn/nvn_hmap.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum { TILE = 0x10000, HEAP_PAGES = 2048, MAXLV = 16 };   // 64 KB tiles; heaps of 128 MB

// heap: retained id<MTLHeap>; free: a stack of free page indices; ref: the tiles mapped to each page; key: the guest memory a shared page holds (0: one tile's own)
typedef struct { void *heap; uint32_t nfree; uint16_t free[HEAP_PAGES]; uint32_t ref[HEAP_PAGES]; uint64_t key[HEAP_PAGES]; } sheap;
static sheap **g_heaps; static unsigned g_nheaps;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_pages, g_tiles;   // heap pages in use / tiles mapped to them, under g_mu
// Tiles with the same memory behind them share one heap page. Mario Kart World maps 3.47 GB of virtual pages of its sparse pools onto 0.98 GB of
// physical pages (the same physical tile behind many virtual tiles); a page of its own for each tile took 2.9 GB of heaps by the first race and 4.1 GB
// by the third. Key: the guest's physical address of the tile's 64 KB (one contiguous run) with the pixel format, which decides the tile's layout in
// the page; value: heap page + 1. Under g_mu
static nvn_hmap g_shared = {.klen = 8};
static uint64_t share_key(uint64_t phys, MTLPixelFormat pf, int gpu) { return phys ? phys | (uint64_t)pf << 52 | (uint64_t)gpu << 62 : 0; }
// Diagnostic for GPU-written textures (the reference entry's limit): the share keys of their pages that went back to the heaps while the guest kept the physical memory.
// The GPU keeps what such memory holds; the host does not, so a tile mapped onto it later starts as zeros (counted as lost, nvn_stex_lost), where a tile mapped onto
// memory no tile held before is new (counted as fresh). A key is forgotten when its physical pool goes (nvn_stex_forget). Under g_mu
enum { FREED_MAX = 1 << 20 };
#define KEY_GPU (1ull << 62)
#define KEY_ADDR(k) ((k) & ((1ull << 52) - 1))
static nvn_hmap g_freed = {.klen = 8};
static uint64_t g_lost_tiles, g_fresh_tiles;

typedef struct { int32_t page; uint8_t dirty; uint64_t key; } ent;   // page: heap index << 16 | page index (a tile), 0 (the mip tail entry, mapped), -1 = unmapped; key: its page's share key (0: not shared)
struct nvn_stex {
    void *tex;                       // id<MTLTexture>, retained while owned
    MTLPixelFormat pf;
    int share;                       // its tiles may share heap pages (block-compressed formats, and every GPU-written texture: see g_shared)
    int gpu;                         // GPU-written (nvn_stex_new): tiles are never uploaded, a page of its own starts as zeros
    int owned;                       // tex holds a reference (until nvn_stex_take)
    nvn_texdesc d, tile;             // the texture; one 64 KB tile as a block-linear texture of its own (for nvn_deswizzle)
    nvn_texlayout L;
    unsigned slices, levels, tiled, mtail, tail_pages, bw, bh, bpb;   // tiled: the guest's tiled levels; mtail: Metal's first level in its tail (levels when none)
    uint32_t tw, th;                 // a tile in texels
    uint32_t tx[MAXLV], ty[MAXLV];   // tiles per row and column of each level Metal tiles
    uint64_t lvoff[MAXLV + 1];       // level offsets in a layer; lvoff[tiled] = the guest's mip tail
    unsigned per_slice, tail_tiles;  // entries per layer: every tile of the guest's tiled levels, then one for its mip tail; Metal pages behind that tail
    ent *e;
    int32_t *tailpg;                 // slices × tail_tiles pages of the mapped mip tails
    int pending;
    uint64_t lost, fresh;            // GPU-written: tiles mapped onto memory whose page had gone back to the heaps (content lost) / onto memory no tile held before
};

static uint32_t mipd(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

int nvn_stex_wanted(id<MTLDevice> dev, const nvn_texdesc *d, MTLPixelFormat pf, uint64_t min_bytes) {
    if (!dev || !d || (d->target != 1 && d->target != 4) || !(d->flags & 0x20) || (d->flags & 0x52) || d->samples > 1) return 0;   // 2D / 2D array, SPARSE, not LINEAR / VIDEO_DECODE
    nvn_texlayout L; nvn_tex_layout(d, &L);
    if (L.size < min_bytes || L.align != TILE) return 0;
    uint32_t tw, th, td; int tiled = nvn_tex_sparse_tiles(d, &tw, &th, &td);
    if (tiled <= 0 || tiled > MAXLV || td != 1 || (d->levels ? d->levels : 1) > MAXLV) return 0;
    MTLSize ts = [dev sparseTileSizeWithTextureType:d->target == 4 ? MTLTextureType2DArray : MTLTextureType2D pixelFormat:pf sampleCount:1 sparsePageSize:MTLSparsePageSize64];
    return ts.width == tw && ts.height == th && ts.depth == 1;
}

nvn_stex *nvn_stex_new(id<MTLDevice> dev, MTLTextureDescriptor *desc, const nvn_texdesc *d, int gpu) {
    if (!nvn_stex_wanted(dev, d, desc.pixelFormat, 0)) return NULL;
    MTLTextureDescriptor *td = [desc copy];
    td.storageMode = MTLStorageModePrivate; td.placementSparsePageSize = MTLSparsePageSize64;
    // A GPU-written texture's tiles share pages by their physical memory as on the GPU (g_shared), whatever the format: it is not compressed losslessly,
    // whose metadata would not follow a page written through another texture (block-compressed formats are never compressed so)
    if (gpu) td.allowGPUOptimizedContents = NO;
    id<MTLTexture> t = [dev newTextureWithDescriptor:td];
    if (!t) return NULL;
    nvn_stex *s = calloc(1, sizeof *s);
    s->d = *d; nvn_tex_layout(d, &s->L); s->levels = d->levels ? d->levels : 1;
    int bw, bh; s->bpb = (unsigned)nvn_fmt_bpb(d->format, &bw, &bh); s->bw = (unsigned)bw; s->bh = (unsigned)bh; s->share = bw > 1 || gpu; s->gpu = gpu; s->owned = 1;
    uint32_t tdp; s->tiled = (unsigned)nvn_tex_sparse_tiles(d, &s->tw, &s->th, &tdp);
    s->mtail = (unsigned)MIN(t.firstMipmapInTail, (NSUInteger)s->levels);
    s->slices = d->target == 4 ? (d->depth ? d->depth : 1) : 1;
    // One tile as a texture: tw × th texels, its rows one block of tile-height GOBs (packaged block height), 64 KB
    unsigned rows = s->th / s->bh, h = 0; while ((8u << h) < rows) h++;
    s->tile = (nvn_texdesc){1, d->format, 0, 0, s->tw, s->th, 1, 1, 0, (int)h + 1};
    nvn_texlayout tl; nvn_tex_layout(&s->tile, &tl);
    uint32_t tile_gobs = s->tw / s->bw * s->bpb / 64;
    int ok = tl.size == TILE && (8u << h) == rows && t.tailSizeInBytes % TILE == 0 && tile_gobs && s->mtail >= s->tiled;
    for (unsigned l = 0; ok && l < s->mtail; l++) {
        s->tx[l] = (mipd(d->width, l) + s->tw - 1) / s->tw; s->ty[l] = (mipd(d->height, l) + s->th - 1) / s->th;
        if (l >= s->tiled) { s->tail_tiles += s->tx[l] * s->ty[l]; continue; }
        s->lvoff[l] = nvn_tex_level_offset(d, l);
        ok = nvn_tex_level_gobs(d, l) == s->tx[l] * tile_gobs;   // the level padded to whole tiles, as the guest lays it out
        s->per_slice += s->tx[l] * s->ty[l];
    }
    if (!ok) { free(s); return NULL; }
    s->lvoff[s->tiled] = s->tiled < s->levels ? nvn_tex_level_offset(d, s->tiled) : s->L.layer_size;
    s->tail_pages = s->mtail < s->levels ? (unsigned)(t.tailSizeInBytes / TILE) : 0;
    s->tail_tiles += s->tail_pages;
    if (s->tiled < s->levels) s->per_slice++;   // the guest's mip tail
    s->e = malloc(sizeof *s->e * s->per_slice * s->slices);
    for (size_t i = 0; i < (size_t)s->per_slice * s->slices; i++) s->e[i] = (ent){-1, 0, 0};
    s->pf = td.pixelFormat;
    s->tailpg = malloc(sizeof *s->tailpg * (s->tail_tiles ? s->tail_tiles : 1) * s->slices);
    s->tex = (__bridge_retained void *)t;
    return s;
}

id<MTLTexture> nvn_stex_texture(const nvn_stex *s) { return s ? (__bridge id<MTLTexture>)s->tex : nil; }
int nvn_stex_pending(const nvn_stex *s) { return s && s->pending; }
uint64_t nvn_stex_mapped_bytes(void) { pthread_mutex_lock(&g_mu); uint64_t b = g_pages * TILE; pthread_mutex_unlock(&g_mu); return b; }
uint64_t nvn_stex_tile_bytes(void) { pthread_mutex_lock(&g_mu); uint64_t b = g_tiles * TILE; pthread_mutex_unlock(&g_mu); return b; }
uint64_t nvn_stex_heap_bytes(void) { pthread_mutex_lock(&g_mu); uint64_t b = (uint64_t)g_nheaps * HEAP_PAGES * TILE; pthread_mutex_unlock(&g_mu); return b; }
void nvn_stex_lost(const nvn_stex *s, uint64_t *lost, uint64_t *fresh) {
    pthread_mutex_lock(&g_mu); *lost = s ? s->lost : g_lost_tiles; *fresh = s ? s->fresh : g_fresh_tiles; pthread_mutex_unlock(&g_mu);
}
void nvn_stex_forget(uint64_t lo, uint64_t n) {
    pthread_mutex_lock(&g_mu);
    for (uint32_t i = 0; i < g_freed.cap;) {   // a deletion shifts later entries back into slot i: look at it again
        uint64_t k; memcpy(&k, g_freed.keys + (size_t)i * 8, 8);
        if (g_freed.vals[i] && KEY_ADDR(k) >= lo && KEY_ADDR(k) - lo < n) nvn_hm_del(&g_freed, &k); else i++;
    }
    pthread_mutex_unlock(&g_mu);
}

// A heap page for one tile (→ heap << 16 | page, -1 when no heap can be had): the page already holding key's memory if there is one (key 0: a page of its own);
// *alone: 1 when the page is new to this tile (its content undefined); *lost: 1 when it is a GPU-written key whose page had gone back to the heaps (g_freed)
static int page_alloc(id<MTLDevice> dev, const nvn_stex_host *h, uint64_t key, int *alone, int *lost) {
    *lost = 0;
    pthread_mutex_lock(&g_mu);
    void *sh = key ? nvn_hm_get(&g_shared, &key) : NULL;
    if (sh) { int p = (int)((intptr_t)sh - 1); g_heaps[p >> 16]->ref[p & 0xffff]++; g_tiles++; pthread_mutex_unlock(&g_mu); *alone = 0; return p; }
    int p = -1; id<MTLHeap> fresh = nil;
    for (unsigned i = 0; i < g_nheaps && p < 0; i++) if (g_heaps[i]->nfree) p = (int)(i << 16 | g_heaps[i]->free[--g_heaps[i]->nfree]);
    if (p < 0) {
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.type = MTLHeapTypePlacement; hd.storageMode = MTLStorageModePrivate; hd.size = (NSUInteger)HEAP_PAGES * TILE; hd.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;
        fresh = g_nheaps < 0x8000 ? [dev newHeapWithDescriptor:hd] : nil;
        if (!fresh) { pthread_mutex_unlock(&g_mu); return -1; }
        sheap *hp = calloc(1, sizeof *hp), **grown = hp ? realloc(g_heaps, sizeof *g_heaps * (g_nheaps + 1)) : NULL;
        if (!grown) { free(hp); pthread_mutex_unlock(&g_mu); return -1; }   // fresh goes with ARC, as a heap Metal did not give
        g_heaps = grown; hp->heap = (__bridge_retained void *)fresh;
        for (unsigned k = 0; k < HEAP_PAGES; k++) hp->free[k] = (uint16_t)(HEAP_PAGES - 1 - k);   // pops 0, 1, 2, …
        hp->nfree = HEAP_PAGES - 1;                                                                // page 0 is this call's
        g_heaps[g_nheaps] = hp;
        p = (int)(g_nheaps++ << 16);
    }
    sheap *hp = g_heaps[p >> 16]; hp->ref[p & 0xffff] = 1; hp->key[p & 0xffff] = key;
    if (key & KEY_GPU) { *lost = nvn_hm_del(&g_freed, &key) != NULL; if (*lost) g_lost_tiles++; else g_fresh_tiles++; }
    if (key) nvn_hm_put(&g_shared, &key, (void *)(intptr_t)(p + 1));
    g_pages++; g_tiles++;
    pthread_mutex_unlock(&g_mu);
    if (fresh && h && h->resident) h->resident(h->ctx, fresh);
    *alone = 1; return p;
}
static void page_free(int p) {   // one tile less on the page; the last one frees it
    pthread_mutex_lock(&g_mu); sheap *hp = g_heaps[p >> 16]; unsigned i = (unsigned)(p & 0xffff);
    if (!--hp->ref[i]) {
        if (hp->key[i] & KEY_GPU) { if (g_freed.n >= FREED_MAX) nvn_hm_clear(&g_freed, NULL); nvn_hm_put(&g_freed, &hp->key[i], (void *)1); }
        if (hp->key[i]) nvn_hm_del(&g_shared, &hp->key[i]);
        hp->key[i] = 0; hp->free[hp->nfree++] = (uint16_t)i; g_pages--; }
    g_tiles--; pthread_mutex_unlock(&g_mu);
}

typedef struct { MTL4UpdateSparseTextureMappingOperation op; int heap; } mapop;   // heap -1: an unmap
typedef struct { mapop *v; size_t n, cap; } oplist;
static void op_add(oplist *o, int map, MTLRegion r, unsigned lv, unsigned z, int page) {
    if (o->n == o->cap) { mapop *v = realloc(o->v, sizeof *o->v * (o->cap = o->cap ? o->cap * 2 : 256)); if (!v) abort(); o->v = v; }
    o->v[o->n++] = (mapop){{map ? MTLSparseTextureMappingModeMap : MTLSparseTextureMappingModeUnmap, r, lv, z, map ? (NSUInteger)(page & 0xffff) : 0}, map ? page >> 16 : -1};
}
static int op_cmp(const void *a, const void *b) { return ((const mapop *)a)->heap - ((const mapop *)b)->heap; }
static void ops_apply(id<MTL4CommandQueue> q, id<MTLTexture> t, oplist *o) {   // one call per heap (unmaps with no heap)
    if (!o->n) return;
    qsort(o->v, o->n, sizeof *o->v, op_cmp);
    MTL4UpdateSparseTextureMappingOperation *buf = malloc(sizeof *buf * o->n);
    for (size_t i = 0; i < o->n;) {
        size_t j = i; while (j < o->n && o->v[j].heap == o->v[i].heap) { buf[j - i] = o->v[j].op; j++; }
        id<MTLHeap> heap = nil;
        if (o->v[i].heap >= 0) { pthread_mutex_lock(&g_mu); heap = (__bridge id<MTLHeap>)g_heaps[o->v[i].heap]->heap; pthread_mutex_unlock(&g_mu); }
        [q updateTextureMappings:t heap:heap operations:buf count:j - i];
        i = j;
    }
    free(buf); o->n = 0;
}

// Entry k of layer z: a tile of the guest's tiled levels (its 64 KB and its place), or the guest's mip tail (its range, lv = tiled)
static void ent_geom(const nvn_stex *s, unsigned z, unsigned k, uint64_t *a, uint64_t *n, unsigned *lv, uint32_t *x, uint32_t *y) {
    uint64_t layer = (uint64_t)z * s->L.layer_size;
    for (unsigned l = 0; l < s->tiled; l++) {
        unsigned c = s->tx[l] * s->ty[l];
        if (k < c) { *lv = l; *x = k % s->tx[l]; *y = k / s->tx[l]; *a = layer + s->lvoff[l] + (uint64_t)k * TILE; *n = TILE; return; }
        k -= c;
    }
    *lv = s->tiled; *x = *y = 0; *a = layer + s->lvoff[s->tiled]; *n = s->L.layer_size - s->lvoff[s->tiled];
}
// Metal tile j behind a guest mip tail: the tiles of the levels Metal still tiles (tiled .. mtail - 1), then its own tail's pages
static void tail_tile(const nvn_stex *s, unsigned j, unsigned *lv, MTLRegion *r) {
    for (unsigned l = s->tiled; l < s->mtail; l++) {
        unsigned c = s->tx[l] * s->ty[l];
        if (j < c) { *lv = l; *r = MTLRegionMake3D(j % s->tx[l], j / s->tx[l], 0, 1, 1, 1); return; }
        j -= c;
    }
    *lv = s->mtail; *r = MTLRegionMake3D(j, 0, 0, 1, 1, 1);
}
// Maps (map = 1) or unmaps an entry's Metal tiles: → 1 when a page of its own came with it (its content undefined), 0 when it shares a page already holding its memory
// (or on an unmap), -1 when no heap page can be had (nothing of the entry stays mapped then). key: the share key of a tile's memory (page_alloc)
static int ent_map(nvn_stex *s, oplist *o, id<MTLDevice> dev, const nvn_stex_host *h, unsigned z, unsigned k, int map, uint64_t key) {
    ent *e = &s->e[(size_t)z * s->per_slice + k];
    uint64_t a, n; unsigned lv; uint32_t x, y; ent_geom(s, z, k, &a, &n, &lv, &x, &y);
    if (lv < s->tiled) {
        if (!map) { op_add(o, 0, MTLRegionMake3D(x, y, 0, 1, 1, 1), lv, z, 0); page_free(e->page); e->page = -1; e->key = 0; return 0; }
        int alone, lost, p = page_alloc(dev, h, key, &alone, &lost); if (p < 0) return -1;
        if (key & KEY_GPU) { if (lost) s->lost++; else if (alone) s->fresh++; }
        op_add(o, 1, MTLRegionMake3D(x, y, 0, 1, 1, 1), lv, z, p); e->page = p; e->key = key; return alone;
    }
    int32_t *pg = s->tailpg + (size_t)z * s->tail_tiles;
    for (unsigned j = 0; j < s->tail_tiles; j++) {
        unsigned tl; MTLRegion r; tail_tile(s, j, &tl, &r);
        if (!map) { op_add(o, 0, r, tl, z, 0); page_free(pg[j]); continue; }
        int alone, lost, p = page_alloc(dev, h, 0, &alone, &lost); (void)alone; (void)lost;   // a mip tail's pages are its own
        if (p < 0) { while (j--) { tail_tile(s, j, &tl, &r); op_add(o, 0, r, tl, z, 0); page_free(pg[j]); } return -1; }
        op_add(o, 1, r, tl, z, p); pg[j] = p;
    }
    e->page = map ? 0 : -1;
    return map;
}

// One upload of entry lv / (x, y): the region of level l it fills and the linear bytes it is deswizzled into (the whole tile, or the whole level of a tail)
static void up_geom(const nvn_stex *s, unsigned lv, uint32_t x, uint32_t y, unsigned l, MTLOrigin *o, MTLSize *sz, size_t *pitch, size_t *bytes) {
    uint32_t w = mipd(s->d.width, l), hh = mipd(s->d.height, l), x0 = 0, y0 = 0, cw = w, ch = hh;
    if (lv < s->tiled) { x0 = x * s->tw; y0 = y * s->th; w = MIN(s->tw, w - x0); hh = MIN(s->th, hh - y0); cw = s->tw; ch = s->th; }
    *o = MTLOriginMake(x0, y0, 0); *sz = MTLSizeMake(w, hh, 1);
    *pitch = (size_t)((cw + s->bw - 1) / s->bw) * s->bpb; *bytes = *pitch * ((ch + s->bh - 1) / s->bh);
}
static size_t up_bytes(const nvn_stex *s, unsigned lv) {   // staging for one entry, each copy 256-byte aligned
    size_t n = 0, pitch, b; MTLOrigin o; MTLSize sz;
    for (unsigned l = lv; l < (lv < s->tiled ? lv + 1 : s->levels); l++) { up_geom(s, lv, 0, 0, l, &o, &sz, &pitch, &b); n += (b + 255) & ~(size_t)255; }
    return n;
}

int64_t nvn_stex_sync(nvn_stex *s, id<MTL4CommandQueue> q, const nvn_stex_guest *g, const nvn_stex_host *h, uint64_t budget) {
    if (!s || !q || !g || !h) return -1;
    id<MTLTexture> t = (__bridge id<MTLTexture>)s->tex;
    const unsigned per = s->per_slice;   // entries per layer, at least one (a tile or the mip tail); kept so the entry index splits into layer and entry below
    if (!per) return 0;
    size_t ne = (size_t)per * s->slices; oplist o = {0}; int fail = 0;
    for (unsigned z = 0; z < s->slices; z++) for (unsigned k = 0; k < s->per_slice; k++) {
        ent *e = &s->e[(size_t)z * s->per_slice + k];
        uint64_t a, n; unsigned lv; uint32_t x, y; ent_geom(s, z, k, &a, &n, &lv, &x, &y);
        // A tile has memory or not; a mip tail of several 64 KB pages has memory when any of them has (its unmapped pages read 0, as on the GPU)
        int m = 0; for (uint64_t p = 0; p < n && !m; p += TILE) m = g->mapped(g->ctx, g->base + a + p, MIN((uint64_t)TILE, n - p));
        if (!m) { if (e->page >= 0) ent_map(s, &o, t.device, h, z, k, 0, 0); e->dirty = 0; continue; }
        // A tile's share key, asked for only when it is mapped anew or its pages changed (a new mapping in its range is a change): a tile whose memory moved
        // leaves the page it shared. A tile mapped anew is uploaded, so the page holds the memory as it is now, whoever uploaded it before; a GPU-written
        // texture's tile is not (its memory is the page's), a page of its own is zeroed
        #define KEY() (s->share && lv < s->tiled && g->backing ? share_key(g->backing(g->ctx, g->base + a), s->pf, s->gpu) : 0)
        if (e->page < 0) { int r = ent_map(s, &o, t.device, h, z, k, 1, KEY()); if (r < 0) fail = 1; else e->dirty = s->gpu ? (uint8_t)r : 1; }
        else if (g->changed(g->ctx, g->base + a, n)) {
            uint64_t key = KEY(); if (!s->gpu) e->dirty = 1;
            if (key != e->key) { ent_map(s, &o, t.device, h, z, k, 0, 0); int r = ent_map(s, &o, t.device, h, z, k, 1, key); if (r < 0) { fail = 1; e->dirty = 0; } else e->dirty = s->gpu ? (uint8_t)r : 1; }
        }
        #undef KEY
    }
    ops_apply(q, t, &o); free(o.v);
    // The entries uploaded now (up to the budget), their staging taken in one piece, then deswizzled into it, then the copies encoded
    size_t total = 0, last = 0; uint64_t bytes = 0;
    for (; last < ne; last++) {
        if (!s->e[last].dirty) continue;
        if (budget && bytes >= budget) break;
        uint64_t a, n; unsigned lv; uint32_t x, y; ent_geom(s, (unsigned)(last / per), (unsigned)(last % per), &a, &n, &lv, &x, &y);
        total += up_bytes(s, lv); bytes += n;
    }
    s->pending = 0; for (size_t i = last; i < ne && !s->pending; i++) s->pending = s->e[i].dirty;
    if (!total) return fail ? -1 : 0;
    size_t base; id<MTLBuffer> stg = h->stage(h->ctx, total, &base);
    if (!stg) { s->pending = 1; return -1; }
    typedef struct { size_t off, pitch, bytes; MTLOrigin o; MTLSize sz; unsigned z, l; } cp;
    cp *cps = malloc(sizeof *cps * (total / 256 + 1)); size_t ncp = 0, at = base; uint8_t *tmp = NULL; uint64_t tmp_n = 0;
    for (size_t i = 0; i < last; i++) {
        ent *e = &s->e[i]; if (!e->dirty) continue;
        unsigned z = (unsigned)(i / per); uint64_t a, n; unsigned lv; uint32_t x, y; ent_geom(s, z, (unsigned)(i % per), &a, &n, &lv, &x, &y);
        if (!s->gpu) { if (n > tmp_n) { free(tmp); tmp = malloc(n); tmp_n = n; } g->copy(g->ctx, tmp, g->base + a, n); }
        for (unsigned l = lv; l < (lv < s->tiled ? lv + 1 : s->levels); l++) {   // a tile, or every level of the mip tail
            cp c = {at, 0, 0, {0}, {0}, z, l}; up_geom(s, lv, x, y, l, &c.o, &c.sz, &c.pitch, &c.bytes);
            uint8_t *dst = (uint8_t *)stg.contents + at;
            if (s->gpu) memset(dst, 0, c.bytes);
            else if (lv < s->tiled ? nvn_deswizzle(&s->tile, 0, 0, tmp, dst, c.pitch) : nvn_deswizzle_part(&s->d, l, z, tmp, a, n, dst, c.pitch)) { fail = 1; continue; }
            cps[ncp++] = c; at += (c.bytes + 255) & ~(size_t)255;
        }
        e->dirty = 0;
    }
    free(tmp);
    id<MTL4ComputeCommandEncoder> enc = h->encoder(h->ctx);   // after the staging: taking it may have started a new upload batch
    if (enc && ncp) [enc barrierAfterQueueStages:MTLStageResourceState beforeStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];   // after the mapping updates
    for (size_t i = 0; enc && i < ncp; i++)
        [enc copyFromBuffer:stg sourceOffset:cps[i].off sourceBytesPerRow:cps[i].pitch sourceBytesPerImage:cps[i].bytes sourceSize:cps[i].sz
                  toTexture:t destinationSlice:cps[i].z destinationLevel:cps[i].l destinationOrigin:cps[i].o];
    free(cps);
    if (!enc) return -1;
    return fail && !bytes ? -1 : (int64_t)bytes;
}

id<MTLTexture> nvn_stex_take(nvn_stex *s) {
    if (!s || !s->owned) return nil;
    s->owned = 0; return (__bridge_transfer id<MTLTexture>)s->tex;   // s->tex stays as an unretained pointer for its syncs
}
void nvn_stex_free(nvn_stex *s, id<MTL4CommandQueue> q) {
    if (!s) return;
    oplist o = {0};
    if (!s->owned) q = nil;   // its texture is going away (nvn_stex_take): its mappings go with it, its pages only return to the heaps
    id<MTLTexture> t = s->owned ? (__bridge_transfer id<MTLTexture>)s->tex : nil;
    for (unsigned z = 0; z < s->slices; z++) for (unsigned k = 0; k < s->per_slice; k++)
        if (s->e[(size_t)z * s->per_slice + k].page >= 0) ent_map(s, &o, nil, NULL, z, k, 0, 0);
    if (q) ops_apply(q, t, &o);
    free(o.v); free(s->e); free(s->tailpg); free(s);
}
