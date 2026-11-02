// NVN interface layer: takes over the nvnBootstrapLoader imported by main; the NVN driver in sdk is not executed at all.
// All 523 NVN names return counting thunks (nvn_thunks.S), and the thunks forward to the implementations here:
//   - those with semantics (device / memory pools / textures and samplers / queue / window / sync / events / command buffer memory) are implemented per NVN semantics, and the objects are real;
//   - those that only change GPU state and have no CPU-visible result (state objects, bindings, draws, clears...) are recorded as commands and replayed at submit;
//   - the rest (the 307 that main only loads but never calls + those without a prototype in the header) go to nvn_unimpl_entry: reports a failure and prints the name when called.
// GPU address = CPU address (memory pools use the real memory given by the game, also built as zero-copy MTLBuffer). Commands are recorded into the command memory given by the game and replayed at submit into a
// Metal command buffer (nvn_mtl.m): buffer copies / uniform updates go through blit (GPU ordering), event value writes / counters are written after the GPU completes;
// sync object = submit fence value, and waits are released only after the GPU completes.
// Every implementation gets a compile-time signature check against the prototypes in nvn_protos.h (NVNboolean parameters are received as uint32_t, only the low 8 bits are looked at).
// Environment switches: DM_NVN_FRAMES=N prints the call table per frame for the first N frames (default 3); DM_NVN_TSV=path writes the per-frame call table and timing to a TSV (DM_NVN_TSV_CALLS=0 writes timing only); DM_SLOWFRAME=1 writes only the per-frame breakdown of dropped frames to the log; DM_NVN_LOG=1 logs object creation.
#include "../../host/hle.h"
#include "nvn_protos.h"
#include "nvn_count.h"
#include "nvn_tex.h"
#include "nvn_mtl.h"
#include "nvn_pace.h"
#include "aoc_timing.h"
#include "aoc_stats.h"
#include "../../host/front.h"
#include "nvn_dirty.h"
#include <mach/mach_time.h>
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <TargetConditionals.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#if TARGET_OS_OSX
#define DM_MEM_CAP_DEFAULT_GB 24
#else
#define DM_MEM_CAP_DEFAULT_GB 0
#endif
#include <pthread/qos.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <mach/thread_policy.h>
#include <assert.h>
#include <stdatomic.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

// ---------- Slot table, thunks, unimplemented stubs ----------
typedef struct { _Atomic uint64_t cnt; void *fn; } nvn_slot_t;
nvn_slot_t nvn_slot[NVN_NFUNC];                 // The thunk accesses it by i*16; the layout must not change
_Static_assert(sizeof(nvn_slot_t) == 16, "the trampolines are addressed in 16-byte steps");
extern void *nvn_thunk[NVN_NFUNC];
extern const char *nvn_names[NVN_NFUNC];
extern void nvn_unimpl_entry(void);
extern void nvn_prof_entry(void);
extern void nvn_big_entry(void);
void *nvn_big_real[NVN_NFUNC];            // the real implementation of functions that run on the big host stack (nvn_entry.S nvn_big_entry)
void *nvn_real[NVN_NFUNC];               // DM_NVN_PROF: the real implementation called by the timing wrapper
_Atomic uint64_t nvn_prof_ticks[NVN_NFUNC]; // DM_NVN_PROF: cumulative time of each function (cntvct counts)
static uint8_t g_unimpl_seen[NVN_NFUNC];

uint64_t nvn_unimpl_c(nvn_slot_t *slot, uint64_t x0, uint64_t x1, uint64_t x2) {
    unsigned i = (unsigned)(slot - nvn_slot);
    if (!g_unimpl_seen[i]++) LOG("[RED] NVN unimplemented function called: %s (x0=%#llx x1=%#llx x2=%#llx)", nvn_names[i], x0, x1, x2);
    return 0;
}

typedef struct { const char *name; void *fn; } nvn_impl;
#define NVN(name, ret, ...)                                                                       \
    static ret h_##name(__VA_ARGS__);                                                             \
    static const NVNT_##name chk_##name __attribute__((unused)) = h_##name;                       \
    __attribute__((used, section("__DATA,swnvn"))) static const nvn_impl e_##name = {#name, (void *)h_##name}; \
    static ret h_##name(__VA_ARGS__)
// Newer interfaces without prototypes: no signature check
#define NVN_NOPROTO(name, ret, ...)                                                               \
    static ret h_##name(__VA_ARGS__);                                                             \
    __attribute__((used, section("__DATA,swnvn"))) static const nvn_impl e_##name = {#name, (void *)h_##name}; \
    static ret h_##name(__VA_ARGS__)

static int g_log;
uint64_t nvn_new_serial(void) { static _Atomic uint64_t n; return ++n; }
static unsigned g_nsem;                          // Number of semantic implementations (filled by nvn_init_slots)
#define OLOG(...) do { if (g_log) LOG("[nvn] " __VA_ARGS__); } while (0)

static void nvn_init_slots(void) {
    static int done; if (done) return; done = 1;
    g_log = getenv("DM_NVN_LOG") != NULL;
    unsigned long sz;
    const nvn_impl *im = (const nvn_impl *)getsectiondata(&_mh_execute_header, "__DATA", "swnvn", &sz);
    unsigned nim = im ? (unsigned)(sz / sizeof *im) : 0, hit = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) nvn_slot[i].fn = (void *)nvn_unimpl_entry;
    for (unsigned k = 0; k < nim; k++) {
        unsigned i = 0;
        while (i < NVN_NFUNC && strcmp(nvn_names[i], im[k].name)) i++;
        if (i == NVN_NFUNC) die("NVN implementation %s is not in main's name table", im[k].name);
        nvn_slot[i].fn = im[k].fn; hit++;
    }
    g_nsem = hit;
}

static void *nvn_lookup(const char *name) {
    nvn_init_slots();
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (!strcmp(nvn_names[i], name)) return nvn_thunk[i];
    LOG("[RED] NVN address lookup of an unknown name: %s", name ? name : "(null)");
    return NULL;
}

// nvnBootstrapLoader is imported by main (defined in sdk); after it is forcibly taken over, the NVN driver in sdk is no longer referenced
HLE_FORCE("nvnBootstrapLoader", "the NVN graphics API is replaced wholesale with a host null device (Metal behind it); the SDK's NVN driver does not run",
          void *, h_nvnBootstrapLoader, const char *name) { return nvn_lookup(name); }

// ---------- Boolean parameters: Switch callers do not extend them, only look at the low 8 bits ----------
#define B(x) ((x) & 0xff)

#include "nvn_obj.h"

// ---------- Device ----------
NVN(nvnDeviceBuilderSetDefaults, void, NVNdeviceBuilder *b) { memset(b, 0, sizeof(dev_builder_t)); }
NVN(nvnDeviceBuilderSetFlags, void, NVNdeviceBuilder *b, int f) { ((dev_builder_t *)b)->flags = f; }
NVN(nvnDeviceInitialize, NVNboolean, NVNdevice *d, const NVNdeviceBuilder *b) {
    dev_t_ *o = (dev_t_ *)d; memset(o, 0, sizeof *o);
    o->magic = MAGIC('D', 'E', 'V', 0); o->flags = ((const dev_builder_t *)b)->flags;
    o->origin = NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT; mtl_set_origin_lower_left(1);   // NVN's default; every game so far switched to upper-left right away, Link's Awakening keeps it
    mtl_set_depth_zero(0);   // NVN's default depth mode: near = -w (GL)
    OLOG("DeviceInitialize %p flags=%#x", (void *)d, o->flags);
    return 1;
}
NVN(nvnDeviceFinalize, void, NVNdevice *d) { ((dev_t_ *)d)->magic = 0; }
NVN(nvnDeviceGetProcAddress, PFNNVNGENERICFUNCPTRPROC, const NVNdevice *d, const char *name) {
    (void)d; return (PFNNVNGENERICFUNCPTRPROC)nvn_lookup(name);
}

// Device properties: values measured on Tegra X1 (nnheaders nvn_DeviceConstantsNX.h, obtained by calling nvnDeviceGetInteger item by item on SMO)
static const int g_devinfo[] = {
    // API version: 55.15 here, 53.313 for SDK 10 and older (nvn_api below; the profile's device_api overrides): SMBW (SDK 16) requires major == 55 and minor >= 15 (main 0x5a7b48); Super Mario 3D World + Bowser's Fury (SDK 10) requires major == 53 and minor >= 313 (main+0x711c88),
    // and on a mismatch it creates no device and leaves the device pointer null, which crashes in nnMain right after. DM_NVN_API=major.minor overrides
    [0x00] = 55, [0x01] = 15,
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 4096,
};
// The NVN API version the game's own driver would report follows its SDK: games check it and create no device on a mismatch. SDK 10 and older are API 53 (Super Mario 3D World 10.4,
// Age of Calamity 9.6, Link's Awakening 8, Odyssey 7.3, Three Houses 6.5); SDK 11 and later take 55.15
__attribute__((weak)) int dm_sdk_major(void) { return 0; }   // loader.c; graphics unit tests have no loader
__attribute__((weak)) int dm_glslc_gpu_code(int *major, int *minor) { *major = *minor = 0; return 0; }   // hle_glslc.c: the game's own GLSLC's GPU code version
static void nvn_api(int api[2]) {
#ifdef DM_GAME_NVN_API_MAJOR
    api[0] = DM_GAME_NVN_API_MAJOR; api[1] = DM_GAME_NVN_API_MINOR; LOG("[nvn] device API %d.%d (game profile device_api)", api[0], api[1]);
#else
    int m = dm_sdk_major(); api[0] = DM_SDK_OLD(m) ? 53 : g_devinfo[0]; api[1] = DM_SDK_OLD(m) ? 313 : g_devinfo[1]; LOG("[nvn] device API %d.%d (SDK %d)", api[0], api[1], m);
#endif
}
NVN(nvnDeviceGetInteger, void, const NVNdevice *d, NVNdeviceInfo what, int *out) {
    (void)d;
    unsigned w = (unsigned)what;
    // 0x5F separate sampler binding count and 0x60/0x61 are not in the table measured on SMO: 0x5F is given 4096 per the sampler pool limit, unverified; 0x60/0x61 report a failure and return 0
    // 0x67 (SDK 19): FINAL FANTASY TACTICS - The Ivalice Chronicles reads it before every program it loads and passes it as the alignment of its copies of the shader's control section and code
    // (main+0x49800; its allocator raises anything below 8 to 8). Not measured on hardware: 256, the alignment of shader code in a memory pool; the host reads shaders by key, so any value works
    if (w == 0x67) { *out = 256; return; }
    if (w >= sizeof g_devinfo / sizeof *g_devinfo) { LOG("[RED] nvnDeviceGetInteger unknown item %#x, returning 0", w); *out = 0; return; }
    *out = g_devinfo[w];
    // 0x2E / 0x30, the newest GPU code version the driver runs: the table's 1.14 is SMO's SDK 7 driver, while each SDK's driver runs the code of the GLSLC in the same SDK. A game that compiles
    // at run time checks its GLSLC's version against this range first (Catherine: Full Body, SDK 8.2.1: its GLSLC writes 1.15, main+0x330dcc refused it, no built-in program was compiled
    // and the first uniform lookup read a null program, main+0x3316dc), so the host reports at least the version of the GLSLC the game carries. A game that checks the shaders it ships
    // does the same (Bayonetta 3, SDK 13.3.2: PlatinumGames' shader objects carry their GPU code version, 1.9 to 1.16, and main+0xd0da20 refuses one above the driver's newest; at 1.14
    // its first built-in shader, hwfillcolor.vso at 1.15, was refused, the init failed and nnMain returned), so the SDK also sets a floor: what that SDK's GLSLC writes, 1.15 from SDK 8
    // (Catherine: Full Body's 8.2.1) and 1.16 from SDK 13 (Bayonetta 3's newest objects). A wider range only lets more shaders pass these checks
    if (w == 0x2E || w == 0x30) {
        int gv[2], m = dm_sdk_major(), sv[2] = {1, m >= 13 ? 16 : m >= 8 ? 15 : 0}, cur[2] = {g_devinfo[0x2E], g_devinfo[0x30]};
        if (sv[1] && (sv[0] > cur[0] || (sv[0] == cur[0] && sv[1] > cur[1]))) { cur[0] = sv[0]; cur[1] = sv[1]; }
        if (dm_glslc_gpu_code(&gv[0], &gv[1]) && (gv[0] > cur[0] || (gv[0] == cur[0] && gv[1] > cur[1]))) { cur[0] = gv[0]; cur[1] = gv[1]; }
        *out = w == 0x2E ? cur[0] : cur[1];
    }
    if (w <= 1) { static int api[2] = {-1, -1}; if (api[0] < 0) { nvn_api(api); const char *e = getenv("DM_NVN_API"); if (e) sscanf(e, "%d.%d", &api[0], &api[1]); } *out = api[w]; }
    OLOG("DeviceGetInteger %#x -> %d", w, *out);
}
NVN(nvnDeviceGetImageHandle, NVNimageHandle, const NVNdevice *d, int id) { (void)d; return (NVNimageHandle)id; }
// Separate texture / sampler handles: value = descriptor index (the sampler index goes in bit 20 and above, consistent with the bit layout of combined handles)
NVN(nvnDeviceGetSeparateTextureHandle, NVNseparateTextureHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateTextureHandle){(uint64_t)id}; }
NVN(nvnDeviceGetSeparateSamplerHandle, NVNseparateSamplerHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateSamplerHandle){(uint64_t)id << 20}; }
NVN(nvnDeviceInstallDebugCallback, void, NVNdevice *d, const PFNNVNDEBUGCALLBACKPROC cb, void *data, uint32_t enable) {
    dev_t_ *o = (dev_t_ *)d; o->dbg_cb = B(enable) ? (void *)cb : NULL; o->dbg_data = data;
}
NVN(nvnDeviceSetWindowOriginMode, void, NVNdevice *d, NVNwindowOriginMode m) { ((dev_t_ *)d)->origin = m; mtl_set_origin_lower_left(m == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT); LOG("[nvn] window origin %s", m ? "upper left (as D3D / Metal)" : "lower left (GL)"); }
NVN(nvnDeviceSetDepthMode, void, NVNdevice *d, NVNdepthMode m) { ((dev_t_ *)d)->depth_mode = m; mtl_set_depth_zero(m != 0); LOG("[nvn] depth mode %s", m ? "near=0 (as D3D / Metal)" : "near=-w (GL)"); }
NVN(nvnDeviceFinalizeCommandHandle, void, NVNdevice *d, NVNcommandHandle h) { (void)d; (void)h; }

