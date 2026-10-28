// OpenGL → Metal layer for games that render with OpenGL through EGL (Digimon Story Cyber Sleuth: Complete Edition: Media.Vision's MVGL engine on NVIDIA's desktop GL 4.5 core
// driver, subsdk0, which the host does not load; Balatro: LÖVE 11.4 on SDL, the same driver). main imports the EGL functions, which bind to the ones here through the host's import
// table (hle.h swhle). GL functions use that same table for direct imports and eglGetProcAddress (a glad loader asks for 2,956 names; Digimon's code calls 79 of them,
// LÖVE's about 150, counted with dm/tools/loader_census.py). Only those are implemented; every other loader name gets a null pointer (DM_GL_STUBS=1: a stub that reports its first call).
// LÖVE takes the core profile's paths (GL_CONTEXT_PROFILE_MASK): its vertex streams are persistently mapped buffers (glBufferStorage, glMapBufferRange) that fences
// (glFenceSync / glClientWaitSync) keep the CPU from rewriting while a frame reads them; with a compatibility profile it would draw from client memory, which Metal cannot.
// Model:
//   · one context, used from the thread that made it current; GL calls are encoded into Metal as they come: one MTLCommandBuffer per frame on this layer's own queue,
//     committed at eglSwapBuffers (signalling g_ev with its serial for the present path), a render encoder per stretch of draws into one framebuffer;
//   · rendering keeps GL's orientation: images are stored bottom row first, viewports are flipped (negative height), the winding is reversed, and the present flips the
//     window image (the NVN layer's lower-left origin, mtl_set_origin_lower_left); scissor rectangles and gl_FragCoord then need no change;
//   · the window surface is three textures registered with the NVN layer's present path (mtl_ext_tex), as the Vulkan layer's swapchain, plus a depth / stencil texture;
//   · buffers are shared MTLBuffers; glBufferData on a buffer the GPU may still read gets a new one (orphaning, as GL drivers do);
//   · textures are created on first use from the levels given so far (the game defines a texture level by level, and Metal needs the level count up front); an upload into a
//     texture the GPU may still read goes through a blit in command order, any other is a CPU write;
//   · shaders come from the offline library built by dm/gfx/tools/gl_shaders.py (<DM_MSL_DIR>/gl/): a shader is found by the hash of its source and its stage; its table row lists the
//     vertex inputs, the members of its default uniform block (glUniform* writes them into CPU copies that are set as bytes at each draw that needs them) and its samplers.
//     glGetAttribLocation returns the Metal attribute index of the input, so the game's attribute index is the Metal one; vertex buffers are at slot 30 − index.
// Environment: DM_GL_LOG=1 logs object creation; DM_GL_CAPTURE=<dir> writes every shader source the game compiles (<key>.vert / <key>.frag, read by gl_shaders.py).
#include "../../host/hle.h"
#include "../nvn/nvn_mtl.h"
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>

int mtl_fps_target(void);
int mtl_link_active(void); int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns);   // nvn_mtl.m
void dm_vi_layer_size(int *w, int *h);   // hle_vi.c: the size of the last sized vi::CreateLayer

#define GLOG(...) LOG("[gl] " __VA_ARGS__)
#define RED1(...) do { static int once_; if (!once_) { once_ = 1; LOG("[gl] [RED] " __VA_ARGS__); } } while (0)
static int g_log;
#define OLOG(...) do { if (g_log) GLOG(__VA_ARGS__); } while (0)

typedef uint32_t GLenum, GLuint, GLbitfield; typedef int32_t GLint, GLsizei; typedef float GLfloat; typedef double GLdouble; typedef char GLchar; typedef intptr_t GLsizeiptr, GLintptr;
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface; typedef int32_t EGLint; typedef uint32_t EGLBoolean, EGLenum;

// GL: one GL function, registered in the host's import table for both direct imports and eglGetProcAddress. GLB: the same, run on the thread's big host stack
// (hle_bigstack.c, register arguments only) for the functions that call into Metal: a game thread's stack can be small. EGL uses the same import table and big-stack entry.
// GLboolean and other arguments narrower than 32 bits are taken as uint32_t and truncated here (hle.h: Switch callers do not extend them)
#define GL(name, ret, ...)                                                                       \
    static ret h_##name(__VA_ARGS__);                                                            \
    __attribute__((used, section("__DATA,swhle"))) static const hle_ent e_##name = {#name, (void *)h_##name}; \
    static ret h_##name(__VA_ARGS__)
#define BIGSTUB(fn)                                                                              \
    extern const char fn##_bs[];                                                                 \
    __asm__(".text\n.p2align 2\n.globl _" #fn "_bs\n.private_extern _" #fn "_bs\n_" #fn "_bs:\n" \
            "    adrp x16, _" #fn "@PAGE\n    add x16, x16, _" #fn "@PAGEOFF\n    b _dm_bigstack_enter\n")
#define GLB(name, ret, ...)                                                                      \
    static ret h_##name(__VA_ARGS__) __attribute__((used));                                      \
    BIGSTUB(h_##name);                                                                           \
    __attribute__((used, section("__DATA,swhle"))) static const hle_ent e_##name = {#name, (void *)h_##name##_bs}; \
    static ret h_##name(__VA_ARGS__)
#define EGL(name, ret, ...)                                                                      \
    static ret h_##name(__VA_ARGS__) __attribute__((used));                                      \
    BIGSTUB(h_##name);                                                                           \
    __attribute__((used, section("__DATA,swhle"))) static const hle_ent e_##name = {#name, (void *)h_##name##_bs}; \
    static ret h_##name(__VA_ARGS__)

// ---------- GL enums ----------
enum {
    GL_POINTS = 0, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS,
    GL_NEVER = 0x200, GL_SRC_COLOR = 0x300, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR,
    GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE,
    GL_FRONT = 0x404, GL_BACK = 0x405, GL_FRONT_AND_BACK = 0x408,
    GL_CULL_FACE = 0xb44, GL_DEPTH_TEST = 0xb71, GL_STENCIL_TEST = 0xb90, GL_VIEWPORT = 0xba2, GL_BLEND = 0xbe2, GL_SCISSOR_TEST = 0xc11,
    GL_UNPACK_ROW_LENGTH = 0xcf2, GL_UNPACK_ALIGNMENT = 0xcf5, GL_MAX_TEXTURE_SIZE = 0xd33, GL_INDEX_SHIFT = 0xd12, GL_INDEX_OFFSET = 0xd13,
    GL_TEXTURE_2D = 0xde1, GL_TEXTURE_BORDER_COLOR = 0x1004,
    GL_BYTE = 0x1400, GL_UNSIGNED_BYTE, GL_SHORT, GL_UNSIGNED_SHORT, GL_INT, GL_UNSIGNED_INT, GL_FLOAT, GL_HALF_FLOAT = 0x140b,
    GL_INVERT = 0x150a, GL_COLOR_INDEX = 0x1900, GL_STENCIL_INDEX = 0x1901, GL_DEPTH_COMPONENT = 0x1902, GL_RED = 0x1903, GL_GREEN, GL_BLUE, GL_ALPHA, GL_RGB, GL_RGBA,
    GL_LUMINANCE, GL_LUMINANCE_ALPHA,
    GL_KEEP = 0x1e00, GL_REPLACE, GL_INCR, GL_DECR, GL_VENDOR = 0x1f00, GL_RENDERER, GL_VERSION, GL_EXTENSIONS,
    GL_NEAREST = 0x2600, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST = 0x2700, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_CLAMP = 0x2900, GL_REPEAT,
    GL_FUNC_ADD = 0x8006, GL_MIN, GL_MAX, GL_FUNC_SUBTRACT = 0x800a, GL_FUNC_REVERSE_SUBTRACT,
    GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1, GL_POLYGON_OFFSET_FILL = 0x8037,
    GL_RGB8 = 0x8051, GL_RGBA4 = 0x8056, GL_RGB5_A1, GL_RGBA8, GL_RGB10_A2, GL_RGBA16 = 0x805b,
    GL_TEXTURE_WRAP_R = 0x8072, GL_COLOR_TABLE = 0x80d0, GL_BGRA = 0x80e1,
    GL_CLAMP_TO_BORDER = 0x812d, GL_CLAMP_TO_EDGE = 0x812f, GL_TEXTURE_MAX_LEVEL = 0x813d,
    GL_DEPTH_COMPONENT16 = 0x81a5, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT32,
    GL_MAJOR_VERSION = 0x821b, GL_MINOR_VERSION, GL_NUM_EXTENSIONS, GL_RG = 0x8227, GL_R8 = 0x8229, GL_RG8 = 0x822b, GL_R16F = 0x822d, GL_R32F, GL_RG16F, GL_RG32F,
    GL_UNSIGNED_SHORT_5_6_5 = 0x8363, GL_MIRRORED_REPEAT = 0x8370,
    GL_COMPRESSED_RGB_S3TC_DXT1 = 0x83f0, GL_COMPRESSED_RGBA_S3TC_DXT1, GL_COMPRESSED_RGBA_S3TC_DXT3, GL_COMPRESSED_RGBA_S3TC_DXT5,
    GL_TEXTURE0 = 0x84c0, GL_MAX_RENDERBUFFER_SIZE = 0x84e8, GL_DEPTH_STENCIL = 0x84f9, GL_UNSIGNED_INT_24_8 = 0x84fa, GL_MAX_TEXTURE_LOD_BIAS = 0x84fd,
    GL_TEXTURE_MAX_ANISOTROPY = 0x84fe, GL_MAX_TEXTURE_MAX_ANISOTROPY, GL_TEXTURE_LOD_BIAS = 0x8501,
    GL_INCR_WRAP = 0x8507, GL_DECR_WRAP, GL_TEXTURE_CUBE_MAP = 0x8513, GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z = 0x851a,
    GL_MAX_VERTEX_ATTRIBS = 0x8869, GL_MAX_TEXTURE_IMAGE_UNITS = 0x8872, GL_RGBA32F = 0x8814, GL_RGB32F, GL_RGBA16F = 0x881a, GL_RGB16F,
    GL_TEXTURE_COMPARE_MODE = 0x884c, GL_TEXTURE_COMPARE_FUNC,
    GL_ARRAY_BUFFER = 0x8892, GL_ELEMENT_ARRAY_BUFFER, GL_DEPTH24_STENCIL8 = 0x88f0,
    GL_FRAGMENT_SHADER = 0x8b30, GL_VERTEX_SHADER, GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS = 0x8b4d, GL_SHADER_TYPE = 0x8b4f, GL_DELETE_STATUS = 0x8b80, GL_COMPILE_STATUS,
    GL_LINK_STATUS, GL_VALIDATE_STATUS, GL_INFO_LOG_LENGTH, GL_ATTACHED_SHADERS, GL_ACTIVE_UNIFORMS, GL_ACTIVE_UNIFORM_MAX_LENGTH, GL_SHADER_SOURCE_LENGTH,
    GL_ACTIVE_ATTRIBUTES, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, GL_SHADING_LANGUAGE_VERSION = 0x8b8c, GL_CURRENT_PROGRAM,
    GL_R11F_G11F_B10F = 0x8c3a, GL_SRGB8_ALPHA8 = 0x8c43, GL_COMPRESSED_SRGB_S3TC_DXT1 = 0x8c4c, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3,
    GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5, GL_DEPTH_COMPONENT32F = 0x8cac, GL_DEPTH32F_STENCIL8,
    GL_FRAMEBUFFER_BINDING = 0x8ca6, GL_RENDERBUFFER_BINDING, GL_COLOR_ATTACHMENT0 = 0x8ce0, GL_DEPTH_ATTACHMENT = 0x8d00, GL_STENCIL_ATTACHMENT = 0x8d20,
    GL_FRAMEBUFFER = 0x8d40, GL_RENDERBUFFER, GL_STENCIL_INDEX8 = 0x8d48, GL_RGB565 = 0x8d62, GL_MAX_SAMPLES = 0x8d57,
    GL_COMPRESSED_RED_RGTC1 = 0x8dbb, GL_COMPRESSED_SIGNED_RED_RGTC1, GL_COMPRESSED_RG_RGTC2, GL_COMPRESSED_SIGNED_RG_RGTC2,
    GL_MAX_VERTEX_UNIFORM_VECTORS = 0x8dfb, GL_MAX_VARYING_VECTORS, GL_MAX_FRAGMENT_UNIFORM_VECTORS,
    GL_COMPRESSED_RGBA_BPTC_UNORM = 0x8e8c, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM, GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT, GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT,
    GL_COMPRESSED_RGBA_ASTC_4x4 = 0x93b0, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4 = 0x93d0,
    GL_DEPTH_STENCIL_ATTACHMENT = 0x821a,
    GL_CW = 0x900, GL_CCW, GL_POINT_SIZE = 0xb11, GL_POINT_SIZE_RANGE, GL_CULL_FACE_MODE = 0xb45, GL_FRONT_FACE, GL_SCISSOR_BOX = 0xc10, GL_PACK_ALIGNMENT = 0xd05,
    GL_MAX_VIEWPORT_DIMS = 0xd3a, GL_COLOR = 0x1800, GL_DEPTH, GL_STENCIL, GL_ZERO = 0, GL_ONE = 1,
    GL_TEXTURE_BINDING_3D = 0x806a, GL_TEXTURE_3D = 0x806f, GL_MAX_3D_TEXTURE_SIZE = 0x8073, GL_SAMPLE_BUFFERS = 0x80a8, GL_SAMPLES, GL_CONTEXT_FLAGS = 0x821e,
    GL_ALIASED_POINT_SIZE_RANGE = 0x846d, GL_TEXTURE_BINDING_CUBE_MAP = 0x8514, GL_MAX_CUBE_MAP_TEXTURE_SIZE = 0x851c, GL_MAX_DRAW_BUFFERS = 0x8824, GL_MAX_ARRAY_TEXTURE_LAYERS = 0x88ff,
    GL_FLOAT_VEC2 = 0x8b50, GL_FLOAT_VEC3, GL_FLOAT_VEC4, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4, GL_BOOL, GL_FLOAT_MAT2 = 0x8b5a, GL_FLOAT_MAT3, GL_FLOAT_MAT4, GL_SAMPLER_2D = 0x8b5e,
    GL_SAMPLER_3D, GL_SAMPLER_CUBE, GL_SAMPLER_2D_SHADOW = 0x8b62, GL_TEXTURE_2D_ARRAY = 0x8c1a, GL_TEXTURE_BINDING_2D_ARRAY = 0x8c1d,
    GL_READ_FRAMEBUFFER = 0x8ca8, GL_DRAW_FRAMEBUFFER, GL_READ_FRAMEBUFFER_BINDING, GL_RENDERBUFFER_SAMPLES, GL_FRAMEBUFFER_COMPLETE = 0x8cd5, GL_MAX_COLOR_ATTACHMENTS = 0x8cdf,
    GL_RENDERBUFFER_WIDTH = 0x8d42, GL_RENDERBUFFER_HEIGHT, GL_RENDERBUFFER_INTERNAL_FORMAT, GL_FRAMEBUFFER_SRGB = 0x8db9, GL_SAMPLER_2D_ARRAY = 0x8dc1,
    GL_UNSIGNED_INT_VEC2 = 0x8dc6, GL_UNSIGNED_INT_VEC3, GL_UNSIGNED_INT_VEC4, GL_TEXTURE_SWIZZLE_R = 0x8e42, GL_TEXTURE_SWIZZLE_A = 0x8e45,
    GL_ALREADY_SIGNALED = 0x911a, GL_TIMEOUT_EXPIRED, GL_CONDITION_SATISFIED, GL_CONTEXT_PROFILE_MASK = 0x9126,
};
#define GL_MAP_INVALIDATE_BUFFER_BIT 0x8
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x1
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x100
#define GL_STENCIL_BUFFER_BIT 0x400

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
static void *zalloc(size_t n) { void *p = calloc(1, n); if (!p) die("[gl] out of memory %zu", n); return p; }

// ---------- Device-wide state ----------
static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_q;
static id<MTLSharedEvent> g_ev;                  // signalled with a command buffer's serial at its end
static id<MTLTexture> g_tex0, g_cube0, g_tex3d0, g_arr0;   // bound where a sampler's unit has no texture (or an unusable one): opaque black, as GL samples an incomplete texture
static id<MTLSamplerState> g_samp0;
static id<MTLLibrary> g_lib, g_util;             // the game's shaders; the layer's own (clears)
static uint64_t g_serial = 1;                    // serial of the command buffer being recorded (g_cb); committed ones have lower serials
static _Atomic uint64_t g_done;                  // highest serial whose command buffer has completed
static id<MTLCommandBuffer> g_cb;
static id<MTLRenderCommandEncoder> g_enc;
static _Atomic uint64_t g_n_draw, g_n_pass, g_n_up, g_n_blit, g_n_pso, g_n_cb;

// ---------- Shader table (<DM_MSL_DIR>/gl/gl_shaders.tsv) ----------
enum { T_FLOAT, T_VEC2, T_VEC3, T_VEC4, T_INT, T_IVEC2, T_IVEC3, T_IVEC4, T_UINT, T_UVEC2, T_UVEC3, T_UVEC4, T_BOOL, T_MAT2, T_MAT3, T_MAT4,
       T_SAMP2D, T_SAMPCUBE, T_SAMP3D, T_SAMP2DARR, T_SAMP2DSH, T_OTHER };
static const char *const g_tnames[] = {"float", "vec2", "vec3", "vec4", "int", "ivec2", "ivec3", "ivec4", "uint", "uvec2", "uvec3", "uvec4", "bool", "mat2", "mat3", "mat4",
                                       "sampler2D", "samplerCube", "sampler3D", "sampler2DArray", "sampler2DShadow"};
static const GLenum g_tenums[] = {GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4, GL_INT, GL_INT_VEC2, GL_INT_VEC3, GL_INT_VEC4, GL_UNSIGNED_INT, GL_UNSIGNED_INT_VEC2,
                                  GL_UNSIGNED_INT_VEC3, GL_UNSIGNED_INT_VEC4, GL_BOOL, GL_FLOAT_MAT2, GL_FLOAT_MAT3, GL_FLOAT_MAT4, GL_SAMPLER_2D, GL_SAMPLER_CUBE, GL_SAMPLER_3D,
                                  GL_SAMPLER_2D_ARRAY, GL_SAMPLER_2D_SHADOW, GL_FLOAT};   // glGetActiveUniform's types
static int tcode(const char *s) { for (int i = 0; i < T_OTHER; i++) if (!strcmp(s, g_tnames[i])) return i; return T_OTHER; }
static int tcomps(int t) { static const int c[] = {1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 1, 4, 9, 16}; return t < T_SAMP2D ? c[t] : 1; }
static int is_samp(int t) { return t >= T_SAMP2D && t < T_OTHER; }
static int is_intt(int t) { return t >= T_INT && t <= T_BOOL; }
typedef struct { char name[48]; uint8_t slot, type; } shattr;                         // a vertex input
typedef struct { char name[48]; uint16_t off, count, stride; uint8_t type; } shuni;   // a default block member (count 0 = not an array)
typedef struct { char name[48]; uint8_t slot, type; } shsamp;                         // a combined sampler: texture and sampler slot
typedef struct { char key[17]; int frag, ub_slot, ub_size, na, nu, nt; shattr *a; shuni *u; shsamp *t; } shinfo;
static shinfo *g_sh; static int g_nsh;
static int sh_cmp(const void *a, const void *b) { int c = memcmp(((const shinfo *)a)->key, ((const shinfo *)b)->key, 16); return c ? c : ((const shinfo *)a)->frag - ((const shinfo *)b)->frag; }
static void sh_load(void) {
    const char *d = getenv("DM_MSL_DIR"); char p[1024];
    if (!d) { GLOG("[RED] DM_MSL_DIR not set: the GL shader library cannot be found"); return; }
    snprintf(p, sizeof p, "%s/gl/gl_shaders.tsv", d);
    FILE *f = fopen(p, "r");
    if (!f) { GLOG("[RED] cannot open %s (run dm/gfx/tools/gl_shaders.py first)", p); return; }
    static char line[65536]; int cap = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *rest = line, *key = strsep(&rest, "\t"), *stage = strsep(&rest, "\t"), *res = strsep(&rest, "\t\n");
        if (!key || !stage || strlen(key) != 16) continue;
        if (g_nsh == cap) { cap = cap ? cap * 2 : 1024; g_sh = realloc(g_sh, cap * sizeof *g_sh); }
        shinfo *s = &g_sh[g_nsh++]; memset(s, 0, sizeof *s);
        memcpy(s->key, key, 16); s->frag = !strcmp(stage, "frag"); s->ub_slot = -1;
        int na = 0, nu = 0, nt = 0;
        for (char *c = res; c && *c; c++) if (c == res || c[-1] == ' ') { na += *c == 'a'; nu += *c == 'u'; nt += *c == 't'; }
        s->a = zalloc((na + 1) * sizeof *s->a); s->u = zalloc((nu + 1) * sizeof *s->u); s->t = zalloc((nt + 1) * sizeof *s->t);
        for (char *t = res ? strtok(res, " ") : NULL; t; t = strtok(NULL, " ")) {
            char nm[64], ty[32]; unsigned a1, a2, a3;
            if (t[0] == 'a' && sscanf(t, "a:%63[^=]=%u:%31s", nm, &a1, ty) == 3 && a1 < 16) { shattr *x = &s->a[s->na++]; strlcpy(x->name, nm, sizeof x->name); x->slot = a1; x->type = tcode(ty); }
            else if (t[0] == 'u' && sscanf(t, "u:%63[^=]=%u:%31[^:]:%u:%u", nm, &a1, ty, &a2, &a3) == 5) {
                shuni *x = &s->u[s->nu++]; strlcpy(x->name, nm, sizeof x->name); x->off = a1; x->type = tcode(ty); x->count = a2; x->stride = a3;
            } else if (t[0] == 't' && sscanf(t, "t:%63[^=]=%u:%31s", nm, &a1, ty) == 3) { shsamp *x = &s->t[s->nt++]; strlcpy(x->name, nm, sizeof x->name); x->slot = a1; x->type = tcode(ty); }
            else if (t[0] == 'b' && sscanf(t, "b:%u:%u", &a1, &a2) == 2) { s->ub_slot = a1; s->ub_size = a2; }
        }
        if (s->ub_slot >= 15) GLOG("[RED] shader %s: uniform block at buffer slot %d meets the vertex buffer slots", s->key, s->ub_slot);
    }
    fclose(f);
    qsort(g_sh, g_nsh, sizeof *g_sh, sh_cmp);
    snprintf(p, sizeof p, "%s/gl/gl_shaders.metallib", d);
    NSError *err = nil;
    g_lib = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:@(p)] error:&err];
    if (!g_lib) GLOG("[RED] shader library failed to load %s: %s", p, err.localizedDescription.UTF8String);
}
static const shinfo *sh_find(const char *key, int frag) { shinfo k; memcpy(k.key, key, 16); k.frag = frag; return g_nsh ? bsearch(&k, g_sh, g_nsh, sizeof *g_sh, sh_cmp) : NULL; }

