#include <malloc/malloc.h>
// NVN 接口层：接管 main 导入的 nvnBootstrapLoader，sdk 里的 NVN 驱动整块不执行。
// 523 个 NVN 名字一律返回计数跳板（nvn_thunks.S），跳板再转到这里的实现：
//   · 有语义的（设备 / 内存池 / 贴图与采样器 / 队列 / 窗口 / 同步 / 事件 / 命令缓冲内存）按 NVN 语义实现，对象是真的；
//   · 只改 GPU 状态、没有 CPU 可见结果的（状态对象、绑定、绘制、清除…）录成命令，提交时回放；
//   · 其余（main 只加载不调用的 307 个 + 头文件无原型的）接 nvn_unimpl_entry：被调到即报红打印名字。
// GPU 地址 = CPU 地址（内存池用游戏给的真内存，同时建成零拷贝 MTLBuffer）。命令录进游戏给的命令内存，提交时回放成
// Metal 命令缓冲（nvn_mtl.m）：缓冲拷贝 / uniform 更新走 blit（GPU 时序），事件写值 / 计数器在 GPU 完成后写；
// 同步对象 = 提交栅栏值，等待只在 GPU 完成后放行。
// 每个实现都按 nvn_protos.h 的原型做编译期签名检查（NVNboolean 参数按 uint32_t 收、只看低 8 位）。
// 环境开关：DM_NVN_FRAMES=N 前 N 帧逐帧打印调用表（默认 3）；DM_NVN_TSV=路径 每帧调用表与计时写 TSV（DM_NVN_TSV_CALLS=0 只写计时）；DM_SLOWFRAME=1 只把掉帧的逐帧分项写进日志；DM_NVN_LOG=1 记对象创建。
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
#include "nvn_hmap.h"
#include "nvn_mtl.h"
#include "nvn_pace.h"
#include "nvn_diag_range.h"
#include "../../host/front.h"
#include "nvn_dirty.h"
#include "totk_vpool.h"
#include "totk_lifelog.h"
#include <mach/mach_time.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <TargetConditionals.h>
#include <unistd.h>
#if TARGET_OS_OSX
#define DM_MEM_CAP_DEFAULT_GB 24
#else
#define DM_MEM_CAP_DEFAULT_GB 0
#endif
#include <pthread/qos.h>
#include <sys/resource.h>
#include <mach/thread_policy.h>
#include <assert.h>
#include <stdatomic.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

// ---------- 槽表、跳板、未实现桩 ----------
// ---------- Slot table, thunks, unimplemented stubs ----------
typedef struct { _Atomic uint64_t cnt; void *fn; } nvn_slot_t;
nvn_slot_t nvn_slot[NVN_NFUNC];                 // 跳板按 i*16 访问，布局不可改 | The thunk accesses it by i*16; the layout must not change
_Static_assert(sizeof(nvn_slot_t) == 16, "跳板按 16 字节步长寻址");
extern void *nvn_thunk[NVN_NFUNC];
extern const char *nvn_names[NVN_NFUNC];
extern void nvn_unimpl_entry(void);
extern void nvn_prof_entry(void);
void *nvn_real[NVN_NFUNC];               // DM_NVN_PROF：计时包装调用的真实现 | DM_NVN_PROF: the real implementation called by the timing wrapper
_Atomic uint64_t nvn_prof_ticks[NVN_NFUNC]; // DM_NVN_PROF：各函数累计耗时（cntvct 计数） | DM_NVN_PROF: cumulative time of each function (cntvct counts)
static uint8_t g_unimpl_seen[NVN_NFUNC];

uint64_t nvn_unimpl_c(nvn_slot_t *slot, uint64_t x0, uint64_t x1, uint64_t x2) {
    unsigned i = (unsigned)(slot - nvn_slot);
    if (!g_unimpl_seen[i]++) LOG("【红】NVN 未实现被调用：%s (x0=%#llx x1=%#llx x2=%#llx)", nvn_names[i], x0, x1, x2);
    die("unimplemented NVN call %s",nvn_names[i]);
}

#include "nvn_impl.h"

static int g_log;
uint64_t nvn_new_serial(void) { static _Atomic uint64_t n; return ++n; }
static unsigned g_nsem;                          // 语义实现个数（nvn_init_slots 填） | Number of semantic implementations (filled by nvn_init_slots)
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
        if (i == NVN_NFUNC) die("NVN 实现 %s 不在 main 的名字表里", im[k].name);
        nvn_slot[i].fn = im[k].fn; hit++;
    }
    g_nsem = hit;
}

// TOTK: resolve an implemented entry by name (for NVN_EXTRA fallbacks that forward to regular entries).
void *nvn_slot_fn(const char *name) {
    nvn_init_slots();
    for (unsigned i = 0; i < NVN_NFUNC; i++) if (!strcmp(nvn_names[i], name)) return nvn_slot[i].fn;
    die("nvn_slot_fn: unknown %s", name);
}
void *runtime_hle_wrap(void *fn);   // runtime/hle_stack.c: run on the calling guest thread's host stack
void *runtime_hle_wrap_named(void *fn, const char *name);   // the same, with the NVN name for diagnostics (hletrace)
static void *nvn_lookup(const char *name) {
    nvn_init_slots();
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (!strcmp(nvn_names[i], name)) return nvn_thunk[i];
    unsigned long sz = 0;
    const nvn_impl *ex = (const nvn_impl *)getsectiondata(&_mh_execute_header, "__DATA", "totknvnx", &sz);
    for (unsigned k = 0; ex && name && k < sz / sizeof *ex; k++) if (!strcmp(ex[k].name, name)) return ex[k].fn;
    LOG("【红】NVN 取址未知名字：%s（未实现：若被调用会跳到空地址）", name ? name : "(null)");
    return NULL;
}

// nvnBootstrapLoader 由 main 导入（sdk 定义）；强制接管后 sdk 的 NVN 驱动不再被引用
// nvnBootstrapLoader is imported by main (defined in sdk); after it is forcibly taken over, the NVN driver in sdk is no longer referenced
HLE_FORCE("nvnBootstrapLoader", "NVN 图形接口整块换成宿主空设备（后续 Metal），sdk 的 NVN 驱动不执行",
          void *, h_nvnBootstrapLoader, const char *name) { return runtime_hle_wrap_named(nvn_lookup(name), name); }

// ---------- 布尔参数：Switch 调用方不扩展，只看低 8 位 ----------
// ---------- Boolean parameters: Switch callers do not extend them, only look at the low 8 bits ----------
#define B(x) ((x) & 0xff)

#include "nvn_obj.h"
#include "totk_timeline.h"
void mtl_args_stats(uint64_t *enc, uint64_t *reuse);

// ---------- 设备 ----------
// ---------- Device ----------
// TOTK translation thread (see "translation thread" below)
static uint64_t fence_now(int *token);
static int fence_wait(uint64_t k, uint64_t timeout_ns);
static void x_drain(void);
static int async_replay(void);
static int call_stats_on(void);
static void call_stats_end(const void *cb, const uint8_t *copy);
NVN(nvnDeviceBuilderSetDefaults, void, NVNdeviceBuilder *b) { memset(b, 0, sizeof(dev_builder_t)); }
NVN(nvnDeviceBuilderSetFlags, void, NVNdeviceBuilder *b, int f) { ((dev_builder_t *)b)->flags = f; }
NVN(nvnDeviceInitialize, NVNboolean, NVNdevice *d, const NVNdeviceBuilder *b) {
    dev_t_ *o = (dev_t_ *)d; memset(o, 0, sizeof *o);
    o->magic = MAGIC('D', 'E', 'V', 0); o->flags = ((const dev_builder_t *)b)->flags;
    OLOG("DeviceInitialize %p flags=%#x", (void *)d, o->flags);
    return 1;
}
NVN(nvnDeviceFinalize, void, NVNdevice *d) { ((dev_t_ *)d)->magic = 0; }
NVN(nvnDeviceGetProcAddress, PFNNVNGENERICFUNCPTRPROC, const NVNdevice *d, const char *name) {
    (void)d; return (PFNNVNGENERICFUNCPTRPROC)runtime_hle_wrap_named(nvn_lookup(name), name);
}

// 设备属性：Tegra X1 实测值（nnheaders nvn_DeviceConstantsNX.h，在 SMO 上逐项调 nvnDeviceGetInteger 得到）
// Device properties: values measured on Tegra X1 (nnheaders nvn_DeviceConstantsNX.h, obtained by calling nvnDeviceGetInteger item by item on SMO)
static const int g_devinfo[] = {
#ifdef GAME_BOTW
    [0x00] = 53, [0x01] = 311,  // Breath of the Wild 1.6.0 main+0xe55304 requires major 53, minor >= 311 (botw.md).
#else
    [0x00] = 55, [0x01] = 15,   // Experimental ABI target: TOTK main+e9d614 requires major 55, minor >= 15. Unsupported functions stop.
#endif
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
NVN(nvnDeviceGetInteger, void, const NVNdevice *d, NVNdeviceInfo what, int *out) {
    (void)d;
    unsigned w = (unsigned)what;
    // 0x5F 分离采样器绑定数、0x60/0x61 在 SMO 实测表里没有：0x5F 按采样器池上限给 4096 未核，0x60/0x61 报红给 0
    // 0x5F separate sampler binding count and 0x60/0x61 are not in the table measured on SMO: 0x5F is given 4096 per the sampler pool limit, unverified; 0x60/0x61 report a failure and return 0
    if (w >= sizeof g_devinfo / sizeof *g_devinfo) { die("unsupported NVN device property %#x", w); }
    *out = g_devinfo[w];
    /* DM_NVN_GPU_CODE_MAX_MINOR (14..31): the newest GLSLC GPU code minor version accepted. The measured driver says 14;
       Bayonetta 3 mixes 1.15, 1.16 and maybe newer binaries, and its runtime sets 31. TOTK keeps the measured value. */
    if (w == NVN_DEVICE_INFO_GLSLC_MAX_SUPPORTED_GPU_CODE_MINOR_VERSION) {
        const char *e = getenv("DM_NVN_GPU_CODE_MAX_MINOR");
        if (e) { int v = atoi(e); if (v < 14 || v > 31) die("DM_NVN_GPU_CODE_MAX_MINOR must be 14..31"); *out = v; }
    }
    OLOG("DeviceGetInteger %#x -> %d", w, *out);
}
/* Metal chooses fast-clear encodings itself; this backend has no programmable
 * NVN fast-clear registry. Report unavailable so the game retains normal clears. */
NVN(nvnDeviceRegisterFastClearColor, NVNboolean, NVNdevice *d,const float *color,NVNformat format){(void)d;(void)color;(void)format;return 0;}
// TOTK: fast-clear registration is an optimisation hint; "not registered" keeps the normal clear path.
NVN(nvnDeviceRegisterFastClearColori, NVNboolean, NVNdevice *d, const int *c, NVNformat f) { (void)d; (void)c; (void)f; return 0; }
NVN(nvnDeviceRegisterFastClearColorui, NVNboolean, NVNdevice *d, const uint32_t *c, NVNformat f) { (void)d; (void)c; (void)f; return 0; }
NVN_NOPROTO(nvnDeviceRegisterFastClearDepth, NVNboolean, NVNdevice *d) { (void)d; return 0; }
NVN(nvnDeviceGetImageHandle, NVNimageHandle, const NVNdevice *d, int id) { (void)d; return (NVNimageHandle)id; }
// 分离贴图 / 采样器句柄：值 = 描述符号（采样器号放 20 位以上，与组合句柄的位布局一致）
// Separate texture / sampler handles: value = descriptor index (the sampler index goes in bit 20 and above, consistent with the bit layout of combined handles)
// TOTK: combined handle = texture id (20 bits) | sampler id << 20, the encoding the replay layer decodes.
NVN(nvnDeviceGetTextureHandle, NVNtextureHandle, const NVNdevice *d, int tid, int sid) {
    (void)d; if (tid < 0 || tid >= (1 << 20) || sid < 0 || sid >= (1 << 12)) die("texture handle ids out of range %d/%d", tid, sid);
    return (NVNtextureHandle)((uint64_t)tid | (uint64_t)sid << 20);
}
NVN(nvnDeviceGetTexelFetchHandle, NVNtextureHandle, const NVNdevice *d, int tid) {
    (void)d; if (tid < 0 || tid >= (1 << 20)) die("texel fetch id out of range %d", tid); return (NVNtextureHandle)tid;
}
NVN(nvnDeviceGetSeparateTextureHandle, NVNseparateTextureHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateTextureHandle){(uint64_t)id}; }
NVN(nvnDeviceGetSeparateSamplerHandle, NVNseparateSamplerHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateSamplerHandle){(uint64_t)id << 20}; }
NVN(nvnDeviceInstallDebugCallback, void, NVNdevice *d, const PFNNVNDEBUGCALLBACKPROC cb, void *data, uint32_t enable) {
    dev_t_ *o = (dev_t_ *)d; o->dbg_cb = B(enable) ? (void *)cb : NULL; o->dbg_data = data;
}
NVN(nvnDeviceSetWindowOriginMode, void, NVNdevice *d, NVNwindowOriginMode m) { ((dev_t_ *)d)->origin = m; LOG("[nvn] 窗口原点 %s", m ? "左上（同 D3D / Metal）" : "左下（GL）"); }
#ifdef GAME_BOTW
/* NVN's default depth mode is NEAR_IS_MINUS_W (enum value 0, the OpenGL convention): a device the game never calls
   nvnDeviceSetDepthMode on keeps it. TOTK sets it explicitly; Breath of the Wild never does, and its full-screen copy
   to the swapchain has clip z = -1 (the near plane), which the 0..w convention clipped away: the window stayed magenta
   (botw.md). */
static _Atomic int g_depth_near_minus_w = 1;
#else
static _Atomic int g_depth_near_minus_w;
#endif
int totk_depth_near_minus_w(void) { return atomic_load(&g_depth_near_minus_w); }
NVN(nvnDeviceSetDepthMode, void, NVNdevice *d, NVNdepthMode m) { ((dev_t_ *)d)->depth_mode = m; atomic_store(&g_depth_near_minus_w, m == NVN_DEPTH_MODE_NEAR_IS_MINUS_W); LOG("[nvn] 深度模式 %s", m ? "近=0（同 D3D / Metal）" : "近=-w（GL）：顶点着色器翻译时做 z=(z+w)/2 转换"); }
NVN(nvnDeviceFinalizeCommandHandle, void, NVNdevice *d, NVNcommandHandle h) { (void)d; (void)h; }

