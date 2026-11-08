/**
 *   ctx = mtlb_init();
 *   rt  = mtlb_target(ctx, w, h, MTLB_FMT_BGRA8);
 *
 * mtl_backend.h - minimal Metal offscreen backend (pure C interface, the base of the NVN→Metal backend)
 * Call model:
 *   ctx = mtlb_init();
 *   rt  = mtlb_target(ctx, w, h, MTLB_FMT_BGRA8);
 *   mtlb_clear(ctx, rt, ...);      // record a pass into the "current frame" command buffer
 *   mtlb_triangle(ctx, rt);        // record another pass, same command buffer
 *   mtlb_readback(ctx, rt, buf, n);// commit the current frame, wait for the GPU to finish, read back pixels
 * One frame = one MTLCommandBuffer, created when the first pass is recorded and committed at readback.
 * All functions return 0 on success / -1 on failure (the failure reason is printed to stderr). Not thread-safe.
 */
#ifndef MTL_BACKEND_H
#define MTL_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mtlb_ctx mtlb_ctx; /* device + command queue + current-frame command buffer + pipeline cache */
typedef struct mtlb_rt  mtlb_rt;  /* an offscreen render target */

typedef enum {
    MTLB_FMT_BGRA8 = 0, /* byte order in memory B,G,R,A (unorm) */
    MTLB_FMT_RGBA8 = 1, /* byte order in memory R,G,B,A (unorm) */
} mtlb_fmt;

mtlb_ctx *mtlb_init(void);
void      mtlb_shutdown(mtlb_ctx *ctx);

mtlb_rt  *mtlb_target(mtlb_ctx *ctx, uint32_t w, uint32_t h, mtlb_fmt fmt);
void      mtlb_target_free(mtlb_rt *rt);

int mtlb_clear(mtlb_ctx *ctx, mtlb_rt *rt, float r, float g, float b, float a);
int mtlb_triangle(mtlb_ctx *ctx, mtlb_rt *rt);

/* buf is at least w*h*4 bytes; rows tightly packed, row 0 is the top of the image, byte order same as rt's format. */
int mtlb_readback(mtlb_ctx *ctx, mtlb_rt *rt, void *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
#endif
