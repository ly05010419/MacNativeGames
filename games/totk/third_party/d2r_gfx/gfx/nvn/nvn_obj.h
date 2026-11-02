// NVN 宿主侧对象布局与命令记录格式：hle_nvn.c（接口层，写）与 nvn_mtl.m（Metal 回放层，读）共用。
// 对象就地存在游戏分配的 NVN 对象内存里，大小不超过 nvn.h 的保留大小（FITS 静态断言）。
// 状态对象（Blend / Color / … 4–0x18 字节）是游戏按值拷贝的小结构，布局用位域就地打包。
// NVN host-side object layout and command record format: shared by hle_nvn.c (interface layer, writes) and nvn_mtl.m (Metal replay layer, reads).
// Objects are stored in place in the NVN object memory allocated by the game, with size not exceeding the reserved size in nvn.h (FITS static assertion).
// State objects (Blend / Color / ... 4–0x18 bytes) are small structs the game copies by value; their layout is packed in place with bit-fields.
#pragma once
/* 1 in a Breath of the Wild build: defaults measured on Tears of the Kingdom only stay off there (#750). */
#ifdef GAME_BOTW
#define TOTK_BOTW_BUILD 1
#else
#define TOTK_BOTW_BUILD 0
#endif
#include <stdint.h>
#include <string.h>
#include "nvn.h"
#include "nvn_tex.h"

#define MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FITS(T, H) _Static_assert(sizeof(T) <= sizeof(H), #T " 超出 " #H " 的保留大小")

typedef struct { uint32_t flags; } dev_builder_t;                       FITS(dev_builder_t, NVNdeviceBuilder);
typedef struct { uint32_t magic, flags, origin, depth_mode; void *dbg_cb, *dbg_data; } dev_t_;  FITS(dev_t_, NVNdevice);

typedef struct { NVNdevice *dev; uint32_t flags; uint64_t cmd_sz, comp_sz, ctl_sz, flush; void *mem; uint64_t mem_sz; } queue_builder_t;
FITS(queue_builder_t, NVNqueueBuilder);
typedef struct { uint32_t magic; NVNdevice *dev; uint64_t submits; } queue_t;                 FITS(queue_t, NVNqueue);

typedef struct { NVNdevice *dev; void *mem; uint64_t size; uint32_t flags; } pool_builder_t;  FITS(pool_builder_t, NVNmemoryPoolBuilder);
// game_mem/host/span: TOTK host-owned VM storage for physical/virtual pools (totk_vpool.c); host is NULL for game-backed pools.
typedef struct { uint32_t magic, flags; uint8_t *mem; uint64_t size; uint8_t *game_mem, *host; uint64_t span; } pool_t; FITS(pool_t, NVNmemoryPool);

typedef struct { const pool_t *pool; int64_t off; int32_t n; } dpool_t;                       // 贴图 / 采样器描述符池 | Texture / sampler descriptor pool
FITS(dpool_t, NVNtexturePool); FITS(dpool_t, NVNsamplerPool);

typedef struct {
    NVNdevice *dev; nvn_texdesc d; pool_t *pool; int64_t off;
    uint8_t has_layout, layout[8];
    const void *pkg_data;                // TOTK: SetPackagedTextureData source, copied into storage at Initialize
} tex_builder_t;                                                                             FITS(tex_builder_t, NVNtextureBuilder);
typedef struct {
    uint32_t magic; nvn_texdesc d; uint8_t has_layout, layout[8];
    nvn_texlayout L; uint8_t *mem; pool_t *pool; int64_t off;
    uint64_t serial;                     // 初始化序号：回放层按它在宿主表里找 MTLTexture（游戏可能按值拷贝对象，不能存对象指针） | Initialization serial: the replay layer uses it to find the MTLTexture in the host table (the game may copy the object by value, so object pointers cannot be stored)
} tex_t;                                                                                     FITS(tex_t, NVNtexture);
// TOTK: the game reserves 16 bytes for NVNtextureView on its stack (main+0x823398 SetDefaults at sp+0x30, its own
// data at sp+0x40), so the view is packed into 16 bytes. swz = NVN view swizzle selectors.
typedef struct {
    uint8_t set_levels : 1, set_layers : 1, set_format : 1, set_target : 1, set_swizzle : 1, pad0 : 3;
    uint8_t base_level, nlevels, target; uint16_t min_layer, nlayers; uint16_t format; uint8_t swz[4]; uint8_t pad1[2];
} view_t;
_Static_assert(sizeof(view_t) == 16, "NVNtextureView is 16 bytes in TOTK's NVN");
FITS(view_t, NVNtextureView);

// 采样器参数（builder 与对象共用一份）
// Sampler parameters (one copy shared by builder and object)
typedef struct { uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func, reduction; float lod_min, lod_max, lod_bias, aniso, border[4], lod_snap; } samp_par;   // TOTK: lod_snap stored for queries (Metal has no equivalent)   // 枚举值都 < 256 | Enum values are all < 256
typedef struct { NVNdevice *dev; samp_par p; } samp_builder_t;                                FITS(samp_builder_t, NVNsamplerBuilder);
typedef struct { uint32_t magic; samp_par p; } samp_t;                                        FITS(samp_t, NVNsampler);
_Static_assert(sizeof(samp_par) == 44, "TOTK sampler parameters");

typedef struct { uint32_t magic, nstage; NVNshaderData sd[6]; uint64_t serial; } prog_t;     FITS(prog_t, NVNprogram);   // serial：回放层解析结果的表键 | serial: table key for the replay layer's resolved result

