static const char sc_attrs_probe[] __attribute__((unused)) = "scenecap-attrs-probe-1";
/* scenecap NVN -> Metal glue (implementation).
 *
 * Textually included at the end of each game's gfx/nvn/nvn_mtl.m, after the replay layer's own
 * definitions, because it reads that file's static state (S, R, g_pass, g_cur_frame, the texture
 * registry, prog_get, the Metal queue helpers).  Define SCENECAP_NVN_TOTK to 0 (nier) or 1 (totk)
 * before including it; the two forks differ only in how a bound texture handle is resolved.
 *
 * The hook API is scenecap_nvn.h; the recording logic is the game-independent capture module
 * (../../format + scenecap_capture.c).  Everything here is a no-op until SCENECAP=1 (or the
 * "scenecap" diagnostic command) asks for a capture.
 */
#include "scenecap_capture.h"

#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#if !defined(SCENECAP_NVN_TOTK)
#error "define SCENECAP_NVN_TOTK (0 for nier, 1 for totk) before including scenecap_nvn.inc"
#endif

/* Game name recorded when SCENECAP_GAME is not set.  A fork defines its own (odyssey, pla); totk and nier keep
 * the historical default. */
#ifndef SCENECAP_NVN_DEFAULT_GAME
#define SCENECAP_NVN_DEFAULT_GAME (SCENECAP_NVN_TOTK ? "totk" : "nier")
#endif

#define SC_TEX_MAX 4096
#define SC_SHADER_MAX 256
#define SC_BIND_MAX 2600

typedef struct {
    uint64_t serial;
    tex_t meta;
    __strong id<MTLTexture> mtl;
} sc_tex_ent;

typedef struct {
    char key[96];
    __strong NvShader *sh;
} sc_shader_ent;

static sc_tex_ent g_sc_tex[SC_TEX_MAX];
static int g_sc_ntex;
static sc_shader_ent g_sc_shader[SC_SHADER_MAX];
static int g_sc_nshader;
static scenecap_capture *g_sc_cap;
static int g_sc_state; /* 0 uninitialised, 1 disabled, 2 capturing */
static int g_sc_in_pass;
static char g_sc_request[1024];
static _Atomic int g_sc_requested;
static _Atomic unsigned g_sc_want_frames;  /* 0 = SCENECAP_FRAMES / default; set by dm_gfx_scenecap_record */
static char g_sc_game[64], g_sc_title[64], g_sc_version[64], g_sc_commit[96], g_sc_port[64], g_sc_build[96];
static char g_sc_gpu[128], g_sc_macos[128], g_sc_metal[64], g_sc_when[64];
static __strong NSData *g_sc_source;  /* keeps the MSL bytes handed to the capture module alive */
static __strong NSData *g_sc_refl;

static void sc_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char line[512];
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    MLOG("scenecap: %s", line);
}

/* ---------------------------------------------------------------------------------------------------------- *
 * Small mappings: NVN enums and Metal pixel formats to the names the manifest uses.
 * ---------------------------------------------------------------------------------------------------------- */

static const char *sc_pf_name(uint32_t pf) {
    switch (pf) {
    case MTLPixelFormatR8Unorm: return "r8unorm";
    case MTLPixelFormatR8Snorm: return "r8snorm";
    case MTLPixelFormatR8Uint: return "r8uint";
    case MTLPixelFormatR8Sint: return "r8sint";
    case MTLPixelFormatR16Unorm: return "r16unorm";
    case MTLPixelFormatR16Snorm: return "r16snorm";
    case MTLPixelFormatR16Float: return "r16float";
    case MTLPixelFormatR16Uint: return "r16uint";
    case MTLPixelFormatR16Sint: return "r16sint";
    case MTLPixelFormatRG8Unorm: return "rg8unorm";
    case MTLPixelFormatRG8Snorm: return "rg8snorm";
    case MTLPixelFormatRG8Uint: return "rg8uint";
    case MTLPixelFormatRG8Sint: return "rg8sint";
    case MTLPixelFormatR32Uint: return "r32uint";
    case MTLPixelFormatR32Sint: return "r32sint";
    case MTLPixelFormatR32Float: return "r32float";
    case MTLPixelFormatRG16Unorm: return "rg16unorm";
    case MTLPixelFormatRG16Snorm: return "rg16snorm";
    case MTLPixelFormatRG16Float: return "rg16float";
    case MTLPixelFormatRG16Uint: return "rg16uint";
    case MTLPixelFormatRG16Sint: return "rg16sint";
    case MTLPixelFormatRGBA8Unorm: return "rgba8unorm";
    case MTLPixelFormatRGBA8Unorm_sRGB: return "rgba8unorm_srgb";
    case MTLPixelFormatRGBA8Snorm: return "rgba8snorm";
    case MTLPixelFormatRGBA8Uint: return "rgba8uint";
    case MTLPixelFormatRGBA8Sint: return "rgba8sint";
    case MTLPixelFormatBGRA8Unorm: return "bgra8unorm";
    case MTLPixelFormatBGRA8Unorm_sRGB: return "bgra8unorm_srgb";
    case MTLPixelFormatRGB10A2Unorm: return "rgb10a2unorm";
    case MTLPixelFormatRG11B10Float: return "rg11b10float";
    case MTLPixelFormatRG32Uint: return "rg32uint";
    case MTLPixelFormatRG32Sint: return "rg32sint";
    case MTLPixelFormatRG32Float: return "rg32float";
    case MTLPixelFormatRGBA16Unorm: return "rgba16unorm";
    case MTLPixelFormatRGBA16Snorm: return "rgba16snorm";
    case MTLPixelFormatRGBA16Float: return "rgba16float";
    case MTLPixelFormatRGBA16Uint: return "rgba16uint";
    case MTLPixelFormatRGBA16Sint: return "rgba16sint";
    case MTLPixelFormatRGBA32Uint: return "rgba32uint";
    case MTLPixelFormatRGBA32Sint: return "rgba32sint";
    case MTLPixelFormatRGBA32Float: return "rgba32float";
    case MTLPixelFormatDepth16Unorm: return "depth16unorm";
    case MTLPixelFormatDepth32Float: return "depth32float";
    case MTLPixelFormatDepth32Float_Stencil8: return "depth32float_stencil8";
    case MTLPixelFormatStencil8: return "stencil8";
    case MTLPixelFormatBC1_RGBA: return "bc1_rgba";
    case MTLPixelFormatBC1_RGBA_sRGB: return "bc1_rgba_srgb";
    case MTLPixelFormatBC2_RGBA: return "bc2_rgba";
    case MTLPixelFormatBC2_RGBA_sRGB: return "bc2_rgba_srgb";
    case MTLPixelFormatBC3_RGBA: return "bc3_rgba";
    case MTLPixelFormatBC3_RGBA_sRGB: return "bc3_rgba_srgb";
    case MTLPixelFormatBC4_RUnorm: return "bc4_runorm";
    case MTLPixelFormatBC5_RGUnorm: return "bc5_rgunorm";
    case MTLPixelFormatBC7_RGBAUnorm: return "bc7_rgbaunorm";
    case MTLPixelFormatBC7_RGBAUnorm_sRGB: return "bc7_rgbaunorm_srgb";
    case MTLPixelFormatASTC_4x4_LDR: return "astc_4x4";
    case MTLPixelFormatASTC_4x4_sRGB: return "astc_4x4_srgb";
    case MTLPixelFormatASTC_5x5_LDR: return "astc_5x5";
    case MTLPixelFormatASTC_5x5_sRGB: return "astc_5x5_srgb";
    case MTLPixelFormatASTC_6x6_LDR: return "astc_6x6";
    case MTLPixelFormatASTC_6x6_sRGB: return "astc_6x6_srgb";
    case MTLPixelFormatASTC_8x8_LDR: return "astc_8x8";
    case MTLPixelFormatASTC_8x8_sRGB: return "astc_8x8_srgb";
    case MTLPixelFormatASTC_10x10_LDR: return "astc_10x10";
    case MTLPixelFormatASTC_10x10_sRGB: return "astc_10x10_srgb";
    case MTLPixelFormatASTC_12x12_LDR: return "astc_12x12";
    case MTLPixelFormatASTC_12x12_sRGB: return "astc_12x12_srgb";
    default: return "unknown";
    }
}

