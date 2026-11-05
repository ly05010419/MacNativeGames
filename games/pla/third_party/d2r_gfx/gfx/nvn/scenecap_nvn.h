/* scenecap NVN -> Metal glue: the hooks the replay layer calls.
 *
 * The replay layer (games/<game>/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m) includes this header and calls the
 * hooks below at the four points where it decides what to draw.  The implementation is
 * scenecap_nvn.inc, textually included at the end of nvn_mtl.m because it reads the file's static
 * replay state (S, R, g_pass, g_cur_frame, the texture registry, the shader objects).
 *
 * Every hook is a no-op unless SCENECAP=1 is in the environment, so a build that carries the hooks
 * behaves exactly like one that does not (no extra allocation, no extra Metal work).
 *
 * Threading: call all hooks from the replay / decision thread (the one that runs rp_begin_ex,
 * draw_inner and dispatch).  The render-stream encoder thread must not call them.
 *
 * The pointer arguments are typed void * here so this header stays free of NVN and Metal types;
 * the call sites pass the objects they already have (MTL4RenderPassDescriptor, p_draw, p_draw_idx,
 * p_dispatch).
 */
#ifndef SCENECAP_NVN_H
#define SCENECAP_NVN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when the environment asked for a capture (SCENECAP=1).  Cheap: the environment is read once. */
int scenecap_nvn_on(void);

/* Frame boundary: call once per game frame from mtl_set_frame(), before the frame's commands replay.
 * Opens the capture window and writes the package when the window ends. */
void scenecap_nvn_frame(uint64_t frame);

/* Render pass: call from rp_begin_ex() once the descriptor and g_pass are final, and from rp_end()
 * before the encoder is ended.  descriptor is an MTL4RenderPassDescriptor. */
void scenecap_nvn_pass_begin(void *descriptor, uint32_t width, uint32_t height);
void scenecap_nvn_pass_end(void);

/* Draw: call from draw_inner() once the program is resolved and before the draw is encoded.
 * draw / indexed_draw are p_draw / p_draw_idx (either may be NULL). */
void scenecap_nvn_draw(const void *draw, const void *indexed_draw);

/* Optional per-draw override for GS-compacted consumers (NieR body/cloth).  When active, the
 * recorder writes the *resolved* index buffer + count (host survivor list) and an optional
 * capture-position buffer at Metal vertex slot 28 (POSCAP), instead of the guest placeholder
 * {count=1, no usable IB}.  Call immediately before scenecap_nvn_draw(); cleared after that call.
 * Without this, body colour draws land in the package as 1-index no-ops and cannot be iterated
 * offline. */
typedef struct scenecap_draw_resolve {
    int active;
    uint64_t index_buffer;   /* GPU address of the index buffer (host list) */
    uint64_t index_bytes;    /* bytes available from that address */
    uint32_t index_count;    /* survivor index count (must be > 0) */
    uint32_t index_type;     /* 0=u8, 1=u16, 2=u32 — same as NVN / p_draw_idx.itype */
    const void *index_data;  /* CPU bytes (list_cpu); preferred over GPU readback */
    uint64_t poscap;         /* if non-zero, bind as vertex buffer index 28 */
    uint64_t poscap_bytes;
    const void *poscap_data; /* optional CPU snapshot of the capture buffer */
} scenecap_draw_resolve;

void scenecap_nvn_set_draw_resolve(const scenecap_draw_resolve *resolve); /* NULL clears */

/* Compute dispatch: call from dispatch() before the compute encoder is built.
 * dispatch is a p_dispatch. */
void scenecap_nvn_dispatch(const void *dispatch);

/* Starts a capture at the next frame with the given package path ("scenecap <path>" from the
 * diagnostic input file).  path may be NULL to use the environment's path. */
void scenecap_nvn_request(const char *path);

/* Releases the capture (the replay layer calls it on shutdown when it has a teardown hook). */
void scenecap_nvn_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SCENECAP_NVN_H */