typedef struct { uint32_t magic, fenced; uint64_t fence; } sync_t;                            FITS(sync_t, NVNsync);
typedef struct { const pool_t *pool; int64_t off; } ev_builder_t;                             FITS(ev_builder_t, NVNeventBuilder);
typedef struct { uint32_t magic; volatile uint32_t *val; } ev_t;                              FITS(ev_t, NVNevent);

typedef struct { NVNdevice *dev; void *native; int32_t ntex, interval, active; NVNtexture *tex[4]; } win_builder_t;
FITS(win_builder_t, NVNwindowBuilder);
// avail[i]：交换链第 i 张可再次获取的时刻（ns，单调时钟）= 它的显示时刻 + 一个出图周期；pace：本窗口的出图节拍器目标时刻
// avail[i]: time at which swapchain image i can be acquired again (ns, monotonic clock) = its display time + one present period; pace: target time of this window's present pacer
typedef struct { uint32_t magic; void *native; int32_t ntex, interval, next, active, crop[4]; NVNtexture *tex[4]; uint64_t presents, avail[4], pace, ticket[4]; } win_t;   // ticket[i]：第 i 张最近一次出图的票号 | ticket[i]: ticket of the most recent present of image i
FITS(win_t, NVNwindow);

// Command records live in host memory; finished copies are owned by guest control memory.
typedef struct {
    uint32_t magic, recording;
    uint8_t *cur;
    uint64_t guest_flags;                // +0x10: NVN 55 inline code writes the dirty flag here
    uint8_t *end;
    // TOTK: NVN 55 headers inline fast paths that read the command write pointer at +0x20 and its end at +0x28
    // (main+0x70644c) and, when 0x30 bytes fit, emit raw GPU methods themselves. ~0 / 0 never fit, so every
    // call takes the API path (the *Deferred entry points).
    uint64_t inline_cur, inline_end;
    PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb; void *cb_data;
    NVNdevice *dev;
    uint8_t *seg;                        // TOTK: start of the host recording buffer (records never go into game memory)
    void *ctl; size_t ctl_size;          // TOTK: control memory given by the game; recording copies live as long as it
    size_t game_size;                    // TOTK: command memory the game added (accounting only; not used for storage)
    uintptr_t cmd_base;                  // TOTK: pool storage address + offset of the command memory the game added
    size_t cmd_used, ctl_used, rec_start;   // rec_start: cmd_used when the current recording began
    size_t hw_used, last_rec, hw_start;            // TOTK: command memory the real NVN driver would have consumed (what GetCommandMemoryUsed reports; the game sizes resource heaps from it), size of the last record           // TOTK: usage reported to the game since the memory was added (main+0x711e4c submits only non-empty buffers)
} cmd_t;                                                                                     FITS(cmd_t, NVNcommandBuffer);
_Static_assert(__builtin_offsetof(cmd_t, guest_flags) == 0x10, "NVN inline dirty flag");
_Static_assert(__builtin_offsetof(cmd_t, inline_cur) == 0x20 && __builtin_offsetof(cmd_t, inline_end) == 0x28, "NVN inline fast-path fields");

// ---------- 状态对象（默认值按 NVN / GL 默认） ----------
// ---------- State objects (defaults follow the NVN / GL defaults) ----------
typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; } color_st;            FITS(color_st, NVNcolorState);
typedef struct { uint32_t mask; } chmask_st;            /* 每目标 4 位：r=1 g=2 b=4 a=8，置位 = 写 | 4 bits per target: r=1 g=2 b=4 a=8, bit set = write */ FITS(chmask_st, NVNchannelMaskState);
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

// ---------- 命令记录：{op, size（含头，8 对齐）} + 载荷 ----------
// ---------- Command records: {op, size (including header, aligned to 8)} + payload ----------
typedef struct { uint32_t op, size; } op_hdr;
enum {
    OP_END, OP_JUMP, OP_BEGIN, OP_CALL,
    OP_SIGNAL_EVENT, OP_REPORT_COUNTER, OP_COPY_B2B, OP_UPDATE_UB,                 // CPU 可见副作用 | CPU-visible side effects
    OP_SET_RT, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_VIEWPORTS, OP_SCISSOR, OP_DEPTH_RANGE,
    OP_BIND_PROGRAM, OP_BIND_STATE, OP_BIND_VATTR, OP_BIND_VSTREAM, OP_BIND_VBUF,
    OP_BIND_UBO, OP_BIND_SSBO, OP_BIND_TEX, OP_BIND_SAMP,
    OP_STENCIL, OP_POLY_OFFSET, OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BARRIER,
    OP_SET_TEX_POOL, OP_SET_SAMP_POOL, OP_COPY_B2T, OP_COPY_STRIDE, OP_BLEND_COLOR, OP_COPY_T2B, OP_BIND_IMG, OP_COPY_T2T, OP_RESET_COUNTER, OP_CLEAR_TEX, OP_DEPTH_CLAMP, OP_RENDER_ENABLE, OP_RASTER_DISCARD, OP_RENDER_COND, OP_COUNT_
};
// 载荷（紧跟 op_hdr）
// Payload (immediately follows op_hdr)
typedef struct { int32_t n; const tex_t *color[8]; view_t cview[8]; uint8_t has_cview[8]; const tex_t *depth; view_t dview; uint8_t has_dview; } p_set_rt;
typedef struct { int32_t index, mask; float rgba[4]; } p_clear_color;
#ifdef GAME_BOTW
/* The same texture level / layer attached at more than one colour slot: only the first slot is attached (botw.md; the
   finding and fix come from nxnative, MIT, github.com/seedds/nxnative, rt_dup_slot). Breath of the Wild's G-buffer passes
   attach the albedo and another buffer twice each (c1 = c2, c3 = c4) with channel write masks 0 on the second ones. The
   Switch's ROP writes only masked channels, so the slots do not interfere; on Apple's tile-based GPUs each attachment has
   its own tile memory, stored back whole at the end of the pass, so the copy stored later (loaded, never written)
   overwrote the other's writes in a different order per tile: the albedo kept scattered squares. Returns the earlier slot
   holding the same image, or -1. Slots are compared by serial: records hold copies of the texture objects.
   The macros below are the call sites in nvn_mtl.m: they sit at the end of existing lines and expand to nothing for TOTK,
   so TOTK's code and line numbers (res_add_at) stay as they were. */
