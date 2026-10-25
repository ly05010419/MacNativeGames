// Vulkan → Metal layer for games that render with Vulkan (Hades: The Forge renderer). The game's vk* imports bind to the functions here through the host's import
// table (hle.h swhle); the Vulkan loader (subsdk0) and NVIDIA's driver (subsdk1) are not loaded. vkGet{Instance,Device}ProcAddr look names up in the same table
// (a name ending in KHR is also tried without it, for extensions promoted to core 1.1); every name the game asks for is logged once.
// Model:
//   · one physical device shaped like the Switch's Tegra X1: Vulkan 1.1, one queue family, 3 memory types on one heap;
//   · device memory = one shared-storage MTLBuffer, created on the first buffer bind or map (memory that only backs images never gets one); images are
//     standalone private MTLTextures and ignore their memory binding, except linear 2D images, whose texture is made over the bound buffer;
//   · command buffers are recorded into a host list and replayed at vkQueueSubmit into one MTLCommandBuffer on this layer's own queue, which then signals g_ev
//     with the next value; fences complete from the completion handler; semaphores and pipeline barriers do nothing (one serial queue, Metal tracks hazards);
//     queries are results of their submit: timestamps its completion time, occlusion counts from a visibility buffer;
//   · shaders come from the offline library built by dm/gfx/tools/vk_shaders.py (<DM_MSL_DIR>/vk/): a module is found by the hash of its SPIR-V, and its table
//     row says which Metal slot each (set, binding) went to (an array of n descriptors: n slots from there; push constants: their own buffer slot);
//     vertex buffers sit at slot 30 - binding;
//   · swapchain images are registered with the NVN layer's present path (mtl_ext_tex), which shows a frame once g_ev reaches the value of the frame's last submit.
// Environment: DM_VK_LOG=1 logs object creation; DM_VK_CAPTURE=<dir> writes every SPIR-V module the game creates (sh_capture).
#define VK_NO_PROTOTYPES
#include "../../host/hle.h"
#include "../nvn/nvn_mtl.h"
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_vi.h>
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>

int mtl_fps_target(void);   // nvn_mtl.m: 60, or 30 when the frame rate is halved
int mtl_link_active(void); int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns);   // nvn_mtl.m: whether the display link presents, and the wait for its ticks
void dm_vi_layer_crop(int c[4]);   // hle_vi.c: x, y, w, h the game last cropped its layer to (all 0 = none)

#define VLOG(...) LOG("[vk] " __VA_ARGS__)
#define RED1(...) do { static int once_; if (!once_) { once_ = 1; LOG("[vk] [RED] " __VA_ARGS__); } } while (0)
static int g_log;
#define OLOG(...) do { if (g_log) VLOG(__VA_ARGS__); } while (0)

// VK: one Vulkan function, checked against its PFN_ type and registered in the host's import table under its own name
#define VK(name, ret, ...)                                                                       \
    static ret h_##name(__VA_ARGS__);                                                            \
    static const PFN_##name chk_##name __attribute__((unused)) = h_##name;                       \
    __attribute__((used, section("__DATA,swhle"))) static const hle_ent e_##name = {#name, (void *)h_##name}; \
    static ret h_##name(__VA_ARGS__)
// VKB: the same, run on the thread's big host stack (hle_bigstack.c; register arguments only): for the functions that call into Metal on game threads, whose stacks can be small
#define VKB(name, ret, ...)                                                                      \
    static ret h_##name(__VA_ARGS__) __attribute__((used));                                      \
    static const PFN_##name chk_##name __attribute__((unused)) = h_##name;                       \
    extern const char h_##name##_bs[];                                                           \
    __asm__(".text\n.p2align 2\n.globl _h_" #name "_bs\n.private_extern _h_" #name "_bs\n_h_" #name "_bs:\n" \
            "    adrp x16, _h_" #name "@PAGE\n    add x16, x16, _h_" #name "@PAGEOFF\n    b _dm_bigstack_enter\n"); \
    __attribute__((used, section("__DATA,swhle"))) static const hle_ent e_##name = {#name, (void *)h_##name##_bs}; \
    static ret h_##name(__VA_ARGS__)

static uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
static uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }

// ---------- Formats ----------
enum { K_COLOR, K_INT, K_DEPTH, K_DS, K_STENCIL, K_BLOCK };
typedef struct { VkFormat vk; MTLPixelFormat pf; MTLVertexFormat vf; uint8_t bytes, bw, bh, kind; } fmt_t;   // bytes per texel, or per block (bw x bh) for compressed formats
#define FC(v, p, x, b) {VK_FORMAT_##v, MTLPixelFormat##p, MTLVertexFormat##x, b, 1, 1, K_COLOR}
#define FI(v, p, x, b) {VK_FORMAT_##v, MTLPixelFormat##p, MTLVertexFormat##x, b, 1, 1, K_INT}
#define FD(v, p, b, k) {VK_FORMAT_##v, MTLPixelFormat##p, MTLVertexFormatInvalid, b, 1, 1, k}
#define FB(v, p, b, w, h) {VK_FORMAT_##v, MTLPixelFormat##p, MTLVertexFormatInvalid, b, w, h, K_BLOCK}
#define FV(v, x, b) {VK_FORMAT_##v, MTLPixelFormatInvalid, MTLVertexFormat##x, b, 1, 1, K_COLOR}
static const fmt_t g_fmts[] = {
    FC(R8_UNORM, R8Unorm, UCharNormalized, 1), FC(R8_SNORM, R8Snorm, CharNormalized, 1), FI(R8_UINT, R8Uint, UChar, 1), FI(R8_SINT, R8Sint, Char, 1),
    FC(R8G8_UNORM, RG8Unorm, UChar2Normalized, 2), FC(R8G8_SNORM, RG8Snorm, Char2Normalized, 2), FI(R8G8_UINT, RG8Uint, UChar2, 2), FI(R8G8_SINT, RG8Sint, Char2, 2),
    FC(R8G8B8A8_UNORM, RGBA8Unorm, UChar4Normalized, 4), FC(R8G8B8A8_SNORM, RGBA8Snorm, Char4Normalized, 4), FI(R8G8B8A8_UINT, RGBA8Uint, UChar4, 4),
    FI(R8G8B8A8_SINT, RGBA8Sint, Char4, 4), FC(R8G8B8A8_SRGB, RGBA8Unorm_sRGB, Invalid, 4),
    FC(B8G8R8A8_UNORM, BGRA8Unorm, UChar4Normalized_BGRA, 4), FC(B8G8R8A8_SRGB, BGRA8Unorm_sRGB, Invalid, 4),
    FC(A8B8G8R8_UNORM_PACK32, RGBA8Unorm, UChar4Normalized, 4), FC(A8B8G8R8_SRGB_PACK32, RGBA8Unorm_sRGB, Invalid, 4),
    FC(A2B10G10R10_UNORM_PACK32, RGB10A2Unorm, UInt1010102Normalized, 4), FI(A2B10G10R10_UINT_PACK32, RGB10A2Uint, Invalid, 4),
    FC(B10G11R11_UFLOAT_PACK32, RG11B10Float, FloatRG11B10, 4), FC(E5B9G9R9_UFLOAT_PACK32, RGB9E5Float, FloatRGB9E5, 4),
    FC(R5G6B5_UNORM_PACK16, B5G6R5Unorm, Invalid, 2),
    FC(R16_UNORM, R16Unorm, UShortNormalized, 2), FC(R16_SNORM, R16Snorm, ShortNormalized, 2), FI(R16_UINT, R16Uint, UShort, 2), FI(R16_SINT, R16Sint, Short, 2),
    FC(R16_SFLOAT, R16Float, Half, 2),
    FC(R16G16_UNORM, RG16Unorm, UShort2Normalized, 4), FC(R16G16_SNORM, RG16Snorm, Short2Normalized, 4), FI(R16G16_UINT, RG16Uint, UShort2, 4),
    FI(R16G16_SINT, RG16Sint, Short2, 4), FC(R16G16_SFLOAT, RG16Float, Half2, 4),
    FC(R16G16B16A16_UNORM, RGBA16Unorm, UShort4Normalized, 8), FC(R16G16B16A16_SNORM, RGBA16Snorm, Short4Normalized, 8),
    FI(R16G16B16A16_UINT, RGBA16Uint, UShort4, 8), FI(R16G16B16A16_SINT, RGBA16Sint, Short4, 8), FC(R16G16B16A16_SFLOAT, RGBA16Float, Half4, 8),
    FI(R32_UINT, R32Uint, UInt, 4), FI(R32_SINT, R32Sint, Int, 4), FC(R32_SFLOAT, R32Float, Float, 4),
    FI(R32G32_UINT, RG32Uint, UInt2, 8), FI(R32G32_SINT, RG32Sint, Int2, 8), FC(R32G32_SFLOAT, RG32Float, Float2, 8),
    FI(R32G32B32_UINT, Invalid, UInt3, 12), FI(R32G32B32_SINT, Invalid, Int3, 12), FC(R32G32B32_SFLOAT, Invalid, Float3, 12),
    FI(R32G32B32A32_UINT, RGBA32Uint, UInt4, 16), FI(R32G32B32A32_SINT, RGBA32Sint, Int4, 16), FC(R32G32B32A32_SFLOAT, RGBA32Float, Float4, 16),
    FD(D16_UNORM, Depth16Unorm, 2, K_DEPTH), FD(X8_D24_UNORM_PACK32, Depth32Float, 4, K_DEPTH), FD(D32_SFLOAT, Depth32Float, 4, K_DEPTH),
    FD(S8_UINT, Stencil8, 1, K_STENCIL), FD(D24_UNORM_S8_UINT, Depth32Float_Stencil8, 4, K_DS), FD(D32_SFLOAT_S8_UINT, Depth32Float_Stencil8, 8, K_DS),
    FB(BC1_RGB_UNORM_BLOCK, BC1_RGBA, 8, 4, 4), FB(BC1_RGB_SRGB_BLOCK, BC1_RGBA_sRGB, 8, 4, 4), FB(BC1_RGBA_UNORM_BLOCK, BC1_RGBA, 8, 4, 4),
    FB(BC1_RGBA_SRGB_BLOCK, BC1_RGBA_sRGB, 8, 4, 4), FB(BC2_UNORM_BLOCK, BC2_RGBA, 16, 4, 4), FB(BC2_SRGB_BLOCK, BC2_RGBA_sRGB, 16, 4, 4),
    FB(BC3_UNORM_BLOCK, BC3_RGBA, 16, 4, 4), FB(BC3_SRGB_BLOCK, BC3_RGBA_sRGB, 16, 4, 4), FB(BC4_UNORM_BLOCK, BC4_RUnorm, 8, 4, 4),
    FB(BC4_SNORM_BLOCK, BC4_RSnorm, 8, 4, 4), FB(BC5_UNORM_BLOCK, BC5_RGUnorm, 16, 4, 4), FB(BC5_SNORM_BLOCK, BC5_RGSnorm, 16, 4, 4),
    FB(BC6H_UFLOAT_BLOCK, BC6H_RGBUfloat, 16, 4, 4), FB(BC6H_SFLOAT_BLOCK, BC6H_RGBFloat, 16, 4, 4), FB(BC7_UNORM_BLOCK, BC7_RGBAUnorm, 16, 4, 4),
    FB(BC7_SRGB_BLOCK, BC7_RGBAUnorm_sRGB, 16, 4, 4),
    FB(ASTC_4x4_UNORM_BLOCK, ASTC_4x4_LDR, 16, 4, 4), FB(ASTC_4x4_SRGB_BLOCK, ASTC_4x4_sRGB, 16, 4, 4), FB(ASTC_5x5_UNORM_BLOCK, ASTC_5x5_LDR, 16, 5, 5),
    FB(ASTC_5x5_SRGB_BLOCK, ASTC_5x5_sRGB, 16, 5, 5), FB(ASTC_6x6_UNORM_BLOCK, ASTC_6x6_LDR, 16, 6, 6), FB(ASTC_6x6_SRGB_BLOCK, ASTC_6x6_sRGB, 16, 6, 6),
    FB(ASTC_8x8_UNORM_BLOCK, ASTC_8x8_LDR, 16, 8, 8), FB(ASTC_8x8_SRGB_BLOCK, ASTC_8x8_sRGB, 16, 8, 8),
    // Vertex-only formats: integers read as unnormalized floats (USCALED / SSCALED, Metal converts the integer formats to the shader's float type) and three components.
    // Super Mario Galaxy's GX vertex data is mostly these (positions as R16G16B16_SSCALED, colours as R8G8B8A8_USCALED…)
    FV(R8_USCALED, UChar, 1), FV(R8_SSCALED, Char, 1), FV(R8G8_USCALED, UChar2, 2), FV(R8G8_SSCALED, Char2, 2),
    FV(R8G8B8_UNORM, UChar3Normalized, 3), FV(R8G8B8_SNORM, Char3Normalized, 3), FV(R8G8B8_USCALED, UChar3, 3), FV(R8G8B8_SSCALED, Char3, 3),
    FV(R8G8B8_UINT, UChar3, 3), FV(R8G8B8_SINT, Char3, 3), FV(R8G8B8A8_USCALED, UChar4, 4), FV(R8G8B8A8_SSCALED, Char4, 4),
    FV(R16_USCALED, UShort, 2), FV(R16_SSCALED, Short, 2), FV(R16G16_USCALED, UShort2, 4), FV(R16G16_SSCALED, Short2, 4),
    FV(R16G16B16_UNORM, UShort3Normalized, 6), FV(R16G16B16_SNORM, Short3Normalized, 6), FV(R16G16B16_USCALED, UShort3, 6), FV(R16G16B16_SSCALED, Short3, 6),
    FV(R16G16B16_UINT, UShort3, 6), FV(R16G16B16_SINT, Short3, 6), FV(R16G16B16_SFLOAT, Half3, 6),
    FV(R16G16B16A16_USCALED, UShort4, 8), FV(R16G16B16A16_SSCALED, Short4, 8),
};
static const fmt_t *g_fmt_by[VK_FORMAT_ASTC_12x12_SRGB_BLOCK + 1];
static void fmt_init(void) { for (size_t i = 0; i < sizeof g_fmts / sizeof *g_fmts; i++) g_fmt_by[g_fmts[i].vk] = &g_fmts[i]; }
static const fmt_t *fmt_of(VkFormat f) {
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, fmt_init);
    return (unsigned)f < sizeof g_fmt_by / sizeof *g_fmt_by ? g_fmt_by[f] : NULL;
}
static int fmt_has_depth(const fmt_t *f) { return f && (f->kind == K_DEPTH || f->kind == K_DS); }
static int fmt_has_stencil(const fmt_t *f) { return f && (f->kind == K_STENCIL || f->kind == K_DS); }

// ---------- Device-wide state ----------
static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_q;
static id<MTLSharedEvent> g_ev;                 // signalled with ++g_ev_val at the end of every submit
static id<MTLDepthStencilState> g_dss_off;      // pipelines without depth / stencil
static uint64_t g_ev_val;                       // under g_qmu
static _Atomic uint64_t g_done;                 // highest value whose command buffer has completed
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER, g_done_mu = PTHREAD_MUTEX_INITIALIZER, g_mem_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_n_draw, g_n_pass, g_n_submit, g_n_copy, g_n_pso, g_mem_bytes;

// Shader table (<DM_MSL_DIR>/vk/vk_shaders.tsv): resource kind u / b / t / s / c, its (set, binding) in the game's SPIR-V and the Metal slot it was moved to;
// kind p is the push-constant block (set and binding 0), whose bytes are set straight into its buffer slot
typedef struct { char kind; uint8_t set, slot, count; uint16_t binding; } shres;   // count: an array of descriptors, elements in slots slot … slot + count − 1
typedef struct { char key[17]; int frag, nres; shres res[24]; } shinfo;
static shinfo *g_sh; static int g_nsh;
static id<MTLLibrary> g_lib;
static int sh_cmp(const void *a, const void *b) { return memcmp(((const shinfo *)a)->key, ((const shinfo *)b)->key, 16); }
static void sh_load(void) {
    const char *d = getenv("DM_MSL_DIR"); char p[1024];
    if (!d) { LOG("[vk] [RED] DM_MSL_DIR not set: the Vulkan shader library cannot be found"); return; }
    snprintf(p, sizeof p, "%s/vk/vk_shaders.tsv", d);
    FILE *f = fopen(p, "r");
    if (!f) { LOG("[vk] [RED] cannot open %s (run dm/gfx/tools/vk_shaders.py first)", p); return; }
    char line[4096]; int cap = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        char *rest = line, *key = strsep(&rest, "\t"), *stage = strsep(&rest, "\t"), *res = strsep(&rest, "\t\n");   // strsep keeps the empty resource field of a shader without resources
        if (!key || !stage || strlen(key) != 16) continue;
        if (g_nsh == cap) { cap = cap ? cap * 2 : 128; g_sh = realloc(g_sh, cap * sizeof *g_sh); }
        shinfo *s = &g_sh[g_nsh++]; memset(s, 0, sizeof *s);
        memcpy(s->key, key, 16); s->frag = !strcmp(stage, "frag");
        for (char *t = res ? strtok(res, " ") : NULL; t; t = strtok(NULL, " ")) {
            char k; unsigned st, b, sl, n = 1;
            if (sscanf(t, "%c:%u.%u=%u*%u", &k, &st, &b, &sl, &n) < 4 || !n || sl + n > 31 || s->nres == 24) { LOG("[vk] [RED] cannot parse a resource entry of shader table %s: %s", s->key, t); continue; }
            s->res[s->nres++] = (shres){k, (uint8_t)st, (uint8_t)sl, (uint8_t)n, (uint16_t)b};
        }
    }
    fclose(f);
    qsort(g_sh, g_nsh, sizeof *g_sh, sh_cmp);   // looked up by key (Super Mario Galaxy creates over 3,000 modules at boot)
    snprintf(p, sizeof p, "%s/vk/vk_shaders.metallib", d);
    NSError *err = nil;
    g_lib = [g_dev newLibraryWithURL:[NSURL fileURLWithPath:@(p)] error:&err];
    if (!g_lib) LOG("[vk] [RED] shader library failed to load %s: %s", p, err.localizedDescription.UTF8String);
}
static const shinfo *sh_find(const char *key) { shinfo k; memcpy(k.key, key, 16); return g_nsh ? bsearch(&k, g_sh, g_nsh, sizeof *g_sh, sh_cmp) : NULL; }

