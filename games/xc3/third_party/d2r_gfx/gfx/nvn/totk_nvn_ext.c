#include <mach/mach_time.h>
// TOTK: NVN entry points beyond the donor implementation that only report stored object state,
// attach debugging metadata, or convert timestamps. None of them changes rendering results.
#include "../../host/hle.h"
#include "nvn_impl.h"
#include "nvn_obj.h"
#include "nvn_tex.h"
#include <string.h>
#include <time.h>

// ---------- Debug labels, markers, groups, checkpoints: meaningful only to NVN debugging tools ----------
NVN(nvnDeviceSetDebugLabel, void, NVNdevice *o, const char *s) { (void)o; (void)s; }
NVN(nvnQueueSetDebugLabel, void, NVNqueue *o, const char *s) { (void)o; (void)s; }
NVN(nvnWindowSetDebugLabel, void, NVNwindow *o, const char *s) { (void)o; (void)s; }
NVN(nvnProgramSetDebugLabel, void, NVNprogram *o, const char *s) { (void)o; (void)s; }
NVN(nvnMemoryPoolSetDebugLabel, void, NVNmemoryPool *o, const char *s) { (void)o; (void)s; }
NVN(nvnTexturePoolSetDebugLabel, void, NVNtexturePool *o, const char *s) { (void)o; (void)s; }
NVN(nvnSamplerPoolSetDebugLabel, void, NVNsamplerPool *o, const char *s) { (void)o; (void)s; }
NVN(nvnBufferSetDebugLabel, void, NVNbuffer *o, const char *s) { (void)o; (void)s; }
NVN(nvnTextureSetDebugLabel, void, NVNtexture *o, const char *s) { (void)o; (void)s; }
NVN(nvnSamplerSetDebugLabel, void, NVNsampler *o, const char *s) { (void)o; (void)s; }
NVN(nvnCommandBufferSetDebugLabel, void, NVNcommandBuffer *o, const char *s) { (void)o; (void)s; }
NVN(nvnSyncSetDebugLabel, void, NVNsync *o, const char *s) { (void)o; (void)s; }
NVN(nvnCommandBufferPushDebugGroupStatic, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN(nvnCommandBufferPushDebugGroupDynamic, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN(nvnCommandBufferPushDebugGroup, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN(nvnCommandBufferPopDebugGroup, void, NVNcommandBuffer *c) { (void)c; }
NVN(nvnCommandBufferPopDebugGroupId, void, NVNcommandBuffer *c, uint32_t id) { (void)c; (void)id; }
NVN(nvnCommandBufferInsertDebugMarkerStatic, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN(nvnCommandBufferInsertDebugMarkerDynamic, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN(nvnCommandBufferInsertDebugMarker, void, NVNcommandBuffer *c, const char *s) { (void)c; (void)s; }
NVN_NOPROTO(nvnCommandBufferInsertCheckpointStatic, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN_NOPROTO(nvnCommandBufferInsertCheckpointDynamic, void, NVNcommandBuffer *c, uint32_t id, const char *s) { (void)c; (void)id; (void)s; }
NVN(nvnDeviceIsExternalDebuggerAttached, NVNboolean, const NVNdevice *d) { (void)d; return 0; }

// ---------- Timestamps: counters are written in Tegra 19.2 MHz ticks (gpu_ticks in nvn_mtl.m) ----------
NVN(nvnDeviceGetCurrentTimestampInNanoseconds, uint64_t, const NVNdevice *d) {
    (void)d; mach_timebase_info_data_t tb; mach_timebase_info(&tb); return (uint64_t)((__uint128_t)mach_absolute_time() * tb.numer / tb.denom);   // TOTK: GetSystemTick's base (mach_absolute_time, no sleep), see gpu_ticks
}
NVN(nvnDeviceGetTimestampInNanoseconds, uint64_t, const NVNdevice *d, const NVNcounterData *c) {
    (void)d; return c->timestamp / 12 * 625 + c->timestamp % 12 * 625 / 12;
}

// ---------- Memory pools and buffers: unified memory, CPU and GPU views are coherent ----------
NVN(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *p) { return (size_t)((const pool_t *)p)->size; }
// The CPU is about to read what the GPU wrote: GPU renderings into this pool's textures are written back (nvn_mtl.m).
void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n);
NVN(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) { if (off >= 0) mtl_pool_invalidate((const pool_t *)p, (uint64_t)off, n); }
NVN(nvnBufferInvalidateMappedRange, void, const NVNbuffer *b, ptrdiff_t off, size_t n) { (void)b; (void)off; (void)n; }

// ---------- Texture builder and texture queries ----------
#define CTB(b) ((const tex_builder_t *)(b))
#define CTX(t) ((const tex_t *)(t))
static void get_swizzle(const nvn_texdesc *d, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    *r = (NVNtextureSwizzle)d->swizzle_r; *g = (NVNtextureSwizzle)d->swizzle_g; *b = (NVNtextureSwizzle)d->swizzle_b; *a = (NVNtextureSwizzle)d->swizzle_a;
}
NVN_NOPROTO(nvnTextureBuilderGetDevice, NVNdevice *, const NVNtextureBuilder *b) { return CTB(b)->dev; }
NVN(nvnTextureBuilderGetTarget, NVNtextureTarget, const NVNtextureBuilder *b) { return (NVNtextureTarget)CTB(b)->d.target; }
NVN(nvnTextureBuilderGetWidth, int, const NVNtextureBuilder *b) { return (int)CTB(b)->d.width; }
NVN(nvnTextureBuilderGetHeight, int, const NVNtextureBuilder *b) { return (int)CTB(b)->d.height; }
NVN(nvnTextureBuilderGetDepth, int, const NVNtextureBuilder *b) { return (int)CTB(b)->d.depth; }
NVN(nvnTextureBuilderGetLevels, int, const NVNtextureBuilder *b) { return (int)CTB(b)->d.levels; }
NVN(nvnTextureBuilderGetFormat, NVNformat, const NVNtextureBuilder *b) { return (NVNformat)CTB(b)->d.format; }
NVN(nvnTextureBuilderGetSamples, int, const NVNtextureBuilder *b) { return CTB(b)->d.samples; }
NVN(nvnTextureBuilderGetSwizzle, void, const NVNtextureBuilder *b, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *bl, NVNtextureSwizzle *a) { get_swizzle(&CTB(b)->d, r, g, bl, a); }
NVN(nvnTextureBuilderGetDepthStencilMode, NVNtextureDepthStencilMode, const NVNtextureBuilder *b) { return CTB(b)->d.stencil_mode ? NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH; }
NVN(nvnTextureBuilderGetStride, ptrdiff_t, const NVNtextureBuilder *b) { return (ptrdiff_t)CTB(b)->d.stride; }
NVN_NOPROTO(nvnTextureBuilderGetMemoryPool, const NVNmemoryPool *, const NVNtextureBuilder *b) { return (const NVNmemoryPool *)(const void *)CTB(b)->pool; }
NVN(nvnTextureBuilderGetMemoryOffset, ptrdiff_t, const NVNtextureBuilder *b) { return (ptrdiff_t)CTB(b)->off; }
NVN(nvnTextureGetTarget, NVNtextureTarget, const NVNtexture *t) { return (NVNtextureTarget)CTX(t)->d.target; }
NVN(nvnTextureGetLevels, int, const NVNtexture *t) { return (int)CTX(t)->d.levels; }
NVN(nvnTextureGetFormat, NVNformat, const NVNtexture *t) { return (NVNformat)CTX(t)->d.format; }
NVN(nvnTextureGetSamples, int, const NVNtexture *t) { return CTX(t)->d.samples; }
NVN(nvnTextureGetSwizzle, void, const NVNtexture *t, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) { get_swizzle(&CTX(t)->d, r, g, b, a); }
NVN(nvnTextureGetDepthStencilMode, NVNtextureDepthStencilMode, const NVNtexture *t) { return CTX(t)->d.stencil_mode ? NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH; }
NVN(nvnTextureGetStride, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->d.stride; }
NVN(nvnTextureGetTextureAddress, NVNtextureAddress, const NVNtexture *t) { return (NVNtextureAddress)(uintptr_t)CTX(t)->mem; }
NVN_NOPROTO(nvnTextureGetMemoryPool, const NVNmemoryPool *, const NVNtexture *t) { return (const NVNmemoryPool *)(const void *)CTX(t)->pool; }
NVN(nvnTextureGetMemoryOffset, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->off; }
NVN(nvnTextureGetDebugID, uint64_t, const NVNtexture *t) { return CTX(t)->serial; }

// ---------- Window queries ----------
#define CWIN(w) ((const win_t *)(w))
#define CWB(b) ((const win_builder_t *)(b))
NVN_NOPROTO(nvnWindowBuilderGetDevice, NVNdevice *, const NVNwindowBuilder *b) { return CWB(b)->dev; }
NVN(nvnWindowBuilderGetNativeWindow, NVNnativeWindow, const NVNwindowBuilder *b) { return (NVNnativeWindow)CWB(b)->native; }
NVN(nvnWindowBuilderGetPresentInterval, int, const NVNwindowBuilder *b) { return CWB(b)->interval; }
NVN_NOPROTO(nvnWindowBuilderGetNumTextures, int, const NVNwindowBuilder *b) { return CWB(b)->ntex; }
NVN_NOPROTO(nvnWindowBuilderGetTexture, NVNtexture *, const NVNwindowBuilder *b, int i) { return i >= 0 && i < CWB(b)->ntex ? CWB(b)->tex[i] : NULL; }
NVN(nvnWindowGetNativeWindow, NVNnativeWindow, const NVNwindow *w) { return (NVNnativeWindow)CWIN(w)->native; }
NVN(nvnWindowGetPresentInterval, int, const NVNwindow *w) { return CWIN(w)->interval; }
NVN_NOPROTO(nvnWindowGetNumTextures, int, const NVNwindow *w) { return CWIN(w)->ntex; }
NVN(nvnWindowGetCrop, void, const NVNwindow *w, NVNrectangle *r) {
    const win_t *o = CWIN(w);
    if (!o->crop[2] && !o->crop[3] && o->ntex) { *r = (NVNrectangle){0, 0, (int)CTX(o->tex[0])->d.width, (int)CTX(o->tex[0])->d.height}; return; }   // default crop: whole image
    r->x = o->crop[0]; r->y = o->crop[1]; r->width = o->crop[2]; r->height = o->crop[3];
}

// ---------- Texture views ----------
#define VW(v) ((view_t *)(v))
#define CVW(v) ((const view_t *)(v))
NVN(nvnTextureViewSetSwizzle, void, NVNtextureView *v, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle b, NVNtextureSwizzle a) {
    if ((unsigned)r > 5 || (unsigned)g > 5 || (unsigned)b > 5 || (unsigned)a > 5) die("invalid NVN texture view swizzle");
    VW(v)->set_swizzle = 1; VW(v)->swz[0] = (uint8_t)r; VW(v)->swz[1] = (uint8_t)g; VW(v)->swz[2] = (uint8_t)b; VW(v)->swz[3] = (uint8_t)a;
}
// A view may sample the stencil plane of a depth-stencil texture (Bayonetta 3); nvn_mtl.m makes it an X32_Stencil8 view.
NVN(nvnTextureViewSetDepthStencilMode, void, NVNtextureView *v, NVNtextureDepthStencilMode m) {
    if (m != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH && m != NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL) die("invalid NVN depth/stencil mode %d", (int)m);
    VW(v)->set_stencil = m == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL;
}
NVN(nvnTextureViewGetLevels, NVNboolean, const NVNtextureView *v, int *base, int *n) { *base = CVW(v)->base_level; *n = CVW(v)->nlevels; return CVW(v)->set_levels; }
NVN(nvnTextureViewGetFormat, NVNboolean, const NVNtextureView *v, NVNformat *f) { *f = (NVNformat)CVW(v)->format; return CVW(v)->set_format; }
NVN(nvnTextureViewGetTarget, NVNboolean, const NVNtextureView *v, NVNtextureTarget *t) { *t = (NVNtextureTarget)CVW(v)->target; return CVW(v)->set_target; }
NVN(nvnTextureViewGetSwizzle, NVNboolean, const NVNtextureView *v, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    const view_t *o = CVW(v);
    *r = (NVNtextureSwizzle)o->swz[0]; *g = (NVNtextureSwizzle)o->swz[1]; *b = (NVNtextureSwizzle)o->swz[2]; *a = (NVNtextureSwizzle)o->swz[3];
    return o->set_swizzle;
}
NVN(nvnTextureViewGetDepthStencilMode, NVNboolean, const NVNtextureView *v, NVNtextureDepthStencilMode *m) {
    *m = CVW(v)->set_stencil ? NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH; return CVW(v)->set_stencil;
}
NVN(nvnTextureViewCompare, NVNboolean, const NVNtextureView *a, const NVNtextureView *b) {
    const view_t *x = CVW(a), *y = CVW(b);
    if (x->set_levels != y->set_levels || x->set_layers != y->set_layers || x->set_format != y->set_format || x->set_target != y->set_target || x->set_swizzle != y->set_swizzle || x->set_stencil != y->set_stencil) return 0;
    if (x->set_levels && (x->base_level != y->base_level || x->nlevels != y->nlevels)) return 0;
    if (x->set_layers && (x->min_layer != y->min_layer || x->nlayers != y->nlayers)) return 0;
    if (x->set_format && x->format != y->format) return 0;
    if (x->set_target && x->target != y->target) return 0;
    return !x->set_swizzle || !memcmp(x->swz, y->swz, 4);
}


// ---------- Sampler queries ----------
#define CSB(b) (&((const samp_builder_t *)(b))->p)
#define CSP(s) (&((const samp_t *)(s))->p)
#define MSB(b) (&((samp_builder_t *)(b))->p)
static void border_from_int(samp_par *p, const int32_t *c, int is_signed) { for (int i = 0; i < 4; i++) p->border[i] = is_signed ? (float)c[i] : (float)(uint32_t)c[i]; }
NVN(nvnSamplerBuilderSetBorderColori, void, NVNsamplerBuilder *b, const int *c) { border_from_int(MSB(b), c, 1); }
NVN(nvnSamplerBuilderSetBorderColorui, void, NVNsamplerBuilder *b, const uint32_t *c) { border_from_int(MSB(b), (const int32_t *)c, 0); }
NVN_NOPROTO(nvnSamplerBuilderGetDevice, NVNdevice *, const NVNsamplerBuilder *b) { return ((const samp_builder_t *)b)->dev; }
static void lod_clamp(const samp_par *p, float *lo, float *hi) { *lo = p->lod_min; *hi = p->lod_max; }
static void compare(const samp_par *p, NVNcompareMode *m, NVNcompareFunc *f) { *m = (NVNcompareMode)p->cmp_mode; *f = (NVNcompareFunc)p->cmp_func; }
static void border_i(const samp_par *p, int *c) { for (int i = 0; i < 4; i++) c[i] = (int)p->border[i]; }
static void border_ui(const samp_par *p, uint32_t *c) { for (int i = 0; i < 4; i++) c[i] = (uint32_t)p->border[i]; }
NVN(nvnSamplerBuilderGetLodClamp, void, const NVNsamplerBuilder *b, float *lo, float *hi) { lod_clamp(CSB(b), lo, hi); }
NVN(nvnSamplerBuilderGetLodBias, float, const NVNsamplerBuilder *b) { return CSB(b)->lod_bias; }
NVN(nvnSamplerBuilderGetCompare, void, const NVNsamplerBuilder *b, NVNcompareMode *m, NVNcompareFunc *f) { compare(CSB(b), m, f); }
NVN(nvnSamplerBuilderGetBorderColor, void, const NVNsamplerBuilder *b, float *c) { memcpy(c, CSB(b)->border, 16); }
NVN(nvnSamplerBuilderGetBorderColori, void, const NVNsamplerBuilder *b, int *c) { border_i(CSB(b), c); }
NVN(nvnSamplerBuilderGetBorderColorui, void, const NVNsamplerBuilder *b, uint32_t *c) { border_ui(CSB(b), c); }
NVN(nvnSamplerBuilderGetMaxAnisotropy, float, const NVNsamplerBuilder *b) { return CSB(b)->aniso; }
NVN(nvnSamplerBuilderGetLodSnap, float, const NVNsamplerBuilder *b) { return CSB(b)->lod_snap; }
NVN(nvnSamplerGetMinMagFilter, void, const NVNsampler *s, NVNminFilter *mi, NVNmagFilter *ma) { *mi = (NVNminFilter)CSP(s)->minf; *ma = (NVNmagFilter)CSP(s)->magf; }
NVN(nvnSamplerGetWrapMode, void, const NVNsampler *s, NVNwrapMode *a, NVNwrapMode *b, NVNwrapMode *c) { *a = (NVNwrapMode)CSP(s)->ws; *b = (NVNwrapMode)CSP(s)->wt; *c = (NVNwrapMode)CSP(s)->wr; }
NVN(nvnSamplerGetLodClamp, void, const NVNsampler *s, float *lo, float *hi) { lod_clamp(CSP(s), lo, hi); }
NVN(nvnSamplerGetLodBias, float, const NVNsampler *s) { return CSP(s)->lod_bias; }
NVN(nvnSamplerGetCompare, void, const NVNsampler *s, NVNcompareMode *m, NVNcompareFunc *f) { compare(CSP(s), m, f); }
NVN(nvnSamplerGetBorderColor, void, const NVNsampler *s, float *c) { memcpy(c, CSP(s)->border, 16); }
NVN(nvnSamplerGetBorderColori, void, const NVNsampler *s, int *c) { border_i(CSP(s), c); }
NVN(nvnSamplerGetBorderColorui, void, const NVNsampler *s, uint32_t *c) { border_ui(CSP(s), c); }
NVN(nvnSamplerGetMaxAnisotropy, float, const NVNsampler *s) { return CSP(s)->aniso; }
NVN(nvnSamplerCompare, NVNboolean, const NVNsampler *a, const NVNsampler *b) { return !memcmp(CSP(a), CSP(b), sizeof(samp_par)); }
NVN(nvnSamplerGetDebugID, uint64_t, const NVNsampler *s) { return (uint64_t)(uintptr_t)s; }

// ---------- Fixed-function state objects: setters without a Metal equivalent store the value, and the
// replay reports any non-default use (nvn_mtl.m draw); getters return what was set. ----------
#define CS(T, s) ((const T *)(const void *)(s))
#define MS(T, s) ((T *)(void *)(s))
NVN(nvnColorStateSetLogicOp, void, NVNcolorState *s, NVNlogicOp op) { if ((unsigned)op > 15) die("logic op %d", (int)op); MS(color_st, s)->logic_op = op; }
NVN(nvnColorStateSetAlphaTest, void, NVNcolorState *s, NVNalphaFunc f) { if ((unsigned)f > 15) die("alpha func %d", (int)f); MS(color_st, s)->alpha_func = f; }
NVN(nvnColorStateGetBlendEnable, NVNboolean, const NVNcolorState *s, int i) { return i >= 0 && i < 8 && (CS(color_st, s)->blend_en >> i & 1); }
NVN(nvnColorStateGetLogicOp, NVNlogicOp, const NVNcolorState *s) { return (NVNlogicOp)CS(color_st, s)->logic_op; }
NVN(nvnColorStateGetAlphaTest, NVNalphaFunc, const NVNcolorState *s) { return (NVNalphaFunc)CS(color_st, s)->alpha_func; }
NVN(nvnChannelMaskStateGetChannelMask, void, const NVNchannelMaskState *s, int i, NVNboolean *r, NVNboolean *g, NVNboolean *b, NVNboolean *a) {
    uint32_t m = i >= 0 && i < 8 ? CS(chmask_st, s)->mask >> (4 * i) : 0; *r = m & 1; *g = m >> 1 & 1; *b = m >> 2 & 1; *a = m >> 3 & 1;
}
NVN(nvnBlendStateGetBlendTarget, int, const NVNblendState *s) { return (int)CS(blend_st, s)->target; }
NVN(nvnBlendStateGetBlendFunc, void, const NVNblendState *s, NVNblendFunc *sr, NVNblendFunc *dr, NVNblendFunc *sa, NVNblendFunc *da) {
    const blend_st *b = CS(blend_st, s); *sr = (NVNblendFunc)b->src_rgb; *dr = (NVNblendFunc)b->dst_rgb; *sa = (NVNblendFunc)b->src_a; *da = (NVNblendFunc)b->dst_a;
}
NVN(nvnBlendStateGetBlendEquation, void, const NVNblendState *s, NVNblendEquation *rgb, NVNblendEquation *a) { *rgb = (NVNblendEquation)CS(blend_st, s)->eq_rgb; *a = (NVNblendEquation)CS(blend_st, s)->eq_a; }
NVN(nvnBlendStateSetAdvancedMode, void, NVNblendState *s, NVNblendAdvancedMode m) { (void)s; if (m) die("advanced blend mode %d not implemented", (int)m); }
NVN(nvnBlendStateSetAdvancedOverlap, void, NVNblendState *s, NVNblendAdvancedOverlap o) { (void)s; (void)o; }   /* only meaningful with an advanced mode */
NVN(nvnBlendStateSetAdvancedPremultipliedSrc, void, NVNblendState *s, uint32_t v) { (void)s; (void)v; }
NVN(nvnBlendStateSetAdvancedNormalizedDst, void, NVNblendState *s, uint32_t v) { (void)s; (void)v; }
NVN(nvnMultisampleStateGetMultisampleEnable, NVNboolean, const NVNmultisampleState *s) { return CS(ms_st, s)->enable; }
NVN(nvnMultisampleStateGetSamples, int, const NVNmultisampleState *s) { return CS(ms_st, s)->samples; }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable, NVNboolean, const NVNmultisampleState *s) { return CS(ms_st, s)->a2c; }
NVN(nvnMultisampleStateGetRasterSamples, int, NVNmultisampleState *s) { return CS(ms_st, s)->raster_samples; }
NVN(nvnMultisampleStateSetRasterSamples, void, NVNmultisampleState *s, int n) { if (n < 0 || n > 16) die("raster samples %d", n); MS(ms_st, s)->raster_samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetAlphaToCoverageDither, void, NVNmultisampleState *s, uint32_t e) { MS(ms_st, s)->a2c_dither = (e & 0xff) != 0; }
NVN(nvnPolygonStateGetCullFace, NVNface, const NVNpolygonState *s) { return (NVNface)CS(poly_st, s)->cull; }
NVN(nvnPolygonStateGetFrontFace, NVNfrontFace, const NVNpolygonState *s) { return (NVNfrontFace)CS(poly_st, s)->front; }
NVN(nvnPolygonStateGetPolygonMode, NVNpolygonMode, const NVNpolygonState *s) { return (NVNpolygonMode)CS(poly_st, s)->mode; }
NVN(nvnPolygonStateGetPolygonOffsetEnables, NVNpolygonOffsetEnable, const NVNpolygonState *s) { return (NVNpolygonOffsetEnable)CS(poly_st, s)->offset_en; }
NVN(nvnDepthStencilStateGetDepthTestEnable, NVNboolean, const NVNdepthStencilState *s) { return CS(ds_st, s)->dt; }
NVN(nvnDepthStencilStateGetDepthWriteEnable, NVNboolean, const NVNdepthStencilState *s) { return CS(ds_st, s)->dw; }
NVN(nvnDepthStencilStateGetDepthFunc, NVNdepthFunc, const NVNdepthStencilState *s) { return (NVNdepthFunc)CS(ds_st, s)->df; }
NVN(nvnDepthStencilStateGetStencilTestEnable, NVNboolean, const NVNdepthStencilState *s) { return CS(ds_st, s)->st; }
NVN(nvnDepthStencilStateGetStencilFunc, NVNstencilFunc, const NVNdepthStencilState *s, NVNface f) { return (NVNstencilFunc)(f == NVN_FACE_BACK ? CS(ds_st, s)->sf_b : CS(ds_st, s)->sf_f); }
NVN(nvnDepthStencilStateGetStencilOp, void, const NVNdepthStencilState *s, NVNface f, NVNstencilOp *fail, NVNstencilOp *zf, NVNstencilOp *zp) {
    const ds_st *d = CS(ds_st, s); int back = f == NVN_FACE_BACK;
    *fail = (NVNstencilOp)(back ? d->fail_b : d->fail_f); *zf = (NVNstencilOp)(back ? d->zfail_b : d->zfail_f); *zp = (NVNstencilOp)(back ? d->zpass_b : d->zpass_f);
}
NVN(nvnVertexAttribStateGetFormat, void, const NVNvertexAttribState *s, NVNformat *f, ptrdiff_t *off) { *f = (NVNformat)CS(va_st, s)->fmt; *off = (ptrdiff_t)CS(va_st, s)->offset; }
NVN(nvnVertexAttribStateGetStreamIndex, int, const NVNvertexAttribState *s) { return (int)CS(va_st, s)->stream; }
NVN(nvnVertexStreamStateGetStride, ptrdiff_t, const NVNvertexStreamState *s) { return (ptrdiff_t)CS(vs_st, s)->stride; }
NVN(nvnVertexStreamStateGetDivisor, int, const NVNvertexStreamState *s) { return (int)CS(vs_st, s)->divisor; }

// ---------- Rasterisation state without a Metal equivalent: lines are 1 pixel wide and point size comes from
// the vertex shader on Metal. Non-default values are reported once (the drawing proceeds without them). ----------
NVN(nvnCommandBufferSetLineWidth, void, NVNcommandBuffer *c, float w) {
    (void)c; if (w != 1.0f) { static int once; if (!once++) LOG("【红】SetLineWidth %g: Metal draws 1-pixel lines", w); }
}
NVN(nvnCommandBufferSetPointSize, void, NVNcommandBuffer *c, float sz) {
    (void)c; if (sz != 1.0f) { static int once; if (!once++) LOG("【红】SetPointSize %g: Metal takes point size from the vertex shader", sz); }
}

// ---------- Entry points outside main's name table (NVN 55 inline fast-path fallbacks and newer queries) ----------
// The *Deferred binds are what the inline fast paths call when the command write pointer has no room
// (always, see cmd_t.inline_cur). Texture / image / separate handle binds read the game's handle array at submit,
// as the original driver does (hle_nvn.c nvn_bind_handles_deferred); buffer ranges still bind at record time.
#define CALL_SLOT(name) ((NVNT_##name)nvn_slot_fn(#name))
void *nvn_slot_fn(const char *name);
void nvn_bind_handles_deferred(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h, size_t hw_bytes);
NVN_EXTRA(nvnCommandBufferBindTexturesDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) { nvn_bind_handles_deferred(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h, 0); }
NVN_EXTRA(nvnCommandBufferBindUniformBuffersDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *r) { static int once; if (!once++) LOG("[nvn] BindUniformBuffersDeferred 首次调用（缓冲范围仍按录制时绑定）"); CALL_SLOT(nvnCommandBufferBindUniformBuffers)(c, st, first, n, r); }
NVN_EXTRA(nvnCommandBufferBindStorageBuffersDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *r) { static int once; if (!once++) LOG("[nvn] BindStorageBuffersDeferred 首次调用（缓冲范围仍按录制时绑定）"); CALL_SLOT(nvnCommandBufferBindStorageBuffers)(c, st, first, n, r); }
NVN_EXTRA(nvnCommandBufferBindImagesDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNimageHandle *h) { nvn_bind_handles_deferred(c, OP_BIND_IMG, st, first, n, (const uint64_t *)h, 16 + 8 * (size_t)n); }
NVN_EXTRA(nvnCommandBufferBindSeparateTexturesDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateTextureHandle *h) { nvn_bind_handles_deferred(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h, 16 + 8 * (size_t)n); }
NVN_EXTRA(nvnCommandBufferBindSeparateSamplersDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateSamplerHandle *h) { nvn_bind_handles_deferred(c, OP_BIND_SAMP, st, first, n, (const uint64_t *)h, 16 + 8 * (size_t)n); }
/* Memory callbacks are never needed: records live in host memory (hle_nvn.c cmd_reserve). */
NVN_EXTRA(nvnCommandBufferSetControlMemoryCallbackEnabled, void, NVNcommandBuffer *c, NVNboolean e) { (void)c; (void)e; }
NVN_NOPROTO(nvnCommandBufferSetCommandMemoryCallbackEnabled, void, NVNcommandBuffer *c, NVNboolean e) { (void)c; (void)e; }
/* Tiled-cache subtile size, ZCull compression and debug checkpoints are hardware tuning / debugging hints. */
NVN_EXTRA(nvnCommandBufferOverrideSubtileSize, void, NVNcommandBuffer *c) { (void)c; }
NVN_EXTRA(nvnCommandBufferSetZCullZF32CompressionEnable, void, NVNcommandBuffer *c, NVNboolean e) { (void)c; (void)e; }
NVN_EXTRA(nvnDeviceSetAutomaticCheckpointsMask, void, NVNdevice *d, uint32_t m) { (void)d; (void)m; }
/* NVN default provoking vertex is the last vertex (GL convention); Metal uses the first. Only the default is accepted. */
NVN_EXTRA(nvnCommandBufferSetProvokingVertexMode, void, NVNcommandBuffer *c, int mode) {
    (void)c; if (mode != 0) { static int once; if (!once++) LOG("【红】SetProvokingVertexMode %d: provoking-vertex conversion not implemented", mode); }
}
NVN_EXTRA(nvnTextureBuilderGetViewOffset, ptrdiff_t, const NVNtextureBuilder *b, const NVNtextureView *v) {
    const tex_builder_t *o = CTB(b); const view_t *w = CVW(v);
    if (!w) return 0;
    nvn_texlayout L; nvn_tex_layout(&o->d, &L);
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&o->d, w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * L.layer_size;
    return (ptrdiff_t)off;
}
NVN_EXTRA(nvnProgramGetSubtileSize, void, const NVNprogram *p) { (void)p; die("nvnProgramGetSubtileSize: signature not identified"); }

// ---------- ZCull (Tegra depth-cull cache): no Metal equivalent; zero storage is needed and save/restore have nothing to move ----------
NVN(nvnTextureBuilderGetZCullStorageSize, size_t, const NVNtextureBuilder *b) { (void)b; return 0; }
NVN(nvnTextureGetZCullStorageSize, size_t, const NVNtexture *t) { (void)t; return 0; }
NVN(nvnCommandBufferSaveZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }
NVN(nvnCommandBufferRestoreZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }

// ---------- Tegra tiled-cache and rasterisation hints ----------
NVN(nvnCommandBufferSetTiledCacheAction, void, NVNcommandBuffer *c, NVNtiledCacheAction a) { (void)c; (void)a; }        /* tiled-cache flush/enable hint */
NVN(nvnCommandBufferSetTiledCacheTileSize, void, NVNcommandBuffer *c, int w, int h) { (void)c; (void)w; (void)h; }
NVN(nvnCommandBufferSetSubpixelPrecisionBias, void, NVNcommandBuffer *c, int x, int y) { (void)c; (void)x; (void)y; }
NVN(nvnCommandBufferDiscardColor, void, NVNcommandBuffer *c, int i) { (void)c; (void)i; }          /* contents become undefined: keeping them is valid */
NVN(nvnCommandBufferDiscardDepthStencil, void, NVNcommandBuffer *c) { (void)c; }
NVN(nvnCommandBufferSetConservativeRasterEnable, void, NVNcommandBuffer *c, uint32_t e) {
    (void)c; if (e & 0xff) { static int once; if (!once++) LOG("【红】conservative rasterisation requested: not available on Metal (drawn normally)"); }
}
NVN(nvnCommandBufferSetConservativeRasterDilate, void, NVNcommandBuffer *c, float d) { (void)c; (void)d; }
NVN(nvnCommandBufferSetSampleMask, void, NVNcommandBuffer *c, int m) {
    (void)c; if ((m & 0xffff) != 0xffff) { static int once; if (!once++) LOG("【红】SetSampleMask %#x: fixed-function sample mask not applied", m); }
}
NVN(nvnCommandBufferSetAlphaRef, void, NVNcommandBuffer *c, float r) { (void)c; (void)r; }   /* only used with the alpha test, reported in the replay */
/* Metal always restarts strips at the all-ones index; other restart indices are reported. */
NVN(nvnCommandBufferSetPrimitiveRestart, void, NVNcommandBuffer *c, uint32_t e, int index) {
    (void)c; if ((e & 0xff) && index != -1 && index != 0xffff) { static int once; if (!once++) LOG("【红】SetPrimitiveRestart index %#x: Metal restarts only at the all-ones index", index); }
}
/* Metal has no depth-bounds test. Disabling is exact; an enabled range is drawn without the test and reported. */
NVN(nvnCommandBufferSetDepthBounds, void, NVNcommandBuffer *c, uint32_t e, float lo, float hi) {
    (void)c; if ((e & 0xff) && (lo > 0.0f || hi < 1.0f)) { static int once; if (!once++) LOG("【红】SetDepthBounds [%g, %g]: depth-bounds test not available on Metal (fragments outside the range are kept)", lo, hi); }
}