/* Block-compressed layout: returns 1 and fills (bw,bh,bbs) for BC/ASTC; 0 for linear formats. */
static int sc_block_layout(MTLPixelFormat pf, NSUInteger *bw, NSUInteger *bh, NSUInteger *bbs) {
    switch (pf) {
    case MTLPixelFormatBC1_RGBA:
    case MTLPixelFormatBC1_RGBA_sRGB:
    case MTLPixelFormatBC4_RUnorm:
        *bw = 4; *bh = 4; *bbs = 8; return 1;
    case MTLPixelFormatBC2_RGBA:
    case MTLPixelFormatBC2_RGBA_sRGB:
    case MTLPixelFormatBC3_RGBA:
    case MTLPixelFormatBC3_RGBA_sRGB:
    case MTLPixelFormatBC5_RGUnorm:
    case MTLPixelFormatBC7_RGBAUnorm:
    case MTLPixelFormatBC7_RGBAUnorm_sRGB:
        *bw = 4; *bh = 4; *bbs = 16; return 1;
    case MTLPixelFormatASTC_4x4_LDR:
    case MTLPixelFormatASTC_4x4_sRGB:
        *bw = 4; *bh = 4; *bbs = 16; return 1;
    case MTLPixelFormatASTC_5x5_LDR:
    case MTLPixelFormatASTC_5x5_sRGB:
        *bw = 5; *bh = 5; *bbs = 16; return 1;
    case MTLPixelFormatASTC_6x6_LDR:
    case MTLPixelFormatASTC_6x6_sRGB:
        *bw = 6; *bh = 6; *bbs = 16; return 1;
    case MTLPixelFormatASTC_8x8_LDR:
    case MTLPixelFormatASTC_8x8_sRGB:
        *bw = 8; *bh = 8; *bbs = 16; return 1;
    case MTLPixelFormatASTC_10x10_LDR:
    case MTLPixelFormatASTC_10x10_sRGB:
        *bw = 10; *bh = 10; *bbs = 16; return 1;
    case MTLPixelFormatASTC_12x12_LDR:
    case MTLPixelFormatASTC_12x12_sRGB:
        *bw = 12; *bh = 12; *bbs = 16; return 1;
    default:
        return 0;
    }
}

static uint32_t sc_texture_type(const tex_t *t) {
    switch (t->d.target) {
    case NVN_TEXTURE_TARGET_1D: return 0;
    case NVN_TEXTURE_TARGET_1D_ARRAY: return 0;
    case NVN_TEXTURE_TARGET_3D: return 2;
    case NVN_TEXTURE_TARGET_CUBEMAP: return 3;
    case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: return 5;
    case NVN_TEXTURE_TARGET_2D_ARRAY: return 4;
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE:
    case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: return 6;
    default: return t->d.samples > 1 ? 6 : 1;
    }
}

static uint8_t sc_blend_factor(uint32_t f) {
    switch (f) {
    case NVN_BLEND_FUNC_ZERO: return SCENECAP_BLEND_ZERO;
    case NVN_BLEND_FUNC_ONE: return SCENECAP_BLEND_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR: return SCENECAP_BLEND_SOURCE_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return SCENECAP_BLEND_ONE_MINUS_SOURCE_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA: return SCENECAP_BLEND_SOURCE_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return SCENECAP_BLEND_ONE_MINUS_SOURCE_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA: return SCENECAP_BLEND_DESTINATION_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return SCENECAP_BLEND_ONE_MINUS_DESTINATION_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR: return SCENECAP_BLEND_DESTINATION_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return SCENECAP_BLEND_ONE_MINUS_DESTINATION_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return SCENECAP_BLEND_SOURCE_ALPHA_SATURATED;
    case NVN_BLEND_FUNC_CONSTANT_COLOR: return SCENECAP_BLEND_BLEND_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return SCENECAP_BLEND_ONE_MINUS_BLEND_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA: return SCENECAP_BLEND_BLEND_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return SCENECAP_BLEND_ONE_MINUS_BLEND_ALPHA;
    case NVN_BLEND_FUNC_SRC1_COLOR: return SCENECAP_BLEND_SOURCE_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return SCENECAP_BLEND_ONE_MINUS_SOURCE_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA: return SCENECAP_BLEND_SOURCE_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return SCENECAP_BLEND_ONE_MINUS_SOURCE_ALPHA;
    default: return SCENECAP_BLEND_ONE;
    }
}

static uint8_t sc_blend_op(uint32_t e) {
    switch (e) {
    case NVN_BLEND_EQUATION_SUB: return SCENECAP_BLEND_OP_SUBTRACT;
    case NVN_BLEND_EQUATION_MIN: return SCENECAP_BLEND_OP_MIN;
    case NVN_BLEND_EQUATION_MAX: return SCENECAP_BLEND_OP_MAX;
    case NVN_BLEND_EQUATION_REVERSE_SUB: return SCENECAP_BLEND_OP_REVERSE_SUBTRACT;
    default: return SCENECAP_BLEND_OP_ADD;
    }
}