// ---------- 内存池 ----------
// ---------- Memory pools ----------
NVN(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) { memset(b, 0, sizeof(pool_builder_t)); }
NVN(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) { ((pool_builder_t *)b)->dev = d; }
NVN(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *m, size_t n) { ((pool_builder_t *)b)->mem = m; ((pool_builder_t *)b)->size = n; }
NVN(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int f) { ((pool_builder_t *)b)->flags = f; }
// TOTK: logging switches checked on per-call paths (command memory, recording, submits) are read once.
static int cmdlog_env(void) { static int on = -1; if (on < 0) on = getenv("TOTK_CMD_LOG") != NULL; return on; }
static int poollog_env(void) { static int on = -1; if (on < 0) on = getenv("TOTK_POOL_LOG") != NULL; return on; }
#ifdef GAME_BOTW
static void guest_chain(const char *what);
#endif
NVN(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    const pool_builder_t *s = (const pool_builder_t *)b; pool_t *o = (pool_t *)p;
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('P', 'O', 'O', 'L'); o->flags = s->flags; o->mem = s->mem; o->size = s->size;
    if ((o->flags & (NVN_MEMORY_POOL_FLAGS_PHYSICAL | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) && !totk_vpool_init(o)) { o->magic = 0; return 0; }
    int gpu_visible = !(o->flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS);
    if (o->mem && gpu_visible) { mtl_pool_add(o->mem, o->size); nvn_dirty_add_range(o->mem, o->size); }
    if (!o->mem) LOG("【红】MemoryPoolInitialize 池没有存储 flags=%#x size=%#llx", o->flags, o->size);
    OLOG("MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, o->size, o->flags);
    if (poollog_env()) LOG("[pool] %p mem=%p..%p size=%#llx flags=%#x game=%p", (void *)p, (void *)o->mem, (void *)(o->mem + o->size), o->size, o->flags, (void *)o->game_mem);
#ifdef GAME_BOTW
    { static long want = -2; if (want == -2) { const char *e = getenv("TOTK_POOL_CHAIN"); want = e ? strtol(e, NULL, 0) : -1; }   /* diagnostics: TOTK_POOL_CHAIN=<size> logs who makes pools of that size */
      if (want >= 0 && (long)o->size == want) { LOG("[pool] %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, o->size, o->flags); guest_chain("pool of the watched size"); } }
#endif
    return o->mem != NULL;
}
NVN(nvnMemoryPoolFinalize, void, NVNmemoryPool *p) {
    pool_t *o = (pool_t *)p;
    if (o->mem && !(o->flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS)) { x_drain(); nvn_dirty_remove_range(o->mem); mtl_pool_del(o->mem); }
    totk_vpool_fini(o); o->magic = 0;
}
NVN(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }
NVN(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p){return (NVNmemoryPoolFlags)((const pool_t *)p)->flags;}
NVN(nvnMemoryPoolMap, void *, const NVNmemoryPool *p){const pool_t *v=(const void *)p;
#ifdef GAME_BOTW
 if(v->magic!=MAGIC('P','O','O','L')||(v->flags&NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)||!v->mem){static int n;if(n++<12)LOG("【红】nvnMemoryPoolMap(%p) -> NULL: magic %#x flags %#x mem %p size %#llx",(void *)p,v->magic,v->flags,(void *)v->mem,(unsigned long long)v->size);}
#endif
 if(v->magic!=MAGIC('P','O','O','L'))return NULL;return (v->flags&NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)?NULL:v->mem;}
NVN(nvnMemoryPoolMapVirtual, NVNboolean, NVNmemoryPool *p, int n, const NVNmappingRequest *r) {
    return (NVNboolean)totk_vpool_map((pool_t *)p, n, (const totk_mapping_request *)(const void *)r);
}
NVN(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) { return (NVNbufferAddress)((const pool_t *)p)->mem; }

// Buffer objects reference their pool; they do not own the backing allocation.
typedef struct{NVNdevice *dev;pool_t *pool;int64_t off;uint64_t size;} totk_buffer_builder;
typedef struct{uint32_t magic;pool_t *pool;int64_t off;uint64_t size;} totk_buffer;
FITS(totk_buffer_builder,NVNbufferBuilder);FITS(totk_buffer,NVNbuffer);
NVN(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *b,NVNdevice *d){((totk_buffer_builder *)b)->dev=d;}
NVN(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b){NVNdevice *d=((totk_buffer_builder *)b)->dev;memset(b,0,sizeof(totk_buffer_builder));((totk_buffer_builder *)b)->dev=d;}
NVN(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b,NVNmemoryPool *p,ptrdiff_t off,size_t size){totk_buffer_builder *v=(void *)b;v->pool=(void *)p;v->off=off;v->size=size;}
NVN(nvnBufferInitialize, NVNboolean, NVNbuffer *b,const NVNbufferBuilder *builder){const totk_buffer_builder *v=(const void *)builder;
#ifdef GAME_BOTW
 /* Breath of the Wild creates a 256 KiB buffer at the start of a 144 KiB pool and never checks the result: the driver does
    not compare the two (the buffer is an address and a size), so the buffer is made as asked and only its first part is
    backed by the pool (botw.md). Refusing it left the game's NVNbuffer uninitialized; the first GetAddress read garbage. */
 if(v->pool&&v->pool->magic==MAGIC('P','O','O','L')&&v->off>=0&&(uint64_t)v->off<=v->pool->size&&v->size>v->pool->size-(uint64_t)v->off){
  static int n;if(n++<8){LOG("[nvn] buffer of %llu bytes at offset %lld of a %llu-byte pool %p (mem %p, flags %#x): made as asked (the driver does not check)",(unsigned long long)v->size,(long long)v->off,(unsigned long long)v->pool->size,(void *)v->pool,(void *)v->pool->mem,v->pool->flags);guest_chain("oversized buffer");}
  *(totk_buffer *)b=(totk_buffer){MAGIC('B','U','F',0),v->pool,v->off,v->size};return 1;}
#endif
 if(!v->pool||v->pool->magic!=MAGIC('P','O','O','L')||v->off<0||(uint64_t)v->off>v->pool->size||v->size>v->pool->size-(uint64_t)v->off)return 0;*(totk_buffer *)b=(totk_buffer){MAGIC('B','U','F',0),v->pool,v->off,v->size};return 1;}
NVN(nvnBufferFinalize, void, NVNbuffer *b){memset(b,0,sizeof(totk_buffer));}
NVN(nvnBufferMap, void *, const NVNbuffer *b){const totk_buffer *v=(const void *)b;
#ifdef GAME_BOTW
 if(!(v->pool&&v->pool->mem)){static int n;if(n++<12)LOG("【红】nvnBufferMap(%p) -> NULL: magic %#x pool %p offset %lld size %llu",(void *)b,v->magic,(void *)v->pool,(long long)v->off,(unsigned long long)v->size);}
#endif
 return v->pool&&v->pool->mem?v->pool->mem+v->off:NULL;}
NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *b){return (uintptr_t)h_nvnBufferMap(b);}
NVN(nvnBufferGetMemoryPool, NVNmemoryPool *, const NVNbuffer *b){return (void *)((const totk_buffer *)b)->pool;}
NVN(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer *b){return ((const totk_buffer *)b)->off;}
NVN(nvnBufferGetSize, size_t, const NVNbuffer *b){return ((const totk_buffer *)b)->size;}
NVN(nvnBufferFlushMappedRange, void, const NVNbuffer *b,ptrdiff_t off,size_t size){const totk_buffer *v=(const void *)b;if(off<0||(uint64_t)off>v->size||size>v->size-(uint64_t)off)die("invalid NVN buffer flush range");dm_guest_write_prepare((uint8_t *)h_nvnBufferMap(b)+off,size);}

// ---------- 描述符池 ----------
// ---------- Descriptor pools ----------
/* Descriptor pools and their id ranges. Registrations are kept in one table per kind (mtl_reg_tex / mtl_reg_samp), so each
   texture or sampler pool gets its own range of ids there: the first pool initialised keeps ids as they are, a further
   pool starts after the ranges taken so far. A pool initialised again on the same memory keeps its range. Binds are
   translated into the range of the pool their command buffer set last (SetTexturePool / SetSamplerPool).
   Xenoblade Chronicles 3 draws its movies with a second texture pool (2048 entries) and sampler pool next to the main ones;
   with one shared range the movie's registrations replaced the game's textures of the same ids (the battle scene was black). */
typedef struct { const void *mem; int64_t off; int32_t n, base; } dpool_range;
static dpool_range g_dp_range[2][16]; static int g_dp_n[2]; static int32_t g_dp_next[2];
static pthread_mutex_t g_dp_mu = PTHREAD_MUTEX_INITIALIZER;
static int dpool_range_of(int kind, const void *mem, int64_t off, int32_t n) {   // kind 0 = textures, 1 = samplers
    pthread_mutex_lock(&g_dp_mu); int i = 0;
    while (i < g_dp_n[kind] && !(g_dp_range[kind][i].mem == mem && g_dp_range[kind][i].off == off)) i++;
    if (i == g_dp_n[kind]) {
        if (i == 16) die("more than 16 %s pools", kind ? "sampler" : "texture");
        g_dp_range[kind][i] = (dpool_range){mem, off, n, g_dp_next[kind]}; g_dp_n[kind]++; g_dp_next[kind] += n;
        if (g_dp_next[kind] > (kind ? (1 << 12) * 4 : 1 << 20)) die("%s pool ids out of range (%d)", kind ? "sampler" : "texture", g_dp_next[kind]);
        if (i) LOG("[nvn] %s pool %d of %d entries: ids from %d", kind ? "sampler" : "texture", i, n, g_dp_range[kind][i].base);
    } else if (n > g_dp_range[kind][i].n) die("%s pool %d initialised again with %d entries (was %d)", kind ? "sampler" : "texture", i, n, g_dp_range[kind][i].n);
    pthread_mutex_unlock(&g_dp_mu); return i;
}
static int32_t dpool_base(int kind, int idx) { return idx >= 0 && idx < g_dp_n[kind] ? g_dp_range[kind][idx].base : 0; }
NVN(nvnTexturePoolInitialize, NVNboolean, NVNtexturePool *tp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    int idx = dpool_range_of(0, p, off, n);
    *(dpool_t *)tp = (dpool_t){(const pool_t *)p, off, n, idx}; OLOG("TexturePool n=%d", n); mtl_set_tex_pool_n(dpool_base(0, idx) + n); return 1;
}
NVN(nvnTexturePoolFinalize, void, NVNtexturePool *tp) { (void)tp; }
extern uint64_t g_frame_pub;
NVN(nvnTexturePoolRegisterTexture, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) {
    { static long want = -2; if (want == -2) { const char *e = getenv("DM_NVN_REGLOG"); want = e ? atol(e) : -1; }   // TOTK diagnostic: DM_NVN_REGLOG=<id> logs every registration of that texture id
        static long rf0 = -2, rf1; if (rf0 == -2) { const char *e = getenv("DM_NVN_REGLOG_FRAMES"); rf0 = e ? atol(e) : -1; rf1 = e && strchr(e, '-') ? atol(strchr(e, '-') + 1) : rf0; }   // DM_NVN_REGLOG_FRAMES=a-b: log every registration in those frames
        if ((want >= 0 && id == want) || (rf0 >= 0 && (long)g_frame_pub >= rf0 && (long)g_frame_pub <= rf1)) { const tex_t *x = (const tex_t *)t; const view_t *vv = (const view_t *)v;
            LOG("[reglog] F%llu id=%d ser=%llu fmt=%#x %ux%u lv%u flags=%#x mem=%p pool=%p off=%lld view=%s vfmt=%#x vlv=%d+%d", (unsigned long long)g_frame_pub, id, (unsigned long long)x->serial, x->d.format, x->d.width, x->d.height, x->d.levels, x->d.flags, (void *)x->mem, (void *)x->pool, (long long)x->off,
                vv ? "yes" : "no", vv && vv->set_format ? vv->format : 0, vv && vv->set_levels ? vv->base_level : -1, vv && vv->set_levels ? vv->nlevels : 0); } }
    id += dpool_base(0, ((const dpool_t *)tp)->idx);
    totk_life_reg(id, (const tex_t *)t);
    mtl_reg_tex(id, (const tex_t *)t, (const view_t *)v); }

// Images and textures share one registration here (an image handle is the texture pool index, nvnDeviceGetImageHandle),
// so registering an image is registering the texture. TOTK never calls it; Bayonetta 3 does.
NVN(nvnTexturePoolRegisterImage, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) {
    h_nvnTexturePoolRegisterTexture(tp, id, t, v);
}
NVN(nvnSamplerPoolInitialize, NVNboolean, NVNsamplerPool *sp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)sp = (dpool_t){(const pool_t *)p, off, n, dpool_range_of(1, p, off, n)}; OLOG("SamplerPool n=%d", n); return 1;
}
NVN(nvnSamplerPoolFinalize, void, NVNsamplerPool *sp) { (void)sp; }
NVN(nvnTexturePoolGetMemoryPool, const NVNmemoryPool *, const NVNtexturePool *tp) { return (const NVNmemoryPool *)((const dpool_t *)tp)->pool; }
NVN(nvnTexturePoolGetMemoryOffset, ptrdiff_t, const NVNtexturePool *tp) { return (ptrdiff_t)((const dpool_t *)tp)->off; }
NVN(nvnTexturePoolGetSize, int, const NVNtexturePool *tp) { return ((const dpool_t *)tp)->n; }
NVN(nvnSamplerPoolGetMemoryPool, const NVNmemoryPool *, const NVNsamplerPool *sp) { return (const NVNmemoryPool *)((const dpool_t *)sp)->pool; }
NVN(nvnSamplerPoolGetMemoryOffset, ptrdiff_t, const NVNsamplerPool *sp) { return (ptrdiff_t)((const dpool_t *)sp)->off; }
NVN(nvnSamplerPoolGetSize, int, const NVNsamplerPool *sp) { return ((const dpool_t *)sp)->n; }
NVN(nvnSamplerPoolRegisterSampler, void, const NVNsamplerPool *sp, int id, const NVNsampler *s) { mtl_reg_samp(id + dpool_base(1, ((const dpool_t *)sp)->idx), &((const samp_t *)s)->p); }

// ---------- 贴图 ----------
// ---------- Textures ----------
#define TB(b) ((tex_builder_t *)(b))
NVN(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    NVNdevice *dev = TB(b)->dev; memset(b, 0, sizeof(tex_builder_t)); TB(b)->dev = dev;
    TB(b)->d.swizzle_r=2;TB(b)->d.swizzle_g=3;TB(b)->d.swizzle_b=4;TB(b)->d.swizzle_a=5;
    TB(b)->d.target = NVN_TEXTURE_TARGET_2D; TB(b)->d.width = TB(b)->d.height = TB(b)->d.depth = TB(b)->d.levels = 1;
}
NVN(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) { TB(b)->dev = d; }
NVN(nvnTextureBuilderSetFlags, void, NVNtextureBuilder *b, int f) { TB(b)->d.flags = f; }
NVN(nvnTextureBuilderSetTarget, void, NVNtextureBuilder *b, NVNtextureTarget t) { TB(b)->d.target = t; }
NVN(nvnTextureBuilderSetWidth, void, NVNtextureBuilder *b, int v) { TB(b)->d.width = v; }
static void size_chain(int w, int h);
NVN(nvnTextureBuilderSetHeight, void, NVNtextureBuilder *b, int v) { TB(b)->d.height = v; size_chain(TB(b)->d.width, v); }
NVN(nvnTextureBuilderSetDepth, void, NVNtextureBuilder *b, int v) { TB(b)->d.depth = v; }
NVN(nvnTextureBuilderSetSize1D, void, NVNtextureBuilder *b, int w) { TB(b)->d.width = w; TB(b)->d.height = TB(b)->d.depth = 1; }
static void guest_chain(const char *what);
NVN(nvnTextureBuilderSetSize2D, void, NVNtextureBuilder *b, int w, int h) { TB(b)->d.width=w;TB(b)->d.height=h;TB(b)->d.depth=1;
    /* TOTK_SIZE_CHAIN=<w>x<h> (diagnostic): the game call chain of the first 60 textures given this size (where render target sizes come from) */
    size_chain(w, h); }
static void size_chain(int w, int h) {
    static int sw = -1, sh, n; if (sw < 0) { const char *e = getenv("TOTK_SIZE_CHAIN"); if (!e || sscanf(e, "%dx%d", &sw, &sh) != 2) sw = 0; }
    if (sw && w == sw && h == sh && n++ < 60) { char m[64]; snprintf(m, sizeof m, "texture size %dx%d", w, h); guest_chain(m); }
}
NVN(nvnTextureBuilderSetSize3D, void, NVNtextureBuilder *b, int w, int h, int d) { TB(b)->d.width=w;TB(b)->d.height=h;TB(b)->d.depth=d; }
NVN(nvnTextureBuilderSetLevels, void, NVNtextureBuilder *b, int v) { TB(b)->d.levels = v; }
NVN(nvnTextureBuilderSetFormat, void, NVNtextureBuilder *b, NVNformat f) { TB(b)->d.format = f; }
// Xenoblade Chronicles 3: the mode is set on the builder, so the texture itself (with no view) samples its stencil plane;
// the replay layer reads it like a view's set_stencil (nvn_mtl.m, X32_Stencil8 of a Depth32Float_Stencil8 texture).
NVN(nvnTextureBuilderSetDepthStencilMode, void, NVNtextureBuilder *b,NVNtextureDepthStencilMode mode){if(mode!=NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH&&mode!=NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL)die("invalid NVN depth-stencil mode");TB(b)->d.stencil_mode=mode==NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL;}
NVN(nvnTextureBuilderSetSwizzle, void, NVNtextureBuilder *b,NVNtextureSwizzle r,NVNtextureSwizzle g,NVNtextureSwizzle blue,NVNtextureSwizzle a){if((unsigned)r>5||(unsigned)g>5||(unsigned)blue>5||(unsigned)a>5)die("invalid NVN texture swizzle");TB(b)->d.swizzle_r=r;TB(b)->d.swizzle_g=g;TB(b)->d.swizzle_b=blue;TB(b)->d.swizzle_a=a;}
NVN(nvnTextureBuilderSetSamples, void, NVNtextureBuilder *b, int s) { TB(b)->d.samples = s; }
NVN(nvnTextureBuilderSetStride, void, NVNtextureBuilder *b, ptrdiff_t s) { TB(b)->d.stride = (uint32_t)s; }
NVN(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t off) { TB(b)->pool = (pool_t *)p; TB(b)->off = off; }
NVN(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b, const NVNpackagedTextureLayout *l) {
    TB(b)->has_layout = l != NULL; if (l) memcpy(TB(b)->layout, l->layout, 8);
    TB(b)->d.pkg_block_h = l ? l->layout[0] + 1 : 0;
}
// TOTK: packaged (already GPU-laid-out) data copied into the texture's pool storage at Initialize; the copy is
// a CPU write into the pool, so dirty tracking re-uploads it like any other packaged texture content.
NVN(nvnTextureBuilderSetPackagedTextureData, void, NVNtextureBuilder *b, const void *data) { TB(b)->pkg_data = data; }
NVN(nvnTextureBuilderGetPackagedTextureData, const void *, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->pkg_data; }
/* Storage classes are opaque compatibility identifiers for this backend.
 * All encoded texture storage lives in byte-addressed host pools and is decoded
 * per texture descriptor; class 1 therefore shares that backing convention.
 * Buffer textures use class 0. This is not a Tegra memory-kind number.
 */
static NVNstorageClass totk_storage_class(const nvn_texdesc *d){return d->target==NVN_TEXTURE_TARGET_BUFFER?0:1;}
NVN(nvnTextureBuilderGetStorageClass, NVNstorageClass, const NVNtextureBuilder *b){return totk_storage_class(&((const tex_builder_t *)b)->d);}
NVN(nvnTextureBuilderGetFlags, NVNtextureFlags, const NVNtextureBuilder *b) { return (NVNtextureFlags)((const tex_builder_t *)b)->d.flags; }
static uint64_t g_layout_logged;
static void log_layout(const tex_builder_t *b, const nvn_texlayout *L) {
    if (b->has_layout && g_layout_logged++ < 16)
        LOG("[nvn] 预排布贴图 %ux%ux%u 级%u 格式%#x 目标%d 布局字节 %02x%02x%02x%02x%02x%02x%02x%02x → 块高 log2=%u（取布局第 0 字节）尺寸 %#llx",
            b->d.width, b->d.height, b->d.depth, b->d.levels, b->d.format, b->d.target, b->layout[0], b->layout[1], b->layout[2],
            b->layout[3], b->layout[4], b->layout[5], b->layout[6], b->layout[7], L->tileH, L->size);
}
NVN(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L);
    if (!L.bpb) LOG("【红】TextureBuilderGetStorageSize 未知格式 %#x", ((const tex_builder_t *)b)->d.format);
    log_layout((const tex_builder_t *)b, &L);
    { static int sizelog = -1; if (sizelog < 0) sizelog = getenv("DM_NVN_SIZELOG") != NULL;
      if (sizelog) { const nvn_texdesc *d = &((const tex_builder_t *)b)->d;
        fprintf(stderr, "[nvn-size] t=%d f=%#x fl=%#x s=%d w=%u h=%u d=%u lv=%u st=%u pbh=%d size=%#llx tileH=%u tileD=%u layer=%#llx\n",
            d->target, d->format, d->flags, d->samples, d->width, d->height, d->depth, d->levels, d->stride, d->pkg_block_h,
            (unsigned long long)L.size, L.tileH, L.tileD, (unsigned long long)L.layer_size); } }
    return L.size;
}
NVN(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L); return L.align;
}
#define TX(t) ((tex_t *)(t))
#define CTX(t) ((const tex_t *)(t))
static void live_put(const tex_t *o);
NVN(nvnTextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    const tex_builder_t *s = (const tex_builder_t *)b; tex_t *o = TX(t);
    if (o->magic == MAGIC('T', 'E', 'X', 0) && o->serial) {   // re-initialised without Finalize: release the previous serial's host texture, else it leaks
        static _Atomic unsigned n; if (atomic_fetch_add(&n, 1) < 3) LOG("[nvn] texture %p re-initialised without Finalize (serial %llu released)", (void *)t, (unsigned long long)o->serial);
        mtl_tex_release(o);
    }
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('T', 'E', 'X', 0); o->d = s->d; o->pool = s->pool; o->off = s->off;
    o->has_layout = s->has_layout; memcpy(o->layout, s->layout, 8);
    nvn_tex_layout(&o->d, &o->L);
    o->mem = s->pool && s->pool->mem ? s->pool->mem + s->off : NULL;
    if (s->pkg_data) {
        if (!o->mem) die("packaged texture data without CPU-visible storage");
        dm_guest_write_prepare(o->mem, (size_t)o->L.size);
        memcpy(o->mem, s->pkg_data, (size_t)o->L.size);
    }
    o->serial = nvn_new_serial();
    live_put(o);
    if (totk_life_match(o)) totk_life_log("init tex=%p ser=%llu mem=%p", (void *)t, (unsigned long long)o->serial, (void *)o->mem);
    OLOG("Texture %p %ux%ux%u lv%u fmt=%#x tgt=%d flags=%#x size=%#llx", (void *)t, o->d.width, o->d.height, o->d.depth,
         o->d.levels, o->d.format, o->d.target, o->d.flags, o->L.size);
    return 1;
}
/* TOTK: live texture by storage address. The game re-initialises transient textures (G-buffer etc.) every frame at the
   same pool memory without re-registering the texture-pool descriptor (an NVN descriptor refers to the memory, so it
   stays valid on hardware). A registration whose serial has since been finalised is redirected by the replay to the
   texture currently living at that address (nvn_tex_live_at). */
static nvn_hmap g_live_mem = {.klen = 8};   // storage address -> malloc'd tex_t copy of the live texture
static pthread_mutex_t g_live_mu = PTHREAD_MUTEX_INITIALIZER;
static void live_put(const tex_t *o) {
    if (!o->mem) return; uint64_t k = (uint64_t)(uintptr_t)o->mem; tex_t *c = malloc(sizeof *c); if (!c) die("live texture copy"); *c = *o;
    pthread_mutex_lock(&g_live_mu); tex_t *old = nvn_hm_put(&g_live_mem, &k, c); pthread_mutex_unlock(&g_live_mu); free(old);
}
static void live_del(const tex_t *o);
void live_del_x(const tex_t *o) { live_del(o); }
static void live_del(const tex_t *o) {
    if (!o->mem) return; uint64_t k = (uint64_t)(uintptr_t)o->mem;
    pthread_mutex_lock(&g_live_mu); tex_t *cur = nvn_hm_get(&g_live_mem, &k); if (cur && cur->serial == o->serial) { nvn_hm_del(&g_live_mem, &k); free(cur); } pthread_mutex_unlock(&g_live_mu);
}
int nvn_tex_live_at(const void *mem, tex_t *out) {
    if (!mem) return 0; uint64_t k = (uint64_t)(uintptr_t)mem;
    pthread_mutex_lock(&g_live_mu); tex_t *cur = nvn_hm_get(&g_live_mem, &k); if (cur) *out = *cur; pthread_mutex_unlock(&g_live_mu);
    return cur != NULL;
}
// TOTK translation thread: the release runs in queue order on a copy (only the serial and storage address are used), so
// a texture finalized right after a submit stays usable by that submit's replay, without waiting for the queue.
NVN(nvnTextureFinalize, void, NVNtexture *t) {
    if (totk_life_match(TX(t))) totk_life_log("fin tex=%p ser=%llu mem=%p", (void *)t, (unsigned long long)TX(t)->serial, (void *)TX(t)->mem);
    if (async_replay()) { void x_texrel(const tex_t *o); x_texrel(TX(t)); TX(t)->magic = 0; return; }
    live_del(TX(t)); mtl_tex_release(TX(t)); TX(t)->magic = 0;
}
NVN(nvnTextureGetStorageClass, NVNstorageClass, const NVNtexture *t){return totk_storage_class(&CTX(t)->d);}
NVN(nvnTextureGetFlags, NVNtextureFlags, const NVNtexture *t) { return (NVNtextureFlags)CTX(t)->d.flags; }
NVN(nvnTextureGetWidth, int, const NVNtexture *t) { return (int)CTX(t)->d.width; }
NVN(nvnTextureGetHeight, int, const NVNtexture *t) { return (int)CTX(t)->d.height; }
NVN(nvnTextureGetDepth, int, const NVNtexture *t) { return (int)CTX(t)->d.depth; }
NVN(nvnTextureGetStorageSize, int, const NVNtexture *t) { return (int)CTX(t)->L.size; }
NVN(nvnTextureCompare, NVNboolean, const NVNtexture *a, const NVNtexture *b) {
    const tex_t *x = CTX(a), *y = CTX(b);
    return !memcmp(&x->d, &y->d, sizeof x->d) && x->mem == y->mem;
}
// 头文件无原型；main 0x174899c 一类调用点把返回值与 1 比，不等则置某状态位 0x100。含义未核，先给 1
// No prototype in the header; call sites such as main 0x174899c compare the return value with 1 and set some status bit 0x100 if not equal. Meaning unverified; return 1 for now
NVN_NOPROTO(nvnTextureGetRawStorageClass, int, const NVNtexture *t) { (void)t; return 1; }
#define VW(v) ((view_t *)(v))
NVN(nvnTextureViewSetDefaults, void, NVNtextureView *v) { memset(v, 0, sizeof(view_t)); }
NVN(nvnTextureViewSetLevels, void, NVNtextureView *v, int base, int n) { if ((unsigned)base > 255 || (unsigned)n > 255) die("texture view levels %d/%d", base, n); VW(v)->set_levels = 1; VW(v)->base_level = base; VW(v)->nlevels = n; }
NVN(nvnTextureViewSetLayers, void, NVNtextureView *v, int min, int n) { if ((unsigned)min > 65535 || (unsigned)n > 65535) die("texture view layers %d/%d", min, n); VW(v)->set_layers = 1; VW(v)->min_layer = min; VW(v)->nlayers = n; }
NVN(nvnTextureViewSetFormat, void, NVNtextureView *v, NVNformat f) { if ((unsigned)f > 65535) die("texture view format %d", (int)f); VW(v)->set_format = 1; VW(v)->format = f; }
NVN(nvnTextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) { if ((unsigned)t > 255) die("texture view target %d", (int)t); VW(v)->set_target = 1; VW(v)->target = t; }
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
// CPU 读贴图（NVN ReadTexels）：预排布贴图以池内存为真值（解排布直读），GPU 写出的贴图从 MTLTexture 读回（nvn_mtl.m）
// CPU texture read (NVN ReadTexels): for packaged textures the pool memory is the ground truth (de-layout and read directly); GPU-written textures are read back from the MTLTexture (nvn_mtl.m)
static void read_texels(const tex_t *o, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, ptrdiff_t row, ptrdiff_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(o->d.format, &bw, &bh);
    uint32_t wb = (uint32_t)((r->width + bw - 1) / bw) * bpb, hb = (uint32_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    x_drain(); mtl_read_texels(o, level, layer0, r, dst, (size_t)row, (size_t)img);
}
NVN(nvnTextureReadTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst) {
    read_texels(CTX(t), (const view_t *)v, r, dst, 0, 0);
}
NVN(nvnTextureReadTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst, ptrdiff_t row, ptrdiff_t img) {
    read_texels(CTX(t), (const view_t *)v, r, dst, row, img);
}
static void write_texels(const tex_t *o, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, ptrdiff_t row, ptrdiff_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(o->d.format, &bw, &bh);
    uint32_t wb = (uint32_t)((r->width + bw - 1) / bw) * bpb, hb = (uint32_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    if (row < (ptrdiff_t)wb || img < row * (ptrdiff_t)hb) die("WriteTexels: stride smaller than region");
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    if (v && v->set_format && v->format != o->d.format) die("WriteTexels: format-reinterpreting view not supported");
    x_drain(); mtl_write_texels(o, level, layer0, r, src, (size_t)row, (size_t)img);
}
NVN(nvnTextureWriteTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src) {
    write_texels(CTX(t), (const view_t *)v, r, src, 0, 0);
}
NVN(nvnTextureWriteTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src, ptrdiff_t row, ptrdiff_t img) {
    write_texels(CTX(t), (const view_t *)v, r, src, row, img);
}
NVN(nvnTextureFlushTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }
NVN(nvnTextureInvalidateTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }

// ---------- 采样器 ----------
// ---------- Samplers ----------
#define SB(b) (&((samp_builder_t *)(b))->p)
NVN(nvnSamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    NVNdevice *dev = ((samp_builder_t *)b)->dev; memset(b, 0, sizeof(samp_builder_t)); ((samp_builder_t *)b)->dev = dev;
    samp_par *o = SB(b);
    o->minf = NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR; o->magf = NVN_MAG_FILTER_LINEAR;   // GL 默认 | GL defaults
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
NVN(nvnSamplerBuilderSetLodSnap, void, NVNsamplerBuilder *b, float v) { SB(b)->lod_snap = v; }   // Metal has no LOD snapping; stored for queries   // Metal 无对应，忽略 | No Metal equivalent, ignored
NVN(nvnSamplerBuilderGetMinMagFilter, void, const NVNsamplerBuilder *b, NVNminFilter *mi, NVNmagFilter *ma) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *mi = (NVNminFilter)o->minf; *ma = (NVNmagFilter)o->magf;
}
NVN(nvnSamplerBuilderGetWrapMode, void, const NVNsamplerBuilder *b, NVNwrapMode *s, NVNwrapMode *t, NVNwrapMode *r) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *s = (NVNwrapMode)o->ws; *t = (NVNwrapMode)o->wt; *r = (NVNwrapMode)o->wr;
}
NVN(nvnSamplerBuilderSetReductionFilter, void, NVNsamplerBuilder *b, NVNsamplerReduction r) { if ((unsigned)r > 2) die("invalid sampler reduction %d", (int)r); SB(b)->reduction = (uint8_t)r; }
NVN(nvnSamplerBuilderGetReductionFilter, NVNsamplerReduction, const NVNsamplerBuilder *b) { return (NVNsamplerReduction)((const samp_builder_t *)b)->p.reduction; }
NVN(nvnSamplerGetReductionFilter, NVNsamplerReduction, const NVNsampler *s) { return (NVNsamplerReduction)((const samp_t *)s)->p.reduction; }
NVN(nvnSamplerInitialize, NVNboolean, NVNsampler *s, const NVNsamplerBuilder *b) {
    samp_t *o = (samp_t *)s; o->magic = MAGIC('S', 'M', 'P', 0); o->p = ((const samp_builder_t *)b)->p;
    { static int samplog = -1; if (samplog < 0) samplog = getenv("DM_NVN_SAMPLOG") != NULL;
      if (samplog) { const samp_par *q = &o->p;
        fprintf(stderr, "[nvn-samp] s=%p min=%u mag=%u wrap=%u/%u/%u cmp=%u/%u red=%u lod=%.6g..%.6g bias=%.6g aniso=%.6g border=%.4g,%.4g,%.4g,%.4g snap=%.6g\n",
            (void *)s, q->minf, q->magf, q->ws, q->wt, q->wr, q->cmp_mode, q->cmp_func, q->reduction, q->lod_min, q->lod_max, q->lod_bias, q->aniso,
            q->border[0], q->border[1], q->border[2], q->border[3], q->lod_snap); } }
    return 1;
}
NVN(nvnSamplerFinalize, void, NVNsampler *s) { ((samp_t *)s)->magic = 0; }

// ---------- 程序 ----------
// ---------- Programs ----------
NVN(nvnProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *d) {
    (void)d; memset(p, 0, sizeof(prog_t)); ((prog_t *)p)->magic = MAGIC('P', 'R', 'O', 'G'); return 1;
}
NVN(nvnProgramFinalize, void, NVNprogram *p) { x_drain(); mtl_prog_release((prog_t *)p); ((prog_t *)p)->magic = 0; }
/* The driver reads a shader's control section during SetShaders; the game may free or reuse that memory afterwards
   (Bayonetta 3 does; TOTK kept it). The replay reads control sections later, so each one is copied here into storage
   that lives for the process, one copy per distinct content. A section is the size its word at +0x20 gives, at least
   0x880 (the extent the replay reads); Xenoblade Chronicles 3 has longer ones (0x9e1), whose tail used to be cut. */
#define CONTROL_BYTES 0x880
static size_t control_size(const uint8_t *b) { uint32_t n; memcpy(&n, b + 0x20, 4); return n > CONTROL_BYTES && n <= 0x4000 ? n : CONTROL_BYTES; }
static uint64_t control_hash(const uint8_t *b, size_t n) { uint64_t h = 1469598103934665603ull; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; return h; }
static const void *control_intern(const void *control) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static const uint8_t **slots; static size_t cap, used;
    if (!control) return NULL;
    const uint8_t *b = control; size_t n = control_size(b); uint64_t h = control_hash(b, n);
    pthread_mutex_lock(&mu);
    if (used * 2 >= cap) {   // grow and rehash (open addressing)
        size_t ncap = cap ? cap * 2 : 4096; const uint8_t **ns = calloc(ncap, sizeof *ns); if (!ns) die("control section table");
        for (size_t i = 0; i < cap; i++) if (slots[i]) {
            size_t at = control_hash(slots[i], control_size(slots[i])) & (ncap - 1); while (ns[at]) at = (at + 1) & (ncap - 1); ns[at] = slots[i];
        }
        free(slots); slots = ns; cap = ncap;
    }
    size_t at = h & (cap - 1);
    while (slots[at] && (control_size(slots[at]) != n || memcmp(slots[at], b, n))) at = (at + 1) & (cap - 1);
    if (!slots[at]) { uint8_t *c = malloc(n); if (!c) die("control section copy"); memcpy(c, b, n); slots[at] = c; used++; }
    const void *r = slots[at]; pthread_mutex_unlock(&mu); return r;
}
NVN(nvnProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *sd) {
    NVN_BLOCKING();
    prog_t *o = (prog_t *)p;
    if (n < 0 || n > 6) { LOG("【红】ProgramSetShaders 阶段数 %d", n); return 0; }
    mtl_prog_release(o); o->nstage = n; memcpy(o->sd, sd, n * sizeof *sd); o->serial = nvn_new_serial();
    for (int i = 0; i < n; i++) o->sd[i].control = control_intern(o->sd[i].control);
    mtl_prog_prefetch(o);
    OLOG("ProgramSetShaders %p n=%d data0=%#llx", (void *)p, n, n ? (unsigned long long)sd[0].data : 0ull);
    return 1;
}

// TOTK: NVNshaderDataExt = {data, control, reserved}, 0x18 bytes (runtime dump of main's call; reserved observed 0).
typedef struct { NVNbufferAddress data; const void *control; uint64_t reserved; } totk_shader_data_ext;
_Static_assert(sizeof(totk_shader_data_ext) == 0x18, "NVNshaderDataExt stride");
NVN_NOPROTO(nvnProgramSetShadersExt, NVNboolean, NVNprogram *p, int n, const totk_shader_data_ext *sd) {
    if (n < 0 || n > 6) { LOG("【红】ProgramSetShadersExt 阶段数 %d", n); return 0; }
    NVNshaderData plain[6];
    for (int i = 0; i < n; i++) {
        if (sd[i].reserved) die("ProgramSetShadersExt: unknown non-zero field %#llx in stage %d", (unsigned long long)sd[i].reserved, i);
        plain[i] = (NVNshaderData){sd[i].data, sd[i].control};
    }
    return h_nvnProgramSetShaders(p, n, plain);
}

// ---------- 同步 / 事件 ----------
// ---------- Sync / events ----------
NVN(nvnSyncInitialize, NVNboolean, NVNsync *s, NVNdevice *d) { (void)d; *(sync_t *)s = (sync_t){MAGIC('S', 'Y', 'N', 'C'), 0, 0}; return 1; }
NVN(nvnSyncFinalize, void, NVNsync *s) { ((sync_t *)s)->magic = 0; }
// 围栏 = 「此前提交到队列的命令全部完成」：记下最近一次提交的栅栏值
// Fence = "all commands previously submitted to the queue have completed": record the fence value of the most recent submit
// TOTK: with the translation thread a fence is a submit count (bit 2 of fenced), see fence_now / fence_wait.
/* TOTK_SYNC_CHAIN=<n>[:<from frame>] (diagnostic): guest call chains of the first n fence placements and of the first n waits that block,
   to find the game code that makes frame N+1 wait for frame N on the GPU (see the 2026-10-15 timeline). */
static void guest_chain(const char *what);
static uint64_t g_frame;   // tentative; defined with the frame bookkeeping below
static uint64_t g_game_sys;
extern _Atomic int runtime_survey_pending;
extern void runtime_survey_tick(uint64_t frame);
extern const int32_t runtime_survey_sync[2];
/* TOTK_SURVEY_DEBUG=1: while a command is pending, the first 6 waited syncs, their thread and guest call chain, and where the
   system object holds them (pointer at sys+o, or inside / pointed to from the object at *(sys+o)). */
static int vm_read8(uint64_t a, uint64_t *out);
static void survey_wait_debug(const NVNsync *s) {
    static int on = -1; static _Atomic int left = 6;
    if (on < 0) { const char *e = getenv("TOTK_SURVEY_DEBUG"); on = e && !strcmp(e, "1"); }
    if (!on || atomic_fetch_sub(&left, 1) <= 0) return;
    uint64_t sv = (uintptr_t)s, tid = 0; pthread_threadid_np(NULL, &tid);
    LOG("[survey] wait sync=%p frame=%llu thread=%llu", (const void *)s, (unsigned long long)g_frame, (unsigned long long)tid);
    for (uint64_t o = 0; g_game_sys && o < 0x800; o += 8) {
        uint64_t q;
        if (!vm_read8(g_game_sys + o, &q) || !q) continue;
        if (q == sv) LOG("[survey]   = *(sys+%#llx)", (unsigned long long)o);
        if (sv >= q && sv - q < 0x800) LOG("[survey]   = *(sys+%#llx)+%#llx", (unsigned long long)o, (unsigned long long)(sv - q));
        for (uint64_t o2 = 0; o2 < 0x800; o2 += 8) { uint64_t r; if (vm_read8(q + o2, &r) && r == sv) LOG("[survey]   = *(*(sys+%#llx)+%#llx)", (unsigned long long)o, (unsigned long long)o2); }
    }
    guest_chain("survey wait");
}
static void survey_after_wait(const NVNsync *s) {
    if (__builtin_expect(atomic_load_explicit(&runtime_survey_pending, memory_order_relaxed), 0)) {
        survey_wait_debug(s);
        uintptr_t at = g_game_sys ? *(const uintptr_t *)(uintptr_t)(g_game_sys + runtime_survey_sync[0]) : 0;
        if (at && runtime_survey_sync[1] >= 0) at = *(const uintptr_t *)(at + runtime_survey_sync[1]);
        if (at && (uintptr_t)s == at) runtime_survey_tick(g_frame);
    }
}
/* TOTK_SYNC_CHAIN=<n>[:<from frame>] */
static int sync_chain_left(int k) {
    static int lim = -2; static uint64_t from; static _Atomic int used[2];
    if (lim == -2) { const char *e = getenv("TOTK_SYNC_CHAIN"); lim = e ? atoi(e) : 0; from = e && strchr(e, ':') ? strtoull(strchr(e, ':') + 1, NULL, 10) : 0; }
    return lim > 0 && g_frame >= from && atomic_fetch_add(&used[k], 1) < lim;
}
// A queue waiting on a sync: replay runs every queue's work in submission order on the single Metal queue, so the work
// that signals the sync has already been replayed (as nvnCommandBufferWaitSync). TOTK never calls it; Bayonetta 3 does.
NVN(nvnQueueWaitSync, NVNboolean, NVNqueue *q, const NVNsync *s) { (void)q; (void)s; return 1; }
NVN(nvnQueueFenceSync, void, NVNqueue *q, NVNsync *s, NVNsyncCondition c, int f) { (void)q; (void)c; (void)f; int tok; uint64_t v = fence_now(&tok); ((sync_t *)s)->fenced = tok ? 5 : 1; ((sync_t *)s)->fence = v;
    if (sync_chain_left(0)) { char w[96]; snprintf(w, sizeof w, "FenceSync sync=%p fence=%llu cond=%d flags=%d", (void *)s, (unsigned long long)v, (int)c, f); guest_chain(w); } }
// A fence inside a command buffer (Bayonetta 3; TOTK only fences queues). Approximation: it stands for the next submission
// (fence_now() + 1), where the command buffer normally goes. A later real submission releases waiters early (results read
// before the GPU wrote them), an earlier one makes them wait longer; neither can hang.
NVN(nvnCommandBufferFenceSync, void, NVNcommandBuffer *c, NVNsync *s, NVNsyncCondition cond, int flags) {
    (void)c; (void)cond; (void)flags; int tok; uint64_t v = fence_now(&tok) + 1;
    ((sync_t *)s)->fenced = tok ? 5 : 1; ((sync_t *)s)->fence = v;
}
// 没围栏过的同步对象按真机行为等到超时；已围栏的只在 GPU 真正完成到该栅栏值后放行
// A sync object that was never fenced waits until timeout, as on a real device; a fenced one is released only after the GPU has actually completed up to that fence value
// TOTK_SYNC_LOG=1: CPU waits by kind (0 fence sync, 1 window-acquire sync): calls, calls that had to block, blocked time,
// and calls whose fence was the newest committed one (a full drain of the queue).
_Atomic uint64_t g_sw_calls[2], g_sw_blocked[2], g_sw_ns[2], g_sw_newest[2];
static int synclog_env(void) { static int on = -1; if (on < 0) on = getenv("TOTK_SYNC_LOG") != NULL; return on; }
NVN(nvnSyncWait, NVNsyncWaitResult, const NVNsync *s, uint64_t timeout) {
    NVN_BLOCKING();
    const sync_t *o = (const sync_t *)s;
    if (!o->fenced) return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    int k = (o->fenced & 2) != 0, lg = synclog_env();
    if (lg) { atomic_fetch_add(&g_sw_calls[k], 1); if (o->fence == mtl_last_fence()) atomic_fetch_add(&g_sw_newest[k], 1); }
    int tok = (o->fenced & 4) != 0;
    if (tok ? fence_wait(o->fence, 0) : mtl_wait(o->fence, 0)) { survey_after_wait(s); return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED; }
    uint64_t t0 = lg ? now_ns() : 0;
    totk_tl(TL_WAIT_IN, o->fence, o->fence == mtl_last_fence());
    if (sync_chain_left(1)) { char w[96]; snprintf(w, sizeof w, "SyncWait sync=%p fence=%llu newest=%d", (const void *)s, (unsigned long long)o->fence, o->fence == mtl_last_fence()); guest_chain(w); }
    int ok = tok ? fence_wait(o->fence, timeout) : mtl_wait(o->fence, timeout);
    totk_tl(TL_WAIT_OUT, o->fence, ok);
    if (lg) { atomic_fetch_add(&g_sw_blocked[k], 1); atomic_fetch_add(&g_sw_ns[k], now_ns() - t0); }
    if (ok) survey_after_wait(s);
    return ok ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}
NVN(nvnEventBuilderSetDefaults, void, NVNeventBuilder *b) { memset(b, 0, sizeof(ev_builder_t)); }
NVN(nvnEventBuilderSetStorage, void, NVNeventBuilder *b, const NVNmemoryPool *p, int64_t off) { *(ev_builder_t *)b = (ev_builder_t){(const pool_t *)p, off}; }
NVN(nvnEventInitialize, NVNboolean, NVNevent *e, const NVNeventBuilder *b) {
    const ev_builder_t *s = (const ev_builder_t *)b;
    if (!s->pool || !s->pool->mem) { LOG("【红】EventInitialize 没有存储"); return 0; }
    *(ev_t *)e = (ev_t){MAGIC('E', 'V', 'N', 'T'), (volatile uint32_t *)(s->pool->mem + s->off)}; return 1;
}
NVN(nvnEventFinalize, void, NVNevent *e) { ((ev_t *)e)->magic = 0; }
NVN(nvnEventGetValue, uint32_t, const NVNevent *e) { return *((const ev_t *)e)->val; }
static void ev_signal(const ev_t *e, int mode, uint32_t v) { if (mode == NVN_EVENT_SIGNAL_MODE_ADD) *e->val += v; else *e->val = v; }
NVN(nvnEventSignal, void, NVNevent *e, NVNeventSignalMode m, uint32_t v) { ev_signal((const ev_t *)e, m, v); }

// ---------- 队列 ----------
// ---------- Queue ----------
#define QB(b) ((queue_builder_t *)(b))
NVN(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    NVNdevice *dev = QB(b)->dev; memset(b, 0, sizeof(queue_builder_t)); QB(b)->dev = dev;
    QB(b)->cmd_sz = 65536; QB(b)->comp_sz = 262144; QB(b)->ctl_sz = 16384; QB(b)->flush = 4096;   // 设备默认值 | Device defaults
}
NVN(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) { QB(b)->dev = d; }
NVN(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { QB(b)->flags = f; }
NVN(nvnQueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->cmd_sz = n; }
NVN(nvnQueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->comp_sz = n; }
NVN(nvnQueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t n) { QB(b)->ctl_sz = n; }
NVN(nvnQueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t n) { QB(b)->flush = n; }
// 队列内存：命令 + 计算 + 控制三块之和按 4 KiB 取整（宿主不使用，只需游戏按此大小分配）
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
NVN(nvnQueueFinish, void, NVNqueue *q) { NVN_BLOCKING(); (void)q; totk_tl(TL_FINISH_IN, 0, 0); x_drain(); mtl_finish(); totk_tl(TL_FINISH_OUT, 0, 0); }

// ---------- 命令缓冲：录进游戏的命令内存，提交时执行 CPU 可见副作用 ----------
// ---------- Command buffer: record into the game's command memory, perform CPU-visible side effects at submit ----------
#define CB(c) ((cmd_t *)(c))
#ifdef GAME_BOTW
/* Diagnostics (TOTK_CB_CHECK=1, Breath of the Wild): each record appended and each Begin / EndRecording first checks that
   the host fields of the command buffer still make sense -- the write pointer inside the host segment and the two inline
   sentinels intact -- and logs the first violations with the game code that called (a game writing into the
   NVNcommandBuffer object directly, through code inlined from NVN 53's headers, would show up here). */
static void guest_chain(const char *what);
/* The write pointer each command buffer had after our own last change (side table): a different one at the next record
   means something else moved it. */
#define CB_SEEN 4096
static struct { const cmd_t *o; uint8_t *cur; } g_cb_seen[CB_SEEN];
static pthread_mutex_t g_cb_seen_mu = PTHREAD_MUTEX_INITIALIZER;
static int cb_check_on(void) { static int on = -1; if (on < 0) on = getenv("TOTK_CB_CHECK") != NULL; return on; }
static void cb_seen_set(const cmd_t *o) {
    if (!cb_check_on()) return;
    unsigned i = (unsigned)(((uintptr_t)o >> 5) % CB_SEEN);
    pthread_mutex_lock(&g_cb_seen_mu); g_cb_seen[i].o = o; g_cb_seen[i].cur = o->cur; pthread_mutex_unlock(&g_cb_seen_mu);
}
static void cb_check(const cmd_t *o, const char *where) {
    if (!cb_check_on()) return;
    unsigned i = (unsigned)(((uintptr_t)o >> 5) % CB_SEEN);
    pthread_mutex_lock(&g_cb_seen_mu); int moved = g_cb_seen[i].o == o && g_cb_seen[i].cur != o->cur; uint8_t *was = g_cb_seen[i].cur; pthread_mutex_unlock(&g_cb_seen_mu);
    if (moved) { static _Atomic int said_m; if (atomic_fetch_add(&said_m, 1) < 12) {
        const uint64_t *q = (const uint64_t *)(uintptr_t)was;
        LOG("【红】[cb-check] %s: the write pointer of command buffer %p moved from %p to %p outside our code (%+lld bytes); bytes there: %016llx %016llx %016llx %016llx",
            where, (const void *)o, (void *)was, (void *)o->cur, (long long)(o->cur - was), (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3]);
        guest_chain("cb-check moved"); } }
    int bad = o->inline_cur != ~0ull || o->inline_end != 0 || (o->seg && (o->cur < o->seg || o->cur > o->end));
    static _Atomic int said;
    if (bad && atomic_fetch_add(&said, 1) < 12) {
        const uint64_t *w = (const uint64_t *)o;
        LOG("【红】[cb-check] %s: command buffer %p seg %p cur %p end %p flags10 %#llx inline %#llx/%#llx recording %u; words %016llx %016llx %016llx %016llx %016llx %016llx",
            where, (const void *)o, (void *)o->seg, (void *)o->cur, (void *)o->end, (unsigned long long)o->guest_flags, (unsigned long long)o->inline_cur, (unsigned long long)o->inline_end, o->recording,
            (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3], (unsigned long long)w[4], (unsigned long long)w[5]);
        guest_chain("cb-check");
    }
}
#endif
#define OP_RESERVE 24                             // 段尾永远留够一条 JUMP / END | Always keep enough room at the segment end for one JUMP / END

NVN(nvnCommandBufferInitialize, NVNboolean, NVNcommandBuffer *c, NVNdevice *d) {
    memset(c, 0, sizeof(cmd_t)); CB(c)->magic = MAGIC('C', 'M', 'D', 'B'); CB(c)->dev = d; CB(c)->inline_cur = ~0ull; CB(c)->inline_end = 0; return 1;
}
static void release_owned_copies(const void *cb);
NVN(nvnCommandBufferFinalize, void, NVNcommandBuffer *c) {
#ifndef GAME_BOTW
    release_owned_copies(c);
#endif
    free(CB(c)->seg); CB(c)->seg = CB(c)->cur = CB(c)->end = NULL; CB(c)->magic = 0; }
NVN(nvnCommandBufferSetMemoryCallback, void, NVNcommandBuffer *c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) { CB(c)->cb = cb; }
NVN(nvnCommandBufferSetMemoryCallbackData, void, NVNcommandBuffer *c, void *d) { CB(c)->cb_data = d; }
static uint8_t *align8(uint8_t *p) { return (uint8_t *)(((uintptr_t)p + 7) & ~(uintptr_t)7); }
// 诊断 DM_NVN_CMDSTAT=1：每 300 帧记一行命令录制的用量——每帧录了多少字节 / 多少条、游戏每次给多大一段命令内存、每帧补内存回调几次、宿主补段几次、
// 各类命令的条数与字节（找「我们的记录比真机命令大多少」）。只计数，不改录制 / 回放
// Diagnostic DM_NVN_CMDSTAT=1: every 300 frames log one line of command recording usage -- how many bytes / records were recorded per frame, how large a command memory segment the game gives each time, how many out-of-memory callbacks per frame, how many host-supplied segments,
// and the count and bytes of each command type (to find "how much larger our records are than real-device commands"). Counts only; does not change recording / replay
static struct { int on; uint64_t bytes, recs, grants, grant_bytes, grant_min, grant_max, cbs, hostseg, op_n[64], op_b[64], frame0, waste; } g_cs = { .on = -1 };
static pthread_mutex_t g_cs_mu = PTHREAD_MUTEX_INITIALIZER;
uint64_t dm_nvn_frame_count(void);
static int cs_on(void) { if (g_cs.on < 0) g_cs.on = getenv("DM_NVN_CMDSTAT") != NULL; return g_cs.on; }
static void cs_op(uint32_t op, uint32_t size) { if (!cs_on() || op >= 64) return; pthread_mutex_lock(&g_cs_mu); g_cs.op_n[op]++; g_cs.op_b[op] += size; pthread_mutex_unlock(&g_cs_mu); }
static void cs_dump(void) {   // 每帧调一次（出图处） | Called once per frame (at present)
    if (!cs_on()) return; uint64_t f = dm_nvn_frame_count(); if (f - g_cs.frame0 < 300) return;
    pthread_mutex_lock(&g_cs_mu); double n = (double)(f - g_cs.frame0); char top[700]; int k = 0; uint8_t used[64] = {0};
    for (int r = 0; r < 8; r++) { int bi = -1; for (int i = 0; i < 64; i++) if (!used[i] && g_cs.op_b[i] && (bi < 0 || g_cs.op_b[i] > g_cs.op_b[bi])) bi = i; if (bi < 0) break; used[bi] = 1;
        k += snprintf(top + k, sizeof top - k, " 命令%d：%.0f 条×%.0f 字节=%.1f KB；", bi, g_cs.op_n[bi] / n, (double)g_cs.op_b[bi] / (double)g_cs.op_n[bi], g_cs.op_b[bi] / n / 1024.0); }
    LOG("[nvn] 命令内存 至帧 %llu：每帧录 %.1f KB / %.0f 条；游戏给段 每帧 %.2f 次，每段 均 %.1f KB（最小 %.1f / 最大 %.1f）；补内存回调 每帧 %.2f 次；宿主补段 每帧 %.3f 次；换段丢弃的段尾 每帧 %.1f KB｜每帧%s",
        (unsigned long long)f, g_cs.bytes / n / 1024.0, g_cs.recs / n, g_cs.grants / n, g_cs.grants ? g_cs.grant_bytes / (double)g_cs.grants / 1024.0 : 0, g_cs.grant_min / 1024.0, g_cs.grant_max / 1024.0, g_cs.cbs / n, g_cs.hostseg / n, g_cs.waste / n / 1024.0, top);
    int on = g_cs.on; memset(&g_cs, 0, sizeof g_cs); g_cs.on = on; g_cs.frame0 = f; pthread_mutex_unlock(&g_cs_mu);
}
static __thread uint32_t t_rec_hist[OP_COUNT_]; static __thread uint32_t t_rec_n;
static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) { op_hdr h = {op, size}; memcpy(p, &h, sizeof h); if (op) { cs_op(op, size); if (op < OP_COUNT_) t_rec_hist[op]++; } }
// TOTK: records are written into a growable host buffer per command buffer, never into game command memory.
// Games hand the same command memory to several command buffers recording concurrently (budgeted for real NVN
// encodings); our records are larger and would overwrite each other there. The game's memory is accounted only.
static void cmd_reserve(cmd_t *o, size_t need) {
    if (o->seg && o->cur + need + OP_RESERVE <= o->end) return;
    size_t used = o->seg ? (size_t)(o->cur - o->seg) : 0;
    size_t cap = o->seg ? (size_t)(o->end - o->seg) : 0; if (!cap) cap = 64 << 10;
    while (used + need + OP_RESERVE > cap) cap *= 2;
    uint8_t *b = realloc(o->seg, cap); if (!b) die("command recording buffer allocation (%zu bytes)", cap);
    o->seg = b; o->cur = b + used; o->end = b + cap;
}

// TOTK diagnostics: guest return chain of the current NVN call (frames link through runtime_hle_switch).
void runtime_describe_address(uint64_t a, char *out, size_t n);
static void guest_chain(const char *what) {
    char line[1024]; size_t o = 0; uint64_t *fp = (uint64_t *)__builtin_frame_address(0);
    for (int i = 0; i < 14 && fp && !((uintptr_t)fp & 7); i++) { char w[96]; runtime_describe_address(fp[1], w, sizeof w); if (o < sizeof line - 100) o += snprintf(line + o, sizeof line - o, " <- %s", w); if ((uint64_t *)fp[0] == fp) break; fp = (uint64_t *)fp[0]; }
    LOG("[chain] %s%s", what, line);
}
/* Offset in main of the first guest return address on the frame chain (0 = none): which game code issued the call. */
static uint64_t guest_main_caller(void) {
    extern int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);
    uint64_t *fp = (uint64_t *)__builtin_frame_address(0);
    for (int i = 0; i < 16 && fp && !((uintptr_t)fp & 7); i++) {
        const char *nm; uint64_t off;
        if (runtime_module_offset(fp[1], &nm, &off) && !strcmp(nm, "main")) return off;
        if ((uint64_t *)fp[0] == fp) break; fp = (uint64_t *)fp[0];
    }
    return 0;
}
static void release_copies(uintptr_t lo, uintptr_t hi);
NVN(nvnCommandBufferAddCommandMemory, void, NVNcommandBuffer *c, const NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    if (cmdlog_env()) LOG("[cmd] AddCommandMemory cb=%p size=%#zx (accounting only)", (void *)c, n);
    /* Command memory can be shared by independent recordings with distinct control memory. */
    uintptr_t base = (uintptr_t)(p ? ((const pool_t *)p)->mem : NULL) + (uintptr_t)off;
    CB(c)->cmd_base = base; CB(c)->game_size = n; CB(c)->cmd_used = 0; CB(c)->hw_used = 0;
}
// The SDK encodes handles in control memory (sdk+0x444530). Reusing that storage retires its copies.
NVN(nvnCommandBufferAddControlMemory, void, NVNcommandBuffer *c, void *m, size_t n) {
    if (cmdlog_env()) LOG("[cmd] AddControlMemory cb=%p mem=%p size=%#zx", (void *)c, m, n);
#ifndef GAME_BOTW   /* NVN 53 handles do not live in control memory: Breath of the Wild re-adds a command buffer's control memory and keeps calling handles recorded before (botw.md) */
    release_copies((uintptr_t)m, (uintptr_t)m + n);
#endif
    CB(c)->ctl = m; CB(c)->ctl_size = n; CB(c)->ctl_used = 0;
}
NVN(nvnCommandBufferGetControlMemorySize, size_t, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->ctl_size; }
/* Usage is reported (records live in host memory, but the game decides from it whether a recording is empty);
   it is bounded by the memory the game provided, so the game never sees more used than it gave. */
