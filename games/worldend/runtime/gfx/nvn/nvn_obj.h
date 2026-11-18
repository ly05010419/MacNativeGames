// NVN host-side object layout and command record format: shared by hle_nvn.c (interface layer, writes) and nvn_mtl.m (Metal replay layer, reads).
// Objects are stored in place in the NVN object memory allocated by the game, with size not exceeding the reserved size in nvn.h (FITS static assertion).
// State objects (Blend / Color / ... 4–0x18 bytes) are small structs the game copies by value; their layout is packed in place with bit-fields.
#pragma once
#include <stdint.h>
#include <string.h>
#include "nvn.h"
#include "nvn_tex.h"

#define MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FITS(T, H) _Static_assert(sizeof(T) <= sizeof(H), #T " exceeds the reserved size of " #H)

typedef struct { uint32_t flags; } dev_builder_t;                       FITS(dev_builder_t, NVNdeviceBuilder);
typedef struct { uint32_t magic, flags, origin, depth_mode; void *dbg_cb, *dbg_data; } dev_t_;  FITS(dev_t_, NVNdevice);

typedef struct { NVNdevice *dev; uint32_t flags; uint64_t cmd_sz, comp_sz, ctl_sz, flush; void *mem; uint64_t mem_sz; } queue_builder_t;
FITS(queue_builder_t, NVNqueueBuilder);
typedef struct { uint32_t magic; NVNdevice *dev; uint64_t submits; } queue_t;                 FITS(queue_t, NVNqueue);

typedef struct { NVNdevice *dev; void *mem; uint64_t size; uint32_t flags; } pool_builder_t;  FITS(pool_builder_t, NVNmemoryPoolBuilder);
typedef struct { uint32_t magic, flags; uint8_t *mem; uint64_t size; } pool_t;                FITS(pool_t, NVNmemoryPool);

typedef struct { const pool_t *pool; int64_t off; int32_t n; } dpool_t;                       // Texture / sampler descriptor pool
FITS(dpool_t, NVNtexturePool); FITS(dpool_t, NVNsamplerPool);

typedef struct {
    NVNdevice *dev; nvn_texdesc d; pool_t *pool; int64_t off;
    uint8_t has_layout, layout[8];
    const void *pkg_data;                // TextureBuilderSetPackagedTextureData: copied into the storage at TextureInitialize (13 Sentinels)
    uint8_t set_swz : 1, stencil : 1; uint16_t swz;   // TextureBuilderSetSwizzle: r | g << 3 | b << 6 | a << 9 (NVN and Metal channel enums have the same values: 0 zero 1 one 2 R 3 G 4 B 5 A);
                                         // stencil: TextureBuilderSetDepthStencilMode STENCIL (the texture reads the stencil of its depth-stencil storage, as a stencil-mode view does)
} tex_builder_t;                                                                             FITS(tex_builder_t, NVNtextureBuilder);
// Field order packed to 0x80: newer SDKs (SMBW, NVN 55) allocate NVNtexture as 0x80 bytes (allocated from main 0x978c3c, initialized at 0x978cd4);
// the former 0x88 wrote past the object's end and corrupted the adjacent heap block's free list. has_layout sits in the alignment gap after d, layout and serial fill up to 0x80.
typedef struct {
    // set_swz / stencil / swz (as in tex_builder_t) take the alignment gap after d; the size is still 0x80
    uint32_t magic; nvn_texdesc d; uint8_t has_layout, set_swz : 1, stencil : 1; uint16_t swz; nvn_texlayout L; uint8_t *mem; pool_t *pool; int64_t off; uint8_t layout[8];
    uint64_t serial;                     // Initialization serial: the replay layer uses it to find the MTLTexture in the host table (the game may copy the object by value, so object pointers cannot be stored)
} tex_t;                                                                                     FITS(tex_t, NVNtexture);
_Static_assert(sizeof(tex_t) <= 0x80, "tex_t exceeds the newer SDKs' NVNtexture (0x80)");
// The newer SDK's (NVN 55, SMBW) NVNtextureView is only 16 bytes: the game keeps it in a 16-byte stack slot (main +0x5796c), and the former 28-byte record wrote out of bounds over the saved fp / lr,
//   so the function returned to 0x100000000 and crashed. Packed into 16 bytes: levels / layers / format / target + swizzle and depth-stencil mode (the last two only recorded for now)
typedef struct { uint8_t set_levels : 1, set_layers : 1, set_format : 1, set_target : 1, set_swizzle : 1, set_dsmode : 1, dsmode : 2;
                 uint8_t target, base_level, nlevels; uint16_t format, min_layer, nlayers; uint8_t swz[4]; uint16_t _rsv; } view_t;