static uint8_t sc_compare(uint32_t f) {
    switch (f) {
    case NVN_DEPTH_FUNC_NEVER: return SCENECAP_COMPARE_NEVER;
    case NVN_DEPTH_FUNC_LESS: return SCENECAP_COMPARE_LESS;
    case NVN_DEPTH_FUNC_EQUAL: return SCENECAP_COMPARE_EQUAL;
    case NVN_DEPTH_FUNC_LEQUAL: return SCENECAP_COMPARE_LESS_EQUAL;
    case NVN_DEPTH_FUNC_GREATER: return SCENECAP_COMPARE_GREATER;
    case NVN_DEPTH_FUNC_NOTEQUAL: return SCENECAP_COMPARE_NOT_EQUAL;
    case NVN_DEPTH_FUNC_GEQUAL: return SCENECAP_COMPARE_GREATER_EQUAL;
    default: return SCENECAP_COMPARE_ALWAYS;
    }
}

static uint32_t sc_index_type(int32_t itype) {
    if (itype == 0) return SCENECAP_INDEX_UINT8;
    if (itype == 1) return SCENECAP_INDEX_UINT16;
    return SCENECAP_INDEX_UINT32;
}

static uint32_t sc_primitive(uint32_t prim) {
    switch (prim) {
    case NVN_DRAW_PRIMITIVE_POINTS: return SCENECAP_PRIM_POINT;
    case NVN_DRAW_PRIMITIVE_LINES: return SCENECAP_PRIM_LINE;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: return SCENECAP_PRIM_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_LINE_LOOP: return SCENECAP_PRIM_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return SCENECAP_PRIM_TRIANGLE_STRIP;
    default: return SCENECAP_PRIM_TRIANGLE;
    }
}

/* ---------------------------------------------------------------------------------------------------------- *
 * Texture bookkeeping: serial -> (texture metadata, Metal texture object).
 * ---------------------------------------------------------------------------------------------------------- */

static const tex_t *sc_tex_meta(uint64_t handle) {
#if SCENECAP_NVN_TOTK
    const treg_t *r = bound_tex(handle);
    return r ? &r->t : NULL;
#else
    int tid = (int)(handle & 0xfffff);
    if (tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok) return NULL;
    return &g_treg[tid].t;
#endif
}

static void sc_remember_tex(const tex_t *t, id<MTLTexture> mtl) {
    if (!t || !mtl || !t->serial) return;
    for (int i = 0; i < g_sc_ntex; i++)
        if (g_sc_tex[i].serial == t->serial) {
            g_sc_tex[i].meta = *t;
            g_sc_tex[i].mtl = mtl;
            return;
        }
    if (g_sc_ntex >= SC_TEX_MAX) return;
    g_sc_tex[g_sc_ntex].serial = t->serial;
    g_sc_tex[g_sc_ntex].meta = *t;
    g_sc_tex[g_sc_ntex].mtl = mtl;
    g_sc_ntex++;
}

static sc_tex_ent *sc_find_tex(uint64_t serial) {
    for (int i = 0; i < g_sc_ntex; i++)
        if (g_sc_tex[i].serial == serial) return &g_sc_tex[i];
    return NULL;
}

static void sc_remember_shader(NvShader *sh) {
    if (!sh || !sh->key[0]) return;
    for (int i = 0; i < g_sc_nshader; i++)
        if (!strcmp(g_sc_shader[i].key, sh->key)) {
            g_sc_shader[i].sh = sh;
            return;
        }
    if (g_sc_nshader >= SC_SHADER_MAX) return;
    snprintf(g_sc_shader[g_sc_nshader].key, sizeof g_sc_shader[g_sc_nshader].key, "%s", sh->key);
    g_sc_shader[g_sc_nshader].sh = sh;
    g_sc_nshader++;
}

static NvShader *sc_find_shader(const char *key) {
    for (int i = 0; i < g_sc_nshader; i++)
        if (!strcmp(g_sc_shader[i].key, key)) return g_sc_shader[i].sh;
    return nil;
}

/* ---------------------------------------------------------------------------------------------------------- *
 * Host callbacks.
 * ---------------------------------------------------------------------------------------------------------- */

static int sc_host_texture_info(void *user, uint64_t serial, scenecap_texture_info *out) {
    (void)user;
    sc_tex_ent *e = sc_find_tex(serial);
    if (!e || !e->mtl) return 0;
    id<MTLTexture> m = e->mtl;
    memset(out, 0, sizeof *out);
    out->width = (uint32_t)m.width;
    out->height = (uint32_t)m.height;
    out->depth = (uint32_t)m.depth;
    out->mip_levels = (uint32_t)m.mipmapLevelCount;
    out->array_layers = (uint32_t)m.arrayLength;
    out->sample_count = (uint32_t)m.sampleCount;
    out->texture_type = sc_texture_type(&e->meta);
    snprintf(out->pixel_format, sizeof out->pixel_format, "%s", sc_pf_name((uint32_t)m.pixelFormat));
    return 1;
}

static size_t sc_pixel_bytes(MTLPixelFormat pf) {
    /* Depth32Float_Stencil8 is snapshotted with MTLBlitOptionDepthFromDepthStencil → 4-byte depth floats. */
    switch (pf) {
    case MTLPixelFormatR8Unorm:
    case MTLPixelFormatR8Snorm:
    case MTLPixelFormatR8Uint:
    case MTLPixelFormatR8Sint:
    case MTLPixelFormatStencil8:
        return 1;
    case MTLPixelFormatR16Unorm:
    case MTLPixelFormatR16Snorm:
    case MTLPixelFormatR16Float:
    case MTLPixelFormatR16Uint:
    case MTLPixelFormatR16Sint:
    case MTLPixelFormatRG8Unorm:
    case MTLPixelFormatRG8Snorm:
    case MTLPixelFormatRG8Uint:
    case MTLPixelFormatRG8Sint:
    case MTLPixelFormatDepth16Unorm:
        return 2;
    case MTLPixelFormatR32Uint:
    case MTLPixelFormatR32Sint:
    case MTLPixelFormatR32Float:
    case MTLPixelFormatRG16Unorm:
    case MTLPixelFormatRG16Snorm:
    case MTLPixelFormatRG16Float:
    case MTLPixelFormatRG16Uint:
    case MTLPixelFormatRG16Sint:
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
    case MTLPixelFormatDepth32Float:
    case MTLPixelFormatDepth32Float_Stencil8:
    case MTLPixelFormatRGB10A2Unorm:
    case MTLPixelFormatRG11B10Float:
        return 4;
    case MTLPixelFormatRG32Uint:
    case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRG32Float:
    case MTLPixelFormatRGBA16Unorm:
    case MTLPixelFormatRGBA16Snorm:
    case MTLPixelFormatRGBA16Float:
    case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatRGBA16Sint:
        return 8;
    case MTLPixelFormatRGBA32Uint:
    case MTLPixelFormatRGBA32Sint:
    case MTLPixelFormatRGBA32Float:
        return 16;
    default:
        return 4;
    }
}

