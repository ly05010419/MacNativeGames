// Metal execution layer of the NVN null device (C interface): memory pool = zero-copy MTLBuffer, submit = MTLCommandBuffer, sync = MTLSharedEvent.
// Ordering convention (to avoid repeating the GO_MAP_SHARE corrupted-picture mistake of the x86 route): every visible effect of "GPU completion" (sync object ready, event values, counters)
// happens only after the Metal command buffer has actually completed; CPU-side waits look only at the completed value of the MTLSharedEvent.
#pragma once
#include <stdint.h>
#include <stddef.h>

void mtl_init(void);
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes);
void mtl_set_depth_zero(int on);          // NVN depth mode: 1 = near is 0 (D3D / Metal), 0 = near is -w (GL, NVN's default); picks the vertex shaders' entry point (see shader_load in nvn_mtl.m)
void mtl_set_origin_lower_left(int on);   // NVN window origin: 1 = lower left (GL, NVN's default), 0 = upper left (D3D / Metal); see nvn_mtl.m
void mtl_set_queue_origin_lower_left(int on); // Raster flip captured at NVN queue creation; selected under the submit lock.
void bf_want_note(const char *key);

// Memory pool: [mem, mem+size) is expanded outward to page boundaries to build a zero-copy buffer; look up the buffer and offset by GPU address (= CPU address)
void mtl_pool_add(const void *mem, uint64_t size);
void mtl_pool_del(const void *mem);
// memory pool Finalize: evict the textures built on this pool (pool object address) from the storage-sharing table
void mtl_alias_drop_pool(const void *pool);
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off);   // 1 on success; buf is a (__bridge) id<MTLBuffer>

typedef struct {
    uint32_t pso, cps, shaders, dss;
    uint32_t view_nonnull, view_slots, live_tex, sview_nonnull;
    uint32_t pools;
    uint64_t pool_adds;
    uint32_t c1_bufs; uint64_t c1_alloc, c1_data;   // Shader c1 constant segment: buffer count (4 MiB blocks + dedicated 64 KiB), allocated bytes, actual bytes
} mtl_res_count;
void mtl_res_counts(uint32_t tick, const char *why);   // Print one line of resource counts (the host calls it per tick / event, see dm_res_report_register in dm.h)
void mtl_res_snapshot(mtl_res_count *out);

// One submit: begin -> several operations -> commit (returns the fence value at which this submit completes)
void mtl_prepare(void);                                // the first submission's prepare step (prewarm, pipeline prebuild): no GPU work, called outside the graphics gate
void mtl_sub_begin(void);
void mtl_queue_activate(uint64_t serial);              // submit lock held: restore this queue's persistent GPU state
void mtl_queue_forget(uint64_t serial);                // finalize: discard its state, including guest object pointers
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n);               // Buffer copy in GPU ordering
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v);   // Write the event value after completion (0 write / 1 add)
void mtl_sub_counter(uint64_t addr, uint32_t type);                       // Write the counter record after completion
uint64_t mtl_sub_commit(void);

uint64_t mtl_last_fence(void);                       // Fence value of the most recent submit (0 = never submitted)
int mtl_wait(uint64_t fence, uint64_t timeout_ns);   // Returns 1 when the GPU has completed up to fence, 0 on timeout
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns);   // the same for a sync WindowAcquireTexture handed out (its own line in the wait ledger, DM_NVN_WAITS)
void mtl_finish(void);                               // Wait for all submitted commands to complete
void mtl_drain(void);                                // Wait for all commands on the queue (including presents) to complete: submit an empty command buffer and wait for it
void mtl_xv_window_tex(uint64_t serial);   // Register a window (swapchain) texture: the cross-submit validity ledger never collects it
// External producers (the Vulkan layer, dm/gfx/vk): the shared Metal device; a serial for one of their textures; present it once their event reaches ev_val (ticket for mtl_present_wait)
void *mtl_device(void);
void *mtl_device_classic(void);   // the same for a layer that draws only on classic queues: a device without Metal 4 (a virtual Mac) starts too, and its presents are dropped
uint64_t mtl_ext_tex(void *tex);
uint64_t mtl_present_ext(uint64_t serial, double period, void *ev, uint64_t ev_val, const int crop[4]);   // crop: x, y, w, h of the texture to show (NULL = all of it)
void mtl_present_wait(uint64_t ticket);   // as in nvn_obj.h: wait until that present has been handed to the GPU