FITS(view_t, NVNtextureView);
_Static_assert(sizeof(view_t) <= 16, "view_t exceeds the newer SDKs' NVNtextureView (16 bytes)");

// Sampler parameters (one copy shared by builder and object)
typedef struct { uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func, reduction; float lod_min, lod_max, lod_bias, aniso, border[4]; } samp_par;   // Enum values are all < 256; reduction takes the padding byte
typedef struct { NVNdevice *dev; samp_par p; } samp_builder_t;                                FITS(samp_builder_t, NVNsamplerBuilder);
typedef struct { uint32_t magic; samp_par p; } samp_t;                                        FITS(samp_t, NVNsampler);

// The per-stage shader keys are not kept in prog_t: the game's real NVNprogram may be smaller than the header's 0xC0 (growing prog_t corrupted the adjacent object in Mario); see prog_keys in hle_nvn.c
typedef struct { uint32_t magic, nstage; NVNshaderData sd[6]; uint64_t serial; } prog_t;     FITS(prog_t, NVNprogram);   // serial: table key for the replay layer's resolved result

typedef struct { uint32_t magic, fenced; uint64_t fence; } sync_t;                            FITS(sync_t, NVNsync);
typedef struct { const pool_t *pool; int64_t off; } ev_builder_t;                             FITS(ev_builder_t, NVNeventBuilder);
typedef struct { uint32_t magic; volatile uint32_t *val; } ev_t;                              FITS(ev_t, NVNevent);

typedef struct { NVNdevice *dev; void *native; int32_t ntex, interval; NVNtexture *tex[4]; } win_builder_t;
FITS(win_builder_t, NVNwindowBuilder);
// avail[i]: time at which swapchain image i can be acquired again (ns, monotonic clock) = its display time + one present period; pace: target time of this window's present pacer
typedef struct { uint32_t magic; void *native; int32_t ntex, interval, next, crop[4]; NVNtexture *tex[4]; uint64_t presents, avail[4], pace, ticket[4]; } win_t;   // ticket[i]: ticket of the most recent present of image i
FITS(win_t, NVNwindow);

// Command buffer: commands are written into the command memory given by the game (AddCommandMemory), handle = segment start address; when it runs out, the game is called back for more memory
typedef struct {
    uint32_t magic, recording;
    uint8_t *cur;                        // Write pointer of the current command memory segment
    uint64_t nvn_flag10;
                                         // +0x10 is left to the game: its inlined NVN header code writes byte 1 here after AddCommandMemory and before BeginRecording / CallCommands (an internal flag of real NVN; main+0x4da5c, main+0xb8dec etc.).
                                         // This used to be the low byte of end: rewritten to 01 it cut the segment end by up to 254 bytes, and SMBW's material setup handles (512-byte blocks at minimum) kept overflowing into host segments at the iPhone's addresses
    uint8_t *end;                        // End of the current command memory segment
    uint8_t *begin;                      // First record of this recording (its handle); recording 2 = not written yet
    PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb; void *cb_data;
    NVNdevice *dev;
    uint64_t used;                       // Bytes written since the game last added command memory, across recordings and host segments, including segment-switch JUMPs (returned by GetCommandMemoryUsed)
    int32_t tex_base, samp_base;         // Host-table bases of the texture / sampler pool this command buffer last bound (hle_nvn.c dpool_add; 0 = the first pool of its kind)
} cmd_t;                                                                                   FITS(cmd_t, NVNcommandBuffer);