static long sc_host_texture_read(void *user, uint64_t serial, uint32_t level, uint32_t layer, void *dst,
                                 size_t capacity) {
    (void)user;
    sc_tex_ent *e = sc_find_tex(serial);
    if (!e || !e->mtl) return -1;
    id<MTLTexture> t = e->mtl;
    if (t.sampleCount > 1 || t.framebufferOnly) return -1;
    NSUInteger w = MAX(t.width >> level, 1), h = MAX(t.height >> level, 1);
    NSUInteger bw = 1, bh = 1, bbs = 1;
    int is_block = sc_block_layout(t.pixelFormat, &bw, &bh, &bbs);
    NSUInteger row_blocks = is_block ? (w + bw - 1) / bw : w;
    NSUInteger col_blocks = is_block ? (h + bh - 1) / bh : h;
    size_t row = is_block ? (size_t)row_blocks * bbs : (size_t)w * sc_pixel_bytes(t.pixelFormat);
    size_t image = row * (is_block ? col_blocks : h);
    if (image > capacity) return -1;
    /* Metal requires destinationBytesPerRow to be a multiple of the block row for compressed formats;
       for linear formats keep the historical 256-byte alignment. */
    NSUInteger aligned = is_block ? (NSUInteger)row : ((row + 255) & ~(NSUInteger)255);
    if (aligned < row) aligned = (NSUInteger)row;
    id<MTLBuffer> rb = [g_dev newBufferWithLength:aligned * (is_block ? col_blocks : h)
                                          options:MTLResourceStorageModeShared];
    if (!rb) return -1;
    id<MTLCommandBuffer> cb = q_cb();
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    /* Depth+stencil targets are snapshotted as depth-only float32 (4 B/texel), matching the replayer's
       depth32float_stencil8 upload path that only needs the depth plane for EQUAL tests. */
    MTLBlitOption opt = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil
                                                                             : MTLBlitOptionNone;
    [blit copyFromTexture:t
              sourceSlice:layer
              sourceLevel:level
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:rb
        destinationOffset:0
   destinationBytesPerRow:aligned
 destinationBytesPerImage:aligned * (is_block ? col_blocks : h)
                  options:opt];
    [blit endEncoding];
    cb_commit(cb);
    cb_wait(cb);
    const uint8_t *src = (const uint8_t *)rb.contents;
    uint8_t *out = (uint8_t *)dst;
    NSUInteger rows = is_block ? col_blocks : h;
    for (NSUInteger y = 0; y < rows; y++) memcpy(out + y * row, src + y * aligned, row);
    return (long)image;
}

static long sc_host_buffer_read(void *user, uint64_t address, uint64_t length, void *dst, size_t capacity) {
    (void)user;
    if (!address || !length || length > capacity) return -1;
    void *mb = NULL;
    uint64_t mo = 0;
    if (!mtl_addr(address, (size_t)length, &mb, &mo)) return -1;
    pend_materialize(address, (uint8_t *)dst, (size_t)length, 0);
    return (long)length;
}

static int sc_read_file(NSData *__strong *keep, NSString *path, const char **text, size_t *length) {
    if (!path) return 0;
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data || !data.length) return 0;
    *keep = data;
    *text = (const char *)data.bytes;
    *length = (size_t)data.length;
    return 1;
}

static int sc_host_shader_source(void *user, const char *key, const char **text, size_t *length) {
    (void)user;
    NvShader *sh = sc_find_shader(key);
    if (!sh) return 0;
    NSString *path = sh->src_path;
    if (!path) return 0;
    if (sc_read_file(&g_sc_source, path, text, length)) return 1;
    /* The iOS replay keeps only the path; read the source next to it. */
    NSString *candidate = [path stringByAppendingString:@".metal"];
    return sc_read_file(&g_sc_source, candidate, text, length);
}

static int sc_host_shader_reflection(void *user, const char *key, const char **text, size_t *length) {
    (void)user;
    NvShader *sh = sc_find_shader(key);
    if (!sh) return 0;
    NSString *path = sh->src_path;
    if (!path) return 0;
    NSString *json = [[path stringByDeletingPathExtension] stringByAppendingPathExtension:@"json"];
    return sc_read_file(&g_sc_refl, json, text, length);
}

static void sc_host_log(void *user, const char *message) {
    (void)user;
    MLOG("scenecap: %s", message);
}

/* ---------------------------------------------------------------------------------------------------------- *
 * Capture setup.
 * ---------------------------------------------------------------------------------------------------------- */

static void sc_iso8601(char *out, size_t size) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void sc_copy_env(char *dst, size_t size, const char *name, const char *fallback) {
    const char *v = getenv(name);
    snprintf(dst, size, "%s", v && *v ? v : fallback);
}

static void sc_fill_metadata(scenecap_options *o) {
    sc_copy_env(g_sc_game, sizeof g_sc_game, "SCENECAP_GAME", SCENECAP_NVN_DEFAULT_GAME);
    sc_copy_env(g_sc_title, sizeof g_sc_title, "SCENECAP_TITLE_ID", "unknown");
    sc_copy_env(g_sc_version, sizeof g_sc_version, "SCENECAP_GAME_VERSION", "unknown");
    sc_copy_env(g_sc_commit, sizeof g_sc_commit, "SCENECAP_GIT_COMMIT", "unknown");
    sc_copy_env(g_sc_port, sizeof g_sc_port, "SCENECAP_PORT_VERSION", "d2r_gfx");
    sc_copy_env(g_sc_build, sizeof g_sc_build, "SCENECAP_BUILD_VERSION", "unknown");
    if (!g_sc_gpu[0]) {
        NSString *name = g_dev.name;
        snprintf(g_sc_gpu, sizeof g_sc_gpu, "%s", name ? name.UTF8String : "unknown");
    }
    if (!g_sc_macos[0]) {
        NSString *v = NSProcessInfo.processInfo.operatingSystemVersionString;
        snprintf(g_sc_macos, sizeof g_sc_macos, "%s", v ? v.UTF8String : "unknown");
    }
    if (!g_sc_metal[0]) {
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 260000
        snprintf(g_sc_metal, sizeof g_sc_metal, "Metal 4");
#else
        snprintf(g_sc_metal, sizeof g_sc_metal, "Metal 3");
#endif
    }
    sc_iso8601(g_sc_when, sizeof g_sc_when);
    o->game = g_sc_game;
    o->title_id = g_sc_title;
    o->game_version = g_sc_version;
    o->git_commit = g_sc_commit;
    o->port_version = g_sc_port;
    o->build_version = g_sc_build;
    o->gpu = g_sc_gpu;
    o->macos = g_sc_macos;
    o->metal = g_sc_metal;
    o->captured_at = g_sc_when;
}