#ifdef GAME_BOTW
static int used_log_left(void);
static uint64_t guest_main_caller(void);
NVN(nvnCommandBufferGetControlMemoryUsed, size_t, const NVNcommandBuffer *c) { const cmd_t *o = (const cmd_t *)c; size_t r = o->ctl_used < o->ctl_size ? o->ctl_used : o->ctl_size;
    if (used_log_left()) LOG("[cmd-used] control memory used %#zx (given %#zx) asked by main+%#llx", r, o->ctl_size, (unsigned long long)guest_main_caller());
    return r; }
#else
NVN(nvnCommandBufferGetControlMemoryUsed, size_t, const NVNcommandBuffer *c) { const cmd_t *o = (const cmd_t *)c; return o->ctl_used < o->ctl_size ? o->ctl_used : o->ctl_size; }
#endif
NVN(nvnCommandBufferGetControlMemoryFree, size_t, const NVNcommandBuffer *c) { const cmd_t *o = (const cmd_t *)c; return o->ctl_size - (o->ctl_used < o->ctl_size ? o->ctl_used : o->ctl_size); }
NVN(nvnCommandBufferGetCommandMemorySize, size_t, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->game_size; }
#ifdef GAME_BOTW
/* Diagnostics (TOTK_CMD_USED_LOG=<n>): the first n usage queries with the game code that asked and the answer. */
static int used_log_left(void) { static int left = -2; if (left == -2) { const char *e = getenv("TOTK_CMD_USED_LOG"); left = e ? atoi(e) : 0; } return left > 0 ? left-- : 0; }
static uint64_t guest_main_caller(void);
NVN(nvnCommandBufferGetCommandMemoryUsed, size_t, const NVNcommandBuffer *c) { const cmd_t *o = (const cmd_t *)c; size_t r = o->hw_used < o->game_size ? o->hw_used : o->game_size;
    if (used_log_left()) LOG("[cmd-used] command memory used %#zx (records %#zx, given %#zx) asked by main+%#llx", r, o->cmd_used, o->game_size, (unsigned long long)guest_main_caller());
    return r; }
#else
NVN(nvnCommandBufferGetCommandMemoryUsed, size_t, const NVNcommandBuffer *c) { const cmd_t *o = (const cmd_t *)c; return o->hw_used < o->game_size ? o->hw_used : o->game_size; }
#endif
NVN(nvnCommandBufferGetCommandMemoryFree, size_t, const NVNcommandBuffer *c) { const cmd_t *o = (const cmd_t *)c; return o->game_size - (o->hw_used < o->game_size ? o->hw_used : o->game_size); }
NVN(nvnCommandBufferGetMemoryCallback, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->cb; }
NVN_NOPROTO(nvnCommandBufferGetMemoryCallbackData, void *, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->cb_data; }
// 取一条记录的空间；不够则按 NVN 约定回调游戏补命令内存
// Get space for one record; if not enough, call back into the game per the NVN convention to add command memory
static uint8_t *cmd_alloc(cmd_t *o, uint32_t size) {
#ifdef GAME_BOTW
    cb_check(o, "record");
#endif
    size = (size + 7) & ~7u;
    cmd_reserve(o, size);
    if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.bytes += size; g_cs.recs++; pthread_mutex_unlock(&g_cs_mu); }
    uint8_t *p = o->cur; o->cur += size; o->cmd_used += size; o->hw_used += size; o->last_rec = size; put_hdr(p, 0, size);
#ifdef GAME_BOTW
    cb_seen_set(o);
#endif
    return p;
}
/* TOTK: the game sums nvnCommandBufferGetCommandMemoryUsed over recordings to size resource heaps (RSTB), so usage must be what
   the real NVN pushbuffer encoding takes, not our record size. Known encodings (Maxwell methods, from a real recording):
   DrawElements 12 words (index format, index address, 6-word draw macro, 1-word macro), DrawElementsBaseVertex 11 words,
   BindVertexAttribState 1+n words, BindVertexStreamState 16+n words, BindVertexBuffer 6 words each (address + limit). */
static void hw_fix(cmd_t *o, size_t bytes) { o->hw_used += bytes; o->hw_used -= o->last_rec; o->last_rec = bytes; }
static int reclog_limit(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_RECLOG"); v = e ? atoi(e) : 0; } return v; }
static void csh_reset(const void *cb);
NVN(nvnCommandBufferBeginRecording, void, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    if (o->recording) LOG("【红】BeginRecording 重入");
    o->recording = 1; o->cur = o->seg;   /* the previous recording was copied at EndRecording */
    csh_reset(c);
#ifdef GAME_BOTW
    cb_seen_set(o);
#endif
    o->rec_start = o->cmd_used;
    uint8_t *p = cmd_alloc(o, 8); put_hdr(p, OP_BEGIN, 8); o->hw_used -= 8;   /* BEGIN/END records have no hardware equivalent */
    o->hw_start = o->hw_used; memset(t_rec_hist, 0, sizeof t_rec_hist);
}
// TOTK: an NVN command handle lives in the command buffer's control memory; games reuse command memory
// for later recordings while still calling earlier handles (observed: temporary command buffers sharing one
// 4 MiB command region, each with its own control memory). The finished recording is therefore copied into
// host memory (following segment jumps), and the copy lives until the same control memory is added again.
typedef struct { uintptr_t ctl_lo, ctl_hi; uint8_t *copy; uint64_t stale_at; uint32_t token, orphan; uintptr_t owner; } cmd_copy;   // stale_at: submit count when its memory was re-added (0 = live); token: its mark is in the control memory (copy_token_write)
static uint64_t g_submits;
/* Recently freed handles, to report what a later use of a freed handle looked like. */
#define FREED_RING 65536
static struct { uintptr_t h; uint64_t freed_at, stale_at, used_at, made_at; uintptr_t lo; } g_freed[FREED_RING]; static unsigned g_nfreed;
static size_t g_ncopies;
/* TOTK: copies indexed by the 4 KiB page of their control-memory position (a copy covers one byte, ctl_lo). Releasing a
   re-added control region looks at its pages only: it used to scan all ~30,000 live copies under one lock on every
   AddControlMemory (about 65 per frame, from three recording threads), ~19 ms of game-thread time per frame on iPhone 14. */
#define COPY_PAGE_SHIFT 12
typedef struct { uintptr_t page; cmd_copy *v; uint32_t n, cap; uint16_t used, empty; } copy_bucket;   // empty: all its copies are gone (cbk_compact drops it)
static copy_bucket *g_cbk; static size_t g_cbk_cap, g_cbk_used, g_cbk_empty;   // open addressing, power-of-two capacity; emptied buckets go at a sweep window start
static uint64_t g_cbk_compactions;
static size_t cbk_slot(uintptr_t page) { uint64_t h = (uint64_t)page * 0x9E3779B97F4A7C15ull; return (size_t)(h >> 20) & (g_cbk_cap - 1); }
static copy_bucket *cbk_get(uintptr_t page, int create) {   // g_copy_mu held
    if (create && (g_cbk_used + 1) * 2 > g_cbk_cap) {
        size_t oc = g_cbk_cap; copy_bucket *ob = g_cbk; g_cbk_cap = oc ? oc * 2 : 1024; g_cbk = calloc(g_cbk_cap, sizeof *g_cbk); if (!g_cbk) die("command copy index");
        for (size_t i = 0; i < oc; i++) if (ob[i].used) { size_t k = cbk_slot(ob[i].page); while (g_cbk[k].used) k = (k + 1) & (g_cbk_cap - 1); g_cbk[k] = ob[i]; }
        free(ob);
    }
    if (!g_cbk_cap) return NULL;
    for (size_t k = cbk_slot(page);; k = (k + 1) & (g_cbk_cap - 1)) {
        if (g_cbk[k].used && g_cbk[k].page == page) return &g_cbk[k];
        if (!g_cbk[k].used) { if (!create) return NULL; g_cbk[k] = (copy_bucket){page, NULL, 0, 0, 1, 0}; g_cbk_used++; return &g_cbk[k]; }
    }
}
static pthread_mutex_t g_copy_mu = PTHREAD_MUTEX_INITIALIZER;
/* The opaque handle points past this host-only validation and accounting prefix. */
#define COPY_MAGIC 0x59504f43444d43ull   /* "CMDCOPY" */
#ifdef GAME_BOTW
typedef struct { uint64_t magic; _Atomic uint64_t used_at; uint64_t made_at; uint32_t body_bytes, last_size; _Atomic uint32_t used; uint32_t pad_; } copy_prefix;   // used: called or submitted at least once (NVN 53 lifetime, release_copies)
#else
typedef struct { uint64_t magic; _Atomic uint64_t used_at; uint64_t made_at; uint32_t body_bytes, last_size; _Atomic uint32_t used; uint32_t pad_; } copy_prefix;
#endif
static _Atomic uint64_t g_copy_bytes;   /* live recording-copy bytes (logged with the copy count) */
static copy_prefix *copy_of(NVNcommandHandle h) { copy_prefix *c = (copy_prefix *)(uintptr_t)h - 1; return c; }
static uint8_t *flatten_recording(const uint8_t *p) {
    size_t n = sizeof(copy_prefix), cap = 4096; uint8_t *out = malloc(cap);
    int bulk_ok = 1; uint32_t last_size = 0;
    if (!out) die("command copy allocation");
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7)) die("corrupt recording at %p (op %u size %u)", (const void *)p, h.op, h.size);
        if (h.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (h.op >= OP_COUNT_ || h.size > (64u << 20)) bulk_ok = 0;
        if (n == sizeof(copy_prefix)) { if (h.op != OP_BEGIN || h.size != 8) bulk_ok = 0; }
        else if (h.op == OP_BEGIN) bulk_ok = 0;
        else if (h.op != OP_END) last_size = h.size;
        if (n + h.size > cap) { while (n + h.size > cap) cap *= 2; out = realloc(out, cap); if (!out) die("command copy allocation"); }
        memcpy(out + n, p, h.size); n += h.size;
        if (h.op == OP_END) {
            uint8_t *trimmed = realloc(out, n); if (trimmed) out = trimmed;
            copy_prefix *c = (copy_prefix *)out; c->magic = COPY_MAGIC; c->used_at = 0; c->made_at = g_submits;
            c->used = 0;
            size_t body = n >= sizeof(copy_prefix) + 16 ? n - sizeof(copy_prefix) - 16 : 0;
            c->body_bytes = bulk_ok && h.size == 8 && body <= UINT32_MAX ? (uint32_t)body : 0;
            c->last_size = c->body_bytes ? last_size : 0;
            g_copy_bytes += malloc_size(out); return out + sizeof(copy_prefix);
        }
        p += h.size;
    }
}
/* Track handle use for diagnostics; lifetime depends on control-memory reuse, never elapsed frames. */
static void copy_touch(NVNcommandHandle h, uint64_t now) {
    copy_prefix *c = copy_of(h);
    if (c->magic != COPY_MAGIC) {
        for (unsigned i = 0; i < FREED_RING && i < g_nfreed; i++) if (g_freed[i].h == (uintptr_t)h)
            LOG("【红】handle %#llx made at submit %llu was freed at submit %llu (memory start %#lx re-added at submit %llu, last use at submit %llu); now submit %llu",
                (unsigned long long)h, (unsigned long long)g_freed[i].made_at, (unsigned long long)g_freed[i].freed_at, (unsigned long)g_freed[i].lo,
                (unsigned long long)g_freed[i].stale_at, (unsigned long long)g_freed[i].used_at, (unsigned long long)now);
        die("command handle %#llx is not a live recording (freed or never recorded)", (unsigned long long)h);
    }
    c->used_at = now;
    c->used = 1;
}
static void copy_done(NVNcommandHandle h) { copy_of(h)->used = 2; }

static uint64_t g_copy_drop_readd, g_copy_drop_token, g_copy_drop_final, g_copy_mark_faults;   /* why copies were released: their control memory was added again / their mark there is gone (of those: unmapped) */
static void copy_drop(copy_bucket *b, uint32_t i, int by_token) {   /* g_copy_mu held: release copy i of bucket b */
    cmd_copy *c = &b->v[i];
    c->stale_at = g_submits + 1;
    copy_prefix *pf = (copy_prefix *)(c->copy - sizeof(copy_prefix));
    if (cmdlog_env()) LOG("[cmd] free copy=%p%s", (void *)c->copy, by_token ? " (its mark in the control memory is gone)" : "");
    g_freed[g_nfreed++ % FREED_RING] = (typeof(g_freed[0])){(uintptr_t)c->copy, g_submits, c->stale_at, pf->used_at, pf->made_at, c->ctl_lo};
    pf->magic = 0; g_copy_bytes -= malloc_size(pf); free(pf); b->v[i] = b->v[--b->n]; g_ncopies--;
    if (by_token == 2) g_copy_drop_final++; else if (by_token) g_copy_drop_token++; else g_copy_drop_readd++;
    if (!b->n && !b->empty) { b->empty = 1; g_cbk_empty++; }
}
static int async_replay(void);
static uintptr_t owner_page(const void *cb) { return ((uintptr_t)cb >> COPY_PAGE_SHIFT) | ((uintptr_t)1 << 63); }
static void release_owned_copies(const void *cb) {
    if (async_replay()) return;
    pthread_mutex_lock(&g_copy_mu);
    copy_bucket *b = cbk_get(owner_page(cb), 0);
    if (b) for (uint32_t i = 0; i < b->n;) {
        cmd_copy *k = &b->v[i];
        if (k->owner != (uintptr_t)cb) { i++; continue; }
        if (atomic_load(&copy_of((NVNcommandHandle)(uintptr_t)k->copy)->used) == 2) { copy_drop(b, i, 2); continue; }
        k->orphan = 1; i++;
    }
    pthread_mutex_unlock(&g_copy_mu);
}
static void release_copies(uintptr_t lo, uintptr_t hi) {
    if (hi <= lo) return;
    pthread_mutex_lock(&g_copy_mu);
    for (uintptr_t pg = lo >> COPY_PAGE_SHIFT; pg <= (hi - 1) >> COPY_PAGE_SHIFT; pg++) {
        copy_bucket *b = cbk_get(pg, 0); if (!b) continue;
        for (uint32_t i = 0; i < b->n;) {
            cmd_copy *c = &b->v[i];
            if (!(c->ctl_lo < hi && lo < c->ctl_hi)) { i++; continue; }
#ifdef GAME_BOTW
            /* NVN 53 (botw.md): a recording the game has not used yet stays, though the command memory it went into is handed
               out again: Breath of the Wild recycles its 512-byte command chunks within a frame and calls such handles later
               in the same frame (boot37 / boot38). Once used, a recycled chunk retires it. */
            if (!((const copy_prefix *)(c->copy - sizeof(copy_prefix)))->used) { i++; continue; }
#endif
            copy_drop(b, i, 0);
        }
    }
    pthread_mutex_unlock(&g_copy_mu);
}
/* TOTK (2026-10-17): a recording copy also goes when the game reuses its control memory for something else. The original
   driver's handle is the address of a descriptor it writes into the recorder's control memory (sdk+0x444530), so a game
   that overwrites or unmaps that memory has no valid handle left. EndRecording writes a mark (this magic and the copy's
   address) into the 64 bytes of control memory it reports as used; the sweep's walk (pin_scan) releases a copy whose mark
   is gone. Without it, copies were released only when the same control memory was added again, and a game that frees
   a command buffer's control memory to its heap (teleporting between towns: new areas, new command buffers) left every
   copy recorded there behind: 88,000 -> 483,000 live copies (29 -> 146 MiB) in a 27-minute session (analysis/long19),
   and the sweep walks them all. TOTK_COPY_TOKEN=0: no marks (the old behaviour). */
#define COPY_TOKEN_MAGIC 0x4e454b4f54594d43ull
extern __thread sigjmp_buf *runtime_fault_guard;   // runtime/loader.c: a guarded read of guest memory survives a fault
static int async_replay(void);   /* below: TOTK_ASYNC_REPLAY */
/* With the translation thread (TOTK_ASYNC_REPLAY=1, iPhone default) the game may reuse a submitted command buffer's control
   memory as soon as the submit call returns, before the queued replay used the copy: marks are not used there yet (a copy
   last submitted at or after the submission being replayed would have to be kept). */
static int copy_token_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_COPY_TOKEN"); v = !(e && !strcmp(e, "0")) && !async_replay();
    if (!v) LOG("[cmd] 录制副本记号：关（%s）：副本只在控制内存被重新加入时释放", e && !strcmp(e, "0") ? "TOTK_COPY_TOKEN=0" : "翻译线程模式还不支持"); } return v; }
static void copy_token_write(uintptr_t at, const uint8_t *copy) { uint64_t v[2] = {COPY_TOKEN_MAGIC, (uint64_t)(uintptr_t)copy}; memcpy((void *)at, v, sizeof v); }
static int copy_token_negative(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_PROBE_NEGATIVE"); v = e && atoi(e) == 22; } return v; }   /* probe negative 22: marks never checked */
/* Reading a mark (pin_scan): the fault guard is armed only around the 16-byte read; the jump buffer is set once per walked
   slice with the signal mask saved (sigsetjmp(…, 1)) -- the crash handler runs with SIGSEGV / SIGBUS blocked, and a jump out
   of it that does not restore the mask leaves them blocked for good: the thread's next fault (a dirty-page write) then
   faults forever at 100 % CPU (2026-10-17 analysis/long20, first version with sigsetjmp(…, 0) per read: hung while loading). */
static sigjmp_buf *g_mark_jb;   /* only the replay thread walks copies (g_copy_mu held) */
static int copy_token_intact(const cmd_copy *c) {
    if (copy_token_negative()) return 1;
    uint64_t v[2];
    runtime_fault_guard = g_mark_jb;
    memcpy(v, (const void *)c->ctl_lo, sizeof v);
    runtime_fault_guard = NULL;
    return v[0] == COPY_TOKEN_MAGIC && v[1] == (uint64_t)(uintptr_t)c->copy;
}
/* Drop the buckets whose copies are all gone (g_copy_mu held, between sweep windows: the walk keeps bucket positions). */
static void cbk_compact(void) {
    if (g_cbk_empty < 4096 || g_cbk_empty * 2 < g_cbk_used) return;
    size_t oc = g_cbk_cap, live = g_cbk_used - g_cbk_empty, cap = 1024; copy_bucket *ob = g_cbk;
    while (cap < live * 2 + 2) cap *= 2;
    g_cbk = calloc(cap, sizeof *g_cbk); if (!g_cbk) die("command copy index"); g_cbk_cap = cap;
    for (size_t i = 0; i < oc; i++) if (ob[i].used) {
        if (ob[i].empty) { free(ob[i].v); continue; }
        size_t k = cbk_slot(ob[i].page); while (g_cbk[k].used) k = (k + 1) & (g_cbk_cap - 1); g_cbk[k] = ob[i];
    }
    free(ob); g_cbk_used = live; g_cbk_empty = 0; g_cbk_compactions++;
}
NVN(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    size_t hw_before = o->hw_used; uint8_t *p = cmd_alloc(o, 8); put_hdr(p, OP_END, 8); o->hw_used -= 8;
    o->recording = 0;
    if (reclog_limit() && (int)t_rec_n < reclog_limit()) { char line[1024]; size_t n = 0; t_rec_n++;
        for (unsigned i = 1; i < OP_COUNT_ && n < sizeof line - 24; i++) if (t_rec_hist[i]) n += snprintf(line + n, sizeof line - n, " op%u×%u", i, t_rec_hist[i]);
        LOG("[rec] #%u cb=%p ours=%zu hw=%zu%s", t_rec_n, (void *)c, o->cmd_used - o->rec_start - 16, hw_before - o->hw_start, line); }
    uint8_t *copy = flatten_recording(o->seg);
    if (call_stats_on()) call_stats_end(c, copy);
    /* Finalizing the recorder does not retire its handle; only reusing the backing control region does. */
#ifdef GAME_BOTW
    /* NVN 53 (Breath of the Wild, botw.md): the handle is the recording's position in the command buffer's control memory,
       as the original driver returns a descriptor address there. The game records into the same control memory position
       frame after frame and keeps calling the handle value it got once: it means "what was last recorded here". So the
       handle returned is that position (a guest address), the copy is found by it at every call (botw_handle_copy), and a
       new recording at the same position replaces the old copy. Re-adding control or command memory releases nothing:
       2026-10-19 runs with either rule died calling a handle whose memory had been added again (boot33, boot35-40). */
    uintptr_t lo = o->ctl && o->ctl_used + 64 <= o->ctl_size ? (uintptr_t)o->ctl + o->ctl_used : 0;
    uintptr_t hi = lo ? lo + 1 : 0;
    int token = 0;
#else
    uintptr_t lo = (uintptr_t)o->ctl + (o->ctl_used < o->ctl_size ? o->ctl_used : 0);
    uintptr_t hi = o->ctl ? lo + 1 : 0;
    int token = o->ctl && o->ctl_used + 64 <= o->ctl_size && copy_token_on();   /* only inside the control memory the game gave */
#endif
    o->ctl_used += 64;
    if (token) copy_token_write(lo, copy);
    pthread_mutex_lock(&g_copy_mu);
    uintptr_t owner = o->ctl ? 0 : (uintptr_t)c;
    { copy_bucket *b = cbk_get(owner ? owner_page(c) : lo >> COPY_PAGE_SHIFT, 1);
#ifdef GAME_BOTW
      if (lo) for (uint32_t i = 0; i < b->n; i++) if (b->v[i].ctl_lo == lo) { copy_drop(b, i, 0); break; }   /* re-recorded at this position */
#endif
      if (b->empty) { b->empty = 0; g_cbk_empty--; }
      if (b->n == b->cap) { b->cap = b->cap ? b->cap * 2 : 8; b->v = realloc(b->v, b->cap * sizeof *b->v); if (!b->v) die("command copy table"); }
      b->v[b->n++] = (cmd_copy){lo, hi, copy, 0, (uint32_t)token, 0, owner}; g_ncopies++; }
    { static uint64_t made; if (++made % 20000 == 0)
        LOG("[cmd] recording copies: %llu made, %zu live, %.1f MiB, submit %llu; released %llu at a re-added control memory, %llu whose mark there was gone (%llu unmapped), %llu after their command buffer was finalized; index %zu buckets (%zu empty) of %zu, compacted %llu times",
            (unsigned long long)made, g_ncopies, g_copy_bytes / 1048576.0, (unsigned long long)g_submits, (unsigned long long)g_copy_drop_readd, (unsigned long long)g_copy_drop_token, (unsigned long long)g_copy_mark_faults, (unsigned long long)g_copy_drop_final,
            g_cbk_used, g_cbk_empty, g_cbk_cap, (unsigned long long)g_cbk_compactions); }
    pthread_mutex_unlock(&g_copy_mu);
    if (cmdlog_env()) LOG("[cmd] EndRecording cb=%p recorded=%p copy=%p ctl=%p", (void *)c, (void *)o->seg, (void *)copy, o->ctl);
    if (totk_life_on() && totk_life_cb_take(c)) { totk_life_log("end cb=%p handle=%p", (void *)c, (void *)copy); totk_life_handle_add((uint64_t)(uintptr_t)copy); }
    if (getenv("TOTK_CHAIN_LOG")) { static int n; if (n++ < 200) guest_chain("EndRecording"); }
#ifdef GAME_BOTW
    if (lo) return (NVNcommandHandle)lo;
    { static uint64_t n; if (n++ < 4) LOG("[cmd] recording without room in its control memory: the handle is the copy itself (kept)"); }
#endif
    return (NVNcommandHandle)copy;
}
#ifdef GAME_BOTW
/* The copy a handle names now: a control-memory position (EndRecording) is looked up; anything else is a copy itself. */
static NVNcommandHandle botw_handle_copy(NVNcommandHandle h) {
    uint8_t *copy = NULL;
    pthread_mutex_lock(&g_copy_mu);
    copy_bucket *b = cbk_get((uintptr_t)h >> COPY_PAGE_SHIFT, 0);
    if (b) for (uint32_t i = 0; i < b->n; i++) if (b->v[i].ctl_lo == (uintptr_t)h) { copy = b->v[i].copy; break; }
    pthread_mutex_unlock(&g_copy_mu);
    return copy ? (NVNcommandHandle)(uintptr_t)copy : h;
}
#endif
NVN(nvnCommandBufferIsRecording, NVNboolean, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->recording != 0; }
// TOTK: the referenced recording is copied into the caller at this point (it is complete and valid now). The game
// recycles command memory in small steps, so a handle may be gone by the time the caller is submitted; inlining
// keeps the caller self-contained. Copying at the call position is the observable behaviour of both entry points.
/* TOTK_CALL_STATS=1: how much of a frame comes from called command lists and how much of that repeats unchanged
   (same handle, same bytes as the previous frame): the measure of what caching translated lists could save. Printed
   every 300 frames by nvnQueuePresentTexture. */
static uint64_t g_frame;   // defined with the frame bookkeeping below
typedef struct { uint64_t handle, hash, frame; } call_seen;
#define CALL_SEEN 8192
static call_seen g_call_seen[CALL_SEEN];
static uint64_t g_cs_calls, g_cs_draws, g_cs_same_calls, g_cs_same_draws, g_cs_bytes, g_cs_same_bytes;
static int call_stats_on(void) { static int v = -1; if (v < 0) v = getenv("TOTK_CALL_STATS") != NULL; return v; }
// Nested lists are inlined into their caller, so a draw would be counted at every level: each finished list keeps its
// own draws only (total minus what was inlined into it while recording), in host-side direct-mapped tables.
#define CS_TAB 16384
static struct { const void *cb; uint64_t inl; } g_cs_inl[CS_TAB];
static struct { uint64_t handle, own; } g_cs_own[CS_TAB];
static uint64_t list_draws(const uint8_t *q) { uint64_t d = 0; for (;;) { op_hdr x; memcpy(&x, q, sizeof x); if (x.op == OP_END || x.size < 8) break; if (x.op == OP_DRAW || x.op == OP_DRAW_IDX) d++; q += x.size; } return d; }
static void call_stats_end(const void *cb, const uint8_t *copy) {
    unsigned i = (unsigned)(((uintptr_t)cb >> 4) % CS_TAB); uint64_t inl = g_cs_inl[i].cb == cb ? g_cs_inl[i].inl : 0; g_cs_inl[i].cb = NULL; g_cs_inl[i].inl = 0;
    uint64_t all = list_draws(copy); unsigned j = (unsigned)(((uintptr_t)copy >> 4) % CS_TAB);
    g_cs_own[j].handle = (uint64_t)(uintptr_t)copy; g_cs_own[j].own = all > inl ? all - inl : 0;
}
static void call_stats_note(const void *cb, uint64_t hd, const uint8_t *p) {
    uint64_t h = 1469598103934665603ull, draws = 0, alld = 0, bytes = 0;
    for (const uint8_t *q = p;;) { op_hdr x; memcpy(&x, q, sizeof x); if (x.op == OP_END || x.size < 8) break;
        if (x.op == OP_DRAW || x.op == OP_DRAW_IDX) alld++;
        for (uint32_t i = 0; i < x.size; i += 8) { uint64_t w; memcpy(&w, q + i, 8); h = (h ^ w) * 1099511628211ull; }
        bytes += x.size; q += x.size; }
    { unsigned j = (unsigned)((hd >> 4) % CS_TAB); draws = g_cs_own[j].handle == hd ? g_cs_own[j].own : alld; }
    { unsigned i = (unsigned)(((uintptr_t)cb >> 4) % CS_TAB); if (g_cs_inl[i].cb != cb) { g_cs_inl[i].cb = cb; g_cs_inl[i].inl = 0; } g_cs_inl[i].inl += alld; }
    call_seen *e = &g_call_seen[(hd >> 4) % CALL_SEEN];
    int same = e->handle == hd && e->hash == h && e->frame + 1 >= g_frame;
    g_cs_calls++; g_cs_draws += draws; g_cs_bytes += bytes;
    if (same) { g_cs_same_calls++; g_cs_same_draws += draws; g_cs_same_bytes += bytes; }
    *e = (call_seen){hd, h, g_frame};
}
static _Atomic int g_inline_bulk_on = -1;
static int g_inline_bulk_negative;
static int inline_bulk_on(void) {
    int v = atomic_load_explicit(&g_inline_bulk_on, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("TOTK_INLINE_BULK"); int expected = -1;
        atomic_compare_exchange_strong(&g_inline_bulk_on, &expected, e && !strcmp(e, "1"));
        v = atomic_load_explicit(&g_inline_bulk_on, memory_order_relaxed);
    }
    return v;
}
void dm_nvn_inline_bulk_set(int v) {
    atomic_store_explicit(&g_inline_bulk_on, v != 0, memory_order_relaxed);
    LOG("[flag] inline_bulk = %d", v != 0);
}
static void inline_recording_body(NVNcommandBuffer *c, NVNcommandHandle hd);
static void inline_recording(NVNcommandBuffer *c, NVNcommandHandle hd) { inline_recording_body(c, hd); copy_done(hd); }
static void inline_recording_body(NVNcommandBuffer *c, NVNcommandHandle hd) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)hd;
    if (!p) die("CallCommands/CopyCommands with a null handle");
    copy_touch(hd, g_submits);
    if (call_stats_on()) call_stats_note(c, hd, p);
    /* EndRecording already flattened and checked this contiguous body. Copy
       its bytes now, at exactly the original call position. Deferred bindings
       remain deferred; source handle retirement cannot affect the caller.
       CMDSTAT retains the record-by-record path and its exact counters. */
    const copy_prefix *meta = copy_of(hd);
    if (inline_bulk_on() && !cs_on() && meta->body_bytes && meta->last_size) {
        cmd_t *o = CB(c);
        uint8_t *dst = cmd_alloc(o, meta->body_bytes);
        memcpy(dst, p + 8, meta->body_bytes);
        o->last_rec = meta->last_size;
        if (g_inline_bulk_negative) dst[8] ^= 1;
        return;
    }
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.size > (64u << 20) || h.op >= OP_COUNT_) die("corrupt recording in called handle %#llx (op %u size %u)", (unsigned long long)hd, h.op, h.size);
        if (h.op == OP_END) return;
        if (h.op != OP_BEGIN) { uint8_t *d = cmd_alloc(CB(c), h.size); memcpy(d, p, h.size); }
        p += h.size;
    }
}
static void life_call(NVNcommandBuffer *c, int n, const NVNcommandHandle *h, const char *what) {   // DM_NVN_LIFELOG
    if (totk_life_on()) for (int i = 0; i < n; i++) if (totk_life_handle_has(h[i])) { totk_life_log("%s cb=%p handle=%#llx", what, (void *)c, (unsigned long long)h[i]); totk_life_cb_mark(c); }
}
NVN(nvnCommandBufferCopyCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {
    life_call(c, n, h, "copy");
#ifdef GAME_BOTW
    for (int i = 0; i < n; i++) inline_recording(c, botw_handle_copy(h[i]));
#else
    for (int i = 0; i < n; i++) inline_recording(c, h[i]);
#endif
}
NVN(nvnCommandBufferCallCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {
    if (cmdlog_env()) for (int i = 0; i < n; i++) LOG("[cmd] CallCommands cb=%p handle=%#llx", (void *)c, (unsigned long long)h[i]);
    life_call(c, n, h, "call");
#ifdef GAME_BOTW
    for (int i = 0; i < n; i++) inline_recording(c, botw_handle_copy(h[i]));
#else
    for (int i = 0; i < n; i++) inline_recording(c, h[i]);
#endif
}
NVN(nvnCommandBufferSignalEvent, void, NVNcommandBuffer *c, const NVNevent *e, NVNeventSignalMode m, NVNeventSignalLocation loc, int flags, uint32_t v) {
    (void)loc; (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_SIGNAL_EVENT, 24);
    const ev_t *ev = (const ev_t *)e; volatile uint32_t *val = ev->val; uint32_t mm = m;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
NVN(nvnCommandBufferResetCounter, void, NVNcommandBuffer *c, NVNcounterType t) {
    uint8_t *p = cmd_alloc(CB(c), 16); put_hdr(p, OP_RESET_COUNTER, 16); uint64_t tt = t; memcpy(p + 8, &tt, 8);
}
// TOTK: TOTK_CMD_LOG=1 logs selected recorded commands; TOTK_CMD_LOG_FRAMES=a-b restricts it to a frame window (g_frame_pub)
static int cmdlog_on(void) {
    static int on = -1; static long f0 = -1, f1 = -1;
    if (on < 0) { on = getenv("TOTK_CMD_LOG") != NULL; const char *e = getenv("TOTK_CMD_LOG_FRAMES"); if (e) { f0 = atol(e); f1 = strchr(e, '-') ? atol(strchr(e, '-') + 1) : f0; } }
    if (!on) return 0; if (f0 < 0) return 1; extern uint64_t g_frame_pub; return (long)g_frame_pub >= f0 && (long)g_frame_pub <= f1;
}
NVN(nvnCommandBufferReportCounter, void, NVNcommandBuffer *c, NVNcounterType t, NVNbufferAddress a) {
    if (cmdlog_on()) LOG("[cmd] F%llu ReportCounter type=%d addr=%#llx", (unsigned long long)g_frame_pub, (int)t, (unsigned long long)a);
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_REPORT_COUNTER, 24); uint64_t tt = t; memcpy(p + 8, &tt, 8); memcpy(p + 16, &a, 8);
}
NVN(nvnCommandBufferCopyBufferToBuffer, void, NVNcommandBuffer *c, NVNbufferAddress src, NVNbufferAddress dst, size_t n, int flags) {
    (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 32); put_hdr(p, OP_COPY_B2B, 32); uint64_t nn = n;
    memcpy(p + 8, &src, 8); memcpy(p + 16, &dst, 8); memcpy(p + 24, &nn, 8);
}
// Buffer clear (Bayonetta 3; TOTK never calls it): recorded as an inline buffer write of the 32-bit value repeated, so it
// takes the uniform-update path on the GPU timeline (ordering with draws and copies included). The recording grows by the
// cleared size; a dedicated fill op would avoid that. Command memory used counts a short pushbuffer method, like other ops.
NVN(nvnCommandBufferClearBuffer, void, NVNcommandBuffer *c, NVNbufferAddress buf, size_t n, uint32_t value) {
    if (n % 4 || n > (64u << 20)) die("ClearBuffer of %zu bytes", n);
    uint32_t sz = (uint32_t)(24 + n);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, OP_UPDATE_UB, (sz + 7) & ~7u);
    uint64_t dst = buf, nn = n; memcpy(p + 8, &dst, 8); memcpy(p + 16, &nn, 8);
    for (size_t k = 0; k < n; k += 4) memcpy(p + 24 + k, &value, 4);
    hw_fix(CB(c), 24);
}
// uniform 更新：数据在录制时拷进命令内存（NVN 语义：录制后调用方可改原数据），提交时写进缓冲
// uniform update: the data is copied into command memory at record time (NVN semantics: the caller may modify the original data after recording) and written into the buffer at submit
NVN(nvnCommandBufferUpdateUniformBuffer, void, NVNcommandBuffer *c, NVNbufferAddress buf, size_t bufsz, ptrdiff_t off, size_t n, const void *data) {
    (void)bufsz;
    uint32_t sz = (uint32_t)(24 + n);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, OP_UPDATE_UB, (sz + 7) & ~7u);
    uint64_t dst = buf + off, nn = n; memcpy(p + 8, &dst, 8); memcpy(p + 16, &nn, 8); memcpy(p + 24, data, n);
}