static void dev_init_once(void) {
    @autoreleasepool {
        g_dev = (__bridge id<MTLDevice>)mtl_device_classic();
        g_q = [g_dev newCommandQueueWithMaxCommandBufferCount:64]; g_q.label = @"vk";
        g_ev = [g_dev newSharedEvent];
        g_dss_off = [g_dev newDepthStencilStateWithDescriptor:[MTLDepthStencilDescriptor new]];
        sh_load();
        VLOG("Metal device %s; %d shader table entries", g_dev.name.UTF8String, g_nsh);
    }
}
static void dev_init(void) { static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, dev_init_once); }

// ---------- Objects ----------
// Dispatchable handles: their first word is where a loader keeps its dispatch table
struct VkInstance_T { void *loader; };
struct VkPhysicalDevice_T { void *loader; };
struct VkDevice_T { void *loader; };
struct VkQueue_T { void *loader; };
static struct VkInstance_T g_inst; static struct VkPhysicalDevice_T g_pdev; static struct VkDevice_T g_device; static struct VkQueue_T g_queue;

struct VkSurfaceKHR_T { void *win; };
struct VkDeviceMemory_T { VkDeviceSize size; uint32_t type; __strong id<MTLBuffer> buf; _Atomic(void *) raw; };
struct VkBuffer_T { VkDeviceSize size; VkBufferUsageFlags usage; VkDeviceMemory mem; VkDeviceSize off; };
struct VkBufferView_T { VkBuffer buf; const fmt_t *f; VkDeviceSize off, range; __strong id<MTLTexture> tex; };
struct VkImage_T { __strong id<MTLTexture> tex; const fmt_t *f; VkFormat fmt; VkImageType type; uint32_t w, h, d, mips, layers, samples; VkDeviceSize size; uint64_t serial;
                  VkImageUsageFlags usage; uint32_t pitch; };   // pitch: row pitch of a linear image (its texture is made over the bound memory), 0 otherwise
struct VkImageView_T { __strong id<MTLTexture> tex; VkImage img; };
struct VkSampler_T { __strong id<MTLSamplerState> s; };
struct VkShaderModule_T { const shinfo *si; __strong id<MTLFunction> fn, fn_d; char key[17]; int tried_d; };   // fn_d: the dual-source variant vk_<key>_d, looked up on first use
struct VkPipelineCache_T { int unused; };
// Query pool: a result is valid once the submit that wrote it has completed (g_done >= ticket). Timestamps are the completion time of their submit (in ns,
// timestampPeriod 1), the same for every timestamp of one submit; occlusion results are copied from the submit's visibility buffer when it completes
struct VkQueryPool_T { VkQueryType type; uint32_t n; uint64_t *val, *ticket; };
struct VkSemaphore_T { int unused; };
#define FENCE_NONE UINT64_MAX
struct VkFence_T { _Atomic uint64_t val; };   // 0 = signalled; FENCE_NONE = unsignalled with nothing pending; otherwise signalled once g_done reaches it

#define MAXB 64
typedef struct { uint32_t binding, count, base; VkDescriptorType type; int dyn, imm; } lbind;   // base: first element in a set's array; dyn: first dynamic offset in the set (-1 if not dynamic)
struct VkDescriptorSetLayout_T { uint32_t n, total, ndyn; lbind b[MAXB]; int8_t idx[128]; VkSampler *imm; };
// A descriptor holds the Metal objects it was written with (retained), as a hardware descriptor holds a copy of the object's handle: a set may still name a view, sampler
// or buffer the game has destroyed (the hardware reads it only when a draw samples it), and the replay binds every element of an array the shader takes. Descriptors
// used to point at this layer's view / sampler / buffer structs, and Super Mario Galaxy 2 crashed reading a freed view's texture from element 1 of its 8-texture array
// when the ship took off after its first galaxy (GitHub issue 68). buf / off: the buffer's memory and its offset there plus the descriptor's; tex: an image view's
// texture or a texel buffer's
typedef struct { __strong id<MTLBuffer> buf; VkDeviceSize off; __strong id<MTLTexture> tex; __strong id<MTLSamplerState> smp; } desc_t;
struct VkDescriptorPool_T { struct VkDescriptorSet_T *sets; };
struct VkDescriptorSet_T { VkDescriptorSetLayout L; VkDescriptorPool pool; struct VkDescriptorSet_T *prev, *next; desc_t *d; };   // d: L->total descriptors (ARC objects: no flexible array)
struct VkDescriptorUpdateTemplate_T { uint32_t n; VkDescriptorUpdateTemplateEntry e[]; };
struct VkPipelineLayout_T { uint32_t nset, pc_size; VkDescriptorSetLayout set[8]; };   // pc_size: end of the highest push-constant range

typedef struct { VkFormat fmt; VkAttachmentLoadOp load, sload; VkAttachmentStoreOp store, sstore; } ratt;
struct VkRenderPass_T { uint32_t natt, ncol; ratt att[16]; int col[8], res[8], depth; };
struct VkFramebuffer_T { uint32_t n, w, h, layers; VkImageView v[16]; };
struct VkPipeline_T {
    __strong id<MTLRenderPipelineState> pso; __strong id<MTLDepthStencilState> dss;
    const shinfo *vs, *fs; uint32_t vb_mask; VkPipelineLayout layout;
    MTLPrimitiveType prim; MTLCullMode cull; MTLWinding wind; MTLTriangleFillMode fill; MTLDepthClipMode clip;
    int bias; float bias_c, bias_s, bias_cl; float blend[4]; uint32_t sref_f, sref_b;
    int dyn_vp, dyn_sc, has_vp, has_sc; MTLViewport vp; VkRect2D sc;
};
struct VkCommandPool_T { struct VkCommandBuffer_T *cbs; };
struct VkCommandBuffer_T { void *loader; VkCommandPool pool; struct VkCommandBuffer_T *prev, *next; uint8_t *buf; size_t n, cap; };
struct VkSwapchainKHR_T { uint32_t n, next; VkImage img[4]; uint64_t ticket[4]; };

static void *zalloc(size_t n) { void *p = calloc(1, n); if (!p) die("[vk] out of memory %zu", n); return p; }

// ---------- Instance and physical device ----------
static VkResult fill_names(const char *const *names, uint32_t n, uint32_t *count, VkExtensionProperties *p) {
    if (!p) { *count = n; return VK_SUCCESS; }
    uint32_t k = umin(*count, n);
    for (uint32_t i = 0; i < k; i++) { memset(&p[i], 0, sizeof p[i]); strlcpy(p[i].extensionName, names[i], sizeof p[i].extensionName); p[i].specVersion = 1; }
    *count = k; return k < n ? VK_INCOMPLETE : VK_SUCCESS;
}
static const char *const g_inst_ext[] = {"VK_KHR_surface", "VK_NN_vi_surface", "VK_KHR_get_physical_device_properties2"};
static const char *const g_dev_ext[] = {"VK_KHR_swapchain", "VK_KHR_descriptor_update_template", "VK_KHR_maintenance1", "VK_KHR_get_memory_requirements2",
                                        "VK_KHR_dedicated_allocation", "VK_KHR_bind_memory2"};
static void join_names(char *out, size_t n, const char *const *names, uint32_t count) {
    out[0] = 0; size_t o = 0;
    for (uint32_t i = 0; i < count && o + 2 < n; i++) o += snprintf(out + o, n - o, "%s%s", i ? " " : "", names[i]);
    if (!count) strlcpy(out, "none", n);
}