static int sc_start(const char *path, uint32_t want_frames) {
    if (g_sc_state == 2) return 1;
    scenecap_options o;
    memset(&o, 0, sizeof o);
    int env_on = scenecap_options_from_env(&o);
    if (path && *path) {
        snprintf(g_sc_request, sizeof g_sc_request, "%s", path);
        o.output_path = g_sc_request;
        if (!env_on) {
            if (!o.frame_count) o.frame_count = 1;
            if (!o.generator) o.generator = "scenecap-nvn/1.0";
            /* Command/F9 path: ignore SCENECAP_FIRST_FRAME deferral — the user asked for "next frame". */
            o.first_frame = 0;
        }
        if (want_frames) o.frame_count = want_frames;  /* explicit count from the command line or the hotkey */
        /* Courtyard body frames need multi-GB VBs; if the env left the 512 MiB default, bump it. */
        if (o.max_total_bytes <= 512ull * 1024ull * 1024ull) {
            o.max_total_bytes = 8ull * 1024ull * 1024ull * 1024ull;
            if (o.max_buffer_bytes <= 16ull * 1024ull * 1024ull)
                o.max_buffer_bytes = 256ull * 1024ull * 1024ull;
        }
    } else if (!env_on) {
        g_sc_state = 1;
        return 0;
    }
    sc_fill_metadata(&o);
    scenecap_host host;
    memset(&host, 0, sizeof host);
    host.texture_info = sc_host_texture_info;
    host.texture_read = sc_host_texture_read;
    host.buffer_read = sc_host_buffer_read;
    host.shader_source = sc_host_shader_source;
    host.shader_reflection = sc_host_shader_reflection;
    host.log = sc_host_log;
    char err[512];
    g_sc_cap = scenecap_create(&host, &o, err, sizeof err);
    if (!g_sc_cap) {
        sc_log("cannot start: %s", err);
        g_sc_state = 1;
        return 0;
    }
    g_sc_state = 2;
    g_sc_ntex = 0;
    g_sc_nshader = 0;
    sc_log("capturing frames %llu+%u into %s", (unsigned long long)o.first_frame, o.frame_count,
           o.output_path ? o.output_path : "(default)");
    return 1;
}

/* ---------------------------------------------------------------------------------------------------------- *
 * Hooks.
 * ---------------------------------------------------------------------------------------------------------- */

int scenecap_nvn_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SCENECAP");
        on = e && *e && strcmp(e, "0") != 0;
        if (on) MLOG("scenecap: capture requested by SCENECAP=%s", e);
    }
    return on;
}

/* --------------------------------------------------------------------------------------------------------- *
 * Development control socket: a capture-enabled build (SCENECAP_HOOKS=1) listens on
 * /tmp/scenecap-<pid>.sock so the CLI can drive captures without a hotkey or input file:
 *     capture <frames> <path>   start a multi-frame capture at the next frame boundary
 *     single <path>             capture one frame
 *     status                    report the capture state
 * The thread only sets the same atomics the input command sets; every capture decision still runs on the
 * game's render thread.  Not present in builds that leave SCENECAP_HOOKS at 0.                              */
static void *sc_ctl_main(void *arg) {
    (void)arg;
    char sock_path[128];
    snprintf(sock_path, sizeof sock_path, "/tmp/scenecap-%d.sock", (int)getpid());
    unlink(sock_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock_path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 4) != 0) { close(fd); return NULL; }
    sc_log("control socket %s", sock_path);
    for (;;) {
        int c = accept(fd, NULL, NULL);
        if (c < 0) continue;
        char buf[1200], reply[1200];
        ssize_t n = read(c, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            char *nl = strchr(buf, '\n'); if (nl) *nl = 0;
            if (!strncmp(buf, "capture ", 8) || !strncmp(buf, "single ", 7)) {
                int single = buf[0] == 's';
                char *p = buf + (single ? 7 : 8);
                unsigned frames = single ? 1 : (unsigned)strtoul(p, &p, 10);
                while (*p == ' ') p++;
                if (!*p) snprintf(reply, sizeof reply, "error missing path\n");
                else {
                    pthread_mutex_lock(&g_mu);
                    snprintf(g_sc_request, sizeof g_sc_request, "%s", p);
                    pthread_mutex_unlock(&g_mu);
                    atomic_store(&g_sc_want_frames, frames ? frames : 1);
                    atomic_store(&g_sc_requested, 1);
                    snprintf(reply, sizeof reply, "ok %s\n", p);
                }
            } else if (!strncmp(buf, "status", 6)) {
                snprintf(reply, sizeof reply, "state %d\n", g_sc_state);
            } else snprintf(reply, sizeof reply, "error unknown command\n");
            (void)!write(c, reply, strlen(reply));
        }
        close(c);
    }
}
static void sc_ctl_start(void) {
    pthread_t t;
    if (pthread_create(&t, NULL, sc_ctl_main, NULL) == 0) pthread_detach(t);
}
__attribute__((constructor)) static void sc_ctl_boot(void) { sc_ctl_start(); }

/* Only inspects state: the hooks below run inside the render encoder (and possibly on its own thread), so
 * staring a capture here would do host reads inside a streamed pass.  The frame hook owns every start. */
static int sc_ready(void) {
    return g_sc_state == 2 && g_sc_cap && scenecap_active(g_sc_cap);
}

void scenecap_nvn_frame(uint64_t frame) {
    /* Keep running while a capture started by the input command is still open: that path never sets
     * SCENECAP=1, and returning here would leave the window without its closing frame_begin. */
    if (!scenecap_nvn_on() && !atomic_load(&g_sc_requested) && !g_sc_cap) {
        if (g_sc_state == 0) g_sc_state = 1;
        return;
    }
    if (g_sc_state == 0 && scenecap_nvn_on()) sc_start(NULL, 0);
    if (atomic_load(&g_sc_requested)) {
        char path[sizeof g_sc_request];
        pthread_mutex_lock(&g_mu);
        snprintf(path, sizeof path, "%s", g_sc_request);
        pthread_mutex_unlock(&g_mu);
        atomic_store(&g_sc_requested, 0);
        g_sc_state = 0;
        unsigned want = atomic_load(&g_sc_want_frames);
        atomic_store(&g_sc_want_frames, 0);
        if (!sc_start(path[0] ? path : NULL, want)) return;
    } else if (g_sc_state == 0 && !sc_start(NULL, 0)) {
        return;
    }
    if (!g_sc_cap) return;
    scenecap_frame_begin(g_sc_cap, frame);
}