// ---------- Memory pools ----------
NVN(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) { memset(b, 0, sizeof(pool_builder_t)); }
NVN(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) { ((pool_builder_t *)b)->dev = d; }
NVN(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *m, size_t n) { ((pool_builder_t *)b)->mem = m; ((pool_builder_t *)b)->size = n; }
NVN(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int f) { ((pool_builder_t *)b)->flags = f; }
static uint8_t *vpool_reserve(const pool_t *o); static void vpool_release(const pool_t *o);   // sparse virtual pools, see MapVirtual
NVN(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    const pool_builder_t *s = (const pool_builder_t *)b; pool_t *o = (pool_t *)p;
    o->magic = MAGIC('P', 'O', 'O', 'L'); o->flags = s->flags; o->mem = s->mem; o->size = s->size;
    if (!o->mem && (o->flags & 0x200)) { uint8_t *vr = vpool_reserve(o); if (vr) o->mem = vr; }   // a sparse virtual pool's own address range (see MapVirtual)
    else if (o->mem) { mtl_pool_add(o->mem, o->size); nvn_dirty_add_range(o->mem, o->size); }
    if (!o->mem && !(o->flags & 0x200)) LOG("[RED] MemoryPoolInitialize: non-virtual pool has no storage flags=%#x size=%#llx", o->flags, o->size);
    if (getenv("DM_NVN_VLOG")) { LOG("[nvn] MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, (unsigned long long)o->size, o->flags); void dm_backtrace(uint64_t fp); dm_backtrace((uint64_t)__builtin_frame_address(0)); }
    OLOG("MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, o->size, o->flags);
    return 1;
}
static void fl_add(const uint8_t *a, uint64_t n); static void fl_drop(const uint8_t *a, uint64_t n);   // flushed ranges, see nvn_flushed
NVN(nvnMemoryPoolFinalize, void, NVNmemoryPool *p) { if (((pool_t *)p)->flags & 0x200) vpool_release((pool_t *)p);
    if (((pool_t *)p)->mem && !(((pool_t *)p)->flags & 0x200)) {   // A virtual pool's storage is borrowed from a physical pool; do not unregister it
 nvn_dirty_remove_range(((pool_t *)p)->mem); mtl_pool_del(((pool_t *)p)->mem); fl_drop(((pool_t *)p)->mem, ((pool_t *)p)->size); } mtl_alias_drop_pool(p); ((pool_t *)p)->magic = 0; }
static void ft_flush(const uint8_t *a, uint64_t n);
NVN(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) {   // see ft_add and nvn_flushed
    const pool_t *o = (const pool_t *)p;
    if (!o->mem) return;
    if (!(o->flags & 0x200)) fl_add(o->mem + off, n);
    ft_flush(o->mem + off, n);
}
NVN(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) { return (NVNbufferAddress)((const pool_t *)p)->mem; }

// ---------- Descriptor pools ----------
// One ID space per pool. In NVN a texture or sampler ID indexes the pool the command buffer has bound (SetTexturePool / SetSamplerPool), and two pools may use the same IDs. The replay keeps
// one table for each kind, so every pool gets a base in it, in creation order: registrations go to base + ID, and binds recorded on a command buffer get the base of the pool that command
// buffer last bound (bind_handles). A game with one pool of each kind is unchanged (base 0).
// Fire Emblem: Three Houses: a 12,544-entry texture pool and a 3,856-entry sampler pool for the scene, and 4,096-entry ones its 2D UI command buffer binds. Both fill IDs from 256 up; the scene's
// default 8×8 white texture overwrote the UI's textures, and the title menu's labels, the name entry and other menus drew as plain bars or not at all. The scene also registers each draw's
// textures into fresh IDs of a ring in its large pool (8,729 and up a few seconds in); with the table's capacity taken from the last pool alone, those all read as empty slots and the screen was black
enum { NDPOOL = 16 };
static struct dpool_base { const void *pool; int base; } g_tpb[NDPOOL], g_spb[NDPOOL];
static int g_ntpb, g_nspb, g_tnext, g_snext;
static pthread_mutex_t g_dpool_mu = PTHREAD_MUTEX_INITIALIZER;
static int dpool_add(struct dpool_base *t, int *nt, int *next, const void *pool, int n, const char *kind) {   // → the pool's base (assigned at its first Initialize)
    pthread_mutex_lock(&g_dpool_mu);
    int i = 0; while (i < *nt && t[i].pool != pool) i++;
    if (i == *nt) {
        if (*nt == NDPOOL) { pthread_mutex_unlock(&g_dpool_mu); LOG("[RED] %s pools exceed %d: later ones share the first pool's numbering", kind, NDPOOL); return 0; }
        t[i] = (struct dpool_base){pool, *next}; (*nt)++; *next += (n + 255) & ~255;
        if (i) LOG("[nvn] pool #%d (%s, %d entries): numbered from %d in the host table, separate from the earlier pools", i + 1, kind, n, t[i].base);
    }
    int b = t[i].base; pthread_mutex_unlock(&g_dpool_mu); return b;
}
static int dpool_base(const struct dpool_base *t, int nt, const void *pool) { for (int i = 0; i < nt; i++) if (t[i].pool == pool) return t[i].base; return 0; }
int nvn_dpool_base(const void *pool, int tex) { return tex ? dpool_base(g_tpb, g_ntpb, pool) : dpool_base(g_spb, g_nspb, pool); }   // the replay's bound pools (nvn_mtl.m state_op)
// The pools each command buffer last bound are kept in its own host state (cmd_t tex_base / samp_base). They used to be a 64-entry list of command buffers that was never cleared:
// Dark Souls Remastered binds one of 12 sampler pools in more than 64 command buffers, and every later one read its samplers through the first pool's numbering
NVN(nvnTexturePoolInitialize, NVNboolean, NVNtexturePool *tp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)tp = (dpool_t){(const pool_t *)p, off, n}; OLOG("TexturePool n=%d", n);
    dpool_add(g_tpb, &g_ntpb, &g_tnext, tp, n, "texture"); mtl_set_tex_pool_n(g_tnext);
    return 1;
}
NVN(nvnTexturePoolFinalize, void, NVNtexturePool *tp) { (void)tp; }
NVN(nvnTexturePoolRegisterTexture, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) {
    mtl_reg_tex(dpool_base(g_tpb, g_ntpb, tp) + id, (const tex_t *)t, (const view_t *)v);
}
// Image descriptors live in the same pool index space as texture descriptors, and image handles are pool indices (nvnDeviceGetImageHandle; BindImages below), so an image
// registers into the same table. Hyrule Warriors: Age of Calamity registers the images its compute shaders write in the first battle (earlier games registered only textures)
NVN(nvnTexturePoolRegisterImage, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) {
    mtl_reg_tex(dpool_base(g_tpb, g_ntpb, tp) + id, (const tex_t *)t, (const view_t *)v);
}
NVN(nvnSamplerPoolInitialize, NVNboolean, NVNsamplerPool *sp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)sp = (dpool_t){(const pool_t *)p, off, n}; OLOG("SamplerPool n=%d", n); dpool_add(g_spb, &g_nspb, &g_snext, sp, n, "sampler"); return 1;
}
NVN(nvnSamplerPoolFinalize, void, NVNsamplerPool *sp) { (void)sp; }
NVN(nvnSamplerPoolRegisterSampler, void, const NVNsamplerPool *sp, int id, const NVNsampler *s) { mtl_reg_samp(dpool_base(g_spb, g_nspb, sp) + id, &((const samp_t *)s)->p); }
// straight from a builder, without a sampler object (FINAL FANTASY TACTICS - The Ivalice Chronicles registers its samplers this way)
NVN(nvnSamplerPoolRegisterSamplerBuilder, void, const NVNsamplerPool *sp, int id, const NVNsamplerBuilder *b) { mtl_reg_samp(dpool_base(g_spb, g_nspb, sp) + id, &((const samp_builder_t *)b)->p); }
// Unreal Engine reads a descriptor pool's size back at device setup (Octopath Traveler 0)
NVN(nvnTexturePoolGetMemoryPool, const NVNmemoryPool *, const NVNtexturePool *tp) { return (const NVNmemoryPool *)((const dpool_t *)tp)->pool; }
NVN(nvnTexturePoolGetMemoryOffset, ptrdiff_t, const NVNtexturePool *tp) { return (ptrdiff_t)((const dpool_t *)tp)->off; }
NVN(nvnTexturePoolGetSize, int, const NVNtexturePool *tp) { return ((const dpool_t *)tp)->n; }
NVN(nvnSamplerPoolGetMemoryPool, const NVNmemoryPool *, const NVNsamplerPool *sp) { return (const NVNmemoryPool *)((const dpool_t *)sp)->pool; }
NVN(nvnSamplerPoolGetMemoryOffset, ptrdiff_t, const NVNsamplerPool *sp) { return (ptrdiff_t)((const dpool_t *)sp)->off; }
NVN(nvnSamplerPoolGetSize, int, const NVNsamplerPool *sp) { return ((const dpool_t *)sp)->n; }

// ---------- Textures ----------
#define TB(b) ((tex_builder_t *)(b))
NVN(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    NVNdevice *dev = TB(b)->dev; memset(b, 0, sizeof(tex_builder_t)); TB(b)->dev = dev;
    TB(b)->d.target = NVN_TEXTURE_TARGET_2D; TB(b)->d.width = TB(b)->d.height = TB(b)->d.depth = TB(b)->d.levels = 1;
}
NVN(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) { TB(b)->dev = d; }
NVN(nvnTextureBuilderSetFlags, void, NVNtextureBuilder *b, int f) { TB(b)->d.flags = f; }
NVN(nvnTextureBuilderSetTarget, void, NVNtextureBuilder *b, NVNtextureTarget t) { TB(b)->d.target = t; }
NVN(nvnTextureBuilderSetWidth, void, NVNtextureBuilder *b, int v) { TB(b)->d.width = v; }
NVN(nvnTextureBuilderSetHeight, void, NVNtextureBuilder *b, int v) { TB(b)->d.height = v; }
NVN(nvnTextureBuilderSetDepth, void, NVNtextureBuilder *b, int v) { TB(b)->d.depth = v; }
NVN(nvnTextureBuilderSetSize1D, void, NVNtextureBuilder *b, int w) { TB(b)->d.width = w; TB(b)->d.height = TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetLevels, void, NVNtextureBuilder *b, int v) { TB(b)->d.levels = v; }
NVN(nvnTextureBuilderSetFormat, void, NVNtextureBuilder *b, NVNformat f) { TB(b)->d.format = f; }
NVN(nvnTextureBuilderSetSamples, void, NVNtextureBuilder *b, int s) { TB(b)->d.samples = s; }
NVN(nvnTextureBuilderSetStride, void, NVNtextureBuilder *b, ptrdiff_t s) { TB(b)->d.stride = (uint32_t)s; }
NVN(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t off) { TB(b)->pool = (pool_t *)p; TB(b)->off = off; }
NVN(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b, const NVNpackagedTextureLayout *l) {
    TB(b)->has_layout = l != NULL; if (l) memcpy(TB(b)->layout, l->layout, 8);
    TB(b)->d.pkg_block_h = l ? l->layout[0] + 1 : 0;
}
NVN(nvnTextureBuilderGetFlags, NVNtextureFlags, const NVNtextureBuilder *b) { return (NVNtextureFlags)((const tex_builder_t *)b)->d.flags; }
static uint64_t g_layout_logged;
static void log_layout(const tex_builder_t *b, const nvn_texlayout *L) {
    if (b->has_layout && g_layout_logged++ < 16)
        LOG("[nvn] packaged texture %ux%ux%u levels %u format %#x target %d layout bytes %02x%02x%02x%02x%02x%02x%02x%02x → block height log2=%u (from layout byte 0) size %#llx",
            b->d.width, b->d.height, b->d.depth, b->d.levels, b->d.format, b->d.target, b->layout[0], b->layout[1], b->layout[2],
            b->layout[3], b->layout[4], b->layout[5], b->layout[6], b->layout[7], L->tileH, L->size);
}
NVN(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L);
    if (!L.bpb) LOG("[RED] TextureBuilderGetStorageSize unknown format %#x", ((const tex_builder_t *)b)->d.format);
    log_layout((const tex_builder_t *)b, &L);
    return L.size;
}
NVN(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L); return L.align;
}
#define TX(t) ((tex_t *)(t))
#define CTX(t) ((const tex_t *)(t))
// Textures the CPU fills in place (profile tex_flush_upload): Tears of the Kingdom streams its textures (TexToGo, already in the GPU's block-linear layout) straight into
// CPU-cached memory pools and publishes each with nvnMemoryPoolFlushMappedRange (9,436 calls at boot), without a packaged layout or WriteTexels. They stayed GPU-only
// textures with content 0, and the G-buffer's base colour came out black. Textures that can hold such data (not render targets, not linear, no packaged layout, in a pool
// the CPU can write) are registered by address range; a flush that covers part of one marks it CPU-written (mtl_tex_cpu_written: uploaded from guest memory, re-uploaded
// when its pages change). Off by default: in another game a flushed range could cover an image a shader writes, whose GPU content the upload would replace
typedef struct { const uint8_t *mem; uint64_t size; tex_t t; uint64_t stamp; const void *span; } flush_tex;   // stamp: dirty-page generation at registration (tex_dirty_upload); span: its tracked range (nvn_dirty_span), so a check needs no search over every pool
static flush_tex *g_ft; static size_t g_nft, g_cft; static pthread_mutex_t g_ft_mu = PTHREAD_MUTEX_INITIALIZER;
static void ft_add(const tex_t *t) {
    // Buffer textures are views onto the pool buffer the GPU reads directly (tex_mtl in nvn_mtl.m); an upload would push Age of Calamity's 1,048,576-texel R32F one through tex_desc, past Metal's 2D limit
    if (!DM_GAME_TEX_FLUSH_UPLOAD || !t->mem || !t->pool || t->has_layout || t->d.target == NVN_TEXTURE_TARGET_BUFFER || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) ||
        (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_COMPRESSIBLE))) return;
    pthread_mutex_lock(&g_ft_mu);
    if (g_nft == g_cft && !(g_ft = realloc(g_ft, (g_cft = g_cft ? 2 * g_cft : 1024) * sizeof *g_ft))) die("flush_tex allocation failed");
    uint64_t st = nvn_dirty_now(); if (DM_GAME_TEX_DIRTY_UPLOAD) nvn_dirty_protect(t->mem, t->L.size);   // see nvn_ft_written
    g_ft[g_nft++] = (flush_tex){t->mem, t->L.size, *t, st, nvn_dirty_span(t->mem, t->L.size)};
    pthread_mutex_unlock(&g_ft_mu);
}
static void ft_del(uint64_t serial) {
    if (!DM_GAME_TEX_FLUSH_UPLOAD || !serial) return;
    pthread_mutex_lock(&g_ft_mu);
    for (size_t i = 0; i < g_nft; i++) if (g_ft[i].t.serial == serial) { g_ft[i] = g_ft[--g_nft]; break; }
    pthread_mutex_unlock(&g_ft_mu);
}
// Profile tex_dirty_upload: Tears of the Kingdom fills some textures from the CPU after creating them and publishes them with no NVN flush at all (os::FlushDataCache runs
// natively; its 121-layer ASTC terrain material arrays, 42 MB each). They stayed GPU-only textures with content 0, which ASTC decodes as the error colour, and the cave came out magenta.
// ft_add write-protects a registered texture's pages, and on its first resolution (tex_mtl, submit thread) a write since then makes it CPU-written like a flushed one
int nvn_ft_written(const tex_t *t) {
    if (!DM_GAME_TEX_DIRTY_UPLOAD || !t->serial) return 0;
    int r = 0; pthread_mutex_lock(&g_ft_mu);
    for (size_t i = 0; i < g_nft; i++) if (g_ft[i].t.serial == t->serial) {
        if (nvn_dirty_since_span(g_ft[i].span, g_ft[i].mem, g_ft[i].size, g_ft[i].stamp)) { r = 1; g_ft[i] = g_ft[--g_nft]; }
        break; }
    pthread_mutex_unlock(&g_ft_mu);
    if (r) mtl_tex_cpu_written(t);
    return r;
}
// Later writes (tex_dirty_upload): a texture first resolved before the CPU filled it stays a GPU-only texture, and the first-use check above never runs for it again.
// Once per submission, when guest memory has changed since the last look, the registered textures whose pages were written since registration become CPU-written
// (tex_mtl_ in nvn_mtl.m then drops the GPU-only texture and uploads it). Tears of the Kingdom fills its 5120x5120 BC4 roughness atlas from the CPU after first binding it,
// as tiles stream in. A texture the GPU writes (render target, shader-written image) may share pages with data the CPU writes: it leaves the list without an upload, which would
// replace the GPU's content
// Each poll looks at no more than FT_POLL_BYTES of registered textures, going on where the last one stopped; any guest write starts another pass over the whole list.
// Checking every texture at every submission cost TOTK 0.4 ms of submit CPU per frame on the Mac; this way a texture is picked up a few submissions later at most
enum { FT_POLL_BYTES = 128 << 20 };
void nvn_ft_poll(void) {
    if (!DM_GAME_TEX_DIRTY_UPLOAD) return;
    static uint64_t gen; static size_t cur, left;
    static tex_t *hit; static size_t chit; size_t nhit = 0;
    uint64_t now = nvn_dirty_now(), scanned = 0;
    pthread_mutex_lock(&g_ft_mu);
    if (now != gen) { gen = now; left = g_nft; }   // guest memory changed: every entry gets one more look
    while (left && g_nft && scanned < FT_POLL_BYTES) {
        if (cur >= g_nft) cur = 0;
        scanned += g_ft[cur].size; left--;
        if (nvn_dirty_since_span(g_ft[cur].span, g_ft[cur].mem, g_ft[cur].size, g_ft[cur].stamp)) {
            if (nhit == chit && !(hit = realloc(hit, (chit = chit ? 2 * chit : 64) * sizeof *hit))) die("nvn_ft_poll allocation failed");
            hit[nhit++] = g_ft[cur].t; g_ft[cur] = g_ft[--g_nft];
        } else cur++;
    }
    pthread_mutex_unlock(&g_ft_mu);
    for (size_t i = 0; i < nhit; i++) if (!mtl_tex_gpu_owned(hit[i].serial)) mtl_tex_cpu_written(&hit[i]);   // outside g_ft_mu: the ownership check takes the replay's serial table lock
}
static void ft_flush(const uint8_t *a, uint64_t n) {
    if (!DM_GAME_TEX_FLUSH_UPLOAD || !a || !n) return;
    pthread_mutex_lock(&g_ft_mu);
    for (size_t i = 0; i < g_nft;)
        if (g_ft[i].mem < a + n && a < g_ft[i].mem + g_ft[i].size) { mtl_tex_cpu_written(&g_ft[i].t); g_ft[i] = g_ft[--g_nft]; } else i++;
    pthread_mutex_unlock(&g_ft_mu);
}
// Flushed ranges (every game): what the CPU published with nvnMemoryPoolFlushMappedRange, as disjoint [lo, hi) spans sorted by start (touching spans merge), dropped with their pool.
// Until the GPU first uses a texture it has not written it, so storage the game flushed before that first use is the texture's content: tex_init_fill (nvn_mtl.m) fills the new GPU
// texture from it once, as it does for a pool the CPU cannot access. Breath of the Wild copies its precomputed sky scattering tables (System/KSys/sky.skybin: a 256x32x16 RGBA16F 3D
// texture and two 2D ones) into CPU_CACHED pools of their own with memcpy and flushes them (main+0x1257d50); as GPU textures of zeros the sky had no scattered light and came out
// brownish grey instead of blue (GitHub issue #52). Textures the CPU keeps writing after their first use still need the profile's tex_flush_upload / tex_dirty_upload
typedef struct { const uint8_t *lo, *hi; } fl_span;
static fl_span *g_fl; static size_t g_nfl, g_cfl; static pthread_mutex_t g_fl_mu = PTHREAD_MUTEX_INITIALIZER;
static size_t fl_find(const uint8_t *a, int touch) {   // the first span that ends after a (touch: or at a); g_fl_mu held
    size_t l = 0, h = g_nfl;
    while (l < h) { size_t m = (l + h) / 2; if (g_fl[m].hi < a || (!touch && g_fl[m].hi == a)) l = m + 1; else h = m; }
    return l;
}
static void fl_add(const uint8_t *a, uint64_t n) {
    if (!a || !n) return;
    const uint8_t *b = a + n;
    pthread_mutex_lock(&g_fl_mu);
    size_t i = fl_find(a, 1), j = i;
    if (i < g_nfl && g_fl[i].lo <= a && b <= g_fl[i].hi) { pthread_mutex_unlock(&g_fl_mu); return; }   // already covered (a buffer flushed every frame)
    while (j < g_nfl && g_fl[j].lo <= b) { if (g_fl[j].lo < a) a = g_fl[j].lo; if (g_fl[j].hi > b) b = g_fl[j].hi; j++; }   // spans i .. j-1 overlap or touch [a, b): merged into one
    if (i == j) {
        if (g_nfl == g_cfl && !(g_fl = realloc(g_fl, (g_cfl = g_cfl ? 2 * g_cfl : 256) * sizeof *g_fl))) die("flushed range allocation failed");
        memmove(g_fl + i + 1, g_fl + i, (g_nfl - i) * sizeof *g_fl); g_nfl++;
    } else if (j > i + 1) { memmove(g_fl + i + 1, g_fl + j, (g_nfl - j) * sizeof *g_fl); g_nfl -= j - i - 1; }
    g_fl[i] = (fl_span){a, b};
    pthread_mutex_unlock(&g_fl_mu);
}
static void fl_drop(const uint8_t *a, uint64_t n) {   // a pool goes: its part of every span (spans of neighbouring pools may have merged with it)
    const uint8_t *b = a + n;
    pthread_mutex_lock(&g_fl_mu);
    for (size_t i = fl_find(a, 0); i < g_nfl && g_fl[i].lo < b;) {
        fl_span s = g_fl[i];
        if (s.lo < a && s.hi > b) {   // the pool lies inside the span: split it
            if (g_nfl == g_cfl && !(g_fl = realloc(g_fl, (g_cfl = 2 * g_cfl) * sizeof *g_fl))) die("flushed range allocation failed");
            memmove(g_fl + i + 2, g_fl + i + 1, (g_nfl - i - 1) * sizeof *g_fl); g_nfl++;
            g_fl[i].hi = a; g_fl[i + 1] = (fl_span){b, s.hi}; break;
        }
        if (s.lo < a) { g_fl[i].hi = a; i++; }
        else if (s.hi > b) { g_fl[i].lo = b; break; }
        else { memmove(g_fl + i, g_fl + i + 1, (g_nfl - i - 1) * sizeof *g_fl); g_nfl--; }
    }
    pthread_mutex_unlock(&g_fl_mu);
}
int nvn_flushed(const void *p, uint64_t n) {   // whether the CPU flushed any of [p, p + n)
    const uint8_t *a = p;
    pthread_mutex_lock(&g_fl_mu);
    size_t i = fl_find(a, 0); int r = i < g_nfl && g_fl[i].lo < a + n;
    pthread_mutex_unlock(&g_fl_mu);
    return r;
}
NVN(nvnTextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    const tex_builder_t *s = (const tex_builder_t *)b; tex_t *o = TX(t);
    // Re-initializing a texture object that is still live (without a Finalize first): release the host texture of the old serial first. SMBW re-initializes its transient render targets on the same object every frame and never Finalizes them;
    //   previously every init created a new private MTLTexture and the old one stayed in the serial table forever, so GPU memory grew about 13 MB per frame (killed by the memory watchdog at 24 GB)
    if (o->magic == MAGIC('T', 'E', 'X', 0) && o->serial) { static uint64_t n; if (!n++) LOG("[nvn] TextureInitialize on a texture object that is still live (no Finalize): releasing the old host texture (first case ser=%llu)", (unsigned long long)o->serial); mtl_tex_release(o); }
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('T', 'E', 'X', 0); o->d = s->d; o->pool = s->pool; o->off = s->off;
    o->has_layout = s->has_layout; memcpy(o->layout, s->layout, 8); o->set_swz = s->set_swz; o->stencil = s->stencil; o->swz = s->swz;
    nvn_tex_layout(&o->d, &o->L);
    o->mem = s->pool && s->pool->mem ? s->pool->mem + s->off : NULL;
    o->serial = nvn_new_serial(); mtl_tex_init(o);
    {   // DM_NVN_TEXINFO=WxH also logs every texture of that size as it is initialized (render targets included): which ones share storage, and how their descriptions differ
        static int ti = -1; static uint32_t tw, th;
        if (ti < 0) { const char *e = getenv("DM_NVN_TEXINFO"); ti = e && sscanf(e, "%ux%u", &tw, &th) == 2; }
        if (ti && o->d.width == tw && o->d.height == th) { static uint64_t n; if (n++ < 200) LOG("[nvn] TEXINFO init ser=%llu mem=%p pool=%p(mem %p size %#llx flags %#x) off=%#llx target=%d format=%#x flags=%#x samples=%d %ux%ux%u levels=%u stride=%u size=%#llx",
            (unsigned long long)o->serial, (void *)o->mem, (void *)o->pool, o->pool ? (void *)o->pool->mem : NULL, o->pool ? (unsigned long long)o->pool->size : 0ull, o->pool ? o->pool->flags : 0,
            (unsigned long long)o->off, o->d.target, o->d.format, o->d.flags, o->d.samples, o->d.width, o->d.height, o->d.depth, o->d.levels, o->d.stride, (unsigned long long)o->L.size); }
    }
    // See SetPackagedTextureData. Linear textures are not re-uploaded as packaged: they are created on the pool buffer (tex_linear), whatever the pool's CPU access, and read what was copied in
    if (s->pkg_data && o->mem) {
        memcpy(o->mem, s->pkg_data, o->L.size);
        if (!(o->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) mtl_tex_cpu_written(o);
    }
    OLOG("Texture %p %ux%ux%u lv%u fmt=%#x tgt=%d flags=%#x size=%#llx", (void *)t, o->d.width, o->d.height, o->d.depth,
         o->d.levels, o->d.format, o->d.target, o->d.flags, o->L.size);
    ft_add(o);
    return 1;
}
NVN(nvnTextureFinalize, void, NVNtexture *t) { ft_del(TX(t)->serial); mtl_tex_release(TX(t)); TX(t)->magic = 0; }
NVN(nvnTextureGetFlags, NVNtextureFlags, const NVNtexture *t) { return (NVNtextureFlags)CTX(t)->d.flags; }
NVN(nvnTextureGetWidth, int, const NVNtexture *t) { return (int)CTX(t)->d.width; }
NVN(nvnTextureGetHeight, int, const NVNtexture *t) { return (int)CTX(t)->d.height; }
NVN(nvnTextureGetDepth, int, const NVNtexture *t) { return (int)CTX(t)->d.depth; }
NVN(nvnTextureGetStorageSize, int, const NVNtexture *t) { return (int)CTX(t)->L.size; }
NVN(nvnTextureGetStride, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->d.stride; }   // the row stride of a linear texture as built (0 for block-linear ones)
// The queries below are used by Engage (Unity) at startup: it calls MemoryPoolMap on "the pool the texture / buffer lives in", and with a stub returning 0 it dereferenced a null pool and crashed
NVN(nvnTextureGetLevels, int, const NVNtexture *t) { return (int)CTX(t)->d.levels; }
NVN(nvnTextureGetSamples, int, const NVNtexture *t) { return CTX(t)->d.samples; }
NVN(nvnTextureGetMemoryPool, NVNmemoryPool *, const NVNtexture *t) { return (NVNmemoryPool *)CTX(t)->pool; }
NVN(nvnTextureGetMemoryOffset, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->off; }
// The GPU address of the texture's storage (DRAGON QUEST XI S, Unreal Engine 4.18, asks for it at start): pool address + offset, the same address space as nvnMemoryPoolGetBufferAddress
NVN(nvnTextureGetTextureAddress, NVNtextureAddress, const NVNtexture *t) { const tex_t *o = CTX(t); return o->pool ? (NVNtextureAddress)(o->pool->mem + o->off) : 0; }
// The size of a texture's ZCull save area (SaveZCullData / RestoreZCullData). The host does no ZCull and never touches the area, but games allocate it from their own pools, and NVN
// does not report 0. Cuphead (Unity 2017.4) allocates one per depth target (main+0x1948060) from its pool allocator, which gives a zero-size request the offset of a live block; its
// map of allocations then loses that block (the zero-size one's size overwrites it, and freeing the zero-size one deletes it), and the block's own free later pushes a garbage-sized
// range onto the free list (main+0x1945b10). Buffers allocated after that overlap live ones: copies read staging bytes other copies had overwritten, and sprites broke into slivers,
// went missing, or flickered every fourth frame (the upload ring's period). The hardware's formula is not known; this one grows with the texture: 8 bytes per 32x32 tile and layer,
// at least 256
static size_t zcull_size(const nvn_texdesc *d) {
    uint64_t tiles = (uint64_t)((d->width + 31) / 32) * ((d->height + 31) / 32) * (d->depth ? d->depth : 1);
    return tiles ? (size_t)((tiles * 8 + 0xff) & ~0xffull) : 0x100;
}
NVN(nvnTextureGetZCullStorageSize, size_t, const NVNtexture *t) { return zcull_size(&CTX(t)->d); }
static unsigned tex_swz(const tex_t *t) { return t->set_swz ? t->swz : 2u | 3u << 3 | 4u << 6 | 5u << 9; }   // the channel mapping: identity R G B A when TextureBuilderSetSwizzle was not called
// The mapping counts: BotW and Odyssey rebuild layout captures on the same storage with a new mapping, and replace the registered descriptor only if the two compare different (BotW main+0xec33f0)
NVN(nvnTextureCompare, NVNboolean, const NVNtexture *a, const NVNtexture *b) {
    const tex_t *x = CTX(a), *y = CTX(b);
    return !memcmp(&x->d, &y->d, sizeof x->d) && x->mem == y->mem && tex_swz(x) == tex_swz(y);
}
// No prototype in the header; call sites such as main 0x174899c compare the return value with 1 and set some status bit 0x100 if not equal. Meaning unverified; return 1 for now
NVN_NOPROTO(nvnTextureGetRawStorageClass, int, const NVNtexture *t) { (void)t; return 1; }
#define VW(v) ((view_t *)(v))
NVN(nvnTextureViewSetDefaults, void, NVNtextureView *v) { memset(v, 0, sizeof(view_t)); }
NVN(nvnTextureViewSetLevels, void, NVNtextureView *v, int base, int n) { VW(v)->set_levels = 1; VW(v)->base_level = base; VW(v)->nlevels = n; }
NVN(nvnTextureViewSetLayers, void, NVNtextureView *v, int min, int n) { VW(v)->set_layers = 1; VW(v)->min_layer = min; VW(v)->nlayers = n; }
NVN(nvnTextureViewSetFormat, void, NVNtextureView *v, NVNformat f) { VW(v)->set_format = 1; VW(v)->format = f; }
NVN(nvnTextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) { VW(v)->set_target = 1; VW(v)->target = t; }
NVN(nvnTextureViewGetLayers, NVNboolean, const NVNtextureView *v, int *min, int *n) {
    const view_t *o = (const view_t *)v; *min = o->min_layer; *n = o->nlayers; return o->set_layers;
}
NVN(nvnTextureGetViewOffset, ptrdiff_t, const NVNtexture *t, const NVNtextureView *v) {
    const tex_t *o = CTX(t); const view_t *w = (const view_t *)v;
    if (!w) return 0;
    uint64_t off = w->set_levels ? nvn_tex_level_offset(&o->d, (unsigned)w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * o->L.layer_size;
    return (ptrdiff_t)off;
}
// CPU texture read (NVN ReadTexels): for packaged textures the pool memory is the ground truth (de-layout and read directly); GPU-written textures are read back from the MTLTexture (nvn_mtl.m)
static void read_texels(const tex_t *o, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, ptrdiff_t row, ptrdiff_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(o->d.format, &bw, &bh);
    uint32_t wb = (uint32_t)((r->width + bw - 1) / bw) * bpb, hb = (uint32_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    mtl_read_texels(o, level, layer0, r, dst, (size_t)row, (size_t)img);
}
NVN(nvnTextureReadTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst) {
    read_texels(CTX(t), (const view_t *)v, r, dst, 0, 0);
}
NVN(nvnTextureReadTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst, ptrdiff_t row, ptrdiff_t img) {
    read_texels(CTX(t), (const view_t *)v, r, dst, row, img);
}
NVN(nvnTextureInvalidateTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }

// ---------- Samplers ----------
#define SB(b) (&((samp_builder_t *)(b))->p)
NVN(nvnSamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    NVNdevice *dev = ((samp_builder_t *)b)->dev; memset(b, 0, sizeof(samp_builder_t)); ((samp_builder_t *)b)->dev = dev;
    samp_par *o = SB(b);
    o->minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR; o->magf = NVN_MAG_FILTER_LINEAR;   // GL defaults
    o->ws = o->wt = o->wr = NVN_WRAP_MODE_REPEAT; o->cmp_func = NVN_COMPARE_FUNC_LESS;
    o->lod_min = -1000.f; o->lod_max = 1000.f; o->aniso = 1.f;
}
NVN(nvnSamplerBuilderSetDevice, void, NVNsamplerBuilder *b, NVNdevice *d) { ((samp_builder_t *)b)->dev = d; }
NVN(nvnSamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder *b, NVNminFilter mi, NVNmagFilter ma) { SB(b)->minf = mi; SB(b)->magf = ma; }
NVN(nvnSamplerBuilderSetWrapMode, void, NVNsamplerBuilder *b, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) { SB(b)->ws = s; SB(b)->wt = t; SB(b)->wr = r; }
NVN(nvnSamplerBuilderSetLodClamp, void, NVNsamplerBuilder *b, float lo, float hi) { SB(b)->lod_min = lo; SB(b)->lod_max = hi; }
NVN(nvnSamplerBuilderSetLodBias, void, NVNsamplerBuilder *b, float v) { SB(b)->lod_bias = v; }
NVN(nvnSamplerBuilderSetCompare, void, NVNsamplerBuilder *b, NVNcompareMode m, NVNcompareFunc f) { SB(b)->cmp_mode = m; SB(b)->cmp_func = f; }
NVN(nvnSamplerBuilderSetBorderColor, void, NVNsamplerBuilder *b, const float *c) { memcpy(SB(b)->border, c, 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy, void, NVNsamplerBuilder *b, float v) { SB(b)->aniso = v; }
NVN(nvnSamplerBuilderSetLodSnap, void, NVNsamplerBuilder *b, float v) { (void)b; (void)v; }   // No Metal equivalent, ignored
NVN(nvnSamplerBuilderGetMinMagFilter, void, const NVNsamplerBuilder *b, NVNminFilter *mi, NVNmagFilter *ma) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *mi = (NVNminFilter)o->minf; *ma = (NVNmagFilter)o->magf;
}
NVN(nvnSamplerBuilderGetWrapMode, void, const NVNsamplerBuilder *b, NVNwrapMode *s, NVNwrapMode *t, NVNwrapMode *r) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *s = (NVNwrapMode)o->ws; *t = (NVNwrapMode)o->wt; *r = (NVNwrapMode)o->wr;
}
NVN(nvnSamplerBuilderGetLodClamp, void, const NVNsamplerBuilder *b, float *lo, float *hi) {   // Bayonetta 3 reads it back while building its samplers
    const samp_par *o = &((const samp_builder_t *)b)->p; *lo = o->lod_min; *hi = o->lod_max;
}
NVN(nvnSamplerInitialize, NVNboolean, NVNsampler *s, const NVNsamplerBuilder *b) {
    samp_t *o = (samp_t *)s; o->magic = MAGIC('S', 'M', 'P', 0); o->p = ((const samp_builder_t *)b)->p; return 1;
}
NVN(nvnSamplerFinalize, void, NVNsampler *s) { ((samp_t *)s)->magic = 0; }

// ---------- Programs ----------
NVN(nvnProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *d) {
    (void)d; memset(p, 0, sizeof(prog_t)); ((prog_t *)p)->magic = MAGIC('P', 'R', 'O', 'G'); return 1;
}
static void pk_drop(const void *p);
NVN(nvnProgramFinalize, void, NVNprogram *p) { mtl_prog_release((prog_t *)p); ((prog_t *)p)->magic = 0; pk_drop(p); }
// a program's per-stage keys (side table: guest NVNprogram address → host object; bit i of ok = stage i computed)
typedef struct { uint8_t key[6][8]; uint8_t ok; } prog_keys_t;
static pthread_mutex_t g_pk_mu = PTHREAD_MUTEX_INITIALIZER; static CFMutableDictionaryRef g_pk;   // a dedicated table (keys not shared with other side tables)
static prog_keys_t *pk_get(const void *p, int make) {
    pthread_mutex_lock(&g_pk_mu);
    if (!g_pk) g_pk = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    prog_keys_t *k = (prog_keys_t *)CFDictionaryGetValue(g_pk, p);
    if (!k && make) { k = calloc(1, sizeof *k); CFDictionarySetValue(g_pk, p, k); }
    pthread_mutex_unlock(&g_pk_mu);
    return k;
}
static void pk_drop(const void *p) {
    pthread_mutex_lock(&g_pk_mu);
    prog_keys_t *k = g_pk ? (prog_keys_t *)CFDictionaryGetValue(g_pk, p) : NULL;
    if (k) { CFDictionaryRemoveValue(g_pk, p); free(k); }
    pthread_mutex_unlock(&g_pk_mu);
}
// mapped = 1: computed only when the code area is in a registered memory pool (at SetShaders: Mario sets some programs with an unmapped address first and fills it in before the first draw)
static int keys_hash(const NVNshaderData *sd, int mapped, uint8_t out[8]) {
    uint32_t ncode = 0; void *mb; uint64_t mo;
    if (!sd->control) return 0;
    memcpy(&ncode, (const uint8_t *)sd->control + 0x704, 4);
    int code = sd->data && ncode && ncode < (1u << 24);
    if (mapped && !(code && mtl_addr(sd->data, ncode, &mb, &mo))) return 0;
    // Control +0x20 holds the section's own length: sections of at least 0x880 bytes (all of Mario's, Engage's, Dread's and 13 Sentinels') use the first 0x880; SDK 10's 3D World has 0x850-byte ones followed by other data, so only their own length is used (shader_split.py ctl_len)
    uint32_t nctl = 0x880, own; memcpy(&own, (const uint8_t *)sd->control + 0x20, 4);
    if (own >= 0x760 && own < 0x880) nctl = own;
    uint8_t md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_CTX hc; CC_SHA256_Init(&hc);
    CC_SHA256_Update(&hc, sd->control, nctl);
    if (code) CC_SHA256_Update(&hc, (const void *)(uintptr_t)sd->data, ncode);
    CC_SHA256_Final(md, &hc); memcpy(out, md, 8);
    return 1;
}
int prog_key(const prog_t *p, unsigned st, uint8_t key[8]) {
    if (st >= p->nstage) return 0;
    prog_keys_t *k = pk_get(p, 1);
    if (!(k->ok >> st & 1)) { keys_hash(&p->sd[st], 0, k->key[st]); k->ok |= (uint8_t)(1u << st); }
    memcpy(key, k->key[st], 8);
    return 1;
}
NVN(nvnProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *sd) {
    prog_t *o = (prog_t *)p;
    if (n < 0 || n > 6) { LOG("[RED] ProgramSetShaders stage count %d", n); return 0; }
    mtl_prog_release(o); o->nstage = n; memcpy(o->sd, sd, n * sizeof *sd); o->serial = nvn_new_serial();
    // The keys are computed here (see prog_key in nvn_obj.h): the control section is only guaranteed valid during this call; the code area lives in a memory pool and stays valid while the program is in use
    static int dump = -1; if (dump < 0) dump = getenv("DM_NVN_SHDUMP") != NULL;
    prog_keys_t *k = pk_get(p, 1);
    k->ok = 0;
    for (int i = 0; i < n; i++) {
        if (keys_hash(&sd[i], 1, k->key[i])) k->ok |= (uint8_t)(1u << i);
        // DM_NVN_SHDUMP (debugging): keep a copy of the control section so the replay layer can save shaders missing from the map (never freed)
        if (dump && sd[i].control) { void *c = malloc(0x880); memcpy(c, sd[i].control, 0x880); o->sd[i].control = c; }
    }
    OLOG("ProgramSetShaders %p n=%d data0=%#llx", (void *)p, n, n ? (unsigned long long)sd[0].data : 0ull);
    return 1;
}

// ---------- Sync / events ----------
NVN(nvnSyncInitialize, NVNboolean, NVNsync *s, NVNdevice *d) { (void)d; *(sync_t *)s = (sync_t){MAGIC('S', 'Y', 'N', 'C'), 0, 0}; return 1; }
NVN(nvnSyncFinalize, void, NVNsync *s) { ((sync_t *)s)->magic = 0; }
NVN(nvnSyncSetDebugLabel, void, NVNsync *s, const char *l) { (void)s; (void)l; }
// Fence = "all commands previously submitted to the queue have completed": record the fence value of the most recent submit
NVN(nvnQueueFenceSync, void, NVNqueue *q, NVNsync *s, NVNsyncCondition c, int f) { (void)q; (void)c; (void)f; ((sync_t *)s)->fenced = 1; ((sync_t *)s)->fence = mtl_last_fence(); }
// A sync object that was never fenced waits until timeout, as on a real device; a fenced one is released only after the GPU has actually completed up to that fence value
// A GPU-side wait on a sync: the host has a single Metal queue that executes submissions in order, so commands before the fence are always done first and nothing needs waiting. A sync not fenced yet (fenced later by another thread) is not waited for here; logged once
NVN(nvnQueueWaitSync, NVNboolean, NVNqueue *q, const NVNsync *s) { (void)q; static int once; if (!((const sync_t *)s)->fenced && !once++) LOG("[nvn] QueueWaitSync: the sync object has no fence yet, the host does not wait"); return 1; }
// ZCull data: the host does no ZCull, so there is nothing to save / restore
NVN(nvnCommandBufferRestoreZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }
NVN(nvnCommandBufferSaveZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }
NVN(nvnSyncWait, NVNsyncWaitResult, const NVNsync *s, uint64_t timeout) {
    const sync_t *o = (const sync_t *)s;
    if (!o->fenced) return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    if (mtl_wait(o->fence, 0)) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    return mtl_wait(o->fence, timeout) ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}
NVN(nvnEventBuilderSetDefaults, void, NVNeventBuilder *b) { memset(b, 0, sizeof(ev_builder_t)); }
NVN(nvnEventBuilderSetStorage, void, NVNeventBuilder *b, const NVNmemoryPool *p, int64_t off) { *(ev_builder_t *)b = (ev_builder_t){(const pool_t *)p, off}; }
NVN(nvnEventInitialize, NVNboolean, NVNevent *e, const NVNeventBuilder *b) {
    const ev_builder_t *s = (const ev_builder_t *)b;
    if (!s->pool || !s->pool->mem) { LOG("[RED] EventInitialize has no storage"); return 0; }
    *(ev_t *)e = (ev_t){MAGIC('E', 'V', 'N', 'T'), (volatile uint32_t *)(s->pool->mem + s->off)}; return 1;
}
NVN(nvnEventFinalize, void, NVNevent *e) { ((ev_t *)e)->magic = 0; }
NVN(nvnEventGetValue, uint32_t, const NVNevent *e) { return *((const ev_t *)e)->val; }
static void ev_signal(const ev_t *e, int mode, uint32_t v) { if (mode == NVN_EVENT_SIGNAL_MODE_ADD) *e->val += v; else *e->val = v; }
NVN(nvnEventSignal, void, NVNevent *e, NVNeventSignalMode m, uint32_t v) { ev_signal((const ev_t *)e, m, v); }

// ---------- Queue ----------
#define QB(b) ((queue_builder_t *)(b))
NVN(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    NVNdevice *dev = QB(b)->dev; memset(b, 0, sizeof(queue_builder_t)); QB(b)->dev = dev;
    QB(b)->cmd_sz = 65536; QB(b)->comp_sz = 262144; QB(b)->ctl_sz = 16384; QB(b)->flush = 4096;   // Device defaults
}
NVN(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) { QB(b)->dev = d; }
NVN(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { QB(b)->flags = f; }
NVN(nvnQueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->cmd_sz = n; }
NVN(nvnQueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->comp_sz = n; }
NVN(nvnQueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->ctl_sz = n; }
NVN(nvnQueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t n) { QB(b)->flush = n; }
// Queue memory: the sum of the command + compute + control blocks rounded to 4 KiB (the host does not use it; the game only needs to allocate this size)
NVN(nvnQueueBuilderGetQueueMemorySize, size_t, const NVNqueueBuilder *b) {
    const queue_builder_t *o = (const queue_builder_t *)b;
    return (o->cmd_sz + o->comp_sz + o->ctl_sz + 4095) & ~(size_t)4095;
}
NVN(nvnQueueBuilderSetQueueMemory, void, NVNqueueBuilder *b, void *m, size_t n) { QB(b)->mem = m; QB(b)->mem_sz = n; }
NVN(nvnQueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    *(queue_t *)q = (queue_t){MAGIC('Q', 'U', 'E', 'U'), ((const queue_builder_t *)b)->dev, 0};
    OLOG("QueueInitialize %p", (void *)q); return 1;
}
NVN(nvnQueueFinalize, void, NVNqueue *q) { ((queue_t *)q)->magic = 0; }
NVN(nvnQueueGetError, NVNqueueGetErrorResult, NVNqueue *q, NVNqueueErrorInfo *info) {
    (void)q; if (info) memset(info, 0, sizeof *info); return NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR;
}
NVN(nvnQueueFlush, void, NVNqueue *q) { (void)q; }
NVN(nvnQueueFinish, void, NVNqueue *q) { (void)q; mtl_finish(); }
// NVN 55 telemetry queries. The host does not allocate the builder's command / compute / control blocks, so their reported usage is zero; the game only reads these figures for diagnostics.
NVN_NOPROTO(nvnQueueGetTotalCommandMemoryUsed, size_t, const NVNqueue *q) { (void)q; return 0; }
NVN_NOPROTO(nvnQueueGetTotalControlMemoryUsed, size_t, const NVNqueue *q) { (void)q; return 0; }
NVN_NOPROTO(nvnQueueGetTotalComputeMemoryUsed, size_t, const NVNqueue *q) { (void)q; return 0; }
NVN_NOPROTO(nvnQueueResetMemoryUsageCounts, void, NVNqueue *q) { (void)q; }

// ---------- Command buffer: record into the game's command memory, perform CPU-visible side effects at submit ----------
#define CB(c) ((cmd_t *)(c))
// Room kept at the segment end after every record but the END: one JUMP. The END needs none (nothing follows it), and every other record is at least 16 bytes, so a JUMP
// only ever stands where a record was to go: linking never writes past the bytes the recording takes in one piece, which is what GetCommandMemoryUsed reports and games size
// command memory by. Tears of the Kingdom (GitHub issue #61) gives each model's prerecorded lists fixed slots of the size a recording of that list measured, slot k at
// base + size × k, and passes AddCommandMemory max(size, 512) (main+0xbb120). With 24 bytes kept for "a JUMP or an END", a 496-byte list's END did not fit in 512,
// went to a host segment, and the 16-byte JUMP at 488 reached 8 bytes into the next slot; recording that slot wrote its first record over the JUMP's target, and the
// replay jumped to 0x1000000003 (that CALL record's header) after 8 to 30 minutes of play
#define OP_RESERVE 16

NVN(nvnCommandBufferInitialize, NVNboolean, NVNcommandBuffer *c, NVNdevice *d) {
    memset(c, 0, sizeof(cmd_t)); CB(c)->magic = MAGIC('C', 'M', 'D', 'B'); CB(c)->dev = d; return 1;
}
NVN(nvnCommandBufferFinalize, void, NVNcommandBuffer *c) { CB(c)->magic = 0; }
NVN(nvnCommandBufferSetMemoryCallback, void, NVNcommandBuffer *c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) { CB(c)->cb = cb; }
NVN(nvnCommandBufferSetMemoryCallbackData, void, NVNcommandBuffer *c, void *d) { CB(c)->cb_data = d; }
static uint8_t *align8(uint8_t *p) { return (uint8_t *)(((uintptr_t)p + 7) & ~(uintptr_t)7); }
// Diagnostic DM_NVN_CMDSTAT=1: every 300 frames log one line of command recording usage -- how many bytes / records were recorded per frame, how large a command memory segment the game gives each time, how many out-of-memory callbacks per frame, how many host-supplied segments,
// and the count and bytes of each command type (to find "how much larger our records are than real-device commands"). Counts only; does not change recording / replay
static struct { int on; uint64_t bytes, recs, grants, grant_bytes, grant_min, grant_max, cbs, hostseg, op_n[64], op_b[64], frame0, waste; } g_cs = { .on = -1 };
static pthread_mutex_t g_cs_mu = PTHREAD_MUTEX_INITIALIZER;
uint64_t dm_nvn_frame_count(void);
static int cs_on(void) { if (g_cs.on < 0) g_cs.on = getenv("DM_NVN_CMDSTAT") != NULL; return g_cs.on; }
static void cs_op(uint32_t op, uint32_t size) { if (!cs_on() || op >= 64) return; pthread_mutex_lock(&g_cs_mu); g_cs.op_n[op]++; g_cs.op_b[op] += size; pthread_mutex_unlock(&g_cs_mu); }
static void cs_dump(void) {   // Called once per frame (at present)
    if (!cs_on()) return; uint64_t f = dm_nvn_frame_count(); if (f - g_cs.frame0 < 300) return;
    pthread_mutex_lock(&g_cs_mu); double n = (double)(f - g_cs.frame0); char top[700]; int k = 0; uint8_t used[64] = {0};
    for (int r = 0; r < 8; r++) { int bi = -1; for (int i = 0; i < 64; i++) if (!used[i] && g_cs.op_b[i] && (bi < 0 || g_cs.op_b[i] > g_cs.op_b[bi])) bi = i; if (bi < 0) break; used[bi] = 1;
        k += snprintf(top + k, sizeof top - k, " op %d: %.0f × %.0f bytes = %.1f KB;", bi, g_cs.op_n[bi] / n, (double)g_cs.op_b[bi] / (double)g_cs.op_n[bi], g_cs.op_b[bi] / n / 1024.0); }
    LOG("[nvn] command memory up to frame %llu: recorded per frame %.1f KB / %.0f records; segments from the game per frame %.2f, mean %.1f KB each (min %.1f / max %.1f); out-of-memory callbacks per frame %.2f; host-lent segments per frame %.3f; segment tails dropped on a switch per frame %.1f KB | per frame%s",
        (unsigned long long)f, g_cs.bytes / n / 1024.0, g_cs.recs / n, g_cs.grants / n, g_cs.grants ? g_cs.grant_bytes / (double)g_cs.grants / 1024.0 : 0, g_cs.grant_min / 1024.0, g_cs.grant_max / 1024.0, g_cs.cbs / n, g_cs.hostseg / n, g_cs.waste / n / 1024.0, top);
    int on = g_cs.on; memset(&g_cs, 0, sizeof g_cs); g_cs.on = on; g_cs.frame0 = f; pthread_mutex_unlock(&g_cs_mu);
}
static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) { op_hdr h = {op, size}; memcpy(p, &h, sizeof h); if (op) cs_op(op, size); }
// New segment: if the recording in progress has records, write a JUMP at the end of the old segment to link it. A recording with none yet starts in the new segment
// (its first record is its handle), and the bytes at the old write position are not its own: after an END that took the last bytes there is no room there at all
static void cmd_new_segment(cmd_t *o, uint8_t *mem, size_t n) {
    uint8_t *start = align8(mem), *end = mem + n;
    if (o->recording == 1 && o->cur) { if (cs_on() && o->end > o->cur) { pthread_mutex_lock(&g_cs_mu); g_cs.waste += (uint64_t)(o->end - o->cur); pthread_mutex_unlock(&g_cs_mu); } put_hdr(o->cur, OP_JUMP, 16); memcpy(o->cur + 8, &start, 8);
        o->used += 16; }   // the JUMP also occupies guest memory of this recording: count it in used, so the game packing the next recording right behind by used does not overwrite it
    o->cur = start; o->end = end;
}
// Host-supplied segment when the game's own command memory runs out: when the game's out-of-memory callback cannot allocate from its command memory pool, it calls AddCommandMemory with an empty pool (p = NULL, n = 0)
// (opening the advanced stats page on the level 88 sorceress save: many list items in one frame, and the original implementation segfaulted here reading NULL+8). Our records are larger than real-device NVN commands, so the budget the game reserved for real-device usage is not necessarily enough,
// so this is not an error state of the game but something we must catch: provide a segment of host memory to keep recording. The segment is replayed in place at submit and reclaimed after being kept for 8 frames (submit is in the same frame as recording or the next one).
// (Do not use the whole game chunk as the range: SMBW sub-allocates many recordings out of one big chunk, and a later grant inside that chunk freed segments still in use.)
// Reclaim follows the game's command memory chunks, not a frame count: the game caches recorded command handles and CALLs them every frame until it re-records them itself (SMBW's object setup handles);
// once a handle was recorded into a host segment, the old "reclaim 8 frames after lending" rewrote the segment while the game still CALLed it (with overflow forced on the Mac, the replay read garbage commands: RGBA16F used as a depth format for a pipeline).
// A segment hangs off one recording through the JUMP at the end of its guest part: that recording occupies [BEGIN, end of JUMP) in guest memory, and the game only gives those bytes out again once they are dead
// (when it re-records that material in place; packing the next recording right behind by used never touches them, since used includes the JUMP). So a segment remembers that guest range and is reclaimed when the game gives out memory
// overlapping it; segments overflowing from a host segment keep the same range.
// The segment pool grows on demand (SMBW opening course: many small command buffers each overflow once per frame, and the old fixed 64 segments × 1 MB ran out within 8 frames); abort only beyond HOSTSEG_CAP in total.
#define HOSTSEG_SZ (256u << 10)
#define HOSTSEG_CAP (1u << 30)
uint64_t dm_nvn_frame_count(void);
typedef struct { uint8_t *mem; uint64_t frame; int busy; uintptr_t org_lo, org_hi; } hostseg_t;   // org: the guest range of the recording it hangs off (0 = unknown, never reclaimed)
static hostseg_t *g_hostseg; static int g_nhostseg, g_caphostseg;
static pthread_mutex_t g_hostseg_mu = PTHREAD_MUTEX_INITIALIZER;
// Take a segment: origin = the current recording's guest bytes [BEGIN, end of JUMP); if the write pointer is already in a host segment (overflowing again), keep that segment's origin
static uint8_t *hostseg_take(const cmd_t *o) {
    static unsigned total; uint64_t f = dm_nvn_frame_count(); uint8_t *m = NULL;
    uintptr_t lo = 0, hi = 0, cur = o ? (uintptr_t)o->cur : 0;
    pthread_mutex_lock(&g_hostseg_mu);
    int in_seg = 0;
    for (int i = 0; i < g_nhostseg && cur; i++) if (cur >= (uintptr_t)g_hostseg[i].mem && cur <= (uintptr_t)g_hostseg[i].mem + HOSTSEG_SZ) { lo = g_hostseg[i].org_lo; hi = g_hostseg[i].org_hi; in_seg = 1; break; }
    if (!in_seg && o && o->recording && o->cur) { lo = o->recording == 1 && o->begin && (uintptr_t)o->begin <= cur ? (uintptr_t)o->begin : cur; hi = cur + 16; }
    for (int i = 0; i < g_nhostseg && !m; i++)
        if (!g_hostseg[i].busy) { g_hostseg[i] = (hostseg_t){ g_hostseg[i].mem, f, 1, lo, hi }; m = g_hostseg[i].mem; }
    if (!m && (uint64_t)(g_nhostseg + 1) * HOSTSEG_SZ <= HOSTSEG_CAP) {
        if (g_nhostseg == g_caphostseg) { g_caphostseg = g_caphostseg ? g_caphostseg * 2 : 64; g_hostseg = realloc(g_hostseg, sizeof *g_hostseg * (size_t)g_caphostseg); }
        m = malloc(HOSTSEG_SZ);
        g_hostseg[g_nhostseg++] = (hostseg_t){ m, f, 1, lo, hi };
    }
    unsigned t = ++total; int n = g_nhostseg;
    pthread_mutex_unlock(&g_hostseg_mu);
    if (!m) die("NVN command memory: the game's pool is empty and host-lent segments reached %u MB without being reclaimed by the game's blocks", HOSTSEG_CAP >> 20);
    if (t == 1 || t % 1024 == 0) LOG("[nvn] the game's command memory ran short, the host lends a segment (%u times so far, frame %llu, segment pool %d segments × %u KB)", t, (unsigned long long)f, n, HOSTSEG_SZ >> 10);
    return m;
}
// The game gives out [lo, hi): segments whose recording's guest bytes overlap it are dead, reclaim them
static void hostseg_release(uintptr_t lo, uintptr_t hi) {
    pthread_mutex_lock(&g_hostseg_mu);
    for (int i = 0; i < g_nhostseg; i++) if (g_hostseg[i].busy && g_hostseg[i].org_hi > lo && g_hostseg[i].org_lo < hi) g_hostseg[i].busy = 0;
    pthread_mutex_unlock(&g_hostseg_mu);
}
NVN(nvnCommandBufferAddCommandMemory, void, NVNcommandBuffer *c, const NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    const pool_t *pl = (const pool_t *)p;
    if (!pl || n < 64) { if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.hostseg++; pthread_mutex_unlock(&g_cs_mu); } cmd_new_segment(CB(c), hostseg_take(CB(c)), HOSTSEG_SZ); return; }
    if (!pl->mem) { LOG("[RED] AddCommandMemory: pool has no CPU storage"); return; }
    hostseg_release((uintptr_t)(pl->mem + off), (uintptr_t)(pl->mem + off + n));
    if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.grants++; g_cs.grant_bytes += n; if (!g_cs.grant_min || n < g_cs.grant_min) g_cs.grant_min = n; if (n > g_cs.grant_max) g_cs.grant_max = n; pthread_mutex_unlock(&g_cs_mu); }
    { static int cblog = -1; if (cblog < 0) cblog = getenv("DM_NVN_CBLOG") != NULL; static uint64_t ng; if (cblog && (++ng <= 60 || ng % 500 == 0)) LOG("[nvn] AddCommandMemory #%llu cmd=%p pool %p +%#lx length %#zx (recording %d)", (unsigned long long)ng, (void *)c, (const void *)pl, (long)off, n, CB(c)->recording); }
    cmd_new_segment(CB(c), pl->mem + off, n);
    CB(c)->used = 0;   // GetCommandMemoryUsed counts from the start of the memory the game last added, across recordings (see there)
}
NVN(nvnCommandBufferAddControlMemory, void, NVNcommandBuffer *c, void *m, size_t n) { (void)c; (void)m; (void)n; }
// Get space for one record and keep `keep` bytes after it (OP_RESERVE, 0 for the END); if not enough, call back into the game per the NVN convention to add command memory
static uint8_t *cmd_alloc_keep(cmd_t *o, uint32_t size, uint32_t keep) {
    size = (size + 7) & ~7u;
    for (int tries = 0; !o->cur || o->cur + size + keep > o->end; tries++) {
        // The game's callback cannot supply enough (newer-SDK games hand out small chunks sized for real-device commands, or disable the callback and manage memory themselves): continue recording in a host segment instead of aborting
        if (!o->cb || tries == 2) {
            static int once; if (!once++) LOG("[nvn] command memory: the game's callback did not add enough (cb=%p, need %u bytes), using a host-lent segment", (void *)o->cb, size + keep);
            cmd_new_segment(o, hostseg_take(o), HOSTSEG_SZ); break;
        }
        if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.cbs++; pthread_mutex_unlock(&g_cs_mu); }
        { static int cblog = -1; if (cblog < 0) cblog = getenv("DM_NVN_CBLOG") != NULL; static uint64_t ncb;
          if (cblog && (++ncb <= 40 || ncb % 1000 == 0)) LOG("[nvn] command memory callback #%llu cmd=%p need %u (used in this segment %ld)", (unsigned long long)ncb, (void *)o, size + keep, o->cur && o->end ? (long)(o->cur - (o->end - 1)) : -1L); }
        o->cb((NVNcommandBuffer *)o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, size + keep, o->cb_data);
    }
    if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.bytes += size; g_cs.recs++; pthread_mutex_unlock(&g_cs_mu); }
    uint8_t *p = o->cur; o->cur += size; o->used += size; put_hdr(p, 0, size);
    if (o->recording == 2) { o->begin = p; o->recording = 1; }   // The recording's first record is its handle
    return p;
}
static uint8_t *cmd_alloc(cmd_t *o, uint32_t size) { return cmd_alloc_keep(o, size, OP_RESERVE); }
// A recording starts with its first command, as in NVN: no BEGIN record, so an empty recording takes only its END. Games size command memory by GetCommandMemoryUsed
// (nvn_obj.h draw_rec): Breath of the Wild sums one scratch recording per mesh and per shape and render pass into each model's prerecorded commands, and with an
// 8-byte BEGIN on each of several hundred recordings a model came out a 4 KB page over its resource size table entry
NVN(nvnCommandBufferBeginRecording, void, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    if (o->recording) LOG("[RED] BeginRecording re-entered");
    o->recording = 2;   // 2: recording, no command yet (cmd_alloc sets begin)
}
NVN(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    uint8_t *p = cmd_alloc_keep(o, 8, 0); put_hdr(p, OP_END, 8);   // nothing follows: the END may take the segment's last bytes
    o->recording = 0;
    return (NVNcommandHandle)o->begin;
}
NVN(nvnCommandBufferIsRecording, NVNboolean, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->recording != 0; }
NVN(nvnCommandBufferCopyCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {
    for (int i = 0; i < n; i++) { uint8_t *p = cmd_alloc(CB(c), 16); put_hdr(p, OP_CALL, 16); memcpy(p + 8, &h[i], 8); }
}
NVN(nvnCommandBufferSignalEvent, void, NVNcommandBuffer *c, const NVNevent *e, NVNeventSignalMode m, NVNeventSignalLocation loc, int flags, uint32_t v) {
    (void)loc; (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_SIGNAL_EVENT, 24);
    const ev_t *ev = (const ev_t *)e; volatile uint32_t *val = ev->val; uint32_t mm = m;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
// A fence recorded in a command buffer: the replay fences the sync with the submission that reaches this point (OP_FENCE_SYNC). Unreal Engine's NVN RHI fences its command buffers this way (Octopath Traveler 0); Pokémon Legends: Z-A fences once a frame this way and waits on it
NVN(nvnCommandBufferFenceSync, void, NVNcommandBuffer *c, NVNsync *s, NVNsyncCondition cond, int flags) {
    (void)cond; (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 16); put_hdr(p, OP_FENCE_SYNC, 16); memcpy(p + 8, &s, 8);
}
NVN(nvnCommandBufferResetCounter, void, NVNcommandBuffer *c, NVNcounterType t) {
    uint8_t *p = cmd_alloc(CB(c), 16); put_hdr(p, OP_RESET_COUNTER, 16); uint64_t tt = t; memcpy(p + 8, &tt, 8);
}
NVN(nvnCommandBufferReportCounter, void, NVNcommandBuffer *c, NVNcounterType t, NVNbufferAddress a) {
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_REPORT_COUNTER, 24); uint64_t tt = t; memcpy(p + 8, &tt, 8); memcpy(p + 16, &a, 8);
}
NVN(nvnCommandBufferCopyBufferToBuffer, void, NVNcommandBuffer *c, NVNbufferAddress src, NVNbufferAddress dst, size_t n, int flags) {
    (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 32); put_hdr(p, OP_COPY_B2B, 32); uint64_t nn = n;
    memcpy(p + 8, &src, 8); memcpy(p + 16, &dst, 8); memcpy(p + 24, &nn, 8);
}
// uniform update: the data is copied into command memory at record time (NVN semantics: the caller may modify the original data after recording) and written into the buffer at submit
NVN(nvnCommandBufferUpdateUniformBuffer, void, NVNcommandBuffer *c, NVNbufferAddress buf, size_t bufsz, ptrdiff_t off, size_t n, const void *data) {
    (void)bufsz;
    uint32_t sz = (uint32_t)(24 + n);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, OP_UPDATE_UB, (sz + 7) & ~7u);
    uint64_t dst = buf + off, nn = n; memcpy(p + 8, &dst, 8); memcpy(p + 16, &nn, 8); memcpy(p + 24, data, n);
}

