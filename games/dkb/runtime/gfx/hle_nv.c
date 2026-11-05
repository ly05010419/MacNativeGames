// nv:: (the GPU driver glue layer beneath NVN: creates transfer memory, connects to the nvdrv service). NVN has been replaced wholesale with a host null device, so this layer has nothing to do.
// The allocator callbacks are recorded in the host global g_nv_alloc; the graphics layer takes them from here later when it needs to allocate memory with the game's allocator.
#include "../host/hle.h"

typedef void *(*nv_alloc_fn)(unsigned long size, unsigned long align, void *user);
typedef void (*nv_free_fn)(void *p, void *user);
typedef void *(*nv_realloc_fn)(void *p, unsigned long size, void *user);
struct nv_allocator { nv_alloc_fn alloc; nv_free_fn free; nv_realloc_fn realloc; void *user; };
struct nv_allocator g_nv_alloc, g_nv_devtools_alloc;
// The graphics pool the game installs (nv::MemoryInfo reports on it). Nothing is allocated from it on the host: the graphics layer allocates its own Metal objects
static struct { void *mem; unsigned long size; int inited; } g_nv_pool;

#define WHY "NVN has been replaced wholesale with a host null device, so the driver glue layer has nothing to do"
HLE_FORCE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "; allocator recorded in g_nv_alloc",
          void, h_nv_SetAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_alloc = (struct nv_allocator){a, f, r, u}; }
HLE_FORCE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", WHY "; allocator recorded in g_nv_devtools_alloc",
          void, h_nv_SetDevAlloc, nv_alloc_fn a, nv_free_fn f, nv_realloc_fn r, void *u) { g_nv_devtools_alloc = (struct nv_allocator){a, f, r, u}; }
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm", WHY "; pool recorded for nv::GetGraphicsMemoryInfo", void, h_nv_Init, void *mem, unsigned long size) {
    g_nv_pool = (typeof(g_nv_pool)){mem, size, 1};
    LOG("nv::InitializeGraphics: pool %.2f MB, recorded (the host's graphics layer allocates its own)", size / 1048576.0);
}
HLE_FORCE("_ZN2nv16FinalizeGraphicsEv", WHY, void, h_nv_Fin, void) { g_nv_pool = (typeof(g_nv_pool)){0}; }
// nv::InitCheck aborts unless graphics are initialised. Its own implementation tests the SDK's flag, which nv::InitializeGraphics sets, and that runs on the host instead:
// Donkey Kong Bananza (SDK 20.5.19 Ounce) calls it from main+0xe19d04 once gameplay starts, and aborted at sdk+0x57582c. The host's record of the initialisation answers it
HLE_FORCE("_ZN2nv9InitCheckEv", WHY "; nv::InitializeGraphics recorded on the host", void, h_nv_InitCheck, void) {
    if (!g_nv_pool.inited) die("nv::InitCheck: graphics not initialised (nv::InitializeGraphics was not called)");
}
// nv::MemoryInfo is 64-bit counters, the SDK fills them from sign-extended 32-bit driver counters. Its size follows the SDK: 0x20 bytes (four counters) on SDK 10 and older,
// 0x40 bytes from SDK 11 on (where the SDK zeroes all 0x40 when nv is not initialised). Writing 0x40 on an old SDK overran the caller's record by 32 bytes (Hyrule Warriors:
// Definitive Edition, SDK 3.5.1). Its own implementation reads the NV driver object, which the host never creates, so it faults: Well Dweller crashed there (sdk+0x4656c8,
// x0 = 0) while reporting video memory at startup. Report the pool the game installed, with nothing used from it
HLE_FORCE("_ZN2nv21GetGraphicsMemoryInfoEPNS_10MemoryInfoE", WHY "; pool figures from nv::InitializeGraphics, no driver statistics", void, h_nv_MemInfo, uint64_t *out) {
    memset(out, 0, DM_SDK_OLD(dm_sdk_major()) ? 0x20 : 0x40);
    if (g_nv_pool.inited) { out[0] = g_nv_pool.size; out[2] = g_nv_pool.size; }   // [0] total, [1] used (0), [2] free: within the four counters of both sizes
}
HLE_FORCE("_ZN2nv22DumpGraphicsMemoryInfoEv", WHY "; the dump goes to the host log", void, h_nv_Dump, void) {
    LOG("nv::DumpGraphicsMemoryInfo: pool %.2f MB, 0 used (host graphics layer)", g_nv_pool.size / 1048576.0);
}