VK(vkEnumerateInstanceVersion, VkResult, uint32_t *v) { *v = VK_API_VERSION_1_1; return VK_SUCCESS; }
VK(vkEnumerateInstanceLayerProperties, VkResult, uint32_t *n, VkLayerProperties *p) { (void)p; *n = 0; return VK_SUCCESS; }
VK(vkEnumerateInstanceExtensionProperties, VkResult, const char *layer, uint32_t *n, VkExtensionProperties *p) {
    if (layer) { *n = 0; return VK_ERROR_LAYER_NOT_PRESENT; }
    return fill_names(g_inst_ext, sizeof g_inst_ext / sizeof *g_inst_ext, n, p);
}
VK(vkCreateInstance, VkResult, const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void)a; g_log = getenv("DM_VK_LOG") != NULL;
    const VkApplicationInfo *ai = ci->pApplicationInfo; char ext[512];
    join_names(ext, sizeof ext, ci->ppEnabledExtensionNames, ci->enabledExtensionCount);
    VLOG("CreateInstance: application %s, engine %s, API %u.%u; extensions %s; %u layers", ai && ai->pApplicationName ? ai->pApplicationName : "-",
         ai && ai->pEngineName ? ai->pEngineName : "-", ai ? VK_API_VERSION_MAJOR(ai->apiVersion) : 0, ai ? VK_API_VERSION_MINOR(ai->apiVersion) : 0, ext, ci->enabledLayerCount);
    *out = &g_inst; return VK_SUCCESS;
}
VK(vkDestroyInstance, void, VkInstance i, const VkAllocationCallbacks *a) { (void)i; (void)a; }
VK(vkEnumeratePhysicalDevices, VkResult, VkInstance i, uint32_t *n, VkPhysicalDevice *p) {
    (void)i; if (!p) { *n = 1; return VK_SUCCESS; }
    if (!*n) return VK_INCOMPLETE;
    *n = 1; p[0] = &g_pdev; return VK_SUCCESS;
}
// Properties and limits: a Tegra X1 under NVIDIA's Switch driver, except where Metal is stricter
static void pdev_props(VkPhysicalDeviceProperties *p) {
    memset(p, 0, sizeof *p);
    p->apiVersion = VK_API_VERSION_1_1; p->driverVersion = VK_MAKE_API_VERSION(0, 1, 0, 0); p->vendorID = 0x10de; p->deviceID = 0x92ba;
    p->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU; strlcpy(p->deviceName, "NVIDIA Tegra X1 (nvgpu)", sizeof p->deviceName);
    VkPhysicalDeviceLimits *l = &p->limits;
    l->maxImageDimension1D = l->maxImageDimension2D = l->maxImageDimensionCube = 16384; l->maxImageDimension3D = 2048; l->maxImageArrayLayers = 2048;
    l->maxTexelBufferElements = 1u << 27; l->maxUniformBufferRange = 65536; l->maxStorageBufferRange = 1u << 30; l->maxPushConstantsSize = 256;
    l->maxMemoryAllocationCount = 4096; l->maxSamplerAllocationCount = 4000; l->bufferImageGranularity = 1024; l->maxBoundDescriptorSets = 8;
    l->maxPerStageDescriptorSamplers = 16; l->maxPerStageDescriptorUniformBuffers = 15; l->maxPerStageDescriptorStorageBuffers = 16;
    l->maxPerStageDescriptorSampledImages = 31; l->maxPerStageDescriptorStorageImages = 8; l->maxPerStageDescriptorInputAttachments = 8; l->maxPerStageResources = 128;
    l->maxDescriptorSetSamplers = 4000; l->maxDescriptorSetUniformBuffers = 90; l->maxDescriptorSetUniformBuffersDynamic = 15;
    l->maxDescriptorSetStorageBuffers = 96; l->maxDescriptorSetStorageBuffersDynamic = 16; l->maxDescriptorSetSampledImages = 4096;
    l->maxDescriptorSetStorageImages = 4096; l->maxDescriptorSetInputAttachments = 8;
    l->maxVertexInputAttributes = 31; l->maxVertexInputBindings = 14; l->maxVertexInputAttributeOffset = 2047; l->maxVertexInputBindingStride = 2048; l->maxVertexOutputComponents = 128;
    l->maxFragmentInputComponents = 128; l->maxFragmentOutputAttachments = 8; l->maxFragmentDualSrcAttachments = 1; l->maxFragmentCombinedOutputResources = 16;
    l->maxComputeSharedMemorySize = 32768; l->maxComputeWorkGroupCount[0] = l->maxComputeWorkGroupCount[1] = l->maxComputeWorkGroupCount[2] = 65535;
    l->maxComputeWorkGroupInvocations = 1024; l->maxComputeWorkGroupSize[0] = l->maxComputeWorkGroupSize[1] = 1024; l->maxComputeWorkGroupSize[2] = 64;
    l->subPixelPrecisionBits = 8; l->subTexelPrecisionBits = 8; l->mipmapPrecisionBits = 8; l->maxDrawIndexedIndexValue = 0xffffffffu; l->maxDrawIndirectCount = 0xffffffffu;
    l->maxSamplerLodBias = 15; l->maxSamplerAnisotropy = 16; l->maxViewports = 1; l->maxViewportDimensions[0] = l->maxViewportDimensions[1] = 16384;
    l->viewportBoundsRange[0] = -32768; l->viewportBoundsRange[1] = 32767; l->viewportSubPixelBits = 8;
    l->minMemoryMapAlignment = 64; l->minTexelBufferOffsetAlignment = 256; l->minUniformBufferOffsetAlignment = 256; l->minStorageBufferOffsetAlignment = 256;
    l->minTexelOffset = -8; l->maxTexelOffset = 7; l->minTexelGatherOffset = -32; l->maxTexelGatherOffset = 31;
    l->minInterpolationOffset = -0.5f; l->maxInterpolationOffset = 0.4375f; l->subPixelInterpolationOffsetBits = 4;
    l->maxFramebufferWidth = l->maxFramebufferHeight = 16384; l->maxFramebufferLayers = 2048;
    l->framebufferColorSampleCounts = l->framebufferDepthSampleCounts = l->framebufferStencilSampleCounts = l->framebufferNoAttachmentsSampleCounts = 0x7;
    l->maxColorAttachments = 8; l->sampledImageColorSampleCounts = l->sampledImageIntegerSampleCounts = l->sampledImageDepthSampleCounts = l->sampledImageStencilSampleCounts = 0x7;
    l->storageImageSampleCounts = 1; l->maxSampleMaskWords = 1; l->timestampComputeAndGraphics = 1; l->timestampPeriod = 1;
    l->maxClipDistances = 8; l->maxCullDistances = 0; l->maxCombinedClipAndCullDistances = 8; l->discreteQueuePriorities = 2;
    l->pointSizeRange[0] = 1; l->pointSizeRange[1] = 511; l->lineWidthRange[0] = l->lineWidthRange[1] = 1; l->pointSizeGranularity = 0.0625f; l->lineWidthGranularity = 0;
    l->strictLines = 1; l->standardSampleLocations = 1; l->optimalBufferCopyOffsetAlignment = 256; l->optimalBufferCopyRowPitchAlignment = 256; l->nonCoherentAtomSize = 64;
}
static void pdev_feats(VkPhysicalDeviceFeatures *f) {
    memset(f, 0, sizeof *f);
    f->robustBufferAccess = f->fullDrawIndexUint32 = f->imageCubeArray = f->independentBlend = f->sampleRateShading = f->dualSrcBlend = 1;
    f->drawIndirectFirstInstance = f->depthClamp = f->depthBiasClamp = f->fillModeNonSolid = f->largePoints = f->samplerAnisotropy = 1;
    f->textureCompressionBC = f->textureCompressionASTC_LDR = f->occlusionQueryPrecise = f->fragmentStoresAndAtomics = 1;
    f->shaderImageGatherExtended = f->shaderClipDistance = f->shaderResourceMinLod = 1;
}
static void pdev_mem(VkPhysicalDeviceMemoryProperties *m) {
    memset(m, 0, sizeof *m);
    const VkMemoryPropertyFlags dl = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    m->memoryTypeCount = 3;
    m->memoryTypes[0] = (VkMemoryType){dl, 0}; m->memoryTypes[1] = (VkMemoryType){dl | hv, 0}; m->memoryTypes[2] = (VkMemoryType){dl | hv | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0};
    m->memoryHeapCount = 1; m->memoryHeaps[0] = (VkMemoryHeap){3ull << 30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
}
static void dedicated_req(void *pn) {
    for (VkBaseOutStructure *s = pn; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) { VkMemoryDedicatedRequirements *d = (void *)s; d->prefersDedicatedAllocation = d->requiresDedicatedAllocation = VK_FALSE; }
}
VK(vkGetPhysicalDeviceProperties, void, VkPhysicalDevice pd, VkPhysicalDeviceProperties *p) { (void)pd; pdev_props(p); }
VK(vkGetPhysicalDeviceProperties2, void, VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p) { (void)pd; pdev_props(&p->properties); }
VK(vkGetPhysicalDeviceFeatures, void, VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f) { (void)pd; pdev_feats(f); }
VK(vkGetPhysicalDeviceFeatures2, void, VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f) { (void)pd; pdev_feats(&f->features); }
VK(vkGetPhysicalDeviceMemoryProperties, void, VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *m) { (void)pd; pdev_mem(m); }
VK(vkGetPhysicalDeviceMemoryProperties2, void, VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *m) { (void)pd; pdev_mem(&m->memoryProperties); }
VK(vkGetPhysicalDeviceQueueFamilyProperties, void, VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties *p) {
    (void)pd; if (!p) { *n = 1; return; }
    if (!*n) return;
    *n = 1; p[0] = (VkQueueFamilyProperties){VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT, 16, 64, {1, 1, 1}};   // every VkQueue is the same serial Metal queue
}
VK(vkGetPhysicalDeviceFormatProperties, void, VkPhysicalDevice pd, VkFormat format, VkFormatProperties *p) {
    (void)pd; memset(p, 0, sizeof *p);
    const fmt_t *f = fmt_of(format); if (!f) return;
    if (f->vf != MTLVertexFormatInvalid) p->bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
    if (f->pf == MTLPixelFormatInvalid) return;
    VkFormatFeatureFlags t = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    if (f->kind != K_INT && f->kind != K_STENCIL) t |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (f->kind == K_COLOR || f->kind == K_INT) {
        t |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if (f->kind == K_COLOR) t |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        p->bufferFeatures |= VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    }
    if (f->kind == K_DEPTH || f->kind == K_DS || f->kind == K_STENCIL) t |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    p->optimalTilingFeatures = t;
    p->linearTilingFeatures = f->kind == K_COLOR || f->kind == K_INT ? t : 0;
}
VK(vkGetPhysicalDeviceImageFormatProperties, VkResult, VkPhysicalDevice pd, VkFormat format, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
   VkImageCreateFlags flags, VkImageFormatProperties *p) {
    (void)pd; const fmt_t *f = fmt_of(format);
    if (!f || f->pf == MTLPixelFormatInvalid) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    memset(p, 0, sizeof *p);
    p->maxExtent = type == VK_IMAGE_TYPE_3D ? (VkExtent3D){2048, 2048, 2048} : (VkExtent3D){16384, type == VK_IMAGE_TYPE_1D ? 1 : 16384, 1};
    p->maxMipLevels = 15; p->maxArrayLayers = type == VK_IMAGE_TYPE_3D ? 1 : 2048;
    p->sampleCounts = type == VK_IMAGE_TYPE_2D && tiling == VK_IMAGE_TILING_OPTIMAL && !(flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
                      (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) ? 0x7 : 1;
    p->maxResourceSize = 1ull << 32;
    return VK_SUCCESS;
}
VK(vkGetImageSparseMemoryRequirements, void, VkDevice d, VkImage i, uint32_t *n, VkSparseImageMemoryRequirements *r) { (void)d; (void)i; (void)r; *n = 0; }

// ---------- Surface ----------
static VkExtent2D surf_ext(void) { return dm_opmode() ? (VkExtent2D){1920, 1080} : (VkExtent2D){1280, 720}; }
VK(vkCreateViSurfaceNN, VkResult, VkInstance i, const VkViSurfaceCreateInfoNN *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *out) {
    (void)i; (void)a; VkSurfaceKHR s = zalloc(sizeof *s); s->win = ci->window; *out = s;
    VLOG("CreateViSurfaceNN window %p", ci->window); return VK_SUCCESS;
}
VK(vkDestroySurfaceKHR, void, VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a) { (void)i; (void)a; free(s); }
VK(vkGetPhysicalDeviceSurfaceSupportKHR, VkResult, VkPhysicalDevice pd, uint32_t fam, VkSurfaceKHR s, VkBool32 *ok) { (void)pd; (void)s; *ok = fam == 0; return VK_SUCCESS; }
VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, VkResult, VkPhysicalDevice pd, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
    (void)pd; (void)s; memset(c, 0, sizeof *c);
    c->minImageCount = 2; c->maxImageCount = 4; c->currentExtent = surf_ext(); c->minImageExtent = (VkExtent2D){1, 1}; c->maxImageExtent = (VkExtent2D){1920, 1080};
    // INHERIT as well: Super Mario Galaxy creates its swapchain with preTransform INHERIT and asserts when the surface does not list it (transforms are ignored here)
    c->maxImageArrayLayers = 1; c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR; c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;
    c->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    return VK_SUCCESS;
}
VK(vkGetPhysicalDeviceSurfaceFormatsKHR, VkResult, VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkSurfaceFormatKHR *f) {
    (void)pd; (void)s;
    static const VkSurfaceFormatKHR fs[] = {{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
                                            {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}, {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    const uint32_t k = sizeof fs / sizeof *fs;
    if (!f) { *n = k; return VK_SUCCESS; }
    uint32_t m = umin(*n, k); memcpy(f, fs, m * sizeof *fs); *n = m; return m < k ? VK_INCOMPLETE : VK_SUCCESS;
}
VK(vkGetPhysicalDeviceSurfacePresentModesKHR, VkResult, VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *n, VkPresentModeKHR *m) {
    (void)pd; (void)s; if (!m) { *n = 1; return VK_SUCCESS; }
    if (!*n) return VK_INCOMPLETE;
    *n = 1; m[0] = VK_PRESENT_MODE_FIFO_KHR; return VK_SUCCESS;
}

// ---------- Device and queue ----------
VK(vkEnumerateDeviceLayerProperties, VkResult, VkPhysicalDevice pd, uint32_t *n, VkLayerProperties *p) { (void)pd; (void)p; *n = 0; return VK_SUCCESS; }
VK(vkEnumerateDeviceExtensionProperties, VkResult, VkPhysicalDevice pd, const char *layer, uint32_t *n, VkExtensionProperties *p) {
    (void)pd; if (layer) { *n = 0; return VK_ERROR_LAYER_NOT_PRESENT; }
    return fill_names(g_dev_ext, sizeof g_dev_ext / sizeof *g_dev_ext, n, p);
}
VKB(vkCreateDevice, VkResult, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    (void)pd; (void)a; dev_init();
    char ext[512]; join_names(ext, sizeof ext, ci->ppEnabledExtensionNames, ci->enabledExtensionCount);
    uint32_t nq = 0; for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) nq += ci->pQueueCreateInfos[i].queueCount;
    VLOG("CreateDevice: %u queues; extensions %s", nq, ext);
    *out = &g_device; return VK_SUCCESS;
}
VK(vkDestroyDevice, void, VkDevice d, const VkAllocationCallbacks *a) { (void)d; (void)a; }
VK(vkGetDeviceQueue, void, VkDevice d, uint32_t fam, uint32_t idx, VkQueue *q) { (void)d; (void)fam; (void)idx; *q = &g_queue; }

// ---------- Memory and buffers ----------
static id<MTLBuffer> mem_buf(VkDeviceMemory m) {
    void *r = atomic_load_explicit(&m->raw, memory_order_acquire);
    if (r) return (__bridge id<MTLBuffer>)r;
    pthread_mutex_lock(&g_mem_mu);
    if (!m->buf) {
        m->buf = [g_dev newBufferWithLength:m->size ? m->size : 16 options:MTLResourceStorageModeShared];
        if (!m->buf) LOG("[vk] [RED] cannot create an MTLBuffer of %llu bytes", (unsigned long long)m->size);
        else { atomic_store_explicit(&m->raw, (__bridge void *)m->buf, memory_order_release); g_mem_bytes += m->size; }
    }
    pthread_mutex_unlock(&g_mem_mu);
    return m->buf;
}
VK(vkAllocateMemory, VkResult, VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    (void)d; (void)a;
    if (ai->memoryTypeIndex >= 3) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkDeviceMemory m = zalloc(sizeof *m); m->size = ai->allocationSize; m->type = ai->memoryTypeIndex;
    OLOG("AllocateMemory %p %llu bytes type %u", (void *)m, (unsigned long long)m->size, m->type);
    *out = m; return VK_SUCCESS;
}
VKB(vkFreeMemory, void, VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    (void)d; (void)a; if (!m) return;
    if (m->buf) g_mem_bytes -= m->size;
    m->buf = nil; free(m);
}
VKB(vkMapMemory, VkResult, VkDevice d, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags fl, void **pp) {
    (void)d; (void)size; (void)fl;
    if (m->type == 0) RED1("mapped invisible memory (type 0)");
    id<MTLBuffer> b = mem_buf(m); if (!b) return VK_ERROR_MEMORY_MAP_FAILED;
    *pp = (uint8_t *)b.contents + off; return VK_SUCCESS;
}
VK(vkUnmapMemory, void, VkDevice d, VkDeviceMemory m) { (void)d; (void)m; }
VK(vkFlushMappedMemoryRanges, VkResult, VkDevice d, uint32_t n, const VkMappedMemoryRange *r) { (void)d; (void)n; (void)r; return VK_SUCCESS; }   // shared storage is coherent
VK(vkInvalidateMappedMemoryRanges, VkResult, VkDevice d, uint32_t n, const VkMappedMemoryRange *r) { (void)d; (void)n; (void)r; return VK_SUCCESS; }

VK(vkCreateBuffer, VkResult, VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    (void)d; (void)a; VkBuffer b = zalloc(sizeof *b); b->size = ci->size; b->usage = ci->usage; *out = b; return VK_SUCCESS;
}
VK(vkDestroyBuffer, void, VkDevice d, VkBuffer b, const VkAllocationCallbacks *a) { (void)d; (void)a; free(b); }
VK(vkGetBufferMemoryRequirements, void, VkDevice d, VkBuffer b, VkMemoryRequirements *r) {
    (void)d; r->alignment = 256; r->size = (b->size + 255) & ~255ull; r->memoryTypeBits = 7;   // 256: Metal's constant-buffer offset alignment on the Mac
}
VK(vkGetBufferMemoryRequirements2, void, VkDevice d, const VkBufferMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r) {
    h_vkGetBufferMemoryRequirements(d, i->buffer, &r->memoryRequirements); dedicated_req(r->pNext);
}
VKB(vkBindBufferMemory, VkResult, VkDevice d, VkBuffer b, VkDeviceMemory m, VkDeviceSize off) {
    (void)d; b->mem = m; b->off = off; return mem_buf(m) ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
VKB(vkBindBufferMemory2, VkResult, VkDevice d, uint32_t n, const VkBindBufferMemoryInfo *bi) {
    for (uint32_t i = 0; i < n; i++) { VkResult r = h_vkBindBufferMemory(d, bi[i].buffer, bi[i].memory, bi[i].memoryOffset); if (r) return r; }
    return VK_SUCCESS;
}
VK(vkCreateBufferView, VkResult, VkDevice d, const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *a, VkBufferView *out) {
    (void)d; (void)a; VkBufferView v = zalloc(sizeof *v); v->buf = ci->buffer; v->f = fmt_of(ci->format); v->off = ci->offset; v->range = ci->range;
    if (!v->f || v->f->pf == MTLPixelFormatInvalid) RED1("buffer view format %d not mapped", ci->format);
    *out = v; return VK_SUCCESS;
}
VK(vkDestroyBufferView, void, VkDevice d, VkBufferView v, const VkAllocationCallbacks *a) { (void)d; (void)a; if (v) { v->tex = nil; free(v); } }
static id<MTLTexture> bview_tex(VkBufferView v) {   // texel buffer as a linear texture over the buffer's storage, made at first use
    if (v->tex || !v->f || v->f->pf == MTLPixelFormatInvalid || !v->buf->mem) return v->tex;
    VkDeviceSize range = v->range == VK_WHOLE_SIZE ? v->buf->size - v->off : v->range, n = range / v->f->bytes;
    MTLTextureDescriptor *td = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:v->f->pf width:n resourceOptions:MTLResourceStorageModeShared usage:MTLTextureUsageShaderRead];
    v->tex = [mem_buf(v->buf->mem) newTextureWithDescriptor:td offset:v->buf->off + v->off bytesPerRow:n * v->f->bytes];
    if (!v->tex) RED1("cannot create the texel buffer (offset %llu)", (unsigned long long)(v->buf->off + v->off));
    return v->tex;
}

// ---------- Images, views, samplers ----------
static VkDeviceSize img_bytes(const fmt_t *f, uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t layers, uint32_t samples) {
    VkDeviceSize s = 0;
    for (uint32_t m = 0; m < mips; m++) {
        uint32_t mw = umax(1, w >> m), mh = umax(1, h >> m), md = umax(1, d >> m);
        s += (VkDeviceSize)((mw + f->bw - 1) / f->bw) * ((mh + f->bh - 1) / f->bh) * md * f->bytes;
    }
    return s * layers * samples;
}
VKB(vkCreateImage, VkResult, VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out) {
    (void)d; (void)a; dev_init();
    const fmt_t *f = fmt_of(ci->format);
    if (!f || f->pf == MTLPixelFormatInvalid) { LOG("[vk] [RED] image format %d not mapped", ci->format); return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    // Linear 2D images with one level and layer alias their memory, as on the Switch: the texture is made over the bound buffer at vkBindImageMemory, with rows at
    // Metal's linear-texture alignment (which vkGetImageSubresourceLayout reports). Super Mario Galaxy fills staging images from the CPU this way and samples a 4x4 one
    if (ci->tiling == VK_IMAGE_TILING_LINEAR && ci->imageType == VK_IMAGE_TYPE_2D && ci->mipLevels <= 1 && ci->arrayLayers <= 1 && ci->samples <= 1 && f->kind != K_BLOCK && !fmt_has_depth(f) && !fmt_has_stencil(f)) {
        NSUInteger al = [g_dev minimumLinearTextureAlignmentForPixelFormat:f->pf]; if (!al) al = 256;
        VkImage im = zalloc(sizeof *im);
        im->f = f; im->fmt = ci->format; im->type = ci->imageType; im->w = ci->extent.width; im->h = umax(1, ci->extent.height); im->d = im->mips = im->layers = im->samples = 1;
        im->usage = ci->usage; im->pitch = (uint32_t)(((NSUInteger)im->w * f->bytes + al - 1) / al * al); im->size = (VkDeviceSize)im->pitch * im->h;
        OLOG("CreateImage %p %ux%u format %d linear (pitch %u) usage %#x", (void *)im, im->w, im->h, ci->format, im->pitch, ci->usage);
        *out = im; return VK_SUCCESS;
    }
    if (ci->tiling == VK_IMAGE_TILING_LINEAR) RED1("linear image %ux%u format %d levels %u layers %u: content does not follow the bound memory", ci->extent.width, ci->extent.height, ci->format, ci->mipLevels, ci->arrayLayers);
    @autoreleasepool {
        uint32_t layers = umax(1, ci->arrayLayers), samples = umax(1, ci->samples);
        int cube = (ci->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && layers % 6 == 0;
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.pixelFormat = f->pf; td.width = ci->extent.width; td.height = umax(1, ci->extent.height); td.depth = umax(1, ci->extent.depth);
        td.mipmapLevelCount = umax(1, ci->mipLevels); td.sampleCount = samples;
        if (ci->imageType == VK_IMAGE_TYPE_3D) td.textureType = MTLTextureType3D;
        else if (ci->imageType == VK_IMAGE_TYPE_1D) td.textureType = layers > 1 ? MTLTextureType1DArray : MTLTextureType1D;
        else if (samples > 1) td.textureType = layers > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
        else if (cube) td.textureType = layers > 6 ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
        else td.textureType = layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
        td.arrayLength = cube ? layers / 6 : layers;
        MTLTextureUsage u = MTLTextureUsageShaderRead;
        if (ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) u |= MTLTextureUsageRenderTarget;
        if (ci->usage & VK_IMAGE_USAGE_STORAGE_BIT) u |= MTLTextureUsageShaderWrite;
        // A stencil-aspect view of a depth-stencil image is an X32_Stencil8 view, which Metal allows only with the format-view usage (Vulkan needs no mutable-format flag for it;
        // the API validation layer aborts without it). As in the NVN layer, it costs nothing measured on a depth-stencil texture
        if ((ci->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) || f->pf == MTLPixelFormatDepth32Float_Stencil8) u |= MTLTextureUsagePixelFormatView;
        td.usage = u; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
        if (!t) { LOG("[vk] [RED] cannot create image %ux%ux%u format %d levels %u layers %u", ci->extent.width, ci->extent.height, ci->extent.depth, ci->format, ci->mipLevels, layers); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
        VkImage im = zalloc(sizeof *im);
        im->tex = t; im->f = f; im->fmt = ci->format; im->type = ci->imageType; im->w = (uint32_t)td.width; im->h = (uint32_t)td.height; im->d = (uint32_t)td.depth;
        im->mips = (uint32_t)td.mipmapLevelCount; im->layers = layers; im->samples = samples; im->size = img_bytes(f, im->w, im->h, im->d, im->mips, layers, samples);
        OLOG("CreateImage %p %ux%ux%u format %d levels %u layers %u usage %#x", (void *)im, im->w, im->h, im->d, ci->format, im->mips, layers, ci->usage);
        *out = im;
    }
    return VK_SUCCESS;
}
VKB(vkDestroyImage, void, VkDevice d, VkImage im, const VkAllocationCallbacks *a) { (void)d; (void)a; if (im) { im->tex = nil; free(im); } }
VK(vkGetImageMemoryRequirements, void, VkDevice d, VkImage im, VkMemoryRequirements *r) {
    (void)d; r->alignment = 4096; r->size = (im->size + 4095) & ~4095ull; r->memoryTypeBits = 7;
}
VK(vkGetImageMemoryRequirements2, void, VkDevice d, const VkImageMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r) {
    h_vkGetImageMemoryRequirements(d, i->image, &r->memoryRequirements); dedicated_req(r->pNext);
}
VK(vkGetImageSubresourceLayout, void, VkDevice d, VkImage im, const VkImageSubresource *sr, VkSubresourceLayout *l) {   // a linear image's own pitch; others tightly packed, mip levels then layers
    (void)d; const fmt_t *f = im->f; VkDeviceSize off = 0;
    if (im->pitch) { l->offset = 0; l->rowPitch = im->pitch; l->depthPitch = l->arrayPitch = l->size = im->size; return; }
    for (uint32_t m = 0; m < sr->mipLevel && m < im->mips; m++) off += img_bytes(f, umax(1, im->w >> m), umax(1, im->h >> m), umax(1, im->d >> m), 1, im->layers, im->samples);
    uint32_t w = umax(1, im->w >> sr->mipLevel), h = umax(1, im->h >> sr->mipLevel), dd = umax(1, im->d >> sr->mipLevel);
    l->rowPitch = (VkDeviceSize)((w + f->bw - 1) / f->bw) * f->bytes; l->depthPitch = l->rowPitch * ((h + f->bh - 1) / f->bh);
    l->arrayPitch = l->depthPitch * dd; l->size = l->arrayPitch; l->offset = off + l->arrayPitch * sr->arrayLayer;
}
VKB(vkBindImageMemory, VkResult, VkDevice d, VkImage im, VkDeviceMemory m, VkDeviceSize off) {   // other images keep their own storage
    (void)d; if (!im->pitch) return VK_SUCCESS;
    id<MTLBuffer> b = mem_buf(m); if (!b) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:im->f->pf width:im->w height:im->h mipmapped:NO];
        td.storageMode = MTLStorageModeShared; td.resourceOptions = MTLResourceStorageModeShared;
        td.usage = MTLTextureUsageShaderRead | (im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT ? MTLTextureUsageRenderTarget : 0) | (im->usage & VK_IMAGE_USAGE_STORAGE_BIT ? MTLTextureUsageShaderWrite : 0);
        im->tex = [b newTextureWithDescriptor:td offset:off bytesPerRow:im->pitch];
    }
    if (!im->tex) { LOG("[vk] [RED] cannot create linear image %ux%u format %d (offset %llu pitch %u)", im->w, im->h, im->fmt, (unsigned long long)off, im->pitch); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    return VK_SUCCESS;
}
VKB(vkBindImageMemory2, VkResult, VkDevice d, uint32_t n, const VkBindImageMemoryInfo *bi) {
    for (uint32_t i = 0; i < n; i++) { VkResult r = h_vkBindImageMemory(d, bi[i].image, bi[i].memory, bi[i].memoryOffset); if (r) return r; }
    return VK_SUCCESS;
}

static MTLTextureSwizzle swz(VkComponentSwizzle s, MTLTextureSwizzle own) {
    switch (s) {
        case VK_COMPONENT_SWIZZLE_ZERO: return MTLTextureSwizzleZero;
        case VK_COMPONENT_SWIZZLE_ONE: return MTLTextureSwizzleOne;
        case VK_COMPONENT_SWIZZLE_R: return MTLTextureSwizzleRed;
        case VK_COMPONENT_SWIZZLE_G: return MTLTextureSwizzleGreen;
        case VK_COMPONENT_SWIZZLE_B: return MTLTextureSwizzleBlue;
        case VK_COMPONENT_SWIZZLE_A: return MTLTextureSwizzleAlpha;
        default: return own;
    }
}
VKB(vkCreateImageView, VkResult, VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out) {
    (void)d; (void)a;
    VkImage im = ci->image; const fmt_t *f = fmt_of(ci->format);
    MTLPixelFormat pf = f && f->pf != MTLPixelFormatInvalid ? f->pf : im->f->pf;
    const VkImageSubresourceRange *r = &ci->subresourceRange;
    if (r->aspectMask == VK_IMAGE_ASPECT_STENCIL_BIT && pf == MTLPixelFormatDepth32Float_Stencil8) pf = MTLPixelFormatX32_Stencil8;
    uint32_t lv = r->levelCount == VK_REMAINING_MIP_LEVELS ? im->mips - r->baseMipLevel : r->levelCount;
    uint32_t ly = r->layerCount == VK_REMAINING_ARRAY_LAYERS ? im->layers - r->baseArrayLayer : r->layerCount;
    MTLTextureType tt;
    switch (ci->viewType) {
        case VK_IMAGE_VIEW_TYPE_1D: tt = MTLTextureType1D; break;
        case VK_IMAGE_VIEW_TYPE_1D_ARRAY: tt = MTLTextureType1DArray; break;
        case VK_IMAGE_VIEW_TYPE_3D: tt = MTLTextureType3D; break;
        case VK_IMAGE_VIEW_TYPE_CUBE: tt = MTLTextureTypeCube; break;
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: tt = MTLTextureTypeCubeArray; break;
        case VK_IMAGE_VIEW_TYPE_2D_ARRAY: tt = im->samples > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DArray; break;
        default: tt = im->samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D; break;
    }
    int bc1rgb = im->fmt == VK_FORMAT_BC1_RGB_UNORM_BLOCK || im->fmt == VK_FORMAT_BC1_RGB_SRGB_BLOCK;   // Metal decodes BC1 with punch-through alpha; Vulkan's RGB variant has none
    MTLTextureSwizzleChannels sw = MTLTextureSwizzleChannelsMake(swz(ci->components.r, MTLTextureSwizzleRed), swz(ci->components.g, MTLTextureSwizzleGreen),
                                                                 swz(ci->components.b, MTLTextureSwizzleBlue), bc1rgb ? MTLTextureSwizzleOne : swz(ci->components.a, MTLTextureSwizzleAlpha));
    int ident = sw.red == MTLTextureSwizzleRed && sw.green == MTLTextureSwizzleGreen && sw.blue == MTLTextureSwizzleBlue && sw.alpha == MTLTextureSwizzleAlpha;
    VkImageView v = zalloc(sizeof *v); v->img = im;
    if (pf == im->tex.pixelFormat && tt == im->tex.textureType && !r->baseMipLevel && lv == im->mips && !r->baseArrayLayer && ly == im->layers && ident) v->tex = im->tex;
    else @autoreleasepool {
        v->tex = [im->tex newTextureViewWithPixelFormat:pf textureType:tt levels:NSMakeRange(r->baseMipLevel, lv)
                                                  slices:NSMakeRange(tt == MTLTextureType3D ? 0 : r->baseArrayLayer, tt == MTLTextureType3D ? 1 : ly) swizzle:sw];
        if (!v->tex) LOG("[vk] [RED] cannot create image view: format %d → %lu type %lu levels %u+%u layers %u+%u", im->fmt, (unsigned long)pf, (unsigned long)tt, r->baseMipLevel, lv, r->baseArrayLayer, ly);
    }
    *out = v; return VK_SUCCESS;
}
VKB(vkDestroyImageView, void, VkDevice d, VkImageView v, const VkAllocationCallbacks *a) { (void)d; (void)a; if (v) { v->tex = nil; free(v); } }

static MTLSamplerAddressMode addr_mode(VkSamplerAddressMode m) {
    switch (m) {
        case VK_SAMPLER_ADDRESS_MODE_REPEAT: return MTLSamplerAddressModeRepeat;
        case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
        case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return MTLSamplerAddressModeMirrorClampToEdge;
        default: return MTLSamplerAddressModeClampToEdge;
    }
}
VKB(vkCreateSampler, VkResult, VkDevice d, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out) {
    (void)d; (void)a; dev_init();
    @autoreleasepool {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = ci->minFilter == VK_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.magFilter = ci->magFilter == VK_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = ci->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        sd.sAddressMode = addr_mode(ci->addressModeU); sd.tAddressMode = addr_mode(ci->addressModeV); sd.rAddressMode = addr_mode(ci->addressModeW);
        sd.maxAnisotropy = ci->anisotropyEnable ? (NSUInteger)fminf(fmaxf(ci->maxAnisotropy, 1), 16) : 1;
        if (ci->compareEnable) sd.compareFunction = (MTLCompareFunction)ci->compareOp;   // VkCompareOp and MTLCompareFunction share their values
        sd.borderColor = ci->borderColor >= VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE ? MTLSamplerBorderColorOpaqueWhite :
                         ci->borderColor >= VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorTransparentBlack;
        sd.lodMinClamp = ci->minLod; sd.lodMaxClamp = ci->maxLod;
        sd.normalizedCoordinates = !ci->unnormalizedCoordinates;
        if (ci->mipLodBias != 0) RED1("sampler LOD bias %.2f not implemented", ci->mipLodBias);
        VkSampler s = zalloc(sizeof *s); s->s = [g_dev newSamplerStateWithDescriptor:sd];
        if (!s->s) LOG("[vk] [RED] cannot create the sampler");
        *out = s;
    }
    return VK_SUCCESS;
}
VKB(vkDestroySampler, void, VkDevice d, VkSampler s, const VkAllocationCallbacks *a) { (void)d; (void)a; if (s) { s->s = nil; free(s); } }

// ---------- Descriptor sets ----------
static int is_dyn(VkDescriptorType t) { return t == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC; }
VK(vkCreateDescriptorSetLayout, VkResult, VkDevice d, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    (void)d; (void)a;
    if (ci->bindingCount > MAXB) { LOG("[vk] [RED] descriptor set layout with %u bindings, over %d", ci->bindingCount, MAXB); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkDescriptorSetLayout L = zalloc(sizeof *L); memset(L->idx, -1, sizeof L->idx);
    const VkDescriptorSetLayoutBinding *src[MAXB];
    for (uint32_t i = 0; i < ci->bindingCount; i++) {   // sorted by binding number: the order dynamic offsets are consumed in
        uint32_t j = i; src[i] = &ci->pBindings[i];
        while (j && src[j - 1]->binding > src[j]->binding) { const VkDescriptorSetLayoutBinding *t = src[j]; src[j] = src[j - 1]; src[j - 1] = t; j--; }
    }
    int nimm = 0;
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *b = src[i]; lbind *o = &L->b[L->n];
        if (b->binding >= 128) { LOG("[vk] [RED] binding number %u over 127", b->binding); continue; }
        o->binding = b->binding; o->count = b->descriptorCount; o->type = b->descriptorType; o->base = L->total; o->dyn = -1;
        if (is_dyn(b->descriptorType)) { o->dyn = (int)L->ndyn; L->ndyn += b->descriptorCount; }
        o->imm = b->pImmutableSamplers && (b->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        nimm += o->imm;
        L->idx[b->binding] = (int8_t)L->n++; L->total += b->descriptorCount;
    }
    if (nimm) {
        L->imm = zalloc(umax(1, L->total) * sizeof *L->imm);
        for (uint32_t i = 0, k = 0; i < ci->bindingCount; i++) {
            const VkDescriptorSetLayoutBinding *b = src[i]; if (b->binding >= 128) continue;
            if (L->b[k].imm) memcpy(&L->imm[L->b[k].base], b->pImmutableSamplers, b->descriptorCount * sizeof *L->imm);
            k++;
        }
    }
    if (L->ndyn > 16) LOG("[vk] [RED] %u dynamic offsets in one set, over 16", L->ndyn);
    *out = L; return VK_SUCCESS;
}
VK(vkDestroyDescriptorSetLayout, void, VkDevice d, VkDescriptorSetLayout L, const VkAllocationCallbacks *a) { (void)d; (void)L; (void)a; }   // kept: sets allocated from it still read it
VK(vkCreateDescriptorPool, VkResult, VkDevice d, const VkDescriptorPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorPool *out) {
    (void)d; (void)ci; (void)a; *out = zalloc(sizeof **out); return VK_SUCCESS;
}
static void set_free(VkDescriptorSet s) {
    VkDescriptorPool p = s->pool;
    if (s->prev) s->prev->next = s->next; else p->sets = s->next;
    if (s->next) s->next->prev = s->prev;
    for (uint32_t k = 0; k < s->L->total; k++) s->d[k] = (desc_t){0};   // releases what the descriptors hold
    free(s->d); free(s);
}
VK(vkResetDescriptorPool, VkResult, VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f) { (void)d; (void)f; while (p->sets) set_free(p->sets); return VK_SUCCESS; }
VK(vkDestroyDescriptorPool, void, VkDevice d, VkDescriptorPool p, const VkAllocationCallbacks *a) { (void)a; if (p) { h_vkResetDescriptorPool(d, p, 0); free(p); } }
VK(vkAllocateDescriptorSets, VkResult, VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out) {
    (void)d;
    for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        VkDescriptorSetLayout L = ai->pSetLayouts[i];
        VkDescriptorSet s = zalloc(sizeof *s); s->d = zalloc(umax(1, L->total) * sizeof(desc_t));
        s->L = L; s->pool = ai->descriptorPool; s->next = s->pool->sets; if (s->next) s->next->prev = s; s->pool->sets = s;
        if (L->imm) for (uint32_t k = 0; k < L->total; k++) s->d[k].smp = L->imm[k] ? L->imm[k]->s : nil;
        out[i] = s;
    }
    return VK_SUCCESS;
}
VK(vkFreeDescriptorSets, VkResult, VkDevice d, VkDescriptorPool p, uint32_t n, const VkDescriptorSet *s) {
    (void)d; (void)p; for (uint32_t i = 0; i < n; i++) if (s[i]) set_free(s[i]);
    return VK_SUCCESS;
}
// Element `elem` of `binding`; an index past the binding's count continues into the next bindings (Vulkan's consecutive-binding updates)
static desc_t *set_elem(VkDescriptorSet s, uint32_t binding, uint32_t elem, const lbind **lb) {
    const VkDescriptorSetLayout L = s->L; int i = binding < 128 ? L->idx[binding] : -1;
    if (i < 0) { RED1("write descriptor: binding %u not in the layout", binding); return NULL; }
    while (elem >= L->b[i].count) { elem -= L->b[i].count; if (++i >= (int)L->n) { RED1("write descriptor past the end of the layout (binding %u)", binding); return NULL; } }
    *lb = &L->b[i]; return &s->d[L->b[i].base + elem];
}
static void desc_write(desc_t *e, const lbind *lb, VkDescriptorType t, const VkDescriptorImageInfo *ii, const VkDescriptorBufferInfo *bi, const VkBufferView *bv) {
    switch (t) {
        case VK_DESCRIPTOR_TYPE_SAMPLER: if (!lb->imm) e->smp = ii->sampler ? ii->sampler->s : nil; break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: e->tex = ii->imageView ? ii->imageView->tex : nil; if (!lb->imm) e->smp = ii->sampler ? ii->sampler->s : nil; break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: e->tex = ii->imageView ? ii->imageView->tex : nil; break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: e->tex = *bv ? bview_tex(*bv) : nil; break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC: case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
            VkBuffer b = bi->buffer; e->buf = b && b->mem ? mem_buf(b->mem) : nil; e->off = b ? b->off + bi->offset : 0; break;   // a buffer is bound to its memory before it is written
        }
        default: RED1("descriptor type %d not implemented", t); break;
    }
}
VK(vkUpdateDescriptorSets, void, VkDevice d, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) {
    (void)d;
    for (uint32_t i = 0; i < nw; i++)
        for (uint32_t j = 0; j < w[i].descriptorCount; j++) {
            const lbind *lb; desc_t *e = set_elem(w[i].dstSet, w[i].dstBinding, w[i].dstArrayElement + j, &lb); if (!e) break;
            desc_write(e, lb, w[i].descriptorType, w[i].pImageInfo ? &w[i].pImageInfo[j] : NULL, w[i].pBufferInfo ? &w[i].pBufferInfo[j] : NULL,
                       w[i].pTexelBufferView ? &w[i].pTexelBufferView[j] : NULL);
        }
    for (uint32_t i = 0; i < nc; i++)
        for (uint32_t j = 0; j < c[i].descriptorCount; j++) {
            const lbind *la, *lb; desc_t *s = set_elem(c[i].srcSet, c[i].srcBinding, c[i].srcArrayElement + j, &la), *t = set_elem(c[i].dstSet, c[i].dstBinding, c[i].dstArrayElement + j, &lb);
            if (s && t) *t = *s;
        }
}
VK(vkCreateDescriptorUpdateTemplate, VkResult, VkDevice d, const VkDescriptorUpdateTemplateCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out) {
    (void)d; (void)a;
    if (ci->templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET) RED1("descriptor update template type %d not implemented", ci->templateType);
    VkDescriptorUpdateTemplate t = zalloc(sizeof *t + ci->descriptorUpdateEntryCount * sizeof(VkDescriptorUpdateTemplateEntry));
    t->n = ci->descriptorUpdateEntryCount; memcpy(t->e, ci->pDescriptorUpdateEntries, t->n * sizeof *t->e);
    *out = t; return VK_SUCCESS;
}
VK(vkDestroyDescriptorUpdateTemplate, void, VkDevice d, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks *a) { (void)d; (void)a; free(t); }
VK(vkUpdateDescriptorSetWithTemplate, void, VkDevice d, VkDescriptorSet s, VkDescriptorUpdateTemplate t, const void *data) {
    (void)d;
    for (uint32_t i = 0; i < t->n; i++) {
        const VkDescriptorUpdateTemplateEntry *en = &t->e[i];
        for (uint32_t j = 0; j < en->descriptorCount; j++) {
            const uint8_t *p = (const uint8_t *)data + en->offset + j * en->stride;
            const lbind *lb; desc_t *e = set_elem(s, en->dstBinding, en->dstArrayElement + j, &lb); if (!e) break;
            desc_write(e, lb, en->descriptorType, (const VkDescriptorImageInfo *)p, (const VkDescriptorBufferInfo *)p, (const VkBufferView *)p);
        }
    }
}
VK(vkCreatePipelineLayout, VkResult, VkDevice d, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    (void)d; (void)a; VkPipelineLayout l = zalloc(sizeof *l);
    if (ci->setLayoutCount > 8) LOG("[vk] [RED] pipeline layout with %u sets, over 8", ci->setLayoutCount);
    l->nset = umin(ci->setLayoutCount, 8); memcpy(l->set, ci->pSetLayouts, l->nset * sizeof *l->set);
    for (uint32_t i = 0; i < ci->pushConstantRangeCount; i++) l->pc_size = umax(l->pc_size, ci->pPushConstantRanges[i].offset + ci->pPushConstantRanges[i].size);
    if (l->pc_size > 256) { LOG("[vk] [RED] push constant range %u bytes, over 256", l->pc_size); l->pc_size = 256; }
    *out = l; return VK_SUCCESS;
}
VK(vkDestroyPipelineLayout, void, VkDevice d, VkPipelineLayout l, const VkAllocationCallbacks *a) { (void)d; (void)l; (void)a; }   // kept: pipelines point at it

// ---------- Render passes, framebuffers ----------
VK(vkCreateRenderPass, VkResult, VkDevice d, const VkRenderPassCreateInfo *ci, const VkAllocationCallbacks *a, VkRenderPass *out) {
    (void)d; (void)a;
    if (ci->attachmentCount > 16 || !ci->subpassCount) { LOG("[vk] [RED] render pass: %u attachments, %u subpasses", ci->attachmentCount, ci->subpassCount); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    if (ci->subpassCount > 1) RED1("render pass has %u subpasses: only the first is used", ci->subpassCount);
    VkRenderPass rp = zalloc(sizeof *rp); rp->natt = ci->attachmentCount;
    for (uint32_t i = 0; i < rp->natt; i++) {
        const VkAttachmentDescription *at = &ci->pAttachments[i];
        rp->att[i] = (ratt){at->format, at->loadOp, at->stencilLoadOp, at->storeOp, at->stencilStoreOp};
    }
    const VkSubpassDescription *sp = &ci->pSubpasses[0];
    rp->ncol = umin(sp->colorAttachmentCount, 8);
    for (uint32_t i = 0; i < rp->ncol; i++) {
        uint32_t c = sp->pColorAttachments[i].attachment, r = sp->pResolveAttachments ? sp->pResolveAttachments[i].attachment : VK_ATTACHMENT_UNUSED;
        rp->col[i] = c == VK_ATTACHMENT_UNUSED ? -1 : (int)c; rp->res[i] = r == VK_ATTACHMENT_UNUSED ? -1 : (int)r;
    }
    rp->depth = sp->pDepthStencilAttachment && sp->pDepthStencilAttachment->attachment != VK_ATTACHMENT_UNUSED ? (int)sp->pDepthStencilAttachment->attachment : -1;
    if (sp->inputAttachmentCount) RED1("input attachments not implemented");
    *out = rp; return VK_SUCCESS;
}
VK(vkDestroyRenderPass, void, VkDevice d, VkRenderPass rp, const VkAllocationCallbacks *a) { (void)d; (void)rp; (void)a; }   // kept: recorded commands may still name it
VK(vkCreateFramebuffer, VkResult, VkDevice d, const VkFramebufferCreateInfo *ci, const VkAllocationCallbacks *a, VkFramebuffer *out) {
    (void)d; (void)a;
    if (ci->attachmentCount > 16) { LOG("[vk] [RED] framebuffer with %u attachments", ci->attachmentCount); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    VkFramebuffer fb = zalloc(sizeof *fb); fb->n = ci->attachmentCount; fb->w = ci->width; fb->h = ci->height; fb->layers = ci->layers;
    memcpy(fb->v, ci->pAttachments, fb->n * sizeof *fb->v);
    *out = fb; return VK_SUCCESS;
}
VK(vkDestroyFramebuffer, void, VkDevice d, VkFramebuffer fb, const VkAllocationCallbacks *a) { (void)d; (void)a; free(fb); }

// ---------- Shaders and pipelines ----------
// DM_VK_CAPTURE=<dir> (Mac): every SPIR-V module the game creates is written to <dir>/<key>.spv. Super Mario Galaxy generates its GX shaders as SPIR-V at run time
// (over 3,000 at boot, from the pipeline database), so no game file holds them; vk_shaders.py translates a capture folder like a RomFS
static void sh_capture(const char *key, const void *code, size_t n) {
    static const char *dir; static int init;
    if (!init) { init = 1; dir = getenv("DM_VK_CAPTURE"); if (dir) VLOG("shader capture → %s", dir); }
    if (!dir) return;
    char p[1024]; snprintf(p, sizeof p, "%s/%s.spv", dir, key);
    if (access(p, F_OK) == 0) return;
    char t[1100]; snprintf(t, sizeof t, "%s.%d.tmp", p, getpid());
    FILE *f = fopen(t, "wb"); if (!f) { RED1("shader capture cannot write to %s", dir); return; }
    int ok = fwrite(code, 1, n, f) == n; ok &= fclose(f) == 0;
    if (ok) rename(t, p); else unlink(t);   // whole files only: a capture run may be killed at any time
}
VKB(vkCreateShaderModule, VkResult, VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out) {
    (void)d; (void)a; dev_init();
    uint8_t h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(ci->pCode, (CC_LONG)ci->codeSize, h);
    VkShaderModule m = zalloc(sizeof *m);
    for (int i = 0; i < 8; i++) snprintf(m->key + 2 * i, 3, "%02x", h[i]);
    sh_capture(m->key, ci->pCode, ci->codeSize);
    m->si = sh_find(m->key);
    @autoreleasepool { if (m->si && g_lib) m->fn = [g_lib newFunctionWithName:[NSString stringWithFormat:@"vk_%s", m->key]]; }
    if (!m->fn) LOG("[vk] [RED] shader %s (%zu bytes of SPIR-V) is not in the library", m->key, ci->codeSize);
    else OLOG("CreateShaderModule %s", m->key);
    *out = m; return VK_SUCCESS;
}
VKB(vkDestroyShaderModule, void, VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a) { (void)d; (void)a; if (m) { m->fn = m->fn_d = nil; free(m); } }
// Pipeline caches hold nothing: the game's initial data is NVIDIA's binary cache (Super Mario Galaxy's data/database/vulkan/*.cache), and Metal keeps its own
VK(vkCreatePipelineCache, VkResult, VkDevice d, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineCache *out) {
    (void)d; (void)ci; (void)a; *out = zalloc(sizeof **out); return VK_SUCCESS;
}
VK(vkDestroyPipelineCache, void, VkDevice d, VkPipelineCache c, const VkAllocationCallbacks *a) { (void)d; (void)a; free(c); }
VK(vkGetPipelineCacheData, VkResult, VkDevice d, VkPipelineCache c, size_t *n, void *data) { (void)d; (void)c; (void)data; *n = 0; return VK_SUCCESS; }
VK(vkMergePipelineCaches, VkResult, VkDevice d, VkPipelineCache dst, uint32_t n, const VkPipelineCache *src) { (void)d; (void)dst; (void)n; (void)src; return VK_SUCCESS; }

static MTLBlendFactor blend_f(VkBlendFactor f) {
    static const MTLBlendFactor t[] = {MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor, MTLBlendFactorDestinationColor,
        MTLBlendFactorOneMinusDestinationColor, MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationAlpha, MTLBlendFactorOneMinusDestinationAlpha,
        MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor, MTLBlendFactorBlendAlpha, MTLBlendFactorOneMinusBlendAlpha, MTLBlendFactorSourceAlphaSaturated,
        MTLBlendFactorSource1Color, MTLBlendFactorOneMinusSource1Color, MTLBlendFactorSource1Alpha, MTLBlendFactorOneMinusSource1Alpha};
    return (unsigned)f < sizeof t / sizeof *t ? t[f] : MTLBlendFactorOne;
}
static MTLColorWriteMask write_mask(VkColorComponentFlags m) {   // the bit order is reversed: Vulkan R = 1 … A = 8, Metal R = 8 … A = 1
    return (m & VK_COLOR_COMPONENT_R_BIT ? MTLColorWriteMaskRed : 0) | (m & VK_COLOR_COMPONENT_G_BIT ? MTLColorWriteMaskGreen : 0) |
           (m & VK_COLOR_COMPONENT_B_BIT ? MTLColorWriteMaskBlue : 0) | (m & VK_COLOR_COMPONENT_A_BIT ? MTLColorWriteMaskAlpha : 0);
}
static MTLStencilDescriptor *stencil_d(const VkStencilOpState *s) {   // VkStencilOp / VkCompareOp share their values with Metal's
    MTLStencilDescriptor *d = [MTLStencilDescriptor new];
    d.stencilCompareFunction = (MTLCompareFunction)s->compareOp; d.stencilFailureOperation = (MTLStencilOperation)s->failOp;
    d.depthFailureOperation = (MTLStencilOperation)s->depthFailOp; d.depthStencilPassOperation = (MTLStencilOperation)s->passOp;
    d.readMask = s->compareMask; d.writeMask = s->writeMask;
    return d;
}
static MTLViewport vp_of(const VkViewport *v) { return (MTLViewport){v->x, v->y, v->width, v->height, v->minDepth, v->maxDepth}; }   // the shaders flip y, so the viewport stays as given
static int src1_factor(VkBlendFactor f) { return f >= VK_BLEND_FACTOR_SRC1_COLOR && f <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA; }
static int dual_source(const VkPipelineColorBlendStateCreateInfo *cb) {   // a blend factor reads the second source
    for (uint32_t i = 0; cb && i < cb->attachmentCount; i++) {
        const VkPipelineColorBlendAttachmentState *b = &cb->pAttachments[i];
        if (b->blendEnable && (src1_factor(b->srcColorBlendFactor) || src1_factor(b->dstColorBlendFactor) || src1_factor(b->srcAlphaBlendFactor) || src1_factor(b->dstAlphaBlendFactor))) return 1;
    }
    return 0;
}
static VkPipeline pipe_make(const VkGraphicsPipelineCreateInfo *ci) {
    VkPipeline p = zalloc(sizeof *p); p->layout = ci->layout;
    @autoreleasepool {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        char keys[64] = ""; int dual = dual_source(ci->pColorBlendState);
        for (uint32_t s = 0; s < ci->stageCount; s++) {
            const VkPipelineShaderStageCreateInfo *st = &ci->pStages[s]; VkShaderModule m = st->module;
            if (st->pSpecializationInfo && st->pSpecializationInfo->mapEntryCount) RED1("specialization constants not implemented");
            if (st->stage == VK_SHADER_STAGE_VERTEX_BIT) { pd.vertexFunction = m->fn; p->vs = m->si; }
            else if (st->stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
                if (dual && m->fn) {   // pipelines may be built on several threads
                    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; pthread_mutex_lock(&mu);
                    if (!m->tried_d) { m->tried_d = 1; m->fn_d = [g_lib newFunctionWithName:[NSString stringWithFormat:@"vk_%s_d", m->key]]; }
                    pthread_mutex_unlock(&mu);
                }
                pd.fragmentFunction = dual && m->fn_d ? m->fn_d : m->fn; p->fs = m->si;
            }
            else RED1("shader stage %#x not implemented", st->stage);
            strlcat(keys, s ? " " : "", sizeof keys); strlcat(keys, m->key, sizeof keys);
        }
        if (!pd.vertexFunction) { LOG("[vk] [RED] pipeline has no usable vertex shader (%s)", keys); return p; }
        const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        uint32_t used = 0;   // bindings some mapped attribute reads: Metal's validation rejects a buffer layout no attribute uses
        for (uint32_t i = 0; vi && i < vi->vertexAttributeDescriptionCount; i++) {
            const VkVertexInputAttributeDescription *at = &vi->pVertexAttributeDescriptions[i]; const fmt_t *f = fmt_of(at->format);
            if (!f || f->vf == MTLVertexFormatInvalid || at->location >= 31 || at->binding >= 14) { LOG("[vk] [RED] vertex attribute %u format %d not mapped", at->location, at->format); continue; }
            vd.attributes[at->location].format = f->vf; vd.attributes[at->location].offset = at->offset; vd.attributes[at->location].bufferIndex = 30 - at->binding;
            used |= 1u << at->binding;
        }
        for (uint32_t i = 0; vi && i < vi->vertexBindingDescriptionCount; i++) {
            const VkVertexInputBindingDescription *b = &vi->pVertexBindingDescriptions[i];
            if (b->binding >= 14) { LOG("[vk] [RED] vertex binding %u over 13", b->binding); continue; }
            if (!(used >> b->binding & 1)) continue;
            MTLVertexBufferLayoutDescriptor *l = vd.layouts[30 - b->binding];
            if (b->stride) { l.stride = b->stride; l.stepFunction = b->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex; l.stepRate = 1; }
            else { l.stride = 16; l.stepFunction = MTLVertexStepFunctionConstant; l.stepRate = 0; }
            p->vb_mask |= 1u << b->binding;
        }
        pd.vertexDescriptor = vd;
        switch (ci->pInputAssemblyState ? ci->pInputAssemblyState->topology : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
            case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: p->prim = MTLPrimitiveTypePoint; pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint; break;
            case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: p->prim = MTLPrimitiveTypeLine; pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine; break;
            case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: p->prim = MTLPrimitiveTypeLineStrip; pd.inputPrimitiveTopology = MTLPrimitiveTopologyClassLine; break;
            case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: p->prim = MTLPrimitiveTypeTriangleStrip; break;
            case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: p->prim = MTLPrimitiveTypeTriangle; break;
            default: RED1("primitive topology %d not implemented", ci->pInputAssemblyState->topology); p->prim = MTLPrimitiveTypeTriangle; break;
        }
        const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;
        p->cull = !rs || rs->cullMode == VK_CULL_MODE_NONE ? MTLCullModeNone : rs->cullMode == VK_CULL_MODE_FRONT_BIT ? MTLCullModeFront : rs->cullMode == VK_CULL_MODE_BACK_BIT ? MTLCullModeBack : MTLCullModeNone;
        if (rs && rs->cullMode == VK_CULL_MODE_FRONT_AND_BACK) RED1("culling both front and back faces not implemented");
        p->wind = rs && rs->frontFace == VK_FRONT_FACE_CLOCKWISE ? MTLWindingClockwise : MTLWindingCounterClockwise;   // framebuffer winding is the same once the shaders flip y
        p->fill = rs && rs->polygonMode == VK_POLYGON_MODE_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
        p->clip = rs && rs->depthClampEnable ? MTLDepthClipModeClamp : MTLDepthClipModeClip;
        if (rs && rs->depthBiasEnable) { p->bias = 1; p->bias_c = rs->depthBiasConstantFactor; p->bias_s = rs->depthBiasSlopeFactor; p->bias_cl = rs->depthBiasClamp; }
        if (rs && rs->rasterizerDiscardEnable) pd.rasterizationEnabled = NO;
        const VkPipelineMultisampleStateCreateInfo *ms = ci->pMultisampleState;
        pd.rasterSampleCount = ms ? umax(1, ms->rasterizationSamples) : 1; pd.alphaToCoverageEnabled = ms && ms->alphaToCoverageEnable;
        VkRenderPass rp = ci->renderPass;
        if (ci->subpass) RED1("pipeline uses subpass %u", ci->subpass);
        const VkPipelineColorBlendStateCreateInfo *cb = ci->pColorBlendState;
        if (cb && cb->logicOpEnable) RED1("logic op blending not implemented");
        for (uint32_t i = 0; i < rp->ncol; i++) {
            int at = rp->col[i]; if (at < 0) continue;
            const fmt_t *f = fmt_of(rp->att[at].fmt); MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[i];
            ca.pixelFormat = f ? f->pf : MTLPixelFormatInvalid;
            if (cb && i < cb->attachmentCount) {
                const VkPipelineColorBlendAttachmentState *b = &cb->pAttachments[cb->attachmentCount > 1 ? i : 0];
                ca.writeMask = write_mask(b->colorWriteMask);
                if (b->blendEnable && f && f->kind == K_COLOR) {
                    ca.blendingEnabled = YES;
                    ca.sourceRGBBlendFactor = blend_f(b->srcColorBlendFactor); ca.destinationRGBBlendFactor = blend_f(b->dstColorBlendFactor); ca.rgbBlendOperation = (MTLBlendOperation)b->colorBlendOp;
                    ca.sourceAlphaBlendFactor = blend_f(b->srcAlphaBlendFactor); ca.destinationAlphaBlendFactor = blend_f(b->dstAlphaBlendFactor); ca.alphaBlendOperation = (MTLBlendOperation)b->alphaBlendOp;
                }
            }
        }
        if (cb) memcpy(p->blend, cb->blendConstants, sizeof p->blend);
        const fmt_t *df = rp->depth >= 0 ? fmt_of(rp->att[rp->depth].fmt) : NULL;
        if (fmt_has_depth(df)) pd.depthAttachmentPixelFormat = df->pf;
        if (fmt_has_stencil(df)) pd.stencilAttachmentPixelFormat = df->pf;
        const VkPipelineDepthStencilStateCreateInfo *ds = ci->pDepthStencilState;
        if (df && ds) {
            MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
            if (fmt_has_depth(df) && ds->depthTestEnable) { dd.depthCompareFunction = (MTLCompareFunction)ds->depthCompareOp; dd.depthWriteEnabled = ds->depthWriteEnable != 0; }
            if (fmt_has_stencil(df) && ds->stencilTestEnable) { dd.frontFaceStencil = stencil_d(&ds->front); dd.backFaceStencil = stencil_d(&ds->back); p->sref_f = ds->front.reference; p->sref_b = ds->back.reference; }
            if (ds->depthBoundsTestEnable) RED1("depth bounds test not implemented");
            p->dss = [g_dev newDepthStencilStateWithDescriptor:dd];
        } else p->dss = g_dss_off;
        const VkPipelineDynamicStateCreateInfo *dy = ci->pDynamicState;
        for (uint32_t i = 0; dy && i < dy->dynamicStateCount; i++) {
            VkDynamicState s = dy->pDynamicStates[i];
            if (s == VK_DYNAMIC_STATE_VIEWPORT) p->dyn_vp = 1; else if (s == VK_DYNAMIC_STATE_SCISSOR) p->dyn_sc = 1; else RED1("dynamic state %d not implemented (the imports have no matching set function)", s);
        }
        const VkPipelineViewportStateCreateInfo *vs = ci->pViewportState;
        if (!p->dyn_vp && vs && vs->pViewports && vs->viewportCount) { p->vp = vp_of(&vs->pViewports[0]); p->has_vp = 1; }
        if (!p->dyn_sc && vs && vs->pScissors && vs->scissorCount) { p->sc = vs->pScissors[0]; p->has_sc = 1; }
        NSError *err = nil;
        p->pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!p->pso) { static int n; if (n++ < 20) LOG("[vk] [RED] cannot build the pipeline (%s): %s", keys, err.localizedDescription.UTF8String); }
        else g_n_pso++;
    }
    return p;
}
VKB(vkCreateGraphicsPipelines, VkResult, VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    (void)d; (void)c; (void)a; dev_init();
    for (uint32_t i = 0; i < n; i++) out[i] = pipe_make(&ci[i]);   // a pipeline Metal refuses is still handed out: its draws are skipped
    return VK_SUCCESS;
}
VKB(vkDestroyPipeline, void, VkDevice d, VkPipeline p, const VkAllocationCallbacks *a) { (void)d; (void)a; if (p) { p->pso = nil; p->dss = nil; free(p); } }

// ---------- Command buffers: recording ----------
enum { C_BRP = 1, C_ERP, C_PIPE, C_SETS, C_VB, C_IB, C_VP, C_SC, C_DRAWI, C_COPYB, C_COPYB2I, C_DRAW, C_PUSH, C_COPYI, C_COPYI2B, C_QRESET, C_QTS, C_QBEGIN, C_QEND };
typedef struct { uint32_t op, size; } chdr;
typedef struct { VkRenderPass rp; VkFramebuffer fb; VkRect2D area; uint32_t nclear; VkClearValue clear[]; } c_brp;
typedef struct { VkPipeline p; } c_pipe;
typedef struct { uint32_t first, n, ndyn; VkDescriptorSet s[8]; uint32_t dyn[]; } c_sets;
typedef struct { uint32_t first, n; VkBuffer b[14]; VkDeviceSize o[14]; } c_vb;
typedef struct { VkBuffer b; VkDeviceSize o; VkIndexType t; } c_ib;
typedef struct { VkViewport v; } c_vp;
typedef struct { VkRect2D r; } c_sc;
typedef struct { uint32_t count, inst, first; int32_t voff; uint32_t first_inst; } c_drawi;
typedef struct { VkBuffer src, dst; uint32_t n; VkBufferCopy r[]; } c_copyb;
typedef struct { VkBuffer src; VkImage dst; uint32_t n; VkBufferImageCopy r[]; } c_copyb2i;
typedef struct { uint32_t count, inst, first, first_inst; } c_draw;
typedef struct { uint32_t off, size; uint8_t data[]; } c_push;
typedef struct { VkImage src, dst; uint32_t n; VkImageCopy r[]; } c_copyi;
typedef struct { VkImage src; VkBuffer dst; uint32_t n; VkBufferImageCopy r[]; } c_copyi2b;
typedef struct { VkQueryPool q; uint32_t first, n; } c_query;
static void *rec(VkCommandBuffer cb, uint32_t op, size_t sz) {
    size_t need = (sizeof(chdr) + sz + 7) & ~(size_t)7;
    if (cb->n + need > cb->cap) {
        size_t c = cb->cap ? cb->cap * 2 : 4096; while (c < cb->n + need) c *= 2;
        cb->buf = realloc(cb->buf, c); if (!cb->buf) die("[vk] out of command buffer memory %zu", c); cb->cap = c;
    }
    chdr *h = (chdr *)(cb->buf + cb->n); h->op = op; h->size = (uint32_t)need; cb->n += need;
    return h + 1;
}
VK(vkCreateCommandPool, VkResult, VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *out) {
    (void)d; (void)ci; (void)a; *out = zalloc(sizeof **out); return VK_SUCCESS;
}
static void cb_free(VkCommandBuffer cb) {
    VkCommandPool p = cb->pool;
    if (cb->prev) cb->prev->next = cb->next; else p->cbs = cb->next;
    if (cb->next) cb->next->prev = cb->prev;
    free(cb->buf); free(cb);
}
VK(vkDestroyCommandPool, void, VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a) { (void)d; (void)a; if (p) { while (p->cbs) cb_free(p->cbs); free(p); } }
VK(vkResetCommandPool, VkResult, VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) { (void)d; (void)f; for (VkCommandBuffer c = p->cbs; c; c = c->next) c->n = 0; return VK_SUCCESS; }
VK(vkAllocateCommandBuffers, VkResult, VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out) {
    (void)d;
    if (ai->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) RED1("secondary command buffers not implemented");
    for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
        VkCommandBuffer cb = zalloc(sizeof *cb); cb->pool = ai->commandPool;
        cb->next = cb->pool->cbs; if (cb->next) cb->next->prev = cb; cb->pool->cbs = cb;
        out[i] = cb;
    }
    return VK_SUCCESS;
}
VK(vkFreeCommandBuffers, void, VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer *cbs) { (void)d; (void)p; for (uint32_t i = 0; i < n; i++) if (cbs[i]) cb_free(cbs[i]); }
VK(vkBeginCommandBuffer, VkResult, VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) { (void)bi; cb->n = 0; return VK_SUCCESS; }
VK(vkEndCommandBuffer, VkResult, VkCommandBuffer cb) { (void)cb; return VK_SUCCESS; }
VK(vkResetCommandBuffer, VkResult, VkCommandBuffer cb, VkCommandBufferResetFlags f) { (void)f; cb->n = 0; return VK_SUCCESS; }
VK(vkCmdBeginRenderPass, void, VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents sc) {
    (void)sc; uint32_t nc = umin(bi->clearValueCount, 16);
    c_brp *c = rec(cb, C_BRP, sizeof *c + nc * sizeof(VkClearValue));
    c->rp = bi->renderPass; c->fb = bi->framebuffer; c->area = bi->renderArea; c->nclear = nc;
    if (nc) memcpy(c->clear, bi->pClearValues, nc * sizeof(VkClearValue));
}
VK(vkCmdEndRenderPass, void, VkCommandBuffer cb) { rec(cb, C_ERP, 0); }
VK(vkCmdBindPipeline, void, VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS) { RED1("compute pipelines not implemented"); return; }
    ((c_pipe *)rec(cb, C_PIPE, sizeof(c_pipe)))->p = p;
}
VK(vkCmdBindDescriptorSets, void, VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t ndyn, const uint32_t *dyn) {
    (void)l; if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    c_sets *c = rec(cb, C_SETS, sizeof *c + ndyn * sizeof(uint32_t));
    c->first = first; c->n = umin(n, 8); c->ndyn = ndyn; memcpy(c->s, s, c->n * sizeof *s); if (ndyn) memcpy(c->dyn, dyn, ndyn * sizeof *dyn);
}
VK(vkCmdBindVertexBuffers, void, VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    c_vb *c = rec(cb, C_VB, sizeof *c); c->first = first; c->n = first < 14 ? umin(n, 14 - first) : 0;
    memcpy(c->b, b, c->n * sizeof *b); memcpy(c->o, o, c->n * sizeof *o);
}
VK(vkCmdBindIndexBuffer, void, VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) { c_ib *c = rec(cb, C_IB, sizeof *c); c->b = b; c->o = o; c->t = t; }
VK(vkCmdSetViewport, void, VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) { if (!first && n) ((c_vp *)rec(cb, C_VP, sizeof(c_vp)))->v = v[0]; }
VK(vkCmdSetScissor, void, VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r) { if (!first && n) ((c_sc *)rec(cb, C_SC, sizeof(c_sc)))->r = r[0]; }
VK(vkCmdDrawIndexed, void, VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, int32_t voff, uint32_t first_inst) {
    *(c_drawi *)rec(cb, C_DRAWI, sizeof(c_drawi)) = (c_drawi){count, inst, first, voff, first_inst};
}
VK(vkCmdCopyBuffer, void, VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t n, const VkBufferCopy *r) {
    c_copyb *c = rec(cb, C_COPYB, sizeof *c + n * sizeof *r); c->src = src; c->dst = dst; c->n = n; memcpy(c->r, r, n * sizeof *r);
}
VK(vkCmdCopyBufferToImage, void, VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout lay, uint32_t n, const VkBufferImageCopy *r) {
    (void)lay; c_copyb2i *c = rec(cb, C_COPYB2I, sizeof *c + n * sizeof *r); c->src = src; c->dst = dst; c->n = n; memcpy(c->r, r, n * sizeof *r);
}
VK(vkCmdDraw, void, VkCommandBuffer cb, uint32_t count, uint32_t inst, uint32_t first, uint32_t first_inst) {
    *(c_draw *)rec(cb, C_DRAW, sizeof(c_draw)) = (c_draw){count, inst, first, first_inst};
}
VK(vkCmdPushConstants, void, VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *v) {
    (void)l; (void)st;
    if (off >= 256 || size > 256 - off) { RED1("push constant out of range: offset %u length %u", off, size); return; }
    c_push *c = rec(cb, C_PUSH, sizeof *c + size); c->off = off; c->size = size; memcpy(c->data, v, size);
}
VK(vkCmdCopyImage, void, VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl, uint32_t n, const VkImageCopy *r) {
    (void)sl; (void)dl; c_copyi *c = rec(cb, C_COPYI, sizeof *c + n * sizeof *r); c->src = src; c->dst = dst; c->n = n; memcpy(c->r, r, n * sizeof *r);
}
VK(vkCmdCopyImageToBuffer, void, VkCommandBuffer cb, VkImage src, VkImageLayout lay, VkBuffer dst, uint32_t n, const VkBufferImageCopy *r) {
    (void)lay; c_copyi2b *c = rec(cb, C_COPYI2B, sizeof *c + n * sizeof *r); c->src = src; c->dst = dst; c->n = n; memcpy(c->r, r, n * sizeof *r);
}
VK(vkCmdResetQueryPool, void, VkCommandBuffer cb, VkQueryPool q, uint32_t first, uint32_t n) { *(c_query *)rec(cb, C_QRESET, sizeof(c_query)) = (c_query){q, first, n}; }
VK(vkCmdWriteTimestamp, void, VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool q, uint32_t i) { (void)st; *(c_query *)rec(cb, C_QTS, sizeof(c_query)) = (c_query){q, i, 1}; }
VK(vkCmdBeginQuery, void, VkCommandBuffer cb, VkQueryPool q, uint32_t i, VkQueryControlFlags f) { (void)f; *(c_query *)rec(cb, C_QBEGIN, sizeof(c_query)) = (c_query){q, i, 1}; }
VK(vkCmdEndQuery, void, VkCommandBuffer cb, VkQueryPool q, uint32_t i) { *(c_query *)rec(cb, C_QEND, sizeof(c_query)) = (c_query){q, i, 1}; }
// Barriers order nothing here: one serial queue, and Metal tracks hazards between the passes and blits it encodes
VK(vkCmdPipelineBarrier, void, VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m,
   uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    (void)cb; (void)s; (void)d; (void)f; (void)nm; (void)m; (void)nb; (void)b; (void)ni; (void)i;
}

// ---------- Command buffers: replay at submit ----------
// Queries of one submit: its event value (the ticket every query it writes gets) and, when it has occlusion queries, a visibility buffer that every render pass
// of the submit counts into (one 8-byte slot per BeginQuery), copied to the pools when the submit completes
typedef struct { VkQueryPool q; uint32_t idx, slot; } occ_t;
typedef struct { uint64_t v; id<MTLBuffer> vis; uint32_t nvis, cap; occ_t *occ; } subq_t;
typedef struct {
    id<MTLCommandBuffer> cb; id<MTLRenderCommandEncoder> re; id<MTLBlitCommandEncoder> be;
    VkPipeline pipe, applied; uint32_t rt_w, rt_h;
    struct { VkDescriptorSet s; uint32_t dyn[16]; } set[8];
    VkBuffer vb[14]; VkDeviceSize vbo[14];
    VkBuffer ib; VkDeviceSize ibo; MTLIndexType it; uint32_t isz;
    VkViewport vp; VkRect2D sc; int have_vp, have_sc, vp_dirty, sc_dirty;
    uint8_t pc[256];
    subq_t *sq;
    // The current render pass: its descriptor (attachments created with deferred store actions) and the store actions it ends with; cut = its encoder was ended
    // for a copy in the middle of the pass and is resumed (loading the attachments) at the next draw
    MTLRenderPassDescriptor *rpd; MTLStoreAction st_col[8], st_d, st_s; int cut;
} rs_t;
#define DONE_RING 1024
static uint64_t g_done_ns[DONE_RING];   // completion time of submit v at [v % DONE_RING] (timestamp query results)
static void end_blit(rs_t *s) { if (s->be) { [s->be endEncoding]; s->be = nil; } }
static MTLStoreAction keep(MTLStoreAction a) { return a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve ? MTLStoreActionStoreAndMultisampleResolve : MTLStoreActionStore; }
static void end_rp(rs_t *s, int final) {   // final: the pass's own store actions; otherwise everything is stored, to be loaded again when the pass resumes
    if (!s->re) return;
    MTLRenderPassDescriptor *d = s->rpd;
    for (int i = 0; i < 8; i++) if (d.colorAttachments[i].texture) [s->re setColorStoreAction:final ? s->st_col[i] : keep(s->st_col[i]) atIndex:i];
    if (d.depthAttachment.texture) [s->re setDepthStoreAction:final ? s->st_d : MTLStoreActionStore];
    if (d.stencilAttachment.texture) [s->re setStencilStoreAction:final ? s->st_s : MTLStoreActionStore];
    [s->re endEncoding]; s->re = nil;
}
// A copy inside a render pass, which NVIDIA's driver runs: Super Mario Galaxy copies from its EFB in the middle of a pass (vkCmdCopyImage). The encoder ends here and
// the pass resumes at its next draw (resume_rp); without that, the rest of the pass's draws were dropped
static id<MTLBlitCommandEncoder> blit(rs_t *s, uint32_t op) {
    if (s->re) { static int n; if (n++ < 3) OLOG("copy inside a render pass (command %u): the pass is split and the next draw continues it", op); end_rp(s, 0); s->cut = 1; }
    if (!s->be) s->be = [s->cb blitCommandEncoder];
    return s->be;
}
static void resume_rp(rs_t *s) {
    end_blit(s);
    MTLRenderPassDescriptor *d = s->rpd;
    for (int i = 0; i < 8; i++) if (d.colorAttachments[i].texture) d.colorAttachments[i].loadAction = MTLLoadActionLoad;
    if (d.depthAttachment.texture) d.depthAttachment.loadAction = MTLLoadActionLoad;
    if (d.stencilAttachment.texture) d.stencilAttachment.loadAction = MTLLoadActionLoad;
    s->re = [s->cb renderCommandEncoderWithDescriptor:d]; s->cut = 0;
    s->applied = NULL; s->vp_dirty = s->sc_dirty = 1;
}
static MTLLoadAction load_a(VkAttachmentLoadOp o) { return o == VK_ATTACHMENT_LOAD_OP_CLEAR ? MTLLoadActionClear : o == VK_ATTACHMENT_LOAD_OP_LOAD ? MTLLoadActionLoad : MTLLoadActionDontCare; }
static MTLStoreAction store_a(VkAttachmentStoreOp o) { return o == VK_ATTACHMENT_STORE_OP_DONT_CARE ? MTLStoreActionDontCare : MTLStoreActionStore; }
static void begin_rp(rs_t *s, const c_brp *c) {
    end_blit(s);
    end_rp(s, 1); s->cut = 0;
    VkRenderPass rp = c->rp; VkFramebuffer fb = c->fb;
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    uint32_t w = fb->w, h = fb->h;
    for (uint32_t i = 0; i < rp->ncol; i++) {
        int a = rp->col[i]; if (a < 0 || (uint32_t)a >= fb->n) continue;
        const ratt *at = &rp->att[a]; MTLRenderPassColorAttachmentDescriptor *ca = d.colorAttachments[i];
        ca.texture = fb->v[a]->tex; w = umin(w, (uint32_t)ca.texture.width); h = umin(h, (uint32_t)ca.texture.height);
        ca.loadAction = load_a(at->load); s->st_col[i] = store_a(at->store); ca.storeAction = MTLStoreActionUnknown;
        if (at->load == VK_ATTACHMENT_LOAD_OP_CLEAR && (uint32_t)a < c->nclear) { const float *f = c->clear[a].color.float32; ca.clearColor = MTLClearColorMake(f[0], f[1], f[2], f[3]); }
        int r = rp->res[i];
        if (r >= 0 && (uint32_t)r < fb->n) { ca.resolveTexture = fb->v[r]->tex; s->st_col[i] = at->store == VK_ATTACHMENT_STORE_OP_DONT_CARE ? MTLStoreActionMultisampleResolve : MTLStoreActionStoreAndMultisampleResolve; }
    }
    if (rp->depth >= 0 && (uint32_t)rp->depth < fb->n) {
        int a = rp->depth; const ratt *at = &rp->att[a]; const fmt_t *f = fmt_of(at->fmt); id<MTLTexture> t = fb->v[a]->tex;
        w = umin(w, (uint32_t)t.width); h = umin(h, (uint32_t)t.height);
        if (fmt_has_depth(f)) {
            d.depthAttachment.texture = t; d.depthAttachment.loadAction = load_a(at->load); s->st_d = store_a(at->store); d.depthAttachment.storeAction = MTLStoreActionUnknown;
            if ((uint32_t)a < c->nclear) d.depthAttachment.clearDepth = c->clear[a].depthStencil.depth;
        }
        if (fmt_has_stencil(f)) {
            d.stencilAttachment.texture = t; d.stencilAttachment.loadAction = load_a(at->sload); s->st_s = store_a(at->sstore); d.stencilAttachment.storeAction = MTLStoreActionUnknown;
            if ((uint32_t)a < c->nclear) d.stencilAttachment.clearStencil = c->clear[a].depthStencil.stencil;
        }
    }
    if (!rp->ncol && rp->depth < 0) { d.renderTargetWidth = fb->w; d.renderTargetHeight = fb->h; d.defaultRasterSampleCount = 1; }
    if (s->sq && s->sq->vis) d.visibilityResultBuffer = s->sq->vis;
    s->rpd = d; s->re = [s->cb renderCommandEncoderWithDescriptor:d];
    s->applied = NULL; s->rt_w = w; s->rt_h = h;
    // Dynamic viewport and scissor persist across render passes within a command buffer, as in Vulkan; the render target's size stands in only until the game sets them.
    // Resetting them at every pass drew Super Mario Galaxy's handheld UI 1.5x too big: it sets a 1280x720 viewport once and then draws through several passes into its 1920x1080 swapchain
    if (!s->have_vp) s->vp = (VkViewport){0, 0, (float)w, (float)h, 0, 1};
    if (!s->have_sc) s->sc = (VkRect2D){{0, 0}, {w, h}};
    s->vp_dirty = s->sc_dirty = 1;
    g_n_pass++;
}
static void bind_res(rs_t *s, const shinfo *si, int frag) {
    if (!si) return;
    id<MTLRenderCommandEncoder> re = s->re;
    for (int i = 0; i < si->nres; i++) {
        const shres *r = &si->res[i];
        if (r->kind == 'p') continue;   // bind_push
        VkDescriptorSet ds = r->set < 8 ? s->set[r->set].s : NULL;
        if (!ds) { RED1("shader %s: set %u is not bound", si->key, r->set); continue; }
        const VkDescriptorSetLayout L = ds->L; int bi = r->binding < 128 ? L->idx[r->binding] : -1;
        if (bi < 0 || !L->b[bi].count) {
            char have[256] = ""; size_t o = 0;
            for (uint32_t k = 0; k < L->n && o + 8 < sizeof have; k++) o += snprintf(have + o, sizeof have - o, " %u", L->b[k].binding);
            const VkPipelineLayout pl = s->pipe->layout; const VkDescriptorSetLayout PL = pl && r->set < pl->nset ? pl->set[r->set] : NULL;
            RED1("shader %s: %u.%u is not in the bound set's layout %p (it has:%s); the pipeline layout's set %u is %p%s", si->key, r->set, r->binding, (void *)L, have, r->set, (void *)PL,
                 PL && r->binding < 128 && PL->idx[r->binding] >= 0 ? ", which has this binding: the set was not rebound with the pipeline" : "");
            continue;
        }
        const lbind *lb = &L->b[bi]; uint32_t n = umin(r->count, lb->count);
        for (uint32_t k = 0; k < n; k++) {
            const desc_t *e = &ds->d[lb->base + k]; NSUInteger slot = r->slot + k;
            switch (r->kind) {
                case 'u': case 'b': {
                    if (!e->buf) { RED1("shader %s: buffer %u.%u[%u] is empty", si->key, r->set, r->binding, k); break; }
                    VkDeviceSize off = e->off; if (lb->dyn >= 0 && lb->dyn + k < 16) off += s->set[r->set].dyn[lb->dyn + k];
                    if (frag) [re setFragmentBuffer:e->buf offset:off atIndex:slot]; else [re setVertexBuffer:e->buf offset:off atIndex:slot];
                    break;
                }
                case 't': case 'c': {
                    if (!e->tex) { RED1("shader %s: texture %u.%u[%u] is empty", si->key, r->set, r->binding, k); break; }
                    if (frag) [re setFragmentTexture:e->tex atIndex:slot]; else [re setVertexTexture:e->tex atIndex:slot];
                    if (r->kind == 'c' && e->smp) { if (frag) [re setFragmentSamplerState:e->smp atIndex:slot]; else [re setVertexSamplerState:e->smp atIndex:slot]; }
                    break;
                }
                case 's':
                    if (!e->smp) { RED1("shader %s: sampler %u.%u[%u] is empty", si->key, r->set, r->binding, k); break; }
                    if (frag) [re setFragmentSamplerState:e->smp atIndex:slot]; else [re setVertexSamplerState:e->smp atIndex:slot];
                    break;
            }
        }
    }
}
static void bind_push(rs_t *s, const shinfo *si, int frag) {   // the push-constant block (kind p): the whole range of the pipeline layout, at least 16 bytes
    if (!si) return;
    for (int i = 0; i < si->nres; i++) {
        if (si->res[i].kind != 'p') continue;
        const VkPipelineLayout l = s->pipe->layout; uint32_t n = (umax(l ? l->pc_size : 0, 16) + 15) & ~15u;
        if (frag) [s->re setFragmentBytes:s->pc length:n atIndex:si->res[i].slot]; else [s->re setVertexBytes:s->pc length:n atIndex:si->res[i].slot];
    }
}
static int pre_draw(rs_t *s) {
    if (!s->re && s->cut) resume_rp(s);
    VkPipeline p = s->pipe; id<MTLRenderCommandEncoder> re = s->re;
    if (!re) { RED1("draw outside a render pass"); return 0; }
    if (!p || !p->pso) return 0;
    if (s->applied != p) {
        [re setRenderPipelineState:p->pso]; [re setDepthStencilState:p->dss ? p->dss : g_dss_off];
        [re setCullMode:p->cull]; [re setFrontFacingWinding:p->wind]; [re setTriangleFillMode:p->fill]; [re setDepthClipMode:p->clip];
        [re setDepthBias:p->bias ? p->bias_c : 0 slopeScale:p->bias ? p->bias_s : 0 clamp:p->bias ? p->bias_cl : 0];
        [re setBlendColorRed:p->blend[0] green:p->blend[1] blue:p->blend[2] alpha:p->blend[3]];
        [re setStencilFrontReferenceValue:p->sref_f backReferenceValue:p->sref_b];
        if (p->has_vp && (!s->applied || !s->applied->has_vp || memcmp(&s->applied->vp, &p->vp, sizeof p->vp))) s->vp_dirty = 1;
        if (p->has_sc) s->sc_dirty = 1;
        s->applied = p;
    }
    if (s->vp_dirty) {
        MTLViewport v = p->has_vp ? p->vp : vp_of(&s->vp);
        [re setViewport:v]; s->vp_dirty = 0;
    }
    if (s->sc_dirty) {   // Metal requires the scissor to lie inside the render target
        VkRect2D r = p->has_sc ? p->sc : s->sc;
        int32_t x0 = r.offset.x < 0 ? 0 : r.offset.x, y0 = r.offset.y < 0 ? 0 : r.offset.y;
        int64_t x1 = (int64_t)r.offset.x + r.extent.width, y1 = (int64_t)r.offset.y + r.extent.height;
        if (x1 > s->rt_w) x1 = s->rt_w;
        if (y1 > s->rt_h) y1 = s->rt_h;
        if (x0 > x1) x0 = (int32_t)x1;
        if (y0 > y1) y0 = (int32_t)y1;
        [re setScissorRect:(MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}]; s->sc_dirty = 0;
    }
    bind_res(s, p->vs, 0); bind_res(s, p->fs, 1); bind_push(s, p->vs, 0); bind_push(s, p->fs, 1);
    for (uint32_t i = 0; i < 14; i++)
        if (p->vb_mask >> i & 1) {
            VkBuffer b = s->vb[i]; if (!b || !b->mem) { RED1("vertex buffer %u not bound", i); return 0; }
            [re setVertexBuffer:mem_buf(b->mem) offset:b->off + s->vbo[i] atIndex:30 - i];
        }
    return 1;
}
static void replay(rs_t *s, VkCommandBuffer cb) {
    for (size_t o = 0; o < cb->n;) {
        const chdr *h = (const chdr *)(cb->buf + o); const void *c = h + 1; o += h->size;
        switch (h->op) {
            case C_BRP: begin_rp(s, c); break;
            case C_ERP: end_rp(s, 1); s->cut = 0; s->rpd = nil; break;
            case C_PIPE: s->pipe = ((const c_pipe *)c)->p; break;
            case C_SETS: {
                const c_sets *x = c; uint32_t k = 0;
                for (uint32_t i = 0; i < x->n && x->first + i < 8; i++) {
                    uint32_t si = x->first + i; VkDescriptorSet ds = x->s[i]; s->set[si].s = ds;
                    for (uint32_t j = 0; ds && j < ds->L->ndyn; j++, k++) if (j < 16) s->set[si].dyn[j] = k < x->ndyn ? x->dyn[k] : 0;
                }
                break;
            }
            case C_VB: { const c_vb *x = c; for (uint32_t i = 0; i < x->n; i++) { s->vb[x->first + i] = x->b[i]; s->vbo[x->first + i] = x->o[i]; } break; }
            case C_IB: { const c_ib *x = c; s->ib = x->b; s->ibo = x->o; s->it = x->t == VK_INDEX_TYPE_UINT32 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16; s->isz = x->t == VK_INDEX_TYPE_UINT32 ? 4 : 2; break; }
            case C_VP: s->vp = ((const c_vp *)c)->v; s->vp_dirty = 1; s->have_vp = 1; break;
            case C_SC: s->sc = ((const c_sc *)c)->r; s->sc_dirty = 1; s->have_sc = 1; break;
            case C_DRAWI: {
                const c_drawi *x = c;
                if (!x->count || !x->inst || !pre_draw(s)) break;
                if (!s->ib || !s->ib->mem) { RED1("index buffer not bound"); break; }
                [s->re drawIndexedPrimitives:s->pipe->prim indexCount:x->count indexType:s->it indexBuffer:mem_buf(s->ib->mem)
                           indexBufferOffset:s->ib->off + s->ibo + (VkDeviceSize)x->first * s->isz instanceCount:x->inst baseVertex:x->voff baseInstance:x->first_inst];
                g_n_draw++;
                break;
            }
            case C_COPYB: {
                const c_copyb *x = c; if (!x->src->mem || !x->dst->mem) { RED1("a copied buffer has no bound memory"); break; }
                id<MTLBlitCommandEncoder> be = blit(s, h->op); id<MTLBuffer> sb = mem_buf(x->src->mem), db = mem_buf(x->dst->mem);
                for (uint32_t i = 0; i < x->n; i++)
                    [be copyFromBuffer:sb sourceOffset:x->src->off + x->r[i].srcOffset toBuffer:db destinationOffset:x->dst->off + x->r[i].dstOffset size:x->r[i].size];
                g_n_copy += x->n;
                break;
            }
            case C_COPYB2I: {
                const c_copyb2i *x = c; VkImage im = x->dst; const fmt_t *f = im->f;
                if (!x->src->mem || !im->tex) { RED1("the upload buffer has no bound memory or the image was not created"); break; }
                id<MTLBlitCommandEncoder> be = blit(s, h->op); id<MTLBuffer> sb = mem_buf(x->src->mem);
                for (uint32_t i = 0; i < x->n; i++) {
                    const VkBufferImageCopy *r = &x->r[i];
                    uint32_t rl = r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width, ih = r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height;
                    NSUInteger bpr = (NSUInteger)((rl + f->bw - 1) / f->bw) * f->bytes, bpi = bpr * ((ih + f->bh - 1) / f->bh);
                    int is3d = im->type == VK_IMAGE_TYPE_3D;
                    MTLBlitOption opt = f->kind == K_DS ? (r->imageSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil) : MTLBlitOptionNone;
                    for (uint32_t l = 0; l < (is3d ? 1 : r->imageSubresource.layerCount); l++)
                        [be copyFromBuffer:sb sourceOffset:x->src->off + r->bufferOffset + l * bpi sourceBytesPerRow:bpr sourceBytesPerImage:is3d || im->layers > 1 ? bpi : 0
                                sourceSize:MTLSizeMake(r->imageExtent.width, r->imageExtent.height, is3d ? r->imageExtent.depth : 1) toTexture:im->tex
                          destinationSlice:is3d ? 0 : r->imageSubresource.baseArrayLayer + l destinationLevel:r->imageSubresource.mipLevel
                         destinationOrigin:MTLOriginMake(r->imageOffset.x, r->imageOffset.y, is3d ? r->imageOffset.z : 0) options:opt];
                }
                g_n_copy += x->n;
                break;
            }
            case C_DRAW: {
                const c_draw *x = c;
                if (!x->count || !x->inst || !pre_draw(s)) break;
                [s->re drawPrimitives:s->pipe->prim vertexStart:x->first vertexCount:x->count instanceCount:x->inst baseInstance:x->first_inst];
                g_n_draw++;
                break;
            }
            case C_PUSH: { const c_push *x = c; memcpy(s->pc + x->off, x->data, x->size); break; }
            case C_COPYI: {
                const c_copyi *x = c; VkImage a = x->src, b = x->dst;
                if (!a->tex || !b->tex) { RED1("a copied image was not created"); break; }
                if (a->tex.pixelFormat != b->tex.pixelFormat) { RED1("image copy between different formats (%d → %d) not implemented", a->fmt, b->fmt); break; }
                id<MTLBlitCommandEncoder> be = blit(s, h->op);
                for (uint32_t i = 0; i < x->n; i++) {
                    const VkImageCopy *r = &x->r[i]; int a3 = a->type == VK_IMAGE_TYPE_3D, b3 = b->type == VK_IMAGE_TYPE_3D;
                    uint32_t nl = a3 ? 1 : r->srcSubresource.layerCount;
                    for (uint32_t l = 0; l < nl; l++)
                        [be copyFromTexture:a->tex sourceSlice:a3 ? 0 : r->srcSubresource.baseArrayLayer + l sourceLevel:r->srcSubresource.mipLevel
                               sourceOrigin:MTLOriginMake(r->srcOffset.x, r->srcOffset.y, a3 ? r->srcOffset.z : 0)
                                 sourceSize:MTLSizeMake(r->extent.width, r->extent.height, a3 ? r->extent.depth : 1) toTexture:b->tex
                           destinationSlice:b3 ? 0 : r->dstSubresource.baseArrayLayer + l destinationLevel:r->dstSubresource.mipLevel
                          destinationOrigin:MTLOriginMake(r->dstOffset.x, r->dstOffset.y, b3 ? r->dstOffset.z : 0)];
                }
                g_n_copy += x->n;
                break;
            }
            case C_COPYI2B: {   // read-back into a buffer (shared storage: the CPU sees it once the submit's fence has signalled)
                const c_copyi2b *x = c; VkImage im = x->src; const fmt_t *f = im->f;
                if (!x->dst->mem || !im->tex) { RED1("the readback buffer has no bound memory or the image was not created"); break; }
                id<MTLBlitCommandEncoder> be = blit(s, h->op); id<MTLBuffer> db = mem_buf(x->dst->mem);
                for (uint32_t i = 0; i < x->n; i++) {
                    const VkBufferImageCopy *r = &x->r[i]; int is3d = im->type == VK_IMAGE_TYPE_3D;
                    uint32_t rl = r->bufferRowLength ? r->bufferRowLength : r->imageExtent.width, ih = r->bufferImageHeight ? r->bufferImageHeight : r->imageExtent.height;
                    MTLBlitOption opt = MTLBlitOptionNone; NSUInteger tb = f->bytes;
                    if (f->kind == K_DS) {   // one aspect: Metal writes 4-byte depth or 1-byte stencil, as Vulkan's buffer layout for that aspect
                        int st = r->imageSubresource.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT;
                        opt = st ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionDepthFromDepthStencil; tb = st ? 1 : 4;
                    }
                    NSUInteger bpr = f->kind == K_BLOCK ? (NSUInteger)((rl + f->bw - 1) / f->bw) * f->bytes : rl * tb, bpi = bpr * ((ih + f->bh - 1) / f->bh);
                    for (uint32_t l = 0; l < (is3d ? 1 : r->imageSubresource.layerCount); l++)
                        [be copyFromTexture:im->tex sourceSlice:is3d ? 0 : r->imageSubresource.baseArrayLayer + l sourceLevel:r->imageSubresource.mipLevel
                               sourceOrigin:MTLOriginMake(r->imageOffset.x, r->imageOffset.y, is3d ? r->imageOffset.z : 0)
                                 sourceSize:MTLSizeMake(r->imageExtent.width, r->imageExtent.height, is3d ? r->imageExtent.depth : 1)
                                   toBuffer:db destinationOffset:x->dst->off + r->bufferOffset + l * bpi destinationBytesPerRow:bpr destinationBytesPerImage:bpi options:opt];
                }
                g_n_copy += x->n;
                break;
            }
            case C_QRESET: { const c_query *x = c; for (uint32_t i = 0; i < x->n && x->first + i < x->q->n; i++) x->q->ticket[x->first + i] = FENCE_NONE; break; }
            case C_QTS: { const c_query *x = c; if (x->first < x->q->n) x->q->ticket[x->first] = s->sq->v; break; }
            case C_QBEGIN: {
                const c_query *x = c; subq_t *q = s->sq;
                if (!s->re && s->cut) resume_rp(s);
                if (x->q->type != VK_QUERY_TYPE_OCCLUSION) { RED1("query type %d not implemented", x->q->type); break; }
                if (!s->re || !q->vis || q->nvis == q->cap || x->first >= x->q->n) { RED1("occlusion query outside a render pass or past the buffer"); break; }
                q->occ[q->nvis] = (occ_t){x->q, x->first, q->nvis};
                [s->re setVisibilityResultMode:MTLVisibilityResultModeCounting offset:q->nvis * 8]; q->nvis++;
                x->q->ticket[x->first] = q->v;
                break;
            }
            case C_QEND: if (s->re) [s->re setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0]; break;
            default: LOG("[vk] [RED] unknown command %u in the command buffer", h->op); return;
        }
    }
    if (s->re || s->cut) { RED1("render pass not ended when the command buffer ended"); end_rp(s, 1); s->cut = 0; }
    end_blit(s);
}

// ---------- Queue, fences, semaphores ----------
static int fence_done(VkFence f) { uint64_t v = atomic_load(&f->val); return v != FENCE_NONE && atomic_load(&g_done) >= v; }
static void done_advance(uint64_t v) {
    pthread_mutex_lock(&g_done_mu);
    if (v > atomic_load(&g_done)) atomic_store(&g_done, v);
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_done_mu);
}
VKB(vkQueueSubmit, VkResult, VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) {
    (void)q; dm_gfx_enter();   // blocks while the app is in the background (iOS allows no GPU work there)
    @autoreleasepool {
        pthread_mutex_lock(&g_qmu);
        id<MTLCommandBuffer> cb = [g_q commandBuffer];
        subq_t *sq = zalloc(sizeof *sq); sq->v = g_ev_val + 1;
        for (uint32_t i = 0; i < n; i++)   // occlusion queries need a visibility buffer before the first render pass is encoded
            for (uint32_t j = 0; j < si[i].commandBufferCount; j++) {
                VkCommandBuffer c = si[i].pCommandBuffers[j];
                for (size_t o = 0; o < c->n; o += ((const chdr *)(c->buf + o))->size) sq->cap += ((const chdr *)(c->buf + o))->op == C_QBEGIN;
            }
        if (sq->cap) {
            sq->vis = [g_dev newBufferWithLength:sq->cap * 8 options:MTLResourceStorageModeShared]; memset(sq->vis.contents, 0, sq->cap * 8);
            sq->occ = zalloc(sq->cap * sizeof *sq->occ);
        }
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t j = 0; j < si[i].commandBufferCount; j++) { rs_t s = {0}; s.cb = cb; s.sq = sq; replay(&s, si[i].pCommandBuffers[j]); }
        uint64_t v = ++g_ev_val;
        [cb encodeSignalEvent:g_ev value:v];
        if (f) atomic_store(&f->val, v);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
            if (c.error) { static int k; if (k++ < 20) LOG("[vk] [RED] command buffer error (submission %llu): %s", (unsigned long long)v, c.error.localizedDescription.UTF8String); }
            g_done_ns[v % DONE_RING] = now_ns();
            if (sq->nvis) { const uint64_t *r = sq->vis.contents; for (uint32_t k = 0; k < sq->nvis; k++) sq->occ[k].q->val[sq->occ[k].idx] = r[sq->occ[k].slot]; }
            sq->vis = nil; free(sq->occ); free(sq);
            done_advance(v);   // after the results: a query is read as available once g_done reaches its ticket
        }];
        [cb commit];
        g_n_submit++;
        pthread_mutex_unlock(&g_qmu);
    }
    dm_gfx_leave();
    return VK_SUCCESS;
}
VK(vkQueueBindSparse, VkResult, VkQueue q, uint32_t n, const VkBindSparseInfo *bi, VkFence f) {
    (void)q; (void)bi; if (n) RED1("sparse binding not implemented");
    if (f) atomic_store(&f->val, 0);
    return n ? VK_ERROR_FEATURE_NOT_PRESENT : VK_SUCCESS;
}
static void wait_done(uint64_t v) {
    pthread_mutex_lock(&g_done_mu);
    while (atomic_load(&g_done) < v) pthread_cond_wait(&g_done_cv, &g_done_mu);
    pthread_mutex_unlock(&g_done_mu);
}
void dm_vk_drain(void) { pthread_mutex_lock(&g_qmu); uint64_t v = g_ev_val; pthread_mutex_unlock(&g_qmu); wait_done(v); }   // dm_gfx_suspend (hle_nvn.c)
VKB(vkQueueWaitIdle, VkResult, VkQueue q) { (void)q; pthread_mutex_lock(&g_qmu); uint64_t v = g_ev_val; pthread_mutex_unlock(&g_qmu); wait_done(v); return VK_SUCCESS; }
VKB(vkDeviceWaitIdle, VkResult, VkDevice d) { (void)d; return h_vkQueueWaitIdle(&g_queue); }
VK(vkCreateFence, VkResult, VkDevice d, const VkFenceCreateInfo *ci, const VkAllocationCallbacks *a, VkFence *out) {
    (void)d; (void)a; VkFence f = zalloc(sizeof *f); atomic_store(&f->val, ci->flags & VK_FENCE_CREATE_SIGNALED_BIT ? 0 : FENCE_NONE); *out = f; return VK_SUCCESS;
}
VK(vkDestroyFence, void, VkDevice d, VkFence f, const VkAllocationCallbacks *a) { (void)d; (void)a; free(f); }
VK(vkResetFences, VkResult, VkDevice d, uint32_t n, const VkFence *f) { (void)d; for (uint32_t i = 0; i < n; i++) atomic_store(&f[i]->val, FENCE_NONE); return VK_SUCCESS; }
VK(vkGetFenceStatus, VkResult, VkDevice d, VkFence f) { (void)d; return fence_done(f) ? VK_SUCCESS : VK_NOT_READY; }
VKB(vkWaitForFences, VkResult, VkDevice d, uint32_t n, const VkFence *fs, VkBool32 all, uint64_t timeout) {
    (void)d; uint64_t t0 = now_ns(); int warned = 0;
    pthread_mutex_lock(&g_done_mu);
    for (;;) {
        uint32_t k = 0; for (uint32_t i = 0; i < n; i++) k += fence_done(fs[i]);
        if (all ? k == n : k > 0) { pthread_mutex_unlock(&g_done_mu); return VK_SUCCESS; }
        uint64_t el = now_ns() - t0;
        if (el >= timeout) { pthread_mutex_unlock(&g_done_mu); return VK_TIMEOUT; }
        if (!warned && el > 5000000000ull) { warned = 1; LOG("[vk] [RED] fence wait over 5 s (%u fences, %u done; a fence never submitted never completes)", n, k); }
        uint64_t left = timeout - el, step = left < 100000000ull ? left : 100000000ull;
        struct timespec ts = {(time_t)(step / 1000000000ull), (long)(step % 1000000000ull)};
        pthread_cond_timedwait_relative_np(&g_done_cv, &g_done_mu, &ts);
    }
}
VK(vkCreateQueryPool, VkResult, VkDevice d, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *out) {
    (void)d; (void)a;
    if (ci->queryType != VK_QUERY_TYPE_TIMESTAMP && ci->queryType != VK_QUERY_TYPE_OCCLUSION) LOG("[vk] [RED] query pool type %d not implemented", ci->queryType);
    VkQueryPool q = zalloc(sizeof *q); q->type = ci->queryType; q->n = ci->queryCount;
    q->val = zalloc(umax(1, q->n) * sizeof *q->val); q->ticket = zalloc(umax(1, q->n) * sizeof *q->ticket);
    for (uint32_t i = 0; i < q->n; i++) q->ticket[i] = FENCE_NONE;
    VLOG("query pool %p: type %d, %u queries", (void *)q, q->type, q->n);
    *out = q; return VK_SUCCESS;
}
VK(vkDestroyQueryPool, void, VkDevice d, VkQueryPool q, const VkAllocationCallbacks *a) { (void)d; (void)a; if (q) { free(q->val); free(q->ticket); free(q); } }
VKB(vkGetQueryPoolResults, VkResult, VkDevice d, VkQueryPool q, uint32_t first, uint32_t n, size_t size, void *data, VkDeviceSize stride, VkQueryResultFlags fl) {
    (void)d; VkResult res = VK_SUCCESS; int w64 = fl & VK_QUERY_RESULT_64_BIT, av = fl & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
    for (uint32_t i = 0; i < n && first + i < q->n; i++) {
        uint32_t k = first + i; uint64_t t = q->ticket[k];
        if (t != FENCE_NONE && (fl & VK_QUERY_RESULT_WAIT_BIT)) wait_done(t);   // a query no submit has written yet stays unavailable instead of waiting forever
        int ok = t != FENCE_NONE && atomic_load(&g_done) >= t;
        uint8_t *p = (uint8_t *)data + (size_t)i * stride; if ((size_t)(p - (uint8_t *)data) + (w64 ? 8 : 4) * (av ? 2 : 1) > size) break;
        if (ok || (fl & VK_QUERY_RESULT_PARTIAL_BIT)) {
            uint64_t v = !ok ? 0 : q->type == VK_QUERY_TYPE_TIMESTAMP ? g_done_ns[t % DONE_RING] : q->val[k];
            if (w64) memcpy(p, &v, 8); else { uint32_t v32 = (uint32_t)v; memcpy(p, &v32, 4); }
        }
        if (av) { uint64_t a1 = ok; if (w64) memcpy(p + 8, &a1, 8); else { uint32_t a32 = ok; memcpy(p + 4, &a32, 4); } }
        if (!ok) res = VK_NOT_READY;
    }
    return res;
}
VK(vkCreateSemaphore, VkResult, VkDevice d, const VkSemaphoreCreateInfo *ci, const VkAllocationCallbacks *a, VkSemaphore *out) {
    (void)d; (void)ci; (void)a; *out = zalloc(sizeof **out); return VK_SUCCESS;   // every submit runs in order on one queue: waits are already satisfied
}
VK(vkDestroySemaphore, void, VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a) { (void)d; (void)a; free(s); }