// ---------- Render commands: parameters are recorded into command memory in the payload format of nvn_obj.h; for replay see nvn_mtl.m ----------
static void *rec(NVNcommandBuffer *c, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
#define REC(c, OP, T) ((T *)rec((c), (OP), sizeof(T)))
// Binding records with arrays are allocated by the actual count: prefix fields + n elements (the replay side reads only the first n). Previously they were recorded at fixed length for the maximum count (bind vertex buffers 272 bytes, bind textures / samplers 280 bytes,
// while there are usually only 1–2 in practice), and the empty tail took 40–50% of the command bytes per frame; the advanced stats page of the level 88 sorceress recorded 12.5 MB per frame, using up the game's own command memory pool (about 105 segments x 128 KB per frame)
#define RECN(c, OP, T, ARR, N) ((T *)rec((c), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))
// Draws carry the indirect / multi-draw fields (ind onwards) only when they use them: a plain indexed draw record is 0x28 bytes instead of 0x40 (nvn_obj.h draw_rec)
static void rec_draw(NVNcommandBuffer *c, p_draw d) { size_t n = d.ind || d.mdi_cnt ? sizeof d : offsetof(p_draw, ind); memcpy(rec(c, OP_DRAW, n), &d, n); }
static void rec_draw_idx(NVNcommandBuffer *c, p_draw_idx d) { size_t n = d.ind || d.mdi_cnt ? sizeof d : offsetof(p_draw_idx, ind); memcpy(rec(c, OP_DRAW_IDX, n), &d, n); }
static void copy_view(view_t *dst, uint8_t *has, const NVNtextureView *v) { *has = v != NULL; if (v) *dst = *(const view_t *)v; }
// Records with textures snapshot the texture objects (0x80 bytes each, after the record struct) and replay reads the snapshots: an NVN command no longer depends on the API object once recorded, and the game may finalize, free or reuse it before submitting.
// Unity (Engage) does so when it changes scenes; only the object pointer used to be recorded, and replay read other data (a texture 1869367126 high, a Metal assertion abort).
#define SNAP_OFF(T) ((sizeof(T) + 7) & ~(size_t)7)
#define REC_SNAP(c, OP, T, K) ((T *)rec((c), (OP), SNAP_OFF(T) + (size_t)(K) * sizeof(tex_t)))
static const tex_t *tex_snap(void *rec_, size_t off, int k, const NVNtexture *t, uint64_t ra) {
    if (!t) return NULL;
    tex_t *s = (tex_t *)((uint8_t *)rec_ + off) + k;
    memcpy(s, t, sizeof *s);
    // The object is invalid at record time (Unity finalizes and re-initializes the same render target object between recording and submitting): keep the object pointer as before and read it at submit, when it is usually valid again; replay still checks the magic
    if (s->magic != MAGIC('T', 'E', 'X', 0)) {
        static int red;
        if (red++ < 4) {   // the first 4 log the call site and the start of the object, to tell what the game passed
            module_t *m = module_of((void *)ra); const uint64_t *q = (const uint64_t *)t;
            LOG("[RED] texture object invalid while recording (uninitialized or already finalized): %p index %d, call site %s+%#llx, object start %llx %llx %llx %llx %llx %llx; read again at submit", (const void *)t, k,
                m ? m->name : "?", m ? (unsigned long long)(ra - (uint64_t)m->base) : (unsigned long long)ra,
                (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5]);
        }
        return (const tex_t *)t;
    }
    return s;
}
NVN(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *c, int n, const NVNtexture *const *ct, const NVNtextureView *const *cv,
    const NVNtexture *dt, const NVNtextureView *dv) {
    if (n < 0 || n > 8) { LOG("[RED] SetRenderTargets count %d", n); n = n < 0 ? 0 : 8; }
    p_set_rt *r = REC_SNAP(c, OP_SET_RT, p_set_rt, n + 1); r->n = n;
    for (int i = 0; i < n; i++) { r->color[i] = ct ? tex_snap(r, SNAP_OFF(p_set_rt), i, ct[i], (uint64_t)__builtin_return_address(0)) : NULL; copy_view(&r->cview[i], &r->has_cview[i], cv ? cv[i] : NULL); }
    r->depth = tex_snap(r, SNAP_OFF(p_set_rt), n, dt, (uint64_t)__builtin_return_address(0)); copy_view(&r->dview, &r->has_dview, dv);
}
NVN(nvnCommandBufferClearColor, void, NVNcommandBuffer *c, int index, const float *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; memcpy(r->rgba, rgba, 16);
}
// Clears of integer targets (Paper Mario: The Thousand-Year Door): recorded as floats, which both replay paths convert to the attachment's integer type (the load action's MTLClearColor,
// clear_color_tri); exact up to 2^24 in magnitude, and 0xffffffff rounds to 2^32, which converts back to it
NVN(nvnCommandBufferClearColori, void, NVNcommandBuffer *c, int index, const int *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; for (int i = 0; i < 4; i++) r->rgba[i] = (float)rgba[i];
}
NVN(nvnCommandBufferClearColorui, void, NVNcommandBuffer *c, int index, const uint32_t *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; for (int i = 0; i < 4; i++) r->rgba[i] = (float)rgba[i];
}
NVN(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *c, float depth, uint32_t dmask, int stencil, int smask) {
    p_clear_ds *r = REC(c, OP_CLEAR_DS, p_clear_ds); *r = (p_clear_ds){depth, (int)B(dmask), stencil, smask};
}
NVN(nvnCommandBufferSetViewports, void, NVNcommandBuffer *c, int first, int n, const float *v) {
    if (n < 0 || first + n > 16) { LOG("[RED] SetViewports first=%d n=%d", first, n); return; }
    p_viewports *r = REC(c, OP_VIEWPORTS, p_viewports); r->first = first; r->n = n;
    for (int i = 0; i < n; i++) memcpy(r->v[i], v + 4 * i, 16);
}
NVN(nvnCommandBufferSetScissor, void, NVNcommandBuffer *c, int x, int y, int w, int h) { *REC(c, OP_SCISSOR, p_scissor) = (p_scissor){x, y, w, h}; }
NVN(nvnCommandBufferSetDepthRange, void, NVNcommandBuffer *c, float n, float f) { *REC(c, OP_DEPTH_RANGE, p_depth_range) = (p_depth_range){n, f}; }
NVN(nvnCommandBufferBindProgram, void, NVNcommandBuffer *c, const NVNprogram *p, int stages) {
    *REC(c, OP_BIND_PROGRAM, p_bind_program) = (p_bind_program){(const prog_t *)p, stages};
}
static void bind_state(NVNcommandBuffer *c, int kind, const void *st, size_t n) { p_bind_state *r = REC(c, OP_BIND_STATE, p_bind_state); r->kind = kind; memcpy(r->bytes, st, n); }
NVN(nvnCommandBufferBindBlendState, void, NVNcommandBuffer *c, const NVNblendState *st) { bind_state(c, 0, st, sizeof(blend_st)); }
NVN(nvnCommandBufferBindColorState, void, NVNcommandBuffer *c, const NVNcolorState *st) { bind_state(c, 1, st, sizeof(color_st)); }
NVN(nvnCommandBufferBindChannelMaskState, void, NVNcommandBuffer *c, const NVNchannelMaskState *st) { bind_state(c, 2, st, sizeof(chmask_st)); }
NVN(nvnCommandBufferBindMultisampleState, void, NVNcommandBuffer *c, const NVNmultisampleState *st) { bind_state(c, 3, st, sizeof(ms_st)); }
NVN(nvnCommandBufferBindPolygonState, void, NVNcommandBuffer *c, const NVNpolygonState *st) { bind_state(c, 4, st, sizeof(poly_st)); }
NVN(nvnCommandBufferBindDepthStencilState, void, NVNcommandBuffer *c, const NVNdepthStencilState *st) { bind_state(c, 5, st, sizeof(ds_st)); }
NVN(nvnCommandBufferBindVertexAttribState, void, NVNcommandBuffer *c, int n, const NVNvertexAttribState *st) {
    if (n < 0 || n > 16) { LOG("[RED] BindVertexAttribState n=%d", n); return; }
    p_bind_vattr *r = RECN(c, OP_BIND_VATTR, p_bind_vattr, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 4 * n);
}
NVN(nvnCommandBufferBindVertexStreamState, void, NVNcommandBuffer *c, int n, const NVNvertexStreamState *st) {
    if (n < 0 || n > 16) { LOG("[RED] BindVertexStreamState n=%d", n); return; }
    p_bind_vstream *r = RECN(c, OP_BIND_VSTREAM, p_bind_vstream, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 8 * n);
}
NVN(nvnCommandBufferBindVertexBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *rg) {
    if (n < 0 || first + n > 16) { LOG("[RED] BindVertexBuffers first=%d n=%d", first, n); return; }
    p_bind_vbuf *r = RECN(c, OP_BIND_VBUF, p_bind_vbuf, r, n); r->first = first; r->n = n; memcpy(r->r, rg, sizeof *rg * n);
}
NVN(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {
    *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, i, a, n};
}
NVN(nvnCommandBufferBindUniformBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) {
    for (int i = 0; i < n; i++) *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size};
}
NVN(nvnCommandBufferBindStorageBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {
    *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, i, a, n};
}
NVN(nvnCommandBufferBindStorageBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) {
    for (int i = 0; i < n; i++) *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size};
}
static p_bind_handles *bind_handles(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) {
    if (n < 0 || n > 32) { LOG("[RED] bind handle count %d", n); return NULL; }
    p_bind_handles *r = RECN(c, op, p_bind_handles, h, n); r->stage = st; r->first = first; r->n = n; r->defer = 0; r->src = 0; memcpy(r->h, h, 8 * n);
    {   // this command buffer may have bound a pool other than the first: move its IDs to that pool's part of the table (handle: texture ID in bits 0-19, sampler ID from bit 20)
        int tb = CB(c)->tex_base, sb = CB(c)->samp_base;
        for (int i = 0; i < n && (tb || sb); i++) {
            uint64_t v = r->h[i], tid = v & 0xfffff, sid = v >> 20;
            if (op == OP_BIND_SAMP) { if (sid) r->h[i] = (sid + (uint64_t)sb) << 20; }
            else { if (tid) tid += (uint64_t)tb; if (sid && op == OP_BIND_TEX) sid += (uint64_t)sb; r->h[i] = tid | sid << 20; }
        }
    }
    return r;
}
// Image (writable texture): handle = texture pool index (GetImageHandle returns it as-is; RegisterImage writes the same table as RegisterTexture)
NVN(nvnCommandBufferBindImages, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNimageHandle *h) {
    bind_handles(c, OP_BIND_IMG, st, first, n, (const uint64_t *)h);
}
// Separate texture binds: on hardware they fill their own handle table (c2 0x168 + 8 slot), apart from BindTexture's, so they go to slots 128 + n (NVN_SEPTEX_SLOT0)
NVN(nvnCommandBufferBindSeparateTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateTextureHandle *h) {
    bind_handles(c, OP_BIND_TEX, st, NVN_SEPTEX_SLOT0 + first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateTextureHandle h) { uint64_t v; memcpy(&v, &h, 8); bind_handles(c, OP_BIND_TEX, st, NVN_SEPTEX_SLOT0 + i, 1, &v); }   // single (Engage): as the plural version
NVN(nvnCommandBufferBindSeparateSamplers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateSamplerHandle *h) {
    bind_handles(c, OP_BIND_SAMP, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateSampler, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateSamplerHandle h) {
    bind_handles(c, OP_BIND_SAMP, st, i, 1, &h.value);
}
NVN(nvnCommandBufferSetStencilValueMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, m, 0, 0, 0}; }
NVN(nvnCommandBufferSetStencilMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, 0, m, 0, 1}; }
NVN(nvnCommandBufferSetStencilRef, void, NVNcommandBuffer *c, NVNface f, int v) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, 0, 0, v, 2}; }
NVN(nvnCommandBufferSetPolygonOffsetClamp, void, NVNcommandBuffer *c, float fa, float un, float cl) {
    *REC(c, OP_POLY_OFFSET, p_poly_offset) = (p_poly_offset){fa, un, cl};
}
NVN(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n) { rec_draw(c, (p_draw){pr, first, n, 1, 0, 0}); }
// Indirect: the {count, instances, first, base instance} record at ind is usually written by the GPU earlier in the frame (Hyrule Warriors: Age of Calamity's first battle), so the replay
// issues a Metal indirect draw from the pool buffer instead of reading it at record or replay time (nvn_mtl.m draw)
NVN(nvnCommandBufferDrawArraysIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNbufferAddress ind) { rec_draw(c, (p_draw){pr, 0, 0, 1, 0, ind}); }
NVN(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib,
    int bv, int bi, int inst) {
    rec_draw_idx(c, (p_draw_idx){pr, it, n, bv, bi, inst, ib});
}
NVN(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *c, int x, int y, int z) {
    // The thunk is a tail jump (br), so the return address here is the call site in the game
    *REC(c, OP_DISPATCH, p_dispatch) = (p_dispatch){x, y, z, (uint64_t)__builtin_return_address(0)};
}
// Indirect: the three group counts at ind are read by the GPU when the dispatch runs (nvn_mtl.m dispatch)
NVN(nvnCommandBufferDispatchComputeIndirect, void, NVNcommandBuffer *c, NVNbufferAddress ind) {
    *REC(c, OP_DISPATCH, p_dispatch) = (p_dispatch){0, 0, 0, (uint64_t)__builtin_return_address(0), ind};
}
NVN(nvnCommandBufferBarrier, void, NVNcommandBuffer *c, int bits) { *(int32_t *)rec(c, OP_BARRIER, 8) = bits; }
NVN(nvnCommandBufferSetTexturePool, void, NVNcommandBuffer *c, const NVNtexturePool *p) { REC(c, OP_SET_TEX_POOL, p_set_pool)->pool = (const dpool_t *)p; CB(c)->tex_base = dpool_base(g_tpb, g_ntpb, p); }
NVN(nvnCommandBufferSetSamplerPool, void, NVNcommandBuffer *c, const NVNsamplerPool *p) { REC(c, OP_SET_SAMP_POOL, p_set_pool)->pool = (const dpool_t *)p; CB(c)->samp_base = dpool_base(g_spb, g_nspb, p); }

