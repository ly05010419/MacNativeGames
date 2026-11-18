// Unreal Engine's allocator on a host malloc zone. Unreal Engine 4 / 5's FMallocBinned3 reserves 37 GB of address space for its pools (74 of 512 MB), which an iPhone cannot give (about
// 5 GB without the extended virtual addressing entitlement), and its pools are too uneven to shrink. The rewriter finds it in any main without a build ID (dm/tools/ue_malloc.py: its name
// string, getter, vtable, constructor and the constructor's one call), cuts the call, puts observation gate OBS_UE_MALLOC on it and names the vtable and its slot layout in the fixups table
// (UE_MALLOC_VT, UE_MALLOC_SLOT). At the gate the host puts this vtable in the object instead (x0 there: made by GCreateMalloc, or a bss object in nninitStartup). Every FMemory allocation
// reaches it through the object (FPlatformMemory::BaseAllocator returns it, and the thread-local storage allocator calls its Malloc / Free slots), and the guest's own trivial functions stay
// in the slots that do nothing. The slot of the name getter (GetDescriptiveName) names the layout:
// Unreal Engine 5 (Octopath Traveler 0; the getter at slot 22: the engine's FMalloc interface, measured from the vtable and the thread-local callbacks): 0-1 destructors, 2-5 FExec, 6 Malloc, 7 TryMalloc, 8 Realloc, 9 TryRealloc, 10 Free, 11 QuantizeSize, 12 GetAllocationSize, 13 Trim, 14 SetupTLSCachesOnCurrentThread,
// 15 ClearAndDisableTLSCachesOnCurrentThread, 16 InitializeStatsMetadata, 17 UpdateStats, 18 GetAllocatorStats, 19 DumpAllocatorStats, 20 IsInternallyThreadSafe, 21 ValidateHeap,
// 22 GetDescriptiveName, 23-25 OnMallocInitialized / OnPreFork / OnPostFork (the thread-local callbacks main+0x12e80 / main+0x12ec0 call +0x30 and +0x50).
// Unreal Engine 4.26 and 4.27 (DRAGON QUEST VII Reimagined, Pikmin 4, OCTOPATH TRAVELER II; the getter at slot 19) have one FExec slot and no fork callbacks: 0-1 destructors, 2 Exec, 3 Malloc, 4 TryMalloc, 5 Realloc, 6 TryRealloc, 7 Free, 8 QuantizeSize,
// 9 GetAllocationSize, 10 Trim, 11 SetupTLSCachesOnCurrentThread, 12 ClearAndDisableTLSCachesOnCurrentThread, 13 InitializeStatsMetadata, 14 UpdateStats, 15 GetAllocatorStats,
// 16 DumpAllocatorStats, 17 IsInternallyThreadSafe, 18 ValidateHeap, 19 GetDescriptiveName, 20 OnMallocInitialized (main+0x5e4c0e8: TryMalloc / TryRealloc call +0x18 / +0x28, GCreateMalloc
// calls +0x88 and +0xa0; Free is main+0x10, which reads the pool base as GetAllocationSize does). OCTOPATH TRAVELER II's vtable has no OnMallocInitialized, so the 21st word copied
// is the next vtable's offset to top, which nothing calls
// Unreal Engine 5.6 (FINAL FANTASY RESONANCE; the getter at slot 26) adds MallocZeroed / TryMallocZeroed after Free and two TLS cache calls after the setup, and two cached-memory queries at the end: 0-1 destructors,
// 2-5 FExec, 6 Malloc, 7 TryMalloc, 8 Realloc, 9 TryRealloc, 10 Free, 11 MallocZeroed, 12 TryMallocZeroed, 13 QuantizeSize, 14 GetAllocationSize, 15 Trim, 16 SetupTLSCachesOnCurrentThread,
// 17 MarkTLSCachesAsUsedOnCurrentThread, 18 MarkTLSCachesAsUnusedOnCurrentThread, 19 ClearAndDisableTLSCachesOnCurrentThread, 20 InitializeStatsMetadata, 21 UpdateStats, 22 GetAllocatorStats,
// 23 DumpAllocatorStats, 24 IsInternallyThreadSafe, 25 ValidateHeap, 26 GetDescriptiveName, 27-29 OnMallocInitialized / OnPreFork / OnPostFork, 30-31 the free cached memory sizes (main+0x67df800;
// the base FMalloc vtable main+0x67dff48 has 0 for the three pure slots 6 / 8 / 10, its 7 / 9 call +0x30 / +0x40, and its 11 and 12 are one function: +0x30, then memset). The zeroed slots
// keep the guest's function, which calls Malloc through the vtable
// Unreal Engine 3 (Batman: Arkham Knight, with a backported binned allocator) takes UE3's 32-bit sizes (DWORD Count / Alignment) and has no TryMalloc / TryRealloc: 0-1 destructors, 2 Exec,
// 3 QuantizeSize, 4 Malloc, 5 Realloc, 6 Free (call sites by vtable offset: 4 / 5 / 6 at 8090 / 8423 / 20308; malloc main+0xb2e3d0 calls +0x20 with alignment 16), 13 the TLS cache flush,
// 14 IsInternallyThreadSafe (1), 20 GetAllocationSize (stores a 32-bit size), 21 the TLS cache setup, 22 Trim (main+0x329a408). Its object is in the bss and its constructor is inlined into
// the GMalloc setup, which the cut skips (the game's patches.py). It has no Binned3 name to find, so it keeps a row of fixed addresses, which runs only on the main build it was found in (AGENTS.md)
#include "hle.h"
#include <malloc/malloc.h>
#include <mach/mach.h>
#include <stdatomic.h>