// The layer's own shaders: a quad that clears what a scissored or masked glClear covers (a render pass can only clear its attachments whole)
static const char *const g_util_src =
    "#include <metal_stdlib>\nusing namespace metal;\n"
    "struct V { float4 p [[position]]; };\n"
    "vertex V clr_vs(uint i [[vertex_id]], constant float &z [[buffer(0)]]) { V o; float2 t = float2((i << 1) & 2, i & 2); o.p = float4(t * 2 - 1, z, 1); return o; }\n"
    "fragment float4 clr_fs(constant float4 &c [[buffer(0)]]) { return c; }\n"
    "fragment void clr_fs_none() {}\n";

static void dev_init_once(void) {
    @autoreleasepool {
        g_log = getenv("DM_GL_LOG") && atoi(getenv("DM_GL_LOG"));
        g_dev = (__bridge id<MTLDevice>)mtl_device_classic();
        g_q = [g_dev newCommandQueueWithMaxCommandBufferCount:64]; g_q.label = @"gl";
        g_ev = [g_dev newSharedEvent];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        const uint32_t black = 0xff000000u, blk6[6] = {black, black, black, black, black, black};
        g_tex0 = [g_dev newTextureWithDescriptor:td];
        [g_tex0 replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&black bytesPerRow:4];
        td = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm size:1 mipmapped:NO];
        g_cube0 = [g_dev newTextureWithDescriptor:td];
        for (int f = 0; f < 6; f++) [g_cube0 replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:f withBytes:&blk6[f] bytesPerRow:4 bytesPerImage:4];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        td.textureType = MTLTextureType3D; g_tex3d0 = [g_dev newTextureWithDescriptor:td];
        [g_tex3d0 replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:&black bytesPerRow:4 bytesPerImage:4];
        td.textureType = MTLTextureType2DArray; g_arr0 = [g_dev newTextureWithDescriptor:td];
        [g_arr0 replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:0 withBytes:&black bytesPerRow:4 bytesPerImage:0];
        g_samp0 = [g_dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
        NSError *err = nil;
        g_util = [g_dev newLibraryWithSource:@(g_util_src) options:nil error:&err];
        if (!g_util) GLOG("[RED] the layer's own shaders do not compile: %s", err.localizedDescription.UTF8String);
        sh_load();
        mtl_set_origin_lower_left(1);   // the window image is stored bottom row first; the present flips it
        GLOG("Metal device %s; %d shader table entries", g_dev.name.UTF8String, g_nsh);
    }
}
static void dev_init(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, dev_init_once); }

// ---------- Command buffers ----------
// Transient memory (converted indices, staging for blits, large uniform blocks): bump allocation in 4 MB shared buffers, each reused once the command buffer that last used it completed
typedef struct { __strong id<MTLBuffer> buf; size_t used; uint64_t serial; } arena_t;
static arena_t g_arena[64]; static int g_narena, g_cur_arena = -1;
static id<MTLBuffer> arena_alloc(size_t n, size_t *off) {
    n = (n + 255) & ~(size_t)255;
    if (g_cur_arena >= 0) { arena_t *a = &g_arena[g_cur_arena]; if (a->used + n <= a->buf.length) { *off = a->used; a->used += n; a->serial = g_serial; return a->buf; } }
    size_t sz = umax(4u << 20, (uint32_t)n);
    for (int i = 0; i < g_narena; i++) {
        arena_t *a = &g_arena[i];
        if (i != g_cur_arena && a->serial <= atomic_load(&g_done) && a->buf.length >= sz) { g_cur_arena = i; a->used = n; a->serial = g_serial; *off = 0; return a->buf; }
    }
    int k = -1;
    if (g_narena < 64) k = g_narena++;
    else for (int i = 0; i < 64; i++) if (g_arena[i].serial <= atomic_load(&g_done)) { k = i; break; }
    if (k < 0) { RED1("transient memory: all 64 blocks in flight"); k = (g_cur_arena + 1) % 64; }
    g_arena[k].buf = [g_dev newBufferWithLength:sz options:MTLResourceStorageModeShared]; g_arena[k].used = n; g_arena[k].serial = g_serial;
    g_cur_arena = k; *off = 0; return g_arena[k].buf;
}
// The last command buffer committed (frame or upload): the GPU runs them in order, so waiting for it waits for all (dm_gl_drain, when the app goes to the background)
static id<MTLCommandBuffer> g_last_cb; static pthread_mutex_t g_last_mu = PTHREAD_MUTEX_INITIALIZER;
static void cb_commit_one(id<MTLCommandBuffer> cb) { pthread_mutex_lock(&g_last_mu); [cb commit]; g_last_cb = cb; pthread_mutex_unlock(&g_last_mu); g_n_cb++; }
void dm_gl_drain(void) {   // dm_gfx_suspend (hle_nvn.c)
    pthread_mutex_lock(&g_last_mu); id<MTLCommandBuffer> cb = g_last_cb; pthread_mutex_unlock(&g_last_mu);
    [cb waitUntilCompleted];
}
static id<MTLCommandBuffer> cb_get(void) {
    if (!g_cb) { g_cb = [g_q commandBuffer]; g_cb.label = @"gl frame"; }
    return g_cb;
}
static void pass_end(void);
// Commit the frame's command buffer: the event and g_done reach its serial when it completes
static uint64_t cb_commit(void) {
    pass_end();
    id<MTLCommandBuffer> cb = cb_get();
    uint64_t s = g_serial++;
    [cb encodeSignalEvent:g_ev value:s];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
        if (c.status == MTLCommandBufferStatusError) { static int n; if (n++ < 10) GLOG("[RED] command buffer %llu failed: %s", (unsigned long long)s, c.error.localizedDescription.UTF8String); }
        uint64_t d = atomic_load(&g_done); while (d < s && !atomic_compare_exchange_weak(&g_done, &d, s)) {}
    }];
    cb_commit_one(cb);
    g_cb = nil;
    return s;
}

// ---------- Objects ----------
// GL names index plain tables; a name is handed out by glGen* / glCreate* and its slot freed by glDelete*
#define NAMES(T, tab, n, cap) static T **tab; static uint32_t n, cap;
static uint32_t name_new(void ***tab, uint32_t *n, uint32_t *cap, size_t sz) {
    for (uint32_t i = 1; i < *n; i++) if (!(*tab)[i]) { (*tab)[i] = zalloc(sz); return i; }
    if (*n == 0) *n = 1;
    if (*n >= *cap) { *cap = *cap ? *cap * 2 : 256; *tab = realloc(*tab, *cap * sizeof **tab); memset(*tab + *n, 0, (*cap - *n) * sizeof **tab); }
    (*tab)[*n] = zalloc(sz); return (*n)++;
}
static void *name_at(void ***tab, uint32_t *n, uint32_t *cap, uint32_t k, size_t sz) {   // the object of name k, made if it is not there
    if (k >= *cap) { uint32_t c = *cap ? *cap : 256; while (c <= k) c *= 2; *tab = realloc(*tab, c * sizeof **tab); memset(*tab + *cap, 0, (c - *cap) * sizeof **tab); *cap = c; }
    if (*n <= k) *n = k + 1;
    if (!(*tab)[k]) (*tab)[k] = zalloc(sz);
    return (*tab)[k];
}

// use: serial of the last command buffer that read it; immutable: made by glBufferStorage (LÖVE's persistently mapped streams), never orphaned, the game keeps a pointer into it
typedef struct { __strong id<MTLBuffer> mb; size_t size; uint64_t use; int immutable; } buf_t;
NAMES(buf_t, g_bufs, g_nbufs, g_capbufs)
static buf_t *buf_of(GLuint n) { return n && n < g_nbufs ? g_bufs[n] : NULL; }

// A texture: target, format, levels given so far (pend[face][level], until the Metal texture is made), sampler parameters. d: depth (3D) or layers (2D array);
// slevels: the level count glTexStorage gave (LÖVE allocates every level up front); swz: GL_TEXTURE_SWIZZLE_R..A (LÖVE's LA8 is RG8 read as R, R, R, G in a core profile)
#define MAXLV 16
typedef struct { void *data; size_t pitch, size; uint32_t w, h; } level_t;
typedef struct {
    GLenum target; int made, fmt, levels, slevels; uint32_t w, h, d; uint64_t use, gen;
    level_t pend[6][MAXLV]; int defined;   // defined: bit per level whose face(s) were given
    __strong id<MTLTexture> tex; __strong id<MTLSamplerState> smp;
    GLint minf, magf, ws, wt, wr, maxlv, cmp, swz[4]; float aniso, border[4]; int smp_dirty;
} tex_t;
NAMES(tex_t, g_texs, g_ntexs, g_captexs)
static tex_t *tex_of(GLuint n) { return n && n < g_ntexs ? g_texs[n] : NULL; }

typedef struct { int fmt; uint32_t w, h; __strong id<MTLTexture> tex; uint64_t gen; } rb_t;
NAMES(rb_t, g_rbs, g_nrbs, g_caprbs)
static rb_t *rb_of(GLuint n) { return n && n < g_nrbs ? g_rbs[n] : NULL; }

typedef struct { int kind; GLuint name; int level, face; } att_t;   // kind 0 none, 1 texture, 2 renderbuffer; face: a cube face, an array layer or a 3D slice
typedef struct { att_t col, depth, stencil; uint64_t gen; } fb_t;
NAMES(fb_t, g_fbs, g_nfbs, g_capfbs)
static fb_t *fb_of(GLuint n) { return n && n < g_nfbs ? g_fbs[n] : NULL; }

typedef struct { uint8_t on, size, norm, div; GLenum type; GLsizei stride; uint64_t off; GLuint buf; } vattr_t;   // div: glVertexAttribDivisor (0 = per vertex)
typedef struct { vattr_t a[16]; GLuint ebo; } vao_t;
NAMES(vao_t, g_vaos, g_nvaos, g_capvaos)
static vao_t g_vao0;   // the default vertex array (GL core has none, the compatibility profile does)

typedef struct { GLenum type; char *src; size_t len; char key[17]; const shinfo *si; __strong id<MTLFunction> fn; int compiled; } shader_t;
typedef struct { char name[48]; uint8_t type; uint16_t count; int32_t off[2]; uint16_t stride[2]; int slot[2]; int unit; } puni_t;   // [0] vertex, [1] fragment
typedef struct { uint32_t vf[16]; uint16_t stride[16]; uint8_t div[16]; uint32_t cpf, dpf, spf; uint8_t blend, mask, bsrc, bdst, bsa, bda, eqc, eqa; } pkey_t;
typedef struct { pkey_t k; void *pso; } pent_t;   // pso: a retained id<MTLRenderPipelineState>, NULL = failed
// amap: the game's attribute index of each Metal vertex input (glBindAttribLocation before the link, else the input's own number, which glGetAttribLocation returns);
// LÖVE binds VertexPosition, VertexTexCoord, VertexColor and ConstantColor to 0–3 in every program
typedef struct { char name[48]; int index; } abind_t;
typedef struct {
    GLuint vs, fs; int linked; const shinfo *si[2]; __strong id<MTLFunction> fn[2];
    int nu; puni_t *u; uint8_t *ub[2]; uint64_t ub_gen[2]; uint32_t serial;
    pent_t *pc; int npc, cappc;
    abind_t bind[16]; int nbind; int8_t amap[16];
} prog_t;
// Shaders and programs share one name space, as in GL
typedef struct { int is_prog; union { shader_t s; prog_t p; }; } sp_t;
NAMES(sp_t, g_sps, g_nsps, g_capsps)
static shader_t *shader_of(GLuint n) { return n && n < g_nsps && g_sps[n] && !g_sps[n]->is_prog ? &g_sps[n]->s : NULL; }
static prog_t *prog_of(GLuint n) { return n && n < g_nsps && g_sps[n] && g_sps[n]->is_prog ? &g_sps[n]->p : NULL; }

// ---------- Context state ----------
// units: as many as GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS says (LÖVE binds a texture of each type to every one at start); fb / read_fb: the draw and read framebuffers;
// va: each attribute's current value (glVertexAttrib4f), what a vertex input reads when its array is disabled: LÖVE's VertexColor and ConstantColor
#define NUNITS 96
static struct {
    GLuint array_buf, rb, fb, read_fb, prog, vao; int unit;
    struct { GLuint t2d, tcube, t3d, t2da; } units[NUNITS];
    float clear_c[4]; double clear_d; int clear_s;
    uint8_t cmask, dmask, depth, blend, cull, scissor, stencil, poffset, srgb;
    GLenum dfunc, cull_face, front, bsrc, bdst, bsa, bda, eqc, eqa;
    GLenum sfunc, sfail, szfail, spass; GLint sref; GLuint smask, swmask;
    GLint vp[4], sc[4]; float pfactor, punits, psize;
    float va[16][4];
    int unpack_align, unpack_row, pack_align;
    uint32_t palette[256]; int npalette, color_table, index_shift, index_offset;
    int swap_interval;
} S = {.cmask = 0xf, .dmask = 1, .dfunc = 0x201, .cull_face = GL_BACK, .front = GL_CCW, .bsrc = 1, .bdst = 0, .bsa = 1, .bda = 0, .eqc = GL_FUNC_ADD, .eqa = GL_FUNC_ADD,
       .sfunc = 0x207, .sfail = GL_KEEP, .szfail = GL_KEEP, .spass = GL_KEEP, .smask = 0xffffffff, .swmask = 0xffffffff, .clear_d = 1, .unpack_align = 4, .pack_align = 4,
       .swap_interval = 1, .psize = 1,
       .va = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1},
              {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}}};
static vao_t *cur_vao(void) { return S.vao && S.vao < g_nvaos && g_vaos[S.vao] ? g_vaos[S.vao] : &g_vao0; }
static GLuint *unit_slot(int unit, GLenum target) {   // a unit's binding of a texture target (a cube face names the cube); NULL for a target this layer does not have
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) target = GL_TEXTURE_CUBE_MAP;
    if (unit < 0 || unit >= NUNITS) return NULL;
    switch (target) {
        case GL_TEXTURE_2D: return &S.units[unit].t2d; case GL_TEXTURE_CUBE_MAP: return &S.units[unit].tcube;
        case GL_TEXTURE_3D: return &S.units[unit].t3d; case GL_TEXTURE_2D_ARRAY: return &S.units[unit].t2da;
    }
    return NULL;
}
static tex_t *bound_tex(GLenum target) { GLuint *s = unit_slot(S.unit, target); return s ? tex_of(*s) : NULL; }

// ---------- The window surface ----------
#define NWIN 3
static struct { int made; uint32_t w, h; __strong id<MTLTexture> col[NWIN], ds; uint64_t serial[NWIN], ticket[NWIN]; int cur; } g_win;
static void win_make(uint32_t w, uint32_t h) {
    @autoreleasepool {
        for (int i = 0; i < NWIN; i++) if (g_win.ticket[i]) mtl_present_wait(g_win.ticket[i]);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        for (int i = 0; i < NWIN; i++) { g_win.col[i] = [g_dev newTextureWithDescriptor:td]; g_win.serial[i] = mtl_ext_tex((__bridge void *)g_win.col[i]); g_win.ticket[i] = 0; }
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8 width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        g_win.ds = [g_dev newTextureWithDescriptor:td];
        g_win.w = w; g_win.h = h; g_win.cur = 0; g_win.made = 1;
    }
    GLOG("window surface %ux%u (RGBA8, depth 32F + stencil 8, %d images)", w, h, NWIN);
}

// ---------- Formats ----------
// fmt: index into g_fmt. A swizzle only where Metal's default differs from GL's: Metal reads a channel a format lacks as 0 (alpha 1), as GL does, so only the luminance /
// alpha formats and DXT1 without alpha have one (Metal refuses a swizzle on a render target, and GL cannot render into those). Conversions on upload: CV_COPY (the bytes as given), CV_RGB8 (RGB → RGBA), CV_565 / CV_4444 / CV_5551 (16-bit packed → RGBA8), CV_INDEX (8-bit colour
// indices through the glColorTable palette). bw / bh: block size of a compressed format, bytes: per texel or per block
enum { CV_COPY, CV_RGB8, CV_565, CV_4444, CV_5551, CV_INDEX, CV_NONE };
enum { K_COL, K_DEPTH, K_DS, K_BLOCK };
typedef struct { MTLPixelFormat pf; uint8_t bytes, bw, bh, kind; MTLTextureSwizzleChannels swz; const char *name; } fmt_t;
#define SWZ(r, g, b, a) {MTLTextureSwizzle##r, MTLTextureSwizzle##g, MTLTextureSwizzle##b, MTLTextureSwizzle##a}
#define SW0 SWZ(Red, Green, Blue, Alpha)
enum { F_RGBA8, F_BGRA8, F_SRGBA8, F_R8, F_RG8, F_L8, F_A8, F_LA8, F_RGBA16F, F_RGBA32F, F_R16F, F_RG16F, F_R32F, F_RG32F, F_RG11B10F, F_RGB10A2, F_RGBA16,
       F_D16, F_D32F, F_D32FS8, F_S8, F_BC1_RGB, F_BC1, F_BC2, F_BC3, F_BC1_SRGB, F_BC2_SRGB, F_BC3_SRGB, F_BC4, F_BC4S, F_BC5, F_BC5S, F_BC7, F_BC7_SRGB, F_BC6S, F_BC6U,
       F_ASTC4, F_ASTC4_SRGB, F_N };
static const fmt_t g_fmt[F_N] = {
    [F_RGBA8] = {MTLPixelFormatRGBA8Unorm, 4, 1, 1, K_COL, SW0, "RGBA8"}, [F_BGRA8] = {MTLPixelFormatBGRA8Unorm, 4, 1, 1, K_COL, SW0, "BGRA8"},
    [F_SRGBA8] = {MTLPixelFormatRGBA8Unorm_sRGB, 4, 1, 1, K_COL, SW0, "SRGB8_ALPHA8"},
    [F_R8] = {MTLPixelFormatR8Unorm, 1, 1, 1, K_COL, SW0, "R8"}, [F_RG8] = {MTLPixelFormatRG8Unorm, 2, 1, 1, K_COL, SW0, "RG8"},
    [F_L8] = {MTLPixelFormatR8Unorm, 1, 1, 1, K_COL, SWZ(Red, Red, Red, One), "LUMINANCE"}, [F_A8] = {MTLPixelFormatR8Unorm, 1, 1, 1, K_COL, SWZ(Zero, Zero, Zero, Red), "ALPHA"},
    [F_LA8] = {MTLPixelFormatRG8Unorm, 2, 1, 1, K_COL, SWZ(Red, Red, Red, Green), "LUMINANCE_ALPHA"},
    [F_RGBA16F] = {MTLPixelFormatRGBA16Float, 8, 1, 1, K_COL, SW0, "RGBA16F"}, [F_RGBA32F] = {MTLPixelFormatRGBA32Float, 16, 1, 1, K_COL, SW0, "RGBA32F"},
    [F_R16F] = {MTLPixelFormatR16Float, 2, 1, 1, K_COL, SW0, "R16F"}, [F_RG16F] = {MTLPixelFormatRG16Float, 4, 1, 1, K_COL, SW0, "RG16F"},
    [F_R32F] = {MTLPixelFormatR32Float, 4, 1, 1, K_COL, SW0, "R32F"}, [F_RG32F] = {MTLPixelFormatRG32Float, 8, 1, 1, K_COL, SW0, "RG32F"},
    [F_RG11B10F] = {MTLPixelFormatRG11B10Float, 4, 1, 1, K_COL, SW0, "R11F_G11F_B10F"},
    [F_RGB10A2] = {MTLPixelFormatRGB10A2Unorm, 4, 1, 1, K_COL, SW0, "RGB10_A2"}, [F_RGBA16] = {MTLPixelFormatRGBA16Unorm, 8, 1, 1, K_COL, SW0, "RGBA16"},
    [F_D16] = {MTLPixelFormatDepth16Unorm, 2, 1, 1, K_DEPTH, SW0, "DEPTH16"}, [F_D32F] = {MTLPixelFormatDepth32Float, 4, 1, 1, K_DEPTH, SW0, "DEPTH24/32"},
    [F_D32FS8] = {MTLPixelFormatDepth32Float_Stencil8, 8, 1, 1, K_DS, SW0, "DEPTH24_STENCIL8"}, [F_S8] = {MTLPixelFormatStencil8, 1, 1, 1, K_DS, SW0, "STENCIL8"},
    [F_BC1_RGB] = {MTLPixelFormatBC1_RGBA, 8, 4, 4, K_BLOCK, SWZ(Red, Green, Blue, One), "DXT1 RGB"}, [F_BC1] = {MTLPixelFormatBC1_RGBA, 8, 4, 4, K_BLOCK, SW0, "DXT1"},
    [F_BC2] = {MTLPixelFormatBC2_RGBA, 16, 4, 4, K_BLOCK, SW0, "DXT3"}, [F_BC3] = {MTLPixelFormatBC3_RGBA, 16, 4, 4, K_BLOCK, SW0, "DXT5"},
    [F_BC1_SRGB] = {MTLPixelFormatBC1_RGBA_sRGB, 8, 4, 4, K_BLOCK, SW0, "DXT1 sRGB"}, [F_BC2_SRGB] = {MTLPixelFormatBC2_RGBA_sRGB, 16, 4, 4, K_BLOCK, SW0, "DXT3 sRGB"},
    [F_BC3_SRGB] = {MTLPixelFormatBC3_RGBA_sRGB, 16, 4, 4, K_BLOCK, SW0, "DXT5 sRGB"},
    [F_BC4] = {MTLPixelFormatBC4_RUnorm, 8, 4, 4, K_BLOCK, SW0, "RGTC1"}, [F_BC4S] = {MTLPixelFormatBC4_RSnorm, 8, 4, 4, K_BLOCK, SW0, "RGTC1 signed"},
    [F_BC5] = {MTLPixelFormatBC5_RGUnorm, 16, 4, 4, K_BLOCK, SW0, "RGTC2"}, [F_BC5S] = {MTLPixelFormatBC5_RGSnorm, 16, 4, 4, K_BLOCK, SW0, "RGTC2 signed"},
    [F_BC7] = {MTLPixelFormatBC7_RGBAUnorm, 16, 4, 4, K_BLOCK, SW0, "BPTC"}, [F_BC7_SRGB] = {MTLPixelFormatBC7_RGBAUnorm_sRGB, 16, 4, 4, K_BLOCK, SW0, "BPTC sRGB"},
    [F_BC6S] = {MTLPixelFormatBC6H_RGBFloat, 16, 4, 4, K_BLOCK, SW0, "BPTC signed float"},
    [F_BC6U] = {MTLPixelFormatBC6H_RGBUfloat, 16, 4, 4, K_BLOCK, SW0, "BPTC unsigned float"},
    [F_ASTC4] = {MTLPixelFormatASTC_4x4_LDR, 16, 4, 4, K_BLOCK, SW0, "ASTC 4x4"}, [F_ASTC4_SRGB] = {MTLPixelFormatASTC_4x4_sRGB, 16, 4, 4, K_BLOCK, SW0, "ASTC 4x4 sRGB"},
};
// internal format (+ format / type for unsized ones) → fmt and the conversion of the given data; -1 when not mapped
static int fmt_pick(GLint ifmt, GLenum format, GLenum type, int *cv) {
    *cv = CV_COPY;
    if (format == GL_COLOR_INDEX) { *cv = type == GL_UNSIGNED_BYTE ? CV_INDEX : CV_NONE; return F_RGBA8; }
    if (type == GL_UNSIGNED_SHORT_5_6_5) { *cv = CV_565; return F_RGBA8; }
    if (type == GL_UNSIGNED_SHORT_4_4_4_4) { *cv = CV_4444; return F_RGBA8; }
    if (type == GL_UNSIGNED_SHORT_5_5_5_1) { *cv = CV_5551; return F_RGBA8; }
    int half = type == GL_HALF_FLOAT, fl = type == GL_FLOAT;
    switch (ifmt) {
        case GL_RGBA: case GL_RGBA8: case 4:
            if (fl) return F_RGBA32F;
            if (half) return F_RGBA16F;
            if (format == GL_BGRA) return F_BGRA8;
            if (format == GL_RGB) { *cv = CV_RGB8; return F_RGBA8; }
            return F_RGBA8;
        case GL_RGB: case GL_RGB8: case 3:
            if (fl || half) { *cv = CV_NONE; return F_RGBA16F; }
            if (format == GL_RGBA) return F_RGBA8;
            *cv = CV_RGB8; return F_RGBA8;
        case GL_BGRA: return F_BGRA8;
        case GL_RGBA4: case GL_RGB5_A1: case GL_RGB565: return F_RGBA8;   // glTexStorage2D's sized formats (no data: the uploads are converted by their type)
        case GL_SRGB8_ALPHA8: return F_SRGBA8;
        case GL_RED: case GL_R8: return fl ? F_R32F : half ? F_R16F : F_R8;
        case GL_RG: case GL_RG8: return fl ? F_RG32F : half ? F_RG16F : F_RG8;
        case GL_LUMINANCE: case 1: return F_L8;
        case GL_ALPHA: return F_A8;
        case GL_LUMINANCE_ALPHA: case 2: return F_LA8;
        case GL_RGBA16F: if (fl) { *cv = CV_NONE; } return F_RGBA16F;
        case GL_RGBA32F: return F_RGBA32F;
        case GL_R16F: return F_R16F;
        case GL_RG16F: return F_RG16F;
        case GL_R32F: return F_R32F;
        case GL_RG32F: return F_RG32F;
        case GL_R11F_G11F_B10F: if (type != 0x8c3b) *cv = CV_NONE; return F_RG11B10F;
        case GL_RGB10_A2: return F_RGB10A2;
        case GL_RGBA16: return F_RGBA16;
        case GL_DEPTH_COMPONENT16: return F_D16;
        case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32: case GL_DEPTH_COMPONENT32F: if (type != GL_FLOAT) *cv = CV_NONE; return F_D32F;
        case GL_DEPTH_STENCIL: case GL_DEPTH24_STENCIL8: case GL_DEPTH32F_STENCIL8: *cv = CV_NONE; return F_D32FS8;
        case GL_STENCIL_INDEX8: *cv = CV_NONE; return F_S8;
        case GL_COMPRESSED_RGB_S3TC_DXT1: return F_BC1_RGB;
        case GL_COMPRESSED_RGBA_S3TC_DXT1: return F_BC1;
        case GL_COMPRESSED_RGBA_S3TC_DXT3: return F_BC2;
        case GL_COMPRESSED_RGBA_S3TC_DXT5: return F_BC3;
        case GL_COMPRESSED_SRGB_S3TC_DXT1: case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1: return F_BC1_SRGB;
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3: return F_BC2_SRGB;
        case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5: return F_BC3_SRGB;
        case GL_COMPRESSED_RED_RGTC1: return F_BC4;
        case GL_COMPRESSED_SIGNED_RED_RGTC1: return F_BC4S;
        case GL_COMPRESSED_RG_RGTC2: return F_BC5;
        case GL_COMPRESSED_SIGNED_RG_RGTC2: return F_BC5S;
        case GL_COMPRESSED_RGBA_BPTC_UNORM: return F_BC7;
        case GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM: return F_BC7_SRGB;
        case GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT: return F_BC6S;
        case GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT: return F_BC6U;
        case GL_COMPRESSED_RGBA_ASTC_4x4: return F_ASTC4;
        case GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4: return F_ASTC4_SRGB;
    }
    return -1;
}
static size_t fmt_pitch(int f, uint32_t w) { const fmt_t *m = &g_fmt[f]; return (size_t)((w + m->bw - 1) / m->bw) * m->bytes; }
static size_t fmt_rows(int f, uint32_t h) { const fmt_t *m = &g_fmt[f]; return (h + m->bh - 1) / m->bh; }