NVN(nvnCommandBufferCopyBufferToTexture, void, NVNcommandBuffer *c, NVNbufferAddress src, const NVNtexture *t, const NVNtextureView *v,
    const NVNcopyRegion *r, int flags) {
    p_copy_b2t *o = REC_SNAP(c, OP_COPY_B2T, p_copy_b2t, 1); o->src = src; o->tex = tex_snap(o, SNAP_OFF(p_copy_b2t), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
NVN(nvnCommandBufferCopyTextureToBuffer, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r,
    NVNbufferAddress dst, int flags) {
    p_copy_b2t *o = REC_SNAP(c, OP_COPY_T2B, p_copy_b2t, 1); o->src = dst; o->tex = tex_snap(o, SNAP_OFF(p_copy_b2t), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
// Clear a texture region with the given value (float / unsigned integer versions; mask = channel write mask)
NVN(nvnCommandBufferClearTexture, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const float *rgba, int mask) {
    p_clear_tex *o = REC_SNAP(c, OP_CLEAR_TEX, p_clear_tex, 1); o->tex = tex_snap(o, SNAP_OFF(p_clear_tex), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 0; memcpy(o->c.f, rgba, 16);
}
NVN(nvnCommandBufferClearTextureui, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const uint32_t *rgba, int mask) {
    p_clear_tex *o = REC_SNAP(c, OP_CLEAR_TEX, p_clear_tex, 1); o->tex = tex_snap(o, SNAP_OFF(p_clear_tex), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 1; memcpy(o->c.u, rgba, 16);
}
NVN(nvnCommandBufferCopyTextureToTexture, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNcopyRegion *sr,
    const NVNtexture *d, const NVNtextureView *dv, const NVNcopyRegion *dr, int flags) {
    p_copy_t2t *o = REC_SNAP(c, OP_COPY_T2T, p_copy_t2t, 2); o->src = tex_snap(o, SNAP_OFF(p_copy_t2t), 0, s, (uint64_t)__builtin_return_address(0)); o->dst = tex_snap(o, SNAP_OFF(p_copy_t2t), 1, d, (uint64_t)__builtin_return_address(0));
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv); o->sr = *sr; o->dr = *dr; o->flags = flags;
}
NVN(nvnCommandBufferSetCopyRowStride, void, NVNcommandBuffer *c, ptrdiff_t st) { p_copy_stride *o = REC(c, OP_COPY_STRIDE, p_copy_stride); o->stride = st; o->image = 0; }
NVN(nvnCommandBufferSetCopyImageStride, void, NVNcommandBuffer *c, ptrdiff_t st) { p_copy_stride *o = REC(c, OP_COPY_STRIDE, p_copy_stride); o->stride = st; o->image = 1; }   // Link's Awakening
NVN(nvnCommandBufferSetBlendColor, void, NVNcommandBuffer *c, const float *v) { memcpy(REC(c, OP_BLEND_COLOR, p_blend_color)->rgba, v, 16); }

// ---------- State objects: packed in place (nvn_obj.h) ----------
#define ST(T, x) ((T *)(x))
NVN(nvnBlendStateSetDefaults, void, NVNblendState *s) { *ST(blend_st, s) = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD}; }
NVN(nvnBlendStateSetBlendTarget, void, NVNblendState *s, int t) { ST(blend_st, s)->target = t; }
NVN(nvnBlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
NVN(nvnBlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) { ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea; }
// Advanced blend modes (NV_blend_equation_advanced: multiply, screen, overlay, ...) have no Metal blend state; mode 0 (none, which Luigi's Mansion 3 sets on every blend state) is the default
NVN(nvnBlendStateSetAdvancedMode, void, NVNblendState *s, NVNblendAdvancedMode mode) {
    (void)s; static int once; if (mode && !once++) LOG("[RED] nvnBlendStateSetAdvancedMode %d: advanced blend modes are not implemented, the state blends with its blend functions", mode);
}
NVN(nvnColorStateSetDefaults, void, NVNcolorState *s) { *ST(color_st, s) = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; }
NVN(nvnColorStateSetBlendEnable, void, NVNcolorState *s, int i, uint32_t en) {
    color_st *o = ST(color_st, s); if (i < 0 || i > 7) return;
    o->blend_en = B(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
NVN(nvnChannelMaskStateSetDefaults, void, NVNchannelMaskState *s) { ST(chmask_st, s)->mask = 0xffffffffu; }
NVN(nvnChannelMaskStateSetChannelMask, void, NVNchannelMaskState *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (i < 0 || i > 7) return;
    uint32_t m = (B(r) ? 1 : 0) | (B(g) ? 2 : 0) | (B(b) ? 4 : 0) | (B(a) ? 8 : 0);
    chmask_st *o = ST(chmask_st, s); o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NVN(nvnMultisampleStateSetDefaults, void, NVNmultisampleState *s) { memset(s, 0, sizeof(ms_st)); ST(ms_st, s)->enable = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->enable = B(e) != 0; }
NVN(nvnMultisampleStateSetSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocationsEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_en = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocations, void, NVNmultisampleState *s, int start, int n, const float *v) { (void)s; (void)start; (void)n; (void)v; }
NVN(nvnMultisampleStateGetSamples, int, const NVNmultisampleState *s) { return ((const ms_st *)s)->samples; }
NVN(nvnMultisampleStateGetMultisampleEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->enable; }
NVN(nvnPolygonStateSetDefaults, void, NVNpolygonState *s) { *ST(poly_st, s) = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; }
NVN(nvnPolygonStateSetCullFace, void, NVNpolygonState *s, NVNface f) { ST(poly_st, s)->cull = f; }
NVN(nvnPolygonStateSetFrontFace, void, NVNpolygonState *s, NVNfrontFace f) { ST(poly_st, s)->front = f; }
NVN(nvnPolygonStateSetPolygonMode, void, NVNpolygonState *s, NVNpolygonMode m) { ST(poly_st, s)->mode = m; }
NVN(nvnPolygonStateSetPolygonOffsetEnables, void, NVNpolygonState *s, int e) { ST(poly_st, s)->offset_en = e; }
NVN(nvnDepthStencilStateSetDefaults, void, NVNdepthStencilState *s) {
    *ST(ds_st, s) = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}
NVN(nvnDepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dt = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dw = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthFunc, void, NVNdepthStencilState *s, NVNdepthFunc f) { ST(ds_st, s)->df = f; }
NVN(nvnDepthStencilStateSetStencilTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->st = B(e) != 0; }
NVN(nvnDepthStencilStateSetStencilFunc, void, NVNdepthStencilState *s, NVNface f, NVNstencilFunc fn) {
    ds_st *o = ST(ds_st, s); if (f & NVN_FACE_FRONT) o->sf_f = fn; if (f & NVN_FACE_BACK) o->sf_b = fn;
}
NVN(nvnDepthStencilStateSetStencilOp, void, NVNdepthStencilState *s, NVNface f, NVNstencilOp fail, NVNstencilOp zfail, NVNstencilOp zpass) {
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) { o->fail_f = fail; o->zfail_f = zfail; o->zpass_f = zpass; }
    if (f & NVN_FACE_BACK) { o->fail_b = fail; o->zfail_b = zfail; o->zpass_b = zpass; }
}
NVN(nvnVertexAttribStateSetDefaults, void, NVNvertexAttribState *s) { *ST(va_st, s) = (va_st){0, 0, 0}; }
NVN(nvnVertexAttribStateSetFormat, void, NVNvertexAttribState *s, NVNformat f, ptrdiff_t off) {
    if (off < 0 || off >= (1 << 19)) LOG("[RED] vertex attribute offset %td exceeds the packed range", off);
    ST(va_st, s)->fmt = f; ST(va_st, s)->offset = (uint32_t)off;
}
NVN(nvnVertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { ST(va_st, s)->stream = i; }
NVN(nvnVertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { *ST(vs_st, s) = (vs_st){0, 0}; }
NVN(nvnVertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { ST(vs_st, s)->stride = (uint32_t)st; }
NVN(nvnVertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { ST(vs_st, s)->divisor = d; }
// Getters: Monster Hunter Rise reads its stream states back when it builds a draw (about 24,000 calls in its character creation)
NVN(nvnVertexStreamStateGetStride, ptrdiff_t, const NVNvertexStreamState *s) { return (ptrdiff_t)((const vs_st *)s)->stride; }
NVN(nvnVertexStreamStateGetDivisor, int, const NVNvertexStreamState *s) { return ((const vs_st *)s)->divisor; }
NVN(nvnVertexAttribStateGetFormat, void, const NVNvertexAttribState *s, NVNformat *f, ptrdiff_t *off) { const va_st *o = (const va_st *)s; if (f) *f = (NVNformat)o->fmt; if (off) *off = (ptrdiff_t)o->offset; }
NVN(nvnVertexAttribStateGetStreamIndex, int, const NVNvertexAttribState *s) { return ((const va_st *)s)->stream; }

static _Atomic uint64_t g_sub_wall_ns, g_sub_cpu_ns, g_sub_instr;   // Wall clock / this thread's CPU time / this thread's retired instruction count for submit (excluding present) (taken away per frame)
static _Atomic uint64_t g_cum_sub_instr, g_cum_sub_cpu_ns;   // Cumulative versions of the above (the "translation" summary row every 300 frames takes the difference; on a real device read through the log port: instructions per draw / submit CPU per frame)
extern int thread_selfcounts(int type, void *buf, size_t nbytes);   // This thread's cumulative {instructions, cycles} (libSystem private interface, present on both Mac and iOS)
static uint64_t thr_instr(void) { uint64_t c[2] = {0, 0}; thread_selfcounts(1, c, sizeof c); return c[0]; }
// Supplementary readings for the submit path (on a real device Metal 4 instruction count is -18% while submit CPU time is +49%; need to tell apart kernel mode, execution efficiency, or frequency throttling):
// this thread's cycle count (the second item of the same interface), kernel-mode CPU time (THREAD_BASIC_INFO), wall clock
static void thr_counts(uint64_t *instr, uint64_t *cyc) { uint64_t c[2] = {0, 0}; thread_selfcounts(1, c, sizeof c); *instr = c[0]; *cyc = c[1]; }
static uint64_t thr_sys_ns(void) {
    thread_basic_info_data_t bi; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT; mach_port_t me = mach_thread_self(); uint64_t r = 0;
    if (thread_info(me, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) == KERN_SUCCESS) r = (uint64_t)bi.system_time.seconds * 1000000000ull + (uint64_t)bi.system_time.microseconds * 1000ull;
    mach_port_deallocate(mach_task_self(), me); return r;
}
static _Atomic uint64_t g_cum_sub_cyc, g_cum_sub_sys_ns, g_cum_sub_wall_ns;
static int frame_rec(void);
static _Atomic uint64_t g_nvn_ns;   // Host time spent in submit / present this frame (replay encoding + present), used by the timing instrumentation

// ---------- Lifecycle: dm_gfx_suspend (called before iOS goes to the background; equivalent to Switch suspend) ----------
// on=1: subsequent submits / presents block until on=0; before returning, wait for in-progress submit / present calls to exit and for all in-flight GPU work to complete (iOS requires no GPU work in the background).
static pthread_mutex_t g_susp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_susp_cv = PTHREAD_COND_INITIALIZER;
static int g_susp, g_inflight_api;
__thread _Atomic int dm_host_work;   // see hle.h (defined here: the graphics unit tests compile this file but not hle_thread.c)
static void gfx_enter(void) { pthread_mutex_lock(&g_susp_mu); while (g_susp) pthread_cond_wait(&g_susp_cv, &g_susp_mu); g_inflight_api++; pthread_mutex_unlock(&g_susp_mu); dm_host_work++; }
static void gfx_leave(void) { dm_host_work--; pthread_mutex_lock(&g_susp_mu); g_inflight_api--; pthread_cond_broadcast(&g_susp_cv); pthread_mutex_unlock(&g_susp_mu); }
void dm_gfx_enter(void) { gfx_enter(); }   // the same gate for the Vulkan layer's submits and presents (dm/gfx/vk)
void dm_gfx_leave(void) { gfx_leave(); }
__attribute__((weak)) void dm_vk_drain(void) {}   // dm/gfx/vk/hle_vk.m: wait for the Vulkan layer's queue (the graphics unit tests do not link it)
__attribute__((weak)) void dm_gl_drain(void) {}   // dm/gfx/gl/hle_gl.m: the same for the GL layer
void dm_gfx_suspend(int on) {
    pthread_mutex_lock(&g_susp_mu);
    g_susp = on != 0;
    if (on) while (g_inflight_api) pthread_cond_wait(&g_susp_cv, &g_susp_mu);
    else pthread_cond_broadcast(&g_susp_cv);
    pthread_mutex_unlock(&g_susp_mu);
    if (on) { dm_vk_drain(); dm_gl_drain(); mtl_drain(); LOG("[nvn] graphics suspended: in-flight GPU work drained"); } else LOG("[nvn] graphics resumed");   // the Vulkan and GL queues first: presents on the NVN queue wait for their frames
}
// One submission at a time: the Metal replay keeps one command buffer and one upload batch for the submission in progress (nvn_mtl.m g_cb). Fire Emblem: Three Houses initializes two queues and
// submits on both from two threads; the second submission arrived while the first was replaying and the host stopped with "submit re-entered" at the ninth frame. NVN orders work on different queues only
// through syncs, so replaying the two queues' submissions in lock order is one valid order
static pthread_mutex_t g_sub_mu = PTHREAD_MUTEX_INITIALIZER;
NVN(nvnQueueSubmitCommands, void, NVNqueue *q, int n, const NVNcommandHandle *h) {
    gfx_enter();
    pthread_mutex_lock(&g_sub_mu);
    uint64_t t0 = now_ns(), c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID), i0, y0, s0 = thr_sys_ns(); thr_counts(&i0, &y0);
    ((queue_t *)q)->submits++;
    mtl_sub_begin();
    nvn_ft_poll();
    mtl_sub_index((const uint64_t *)h, n);
    for (int i = 0; i < n; i++) { mtl_exec_rest((const uint64_t *)h + i + 1, n - i - 1); mtl_exec((const uint8_t *)h[i]); }
    mtl_exec_rest(NULL, 0);
    mtl_sub_commit();
    pthread_mutex_unlock(&g_sub_mu);
    uint64_t dw = now_ns() - t0;
    atomic_fetch_add(&g_nvn_ns, dw); atomic_fetch_add(&g_sub_wall_ns, dw);
    uint64_t i1, y1; thr_counts(&i1, &y1); uint64_t dc = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c0, di = i1 - i0;
    atomic_fetch_add(&g_cum_sub_cyc, y1 - y0); atomic_fetch_add(&g_cum_sub_sys_ns, thr_sys_ns() - s0); atomic_fetch_add(&g_cum_sub_wall_ns, dw);
    atomic_fetch_add(&g_sub_cpu_ns, dc); atomic_fetch_add(&g_cum_sub_cpu_ns, dc);
    atomic_fetch_add(&g_sub_instr, di); atomic_fetch_add(&g_cum_sub_instr, di);
    gfx_leave();
}

// Commands that only affect GPU state / only execute on the GPU: the null device only counts them (the thunk has already counted)
static void nvn_nop(void) {}
static const char *const g_nops[] = {
    "nvnCommandBufferSetPatchSize",
    "nvnCommandBufferSetDepthBounds",
    "nvnCommandBufferWaitSync",
    "nvnCommandBufferSetShaderScratchMemory",
    "nvnCommandBufferPushDebugGroupDynamic",
    "nvnCommandBufferPopDebugGroup",
    "nvnCommandBufferInsertDebugMarkerDynamic",
    "nvnCommandBufferInsertDebugMarker",   // 13 Sentinels
    "nvnCommandBufferPushDebugGroup",      // Super Mario 3D World + Bowser's Fury (NVN 53)
    "nvnCommandBufferPushDebugGroupStatic",
    "nvnCommandBufferPopDebugGroupId",
    "nvnCommandBufferInsertDebugMarkerStatic",
    "nvnCommandBufferWaitEvent",
    "nvnWindowFinalize",
};
__attribute__((constructor)) static void nvn_nops_register(void) {
    // Empty implementations overwrite the unimplemented stubs after the slot table is initialized; a misspelled name (not in main's name table) reports a failure and aborts at startup
    nvn_init_slots();
    for (unsigned k = 0; k < sizeof g_nops / sizeof *g_nops; k++) {
        unsigned i = 0;
        while (i < NVN_NFUNC && strcmp(nvn_names[i], g_nops[k])) i++;
        if (i == NVN_NFUNC) die("NVN empty implementation %s is not in main's name table", g_nops[k]);
        if (nvn_slot[i].fn != (void *)nvn_unimpl_entry) die("NVN %s has an implementation and is also listed as an empty implementation", g_nops[k]);
        nvn_slot[i].fn = (void *)nvn_nop;
    }
    unsigned nnop = sizeof g_nops / sizeof *g_nops;
    // Implemented functions other than command recording (nvnCommandBuffer*, tens of thousands per frame, where the host only appends records) run on the big host stack: submit / present / object creation do Metal work, and a game thread's stack may be too small (see nvn_entry.S nvn_big_entry)
    unsigned nbig = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (nvn_slot[i].fn != (void *)nvn_unimpl_entry && nvn_slot[i].fn != (void *)nvn_nop && strncmp(nvn_names[i], "nvnCommandBuffer", 16)) {
            nvn_big_real[i] = nvn_slot[i].fn; nvn_slot[i].fn = (void *)nvn_big_entry; nbig++;
        }
    // DM_NVN_PROF=1: implemented functions (except unimplemented stubs and present: present includes the display-link wait and is counted separately) get a timing wrapper, used to break down the time spent in the "translation layer recording phase"
    if (getenv("DM_NVN_PROF"))
        for (unsigned i = 0; i < NVN_NFUNC; i++)
            if (nvn_slot[i].fn != (void *)nvn_unimpl_entry && strcmp(nvn_names[i], "nvnQueuePresentTexture")) { nvn_real[i] = nvn_slot[i].fn; nvn_slot[i].fn = (void *)nvn_prof_entry; }
    LOG("[nvn] null device: %u names, %u implemented (%u semantic + %u empty, %u of them on the large host stack), the other %u go to the red-reporting stub",
        NVN_NFUNC, g_nsem + nnop, g_nsem, nnop, nbig, NVN_NFUNC - g_nsem - nnop);
}

// ---------- Window / present ----------
#define WB(b) ((win_builder_t *)(b))
NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder *b) { NVNdevice *dev = WB(b)->dev; memset(b, 0, sizeof(win_builder_t)); WB(b)->dev = dev; WB(b)->interval = 1; }
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder *b, NVNdevice *d) { WB(b)->dev = d; }
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder *b, NVNnativeWindow w) { WB(b)->native = w; }
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder *b, int n, NVNtexture *const *t) {
    if (n < 0 || n > 4) { LOG("[RED] WindowBuilderSetTextures count %d", n); n = n < 0 ? 0 : 4; }
    WB(b)->ntex = n; for (int i = 0; i < n; i++) WB(b)->tex[i] = t[i];
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder *b, int v) { WB(b)->interval = v; }
NVN(nvnWindowInitialize, NVNboolean, NVNwindow *w, const NVNwindowBuilder *b) {
    const win_builder_t *s = (const win_builder_t *)b; win_t *o = (win_t *)w;
    memset(o, 0, sizeof *o); o->magic = MAGIC('W', 'I', 'N', 'D'); o->native = s->native; o->ntex = s->ntex; o->interval = s->interval;
    memcpy(o->tex, s->tex, sizeof o->tex);
    for (int i = 0; i < o->ntex; i++) if (o->tex[i]) mtl_xv_window_tex(CTX(o->tex[i])->serial);
    LOG("[nvn] WindowInitialize swap chain %d textures, interval %d, first %dx%d format %#x", o->ntex, o->interval,
        o->ntex ? (int)CTX(o->tex[0])->d.width : 0, o->ntex ? (int)CTX(o->tex[0])->d.height : 0, o->ntex ? CTX(o->tex[0])->d.format : 0);
    return 1;
}
NVN(nvnWindowGetPresentInterval, int, const NVNwindow *w) { return ((const win_t *)w)->interval; }   // Link's Awakening reads it back every frame
NVN(nvnWindowSetPresentInterval, void, NVNwindow *w, int v) { if (((win_t *)w)->interval != v) LOG("[nvn] the game sets present interval %d → %d", ((win_t *)w)->interval, v); ((win_t *)w)->interval = v; }
NVN(nvnWindowSetCrop, void, NVNwindow *w, int x, int y, int ww, int hh) {
    win_t *o = (win_t *)w; int v[4] = {x, y, ww, hh};
    if (memcmp(o->crop, v, sizeof v)) LOG("[nvn] WindowCrop %d,%d %dx%d", x, y, ww, hh);
    memcpy(o->crop, v, sizeof v);
}
NVN(nvnWindowGetCrop, void, const NVNwindow *w, NVNrectangle *r) {   // Bayonetta 3 reads it back every few frames; before any SetCrop it is the whole first texture, as NVN starts it
    const win_t *o = (const win_t *)w;
    if (o->crop[2] > 0 && o->crop[3] > 0) { r->x = o->crop[0]; r->y = o->crop[1]; r->width = o->crop[2]; r->height = o->crop[3]; return; }
    r->x = r->y = 0; r->width = o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.width : 0; r->height = o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.height : 0;
}
static void trow(uint64_t f, const char *name, double v);
static void set_time_constraint(uint64_t period_ns);
static void rt_comp_tick(uint64_t period_ns);
static double g_rt_comp_ms;   // Currently declared real-time computation amount (ms; 0 = not declared)
static double g_rt_period_ms;
static void sleep_until_ns(uint64_t dl);
static uint64_t present_period(const win_t *o);
static uint64_t rt_period_ns(const win_t *o, uint64_t period);
static int present_itv(const win_t *o);
__attribute__((weak)) void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = 0; *ns = 0; }   // The host's hle_thread.c provides the ground truth; 0 in graphics unit tests
__attribute__((weak)) uint32_t dm_guest_work_qos(unsigned *count) { if (count) *count = 0; return QOS_CLASS_UNSPECIFIED; }
__attribute__((weak)) void dm_yield_top(char *out, size_t n) { if (n) snprintf(out, n, "-"); }   // hle_thread.c: the threads that yielded most since the last call; none in graphics unit tests
__attribute__((weak)) void dm_thread_cpu(char *out, size_t n, double secs) { (void)secs; if (n) snprintf(out, n, "-"); }   // hle_thread.c: guest-thread CPU by Switch priority band since the last call
void mtl_res_stats(char *out, size_t n);
void mtl_barrier_stats(uint64_t *skip, uint64_t *full);
extern uint32_t mtl_present_qos(void);
static uint64_t g_frame;
static uint64_t g_acq_wait_ns, g_pq_wait_ns;   // Accumulated display-link wait / wait for the present queue to return inside acquire, recorded per frame at present (there can be several acquires within one frame)
uint64_t dm_nvn_frame_count(void) { return g_frame; }
extern uint64_t g_t0_ns;   // Host process start point (main.c; same base as the UIMSG timestamps)
NVN(nvnWindowAcquireTexture, NVNwindowAcquireTextureResult, NVNwindow *w, NVNsync *s, int *idx) {
    win_t *o = (win_t *)w;
    *idx = o->ntex ? o->next++ % o->ntex : 0;
    // Vsync blocks here (real-device NVN: when all textures in the swapchain are waiting for display, wait for the next one to be released), not in present -- the game often calls present while holding a lock
    uint64_t at = *idx < 4 ? o->avail[*idx] : 0, now = dm_detclock_enabled() ? guest_ns() : now_ns(), wait = at > now ? at - now : 0;
    if (!dm_detclock_enabled()) {   // Register the acquire thread as a time-constraint thread (no longer registered only when it "arrived early": it must also declare itself when it is constantly behind in the hot state)
        static uint64_t rt_period; uint64_t period = present_period(o), rtp = rt_period_ns(o, period);
        if (rtp && rt_period != rtp) { rt_period = rtp; set_time_constraint(rtp); }
        rt_comp_tick(rtp);
        if (mtl_link_active()) { if (!mtl_link_wait_tick(present_itv(o), period ? 2 * period : 33333333ull) && wait) sleep_until_ns(at); }   // DM_PRESENT_LINK: pacing fully follows the display link (wait for the present interval's display-link ticks when running fast, go immediately when behind); our own 60 Hz clock is only the fallback when no display-link tick arrives
        else if (wait) sleep_until_ns(at);
    } else {   // DM_DETCLOCK_PACE=1 (used by the screenshot gate): while the guest clock advances per frame, each frame's wall clock is no shorter than one step -- frame count and in-game time are deterministic, and the background loading thread has at least one step of wall clock available per frame;
               // When the machine is fully loaded frames only get slower and the background only gets more wall clock, so the picture captured at "the Nth frame after the message" is independent of load
        static int pace = -1; static uint64_t step, last;
        if (pace < 0) { const char *e = getenv("DM_DETCLOCK_PACE"), *d = getenv("DM_DETCLOCK"); step = d ? strtoull(d, 0, 0) : 0; pace = e && atoi(e) && step; }
        if (pace) { if (last && now_ns() < last + step) sleep_until_ns(last + step); last = now_ns(); }
    }
    g_acq_wait_ns += wait; trow(g_frame, "@late_ms", wait ? (now_ns() - at) / 1e6 : 0);
    // The previous present of this texture has not yet been processed by the present thread (display cannot keep up, a whole swapchain round behind): wait for it to be returned, then hand it to the game to draw
    uint64_t tk = *idx < 4 ? o->ticket[*idx] : 0;
    if (tk) { uint64_t w0 = now_ns(); mtl_present_wait(tk); g_pq_wait_ns += now_ns() - w0; }
    if (s) { ((sync_t *)s)->fenced = 1; ((sync_t *)s)->fence = mtl_last_fence(); }   // The swapchain texture becomes available once the commits before this point have completed
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
// The older API (NVN 53, Super Mario 3D World + Bowser's Fury): the queue waits for the texture on the GPU and no sync object is handed out. The host replays in submission order, so later commands already come after earlier presents; the same as WindowAcquireTexture without a sync object
NVN(nvnQueueAcquireTexture, NVNqueueAcquireTextureResult, NVNqueue *q, NVNwindow *w, int *idx) {
    (void)q; return h_nvnWindowAcquireTexture(w, NULL, idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS ? (NVNqueueAcquireTextureResult)0 : (NVNqueueAcquireTextureResult)1;
}

// Frame boundary: on each present, print / write to disk the call count of each NVN function for this frame
static uint64_t g_prev[NVN_NFUNC];
static FILE *g_tsv;
static void dump_frame(void) {
    static int inited, nframes, calls = 1;
    if (!inited) {
        inited = 1;
        const char *e = getenv("DM_NVN_FRAMES"); if (e) nframes = atoi(e);
        const char *c = getenv("DM_NVN_TSV_CALLS"); if (c) calls = atoi(c);   // 0 = write only the @ timing rows (pulling files from a real device has a size limit, and the call-count rows take about 2/3)
        const char *t = getenv("DM_NVN_TSV"); if (t && !(g_tsv = fopen(t, "w"))) LOG("[RED] cannot open %s", t);
        if (g_tsv) fprintf(g_tsv, "# kind: nvn-per-frame  value = call count when name is an NVN function name; names starting with @ are timing probes (ms / count)\nframe\tname\tvalue\n");
    }
    uint64_t total = 0; unsigned nfn = 0;
    char line[8192]; size_t o = 0; line[0] = 0;
    for (unsigned i = 0; (g_flag.bootprof || g_tsv || nframes > 0) && i < NVN_NFUNC; i++) {
        uint64_t c = atomic_load(&nvn_slot[i].cnt), d = c - g_prev[i];
        g_prev[i] = c;
        if (!d) continue;
        total += d; nfn++;
        if (g_tsv && calls) fprintf(g_tsv, "%llu\t%s\t%llu\n", g_frame, nvn_names[i], d);
        if (g_frame <= (uint64_t)nframes && o < sizeof line - 96) o += snprintf(line + o, sizeof line - o, " %s=%llu", nvn_names[i] + 3, d);
    }
    if (g_tsv) fflush(g_tsv);
    if (nframes > 0 && g_frame <= (uint64_t)nframes) LOG("[nvn] %s %llu: %u functions %llu calls |%s", g_frame ? "frame" : "init", g_frame, nfn, total, line);
    else if ((g_flag.bootprof || g_tsv) && g_frame % 300 == 0) LOG("[nvn] frame %llu: %u functions %llu calls", g_frame, nfn, total);
    g_frame++; cs_dump();
    // Every 300 frames read resident memory and hand it to the graphics layer to print the memory readout line (mtl_mem_stat, low frequency, capped). Memory watchdog: when resident memory exceeds the cap, log a failure and exit on its own. On by default only on the Mac host (24 GB: runaway host memory
    // drags the whole machine into a kernel panic, which happened twice with other programs on the same machine), or set for tests with DM_MEM_CAP_GB=<integer>; the player's iOS app has no cap (exiting before the system kills it is pointless and only adds another kind of crash)
    if (g_frame % 300 == 0) { task_vm_info_data_t vi; mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS) { unsigned long long mb = (unsigned long long)(vi.phys_footprint >> 20); void mtl_mem_stat(unsigned long long, unsigned long long); if (g_flag.bootprof) mtl_mem_stat(g_frame, mb);
            static long cap = -1; if (cap < 0) { const char *e = getenv("DM_MEM_CAP_GB"); cap = e ? atol(e) : DM_MEM_CAP_DEFAULT_GB; }
            if (cap > 0 && mb > (unsigned long long)cap * 1024) { LOG("[RED] memory watchdog: resident %llu MB over the limit of %ld GB (frame %llu), exiting", mb, cap, (unsigned long long)g_frame); _exit(86); } } }
    dm_guest_frame_tick();   // Deterministic guest clock (DM_DETCLOCK, host hle_os.c): each present advances a fixed step; a no-op when not enabled
}
// ---------- Timing instrumentation (per-frame rows written to DM_NVN_TSV: frame / @metric / value; every 300 frames a summary of p50 / p95 / max) ----------
#define TWIN 300
static double g_tw[6][TWIN]; static unsigned g_tn[6];
static double g_pres_iv[TWIN]; static unsigned g_presn;   // Present interval (ms; -1 = dropped by the system, 0 = no reading)
static double g_pres_exp[TWIN], g_pres_exp_last;   // how long that frame should have stayed on screen (ms, pres_expect_ms); the last one for slow_emit
// The requested hold in milliseconds for each present, by present number. On-screen intervals are read 8 presents late, so each is judged against the holds of its own presents:
// with the frame step at 2 vsyncs every frame stays 33.3 ms, which is not a dropped frame
static double g_hold_ring[16];
static double pres_expect_ms(uint64_t n) {   // the interval between presents n - 1 and n shows frame n - 1: the display link holds it for its own present's hold, the Mac pacer spaces present n by n's, so the larger of the two
    double k = g_hold_ring[n % 16] > g_hold_ring[(n - 1) % 16] ? g_hold_ring[n % 16] : g_hold_ring[(n - 1) % 16];
    double held = k > 0 ? k : 1000.0 / mtl_fps_target(), tgt = 1000.0 / mtl_fps_target();   // at least one frame period of the target (33.3 ms when the game itself asks for 30)
    return held > tgt ? held : tgt;
}
__attribute__((weak)) double dm_brightness_now(void) { return -1; }
__attribute__((weak)) void dm_perf_present(double iv_ms) { (void)iv_ms; }   // Default (unit tests); strong definition in the host's diag.c: always-on performance record perf.txt
uint64_t dm_nvn_sub_cpu_ns(void) { return atomic_load(&g_cum_sub_cpu_ns); }   // Cumulative CPU nanoseconds of submit (excluding present)
__attribute__((weak)) int dm_thermal_now(void) { return -1; }   // Default (Mac); overridden by iOS front_ios.m   // default (Mac); the strong definition in the iOS front end front_ios.m overrides it
__attribute__((weak)) int dm_low_power_now(void) { return 0; }   // Low Power Mode (iOS front_ios.m); off on the Mac
// Per-frame ring for the long-frame log (GPU time only arrives 8 frames late, so store first and judge later)
#define LFN 16
typedef struct { uint64_t f; double fr, nv, t_s, make_ms, up_mb, up_ms; unsigned pso, lib, up, rup; } lf_rec;
static lf_rec g_lf[LFN];
__attribute__((weak)) void dm_longframe_note(const char *what) { LOG("[longframe] %s", what); }   // Default (graphics unit tests); the strong definition in the host's diag.c adds scene context and writes perf.txt
extern void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns); extern uint64_t mtl_upload_bytes(void); extern void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads);
static const char *const g_tname[6] = {"frame time", "frame work", "NVN submit+present", "GPU", "translation layer recording", "translation layer submit"};
static void tpush(int k, double v) { g_tw[k][g_tn[k]++ % TWIN] = v; }
// Thermal attribution (iOS DM_HEAT): per-frame GPU milliseconds are kept in a separate ring, taking the mean / p95 "since the last read" (at most the last HRN frames), without interfering with the timing summary's 300-frame window
#define HRN 1024
static double g_hr[HRN]; static uint64_t g_hn, g_hn_read;
static int dcmp(const void *a, const void *b);
void nvn_gpu_recent(double *avg, double *p95, unsigned *n) {
    uint64_t hi = g_hn, lo = g_hn_read > hi ? hi : g_hn_read; if (hi - lo > HRN) lo = hi - HRN; g_hn_read = hi;
    unsigned c = (unsigned)(hi - lo); static double v[HRN]; double s = 0;
    for (unsigned i = 0; i < c; i++) { v[i] = g_hr[(lo + i) % HRN]; s += v[i]; }
    qsort(v, c, sizeof *v, dcmp); *n = c; *avg = c ? s / c : -1; *p95 = c ? v[(c * 95) / 100 < c ? (c * 95) / 100 : c - 1] : -1;
}
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
// Power over the window since the last summary, for the whole process: CPU energy (task_energy in nJ, Apple silicon), CPU time (100% = one core) and the share of it on performance cores,
// wakeups per second (interrupt / out of platform idle), GPU time per second (task_gpu_utilisation), thermal state, and the guest threads that yielded most (hle_thread.c);
// then render-target traffic per frame (attachment load / store bytes, passes that load and store everything, stores saved by DM_NVN_UNWRITTEN_DC) and guest-thread CPU by Switch priority
static void power_line(uint64_t frame) {
    static uint64_t pt, pe, pcpu, pp, piw, ppw, pg, pf, pld, pst, pfu, pudc; static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    task_power_info_v2_data_t pw; mach_msg_type_number_t n = TASK_POWER_INFO_V2_COUNT;
    if (task_info(mach_task_self(), TASK_POWER_INFO_V2, (task_info_t)&pw, &n) != KERN_SUCCESS) return;
    uint64_t t = now_ns(), cpu = (pw.cpu_energy.total_user + pw.cpu_energy.total_system) * tb.numer / tb.denom, p = pw.task_ptime * tb.numer / tb.denom;
#if defined(__arm64__)
    uint64_t e = pw.task_energy;
#else
    uint64_t e = 0;
#endif
    uint64_t iw = pw.cpu_energy.task_interrupt_wakeups, idw = pw.cpu_energy.task_platform_idle_wakeups, g = pw.gpu_energy.task_gpu_utilisation;
    if (pt && t > pt) { double s = (t - pt) / 1e9; char yt[192]; dm_yield_top(yt, sizeof yt);
        double nf = frame > pf ? (double)(frame - pf) : 1;   // per frame: the fair comparison when one build holds the frame rate better than another
        LOG("[nvn] power up to frame %llu: CPU energy %.2f W, %.1f mJ per frame (%.1f fps); CPU %.0f%% (100 = one core), %.0f%% of it on performance cores; wakeups %.0f/s (out of platform idle %.0f); GPU %.1f ms/s; thermal %d; top yielders: %s", frame,
            (e - pe) / 1e9 / s, (e - pe) / 1e6 / nf, nf / s, (cpu - pcpu) / 1e7 / s, cpu > pcpu ? 100.0 * (double)(p - pp) / (double)(cpu - pcpu) : 0, (iw - piw) / s, (idw - ppw) / s, (g - pg) / 1e6 / s, dm_thermal_now(), yt);
        char rs[400]; mtl_res_stats(rs, sizeof rs); static uint64_t pbs, pbf; uint64_t bs, bf; mtl_barrier_stats(&bs, &bf);
        uint64_t mtl_cut_count(void), mtl_copy_cut_count(void), mtl_blit_splits(void); void mtl_ubd_stats(uint64_t *, uint64_t *); static uint64_t pcut, pccut, pbsp, pud, puc; uint64_t cut = mtl_cut_count(), ccut = mtl_copy_cut_count(), bsp = mtl_blit_splits(), ud, uc; mtl_ubd_stats(&ud, &uc);
        void mtl_ubp_stats(uint64_t *, uint64_t *, uint64_t *); static uint64_t ppm, ppr, ppf; uint64_t pm, pr, pf_; mtl_ubp_stats(&pm, &pr, &pf_);
        void mtl_lc_stats(uint64_t *, uint64_t *); static uint64_t plt, pll; uint64_t lt, ll; mtl_lc_stats(&lt, &ll);
        void mtl_ubp_why(uint64_t w[5]); static uint64_t pw5[5]; uint64_t w5[5]; mtl_ubp_why(w5);
        uint64_t ld, st, fu, udc = mtl_udc_saved_bytes(); mtl_bw(&ld, &st, &fu);
        LOG("[nvn] residency set up to frame %llu: %s; draw-pass entry barriers full %llu / saved %llu; split submits %llu (%llu after buffer → texture copies); passes broken by copies / buffer updates %llu; in-pass uniform updates deferred %llu, passes ended to land them for a read %llu, partly covered blocks read from memory %llu (cached %llu, refused %llu: other GPU writes %llu, earlier submission running %llu, copy or fill queued %llu, size %llu, unreadable %llu); waiting clears taken by a later pass %llu, landed in passes of their own %llu; "
            "attachments per frame load %.0f MB / store %.0f MB, load-all store-all passes %.0f, saved by not storing unwritten attachments %.1f MB", frame, rs, (unsigned long long)(bf - pbf), (unsigned long long)(bs - pbs),
            (unsigned long long)(cut - pcut), (unsigned long long)(ccut - pccut), (unsigned long long)(bsp - pbsp), (unsigned long long)(ud - pud), (unsigned long long)(uc - puc), (unsigned long long)(pm - ppm), (unsigned long long)(pr - ppr), (unsigned long long)(pf_ - ppf), (unsigned long long)(w5[1] - pw5[1]), (unsigned long long)(w5[2] - pw5[2]), (unsigned long long)(w5[3] - pw5[3]), (unsigned long long)(w5[0] - pw5[0]), (unsigned long long)(w5[4] - pw5[4]), (unsigned long long)(lt - plt), (unsigned long long)(ll - pll),
            (ld - pld) / 1048576.0 / nf, (st - pst) / 1048576.0 / nf, (fu - pfu) / nf, (udc - pudc) / 1048576.0 / nf); pbs = bs; pbf = bf; pcut = cut; pccut = ccut; pbsp = bsp; pud = ud; puc = uc; ppm = pm; ppr = pr; ppf = pf_; memcpy(pw5, w5, sizeof pw5); plt = lt; pll = ll;
        char tc[400]; dm_thread_cpu(tc, sizeof tc, s);
        LOG("[nvn] threads up to frame %llu: CPU %s", frame, tc); }
    else { char yt[192], rs[400], tc[400]; dm_yield_top(yt, sizeof yt); mtl_res_stats(rs, sizeof rs); dm_thread_cpu(tc, sizeof tc, 1); }   // the first call only takes the baselines
    pt = t; pe = e; pcpu = cpu; pp = p; piw = iw; ppw = idw; pg = g; pf = frame; mtl_bw(&pld, &pst, &pfu); pudc = mtl_udc_saved_bytes();
}
static void tsummary(uint64_t frame) {
    char line[1024]; size_t o = 0;
    for (int k = 0; k < 6; k++) {
        unsigned n = g_tn[k] < TWIN ? g_tn[k] : TWIN; if (!n) continue;
        double v[TWIN]; memcpy(v, g_tw[k], n * sizeof *v); qsort(v, n, sizeof *v, dcmp);
        o += snprintf(line + o, sizeof line - o, " %s p50 %.2f / p95 %.2f / max %.2f ms (%u frames);", g_tname[k], v[n / 2], v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1], v[n - 1], n);
    }
    LOG("[nvn] timing up to frame %llu:%s", frame, line);
    double tgt = 1000.0 / mtl_fps_target();   // milliseconds one frame should take: 16.7 at 60 fps, 33.3 when the game itself asks for 30 (the over-time / dropped-frame counts below use it)
    { unsigned n = g_tn[0] < TWIN ? g_tn[0] : TWIN, over = 0; double v[TWIN];   // frame interval gets its own line (M1 baseline): number of frames longer than one frame period in this window and p99
      if (n) { memcpy(v, g_tw[0], n * sizeof *v); qsort(v, n, sizeof *v, dcmp); for (unsigned i = 0; i < n; i++) over += v[i] > tgt;
        LOG("[nvn] frame interval up to frame %llu: >%.1fms %u / %u frames, p99 %.2f ms", frame, tgt, over, n, v[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1]); } }
    { unsigned n = g_presn < TWIN ? g_presn : TWIN, k = 0, drop = 0, discard = 0; double v[TWIN];   // on-screen present: dropped frame = on screen longer than its hold × 1.5 (pres_expect_ms) or discarded
      for (unsigned i = 0; i < n; i++) { double x = g_pres_iv[i]; if (x < 0) { drop++; discard++; } else if (x > 0) { v[k++] = x; drop += x > g_pres_exp[i] * 1.5; } }
      task_vm_info_data_t vi; mach_msg_type_number_t vc = TASK_VM_INFO_COUNT; double fp = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &vc) == KERN_SUCCESS ? vi.phys_footprint / 1048576.0 : -1;
      double br = dm_brightness_now();   // footprint / brightness are carried on the on-screen present row: does not depend on the tick-entry observation gate (camp runs have it too)
      if (k) { qsort(v, k, sizeof *v, dcmp); LOG("[nvn] on-screen up to frame %llu: %u frames, dropped %u (discarded %u), p99 %.2f ms; footprint %.0f MiB; brightness %.2f; thermal %d", frame, k + discard, drop, discard, v[(k * 99) / 100 < k ? (k * 99) / 100 : k - 1], fp, br, dm_thermal_now()); }
      g_presn = 0; }
    power_line(frame);
    { static uint64_t prev[4], pf; uint64_t v[4]; mtl_vbuf_stats(&v[0], &v[1], &v[2], &v[3]);
      if (pf && frame > pf) { double nf = (double)(frame - pf);
          LOG("[nvn] vertex bindings up to frame %llu: per frame address lookups %.0f / reused %.0f, vertex table writes %.0f / skipped %.0f", frame,
              (v[0] - prev[0]) / nf, (v[1] - prev[1]) / nf, (v[2] - prev[2]) / nf, (v[3] - prev[3]) / nf); }
      memcpy(prev, v, sizeof prev); pf = frame; }
    { static uint64_t pi, pc, pd, pf; uint64_t ci = atomic_load(&g_cum_sub_instr), cc = atomic_load(&g_cum_sub_cpu_ns), ps_, dr_; mtl_stats(&ps_, &dr_);   // translation layer
      if (pf && frame > pf && dr_ > pd) LOG("[nvn] translation up to frame %llu: %.0f instructions per draw, submit per frame %.2f M instructions / CPU %.3f ms, %.0f draws per frame", frame,
          (double)(ci - pi) / (double)(dr_ - pd), (ci - pi) / 1e6 / (double)(frame - pf), (cc - pc) / 1e6 / (double)(frame - pf), (double)(dr_ - pd) / (double)(frame - pf));
      { static uint64_t py, pk, pw; uint64_t cy = atomic_load(&g_cum_sub_cyc), ck = atomic_load(&g_cum_sub_sys_ns), cw = atomic_load(&g_cum_sub_wall_ns);
        if (pf && frame > pf && ci > pi && cy > py) { double nf = (double)(frame - pf), cyc = (double)(cy - py), cpu = (double)(cc - pc);
          qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0; pthread_get_qos_class_np(pthread_self(), &qc, &rp);
          uint64_t aw = 0, ag = 0; int an = 0; void mtl_alloc_stats(uint64_t *, uint64_t *, int *); mtl_alloc_stats(&aw, &ag, &an);
          static uint64_t pyn, pyns; uint64_t yn = 0, yns = 0; dm_main_yield_stats(&yn, &yns);
          LOG("[nvn] translation detail up to frame %llu: submit per frame %.2f M cycles (%.2f cycles per instruction), CPU %.3f ms of which kernel %.3f ms, wall clock %.3f ms; mean clock %.2f GHz (cycles / CPU time); this thread's QoS %#x; allocator pools %d, GPU waits so far %llu; real-time declared computation %.2f ms; main thread yields per frame %.0f / %.3f ms", frame,
              cyc / 1e6 / nf, cyc / (double)(ci - pi), cpu / 1e6 / nf, (double)(ck - pk) / 1e6 / nf, (double)(cw - pw) / 1e6 / nf, cpu > 0 ? cyc / cpu : 0, (unsigned)qc, an, (unsigned long long)aw, g_rt_comp_ms, (double)(yn - pyn) / nf, (double)(yns - pyns) / 1e6 / nf); pyn = yn; pyns = yns; }
        if (frame % 300 == 0) { unsigned wn = 0; uint32_t wq = dm_guest_work_qos(&wn); thread_extended_info_data_t xi; mach_msg_type_number_t xc = THREAD_EXTENDED_INFO_COUNT; int okx = thread_info(pthread_mach_thread_np(pthread_self()), THREAD_EXTENDED_INFO, (thread_info_t)&xi, &xc) == KERN_SUCCESS;
            thread_time_constraint_policy_data_t tp; mach_msg_type_number_t tc = THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t def = 0; int rt_live = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tp, &tc, &def) == KERN_SUCCESS && !def;   // def = 1: the real-time policy is not in effect (never set / revoked)
            LOG("[nvn] thread policy: main thread real-time computation %.2f ms / period %.2f ms, real-time policy %s, current priority %d (base %d, policy %d); %u worker threads QoS %#x; present QoS %#x", g_rt_comp_ms, g_rt_period_ms, rt_live ? "in effect" : "not in effect", okx ? xi.pth_curpri : -1, okx ? xi.pth_priority : -1, okx ? xi.pth_policy : -1, wn, wq, mtl_present_qos()); }
        py = cy; pk = ck; pw = cw; }
      pi = ci; pc = cc; pd = dr_; pf = frame; }
    uint64_t up, re, fa, pr; mtl_pkg_stats(&up, &re); nvn_dirty_stats(&fa, &pr);
    LOG("[nvn] packaged textures: first uploads %llu, re-uploads on pool content change %llu; write-protection faults %llu, pages unprotected before host writes %llu; missed %llu (cumulative)", up, re, fa, pr, mtl_pkg_miss());
    memset(g_tn, 0, sizeof g_tn);
}
// DM_SLOWFRAME=1 (test switch, read over the log port on a real device): per-frame timings are first stored in a ring of the last 16 frames; dropped frames are judged 8 frames late (GPU / on-screen present times are back by then),
// and only for dropped frames is one line "[slowframe]\t<frame number>\t@<name>=<value>..." written to the log, plus one denominator line every 300 frames. No per-frame file is written: the log tee fsyncs every line, and writing line by line would disturb the readings
#define SRN 16
static struct { uint64_t f; int n; const char *k[64]; double v[64]; } g_sr[SRN];
static int g_slow = -1;
static int slow_on(void) { if (g_slow < 0) g_slow = getenv("DM_SLOWFRAME") != NULL; return g_slow; }
static int frame_rec(void) { return g_tsv || slow_on(); }
static void trow(uint64_t f, const char *name, double v) {
    if (g_tsv) fprintf(g_tsv, "%llu\t%s\t%.3f\n", f, name, v);
    if (slow_on()) { __typeof__(g_sr[0]) *r = &g_sr[f % SRN]; if (r->f != f) { r->f = f; r->n = 0; } if (r->n < 64) { r->k[r->n] = name; r->v[r->n++] = v; } }
}
static double srow(uint64_t f, const char *name, double dflt) {
    __typeof__(g_sr[0]) *r = &g_sr[f % SRN];
    if (r->f == f) for (int i = 0; i < r->n; i++) if (!strcmp(r->k[i], name)) return r->v[i];
    return dflt;
}
// Dropped-frame definition: on-screen present interval > the frame's hold (pres_expect_ms: 16.7 ms at 60 fps, 33.3 ms at 30 or with the frame step at 2 vsyncs) ×1.5 or −1; without a window (no on-screen present time) fall back to the present call interval
static void slow_emit(uint64_t f) {
    static uint64_t judged, slow;
    double pm = srow(f, "@presented_ms", 0), fr = srow(f, "@frame_ms", 0), lim = (g_pres_exp_last > 0 ? g_pres_exp_last : 1000.0 / mtl_fps_target()) * 1.5;   // called right after that frame's interval was recorded
    int has = mtl_has_window(), bad = has ? (pm > lim || pm < 0) : fr > lim;
    if (has ? pm != 0 : fr != 0) judged++;
    if (bad) {
        slow++;
        char b[4096]; size_t o = snprintf(b, sizeof b, "[slowframe]\t%llu", f);
        __typeof__(g_sr[0]) *r = &g_sr[f % SRN];
        for (int i = 0; i < r->n && o < sizeof b - 64; i++) o += snprintf(b + o, sizeof b - o, "\t%s=%.3f", r->k[i], r->v[i]);
        double m = 0; for (uint64_t p = f >= 3 ? f - 3 : 0; p < f; p++) { double x = srow(p, "@frame_ms", 0); if (x > m) m = x; }
        snprintf(b + o, sizeof b - o, "\t@prev3_max_frame_ms=%.3f", m);
        fprintf(stderr, "%s\n", b);
    }
    if (f % 300 == 0) fprintf(stderr, "[slowframe]\t#\tup to frame %llu\tjudged=%llu\tslow=%llu\tcriterion=%s\n", f, judged, slow, has ? "on-screen interval" : "present call interval (no window)");
}
// Declaration for the time-constraint (real-time) thread: period = present period, constraint = period, computation = the CPU time this thread actually needs per frame.
// The computation used to be hard-coded at 2 ms, while the game main thread (game logic + recording + submit replay) takes about 10 ms per frame when hot - a 5× gap between declared and actual:
// a real-time thread that exceeds its declared computation gets demoted by the system, and the performance controller also sets frequency from the declaration (this declaration is how the system learns the frame deadline and the workload).
// DM_RT_COMP=<ms> fixes the declaration; default auto = every 120 frames take the larger of 1.25 × the p90 and 1.1 × the peak of this thread's per-frame CPU time and re-declare if it differs from the declared value by > 15% (capped at 0.9 × period).
// DM_RT=0 leaves the acquire thread on the standard policy (its QoS only), for power A/B: the real-time declaration is how the performance controller learns the frame deadline, and it may hold the clock up for it
static int rt_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_RT"); v = !(e && !strcmp(e, "0")); if (!v) LOG("[nvn] DM_RT=0: the present thread gets no time-constraint policy (QoS only)"); } return v; }
static void set_time_constraint_comp(uint64_t period_ns, uint64_t comp_ns) {
    if (!rt_on()) return;
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    if (comp_ns < 1000000ull) comp_ns = 1000000ull; if (comp_ns > period_ns * 9 / 10) comp_ns = period_ns * 9 / 10;
    thread_time_constraint_policy_data_t p = {
        .period = (uint32_t)(period_ns * tb.denom / tb.numer), .computation = (uint32_t)(comp_ns * tb.denom / tb.numer),
        .constraint = (uint32_t)(period_ns * tb.denom / tb.numer), .preemptible = 1};
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr == KERN_SUCCESS) { g_rt_comp_ms = comp_ns / 1e6; g_rt_period_ms = period_ns / 1e6; }
    LOG("[nvn] present thread time-constraint policy (period %.3f ms, computation %.3f ms) %s", period_ns / 1e6, comp_ns / 1e6, kr == KERN_SUCCESS ? "set" : "failed to set");
}
static int rt_comp_fixed_ms(void) { static int v = -2; if (v == -2) { const char *e = getenv("DM_RT_COMP"); v = e && *e && strcmp(e, "auto") ? atoi(e) : -1; } return v; }   // −1 = auto
// The fixed value, or in auto mode the computation declared so far (auto's starting value of 2 ms only the first time): a reset to 2 ms at each period change under-declared the thread until the next p90
static void set_time_constraint(uint64_t period_ns) { int f = rt_comp_fixed_ms(); set_time_constraint_comp(period_ns, f > 0 ? (uint64_t)f * 1000000ull : g_rt_comp_ms > 0 ? (uint64_t)(g_rt_comp_ms * 1e6) : 2000000ull); }
// Called every frame at acquire: records this thread's CPU time since the last call; in auto mode re-declares based on p90
static void rt_comp_tick(uint64_t period_ns) {
    if (rt_comp_fixed_ms() > 0 || !period_ns || !g_rt_comp_ms) return;
    static uint64_t last; static double win[120]; static int n;
    uint64_t c = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); if (!last) { last = c; return; }
    win[n++] = (double)(c - last) / 1e6; last = c;
    if (n < 120) return;
    double v[120]; memcpy(v, win, sizeof v); qsort(v, 120, sizeof *v, dcmp); n = 0;
    // Also the window's peak: every frame that ran past the declaration stalled the thread for 30–60 ms. Dark Souls Remastered's render thread needs 1.9–2.0 ms a frame with one or
    // two frames in 120 near 4.4 ms; declared at 2.5 ms (p90 × 1.25) it stalled about once a second, at 30 fps and at 60; declared above the peak it held 60 fps at the same CPU energy
    double p90 = v[107], comp = fmax(p90 * 1.25, v[119] * 1.1), cap = period_ns * 0.9 / 1e6; if (comp > cap) comp = cap; if (comp < 1) comp = 1;
    if (comp > g_rt_comp_ms * 1.15 || comp < g_rt_comp_ms * 0.85) set_time_constraint_comp(period_ns, (uint64_t)(comp * 1e6));
}
static void sleep_until_ns(uint64_t dl) {   // absolute time on the monotonic clock
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    uint64_t now = now_ns(); if (dl <= now) return;
    mach_wait_until(mach_absolute_time() + (dl - now) * tb.denom / tb.numer);
}
// Present is fixed at 60 Hz: the adaptive mechanism stepped down at frame 60 in 24/24 real-device logs and never came back up, so for this phase 60 fps is the hard floor.
// DM_PRESENT_HZ=0 (uncapped frame rate) serves only as an explicit performance profiling switch: some of the game's UI logic counts frames (with the same save, uncapped gives
// 4181 SaveIndicatorPanel entries and capped at 60 fps gives 0, which matches the ground truth), so uncapped is a semantically wrong way to run and must not be used as a criterion / cross-check / default. Unit tests set it directly with nvn_present_config
static int g_hz = -1, g_display_mode;
static _Atomic int g_game_itv = 1;   // when pacing by the game's interval, the interval the game currently sets (in game mode 2 = 30 fps; nvn_game_fps reports it to the graphics layer, which sets the display link from it)
void nvn_present_config(int hz, int display_mode) { g_hz = hz; g_display_mode = display_mode; }
static _Atomic uint64_t g_itv2_since;   // start of the game's current run of intervals >= 2 (now_ns; 0 = it is 1 now)
// The frame rate the game itself asks for: 30 in game mode when the present interval has been >= 2 for a full second (the display link follows at 30 Hz, mtl_fps_target), otherwise 60; back to 60 at once when the interval returns to 1. Intervals of 3 or more (20 fps or less) have no matching display-link mode and run at 30.
// The one-second hold: Mario 1.2.1 picks the interval from the previous frame's time (main+0xcfd24), setting 2 for the frame after a slow one and then 1 again (123 times in two minutes on an iPhone). Following that frame by frame would flap the display-link mode;
//   a single interval-2 frame is instead kept on screen for its own period by the display-link callback, and the next acquire waits as many ticks (mtl_link_wait_tick, nvn_mtl.m), without changing the mode
// AoC's task scheduler owns simulation time; presents only choose a display cadence.
__attribute__((weak)) uint8_t *dm_fps_state;
__attribute__((weak)) void dm_fps_lock_tick(void) {}
int nvn_game_fps(void) {
    int target=aoc_timing_target(); if(target) return target;
    uint64_t since=atomic_load(&g_itv2_since);
    return g_hz>0 && !g_display_mode && since && now_ns()-since>=1000000000ull ? 30 : 60;
}
static void present_cfg(const win_t *o) {
    if (g_hz >= 0) return;
    // The interval mode comes from the game profile (present_interval, default game): whether frames follow the present interval the game sets depends on whether the game's logic counts frames, which differs per game. Environment variables override it, and can override the fixed 60 Hz.
    int scr = mtl_display_hz();
    const char *e = getenv("DM_PRESENT_HZ"); g_hz = e ? atoi(e) : 60;
    if (!g_hz) LOG("[nvn] [WARNING] DM_PRESENT_HZ=0: frame rate uncapped, frame pacing unlike the device, UI logic may drift (for profiling only, not for verdicts / cross-checks)");
    // Cut (intentional deviation from the original game's behavior): with DM_PRESENT_INTERVAL=display the present interval set by the game is not followed and one frame is presented per display-link tick,
    // with a fixed hard floor of 60 fps; the logic stays at 25 ticks, and the client's frame-counted UI logic is unaffected -
    // with the same save and the same input, the UI message streams of the two modes match message by message. Proof (before character select): r9 game / display 60 ×3 / display 120 ×2, 6 groups in total, uicmp consistent
    // (the save indicator is exempted as a timing race), and the timing differences of key messages are within game mode's own noise band; proof after entering a run is still missing, pending action script coverage
    const char *m = getenv("DM_PRESENT_INTERVAL"); g_display_mode = m ? !strcmp(m, "display") : DM_GAME_PRESENT_DISPLAY;
    LOG("[nvn] present tick %d Hz, interval mode %s (%s; present interval set by the game %d; %s)", g_hz, g_display_mode ? "display (one frame per tick, a cut)" : "game (original semantics)",
        m ? "DM_PRESENT_INTERVAL" : "game profile present_interval", o->interval, scr > 0 ? "window, fixed 60 Hz" : "no window, fixed 60 Hz");
}
static int present_itv(const win_t *o) { return aoc_timing_target() ? 1 : g_display_mode ? 1 : o->interval > 0 ? o->interval : 1; }