// Slot layouts: which slots the host fills and which do nothing (up to 6; 0 ends the list); the others keep the guest's own functions
enum { UE5, UE4_27, UE5_6, UE3 };
// name: the slot of the name getter, which the rewriter reports (UE_MALLOC_SLOT); size32: the size arguments and results are 32-bit (UE3); try_malloc / try_realloc 0: no such slot
static const struct { int name, nslot, malloc_, try_malloc, realloc_, try_realloc, free_, quantize, get_size, nothing[6]; bool size32; } k_lay[] = {
    [UE5] = { 22, 26, 6, 7, 8, 9, 10, 11, 12, { 13, 14, 15, 19 } },      // Trim, the TLS cache setup and teardown, DumpAllocatorStats
    [UE4_27] = { 19, 21, 3, 4, 5, 6, 7, 8, 9, { 10, 11, 12, 16 } },
    [UE5_6] = { 26, 32, 6, 7, 8, 9, 10, 13, 14, { 15, 16, 17, 18, 19, 23 } },   // Trim, the four TLS cache calls, DumpAllocatorStats
    [UE3] = { -1, 25, 4, 0, 5, 0, 6, 3, 20, { 13, 21, 22 }, true },   // the TLS cache flush and setup, Trim
};
static const struct { const char *build, *what; int layout; uint64_t obj, vtable, cut; uint32_t cut_insn; } k_ue[] = {   // cut_insn: the instruction the cut leaves
    // Batman: Arkham Knight 1.0.5: the allocator object (main+0x51818c0, bss), its vtable (main+0x329a408, 25 slots), the cut (main+0x192ec14: a branch past the inlined constructor)
    { "feae83dc9e9fb34c77d0a62f1fa9ae9f", "Batman: Arkham Knight 1.0.5", UE3, 0x51818c0, 0x329a408, 0x192ec14, 0x140004a4 },
};
enum { NSLOT = 32 };