// ---------- State objects (defaults follow the NVN / GL defaults) ----------
typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; } color_st;            FITS(color_st, NVNcolorState);
typedef struct { uint32_t mask; } chmask_st;            /* 4 bits per target: r=1 g=2 b=4 a=8, bit set = write */ FITS(chmask_st, NVNchannelMaskState);
typedef struct { uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7, eq_rgb : 3, eq_a : 3; } blend_st;
FITS(blend_st, NVNblendState);
typedef struct { uint8_t enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid; } ms_st;
FITS(ms_st, NVNmultisampleState);
typedef struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } poly_st;           FITS(poly_st, NVNpolygonState);
typedef struct {
    uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4,
             fail_f : 4, zfail_f : 4, zpass_f : 4, fail_b : 4, zfail_b : 4, zpass_b : 4;
} ds_st;                                                                                     FITS(ds_st, NVNdepthStencilState);
typedef struct { uint32_t fmt : 8, offset : 19, stream : 5; } va_st;                         FITS(va_st, NVNvertexAttribState);
typedef struct { uint32_t stride, divisor; } vs_st;                                          FITS(vs_st, NVNvertexStreamState);

// ---------- Command records: {op, size (including header, aligned to 8)} + payload ----------
typedef struct { uint32_t op, size; } op_hdr;
enum {
    OP_END, OP_JUMP, OP_BEGIN, OP_CALL,                                             // OP_BEGIN is no longer recorded (hle_nvn.c BeginRecording)
    OP_SIGNAL_EVENT, OP_REPORT_COUNTER, OP_COPY_B2B, OP_UPDATE_UB,                 // CPU-visible side effects
    OP_SET_RT, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_VIEWPORTS, OP_SCISSOR, OP_DEPTH_RANGE,
    OP_BIND_PROGRAM, OP_BIND_STATE, OP_BIND_VATTR, OP_BIND_VSTREAM, OP_BIND_VBUF,
    OP_BIND_UBO, OP_BIND_SSBO, OP_BIND_TEX, OP_BIND_SAMP,
    OP_STENCIL, OP_POLY_OFFSET, OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BARRIER,
    OP_SET_TEX_POOL, OP_SET_SAMP_POOL, OP_COPY_B2T, OP_COPY_STRIDE, OP_BLEND_COLOR, OP_COPY_T2B, OP_BIND_IMG, OP_COPY_T2T, OP_RESET_COUNTER, OP_CLEAR_TEX, OP_CLEAR_BUF, OP_DEPTH_CLAMP, OP_RENDER_COND, OP_FENCE_SYNC, OP_TF_BIND, OP_TF_STATE, OP_RAST_DISCARD, OP_VP_SWIZZLE, OP_BIND_TEX1, OP_PRIM_RESTART, OP_COUNT_
};
// Payload (immediately follows op_hdr)
typedef struct { int32_t n; const tex_t *color[8]; view_t cview[8]; uint8_t has_cview[8]; const tex_t *depth; view_t dview; uint8_t has_dview; } p_set_rt;
typedef struct { int32_t index, mask; float rgba[4]; } p_clear_color;
typedef struct { int32_t mode, pad; uint64_t addr; } p_render_cond;   // mode 0 / 1: SetRenderEnableConditional (render if the 64-bit counters at addr and addr + 16 are equal / not equal), 2 / 3: SetRenderEnable(true / false)
typedef struct { float depth; int32_t depth_mask, stencil, stencil_mask; } p_clear_ds;
typedef struct { int32_t first, n; float v[16][6]; } p_viewports;          // x, y, w, h, near, far (near and far are overridden by DEPTH_RANGE)
typedef struct { int32_t x, y, w, h; } p_scissor;
typedef struct { float n, f; } p_depth_range;
typedef struct { uint32_t on, index; } p_prim_restart;
typedef struct { int32_t on; } p_depth_clamp;   // SetDepthClamp: depth outside [near, far] is clamped instead of clipped (Metal's depth clip mode)
typedef struct { int32_t first, n; uint8_t sw[16][4]; } p_vp_swizzle;   // SetViewportSwizzles: per viewport the source of clip x, y, z, w (NVNviewportSwizzle) XOR the identity (0, 2, 4, 6), so 0 = unchanged
typedef struct { const prog_t *prog; int32_t stages; } p_bind_program;
typedef struct { int32_t kind; uint8_t bytes[0x18]; } p_bind_state;      // kind: 0 blend 1 color 2 chmask 3 ms 4 poly 5 ds
typedef struct { int32_t first, n; uint32_t st[16]; } p_bind_vattr;
typedef struct { int32_t first, n; vs_st st[16]; } p_bind_vstream;
typedef struct { int32_t first, n; NVNbufferRange r[16]; } p_bind_vbuf;
typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;  // UBO / SSBO
// defer / src: the handles of BindTexturesDeferred are read from src at execution time (h at recording time is only a fallback); the replay layer refreshes them with bh_fresh before reading the record (nvn_mtl.m)
typedef struct { int32_t stage, first, n, defer; uint64_t src; uint64_t h[32]; } p_bind_handles; // Separate texture / sampler handles
// nvnCommandBufferBindTexture (one combined handle) is laid out as the hardware's command: the handle 16 bytes from the start of the record, where an engine that patches its recorded commands
// in place writes a new one. Luigi's Mansion 3 takes the command memory base + GetCommandMemoryUsed before each such bind (main+0x7c8964) and stores a handle there + 0x10 every frame
// (main+0xb7d0d4); in a p_bind_handles record that word is the count, and the replay read a handle as a count of 535 million. h is the handle as the game gives it (32 bits: texture ID |
// sampler ID << 20, all a patch writes); tbase / sbase are the numbering bases of the descriptor pools the command buffer had bound (hle_nvn.c bind_handles), added when it is read.
// Read it with bind_rec, which gives a p_bind_handles
typedef struct { int32_t stage, slot; uint64_t h; int32_t tbase, sbase; } p_bind_tex1;
static inline const p_bind_handles *bind_rec(uint32_t op, const void *a, p_bind_handles *t) {
    if (op != OP_BIND_TEX1) return a;
    const p_bind_tex1 *b = a; uint64_t tid = b->h & 0xfffff, sid = (uint32_t)b->h >> 20;
    t->stage = b->stage; t->first = b->slot; t->n = 1; t->defer = 0; t->src = 0;
    t->h[0] = (tid ? tid + (uint64_t)b->tbase : 0) | (sid ? sid + (uint64_t)b->sbase : 0) << 20; return t;
}
// Texture slots of a stage: 0-127 nvnCommandBufferBindTexture's (combined handles), 128-255 the separate texture binds. The driver keeps the two in tables of their own (c2 0x20 + 8 r and
// 0x168 + 8 r, dm/gfx/tools/dmsh_build.py), which the shaders' reflection names as these slots, so a game may bind both kinds in the same frame (Monster Hunter Rise: BindTexture slot 0 beside separate slots 0-4)
#define NVN_SEPTEX_SLOT0 128
typedef struct { int32_t face, value_mask, mask, ref, which; } p_stencil;  // which: 0 value mask 1 write mask 2 reference value
typedef struct { float factor, units, clamp; } p_poly_offset;
typedef struct { int32_t prim, first, count, instances, base_instance; uint64_t ind; uint64_t mdi_cnt; int32_t mdi_i, pad; } p_draw;   // instances 0 counts as 1; ind != 0: DrawArraysIndirect, the arguments are read by the GPU from that address; mdi_cnt != 0: draw mdi_i of an indirect-count multi-draw (drawn only while mdi_i < the u32 at mdi_cnt)
typedef struct { int32_t prim, itype, count, base_vertex, base_instance, instances; uint64_t ibuf, ind; uint64_t mdi_cnt; int32_t mdi_i, pad; } p_draw_idx;   // ind != 0: DrawElementsIndirect, {count, instances, first index, base vertex, base instance} read by the GPU from that address; mdi_cnt: as in p_draw
// A draw record stops before ind unless it is an indirect draw or a multi-draw (hle_nvn.c rec_draw); read it with draw_rec / draw_idx_rec, which give the missing fields as 0.
// Games size command memory by what nvnCommandBufferGetCommandMemoryUsed reports, so a plain draw must not take more of it than the real driver's commands do. Breath of the Wild
// records each mesh's draw into a scratch command buffer and reserves that many bytes per mesh for the model's prerecorded draws, in the heap its resource size table sizes:
// at 0x50 bytes per recorded draw (0x30 now, with its END) three models needed one 4 KB page more than their table entry, and loading them crashed (main+0x155a7a4)
static inline const p_draw *draw_rec(const void *a, uint32_t size, p_draw *t) { if (size - 8 >= sizeof *t) return a; memset(t, 0, sizeof *t); memcpy(t, a, size - 8); return t; }
static inline const p_draw_idx *draw_idx_rec(const void *a, uint32_t size, p_draw_idx *t) { if (size - 8 >= sizeof *t) return a; memset(t, 0, sizeof *t); memcpy(t, a, size - 8); return t; }
typedef struct { int32_t x, y, z; uint64_t caller, ind; } p_dispatch;   // caller: return address of the game call site (to locate the dispatcher); ind != 0: DispatchComputeIndirect, the group counts are read by the GPU from that address
typedef struct { const dpool_t *pool; } p_set_pool;
typedef struct { uint64_t src; const tex_t *tex; view_t view; uint8_t has_view; NVNcopyRegion r; int32_t flags; } p_copy_b2t;
// A host-only bit in flags: DM_COPY_RESOLVE = this is a Downsample (multisample resolve), recorded as a texture copy so everything else treats it as one (reads the source, writes the destination)
#define DM_COPY_RESOLVE 0x40000000
typedef struct { const tex_t *src, *dst; view_t sv, dv; uint8_t has_sv, has_dv; NVNcopyRegion sr, dr; int32_t flags; } p_copy_t2t;   // Reused for T2B: src stores the destination buffer address
typedef struct { const tex_t *tex; view_t view; uint8_t has_view, is_uint; NVNcopyRegion r; int32_t mask; union { float f[4]; uint32_t u[4]; } c; } p_clear_tex;
typedef struct { int64_t stride; int image; } p_copy_stride;   // image: SetCopyImageStride (bytes per layer / depth slice) instead of SetCopyRowStride
typedef struct { uint64_t addr, size; uint32_t value, _pad; } p_clear_buf;   // buffer filled with a 32-bit value (ClearBuffer)
typedef struct { float rgba[4]; } p_blend_color;
typedef struct { int32_t index, pad; uint64_t addr, size; } p_tf_bind;   // BindTransformFeedbackBuffer(s): one record per buffer (index 0-3)
typedef struct { int32_t what, pad; uint64_t ctl; } p_tf_state;          // what: 0 begin, 1 end, 2 pause, 3 resume; ctl = the game's 32-byte control buffer
typedef struct { int32_t on; } p_rast_discard;