// ---------- 渲染命令：参数按 nvn_obj.h 的载荷格式录进命令内存，回放见 nvn_mtl.m ----------
// ---------- Render commands: parameters are recorded into command memory in the payload format of nvn_obj.h; for replay see nvn_mtl.m ----------
static void *rec(NVNcommandBuffer *c, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
#define REC(c, OP, T) ((T *)rec((c), (OP), sizeof(T)))
// 带数组的绑定记录按实际个数分配：前缀字段 + n 个元素（回放端只读前 n 个）。原先按最大个数定长记（绑顶点缓冲 272 字节、绑贴图 / 采样器 280 字节，
// 实际常只有 1–2 个），空尾占每帧命令字节的四到五成；88 级法师的高级属性页每帧录 12.5 MB，把游戏自己的命令内存池（约每帧 105 段 × 128 KB）用空
// Binding records with arrays are allocated by the actual count: prefix fields + n elements (the replay side reads only the first n). Previously they were recorded at fixed length for the maximum count (bind vertex buffers 272 bytes, bind textures / samplers 280 bytes,
// while there are usually only 1–2 in practice), and the empty tail took 40–50% of the command bytes per frame; the advanced stats page of the level 88 sorceress recorded 12.5 MB per frame, using up the game's own command memory pool (about 105 segments x 128 KB per frame)
#define RECN(c, OP, T, ARR, N) ((T *)rec((c), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))
static void copy_view(view_t *dst, uint8_t *has, const NVNtextureView *v) { *has = v != NULL; if (v) *dst = *(const view_t *)v; }
// TOTK: recordings keep texture objects by pointer, but the game may finalize or reuse an NVNtexture (e.g. a
// temporary object) after recording and before submit; NVN itself only captured the GPU state. Records therefore
// point at an immutable host copy per texture initialization (serial).
/* TOTK (2026-10-17): those copies used to live for the whole process. The game initialises its transient render targets
   again every frame (a new serial each time), so a long session piled them up: about 38 a frame, 3.16 million copies and
   405 MB of 128-byte blocks plus a 64 MB index after 27 minutes at the towns (analysis/long18: heap --showSizes), the
   bulk of the long-session memory growth. Now a mark-and-sweep on the replay thread, before a submit's replay, in windows
   of PIN_SWEEP_SUBMITS submits (the live recordings, 39,000 copies / 14 MiB, walked in PIN_SCAN_SLICES steps, one per
   submit: a whole walk at once took 3-6 ms): a copy is marked if a live recording copy refers to it (the ops that hold one: set render
   targets, copy buffer <-> texture, clear texture, copy texture -> texture), if the replay's current render targets do
   (they carry over to the next submit, rt_same reads them), or if it was pinned in the last PIN_SWEEP_SUBMITS submits (a
   recording in progress). It is freed after two windows in a row without a mark: a recording in progress that inlined a
   list released since then, or a copy made after its bucket was walked, is walked in the next one. Marking goes by pointer through a set of the live copies, so a
   stale pointer in an op is never dereferenced. TOTK_PIN_SWEEP=0 keeps every copy (the old behaviour). */
#define PIN_SWEEP_SUBMITS 600   /* one marking window */
#define PIN_SCAN_SLICES 120     /* the live recordings are walked in this many steps, one per submit, early in a window */
typedef struct { tex_t t; uint64_t last_pin, alive_sweep; } pin_t;   /* t first: ops hold (const tex_t *) pins */
_Static_assert(offsetof(pin_t, t) == 0, "ops hold pins as tex_t pointers");
typedef struct { pin_t **v; size_t cap, n; } pin_set;   /* the live copies by address, for marking without dereferencing */
static pthread_mutex_t g_pin_mu = PTHREAD_MUTEX_INITIALIZER;   /* lock order: g_copy_mu, then g_pin_mu */
static pin_t **g_pin_tab; static size_t g_pin_cap, g_pin_n;
static pin_set g_pin_ptrs;
static uint64_t g_pin_sweep_id = 1, g_pin_freed, g_pin_marks;
static size_t pin_slot(uint64_t serial, size_t cap) { return (size_t)((serial * 0x9E3779B97F4A7C15ull) & (cap - 1)); }
static size_t ptr_slot(const void *p, size_t cap) { return (size_t)((((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull) >> 16) & (cap - 1); }
static void ptrs_put(pin_set *s, pin_t *p) { size_t j = ptr_slot(p, s->cap); while (s->v[j]) j = (j + 1) & (s->cap - 1); s->v[j] = p; s->n++; }
static void pin_rebuild(size_t cap) {   /* g_pin_mu held: the serial table and the address set from the live copies (entries set to NULL are gone) */
    pin_t **t2 = calloc(cap, sizeof *t2); pin_set s2 = {calloc(cap, sizeof(pin_t *)), cap, 0};
    if (!t2 || !s2.v) die("texture pin table");
    for (size_t i = 0; i < g_pin_cap; i++) if (g_pin_tab[i]) {
        size_t j = pin_slot(g_pin_tab[i]->t.serial, cap); while (t2[j]) j = (j + 1) & (cap - 1); t2[j] = g_pin_tab[i];
        ptrs_put(&s2, g_pin_tab[i]);
    }
    free(g_pin_tab); g_pin_tab = t2; g_pin_cap = cap;
    free(g_pin_ptrs.v); g_pin_ptrs = s2;
}
static const tex_t *tex_pin(const NVNtexture *nt) {
    const tex_t *t = (const tex_t *)nt;
    if (!t) return NULL;
    if (!t->serial) die("recording references an uninitialized texture object %p", (const void *)t);
    pthread_mutex_lock(&g_pin_mu);
    if ((g_pin_n + 1) * 2 > g_pin_cap) pin_rebuild(g_pin_cap ? g_pin_cap * 2 : 4096);
    size_t j = pin_slot(t->serial, g_pin_cap);
    while (g_pin_tab[j] && g_pin_tab[j]->t.serial != t->serial) j = (j + 1) & (g_pin_cap - 1);
    if (!g_pin_tab[j]) {
        pin_t *c = malloc(sizeof *c); if (!c) die("texture pin copy");
        c->t = *t; c->alive_sweep = g_pin_sweep_id; g_pin_tab[j] = c; g_pin_n++;
        ptrs_put(&g_pin_ptrs, c);   /* same capacity as the serial table, so it has room */
    }
    g_pin_tab[j]->last_pin = __atomic_load_n(&g_submits, __ATOMIC_RELAXED);
    const tex_t *r = &g_pin_tab[j]->t;
    pthread_mutex_unlock(&g_pin_mu);
    return r;
}
static void pin_mark(const void *p) {   /* g_pin_mu held */
    if (!p || !g_pin_ptrs.cap) return;
    for (size_t j = ptr_slot(p, g_pin_ptrs.cap); g_pin_ptrs.v[j]; j = (j + 1) & (g_pin_ptrs.cap - 1))
        if ((const void *)g_pin_ptrs.v[j] == p) { g_pin_ptrs.v[j]->alive_sweep = g_pin_sweep_id; g_pin_marks++; return; }
}
static int pin_negative(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_PROBE_NEGATIVE"); v = e && atoi(e) == 21; } return v; }
static void pin_mark_ops(const uint8_t *p) {   /* one recording copy: OP_BEGIN ... OP_END */
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.op == OP_END || h.size < 8) return;
        const uint8_t *a = p + 8;
        switch (h.op) {
            case OP_SET_RT: { if (pin_negative()) break;   /* probe negative 21: render targets of recordings not marked */
                              const p_set_rt *r = (const void *)a; int n = r->n < 0 ? 0 : r->n > 8 ? 8 : r->n;
                              for (int i = 0; i < n; i++) pin_mark(r->color[i]); pin_mark(r->depth); break; }
            case OP_COPY_B2T: case OP_COPY_T2B: pin_mark(((const p_copy_b2t *)(const void *)a)->tex); break;
            case OP_CLEAR_TEX: pin_mark(((const p_clear_tex *)(const void *)a)->tex); break;
            case OP_COPY_T2T: { const p_copy_t2t *c = (const void *)a; pin_mark(c->src); pin_mark(c->dst); break; }
            default: break;
        }
        p += h.size;
    }
}
void mtl_pin_refs(void (*mark)(const void *));   /* nvn_mtl.m: the replay state that outlives a submit */
static int pin_sweep_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_PIN_SWEEP"); v = !(e && !strcmp(e, "0")); if (!v) LOG("[nvn] TOTK_PIN_SWEEP=0：录制里的贴图副本一直保留（旧行为，长局内存会涨）"); } return v; }
/* One window: the copy buckets are walked in slices (pin_scan), then pin_decide frees what no recording, no replay state
   and no recent recording refers to in this window and the one before. A bucket-table resize during the walk makes the
   window incomplete: no decision for it (a skipped bucket must not cost a reference). */
static size_t g_pin_scan_pos, g_pin_scan_cap; static int g_pin_scan_done = 1, g_pin_scan_ok; static uint64_t g_pin_win_end, g_pin_scanned;
static void pin_window_start(uint64_t now) {   /* g_copy_mu held */
    cbk_compact();
    g_pin_win_end = now + PIN_SWEEP_SUBMITS; g_pin_scan_pos = 0; g_pin_scan_cap = g_cbk_cap; g_pin_scan_done = 0; g_pin_scan_ok = 1; g_pin_scanned = 0;
}
static void pin_scan(size_t buckets) {   /* g_copy_mu and g_pin_mu held */
    if (g_pin_scan_done) return;
    if (g_cbk_cap != g_pin_scan_cap) { g_pin_scan_ok = 0; g_pin_scan_done = 1; return; }
    size_t end = g_pin_scan_pos + buckets < g_pin_scan_cap ? g_pin_scan_pos + buckets : g_pin_scan_cap;
    volatile size_t k = g_pin_scan_pos; volatile uint32_t i = 0;
    sigjmp_buf jb;
    if (sigsetjmp(jb, 1)) { runtime_fault_guard = NULL; copy_drop(&g_cbk[k], i, 1); g_copy_mark_faults++; }   /* reading copy (k, i)'s mark faulted: its control memory is unmapped */
    g_mark_jb = &jb;
    for (; k < end; k++, i = 0) {
        copy_bucket *b = &g_cbk[k]; if (!b->used) continue;
        while (i < b->n) {
            if (b->v[i].orphan && atomic_load(&copy_of((NVNcommandHandle)(uintptr_t)b->v[i].copy)->used) == 2) { copy_drop(b, i, 2); continue; }
            if (b->v[i].token && !copy_token_intact(&b->v[i])) { copy_drop(b, i, 1); continue; }   /* its control memory was reused: the copy goes */
            pin_mark_ops(b->v[i].copy); g_pin_scanned++; i++;
        }
    }
    g_mark_jb = NULL;
    g_pin_scan_pos = end; if (end >= g_pin_scan_cap) g_pin_scan_done = 1;
}
/* Linear probing without tombstones: take the entry at slot i out and move each later entry of the run back if its home
   slot does not lie in the gap (Knuth's algorithm R). home(): the slot an entry hashes to. */
static void probe_delete(pin_t **tab, size_t cap, size_t i, size_t (*home)(const pin_t *, size_t)) {
    for (size_t j = i;;) {
        tab[i] = NULL;
        for (;;) {
            j = (j + 1) & (cap - 1);
            if (!tab[j]) return;
            size_t k = home(tab[j], cap);
            if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue;   /* home inside (i, j]: it may stay */
            break;
        }
        tab[i] = tab[j]; i = j;
    }
}
static size_t home_serial(const pin_t *p, size_t cap) { return pin_slot(p->t.serial, cap); }
/* Serials of the copies freed by the last decision: nvn_mtl.m forgets them as finalized serials (mtl_dead_forget, called
   without the pin locks), since only a recording's texture copy can bring a finalized serial back to the replay. */
static uint64_t *g_pin_gone; static size_t g_pin_gone_n, g_pin_gone_cap;
void mtl_dead_forget(const uint64_t *serials, size_t n);   /* nvn_mtl.m */
static void pin_gone_flush(void) {   /* g_pin_mu not held */
    pthread_mutex_lock(&g_pin_mu); uint64_t *l = g_pin_gone; size_t n = g_pin_gone_n; g_pin_gone = NULL; g_pin_gone_n = g_pin_gone_cap = 0; pthread_mutex_unlock(&g_pin_mu);
    if (n) mtl_dead_forget(l, n);
    free(l);
}
#ifdef GAME_BOTW
/* botw.md (the black save thumbnail): a recording copy whose serial was finalised is re-keyed to the texture now living at
   its storage (botw_tex_follow, nvn_mtl.m): the copy's fields become the live texture's, and it moves to the live serial's
   slot of the serial table (its address, which the ops hold, stays). 1 = copy was a pin and is re-keyed; 0 = not a pin. */
int dm_nvn_pin_rekey(const tex_t *copy, const tex_t *live) {
    pthread_mutex_lock(&g_pin_mu);
    size_t j = 0; int found = 0;
    if (g_pin_cap) for (j = pin_slot(copy->serial, g_pin_cap); g_pin_tab[j]; j = (j + 1) & (g_pin_cap - 1)) if (&g_pin_tab[j]->t == copy) { found = 1; break; }
    if (found) {
        pin_t *p = g_pin_tab[j];
        probe_delete(g_pin_tab, g_pin_cap, j, home_serial);
        p->t = *live;
        for (j = pin_slot(p->t.serial, g_pin_cap); g_pin_tab[j]; j = (j + 1) & (g_pin_cap - 1)) {}
        g_pin_tab[j] = p;
    }
    pthread_mutex_unlock(&g_pin_mu);
    return found;
}
#endif
int dm_nvn_pin_has_serial(uint64_t serial) {   /* nvn_mtl.m (texture finalize): a recording may still bring this serial to the replay */
    pthread_mutex_lock(&g_pin_mu);
    int has = 0;
    if (g_pin_cap) for (size_t j = pin_slot(serial, g_pin_cap); g_pin_tab[j]; j = (j + 1) & (g_pin_cap - 1)) if (g_pin_tab[j]->t.serial == serial) { has = 1; break; }
    pthread_mutex_unlock(&g_pin_mu);
    return has;
}
static size_t home_ptr(const pin_t *p, size_t cap) { return ptr_slot(p, cap); }
static size_t pin_decide(uint64_t now, size_t *recent_out) {   /* g_pin_mu held; the window's walk is complete */
    mtl_pin_refs(pin_mark);
    size_t recent = 0, nf = 0;
    static pin_t **gone; static size_t gone_cap;
    for (size_t i = 0; i < g_pin_cap; i++) {
        pin_t *p = g_pin_tab[i]; if (!p) continue;
        if (p->last_pin + PIN_SWEEP_SUBMITS > now) { p->alive_sweep = g_pin_sweep_id; recent++; }
        if (g_pin_sweep_id - p->alive_sweep >= 2) {
            if (nf == gone_cap) { gone_cap = gone_cap ? gone_cap * 2 : 4096; gone = realloc(gone, gone_cap * sizeof *gone); if (!gone) die("texture pin sweep list"); }
            gone[nf++] = p;
        }
    }
    for (size_t f = 0; f < nf; f++) {   /* out of both tables in place (a rebuild of both took ~2 ms at 46,000 copies), then freed */
        pin_t *p = gone[f]; size_t j;
        for (j = pin_slot(p->t.serial, g_pin_cap); g_pin_tab[j] != p; j = (j + 1) & (g_pin_cap - 1)) if (!g_pin_tab[j]) die("texture pin table lost an entry");
        probe_delete(g_pin_tab, g_pin_cap, j, home_serial);
        for (j = ptr_slot(p, g_pin_ptrs.cap); g_pin_ptrs.v[j] != p; j = (j + 1) & (g_pin_ptrs.cap - 1)) if (!g_pin_ptrs.v[j]) die("texture pin set lost an entry");
        probe_delete(g_pin_ptrs.v, g_pin_ptrs.cap, j, home_ptr);
        if (g_pin_gone_n == g_pin_gone_cap) { g_pin_gone_cap = g_pin_gone_cap ? g_pin_gone_cap * 2 : 4096; g_pin_gone = realloc(g_pin_gone, g_pin_gone_cap * sizeof *g_pin_gone); if (!g_pin_gone) die("texture pin sweep list"); }
        g_pin_gone[g_pin_gone_n++] = p->t.serial;
        g_pin_ptrs.n--; g_pin_n--; free(p);
    }
    g_pin_freed += nf; g_pin_sweep_id++; *recent_out = recent;
    return nf;
}
/* The replay thread, before a submit's replay (no replay state is being built). */
static void pin_step(uint64_t now) {
    static uint64_t windows, skipped, ns_total, ns_max, ns_decide_max;
    if (!pin_sweep_on()) return;
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&g_copy_mu); pthread_mutex_lock(&g_pin_mu);
    if (!g_pin_win_end) pin_window_start(now);
    pin_scan((g_pin_scan_cap + PIN_SCAN_SLICES - 1) / PIN_SCAN_SLICES);
    size_t freed = 0, recent = 0; int decided = 0;
    if (now >= g_pin_win_end && g_pin_scan_done) {
        uint64_t td = now_ns();
        if (g_pin_scan_ok) { freed = pin_decide(now, &recent); decided = 1; } else skipped++;
        uint64_t dd = now_ns() - td; if (dd > ns_decide_max) ns_decide_max = dd;
        windows++; pin_window_start(now);
    }
    size_t live = g_pin_n; uint64_t marks = g_pin_marks;
    pthread_mutex_unlock(&g_pin_mu); pthread_mutex_unlock(&g_copy_mu);
    if (decided) pin_gone_flush();
    uint64_t dt = now_ns() - t0; ns_total += dt; if (dt > ns_max) ns_max = dt;
    if (decided && (windows == 1 || windows % 10 == 0))
        LOG("[nvn] 贴图副本回收 至提交 %llu：在册 %zu 个，本周期释放 %zu 个（累计 %llu，周期 %llu 个、扫描没走完不决定 %llu 个），近 %d 次提交录过 %zu 个，累计标记 %llu 次；每次提交平均 %.3f ms、最长 %.2f ms（决定那次最长 %.2f ms）",
            (unsigned long long)now, live, freed, (unsigned long long)g_pin_freed, (unsigned long long)windows, (unsigned long long)skipped, PIN_SWEEP_SUBMITS, recent,
            (unsigned long long)marks, ns_total / 1e6 / (double)now, ns_max / 1e6, ns_decide_max / 1e6);
}
/* The probe's whole sweep in one call: walk every bucket, then decide (now: the submit count it decides at). */
static void pin_sweep(uint64_t now, int force) {
    (void)force;
    pthread_mutex_lock(&g_copy_mu); pthread_mutex_lock(&g_pin_mu);
    pin_window_start(now); pin_scan(g_cbk_cap); size_t recent; if (g_pin_scan_ok) pin_decide(now, &recent);
    g_pin_win_end = 0;   /* the next pin_step starts a fresh window */
    pthread_mutex_unlock(&g_pin_mu); pthread_mutex_unlock(&g_copy_mu);
    pin_gone_flush();
}
size_t dm_nvn_pin_count(void) { pthread_mutex_lock(&g_pin_mu); size_t n = g_pin_n; pthread_mutex_unlock(&g_pin_mu); return n; }
NVN(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *c, int n, const NVNtexture *const *ct, const NVNtextureView *const *cv,
    const NVNtexture *dt, const NVNtextureView *dv) {
    if (n < 0 || n > 8) { LOG("【红】SetRenderTargets 数量 %d", n); n = n < 0 ? 0 : 8; }
    p_set_rt *r = REC(c, OP_SET_RT, p_set_rt); r->n = n;
    for (int i = 0; i < n; i++) { r->color[i] = ct ? tex_pin(ct[i]) : NULL; copy_view(&r->cview[i], &r->has_cview[i], cv ? cv[i] : NULL); }
    r->depth = tex_pin(dt); copy_view(&r->dview, &r->has_dview, dv);
    if (totk_life_on()) for (int i = 0; i <= n; i++) {   // DM_NVN_LIFELOG: render targets of the followed size (i == n: depth)
        const tex_t *x = i < n ? (ct ? (const tex_t *)ct[i] : NULL) : (const tex_t *)dt;
        if (totk_life_match(x)) { totk_life_log("rt cb=%p %s%d ser=%llu mem=%p", (void *)c, i < n ? "c" : "d", i < n ? i : 0, (unsigned long long)x->serial, (void *)x->mem); totk_life_cb_mark(c); }
    }
}

NVN(nvnCommandBufferClearColor, void, NVNcommandBuffer *c, int index, const float *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; memcpy(r->rgba, rgba, 16);
    if (cmdlog_on()) LOG("[cmd] F%llu ClearColor %d rgba=%g,%g,%g,%g mask=%#x", (unsigned long long)g_frame_pub, index, rgba[0], rgba[1], rgba[2], rgba[3], mask);
}
/* Integer clears (Xenoblade Chronicles 3): the same record as ClearColor. Metal reads a clear colour as integers for an
   integer target, and every value up to 2^24 is exact as a float; a larger one is reported, not silently rounded. */
static void clear_color_int(NVNcommandBuffer *c, int index, const double *v, int mask) {
    float rgba[4];
    for (int i = 0; i < 4; i++) { rgba[i] = (float)v[i]; if ((double)rgba[i] != v[i]) { static int n; if (n++ < 4) LOG("【红】integer clear value %.0f is not exact as a float", v[i]); } }
    h_nvnCommandBufferClearColor(c, index, rgba, mask);
}
NVN(nvnCommandBufferClearColori, void, NVNcommandBuffer *c, int index, const int *rgba, int mask) {
    double v[4] = {rgba[0], rgba[1], rgba[2], rgba[3]}; clear_color_int(c, index, v, mask);
}
NVN(nvnCommandBufferClearColorui, void, NVNcommandBuffer *c, int index, const uint32_t *rgba, int mask) {
    double v[4] = {rgba[0], rgba[1], rgba[2], rgba[3]}; clear_color_int(c, index, v, mask);
}
NVN(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *c, float depth, uint32_t dmask, int stencil, int smask) {
    if (cmdlog_on()) LOG("[cmd] F%llu ClearDepthStencil depth=%g dmask=%u stencil=%d smask=%#x", (unsigned long long)g_frame_pub, depth, dmask, stencil, smask);
    p_clear_ds *r = REC(c, OP_CLEAR_DS, p_clear_ds); *r = (p_clear_ds){depth, (int)B(dmask), stencil, smask};
}
NVN(nvnCommandBufferSetViewports, void, NVNcommandBuffer *c, int first, int n, const float *v) {
    if (n < 0 || first + n > 16) { LOG("【红】SetViewports first=%d n=%d", first, n); return; }
    p_viewports *r = REC(c, OP_VIEWPORTS, p_viewports); r->first = first; r->n = n;
    for (int i = 0; i < n; i++) memcpy(r->v[i], v + 4 * i, 16);
}
NVN(nvnCommandBufferSetScissor, void, NVNcommandBuffer *c, int x, int y, int w, int h) { *REC(c, OP_SCISSOR, p_scissor) = (p_scissor){x, y, w, h}; }

/* CPU-only regression: independently recorded handles can share command storage. */
void dm_nvn_probe_commands(void) {
    NVNcommandBuffer a, b, caller;
    uint8_t control[3][256] = {{0}}, commands[4096] = {0};
    pool_t pool = {.mem = commands};
    NVNcommandBuffer *buffers[] = {&a, &b, &caller};
    size_t before = g_ncopies; uint64_t bytes_before = g_copy_bytes;
    for (unsigned i = 0; i < 3; i++) {
        h_nvnCommandBufferInitialize(buffers[i], NULL);
        h_nvnCommandBufferAddControlMemory(buffers[i], control[i], sizeof control[i]);
        h_nvnCommandBufferAddCommandMemory(buffers[i], (NVNmemoryPool *)&pool, 0, sizeof commands);
    }
    h_nvnCommandBufferBeginRecording(&a);
    uint8_t *end = CB(&a)->end;
    ((uint8_t *)&a)[0x10] = 1; /* Actual inline NVN dirty-flag write. */
    if (CB(&a)->end != end) die("command probe: guest flag corrupted allocator");
    h_nvnCommandBufferSetScissor(&a, 11, 22, 33, 44);
    NVNcommandHandle ha = h_nvnCommandBufferEndRecording(&a);
    h_nvnCommandBufferFinalize(&a); /* Handles outlive their recorder. */
    h_nvnCommandBufferAddCommandMemory(&b, (NVNmemoryPool *)&pool, 0, sizeof commands);
    if (g_ncopies != before + 1) die("command probe: shared command memory retired a handle");
    h_nvnCommandBufferBeginRecording(&b);
    h_nvnCommandBufferSetScissor(&b, 55, 66, 77, 88);
    NVNcommandHandle hb = h_nvnCommandBufferEndRecording(&b);
    h_nvnCommandBufferBeginRecording(&caller);
    h_nvnCommandBufferCallCommands(&caller, 1, &ha);
    h_nvnCommandBufferCopyCommands(&caller, 1, &hb);
    NVNcommandHandle hc = h_nvnCommandBufferEndRecording(&caller);
    h_nvnCommandBufferAddControlMemory(&b, control[0], sizeof control[0]);
    if (g_ncopies != before + 2) die("command probe: control reuse retired wrong recordings");
    copy_touch(hb, g_submits); copy_touch(hc, g_submits);
    const uint8_t *p = (const uint8_t *)(uintptr_t)hc;
    const p_scissor expected[] = {{11,22,33,44}, {55,66,77,88}};
    unsigned n = 0;
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.op == OP_END) break;
        if (h.op == OP_SCISSOR) {
            if (n >= 2 || memcmp(p + 8, &expected[n++], sizeof(p_scissor)))
                die("command probe: cached payload changed");
        }
        p += h.size;
    }
    if (n != 2) die("command probe: cached commands lost");
    h_nvnCommandBufferAddControlMemory(&b, control[1], sizeof control[1]);
    h_nvnCommandBufferAddControlMemory(&caller, control[2], sizeof control[2]);
    h_nvnCommandBufferFinalize(&b); h_nvnCommandBufferFinalize(&caller);
    if (g_ncopies != before || g_copy_bytes != bytes_before) die("command probe: copy leak");
}
/* TOTK (2026-10-17): a recording copy goes when its control memory is reused for other data (its mark there is gone) or
   unmapped (reading the mark faults), and not otherwise: a copy whose mark is intact, or one recorded without room for a
   mark, stays. Negative 22 (marks never checked) must fail here. */
void dm_nvn_probe_copy_marks(void) {
    static uint8_t ctl[3][256], cmds[4096]; memset(ctl, 0, sizeof ctl);
    pool_t pool = {.mem = cmds};
    size_t pg = (size_t)getpagesize(); uint8_t *gone = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (gone == MAP_FAILED) die("copy mark probe: mmap");
    NVNcommandBuffer a, b, c, d; NVNcommandBuffer *all[] = {&a, &b, &c, &d}; uint8_t *mem[] = {ctl[0], ctl[1], ctl[2], gone};
    size_t size[] = {sizeof ctl[0], sizeof ctl[1], 48 /* no room for a mark */, pg};
    size_t before = g_ncopies; uint64_t by_mark0 = g_copy_drop_token;
    NVNcommandHandle h[4];
    for (int i = 0; i < 4; i++) {
        h_nvnCommandBufferInitialize(all[i], NULL);
        h_nvnCommandBufferAddControlMemory(all[i], mem[i], size[i]);
        h_nvnCommandBufferAddCommandMemory(all[i], (NVNmemoryPool *)&pool, 0, sizeof cmds);
        h_nvnCommandBufferBeginRecording(all[i]); h_nvnCommandBufferSetScissor(all[i], i, i, 1, 1); h[i] = h_nvnCommandBufferEndRecording(all[i]);
    }
    uint64_t m[2]; memcpy(m, ctl[0], sizeof m);
    if (m[0] != COPY_TOKEN_MAGIC || m[1] != (uint64_t)h[0]) die("copy mark probe: EndRecording did not write its mark into the control memory");
    memcpy(m, ctl[2], sizeof m); if (m[0] == COPY_TOKEN_MAGIC) die("copy mark probe: a mark was written past the control memory the game gave");
    memset(ctl[0], 0xa5, 16); memset(ctl[2], 0xa5, 16);   /* the game reuses a's and c's control memory for its own data */
    if (mprotect(gone, pg, PROT_NONE)) die("copy mark probe: mprotect");   /* and d's is gone */
    pin_sweep(g_submits, 1);
    { sigset_t cur; sigprocmask(SIG_BLOCK, NULL, &cur); if (sigismember(&cur, SIGSEGV) || sigismember(&cur, SIGBUS)) die("copy mark probe: the fault guard left SIGSEGV / SIGBUS blocked"); }
    if (g_ncopies == before + 4) die("copy mark probe: a recording copy whose control memory was reused was not released");
    if (g_ncopies != before + 2 || g_copy_drop_token != by_mark0 + 2)
        die("copy mark probe: %zu of 4 copies left (expected 2: the intact mark and the one without a mark)", g_ncopies - before);
    copy_touch(h[1], g_submits); copy_touch(h[2], g_submits);   /* the two kept ones are still live recordings */
    munmap(gone, pg);
    h_nvnCommandBufferAddControlMemory(&b, ctl[1], sizeof ctl[1]); h_nvnCommandBufferAddControlMemory(&c, ctl[2], 48);
    for (int i = 0; i < 4; i++) h_nvnCommandBufferFinalize(all[i]);
    if (g_ncopies != before) die("copy mark probe: copy leak");
}
/* TOTK (2026-10-17): texture copies held by recordings go after two windows without a reference (the window a copy is
   made in counts as referenced), and not earlier: not while a live recording, the replay's current render targets or a
   recent recording (pinned in the last window) still refers to them. Negative 21 (render targets of recordings not
   marked) must fail here. */