static malloc_zone_t *g_zone;
static _Atomic int64_t g_used, g_peak;   // bytes in use (usable sizes) and their high-water mark, for the [mem] line (one per 256 MB of new peak)
static void used(int64_t d) {
    int64_t after = atomic_fetch_add(&g_used, d) + d, peak = atomic_load(&g_peak);
    while (after > peak && !atomic_compare_exchange_weak(&g_peak, &peak, after)) {}
    if (after > peak && after >> 28 != peak >> 28) LOG("[mem] Unreal heap (host malloc zone) up to %.2f GB in use", after / 1073741824.0);
}
static size_t align_of(uint32_t a) { return a > 16 ? a : 16; }   // 0 = DEFAULT_ALIGNMENT, 16 bytes as Binned3 gives
static void *alloc(size_t n, uint32_t a) {
    void *p = align_of(a) == 16 ? malloc_zone_malloc(g_zone, n ? n : 1) : malloc_zone_memalign(g_zone, align_of(a), n ? n : 1);
    if (p) used((int64_t)malloc_size(p));
    return p;
}
static void release(void *p) { if (p) { used(-(int64_t)malloc_size(p)); malloc_zone_free(g_zone, p); } }
static void *realloc_aligned(void *p, size_t n, uint32_t a) {
    if (!p) return alloc(n, a);
    if (!n) { release(p); return NULL; }
    size_t old = malloc_size(p);
    if (align_of(a) == 16) { void *q = malloc_zone_realloc(g_zone, p, n); if (q) used((int64_t)malloc_size(q) - (int64_t)old); return q; }
    if (!((uintptr_t)p & (align_of(a) - 1)) && old >= n) return p;
    void *q = alloc(n, a); if (!q) return NULL;
    memcpy(q, p, old < n ? old : n); release(p); return q;
}

static void *m_Malloc(void *self, size_t n, uint32_t a) {
    (void)self; void *p = alloc(n, a);
    if (!p) die("Unreal heap: out of memory (%zu bytes, alignment %u, %.2f GB in use)", n, a, atomic_load(&g_used) / 1073741824.0);
    return p;
}
static void *m_TryMalloc(void *self, size_t n, uint32_t a) { (void)self; return alloc(n, a); }
static void *m_Realloc(void *self, void *p, size_t n, uint32_t a) {
    (void)self; void *q = realloc_aligned(p, n, a);
    if (!q && n) die("Unreal heap: out of memory in Realloc (%zu bytes, alignment %u, %.2f GB in use)", n, a, atomic_load(&g_used) / 1073741824.0);
    return q;
}
static void *m_TryRealloc(void *self, void *p, size_t n, uint32_t a) { (void)self; return realloc_aligned(p, n, a); }
static void m_Free(void *self, void *p) { (void)self; release(p); }
static size_t m_QuantizeSize(void *self, size_t n, uint32_t a) { (void)self; (void)a; return malloc_good_size(n); }
static bool m_GetAllocationSize(void *self, void *p, size_t *out) { (void)self; if (!p) return false; *out = malloc_size(p); return true; }
static void *m_Malloc32(void *self, uint32_t n, uint32_t a) { return m_Malloc(self, n, a); }
static void *m_Realloc32(void *self, void *p, uint32_t n, uint32_t a) { return m_Realloc(self, p, n, a); }
static uint32_t m_QuantizeSize32(void *self, uint32_t n, uint32_t a) { (void)self; (void)a; size_t q = malloc_good_size(n); return q > UINT32_MAX ? n : (uint32_t)q; }
static bool m_GetAllocationSize32(void *self, void *p, uint32_t *out) { (void)self; if (!p) return false; *out = (uint32_t)malloc_size(p); return true; }
static void m_Nothing(void *self) { (void)self; }   // Trim (its bool argument is ignored), the thread-local cache setup and teardown, DumpAllocatorStats
static void *g_vt[NSLOT];
static const char *g_what;

// Observation gate OBS_UE_MALLOC, on the cut constructor call: the object in the register the gate number's upper bits name (x0), before anything calls it
static void obj_made(uint64_t *regs, int param) {
    *(void **)(uintptr_t)regs[param] = g_vt;
    static int once; if (!once++) LOG("%s: Unreal's allocator (FMallocBinned3 at %#llx) runs on a host malloc zone", g_what, (unsigned long long)regs[param]);
}