// The given pixels (GL unpack rules: rows of unpack_row or w pixels, each row aligned to unpack_align) → the Metal texture's bytes, rows packed. NULL data → zeros
static void *pixels_convert(const void *src, uint32_t w, uint32_t h, GLenum format, GLenum type, int f, int cv, size_t *pitch_out) {
    size_t dp = fmt_pitch(f, w), rows = fmt_rows(f, h);
    uint8_t *dst = zalloc(dp * rows + 1); *pitch_out = dp;
    if (!src) return dst;
    if (g_fmt[f].kind == K_BLOCK) { memcpy(dst, src, dp * rows); return dst; }   // compressed data has no unpack rows
    static const int comps_of[] = {[GL_RED - GL_RED] = 1, [GL_GREEN - GL_RED] = 1, [GL_BLUE - GL_RED] = 1, [GL_ALPHA - GL_RED] = 1, [GL_RGB - GL_RED] = 3, [GL_RGBA - GL_RED] = 4,
                                   [GL_LUMINANCE - GL_RED] = 1, [GL_LUMINANCE_ALPHA - GL_RED] = 2};
    int comps = format == GL_BGRA ? 4 : format == GL_RG ? 2 : format == GL_COLOR_INDEX || format == GL_DEPTH_COMPONENT ? 1 :
                format >= GL_RED && format <= GL_LUMINANCE_ALPHA ? comps_of[format - GL_RED] : 4;
    int tbytes = type == GL_FLOAT || type == GL_UNSIGNED_INT || type == GL_INT ? 4 : type == GL_HALF_FLOAT || type == GL_UNSIGNED_SHORT || type == GL_SHORT ? 2 : 1;
    if (cv == CV_565 || cv == CV_4444 || cv == CV_5551) { comps = 1; tbytes = 2; }
    size_t sp = (size_t)(S.unpack_row ? S.unpack_row : w) * comps * tbytes; sp = (sp + S.unpack_align - 1) / S.unpack_align * S.unpack_align;
    const uint8_t *s = src;
    for (uint32_t y = 0; y < h; y++, s += sp) {
        uint8_t *d = dst + y * dp;
        switch (cv) {
            case CV_COPY: memcpy(d, s, umin((uint32_t)dp, (uint32_t)((size_t)w * comps * tbytes))); break;
            case CV_RGB8: for (uint32_t x = 0; x < w; x++) { d[4 * x] = s[3 * x]; d[4 * x + 1] = s[3 * x + 1]; d[4 * x + 2] = s[3 * x + 2]; d[4 * x + 3] = 255; } break;
            case CV_565: for (uint32_t x = 0; x < w; x++) { uint16_t v; memcpy(&v, s + 2 * x, 2); d[4 * x] = (v >> 11) * 255 / 31; d[4 * x + 1] = (v >> 5 & 63) * 255 / 63; d[4 * x + 2] = (v & 31) * 255 / 31; d[4 * x + 3] = 255; } break;
            case CV_4444: for (uint32_t x = 0; x < w; x++) { uint16_t v; memcpy(&v, s + 2 * x, 2); d[4 * x] = (v >> 12) * 17; d[4 * x + 1] = (v >> 8 & 15) * 17; d[4 * x + 2] = (v >> 4 & 15) * 17; d[4 * x + 3] = (v & 15) * 17; } break;
            case CV_5551: for (uint32_t x = 0; x < w; x++) { uint16_t v; memcpy(&v, s + 2 * x, 2); d[4 * x] = (v >> 11) * 255 / 31; d[4 * x + 1] = (v >> 6 & 31) * 255 / 31; d[4 * x + 2] = (v >> 1 & 31) * 255 / 31; d[4 * x + 3] = v & 1 ? 255 : 0; } break;
            case CV_INDEX:
                for (uint32_t x = 0; x < w; x++) {
                    int i = ((int)s[x] << S.index_shift) + S.index_offset; uint32_t c = S.npalette ? S.palette[i < 0 ? 0 : i >= S.npalette ? S.npalette - 1 : i] : 0xff000000u | s[x] * 0x10101u;
                    memcpy(d + 4 * x, &c, 4);
                }
                break;
        }
    }
    return dst;
}

// ---------- Textures ----------
static MTLSamplerAddressMode wrap_of(GLint w) {
    switch (w) { case GL_CLAMP_TO_EDGE: case GL_CLAMP: return MTLSamplerAddressModeClampToEdge; case GL_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
                 case GL_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor; default: return MTLSamplerAddressModeRepeat; }
}
static void tex_init_params(tex_t *t, GLenum target) {
    t->target = target; t->minf = GL_NEAREST_MIPMAP_LINEAR; t->magf = GL_LINEAR; t->ws = t->wt = t->wr = GL_REPEAT; t->maxlv = 1000; t->aniso = 1; t->smp_dirty = 1; t->fmt = -1;
    for (int c = 0; c < 4; c++) t->swz[c] = GL_RED + c;
}
static int tex_faces(const tex_t *t) { return t->target == GL_TEXTURE_CUBE_MAP ? 6 : 1; }
// The swizzle a texture is made with: its format's own (only where Metal's reading differs from GL's), then GL_TEXTURE_SWIZZLE_R..A over it
static MTLTextureSwizzleChannels tex_swz(const tex_t *t) {
    MTLTextureSwizzleChannels f = g_fmt[t->fmt].swz, o;
    const MTLTextureSwizzle src[4] = {f.red, f.green, f.blue, f.alpha};
    MTLTextureSwizzle out[4];
    for (int c = 0; c < 4; c++) { GLint p = t->swz[c]; out[c] = p >= GL_RED && p <= GL_ALPHA ? src[p - GL_RED] : p == GL_ONE ? MTLTextureSwizzleOne : MTLTextureSwizzleZero; }
    o.red = out[0]; o.green = out[1]; o.blue = out[2]; o.alpha = out[3];
    return o;
}
static id<MTLSamplerState> tex_sampler(tex_t *t) {
    if (!t->smp_dirty && t->smp) return t->smp;
    // sampler states are shared: one per distinct parameter set
    typedef struct { GLint minf, magf, ws, wt, wr, lv, cmp; float aniso, b; } skey;
    static struct { skey k; __strong id<MTLSamplerState> s; } cache[256]; static int n;
    int mips = t->levels > 1;
    skey k = {t->minf, t->magf, t->ws, t->wt, t->target == GL_TEXTURE_CUBE_MAP ? GL_CLAMP_TO_EDGE : t->wr, mips ? (GLint)umin((uint32_t)t->maxlv, (uint32_t)t->levels - 1) : 0, t->cmp, t->aniso,
             t->border[3] > 0.5f ? (t->border[0] > 0.5f ? 2.f : 1.f) : 0.f};
    for (int i = 0; i < n; i++) if (!memcmp(&cache[i].k, &k, sizeof k)) { t->smp = cache[i].s; t->smp_dirty = 0; return t->smp; }
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.magFilter = k.magf == GL_NEAREST ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    d.minFilter = k.minf == GL_NEAREST || k.minf == GL_NEAREST_MIPMAP_NEAREST || k.minf == GL_NEAREST_MIPMAP_LINEAR ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
    // a texture of one level samples level 0 whatever its minifying filter (GL would call it incomplete with a mipmap filter and sample black)
    d.mipFilter = !mips || k.minf == GL_NEAREST || k.minf == GL_LINEAR ? MTLSamplerMipFilterNotMipmapped :
                  k.minf == GL_NEAREST_MIPMAP_NEAREST || k.minf == GL_LINEAR_MIPMAP_NEAREST ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    d.lodMaxClamp = mips ? k.lv : 0;
    d.sAddressMode = wrap_of(k.ws); d.tAddressMode = wrap_of(k.wt); d.rAddressMode = wrap_of(k.wr);
    d.borderColor = k.b == 2.f ? MTLSamplerBorderColorOpaqueWhite : k.b == 1.f ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorTransparentBlack;
    d.maxAnisotropy = (NSUInteger)fmaxf(1, fminf(16, k.aniso));
    if (k.cmp) d.compareFunction = (MTLCompareFunction)(k.cmp - GL_NEVER);
    id<MTLSamplerState> s = [g_dev newSamplerStateWithDescriptor:d];
    if (n < 256) { cache[n].k = k; cache[n].s = s; n++; } else RED1("more than 256 distinct sampler parameter sets");
    t->smp = s; t->smp_dirty = 0; return s;
}
static void lv_free(level_t *l) { free(l->data); memset(l, 0, sizeof *l); }
static void tex_drop(tex_t *t) { for (int f = 0; f < 6; f++) for (int l = 0; l < MAXLV; l++) lv_free(&t->pend[f][l]); t->tex = nil; t->made = 0; t->defined = 0; t->levels = 0; t->gen++; }
// Make the Metal texture from the levels given so far: as many levels as are given in a row from level 0 (and fit the size), or as glTexStorage said. rt: made for a framebuffer attachment
static int tex_make(tex_t *t, int rt) {
    if (t->made) return t->tex != nil;
    if (t->fmt < 0 || !t->w || !t->h) return 0;
    int faces = tex_faces(t), lv = 0, vol = t->target == GL_TEXTURE_3D, arr = t->target == GL_TEXTURE_2D_ARRAY;
    uint32_t full = 1; for (uint32_t m = umax(umax(t->w, t->h), vol ? t->d : 1); m > 1; m >>= 1) full++;
    if (t->slevels) lv = (int)umin(umin((uint32_t)t->slevels, full), MAXLV);
    else { while (lv < MAXLV && lv < (int)full && (t->defined >> lv & 1)) lv++; if (!lv) lv = 1; }
    const fmt_t *m = &g_fmt[t->fmt];
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.textureType = faces == 6 ? MTLTextureTypeCube : vol ? MTLTextureType3D : arr ? MTLTextureType2DArray : MTLTextureType2D;
        td.pixelFormat = m->pf; td.width = t->w; td.height = faces == 6 ? t->w : t->h;
        if (vol) td.depth = umax(1, t->d); else if (arr) td.arrayLength = umax(1, t->d);
        td.mipmapLevelCount = lv;
        MTLTextureSwizzleChannels sw = tex_swz(t);
        int swz = sw.red != MTLTextureSwizzleRed || sw.green != MTLTextureSwizzleGreen || sw.blue != MTLTextureSwizzleBlue || sw.alpha != MTLTextureSwizzleAlpha;
        int data = 0; for (int f = 0; f < faces; f++) for (int l = 0; l < lv; l++) data |= t->pend[f][l].data != NULL;
        td.usage = MTLTextureUsageShaderRead | (m->kind != K_BLOCK && !swz ? MTLTextureUsageRenderTarget : 0);
        // a render target (first made as an attachment, with no data given): private, so Metal can compress it, and its uploads are blits (tex_write). Textures the CPU fills stay
        // shared: the movie planes, made with no data and filled by glTexSubImage2D every frame, are written in place once the GPU is done with them
        td.storageMode = m->kind == K_DEPTH || m->kind == K_DS || (rt && !data) ? MTLStorageModePrivate : MTLStorageModeShared;
        if (swz) td.swizzle = sw;
        if (swz && rt) RED1("texture %ux%u %s with a swizzle is a framebuffer attachment (Metal renders into no swizzled texture)", t->w, t->h, m->name);
        t->tex = [g_dev newTextureWithDescriptor:td];
        if (!t->tex) { GLOG("[RED] texture %ux%u %s (%d levels) could not be made", t->w, t->h, m->name, lv); t->made = 1; return 0; }
        for (int f = 0; f < faces; f++) for (int l = 0; l < lv; l++) {
            level_t *p = &t->pend[f][l];
            if (p->data && td.storageMode == MTLStorageModeShared)
                [t->tex replaceRegion:MTLRegionMake2D(0, 0, umax(1, t->w >> l), umax(1, t->h >> l)) mipmapLevel:l slice:f withBytes:p->data bytesPerRow:p->pitch bytesPerImage:0];
        }
    }
    for (int f = 0; f < 6; f++) for (int l = 0; l < MAXLV; l++) lv_free(&t->pend[f][l]);
    t->levels = lv; t->made = 1; t->smp_dirty = 1;
    OLOG("texture %ux%u %s, %d levels%s", t->w, t->h, m->name, lv, faces == 6 ? ", cube" : "");
    return 1;
}
// Write one level's (or a part's) bytes into a made texture, in command order: a CPU write when no command buffer that may still read it is pending,
// else a blit from transient memory (in this frame's command buffer, after ending its render pass, when this frame used it; in a command buffer of its own otherwise).
// face: the cube face or array layer; z: the slice of a 3D texture
static void tex_write(tex_t *t, int face, int lv, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, const void *data, size_t pitch) {
    g_n_up++;
    size_t rows = fmt_rows(t->fmt, h);
    if (t->use <= atomic_load(&g_done) && t->tex.storageMode == MTLStorageModeShared) {
        [t->tex replaceRegion:MTLRegionMake3D(x, y, z, w, h, 1) mipmapLevel:lv slice:face withBytes:data bytesPerRow:pitch bytesPerImage:t->target == GL_TEXTURE_3D ? pitch * rows : 0];
        return;
    }
    size_t off; id<MTLBuffer> st = arena_alloc(pitch * rows, &off);
    memcpy((uint8_t *)st.contents + off, data, pitch * rows);
    id<MTLCommandBuffer> cb;
    if (t->use >= g_serial) { pass_end(); cb = cb_get(); }
    else { cb = [g_q commandBuffer]; cb.label = @"gl upload"; }
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromBuffer:st sourceOffset:off sourceBytesPerRow:pitch sourceBytesPerImage:pitch * rows sourceSize:MTLSizeMake(w, h, 1) toTexture:t->tex
       destinationSlice:face destinationLevel:lv destinationOrigin:MTLOriginMake(x, y, z)];
    [b endEncoding]; g_n_blit++;
    if (cb != g_cb) { dm_gfx_enter(); cb_commit_one(cb); dm_gfx_leave(); }   // no GPU work goes out while the app is in the background (dm_gfx_suspend)
}
// glTexImage2D / glCompressedTexImage2D: the level's bytes, converted
static void tex_image(GLenum target, GLint level, int f, int cv, uint32_t w, uint32_t h, void *conv, size_t pitch) {
    tex_t *t = target == GL_TEXTURE_2D || (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) ? bound_tex(target) : NULL;
    int face = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    if (!t || level < 0 || level >= MAXLV) { RED1("glTexImage2D target %#x level %d with no texture bound", target, level); free(conv); return; }
    (void)cv;
    uint32_t w0 = level ? w << level : w, h0 = level ? h << level : h;
    if (level == 0 && (t->fmt != f || t->w != w || t->h != h) && (t->made || t->defined)) tex_drop(t);   // level 0 respecified: a new texture
    if (!t->made && !t->defined) { t->fmt = f; t->w = level ? umax(w0, 1) : w; t->h = level ? umax(h0, 1) : h; }
    if (t->fmt != f) { RED1("texture level %d in format %s, its level 0 in %s", level, g_fmt[f].name, g_fmt[t->fmt].name); free(conv); return; }
    if (t->made && level < t->levels && t->tex) { if (conv) tex_write(t, face, level, 0, 0, 0, w, h, conv, pitch); free(conv); return; }
    if (t->made) { RED1("texture level %d given after the texture was made with %d levels: ignored", level, t->levels); free(conv); return; }
    level_t *l = &t->pend[face][level]; lv_free(l);
    *l = (level_t){conv, pitch, pitch * fmt_rows(f, h), w, h};
    int faces = t->target == GL_TEXTURE_CUBE_MAP ? 6 : 1, all = 1;
    for (int k = 0; k < faces; k++) all &= t->pend[k][level].data != NULL || (level == 0 && !conv);
    if (all) t->defined |= 1 << level;
}

// ---------- Render passes ----------
// The pass being encoded: framebuffer name, its attachments' generation (a changed attachment ends the pass), clears waiting for the pass to begin
static struct { GLuint fb; uint64_t gen; int open; uint32_t w, h; MTLPixelFormat cpf, dpf, spf; } P;
static struct { int bits; float c[4]; double d; int s; GLuint fb; } g_pend_clear;
static struct { void *pso; void *dss; int cull, wind, vp[4], sc[4], sref; float pf, pu; GLuint prog; uint64_t ub_gen[2]; } E;   // encoder state set so far (NULL / -1 = unset)