void mtl_probe_pin_rt(const tex_t *t);   /* nvn_mtl.m: the replay's current colour target (NULL: none) */
void dm_nvn_probe_texture_pins(void) {
    {   /* in-place deletion keeps every remaining entry findable: 3,000 entries in 8,192 slots (clusters wrap around), half deleted at random */
        enum { N = 3000, CAP = 8192 }; static pin_t pins[N]; static pin_t *tab[CAP]; static uint8_t gone[N];
        memset(tab, 0, sizeof tab); memset(gone, 0, sizeof gone); uint64_t x = 0x9E3779B97F4A7C15ull;
        for (int i = 0; i < N; i++) { pins[i].t.serial = 0xfeed100000000000ull + (uint64_t)i * 977u; size_t j = pin_slot(pins[i].t.serial, CAP); while (tab[j]) j = (j + 1) & (CAP - 1); tab[j] = &pins[i]; }
        for (int r = 0; r < N / 2; r++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17; int i = (int)(x % N); if (gone[i]) continue;
            size_t j = pin_slot(pins[i].t.serial, CAP); while (tab[j] != &pins[i]) { if (!tab[j]) die("pin probe: entry lost before deletion"); j = (j + 1) & (CAP - 1); }
            probe_delete(tab, CAP, j, home_serial); gone[i] = 1;
        }
        size_t left = 0; for (int k = 0; k < CAP; k++) left += tab[k] != NULL;
        for (int i = 0; i < N; i++) {
            size_t j = pin_slot(pins[i].t.serial, CAP); int found = 0;
            for (; tab[j]; j = (j + 1) & (CAP - 1)) if (tab[j] == &pins[i]) { found = 1; break; }
            if (found == gone[i]) die("pin probe: in-place deletion %s entry %d", gone[i] ? "kept a deleted" : "lost a remaining", i);
            left -= !gone[i];
        }
        if (left) die("pin probe: in-place deletion left stray entries");
    }
    const size_t pins0 = dm_nvn_pin_count();
    tex_t t[3]; memset(t, 0, sizeof t); for (int i = 0; i < 3; i++) t[i].serial = 0xfeed0000ull + (uint64_t)i;   /* serials no real texture has */
    NVNcommandBuffer ra, rb; uint8_t ctl_a[256] = {0}, ctl_b[256] = {0}, cmds[4096] = {0}; pool_t pool = {.mem = cmds};
    NVNcommandBuffer *both[] = {&ra, &rb}; uint8_t *ctl[] = {ctl_a, ctl_b};
    for (int i = 0; i < 2; i++) { h_nvnCommandBufferInitialize(both[i], NULL); h_nvnCommandBufferAddControlMemory(both[i], ctl[i], 256); h_nvnCommandBufferAddCommandMemory(both[i], (NVNmemoryPool *)&pool, 0, sizeof cmds); }
    const NVNtexture *c0 = (const NVNtexture *)&t[0], *c2 = (const NVNtexture *)&t[2];
    h_nvnCommandBufferBeginRecording(&ra); h_nvnCommandBufferSetRenderTargets(&ra, 1, &c0, NULL, (const NVNtexture *)&t[1], NULL); NVNcommandHandle ha = h_nvnCommandBufferEndRecording(&ra);
    h_nvnCommandBufferBeginRecording(&rb); h_nvnCommandBufferSetRenderTargets(&rb, 1, &c2, NULL, NULL, NULL); (void)h_nvnCommandBufferEndRecording(&rb);
    if (dm_nvn_pin_count() != pins0 + 3) die("pin probe: three copies expected, %zu", dm_nvn_pin_count() - pins0);
    h_nvnCommandBufferAddControlMemory(&rb, ctl_b, 256);   /* re-adding its control memory retires rb's recording: t[2]'s copy has no reference now */
    const uint64_t far = g_submits + 10 * PIN_SWEEP_SUBMITS;   /* long after the recordings: none counts as recent */
    /* the window a copy is made in counts as referenced; it goes after two more windows without a reference */
    pin_sweep(far, 1); if (dm_nvn_pin_count() != pins0 + 3) die("pin probe: a copy went in the window it was made in");
    pin_sweep(far + 1, 1); if (dm_nvn_pin_count() != pins0 + 3) die("pin probe: a copy went after one window without a reference");
    pin_sweep(far + 2, 1);
    if (dm_nvn_pin_count() < pins0 + 2) die("pin probe: copies a live recording refers to were freed");
    if (dm_nvn_pin_count() > pins0 + 2) die("pin probe: the unreferenced copy was not freed after two windows without a reference");
    pin_sweep(far + 3, 1); pin_sweep(far + 4, 1); if (dm_nvn_pin_count() != pins0 + 2) die("pin probe: copies a live recording refers to were freed");
    const uint8_t *p = (const uint8_t *)(uintptr_t)ha; const p_set_rt *rt = NULL;
    for (;;) { op_hdr h; memcpy(&h, p, sizeof h); if (h.op == OP_END) break; if (h.op == OP_SET_RT) rt = (const void *)(p + 8); p += h.size; }
    if (!rt || !rt->color[0] || rt->color[0]->serial != t[0].serial || !rt->depth || rt->depth->serial != t[1].serial) die("pin probe: the live recording lost its targets");
    /* the replay's current render target keeps its copy after its recording is gone */
    mtl_probe_pin_rt(rt->color[0]);
    h_nvnCommandBufferAddControlMemory(&ra, ctl_a, 256);   /* retires ha */
    pin_sweep(far + 5, 1); pin_sweep(far + 6, 1); pin_sweep(far + 7, 1);
    if (dm_nvn_pin_count() != pins0 + 1) die("pin probe: %zu copies left, expected only the replay's render target", dm_nvn_pin_count() - pins0);
    mtl_probe_pin_rt(NULL);
    /* a copy pinned in the last window stays (a recording in progress may hold it) */
    h_nvnCommandBufferBeginRecording(&rb); h_nvnCommandBufferSetRenderTargets(&rb, 1, &c2, NULL, NULL, NULL); (void)h_nvnCommandBufferEndRecording(&rb);
    h_nvnCommandBufferAddControlMemory(&rb, ctl_b, 256);
    const uint64_t now = g_submits; pin_sweep(now, 1); pin_sweep(now, 1); pin_sweep(now, 1);
    if (dm_nvn_pin_count() != pins0 + 2) die("pin probe: a recently pinned copy was freed (%zu left)", dm_nvn_pin_count() - pins0);
    pin_sweep(far + 8, 1); pin_sweep(far + 9, 1);
    if (dm_nvn_pin_count() != pins0) die("pin probe: %zu copies stayed after nothing referred to them", dm_nvn_pin_count() - pins0);
    h_nvnCommandBufferFinalize(&ra); h_nvnCommandBufferFinalize(&rb);
}
/* Compare the complete production recording and allocator accounting against
   the original per-record copier, including growth, nesting and source reuse. */
void dm_nvn_probe_inline_bulk(void) {
    int saved = atomic_load(&g_inline_bulk_on); unsigned checks = 0;
    const unsigned lengths[] = {0, 1, 7, 3000};
    size_t copies_before = g_ncopies; uint64_t bytes_before = g_copy_bytes;
    for (unsigned scenario = 0; scenario < sizeof lengths / sizeof *lengths; scenario++) {
        NVNcommandBuffer cb[4]; uint8_t control[4][256] = {{0}}, commands[4096] = {0};
        pool_t pool = {.mem = commands};
        for (unsigned i = 0; i < 4; i++) {
            h_nvnCommandBufferInitialize(&cb[i], NULL);
            h_nvnCommandBufferAddControlMemory(&cb[i], control[i], sizeof control[i]);
            h_nvnCommandBufferAddCommandMemory(&cb[i], (NVNmemoryPool *)&pool, 0, sizeof commands);
        }
        h_nvnCommandBufferBeginRecording(&cb[0]);
        uint64_t payload[17]; for (unsigned j = 0; j < 17; j++) payload[j] = 0x12340000u + j;
        for (unsigned j = 0; j < lengths[scenario]; j++) {
            h_nvnCommandBufferSetScissor(&cb[0], j, j + 1, j + 2, j + 3);
            if (j % 7 == 0) h_nvnCommandBufferUpdateUniformBuffer(&cb[0], 0x1000, sizeof payload, 0, (j % 17 + 1) * 8, payload);
        }
        NVNcommandHandle source = h_nvnCommandBufferEndRecording(&cb[0]);
        for (unsigned mode = 0; mode < 2; mode++) {
            atomic_store(&g_inline_bulk_on, mode);
            g_inline_bulk_negative = mode && scenario && getenv("TOTK_PROBE_NEGATIVE") && atoi(getenv("TOTK_PROBE_NEGATIVE")) == 16;
            h_nvnCommandBufferBeginRecording(&cb[mode + 1]);
            h_nvnCommandBufferSetScissor(&cb[mode + 1], 91, 92, 93, 94);
            h_nvnCommandBufferCallCommands(&cb[mode + 1], 1, &source);
            h_nvnCommandBufferCopyCommands(&cb[mode + 1], 1, &source);
        }
        cmd_t *a = CB(&cb[1]), *b = CB(&cb[2]);
        size_t used = a->cur - a->seg;
        if ((size_t)(b->cur - b->seg) != used || memcmp(a->seg, b->seg, used)) die("inline bulk probe: copied bytes differ"); checks++;
        if (a->cmd_used != b->cmd_used || a->hw_used != b->hw_used || a->last_rec != b->last_rec) die("inline bulk probe: accounting differs"); checks++;
        // An operation after the call must still fix up its own hardware size.
        hw_fix(a, 12); hw_fix(b, 12);
        if (a->hw_used != b->hw_used || a->last_rec != b->last_rec) die("inline bulk probe: hardware fix differs"); checks++;
        NVNcommandHandle reference = h_nvnCommandBufferEndRecording(&cb[1]);
        NVNcommandHandle fast = h_nvnCommandBufferEndRecording(&cb[2]);
        h_nvnCommandBufferBeginRecording(&cb[3]);
        h_nvnCommandBufferCallCommands(&cb[3], 1, &fast);
        NVNcommandHandle nested = h_nvnCommandBufferEndRecording(&cb[3]);
        h_nvnCommandBufferAddControlMemory(&cb[0], control[0], sizeof control[0]);
        h_nvnCommandBufferAddControlMemory(&cb[2], control[2], sizeof control[2]);
        // source and intermediate fast handle are gone; nested bytes must survive.
        size_t full_size = copy_of(reference)->body_bytes + 16;
        if (memcmp((const void *)(uintptr_t)reference, (const void *)(uintptr_t)nested, full_size)) die("inline bulk probe: nested copy changed after source retirement"); checks++;
        for (unsigned i = 0; i < 4; i++) {
            h_nvnCommandBufferAddControlMemory(&cb[i], control[i], sizeof control[i]);
            h_nvnCommandBufferFinalize(&cb[i]);
        }
    }
    if (g_ncopies != copies_before || g_copy_bytes != bytes_before) die("inline bulk probe: copy leak"); checks++;
    g_inline_bulk_negative = 0; atomic_store(&g_inline_bulk_on, saved);
    LOG("inline bulk probe: %u byte/accounting/growth/nesting/lifetime checks passed", checks);
}
NVN(nvnCommandBufferSetDepthRange, void, NVNcommandBuffer *c, float n, float f) { *REC(c, OP_DEPTH_RANGE, p_depth_range) = (p_depth_range){n, f}; }
// TOTK: the replay keeps one scissor / depth range (index 0); other indices only matter to shaders that
// select a viewport index, so differing values there are reported instead of silently used.
NVN(nvnCommandBufferSetViewport, void, NVNcommandBuffer *c, int x, int y, int w, int h) { float v[4] = {(float)x, (float)y, (float)w, (float)h}; h_nvnCommandBufferSetViewports(c, 0, 1, v); }
NVN(nvnCommandBufferSetScissors, void, NVNcommandBuffer *c, int first, int n, const int *r) {
    if (first < 0 || n < 0 || first + n > 16) die("SetScissors first=%d n=%d", first, n);
    for (int i = 1; i < n; i++) if (memcmp(r + 4 * i, r, 16)) { static int once; if (!once++) LOG("【红】SetScissors: differing scissor %d ignored (single-scissor replay)", first + i); }
    if (first == 0 && n > 0) h_nvnCommandBufferSetScissor(c, r[0], r[1], r[2], r[3]);
}
NVN(nvnCommandBufferSetDepthRanges, void, NVNcommandBuffer *c, int first, int n, const float *r) {
    if (first < 0 || n < 0 || first + n > 16) die("SetDepthRanges first=%d n=%d", first, n);
    for (int i = 1; i < n; i++) if (memcmp(r + 2 * i, r, 8)) { static int once; if (!once++) LOG("【红】SetDepthRanges: differing range %d ignored (single-range replay)", first + i); }
    if (first == 0 && n > 0) h_nvnCommandBufferSetDepthRange(c, r[0], r[1]);
}
NVN(nvnCommandBufferSetDepthClamp, void, NVNcommandBuffer *c, uint32_t e) { REC(c, OP_DEPTH_CLAMP, p_flag)->value = B(e) != 0; }
NVN(nvnCommandBufferSetRenderEnable, void, NVNcommandBuffer *c, uint32_t e) {
    if (cmdlog_on()) LOG("[cmd] F%llu SetRenderEnable %d", (unsigned long long)g_frame_pub, B(e) != 0);
    REC(c, OP_RENDER_ENABLE, p_flag)->value = B(e) != 0; }
// Conditional rendering (NVN SetRenderEnableConditional): rendering is enabled when the two NVNcounterData.counter values at
// addr / addr+16 compare equal (mode 0) or not equal (mode 1). TOTK gates most of its ClearColor commands with it: the CPU
// writes counter=1 to request a clear and the TIMESTAMP_TOP report issued right after the clear resets it to 0, so the
// clear runs exactly once. The replay evaluates it in GPU order (see cond_eval in nvn_mtl.m).
NVN(nvnCommandBufferSetRenderEnableConditional, void, NVNcommandBuffer *c, NVNconditionalRenderMode m, NVNbufferAddress a) {
    if (cmdlog_on()) { void *bf; uint64_t off; uint64_t x = 0, xt = 0, y = 0, yt = 0; int ok = mtl_addr(a, 32, &bf, &off);
        if (ok) { memcpy(&x, (const void *)(uintptr_t)a, 8); memcpy(&xt, (const void *)(uintptr_t)(a + 8), 8); memcpy(&y, (const void *)(uintptr_t)(a + 16), 8); memcpy(&yt, (const void *)(uintptr_t)(a + 24), 8); }
        LOG("[cmd] F%llu SetRenderEnableConditional mode=%d addr=%#llx mapped=%d x=%llu/%llu y=%llu/%llu", (unsigned long long)g_frame_pub, (int)m, (unsigned long long)a, ok,
            (unsigned long long)x, (unsigned long long)xt, (unsigned long long)y, (unsigned long long)yt); }
    if ((int)m != 0 && (int)m != 1) die("SetRenderEnableConditional: unknown mode %d", (int)m);
    p_render_cond *r = REC(c, OP_RENDER_COND, p_render_cond); r->mode = (int32_t)m; r->pad = 0; r->addr = a;
}
// Rasterizer discard runs vertex work without rasterising; with no transform feedback that is the same as not drawing.
NVN(nvnCommandBufferSetRasterizerDiscard, void, NVNcommandBuffer *c, uint32_t e) { REC(c, OP_RASTER_DISCARD, p_flag)->value = B(e) != 0; }
/* Transform feedback (Xenoblade Chronicles 3): recorded here, emulated at replay (nvn_mtl.m tf_*): the vertex shader stores its
   captured outputs to the bound buffers through the translator's emulation. */
NVN(nvnCommandBufferBindTransformFeedbackBuffer, void, NVNcommandBuffer *c, int index, NVNbufferAddress a, size_t size) {
    if (index < 0 || index >= 4) { LOG("【红】BindTransformFeedbackBuffer index %d", index); return; }
    *REC(c, OP_TF_BIND, p_tf_bind) = (p_tf_bind){index, 0, (uint64_t)a, (uint64_t)size};
}
NVN(nvnCommandBufferBindTransformFeedbackBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *r) {
    for (int i = 0; i < n; i++) h_nvnCommandBufferBindTransformFeedbackBuffer(c, first + i, r ? r[i].address : 0, r ? (size_t)r[i].size : 0);
}
static void tf_ctl(NVNcommandBuffer *c, int kind, NVNbufferAddress ctl, int prim) { *REC(c, OP_TF_CTL, p_tf_ctl) = (p_tf_ctl){(uint64_t)ctl, kind, prim}; }
NVN(nvnCommandBufferBeginTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { tf_ctl(c, TF_BEGIN, a, 0); }
NVN(nvnCommandBufferEndTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { tf_ctl(c, TF_END, a, 0); }
NVN(nvnCommandBufferPauseTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { tf_ctl(c, TF_PAUSE, a, 0); }
NVN(nvnCommandBufferResumeTransformFeedback, void, NVNcommandBuffer *c, NVNbufferAddress a) { tf_ctl(c, TF_RESUME, a, 0); }
NVN(nvnCommandBufferDrawTransformFeedback, void, NVNcommandBuffer *c, NVNdrawPrimitive p, NVNbufferAddress a) { tf_ctl(c, TF_DRAW, a, (int)p); }
/* TOTK diagnostic: TOTK_BINDPROG_LOG=<frame_lo>-<frame_hi> prints every BindProgram of those frames with the program object, its
   vertex-stage shader data address and the guest call chain (module+offset), so a program choice can be traced to game code. */
static uint64_t g_frame;
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off);
static _Atomic int g_diag_env_once = -1;
static const char *bindprog_env(void) { return getenv("TOTK_BINDPROG_LOG"); }
static int diag_env_once_on(void) {
    int v = atomic_load_explicit(&g_diag_env_once, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("TOTK_DIAG_ENV_ONCE"); int initial = -1;
        int desired = e && !strcmp(e, "1");
        atomic_compare_exchange_strong(&g_diag_env_once, &initial, desired);
        v = atomic_load_explicit(&g_diag_env_once, memory_order_relaxed);
    }
    return v;
}
void nvn_diag_env_once_set(int v) {
    atomic_store_explicit(&g_diag_env_once, v != 0, memory_order_relaxed);
    LOG("[flag] diag_env_once = %d", v != 0);
}
static void bindprog_log(const prog_t *o, int stages) {
    static nvn_diag_range_cache range;
    static _Atomic unsigned lines;
    nvn_diag_range_value v = nvn_diag_range_get(&range, diag_env_once_on(), bindprog_env);
    if (v.lo < 0 || (long)g_frame < v.lo || (long)g_frame > v.hi) return;
    if (atomic_load_explicit(&lines, memory_order_relaxed) >= 20000) return;
    if (atomic_fetch_add_explicit(&lines, 1, memory_order_relaxed) >= 20000) return;
    char b[700]; int n = snprintf(b, sizeof b, "[bindprog] frame=%llu mask=%#x prog=%p serial=%llu nstage=%u data0=%#llx ctl0=%p chain:", (unsigned long long)g_frame, stages, (const void *)o, (unsigned long long)(o ? o->serial : 0), o ? o->nstage : 0, o && o->nstage ? (unsigned long long)o->sd[0].data : 0ull, o && o->nstage ? o->sd[0].control : NULL);
    uint64_t *fp = __builtin_frame_address(0); uint64_t *sw = fp ? (uint64_t *)fp[0] : NULL;   /* our frame -> runtime_hle_switch frame record {guest fp, guest lr} */
    uint64_t gfp = sw ? sw[0] : 0, glr = sw ? sw[1] : 0;
    for (int d = 0; d < 8 && n < (int)sizeof b - 48; d++) {
        const char *nm; uint64_t off;
        if (runtime_module_offset(glr, &nm, &off)) n += snprintf(b + n, sizeof b - n, " %s+%#llx", nm, (unsigned long long)off); else n += snprintf(b + n, sizeof b - n, " %#llx", (unsigned long long)glr);
        if (!gfp || (gfp & 7)) break; uint64_t *f = (uint64_t *)(uintptr_t)gfp; glr = f[1]; gfp = f[0]; if (!glr) break;
    }
    LOG("%s", b);
}
typedef struct { const void *cb; const prog_t *prog; uint64_t tex[32], img[8]; } cs_shadow;
static cs_shadow g_csh[64];
static pthread_mutex_t g_csh_mu = PTHREAD_MUTEX_INITIALIZER;
static cs_shadow *csh_get(const void *cb) {
    size_t i = ((uintptr_t)cb >> 4) & 63;
    for (int k = 0; k < 64; k++) { cs_shadow *s = &g_csh[(i + k) & 63]; if (s->cb == cb) return s; if (!s->cb) { memset(s, 0, sizeof *s); s->cb = cb; return s; } }
    cs_shadow *s = &g_csh[i]; memset(s, 0, sizeof *s); s->cb = cb; return s;
}
static void csh_reset(const void *cb) { pthread_mutex_lock(&g_csh_mu); cs_shadow *s = csh_get(cb); memset(s, 0, sizeof *s); s->cb = cb; pthread_mutex_unlock(&g_csh_mu); }
static void csh_dispatch(const void *cb) {
    pthread_mutex_lock(&g_csh_mu); cs_shadow s = *csh_get(cb); pthread_mutex_unlock(&g_csh_mu);
    if (s.prog) mtl_cs_prefetch(s.prog, s.tex, s.img);
}
NVN(nvnCommandBufferBindProgram, void, NVNcommandBuffer *c, const NVNprogram *p, int stages) {
    if (stages & NVN_SHADER_STAGE_COMPUTE_BIT) { pthread_mutex_lock(&g_csh_mu); csh_get(c)->prog = (const prog_t *)p; pthread_mutex_unlock(&g_csh_mu); }
    *REC(c, OP_BIND_PROGRAM, p_bind_program) = (p_bind_program){(const prog_t *)p, stages};
    bindprog_log((const prog_t *)p, stages);
}
static void bind_state(NVNcommandBuffer *c, int kind, const void *st, size_t n) { p_bind_state *r = REC(c, OP_BIND_STATE, p_bind_state); r->kind = kind; memcpy(r->bytes, st, n); }
NVN(nvnCommandBufferBindBlendState, void, NVNcommandBuffer *c, const NVNblendState *st) { bind_state(c, 0, st, sizeof(blend_st)); }
NVN(nvnCommandBufferBindColorState, void, NVNcommandBuffer *c, const NVNcolorState *st) { bind_state(c, 1, st, sizeof(color_st)); }
NVN(nvnCommandBufferBindChannelMaskState, void, NVNcommandBuffer *c, const NVNchannelMaskState *st) { bind_state(c, 2, st, sizeof(chmask_st)); }
NVN(nvnCommandBufferBindMultisampleState, void, NVNcommandBuffer *c, const NVNmultisampleState *st) { bind_state(c, 3, st, sizeof(ms_st)); }
NVN(nvnCommandBufferBindPolygonState, void, NVNcommandBuffer *c, const NVNpolygonState *st) { bind_state(c, 4, st, sizeof(poly_st)); }
NVN(nvnCommandBufferBindDepthStencilState, void, NVNcommandBuffer *c, const NVNdepthStencilState *st) { bind_state(c, 5, st, sizeof(ds_st)); }
NVN(nvnCommandBufferBindVertexAttribState, void, NVNcommandBuffer *c, int n, const NVNvertexAttribState *st) {
    if (n < 0 || n > 16) { LOG("【红】BindVertexAttribState n=%d", n); return; }
    p_bind_vattr *r = RECN(c, OP_BIND_VATTR, p_bind_vattr, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 4 * n); hw_fix(CB(c), 68);   /* the driver always writes all 16 attribute formats (unused = disabled): 17 words */
}
NVN(nvnCommandBufferBindVertexStreamState, void, NVNcommandBuffer *c, int n, const NVNvertexStreamState *st) {
    if (n < 0 || n > 16) { LOG("【红】BindVertexStreamState n=%d", n); return; }
    p_bind_vstream *r = RECN(c, OP_BIND_VSTREAM, p_bind_vstream, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 8 * n); hw_fix(CB(c), 4 * (16 + (size_t)n));
}
NVN(nvnCommandBufferBindVertexBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *rg) {
    if (n < 0 || first + n > 16) { LOG("【红】BindVertexBuffers first=%d n=%d", first, n); return; }
    p_bind_vbuf *r = RECN(c, OP_BIND_VBUF, p_bind_vbuf, r, n); r->first = first; r->n = n; memcpy(r->r, rg, sizeof *rg * n); hw_fix(CB(c), 24 * (size_t)n);
}
NVN(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {
    *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, i, a, n}; hw_fix(CB(c), 20);   /* CB_SIZE/ADDRESS (4 words) + CB_BIND (1) */
}
NVN(nvnCommandBufferBindUniformBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) {
    for (int i = 0; i < n; i++) { *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size}; hw_fix(CB(c), 20); }
}
NVN(nvnCommandBufferBindStorageBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) {
    *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, i, a, n}; hw_fix(CB(c), 20);   /* macro 16 (1 word) + CB_DATA address/size (4) */
}
NVN(nvnCommandBufferBindStorageBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) {
    for (int i = 0; i < n; i++) { *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size}; hw_fix(CB(c), 20); }
}
/* DM_NVN_LIFELOG (totk_lifelog.h): one bound handle of a followed texture; for a deferred bind the logged id is the
   value at record time, the replay's "use" lines show the one resolved at submit. */
static void life_bind(NVNcommandBuffer *c, int op, int st, int slot, const uint64_t *hp, int deferred) {
    int id = (int)(*hp & 0xFFFFF); uint64_t ser = totk_life_reg_serial(id);
    if (!ser) return;
    uint64_t caller = guest_main_caller();
    totk_life_log("bind cb=%p %s%s st=%d slot=%d id=%d ser=%llu hp=%p caller=main+%#llx", (void *)c, op == OP_BIND_TEX ? "tex" : "img", deferred ? " deferred" : "",
                  st, slot, id, (unsigned long long)ser, (const void *)hp, (unsigned long long)caller);
    totk_life_cb_mark(c);
    /* DM_NVN_LIFELOG_WATCH=<main offset of a caller>: write-watch the handle that caller binds first (who stores it, when) */
    { static long want = -2; static int armed; if (want == -2) { const char *e = getenv("DM_NVN_LIFELOG_WATCH"); want = e ? strtol(e, NULL, 0) : -1; }
      if (want > 0 && !armed && caller == (uint64_t)want) { armed = 1; extern void runtime_watch_set(uintptr_t addr, size_t len);
          totk_life_log("watching the handle at %p bound by main+%#lx (id=%d)", (const void *)hp, want, id); runtime_watch_set((uintptr_t)hp, 8); } }
    /* the game's call chain of the first bind per (command buffer, slot): which code picks the handle */
    static const void *chained[32][2]; static int nchained; static pthread_mutex_t chain_mu = PTHREAD_MUTEX_INITIALIZER; int fresh = 0;
    pthread_mutex_lock(&chain_mu);
    { int j = 0; while (j < nchained && !(chained[j][0] == (const void *)c && chained[j][1] == (const void *)(intptr_t)slot)) j++;
      if (j == nchained && nchained < 32) { chained[nchained][0] = c; chained[nchained][1] = (const void *)(intptr_t)slot; nchained++; fresh = 1; } }
    pthread_mutex_unlock(&chain_mu);
    if (fresh) { char w[64]; snprintf(w, sizeof w, "[life] bind chain cb=%p slot=%d", (void *)c, slot); guest_chain(w); }
}
/* The descriptor pools each command buffer set last (cmd_t has no room left: a side table; the first pools by default). */
typedef struct { const void *cb; uint8_t pool[2]; } cb_pools;
static cb_pools g_cb_pools[256]; static int g_cb_pools_n;
static int cb_pools_find(const void *c) { for (int i = 0; i < g_cb_pools_n; i++) if (g_cb_pools[i].cb == c) return i; return -1; }
static void cb_pools_set(NVNcommandBuffer *c, int kind, int idx) {
    pthread_mutex_lock(&g_dp_mu); int i = cb_pools_find(c);
    if (i < 0 && idx) { if (g_cb_pools_n == 256) die("pool table: more than 256 command buffers"); i = g_cb_pools_n++; g_cb_pools[i] = (cb_pools){c, {0, 0}}; }
    if (i >= 0) g_cb_pools[i].pool[kind] = (uint8_t)idx;
    pthread_mutex_unlock(&g_dp_mu);
}
static void cb_pools_get(NVNcommandBuffer *c, uint8_t *tp, uint8_t *sp) {
    pthread_mutex_lock(&g_dp_mu); int i = cb_pools_find(c); *tp = i < 0 ? 0 : g_cb_pools[i].pool[0]; *sp = i < 0 ? 0 : g_cb_pools[i].pool[1]; pthread_mutex_unlock(&g_dp_mu);
}
/* Handles into registration ids: texture id + the texture pool's base; sampler id (bits 20+) + the sampler pool's base.
   A texture handle with sampler 0 is taken as a separate texture handle (no sampler part). */
static void handles_to_ids(int op, uint8_t tpool, uint8_t spool, uint64_t *h, int n) {
    int32_t tb = dpool_base(0, tpool), sb = dpool_base(1, spool);
    if (!tb && !sb) return;
    for (int k = 0; k < n; k++) {
        if (op == OP_BIND_SAMP) { if (h[k]) h[k] = ((h[k] >> 20) + (uint64_t)sb) << 20; continue; }
        uint64_t tid = h[k] & 0xfffff, sid = h[k] >> 20;
        h[k] = (tid + (uint64_t)tb) | (op == OP_BIND_TEX && sid ? sid + (uint64_t)sb : sid) << 20;
    }
}
static void bind_handles_rec(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h, int deferred) {
    if (n < 0 || n > 32) { LOG("【红】绑定句柄数 %d", n); return; }
    p_bind_handles *r = RECN(c, op, p_bind_handles, h, n); r->stage = st; r->first = first; r->n = n; r->pad_[0] = r->pad_[1] = 0;
    cb_pools_get(c, &r->tpool, &r->spool);
    if (st == NVN_SHADER_STAGE_COMPUTE && (op == OP_BIND_TEX || op == OP_BIND_IMG)) {
        pthread_mutex_lock(&g_csh_mu); cs_shadow *s = csh_get(c);
        for (int k = 0; k < n; k++) { int slot = first + k; if (op == OP_BIND_TEX && slot >= 0 && slot < 32) s->tex[slot] = h[k]; else if (op == OP_BIND_IMG && slot >= 0 && slot < 8) s->img[slot] = h[k]; }
        pthread_mutex_unlock(&g_csh_mu);
    }
    r->src = deferred ? (uint64_t)(uintptr_t)h : 0; memcpy(r->h, h, 8 * n); handles_to_ids(op, r->tpool, r->spool, r->h, n);
    if (totk_life_on() && (op == OP_BIND_TEX || op == OP_BIND_IMG)) for (int k = 0; k < n; k++) life_bind(c, op, st, first + k, h + k, deferred);
    hw_fix(CB(c), 16 + 4 * (size_t)n);   /* macro 7 + method 0x1a2c + CB_POS and one 32-bit handle word per binding (64-bit handles: callers override with 16 + 8n) */
}
static void bind_handles(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) { bind_handles_rec(c, op, st, first, n, h, 0); }
/* NVN 55 *Deferred binds (totk_nvn_ext.c): the original driver (sdk+0x46cc00 BindTexturesDeferred) only writes a control
   token {0x17, stage, first, count} and the pointer to the game's handle array; the handles are read when the command
   buffer is submitted. The game relies on that: material command buffers are recorded first and their handle tables
   are filled before the submit. Reading the handles at record time bound the textures registered two frames earlier;
   when the game moved a render target (the linear depth) to other memory, materials, lighting and fog read the old
   memory for two frames: objects flickered and the ground showed through while the camera moved (2026-10-15).
   hw_bytes: command bytes reported for the plain variant (resource heaps are sized from them, keep them unchanged). */
void nvn_bind_handles_deferred(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h, size_t hw_bytes) {
    bind_handles_rec(c, op, st, first, n, h, 1);
    if (hw_bytes && n >= 0 && n <= 32) hw_fix(CB(c), hw_bytes);
}
static _Atomic int g_def_on = -1;                   // TOTK_DEFERRED_BINDS=0 / runtime flag "deferred 0": keep the record-time handles (A/B)
static uint64_t g_def_n, g_def_changed, g_def_logged, g_def_ns;
void nvn_set_deferred_binds(int on) { atomic_store(&g_def_on, on); LOG("[flag] deferred = %d", on); }
static void resolve_deferred(const uint8_t *p, int depth) {   // read the handles of every deferred bind in a submitted recording
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) die("corrupt recording at %p while resolving deferred binds (op %u size %u)", (const void *)p, h.op, h.size);
        if (h.op == OP_END) return;
        if (h.op == OP_JUMP) { memcpy(&p, p + 8, 8); continue; }
        if (h.op == OP_CALL) { uint64_t t; memcpy(&t, p + 8, 8); if (depth >= 16) die("deferred bind resolution: call depth over 16"); resolve_deferred((const uint8_t *)(uintptr_t)t, depth + 1); }
        else if (h.op == OP_BIND_TEX || h.op == OP_BIND_IMG || h.op == OP_BIND_SAMP) {
            p_bind_handles *b = (p_bind_handles *)(uintptr_t)(p + 8);
            if (b->src && b->n > 0 && b->n <= 32) {
                uint64_t cur[32]; memcpy(cur, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n); handles_to_ids(h.op, b->tpool, b->spool, cur, b->n); g_def_n++;
                if (memcmp(cur, b->h, 8 * (size_t)b->n)) {
                    if (g_def_logged++ < 6) LOG("[nvn] 延迟绑定在提交时读到新句柄：阶段 %d 槽 %d 起，录制时 %#llx → 提交时 %#llx", b->stage, b->first, (unsigned long long)b->h[0], (unsigned long long)cur[0]);
                    g_def_changed++; memcpy(b->h, cur, 8 * (size_t)b->n);
                }
            }
        }
        p += h.size;
    }
}
// 图像（可写贴图）：句柄 = 贴图池号（GetImageHandle 原样返回；游戏不调 RegisterImage，图像与贴图共用登记）
// Image (writable texture): handle = texture pool index (GetImageHandle returns it as-is; the game does not call RegisterImage, images and textures share registration)
NVN(nvnCommandBufferBindImages, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNimageHandle *h) {
    bind_handles(c, OP_BIND_IMG, st, first, n, (const uint64_t *)h); if (n >= 0 && n <= 32) hw_fix(CB(c), 16 + 8 * (size_t)n);
}
NVN(nvnCommandBufferBindSeparateTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateTextureHandle *h) {
    bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h); if (n >= 0 && n <= 32) hw_fix(CB(c), 16 + 8 * (size_t)n);
}
NVN(nvnCommandBufferBindSeparateSamplers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateSamplerHandle *h) {
    bind_handles(c, OP_BIND_SAMP, st, first, n, (const uint64_t *)h); if (n >= 0 && n <= 32) hw_fix(CB(c), 16 + 8 * (size_t)n);
}
NVN(nvnCommandBufferBindSeparateSampler, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateSamplerHandle h) {
    bind_handles(c, OP_BIND_SAMP, st, i, 1, &h.value); hw_fix(CB(c), 24);
}
// TOTK: combined texture handles share the texture slots with separate textures (sampler id in bits 20+).
NVN(nvnCommandBufferBindTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNtextureHandle h) { uint64_t v = h; bind_handles(c, OP_BIND_TEX, st, i, 1, &v); }
NVN(nvnCommandBufferBindTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) { bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h); }
NVN(nvnCommandBufferBindImage, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNimageHandle h) { uint64_t v = h; bind_handles(c, OP_BIND_IMG, st, i, 1, &v); hw_fix(CB(c), 24); }
NVN(nvnCommandBufferBindSeparateTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateTextureHandle h) { bind_handles(c, OP_BIND_TEX, st, i, 1, &h.value); hw_fix(CB(c), 24); }
NVN(nvnCommandBufferBindVertexBuffer, void, NVNcommandBuffer *c, int i, NVNbufferAddress a, size_t n) { NVNbufferRange r = {a, n}; h_nvnCommandBufferBindVertexBuffers(c, i, 1, &r); }
/* CPU-only regression: deferred binds take the handles the game's array holds at submit, also through CallCommands;
   immediate binds keep the recorded value. TOTK_PROBE_NEGATIVE=1 skips the submit-time read and must fail. */