// ---------- Swapchain and present ----------
VKB(vkCreateSwapchainKHR, VkResult, VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    (void)d; (void)a; dev_init();
    const fmt_t *f = fmt_of(ci->imageFormat);
    if (!f || f->pf == MTLPixelFormatInvalid) { LOG("[vk] [RED] swap chain format %d not mapped", ci->imageFormat); return VK_ERROR_FORMAT_NOT_SUPPORTED; }
    VkSwapchainKHR sc = zalloc(sizeof *sc); sc->n = umin(umax(ci->minImageCount, 2), 4);
    @autoreleasepool {
        for (uint32_t i = 0; i < sc->n; i++) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f->pf width:ci->imageExtent.width height:ci->imageExtent.height mipmapped:NO];
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | (ci->imageUsage & VK_IMAGE_USAGE_STORAGE_BIT ? MTLTextureUsageShaderWrite : 0);
            td.storageMode = MTLStorageModePrivate;
            VkImage im = zalloc(sizeof *im);
            im->tex = [g_dev newTextureWithDescriptor:td]; im->f = f; im->fmt = ci->imageFormat; im->type = VK_IMAGE_TYPE_2D;
            im->w = ci->imageExtent.width; im->h = ci->imageExtent.height; im->d = im->mips = im->layers = im->samples = 1;
            im->serial = mtl_ext_tex((__bridge void *)im->tex);
            sc->img[i] = im;
        }
    }
    VLOG("swap chain %ux%u format %d, %u images (at least %u requested), present mode %d", ci->imageExtent.width, ci->imageExtent.height, ci->imageFormat, sc->n, ci->minImageCount, ci->presentMode);
    *out = sc; return VK_SUCCESS;
}
VKB(vkDestroySwapchainKHR, void, VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks *a) {
    (void)d; (void)a; if (!sc) return;
    for (uint32_t i = 0; i < sc->n; i++) { if (sc->ticket[i]) mtl_present_wait(sc->ticket[i]); sc->img[i]->tex = nil; free(sc->img[i]); }   // the present path keeps its own reference to each texture
    free(sc);
}
VK(vkGetSwapchainImagesKHR, VkResult, VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *out) {
    (void)d; if (!out) { *n = sc->n; return VK_SUCCESS; }
    uint32_t k = umin(*n, sc->n); memcpy(out, sc->img, k * sizeof *out); *n = k; return k < sc->n ? VK_INCOMPLETE : VK_SUCCESS;
}
static uint64_t g_next_acq;   // pacing without the display link: the earliest time of the next acquire (one frame period after the previous one)
VKB(vkAcquireNextImageKHR, VkResult, VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence f, uint32_t *idx) {
    (void)d; (void)timeout; (void)sem;
    uint32_t i = sc->next++ % sc->n;
    uint64_t period = 1000000000ull / (uint64_t)mtl_fps_target(), now = now_ns();
    // Paced by the display link when it runs, as NVN's acquire (hle_nvn.c nvnWindowAcquireTexture): FIFO presents one frame per vsync. A clock of our own drifts against the screen:
    // Super Mario Galaxy 2 on an iPhone put two frames into some display-link ticks and none into others (about one tick in seven), and on a Mac window it ran at 30 fps
    if (mtl_link_active() && mtl_link_wait_tick((int)(60 / mtl_fps_target()), 2 * period)) g_next_acq = now_ns() + period;
    else {
        if (g_next_acq > now) { uint64_t w = g_next_acq - now; struct timespec ts = {(time_t)(w / 1000000000ull), (long)(w % 1000000000ull)}; nanosleep(&ts, NULL); }
        now = now_ns(); g_next_acq = g_next_acq && now < g_next_acq + period ? g_next_acq + period : now + period;
    }
    if (sc->ticket[i]) mtl_present_wait(sc->ticket[i]);   // its previous present has been shown, so drawing into it again is safe
    if (f) atomic_store(&f->val, 0);
    *idx = i; return VK_SUCCESS;
}
static uint64_t g_frames;
static void frame_end(void) {
    static uint64_t pd, pp, ps, pc;
    uint64_t f = ++g_frames, dr = g_n_draw, pa = g_n_pass, su = g_n_submit, co = g_n_copy;
    if (f <= 3 || f % 300 == 0)
        VLOG("frame %llu: draws %llu, passes %llu, submits %llu, copies %llu (since the last record, %llu frames); pipelines so far %llu, buffer memory %.1f MB", (unsigned long long)f, (unsigned long long)(dr - pd),
             (unsigned long long)(pa - pp), (unsigned long long)(su - ps), (unsigned long long)(co - pc), (unsigned long long)(f <= 3 ? 1 : 300), (unsigned long long)g_n_pso, g_mem_bytes / 1048576.0);
    if (f <= 3 || f % 300 == 0) { pd = dr; pp = pa; ps = su; pc = co; }
    dm_guest_frame_tick();
}
VKB(vkQueuePresentKHR, VkResult, VkQueue q, const VkPresentInfoKHR *pi) {
    (void)q; dm_gfx_enter();
    pthread_mutex_lock(&g_qmu); uint64_t v = g_ev_val; pthread_mutex_unlock(&g_qmu);
    int crop[4]; dm_vi_layer_crop(crop);   // the vi layer crop (hle_vi.c): Super Mario Galaxy shows the top-left 1280x720 of its 1920x1080 swapchain in handheld mode
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        VkSwapchainKHR sc = pi->pSwapchains[i]; uint32_t k = pi->pImageIndices[i];
        int fit = crop[2] > 0 && crop[3] > 0 && k < sc->n && (uint32_t)(crop[0] + crop[2]) <= sc->img[k]->w && (uint32_t)(crop[1] + crop[3]) <= sc->img[k]->h;
        if (k < sc->n) sc->ticket[k] = mtl_present_ext(sc->img[k]->serial, 1.0 / mtl_fps_target(), (__bridge void *)g_ev, v, fit ? crop : NULL);
        if (pi->pResults) pi->pResults[i] = VK_SUCCESS;
    }
    dm_gfx_leave();
    frame_end();
    return VK_SUCCESS;
}