static inline int rt_dup_of(const p_set_rt *r, int i) {
    if (!r->color[i]) return -1;
    int lv = r->has_cview[i] && r->cview[i].set_levels ? r->cview[i].base_level : 0, ly = r->has_cview[i] && r->cview[i].set_layers ? r->cview[i].min_layer : 0;
    for (int j = 0; j < i; j++) {
        if (!r->color[j] || r->color[j]->serial != r->color[i]->serial) continue;
        int lj = r->has_cview[j] && r->cview[j].set_levels ? r->cview[j].base_level : 0, yj = r->has_cview[j] && r->cview[j].set_layers ? r->cview[j].min_layer : 0;
        if (lj == lv && yj == ly) return j;
    }
    return -1;
}
/* a clear of a slot left unattached goes to the slot holding the same image */
#define BOTW_RT_DUP_CLEARS() { for (int i_ = 1; i_ < R.rt.n && i_ < 8; i_++) { int j_ = rt_dup_of(&R.rt, i_); \
        if (j_ >= 0 && R.clear_c[i_]) { if (!R.clear_c[j_]) { R.clear_c[j_] = 1; R.cc[j_] = R.cc[i_]; } R.clear_c[i_] = 0; } } }
#define BOTW_RT_DUP_SKIP(i) if (rt_dup_of(&R.rt, i) >= 0) { static int said_; if (!said_++) MLOG("[botw] colour slot %d holds the same image as an earlier slot (serial %llu): left unattached (rt_dup_of)", i, (unsigned long long)R.rt.color[i]->serial); continue; }
/* prediction: the slot is left unattached when the pass begins */
#define BOTW_RT_DUP_PREDICT(r, cf) { for (int i_ = 1; i_ < (r)->n && i_ < 8; i_++) if (rt_dup_of(r, i_) >= 0) (cf)[i_] = 0; }
/* a clear of a slot left unattached: clear the slot holding the same image */
#define BOTW_RT_DUP_CLEAR_TO_FIRST(c) p_clear_color botw_to_first_; { int f_ = rt_dup_of(&R.rt, (c)->index); if (f_ >= 0) { botw_to_first_ = *(c); botw_to_first_.index = f_; (c) = &botw_to_first_; } }
/* "ashot <path>" requests (end of nvn_mtl.m), taken at the game's present call with the screenshot queue lock held */
void botw_ashot_poll(const void *t, long cur);
#define BOTW_ASHOT_POLL(t, cur) botw_ashot_poll((t), (cur));
/* Uncapped pacing ignores the game's present interval 2 by default (user 2026-10-19: "放开到120帧"; 2026-10-22: 120 by default, 2026-10-28: 144, up to 240 with TOTK_UNCAPPED_HZ); the
   real-time frame step keeps the game's speed (botw.md). TOTK_UNCAPPED_INTERVAL=0 keeps the interval where a clock
   divides it (TOTK_UNCAPPED_HZ, or the display rate on a phone). */
#define UNCAPPED_INTERVAL_FROM_ENV(e) ((e) ? atoi(e) != 0 : 1)
/* TOTK_T2B_LOG=1 (diagnostics, the black save thumbnail): every texture-to-buffer copy is logged with its source
   texture and destination, and after the GPU is done with how many destination bytes are not zero (end of nvn_mtl.m) */
void botw_t2b_note(const void *c, const void *tex, const void *mb, uint64_t mo, size_t n);
#define BOTW_T2B_NOTE(c, t, mb, mo, n) botw_t2b_note((c), (__bridge const void *)(t), (mb), (mo), (n));
/* A GPU copy into pool memory is newer than an earlier GPU rendering of a texture over the same bytes: that rendering's
   write-back record (wb_note, written back at nvnMemoryPoolInvalidateMappedRange) is dropped, else it overwrote the copy
   (the black save thumbnail, botw.md; end of nvn_mtl.m). */