static uint64_t present_period(const win_t *o) {
    present_cfg(o);
    int aoc = aoc_timing_target();
    if (aoc) return 1000000000ull / (uint64_t)aoc;
    int was = atomic_load(&g_game_itv), want = o->interval > 0 ? o->interval : 1;
    if (want != was) {
        atomic_store(&g_game_itv, want);
        if (want < 2) atomic_store(&g_itv2_since, 0);
        else if (was < 2) atomic_store(&g_itv2_since, now_ns() | 1);   // | 1: the start is never 0
    }
    int itv = present_itv(o), hz = g_hz;
    return hz > 0 ? (uint64_t)itv * 1000000000ull / (uint64_t)hz : 0;
}
static uint64_t rt_period_ns(const win_t *o, uint64_t period) { (void)o; return period; }
NVN(nvnQueuePresentTexture, void, NVNqueue *q, NVNwindow *w, int idx) {
    (void)q; win_t *o = (win_t *)w; o->presents++;
    dm_fps_lock_tick();
    static int previous_target=-1;
    int target=aoc_timing_target();
    if(target!=previous_target) { if (g_flag.bootprof) LOG("[aoc-timing] target %d fps, dynamic %d",target?target:30,aoc_timing_dynamic()); previous_target=target; }
    static uint64_t prev_in, prev_out, prev_pass, prev_draw;
    present_cfg(o);
    {
        // Test switch DM_TEST_PRESENT_LAG=every N presents,milliseconds[,presents in a row][;…]: sleep that long before the Nth, 2Nth… present (and the ones after it in the run), imitating an occasional slow frame on a phone (Mario 1.2.1 then sets present interval 2; checks that the display link holds frames by interval),
        // or with a run of 3 a hitch that steps the frame step up ("1,10;1200,30,3": 10 ms more on every frame, and 30 more on three in a row every 1,200). Not set in the product
        static int lag_k = -1, lag_n[4], lag_ms[4], lag_run[4];
        if (lag_k < 0) {
            const char *e = getenv("DM_TEST_PRESENT_LAG"); lag_k = 0;
            while (e && *e && lag_k < 4) {
                int n = 0, ms = 0, r = 1, c = sscanf(e, "%d,%d,%d", &n, &ms, &r);
                if (c < 2 || n <= 0) break;
                lag_n[lag_k] = n; lag_ms[lag_k] = ms; lag_run[lag_k] = c == 3 && r > 1 ? r : 1;
                LOG("DM_TEST_PRESENT_LAG: every %d presents, sleep an extra %d ms (%d presents in a row)", n, ms, lag_run[lag_k]); lag_k++;
                if ((e = strchr(e, ';'))) e++;
            }
        }
        int lag = 0; for (int i = 0; i < lag_k; i++) if (o->presents % (uint64_t)lag_n[i] < (uint64_t)lag_run[i]) lag += lag_ms[i];
        if (lag) usleep((useconds_t)lag * 1000);
    }
    gfx_enter();
    uint64_t t0 = now_ns();
    if (idx >= 0 && idx < o->ntex && idx < 4) o->ticket[idx] = mtl_present(CTX(o->tex[idx]), present_period(o) / 1e9, o->crop);   // enqueue only, return immediately
    gfx_leave();
    atomic_fetch_add(&g_nvn_ns, now_ns() - t0);
    uint64_t f = g_frame, ps, dr; mtl_stats(&ps, &dr);
    if (nvn_real[0] || nvn_real[1]) {   // DM_NVN_PROF: record phase = time of all implemented functions except submit; submit phase = QueueSubmitCommands
        static unsigned isub = ~0u; static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
        if (isub == ~0u) for (isub = 0; isub < NVN_NFUNC && strcmp(nvn_names[isub], "nvnQueueSubmitCommands"); isub++) {}
        uint64_t rec = 0, sub = 0;
        static uint64_t acc[NVN_NFUNC];
        for (unsigned i = 0; i < NVN_NFUNC; i++) { uint64_t t = atomic_exchange(&nvn_prof_ticks[i], 0); acc[i] += t; if (i == isub) sub = t; else rec += t; }
        if ((g_frame + 1) % TWIN == 0) {   // every 300 frames list the 5 most time-consuming functions
            char line[1024]; size_t o = 0;
            for (int k = 0; k < 5; k++) { unsigned best = 0; for (unsigned i = 1; i < NVN_NFUNC; i++) if (acc[i] > acc[best]) best = i;
                if (!acc[best]) break; o += snprintf(line + o, sizeof line - o, " %s %.2f ms;", nvn_names[best] + 3, acc[best] * tb.numer / tb.denom / 1e6); acc[best] = 0; }
            LOG("[nvn] translation layer top 5 by time (sum over the last 300 frames):%s", line); memset(acc, 0, sizeof acc);
        }
        trow(g_frame, "@rec_ms", rec * tb.numer / tb.denom / 1e6); trow(g_frame, "@submit_ms", sub * tb.numer / tb.denom / 1e6);
        tpush(4, rec * tb.numer / tb.denom / 1e6); tpush(5, sub * tb.numer / tb.denom / 1e6);
    }
    if (prev_in) {
        double fr = (t0 - prev_in) / 1e6, wk = (t0 - prev_out) / 1e6, nv = atomic_exchange(&g_nvn_ns, 0) / 1e6;
        tpush(0, fr); tpush(1, wk); tpush(2, nv);
        trow(f, "@frame_ms", fr); trow(f, "@work_ms", wk); trow(f, "@nvn_ms", nv); trow(f, "@itv", aoc_timing_target() ? 60.0 / aoc_timing_target() : (double)present_itv(o));
        double task_seconds; uint64_t task_updates; aoc_timing_clock(&task_seconds,&task_updates);
        trow(f,"@task_seconds",task_seconds); trow(f,"@task_updates",(double)task_updates);
        double scene_seconds; uint64_t scene_updates; aoc_timing_scene_clock(&scene_seconds,&scene_updates);
        trow(f,"@scene_seconds",scene_seconds); trow(f,"@scene_updates",(double)scene_updates); trow(f,"@target_fps",target?target:30);
        // Slow-frame attribution (taken only when DM_NVN_TSV is on): this frame's present-thread CPU time (−1 if the thread changed), CPU of all threads in the process, acquire wait accumulated per frame, present call time (same base as the GPU / on-screen present times: ms since process start)
        trow(f, "@wait_ms", g_acq_wait_ns / 1e6); trow(f, "@pq_wait_ms", g_pq_wait_ns / 1e6);
        if (frame_rec()) {
            static pthread_t pth; static uint64_t pcpu, pproc;
            uint64_t c = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); pthread_t th = pthread_self();
            trow(f, "@cpu_ms", pcpu && pthread_equal(th, pth) ? (c - pcpu) / 1e6 : -1); pth = th; pcpu = c;
            struct rusage ru; getrusage(RUSAGE_SELF, &ru);
            uint64_t pc = (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000000ull + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000ull;
            if (pproc) trow(f, "@proc_cpu_ms", (pc - pproc) / 1e6); pproc = pc;
            trow(f, "@call_ms", (t0 - g_t0_ns) / 1e6);
        }
        trow(f, "@passes", (double)(ps - prev_pass)); trow(f, "@draws", (double)(dr - prev_draw));
        static uint64_t pl, pst, pf, pudc; uint64_t l, st, fu; mtl_bw(&l, &st, &fu);
        trow(f, "@rt_load_mb", (l - pl) / 1048576.0); trow(f, "@rt_store_mb", (st - pst) / 1048576.0); trow(f, "@pass_full_rw", (double)(fu - pf));
        uint64_t udc = mtl_udc_saved_bytes(); trow(f, "@unwritten_dc_mb", (udc - pudc) / 1048576.0);
        pl = l; pst = st; pf = fu; pudc = udc;
        static uint64_t pae, par; uint64_t ae_, ar_; mtl_args_stats(&ae_, &ar_);
        trow(f, "@args_enc", (double)(ae_ - pae)); trow(f, "@args_reuse", (double)(ar_ - par)); pae = ae_; par = ar_;
        static uint64_t puc, pus; uint64_t uc_, us_; mtl_use_stats(&uc_, &us_);
        static uint64_t pdo; uint64_t do_ = mtl_ds_omit(); trow(f, "@ds_omit", (double)(do_ - pdo)); pdo = do_;
        trow(f, "@use_call", (double)(uc_ - puc)); trow(f, "@use_skip", (double)(us_ - pus)); puc = uc_; pus = us_;
        static uint64_t pfa, pre_; uint64_t fa_, pr_, up_, re_; nvn_dirty_stats(&fa_, &pr_); mtl_pkg_stats(&up_, &re_);
        trow(f, "@faults", (double)(fa_ - pfa)); trow(f, "@reuploads", (double)(re_ - pre_)); pfa = fa_; pre_ = re_;
        static uint64_t pmc, pmp, pmn; uint64_t mc_, mpg_, mn_; nvn_dirty_mp_stats(&mc_, &mpg_, &mn_);
        trow(f, "@mprotect_calls", (double)(mc_ - pmc)); trow(f, "@mprotect_pages", (double)(mpg_ - pmp)); trow(f, "@mprotect_ms", (mn_ - pmn) / 1e6); pmc = mc_; pmp = mpg_; pmn = mn_;
        double up_ms = mtl_upload_ns_take() / 1e6; trow(f, "@upload_ms", up_ms);
        trow(f, "@upload_gpu_ms", mtl_upload_gpu_ns_take() / 1e6);
        // Accounting: number of synchronous creations on the submit thread this frame + a timestamp on the same clock as UIMSG (offline, frames are split into front end / loading / in-run by the loading screen intervals)
        static uint64_t plib, ppso; uint64_t lib_, pso_; mtl_create_stats(&lib_, &pso_);
        trow(f, "@lib_new", (double)(lib_ - plib)); trow(f, "@pso_new", (double)(pso_ - ppso));
        {   // long-frame log: this frame's items are first recorded into the ring; whether to print is decided 8 frames later, once the GPU time has arrived (see below)
            static uint64_t plns, ppns, pup, prup, pub; uint64_t lns, pns, up, rup, ub; mtl_create_ns(&lns, &pns); mtl_pkg_stats(&up, &rup); ub = mtl_upload_bytes();
            lf_rec *r = &g_lf[f % LFN]; r->f = f; r->fr = fr; r->nv = nv; r->t_s = (now_ns() - g_t0_ns) / 1e9; r->pso = (unsigned)(pso_ - ppso); r->lib = (unsigned)(lib_ - plib);
            r->make_ms = ((lns - plns) + (pns - ppns)) / 1e6; r->up = (unsigned)(up - pup); r->rup = (unsigned)(rup - prup); r->up_mb = (ub - pub) / 1048576.0; r->up_ms = up_ms;
            plns = lns; ppns = pns; pup = up; prup = rup; pub = ub;
        }
        plib = lib_; ppso = pso_;
        trow(f, "@t_s", (now_ns() - g_t0_ns) / 1e9);
        trow(f, "@drawable_ms", mtl_drawable_ns_take() / 1e6);
        trow(f, "@sub_wall_ms", atomic_exchange(&g_sub_wall_ns, 0) / 1e6); trow(f, "@sub_cpu_ms", atomic_exchange(&g_sub_cpu_ns, 0) / 1e6); trow(f, "@sub_instr", (double)atomic_exchange(&g_sub_instr, 0));
    } else atomic_store(&g_nvn_ns, 0);
    g_acq_wait_ns = g_pq_wait_ns = 0;
    if (f >= 8) { double g = mtl_gpu_ms(f - 8); if (g >= 0) { tpush(3, g); trow(f - 8, "@gpu_ms", g); g_hr[g_hn++ % HRN] = g; }   // GPU time is taken 8 frames late (already completed)
        { lf_rec *r = &g_lf[(f - 8) % LFN];   // DM_LONGFRAME=1 opts into long-frame logging
          static int on = -1; if (on < 0) { const char *e = getenv("DM_LONGFRAME"); on = e && !strcmp(e, "1"); }
          // "Long" is relative to recent conditions: when running steadily at 30 fps (menus / windowless host) or under sustained overload, every frame is > 20 ms; those are not long frames and must not flood the log - a frame has to be 40% above the sliding mean of the last 16 frames to count
          static double ef = 16.7, eg = 8; int lf = r->fr > 20.0 && r->fr > 1.4 * ef, lg = g > 16.7 && g > 1.3 * eg;
          if (r->f == f - 8) { ef += ((r->fr < 3 * ef ? r->fr : 3 * ef) - ef) / 16; eg += ((g < 3 * eg ? g : 3 * eg) - eg) / 16; }
          if (on && r->f == f - 8 && f - 8 > 0 && (lf || lg)) { char b[512];
              snprintf(b, sizeof b, "frame %llu (%.2f s) whole frame %.1f ms, submit+present %.1f ms, GPU %.1f ms; new pipelines %u + shader libraries %u, %.1f ms in all; texture uploads %u first + %u re-uploads, %.1f MB / %.1f ms",
                       (unsigned long long)r->f, r->t_s, r->fr, r->nv, g, r->pso, r->lib, r->make_ms, r->up, r->rup, r->up_mb, r->up_ms);
              // Visibility: during the time a long frame "comes out late", what stays on screen is the previous frame; if the previous frame was a loading screen (or this frame itself is one), all the player sees is the loading screen staying a bit longer, with no motion interrupted
              dm_longframe_note(b); } }
        uint64_t g0, g1; if (frame_rec() && mtl_gpu_span(f - 8, &g0, &g1)) { trow(f - 8, "@gpu_beg_ms", (g0 - g_t0_ns) / 1e6); trow(f - 8, "@gpu_end_ms", (g1 - g_t0_ns) / 1e6); } }
    double hud_gpu = -1;
    for (uint64_t back=1; aoc_stats_enabled() && back<=8 && back<=f; back++) { hud_gpu=mtl_gpu_ms(f-back); if (hud_gpu>=0) break; }
    if (g_flag.bootprof && f % 300 == 0) {
        static double previous_seconds; static uint64_t previous_ns;
        double seconds; uint64_t updates; aoc_timing_clock(&seconds,&updates);
        if(previous_ns) LOG("[aoc-timing] task clock %.3f s / wall %.3f s; %llu updates total",seconds-previous_seconds,(t0-previous_ns)/1e9,(unsigned long long)updates);
        static double previous_scene;
        double scene_seconds; uint64_t scene_updates; aoc_timing_scene_clock(&scene_seconds,&scene_updates);
        if(previous_ns) LOG("[aoc-timing] scene clock %.3f s / wall %.3f s; %llu scene updates total",scene_seconds-previous_scene,(t0-previous_ns)/1e9,(unsigned long long)scene_updates);
        previous_scene=scene_seconds;
        previous_ns=t0; previous_seconds=seconds;
    }
    aoc_stats_frame(t0, hud_gpu, aoc_timing_target() ? aoc_timing_target() : 30);
    prev_in = t0; prev_pass = ps; prev_draw = dr;
    dump_frame();
    if ((g_flag.bootprof || frame_rec()) && g_frame % TWIN == 0) tsummary(g_frame);
    mtl_set_frame(g_frame);
    // Real-device semantics: present only enqueues and returns immediately; this frame's display time is scheduled by the pacer, and the texture can be acquired again only at "display time + one period" (blocking in AcquireTexture)
    uint64_t period = present_period(o), now = dm_detclock_enabled() ? guest_ns() : now_ns();
    g_hold_ring[o->presents % 16] = period / 1e6;
    nvn_pacer pc = {o->pace};
    uint64_t slot = now + nvn_pace(&pc, now, period);
    o->pace = pc.target;
    if (idx >= 0 && idx < 4) o->avail[idx] = period ? slot + period : 0;
    static uint64_t prev_slot; if (prev_slot && period) trow(f, "@display_ms", (slot - prev_slot) / 1e6); prev_slot = slot;   // display interval between two adjacent frames on screen (this is the one to look at for dropped frames)
    // Actual presentation interval (windowed mode): taken 8 presents late (the presented callback has arrived by then), as the difference between the n-th and the (n−1)-th; −1 = discarded
    if (o->presents > 9) { uint64_t n = o->presents - 8; double a_ = mtl_presented_time(n), b_ = mtl_presented_time(n - 1);
        if (a_ < 0) trow(f - 8, "@presented_ms", -1); else if (a_ > 0 && b_ > 0) trow(f - 8, "@presented_ms", (a_ - b_) * 1e3);
        g_pres_exp[g_presn % TWIN] = g_pres_exp_last = pres_expect_ms(n);
        g_pres_iv[g_presn++ % TWIN] = a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0);
        { double iv_ = a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0); mtl_ub_present(iv_); }   // same-process alternating measurement (DM_NVN_UB) tallies on-screen present intervals per phase; when off it is just one atomic read
        dm_perf_present(a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0));   // on-screen present intervals are also kept in a separate ring; tsummary reports dropped frames every 300 frames (M1 criterion)
        if (a_ > 0) trow(f - 8, "@pres_at_ms", a_ * 1e3 - g_t0_ns / 1e6); }   // on-screen present time (ms since process start; presentedTime shares a time base with mach_absolute_time)
    if (f >= 8 && slow_on()) slow_emit(f - 8);
    prev_out = now_ns();
}