void dm_nvn_probe_deferred_binds(void) {
    typedef void (*bind_fn)(NVNcommandBuffer *, NVNshaderStage, int, int, const NVNtextureHandle *);
    bind_fn deferred = (bind_fn)nvn_lookup("nvnCommandBufferBindTexturesDeferred");
    if (!deferred) die("deferred probe: nvnCommandBufferBindTexturesDeferred not exported");
    NVNcommandBuffer cb, caller;
    uint8_t control[2][256] = {{0}}, commands[4096] = {0};
    pool_t pool = {.mem = commands};
    NVNcommandBuffer *buffers[] = {&cb, &caller};
    for (unsigned i = 0; i < 2; i++) {
        h_nvnCommandBufferInitialize(buffers[i], NULL);
        h_nvnCommandBufferAddControlMemory(buffers[i], control[i], sizeof control[i]);
        h_nvnCommandBufferAddCommandMemory(buffers[i], (NVNmemoryPool *)&pool, 0, sizeof commands);
    }
    NVNtextureHandle table[2] = {0x10200001, 0x10200002}, immediate = 0x10200003;
    h_nvnCommandBufferBeginRecording(&cb);
    deferred(&cb, NVN_SHADER_STAGE_FRAGMENT, 4, 2, table);          /* the game fills the table later */
    h_nvnCommandBufferBindTextures(&cb, NVN_SHADER_STAGE_FRAGMENT, 6, 1, &immediate);
    NVNcommandHandle hd = h_nvnCommandBufferEndRecording(&cb);
    h_nvnCommandBufferBeginRecording(&caller);
    h_nvnCommandBufferCallCommands(&caller, 1, &hd);
    NVNcommandHandle hc = h_nvnCommandBufferEndRecording(&caller);
    table[0] = 0x10200011; table[1] = 0x10200012; immediate = 0x10200013;   /* written after recording, before submit */
    int negative = getenv("TOTK_PROBE_NEGATIVE") && atoi(getenv("TOTK_PROBE_NEGATIVE")) == 1;   /* other numbers are later checks of probe-gfx */
    for (unsigned k = 0; k < 2; k++) {
        const uint8_t *p = (const uint8_t *)(uintptr_t)(k ? hc : hd);
        if (!negative) resolve_deferred(p, 0);
        unsigned seen = 0;
        for (;;) {
            op_hdr h; memcpy(&h, p, sizeof h);
            if (h.op == OP_END) break;
            if (h.op == OP_BIND_TEX) {
                const p_bind_handles *b = (const p_bind_handles *)(p + 8);
                if (b->first == 4 && (b->n != 2 || b->h[0] != 0x10200011 || b->h[1] != 0x10200012)) die("deferred probe: %s bind kept the record-time handles %#llx %#llx", k ? "called" : "direct", (unsigned long long)b->h[0], (unsigned long long)b->h[1]);
                if (b->first == 6 && b->h[0] != 0x10200003) die("deferred probe: immediate bind changed after recording");
                seen++;
            }
            p += h.size;
        }
        if (seen != 2) die("deferred probe: %u binds in the %s recording, expected 2", seen, k ? "called" : "direct");
    }
    h_nvnCommandBufferAddControlMemory(&cb, control[1], sizeof control[1]);
    h_nvnCommandBufferAddControlMemory(&caller, control[0], sizeof control[0]);
    h_nvnCommandBufferFinalize(&cb); h_nvnCommandBufferFinalize(&caller);
}
NVN(nvnCommandBufferSetStencilValueMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, m, 0, 0, 0}; }
NVN(nvnCommandBufferSetStencilMask, void, NVNcommandBuffer *c, NVNface f, int m) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, 0, m, 0, 1}; }
NVN(nvnCommandBufferSetStencilRef, void, NVNcommandBuffer *c, NVNface f, int v) { *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, 0, 0, v, 2}; }
NVN(nvnCommandBufferSetPolygonOffsetClamp, void, NVNcommandBuffer *c, float fa, float un, float cl) {
    *REC(c, OP_POLY_OFFSET, p_poly_offset) = (p_poly_offset){fa, un, cl};
}
NVN(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n) { *REC(c, OP_DRAW, p_draw) = (p_draw){pr, first, n, 0, 1}; }
NVN(nvnCommandBufferDrawArraysInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n, int bi, int inst) {
    if (inst <= 0) return;   // NVN: zero instances draw nothing
    *REC(c, OP_DRAW, p_draw) = (p_draw){pr, first, n, bi, inst};
}
NVN(nvnCommandBufferDrawElements, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib) {
    *REC(c, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){pr, it, n, 0, 0, 1, ib}; hw_fix(CB(c), 48);
}
NVN(nvnCommandBufferDrawElementsBaseVertex, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib, int bv) {
    *REC(c, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){pr, it, n, bv, 0, 1, ib}; hw_fix(CB(c), 44);
}
NVN(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib,
    int bv, int bi, int inst) {
    *REC(c, OP_DRAW_IDX, p_draw_idx) = (p_draw_idx){pr, it, n, bv, bi, inst, ib}; hw_fix(CB(c), 44);   /* assumed: same 6-parameter draw macro as the base-vertex form (unverified) */
}
// Indirect draws (Bayonetta 3; TOTK never calls them): arguments in memory, resolved at replay (nvn_mtl.m draw_resolve).
NVN(nvnCommandBufferDrawArraysIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNbufferAddress args) {
    uint8_t *p = rec(c, OP_DRAW, DRAW_TAIL_AT(p_draw) + sizeof(p_draw_indirect));
    *(p_draw *)p = (p_draw){pr, 0, 0, 0, 0}; *(p_draw_indirect *)(p + DRAW_TAIL_AT(p_draw)) = (p_draw_indirect){DRAW_INDIRECT_MAGIC, args, 0, 0, 0};
}
NVN(nvnCommandBufferDrawElementsIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress args) {
    uint8_t *p = rec(c, OP_DRAW_IDX, DRAW_TAIL_AT(p_draw_idx) + sizeof(p_draw_indirect));
    *(p_draw_idx *)p = (p_draw_idx){pr, it, 0, 0, 0, 0, ib}; *(p_draw_indirect *)(p + DRAW_TAIL_AT(p_draw_idx)) = (p_draw_indirect){DRAW_INDIRECT_MAGIC, args, ib, 0, 0};
    hw_fix(CB(c), 44);
}
/* Multi-draws: one indirect record per possible draw (up to the game's maximum), each skipped at replay when its index is not
   below the count in memory (the Count forms) or never (the plain forms, count = max). */
static void multi_draw(NVNcommandBuffer *c, int indexed, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress args,
                       NVNbufferAddress count, int max, ptrdiff_t stride) {
    if (max < 0 || max > 4096) die("multi-draw of %d draws", max);
    if (!stride) stride = indexed ? 20 : 16;
    for (int i = 0; i < max; i++) {
        size_t at = indexed ? DRAW_TAIL_AT(p_draw_idx) : DRAW_TAIL_AT(p_draw);
        uint8_t *p = rec(c, indexed ? OP_DRAW_IDX : OP_DRAW, at + sizeof(p_draw_indirect));
        if (indexed) *(p_draw_idx *)p = (p_draw_idx){pr, it, 0, 0, 0, 0, ib}; else *(p_draw *)p = (p_draw){pr, 0, 0, 0, 0};
        *(p_draw_indirect *)(p + at) = (p_draw_indirect){DRAW_INDIRECT_MAGIC, args + (uint64_t)(i * stride), ib, count, (uint64_t)i};
    }
}
NVN(nvnCommandBufferMultiDrawArraysIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNbufferAddress args, NVNbufferAddress count, int max, ptrdiff_t stride) {
    multi_draw(c, 0, pr, 0, 0, args, count, max, stride);
}
NVN(nvnCommandBufferMultiDrawElementsIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib,
    NVNbufferAddress args, NVNbufferAddress count, int max, ptrdiff_t stride) {
    multi_draw(c, 1, pr, it, ib, args, count, max, stride);
}
NVN(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *c, int x, int y, int z) {
    // 跳板是尾跳（br），这里的返回地址就是游戏里的调用点
    // The thunk is a tail jump (br), so the return address here is the call site in the game
    *REC(c, OP_DISPATCH, p_dispatch) = (p_dispatch){x, y, z, (uint64_t)__builtin_return_address(0)};
    csh_dispatch(c);
}
// The group counts are read by the GPU from {x, y, z} at addr when it runs (Bayonetta 3; TOTK never calls it).
NVN(nvnCommandBufferDispatchComputeIndirect, void, NVNcommandBuffer *c, NVNbufferAddress addr) {
    *REC(c, OP_DISPATCH, p_dispatch) = (p_dispatch){DISPATCH_INDIRECT, 0, 0, (uint64_t)addr};
    csh_dispatch(c);
}
NVN(nvnCommandBufferBarrier, void, NVNcommandBuffer *c, int bits) { *(int32_t *)rec(c, OP_BARRIER, 8) = bits; }
NVN(nvnCommandBufferSetTexturePool, void, NVNcommandBuffer *c, const NVNtexturePool *p) { REC(c, OP_SET_TEX_POOL, p_set_pool)->pool = (const dpool_t *)p; cb_pools_set(c, 0, ((const dpool_t *)p)->idx); }
NVN(nvnCommandBufferSetSamplerPool, void, NVNcommandBuffer *c, const NVNsamplerPool *p) { REC(c, OP_SET_SAMP_POOL, p_set_pool)->pool = (const dpool_t *)p; cb_pools_set(c, 1, ((const dpool_t *)p)->idx); }

NVN(nvnCommandBufferCopyBufferToTexture, void, NVNcommandBuffer *c, NVNbufferAddress src, const NVNtexture *t, const NVNtextureView *v,
    const NVNcopyRegion *r, int flags) {
    p_copy_b2t *o = REC(c, OP_COPY_B2T, p_copy_b2t); o->src = src; o->tex = tex_pin(t); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
NVN(nvnCommandBufferCopyTextureToBuffer, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r,
    NVNbufferAddress dst, int flags) {
    p_copy_b2t *o = REC(c, OP_COPY_T2B, p_copy_b2t); o->src = dst; o->tex = tex_pin(t); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
// 用给定值清贴图区域（浮点 / 无符号整数两版；mask = 通道写掩码）
// Clear a texture region with the given value (float / unsigned integer versions; mask = channel write mask)
NVN(nvnCommandBufferClearTexture, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const float *rgba, int mask) {
    p_clear_tex *o = REC(c, OP_CLEAR_TEX, p_clear_tex); o->tex = tex_pin(t); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 0; memcpy(o->c.f, rgba, 16);
}
NVN(nvnCommandBufferClearTextureui, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const uint32_t *rgba, int mask) {
    p_clear_tex *o = REC(c, OP_CLEAR_TEX, p_clear_tex); o->tex = tex_pin(t); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 1; memcpy(o->c.u, rgba, 16);
}
NVN(nvnCommandBufferCopyTextureToTexture, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNcopyRegion *sr,
    const NVNtexture *d, const NVNtextureView *dv, const NVNcopyRegion *dr, int flags) {
    p_copy_t2t *o = REC(c, OP_COPY_T2T, p_copy_t2t); o->src = tex_pin(s); o->dst = tex_pin(d);
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv); o->sr = *sr; o->dr = *dr; o->flags = flags;
}
NVN(nvnCommandBufferSetCopyRowStride, void, NVNcommandBuffer *c, ptrdiff_t st) { REC(c, OP_COPY_STRIDE, p_copy_stride)->stride = st; }
NVN(nvnCommandBufferSetBlendColor, void, NVNcommandBuffer *c, const float *v) { memcpy(REC(c, OP_BLEND_COLOR, p_blend_color)->rgba, v, 16); }

// ---------- 状态对象：就地打包（nvn_obj.h） ----------
// ---------- State objects: packed in place (nvn_obj.h) ----------
#define ST(T, x) ((T *)(x))
NVN(nvnBlendStateSetDefaults, void, NVNblendState *s) { *ST(blend_st, s) = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD}; }
NVN(nvnBlendStateSetBlendTarget, void, NVNblendState *s, int t) { ST(blend_st, s)->target = t; }
NVN(nvnBlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
NVN(nvnBlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) { ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea; }
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
    if (off < 0 || off >= (1 << 19)) LOG("【红】顶点属性偏移 %td 超出打包范围", off);
    ST(va_st, s)->fmt = f; ST(va_st, s)->offset = (uint32_t)off;
}
NVN(nvnVertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { ST(va_st, s)->stream = i; }
NVN(nvnVertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { *ST(vs_st, s) = (vs_st){0, 0}; }
NVN(nvnVertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { ST(vs_st, s)->stride = (uint32_t)st; }
NVN(nvnVertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { ST(vs_st, s)->divisor = d; }

static _Atomic uint64_t g_sub_wall_ns, g_sub_cpu_ns, g_sub_instr;   // 提交（不含出图）的墙钟 / 本线程 CPU 时间 / 本线程退休指令数（逐帧取走） | Wall clock / this thread's CPU time / this thread's retired instruction count for submit (excluding present) (taken away per frame)
static _Atomic uint64_t g_cum_sub_instr, g_cum_sub_cpu_ns;   // 同上累计（每 300 帧汇总「翻译」行取差，真机经日志口读：每次绘制指令数 / 每帧提交 CPU） | Cumulative versions of the above (the 「翻译」(translation) summary row every 300 frames takes the difference; on a real device read through the log port: instructions per draw / submit CPU per frame)
extern int thread_selfcounts(int type, void *buf, size_t nbytes);   // 本线程累计 {指令, 周期}（libSystem 私有接口，Mac / iOS 都有） | This thread's cumulative {instructions, cycles} (libSystem private interface, present on both Mac and iOS)
static uint64_t thr_instr(void) { uint64_t c[2] = {0, 0}; thread_selfcounts(1, c, sizeof c); return c[0]; }
// 提交路径的补充读数（真机 Metal 4 指令数 −18% 而提交 CPU 时间 +49%，要分清是内核态、执行效率还是被降频）：
// 本线程周期数（同一接口的第二项）、内核态 CPU 时间（THREAD_BASIC_INFO）、墙钟
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
static _Atomic uint64_t g_nvn_ns;   // 本帧在提交 / 出图里花的宿主时间（回放编码 + 出图），计时埋点用 | Host time spent in submit / present this frame (replay encoding + present), used by the timing instrumentation

// ---------- 生命周期：dm_gfx_suspend（iOS 进后台前调用；等效 Switch 挂起） ----------
// on=1：之后的提交 / 出图阻塞到 on=0；返回前等正在进行的提交 / 出图调用退出、并等所有在途 GPU 工作完成（iOS 要求后台无 GPU 工作）。
// ---------- Lifecycle: dm_gfx_suspend (called before iOS goes to the background; equivalent to Switch suspend) ----------
// on=1: subsequent submits / presents block until on=0; before returning, wait for in-progress submit / present calls to exit and for all in-flight GPU work to complete (iOS requires no GPU work in the background).
static pthread_mutex_t g_susp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_susp_cv = PTHREAD_COND_INITIALIZER;
static int g_susp, g_inflight_api;
static void gfx_enter(void) { pthread_mutex_lock(&g_susp_mu); while (g_susp) pthread_cond_wait(&g_susp_cv, &g_susp_mu); g_inflight_api++; pthread_mutex_unlock(&g_susp_mu); }
static void gfx_leave(void) { pthread_mutex_lock(&g_susp_mu); g_inflight_api--; pthread_cond_broadcast(&g_susp_cv); pthread_mutex_unlock(&g_susp_mu); }
void dm_gfx_suspend(int on) {
    pthread_mutex_lock(&g_susp_mu);
    g_susp = on != 0;
    if (on) while (g_inflight_api) pthread_cond_wait(&g_susp_cv, &g_susp_mu);
    else pthread_cond_broadcast(&g_susp_cv);
    pthread_mutex_unlock(&g_susp_mu);
    if (on) { x_drain(); mtl_drain(); LOG("[nvn] 图形挂起：在途 GPU 工作已清空"); } else LOG("[nvn] 图形恢复");
}
_Atomic int dm_submit_busy;   // TOTK diagnostic: submissions being replayed (runtime/gfx_bridge.c write watch)
// ---------- TOTK: translation thread (TOTK_ASYNC_REPLAY=1) ----------
// Submits and presents are queued in order to one translation thread; the game thread records them and returns, so game
// logic of the next frame overlaps the replay + Metal encoding of this one (the design of D2R Mobile b94670c, adapted).
//  - Fences: FenceSync records the number of submits enqueued (bit 2 of `fenced` marks such a token). SyncWait first waits
//    until the translation thread has run that submit, then for the Metal fence that submit ended with. Waiting on a fence
//    therefore means "translated and GPU done", so when the game reuses command / control memory or rewrites objects after
//    a fence, the translation thread has long finished reading them.
//  - Presents are jobs too (after the submit that drew the frame); the acquire of a swapchain image first waits for the
//    job of that image's last present, then for its present ticket as before. The replay frame number travels in the job.
//  - Operations that destroy or read Metal state (pool / texture / program finalize, ReadTexels / WriteTexels, QueueFinish,
//    suspend) wait until the queue is empty first (x_drain), so they never overtake a queued replay that still uses them.
// TOTK_ASYNC_REPLAY=0 (default for now) runs everything on the calling thread as before.
enum { XJ_SUBMIT = 1, XJ_PRESENT, XJ_TEXREL };
typedef struct { int kind, n; uint64_t *h; void *ucap; win_t *win; int idx; double period; int crop[4]; uint64_t frame, sub; tex_t tex; } xjob;   // tex: copy of a finalized texture (XJ_TEXREL); ucap: uniforms captured at the submit call
#define XQ 64
#define XF 4096
static xjob g_xq[XQ];
static uint64_t g_xq_in, g_xq_done, g_xsub_in, g_xsub_done, g_xsub_fence[XF];
static pthread_mutex_t g_xq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_xq_cv = PTHREAD_COND_INITIALIZER;
static win_t *g_xp_win; static uint64_t g_xp_job[4];   // job number of each swapchain image's last present (TOTK has one window)
static _Atomic uint64_t g_x_drains, g_x_drain_ns;
static int async_replay(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("TOTK_ASYNC_REPLAY"); v = e && atoi(e) > 0; if (v) LOG("[nvn] 翻译线程：提交与出图排队交给独立线程回放（TOTK_ASYNC_REPLAY=0 关）"); }
    return v;
}
static void submit_exec(int n, const uint64_t *h, void *ucap);
static void present_exec(win_t *o, int idx, double period, const int crop[4], uint64_t frame);
static void *x_thread(void *unused) {
    (void)unused; pthread_setname_np("totk-translate");
    for (;;) {
        pthread_mutex_lock(&g_xq_mu);
        while (g_xq_done == g_xq_in) pthread_cond_wait(&g_xq_cv, &g_xq_mu);
        xjob j = g_xq[g_xq_done % XQ];
        pthread_mutex_unlock(&g_xq_mu);
        if (j.kind == XJ_SUBMIT) { submit_exec(j.n, j.h, j.ucap); free(j.h); }
        else if (j.kind == XJ_TEXREL) { void live_del_x(const tex_t *o); live_del_x(&j.tex); mtl_tex_release(&j.tex); }
        else {
            // Real-time like the game's render thread that used to replay (set at acquire): otherwise the game's ~50
            // worker threads preempt the replay (measured: replay 6.6 -> 13 ms, 10 fps). Half the period is declared.
            static uint64_t rt; uint64_t per = (uint64_t)(j.period * 1e9);
            if (per && rt != per) { rt = per; void set_time_constraint_comp_x(uint64_t, uint64_t); set_time_constraint_comp_x(per, per / 2); }
            present_exec(j.win, j.idx, j.period, j.crop, j.frame);
        }
        pthread_mutex_lock(&g_xq_mu);
        if (j.kind == XJ_SUBMIT) { g_xsub_fence[j.sub % XF] = mtl_last_fence(); g_xsub_done = j.sub; }
        g_xq_done++;
        pthread_cond_broadcast(&g_xq_cv);
        pthread_mutex_unlock(&g_xq_mu);
    }
    return NULL;
}
static uint64_t x_push(xjob *j) {   // returns the job number (1-based)
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    void x_start(void);
    pthread_once(&once, x_start);
    pthread_mutex_lock(&g_xq_mu);
    while (g_xq_in - g_xq_done >= XQ) pthread_cond_wait(&g_xq_cv, &g_xq_mu);
    if (j->kind == XJ_SUBMIT) j->sub = ++g_xsub_in;
    g_xq[g_xq_in % XQ] = *j;
    uint64_t no = ++g_xq_in;
    pthread_cond_broadcast(&g_xq_cv);
    pthread_mutex_unlock(&g_xq_mu);
    return no;
}
void x_texrel(const tex_t *o) { xjob j = {.kind = XJ_TEXREL}; j.tex = *o; x_push(&j); }
void x_start(void) {
    pthread_t t; pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8u << 20);
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_USER_INTERACTIVE, 0);
    if (pthread_create(&t, &a, x_thread, NULL)) die("translation thread");
    pthread_attr_destroy(&a); pthread_detach(t);
}
static void x_wait_job(uint64_t job) {
    if (!job) return;
    pthread_mutex_lock(&g_xq_mu); while (g_xq_done < job) pthread_cond_wait(&g_xq_cv, &g_xq_mu); pthread_mutex_unlock(&g_xq_mu);
}
static void x_drain(void) {   // everything queued so far has run (no-op when synchronous or idle)
    if (!async_replay()) return;
    NVN_BLOCKING();   // a guest thread gives its virtual core back while it waits (TOTK has three)
    pthread_mutex_lock(&g_xq_mu); uint64_t want = g_xq_in, have = g_xq_done; pthread_mutex_unlock(&g_xq_mu);
    if (have >= want) return;
    uint64_t t0 = now_ns(); x_wait_job(want);
    atomic_fetch_add(&g_x_drains, 1); atomic_fetch_add(&g_x_drain_ns, now_ns() - t0);
}
void nvn_x_drain(void) { x_drain(); }   // for the graphics layer (pool invalidate write-back)
// Fence token for FenceSync: submits enqueued so far (async) or the Metal fence (synchronous).
static uint64_t fence_now(int *token) {
    if (!async_replay()) { *token = 0; return mtl_last_fence(); }
    pthread_mutex_lock(&g_xq_mu); uint64_t k = g_xsub_in; pthread_mutex_unlock(&g_xq_mu); *token = 1; return k;
}
// Waits for a token: the submit has been translated, then the Metal fence it ended with. 1 = reached.
static int fence_wait(uint64_t k, uint64_t timeout_ns) {
    uint64_t t0 = now_ns();
    pthread_mutex_lock(&g_xq_mu);
    if (g_xsub_done < k) {
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
        uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
        dl.tv_sec += ns / 1000000000ull; dl.tv_nsec = ns % 1000000000ull;
        while (g_xsub_done < k) if (pthread_cond_timedwait(&g_xq_cv, &g_xq_mu, &dl)) break;
    }
    int ok = g_xsub_done >= k;
    uint64_t f = !k ? 0 : g_xsub_done - k < XF ? g_xsub_fence[k % XF] : mtl_last_fence();   // older entries were overwritten: a later fence is conservative
    pthread_mutex_unlock(&g_xq_mu);
    if (!ok) return 0;
    uint64_t used = now_ns() - t0;
    return mtl_wait(f, timeout_ns > used ? timeout_ns - used : 0);
}

NVN(nvnQueueSubmitCommands, void, NVNqueue *q, int n, const NVNcommandHandle *h) {
    NVN_BLOCKING();
#ifdef GAME_BOTW
    NVNcommandHandle mapped[64]; NVNcommandHandle *heap = n > 64 ? malloc(sizeof *heap * (size_t)n) : NULL;
    NVNcommandHandle *hv = heap ? heap : mapped; if (n > 64 && !heap) die("submit handle table");
    for (int i = 0; i < n; i++) hv[i] = botw_handle_copy(h[i]);
    h = hv;
#endif
    totk_tl(TL_SUB_IN, (uint64_t)n, 0);
    { uint64_t now = __atomic_add_fetch(&g_submits, 1, __ATOMIC_RELAXED); for (int i = 0; i < n; i++) copy_touch(h[i], now); }
    if (totk_life_on()) { int k = 0; for (int i = 0; i < n; i++) k += totk_life_handle_has(h[i]); totk_life_log("submit n=%d followed=%d", n, k); }
    ((queue_t *)q)->submits++;
    /* TOTK: what the console reads at the submit call is read here, on the game thread, before the submission may be handed
       to the translation thread: the handles of deferred binds (the game fills its handle tables for the next frame as soon
       as this call returns) and, with the translation thread, the bound uniform bytes (other game threads rebuild the
       constant pools right after the submit; see mtl_sub_capture_ubos). */
    if (atomic_load(&g_def_on) < 0) { const char *e = getenv("TOTK_DEFERRED_BINDS"); atomic_store(&g_def_on, !(e && !strcmp(e, "0"))); LOG("[nvn] 延迟绑定：%s", atomic_load(&g_def_on) ? "提交时读句柄" : "录制时读句柄（TOTK_DEFERRED_BINDS=0，旧行为）"); }
    if (atomic_load(&g_def_on)) {
        uint64_t r0 = now_ns();
        for (int i = 0; i < n; i++) resolve_deferred((const uint8_t *)h[i], 0);
        g_def_ns += now_ns() - r0;
        if (((queue_t *)q)->submits % 600 == 0) LOG("[nvn] 延迟绑定 至提交 %llu：提交时读句柄 %llu 次，其中与录制时不同 %llu 次；每次提交读句柄平均 %.3f ms", (unsigned long long)((queue_t *)q)->submits, (unsigned long long)g_def_n, (unsigned long long)g_def_changed, g_def_ns / 1e6 / (double)((queue_t *)q)->submits);
    }
    if (async_replay()) {
        xjob j = {.kind = XJ_SUBMIT, .n = n, .h = malloc(sizeof(uint64_t) * (n > 0 ? n : 1))};
        if (!j.h) die("submit job");
        memcpy(j.h, h, sizeof(uint64_t) * (size_t)n);
        j.ucap = mtl_sub_capture_ubos((const uint64_t *)h, n);
        x_push(&j);
        totk_tl(TL_SUB_OUT, 1, 0);
#ifdef GAME_BOTW
        free(heap);
#endif
        return;
    }
    submit_exec(n, (const uint64_t *)h, NULL);
    for (int i = 0; i < n; i++) copy_done(h[i]);
    /* Measurement only, DM_EXP_SUBMIT_WAIT=1: the submit returns once the GPU has finished it, so the game cannot write
       memory this submission still reads (slow; tells a CPU-ahead race from a translation fault). */
    { static int sw = -1; if (sw < 0) { const char *e = getenv("DM_EXP_SUBMIT_WAIT"); sw = e && atoi(e) > 0; if (sw) LOG("【实验】DM_EXP_SUBMIT_WAIT=1：每次提交等 GPU 做完"); }
      if (sw) mtl_wait(mtl_last_fence(), 2000000000ull); }
    totk_tl(TL_SUB_OUT, 0, 0);
#ifdef GAME_BOTW
    free(heap);
#endif
}
static void submit_exec(int n, const uint64_t *h, void *ucap) {
    gfx_enter();
    totk_tl(TL_REP_BEGIN, 0, 0);
    pin_step(__atomic_load_n(&g_submits, __ATOMIC_RELAXED));   /* TOTK: free texture copies no recording refers to any more */
    if (cmdlog_env()) for (int i = 0; i < n; i++) {
        unsigned ops = 0, draws = 0, calls = 0; const uint8_t *p = (const uint8_t *)h[i];
        for (;;) { op_hdr hh; memcpy(&hh, p, sizeof hh); ops++; if (hh.op == OP_DRAW || hh.op == OP_DRAW_IDX) draws++; if (hh.op == OP_CALL) calls++; if (hh.op == OP_END || hh.size < 8 || ops > 1000000) break; p += hh.size; }
        LOG("[cmd] Submit %d/%d handle=%#llx ops=%u draws=%u calls=%u", i, n, (unsigned long long)h[i], ops, draws, calls); }
    if (getenv("TOTK_CHAIN_LOG")) { static int k; if (k++ < 20) guest_chain("Submit"); }
    uint64_t t0 = now_ns(), c0 = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID), i0, y0, s0 = thr_sys_ns(); thr_counts(&i0, &y0);
    /* TOTK diagnostic (TOTK_WATCH_ADDR): submissions in progress, and their start / end times, to line up guest writes with replay */
    atomic_fetch_add(&dm_submit_busy, 1);
    { static int wl = -1; if (wl < 0) wl = getenv("TOTK_WATCH_ADDR") != NULL; if (wl) LOG("[sub] F%llu start t_us=%llu", (unsigned long long)g_frame_pub, (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000)); }
    mtl_sub_begin();
    mtl_sub_snapshot_ubos_cap((const uint64_t *)h, n, ucap);   // TOTK: uniforms as of the submit call (ucap: bytes captured there)
    mtl_sub_index((const uint64_t *)h, n);
    for (int i = 0; i < n; i++) { mtl_exec_rest((const uint64_t *)h + i + 1, n - i - 1); mtl_exec((const uint8_t *)h[i]); }
    mtl_exec_rest(NULL, 0);
    mtl_sub_commit();
    atomic_fetch_sub(&dm_submit_busy, 1);
    { static int wl = -1; if (wl < 0) wl = getenv("TOTK_WATCH_ADDR") != NULL; if (wl) LOG("[sub] F%llu end t_us=%llu", (unsigned long long)g_frame_pub, (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000)); }
    uint64_t dw = now_ns() - t0;
    atomic_fetch_add(&g_nvn_ns, dw); atomic_fetch_add(&g_sub_wall_ns, dw);
    uint64_t i1, y1; thr_counts(&i1, &y1); uint64_t dc = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - c0, di = i1 - i0;
    atomic_fetch_add(&g_cum_sub_cyc, y1 - y0); atomic_fetch_add(&g_cum_sub_sys_ns, thr_sys_ns() - s0); atomic_fetch_add(&g_cum_sub_wall_ns, dw);
    atomic_fetch_add(&g_sub_cpu_ns, dc); atomic_fetch_add(&g_cum_sub_cpu_ns, dc);
    atomic_fetch_add(&g_sub_instr, di); atomic_fetch_add(&g_cum_sub_instr, di);
    totk_tl(TL_REP_END, 0, 0);
    gfx_leave();
}

// 只影响 GPU 状态 / 只在 GPU 上执行的命令：空设备只计数（跳板已计）
// Commands that only affect GPU state / only execute on the GPU: the null device only counts them (the thunk has already counted)
static void nvn_nop(void) {}
// TOTK: entries that would silently drop GPU work (indirect draws/dispatch, downsample, depth bounds test) were removed
// so a real call stops at the unimplemented stub. WaitSync/WaitEvent rely on in-order replay on the single Metal queue.
static const char *const g_nops[] = {
    "nvnCommandBufferSetPatchSize",
    "nvnCommandBufferWaitSync",
    "nvnCommandBufferSetShaderScratchMemory",
    "nvnCommandBufferWaitEvent",
    "nvnWindowFinalize",
    // Bayonetta 3: Maxwell colour reduction is a framebuffer bandwidth optimisation that does not change what is drawn.
    "nvnCommandBufferSetColorReductionEnable",
    "nvnCommandBufferSetColorReductionThresholds",
};
__attribute__((constructor)) static void nvn_nops_register(void) {
    // 空实现在槽表初始化后覆盖未实现桩；名字写错（不在 main 名字表里）启动即报红中止
    // Empty implementations overwrite the unimplemented stubs after the slot table is initialized; a misspelled name (not in main's name table) reports a failure and aborts at startup
    nvn_init_slots();
    for (unsigned k = 0; k < sizeof g_nops / sizeof *g_nops; k++) {
        unsigned i = 0;
        while (i < NVN_NFUNC && strcmp(nvn_names[i], g_nops[k])) i++;
        if (i == NVN_NFUNC) die("NVN 空实现名 %s 不在 main 名字表里", g_nops[k]);
        if (nvn_slot[i].fn != (void *)nvn_unimpl_entry) die("NVN %s 既有实现又列为空实现", g_nops[k]);
        nvn_slot[i].fn = (void *)nvn_nop;
    }
    unsigned nnop = sizeof g_nops / sizeof *g_nops;
    // DM_NVN_PROF=1：已实现的函数（未实现桩与出图除外：出图含节拍等待，单独计）套计时包装，用于拆「翻译层录制期」耗时
    // DM_NVN_PROF=1: implemented functions (except unimplemented stubs and present: present includes the display-link wait and is counted separately) get a timing wrapper, used to break down the time spent in the "translation layer recording phase"
    if (getenv("DM_NVN_PROF"))
        for (unsigned i = 0; i < NVN_NFUNC; i++)
            if (nvn_slot[i].fn != (void *)nvn_unimpl_entry && strcmp(nvn_names[i], "nvnQueuePresentTexture")) { nvn_real[i] = nvn_slot[i].fn; nvn_slot[i].fn = (void *)nvn_prof_entry; }
    LOG("[nvn] 空设备：名字 %u，已实现 %u（语义 %u + 空实现 %u），其余 %u 个接报红桩",
        NVN_NFUNC, g_nsem + nnop, g_nsem, nnop, NVN_NFUNC - g_nsem - nnop);
}

// ---------- 窗口 / 出图 ----------
// ---------- Window / present ----------
#define WB(b) ((win_builder_t *)(b))
NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder *b) { NVNdevice *dev = WB(b)->dev; memset(b, 0, sizeof(win_builder_t)); WB(b)->dev = dev; WB(b)->interval = 1; }
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder *b, NVNdevice *d) { WB(b)->dev = d; }
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder *b, NVNnativeWindow w) { WB(b)->native = w; }
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder *b, int n, NVNtexture *const *t) {
    if (n < 0 || n > 4) { LOG("【红】WindowBuilderSetTextures 数量 %d", n); n = n < 0 ? 0 : 4; }
    WB(b)->ntex = n; for (int i = 0; i < n; i++) WB(b)->tex[i] = t[i];
}
// TOTK sets two active images. Preserve the requested acquisition limit.
NVN_NOPROTO(nvnWindowBuilderSetNumActiveTextures, void, NVNwindowBuilder *b, int n) { if(n<2||n>4)die("unsupported active texture count %d",n);WB(b)->active=n; }
NVN_NOPROTO(nvnWindowBuilderGetNumActiveTextures, int, const NVNwindowBuilder *b) { const win_builder_t *v=(const win_builder_t *)b;return v->active?v->active:v->ntex; }
NVN_NOPROTO(nvnWindowSetNumActiveTextures, void, NVNwindow *w, int n) { win_t *v=(win_t *)w;if(n<2||n>v->ntex)die("invalid active window texture count %d",n);v->active=n; }
NVN_NOPROTO(nvnWindowGetNumActiveTextures, int, const NVNwindow *w) { const win_t *v=(const win_t *)w;return v->active?v->active:v->ntex; }
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder *b, int v) { WB(b)->interval = v; }
NVN(nvnWindowInitialize, NVNboolean, NVNwindow *w, const NVNwindowBuilder *b) {
    const win_builder_t *s = (const win_builder_t *)b; win_t *o = (win_t *)w;
    memset(o, 0, sizeof *o); o->magic = MAGIC('W', 'I', 'N', 'D'); o->native = s->native; o->ntex = s->ntex; o->interval = s->interval; o->active=s->active?s->active:s->ntex;
    if(o->ntex<2||o->ntex>4||o->active<2||o->active>o->ntex)return 0;
    memcpy(o->tex, s->tex, sizeof o->tex);
    for (int i = 0; i < o->ntex; i++) if (o->tex[i]) mtl_xv_window_tex(CTX(o->tex[i])->serial);
    LOG("[nvn] WindowInitialize 交换链 %d 张 间隔 %d 首张 %dx%d 格式 %#x", o->ntex, o->interval,
        o->ntex ? (int)CTX(o->tex[0])->d.width : 0, o->ntex ? (int)CTX(o->tex[0])->d.height : 0, o->ntex ? CTX(o->tex[0])->d.format : 0);
    return 1;
}
/* TOTK: the game picks its present interval (main+0x8f6bec) from its own frame work time: system object +0x118 = ticks
   (19.2 MHz) from the end of the previous frame function to after its update / draw calls (main+0x8f6a54), present not
   included; interval = clamp(trunc(work * 60 + bias) + 1, min, max), raised after +0xc frames over, lowered only after a
   cooldown. The controller calls nvnWindowSetPresentInterval with the system object saved in its frame (caller x19 at
   x29 + 0x38), so it is picked up here, and every frame's +0x118 is reported with the controller state (game_work_tick). */