void botw_gpu_copy_supersedes(const void *mb, uint64_t mo, size_t n);
#define BOTW_GPU_COPY_SUPERSEDES(mb, mo, n) botw_gpu_copy_supersedes((mb), (mo), (n));
void botw_serial_note(uint64_t serial, const char *what);
#define BOTW_SERIAL_NOTE(k, what) botw_serial_note((k), (what));
#define BOTW_TILELOG_NOTE(t) if (tilelog_match(t)) botw_serial_note((t)->serial, NULL);
void botw_tex_follow(const void *t);
#define BOTW_TEX_FOLLOW(t) botw_tex_follow(t);
int botw_dead_keep(uint64_t serial);
#define BOTW_DEAD_KEEP(k) if (botw_dead_keep(k)) continue;
#define BOTW_TEX_RELEASE_KEEP(k) if (!botw_dead_keep(k))
void botw_dead_forget(const uint64_t *serials, size_t n);
#define BOTW_DEAD_FORGET(s, n) botw_dead_forget((s), (n));
/* Key 3 (MetalFX 2K): Breath of the Wild's own upscale of the finished 1600x900 picture into the 1920x1080 canvas is the
   fragment shader bd7ffdd4 (a 3-vertex triangle: texel reads around the fragment's own coordinates, an edge-directed
   sharpening of the sampled colour; no dynamic resolution, so the valid region is the whole source). MetalFX replaces the
   whole draw (mfxup_dock; botw.md). */
void botw_mfx_scene_ready(void);   /* the upscale draw happened (or MetalFX replaced it): the canvas holds the scene */
/* The same shader in 1.6.0 and 1.9.0. It was first recorded as 0af88d51, the key the runtime gave it before the control
   section's own size went into the key (2026-10-20, botw.md); its code is byte for byte that of bd7ffdd4, the key since,
   so 1.6.0 never matched the old one: no scene for key 2 (#177) and no MetalFX in place of the upscale for key 3
   (2026-10-23). 1.9.0 found bd7ffdd4 by its 1600x900 source and 1920x1080 target on the Kakariko route. */
#define BOTW_MFXUP_KEY "bd7ffdd435c6c3bc997653d5fb9a0cc7f2f411dd51ce88c3cafe6574c8624908-"
#define BOTW_MFXUP_DOCK_KEY(fs) int botw_ = !strncmp((fs)->key, BOTW_MFXUP_KEY, 65); if (botw_) botw_mfx_scene_ready();
#define BOTW_AND_NOT_DOCK && !botw_
#define BOTW_MFXUP_DOCK_REGION(cw, ch, src) if (botw_ && !why) { (cw) = (src).width; (ch) = (src).height; }
/* Breath of the Wild's upscale draw writes RGB only (mask 0x7): the canvas keeps its cleared alpha, which nothing reads
   (the final copy into the swapchain is opaque); MetalFX writes alpha too. TOTK's draw writes all four. */
#define BOTW_MFXUP_MASK_BAD (botw_ ? (S.chmask.mask & 0x7) != 0x7 : (S.chmask.mask & 0xf) != 0xf)
/* Key 2 (MetalFX frame interpolation, totk_mfx.m): Breath of the Wild's canvas is RGB10A2 (no float format), its scene is
   finished by the upscale draw 0af88d51 (every later draw into the canvas is UI: the scene copy is taken before the first of
   them), and its camera block is vertex uniform 0, 2336 bytes (view, view-projection, projection, inverse view, near / far,
   aspect, field of view and size at the same offsets as TOTK's 2368-byte block; botw.md). */
#define MFX_CANVAS_FMT_OK(f) (pf_is_float(f) || (f) == MTLPixelFormatRGB10A2Unorm)
int botw_mfx_ui_draw(void *prog);   /* a draw into the canvas after the upscale draw is UI; the canvas is also drawn into before the scene (cleared and used early in the frame), those draws are not */
void botw_mfx_frame_reset(void);
#define MFX_UI_DRAW(pr) botw_mfx_ui_draw((__bridge void *)(pr))
#define BOTW_MFX_FRAME_RESET() botw_mfx_frame_reset();
/* A frame that draws no UI into the canvas has no scene copy; the first draw into a presented texture after the upscale
   takes the whole canvas as the scene and clears the UI layer (end of nvn_mtl.m). */
void botw_mfx_no_ui_scene(void);
#define BOTW_MFX_NO_UI_SCENE() botw_mfx_no_ui_scene();
/* Key 4 (ultrawide, game_patches_botw.c): every draw of this game into the canvas is a 6-index quad of its UI layouts
   (HUD, menus, map, the inventory's Link) except the upscale of the scene (a full-screen triangle) and the menus'
   blurred picture of it (a quad, botw_uw_scene_picture), which keep the full width. The quads drawn before the upscale are covered by it, so all of
   them are squeezed back to the middle 16:9 (the game's UI shaders are not TOTK's). */
#define UW_UI_BEGINS(pr) 1
int botw_uw_scene_picture(void);   /* end of nvn_mtl.m */
#define UW_NOT_UI_DRAW(vs, dw, di) ((dw ? dw->count : di ? di->count : 0) != 6 || botw_uw_scene_picture())
#define MFX_CAM_SLOT 0
#define MFX_CAM_SIZE 2336
/* Write-back staging (docs/botw-v1.9.0-port.md; end of nvn_mtl.m). Breath of the Wild invalidates 1x1 and 3x3 GPU-written textures every
   frame. The write-back waited for all GPU work (mtl_finish_gpu) and made one blit round trip per texture on the game
   thread: Kakariko ran at 49 FPS, 59 with "flag writeback 0". Now the submission that renders such a texture copies it
   into a shared stage buffer at its end (botw_wb_stage_encode); the invalidate waits only for the fences of the listed
   renderings and swizzles a staged texture from its stage. fence: the submission that rendered it last; stage_fence:
   the submission whose end copied it into stage. */