// ---------- State-only interfaces used by newer-SDK games (SMBW): buffer objects, pool / sampler / texture queries, views, color state, device and window ----------
// They only read and write the object itself and record no commands. Where the host has no matching capability (texture channel swizzle, depth / stencil sampling mode, sampler reduction, virtual mapping) the first occurrence logs one line and the call returns normally.
#define ONCE_LOG(...) do { static int once_; if (!once_++) LOG("[nvn] " __VA_ARGS__); } while (0)

// Buffer object = pool + offset + size; the GPU address and the CPU mapping are both the pool's host pointer + offset (same convention as MemoryPoolGetBufferAddress)
typedef struct { NVNdevice *dev; const pool_t *pool; int64_t off; uint64_t size; } buf_builder_t;   FITS(buf_builder_t, NVNbufferBuilder);
typedef struct { uint32_t magic; const pool_t *pool; int64_t off; uint64_t size; } buf_t;             FITS(buf_t, NVNbuffer);
#define BB(b) ((buf_builder_t *)(b))
#define BF(b) ((const buf_t *)(b))
NVN(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) { NVNdevice *d = BB(b)->dev; memset(b, 0, sizeof(buf_builder_t)); BB(b)->dev = d; }
NVN(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *b, NVNdevice *d) { BB(b)->dev = d; }
NVN(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t off, size_t n) { BB(b)->pool = (const pool_t *)p; BB(b)->off = off; BB(b)->size = n; }
NVN(nvnBufferInitialize, NVNboolean, NVNbuffer *buf, const NVNbufferBuilder *b) {
    const buf_builder_t *bb = (const buf_builder_t *)b;
    if (!bb->pool || !bb->pool->mem) { LOG("[RED] BufferInitialize: pool has no storage (pool %p flags %#x offset %#llx size %#llx)", (const void *)bb->pool, bb->pool ? bb->pool->flags : 0, (unsigned long long)bb->off, (unsigned long long)bb->size); return 0; }
    if ((uint64_t)bb->off + bb->size > bb->pool->size) LOG("[RED] BufferInitialize: past the end of the pool (offset %#llx size %#llx pool %#llx)", (unsigned long long)bb->off, (unsigned long long)bb->size, (unsigned long long)bb->pool->size);
    *(buf_t *)buf = (buf_t){MAGIC('B', 'U', 'F', 0), bb->pool, bb->off, bb->size};
    OLOG("BufferInitialize %p pool=%p off=%#llx size=%#llx", (void *)buf, (const void *)bb->pool, (unsigned long long)bb->off, (unsigned long long)bb->size);
    return 1;
}
NVN(nvnBufferFinalize, void, NVNbuffer *buf) { ((buf_t *)buf)->magic = 0; }
NVN(nvnBufferSetDebugLabel, void, NVNbuffer *buf, const char *s) { (void)buf; (void)s; }
NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *buf) { return (NVNbufferAddress)(BF(buf)->pool->mem + BF(buf)->off); }
NVN(nvnBufferMap, void *, const NVNbuffer *buf) { return BF(buf)->pool->mem + BF(buf)->off; }
NVN(nvnBufferGetSize, size_t, const NVNbuffer *buf) { return (size_t)BF(buf)->size; }
NVN(nvnBufferGetMemoryPool, NVNmemoryPool *, const NVNbuffer *buf) { return (NVNmemoryPool *)BF(buf)->pool; }
NVN(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer *buf) { return (ptrdiff_t)BF(buf)->off; }
NVN(nvnBufferFlushMappedRange, void, const NVNbuffer *buf, ptrdiff_t off, size_t n) { (void)buf; (void)off; (void)n; }        // unified memory: nothing to flush
NVN(nvnBufferInvalidateMappedRange, void, const NVNbuffer *buf, ptrdiff_t off, size_t n) { (void)buf; (void)off; (void)n; }