void scenecap_nvn_pass_begin(void *descriptor, uint32_t width, uint32_t height) {
    if (!sc_ready() || g_sc_in_pass) return;
    MTL4RenderPassDescriptor *pd = (__bridge MTL4RenderPassDescriptor *)descriptor;
    scenecap_attachment atts[10];
    uint32_t n = 0;
    for (int i = 0; i < R.rt.n && i < 8; i++) {
        const tex_t *t = R.rt.color[i];
        if (!t) continue;
        id<MTLTexture> mt = tex_mtl(t);
        if (!mt || !mt.width || !mt.height) continue;
        sc_remember_tex(t, mt);
        scenecap_attachment *a = &atts[n];
        memset(a, 0, sizeof *a);
        a->slot = (uint32_t)i;
        a->has_texture = 1;
        a->texture = t->serial;
        a->level = R.rt.has_cview[i] ? R.rt.cview[i].base_level : 0;
        a->layer = R.rt.has_cview[i] ? R.rt.cview[i].min_layer : 0;
        __typeof__(pd.colorAttachments[0]) ca = pd.colorAttachments[i];
        a->load = ca.loadAction == MTLLoadActionClear ? SCENECAP_LOAD_CLEAR
                  : ca.loadAction == MTLLoadActionLoad ? SCENECAP_LOAD_LOAD
                                                       : SCENECAP_LOAD_DONTCARE;
        a->store = ca.storeAction == MTLStoreActionStore ? SCENECAP_STORE_STORE : SCENECAP_STORE_DONTCARE;
        if (a->load == SCENECAP_LOAD_CLEAR) {
            MTLClearColor c = ca.clearColor;
            a->clear[0] = (float)c.red;
            a->clear[1] = (float)c.green;
            a->clear[2] = (float)c.blue;
            a->clear[3] = (float)c.alpha;
            a->clear_count = 4;
        }
        n++;
    }
    if (R.rt.depth) {
        id<MTLTexture> dt = tex_mtl(R.rt.depth);
        if (dt && dt.width && dt.height) {
            sc_remember_tex(R.rt.depth, dt);
            scenecap_attachment *a = &atts[n];
            memset(a, 0, sizeof *a);
            a->slot = 8;
            a->has_texture = 1;
            a->texture = R.rt.depth->serial;
            a->level = R.rt.has_dview ? R.rt.dview.base_level : 0;
            a->layer = R.rt.has_dview ? R.rt.dview.min_layer : 0;
            a->load = pd.depthAttachment.loadAction == MTLLoadActionClear ? SCENECAP_LOAD_CLEAR
                      : pd.depthAttachment.loadAction == MTLLoadActionLoad ? SCENECAP_LOAD_LOAD
                                                                           : SCENECAP_LOAD_DONTCARE;
            a->store = pd.depthAttachment.storeAction == MTLStoreActionStore ? SCENECAP_STORE_STORE
                                                                             : SCENECAP_STORE_DONTCARE;
            if (a->load == SCENECAP_LOAD_CLEAR) {
                a->clear[0] = (float)pd.depthAttachment.clearDepth;
                a->clear_count = 1;
            }
            n++;
        }
    }
    if (!n) return;
    id<MTLTexture> first = tex_mtl(R.rt.color[0] ? R.rt.color[0] : R.rt.depth);
    scenecap_pass p;
    memset(&p, 0, sizeof p);
    p.kind = SCENECAP_PASS_RENDER;
    p.width = width;
    p.height = height;
    p.pixel_format = sc_pf_name(first ? (uint32_t)first.pixelFormat : 0);
    p.serial = g_pass.serial;
    p.attachments = atts;
    p.attachment_count = n;
    scenecap_pass_begin(g_sc_cap, &p);
    g_sc_in_pass = 1;
}

void scenecap_nvn_pass_end(void) {
    if (g_sc_state == 2 && g_sc_cap && g_sc_in_pass) {
        scenecap_pass_end(g_sc_cap);
        g_sc_in_pass = 0;
    }
}

static void sc_fill_state(scenecap_state *st, uint32_t width, uint32_t height) {
    memset(st, 0, sizeof *st);
    st->blend.enabled = (S.color.blend_en & 0xff) != 0;
    if (st->blend.enabled) {
        st->blend.attachment_count = 8;
        for (int i = 0; i < 8; i++) {
            st->blend.attachments[i].enabled = ((S.color.blend_en >> i) & 1) != 0;
            st->blend.attachments[i].rgb_source = sc_blend_factor(S.blend[i].src_rgb);
            st->blend.attachments[i].rgb_destination = sc_blend_factor(S.blend[i].dst_rgb);
            st->blend.attachments[i].alpha_source = sc_blend_factor(S.blend[i].src_a);
            st->blend.attachments[i].alpha_destination = sc_blend_factor(S.blend[i].dst_a);
            st->blend.attachments[i].rgb_operation = sc_blend_op(S.blend[i].eq_rgb);
            st->blend.attachments[i].alpha_operation = sc_blend_op(S.blend[i].eq_a);
            st->blend.attachments[i].write_mask = (uint8_t)((S.chmask.mask >> (4 * i)) & 0xf);
        }
    }
    st->depth_stencil.depth_test = S.ds.dt != 0;
    st->depth_stencil.depth_write = S.ds.dw != 0;
    st->depth_stencil.depth_compare = sc_compare(S.ds.df);
    st->depth_stencil.stencil_enabled = S.ds.st != 0;
    st->depth_stencil.stencil_front_compare = sc_compare(S.ds.sf_f);
    st->depth_stencil.stencil_back_compare = sc_compare(S.ds.sf_b);
    st->depth_stencil.stencil_read_mask = S.st_vmask[0];
    st->depth_stencil.stencil_write_mask = S.st_wmask[0];
    switch (S.poly.cull) {
    case NVN_FACE_FRONT: st->raster.cull = SCENECAP_CULL_FRONT; break;
    case NVN_FACE_BACK:
    case NVN_FACE_FRONT_AND_BACK: st->raster.cull = SCENECAP_CULL_BACK; break;
    default: st->raster.cull = SCENECAP_CULL_NONE; break;
    }
    switch (S.poly.mode) {
    case NVN_POLYGON_MODE_POINT: st->raster.fill = SCENECAP_FILL_POINTS; break;
    case NVN_POLYGON_MODE_LINE: st->raster.fill = SCENECAP_FILL_LINES; break;
    default: st->raster.fill = SCENECAP_FILL_FILL; break;
    }
    st->raster.frontface = S.poly.front == NVN_FRONT_FACE_CCW ? SCENECAP_FRONT_CCW : SCENECAP_FRONT_CW;
    st->raster.depth_bias = S.po.factor;
    st->raster.depth_slope_scale = S.po.units;
    st->raster.depth_clamp = S.depth_clamp ? 1.0f : 0.0f;
    if (S.has_vp) {
        for (int i = 0; i < 4; i++) st->viewport[i] = S.vp[0][i];
    } else {
        st->viewport[0] = 0;
        st->viewport[1] = 0;
        st->viewport[2] = (float)width;
        st->viewport[3] = (float)height;
    }
    if (R.has_sc) {
        st->scissor[0] = (float)R.sc.x;
        st->scissor[1] = (float)R.sc.y;
        st->scissor[2] = (float)R.sc.w;
        st->scissor[3] = (float)R.sc.h;
    } else {
        st->scissor[0] = 0;
        st->scissor[1] = 0;
        st->scissor[2] = (float)width;
        st->scissor[3] = (float)height;
    }
}