#define BOTW_WB_ENT_FIELDS uint64_t fence; void *stage; uint64_t stage_fence; uint64_t late_fence;
void botw_wb_sub_note(uint64_t serial);
#define BOTW_WB_NOTED(e, k) (e)->fence = g_fence + 1; botw_wb_sub_note(k); (e)->late_fence = botw_late_note(&(e)->t, (e)->obj, (e)->stamp, (e)->fence); botw_render_note(&(e)->t, (e)->obj, (e)->stamp);
#define BOTW_WB_STAGE_FREE(e) if ((e)->stage) { defer_obj_release((__bridge id)(e)->stage); CFRelease((e)->stage); } if (!((e)->late_fence && (e)->late_fence == (e)->fence)) botw_render_gone((e)->t.mem, (e)->obj);
void botw_wb_stage_encode(void);
#define BOTW_WB_STAGE() botw_wb_stage_encode();
/* Only a read-back blit (an entry not staged after its last rendering, wb_tex_back) needs the uploads finished: it runs
   on the other queue. A staged entry is swizzled from its stage once its fence has passed, so a list of staged entries
   does not wait for them: the uploads the replay of the next frame keeps committing sit behind this frame's GPU work, and
   the 1 ms polls held the game's main thread there for about a quarter of its time (1.6.0, Hebra, 2026-10-27). */
#define WB_STAGED(e) ((e)->stage && (e)->stage_fence >= (e)->fence)
#define BOTW_WB_IS_STAGED(e) WB_STAGED(e)   /* TOTK_TIMELINE wb_out: a list with a read-back blit */
#define WB_WAIT_GPU(list, nl) { uint64_t need_ = 0; int blit_ = 0; for (int i_ = 0; i_ < (nl); i_++) { if ((list)[i_]->fence > need_) need_ = (list)[i_]->fence; \
        if ((list)[i_]->stage_fence > need_) need_ = (list)[i_]->stage_fence; blit_ |= !WB_STAGED((list)[i_]); } if (need_) mtl_wait(need_, UINT64_MAX); \
        if (totk_tl_on()) totk_tl(TL_WB_STEP, 1, need_); \
        if (blit_ || !botw_wb_staged_skip_on()) { while (atomic_load(&g_up_pending)) usleep(1000); if (totk_tl_on()) totk_tl(TL_WB_STEP, 2, 0); } }
int botw_wb_staged_skip_on(void);   /* TOTK_WB_STAGED_NOWAIT=0 waits for the uploads also for staged lists (the old behaviour) */
int botw_wb_from_stage(void *e);   /* 1 written back from the stage, 0 failed, WB_POOL_GONE, -1 not staged */
#define BOTW_WB_FROM_STAGE(e, ok, layers) { int s_ = botw_wb_from_stage(e); if (s_ != -1) { (ok) = s_; (layers) = 0; } }
/* BOTW (nvn_mtl.m, after a re-upload): the range that tracked the texture can be gone by now (a neighbour pool destroyed,
   nvn_dirty.c); its protect then did nothing, and a kept "tracked" would let the replay's shortcut call it clean next time */
#define BOTW_REUP_TRACKED(pk, t) (pk)->tracked = nvn_dirty_tracked((t)->mem, (t)->L.size);
/* Water colours (end of nvn_mtl.m): tiny GPU renders the CPU reads without an invalidate are written into their memory
   when the submission that rendered them completes. */
void botw_tiny_note(const void *t, const void *o);
#define BOTW_TINY_NOTE(t, o) botw_tiny_note((t), (__bridge const void *)(o));
void botw_tiny_commit(void);
/* Grass masks (end of nvn_mtl.m): a target the flush has waited for in many frames is written into its memory when its
   submission completes, and the flush no longer waits for it. late_fence: the submission whose end copies it out. */
uint64_t botw_late_note(const tex_t *t, void *obj, uint64_t stamp, uint64_t fence);
void botw_late_commit(void);
int botw_late_filter(void **list, int nl);
void botw_late_drop(uintptr_t lo, uintptr_t hi);
void botw_late_copy_drop(uintptr_t lo, uintptr_t hi);   /* a GPU copy into the memory (botw_gpu_copy_supersedes) */
#define BOTW_TINY_COMMIT() botw_tiny_commit(); botw_late_commit();
#define BOTW_LATE_FILTER(list, nl) (nl) = botw_late_filter((void **)(list), (nl));
#define BOTW_LATE_DROP(lo, hi) botw_late_drop((lo), (hi));
/* Sinking legs (end of nvn_mtl.m): a write-back puts a GPU rendering into memory that other texture objects sample from
   copies uploaded earlier; their next use uploads the memory again, as their descriptors would read it on hardware. */
void botw_wb_landed(const void *mem);
uint64_t botw_wb_gen(void);
int botw_wb_newer(const void *mem, uint64_t gen);
#define BOTW_PKG_FIELDS uint64_t wb_gen;
#define BOTW_WB_LANDED(t) botw_wb_landed((t)->mem);
#define BOTW_PKG_WB_SEEN(pk) (pk)->wb_gen = botw_wb_gen();
#define BOTW_PKG_WB_TAKE() uint64_t wbg_ = botw_wb_gen();
#define BOTW_PKG_WB_SET(pk) (pk)->wb_gen = wbg_;
#define BOTW_PKG_WB_STALE(pk, t, clean) if (clean && botw_wb_newer((t)->mem, (pk)->wb_gen)) clean = 0;
/* ... and while a GPU rendering of the same memory and layout waits for its write-back (the CPU has not written there
   since), a sampled copy reads that rendering, as its descriptor would on hardware. */