// Memory pool queries
NVN(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p) { return (NVNmemoryPoolFlags)((const pool_t *)p)->flags; }
NVN(nvnMemoryPoolMap, void *, const NVNmemoryPool *p) { return ((const pool_t *)p)->mem; }
NVN(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *p) { return (size_t)((const pool_t *)p)->size; }
NVN(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }
NVN(nvnMemoryPoolSetDebugLabel, void, NVNmemoryPool *p, const char *s) { (void)p; (void)s; }
// Debug labels: only for NVN debugging tools, unused by the host
NVN(nvnWindowSetDebugLabel, void, NVNwindow *w, const char *s) { (void)w; (void)s; }
NVN(nvnTexturePoolSetDebugLabel, void, NVNtexturePool *p, const char *s) { (void)p; (void)s; }
NVN(nvnSamplerPoolSetDebugLabel, void, NVNsamplerPool *p, const char *s) { (void)p; (void)s; }
NVN(nvnQueueSetDebugLabel, void, NVNqueue *q, const char *s) { (void)q; (void)s; }
NVN(nvnProgramSetDebugLabel, void, NVNprogram *p, const char *s) { (void)p; (void)s; }
NVN(nvnCommandBufferSetDebugLabel, void, NVNcommandBuffer *c, const char *s) { (void)c; (void)s; }
// Virtual pool (flags 0x200): a GPU address range with no storage; MapVirtual maps pages of physical pools into it. A request is 0x28 bytes: {physical pool, physical offset, virtual offset, size, storage class}.
// Host approach: if every request can be described by one "base" (same physical pool, same physical offset - virtual offset), the virtual pool's storage points straight at that memory of the physical pool -- the same semantics as one set of physical pages on the real device,
//   and buffer addresses / Metal buffer lookups land on the block the physical pool already registered. SMBW has a single 128 MB virtual pool, mapped whole by one request onto one physical pool at offset 0.
//   Inconsistent mappings (true sparse) are not implemented: reported red, returns failure. It used to always fail: the game treated the pool as unusable, a buffer later carved from it got a null pool, and MemoryPoolGetFlags read NULL+4 and crashed
typedef struct { NVNmemoryPool *phys; int64_t poff, voff; uint64_t size; int32_t storage_class, _pad; } map_req;
_Static_assert(sizeof(map_req) == 0x28, "NVNmappingRequest");
// True sparse virtual pools (Monster Hunter Rise: RE Engine's GPU heap is one 64 GB virtual pool, built from separately allocated 4 MB physical pools mapped at offsets of their own; boot maps 45 of them
// over the first 180 MB). A virtual pool larger than the whole application pool cannot be one alias of a physical pool, so at Initialize it gets an address range of its own (reserved, inaccessible),
// and every mapping request vm_remaps the physical pool's pages over its part of that range: the virtual addresses then read and write the same memory as the physical pool, as the GPU page table does
// on hardware, and buffer / texture addresses stay CPU addresses. Each run of mapped pages is registered with the replay as a pool of its own (mtl_pool_add), rebuilt when a request joins or splits runs.
// Unmapping puts inaccessible pages back. The reservation is the pool's size, at most 64 GB on the Mac and 1 GB on iOS.
// NVN maps at 4 KB, the host aliases at its 16 KB page: a request that is not 16 KB aligned (Monster Hunter Rise's ASTC 6x6 textures, whose mip tail starts 0x81000 into the texture),
// or one beyond the reservation, cannot be aliased. So every request is also kept in a page table at 4 KB (the physical memory behind each virtual page, and the mapping generation
// that set it), and the host reads textures of a sparse pool through it (nvn_sparse_copy): such a texture is uploaded from what the table points at, with unmapped pages read as 0,
// and uploaded again only when a mapping in its range changes or the CPU writes a physical page behind it (nvn_sparse_since; the physical pools are dirty-tracked). The game fills
// its sparse textures (flag 0x20, 32 GB into the pool) that way: it writes the tiles on the CPU into 4 MB physical pools, then maps them; no GPU copy writes them
enum { NVPOOL = 4, NVRUN = 1024, VPT_LEAF = 512 };   // a page-table leaf covers 2 MB
typedef struct { const uint8_t *phys; uint64_t gen; } vpte;
static struct vpool { const pool_t *pool; uint8_t *base; uint64_t len, size, top; int nrun; struct { uint64_t lo, hi; } run[NVRUN]; vpte **pt; } g_vp[NVPOOL];   // len: reserved; size: the pool's; top: highest mapped end so far (logged per 128 MB)
static pthread_mutex_t g_vp_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_vp_gen;   // bumped per mapping request
static struct vpool *vpool_of(const pool_t *o) { for (int i = 0; i < NVPOOL; i++) if (g_vp[i].pool == o && g_vp[i].base) return &g_vp[i]; return NULL; }
static uint8_t *vpool_reserve(const pool_t *o) {   // → the reserved range, or NULL for a pool that keeps the alias
    if (!(o->flags & 0x200) || o->size <= DM_GAME_APP_POOL) return NULL;
#if TARGET_OS_IPHONE   // 1 GB: Monster Hunter Rise's buffers stay under 0x20100000 through its opening, its textures (32 GB in) are read through the page table, and 4 GB guest + 1 GB stays near the 5 GB proven on a phone
    uint64_t len = o->size < (1ull << 30) ? o->size : 1ull << 30;
#else
    uint64_t len = o->size < (64ull << 30) ? o->size : 64ull << 30;
#endif
    void *a = mmap(NULL, len, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (a == MAP_FAILED) { LOG("[RED] virtual pool %p: cannot reserve %#llx bytes of address space (errno %d)", (const void *)o, (unsigned long long)len, errno); return NULL; }
    pthread_mutex_lock(&g_vp_mu);
    int i = 0; while (i < NVPOOL && g_vp[i].base) i++;
    if (i == NVPOOL) { pthread_mutex_unlock(&g_vp_mu); munmap(a, len); LOG("[RED] more than %d sparse virtual pools", NVPOOL); return NULL; }
    g_vp[i].pool = o; g_vp[i].base = a; g_vp[i].len = len; g_vp[i].size = o->size; g_vp[i].nrun = 0;
    if (!(g_vp[i].pt = calloc((o->size + (2u << 20) - 1) >> 21, sizeof *g_vp[i].pt))) die("sparse pool page table allocation failed");
    pthread_mutex_unlock(&g_vp_mu);
    LOG("[nvn] virtual pool %p (%#llx bytes, flags %#x): sparse, own address range %p+%#llx, physical pages are remapped into it", (const void *)o, (unsigned long long)o->size, o->flags, a, (unsigned long long)len);
    return a;
}
static void vpool_release(const pool_t *o) {
    pthread_mutex_lock(&g_vp_mu); struct vpool *v = vpool_of(o);
    if (v) { for (int k = 0; k < v->nrun; k++) mtl_pool_del(v->base + v->run[k].lo); munmap(v->base, v->len);
        for (uint64_t l = 0; l < (v->size + (2u << 20) - 1) >> 21; l++) free(v->pt[l]);
        free(v->pt); memset(v, 0, sizeof *v); }
    pthread_mutex_unlock(&g_vp_mu);
}
// Mapped runs of a sparse pool: [lo, hi) sorted, disjoint, adjacent runs merged. Caller holds g_vp_mu
static void vrun_set(struct vpool *v, uint64_t lo, uint64_t hi, int mapped) {
    uint64_t nl[NVRUN][2]; int nn = 0; uint64_t mlo = lo, mhi = hi;
    for (int k = 0; k < v->nrun; k++) {
        uint64_t a = v->run[k].lo, b = v->run[k].hi;
        if (b < lo || a > hi || (!mapped && (b == lo || a == hi))) { if (nn < NVRUN) { nl[nn][0] = a; nl[nn][1] = b; nn++; } continue; }   // untouched (an unmap leaves adjacent runs alone)
        mtl_pool_del(v->base + a);
        if (mapped) { if (a < mlo) mlo = a; if (b > mhi) mhi = b; continue; }   // joins the new run
        if (a < lo && nn < NVRUN) { nl[nn][0] = a; nl[nn][1] = lo; nn++; mtl_pool_add(v->base + a, lo - a); }   // what the unmap leaves on either side
        if (b > hi && nn < NVRUN) { nl[nn][0] = hi; nl[nn][1] = b; nn++; mtl_pool_add(v->base + hi, b - hi); }
    }
    if (mapped && nn < NVRUN) { nl[nn][0] = mlo; nl[nn][1] = mhi; nn++; mtl_pool_add(v->base + mlo, mhi - mlo); }
    if (nn == NVRUN) LOG("[RED] virtual pool %p: more than %d separate mapped runs", (const void *)v->pool, NVRUN);
    for (int a = 1; a < nn; a++) for (int b = a; b > 0 && nl[b][0] < nl[b - 1][0]; b--) { uint64_t t0 = nl[b][0], t1 = nl[b][1]; nl[b][0] = nl[b - 1][0]; nl[b][1] = nl[b - 1][1]; nl[b - 1][0] = t0; nl[b - 1][1] = t1; }
    v->nrun = nn; for (int k = 0; k < nn; k++) { v->run[k].lo = nl[k][0]; v->run[k].hi = nl[k][1]; }
}
// Page table of a sparse pool (see above). Caller holds g_vp_mu
static const vpte *vpt_get(const struct vpool *v, uint64_t o) { const vpte *l = v->pt[o >> 21]; return l ? &l[(o >> 12) & (VPT_LEAF - 1)] : NULL; }
static void vpt_set(struct vpool *v, uint64_t voff, uint64_t size, const uint8_t *phys) {
    uint64_t gen = atomic_fetch_add(&g_vp_gen, 1) + 1;
    for (uint64_t o = 0; o < size; o += 0x1000) {
        vpte **l = &v->pt[(voff + o) >> 21];
        if (!*l && !phys) continue;   // never mapped: stays unmapped
        if (!*l && !(*l = calloc(VPT_LEAF, sizeof **l))) die("sparse pool page table allocation failed");
        (*l)[((voff + o) >> 12) & (VPT_LEAF - 1)] = (vpte){phys ? phys + o : NULL, gen};
    }
}
static int vpool_map(struct vpool *v, const map_req *r) {   // caller holds g_vp_mu; → 0 on failure
    uint64_t pg = (uint64_t)getpagesize();
    if (r->voff < 0 || (uint64_t)r->voff + r->size > v->size) { LOG("[RED] virtual pool %p: request at %#llx+%#llx is beyond the pool's %#llx", (const void *)v->pool, (unsigned long long)r->voff, (unsigned long long)r->size, (unsigned long long)v->size); return 0; }
    if (((uint64_t)r->voff | r->size | (uint64_t)r->poff) & 0xFFF) { LOG("[RED] virtual pool %p: request %#llx+%#llx (physical offset %#llx) is not 4 KB aligned", (const void *)v->pool, (unsigned long long)r->voff, (unsigned long long)r->size, (unsigned long long)r->poff); return 0; }
    const pool_t *ph = (const pool_t *)r->phys;
    if (ph && (!ph->mem || r->poff < 0 || (uint64_t)r->poff + r->size > ph->size)) { LOG("[RED] virtual pool %p: physical pool %p (%#llx bytes, storage %p) cannot supply %#llx+%#llx", (const void *)v->pool, (const void *)ph, (unsigned long long)ph->size, (void *)ph->mem, (unsigned long long)r->poff, (unsigned long long)r->size); return 0; }
    vpt_set(v, (uint64_t)r->voff, r->size, ph ? ph->mem + r->poff : NULL);
    vm_address_t to = (vm_address_t)(v->base + r->voff);
    // Not aliased (see above): only textures read through the page table see it; the GPU cannot address it directly
    if ((uint64_t)r->voff + r->size > v->len || ((uintptr_t)to | r->size | (uint64_t)r->voff) & (pg - 1) || (ph && (((uintptr_t)ph->mem + (uintptr_t)r->poff) & (pg - 1)))) {
        static _Atomic uint64_t n; if (!atomic_fetch_add(&n, 1)) LOG("[nvn] virtual pool %p: a request %s (%s %#llx+%#llx) is kept in the page table only: textures read it, the GPU cannot address it (later cases are not logged)",
                                                                   (const void *)v->pool, (uint64_t)r->voff + r->size > v->len ? "beyond the reservation" : "not aligned to the host page", r->phys ? "map" : "unmap", (unsigned long long)r->voff, (unsigned long long)r->size);
        return 1;
    }
    if (!r->phys) {   // unmap: inaccessible pages again
        if (mmap((void *)to, r->size, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED) { LOG("[RED] virtual pool unmap %#lx+%#llx failed (errno %d)", (unsigned long)to, (unsigned long long)r->size, errno); return 0; }
        vrun_set(v, (uint64_t)r->voff, (uint64_t)r->voff + r->size, 0); return 1;
    }
    uintptr_t from = (uintptr_t)ph->mem + (uintptr_t)r->poff;
    vm_prot_t cur, max;
    kern_return_t kr = vm_remap(mach_task_self(), &to, r->size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)from, FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || to != (vm_address_t)(v->base + r->voff)) { LOG("[RED] virtual pool remap %#lx → %p (%#llx bytes) failed: %d", (unsigned long)from, (void *)(v->base + r->voff), (unsigned long long)r->size, kr); return 0; }
    mprotect((void *)to, r->size, PROT_READ | PROT_WRITE);   // the source may be write-protected by the dirty tracker at this moment; the alias itself is plain memory
    vrun_set(v, (uint64_t)r->voff, (uint64_t)r->voff + r->size, 1);
    uint64_t end = (uint64_t)r->voff + r->size;
    if (end >> 27 > v->top >> 27) LOG("[nvn] virtual pool %p: mapped up to %#llx (%d runs)", (const void *)v->pool, (unsigned long long)end, v->nrun);
    if (end > v->top) v->top = end;
    return 1;
}
NVN(nvnMemoryPoolMapVirtual, NVNboolean, NVNmemoryPool *p, int n, const NVNmappingRequest *rq) {
    pool_t *o = (pool_t *)p; const map_req *r = (const map_req *)rq;
    pthread_mutex_lock(&g_vp_mu); struct vpool *v = vpool_of(o); int ok = 1;
    for (int i = 0; v && i < n && ok; i++) ok = vpool_map(v, &r[i]);
    pthread_mutex_unlock(&g_vp_mu);
    if (v) return (NVNboolean)ok;
    for (int i = 0; i < n; i++) {
        const pool_t *ph = (const pool_t *)r[i].phys;
        if (!ph) continue;   // Unmapping (null physical pool): the host keeps the storage as is
        if (!ph->mem) { LOG("[RED] MapVirtual: physical pool %p has no storage", (const void *)ph); return 0; }
        uint8_t *base = ph->mem + r[i].poff - r[i].voff;
        if (r[i].voff < 0 || (uint64_t)r[i].voff + r[i].size > o->size || (uint64_t)r[i].poff + r[i].size > ph->size) { LOG("[RED] MapVirtual: request out of range (virtual %#llx+%#llx / pool %#llx, physical %#llx / pool %#llx)", (unsigned long long)r[i].voff, (unsigned long long)r[i].size, (unsigned long long)o->size, (unsigned long long)r[i].poff, (unsigned long long)ph->size); return 0; }
        if (o->mem && o->mem != base) { LOG("[RED] MapVirtual: non-contiguous mapping (true sparse) not implemented: virtual pool %p already maps to %p, this request wants %p", (void *)o, (void *)o->mem, (void *)base); return 0; }
        if (!o->mem) LOG("[nvn] MapVirtual: the storage of virtual pool %p (%#llx bytes) points at physical pool %p + %#llx", (void *)o, (unsigned long long)o->size, (const void *)ph, (unsigned long long)(r[i].poff - r[i].voff));
        o->mem = base;
    }
    return 1;
}

// Samplers: the getters read the parameters in the object. Reduction (min / max of the texels a filtered sample covers) maps to the Metal 26 sampler reduction mode (nvn_mtl.m samp_by_id)
NVN(nvnSamplerBuilderSetReductionFilter, void, NVNsamplerBuilder *b, NVNsamplerReduction r) { SB(b)->reduction = (uint8_t)r; }
NVN(nvnSamplerSetDebugLabel, void, NVNsampler *s, const char *l) { (void)s; (void)l; }
#define SP(s) (&((const samp_t *)(s))->p)
NVN(nvnSamplerGetMinMagFilter, void, const NVNsampler *s, NVNminFilter *mi, NVNmagFilter *ma) { *mi = (NVNminFilter)SP(s)->minf; *ma = (NVNmagFilter)SP(s)->magf; }
NVN(nvnSamplerGetWrapMode, void, const NVNsampler *s, NVNwrapMode *a, NVNwrapMode *b, NVNwrapMode *c) { *a = (NVNwrapMode)SP(s)->ws; *b = (NVNwrapMode)SP(s)->wt; *c = (NVNwrapMode)SP(s)->wr; }
NVN(nvnSamplerGetLodClamp, void, const NVNsampler *s, float *lo, float *hi) { *lo = SP(s)->lod_min; *hi = SP(s)->lod_max; }
NVN(nvnSamplerGetLodBias, float, const NVNsampler *s) { return SP(s)->lod_bias; }
NVN(nvnSamplerGetCompare, void, const NVNsampler *s, NVNcompareMode *m, NVNcompareFunc *f) { *m = (NVNcompareMode)SP(s)->cmp_mode; *f = (NVNcompareFunc)SP(s)->cmp_func; }
NVN(nvnSamplerGetBorderColor, void, const NVNsampler *s, float *c) { memcpy(c, SP(s)->border, 16); }
NVN(nvnSamplerGetMaxAnisotropy, float, const NVNsampler *s) { return SP(s)->aniso; }
NVN(nvnSamplerGetReductionFilter, NVNsamplerReduction, const NVNsampler *s) { return (NVNsamplerReduction)SP(s)->reduction; }
// whether two samplers have the same state (games use it to deduplicate sampler pool entries)
NVN(nvnSamplerCompare, NVNboolean, const NVNsampler *a, const NVNsampler *b) {
    const samp_par *x = SP(a), *y = SP(b);
    return x->minf == y->minf && x->magf == y->magf && x->ws == y->ws && x->wt == y->wt && x->wr == y->wr && x->cmp_mode == y->cmp_mode && x->cmp_func == y->cmp_func && x->reduction == y->reduction &&
           x->lod_min == y->lod_min && x->lod_max == y->lod_max && x->lod_bias == y->lod_bias && x->aniso == y->aniso && !memcmp(x->border, y->border, sizeof x->border);
}

// Texture builder / texture / view
NVN(nvnTextureBuilderSetSize2D, void, NVNtextureBuilder *b, int w, int h) { TB(b)->d.width = w; TB(b)->d.height = h; TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetSize3D, void, NVNtextureBuilder *b, int w, int h, int d) { TB(b)->d.width = w; TB(b)->d.height = h; TB(b)->d.depth = d; }
NVN(nvnTextureBuilderSetSwizzle, void, NVNtextureBuilder *b, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    tex_builder_t *o = (tex_builder_t *)b; o->set_swz = 1; o->swz = (uint16_t)((r & 7) | (g & 7) << 3 | (bl & 7) << 6 | (a & 7) << 9);
    if (r != NVN_TEXTURE_SWIZZLE_R || g != NVN_TEXTURE_SWIZZLE_G || bl != NVN_TEXTURE_SWIZZLE_B || a != NVN_TEXTURE_SWIZZLE_A) ONCE_LOG("TextureBuilderSetSwizzle not identity (%d %d %d %d): remapped with a Metal view when sampling", (int)r, (int)g, (int)bl, (int)a);
}
// STENCIL: the texture reads the stencil of its depth-stencil storage, as a stencil-mode view does (tex_by_id in nvn_mtl.m). Unreal Engine 4.27 makes its stencil shader resources this way: a second texture
// on the depth buffer's storage, built in stencil mode (DRAGON QUEST VII Reimagined's character occlusion mask, edbfc7a088be545e, tests stencil bits 0x1 and 0x84 of two of them; read as depth, the
// bits followed the depth values and the hero was drawn as a dithered silhouette inside a dithered square)
NVN(nvnTextureBuilderSetDepthStencilMode, void, NVNtextureBuilder *b, NVNtextureDepthStencilMode m) { TB(b)->stencil = m == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL; }
NVN(nvnTextureBuilderGetStorageClass, NVNstorageClass, const NVNtextureBuilder *b) { (void)b; return 0; }
// Used by Bowser's Fury: the texture's channel swizzle (set by TextureBuilderSetSwizzle at build time, identity R G B A when unset) and storage class (the host has none, 0 as for the builder)
NVN(nvnTextureGetSwizzle, void, const NVNtexture *t, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    unsigned w = tex_swz(CTX(t));
    if (r) *r = (NVNtextureSwizzle)(w & 7); if (g) *g = (NVNtextureSwizzle)(w >> 3 & 7); if (b) *b = (NVNtextureSwizzle)(w >> 6 & 7); if (a) *a = (NVNtextureSwizzle)(w >> 9 & 7);
}
NVN(nvnTextureGetStorageClass, NVNstorageClass, const NVNtexture *t) { (void)t; return 0; }
NVN(nvnTextureBuilderGetZCullStorageSize, size_t, const NVNtextureBuilder *b) { return zcull_size(&TB(b)->d); }   // as for the texture (zcull_size)
// Packaged texture data in CPU memory (not written into the pool by the game): NVN copies it into the texture storage at TextureInitialize (the CPU cannot write a compressible pool; the driver copies). 13 Sentinels creates all its textures this way
NVN(nvnTextureBuilderSetPackagedTextureData, void, NVNtextureBuilder *b, const void *data) { TB(b)->pkg_data = data; }
NVN(nvnTextureGetFormat, NVNformat, const NVNtexture *t) { return (NVNformat)CTX(t)->d.format; }
NVN(nvnTextureGetTarget, NVNtextureTarget, const NVNtexture *t) { return (NVNtextureTarget)CTX(t)->d.target; }
NVN(nvnTextureSetDebugLabel, void, NVNtexture *t, const char *s) { (void)t; (void)s; }
NVN(nvnTextureViewGetLevels, NVNboolean, const NVNtextureView *v, int *base, int *n) { const view_t *w = (const view_t *)v; *base = w->base_level; *n = w->nlevels; return w->set_levels; }
NVN(nvnTextureViewGetTarget, NVNboolean, const NVNtextureView *v, NVNtextureTarget *t) { const view_t *w = (const view_t *)v; *t = (NVNtextureTarget)w->target; return w->set_target; }
NVN(nvnTextureViewSetDepthStencilMode, void, NVNtextureView *v, NVNtextureDepthStencilMode m) { VW(v)->set_dsmode = 1; VW(v)->dsmode = (uint8_t)m; }   // STENCIL: the replay binds a stencil-only view (tex_by_id)
NVN(nvnTextureViewSetSwizzle, void, NVNtextureView *v, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    view_t *w = VW(v); w->set_swizzle = 1; w->swz[0] = (uint8_t)r; w->swz[1] = (uint8_t)g; w->swz[2] = (uint8_t)bl; w->swz[3] = (uint8_t)a;
    if (r != NVN_TEXTURE_SWIZZLE_R || g != NVN_TEXTURE_SWIZZLE_G || bl != NVN_TEXTURE_SWIZZLE_B || a != NVN_TEXTURE_SWIZZLE_A) ONCE_LOG("TextureViewSetSwizzle not identity (%d %d %d %d): remapped with a Metal view when sampling", (int)r, (int)g, (int)bl, (int)a);
}

// Color state: alpha test function, logic op (the fields already exist in color_st)
NVN(nvnColorStateSetAlphaTest, void, NVNcolorState *s, NVNalphaFunc f) { ST(color_st, s)->alpha_func = (uint32_t)f; }
NVN(nvnColorStateSetLogicOp, void, NVNcolorState *s, NVNlogicOp op) { ST(color_st, s)->logic_op = (uint32_t)op; }
// Getters and the rest of the multisample state: Pokémon Legends: Z-A (Trinity, SDK 19) reads its state objects and builders back when it builds its pipeline states; the stub returned
// whatever was left in x0 (the object's address), so a blend enable or depth test read back as true, a texture builder's width as an address
NVN(nvnColorStateGetBlendEnable, NVNboolean, const NVNcolorState *s, int i) { return i >= 0 && i < 8 && (((const color_st *)s)->blend_en >> i & 1); }
NVN(nvnColorStateGetAlphaTest, NVNalphaFunc, const NVNcolorState *s) { return (NVNalphaFunc)((const color_st *)s)->alpha_func; }
NVN(nvnColorStateGetLogicOp, NVNlogicOp, const NVNcolorState *s) { return (NVNlogicOp)((const color_st *)s)->logic_op; }
NVN(nvnChannelMaskStateGetChannelMask, void, const NVNchannelMaskState *s, int i, NVNboolean *r, NVNboolean *g, NVNboolean *b, NVNboolean *a) {
    uint32_t m = i >= 0 && i < 8 ? ((const chmask_st *)s)->mask >> (4 * i) & 0xf : 0;
    if (r) *r = (m & 1) != 0; if (g) *g = (m & 2) != 0; if (b) *b = (m & 4) != 0; if (a) *a = (m & 8) != 0;
}
NVN(nvnBlendStateGetBlendTarget, int, const NVNblendState *s) { return (int)((const blend_st *)s)->target; }
NVN(nvnBlendStateGetBlendFunc, void, const NVNblendState *s, NVNblendFunc *sr, NVNblendFunc *dr, NVNblendFunc *sa, NVNblendFunc *da) {
    const blend_st *o = (const blend_st *)s;
    if (sr) *sr = (NVNblendFunc)o->src_rgb; if (dr) *dr = (NVNblendFunc)o->dst_rgb; if (sa) *sa = (NVNblendFunc)o->src_a; if (da) *da = (NVNblendFunc)o->dst_a;
}
NVN(nvnBlendStateGetBlendEquation, void, const NVNblendState *s, NVNblendEquation *er, NVNblendEquation *ea) {
    const blend_st *o = (const blend_st *)s; if (er) *er = (NVNblendEquation)o->eq_rgb; if (ea) *ea = (NVNblendEquation)o->eq_a;
}
NVN(nvnPolygonStateGetCullFace, NVNface, const NVNpolygonState *s) { return (NVNface)((const poly_st *)s)->cull; }
NVN(nvnPolygonStateGetFrontFace, NVNfrontFace, const NVNpolygonState *s) { return (NVNfrontFace)((const poly_st *)s)->front; }
NVN(nvnPolygonStateGetPolygonMode, NVNpolygonMode, const NVNpolygonState *s) { return (NVNpolygonMode)((const poly_st *)s)->mode; }
NVN(nvnPolygonStateGetPolygonOffsetEnables, NVNpolygonOffsetEnable, const NVNpolygonState *s) { return (NVNpolygonOffsetEnable)((const poly_st *)s)->offset_en; }
NVN(nvnDepthStencilStateGetDepthTestEnable, NVNboolean, const NVNdepthStencilState *s) { return ((const ds_st *)s)->dt; }
NVN(nvnDepthStencilStateGetDepthWriteEnable, NVNboolean, const NVNdepthStencilState *s) { return ((const ds_st *)s)->dw; }
NVN(nvnDepthStencilStateGetDepthFunc, NVNdepthFunc, const NVNdepthStencilState *s) { return (NVNdepthFunc)((const ds_st *)s)->df; }
NVN(nvnDepthStencilStateGetStencilTestEnable, NVNboolean, const NVNdepthStencilState *s) { return ((const ds_st *)s)->st; }
NVN(nvnDepthStencilStateGetStencilFunc, NVNstencilFunc, const NVNdepthStencilState *s, NVNface f) { const ds_st *o = (const ds_st *)s; return (NVNstencilFunc)(f == NVN_FACE_BACK ? o->sf_b : o->sf_f); }
NVN(nvnDepthStencilStateGetStencilOp, void, const NVNdepthStencilState *s, NVNface f, NVNstencilOp *fail, NVNstencilOp *zfail, NVNstencilOp *zpass) {
    const ds_st *o = (const ds_st *)s; int b = f == NVN_FACE_BACK;
    if (fail) *fail = (NVNstencilOp)(b ? o->fail_b : o->fail_f); if (zfail) *zfail = (NVNstencilOp)(b ? o->zfail_b : o->zfail_f); if (zpass) *zpass = (NVNstencilOp)(b ? o->zpass_b : o->zpass_f);
}
// Multisample state: recorded only (the replay uses the sample count, multisample enable and alpha to coverage)
NVN(nvnMultisampleStateSetAlphaToCoverageDither, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c_dither = B(e) != 0; }
NVN(nvnMultisampleStateSetRasterSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->raster_samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetCoverageModulationMode, void, NVNmultisampleState *s, NVNcoverageModulationMode m) { ST(ms_st, s)->cov_mod = (uint8_t)m; }
NVN(nvnMultisampleStateSetCoverageToColorEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->cov2color_en = B(e) != 0; }
NVN(nvnMultisampleStateSetCoverageToColorOutput, void, NVNmultisampleState *s, int i) { ST(ms_st, s)->cov2color_out = (uint8_t)i; }
NVN(nvnMultisampleStateSetSampleLocationsGridEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_grid = B(e) != 0; }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->a2c; }
NVN(nvnMultisampleStateGetAlphaToCoverageDither, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->a2c_dither; }
NVN(nvnMultisampleStateGetRasterSamples, int, NVNmultisampleState *s) { return ((const ms_st *)s)->raster_samples; }
NVN(nvnMultisampleStateGetCoverageModulationMode, NVNcoverageModulationMode, const NVNmultisampleState *s) { return (NVNcoverageModulationMode)((const ms_st *)s)->cov_mod; }
NVN(nvnMultisampleStateGetCoverageToColorEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->cov2color_en; }
NVN(nvnMultisampleStateGetCoverageToColorOutput, int, const NVNmultisampleState *s) { return ((const ms_st *)s)->cov2color_out; }
NVN(nvnMultisampleStateGetSampleLocationsEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->sl_en; }
NVN(nvnMultisampleStateGetSampleLocationsGridEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->sl_grid; }
// Builder and pool queries
NVN(nvnBufferBuilderGetSize, size_t, const NVNbufferBuilder *b) { return (size_t)((const buf_builder_t *)b)->size; }
NVN(nvnBufferBuilderGetMemoryOffset, ptrdiff_t, const NVNbufferBuilder *b) { return (ptrdiff_t)((const buf_builder_t *)b)->off; }
NVN(nvnTextureBuilderGetTarget, NVNtextureTarget, const NVNtextureBuilder *b) { return (NVNtextureTarget)((const tex_builder_t *)b)->d.target; }
NVN(nvnTextureBuilderGetWidth, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.width; }
NVN(nvnTextureBuilderGetHeight, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.height; }
NVN(nvnTextureBuilderGetDepth, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.depth; }
NVN(nvnTextureBuilderGetLevels, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.levels; }
NVN(nvnTextureBuilderGetFormat, NVNformat, const NVNtextureBuilder *b) { return (NVNformat)((const tex_builder_t *)b)->d.format; }
NVN(nvnTextureBuilderGetSamples, int, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->d.samples; }
NVN(nvnTextureBuilderGetStride, ptrdiff_t, const NVNtextureBuilder *b) { return (ptrdiff_t)((const tex_builder_t *)b)->d.stride; }
NVN(nvnTextureViewGetFormat, NVNboolean, const NVNtextureView *v, NVNformat *f) { const view_t *w = (const view_t *)v; if (f) *f = (NVNformat)w->format; return w->set_format; }
NVN(nvnTextureViewGetDepthStencilMode, NVNboolean, const NVNtextureView *v, NVNtextureDepthStencilMode *m) { const view_t *w = (const view_t *)v; if (m) *m = (NVNtextureDepthStencilMode)w->dsmode; return w->set_dsmode; }
NVN(nvnTextureViewGetSwizzle, NVNboolean, const NVNtextureView *v, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    const view_t *w = (const view_t *)v; NVNtextureSwizzle *o[4] = {r, g, b, a};
    for (int c = 0; c < 4; c++) if (o[c]) *o[c] = (NVNtextureSwizzle)(w->set_swizzle ? w->swz[c] : NVN_TEXTURE_SWIZZLE_R + c);
    return w->set_swizzle;
}
NVN(nvnTextureGetDepthStencilMode, NVNtextureDepthStencilMode, const NVNtexture *t) { return ((const tex_t *)t)->stencil ? NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH; }
// Queue priority: the host has one Metal queue
NVN_NOPROTO(nvnQueueBuilderSetQueuePriority, void, NVNqueueBuilder *b, int prio) { (void)b; (void)prio; }
// The driver's external graphics debugger switch (Nsight / NVN debugger attach): Super Smash Bros. Ultimate fetches it with nvnDeviceGetProcAddress(NULL, …) before creating its device and calls
// it without a null check (main+0x37fe92c); the host has no debugger to attach
NVN_NOPROTO(nvnSetExternalDebuggerEnabledNVX, void, NVNboolean enable) { (void)enable; }
// Transform feedback (vertex outputs captured into buffers): Pokémon Legends: Z-A skins its characters this way. A vertex-only program writes skinned positions and packed normals / tangents
// under rasterizer discard, and the character draws read those buffers as vertex streams. Recorded here and run at replay by the shader's main0_tf entry point, a vertex function that stores the
// captured outputs into the bound buffers (dmsh_build.py vs_tf, the layout from the program's control section; nvn_mtl.m tf_* for the replay)
NVN(nvnCommandBufferBindTransformFeedbackBuffer, void, NVNcommandBuffer *c, int i, NVNbufferAddress a, size_t n) { *REC(c, OP_TF_BIND, p_tf_bind) = (p_tf_bind){i, 0, a, n}; }
NVN(nvnCommandBufferBindTransformFeedbackBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *r) {
    for (int i = 0; i < n; i++) *REC(c, OP_TF_BIND, p_tf_bind) = (p_tf_bind){first + i, 0, r[i].address, r[i].size};
}
NVN(nvnCommandBufferBeginTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { *REC(c, OP_TF_STATE, p_tf_state) = (p_tf_state){0, 0, a}; }
NVN(nvnCommandBufferEndTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { *REC(c, OP_TF_STATE, p_tf_state) = (p_tf_state){1, 0, a}; }
NVN(nvnCommandBufferPauseTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { *REC(c, OP_TF_STATE, p_tf_state) = (p_tf_state){2, 0, a}; }
NVN(nvnCommandBufferResumeTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { *REC(c, OP_TF_STATE, p_tf_state) = (p_tf_state){3, 0, a}; }
NVN(nvnCommandBufferDrawTransformFeedback, void, NVNcommandBuffer *c, NVNdrawPrimitive m, NVNbufferAddress a) { (void)c; (void)m; (void)a; ONCE_LOG("[RED] DrawTransformFeedback: drawing the captured vertex count is not implemented, nothing drawn"); }

// Device: combined texture handle = texture id | sampler id << 20 (same encoding as the separate handles)
NVN(nvnDeviceGetTextureHandle, NVNtextureHandle, const NVNdevice *d, int tex, int samp) { (void)d; return (NVNtextureHandle)((uint64_t)(uint32_t)tex | (uint64_t)(uint32_t)samp << 20); }
// Handle for texel fetches only (Monster Hunter Rise): a fetch reads the texture descriptor and no sampler, so the handle is the texture ID with sampler ID 0
NVN(nvnDeviceGetTexelFetchHandle, NVNtextureHandle, const NVNdevice *d, int tex) { (void)d; return (NVNtextureHandle)(uint64_t)(uint32_t)tex; }
// Counter timestamps are 19.2 MHz GPU ticks (nvn_mtl.m gpu_ticks: ns × 12 / 625); convert back to ns. They used to be returned as is, about 52 times smaller than GetCurrentTimestampInNanoseconds, so subtracting one from the other gave garbage
NVN(nvnDeviceGetTimestampInNanoseconds, uint64_t, const NVNdevice *d, const NVNcounterData *c) { (void)d; return c->timestamp * 625 / 12; }
// Current GPU time (ns): the same clock as the counter timestamps -- the guest clock (guest_ns, nvn_mtl.m gpu_ticks)
NVN(nvnDeviceGetCurrentTimestampInNanoseconds, uint64_t, const NVNdevice *d) { (void)d; return guest_ns(); }
NVN(nvnDeviceRegisterFastClearColor, NVNboolean, NVNdevice *d, const float *c, NVNformat f) { (void)d; (void)c; (void)f; return 1; }
// Same as the color one: a fast clear value is only a hint to the Tegra hardware (Tears of the Kingdom registers its depth clear at startup)
NVN(nvnDeviceRegisterFastClearDepth, NVNboolean, NVNdevice *d, float v) { (void)d; (void)v; return 1; }
// The size of the driver's cache for shaders it compiles from intermediate code (The Great Ace Attorney Chronicles sets 10 at startup); the host compiles nothing at run time
NVN(nvnDeviceSetIntermediateShaderCache, void, NVNdevice *d, int n) { (void)d; (void)n; }

// Window: number of active swapchain textures (the host rotates through the count given at creation; logged once)
NVN_NOPROTO(nvnWindowBuilderSetNumActiveTextures, void, NVNwindowBuilder *b, int n) { (void)b; ONCE_LOG("WindowBuilderSetNumActiveTextures %d (the host rotates through the SetTextures count)", n); }
NVN_NOPROTO(nvnWindowSetNumActiveTextures, void, NVNwindow *w, int n) { (void)w; ONCE_LOG("WindowSetNumActiveTextures %d (the host rotates through the SetTextures count)", n); }

// Command buffer memory usage: record bytes written in this recording (accumulated across segments; the host does not use control memory, always 0); the memory callback switch is ignored.
// It used to be cur - begin: once a recording overflowed into a host segment, cur is in host memory (an address below the guest begin) and it returned 0. SMBW asks for this value after recording each material setup handle,
// and on 0 (with control memory also 0) treats the recording as empty and marks the handle invalid (from main+0x4bb6c it clears bit 0 of the material entry +0x50); later draws no longer CALL it and inherit the previous object's vertex buffers / textures:
// on the iPhone a batch of draws on odd frames of the opening course fetched vertices out of bounds, giving huge triangles and NaN (black band, black-edged glow, grey stripes). Without overflow the accumulated value equals cur - begin.
// As in NVN, the count runs from the start of the command memory the game last added (AddCommandMemory), not from BeginRecording: Fire Emblem: Three Houses adds one 16 MB chunk of a ring to its main
// command buffer and records every frame into it; its memory callback (main+0x9ca960) takes chunk start + used as the position the GPU has reached and frees the ring up to there when the frame's fence
// signals. Counted per recording, the position stayed at one frame's size (about 18 KB), the ring never freed past it, and after the first 16 MB (about 15 s) the callback waited for ring space forever
NVN(nvnCommandBufferGetCommandMemoryUsed, size_t, const NVNcommandBuffer *c) { const cmd_t *m = (const cmd_t *)c;
    return m->begin ? (size_t)m->used : 0; }
NVN(nvnCommandBufferGetControlMemoryUsed, size_t, const NVNcommandBuffer *c) { (void)c; return 0; }
// Size and free bytes of the command memory the game last added (DRAGON QUEST XI S, Unreal Engine 4.18): as in NVN, size = used + free; free is what is left in the current segment
NVN(nvnCommandBufferGetCommandMemoryFree, size_t, const NVNcommandBuffer *c) { const cmd_t *m = (const cmd_t *)c; return m->cur && m->end > m->cur ? (size_t)(m->end - m->cur) : 0; }
NVN(nvnCommandBufferGetCommandMemorySize, size_t, const NVNcommandBuffer *c) { return h_nvnCommandBufferGetCommandMemoryUsed(c) + h_nvnCommandBufferGetCommandMemoryFree(c); }
NVN_NOPROTO(nvnCommandBufferSetCommandMemoryCallbackEnabled, void, NVNcommandBuffer *c, uint32_t on) { (void)c; (void)on; }

// nvnProgramSetShadersExt (NVN 55): each stage is 0x18 bytes = NVNshaderData{data, control} + one 8-byte extension field (filled from SMBW main 0x1074ff0).
// The host identifies stages by the control section and does not use the extension field: convert to plain NVNshaderData and hand it to SetShaders.
typedef struct { NVNbufferAddress data; const void *control; uint64_t ext; } shader_data_ext;
NVN_NOPROTO(nvnProgramSetShadersExt, NVNboolean, NVNprogram *p, int n, const shader_data_ext *sd) {
    if (n < 0 || n > 6) { LOG("[RED] ProgramSetShadersExt stage count %d", n); return 0; }
    NVNshaderData plain[6];
    for (int i = 0; i < n; i++) { plain[i].data = sd[i].data; plain[i].control = sd[i].control; if (sd[i].ext) ONCE_LOG("ProgramSetShadersExt extension field nonzero (%#llx): unused by the host", (unsigned long long)sd[i].ext); }
    return h_nvnProgramSetShaders(p, n, plain);
}

// ---------- Commands used by newer-SDK games (SMBW): most map onto existing record formats, the replay layer is unchanged ----------
// Singular / combined forms → plural / separate forms; state the replay layer does not have yet (conditional render, depth bounds, ...) is logged once and drawing continues (better to over-draw than to drop).
#include "../tex/nvn_deswizzle.h"

NVN(nvnCommandBufferCallCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) { h_nvnCommandBufferCopyCommands(c, n, h); }   // the replay layer treats CALL and COPY alike
// Combined texture handle (texture id | sampler id << 20) is recorded into the texture slot as is: the replay layer takes the texture from the low 20 bits and the sampler from the high bits (nvn_mtl.m NVN_RB_SAMPT)
// One combined handle: recorded as the hardware lays the command out, the handle 16 bytes in, where Luigi's Mansion 3 patches it after recording (nvn_obj.h p_bind_tex1)
NVN(nvnCommandBufferBindTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNtextureHandle h) {
    p_bind_tex1 *r = REC(c, OP_BIND_TEX1, p_bind_tex1); *r = (p_bind_tex1){st, i, (uint64_t)h, CB(c)->tex_base, CB(c)->samp_base};
}
_Static_assert(offsetof(p_bind_tex1, h) == 8, "a single texture bind keeps its handle 16 bytes into the record (8 of header)");
NVN(nvnCommandBufferBindTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) { bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h); }
// "Deferred" binding: the GPU reads the handles h points to when it executes this command, not at recording. The per-texture binding of The Legend of Zelda: Breath of the Wild's engine (main+0xec2e64) always uses it,
//   passing the address of the handle field in the texture sampler object with a count of 1; command blocks are recorded once and submitted repeatedly, and the handle is written / replaced afterwards. The host used to copy the handles at recording (mostly 0),
//   so every texture of the deferred lighting composite became the dummy texture 0 and the whole 3D picture was black. The address is recorded and the replay layer reads it when it reads the record (bh_fresh)
NVN_NOPROTO(nvnCommandBufferBindTexturesDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) {
    p_bind_handles *r = bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h);
    if (r && h) { r->defer = 1; r->src = (uint64_t)(uintptr_t)h; }
    if (CB(c)->tex_base || CB(c)->samp_base) ONCE_LOG("[RED] deferred-bound handles are read only at replay: the numbering base of this command buffer's bound descriptor pools is not added (pools %d / %d)", CB(c)->tex_base, CB(c)->samp_base);
}
NVN(nvnCommandBufferBindImage, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNimageHandle h) { uint64_t v = (uint64_t)h; bind_handles(c, OP_BIND_IMG, st, i, 1, &v); }
NVN(nvnCommandBufferBindVertexBuffer, void, NVNcommandBuffer *c, int i, NVNbufferAddress a, size_t n) { NVNbufferRange r = {a, n}; h_nvnCommandBufferBindVertexBuffers(c, i, 1, &r); }

// Draws
NVN(nvnCommandBufferDrawElements, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib) { h_nvnCommandBufferDrawElementsInstanced(c, pr, it, n, ib, 0, 0, 1); }
NVN(nvnCommandBufferDrawElementsBaseVertex, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib, int bv) { h_nvnCommandBufferDrawElementsInstanced(c, pr, it, n, ib, bv, 0, 1); }
NVN(nvnCommandBufferDrawArraysInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n, int bi, int inst) {
    rec_draw(c, (p_draw){pr, first, n, inst, bi});
}
// Indirect, as DrawArraysIndirect: the record at ind is read by the GPU (nvn_mtl.m draw)
// Indirect-count multi-draws (Monster Hunter Rise's GPU-driven scene: compute culling writes the draw records and a u32 count, then up to max draws are made from them; 1–49 per call,
// stride 20, the records and counts in its sparse virtual pool). Metal has no draw count read by the GPU, so each multi-draw becomes max ordinary indirect draws that carry the count's address
// and their index; the replay masks their records on the GPU, zeros past the count (nvn_mtl.m mdx_run). Both used to be empty, and the whole scene stayed undrawn
enum { MDI_MAX = 1024 };
NVN(nvnCommandBufferMultiDrawElementsIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress ind, NVNbufferAddress cnt, int max, ptrdiff_t stride) {
    if (max > MDI_MAX) { ONCE_LOG("[RED] indirect-count multi-draw of up to %d draws: only the first %d are made", max, MDI_MAX); max = MDI_MAX; }
    for (int i = 0; i < max; i++) rec_draw_idx(c, (p_draw_idx){pr, it, 0, 0, 0, 1, ib, ind + (uint64_t)i * (uint64_t)stride, cnt, i});
}
NVN(nvnCommandBufferMultiDrawArraysIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNbufferAddress ind, NVNbufferAddress cnt, int max, ptrdiff_t stride) {
    if (max > MDI_MAX) { ONCE_LOG("[RED] indirect-count multi-draw of up to %d draws: only the first %d are made", max, MDI_MAX); max = MDI_MAX; }
    for (int i = 0; i < max; i++) rec_draw(c, (p_draw){pr, 0, 0, 1, 0, ind + (uint64_t)i * (uint64_t)stride, cnt, i});
}
// Whether the host can read n bytes at a guest address: outside a sparse virtual pool always; inside one only where pages are mapped (nvn_mtl.m reads multi-draw counts there)
int nvn_host_readable(uint64_t a, uint64_t n) {
    int ok = 1; pthread_mutex_lock(&g_vp_mu);
    for (int k = 0; k < NVPOOL; k++) { struct vpool *v = &g_vp[k]; if (!v->base || a < (uint64_t)(uintptr_t)v->base || a >= (uint64_t)(uintptr_t)v->base + v->len) continue;
        uint64_t o = a - (uint64_t)(uintptr_t)v->base; ok = 0;
        for (int r = 0; r < v->nrun; r++) if (o >= v->run[r].lo && o + n <= v->run[r].hi) { ok = 1; break; }
        break; }
    pthread_mutex_unlock(&g_vp_mu); return ok;
}
// Textures of a sparse pool, read through its page table (see the sparse pools above): whether a pool is one, a copy of a texture's bytes (unmapped pages read as 0; → bytes not mapped),
// whether its content may have changed since an upload (a mapping in its range newer than gen, or a physical page behind it written after the dirty-page stamp), and the write
// protection of the physical pages behind it (taken before an upload, as for any packaged texture)
static struct vpool *vpool_at(const void *pool, uint64_t a) {
    for (int k = 0; k < NVPOOL; k++) { struct vpool *v = &g_vp[k]; if (v->base && v->pool == pool && a >= (uint64_t)(uintptr_t)v->base && a < (uint64_t)(uintptr_t)v->base + v->size) return v; }
    return NULL;
}
int nvn_sparse_pool(const void *pool) { int r = 0; for (int k = 0; k < NVPOOL; k++) r |= g_vp[k].base && g_vp[k].pool == pool; return r; }   // no lock: a slot is set once at Initialize, cleared at Finalize; called per texture lookup
uint64_t nvn_sparse_gen(void) { return atomic_load(&g_vp_gen); }
uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t a, uint64_t n) {
    pthread_mutex_lock(&g_vp_mu); struct vpool *v = vpool_at(pool, a); uint64_t miss = 0;
    if (!v) { pthread_mutex_unlock(&g_vp_mu); memset(dst, 0, n); return n; }
    for (uint64_t p = 0, o = a - (uint64_t)(uintptr_t)v->base; p < n;) {
        uint64_t at = o + p, k = 0x1000 - (at & 0xFFF); if (k > n - p) k = n - p;
        const vpte *e = at < v->size ? vpt_get(v, at) : NULL;
        if (e && e->phys) memcpy((uint8_t *)dst + p, e->phys + (at & 0xFFF), k); else { memset((uint8_t *)dst + p, 0, k); miss += k; }
        p += k;
    }
    pthread_mutex_unlock(&g_vp_mu); return miss;
}
// Runs of consecutive physical pages behind [a, a+n): fn on each (nonzero stops the walk and is returned), or 1 at the first page mapped after gen (gen ~0 = do not look)
static int vpt_walk(const void *pool, uint64_t a, uint64_t n, uint64_t gen, int (*fn)(const uint8_t *, uint64_t, uint64_t), uint64_t arg) {
    pthread_mutex_lock(&g_vp_mu); struct vpool *v = vpool_at(pool, a); int r = 0;
    if (!v) { pthread_mutex_unlock(&g_vp_mu); return 1; }
    const uint8_t *rs = NULL, *re = NULL; uint64_t o = a - (uint64_t)(uintptr_t)v->base;
    for (uint64_t at = o & ~0xFFFull; at < o + n && at < v->size && !r; at += 0x1000) {
        const vpte *e = vpt_get(v, at);
        if (!e) { at |= (2u << 20) - 1; at -= 0xFFF; continue; }   // a leaf never mapped: skip its 2 MB
        if (gen != ~0ull && e->gen > gen) { r = 1; break; }
        if (!e->phys) continue;
        if (rs && e->phys == re) { re += 0x1000; continue; }
        if (rs) r = fn(rs, (uint64_t)(re - rs), arg);
        rs = e->phys; re = rs + 0x1000;
    }
    if (!r && rs) r = fn(rs, (uint64_t)(re - rs), arg);
    pthread_mutex_unlock(&g_vp_mu); return r;
}
static int run_written(const uint8_t *p, uint64_t n, uint64_t stamp) { return nvn_dirty_since(p, n, stamp); }
static int run_protect(const uint8_t *p, uint64_t n, uint64_t unused) { (void)unused; nvn_dirty_protect(p, n); return 0; }
int nvn_sparse_since(const void *pool, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp) { return vpt_walk(pool, a, n, gen, run_written, stamp); }
int nvn_sparse_mapped(const void *pool, uint64_t a, uint64_t n) {   // every page of [a, a+n) has memory behind it
    pthread_mutex_lock(&g_vp_mu); struct vpool *v = vpool_at(pool, a); int ok = v != NULL;
    for (uint64_t o = v ? (a - (uint64_t)(uintptr_t)v->base) & ~0xFFFull : 0, end = v ? a - (uint64_t)(uintptr_t)v->base + n : 0; ok && o < end; o += 0x1000) { const vpte *e = o < v->size ? vpt_get(v, o) : NULL; ok = e && e->phys; }
    pthread_mutex_unlock(&g_vp_mu); return ok;
}
void nvn_sparse_protect(const void *pool, uint64_t a, uint64_t n) { vpt_walk(pool, a, n, ~0ull, run_protect, 0); }
NVN(nvnCommandBufferDrawElementsIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress ind) {
    rec_draw_idx(c, (p_draw_idx){pr, it, 0, 0, 0, 1, ib, ind});
}

// Viewport / scissor / depth range: single and multiple forms
NVN(nvnCommandBufferSetViewport, void, NVNcommandBuffer *c, int x, int y, int w, int h) { float v[4] = {(float)x, (float)y, (float)w, (float)h}; h_nvnCommandBufferSetViewports(c, 0, 1, v); }
NVN(nvnCommandBufferSetScissors, void, NVNcommandBuffer *c, int first, int n, const int *r) {
    if (first == 0 && n >= 1) h_nvnCommandBufferSetScissor(c, r[0], r[1], r[2], r[3]);
    if (first + n > 1) ONCE_LOG("SetScissors first=%d n=%d: the host uses only index 0", first, n);
}
NVN(nvnCommandBufferSetDepthRanges, void, NVNcommandBuffer *c, int first, int n, const float *r) {
    if (first == 0 && n >= 1) h_nvnCommandBufferSetDepthRange(c, r[0], r[1]);
    if (first + n > 1) ONCE_LOG("SetDepthRanges first=%d n=%d: the host uses only index 0", first, n);
}

// GPU value write: ReportValue = write a 32-bit value to the address when this point is reached (the replay layer's event write, applied on completion)
NVN(nvnCommandBufferReportValue, void, NVNcommandBuffer *c, uint32_t v, NVNbufferAddress a) {
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_SIGNAL_EVENT, 24);
    volatile uint32_t *val = (volatile uint32_t *)(uintptr_t)a; uint32_t mm = NVN_EVENT_SIGNAL_MODE_WRITE;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
// Clear a buffer to a 32-bit value: one OP_CLEAR_BUF record, filled in order within the submission at replay (fillBuffer when all bytes match, otherwise copied from a pattern buffer)
NVN(nvnCommandBufferClearBuffer, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n, uint32_t v) { *REC(c, OP_CLEAR_BUF, p_clear_buf) = (p_clear_buf){a, n, v, 0}; }

// State the replay layer does not have yet: logged once (conditional rendering is treated as "always draw")
// Render enable (profile render_cond, see OP_RENDER_COND in nvn_mtl.m): recorded and evaluated at replay; without the key everything draws, as before
NVN(nvnCommandBufferSetRenderEnable, void, NVNcommandBuffer *c, uint32_t en) {
    if (DM_GAME_RENDER_COND) *REC(c, OP_RENDER_COND, p_render_cond) = (p_render_cond){(en & 0xff) ? 2 : 3, 0, 0};
    else if (!(en & 0xff)) ONCE_LOG("[RED] SetRenderEnable(false): the host draws anyway for now");
}
NVN(nvnCommandBufferSetRenderEnableConditional, void, NVNcommandBuffer *c, NVNconditionalRenderMode m, NVNbufferAddress a) {
    if (DM_GAME_RENDER_COND) *REC(c, OP_RENDER_COND, p_render_cond) = (p_render_cond){(int)m & 1, 0, a};
    else ONCE_LOG("[RED] SetRenderEnableConditional: the host draws anyway for now");
}
// Fire Emblem: Three Houses turns depth clamp on for its scenes; ignored, the ground right under its close-up battle cameras was clipped away and the lighting pass painted the sky's fog colour there
NVN(nvnCommandBufferSetDepthClamp, void, NVNcommandBuffer *c, uint32_t en) { REC(c, OP_DEPTH_CLAMP, p_depth_clamp)->on = (en & 0xff) != 0; }
// Absolum (FNA3D's NVN driver) keeps the lower-left window origin and sets viewport 0's swizzle every frame (NEGATIVE_Y: clip y negated, how a GL-heritage renderer flips its picture).
// Ignored, its whole picture came out upside down. The replay applies it to the viewport (draw in nvn_mtl.m)
NVN(nvnCommandBufferSetViewportSwizzles, void, NVNcommandBuffer *c, int first, int n, const NVNviewportSwizzle *s) {
    if (n < 0 || first < 0 || first + n > 16) { LOG("[RED] SetViewportSwizzles first=%d n=%d", first, n); return; }
    p_vp_swizzle *r = REC(c, OP_VP_SWIZZLE, p_vp_swizzle); r->first = first; r->n = n;
    for (int i = 0; i < n; i++) for (int k = 0; k < 4; k++) r->sw[i][k] = (uint8_t)((s[4 * i + k] ^ 2 * k) & 7);
}
NVN(nvnCommandBufferSetAlphaRef, void, NVNcommandBuffer *c, float r) { (void)c; ONCE_LOG("SetAlphaRef %.3f: the host has no alpha test (ColorStateSetAlphaTest is only recorded)", r); }
NVN(nvnCommandBufferSetLineWidth, void, NVNcommandBuffer *c, float w) { (void)c; if (w != 1.0f) ONCE_LOG("SetLineWidth %.2f: Metal has only 1-pixel lines", w); }
NVN(nvnCommandBufferSetPointSize, void, NVNcommandBuffer *c, float s) { (void)c; (void)s; ONCE_LOG("SetPointSize: the point size comes from the shader"); }
NVN(nvnCommandBufferSetPrimitiveRestart, void, NVNcommandBuffer *c, uint32_t en, int idx) { *REC(c, OP_PRIM_RESTART, p_prim_restart) = (p_prim_restart){(en & 0xff) != 0, (uint32_t)idx}; }
// Rasterizer discard: draws run their vertex stage only (with transform feedback, the capture); recorded, and at replay such a draw rasterizes nothing (nvn_mtl.m)
NVN(nvnCommandBufferSetRasterizerDiscard, void, NVNcommandBuffer *c, uint32_t en) { REC(c, OP_RAST_DISCARD, p_rast_discard)->on = (en & 0xff) != 0; }
NVN(nvnCommandBufferSetSampleMask, void, NVNcommandBuffer *c, int m) { (void)c; if ((m & 0xffff) != 0xffff) ONCE_LOG("SetSampleMask %#x: ignored by the host for now", m); }
NVN(nvnCommandBufferSetConservativeRasterEnable, void, NVNcommandBuffer *c, uint32_t en) { (void)c; if (en & 0xff) ONCE_LOG("SetConservativeRasterEnable on: ignored by the host for now"); }
NVN(nvnCommandBufferSetTiledCacheAction, void, NVNcommandBuffer *c, NVNtiledCacheAction a) { (void)c; (void)a; }        // Tegra tiled cache hint, no Metal equivalent
// A bound target's contents may be discarded: they are undefined afterwards, so keeping them is correct (The Great Ace Attorney Chronicles discards colour target 0)
NVN(nvnCommandBufferDiscardColor, void, NVNcommandBuffer *c, int i) { (void)c; (void)i; }
NVN(nvnCommandBufferDiscardDepthStencil, void, NVNcommandBuffer *c) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheTileSize, void, NVNcommandBuffer *c, int w, int h) { (void)c; (void)w; (void)h; }
NVN_NOPROTO(nvnCommandBufferSetColorReductionEnable, void, NVNcommandBuffer *c, uint32_t en) { (void)c; (void)en; }   // color compression hints
NVN_NOPROTO(nvnCommandBufferSetColorReductionThresholds, void, NVNcommandBuffer *c, int cls, const int *t) { (void)c; (void)cls; (void)t; }
// Resolve a multisample texture into a single-sample one (whole texture): recorded as a texture copy with DM_COPY_RESOLVE; replay uses Metal's multisample resolve (copy_t2t in nvn_mtl.m)
static void rec_downsample(NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv, uint64_t ra) {
    p_copy_t2t *o = REC_SNAP(c, OP_COPY_T2T, p_copy_t2t, 2);
    o->src = tex_snap(o, SNAP_OFF(p_copy_t2t), 0, s, ra); o->dst = tex_snap(o, SNAP_OFF(p_copy_t2t), 1, d, ra);
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv);
    if (o->src) o->sr = (NVNcopyRegion){0, 0, 0, (int)o->src->d.width, (int)o->src->d.height, 1};
    if (o->dst) o->dr = (NVNcopyRegion){0, 0, 0, (int)o->dst->d.width, (int)o->dst->d.height, 1};
    o->flags = DM_COPY_RESOLVE;
}
NVN(nvnCommandBufferDownsample, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtexture *d) { rec_downsample(c, s, NULL, d, NULL, (uint64_t)__builtin_return_address(0)); }
NVN(nvnCommandBufferDownsampleTextureView, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv) {
    rec_downsample(c, s, sv, d, dv, (uint64_t)__builtin_return_address(0));
}