static void sc_fill_bindings(scenecap_binding *b, uint32_t *count) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 16 && S.vbuf[i].address; i++) {
        b[n].kind = SCENECAP_BIND_VERTEX_BUFFER;
        b[n].stage = SCENECAP_STAGE_VERTEX;
        /* Match bind_vertex_streams: stream i is Metal vertex buffer 16+i. Uniforms use 0..15. */
        b[n].index = 16u + i;
        b[n].resource = S.vbuf[i].address;
        b[n].offset = 0;
        b[n].size = S.vbuf[i].size;
        n++;
    }
    for (uint32_t st = 0; st < 6; st++) {
        for (uint32_t r = 0; r < 128; r++) {
            uint64_t h = S.tex[st][r];
            if (!h) continue;
            const tex_t *t = sc_tex_meta(h);
            if (!t) continue;
            id<MTLTexture> mt = tex_mtl(t);
            if (!mt) continue;
            sc_remember_tex(t, mt);
            b[n].kind = SCENECAP_BIND_TEXTURE;
            b[n].stage = st;
            b[n].index = r;
            b[n].resource = t->serial;
            b[n].offset = 0;
            b[n].size = 0;
            n++;
            if (n >= SC_BIND_MAX) break;
        }
        for (uint32_t r = 0; r < 128; r++) {
            if (!S.samp[st][r]) continue;
            b[n].kind = SCENECAP_BIND_SAMPLER;
            b[n].stage = st;
            b[n].index = r;
            b[n].resource = S.samp[st][r] & 0xfffff;
            b[n].offset = 0;
            b[n].size = 0;
            n++;
            if (n >= SC_BIND_MAX) break;
        }
        for (uint32_t r = 0; r < 16; r++) {
            if (!S.ubo[st][r].addr || !S.ubo[st][r].size) continue;
            b[n].kind = SCENECAP_BIND_UNIFORM_BUFFER;
            b[n].stage = st;
            b[n].index = r;
            b[n].resource = S.ubo[st][r].addr;
            b[n].offset = 0;
            b[n].size = S.ubo[st][r].size;
            n++;
        }
        for (uint32_t r = 0; r < 16; r++) {
            if (!S.ssbo[st][r].addr || !S.ssbo[st][r].size) continue;
            b[n].kind = SCENECAP_BIND_STORAGE_BUFFER;
            b[n].stage = st;
            b[n].index = r;
            b[n].resource = S.ssbo[st][r].addr;
            b[n].offset = 0;
            b[n].size = S.ssbo[st][r].size;
            n++;
        }
        if (n >= SC_BIND_MAX) break;
    }
    *count = n;
}

/* Filled by scenecap_nvn_set_draw_resolve() immediately before scenecap_nvn_draw(). */
static scenecap_draw_resolve g_sc_resolve;

void scenecap_nvn_set_draw_resolve(const scenecap_draw_resolve *resolve) {
    if (resolve) g_sc_resolve = *resolve;
    else memset(&g_sc_resolve, 0, sizeof g_sc_resolve);
}