void botw_render_note(const tex_t *t, void *obj, uint64_t stamp);
void *botw_render_alias(const tex_t *t, const void *own);   /* the pending rendering's MTLTexture (not retained: its record holds it) or NULL */
void botw_render_gone(const void *mem, const void *obj);   /* its write-back record was freed */
extern _Thread_local int botw_bra_attach;   /* tex_mtl for a render target: its own copy, never the alias */
#define BOTW_ATTACH_TEX_U(t) ((__bridge id<MTLTexture>)({ botw_bra_attach = 1; void *r_ = tex_mtl_p(t); botw_bra_attach = 0; r_; }))
#define BOTW_PKG_RENDER_ALIAS(pk, t) if (pk && pk->tex && !pk->owned && !botw_bra_attach) { void *a_ = botw_render_alias(t, (__bridge const void *)pk->tex); if (a_) return a_; }
#else
#define MFX_CANVAS_FMT_OK(f) pf_is_float(f)
#define MFX_UI_DRAW(pr) mfx_ui_vs(pr->sh[0]->key)
#define UW_UI_BEGINS(pr) mfx_ui_vs(pr->sh[0]->key)
#define UW_NOT_UI_DRAW(vs, dw, di) !uw_ui_vs(vs) || (dw ? dw->count : di ? di->count : 0) != 6
#define BOTW_MFX_FRAME_RESET()
#define BOTW_MFX_NO_UI_SCENE()
#define MFX_CAM_SLOT 1
#define MFX_CAM_SIZE 2368
#define BOTW_MFXUP_MASK_BAD (S.chmask.mask & 0xf) != 0xf
#define BOTW_MFXUP_DOCK_KEY(fs)
#define BOTW_AND_NOT_DOCK
#define BOTW_MFXUP_DOCK_REGION(cw, ch, src)
#define BOTW_T2B_NOTE(c, t, mb, mo, n)
#define BOTW_GPU_COPY_SUPERSEDES(mb, mo, n)
#define BOTW_SERIAL_NOTE(k, what)
#define BOTW_TILELOG_NOTE(t)
#define BOTW_TEX_FOLLOW(t)
#define BOTW_DEAD_KEEP(k)
#define BOTW_TEX_RELEASE_KEEP(k)
#define BOTW_DEAD_FORGET(s, n)
/* TOTK: targets the CPU polls without an invalidate (the grass cut map) are written back after the fence wait that
   follows their rendering (mtl_wb_fence_done, nvn_mtl.m; upstream issue #49). */
#define BOTW_WB_ENT_FIELDS uint64_t fence;
#define BOTW_WB_NOTED(e, k) (e)->fence = g_fence + 1;
#define BOTW_WB_STAGE_FREE(e)
#define BOTW_WB_STAGE()
int totk_wb_fence_on(void);
/* TOTK_WB_FENCE=0 waits for all submitted GPU work, as before. Otherwise a write-back waits only for the submissions that
   rendered the listed textures (the game waits for its own sync before invalidating), then for uploads to finish. */
#define BOTW_WB_IS_STAGED(e) 0
#define WB_WAIT_GPU(list, nl) { uint64_t need_ = 0; for (int i_ = 0; i_ < (nl); i_++) if ((list)[i_]->fence > need_) need_ = (list)[i_]->fence; \
    if (totk_wb_fence_on()) { if (need_) mtl_wait(need_, UINT64_MAX); while (atomic_load(&g_up_pending)) usleep(1000); } else mtl_finish_gpu(); }
#define BOTW_WB_FROM_STAGE(e, ok, layers)
#define BOTW_TINY_NOTE(t, o)
#define BOTW_TINY_COMMIT()
#define BOTW_LATE_FILTER(list, nl)
#define BOTW_LATE_DROP(lo, hi)
#define UNCAPPED_INTERVAL_FROM_ENV(e) e && atoi(e)
#define BOTW_RT_DUP_CLEARS()
#define BOTW_RT_DUP_SKIP(i)
#define BOTW_RT_DUP_PREDICT(r, cf)
#define BOTW_RT_DUP_CLEAR_TO_FIRST(c)
#define BOTW_ASHOT_POLL(t, cur)
#define BOTW_REUP_TRACKED(pk, t)
#define BOTW_PKG_FIELDS
#define BOTW_WB_LANDED(t)
#define BOTW_PKG_WB_SEEN(pk)
#define BOTW_PKG_WB_TAKE()
#define BOTW_PKG_WB_SET(pk)
#define BOTW_PKG_WB_STALE(pk, t, clean)
#define BOTW_PKG_RENDER_ALIAS(pk, t)
#define BOTW_ATTACH_TEX_U(t) tex_mtl(t)
#endif
/* Key 5 (MetalFX temporal anti-aliasing, totk_taa.m): the compose draw that samples the linear HDR scene at fragment
   texture slot 0 before tone mapping. TOTK: 0ac9a549 (tone mapping and bloom). Breath of the Wild 1.9.0: f7ee6fe1 (two
   texel reads: the 1600x900 R11G11B10F scene at slot 0, the 320x180 bloom at slot 1; it writes the 1600x900 RGB10A2
   picture that the upscale draw bd7ffdd4 then reads), found by those textures on the Kakariko route. 1.6.0: d14b1c38,
   from the same archive (uking_pass_shader.sharcb), the same 1280 bytes of code and 99.4 % of its instruction words
   equal to 1.9.0's (the nearest of all 1.6.0 fragment shaders; the next is 57 %); f7ee6fe1 is not in 1.6.0 (#178). */