// CPU texture write (NVN WriteTexels): texture storage lives in pool memory in Tegra block-linear layout; decode the whole level → patch the region → re-encode.
// Write protection is lifted and the range marked dirty first, so the replay layer re-uploads it the next time it is used
// Non-3D textures: only the bytes inside the region are written, by block-linear address, without decoding the whole level. Metroid Dread's minimap (main+0x160e80) calls WriteTexels once per texel (a 1×1 region);
// previously every call decoded, patched and re-encoded the whole level, and after a few minutes the main thread spent whole frames there (frames stopped from 175 s). Returns −1 when the region is out of range or the format unsupported, falling back to the whole-level path
static int write_region_bl(const tex_t *t, unsigned level, unsigned layer, const NVNcopyRegion *r, const uint8_t *src, size_t row, int bw, int bh, int bpb) {
    uint32_t row_bytes, rows, slices, gbpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(&t->d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &gbpb) || (int)gbpb != bpb || r->xoffset < 0 || r->yoffset < 0) return -1;
    uint32_t x0 = (uint32_t)(r->xoffset / bw) * (uint32_t)bpb, y0 = (uint32_t)(r->yoffset / bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * (size_t)bpb, hb = (size_t)((r->height + bh - 1) / bh);
    if (x0 + wb > row_bytes || y0 + hb > rows) return -1;
    uint32_t lb = nvn_tex_level_gobs(&t->d, level) * 64; if (lb < row_bytes) lb = row_bytes;   // the row as laid out (padded in a sparse texture's tiled levels)
    for (size_t y = 0; y < hb; y++)
        for (size_t x = 0; x < wb;) {   // 16-byte runs are contiguous within a GOB
            uint32_t bx = x0 + (uint32_t)x; size_t n = 16 - bx % 16; if (n > wb - x) n = wb - x;
            memcpy(t->mem + base + nvn_bl_offset(bx, y0 + (uint32_t)y, 0, lb, rows, th, td), src + y * row + x, n); x += n;
        }
    return 0;
}
static void write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    if (!t->mem) { LOG("[RED] WriteTexels: texture has no storage"); return; }
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { ONCE_LOG("[RED] WriteTexels: unknown format %#x", t->d.format); return; }
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D, nz = r->depth ? r->depth : 1;
    if (!is3d) {
        size_t wb0 = (size_t)((r->width + bw - 1) / bw) * bpb, row0 = row ? row : wb0, img0 = img ? img : row0 * (size_t)((r->height + bh - 1) / bh);
        dm_guest_write_prepare(t->mem, t->L.size);
        int ok = 1;
        for (int z = 0; z < nz && ok; z++) ok = !write_region_bl(t, level, layer0 + (unsigned)(r->zoffset + z), r, src + (size_t)z * img0, row0, bw, bh, bpb);
        if (ok) { mtl_tex_cpu_written(t); return; }
    }
    uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1, lz = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (size_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    uint8_t *lin = malloc(lp * lr * lz); if (!lin) die("WriteTexels allocation failed");
    dm_guest_write_prepare(t->mem, t->L.size);
    // when the region is exactly the whole level (2D) every texel is overwritten, so the old content need not be decoded first (movies write whole frames; saves one deswizzle per 1080p frame)
    int whole = !is3d && !r->xoffset && !r->yoffset && (uint32_t)r->width == lw && (uint32_t)r->height == lh;
    for (int z = 0; z < nz; z++) {
        unsigned layer = is3d ? 0 : layer0 + (unsigned)(r->zoffset + z);
        if (((is3d && z == 0) || !is3d) && !whole) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { ONCE_LOG("[RED] WriteTexels deswizzle failed (format %#x target %d)", t->d.format, t->d.target); free(lin); return; }
        uint8_t *dst = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0) + (size_t)(r->yoffset / bh) * lp + (size_t)(r->xoffset / bw) * bpb;
        for (size_t y = 0; y < hb; y++) memcpy(dst + y * lp, src + (size_t)z * img + y * row, wb);
        if (!is3d || z == nz - 1) nvn_swizzle(&t->d, level, layer, lin, lp, t->mem);
    }
    free(lin);
    mtl_tex_cpu_written(t);
}
NVN(nvnTextureWriteTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src) { write_texels(CTX(t), (const view_t *)v, r, src, 0, 0); }
// with the source's row and image strides in bytes (0 = tightly packed, as WriteTexels)
NVN(nvnTextureWriteTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src, ptrdiff_t row, ptrdiff_t img) {
    write_texels(CTX(t), (const view_t *)v, r, src, (size_t)row, (size_t)img);
}
// The movie decoder's native-texture output (DecoderMode 2, hle_movie.c): a whole frame into level 0 of the client texture, as WriteTexels does; returns the texture format, or −1 when the object is not a texture
int nvn_host_write_texture(const void *nvntex, const void *src, int w, int h, size_t row, int *tw, int *th) {
    const tex_t *t = nvntex;
    uint32_t mg = 0; vm_size_t got = 0;   // the value the client gives is not necessarily a pointer: test-read first
    if (!t || (uintptr_t)t & 7 || vm_read_overwrite(mach_task_self(), (vm_address_t)t, 4, (vm_address_t)&mg, &got) != KERN_SUCCESS || got != 4) return -1;
    if (t->magic != MAGIC('T', 'E', 'X', 0)) return -1;
    if (tw) *tw = (int)t->d.width;
    if (th) *th = (int)t->d.height;
    if (src) { NVNcopyRegion r = {0, 0, 0, w, h, 1}; write_texels(t, NULL, &r, src, row, 0); }
    return (int)t->d.format;
}
// work the host finished on the CPU: the sync object counts as signalled at once (fence 0 is always complete)
int nvn_host_signal_sync(void *nvnsync) {
    sync_t *o = nvnsync;
    if (!o || o->magic != MAGIC('S', 'Y', 'N', 'C')) return -1;
    o->fence = 0; o->fenced = 1; return 0;
}
NVN(nvnTextureFlushTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }   // unified memory + dirty page tracking: nothing to flush