// Entry point of the Metal replay layer (nvn_mtl.m): replays a set of command handles into the current submission
void mtl_exec(const uint8_t *handle);
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value);   // in-order buffer fill within the submission (OP_CLEAR_BUF)
void mtl_exec_rest(const uint64_t *handles, int n);   // Tells which handles in this submission still follow the current handle (used by the pass lookahead across handles)
// Present: draw the swapchain texture to the front-end CAMetalLayer (only counts when there is no window)
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]);   // Enqueue a present and return the ticket immediately; period_s: minimum time this image is displayed (= present period, 0 = unlimited)
void mtl_present_wait(uint64_t ticket);                    // Wait until the present with this ticket has been processed (blit committed to the GPU queue)
void mtl_present_flush(void);                              // Wait until all enqueued presents have been processed
void mtl_present_test_stall(uint64_t ns);                  // Unit test: extra pause in ns before each present on the present thread
int mtl_display_hz(void);                                   // Maximum refresh rate of the screen (Hz) when there is a window, 0 without a window
void mtl_set_tex_pool_n(int n);                            // Texture descriptor pool capacity: texture IDs outside the pool are treated as 'empty slots' (read 0)
// Timing instrumentation (shared by Mac / iOS): submissions are tagged with the frame number; GPU time = sum of GPUEnd−GPUStart over all of that frame's command buffers, readable only after all complete (otherwise −1)
void mtl_set_frame(uint64_t f);
void mtl_ub_present(double iv_ms);   // Same-process alternating measurement (DM_NVN_UB): the present thread reports the actual present interval every frame
double mtl_gpu_ms(uint64_t f);
void mtl_sub_index(const uint64_t *h, int n);   // Start of submission: build the store lookahead index in replay order (nvn_mtl.m)
void xv_test_counts(int *dc, int *viol);
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1);   // The frame's earliest GPUStart / latest GPUEnd (mach absolute time, ns); returns 0 if not all complete or there is no GPU time
void mtl_stats(uint64_t *passes, uint64_t *draws);
void mtl_args_stats(uint64_t *enc, uint64_t *reuse);
void mtl_use_stats(uint64_t *call, uint64_t *skip);
uint64_t mtl_ds_omit(void);   // Cumulative number of passes with no depth attachment because the lookahead proved depth-stencil unused
uint64_t mtl_drawable_ns_take(void);   // Take and zero: cumulative ns spent waiting for nextDrawable in present   // Cumulative useResource calls actually issued / skipped by same-encoder dedup   // Cumulative argument buffer new-encode / reuse counts
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes);   // Cumulative attachment load / store bytes, cumulative number of full load-store passes
uint64_t mtl_udc_saved_bytes(void);                                      // Write-back bytes actually saved by DM_NVN_UNWRITTEN_DC
int mtl_has_window(void);
int mtl_fps_target(void);                                   // target frame rate 30 / 60 (power-saving mode, DM_FPS, the game itself asking for 30)
int mtl_warm_wait(int timeout_ms);
int mtl_link_active(void);
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns);   // vsyncs = the game's present interval
double mtl_presented_time(uint64_t n);
   // Actual presentation time (seconds) of the n-th present (counted from 1); 0 = not ready, −1 = dropped