static id<MTLTexture> att_tex(const att_t *a, uint64_t *gen) {
    if (a->kind == 1) { tex_t *t = tex_of(a->name); if (!t) return nil; if (!t->made) tex_make(t, 1); *gen += t->gen; t->use = g_serial; return t->tex; }
    if (a->kind == 2) { rb_t *r = rb_of(a->name); if (!r) return nil; *gen += r->gen; return r->tex; }
    return nil;
}
static uint64_t fb_gen(GLuint fb) { fb_t *f = fb_of(fb); if (!f) return 0; uint64_t g = f->gen; att_tex(&f->col, &g); att_tex(&f->depth, &g); att_tex(&f->stencil, &g); return g; }
// The size of a framebuffer (its first attachment's level), 0 when it has none
static void fb_size(GLuint fb, uint32_t *w, uint32_t *h) {
    *w = *h = 0;
    if (!fb) { *w = g_win.w; *h = g_win.h; return; }
    fb_t *f = fb_of(fb); if (!f) return;
    const att_t *a = f->col.kind ? &f->col : f->depth.kind ? &f->depth : &f->stencil; uint64_t g = 0;
    id<MTLTexture> t = att_tex(a, &g); if (t) { *w = umax(1, (uint32_t)t.width >> a->level); *h = umax(1, (uint32_t)t.height >> a->level); }
}
static void pass_end(void) {
    if (g_enc) { [g_enc endEncoding]; g_enc = nil; }
    P.open = 0;
}
static void enc_reset(void) { memset(&E, 0, sizeof E); E.cull = E.wind = E.sref = -1; E.vp[2] = E.sc[2] = -1; E.pf = E.pu = NAN; }
static void pass_begin(void) {
    @autoreleasepool {
        id<MTLCommandBuffer> cb = cb_get();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLTexture> c = nil, d = nil, s = nil; int cl = 0, cs = 0, dl = 0, sl = 0;
        uint64_t gen = 0;
        if (!S.fb) {
            if (!g_win.made) { int w, h; dm_vi_layer_size(&w, &h); win_make(w > 0 ? w : 1280, h > 0 ? h : 720); }
            c = g_win.col[g_win.cur]; d = s = g_win.ds;
        } else {
            fb_t *f = fb_of(S.fb);
            if (f) {
                c = att_tex(&f->col, &gen); d = att_tex(&f->depth, &gen); s = att_tex(&f->stencil, &gen);
                cl = f->col.level; cs = f->col.face; dl = f->depth.level; sl = f->stencil.level;
                gen += f->gen;
            }
            if (d && d.pixelFormat != MTLPixelFormatDepth32Float && d.pixelFormat != MTLPixelFormatDepth16Unorm && d.pixelFormat != MTLPixelFormatDepth32Float_Stencil8) d = nil;
            if (s && s.pixelFormat != MTLPixelFormatDepth32Float_Stencil8 && s.pixelFormat != MTLPixelFormatStencil8) s = nil;
            // a depth / stencil texture attached as GL's depth attachment alone (or stencil alone) is both of Metal's: its pixel format has both, and the pipelines say so
            if (d && !s && d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) { s = d; sl = dl; }
            else if (s && !d && s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) { d = s; dl = sl; }
        }
        int pc = g_pend_clear.fb == S.fb ? g_pend_clear.bits : 0;
        if (c) {
            MTLRenderPassColorAttachmentDescriptor *a = rp.colorAttachments[0]; a.texture = c; a.level = cl; a.storeAction = MTLStoreActionStore;
            if (c.textureType == MTLTextureType3D) a.depthPlane = cs; else a.slice = cs;
            if (pc & GL_COLOR_BUFFER_BIT) { a.loadAction = MTLLoadActionClear; a.clearColor = MTLClearColorMake(g_pend_clear.c[0], g_pend_clear.c[1], g_pend_clear.c[2], g_pend_clear.c[3]); }
            else a.loadAction = MTLLoadActionLoad;
        }
        if (d) {
            rp.depthAttachment.texture = d; rp.depthAttachment.level = dl; rp.depthAttachment.storeAction = MTLStoreActionStore;
            if (pc & GL_DEPTH_BUFFER_BIT) { rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = g_pend_clear.d; }
            else rp.depthAttachment.loadAction = MTLLoadActionLoad;
        }
        if (s) {
            rp.stencilAttachment.texture = s; rp.stencilAttachment.level = sl; rp.stencilAttachment.storeAction = MTLStoreActionStore;
            if (pc & GL_STENCIL_BUFFER_BIT) { rp.stencilAttachment.loadAction = MTLLoadActionClear; rp.stencilAttachment.clearStencil = g_pend_clear.s; }
            else rp.stencilAttachment.loadAction = MTLLoadActionLoad;
        }
        if (g_pend_clear.fb == S.fb) g_pend_clear.bits = 0;
        if (!c && !d && !s) { RED1("framebuffer %u has no usable attachment", S.fb); return; }
        id<MTLTexture> any = c ? c : d ? d : s; int lv = c ? cl : d ? dl : sl;
        P.w = umax(1, (uint32_t)any.width >> lv); P.h = umax(1, (uint32_t)any.height >> lv);
        if (!c) { rp.renderTargetWidth = P.w; rp.renderTargetHeight = P.h; }
        P.cpf = c ? c.pixelFormat : MTLPixelFormatInvalid; P.dpf = d ? d.pixelFormat : MTLPixelFormatInvalid; P.spf = s ? s.pixelFormat : MTLPixelFormatInvalid;
        g_enc = [cb renderCommandEncoderWithDescriptor:rp];
        P.fb = S.fb; P.gen = gen; P.open = 1; g_n_pass++;
        enc_reset();
    }
}
// The encoder for the bound framebuffer (a new pass when the binding or an attachment changed)
static int pass_ensure(void) {
    if (P.open && P.fb == S.fb && (!S.fb || fb_gen(S.fb) == P.gen)) return 1;
    pass_end(); pass_begin();
    return P.open;
}
// Clears no draw followed: a pass that only clears
static void clears_flush(void) {
    if (g_pend_clear.bits) { GLuint keep = S.fb; S.fb = g_pend_clear.fb; pass_end(); pass_begin(); pass_end(); S.fb = keep; g_pend_clear.bits = 0; }
}

// ---------- Pipelines ----------
static MTLVertexFormat vfmt_of(const vattr_t *a) {
    int n = a->size < 1 ? 1 : a->size > 4 ? 4 : a->size, no = a->norm;
    static const MTLVertexFormat fl[] = {MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4},
        hf[] = {MTLVertexFormatHalf, MTLVertexFormatHalf2, MTLVertexFormatHalf3, MTLVertexFormatHalf4},
        ub[] = {MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4},
        ubn[] = {MTLVertexFormatUCharNormalized, MTLVertexFormatUChar2Normalized, MTLVertexFormatUChar3Normalized, MTLVertexFormatUChar4Normalized},
        sb[] = {MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4},
        sbn[] = {MTLVertexFormatCharNormalized, MTLVertexFormatChar2Normalized, MTLVertexFormatChar3Normalized, MTLVertexFormatChar4Normalized},
        us[] = {MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4},
        usn[] = {MTLVertexFormatUShortNormalized, MTLVertexFormatUShort2Normalized, MTLVertexFormatUShort3Normalized, MTLVertexFormatUShort4Normalized},
        ss[] = {MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4},
        ssn[] = {MTLVertexFormatShortNormalized, MTLVertexFormatShort2Normalized, MTLVertexFormatShort3Normalized, MTLVertexFormatShort4Normalized},
        ui[] = {MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4},
        si[] = {MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4};
    switch (a->type) {
        case GL_FLOAT: return fl[n - 1];
        case GL_HALF_FLOAT: return hf[n - 1];
        case GL_UNSIGNED_BYTE: return no ? ubn[n - 1] : ub[n - 1];
        case GL_BYTE: return no ? sbn[n - 1] : sb[n - 1];
        case GL_UNSIGNED_SHORT: return no ? usn[n - 1] : us[n - 1];
        case GL_SHORT: return no ? ssn[n - 1] : ss[n - 1];
        case GL_UNSIGNED_INT: return ui[n - 1];
        case GL_INT: return si[n - 1];
    }
    return MTLVertexFormatInvalid;
}
static int vtype_bytes(GLenum t) { return t == GL_FLOAT || t == GL_INT || t == GL_UNSIGNED_INT ? 4 : t == GL_HALF_FLOAT || t == GL_SHORT || t == GL_UNSIGNED_SHORT ? 2 : 1; }
static MTLBlendFactor bf_of(GLenum f) {
    switch (f) {
        case 0: return MTLBlendFactorZero; case 1: return MTLBlendFactorOne;
        case GL_SRC_COLOR: return MTLBlendFactorSourceColor; case GL_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
        case GL_SRC_ALPHA: return MTLBlendFactorSourceAlpha; case GL_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case GL_DST_ALPHA: return MTLBlendFactorDestinationAlpha; case GL_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        case GL_DST_COLOR: return MTLBlendFactorDestinationColor; case GL_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
        case GL_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
        case 0x8001: return MTLBlendFactorBlendColor; case 0x8002: return MTLBlendFactorOneMinusBlendColor; case 0x8003: return MTLBlendFactorBlendAlpha; case 0x8004: return MTLBlendFactorOneMinusBlendAlpha;
    }
    RED1("blend factor %#x not mapped", f); return MTLBlendFactorOne;
}
static MTLBlendOperation beq_of(GLenum e) {
    switch (e) { case GL_FUNC_SUBTRACT: return MTLBlendOperationSubtract; case GL_FUNC_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
                 case GL_MIN: return MTLBlendOperationMin; case GL_MAX: return MTLBlendOperationMax; default: return MTLBlendOperationAdd; }
}
static uint8_t bcode(GLenum f) { return f < 2 ? (uint8_t)f : f >= GL_SRC_COLOR && f <= GL_SRC_ALPHA_SATURATE ? (uint8_t)(f - GL_SRC_COLOR + 2) : f >= 0x8001 && f <= 0x8004 ? (uint8_t)(f - 0x8001 + 11) : 15; }
static GLenum bdecode(uint8_t c) { return c < 2 ? c : c < 11 ? GL_SRC_COLOR + c - 2 : c < 15 ? 0x8001 + c - 11 : 1; }

static id<MTLRenderPipelineState> pso_get(prog_t *p, const pkey_t *k) {
    for (int i = 0; i < p->npc; i++) if (!memcmp(&p->pc[i].k, k, sizeof *k)) return (__bridge id<MTLRenderPipelineState>)p->pc[i].pso;
    id<MTLRenderPipelineState> pso = nil;
    @autoreleasepool {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = p->fn[0]; pd.fragmentFunction = p->fn[1];
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        const shinfo *vs = p->si[0];
        for (int i = 0; i < vs->na; i++) {
            int s = vs->a[i].slot;
            if (k->vf[s]) {
                vd.attributes[s].format = (MTLVertexFormat)k->vf[s]; vd.attributes[s].offset = 0; vd.attributes[s].bufferIndex = 30 - s;
                vd.layouts[30 - s].stride = k->stride[s];
                if (k->div[s]) { vd.layouts[30 - s].stepFunction = MTLVertexStepFunctionPerInstance; vd.layouts[30 - s].stepRate = k->div[s]; }
                else vd.layouts[30 - s].stepFunction = MTLVertexStepFunctionPerVertex;
            } else {   // a disabled array: the attribute's current value (glVertexAttrib4f; (0, 0, 0, 1) by default)
                vd.attributes[s].format = MTLVertexFormatFloat4; vd.attributes[s].offset = 0; vd.attributes[s].bufferIndex = 30 - s;
                vd.layouts[30 - s].stride = 16; vd.layouts[30 - s].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[30 - s].stepRate = 0;
            }
        }
        pd.vertexDescriptor = vd;
        MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
        ca.pixelFormat = (MTLPixelFormat)k->cpf;
        if (k->cpf) {
            ca.writeMask = (k->mask & 1 ? MTLColorWriteMaskRed : 0) | (k->mask & 2 ? MTLColorWriteMaskGreen : 0) | (k->mask & 4 ? MTLColorWriteMaskBlue : 0) | (k->mask & 8 ? MTLColorWriteMaskAlpha : 0);
            if (k->blend) {
                ca.blendingEnabled = YES;
                ca.sourceRGBBlendFactor = bf_of(bdecode(k->bsrc)); ca.destinationRGBBlendFactor = bf_of(bdecode(k->bdst));
                ca.sourceAlphaBlendFactor = bf_of(bdecode(k->bsa)); ca.destinationAlphaBlendFactor = bf_of(bdecode(k->bda));
                ca.rgbBlendOperation = (MTLBlendOperation)k->eqc; ca.alphaBlendOperation = (MTLBlendOperation)k->eqa;
            }
        }
        pd.depthAttachmentPixelFormat = (MTLPixelFormat)k->dpf; pd.stencilAttachmentPixelFormat = (MTLPixelFormat)k->spf;
        NSError *err = nil;
        pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!pso) { static int n; if (n++ < 20) GLOG("[RED] cannot build the pipeline (vs %s, fs %s): %s", p->si[0]->key, p->si[1]->key, err.localizedDescription.UTF8String); }
        else g_n_pso++;
    }
    if (p->npc == p->cappc) { p->cappc = p->cappc ? p->cappc * 2 : 4; p->pc = realloc(p->pc, p->cappc * sizeof *p->pc); }
    p->pc[p->npc].k = *k; p->pc[p->npc].pso = pso ? (__bridge_retained void *)pso : NULL; p->npc++;
    return pso;
}
static id<MTLDepthStencilState> dss_get(void) {
    typedef struct { uint8_t dt, dm, st; GLenum df, sf, s1, s2, s3; GLuint rm, wm; } dkey;
    static struct { dkey k; __strong id<MTLDepthStencilState> s; } cache[128]; static int n;
    dkey k; memset(&k, 0, sizeof k);   // padding too: the cache compares whole keys
    k.dt = S.depth && P.dpf; k.dm = S.dmask && S.depth && P.dpf; k.st = S.stencil && P.spf; k.df = S.depth ? S.dfunc : 0; k.sf = S.stencil ? S.sfunc : 0;
    k.s1 = S.sfail; k.s2 = S.szfail; k.s3 = S.spass; k.rm = S.smask & 0xff; k.wm = S.swmask & 0xff;
    if (!k.st) { k.sf = k.s1 = k.s2 = k.s3 = 0; k.rm = k.wm = 0; }
    for (int i = 0; i < n; i++) if (!memcmp(&cache[i].k, &k, sizeof k)) return cache[i].s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    if (k.dt) { d.depthCompareFunction = (MTLCompareFunction)(k.df - GL_NEVER); d.depthWriteEnabled = k.dm; }
    if (k.st) {
        MTLStencilDescriptor *s = [MTLStencilDescriptor new];
        static const struct { GLenum gl; MTLStencilOperation m; } ops[] = {{GL_KEEP, MTLStencilOperationKeep}, {0, MTLStencilOperationZero}, {GL_REPLACE, MTLStencilOperationReplace},
            {GL_INCR, MTLStencilOperationIncrementClamp}, {GL_DECR, MTLStencilOperationDecrementClamp}, {GL_INVERT, MTLStencilOperationInvert},
            {GL_INCR_WRAP, MTLStencilOperationIncrementWrap}, {GL_DECR_WRAP, MTLStencilOperationDecrementWrap}};
        MTLStencilOperation o[3] = {MTLStencilOperationKeep, MTLStencilOperationKeep, MTLStencilOperationKeep}; GLenum g[3] = {k.s1, k.s2, k.s3};
        for (int j = 0; j < 3; j++) for (size_t i = 0; i < sizeof ops / sizeof *ops; i++) if (ops[i].gl == g[j]) o[j] = ops[i].m;
        s.stencilCompareFunction = (MTLCompareFunction)(k.sf - GL_NEVER); s.stencilFailureOperation = o[0]; s.depthFailureOperation = o[1]; s.depthStencilPassOperation = o[2];
        s.readMask = k.rm; s.writeMask = k.wm;
        d.frontFaceStencil = s; d.backFaceStencil = s;
    }
    id<MTLDepthStencilState> st = [g_dev newDepthStencilStateWithDescriptor:d];
    if (n < 128) { cache[n].k = k; cache[n].s = st; n++; } else RED1("more than 128 depth / stencil states");
    return st;
}

// ---------- Drawing ----------
static void enc_state(void) {
    id<MTLRenderCommandEncoder> e = g_enc;
    // GL viewport (lower-left origin) → flipped Metal viewport (see the file header); the scissor rectangle is the same rows
    int vp[4] = {S.vp[0], S.vp[1], S.vp[2], S.vp[3]};
    if (!vp[2] && !vp[3]) { vp[2] = P.w; vp[3] = P.h; }
    if (memcmp(vp, E.vp, sizeof vp)) { memcpy(E.vp, vp, sizeof vp); [e setViewport:(MTLViewport){vp[0], vp[1] + vp[3], vp[2], -vp[3], 0, 1}]; }
    int sc[4] = {0, 0, (int)P.w, (int)P.h};
    if (S.scissor) {
        int x0 = S.sc[0] < 0 ? 0 : S.sc[0], y0 = S.sc[1] < 0 ? 0 : S.sc[1], x1 = S.sc[0] + S.sc[2], y1 = S.sc[1] + S.sc[3];
        if (x1 > (int)P.w) x1 = P.w; if (y1 > (int)P.h) y1 = P.h;
        sc[0] = x0 < (int)P.w ? x0 : (int)P.w; sc[1] = y0 < (int)P.h ? y0 : (int)P.h; sc[2] = x1 > sc[0] ? x1 - sc[0] : 0; sc[3] = y1 > sc[1] ? y1 - sc[1] : 0;
    }
    if (memcmp(sc, E.sc, sizeof sc)) { memcpy(E.sc, sc, sizeof sc); [e setScissorRect:(MTLScissorRect){sc[0], sc[1], sc[2], sc[3]}]; }
    int cull = !S.cull ? MTLCullModeNone : S.cull_face == GL_FRONT ? MTLCullModeFront : S.cull_face == GL_BACK ? MTLCullModeBack : MTLCullModeNone;
    if (S.cull && S.cull_face == GL_FRONT_AND_BACK) RED1("culling front and back faces not implemented");
    if (cull != E.cull) { E.cull = cull; [e setCullMode:(MTLCullMode)cull]; }
    int wind = S.front == GL_CW ? MTLWindingCounterClockwise : MTLWindingClockwise;   // GL's winding, reversed by the y flip
    if (E.wind != wind) { E.wind = wind; [e setFrontFacingWinding:(MTLWinding)wind]; }
    float pf = S.poffset ? S.pfactor : 0, pu = S.poffset ? S.punits : 0;
    if (pf != E.pf || pu != E.pu) { E.pf = pf; E.pu = pu; [e setDepthBias:pu slopeScale:pf clamp:0]; }
    if (S.stencil && (int)(S.sref & 0xff) != E.sref) { E.sref = S.sref & 0xff; [e setStencilReferenceValue:E.sref]; }
    id<MTLDepthStencilState> ds = dss_get();
    if ((__bridge void *)ds != E.dss) { E.dss = (__bridge void *)ds; [e setDepthStencilState:ds]; }
}
static void bind_uniforms_textures(prog_t *p) {
    id<MTLRenderCommandEncoder> e = g_enc;
    for (int st = 0; st < 2; st++) {
        const shinfo *si = p->si[st];
        if (si->ub_slot >= 0 && si->ub_size > 0 && (E.prog != p->serial || E.ub_gen[st] != p->ub_gen[st])) {
            if (si->ub_size <= 4096) {
                if (st) [e setFragmentBytes:p->ub[st] length:si->ub_size atIndex:si->ub_slot]; else [e setVertexBytes:p->ub[st] length:si->ub_size atIndex:si->ub_slot];
            } else {
                size_t off; id<MTLBuffer> b = arena_alloc(si->ub_size, &off); memcpy((uint8_t *)b.contents + off, p->ub[st], si->ub_size);
                if (st) [e setFragmentBuffer:b offset:off atIndex:si->ub_slot]; else [e setVertexBuffer:b offset:off atIndex:si->ub_slot];
            }
            E.ub_gen[st] = p->ub_gen[st];
        }
    }
    for (int i = 0; i < p->nu; i++) {
        puni_t *u = &p->u[i];
        if (!is_samp(u->type)) continue;
        int unit = u->unit < 0 || u->unit >= NUNITS ? 0 : u->unit;
        GLenum target = u->type == T_SAMPCUBE ? GL_TEXTURE_CUBE_MAP : u->type == T_SAMP3D ? GL_TEXTURE_3D : u->type == T_SAMP2DARR ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
        tex_t *t = tex_of(*unit_slot(unit, target));
        id<MTLTexture> tx = nil; id<MTLSamplerState> sm = g_samp0;
        if (t && (t->made || tex_make(t, 0)) && t->tex) { tx = t->tex; sm = tex_sampler(t); t->use = g_serial; }
        if (!tx) tx = u->type == T_SAMPCUBE ? g_cube0 : u->type == T_SAMP3D ? g_tex3d0 : u->type == T_SAMP2DARR ? g_arr0 : g_tex0;
        for (int st = 0; st < 2; st++) {
            if (u->slot[st] < 0) continue;
            if (st) { [e setFragmentTexture:tx atIndex:u->slot[st]]; [e setFragmentSamplerState:sm atIndex:u->slot[st]]; }
            else { [e setVertexTexture:tx atIndex:u->slot[st]]; [e setVertexSamplerState:sm atIndex:u->slot[st]]; }
        }
    }
    E.prog = p->serial;
}
// Everything a draw needs; 0 = skip it
static int draw_setup(void) {
    prog_t *p = prog_of(S.prog);
    if (!p || !p->linked) { RED1("draw with no usable program (%u)", S.prog); return 0; }
    if (!pass_ensure()) return 0;
    pkey_t k; memset(&k, 0, sizeof k);
    vao_t *v = cur_vao(); const shinfo *vs = p->si[0];
    for (int i = 0; i < vs->na; i++) {
        int s = vs->a[i].slot; vattr_t *a = &v->a[p->amap[s]];
        if (!a->on) continue;
        buf_t *b = buf_of(a->buf);
        if (!b || !b->mb) { RED1("vertex attribute %d reads client memory or a buffer with no data (not implemented)", p->amap[s]); return 0; }
        k.vf[s] = (uint32_t)vfmt_of(a);
        k.stride[s] = a->stride ? a->stride : a->size * vtype_bytes(a->type); k.div[s] = a->div;
        if (!k.vf[s] || (k.stride[s] & 3)) { RED1("vertex attribute %d: type %#x size %d stride %d not mapped", p->amap[s], a->type, a->size, a->stride); return 0; }
    }
    k.cpf = P.cpf; k.dpf = P.dpf; k.spf = P.spf; k.mask = S.cmask;
    if (S.blend && P.cpf) { k.blend = 1; k.bsrc = bcode(S.bsrc); k.bdst = bcode(S.bdst); k.bsa = bcode(S.bsa); k.bda = bcode(S.bda); k.eqc = beq_of(S.eqc); k.eqa = beq_of(S.eqa); }
    id<MTLRenderPipelineState> pso = pso_get(p, &k);
    if (!pso) return 0;
    id<MTLRenderCommandEncoder> e = g_enc;
    if ((__bridge void *)pso != E.pso) { E.pso = (__bridge void *)pso; [e setRenderPipelineState:pso]; }
    enc_state();
    for (int i = 0; i < vs->na; i++) {
        int s = vs->a[i].slot, gi = p->amap[s]; vattr_t *a = &v->a[gi];
        if (a->on) { buf_t *b = buf_of(a->buf); b->use = g_serial; [e setVertexBuffer:b->mb offset:a->off atIndex:30 - s]; }
        else [e setVertexBytes:S.va[gi] length:16 atIndex:30 - s];
    }
    bind_uniforms_textures(p);
    g_n_draw++;
    return 1;
}
static MTLPrimitiveType prim_of(GLenum m, int *conv) {
    *conv = 0;
    switch (m) {
        case GL_POINTS: return MTLPrimitiveTypePoint; case GL_LINES: return MTLPrimitiveTypeLine; case GL_LINE_STRIP: return MTLPrimitiveTypeLineStrip;
        case GL_TRIANGLES: return MTLPrimitiveTypeTriangle; case GL_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
        case GL_LINE_LOOP: *conv = 1; return MTLPrimitiveTypeLineStrip;
        case GL_TRIANGLE_FAN: *conv = 2; return MTLPrimitiveTypeTriangle;
        case GL_QUADS: *conv = 3; return MTLPrimitiveTypeTriangle;
    }
    RED1("primitive mode %#x not implemented", m); *conv = -1; return MTLPrimitiveTypeTriangle;
}
// Indices of a mode Metal does not have (line loop, triangle fan, quads) as a list, from the given indices (or 0 … n−1); returns the new count
static uint32_t idx_convert(int conv, const uint32_t *in, uint32_t n, uint32_t base, uint32_t *out) {
    uint32_t m = 0;
#define IX(i) (in ? in[i] : base + (i))
    if (conv == 1) { for (uint32_t i = 0; i < n; i++) out[m++] = IX(i); if (n) out[m++] = IX(0); }
    else if (conv == 2) for (uint32_t i = 2; i < n; i++) { out[m++] = IX(0); out[m++] = IX(i - 1); out[m++] = IX(i); }
    else if (conv == 3) for (uint32_t i = 0; i + 3 < n; i += 4) { out[m++] = IX(i); out[m++] = IX(i + 1); out[m++] = IX(i + 2); out[m++] = IX(i); out[m++] = IX(i + 2); out[m++] = IX(i + 3); }
#undef IX
    return m;
}
static void draw_arrays(GLenum mode, GLint first, GLsizei count, GLsizei inst) {
    if (count <= 0 || inst <= 0) return;
    @autoreleasepool {
        int conv; MTLPrimitiveType pt = prim_of(mode, &conv);
        if (conv < 0 || !draw_setup()) return;
        if (!conv) { [g_enc drawPrimitives:pt vertexStart:first vertexCount:count instanceCount:inst]; return; }
        size_t off; id<MTLBuffer> b = arena_alloc((size_t)count * 6 * 4 + 16, &off);
        uint32_t n = idx_convert(conv, NULL, count, first, (uint32_t *)((uint8_t *)b.contents + off));
        if (n) [g_enc drawIndexedPrimitives:pt indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:b indexBufferOffset:off instanceCount:inst];
    }
}
// base: added to every index (glDrawElementsBaseVertex: LÖVE draws its batched quads with one shared index buffer so)
static void draw_elements(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base, GLsizei inst) {
    if (count <= 0 || inst <= 0) return;
    @autoreleasepool {
        int conv; MTLPrimitiveType pt = prim_of(mode, &conv);
        if (conv < 0) return;
        buf_t *eb = buf_of(cur_vao()->ebo);
        uintptr_t io = (uintptr_t)indices; int isz = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
        const uint8_t *src = eb ? (eb->mb ? (const uint8_t *)eb->mb.contents + io : NULL) : (const uint8_t *)indices;
        if (!src || (eb && io + (size_t)count * isz > eb->size)) { RED1("glDrawElements: indices outside their buffer"); return; }
        if (!draw_setup()) return;
        if (!conv && isz > 1 && eb && io % 4 == 0) {   // the game's own index buffer
            eb->use = g_serial;
            [g_enc drawIndexedPrimitives:pt indexCount:count indexType:isz == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:eb->mb indexBufferOffset:io
                           instanceCount:inst baseVertex:base baseInstance:0];
            return;
        }
        // client indices, 8-bit indices, an unaligned offset or a converted mode: 32-bit indices in transient memory
        size_t off; id<MTLBuffer> b = arena_alloc((size_t)count * 4 * (conv ? 6 : 1) + 16, &off);
        uint32_t *tmp = conv ? malloc((size_t)count * 4) : (uint32_t *)((uint8_t *)b.contents + off);
        for (GLsizei i = 0; i < count; i++) tmp[i] = isz == 4 ? ((const uint32_t *)src)[i] : isz == 2 ? ((const uint16_t *)src)[i] : src[i];
        uint32_t n = count;
        if (conv) { n = idx_convert(conv, tmp, count, 0, (uint32_t *)((uint8_t *)b.contents + off)); free(tmp); }
        if (n) [g_enc drawIndexedPrimitives:pt indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:b indexBufferOffset:off instanceCount:inst baseVertex:base baseInstance:0];
    }
}
GLB(glDrawArrays, void, GLenum mode, GLint first, GLsizei count) { draw_arrays(mode, first, count, 1); }
GLB(glDrawArraysInstanced, void, GLenum mode, GLint first, GLsizei count, GLsizei inst) { draw_arrays(mode, first, count, inst); }
GLB(glDrawElements, void, GLenum mode, GLsizei count, GLenum type, const void *indices) { draw_elements(mode, count, type, indices, 0, 1); }
GLB(glDrawElementsBaseVertex, void, GLenum mode, GLsizei count, GLenum type, const void *indices, GLint base) { draw_elements(mode, count, type, indices, base, 1); }
GLB(glDrawElementsInstanced, void, GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei inst) { draw_elements(mode, count, type, indices, 0, inst); }
GLB(glDrawElementsInstancedBaseVertex, void, GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei inst, GLint base) { draw_elements(mode, count, type, indices, base, inst); }