void scenecap_nvn_draw(const void *draw, const void *indexed_draw) {
    if (!sc_ready()) return;
    const p_draw *dw = (const p_draw *)draw;
    const p_draw_idx *di = (const p_draw_idx *)indexed_draw;
    if (!dw && !di) return;
    scenecap_draw_resolve resolve = g_sc_resolve;
    memset(&g_sc_resolve, 0, sizeof g_sc_resolve); /* one-shot */
    int have_resolve = resolve.active && resolve.index_buffer && resolve.index_count > 0;
    /* A GPU-indirect consumer draw of the geometry-stage compaction carries the CPU-visible placeholder
     * record: ibuf is never set and the real count lives behind the port-side compaction kernel.
     * When the port resolved the draw (host survivor list + count), record that instead so offline
     * replay can iterate body/cloth colour without re-running the game. */
    if (di && !di->ibuf && !have_resolve) return;
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil;
    if (!pr) return;
    NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!g_sc_in_pass) return;
    if (vs) sc_remember_shader(vs);
    if (fs) sc_remember_shader(fs);
    static scenecap_binding binds[SC_BIND_MAX];
    uint32_t nb = 0;
    sc_fill_bindings(binds, &nb);
    if (have_resolve && resolve.poscap && nb < SC_BIND_MAX) {
        binds[nb].kind = SCENECAP_BIND_VERTEX_BUFFER;
        binds[nb].stage = SCENECAP_STAGE_VERTEX;
        binds[nb].index = 28; /* matches NIER_BODY_POSCAP buffer(28) */
        binds[nb].resource = resolve.poscap;
        binds[nb].offset = 0;
        binds[nb].size = resolve.poscap_bytes ? resolve.poscap_bytes : (64ull * 1024ull);
        binds[nb].data = resolve.poscap_data;
        nb++;
    }
    scenecap_state st;
    uint32_t w = g_pass.tw ? (uint32_t)g_pass.tw : 1, h = g_pass.th ? (uint32_t)g_pass.th : 1;
    sc_fill_state(&st, w, h);
    scenecap_pipeline pipe;
    memset(&pipe, 0, sizeof pipe);
    pipe.vertex_shader = vs ? vs->key : "vs-none";
    pipe.fragment_shader = fs ? fs->key : "fs-none";
    pipe.state = st;
    /* Vertex input: Metal needs a vertex descriptor whenever the MSL vertex function declares attributes.
     * The port builds the same descriptor from the same two tables (see the pipeline builder in nvn_mtl.m):
     * attribute a of stream st lands in Metal buffer 16 + st, and streams with no stride become constants. */
    pipe.attribute_count = 0;
    pipe.layout_count = 0;
    if (vs) {
        const nvn_refl *rf = &vs->rf;
        for (int v = 0; v < rf->nv && pipe.attribute_count < SCENECAP_MAX_VERTEX_ATTRIBUTES; v++) {
            int loc = rf->v[v].loc, a = rf->v[v].nvn;
            if (loc < 0 || loc >= 31) continue;
            MTLVertexFormat vf = (a >= 0 && a < 16) ? vfmt(S.attr[a].fmt) : MTLVertexFormatInvalid;
            scenecap_vertex_attribute *at = &pipe.attributes[pipe.attribute_count++];
            at->location = (uint32_t)loc;
            if (vf == MTLVertexFormatInvalid) {
                at->format = (uint32_t)MTLVertexFormatFloat4;
                at->offset = 0;
                at->buffer_index = 30;
            } else {
                at->format = (uint32_t)vf;
                at->offset = (uint32_t)S.attr[a].offset;
                at->buffer_index = 16u + (uint32_t)S.attr[a].stream;
            }
        }
    }
    static int sc_attr_logged;
    if (!sc_attr_logged) {
        sc_attr_logged = 1;
        fprintf(stderr, "[scenecap] vertex input: nv=%d attributes=%u layouts=%u\n", vs ? vs->rf.nv : -1,
                pipe.attribute_count, pipe.layout_count);
    }
    for (uint32_t st = 0; st < 16 && pipe.layout_count < SCENECAP_MAX_VERTEX_LAYOUTS; st++) {
        scenecap_vertex_layout *ly = &pipe.layouts[pipe.layout_count++];
        uint64_t stride = S.stream[st].stride;
        ly->buffer_index = 16u + st;
        ly->stride = (uint32_t)stride;
        ly->step_function = stride == 0 ? (uint32_t)MTLVertexStepFunctionConstant
                                       : (S.stream[st].divisor ? (uint32_t)MTLVertexStepFunctionPerInstance
                                                               : (uint32_t)MTLVertexStepFunctionPerVertex);
        ly->step_rate = stride == 0 ? 0 : (S.stream[st].divisor ? S.stream[st].divisor : 1);
    }
    scenecap_draw_desc d;
    memset(&d, 0, sizeof d);
    d.pipeline = &pipe;
    d.vertex_start = dw ? (uint32_t)dw->first : 0;
    d.vertex_count = dw ? (uint32_t)dw->count : 0;
    d.index_type = di ? sc_index_type(di->itype) : SCENECAP_INDEX_NONE;
    /* index_start is the first index in the buffer (byte offset / stride); base_vertex is applied by Metal
       as a bias on every fetched index. Older packages wrongly stored base_vertex here — those draws with
       a non-zero base vertex mis-addressed the index buffer on replay. */
    d.index_start = 0;
    d.index_count = di ? (uint32_t)di->count : 0;
    d.index_buffer = di ? di->ibuf : 0;
    d.index_bytes = di ? (uint64_t)di->count * (di->itype == 1 ? 2u : di->itype == 0 ? 1u : 4u) : 0;
    d.base_vertex = di && di->base_vertex > 0 ? (uint32_t)di->base_vertex : 0;
    if (have_resolve) {
        /* Prefer the port-resolved host list so the package matches what Metal actually draws. */
        d.vertex_start = 0;
        d.vertex_count = 0;
        d.index_type = resolve.index_type == 1 ? SCENECAP_INDEX_UINT16
                      : resolve.index_type == 0 ? SCENECAP_INDEX_UINT8
                                                : SCENECAP_INDEX_UINT32;
        d.index_start = 0;
        d.index_count = resolve.index_count;
        d.index_buffer = resolve.index_buffer;
        /* Snapshot only the live indexCount × stride — not the full list capacity. */
        uint32_t stride = resolve.index_type == 1 ? 2u : resolve.index_type == 0 ? 1u : 4u;
        uint64_t need = (uint64_t)resolve.index_count * stride;
        d.index_bytes = need;
        d.index_data = resolve.index_data; /* Metal-only list_cpu when guest pool read would fail */
        if (!d.index_data && resolve.index_bytes && resolve.index_bytes < need)
            d.index_bytes = resolve.index_bytes;
        d.base_vertex = 0;
        static uint64_t rlog;
        if (rlog++ < 12 || !(rlog % 200))
            sc_log("resolved GS consumer: nidx=%u ib=%#llx cpu=%d poscap=%#llx vs=%.8s fs=%.8s",
                   resolve.index_count, (unsigned long long)resolve.index_buffer,
                   resolve.index_data != NULL, (unsigned long long)resolve.poscap,
                   vs ? vs->key : "?", fs ? fs->key : "?");
    }
    d.instances = (dw ? dw->instances : di ? di->instances : 0);
    if (d.instances == 0) d.instances = 1;
    d.primitive = sc_primitive(dw ? (uint32_t)dw->prim : di ? (uint32_t)di->prim : 4);
    d.bindings = binds;
    d.binding_count = nb;
    d.label = NULL;
    scenecap_draw(g_sc_cap, &d);
}

void scenecap_nvn_dispatch(const void *dispatch_ptr) {
    if (!sc_ready()) return;
    scenecap_nvn_pass_end(); /* the replay layer closes the render encoder right after this hook */
    const p_dispatch *dp = (const p_dispatch *)dispatch_ptr;
    if (!dp) return;
    __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil;
    if (!pr) return;
    NvShader *cs = pr->sh[5];
    if (!cs) return;
    sc_remember_shader(cs);
    static scenecap_binding binds[SC_BIND_MAX];
    uint32_t nb = 0;
    sc_fill_bindings(binds, &nb);
    scenecap_state st;
    sc_fill_state(&st, 1, 1);
    scenecap_pipeline pipe;
    memset(&pipe, 0, sizeof pipe);
    pipe.kernel_shader = cs->key;
    pipe.state = st;
    uint32_t grid[3] = {(uint32_t)(dp->x > 0 ? dp->x : 1), (uint32_t)(dp->y > 0 ? dp->y : 1),
                        (uint32_t)(dp->z > 0 ? dp->z : 1)};
    scenecap_dispatch(g_sc_cap, &pipe, binds, nb, grid, NULL);
}

void scenecap_nvn_request(const char *path) {
    pthread_mutex_lock(&g_mu);
    snprintf(g_sc_request, sizeof g_sc_request, "%s", path ? path : "");
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_sc_want_frames, 0);
    atomic_store(&g_sc_requested, 1);
    MLOG("scenecap: capture requested for the next frame (%s)", path && *path ? path : "environment path");
}

/* The diagnostic input file ("scenecap <path>") calls this; the request is picked up at the next frame
 * boundary, on the replay thread, where the capture object may be created safely. */
void dm_gfx_scenecap_next(const char *path) { scenecap_nvn_request(path); }

/* 场景录制：从下一个帧边界开始抓连续 frames 帧，抓满后写出包。路径语义与 scenecap_nvn_request() 相同。 */
void scenecap_nvn_record(const char *path, uint32_t frames) {
    pthread_mutex_lock(&g_mu);
    snprintf(g_sc_request, sizeof g_sc_request, "%s", path ? path : "");
    pthread_mutex_unlock(&g_mu);
    atomic_store(&g_sc_want_frames, frames ? frames : 1);
    atomic_store(&g_sc_requested, 1);
    MLOG("scenecap: recording %u frame(s) (%s)", frames ? frames : 1,
         path && *path ? path : "environment path");
}
void dm_gfx_scenecap_record(const char *path, unsigned frames) { scenecap_nvn_record(path, frames); }

void scenecap_nvn_shutdown(void) {
    if (g_sc_cap) {
        char err[256];
        scenecap_finish(g_sc_cap, err, sizeof err);
        scenecap_destroy(g_sc_cap);
        g_sc_cap = NULL;
    }
    g_sc_state = 1;
}