static int dcmp(const void *a, const void *b);
static int vm_read8(uint64_t a, uint64_t *out) { vm_size_t got = 0; return a && !(a & 7) && vm_read_overwrite(mach_task_self(), (vm_address_t)a, 8, (vm_address_t)out, &got) == KERN_SUCCESS && got == 8; }
extern uintptr_t runtime_frame_controller_return_pc(void);
static void find_game_sys(void) {
    uintptr_t controller_return = runtime_frame_controller_return_pc();
    uint64_t fp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
    for (int i = 0; i < 24 && fp && !(fp & 7); i++) {
        uint64_t f0, f1; if (!vm_read8(fp, &f0) || !vm_read8(fp + 8, &f1)) return;
        if (f1 == controller_return) { uint64_t sys = 0; if (vm_read8(f0 + 0x38, &sys) && sys) { g_game_sys = sys; LOG("[nvn] 游戏系统对象 %#llx（出图间隔控制器的帧里取得）", (unsigned long long)sys); } return; }
        if (f0 == fp) return; fp = f0;
    }
}
static void game_work_tick(uint64_t frame) {
    if (!g_game_sys) return;
    static double win[300]; static int n; static uint64_t over33;
    int64_t t = *(volatile int64_t *)(uintptr_t)(g_game_sys + 0x118); double ms = t / 19200.0;
    if (ms >= 0 && ms < 10000 && n < 300) win[n++] = ms;   // loading frames leave the frame start unset (absolute tick) if (ms > 33.333) over33++;
    if (frame % 300) return;
    if (n < 30) { LOG("[nvn] 游戏帧工作时间（+0x118）至帧 %llu：有效值不足（%d 个），原始 %lld", (unsigned long long)frame, n, (long long)t); n = 0; return; }
    double v[300]; memcpy(v, win, sizeof(double) * n); qsort(v, n, sizeof *v, dcmp);
    const int32_t *c = (const int32_t *)(uintptr_t)(g_game_sys + 0x248);
    LOG("[nvn] 游戏帧工作时间（+0x118）至帧 %llu：p50 %.2f / p90 %.2f / 最大 %.2f ms（%d 帧，超 33.3 ms 累计 %llu 帧）；控制器 区间 %d..%d 偏置 %.3f 升档需 %d 帧 冷却 %d 当前 %d 降档目标 %d 升计数 %d 冷却计数 %d",
        (unsigned long long)frame, v[n / 2], v[n * 9 / 10], v[n - 1], n, (unsigned long long)over33, c[0], c[1], *(const float *)&c[2], c[3], c[4], c[5], c[6], c[7], c[8]);
    { const uint8_t *b = (const uint8_t *)(uintptr_t)g_game_sys;   // TOTK: frame-end mode (main+0xa91f98): +0x223 1 = wait for the last frame's GPU fence
      LOG("[nvn] 帧结束方式 至帧 %llu：+0x222=%#x +0x223=%u（1 每帧等上一帧 GPU 做完）+0x224=%u +0x225=%#x +0xa6=%u +0x50=%d +0x88=%u", (unsigned long long)frame,
          b[0x222], b[0x223], b[0x224], b[0x225], b[0xa6], *(const int32_t *)(b + 0x50), *(const uint32_t *)(b + 0x88)); }
    n = 0;
}
/* TOTK diagnostic "flag framewait 0|1": the game's frame-end mode request (system object +0x224: 2 = no per-frame wait for the
   GPU, 1 = wait), applied by the game at the start of its next frame (main+0x8f69e0). Measurement only. */
void nvn_set_framewait(int on) {
    if (!g_game_sys) { LOG("【红】[flag] framewait：游戏系统对象还没找到"); return; }
    *(volatile uint8_t *)(uintptr_t)(g_game_sys + 0x224) = on ? 1 : 2;
    LOG("[flag] framewait = %d（写 +0x224 = %d）", on, on ? 1 : 2);
}
NVN(nvnWindowSetPresentInterval, void, NVNwindow *w, int v) { if (((win_t *)w)->interval != v) LOG("[nvn] 游戏设出图间隔 %d → %d", ((win_t *)w)->interval, v); ((win_t *)w)->interval = v; if (!g_game_sys) find_game_sys(); }
NVN(nvnWindowSetCrop, void, NVNwindow *w, int x, int y, int ww, int hh) {
    win_t *o = (win_t *)w; int v[4] = {x, y, ww, hh};
    if (memcmp(o->crop, v, sizeof v)) { LOG("[nvn] WindowCrop %d,%d %dx%d", x, y, ww, hh); if (getenv("TOTK_SIZE_CHAIN")) guest_chain("window crop"); }
    memcpy(o->crop, v, sizeof v);
}
static void trow(uint64_t f, const char *name, double v);
static void set_time_constraint(uint64_t period_ns);
static void rt_comp_tick(uint64_t period_ns);
static int g_uncapped;   // TOTK_UNCAPPED configured, see present_cfg; suspended while frame interpolation paces the game (uncapped_now)
static int g_paced_hz;   // the display-following rate, used instead of UNCAPPED_HZ while interpolation paces the game
#include "totk_mfx_mode.h"   /* totk_mfx.m: frame interpolation mode of this game frame (TOTK_MFX, key 2) */
/* TOTK: frame interpolation at 30 fps or at half the refresh rate paces the game itself, so it suspends the uncapped mode;
   the uncapped interpolation mode keeps it. Key 2 cycles the modes at run time (2026-10-16 night). */
static int uncapped_now(void) { int m = mfx_enabled(); return g_uncapped && (m == MFX_OFF || m == MFX_MAX); }
/* 30+30 on a display whose rate is not a multiple of 30 (the user's 100 Hz): the game is paced by our own clock at exactly
   30 fps instead of by the display link (3 refreshes would be 33 fps); the display link still shows each picture at a refresh. */
static int mfx_own_clock(void) { return mfx_enabled() == MFX_30 && (g_paced_hz <= 0 || g_paced_hz % 30); }
/* Opt-in until live scene/clock validation (Breath of the Wild: on by default, UNCAPPED_INTERVAL_FROM_ENV in nvn_obj.h).
   Only applies to the existing Mac real-time uncapped path; original timing, MetalFX and deterministic tests
   retain the guest interval. GPU/texture lifetime waits are unchanged. */
static _Atomic int g_uncapped_interval = -1;
void nvn_set_uncapped_interval(int on) {
    atomic_store(&g_uncapped_interval, on != 0);
    LOG("[flag] uncapped_interval = %d", on != 0);
}
static int uncapped_interval_on(void) {
    int v = atomic_load(&g_uncapped_interval);
    if (v < 0) { const char *e = getenv("TOTK_UNCAPPED_INTERVAL"); v = UNCAPPED_INTERVAL_FROM_ENV(e); atomic_store(&g_uncapped_interval, v); }
    return v;
}
static double g_rt_comp_ms;   // 当前声明的实时计算量（ms；0 = 未声明） | Currently declared real-time computation amount (ms; 0 = not declared)
static double g_rt_period_ms;
static void sleep_until_ns(uint64_t dl);
static uint64_t present_period(const win_t *o);
static unsigned present_ticks(const win_t *o);
extern void runtime_frame_step_update(uint64_t now_ns) __attribute__((weak_import));
extern void movie_host_tick(void) __attribute__((weak_import));   /* TOTK runtime/movie_host.c: pre-rendered cutscenes at their own frame rate */
__attribute__((weak)) void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = 0; *ns = 0; }   // 宿主 hle_thread.c 提供真值；图形单测里为 0 | The host's hle_thread.c provides the ground truth; 0 in graphics unit tests
__attribute__((weak)) uint32_t dm_guest_work_qos(unsigned *count) { if (count) *count = 0; return QOS_CLASS_UNSPECIFIED; }
extern uint32_t mtl_present_qos(void);
uint64_t g_frame_pub;
static uint64_t g_acq_wait_ns, g_pq_wait_ns;   // 取图里节拍等待 / 等出图队列归还的累计，出图时按帧记（一帧内可有多次取图） | Accumulated display-link wait / wait for the present queue to return inside acquire, recorded per frame at present (there can be several acquires within one frame)
uint64_t dm_nvn_frame_count(void) { return g_frame; }
extern uint64_t g_t0_ns;   // 宿主进程起点（main.c；UIMSG 时间戳同基） | Host process start point (main.c; same base as the UIMSG timestamps)
NVN(nvnWindowAcquireTexture, NVNwindowAcquireTextureResult, NVNwindow *w, NVNsync *s, int *idx) {
    NVN_BLOCKING();
    totk_tl(TL_ACQ_IN, 0, 0);
    win_t *o = (win_t *)w;
    *idx = o->active ? o->next++ % o->active : 0;
    // 垂直同步阻塞在这里（真机 NVN：交换链里的贴图都在等显示时，等下一张被释放），不在出图里——游戏常持锁调出图
    // Vsync blocks here (real-device NVN: when all textures in the swapchain are waiting for display, wait for the next one to be released), not in present -- the game often calls present while holding a lock
    uint64_t at = *idx < 4 ? o->avail[*idx] : 0, now = dm_detclock_enabled() ? guest_ns() : now_ns(), wait = at > now ? at - now : 0;
    if (!dm_detclock_enabled()) {   // 取图线程登记为时间约束线程（不再只在「来早了」时才登记：热态一直落后时也要声明） | Register the acquire thread as a time-constraint thread (no longer registered only when it "arrived early": it must also declare itself when it is constantly behind in the hot state)
        static uint64_t rt_period; uint64_t period = present_period(o);
        if (period && rt_period != period) { rt_period = period; set_time_constraint(period); }
        rt_comp_tick(period);
        if (mtl_link_active() && !uncapped_now() && !mfx_own_clock()) { if (!mtl_link_wait_tick(period ? 2 * period : 33333333ull, present_ticks(o)) && wait) sleep_until_ns(at); }   // DM_PRESENT_LINK：节拍完全跟显示节拍（跑得快就等下一拍，落后立刻走）；自己的 60 Hz 时钟只作等不到节拍时的兜底 | DM_PRESENT_LINK: pacing fully follows the display link (wait for the next display-link tick when running fast, go immediately when behind); our own 60 Hz clock is only the fallback when no display-link tick arrives
        else if (wait) sleep_until_ns(at);
    } else {   // DM_DETCLOCK_PACE=1（画面闸门用）：客体时钟按帧走的同时，每帧墙钟不短于一个步长——帧数与游戏内时间确定，后台加载线程每帧至少有一个步长的墙钟可用； | DM_DETCLOCK_PACE=1 (used by the screenshot gate): while the guest clock advances per frame, each frame's wall clock is no shorter than one step -- frame count and in-game time are deterministic, and the background loading thread has at least one step of wall clock available per frame;
               // 机器满载时帧只会更慢、后台得到的墙钟只会更多，所以「消息后第 N 帧」截到的画面与负载无关
               // When the machine is fully loaded frames only get slower and the background only gets more wall clock, so the picture captured at "the Nth frame after the message" is independent of load
        static int pace = -1; static uint64_t step, last;
        if (pace < 0) { const char *e = getenv("DM_DETCLOCK_PACE"), *d = getenv("DM_DETCLOCK"); step = d ? strtoull(d, 0, 0) : 0; pace = e && atoi(e) && step; }
        if (pace) { if (last && now_ns() < last + step) sleep_until_ns(last + step); last = now_ns(); }
    }
    g_acq_wait_ns += wait; trow(g_frame, "@late_ms", wait ? (now_ns() - at) / 1e6 : 0);
    // 该张上一次出图还没被出图线程处理完（显示跟不上、落后一整轮交换链）：等它归还，再交给游戏画
    // The previous present of this texture has not yet been processed by the present thread (display cannot keep up, a whole swapchain round behind): wait for it to be returned, then hand it to the game to draw
    if (async_replay() && *idx < 4 && g_xp_win == o) x_wait_job(g_xp_job[*idx]);   // TOTK: that image's last present has run (its ticket is set)
    uint64_t tk = *idx < 4 ? o->ticket[*idx] : 0;
    if (tk) { uint64_t w0 = now_ns(); mtl_present_wait(tk); g_pq_wait_ns += now_ns() - w0; }
    /* TOTK: the acquire sync means "this swapchain texture is no longer read by the display path". mtl_present_wait above
       already waited until the GPU finished the present copy that last read this texture, and every later use of it is
       ordered after that copy on the one Metal queue. Fencing the sync to the newest submission instead (the D2R behaviour,
       TOTK_ACQ_SYNC=old) made the game's CPU wait for the whole previous frame here, so replay and GPU of consecutive
       frames never overlapped and the GPU idled between frames. Bit 1 of fenced marks acquire syncs for TOTK_SYNC_LOG. */
    if (s) { static int old_sync = -1; if (old_sync < 0) { const char *e = getenv("TOTK_ACQ_SYNC"); old_sync = e && !strcmp(e, "old"); }
             int tok = 0; uint64_t v = old_sync ? fence_now(&tok) : 0;
             ((sync_t *)s)->fenced = tok ? 7 : 3; ((sync_t *)s)->fence = v; }
    totk_tl(TL_ACQ_OUT, (uint64_t)*idx, 0);
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
#ifdef GAME_BOTW
/* NVN 53 (Breath of the Wild 1.6.0, botw.md): the queue-side acquire. The same wait as nvnWindowAcquireTexture without a
   sync: it returns once the texture may be drawn into, and every later GPU use of it is ordered after the present copy on
   the one Metal queue. */
NVN(nvnQueueAcquireTexture, NVNqueueAcquireTextureResult, NVNqueue *q, NVNwindow *w, int *idx) {
    (void)q;
    int ok = h_nvnWindowAcquireTexture(w, NULL, idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
    { static int n; win_t *o = (win_t *)w;
      if (n < 6) { n++; LOG("[nvn] QueueAcquireTexture -> %d (texture serial %llu; swapchain %llu / %llu)", *idx,
                            (unsigned long long)(*idx >= 0 && *idx < o->ntex ? CTX(o->tex[*idx])->serial : 0),
                            (unsigned long long)CTX(o->tex[0])->serial, (unsigned long long)(o->ntex > 1 ? CTX(o->tex[1])->serial : 0)); } }
    return ok ? NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_SUCCESS : NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR;
}
#endif

// 帧边界：每次出图打印 / 落盘本帧各 NVN 函数调用次数
// Frame boundary: on each present, print / write to disk the call count of each NVN function for this frame
static uint64_t g_prev[NVN_NFUNC];
static FILE *g_tsv;
static void dump_frame(void) {
    static int inited, nframes = 3, calls = 1;
    if (!inited) {
        inited = 1;
        const char *e = getenv("DM_NVN_FRAMES"); if (e) nframes = atoi(e);
        const char *c = getenv("DM_NVN_TSV_CALLS"); if (c) calls = atoi(c);   // 0 = 只写 @ 计时行（真机拉文件有大小上限，调用计数行占约 2/3） | 0 = write only the @ timing rows (pulling files from a real device has a size limit, and the call-count rows take about 2/3)
        const char *t = getenv("DM_NVN_TSV"); if (t && !(g_tsv = fopen(t, "w"))) LOG("【红】打不开 %s", t);
        if (g_tsv) fprintf(g_tsv, "# kind: nvn-per-frame  name 为 NVN 函数名时值 = 调用次数；@ 开头为计时埋点（ms / 个）\nframe\tname\tvalue\n");
    }
    uint64_t total = 0; unsigned nfn = 0;
    char line[8192]; size_t o = 0; line[0] = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) {
        uint64_t c = atomic_load(&nvn_slot[i].cnt), d = c - g_prev[i];
        g_prev[i] = c;
        if (!d) continue;
        total += d; nfn++;
        if (g_tsv && calls) fprintf(g_tsv, "%llu\t%s\t%llu\n", g_frame, nvn_names[i], d);
        if (g_frame <= (uint64_t)nframes && o < sizeof line - 96) o += snprintf(line + o, sizeof line - o, " %s=%llu", nvn_names[i] + 3, d);
    }
    if (g_tsv) fflush(g_tsv);
    if (g_frame <= (uint64_t)nframes) LOG("[nvn] %s %llu：%u 个函数 %llu 次调用 |%s", g_frame ? "帧" : "初始化", g_frame, nfn, total, line);
    else if (g_frame % 300 == 0) LOG("[nvn] 帧 %llu：%u 个函数 %llu 次调用", g_frame, nfn, total);
    g_frame++; g_frame_pub = g_frame; cs_dump();
    { void runtime_watch_tick(uint64_t); runtime_watch_tick(g_frame); }   // TOTK diagnostic write watchpoint (TOTK_WATCH_ADDR)
    // 每 300 帧读一次常驻内存，交图形层打内存读数行（mtl_mem_stat，低频封顶）。内存看门狗：常驻超过上限就报红自行退出——只在 Mac 宿主上缺省开（24 GB：宿主内存失控
    // 会把整机拖进内核崩溃，同机别的程序两次如此），或测试时用 DM_MEM_CAP_GB=<整数> 指定；玩家的 iOS 包不设上限（被系统杀之前自己退没有意义，反而多一种闪退）
    // Every 300 frames read resident memory and hand it to the graphics layer to print the memory readout line (mtl_mem_stat, low frequency, capped). Memory watchdog: when resident memory exceeds the cap, log a failure and exit on its own. On by default only on the Mac host (24 GB: runaway host memory
    // drags the whole machine into a kernel panic, which happened twice with other programs on the same machine), or set for tests with DM_MEM_CAP_GB=<integer>; the player's iOS app has no cap (exiting before the system kills it is pointless and only adds another kind of crash)
    if (g_frame % 300 == 0) { task_vm_info_data_t vi; mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS) { unsigned long long mb = (unsigned long long)(vi.phys_footprint >> 20); void mtl_mem_stat(unsigned long long, unsigned long long); mtl_mem_stat(g_frame, mb);
            static long cap = -1; if (cap < 0) { const char *e = getenv("DM_MEM_CAP_GB"); cap = e ? atol(e) : DM_MEM_CAP_DEFAULT_GB; }
            if (cap > 0 && mb > (unsigned long long)cap * 1024) { LOG("【红】内存看门狗：常驻 %llu MB 超过上限 %ld GB（帧 %llu），自行退出", mb, cap, (unsigned long long)g_frame); _exit(86); } } }
    dm_guest_frame_tick();   // 确定性客体时钟（DM_DETCLOCK，宿主 hle_os.c）：每次出图推进固定步长；未开为空操作 | Deterministic guest clock (DM_DETCLOCK, host hle_os.c): each present advances a fixed step; a no-op when not enabled
}
// ---------- 计时埋点（逐帧行写进 DM_NVN_TSV：frame / @指标 / 值；每 300 帧汇总 p50 / p95 / 最大） ----------
// ---------- Timing instrumentation (per-frame rows written to DM_NVN_TSV: frame / @metric / value; every 300 frames a summary of p50 / p95 / max) ----------
#define TWIN 300
static double g_tw[6][TWIN]; static unsigned g_tn[6];
static double g_pres_iv[TWIN]; static unsigned g_presn;   // 上屏间隔（ms；−1 = 被系统丢弃，0 = 无读数） | Present interval (ms; -1 = dropped by the system, 0 = no reading)
__attribute__((weak)) double dm_brightness_now(void) { return -1; }
__attribute__((weak)) void dm_perf_present(double iv_ms) { (void)iv_ms; }   // 缺省（单测）；宿主 diag.c 的强定义：常开性能记录 perf.txt | Default (unit tests); strong definition in the host's diag.c: always-on performance record perf.txt
uint64_t dm_nvn_sub_cpu_ns(void) { return atomic_load(&g_cum_sub_cpu_ns); }   // 提交（不含出图）累计 CPU 纳秒 | Cumulative CPU nanoseconds of submit (excluding present)
__attribute__((weak)) int dm_thermal_now(void) { return -1; }   // 缺省（Mac）；iOS front_ios.m 覆盖   // 缺省（Mac）；iOS 前端 front_ios.m 的强定义覆盖它 | Default (Mac); overridden by iOS front_ios.m   // default (Mac); the strong definition in the iOS front end front_ios.m overrides it
// 长帧日志的逐帧环（GPU 时间滞后 8 帧才有，所以先存后判）
// Per-frame ring for the long-frame log (GPU time only arrives 8 frames late, so store first and judge later)
#define LFN 16
typedef struct { uint64_t f; double fr, nv, t_s, make_ms, up_mb, up_ms; unsigned pso, lib, up, rup; int ls; } lf_rec;   // ls：这一帧出图时读取画面是否盖着 | ls: whether the loading screen was covering the picture when this frame was presented
static lf_rec g_lf[LFN];
__attribute__((weak)) void dm_longframe_note(const char *what) { LOG("[长帧] %s", what); }   // 缺省（图形单测）；宿主 diag.c 的强定义补上场景上下文并写 perf.txt | Default (graphics unit tests); the strong definition in the host's diag.c adds scene context and writes perf.txt
extern void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns); extern uint64_t mtl_upload_bytes(void); extern void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads);
static const char *const g_tname[6] = {"帧时间", "出帧工作", "NVN 提交+出图", "GPU", "翻译层录制期", "翻译层提交期"};
static void tpush(int k, double v) { g_tw[k][g_tn[k]++ % TWIN] = v; }
// 发热归因（iOS DM_HEAT）：每帧 GPU 毫秒另存一圈，取「上次取数以来」的均值 / p95（最多最近 HRN 帧），与计时汇总的 300 帧窗口互不干扰
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
static void tsummary(uint64_t frame) {
    char line[1024]; size_t o = 0;
    for (int k = 0; k < 6; k++) {
        unsigned n = g_tn[k] < TWIN ? g_tn[k] : TWIN; if (!n) continue;
        double v[TWIN]; memcpy(v, g_tw[k], n * sizeof *v); qsort(v, n, sizeof *v, dcmp);
        o += snprintf(line + o, sizeof line - o, " %s p50 %.2f / p95 %.2f / 最大 %.2f ms（%u 帧）；", g_tname[k], v[n / 2], v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1], v[n - 1], n);
    }
    LOG("[nvn] 计时 至帧 %llu：%s", frame, line);
    { unsigned n = g_tn[0] < TWIN ? g_tn[0] : TWIN, over = 0; double v[TWIN];   // 帧间隔另起一行（M1 基线）：本窗口 > 16.7 ms 帧数与 p99 | frame interval gets its own line (M1 baseline): number of frames > 16.7 ms in this window and p99
      if (n) { memcpy(v, g_tw[0], n * sizeof *v); qsort(v, n, sizeof *v, dcmp); for (unsigned i = 0; i < n; i++) over += v[i] > 16.7;
        LOG("[nvn] 帧间隔 至帧 %llu：>16.7ms %u / %u 帧，p99 %.2f ms", frame, over, n, v[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1]); } }
    { unsigned n = g_presn < TWIN ? g_presn : TWIN, k = 0, drop = 0, discard = 0; double v[TWIN];   // 上屏：掉帧 = 间隔 > 16.7 × 1.5 ms 或被丢弃 | on-screen present: dropped frame = interval > 16.7 × 1.5 ms or discarded
      for (unsigned i = 0; i < n; i++) { double x = g_pres_iv[i]; if (x < 0) { drop++; discard++; } else if (x > 0) { v[k++] = x; drop += x > 25.0; } }
      task_vm_info_data_t vi; mach_msg_type_number_t vc = TASK_VM_INFO_COUNT; double fp = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &vc) == KERN_SUCCESS ? vi.phys_footprint / 1048576.0 : -1;
      double br = dm_brightness_now();   // footprint / 亮度随上屏行带出：不依赖拍入口观测门（营地局也有） | footprint / brightness are carried on the on-screen present row: does not depend on the tick-entry observation gate (camp runs have it too)
      if (k) { qsort(v, k, sizeof *v, dcmp); LOG("[nvn] 上屏 至帧 %llu：%u 帧，掉帧 %u（丢弃 %u），p99 %.2f ms；footprint %.0f MiB；亮度 %.2f；温控 %d", frame, k + discard, drop, discard, v[(k * 99) / 100 < k ? (k * 99) / 100 : k - 1], fp, br, dm_thermal_now()); }
      g_presn = 0; }
    { static uint64_t pi, pc, pd, pf; uint64_t ci = atomic_load(&g_cum_sub_instr), cc = atomic_load(&g_cum_sub_cpu_ns), ps_, dr_; mtl_stats(&ps_, &dr_);   // 翻译层 | translation layer
      if (pf && frame > pf && dr_ > pd) LOG("[nvn] 翻译 至帧 %llu：每次绘制 %.0f 条指令，每帧提交 %.2f M 条 / CPU %.3f ms，每帧绘制 %.0f", frame,
          (double)(ci - pi) / (double)(dr_ - pd), (ci - pi) / 1e6 / (double)(frame - pf), (cc - pc) / 1e6 / (double)(frame - pf), (double)(dr_ - pd) / (double)(frame - pf));
      { static uint64_t pae, par; uint64_t ae, ar; mtl_args_stats(&ae, &ar);   // TOTK: argument blocks encoded vs rebound unchanged
        if (pf && frame > pf) LOG("[nvn] 参数块 至帧 %llu：每帧新编 %.0f 个，复用 %.0f 个（%.0f%%）", frame, (double)(ae - pae) / (double)(frame - pf), (double)(ar - par) / (double)(frame - pf),
                                  ae - pae + ar - par ? 100.0 * (double)(ar - par) / (double)(ae - pae + ar - par) : 0.0);
        pae = ae; par = ar; }
      { extern uint64_t g_draw_skip[16]; static uint64_t prev[16]; char b[512]; size_t o = 0; b[0] = 0;
        static const char *why[] = {"no-program", "zero-source", "defer-take", "render-disabled", "raster-discard", "cull-both", "scissor-empty", "deferred", "no-encoder", "no-pipeline", "viewport-empty", "primitive",
                                    "clear-render-disabled", "cond-disabled", "cond-unknown", "shader-missing"};   // TOTK: 12-14 conditional rendering (see cond_eval); 15 a shader variant the iPhone does not have yet
        for (int i = 0; i < 16; i++) { uint64_t d = g_draw_skip[i] - prev[i]; prev[i] = g_draw_skip[i]; if (d && o < sizeof b - 48) o += snprintf(b + o, sizeof b - o, " %s=%llu", why[i], (unsigned long long)d); }
        if (o) LOG("[nvn] TOTK 回放丢弃的绘制 至帧 %llu:%s", frame, b); }
      { static uint64_t py, pk, pw; uint64_t cy = atomic_load(&g_cum_sub_cyc), ck = atomic_load(&g_cum_sub_sys_ns), cw = atomic_load(&g_cum_sub_wall_ns);
        if (pf && frame > pf && ci > pi && cy > py) { double nf = (double)(frame - pf), cyc = (double)(cy - py), cpu = (double)(cc - pc);
          qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0; pthread_get_qos_class_np(pthread_self(), &qc, &rp);
          uint64_t aw = 0, ag = 0; int an = 0; void mtl_alloc_stats(uint64_t *, uint64_t *, int *); mtl_alloc_stats(&aw, &ag, &an);
          static uint64_t pyn, pyns; uint64_t yn = 0, yns = 0; dm_main_yield_stats(&yn, &yns);
          LOG("[nvn] 翻译细项 至帧 %llu：每帧提交 周期 %.2f M（每条指令 %.2f 周期）、CPU %.3f ms 其中内核态 %.3f ms、墙钟 %.3f ms；平均主频 %.2f GHz（周期 / CPU 时间）；本线程 QoS %#x；分配器池 %d 个、累计等 GPU %llu 次；实时声明计算 %.2f ms；主线程让出 每帧 %.0f 次 / %.3f ms", frame,
              cyc / 1e6 / nf, cyc / (double)(ci - pi), cpu / 1e6 / nf, (double)(ck - pk) / 1e6 / nf, (double)(cw - pw) / 1e6 / nf, cpu > 0 ? cyc / cpu : 0, (unsigned)qc, an, (unsigned long long)aw, g_rt_comp_ms, (double)(yn - pyn) / nf, (double)(yns - pyns) / 1e6 / nf); pyn = yn; pyns = yns; }
        { static uint64_t pun, puns; uint64_t un = 0, uns = 0; void mtl_upload_wait_stats(uint64_t *, uint64_t *); mtl_upload_wait_stats(&un, &uns);
          if (pf && frame > pf) LOG("[nvn] 上传槽等待 至帧 %llu：每帧 %.2f 次 / %.3f ms", frame, (double)(un - pun) / (double)(frame - pf), (double)(uns - puns) / 1e6 / (double)(frame - pf));
          pun = un; puns = uns; }
        { static uint64_t pcc; uint64_t mtl_chunk_commits(void), cc = mtl_chunk_commits();
          if (pf && frame > pf) LOG("[nvn] 分段提交 至帧 %llu：每帧额外 %.2f 段", frame, (double)(cc - pcc) / (double)(frame - pf));
          pcc = cc; }
        if (synclog_env() && pf && frame > pf) { static uint64_t pc[2], pb[2], pn[2], pw[2]; double nf = (double)(frame - pf);
          for (int k = 0; k < 2; k++) { uint64_t c = atomic_load(&g_sw_calls[k]), b = atomic_load(&g_sw_blocked[k]), n = atomic_load(&g_sw_ns[k]), w = atomic_load(&g_sw_newest[k]);
            LOG("[nvn] CPU 等同步 至帧 %llu：%s 每帧 %.2f 次，其中真的等 %.2f 次 / %.3f ms，等的是最近一次提交 %.2f 次", frame, k ? "取图同步" : "栅栏同步", (c - pc[k]) / nf, (b - pb[k]) / nf, (n - pn[k]) / 1e6 / nf, (w - pw[k]) / nf);
            pc[k] = c; pb[k] = b; pn[k] = n; pw[k] = w; } }
        { static uint64_t pv[6]; uint64_t v[6]; void mtl_blit_stats(uint64_t[6]); mtl_blit_stats(v);
          if (pf && frame > pf) { double nf = (double)(frame - pf);
            LOG("[nvn] 拷贝编码器 至帧 %llu：每帧 %.1f 个（其中打断渲染通道 %.1f）；uniform 更新 %.1f 次（打断 %.1f），缓冲拷贝 %.1f 次（打断 %.1f）", frame,
                (v[0] - pv[0]) / nf, (v[1] - pv[1]) / nf, (v[2] - pv[2]) / nf, (v[3] - pv[3]) / nf, (v[4] - pv[4]) / nf, (v[5] - pv[5]) / nf); }
          memcpy(pv, v, sizeof pv); }
        { static uint64_t pp[4]; uint64_t v[4]; void mtl_pool_res_stats(uint64_t[4]); mtl_pool_res_stats(v);   // TOTK: memory pools resident on first GPU use (nvn_mtl.m pool_res)
          if (memcmp(v, pp, sizeof pp)) LOG("[nvn] 内存池 至帧 %llu：在册 %llu 个 / %llu MB，其中已放进驻留集 %llu 个 / %llu MB", frame,
              (unsigned long long)v[0], (unsigned long long)(v[1] >> 20), (unsigned long long)v[2], (unsigned long long)(v[3] >> 20));
          memcpy(pp, v, sizeof pp); }
        { static uint64_t pk[2]; uint64_t v[3]; void mtl_keepalive_stats(uint64_t[3]); mtl_keepalive_stats(v);   // TOTK: GPU keep-alive spin kernels (nvn_mtl.m keepalive_start)
          if (pf && frame > pf && v[0] > pk[0]) LOG("[nvn] GPU 保活 至帧 %llu：每帧 %.2f 个自旋核，平均 %.2f ms，最长 %.2f ms", frame, (double)(v[0] - pk[0]) / (double)(frame - pf),
              (double)(v[1] - pk[1]) / 1e3 / (double)(v[0] - pk[0]), (double)v[2] / 1e3);
          pk[0] = v[0]; pk[1] = v[1]; }
        { static uint64_t pl[4]; uint64_t v[4]; void mtl_layer_stats(uint64_t[4]); mtl_layer_stats(v);   // TOTK: layered passes (nvn_mtl.m g_lay)
          if (pf && frame > pf) { double nf = (double)(frame - pf);
            LOG("[nvn] 渲染通道 至帧 %llu：每帧 %.1f 个，其中分层通道 %.2f 个，并入的设目标 %.2f 次，因绘制不能写层号退回 %.2f 次", frame,
                (v[0] - pl[0]) / nf, (v[1] - pl[1]) / nf, (v[2] - pl[2]) / nf, (v[3] - pl[3]) / nf); }
          memcpy(pl, v, sizeof pl); }
        { static uint64_t pr[6]; uint64_t v[6]; void mtl_ren_stats(uint64_t[6]); mtl_ren_stats(v);
          if (pf && frame > pf) { double nf = (double)(frame - pf);
            LOG("[nvn] uniform 换副本 至帧 %llu：每帧更新 %.1f 次，新副本 %.1f 个 / %.1f KB，提前写回 %.2f 次，开编码器后才发现重叠 %.2f 次，表满回退 %.2f 次", frame,
                (v[0] - pr[0]) / nf, (v[1] - pr[1]) / nf, (v[2] - pr[2]) / nf / 1024, (v[3] - pr[3]) / nf, (v[4] - pr[4]) / nf, (v[5] - pr[5]) / nf); }
          memcpy(pr, v, sizeof pr); }
        if (frame % 300 == 0) { unsigned wn = 0; uint32_t wq = dm_guest_work_qos(&wn); thread_extended_info_data_t xi; mach_msg_type_number_t xc = THREAD_EXTENDED_INFO_COUNT; int okx = thread_info(pthread_mach_thread_np(pthread_self()), THREAD_EXTENDED_INFO, (thread_info_t)&xi, &xc) == KERN_SUCCESS;
            thread_time_constraint_policy_data_t tp; mach_msg_type_number_t tc = THREAD_TIME_CONSTRAINT_POLICY_COUNT; boolean_t def = 0; int rt_live = thread_policy_get(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&tp, &tc, &def) == KERN_SUCCESS && !def;   // def = 1：实时策略没在生效（没设上 / 被撤） | def = 1: the real-time policy is not in effect (never set / revoked)
            LOG("[nvn] 线程策略：主线程 实时 计算 %.2f ms / 周期 %.2f ms，实时策略%s，当前优先级 %d（基础 %d，调度类 %d）；工作线程 %u 条 QoS %#x；出图 QoS %#x", g_rt_comp_ms, g_rt_period_ms, rt_live ? "在生效" : "未生效", okx ? xi.pth_curpri : -1, okx ? xi.pth_priority : -1, okx ? xi.pth_policy : -1, wn, wq, mtl_present_qos()); }
        py = cy; pk = ck; pw = cw; }
      pi = ci; pc = cc; pd = dr_; pf = frame; }
    uint64_t up, re, fa, pr; mtl_pkg_stats(&up, &re); nvn_dirty_stats(&fa, &pr);
    LOG("[nvn] 预排布贴图 首传 %llu 张、池内容变化重传 %llu 次；写保护缺页 %llu 次、宿主写前解保护 %llu 页；漏检 %llu（累计）", up, re, fa, pr, mtl_pkg_miss());
    memset(g_tn, 0, sizeof g_tn);
}
// DM_SLOWFRAME=1（测试开关，真机经日志端口读）：逐帧计时先存近 16 帧环；滞后 8 帧（GPU / 上屏时刻已回）判掉帧，
// 只对掉帧写一行「[slowframe]\t帧号\t@名=值…」进日志，另每 300 帧一行分母。不写逐帧文件：日志 tee 每行 fsync，逐行写会扰动读数
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
// 掉帧口径：上屏间隔 > 16.7×1.5 ms 或 −1；无窗口（无上屏时刻）退回出图调用间隔
// Dropped-frame definition: on-screen present interval > 16.7×1.5 ms or −1; without a window (no on-screen present time) fall back to the present call interval
static void slow_emit(uint64_t f) {
    static uint64_t judged, slow;
    double pm = srow(f, "@presented_ms", 0), fr = srow(f, "@frame_ms", 0), lim = 1000.0 / 60 * 1.5;
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
    if (f % 300 == 0) fprintf(stderr, "[slowframe]\t#\t至帧 %llu\tjudged=%llu\tslow=%llu\t判据=%s\n", f, judged, slow, has ? "上屏间隔" : "出图调用间隔（无窗口）");
}
// 时间约束（实时）线程的声明：周期 = 出图周期，约束 = 周期，计算 = 这条线程每帧实际要用的 CPU 时间。
// 原先计算量写死 2 ms，而游戏主线程（游戏逻辑 + 录制 + 提交回放）热态每帧约 10 ms——声明与实际差 5 倍：
// 实时线程超出自己声明的计算量会被系统降级处理，性能控制器也按声明定频（系统就是靠这份声明得知帧期限和工作量的）。
// DM_RT_COMP=<毫秒> 固定声明；缺省 auto = 每 120 帧取本线程每帧 CPU 时间的 p90，与已声明值差 > 15% 就重新声明（上限 0.9 × 周期）。
// Declaration for the time-constraint (real-time) thread: period = present period, constraint = period, computation = the CPU time this thread actually needs per frame.
// The computation used to be hard-coded at 2 ms, while the game main thread (game logic + recording + submit replay) takes about 10 ms per frame when hot - a 5× gap between declared and actual:
// a real-time thread that exceeds its declared computation gets demoted by the system, and the performance controller also sets frequency from the declaration (this declaration is how the system learns the frame deadline and the workload).
// DM_RT_COMP=<ms> fixes the declaration; default auto = every 120 frames take the p90 of this thread's per-frame CPU time and re-declare if it differs from the declared value by > 15% (capped at 0.9 × period).
static void set_time_constraint_comp(uint64_t period_ns, uint64_t comp_ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    if (comp_ns < 1000000ull) comp_ns = 1000000ull; if (comp_ns > period_ns * 9 / 10) comp_ns = period_ns * 9 / 10;
    thread_time_constraint_policy_data_t p = {
        .period = (uint32_t)(period_ns * tb.denom / tb.numer), .computation = (uint32_t)(comp_ns * tb.denom / tb.numer),
        .constraint = (uint32_t)(period_ns * tb.denom / tb.numer), .preemptible = 1};
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr == KERN_SUCCESS) { g_rt_comp_ms = comp_ns / 1e6; g_rt_period_ms = period_ns / 1e6; }
    LOG("[nvn] 出图线程时间约束策略（周期 %.3f ms、计算 %.3f ms）%s", period_ns / 1e6, comp_ns / 1e6, kr == KERN_SUCCESS ? "已设" : "设置失败");
}
// TOTK translation thread: the same policy for the calling thread without touching the render thread's declaration state.
void set_time_constraint_comp_x(uint64_t period_ns, uint64_t comp_ns) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    thread_time_constraint_policy_data_t p = {
        .period = (uint32_t)(period_ns * tb.denom / tb.numer), .computation = (uint32_t)(comp_ns * tb.denom / tb.numer),
        .constraint = (uint32_t)(period_ns * tb.denom / tb.numer), .preemptible = 1};
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    LOG("[nvn] 翻译线程时间约束策略（周期 %.3f ms、计算 %.3f ms）%s", period_ns / 1e6, comp_ns / 1e6, kr == KERN_SUCCESS ? "已设" : "设置失败");
}
static int rt_comp_fixed_ms(void) { static int v = -2; if (v == -2) { const char *e = getenv("DM_RT_COMP"); v = e && *e && strcmp(e, "auto") ? atoi(e) : -1; } return v; }   // −1 = auto
static void set_time_constraint(uint64_t period_ns) { int f = rt_comp_fixed_ms(); set_time_constraint_comp(period_ns, f > 0 ? (uint64_t)f * 1000000ull : 2000000ull); }   // 首次：固定值，或 auto 的起始值 2 ms | first time: the fixed value, or auto's starting value of 2 ms
// 每帧在取图处调：记本线程自上次以来的 CPU 时间；auto 模式下按 p90 重新声明
// Called every frame at acquire: records this thread's CPU time since the last call; in auto mode re-declares based on p90
static void rt_comp_tick(uint64_t period_ns) {
    if (rt_comp_fixed_ms() > 0 || !period_ns || !g_rt_comp_ms) return;
    static uint64_t last; static double win[120]; static int n;
    uint64_t c = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); if (!last) { last = c; return; }
    win[n++] = (double)(c - last) / 1e6; last = c;
    if (n < 120) return;
    double v[120]; memcpy(v, win, sizeof v); qsort(v, 120, sizeof *v, dcmp); n = 0;
    double p90 = v[107], comp = p90 * 1.25, cap = period_ns * 0.9 / 1e6; if (comp > cap) comp = cap; if (comp < 1) comp = 1;
    if (comp > g_rt_comp_ms * 1.15 || comp < g_rt_comp_ms * 0.85) set_time_constraint_comp(period_ns, (uint64_t)(comp * 1e6));
}
static void sleep_until_ns(uint64_t dl) {   // 单调时钟上的绝对时刻 | absolute time on the monotonic clock
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    uint64_t now = now_ns(); if (dl <= now) return;
    mach_wait_until(mach_absolute_time() + (dl - now) * tb.denom / tb.numer);
}
// 出图固定 60 Hz：自适应机制在 24/24 份真机日志第 60 帧退档且不再回升，本期先以 60 帧为硬底。
// DM_PRESENT_HZ=0（不限帧率）只作显式的性能剖析开关：游戏部分界面逻辑按帧计数（同存档不限帧率时
// SaveIndicatorPanel 4181 条、限 60 帧时 0 条，与真值一致），不限帧率在语义上是错的跑法，不能作判据 / 对拍 / 缺省。单测用 nvn_present_config 直接设
// Present is fixed at 60 Hz: the adaptive mechanism stepped down at frame 60 in 24/24 real-device logs and never came back up, so for this phase 60 fps is the hard floor.
// DM_PRESENT_HZ=0 (uncapped frame rate) serves only as an explicit performance profiling switch: some of the game's UI logic counts frames (with the same save, uncapped gives
// 4181 SaveIndicatorPanel entries and capped at 60 fps gives 0, which matches the ground truth), so uncapped is a semantically wrong way to run and must not be used as a criterion / cross-check / default. Unit tests set it directly with nvn_present_config
static int g_hz = -1, g_display_mode;
/* TOTK_UNCAPPED (Mac and iPhone, default on; =0 off): like vsync off in other games. The game no longer waits for the display: it is
   paced by our own clock at UNCAPPED_HZ, and the display link shows the newest finished frame at every refresh (older ones
   are merged away, as when the game runs ahead). The ceiling is 120 because the real-time frame step (game_patches.c)
   clamps a frame to at least 1/120 s: above that the game would run fast. On a display of 120 Hz or more it changes nothing. */