// ---------- Clears ----------
// A clear the whole framebuffer takes (no scissor, nothing masked) becomes the next pass's load action; any other is a quad, which keeps the scissor rectangle and the masks
GLB(glClear, void, GLbitfield mask) {
    @autoreleasepool {
        uint32_t fw, fh; fb_size(S.fb, &fw, &fh);
        int full = !S.scissor || (S.sc[0] <= 0 && S.sc[1] <= 0 && S.sc[0] + S.sc[2] >= (int)fw && S.sc[1] + S.sc[3] >= (int)fh);
        int bits = mask & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        if ((bits & GL_COLOR_BUFFER_BIT) && S.cmask != 0xf) full = 0;
        if ((bits & GL_DEPTH_BUFFER_BIT) && !S.dmask) bits &= ~GL_DEPTH_BUFFER_BIT;
        if ((bits & GL_STENCIL_BUFFER_BIT) && (S.swmask & 0xff) != 0xff) full = 0;
        if (!bits) return;
        if (full) {
            if (g_pend_clear.bits && g_pend_clear.fb != S.fb) clears_flush();
            if (P.open) pass_end();   // a new pass for the draws after it
            g_pend_clear.fb = S.fb; g_pend_clear.bits |= bits; memcpy(g_pend_clear.c, S.clear_c, sizeof S.clear_c); g_pend_clear.d = S.clear_d; g_pend_clear.s = S.clear_s;
            return;
        }
        if (!pass_ensure()) return;
        static __strong id<MTLRenderPipelineState> cache[16]; static uint64_t ck[16]; static int nc;
        uint64_t key = (uint64_t)P.cpf | (uint64_t)P.dpf << 16 | (uint64_t)P.spf << 32 | (uint64_t)((bits & GL_COLOR_BUFFER_BIT) ? S.cmask : 0) << 48;
        id<MTLRenderPipelineState> pso = nil;
        for (int i = 0; i < nc; i++) if (ck[i] == key) pso = cache[i];
        if (!pso && g_util) {
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [g_util newFunctionWithName:@"clr_vs"];
            pd.fragmentFunction = [g_util newFunctionWithName:P.cpf ? @"clr_fs" : @"clr_fs_none"];
            pd.colorAttachments[0].pixelFormat = P.cpf;
            int m = (bits & GL_COLOR_BUFFER_BIT) ? S.cmask : 0;
            pd.colorAttachments[0].writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
            pd.depthAttachmentPixelFormat = P.dpf; pd.stencilAttachmentPixelFormat = P.spf;
            NSError *err = nil; pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
            if (!pso) { RED1("clear pipeline: %s", err.localizedDescription.UTF8String); return; }
            if (nc < 16) { ck[nc] = key; cache[nc++] = pso; }
        }
        if (!pso) return;
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        if ((bits & GL_DEPTH_BUFFER_BIT) && P.dpf) { dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = YES; }
        if ((bits & GL_STENCIL_BUFFER_BIT) && P.spf) {
            MTLStencilDescriptor *s = [MTLStencilDescriptor new]; s.stencilCompareFunction = MTLCompareFunctionAlways; s.depthStencilPassOperation = MTLStencilOperationReplace;
            s.writeMask = S.swmask & 0xff; dd.frontFaceStencil = s; dd.backFaceStencil = s;
        }
        id<MTLRenderCommandEncoder> e = g_enc;
        [e setRenderPipelineState:pso]; [e setDepthStencilState:[g_dev newDepthStencilStateWithDescriptor:dd]];
        [e setCullMode:MTLCullModeNone]; [e setStencilReferenceValue:S.clear_s & 0xff];
        [e setViewport:(MTLViewport){0, 0, P.w, P.h, 0, 1}];
        int sc[4] = {0, 0, (int)P.w, (int)P.h};
        if (S.scissor) {
            int x0 = S.sc[0] < 0 ? 0 : S.sc[0], y0 = S.sc[1] < 0 ? 0 : S.sc[1], x1 = S.sc[0] + S.sc[2], y1 = S.sc[1] + S.sc[3];
            if (x1 > (int)P.w) x1 = P.w; if (y1 > (int)P.h) y1 = P.h;
            sc[0] = x0 < (int)P.w ? x0 : (int)P.w; sc[1] = y0 < (int)P.h ? y0 : (int)P.h; sc[2] = x1 > sc[0] ? x1 - sc[0] : 0; sc[3] = y1 > sc[1] ? y1 - sc[1] : 0;
        }
        [e setScissorRect:(MTLScissorRect){sc[0], sc[1], sc[2], sc[3]}];
        float z = (float)S.clear_d; [e setVertexBytes:&z length:4 atIndex:0];
        [e setFragmentBytes:S.clear_c length:16 atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        enc_reset();   // the next draw sets its own state again
    }
}
GL(glClearColor, void, GLfloat r, GLfloat g, GLfloat b, GLfloat a) { S.clear_c[0] = r; S.clear_c[1] = g; S.clear_c[2] = b; S.clear_c[3] = a; }
GL(glClearDepth, void, GLdouble d) { S.clear_d = d < 0 ? 0 : d > 1 ? 1 : d; }
GL(glClearDepthf, void, GLfloat d) { S.clear_d = d < 0 ? 0 : d > 1 ? 1 : d; }
GL(glClearStencil, void, GLint s) { S.clear_s = s; }
// One buffer cleared to a value of its own, the clear values left as they were (LÖVE clears a canvas so when it is given a colour per canvas); colour attachment 0 only
GLB(glClearBufferfv, void, GLenum buffer, GLint drawbuffer, const GLfloat *v) {
    if (buffer == GL_COLOR && drawbuffer == 0) { float keep[4]; memcpy(keep, S.clear_c, sizeof keep); memcpy(S.clear_c, v, sizeof keep); h_glClear(GL_COLOR_BUFFER_BIT); memcpy(S.clear_c, keep, sizeof keep); }
    else if (buffer == GL_DEPTH) { double keep = S.clear_d; S.clear_d = v[0] < 0 ? 0 : v[0] > 1 ? 1 : v[0]; h_glClear(GL_DEPTH_BUFFER_BIT); S.clear_d = keep; }
    else RED1("glClearBufferfv(%#x, %d) not implemented", buffer, drawbuffer);
}

// ---------- State ----------
static uint8_t *cap_of(GLenum cap) {
    switch (cap) { case GL_DEPTH_TEST: return &S.depth; case GL_BLEND: return &S.blend; case GL_CULL_FACE: return &S.cull; case GL_SCISSOR_TEST: return &S.scissor;
                   case GL_STENCIL_TEST: return &S.stencil; case GL_POLYGON_OFFSET_FILL: return &S.poffset; }
    return NULL;
}
static void cap_set(GLenum cap, int on) {
    uint8_t *c = cap_of(cap);
    if (c) { *c = on; return; }
    if (cap == GL_COLOR_TABLE) { S.color_table = on; return; }
    if (cap == GL_FRAMEBUFFER_SRGB) { if (on && !S.srgb) RED1("GL_FRAMEBUFFER_SRGB: writes into sRGB attachments are not converted"); S.srgb = on; return; }
    if (cap == GL_TEXTURE_2D || cap == 0x809d /* MULTISAMPLE */ || cap == 0x8642 /* PROGRAM_POINT_SIZE */ || cap == 0x8d69 /* PRIMITIVE_RESTART_FIXED_INDEX */ || cap == 0x884f /* TEXTURE_CUBE_MAP_SEAMLESS */
        || cap == 0x92e0 /* DEBUG_OUTPUT */ || cap == 0x8242 /* DEBUG_OUTPUT_SYNCHRONOUS */) return;
    static GLenum seen[32]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == cap) return;
    if (n < 32) { seen[n++] = cap; GLOG("[RED] gl%s(%#x) not implemented", on ? "Enable" : "Disable", cap); }
}
GL(glEnable, void, GLenum cap) { cap_set(cap, 1); }
GL(glDisable, void, GLenum cap) { cap_set(cap, 0); }
GL(glColorMask, void, uint32_t r, uint32_t g, uint32_t b, uint32_t a) { S.cmask = (r & 0xff ? 1 : 0) | (g & 0xff ? 2 : 0) | (b & 0xff ? 4 : 0) | (a & 0xff ? 8 : 0); }
GL(glDepthMask, void, uint32_t m) { S.dmask = (m & 0xff) != 0; }
GL(glDepthFunc, void, GLenum f) { if (f >= GL_NEVER && f <= GL_NEVER + 7) S.dfunc = f; }
GL(glBlendFunc, void, GLenum s, GLenum d) { S.bsrc = S.bsa = s; S.bdst = S.bda = d; }
GL(glBlendFuncSeparate, void, GLenum sc, GLenum dc, GLenum sa, GLenum da) { S.bsrc = sc; S.bdst = dc; S.bsa = sa; S.bda = da; }
GL(glBlendEquation, void, GLenum e) { S.eqc = S.eqa = e; }
GL(glBlendEquationSeparate, void, GLenum c, GLenum a) { S.eqc = c; S.eqa = a; }
GL(glCullFace, void, GLenum m) { S.cull_face = m; }
GL(glFrontFace, void, GLenum m) { if (m == GL_CW || m == GL_CCW) S.front = m; }
// Points are as big as the vertex shader's point size output says (Metal has no fixed size); wireframe has no Metal counterpart in this layer
GL(glPointSize, void, GLfloat s) { S.psize = s; }
GL(glPolygonMode, void, GLenum face, GLenum mode) { (void)face; if (mode != 0x1b02 /* FILL */) RED1("glPolygonMode(%#x) not implemented (filled)", mode); }
GL(glHint, void, GLenum target, GLenum mode) { (void)target; (void)mode; }
GL(glStencilFunc, void, GLenum f, GLint ref, GLuint mask) { if (f >= GL_NEVER && f <= GL_NEVER + 7) S.sfunc = f; S.sref = ref; S.smask = mask; }
GL(glStencilMask, void, GLuint m) { S.swmask = m; }
GL(glStencilOp, void, GLenum sf, GLenum zf, GLenum zp) { S.sfail = sf; S.szfail = zf; S.spass = zp; }
GL(glPolygonOffset, void, GLfloat f, GLfloat u) { S.pfactor = f; S.punits = u; }
GL(glViewport, void, GLint x, GLint y, GLsizei w, GLsizei h) { S.vp[0] = x; S.vp[1] = y; S.vp[2] = w; S.vp[3] = h; }
GL(glScissor, void, GLint x, GLint y, GLsizei w, GLsizei h) { S.sc[0] = x; S.sc[1] = y; S.sc[2] = w; S.sc[3] = h; }
GL(glPixelStorei, void, GLenum p, GLint v) {
    if (p == GL_UNPACK_ALIGNMENT && (v == 1 || v == 2 || v == 4 || v == 8)) S.unpack_align = v;
    else if (p == GL_UNPACK_ROW_LENGTH) S.unpack_row = v;
    else if (p == GL_PACK_ALIGNMENT && (v == 1 || v == 2 || v == 4 || v == 8)) S.pack_align = v;
    else if (p >= 0xcf0 && p <= 0xcf4 && !v) {}   // SWAP_BYTES, LSB_FIRST, SKIP_ROWS, SKIP_PIXELS at their defaults
    else RED1("glPixelStorei(%#x, %d) not implemented", p, v);
}
// Colour-index images through a palette (GL's compatibility pixel transfer: glColorTable, GL_INDEX_SHIFT / GL_INDEX_OFFSET); the game's texture loader sets a palette for its
// paletted images. glTexEnvi is fixed-function texturing, which programs replace
GL(glPixelTransferi, void, GLenum p, GLint v) { if (p == GL_INDEX_SHIFT) S.index_shift = v; else if (p == GL_INDEX_OFFSET) S.index_offset = v; else RED1("glPixelTransferi(%#x) not implemented", p); }
GL(glColorTable, void, GLenum target, GLenum ifmt, GLsizei w, GLenum format, GLenum type, const void *table) {
    if (target != GL_COLOR_TABLE || type != GL_UNSIGNED_BYTE || (format != GL_RGBA && format != GL_RGB) || w <= 0 || !table) { RED1("glColorTable(%#x, %#x, %d, %#x, %#x) not implemented", target, ifmt, w, format, type); return; }
    S.npalette = w > 256 ? 256 : w; const uint8_t *s = table;
    for (int i = 0; i < S.npalette; i++) S.palette[i] = format == GL_RGBA ? (uint32_t)s[4 * i] | s[4 * i + 1] << 8 | s[4 * i + 2] << 16 | (uint32_t)s[4 * i + 3] << 24 : (uint32_t)s[3 * i] | s[3 * i + 1] << 8 | s[3 * i + 2] << 16 | 0xff000000u;
    RED1("paletted texture (glColorTable, %d entries): palette lookup at upload, the index shift is %d", w, S.index_shift);
}
GL(glTexEnvi, void, GLenum t, GLenum p, GLint v) { (void)t; (void)p; (void)v; }
GL(glGetError, GLenum, void) { return 0; }
// Everything recorded so far runs to its end (LÖVE when the window changes)
GLB(glFinish, void, void) { if (g_pend_clear.bits) clears_flush(); dm_gfx_enter(); id<MTLCommandBuffer> cb = cb_get(); cb_commit(); [cb waitUntilCompleted]; dm_gfx_leave(); }
// Debug output and labels (LÖVE's debug groups and its GL debug callback, used when the game asks for debug output): none here
GL(glDebugMessageCallback, void, void *cb, const void *user) { (void)cb; (void)user; }
GL(glDebugMessageControl, void, GLenum src, GLenum type, GLenum sev, GLsizei n, const GLuint *ids, uint32_t on) { (void)src; (void)type; (void)sev; (void)n; (void)ids; (void)on; }
GL(glPushDebugGroup, void, GLenum src, GLuint id, GLsizei len, const GLchar *msg) { (void)src; (void)id; (void)len; (void)msg; }
GL(glPopDebugGroup, void, void) {}
GL(glPushGroupMarkerEXT, void, GLsizei len, const GLchar *msg) { (void)len; (void)msg; }
GL(glPopGroupMarkerEXT, void, void) {}
GL(glObjectLabel, void, GLenum id, GLuint name, GLsizei len, const GLchar *label) { (void)id; (void)name; (void)len; (void)label; }

// ---------- Queries ----------
static const char *const g_exts[] = {"GL_ARB_ES3_2_compatibility", "GL_ARB_ES3_compatibility", "GL_ARB_texture_compression_bptc", "GL_ARB_texture_compression_rgtc",
    "GL_EXT_texture_compression_s3tc", "GL_EXT_texture_filter_anisotropic", "GL_ARB_vertex_array_object", "GL_ARB_direct_state_access", "GL_ARB_half_float_vertex",
    "GL_ARB_framebuffer_object", "GL_ARB_texture_float", "GL_EXT_texture_sRGB", "GL_KHR_texture_compression_astc_ldr", "GL_NV_shadow_samplers_cube"};
#define NEXTS (int)(sizeof g_exts / sizeof *g_exts)
GL(glGetString, const char *, GLenum n) {
    switch (n) {
        case GL_VENDOR: return "NVIDIA Corporation";
        case GL_RENDERER: return "NVIDIA Tegra X1 (nvgpu)/integrated (Metal)";
        case GL_VERSION: return "4.5.0 NVIDIA 349.00";
        case GL_SHADING_LANGUAGE_VERSION: return "4.50 NVIDIA";
        case GL_EXTENSIONS: {   // the compatibility profile's one string
            static char all[1024]; if (!all[0]) for (int i = 0; i < NEXTS; i++) { strlcat(all, g_exts[i], sizeof all); strlcat(all, " ", sizeof all); }
            return all;
        }
    }
    RED1("glGetString(%#x) not implemented", n); return "";
}
GL(glGetStringi, const char *, GLenum n, GLuint i) { return n == GL_EXTENSIONS && i < (GLuint)NEXTS ? g_exts[i] : NULL; }
GL(glGetIntegerv, void, GLenum p, GLint *v) {
    switch (p) {
        case GL_NUM_EXTENSIONS: *v = NEXTS; return;
        case GL_MAJOR_VERSION: *v = 4; return;
        case GL_MINOR_VERSION: *v = 5; return;
        case GL_CONTEXT_PROFILE_MASK: *v = 1; return;   // CONTEXT_CORE_PROFILE_BIT: what the games asked for (Digimon a 4.5 core context); LÖVE takes its core paths (the file header)
        case GL_CONTEXT_FLAGS: *v = 0; return;
        case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: case GL_MAX_CUBE_MAP_TEXTURE_SIZE: *v = 16384; return;
        case GL_MAX_3D_TEXTURE_SIZE: case GL_MAX_ARRAY_TEXTURE_LAYERS: *v = 2048; return;
        case GL_MAX_VIEWPORT_DIMS: v[0] = v[1] = 16384; return;
        case GL_MAX_COLOR_ATTACHMENTS: case GL_MAX_DRAW_BUFFERS: *v = 1; return;   // one colour attachment (fb_t)
        case GL_MAX_VERTEX_ATTRIBS: *v = 16; return;
        case GL_MAX_TEXTURE_IMAGE_UNITS: *v = 32; return;
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *v = NUNITS; return;
        case GL_MAX_VERTEX_UNIFORM_VECTORS: case GL_MAX_FRAGMENT_UNIFORM_VECTORS: *v = 1024; return;
        case GL_MAX_VARYING_VECTORS: *v = 31; return;
        case GL_MAX_SAMPLES: *v = 8; return;
        case GL_SAMPLE_BUFFERS: case GL_SAMPLES: *v = 0; return;   // the window surface is not multisampled
        case GL_VIEWPORT: memcpy(v, S.vp, sizeof S.vp); return;
        case GL_SCISSOR_BOX: memcpy(v, S.sc, sizeof S.sc); return;
        case GL_CULL_FACE_MODE: *v = S.cull_face; return;
        case GL_FRONT_FACE: *v = S.front; return;
        case GL_FRAMEBUFFER_BINDING: *v = S.fb; return;   // and DRAW_FRAMEBUFFER_BINDING
        case GL_READ_FRAMEBUFFER_BINDING: *v = S.read_fb; return;
        case GL_RENDERBUFFER_BINDING: *v = S.rb; return;
        case GL_CURRENT_PROGRAM: *v = S.prog; return;
        case 0x8894: *v = S.array_buf; return;   // ARRAY_BUFFER_BINDING
        case 0x8895: *v = cur_vao()->ebo; return;   // ELEMENT_ARRAY_BUFFER_BINDING
        case 0x85b5: *v = S.vao; return;   // VERTEX_ARRAY_BINDING
        case 0x8069: *v = S.units[S.unit].t2d; return;   // TEXTURE_BINDING_2D
        case GL_TEXTURE_BINDING_CUBE_MAP: *v = S.units[S.unit].tcube; return;
        case GL_TEXTURE_BINDING_3D: *v = S.units[S.unit].t3d; return;
        case GL_TEXTURE_BINDING_2D_ARRAY: *v = S.units[S.unit].t2da; return;
        case 0x84e0: *v = GL_TEXTURE0 + S.unit; return;   // ACTIVE_TEXTURE
        case GL_UNPACK_ALIGNMENT: *v = S.unpack_align; return;
        case GL_PACK_ALIGNMENT: *v = S.pack_align; return;
    }
    static GLenum seen[32]; static int n;
    int known = 0; for (int i = 0; i < n; i++) known |= seen[i] == p;
    if (!known && n < 32) { seen[n++] = p; GLOG("[RED] glGetIntegerv(%#x) not implemented (0)", p); }
    *v = 0;
}
GL(glGetFloatv, void, GLenum p, GLfloat *v) {
    switch (p) {
        case GL_MAX_TEXTURE_LOD_BIAS: *v = 15; return;
        case GL_MAX_TEXTURE_MAX_ANISOTROPY: *v = 16; return;
        case GL_VIEWPORT: for (int i = 0; i < 4; i++) v[i] = (float)S.vp[i]; return;
        case GL_POINT_SIZE: *v = S.psize; return;
        case GL_POINT_SIZE_RANGE: case GL_ALIASED_POINT_SIZE_RANGE: v[0] = 1; v[1] = 511; return;   // Metal's largest point
    }
    RED1("glGetFloatv(%#x) not implemented (0)", p); *v = 0;
}