#ifdef GAME_BOTW
#ifdef GAME_BOTW190
#define TAA_COMPOSE(fs) !strncmp(fs->key, "f7ee6fe10b925543e905dfc9437ce67dc21a118d465b03bb3c9eccafdf2e33c4-", 65)
#else
#define TAA_COMPOSE(fs) !strncmp(fs->key, "d14b1c38f70d2ed8355a18183099d088e798b831f919c8be4d1f797a3cdc2689-", 65)
#endif
#else
#define TAA_COMPOSE(fs) !strncmp(fs->key, "0ac9a54938974396", 16)
#endif
typedef struct { float depth; int32_t depth_mask, stencil, stencil_mask; } p_clear_ds;
typedef struct { int32_t first, n; float v[16][6]; } p_viewports;          // x, y, w, h, 近, 远（近远由 DEPTH_RANGE 覆盖） | x, y, w, h, near, far (near and far are overridden by DEPTH_RANGE)
typedef struct { int32_t x, y, w, h; } p_scissor;
typedef struct { int32_t value; } p_flag;   // TOTK: single boolean render state (OP_DEPTH_CLAMP)
typedef struct { int32_t mode; int32_t pad; uint64_t addr; } p_render_cond;   // TOTK: SetRenderEnableConditional: mode 0 = render if the two NVNcounterData.counter values at addr / addr+16 are equal, 1 = if not equal
typedef struct { float n, f; } p_depth_range;
typedef struct { const prog_t *prog; int32_t stages; } p_bind_program;
typedef struct { int32_t kind; uint8_t bytes[0x18]; } p_bind_state;      // kind：0 blend 1 color 2 chmask 3 ms 4 poly 5 ds
typedef struct { int32_t first, n; uint32_t st[16]; } p_bind_vattr;
typedef struct { int32_t first, n; vs_st st[16]; } p_bind_vstream;
typedef struct { int32_t first, n; NVNbufferRange r[16]; } p_bind_vbuf;
typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;  // UBO / SSBO
// src: NVN 55 *Deferred binds keep the game's handle array pointer (read at submit into h, see hle_nvn.c resolve_deferred); 0 = immediate
typedef struct { int32_t stage, first, n, pad_; uint64_t src; uint64_t h[32]; } p_bind_handles; // 分离贴图 / 采样器句柄 | Separate texture / sampler handles
typedef struct { int32_t face, value_mask, mask, ref, which; } p_stencil;  // which：0 值掩码 1 写掩码 2 参考值 | which: 0 value mask 1 write mask 2 reference value
typedef struct { float factor, units, clamp; } p_poly_offset;
typedef struct { int32_t prim, first, count, base_instance, instances; } p_draw;   // TOTK: instances 0 = one instance (DrawArrays)
typedef struct { int32_t prim, itype, count, base_vertex, base_instance, instances; uint64_t ibuf; } p_draw_idx;
typedef struct { int32_t x, y, z; uint64_t caller; } p_dispatch;   // caller：游戏调用点返回地址（定位派发者） | caller: return address of the game call site (to locate the dispatcher)
typedef struct { const dpool_t *pool; } p_set_pool;
typedef struct { uint64_t src; const tex_t *tex; view_t view; uint8_t has_view; NVNcopyRegion r; int32_t flags; } p_copy_b2t;
typedef struct { const tex_t *src, *dst; view_t sv, dv; uint8_t has_sv, has_dv; NVNcopyRegion sr, dr; int32_t flags; } p_copy_t2t;   // T2B 复用：src 存目标缓冲地址 | Reused for T2B: src stores the destination buffer address
typedef struct { const tex_t *tex; view_t view; uint8_t has_view, is_uint; NVNcopyRegion r; int32_t mask; union { float f[4]; uint32_t u[4]; } c; } p_clear_tex;
typedef struct { int64_t stride; } p_copy_stride;
typedef struct { float rgba[4]; } p_blend_color;