// ---------- Function lookup ----------
static void *vk_lookup(const char *name) {
    static const hle_ent *sec; static unsigned long sz;
    if (!name || strncmp(name, "vk", 2)) return NULL;
    if (!sec) sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &sz);
    void *fn = NULL; size_t n = strlen(name);
    for (int pass = 0; pass < 2 && !fn; pass++) {
        size_t len = pass ? n - 3 : n;
        if (pass && (n < 4 || strcmp(name + n - 3, "KHR"))) break;   // second pass: an extension function promoted to core under the name without KHR
        for (unsigned long i = 0; i < sz / sizeof *sec && !fn; i++) if (!strncmp(sec[i].name, name, len) && !sec[i].name[len]) fn = sec[i].fn;
    }
    static const char *seen[512]; static int nseen; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    int known = 0; for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], name)) { known = 1; break; }
    if (!known && nseen < 512) { seen[nseen++] = strdup(name); VLOG("lookup %s → %s", name, fn ? "host implementation" : "none (returns null)"); }
    pthread_mutex_unlock(&mu);
    return fn;
}
VK(vkGetInstanceProcAddr, PFN_vkVoidFunction, VkInstance i, const char *name) { (void)i; return (PFN_vkVoidFunction)vk_lookup(name); }
VK(vkGetDeviceProcAddr, PFN_vkVoidFunction, VkDevice d, const char *name) { (void)d; return (PFN_vkVoidFunction)vk_lookup(name); }