// ---------- Buffers and vertex arrays ----------
GL(glGenBuffers, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_bufs, &g_nbufs, &g_capbufs, sizeof(buf_t)); }
GL(glDeleteBuffers, void, GLsizei n, const GLuint *b) {
    for (GLsizei i = 0; i < n; i++) { buf_t *x = buf_of(b[i]); if (!x) continue; x->mb = nil; free(x); g_bufs[b[i]] = NULL; if (S.array_buf == b[i]) S.array_buf = 0; if (cur_vao()->ebo == b[i]) cur_vao()->ebo = 0; }
}
GL(glBindBuffer, void, GLenum target, GLuint b) {
    if (target == GL_ARRAY_BUFFER) S.array_buf = b;
    else if (target == GL_ELEMENT_ARRAY_BUFFER) cur_vao()->ebo = b;
    else RED1("glBindBuffer target %#x not implemented", target);
}
static buf_t *bound_buf(GLenum target) { return buf_of(target == GL_ARRAY_BUFFER ? S.array_buf : target == GL_ELEMENT_ARRAY_BUFFER ? cur_vao()->ebo : 0); }
// Orphaning: data for a buffer a pending command buffer may still read goes into a new MTLBuffer, so draws already encoded keep the old contents
GLB(glBufferData, void, GLenum target, GLsizeiptr size, const void *data, GLenum usage) {
    (void)usage;
    buf_t *b = bound_buf(target);
    if (!b) { RED1("glBufferData with no buffer bound to %#x", target); return; }
    if (b->immutable) { RED1("glBufferData on a buffer glBufferStorage made"); return; }
    if (size <= 0) { b->mb = nil; b->size = 0; return; }
    @autoreleasepool {
        if (!b->mb || b->mb.length < (NSUInteger)size || b->use > atomic_load(&g_done) || b->mb.length > (NSUInteger)size * 2 + 4096)
            b->mb = [g_dev newBufferWithLength:(size + 15) & ~(GLsizeiptr)15 options:MTLResourceStorageModeShared];
        b->size = size; b->use = 0;
        if (data) memcpy(b->mb.contents, data, size); else memset(b->mb.contents, 0, size);
    }
}
// Immutable storage that stays mapped: the game writes into the MTLBuffer's own memory and fences what the GPU still reads (LÖVE's StreamBufferPersistentMapSync)
GLB(glBufferStorage, void, GLenum target, GLsizeiptr size, const void *data, GLbitfield flags) {
    (void)flags; buf_t *b = bound_buf(target);
    if (!b || size <= 0) { RED1("glBufferStorage(%#x, %ld) with no buffer bound", target, (long)size); return; }
    @autoreleasepool { b->mb = [g_dev newBufferWithLength:(size + 15) & ~(GLsizeiptr)15 options:MTLResourceStorageModeShared]; }
    b->size = size; b->use = 0; b->immutable = 1;
    if (data) memcpy(b->mb.contents, data, size);
}
// In command order: new bytes for a buffer a pending command buffer may still read go into a copy of it, which later draws use (the draws already encoded keep the old one)
GLB(glBufferSubData, void, GLenum target, GLintptr off, GLsizeiptr size, const void *data) {
    buf_t *b = bound_buf(target);
    if (!b || !b->mb || off < 0 || size < 0 || (size_t)(off + size) > b->size) { RED1("glBufferSubData(%#x, %ld, %ld) outside the bound buffer", target, (long)off, (long)size); return; }
    if (b->use > atomic_load(&g_done)) {
        if (b->immutable) RED1("glBufferSubData into a buffer glBufferStorage made while the GPU may still read it");
        else { @autoreleasepool { id<MTLBuffer> nb = [g_dev newBufferWithLength:b->mb.length options:MTLResourceStorageModeShared]; memcpy(nb.contents, b->mb.contents, b->size); b->mb = nb; } b->use = 0; }
    }
    if (size) memcpy((uint8_t *)b->mb.contents + off, data, size);
}
// The mapping is the MTLBuffer's own memory: a persistent or unsynchronized mapping is the game's to fence; invalidating a whole buffer in flight orphans it, as glBufferData does
GLB(glMapBufferRange, void *, GLenum target, GLintptr off, GLsizeiptr len, GLbitfield access) {
    buf_t *b = bound_buf(target);
    if (!b || !b->mb || off < 0 || len <= 0 || (size_t)(off + len) > b->size) { RED1("glMapBufferRange(%#x, %ld, %ld) outside the bound buffer", target, (long)off, (long)len); return NULL; }
    if (b->use > atomic_load(&g_done) && !b->immutable && (access & GL_MAP_INVALIDATE_BUFFER_BIT)) {
        @autoreleasepool { b->mb = [g_dev newBufferWithLength:b->mb.length options:MTLResourceStorageModeShared]; } b->use = 0;
    } else if (b->use > atomic_load(&g_done) && !(access & (0x40 /* PERSISTENT */ | 0x20 /* UNSYNCHRONIZED */)))
        RED1("glMapBufferRange of a buffer the GPU may still read: not waited for");
    return (uint8_t *)b->mb.contents + off;
}
GL(glUnmapBuffer, uint32_t, GLenum target) { (void)target; return 1; }
GL(glFlushMappedBufferRange, void, GLenum target, GLintptr off, GLsizeiptr len) { (void)target; (void)off; (void)len; }   // shared memory: nothing to flush

// ---------- Sync objects ----------
// A fence is the serial of the command buffer being recorded when it was made, signalled when that command buffer completes (g_ev). LÖVE fences each third of its stream
// buffers at the end of a frame and waits for the fence three frames later, before it writes that third again
typedef struct { uint64_t serial; } sync_t;
GL(glFenceSync, void *, GLenum cond, GLbitfield flags) { (void)cond; (void)flags; sync_t *s = zalloc(sizeof *s); s->serial = g_serial; return s; }
GL(glDeleteSync, void, void *s) { free(s); }
GL(glWaitSync, void, void *s, GLbitfield flags, uint64_t timeout) { (void)s; (void)flags; (void)timeout; }   // a GPU-side wait: the command buffers run in order
GLB(glClientWaitSync, GLenum, void *sy, GLbitfield flags, uint64_t timeout_ns) {
    sync_t *s = sy;
    if (!s) return 0x911d;   // WAIT_FAILED
    if (g_ev.signaledValue >= s->serial) return GL_ALREADY_SIGNALED;
    if (s->serial >= g_serial) {   // its command buffer is still being recorded: commit it now (as GL_SYNC_FLUSH_COMMANDS_BIT asks; without it the wait could never end)
        if (!timeout_ns && !(flags & GL_SYNC_FLUSH_COMMANDS_BIT)) return GL_TIMEOUT_EXPIRED;
        dm_gfx_enter(); cb_commit(); dm_gfx_leave();
    }
    if (!timeout_ns) return GL_TIMEOUT_EXPIRED;
    uint64_t ms = timeout_ns / 1000000;
    return [g_ev waitUntilSignaledValue:s->serial timeoutMS:ms ? ms : 1] ? GL_CONDITION_SATISFIED : GL_TIMEOUT_EXPIRED;
}
GL(glGenVertexArrays, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_vaos, &g_nvaos, &g_capvaos, sizeof(vao_t)); }
GL(glDeleteVertexArrays, void, GLsizei n, const GLuint *v) { for (GLsizei i = 0; i < n; i++) if (v[i] && v[i] < g_nvaos && g_vaos[v[i]]) { free(g_vaos[v[i]]); g_vaos[v[i]] = NULL; if (S.vao == v[i]) S.vao = 0; } }
GL(glBindVertexArray, void, GLuint v) { S.vao = v; }
GL(glEnableVertexAttribArray, void, GLuint i) { if (i < 16) cur_vao()->a[i].on = 1; }
GL(glDisableVertexAttribArray, void, GLuint i) { if (i < 16) cur_vao()->a[i].on = 0; }
GL(glEnableVertexArrayAttrib, void, GLuint vao, GLuint i) { vao_t *v = vao && vao < g_nvaos && g_vaos[vao] ? g_vaos[vao] : &g_vao0; if (i < 16) v->a[i].on = 1; }
GL(glVertexAttribPointer, void, GLuint i, GLint size, GLenum type, uint32_t norm, GLsizei stride, const void *ptr) {
    if (i >= 16) { RED1("vertex attribute %u over 15", i); return; }
    vattr_t *a = &cur_vao()->a[i]; a->size = size == 0x80e1 ? 4 : size; a->type = type; a->norm = (norm & 0xff) != 0; a->stride = stride; a->off = (uintptr_t)ptr; a->buf = S.array_buf;
}
GL(glVertexAttribDivisor, void, GLuint i, GLuint d) { if (i < 16) cur_vao()->a[i].div = d > 255 ? 255 : (uint8_t)d; }
GL(glVertexAttrib4f, void, GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { if (i < 16) { S.va[i][0] = x; S.va[i][1] = y; S.va[i][2] = z; S.va[i][3] = w; } }
GL(glVertexAttrib4fv, void, GLuint i, const GLfloat *v) { if (i < 16) memcpy(S.va[i], v, sizeof S.va[i]); }

// ---------- Textures ----------
GL(glGenTextures, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_texs, &g_ntexs, &g_captexs, sizeof(tex_t)); }
GL(glDeleteTextures, void, GLsizei n, const GLuint *t) {
    for (GLsizei i = 0; i < n; i++) {
        tex_t *x = tex_of(t[i]); if (!x) continue;
        tex_drop(x); x->smp = nil; free(x); g_texs[t[i]] = NULL;
        for (int u = 0; u < NUNITS; u++) {
            if (S.units[u].t2d == t[i]) S.units[u].t2d = 0; if (S.units[u].tcube == t[i]) S.units[u].tcube = 0;
            if (S.units[u].t3d == t[i]) S.units[u].t3d = 0; if (S.units[u].t2da == t[i]) S.units[u].t2da = 0;
        }
    }
}
GL(glActiveTexture, void, GLenum u) { if (u >= GL_TEXTURE0 && u < GL_TEXTURE0 + NUNITS) S.unit = u - GL_TEXTURE0; }
GL(glBindTexture, void, GLenum target, GLuint t) {
    GLuint *slot = target == GL_TEXTURE_2D || target == GL_TEXTURE_CUBE_MAP || target == GL_TEXTURE_3D || target == GL_TEXTURE_2D_ARRAY ? unit_slot(S.unit, target) : NULL;
    if (!slot) { RED1("texture target %#x not implemented", target); return; }
    tex_t *x = tex_of(t);
    if (t && !x) {   // a name glGenTextures never gave: GL makes the texture on its first bind
        if (t > 1u << 20) { RED1("glBindTexture of name %u", t); return; }
        x = name_at((void ***)&g_texs, &g_ntexs, &g_captexs, t, sizeof(tex_t));
    }
    if (x && !x->target) tex_init_params(x, target);
    *slot = t;
}
static void tex_param(GLenum target, GLenum p, float f, GLint i) {
    tex_t *t = bound_tex(target);
    if (!t) return;
    switch (p) {
        case GL_TEXTURE_MIN_FILTER: t->minf = i; break; case GL_TEXTURE_MAG_FILTER: t->magf = i; break;
        case GL_TEXTURE_WRAP_S: t->ws = i; break; case GL_TEXTURE_WRAP_T: t->wt = i; break; case GL_TEXTURE_WRAP_R: t->wr = i; break;
        case GL_TEXTURE_MAX_LEVEL: t->maxlv = i; break; case GL_TEXTURE_MAX_ANISOTROPY: t->aniso = f; break;
        case GL_TEXTURE_COMPARE_MODE: t->cmp = i ? (t->cmp ? t->cmp : 0x203) : 0; break;   // COMPARE_REF_TO_TEXTURE, the function defaults to LEQUAL
        case GL_TEXTURE_COMPARE_FUNC: if (t->cmp) t->cmp = i; break;
        case GL_TEXTURE_LOD_BIAS: if (f != 0) RED1("GL_TEXTURE_LOD_BIAS %g: Metal samplers have no bias (the shaders pass theirs)", f); break;   // LÖVE sets −(mipmap sharpness), 0 by default
        case 0x813a: case 0x813b: case 0x813c: break;   // MIN_LOD / MAX_LOD / BASE_LEVEL
        case GL_TEXTURE_SWIZZLE_R: case GL_TEXTURE_SWIZZLE_R + 1: case GL_TEXTURE_SWIZZLE_R + 2: case GL_TEXTURE_SWIZZLE_A:   // fixed once the texture is made (tex_make)
            if (t->made && t->swz[p - GL_TEXTURE_SWIZZLE_R] != i) RED1("texture swizzle changed after the texture was made: ignored");
            t->swz[p - GL_TEXTURE_SWIZZLE_R] = i; break;
        default: RED1("glTexParameter(%#x) not implemented", p); break;
    }
    t->smp_dirty = 1;
}
GL(glTexParameteri, void, GLenum target, GLenum p, GLint v) { tex_param(target, p, (float)v, v); }
GL(glTexParameterf, void, GLenum target, GLenum p, GLfloat v) { tex_param(target, p, v, (GLint)v); }
GL(glTexParameterfv, void, GLenum target, GLenum p, const GLfloat *v) {
    if (p == GL_TEXTURE_BORDER_COLOR) { tex_t *t = bound_tex(target); if (t) { memcpy(t->border, v, sizeof t->border); t->smp_dirty = 1; } return; }
    tex_param(target, p, v[0], (GLint)v[0]);
}
// glTexImage2D and glTexSubImage2D take nine arguments, the last on the stack, which the big-stack entry does not carry: they pack them here, on the game's stack, and enter there
typedef struct { GLenum target; GLint level, ifmt, x, y; GLsizei w, h; GLenum format, type; const void *pixels; GLsizei size; int sub, compressed; } teximg_args;
static void teximg_impl(const teximg_args *a) __attribute__((used));
BIGSTUB(teximg_impl);
static void teximg_impl(const teximg_args *a) {
    @autoreleasepool {
        if (a->sub) {
            tex_t *t = bound_tex(a->target);
            int face = a->target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && a->target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (int)(a->target - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
            if (!t || t->fmt < 0 || a->w <= 0 || a->h <= 0) return;
            int f = t->fmt, cv = CV_COPY;   // the data is converted to the texture's format
            if (f == F_RGBA8) cv = a->type == GL_UNSIGNED_SHORT_5_6_5 ? CV_565 : a->type == GL_UNSIGNED_SHORT_4_4_4_4 ? CV_4444 : a->type == GL_UNSIGNED_SHORT_5_5_5_1 ? CV_5551 :
                                   a->format == GL_RGB ? CV_RGB8 : a->format == GL_COLOR_INDEX ? CV_INDEX : CV_COPY;
            size_t pitch; void *conv = pixels_convert(a->pixels, a->w, a->h, a->format, a->type, f, cv, &pitch);
            if (!t->made) {   // into a level not made yet: patch the pending copy
                level_t *l = &t->pend[face][a->level < MAXLV ? a->level : 0];
                if (l->data && a->x + a->w <= (GLint)l->w && a->y + a->h <= (GLint)l->h && g_fmt[f].kind != K_BLOCK)
                    for (GLsizei r = 0; r < a->h; r++) memcpy((uint8_t *)l->data + (a->y + r) * l->pitch + (size_t)a->x * g_fmt[f].bytes, (uint8_t *)conv + r * pitch, (size_t)a->w * g_fmt[f].bytes);
                else tex_make(t, 0);
            }
            if (t->made && t->tex && a->level < t->levels) tex_write(t, face, a->level, a->x, a->y, 0, a->w, a->h, conv, pitch);
            free(conv);
            return;
        }
        int cv, f = fmt_pick(a->ifmt, a->format, a->type, &cv);
        if (f < 0) { static GLint seen[16]; static int n; int k = 0; for (int i = 0; i < n; i++) k |= seen[i] == a->ifmt; if (!k && n < 16) { seen[n++] = a->ifmt; GLOG("[RED] texture internal format %#x (format %#x, type %#x) not mapped", a->ifmt, a->format, a->type); } return; }
        if (cv == CV_NONE && a->pixels) { RED1("texture data of format %#x type %#x into %s not converted (zeros)", a->format, a->type, g_fmt[f].name); }
        if (cv == CV_INDEX && !S.color_table) RED1("colour-index texture data with GL_COLOR_TABLE disabled");
        size_t pitch; void *conv;
        if (a->compressed) {
            size_t need = fmt_pitch(f, a->w) * fmt_rows(f, a->h);
            if (g_fmt[f].kind != K_BLOCK || (size_t)a->size < need) { RED1("glCompressedTexImage2D: %d bytes for %dx%d %s (%zu expected)", a->size, a->w, a->h, g_fmt[f].name, need); return; }
            conv = pixels_convert(a->pixels, a->w, a->h, 0, 0, f, CV_COPY, &pitch);
        } else conv = pixels_convert(cv == CV_NONE ? NULL : a->pixels, a->w, a->h, a->format, a->type, f, cv, &pitch);
        tex_image(a->target, a->level, f, cv, a->w, a->h, a->pixels || a->compressed ? conv : (free(conv), (void *)NULL), pitch);
    }
}
GL(glTexImage2D, void, GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels) {
    (void)border; teximg_args a = {target, level, ifmt, 0, 0, w, h, format, type, pixels, 0, 0, 0}; ((void (*)(const teximg_args *))(const void *)teximg_impl_bs)(&a);
}
GL(glTexSubImage2D, void, GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void *pixels) {
    teximg_args a = {target, level, 0, x, y, w, h, format, type, pixels, 0, 1, 0}; ((void (*)(const teximg_args *))(const void *)teximg_impl_bs)(&a);
}
GL(glCompressedTexImage2D, void, GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border, GLsizei size, const void *data) {
    (void)border; teximg_args a = {target, level, (GLint)ifmt, 0, 0, w, h, 0, 0, data, size, 0, 1}; ((void (*)(const teximg_args *))(const void *)teximg_impl_bs)(&a);
}
// glTexStorage2D / 3D: the format, the size and the level count, before any data (LÖVE makes every texture so, the mipmapped ones with all their levels, which glGenerateMipmap
// fills). The Metal texture is made at its first upload, attachment or sampling, as one glTexImage2D gives (tex_make: shared, or private for a render target)
static void tex_storage(GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d) {
    tex_t *t = bound_tex(target);
    int cv, f = fmt_pick((GLint)ifmt, 0, 0, &cv);
    if (!t || f < 0 || levels <= 0 || w <= 0 || h <= 0 || d <= 0) { RED1("glTexStorage(%#x, %d levels, %#x, %dx%dx%d) not implemented", target, levels, ifmt, w, h, d); return; }
    if (t->made || t->defined) tex_drop(t);
    t->fmt = f; t->w = w; t->h = h; t->d = d; t->slevels = levels; t->smp_dirty = 1;
}
GL(glTexStorage2D, void, GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h) { tex_storage(target, levels, ifmt, w, h, 1); }
GL(glTexStorage3D, void, GLenum target, GLsizei levels, GLenum ifmt, GLsizei w, GLsizei h, GLsizei d) { tex_storage(target, levels, ifmt, w, h, d); }
// 3D and array textures (LÖVE makes a 1x1x1 one of each at start for its default textures): data through glTexSubImage3D, a layer (or a 3D slice) at a time.
// glTexSubImage3D's last three arguments are on the stack: 64-bit here, so the Apple and AAPCS64 stack layouts agree
typedef struct { GLenum target; GLint level, x, y, z; GLsizei w, h, d; GLenum format, type; const void *pixels; } tex3d_args;
static void tex3d_impl(const tex3d_args *a) __attribute__((used));
BIGSTUB(tex3d_impl);
static void tex3d_impl(const tex3d_args *a) {
    @autoreleasepool {
        tex_t *t = bound_tex(a->target);
        if (!t || t->fmt < 0 || a->w <= 0 || a->h <= 0 || a->d <= 0 || !a->pixels) return;
        if (!t->made) tex_make(t, 0);
        if (!t->tex || a->level >= t->levels) return;
        int f = t->fmt, cv = f == F_RGBA8 && a->format == GL_RGB ? CV_RGB8 : CV_COPY;
        size_t pitch; void *conv = pixels_convert(a->pixels, a->w, a->h * a->d, a->format, a->type, f, cv, &pitch);   // the layers one after the other
        size_t layer = pitch * fmt_rows(f, a->h);
        for (GLsizei k = 0; k < a->d; k++) {
            if (t->target == GL_TEXTURE_3D) tex_write(t, 0, a->level, a->x, a->y, a->z + k, a->w, a->h, (uint8_t *)conv + k * layer, pitch);
            else tex_write(t, a->z + k, a->level, a->x, a->y, 0, a->w, a->h, (uint8_t *)conv + k * layer, pitch);
        }
        free(conv);
    }
}
GL(glTexSubImage3D, void, GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, uint64_t format, uint64_t type, const void *pixels) {
    tex3d_args a = {target, level, x, y, z, w, h, d, (GLenum)format, (GLenum)type, pixels}; ((void (*)(const tex3d_args *))(const void *)tex3d_impl_bs)(&a);
}
GL(glTexImage3D, void, GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLint border, GLenum format, uint64_t type, const void *pixels) {
    (void)border;
    if (level) { RED1("glTexImage3D of level %d not implemented", level); return; }
    tex_storage(target, 1, (GLenum)ifmt, w, h, d);
    if (pixels) h_glTexSubImage3D(target, 0, 0, 0, 0, w, h, d, format, type, pixels);
}
// Mipmaps from level 0, in command order (after the draws that rendered level 0 this frame): a texture of one level gets its full chain first, level 0 copied over
static void mip_impl(GLenum target) __attribute__((used));
BIGSTUB(mip_impl);
static void mip_impl(GLenum target) {
    @autoreleasepool {
        tex_t *t = bound_tex(target);
        if (!t || (!t->made && !tex_make(t, 0)) || !t->tex) return;
        if (g_fmt[t->fmt].kind != K_COL) { RED1("glGenerateMipmap of a %s texture not implemented", g_fmt[t->fmt].name); return; }
        id<MTLCommandBuffer> cb;
        if (t->use >= g_serial) { pass_end(); cb = cb_get(); } else { cb = [g_q commandBuffer]; cb.label = @"gl mipmaps"; }
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        if (t->levels == 1 && umax(t->w, t->h) > 1) {
            id<MTLTexture> old = t->tex; t->made = 0; t->tex = nil; t->defined = 0; t->slevels = MAXLV;
            tex_make(t, old.storageMode == MTLStorageModePrivate);
            if (!t->tex) { [b endEncoding]; return; }
            for (int f = 0; f < tex_faces(t); f++) [b copyFromTexture:old sourceSlice:f sourceLevel:0 toTexture:t->tex destinationSlice:f destinationLevel:0 sliceCount:1 levelCount:1];
            t->gen++;
        }
        if (t->levels > 1) [b generateMipmapsForTexture:t->tex];
        [b endEncoding]; g_n_blit++; t->use = g_serial;
        if (cb != g_cb) { dm_gfx_enter(); cb_commit_one(cb); dm_gfx_leave(); }
    }
}
GL(glGenerateMipmap, void, GLenum target) { ((void (*)(GLenum))(const void *)mip_impl_bs)(target); }

// ---------- Framebuffers and renderbuffers ----------
GL(glGenFramebuffers, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_fbs, &g_nfbs, &g_capfbs, sizeof(fb_t)); }
GLB(glDeleteFramebuffers, void, GLsizei n, const GLuint *f) {
    for (GLsizei i = 0; i < n; i++) { fb_t *x = fb_of(f[i]); if (!x) continue; if (P.open && P.fb == f[i]) pass_end(); if (g_pend_clear.fb == f[i]) g_pend_clear.bits = 0; free(x); g_fbs[f[i]] = NULL; if (S.fb == f[i]) S.fb = 0; }
}
GLB(glBindFramebuffer, void, GLenum target, GLuint f) {
    if (target != GL_DRAW_FRAMEBUFFER) S.read_fb = f;   // GL_FRAMEBUFFER binds both
    if (target == GL_READ_FRAMEBUFFER || f == S.fb) return;
    if (g_pend_clear.bits && g_pend_clear.fb == S.fb) clears_flush();
    S.fb = f;   // the pass ends at the next draw or clear into another framebuffer (pass_ensure)
}
GL(glCheckFramebufferStatus, GLenum, GLenum target) { (void)target; return GL_FRAMEBUFFER_COMPLETE; }
// One colour attachment: GL_NONE (LÖVE's depth-only canvases) and attachment 0 are the same draw buffer here
GL(glDrawBuffers, void, GLsizei n, const GLenum *b) { if (n > 1) RED1("glDrawBuffers of %d buffers (one colour attachment is implemented)", n); (void)b; }
GL(glDrawBuffer, void, GLenum b) { (void)b; }
GL(glReadBuffer, void, GLenum b) { (void)b; }
GL(glInvalidateFramebuffer, void, GLenum target, GLsizei n, const GLenum *att) { (void)target; (void)n; (void)att; }   // the contents are kept (a store action)
GL(glDiscardFramebufferEXT, void, GLenum target, GLsizei n, const GLenum *att) { (void)target; (void)n; (void)att; }
GL(glGenRenderbuffers, void, GLsizei n, GLuint *out) { for (GLsizei i = 0; i < n; i++) out[i] = name_new((void ***)&g_rbs, &g_nrbs, &g_caprbs, sizeof(rb_t)); }
GL(glDeleteRenderbuffers, void, GLsizei n, const GLuint *r) { for (GLsizei i = 0; i < n; i++) { rb_t *x = rb_of(r[i]); if (!x) continue; x->tex = nil; free(x); g_rbs[r[i]] = NULL; } }
GL(glBindRenderbuffer, void, GLenum target, GLuint r) { (void)target; S.rb = r; }
static void h_glRenderbufferStorage(GLenum target, GLenum ifmt, GLsizei w, GLsizei h);
GLB(glRenderbufferStorageMultisample, void, GLenum target, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h) {
    if (samples > 1) RED1("multisampled renderbuffer (%d samples): made with one sample", samples);
    h_glRenderbufferStorage(target, ifmt, w, h);
}
GL(glGetRenderbufferParameteriv, void, GLenum target, GLenum p, GLint *v) {
    (void)target; rb_t *r = rb_of(S.rb);
    switch (p) {
        case GL_RENDERBUFFER_SAMPLES: *v = 0; return;
        case GL_RENDERBUFFER_WIDTH: *v = r ? (GLint)r->w : 0; return;
        case GL_RENDERBUFFER_HEIGHT: *v = r ? (GLint)r->h : 0; return;
    }
    RED1("glGetRenderbufferParameteriv(%#x) not implemented", p); *v = 0;
}
GLB(glRenderbufferStorage, void, GLenum target, GLenum ifmt, GLsizei w, GLsizei h) {
    (void)target; rb_t *r = rb_of(S.rb);
    int cv, f = fmt_pick((GLint)ifmt, GL_RGBA, GL_UNSIGNED_BYTE, &cv);
    if (!r || f < 0 || w <= 0 || h <= 0) { RED1("glRenderbufferStorage(%#x, %dx%d) not implemented", ifmt, w, h); return; }
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:g_fmt[f].pf width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        r->tex = [g_dev newTextureWithDescriptor:td]; r->fmt = f; r->w = w; r->h = h; r->gen++;
    }
    OLOG("renderbuffer %u: %dx%d %s", S.rb, w, h, g_fmt[f].name);
}
static void fb_attach(GLenum target, GLenum attachment, int kind, GLuint name, int level, int face) {
    fb_t *f = fb_of(target == GL_READ_FRAMEBUFFER ? S.read_fb : S.fb);
    if (!f) { RED1("attachment to framebuffer 0 or to one never generated"); return; }
    att_t a = {kind, name, level, face};
    if (!name) a.kind = 0;
    if (attachment == GL_COLOR_ATTACHMENT0) f->col = a;
    else if (attachment == GL_DEPTH_ATTACHMENT) f->depth = a;
    else if (attachment == GL_STENCIL_ATTACHMENT) f->stencil = a;
    else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT) f->depth = f->stencil = a;
    else { RED1("framebuffer attachment %#x not implemented", attachment); return; }
    f->gen++;
}
GL(glFramebufferTexture2D, void, GLenum target, GLenum att, GLenum textarget, GLuint t, GLint level) {
    int face = textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && textarget <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z ? (int)(textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X) : 0;
    tex_t *x = tex_of(t); if (x && !x->target) tex_init_params(x, textarget == GL_TEXTURE_2D ? GL_TEXTURE_2D : GL_TEXTURE_CUBE_MAP);
    fb_attach(target, att, 1, t, level, face);
}
// A layer of an array texture or a slice of a 3D one
GL(glFramebufferTextureLayer, void, GLenum target, GLenum att, GLuint t, GLint level, GLint layer) { fb_attach(target, att, 1, t, level, layer); }
GL(glFramebufferTexture3D, void, GLenum target, GLenum att, GLenum textarget, GLuint t, GLint level, GLint z) { (void)textarget; fb_attach(target, att, 1, t, level, z); }
GL(glFramebufferRenderbuffer, void, GLenum target, GLenum att, GLenum rbt, GLuint r) { (void)rbt; fb_attach(target, att, 2, r, 0, 0); }
// A framebuffer's colour image: the texture, its level and its slice (a cube face or an array layer) or 3D slice
static id<MTLTexture> fb_color(GLuint fb, int *level, int *slice) {
    *level = *slice = 0;
    if (!fb) { if (!g_win.made) { int w, h; dm_vi_layer_size(&w, &h); win_make(w > 0 ? w : 1280, h > 0 ? h : 720); } return g_win.col[g_win.cur]; }
    fb_t *f = fb_of(fb); uint64_t g = 0;
    if (!f || f->col.kind == 0) return nil;
    *level = f->col.level; *slice = f->col.face;
    return att_tex(&f->col, &g);
}
// glBlitFramebuffer of colour without scaling, flipping or a format change (LÖVE resolves multisampled canvases so); its last two arguments are on the stack, 64-bit here
typedef struct { GLint s[4], d[4]; GLbitfield mask; } blit_args;
static void blit_impl(const blit_args *a) __attribute__((used));
BIGSTUB(blit_impl);
static void blit_impl(const blit_args *a) {
    @autoreleasepool {
        if (a->mask & ~GL_COLOR_BUFFER_BIT) RED1("glBlitFramebuffer of depth or stencil not implemented");
        if (!(a->mask & GL_COLOR_BUFFER_BIT)) return;
        if (g_pend_clear.bits) clears_flush();
        int sl, ss, dl, ds; id<MTLTexture> src = fb_color(S.read_fb, &sl, &ss), dst = fb_color(S.fb, &dl, &ds);
        int w = a->s[2] - a->s[0], h = a->s[3] - a->s[1];
        if (!src || !dst || w <= 0 || h <= 0 || w != a->d[2] - a->d[0] || h != a->d[3] - a->d[1] || src.pixelFormat != dst.pixelFormat || src.sampleCount != dst.sampleCount) {
            RED1("glBlitFramebuffer with scaling, flipping or a format change not implemented"); return;
        }
        pass_end();
        id<MTLBlitCommandEncoder> b = [cb_get() blitCommandEncoder];
        [b copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:MTLOriginMake(a->s[0], a->s[1], 0) sourceSize:MTLSizeMake(w, h, 1)
                 toTexture:dst destinationSlice:ds destinationLevel:dl destinationOrigin:MTLOriginMake(a->d[0], a->d[1], 0)];
        [b endEncoding]; g_n_blit++;
    }
}
GL(glBlitFramebuffer, void, GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, uint64_t mask, uint64_t filter) {
    (void)filter; blit_args a = {{sx0, sy0, sx1, sy1}, {dx0, dy0, dx1, dy1}, (GLbitfield)mask}; ((void (*)(const blit_args *))(const void *)blit_impl_bs)(&a);
}
GL(glResolveMultisampleFramebufferAPPLE, void, void) { RED1("glResolveMultisampleFramebufferAPPLE not implemented"); }
// glReadPixels: everything recorded so far runs first (the frame's command buffer is committed and waited for), then the rows are copied out as GL lays them, bottom row first,
// which is how this layer stores them. RGBA / unsigned byte from an RGBA8 or BGRA8 image (LÖVE's Canvas:newImageData and screenshots)
typedef struct { GLint x, y; GLsizei w, h; GLenum format, type; void *data; } readpix_args;
static void readpix_impl(const readpix_args *a) __attribute__((used));
BIGSTUB(readpix_impl);
static void readpix_impl(const readpix_args *a) {
    @autoreleasepool {
        if (a->w <= 0 || a->h <= 0 || !a->data) return;
        int lv, sl; id<MTLTexture> src = fb_color(S.read_fb, &lv, &sl);
        int bgra = src && src.pixelFormat == MTLPixelFormatBGRA8Unorm;
        if (!src || (src.pixelFormat != MTLPixelFormatRGBA8Unorm && src.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB && !bgra) || a->format != GL_RGBA || a->type != GL_UNSIGNED_BYTE) {
            RED1("glReadPixels(%#x, %#x) of a %lu image not implemented", a->format, a->type, src ? (unsigned long)src.pixelFormat : 0ul); return;
        }
        if (g_pend_clear.bits) clears_flush();
        dm_gfx_enter();
        pass_end();
        size_t pitch = (size_t)a->w * 4; id<MTLBuffer> out = [g_dev newBufferWithLength:pitch * a->h options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = cb_get();
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:src sourceSlice:sl sourceLevel:lv sourceOrigin:MTLOriginMake(a->x, a->y, 0) sourceSize:MTLSizeMake(a->w, a->h, 1) toBuffer:out destinationOffset:0
            destinationBytesPerRow:pitch destinationBytesPerImage:pitch * a->h];
        [b endEncoding];
        cb_commit(); [cb waitUntilCompleted];
        dm_gfx_leave();
        size_t dp = (pitch + S.pack_align - 1) / S.pack_align * S.pack_align;
        for (GLsizei r = 0; r < a->h; r++) {
            uint8_t *d = (uint8_t *)a->data + r * dp; const uint8_t *s = (const uint8_t *)out.contents + r * pitch;
            if (!bgra) memcpy(d, s, pitch);
            else for (GLsizei x = 0; x < a->w; x++) { d[4 * x] = s[4 * x + 2]; d[4 * x + 1] = s[4 * x + 1]; d[4 * x + 2] = s[4 * x]; d[4 * x + 3] = s[4 * x + 3]; }
        }
    }
}
GL(glReadPixels, void, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *data) {
    readpix_args a = {x, y, w, h, format, type, data}; ((void (*)(const readpix_args *))(const void *)readpix_impl_bs)(&a);
}