// Texture release (at Finalize)
void mtl_tex_init(const tex_t *t);      // TextureInitialize: the serial belongs to a live texture object until mtl_tex_release
void mtl_tex_release(tex_t *t);
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads);
uint64_t mtl_pkg_miss(void);
uint64_t mtl_upload_ns_take(void);   // Take and zero: time spent uploading packaged textures on the submit thread since last time   // DM_NVN_DIRTYCHECK diagnostic: number of missed detections   // Packaged textures: number of first uploads, number of re-uploads due to pool content changes
uint64_t mtl_upload_gpu_ns_take(void);   // Take and zero: GPU execution time of upload command buffers (sum of GPUEndTime − GPUStartTime)
void mtl_create_stats(uint64_t *lib, uint64_t *pso);   // Cumulative: number of shader libraries loaded synchronously on the submit thread, number of render + compute pipelines created synchronously (those taken from warm-up do not count)
// CPU texture read: starting at level / layer0, written into dst with row / img byte strides
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);
// Descriptor pool registration (texture ID / sampler ID → object snapshot), program release
void mtl_reg_tex(int id, const tex_t *t, const view_t *v);
// textures written by WriteTexels (recorded by serial): the replay layer treats them like packaged textures, uploaded from guest memory and re-uploaded on dirty pages
void mtl_tex_cpu_written(const tex_t *t);
int mtl_tex_gpu_owned(uint64_t serial);                                  // its host texture is a render target or shader-writable: GPU content, never re-uploaded from guest memory
// A program's per-stage shader key (hle_nvn.c): the first 8 bytes of sha256(first 0x880 bytes of the control section (its own length when shorter) + code area). Computed at SetShaders (NVN lets the caller free the control section once it returns, and Unity does);
// a stage whose code area was not yet in any memory pool then is computed the first time it is asked for. Returns 0 = no such stage
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]);
void mtl_reg_samp(int id, const samp_par *p);
void mtl_prog_release(prog_t *p);
// New serial (> 0, globally increasing)
uint64_t nvn_new_serial(void);