#define UNCAPPED_HZ 120
/* TOTK_UNCAPPED_HZ: the uncapped clock's ceiling, 30..120 (default 120), or "display" for the display's refresh rate: the
   iPhone app's default since 2026-10-17 (user: at most 60 on the iPhone 14's 60 Hz screen, where frames above it are never
   shown and only heat the phone). The game still does not wait for the display, so a scene slower than the ceiling is not
   held to 60 / 30 steps. */
static int uncapped_cap(int scr) {
    const char *e = getenv("TOTK_UNCAPPED_HZ");
    int v = !e ? UNCAPPED_HZ : !strcmp(e, "display") ? (scr > 0 ? scr : 60) : atoi(e);
    return v < 30 ? 30 : v > UNCAPPED_HZ ? UNCAPPED_HZ : v;
}
void nvn_present_config(int hz, int display_mode) { g_hz = g_paced_hz = hz; g_display_mode = display_mode; }
static void present_cfg(const win_t *o) {
    if (g_hz >= 0) return;
    // 有窗口仍缺省 display 间隔模式；无窗口缺省 game 模式。环境变量可覆盖固定 60 Hz。
    // With a window the default is still display interval mode; without a window the default is game mode. Environment variables can override the fixed 60 Hz.
    int scr = mtl_display_hz();
    const char *e = getenv("DM_PRESENT_HZ"), *rt = getenv("TOTK_REALTIME");
    int realtime = !(rt && !strcmp(rt, "0"));
    g_hz = g_paced_hz = e ? atoi(e) : (realtime && scr > 0 ? scr : 60);
    { const char *u = getenv("TOTK_UNCAPPED"), *f60 = getenv("TOTK_FPS60");   // the original 30-frame mode needs fixed pacing; MetalFX interpolation suspends it while on (uncapped_now)
      g_uncapped = !e && realtime && !(u && !strcmp(u, "0")) && !(f60 && !strcmp(f60, "0")) && g_hz < UNCAPPED_HZ; }
    if (g_uncapped) { g_hz = uncapped_cap(scr); LOG("[nvn] 不限帧率（TOTK_UNCAPPED，默认开；=0 关）：不等显示器刷新，自己的时钟上限 %d 帧（TOTK_UNCAPPED_HZ）；显示器 %d Hz 每次刷新显示最新一帧%s", g_hz, scr,
                                       mfx_enabled() ? "（补帧开着时暂停：每两次刷新一帧）" : "（补帧打开时暂停，2 键）"); }
    if (!g_hz) LOG("[nvn] 【警告】DM_PRESENT_HZ=0：不限帧率，帧节奏非真机，界面逻辑可能偏离（仅供性能剖析，不能用于判据 / 对拍）");
    // 剪口（有意偏离原版行为）：DM_PRESENT_INTERVAL=display 时不按游戏设的出图间隔，每个节拍出一帧，
    // 固定 60 帧硬底；逻辑 25 拍不变，且客户端按帧计数的界面逻辑不受影响——
    // 同存档同输入下两模式 UI 消息流逐条一致。证明（选角前）：r9 game / display 60 ×3 / display 120 ×2 共 6 组 uicmp 一致
    // （存档指示按时序竞态豁免），关键消息时刻差在 game 自身噪声带内；进局后的证明尚缺，待动作脚本覆盖
    // Cut (intentional deviation from the original game's behavior): with DM_PRESENT_INTERVAL=display the present interval set by the game is not followed and one frame is presented per display-link tick,
    // with a fixed hard floor of 60 fps; the logic stays at 25 ticks, and the client's frame-counted UI logic is unaffected -
    // with the same save and the same input, the UI message streams of the two modes match message by message. Proof (before character select): r9 game / display 60 ×3 / display 120 ×2, 6 groups in total, uicmp consistent
    // (the save indicator is exempted as a timing race), and the timing differences of key messages are within game mode's own noise band; proof after entering a run is still missing, pending action script coverage
    /* TOTK: game mode is the default. The engine's variable-frame-rate step (main+0x7a83e4) advances game time by the
       present interval it chose for every frame, so presenting on every display tick (D2R's display mode) made menus
       and light scenes run up to twice as fast. DM_PRESENT_INTERVAL=display keeps the old behaviour for comparison. */
    const char *m = getenv("DM_PRESENT_INTERVAL"); g_display_mode = m && !strcmp(m, "display");
    LOG("[nvn] 出图节拍 %d Hz，间隔模式 %s（游戏设的出图间隔 %d；%s）", g_hz, g_display_mode ? "display（每节拍一帧，剪口）" : "game（原版语义）", o->interval,
        e ? "显式指定" : g_uncapped ? "不限帧率" : realtime && scr > 0 ? "跟随显示器" : "固定 60 Hz");
}
static unsigned present_ticks(const win_t *o) {   /* display ticks per game frame */
    present_cfg(o);
    /* TOTK_MFX / key 2: MetalFX makes the frame between two game frames. Half the refresh rate: the game presents every
       second display tick (50 + 50 on a 100 Hz display); 30+30: hz / 30 ticks (2 at 60 Hz; at 100 Hz the own clock paces,
       mfx_own_clock). Held here and not only by the game's controller, because the code carries the 60 FPS patch (the
       controller may choose 1); the real-time frame step keeps game speed right. Uncapped interpolation paces as uncapped. */
    int m = mfx_enabled();
    if (m == MFX_HALF) return 2;
    if (m == MFX_30) { int hz = g_paced_hz > 0 ? g_paced_hz : 60; return (unsigned)(hz >= 45 ? (hz + 15) / 30 : 1); }
    return g_display_mode || (uncapped_now() && !dm_detclock_enabled() && uncapped_interval_on()) || o->interval <= 0 ? 1u : (unsigned)o->interval;
}
static uint64_t present_period(const win_t *o) {
    unsigned itv = present_ticks(o); int m = mfx_enabled(), hz = g_uncapped && !uncapped_now() ? g_paced_hz : g_hz;
    { static int was = -1; if (was >= 0 && m != was)
        LOG("[nvn] 出图节拍切换：%s", m == MFX_30 ? (mfx_own_clock() ? "补帧 30+30：游戏按自己的时钟每秒 30 帧" : "补帧 30+30：跟随显示器，每秒 30 帧") :
                                    m == MFX_HALF ? "补帧 刷新率一半：跟随显示器、每两次刷新一帧" :
                                    m == MFX_MAX ? (g_uncapped ? "补帧 不限帧：不限帧率，每帧补一帧" : "补帧 不限帧：按游戏的出图间隔，每帧补一帧") :
                                    g_uncapped ? "补帧关：不限帧率" : "补帧关：按游戏的出图间隔"); was = m; }
    if (m == MFX_30) return 1000000000ull / 30;   /* exactly 30 fps, by display ticks or by our own clock */
    return hz > 0 ? (uint64_t)itv * 1000000000ull / (uint64_t)hz : 0;
}
/* Exercise production policy, including the shrine's interval=2 and the
   legacy 30 FPS path. No sleep, present or GPU allocation is involved. */
void dm_nvn_probe_present_interval(void) {
    int saved_hz = g_hz, saved_display = g_display_mode, saved_uncapped = g_uncapped;
    int saved_flag = atomic_load(&g_uncapped_interval), checked = 0;
    const char *neg = getenv("TOTK_PROBE_NEGATIVE");
    for (int hz = 60; hz <= 120; hz += 60) for (int uncapped = 0; uncapped < 2; uncapped++)
    for (int display = 0; display < 2; display++) for (int on = 0; on < 2; on++)
    for (int interval = -1; interval <= 3; interval++) {
        win_t w = {.interval = interval}; g_hz = hz; g_display_mode = display; g_uncapped = uncapped;
        atomic_store(&g_uncapped_interval, neg && atoi(neg) == 11 ? 0 : on);
        unsigned expected = display || (uncapped && on && !dm_detclock_enabled()) || interval <= 0 ? 1u : (unsigned)interval;
        if (present_ticks(&w) != expected || present_period(&w) != (uint64_t)expected * 1000000000ull / hz)
            die("present interval probe: effective pacing differs from expected");
        checked++;
    }
    /* Frame interpolation modes (key 2 / flag mfx_interp, taken over at the frame boundary). Half the refresh rate: two
       display ticks per game frame at the display rate, also when the uncapped mode is configured (suspended). 30+30:
       exactly 1/30 s, by display ticks where the rate is a multiple of 30 and by our own clock otherwise (100 Hz).
       Uncapped interpolation: the same pacing as without interpolation. Negative control 20: the request is never taken
       over, so the uncapped pacing stays and the half-rate check fails. */
    extern void mfx_set(int mode); extern int mfx_latch(void);
    int saved_paced = g_paced_hz, saved_mfx = mfx_enabled();
    mfx_set(MFX_HALF); if (!(neg && atoi(neg) == 20)) mfx_latch();
    for (int uncapped = 0; uncapped < 2; uncapped++) for (int interval = -1; interval <= 3; interval++) {
        win_t w = {.interval = interval}; g_paced_hz = 100; g_hz = uncapped ? UNCAPPED_HZ : 100; g_display_mode = 0; g_uncapped = uncapped;
        if (present_ticks(&w) != 2 || present_period(&w) != 2 * 1000000000ull / 100 || uncapped_now() || mfx_own_clock())
            die("present interval probe: frame interpolation must pace two display ticks at the display rate");
        checked++;
    }
    mfx_set(MFX_30); mfx_latch();
    const int rates[] = {60, 100, 120, 144}; const unsigned ticks30[] = {2, 3, 4, 5};
    for (int r = 0; r < 4; r++) for (int uncapped = 0; uncapped < 2; uncapped++) {
        win_t w = {.interval = 1}; g_paced_hz = rates[r]; g_hz = uncapped ? UNCAPPED_HZ : rates[r]; g_display_mode = 0; g_uncapped = uncapped;
        if (present_period(&w) != 1000000000ull / 30 || present_ticks(&w) != ticks30[r] || uncapped_now() || mfx_own_clock() != (rates[r] % 30 != 0))
            die("present interval probe: 30+30 must pace exactly 30 fps (display ticks at 60 / 120 Hz, our own clock at 100 / 144 Hz)");
        checked++;
    }
    mfx_set(MFX_MAX); mfx_latch();
    for (int uncapped = 0; uncapped < 2; uncapped++) for (int interval = -1; interval <= 3; interval++) {
        win_t w = {.interval = interval}; g_paced_hz = 100; g_hz = uncapped ? UNCAPPED_HZ : 100; g_display_mode = 0; g_uncapped = uncapped;
        atomic_store(&g_uncapped_interval, 0);
        unsigned expected = interval <= 0 ? 1u : (unsigned)interval;
        if (present_ticks(&w) != expected || present_period(&w) != (uint64_t)expected * 1000000000ull / (uncapped ? UNCAPPED_HZ : 100) || uncapped_now() != uncapped || mfx_own_clock())
            die("present interval probe: uncapped interpolation must keep the pacing of the mode without interpolation");
        checked++;
    }
    mfx_set(saved_mfx); mfx_latch();
    g_hz = saved_hz; g_paced_hz = saved_paced; g_display_mode = saved_display; g_uncapped = saved_uncapped;
    atomic_store(&g_uncapped_interval, saved_flag);
    LOG("[present-interval] %d pacing cases passed", checked);
}
int (*g_nvn_loadscreen_up)(void);   // 宿主登记：读取画面此刻是否开着（diag.c 订阅界面消息时填；图形单测里为空 = 不设闸） | host registration: whether the loading screen is up right now (filled in when diag.c subscribes to UI messages; null in graphics unit tests = no gate)
// The present itself: screenshot hook, then the present queue (returns at once). frame != 0 (translation thread): also the
// replay frame number, which the synchronous path sets at the end of nvnQueuePresentTexture.
static void present_exec(win_t *o, int idx, double period, const int crop[4], uint64_t frame) {
    gfx_enter();
    { void mtl_shot_at_present(const tex_t *t); if (idx >= 0 && idx < o->ntex && idx < 4) mtl_shot_at_present(CTX(o->tex[idx])); }
    if (idx >= 0 && idx < o->ntex && idx < 4) o->ticket[idx] = mtl_present(CTX(o->tex[idx]), period, (int *)crop);   // 只入队，立即返回 | enqueue only, return immediately
    if (frame) mtl_set_frame(frame);
    gfx_leave();
}
NVN(nvnQueuePresentTexture, void, NVNqueue *q, NVNwindow *w, int idx) {
    NVN_BLOCKING();
    (void)q; win_t *o = (win_t *)w; o->presents++;
    totk_tl_frame(o->presents); totk_tl(TL_PRES_IN, (uint64_t)idx, 0);
    if (runtime_frame_step_update) runtime_frame_step_update(now_ns());
    if (movie_host_tick) movie_host_tick();
    game_work_tick(o->presents);
    static uint64_t prev_in, prev_out, prev_pass, prev_draw;
    // D2R installs g_nvn_loadscreen_up from its UI-message observer. The TOTK
    // native host has no such observer, so also recognize the first world frame
    // from its draw count. The title transition briefly hit 1473 draws in a
    // measured run; the first world frames exceeded 2000, so >1000 would
    // mistakenly stall the title rather than the save-loading screen.
    // Hold that first frame behind the loading screen if cache warm-up is still
    // active. This does not claim that an undiscovered variant cannot appear.
    { static int gate; static uint64_t last_draws;
      uint64_t passes, draws; mtl_stats(&passes, &draws);
      uint64_t this_draws = draws >= last_draws ? draws - last_draws : 0;
      last_draws = draws;
      if (!gate && ((g_nvn_loadscreen_up && g_nvn_loadscreen_up()) || this_draws > 2000)) {
          gate = 1; extern void mtl_note_world(void); mtl_note_world(); int ms = mtl_warm_wait(15000);
          if (ms) LOG("[nvn] 首次进局等管线预热 %d ms（本帧绘制 %llu）", ms, (unsigned long long)this_draws);
      } }
    present_cfg(o);
    uint64_t t0 = now_ns();
    if (async_replay()) {   // TOTK: the present (and the replay frame number below) run on the translation thread, after this frame's submits
        xjob j = {.kind = XJ_PRESENT, .win = o, .idx = idx, .period = present_period(o) / 1e9};
        memcpy(j.crop, o->crop, sizeof j.crop);
        j.frame = g_frame + 1;   // dump_frame below advances g_frame before mtl_set_frame
        uint64_t no = x_push(&j);
        if (idx >= 0 && idx < 4) { g_xp_win = o; g_xp_job[idx] = no; }
    } else present_exec(o, idx, present_period(o) / 1e9, o->crop, 0);
    atomic_fetch_add(&g_nvn_ns, now_ns() - t0);
    uint64_t f = g_frame, ps, dr; mtl_stats(&ps, &dr);
    trow(f, "@present_guest_ticks", o->interval);
    trow(f, "@present_effective_ticks", present_ticks(o));
    if (nvn_real[0] || nvn_real[1]) {   // DM_NVN_PROF：录制期 = 除提交外全部已实现函数耗时；提交期 = QueueSubmitCommands | DM_NVN_PROF: record phase = time of all implemented functions except submit; submit phase = QueueSubmitCommands
        static unsigned isub = ~0u; static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
        if (isub == ~0u) for (isub = 0; isub < NVN_NFUNC && strcmp(nvn_names[isub], "nvnQueueSubmitCommands"); isub++) {}
        uint64_t rec = 0, sub = 0;
        static uint64_t acc[NVN_NFUNC];
        for (unsigned i = 0; i < NVN_NFUNC; i++) { uint64_t t = atomic_exchange(&nvn_prof_ticks[i], 0); acc[i] += t; if (i == isub) sub = t; else rec += t; }
        if ((g_frame + 1) % TWIN == 0) {   // 每 300 帧列出耗时最多的 5 个函数 | every 300 frames list the 5 most time-consuming functions
            char line[1024]; size_t o = 0;
            for (int k = 0; k < 5; k++) { unsigned best = 0; for (unsigned i = 1; i < NVN_NFUNC; i++) if (acc[i] > acc[best]) best = i;
                if (!acc[best]) break; o += snprintf(line + o, sizeof line - o, " %s %.2f ms；", nvn_names[best] + 3, acc[best] * tb.numer / tb.denom / 1e6); acc[best] = 0; }
            LOG("[nvn] 翻译层耗时前 5（近 300 帧合计）：%s", line); memset(acc, 0, sizeof acc);
        }
        trow(g_frame, "@rec_ms", rec * tb.numer / tb.denom / 1e6); trow(g_frame, "@submit_ms", sub * tb.numer / tb.denom / 1e6);
        tpush(4, rec * tb.numer / tb.denom / 1e6); tpush(5, sub * tb.numer / tb.denom / 1e6);
    }
    if (prev_in) {
        double fr = (t0 - prev_in) / 1e6, wk = (t0 - prev_out) / 1e6, nv = atomic_exchange(&g_nvn_ns, 0) / 1e6;
        tpush(0, fr); tpush(1, wk); tpush(2, nv);
        trow(f, "@frame_ms", fr); trow(f, "@work_ms", wk); trow(f, "@nvn_ms", nv);
        // 慢帧归因（只在开 DM_NVN_TSV 时取）：本帧出图线程 CPU 时间（换线程记 −1）、进程全部线程 CPU、取图等待按帧累计、出图调用时刻（与 GPU / 上屏时刻同基：进程起点 ms）
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
        // 分账：本帧提交线程同步创建数 + 与 UIMSG 同一时钟的时刻（离线按读取画面区间把帧分成 前端 / 读取 / 局内）
        // Accounting: number of synchronous creations on the submit thread this frame + a timestamp on the same clock as UIMSG (offline, frames are split into front end / loading / in-run by the loading screen intervals)
        static uint64_t plib, ppso; uint64_t lib_, pso_; mtl_create_stats(&lib_, &pso_);
        trow(f, "@lib_new", (double)(lib_ - plib)); trow(f, "@pso_new", (double)(pso_ - ppso));
        {   // 长帧日志：本帧各项先记进环，等 8 帧后 GPU 时间到了再决定打不打（见下） | long-frame log: this frame's items are first recorded into the ring; whether to print is decided 8 frames later, once the GPU time has arrived (see below)
            static uint64_t plns, ppns, pup, prup, pub; uint64_t lns, pns, up, rup, ub; mtl_create_ns(&lns, &pns); mtl_pkg_stats(&up, &rup); ub = mtl_upload_bytes();
            lf_rec *r = &g_lf[f % LFN]; r->f = f; r->fr = fr; r->nv = nv; r->ls = g_nvn_loadscreen_up ? g_nvn_loadscreen_up() : 0; r->t_s = (now_ns() - g_t0_ns) / 1e9; r->pso = (unsigned)(pso_ - ppso); r->lib = (unsigned)(lib_ - plib);
            r->make_ms = ((lns - plns) + (pns - ppns)) / 1e6; r->up = (unsigned)(up - pup); r->rup = (unsigned)(rup - prup); r->up_mb = (ub - pub) / 1048576.0; r->up_ms = up_ms;
            plns = lns; ppns = pns; pup = up; prup = rup; pub = ub;
        }
        plib = lib_; ppso = pso_;
        trow(f, "@t_s", (now_ns() - g_t0_ns) / 1e9);
        trow(f, "@drawable_ms", mtl_drawable_ns_take() / 1e6);
        trow(f, "@sub_wall_ms", atomic_exchange(&g_sub_wall_ns, 0) / 1e6); trow(f, "@sub_cpu_ms", atomic_exchange(&g_sub_cpu_ns, 0) / 1e6); trow(f, "@sub_instr", (double)atomic_exchange(&g_sub_instr, 0));
    } else atomic_store(&g_nvn_ns, 0);
    g_acq_wait_ns = g_pq_wait_ns = 0;
    if (f >= 8) { double g = mtl_gpu_ms(f - 8); if (g >= 0) { tpush(3, g); trow(f - 8, "@gpu_ms", g); g_hr[g_hn++ % HRN] = g; }   // GPU 时间滞后 8 帧取（已完成） | GPU time is taken 8 frames late (already completed)
        { lf_rec *r = &g_lf[(f - 8) % LFN];   // 长帧日志（缺省开，DM_LONGFRAME=0 关）：整帧 > 20 ms 或 GPU > 16.7 ms、且明显高于近况，才打一行 | long-frame log (on by default, DM_LONGFRAME=0 turns it off): a line is printed only when the whole frame > 20 ms or GPU > 16.7 ms, and it is clearly above recent conditions
          static int on = -1; if (on < 0) { const char *e = getenv("DM_LONGFRAME"); on = !(e && !strcmp(e, "0")); }
          // 「长」是相对近况说的：稳定跑在 30 帧（菜单 / 无窗口宿主）或持续过载时每帧都 > 20 ms，那不是长帧，不刷屏——要比近 16 帧的滑动均值高出四成才算
          // "Long" is relative to recent conditions: when running steadily at 30 fps (menus / windowless host) or under sustained overload, every frame is > 20 ms; those are not long frames and must not flood the log - a frame has to be 40% above the sliding mean of the last 16 frames to count
          static double ef = 16.7, eg = 8; int lf = r->fr > 20.0 && r->fr > 1.4 * ef, lg = g > 16.7 && g > 1.3 * eg;
          if (r->f == f - 8) { ef += ((r->fr < 3 * ef ? r->fr : 3 * ef) - ef) / 16; eg += ((g < 3 * eg ? g : 3 * eg) - eg) / 16; }
          if (on && r->f == f - 8 && f - 8 > 0 && (lf || lg)) { char b[512];
              snprintf(b, sizeof b, "帧 %llu（%.2f s）整帧 %.1f ms，提交+出图 %.1f ms，GPU %.1f ms；新建管线 %u 条 + 着色器库 %u 个共 %.1f ms；上传贴图 首传 %u 张 + 重传 %u 次 %.1f MB / %.1f ms",
                       (unsigned long long)r->f, r->t_s, r->fr, r->nv, g, r->pso, r->lib, r->make_ms, r->up, r->rup, r->up_mb, r->up_ms);
              // 可见性：长帧「晚出来」的那段时间屏幕上停着的是上一帧；上一帧若是读取画面（或本帧自己就是），玩家看到的只是读取画面多停了一下，没有运动被打断
              // Visibility: during the time a long frame "comes out late", what stays on screen is the previous frame; if the previous frame was a loading screen (or this frame itself is one), all the player sees is the loading screen staying a bit longer, with no motion interrupted
              const lf_rec *pv = &g_lf[(f - 9) % LFN]; int hidden = r->ls || (f >= 9 && pv->f == f - 9 && pv->ls);
              if (hidden) strlcat(b, "；接读取画面，不可见", sizeof b);
              dm_longframe_note(b); } }
        uint64_t g0, g1; if (frame_rec() && mtl_gpu_span(f - 8, &g0, &g1)) { trow(f - 8, "@gpu_beg_ms", (g0 - g_t0_ns) / 1e6); trow(f - 8, "@gpu_end_ms", (g1 - g_t0_ns) / 1e6); } }
    prev_in = t0; prev_pass = ps; prev_draw = dr;
    dump_frame();
    if (g_frame % TWIN == 0) tsummary(g_frame);
    if (call_stats_on() && g_frame % 300 == 0) {
        static uint64_t pd; uint64_t ps2, dr2; mtl_stats(&ps2, &dr2); uint64_t all = dr2 - pd; pd = dr2;
        LOG("[nvn] 调用列表 近 300 帧：每帧绘制 %.0f，其中来自被调用列表 %.0f（%.0f%%），与上一帧完全相同的列表里 %.0f（%.0f%%）；每帧调用 %.0f 次 / %.0f KB，相同的 %.0f 次 / %.0f KB",
            all / 300.0, g_cs_draws / 300.0, all ? 100.0 * g_cs_draws / all : 0, g_cs_same_draws / 300.0, all ? 100.0 * g_cs_same_draws / all : 0,
            g_cs_calls / 300.0, g_cs_bytes / 300.0 / 1024, g_cs_same_calls / 300.0, g_cs_same_bytes / 300.0 / 1024);
        g_cs_calls = g_cs_draws = g_cs_same_calls = g_cs_same_draws = g_cs_bytes = g_cs_same_bytes = 0;
    }
    if (!async_replay()) mtl_set_frame(g_frame);
    else if (g_frame % 300 == 0) { uint64_t d = atomic_exchange(&g_x_drains, 0), dn = atomic_exchange(&g_x_drain_ns, 0);
        pthread_mutex_lock(&g_xq_mu); uint64_t q = g_xq_in - g_xq_done; pthread_mutex_unlock(&g_xq_mu);
        LOG("[nvn] 翻译线程 至帧 %llu：排空等待 %llu 次 / %.1f ms（近 300 帧），队列中 %llu 项", (unsigned long long)g_frame, (unsigned long long)d, dn / 1e6, (unsigned long long)q); }
    // 真机语义：出图只入队、立即返回；本帧显示时刻按节拍器排，该贴图到「显示时刻 + 一个周期」才可再获取（阻塞在 AcquireTexture）
    // Real-device semantics: present only enqueues and returns immediately; this frame's display time is scheduled by the pacer, and the texture can be acquired again only at "display time + one period" (blocking in AcquireTexture)
    uint64_t period = present_period(o), now = dm_detclock_enabled() ? guest_ns() : now_ns();
    nvn_pacer pc = {o->pace};
    uint64_t slot = now + nvn_pace(&pc, now, period);
    o->pace = pc.target;
    if (idx >= 0 && idx < 4) o->avail[idx] = period ? slot + period : 0;
    static uint64_t prev_slot; if (prev_slot && period) trow(f, "@display_ms", (slot - prev_slot) / 1e6); prev_slot = slot;   // 屏幕上相邻两帧的显示间隔（掉帧看这个） | display interval between two adjacent frames on screen (this is the one to look at for dropped frames)
    // 实际呈现间隔（窗口模式）：滞后 8 次出图取（呈现回调已到），第 n 次与第 n−1 次之差；−1 = 被丢弃
    // Actual presentation interval (windowed mode): taken 8 presents late (the presented callback has arrived by then), as the difference between the n-th and the (n−1)-th; −1 = discarded
    if (o->presents > 9) { uint64_t n = o->presents - 8; double a_ = mtl_presented_time(n), b_ = mtl_presented_time(n - 1);
        if (a_ < 0) trow(f - 8, "@presented_ms", -1); else if (a_ > 0 && b_ > 0) trow(f - 8, "@presented_ms", (a_ - b_) * 1e3);
        g_pres_iv[g_presn++ % TWIN] = a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0);
        { double iv_ = a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0); mtl_ub_present(iv_); }   // 同进程交替测量（DM_NVN_UB）按相统计上屏间隔；没开时只是一次原子读 | same-process alternating measurement (DM_NVN_UB) tallies on-screen present intervals per phase; when off it is just one atomic read
        dm_perf_present(a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0));   // 上屏间隔另存一圈，tsummary 每 300 帧报掉帧（M1 判据） | on-screen present intervals are also kept in a separate ring; tsummary reports dropped frames every 300 frames (M1 criterion)
        if (a_ > 0) trow(f - 8, "@pres_at_ms", a_ * 1e3 - g_t0_ns / 1e6); }   // 上屏时刻（进程起点 ms；presentedTime 与 mach_absolute_time 同基） | on-screen present time (ms since process start; presentedTime shares a time base with mach_absolute_time)
    if (f >= 8 && slow_on()) slow_emit(f - 8);
    prev_out = now_ns();
    totk_tl(TL_PRES_OUT, 0, 0);
}