// Whether [a, a + n) may be Unreal heap memory: with the host allocator installed, a range outside the guest block that the host has mapped readable and writable.
// Unreal allocates the stacks of the threads it creates with FMemory (OCTOPATH TRAVELER II's game thread: nnMain at main+0x544 mallocs it with 4 KB alignment; its
// FRunnableThreads take theirs from inside larger allocations, so a stack is not always a block start, and malloc has no cheap lookup from an interior pointer), and
// nn::os::CreateThread moves such a stack into the stack region with svcMapMemory, whose source must be backed memory (hle_svc.c map_source_backed): this heap lives
// outside the guest block, so the check refused it and the SDK aborted in MapAliasStack before the game started (the caller has already checked the guest block's own regions)
int dm_ue_heap_owns(uint64_t a, uint64_t n) {
    if (!g_zone || !a || !n) return 0;
    int dm_guest_as_contains(uintptr_t a);   // hle_svc.c
    if (dm_guest_as_contains((uintptr_t)a) || dm_guest_as_contains((uintptr_t)(a + n - 1))) return 0;   // inside the guest block: not this heap
    vm_address_t at = (vm_address_t)a; vm_size_t sz = 0; vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj;
    if (vm_region_64(mach_task_self(), &at, &sz, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) return 0;   // vm_region_64: iOS has no mach_vm.h
    return at <= a && a + n <= at + sz && (info.protection & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE);
}

// The host's vtable over the guest's (the trivial slots stay the guest's), and the zone behind it
static void fill(const uint8_t *vtable, int L, const char *what) {
    memcpy(g_vt, vtable, sizeof g_vt[0] * k_lay[L].nslot);
    bool s32 = k_lay[L].size32;
    g_vt[k_lay[L].malloc_] = s32 ? (void *)m_Malloc32 : (void *)m_Malloc; g_vt[k_lay[L].realloc_] = s32 ? (void *)m_Realloc32 : (void *)m_Realloc;
    if (k_lay[L].try_malloc) g_vt[k_lay[L].try_malloc] = (void *)m_TryMalloc;
    if (k_lay[L].try_realloc) g_vt[k_lay[L].try_realloc] = (void *)m_TryRealloc;
    g_vt[k_lay[L].free_] = (void *)m_Free;
    g_vt[k_lay[L].quantize] = s32 ? (void *)m_QuantizeSize32 : (void *)m_QuantizeSize; g_vt[k_lay[L].get_size] = s32 ? (void *)m_GetAllocationSize32 : (void *)m_GetAllocationSize;
    for (int s = 0; s < 6 && k_lay[L].nothing[s]; s++) g_vt[k_lay[L].nothing[s]] = (void *)m_Nothing;
    g_zone = malloc_create_zone(0, 0); malloc_set_zone_name(g_zone, "guest Unreal heap"); g_what = what;
}

// Called once the modules are loaded and relocated, before any guest code runs
void dm_ue_malloc_install(void) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        const module_t *m = &g_mod[i]; uint8_t *b = m->base;
        if (m->ue_malloc_vt) {   // found by the rewriter (ue_malloc.py)
            int L = -1;
            for (int k = 0; k < (int)(sizeof k_lay / sizeof k_lay[0]); k++) if (k_lay[k].name == (int)m->ue_malloc_slot) L = k;
            if (L < 0) { LOG("[RED] main: Unreal's FMallocBinned3 has its name getter at slot %u, a layout the host does not know: Unreal's own allocator runs on cut code", m->ue_malloc_slot); return; }
            if (!gate_obs_present(OBS_UE_MALLOC)) { LOG("[RED] main: the fixups name Unreal's FMallocBinned3 but main has no allocator gate: the allocator would have no vtable"); return; }
            static char what[64]; snprintf(what, sizeof what, "main %.8s", m->build_id);
            fill(b + m->ue_malloc_vt, L, what);
            gate_obs_set(OBS_UE_MALLOC, obj_made);
            return;
        }
        for (int k = 0; k < (int)(sizeof k_ue / sizeof k_ue[0]); k++) if (!strcmp(m->build_id, k_ue[k].build)) {
            if (*(uint32_t *)(b + k_ue[k].cut) != (k_ue[k].cut_insn ? k_ue[k].cut_insn : 0xd503201f)) { LOG("[RED] %s: main is rewritten without the allocator cut (its patches.py): Unreal's own allocator runs", k_ue[k].what); return; }
            fill(b + k_ue[k].vtable, k_ue[k].layout, k_ue[k].what);
            *(void **)(b + k_ue[k].obj) = g_vt;
            LOG("%s: Unreal's allocator (main+%#llx) runs on a host malloc zone", k_ue[k].what, (unsigned long long)k_ue[k].obj);
        }
    }
}