// Metal 回放层入口（nvn_mtl.m）：把一组命令句柄回放进当前提交
// Entry point of the Metal replay layer (nvn_mtl.m): replays a set of command handles into the current submission
void mtl_exec(const uint8_t *handle);
void mtl_exec_rest(const uint64_t *handles, int n);   // 告知本次提交里当前句柄之后还有哪些句柄（通道前瞻跨句柄用） | Tells which handles in this submission still follow the current handle (used by the pass lookahead across handles)
// 出图：把交换链贴图画到前端 CAMetalLayer（无窗口时只计数）
// Present: draw the swapchain texture to the front-end CAMetalLayer (only counts when there is no window)
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]);   // 出图入队、立即返回票号；period_s：该张至少显示的时长（= 出图周期，0 = 不限） | Enqueue a present and return the ticket immediately; period_s: minimum time this image is displayed (= present period, 0 = unlimited)
void mtl_present_wait(uint64_t ticket);                    // 等该票号的出图处理完（blit 已提交到 GPU 队列） | Wait until the present with this ticket has been processed (blit committed to the GPU queue)
void mtl_present_flush(void);                              // 等已入队的出图全部处理完 | Wait until all enqueued presents have been processed
void mtl_present_test_stall(uint64_t ns);                  // 单测：出图线程每次出图前额外停顿 ns | Unit test: extra pause in ns before each present on the present thread
int mtl_display_hz(void);                                   // 有窗口时屏幕最高刷新率（Hz），无窗口 0 | Maximum refresh rate of the screen (Hz) when there is a window, 0 without a window
void mtl_set_tex_pool_n(int n);                            // 贴图描述符池容量：池外的贴图号按「空槽」处理（读出 0） | Texture descriptor pool capacity: texture IDs outside the pool are treated as 'empty slots' (read 0)
// 计时埋点（Mac / iOS 共用）：提交按帧号打标；GPU 时间 = 该帧全部命令缓冲 GPUEnd−GPUStart 之和，全部完成后才可读（否则 −1）
// Timing instrumentation (shared by Mac / iOS): submissions are tagged with the frame number; GPU time = sum of GPUEnd−GPUStart over all of that frame's command buffers, readable only after all complete (otherwise −1)
void mtl_set_frame(uint64_t f);
void mtl_ub_present(double iv_ms);   // 同进程交替测量（DM_NVN_UB）：出图线程每帧报实际上屏间隔 | Same-process alternating measurement (DM_NVN_UB): the present thread reports the actual present interval every frame
double mtl_gpu_ms(uint64_t f);
void mtl_sub_index(const uint64_t *h, int n);   // 提交开始：按回放顺序建写回前瞻索引（nvn_mtl.m） | Start of submission: build the store lookahead index in replay order (nvn_mtl.m)
void xv_test_counts(int *dc, int *viol);
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1);   // 该帧最早 GPUStart / 最晚 GPUEnd（mach 绝对时间 ns）；未全部完成或无 GPU 时间返回 0 | The frame's earliest GPUStart / latest GPUEnd (mach absolute time, ns); returns 0 if not all complete or there is no GPU time
void mtl_stats(uint64_t *passes, uint64_t *draws);
void mtl_args_stats(uint64_t *enc, uint64_t *reuse);
void mtl_use_stats(uint64_t *call, uint64_t *skip);
uint64_t mtl_ds_omit(void);   // 前瞻证明不用深度模板、未挂深度附件的通道累计数 | Cumulative number of passes with no depth attachment because the lookahead proved depth-stencil unused
uint64_t mtl_drawable_ns_take(void);   // 取走并清零：出图里等 nextDrawable 的累计 ns   // useResource 实发 / 同编码器去重跳过累计次数   // 参数缓冲新编 / 复用累计次数 | Take and zero: cumulative ns spent waiting for nextDrawable in present   // Cumulative useResource calls actually issued / skipped by same-encoder dedup   // Cumulative argument buffer new-encode / reuse counts
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes);   // 附件读入 / 写出累计字节、全量读写通道累计数 | Cumulative attachment load / store bytes, cumulative number of full load-store passes
uint64_t mtl_udc_saved_bytes(void);                                      // DM_NVN_UNWRITTEN_DC 实际省下的写回字节 | Write-back bytes actually saved by DM_NVN_UNWRITTEN_DC
int mtl_has_window(void);
int mtl_warm_wait(int timeout_ms);
int mtl_link_active(void);
/* frame_ticks: 60 Hz ticks one game frame occupies (the game's present interval; 1 in display mode). */
int mtl_link_wait_tick(uint64_t timeout_ns, unsigned frame_ticks);
double mtl_presented_time(uint64_t n);
   // 第 n 次出图（从 1 计）的实际呈现时刻（秒）；0 = 未就绪，−1 = 被丢弃
   // Actual presentation time (seconds) of the n-th present (counted from 1); 0 = not ready, −1 = dropped
// 贴图释放（Finalize 时）
// Texture release (at Finalize)
void mtl_tex_release(tex_t *t);
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads);
uint64_t mtl_pkg_miss(void);
uint64_t mtl_upload_ns_take(void);   // 取走并清零：上次以来提交线程上预排布贴图上传耗时   // DM_NVN_DIRTYCHECK 诊断：漏检次数   // 预排布贴图：首传次数、因池内容变化而重传次数 | Take and zero: time spent uploading packaged textures on the submit thread since last time   // DM_NVN_DIRTYCHECK diagnostic: number of missed detections   // Packaged textures: number of first uploads, number of re-uploads due to pool content changes
uint64_t mtl_upload_gpu_ns_take(void);   // 取走并清零：上传命令缓冲的显卡执行耗时（GPUEndTime − GPUStartTime 累加） | Take and zero: GPU execution time of upload command buffers (sum of GPUEndTime − GPUStartTime)
void mtl_create_stats(uint64_t *lib, uint64_t *pso);   // 累计：提交线程上同步载入的着色器库数、同步新建的渲染 + 计算管线数（预热领用的不算） | Cumulative: number of shader libraries loaded synchronously on the submit thread, number of render + compute pipelines created synchronously (those taken from warm-up do not count)
// CPU 读贴图：level / layer0 起，按 row / img 字节步长写进 dst
// CPU texture read: starting at level / layer0, written into dst with row / img byte strides
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);
void mtl_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img);   // TOTK: CPU texel write (WriteTexels)
// 描述符池登记（贴图号 / 采样器号 → 对象快照）、程序释放
// Descriptor pool registration (texture ID / sampler ID → object snapshot), program release
void mtl_reg_tex(int id, const tex_t *t, const view_t *v);
void mtl_reg_samp(int id, const samp_par *p);
void mtl_prog_release(prog_t *p);
void mtl_prog_prefetch(const prog_t *p);   // TOTK_SHADER_PREFETCH=1: start compiling the program's known shader variants in the background
// 新序号（> 0，全局递增）
// New serial (> 0, globally increasing)
uint64_t nvn_new_serial(void);
