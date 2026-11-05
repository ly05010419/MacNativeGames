// Sparse textures on Metal: a texture of a sparse virtual pool (NVN flag SPARSE) backed by a Metal 4 placement-sparse texture, whose 64 KB tiles are
// mapped from shared placement heaps only where the guest pool has memory, and uploaded tile by tile. Memory follows what the game maps (its own
// budget), not the texture's size: Mario Kart World's 8192×8192×68 ASTC 8×8 and BC6H arrays are 1.45 GB and 5.8 GB of address space each, mostly unmapped.
// docs/reference/nvn-objects.md#sparse-textures-backed-by-metal-placement-sparse-textures
#pragma once
#import <Metal/Metal.h>
#include "../nvn/nvn_tex.h"

// The guest side, the caller's: its texture storage address and its pool's page table (hle_nvn.c nvn_sparse_*)
typedef struct {
    void *ctx;
    uint64_t base;                                                 // guest address of the texture's storage
    int (*mapped)(void *ctx, uint64_t a, uint64_t n);              // every page of [a, a + n) has memory behind it
    int (*changed)(void *ctx, uint64_t a, uint64_t n);             // [a, a + n) was written or mapped since the texture's previous sync (1 when unsure)
    void (*copy)(void *ctx, void *dst, uint64_t a, uint64_t n);    // gathers [a, a + n), unmapped pages as 0
    // Optional: the memory behind the 64 KB at a, the same number for every virtual tile with the same physical memory behind it (its physical address
    // when the 64 KB are one run of it), 0 when unknown or not one run. Tiles with the same memory share one heap page (NULL: none share)
    uint64_t (*backing)(void *ctx, uint64_t a);
} nvn_stex_guest;

// The host side, the caller's: staging memory and the encoder for the uploads, residency of the heaps
typedef struct {
    void *ctx;
    id<MTLBuffer> (*stage)(void *ctx, size_t n, size_t *off);      // n bytes of shared memory, alive until the encoder's command buffer completes (one call per sync)
    id<MTL4ComputeCommandEncoder> (*encoder)(void *ctx);           // the encoder of the upload batch, asked for after stage (which may have started a new batch)
    void (*resident)(void *ctx, id<MTLAllocation> a);              // a new placement heap: add it to the residency set the texture's users run with
} nvn_stex_host;

typedef struct nvn_stex nvn_stex;

// The textures backed this way: 2D or 2D array, block-linear with the SPARSE layout, at least min_bytes of storage, and Metal's 64 KB sparse tile
// of the pixel format equal to the guest's (the standard 64 KB tile shapes on both sides). Others keep the whole-texture path.
int nvn_stex_wanted(id<MTLDevice> dev, const nvn_texdesc *d, MTLPixelFormat pf, uint64_t min_bytes);
// desc: the texture as the caller would create it (type, format, size, levels, usage); the sparse texture has no memory until its first sync. NULL when
// Metal's tiles or mip tail differ from the guest's. gpu: a texture only the GPU writes (a sparse render target): its tiles are never uploaded (the guest's
// copy has no copy function), a tile's page is the memory the GPU writes, shared with every tile of one physical memory, and a page of its own starts as zeros
nvn_stex *nvn_stex_new(id<MTLDevice> dev, MTLTextureDescriptor *desc, const nvn_texdesc *d, int gpu);
id<MTLTexture> nvn_stex_texture(const nvn_stex *s);
// Gives the caller the texture's reference: the texture then lives as long as the caller keeps it, and nvn_stex_free (called when it goes, with q nil)
// only gives the pages back
id<MTLTexture> nvn_stex_take(nvn_stex *s);
// Brings the tiles in line with the guest: maps those whose guest pages have memory and unmaps those that lost it (mapping updates on q, so they
// come before the upload batch's command buffer, which must be committed to q later), then encodes the uploads of mapped tiles that changed, up to
// budget bytes (0 = no limit). Returns the bytes uploaded, -1 on a failure (no heap, no staging). Tiles left over stay pending (nvn_stex_pending).
// A GPU-written texture's guest gives copy NULL: its uploads are the zeros of new pages
int64_t nvn_stex_sync(nvn_stex *s, id<MTL4CommandQueue> q, const nvn_stex_guest *g, const nvn_stex_host *h, uint64_t budget);
int nvn_stex_pending(const nvn_stex *s);
// Unmaps its tiles (their pages go back to the heaps) and releases it; call when the GPU no longer uses the texture
void nvn_stex_free(nvn_stex *s, id<MTL4CommandQueue> q);
uint64_t nvn_stex_mapped_bytes(void);   // heap pages in use by every sparse texture (tiles with the same memory share one; the heaps can hold more)
uint64_t nvn_stex_tile_bytes(void);     // tiles mapped by every sparse texture, shared ones counted for each
uint64_t nvn_stex_heap_bytes(void);     // the placement heaps those pages come from (heaps stay once made)
// Diagnostic, GPU-written textures: tiles mapped onto physical memory whose heap page had gone back to the heaps when no tile mapped it any more (lost: the host
// starts them as zeros where the GPU keeps what the memory held), and tiles mapped onto memory no tile held before (fresh); for s, or all textures (s NULL)
void nvn_stex_lost(const nvn_stex *s, uint64_t *lost, uint64_t *fresh);
// The guest memory [lo, lo + n) is no longer physical memory a tile can map (its pool is finalized): a later mapping at those addresses counts as fresh
void nvn_stex_forget(uint64_t lo, uint64_t n);