// ---------- Shaders and programs ----------
static void sh_capture(const shader_t *s) {
    static const char *dir; static int init;
    if (!init) { init = 1; dir = getenv("DM_GL_CAPTURE"); if (dir) GLOG("shader capture → %s", dir); }
    if (!dir) return;
    char p[1024]; snprintf(p, sizeof p, "%s/%s.%s", dir, s->key, s->type == GL_VERTEX_SHADER ? "vert" : "frag");
    if (access(p, F_OK) == 0) return;
    char t[1100]; snprintf(t, sizeof t, "%s.%d.tmp", p, getpid());
    FILE *f = fopen(t, "wb"); if (!f) { RED1("shader capture cannot write to %s", dir); return; }
    int ok = fwrite(s->src, 1, s->len, f) == s->len; ok &= fclose(f) == 0;
    if (ok) rename(t, p); else unlink(t);   // whole files only: a capture run may be killed at any time
}
GL(glCreateShader, GLuint, GLenum type) {
    GLuint n = name_new((void ***)&g_sps, &g_nsps, &g_capsps, sizeof(sp_t)); g_sps[n]->s.type = type; return n;
}
GL(glCreateProgram, GLuint, void) {
    GLuint n = name_new((void ***)&g_sps, &g_nsps, &g_capsps, sizeof(sp_t)); g_sps[n]->is_prog = 1; return n;
}
GL(glShaderSource, void, GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len) {
    shader_t *s = shader_of(sh); if (!s) return;
    size_t n = 0;
    for (GLsizei i = 0; i < count; i++) n += len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]);
    free(s->src); s->src = zalloc(n + 1); s->len = 0;
    for (GLsizei i = 0; i < count; i++) { size_t k = len && len[i] >= 0 ? (size_t)len[i] : strlen(str[i]); memcpy(s->src + s->len, str[i], k); s->len += k; }
    s->len = strnlen(s->src, s->len);   // the source ends at its first NUL (gl_shaders.py hashes the same text)
    s->compiled = 0;
}
GLB(glCompileShader, void, GLuint sh) {
    shader_t *s = shader_of(sh); if (!s || !s->src) return;
    uint8_t h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(s->src, (CC_LONG)s->len, h);
    for (int i = 0; i < 8; i++) snprintf(s->key + 2 * i, 3, "%02x", h[i]);
    sh_capture(s);
    int frag = s->type == GL_FRAGMENT_SHADER;
    s->si = sh_find(s->key, frag); s->compiled = 1;
    @autoreleasepool { s->fn = s->si && g_lib ? [g_lib newFunctionWithName:[NSString stringWithFormat:@"gl_%s_%c", s->key, frag ? 'f' : 'v']] : nil; }
    if (!s->fn) { static int n; if (n++ < 50) GLOG("[RED] %s shader %s (%zu bytes of GLSL) is not in the library (DM_GL_CAPTURE collects it)", s->type == GL_VERTEX_SHADER ? "vertex" : "fragment", s->key, s->len); }
    else OLOG("CompileShader %s", s->key);
}
GL(glGetShaderiv, void, GLuint sh, GLenum p, GLint *v) {
    shader_t *s = shader_of(sh); if (!s) { *v = 0; return; }
    switch (p) {
        case GL_COMPILE_STATUS: *v = 1; return;   // a shader the library lacks still compiles: its program's draws are skipped (and reported)
        case GL_INFO_LOG_LENGTH: *v = 0; return;
        case GL_SHADER_TYPE: *v = s->type; return;
        case GL_SHADER_SOURCE_LENGTH: *v = (GLint)s->len + 1; return;
        case GL_DELETE_STATUS: *v = 0; return;
    }
    RED1("glGetShaderiv(%#x) not implemented", p); *v = 0;
}
GL(glGetShaderInfoLog, void, GLuint sh, GLsizei size, GLsizei *len, GLchar *log) { (void)sh; if (len) *len = 0; if (log && size > 0) log[0] = 0; }
GL(glDeleteShader, void, GLuint sh) { shader_t *s = shader_of(sh); if (!s) return; free(s->src); s->fn = nil; free(g_sps[sh]); g_sps[sh] = NULL; }
GL(glAttachShader, void, GLuint pr, GLuint sh) {
    prog_t *p = prog_of(pr); shader_t *s = shader_of(sh); if (!p || !s) return;
    if (s->type == GL_VERTEX_SHADER) p->vs = sh; else p->fs = sh;
}
static void prog_clear(prog_t *p) {
    for (int i = 0; i < p->npc; i++) if (p->pc[i].pso) CFRelease(p->pc[i].pso);
    free(p->pc); p->pc = NULL; p->npc = p->cappc = 0;
    free(p->u); p->u = NULL; p->nu = 0; free(p->ub[0]); free(p->ub[1]); p->ub[0] = p->ub[1] = NULL; p->fn[0] = p->fn[1] = nil; p->linked = 0;
}
// Uniforms are merged by name over both stages, as GL links them; a location is uniform index × 4096 + array element
GLB(glLinkProgram, void, GLuint pr) {
    static uint32_t serials;
    prog_t *p = prog_of(pr); if (!p) return;
    prog_clear(p);
    shader_t *vs = shader_of(p->vs), *fs = shader_of(p->fs);
    if (!vs || !fs || !vs->fn || !fs->fn) { RED1("program %u: a shader is missing from the library, its draws are skipped", pr); return; }
    p->si[0] = vs->si; p->si[1] = fs->si; p->fn[0] = vs->fn; p->fn[1] = fs->fn;
    int cap = vs->si->nu + vs->si->nt + fs->si->nu + fs->si->nt; p->u = zalloc((cap + 1) * sizeof *p->u);
    for (int st = 0; st < 2; st++) {
        const shinfo *si = p->si[st];
        p->ub[st] = zalloc(si->ub_size + 16);
        for (int i = 0; i < si->nu + si->nt; i++) {
            const char *nm = i < si->nu ? si->u[i].name : si->t[i - si->nu].name;
            puni_t *u = NULL;
            for (int j = 0; j < p->nu; j++) if (!strcmp(p->u[j].name, nm)) u = &p->u[j];
            if (!u) { u = &p->u[p->nu++]; strlcpy(u->name, nm, sizeof u->name); u->off[0] = u->off[1] = -1; u->slot[0] = u->slot[1] = -1; }
            if (i < si->nu) { const shuni *m = &si->u[i]; u->type = m->type; u->count = m->count; u->off[st] = m->off; u->stride[st] = m->stride; }
            else { const shsamp *m = &si->t[i - si->nu]; u->type = m->type; u->slot[st] = m->slot; }
        }
    }
    // the game's attribute indices: glBindAttribLocation's for the inputs it names, the input's own number for the others (or the lowest index no binding took)
    const shinfo *vi = p->si[0]; uint32_t taken = 0;
    for (int s = 0; s < 16; s++) p->amap[s] = (int8_t)s;
    for (int i = 0; i < vi->na; i++) for (int j = 0; j < p->nbind; j++)
        if (!strcmp(p->bind[j].name, vi->a[i].name)) { p->amap[vi->a[i].slot] = (int8_t)p->bind[j].index; taken |= 1u << p->bind[j].index; }
    for (int i = 0; i < vi->na; i++) {
        int bound = 0; for (int j = 0; j < p->nbind; j++) bound |= !strcmp(p->bind[j].name, vi->a[i].name);
        if (bound) continue;
        int g = vi->a[i].slot; for (int n = 0; n < 16 && (taken >> g & 1); n++) g = (g + 1) % 16;
        p->amap[vi->a[i].slot] = (int8_t)g; taken |= 1u << g;
    }
    p->serial = ++serials; p->ub_gen[0]++; p->ub_gen[1]++; p->linked = 1;
    OLOG("LinkProgram %u: vs %s fs %s, %d uniforms", pr, vs->key, fs->key, p->nu);
}
GL(glBindAttribLocation, void, GLuint pr, GLuint index, const GLchar *name) {   // takes effect at the next link
    prog_t *p = prog_of(pr); if (!p || index >= 16 || !name) return;
    int j = 0; while (j < p->nbind && strcmp(p->bind[j].name, name)) j++;
    if (j == 16) { RED1("more than 16 attribute bindings in a program"); return; }
    if (j == p->nbind) { strlcpy(p->bind[j].name, name, sizeof p->bind[j].name); p->nbind++; }
    p->bind[j].index = (int)index;
}
GL(glGetProgramiv, void, GLuint pr, GLenum p, GLint *v) {
    prog_t *x = prog_of(pr); if (!x) { *v = 0; return; }
    switch (p) {
        case GL_LINK_STATUS: case GL_VALIDATE_STATUS: *v = 1; return;   // as glGetShaderiv's compile status
        case GL_INFO_LOG_LENGTH: *v = 0; return;
        case GL_ACTIVE_UNIFORMS: *v = x->nu; return;
        case GL_ACTIVE_UNIFORM_MAX_LENGTH: { int m = 0; for (int i = 0; i < x->nu; i++) m = (int)umax(m, (uint32_t)strlen(x->u[i].name) + 4); *v = m; return; }
        case GL_ATTACHED_SHADERS: *v = (x->vs != 0) + (x->fs != 0); return;
        case GL_ACTIVE_ATTRIBUTES: *v = x->si[0] ? x->si[0]->na : 0; return;
        case GL_DELETE_STATUS: *v = 0; return;
    }
    RED1("glGetProgramiv(%#x) not implemented", p); *v = 0;
}
GL(glDeleteProgram, void, GLuint pr) { prog_t *p = prog_of(pr); if (!p) return; prog_clear(p); free(g_sps[pr]); g_sps[pr] = NULL; if (S.prog == pr) S.prog = 0; }
GL(glUseProgram, void, GLuint pr) { S.prog = pr; }
GL(glGetAttribLocation, GLint, GLuint pr, const GLchar *name) {
    prog_t *p = prog_of(pr); if (!p || !p->linked) return -1;
    for (int i = 0; i < p->si[0]->na; i++) if (!strcmp(p->si[0]->a[i].name, name)) return p->amap[p->si[0]->a[i].slot];
    return -1;
}
GL(glGetProgramInfoLog, void, GLuint pr, GLsizei size, GLsizei *len, GLchar *log) { (void)pr; if (len) *len = 0; if (log && size > 0) log[0] = 0; }
// The uniforms in the order of the program's table (both stages merged, samplers among them): an array's name ends in [0], as GL reports it (LÖVE's mapActiveUniforms)
GL(glGetActiveUniform, void, GLuint pr, GLuint index, GLsizei size, GLsizei *len, GLint *count, GLenum *type, GLchar *name) {
    prog_t *p = prog_of(pr);
    if (!p || !p->linked || index >= (GLuint)p->nu) { if (len) *len = 0; if (name && size > 0) name[0] = 0; return; }
    const puni_t *u = &p->u[index]; char nm[64];
    snprintf(nm, sizeof nm, "%s%s", u->name, u->count ? "[0]" : "");
    if (name && size > 0) strlcpy(name, nm, (size_t)size);
    if (len) *len = name && size > 0 ? (GLsizei)umin((uint32_t)strlen(nm), (uint32_t)size - 1) : 0;
    if (count) *count = u->count ? u->count : 1;
    if (type) *type = g_tenums[u->type];
}
GL(glGetShaderPrecisionFormat, void, GLenum stage, GLenum prec, GLint *range, GLint *precision) {   // every precision is a 32-bit float or int
    (void)stage; int fl = prec <= 0x8df2;   // LOW / MEDIUM / HIGH_FLOAT, then the INTs
    if (range) { range[0] = fl ? 127 : 31; range[1] = fl ? 127 : 30; }
    if (precision) *precision = fl ? 23 : 0;
}
GL(glGetUniformLocation, GLint, GLuint pr, const GLchar *name) {
    prog_t *p = prog_of(pr); if (!p || !p->linked) return -1;
    char nm[64]; strlcpy(nm, name, sizeof nm); int elem = 0;
    char *br = strchr(nm, '[');
    if (br) { elem = atoi(br + 1); *br = 0; }
    for (int i = 0; i < p->nu; i++) if (!strcmp(p->u[i].name, nm)) return (elem < 0 || elem >= (p->u[i].count ? p->u[i].count : 1)) ? -1 : i * 4096 + elem;
    return -1;
}
// Write count elements from location loc of the current program: comps values each (a matrix: cols columns of comps / cols), into every stage that has the uniform.
// The default block is laid out as std140: a matrix column takes 16 bytes
static void uni_write(GLint loc, GLsizei count, const void *v, int comps, int is_int, int transpose, int cols) {
    prog_t *p = prog_of(S.prog);
    if (!p || !p->linked || loc < 0 || count <= 0) return;
    int i = loc / 4096, el = loc % 4096;
    if (i >= p->nu) return;
    puni_t *u = &p->u[i];
    if (is_samp(u->type)) { if (is_int) u->unit = *(const GLint *)v; return; }
    int n = u->count ? u->count : 1, rows = cols ? comps / cols : comps, size = cols ? cols * 16 : 16;
    if (el + count > n) count = n - el;
    for (int st = 0; st < 2; st++) {
        if (u->off[st] < 0) continue;
        int stride = u->stride[st] ? u->stride[st] : size;
        for (GLsizei k = 0; k < count; k++) {
            uint8_t *d = p->ub[st] + u->off[st] + (size_t)(el + k) * stride;
            if (d + size > p->ub[st] + p->si[st]->ub_size + 16) break;
            const float *s = (const float *)v + (size_t)k * comps;
            if (cols) { for (int c = 0; c < cols; c++) for (int r = 0; r < rows; r++) ((float *)(d + c * 16))[r] = transpose ? s[r * cols + c] : s[c * rows + r]; }
            else memcpy(d, s, (size_t)umin(comps, tcomps(u->type)) * 4);
        }
        p->ub_gen[st]++;
    }
}
// Integer values for a float uniform are converted (GL would reject them; NVIDIA's driver converts), as are float values for an integer one
static void uni_ints(GLint loc, GLsizei n, const GLint *v, int comps) {
    prog_t *p = prog_of(S.prog);
    if (p && p->linked && loc >= 0 && loc / 4096 < p->nu && !is_samp(p->u[loc / 4096].type) && !is_intt(p->u[loc / 4096].type)) {
        float f[64]; for (GLsizei k = 0; k < n && (k + 1) * comps <= 64; k++) for (int c = 0; c < comps; c++) f[k * comps + c] = (float)v[k * comps + c];
        uni_write(loc, n > 64 / comps ? 64 / comps : n, f, comps, 0, 0, 0); return;
    }
    uni_write(loc, n, v, comps, 1, 0, 0);
}
static void uni_floats(GLint loc, GLsizei n, const GLfloat *v, int comps) {
    prog_t *p = prog_of(S.prog);
    if (p && p->linked && loc >= 0 && loc / 4096 < p->nu && is_intt(p->u[loc / 4096].type)) {
        GLint iv[64]; for (GLsizei k = 0; k < n && (k + 1) * comps <= 64; k++) for (int c = 0; c < comps; c++) iv[k * comps + c] = (GLint)v[k * comps + c];
        uni_write(loc, n > 64 / comps ? 64 / comps : n, iv, comps, 1, 0, 0); return;
    }
    uni_write(loc, n, v, comps, 0, 0, 0);
}
GL(glUniform1fv, void, GLint loc, GLsizei n, const GLfloat *v) { uni_floats(loc, n, v, 1); }
GL(glUniform2fv, void, GLint loc, GLsizei n, const GLfloat *v) { uni_floats(loc, n, v, 2); }
GL(glUniform3fv, void, GLint loc, GLsizei n, const GLfloat *v) { uni_floats(loc, n, v, 3); }
GL(glUniform4fv, void, GLint loc, GLsizei n, const GLfloat *v) { uni_floats(loc, n, v, 4); }
GL(glUniform1f, void, GLint loc, GLfloat x) { uni_floats(loc, 1, &x, 1); }
GL(glUniform2f, void, GLint loc, GLfloat x, GLfloat y) { const GLfloat v[] = {x, y}; uni_floats(loc, 1, v, 2); }
GL(glUniform3f, void, GLint loc, GLfloat x, GLfloat y, GLfloat z) { const GLfloat v[] = {x, y, z}; uni_floats(loc, 1, v, 3); }
GL(glUniform4f, void, GLint loc, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { const GLfloat v[] = {x, y, z, w}; uni_floats(loc, 1, v, 4); }
GL(glUniform1i, void, GLint loc, GLint v) { uni_ints(loc, 1, &v, 1); }
GL(glUniform1iv, void, GLint loc, GLsizei n, const GLint *v) { uni_ints(loc, n, v, 1); }
GL(glUniform2iv, void, GLint loc, GLsizei n, const GLint *v) { uni_ints(loc, n, v, 2); }
GL(glUniform3iv, void, GLint loc, GLsizei n, const GLint *v) { uni_ints(loc, n, v, 3); }
GL(glUniform4iv, void, GLint loc, GLsizei n, const GLint *v) { uni_ints(loc, n, v, 4); }
GL(glUniform1uiv, void, GLint loc, GLsizei n, const GLuint *v) { uni_write(loc, n, v, 1, 1, 0, 0); }
GL(glUniform2uiv, void, GLint loc, GLsizei n, const GLuint *v) { uni_write(loc, n, v, 2, 1, 0, 0); }
GL(glUniform3uiv, void, GLint loc, GLsizei n, const GLuint *v) { uni_write(loc, n, v, 3, 1, 0, 0); }
GL(glUniform4uiv, void, GLint loc, GLsizei n, const GLuint *v) { uni_write(loc, n, v, 4, 1, 0, 0); }
#define UMAT(name, c, r) GL(name, void, GLint loc, GLsizei n, uint32_t transpose, const GLfloat *v) { uni_write(loc, n, v, (c) * (r), 0, (transpose & 0xff) != 0, c); }
UMAT(glUniformMatrix2fv, 2, 2) UMAT(glUniformMatrix3fv, 3, 3) UMAT(glUniformMatrix4fv, 4, 4) UMAT(glUniformMatrix2x3fv, 2, 3) UMAT(glUniformMatrix2x4fv, 2, 4)
UMAT(glUniformMatrix3x2fv, 3, 2) UMAT(glUniformMatrix3x4fv, 3, 4) UMAT(glUniformMatrix4x2fv, 4, 2) UMAT(glUniformMatrix4x3fv, 4, 3)
// Values read back from the program's own copy (LÖVE keeps the values a shader initialises its uniforms with): the first stage that has the uniform
static void uni_read(GLuint pr, GLint loc, void *out, int as_int) {
    prog_t *p = prog_of(pr);
    if (!p || !p->linked || loc < 0 || loc / 4096 >= p->nu) return;
    const puni_t *u = &p->u[loc / 4096]; int el = loc % 4096;
    if (is_samp(u->type)) { if (as_int) *(GLint *)out = u->unit; else *(GLfloat *)out = (GLfloat)u->unit; return; }
    int st = u->off[0] >= 0 ? 0 : 1; if (u->off[st] < 0) return;
    int cols = u->type == T_MAT4 ? 4 : u->type == T_MAT3 ? 3 : u->type == T_MAT2 ? 2 : 0, rows = cols ? cols : tcomps(u->type);
    const uint8_t *d = p->ub[st] + u->off[st] + (size_t)el * (u->stride[st] ? u->stride[st] : cols ? cols * 16 : 16);
    for (int c = 0; c < (cols ? cols : 1); c++) for (int r = 0; r < rows; r++) {
        const uint8_t *s = d + c * 16 + r * 4; int k = c * rows + r;
        if (as_int == is_intt(u->type)) memcpy((uint8_t *)out + k * 4, s, 4);
        else if (as_int) { float f; memcpy(&f, s, 4); ((GLint *)out)[k] = (GLint)f; }
        else { GLint i; memcpy(&i, s, 4); ((GLfloat *)out)[k] = (GLfloat)i; }
    }
}
GL(glGetUniformfv, void, GLuint pr, GLint loc, GLfloat *v) { uni_read(pr, loc, v, 0); }
GL(glGetUniformiv, void, GLuint pr, GLint loc, GLint *v) { uni_read(pr, loc, v, 1); }
GL(glGetUniformuiv, void, GLuint pr, GLint loc, GLuint *v) { uni_read(pr, loc, v, 1); }

// ---------- EGL ----------
#define EGL_TRUE 1
static struct { int init; int surface; uint32_t w, h; } g_egl;
static uint64_t g_frames;
// Every 300 frames: draws, passes, uploads; every 1800: memory (the process footprint, Metal's allocations, the GL objects alive), as the NVN layer's memory report
static void frame_end(void) {
    static uint64_t pd, pp, pu, pb, pc;
    uint64_t f = ++g_frames, dr = g_n_draw, pa = g_n_pass, up = g_n_up, bl = g_n_blit, cb = g_n_cb;
    if (f <= 3 || f % 300 == 0) {
        GLOG("frame %llu: draws %llu, passes %llu, uploads %llu (%llu by blit), command buffers %llu (since the last record, %llu frames); pipelines so far %llu",
             (unsigned long long)f, (unsigned long long)(dr - pd), (unsigned long long)(pa - pp), (unsigned long long)(up - pu), (unsigned long long)(bl - pb),
             (unsigned long long)(cb - pc), (unsigned long long)(f <= 3 ? 1 : 300), (unsigned long long)g_n_pso);
        pd = dr; pp = pa; pu = up; pb = bl; pc = cb;
    }
    if (f % 1800 == 0) {
        task_vm_info_data_t vi; mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
        uint64_t foot = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) == KERN_SUCCESS ? vi.phys_footprint : 0;
        uint32_t nt = 0, nb = 0; uint64_t bb = 0;
        for (uint32_t i = 1; i < g_ntexs; i++) nt += g_texs[i] && g_texs[i]->tex != nil;
        for (uint32_t i = 1; i < g_nbufs; i++) if (g_bufs[i] && g_bufs[i]->mb) { nb++; bb += g_bufs[i]->mb.length; }
        GLOG("memory at frame %llu: footprint %.0f MB, Metal allocated %.0f MB; %u textures, %u buffers (%.0f MB), %d transient blocks",
             (unsigned long long)f, foot / 1048576.0, g_dev.currentAllocatedSize / 1048576.0, nt, nb, bb / 1048576.0, g_narena);
    }
    dm_guest_frame_tick();
}
EGL(eglGetDisplay, EGLDisplay, void *native) { (void)native; dev_init(); return (EGLDisplay)&g_egl; }
EGL(eglInitialize, EGLBoolean, EGLDisplay d, EGLint *major, EGLint *minor) { (void)d; dev_init(); g_egl.init = 1; if (major) *major = 1; if (minor) *minor = 5; return EGL_TRUE; }
EGL(eglTerminate, EGLBoolean, EGLDisplay d) { (void)d; return EGL_TRUE; }
EGL(eglReleaseThread, EGLBoolean, void) { return EGL_TRUE; }
// SDL binds 0 (its API before it has chosen one) when it makes the window again (Balatro's love.window.setMode): nothing to change
EGL(eglBindAPI, EGLBoolean, EGLenum api) { if (api && api != 0x30a2) RED1("eglBindAPI(%#x): only desktop OpenGL (0x30a2) is implemented", api); return EGL_TRUE; }
// One configuration: RGBA8, depth 24 (32F here), stencil 8, a window surface, desktop OpenGL; it matches the game's attributes
EGL(eglChooseConfig, EGLBoolean, EGLDisplay d, const EGLint *attr, EGLConfig *cfg, EGLint n, EGLint *num) {
    (void)d; (void)attr;
    if (cfg && n > 0) cfg[0] = (EGLConfig)(uintptr_t)1;
    if (num) *num = 1;
    return EGL_TRUE;
}
// The one configuration's attributes (SDL reads them back after eglChooseConfig): what eglChooseConfig says it is
EGL(eglGetConfigAttrib, EGLBoolean, EGLDisplay d, EGLConfig cfg, EGLint attr, EGLint *v) {
    (void)d; (void)cfg;
    switch (attr) {
        case 0x3020: *v = 32; break;   // BUFFER_SIZE
        case 0x3021: case 0x3022: case 0x3023: case 0x3024: *v = 8; break;   // ALPHA / BLUE / GREEN / RED_SIZE
        case 0x3025: *v = 24; break;   // DEPTH_SIZE
        case 0x3026: *v = 8; break;   // STENCIL_SIZE
        case 0x3028: *v = 1; break;   // CONFIG_ID
        case 0x3031: case 0x3032: *v = 0; break;   // SAMPLES, SAMPLE_BUFFERS
        case 0x302e: *v = 0; break;   // NATIVE_VISUAL_ID
        case 0x3033: *v = 0x5; break;   // SURFACE_TYPE: PBUFFER | WINDOW
        case 0x3040: case 0x3042: *v = 0x8; break;   // RENDERABLE_TYPE, CONFORMANT: OPENGL_BIT
        case 0x3027: *v = 0x3038; break;   // CONFIG_CAVEAT: NONE
        default: RED1("eglGetConfigAttrib(%#x) not implemented (0)", attr); *v = 0; break;
    }
    return EGL_TRUE;
}
EGL(eglQueryString, const char *, EGLDisplay d, EGLint name) {
    (void)d;
    switch (name) { case 0x3053: return "NVIDIA"; case 0x3054: return "1.5"; case 0x3055: return "EGL_KHR_create_context"; case 0x308d: return "OpenGL"; }   // VENDOR, VERSION, EXTENSIONS, CLIENT_APIS
    return NULL;
}
EGL(eglGetError, EGLint, void) { return 0x3000; }   // SUCCESS
EGL(eglQueryAPI, EGLenum, void) { return 0x30a2; }   // OPENGL_API
EGL(eglWaitNative, EGLBoolean, EGLint engine) { (void)engine; return EGL_TRUE; }
EGL(eglWaitGL, EGLBoolean, void) { return EGL_TRUE; }
EGL(eglDestroyContext, EGLBoolean, EGLDisplay d, EGLContext c) { (void)d; (void)c; return EGL_TRUE; }
EGL(eglCreatePbufferSurface, EGLSurface, EGLDisplay d, EGLConfig cfg, const EGLint *attr) { (void)d; (void)cfg; (void)attr; RED1("eglCreatePbufferSurface: drawing into it is not implemented"); return (EGLSurface)(uintptr_t)4; }
EGL(eglCreateContext, EGLContext, EGLDisplay d, EGLConfig cfg, EGLContext share, const EGLint *attr) {
    (void)d; (void)cfg;
    int major = 0, minor = 0; for (int i = 0; attr && attr[i] != 0x3038 && i < 32; i += 2) { if (attr[i] == 0x3098) major = attr[i + 1]; else if (attr[i] == 0x30fb) minor = attr[i + 1]; }
    if (share) RED1("shared EGL contexts are not implemented (one context)");
    GLOG("EGL context: OpenGL %d.%d", major, minor);
    return (EGLContext)(uintptr_t)2;
}
EGL(eglCreateWindowSurface, EGLSurface, EGLDisplay d, EGLConfig cfg, void *win, const EGLint *attr) {
    (void)d; (void)cfg; (void)win; (void)attr; dev_init();
    int w, h; dm_vi_layer_size(&w, &h); if (w <= 0 || h <= 0) { w = 1280; h = 720; }
    if (P.open) pass_end();
    if (g_win.made && (g_win.w != (uint32_t)w || g_win.h != (uint32_t)h)) g_win.made = 0;
    if (!g_win.made) win_make(w, h);
    g_egl.surface = 1;
    return (EGLSurface)(uintptr_t)3;
}
EGL(eglDestroySurface, EGLBoolean, EGLDisplay d, EGLSurface s) { (void)d; (void)s; return EGL_TRUE; }
EGL(eglMakeCurrent, EGLBoolean, EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c) {
    (void)d; (void)rd;
    static pthread_t th; static int known, sized;
    if (c && dr) { pthread_t me = pthread_self(); if (!known || !pthread_equal(th, me)) { th = me; known = 1; OLOG("context current on a new thread"); } }
    // a context's viewport and scissor box start as its first drawable's size (LÖVE reads them back at start)
    if (c && dr && !sized && g_win.made) { sized = 1; if (!S.vp[2] && !S.vp[3]) { S.vp[2] = S.sc[2] = g_win.w; S.vp[3] = S.sc[3] = g_win.h; } }
    return EGL_TRUE;
}
EGL(eglSwapInterval, EGLBoolean, EGLDisplay d, EGLint n) { (void)d; if (n != S.swap_interval) { GLOG("swap interval %d", n); S.swap_interval = n; } return EGL_TRUE; }
static uint64_t g_next_swap;   // pacing without the display link: the earliest time of the next swap
EGL(eglSwapBuffers, EGLBoolean, EGLDisplay d, EGLSurface s) {
    (void)d; (void)s;
    dm_gfx_enter();
    @autoreleasepool {
        if (!g_win.made) { int w, h; dm_vi_layer_size(&w, &h); win_make(w > 0 ? w : 1280, h > 0 ? h : 720); }
        if (g_pend_clear.bits) clears_flush();
        uint64_t v = cb_commit();
        int vs = S.swap_interval < 1 ? 1 : S.swap_interval > 4 ? 4 : S.swap_interval;
        int k = g_win.cur;
        g_win.ticket[k] = mtl_present_ext(g_win.serial[k], vs / 60.0, (__bridge void *)g_ev, v, NULL);
        g_win.cur = (k + 1) % NWIN;
        if (g_win.ticket[g_win.cur]) mtl_present_wait(g_win.ticket[g_win.cur]);   // its previous present has been shown, so drawing into it again is safe
        // Paced by the display link when it runs, as the Vulkan layer's acquire: the swap interval in vsyncs
        uint64_t period = 1000000000ull * vs / 60, now = now_ns();
        if (S.swap_interval > 0 && mtl_link_active() && mtl_link_wait_tick(vs, 2 * period)) g_next_swap = now_ns() + period;
        else if (S.swap_interval > 0) {
            if (g_next_swap > now) { uint64_t w = g_next_swap - now; struct timespec ts = {(time_t)(w / 1000000000ull), (long)(w % 1000000000ull)}; nanosleep(&ts, NULL); }
            now = now_ns(); g_next_swap = g_next_swap && now < g_next_swap + period ? g_next_swap + period : now + period;
        }
    }
    dm_gfx_leave();
    frame_end();
    return EGL_TRUE;
}
// eglGetProcAddress: GL names from the same table as direct imports; anything else is null (the game's loader asks for every GL name it knows)
static void *gl_lookup(const char *name) {
    static const hle_ent *sec; static unsigned long sz;
    if (!sec) sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &sz);
    for (unsigned long i = 0; i < sz / sizeof *sec; i++) if (!strcmp(sec[i].name, name)) return sec[i].fn;
    return NULL;
}
// DM_GL_STUBS=1 (bring-up): a GL name the layer does not implement gets a stub of its own instead of a null pointer, which reports its first call and returns 0; a game that calls
// such a function then says which one instead of jumping to address 0. Off by default: a loader may take a name's null pointer to mean "not there"
#define NSTUB 4096
static const char *g_stub_name[NSTUB]; static uint8_t g_stub_seen[NSTUB]; static int g_nstub;
uint64_t gl_stub_c(uint64_t k, uint64_t x0, uint64_t x1, uint64_t x2) {
    if (k < NSTUB && !g_stub_seen[k]++) GLOG("[RED] %s called: not implemented, returns 0 (x0=%#llx x1=%#llx x2=%#llx)", g_stub_name[k], x0, x1, x2);
    return 0;
}
__asm__(".text\n.p2align 2\ngl_stub_entry:\n    mov x3, x2\n    mov x2, x1\n    mov x1, x0\n    mov x0, x9\n    b _gl_stub_c\n"
        ".p2align 3\n.globl _gl_stubs\n.private_extern _gl_stubs\n_gl_stubs:\n.set gl_stub_k, 0\n.rept 4096\n    mov x9, #gl_stub_k\n    b gl_stub_entry\n.set gl_stub_k, gl_stub_k + 1\n.endr\n");
extern const char gl_stubs[];   // 8 bytes a stub
static void *gl_stub(const char *name) {
    static int on = -1; if (on < 0) on = getenv("DM_GL_STUBS") && atoi(getenv("DM_GL_STUBS"));
    if (!on || g_nstub == NSTUB) return NULL;
    g_stub_name[g_nstub] = name;   // the game's own string (its loader's name table)
    return (void *)(gl_stubs + 8 * g_nstub++);
}
EGL(eglGetProcAddress, void *, const char *name) {
    static int asked, found; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    void *fn = name && !strncmp(name, "gl", 2) ? gl_lookup(name) : NULL;
    pthread_mutex_lock(&mu);
    asked++; if (fn) found++;
    if (asked % 500 == 0 || (fn && g_log)) GLOG("eglGetProcAddress: %d names asked, %d implemented (last %s)", asked, found, name);
    if (!fn && name && !strncmp(name, "gl", 2)) fn = gl_stub(name);
    pthread_mutex_unlock(&mu);
    return fn;
}
