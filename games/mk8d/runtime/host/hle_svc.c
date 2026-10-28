// Horizon kernel system calls (triggered by the SDK runtime once the SDK is loaded). Only what the P1 boot chain needs is implemented: memory layout + heap + query + debug output + timing.
// Register convention: inputs w1.. / outputs w0=Result, x1=value (GetInfo / SetHeapSize), consistent with the public Horizon ABI.
#include "dm.h"
#include <sys/mman.h>
#include <errno.h>
#include <unistd.h>
#include <unistd.h>
#include <sched.h>
#include <time.h>
#include <stdatomic.h>
#include <mach/mach.h>

// Reserve one whole block of guest address space, partitioned the Horizon way (address values are the real addresses returned by the host's mmap).
// It only takes address space, not memory.
// Size 5 GB: the game needs the first 1 GB (alias region, stacks) + one whole heap block (application memory pool of 3285 MiB, see APP_POOL below),
// about 4.2 GB in total; on iOS without the "extended virtual addressing" entitlement a single reservation can be at most 6 GB
// (read on a real device: 6 GB succeeds, 7 GB fails), so it cannot be made any wider --
// with an 8 GB reservation, builds from developer accounts that cannot sign that entitlement (free accounts) would exit at this step.
#define RESERVE DM_GAME_MEM_RESERVE   // SMBW: the heap needs only 1.47 GB, so the block stays at 5 GB (the total proven on a real device; a single 6 GB block can be reserved, but Metal etc. still need address space)
static uint8_t *g_as;              // ASLR region base = the whole block
static size_t g_reserve;           // its size: RESERVE, or more with a DM_ALIAS_SIZE alias region
static uint8_t *g_heap; static _Atomic size_t g_heap_cur; static size_t g_heap_cap;
// The address range lies entirely within the currently opened (readable and writable) part of the guest heap: such reads need no kernel trap to check, memcpy directly (used where guest memory is read every tick, such as diag.c tk_rd)
int dm_guest_heap_readable(uint64_t a, size_t n) { uint64_t b = (uint64_t)(uintptr_t)g_heap; size_t cur = g_heap_cur; return g_heap && a >= b && n <= cur && a - b <= cur - n; }
static uint8_t *g_alias; static size_t g_alias_cap;
static _Atomic uint64_t *g_alias_bits;   // alias region page bitmap (alias_pages)
static uint64_t *g_alias_exposed;   // one bit per host page ever opened; protected by g_map_lock
static uint8_t *g_stack; static size_t g_stack_cap;
// The address lies in the guest address block (every guest thread stack is here: the game's own heap or the stack region); the big-stack entry (hle_bigstack.c) uses it to decide whether to switch stacks
int dm_guest_as_contains(uintptr_t a) { uintptr_t b = (uintptr_t)g_as; return g_as && a >= b && a - b < g_reserve; }

static void *alias_windows_thread(void *arg);
static void *regions_thread(void *arg);
static void mem_init_once(void) {
    size_t pg = (size_t)getpagesize();
    if (RESERVE < 0x10000000 || DM_GAME_MEM_ALIAS > RESERVE - 0x10000000 ||
        DM_GAME_MEM_STACK > RESERVE - 0x10000000 - DM_GAME_MEM_ALIAS ||
        ((RESERVE | DM_GAME_MEM_ALIAS | DM_GAME_MEM_STACK) & (pg - 1)))
        die("guest address-space profile has invalid native boundaries");
    // DM_ALIAS_SIZE=<bytes>: an alias region of that size instead of the profile's, the heap and stacks as they are; the block grows with it. A Switch-sized one (64 GB) holds what
    // a game reserves unchanged, for measuring its address regions on the Mac (the NSP script's heap check); an iPhone has no room for more than the profile's
    size_t alias = DM_GAME_MEM_ALIAS; const char *as = getenv("DM_ALIAS_SIZE");
    if (as && *as) { alias = (size_t)strtoull(as, NULL, 0); if (alias < 0x10000000 || (alias & (pg - 1))) die("DM_ALIAS_SIZE %s: at least 256 MB, in whole host pages", as); }
    g_reserve = RESERVE - DM_GAME_MEM_ALIAS + alias;
    g_as = mmap(NULL, g_reserve, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_as == MAP_FAILED) die("reserve guest AS");
    // Fixed partition (all inside the reserved block): alias 2.875 GB, stack 256 MB, heap the remaining 1.625 GB.
    // In newer SDKs the AddressRegionManager always takes 0x3fe00000 (~1 GB) at the start of the alias region as its node area (sdk 0x134728) before handing out regions; with a 512 MB alias region the very first AllocateAddressRegion fails (0x1803).
    // SMBW also defines a 1792000 KB (1.75 GB) VirtualAddressMemory in HeapDef (System/HeapSize): it is subtracted from the memory pool before the heap is opened (SetHeapSize asks for only 0x5be00000),
    //   and resource loading then MapPhysicalMemory's into the alias region after the node area on demand. With a 1.25 GB alias region only about 256 MB was left there, and 3 MB-class layout archives (WipeCourseIn / WipeWarp)
    //   got no address, were skipped, and crashed on a null pointer when used (main +0x373e04). So alias = node area + 1.75 GB + slack; the heap needs only 1.47 GB, and 2.625 GB is plenty.
    // The alias and stack sizes come from the game profile (mem_alias / mem_stack), the heap takes the rest; Mario: alias 0xB8000000, stack 0x10000000 → stack at +0xC8000000, heap at +0xD8000000
    g_alias = g_as + 0x10000000; g_alias_cap = alias;
    g_stack = g_alias + g_alias_cap; g_stack_cap = DM_GAME_MEM_STACK;
    g_heap  = g_stack + g_stack_cap; g_heap_cap  = g_reserve - (size_t)(g_heap - g_as);
    if (!(g_alias_bits = calloc((g_alias_cap >> 18) + 1, sizeof *g_alias_bits))) die("alias region page bitmap calloc");
    if (!(g_alias_exposed = calloc(g_alias_cap / pg / 64 + 1, sizeof *g_alias_exposed))) die("alias exposure bitmap calloc");
    LOG("guest AS %p +%#llx  heap %p alias %p stack %p", g_as, (unsigned long long)g_reserve, g_heap, g_alias, g_stack);
    const char *aw = getenv("DM_ALIAS_WINDOWS");
    if (aw && atoi(aw) > 0) { pthread_t th; if (!pthread_create(&th, NULL, alias_windows_thread, (void *)(uintptr_t)atoi(aw))) pthread_detach(th); }
    const char *rf = getenv("DM_REGIONS");
    if (rf && *rf) { pthread_t th; if (!pthread_create(&th, NULL, regions_thread, (void *)rf)) pthread_detach(th); }
}
static void mem_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, mem_init_once);
}

// Application memory pool = the 3285 MB the Switch (4 GB model) gives applications. At startup (main+0x1b66160) the game sets the whole total reported by QueryMemoryInfo as the heap, retrying with 2 MB less on failure,
// and hands the whole block to its own StandardAllocator (main+0x32137a0) -- so heap size = pool − image footprint, it has only as much as the host reports.
// Previously 3 GB was reported here and SetHeapSize never failed: the heap was about 150 MB smaller than on the real device, at the run entry peak the main heap had only about 18 MB left, and under high local load a timing shift made a 128 KB request return null,
// which the guest writes to without a null check (CRASH main+0x1b36138, re-verified after merge with inv_tap).
#define APP_POOL DM_GAME_APP_POOL   // game profile app_pool (Mario 0xCD500000)
static size_t image_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) if (!g_mod[i].nro) n += g_mod[i].size;   // an NRO lives in memory the game allocated (its image and bss buffers)
    return (n + 0x1fffff) & ~(size_t)0x1fffff; }
// Giant bss (modules with the patch area before the base, patch_nso.py FAR): the image bytes beyond the file part. Used memory was reported as "heap + 64 MB", where the 64 MB covers ordinary modules' code and data (Mario's two modules are about 74 MB together,
// and the heap it opens = pool − 64 MB − 1.75 GB virtual address memory − 1 MB = 0x5be00000, as on hardware); Metroid Dread's main has about 1.8 GB of bss, the engine's static heap, which the Switch counts as used;
// without it the game asks for pool − 64 MB = 3.15 GB of heap
// Like image_bytes, exclude NROs: their buffers already belong to the game's heap. Counting every offline-mapped NRO's bss made FFX/X-2's used memory exceed its pool before any LoadModule.
static size_t big_bss_bytes(void) { size_t n = 0; for (int i = 0; i < g_nmod; i++) if (!g_mod[i].nro && g_mod[i].patch_off < 0) n += g_mod[i].size - g_mod[i].file_image_len; return n; }

// For memory accounting (memstat.m): the range of the whole guest address space block (base = NULL when not initialized)
void svc_guest_range(uint8_t **base, size_t *size) { *base = g_as; *size = g_reserve; }

enum { I_CoreMask=0, I_PriorityMask=1, I_AliasAddr=2, I_AliasSize=3, I_HeapAddr=4, I_HeapSize=5,
       I_TotalMem=6, I_UsedMem=7, I_DebuggerAttached=8, I_ResourceLimit=9, I_IdleTick=10, I_RandomEntropy=11,
       I_AslrAddr=12, I_AslrSize=13, I_StackAddr=14, I_StackSize=15, I_SysResTotal=16, I_SysResUsed=17,
       I_ProgramId=18, I_InitProcIdRange=19, I_UserExcAddr=20, I_TotalNonSys=21, I_UsedNonSys=22, I_IsApplication=23, I_FreeThreadCount=24 };

static uint8_t g_seen[256];

// Stack destinations and their borrowed sources. Physical alias commits live in
// g_alias_bits. One lock covers memory validation, protections, copying, budgets
// and publication; QueryMemory and used-memory queries take the same snapshot.
// g_map_lock is held over a whole lookup + check + change (and over QueryMemory's reads): map_find's index is only valid under it, since map_del moves the last entry into the freed slot.
// The lock used to cover only the add and the delete, so two threads unmapping at once (stacks of exiting threads) could delete through a stale index and drop another live mapping
#define MAX_MAP 4096
static struct { uint64_t base, size, src; uint32_t state; } g_map[MAX_MAP];
static int g_nmap;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;
static int map_find(uint64_t a, uint64_t n) {       // returns a 1-based index, 0 = no overlap; caller holds g_map_lock
    for (int i = 0; i < g_nmap; i++) if (a < g_map[i].base + g_map[i].size && g_map[i].base < a + n) return i + 1;
    return 0;
}
static void map_add(uint64_t b, uint64_t n, uint64_t src, uint32_t st) {   // caller holds g_map_lock
    if (g_nmap >= MAX_MAP) die("mapping table full");
    g_map[g_nmap++] = (typeof(g_map[0])){b, n, src, st};
}
static void map_del(int i) { g_map[i] = g_map[--g_nmap]; }   // caller holds g_map_lock

// One bit per 4 KB page of the alias region (mapped = 1): Map sets bits, Unmap clears them; returns the number of pages whose state really changed (signed: positive for Map, negative for Unmap)
static int64_t alias_pages(uint64_t off, uint64_t n, int map) {
    int64_t ch = 0;
    for (uint64_t p = off >> 12, e = (off + n + 0xFFF) >> 12; p < e; ) {
        uint64_t sh = p & 63, k = 64 - sh < e - p ? 64 - sh : e - p, m = (k == 64 ? ~0ull : (1ull << k) - 1) << sh;
        uint64_t old = map ? atomic_fetch_or(&g_alias_bits[p >> 6], m) : atomic_fetch_and(&g_alias_bits[p >> 6], ~m);
        ch += __builtin_popcountll((map ? ~old : old) & m);
        p += k;
    }
    return map ? ch : -ch;
}
// Pages of the range not mapped yet (the bitmap is left alone)
static int64_t alias_unmapped(uint64_t off, uint64_t n) {
    int64_t c = 0;
    for (uint64_t p = off >> 12, e = (off + n + 0xFFF) >> 12; p < e; ) {
        uint64_t sh = p & 63, k = 64 - sh < e - p ? 64 - sh : e - p, m = (k == 64 ? ~0ull : (1ull << k) - 1) << sh;
        c += __builtin_popcountll(~atomic_load(&g_alias_bits[p >> 6]) & m);
        p += k;
    }
    return c;
}
static _Atomic int64_t g_alias_mapped;   // bytes mapped in the alias region (page bitmap)

// Word-at-a-time boundaries of the bitmap run containing p (guest page index).
static uint64_t alias_mask(unsigned n) { return n == 64 ? UINT64_MAX : (UINT64_C(1) << n) - 1; }
static uint64_t alias_run_end(uint64_t p, uint64_t end, int mapped) {
    for (uint64_t first = p + 1; first < end;) {
        unsigned shift = first & 63, n = (unsigned)(end - first < 64 - shift ? end - first : 64 - shift);
        uint64_t bits = atomic_load(&g_alias_bits[first >> 6]);
        uint64_t different = (mapped ? ~bits : bits) & (alias_mask(n) << shift);
        if (different) return (first & ~UINT64_C(63)) + __builtin_ctzll(different);
        first += n;
    }
    return end;
}
static void alias_query(uint64_t addr, uint64_t *base, uint64_t *size, int *mapped) {
    uint64_t p = (addr - (uintptr_t)g_alias) >> 12, lo = p;
    *mapped = (atomic_load(&g_alias_bits[p >> 6]) >> (p & 63)) & 1;
    while (lo) {
        uint64_t first = (lo - 1) & ~UINT64_C(63);
        uint64_t bits = atomic_load(&g_alias_bits[first >> 6]);
        uint64_t different = (*mapped ? ~bits : bits) & alias_mask((unsigned)(lo - first));
        if (different) { lo = first + 64 - __builtin_clzll(different); break; }
        lo = first;
    }
    uint64_t hi = alias_run_end(p, g_alias_cap >> 12, *mapped);
    *base = (uintptr_t)g_alias + (lo << 12); *size = (hi - lo) << 12;
}
// Initialize holes using the OLD commit bitmap. Fresh anonymous host pages stay
// lazy; after any exposure, even an untouched 4 KiB sibling may hold stale bytes.
static void alias_initialize(uint64_t off, size_t size) {
    size_t pg = (size_t)getpagesize(), ratio = pg >> 12;
    uint64_t end = (off + size) >> 12;
    for (uint64_t p = off >> 12; p < end;) {
        int mapped = (atomic_load(&g_alias_bits[p >> 6]) >> (p & 63)) & 1;
        uint64_t next = alias_run_end(p, end, mapped);
        if (!mapped) {
            uint64_t clear = p;
            for (uint64_t q = p; q < next;) {
                uint64_t host = q / ratio, stop = (host + 1) * ratio;
                if (stop > next) stop = next;
                if (!(g_alias_exposed[host >> 6] & (UINT64_C(1) << (host & 63)))) {
                    if (clear < q) memset(g_alias + (clear << 12), 0, (q - clear) << 12);
                    clear = stop;
                }
                q = stop;
            }
            if (clear < next) memset(g_alias + (clear << 12), 0, (next - clear) << 12);
        }
        p = next;
    }
    for (uint64_t h = off / pg, last = (off + size - 1) / pg; h <= last; ++h)
        g_alias_exposed[h >> 6] |= UINT64_C(1) << (h & 63);
}

static int memory_contains(uint64_t base, uint64_t capacity, uint64_t addr, uint64_t size) {
    return addr >= base && size <= capacity && addr - base <= capacity - size;
}
static uint32_t map_arguments(uint64_t dst, uint64_t src, uint64_t size) {
    if ((dst | src) & 0xfff) return 0xCC01;
    if (!size || (size & 0xfff)) return 0xCA01;
    if (size > UINT64_MAX - src || size > UINT64_MAX - dst) return 0xD401;
    return memory_contains((uintptr_t)g_stack, g_stack_cap, dst, size) ? 0 : 0xDC01;
}
static int map_source_busy(uint64_t addr, uint64_t size) { // g_map_lock held; nonwrapping ranges
    for (int i = 0; i < g_nmap; ++i)
        if (addr < g_map[i].src + g_map[i].size && g_map[i].src < addr + size) return 1;
    return 0;
}
// Loader text/patch seals use 16 KiB boundaries, independently of host page size.
static void module_readonly_run(uint64_t off, uint64_t start, uint64_t end,
                                uint64_t *lo, uint64_t *hi, uint64_t *perm) {
    if (start >= end) return;
    if (off < start) { if (start < *hi) *hi = start; }
    else if (off >= end) { if (end > *lo) *lo = end; }
    else { if (start > *lo) *lo = start; if (end < *hi) *hi = end; *perm &= ~UINT64_C(2); }
}
static void module_data_query(const module_t *m, uint64_t off, uint64_t *lo, uint64_t *hi, uint64_t *perm) {
    module_readonly_run(off, 0, (m->text_end + 0x3fff) & ~UINT64_C(0x3fff), lo, hi, perm);
    if (m->patch_off > 0) module_readonly_run(off, (uint64_t)m->patch_off & ~UINT64_C(0x3fff), m->size, lo, hi, perm);
    module_readonly_run(off, m->key_page_off, m->key_page_off + m->key_page_size, lo, hi, perm);
}
static int map_source_backed(uint64_t src, uint64_t size) {
    if (memory_contains((uintptr_t)g_heap, g_heap_cur, src, size)) return 1;
    int dm_ue_heap_owns(uint64_t a, uint64_t n);   // Unreal's heap on a host malloc zone outside the guest block (hle_uemalloc.c): Unreal allocates its threads' stacks there
    if (dm_ue_heap_owns(src, size)) return 1;
    if (memory_contains((uintptr_t)g_alias, g_alias_cap, src, size))
        return !alias_unmapped(src - (uintptr_t)g_alias, size);
    module_t *m = module_of((void *)(uintptr_t)src);
    // Active NRO data is valid backing too (FFX/X-2's launcher allocates thread stacks from its bss). Deactivation below checks borrowers under this same lock before reload can restore it.
    if (!m || !m->loaded || !memory_contains((uintptr_t)m->base, m->size, src, size)) return 0;
    uint64_t lo = m->data_off ? m->data_off : (m->ro_off ? m->ro_off : m->text_end);
    uint64_t hi = m->patch_off > 0 ? (uint64_t)m->patch_off : m->size;
    uint64_t off = src - (uintptr_t)m->base, perm = 3;
    module_data_query(m, off, &lo, &hi, &perm);
    return perm == 3 && hi >= lo && memory_contains(lo, hi - lo, off, size);
}
int svc_nro_deactivate(module_t *m) {
    pthread_mutex_lock(&g_map_lock);
    uint64_t base = (uintptr_t)m->base;
    for (int i = 0; i < g_nmap; i++)
        if (g_map[i].src < base + m->size && base < g_map[i].src + g_map[i].size) {
            pthread_mutex_unlock(&g_map_lock); return 0;
        }
    m->loaded = 0;
    pthread_mutex_unlock(&g_map_lock); return 1;
}
static void map_source_query(uint64_t addr, uint64_t *base, uint64_t *size, uint32_t *attr, uint64_t *perm) {
    uint64_t lo = *base, remaining = *size; // the final free page can end at 2^64
    for (int i = 0; i < g_nmap; ++i) {
        uint64_t src = g_map[i].src, end = src + g_map[i].size;
        uint64_t lower = lo, upper = remaining;
        if (addr >= src && addr < end) {
            if (src > lower) lower = src;
            if (end - lo < upper) upper = end - lo;
            *attr = 1; *perm = 0; // Locked / IsBorrowed, not PermissionLocked (bit 4)
        } else if (end <= addr && end > lower) lower = end;
        else if (src > addr && src - lo < upper) upper = src - lo;
        remaining = upper - (lower - lo); lo = lower;
    }
    *base = lo; *size = remaining;
}

// Diagnostic DM_ALIAS_WINDOWS=<window MB>: every 30 s, the mapped memory of each window of that size in the alias region (windows aligned to the size in absolute addresses, counted from the page
// bitmap; windows with nothing mapped are left out). For sizing an allocator that reserves fixed, aligned windows and commits pages from each window's start: Unreal's FMallocBinned3 takes
// 74 pools of 512 MB (Octopath Traveler 0, main+0x9f748)
static void *alias_windows_thread(void *arg) {
    uint64_t win = (uint64_t)(uintptr_t)arg << 20, a0 = (uint64_t)(uintptr_t)g_alias, a1 = a0 + g_alias_cap;
    for (;;) {
        sleep(30);
        char line[8192] = {0}; size_t o = 0; uint64_t tot = 0;
        pthread_mutex_lock(&g_map_lock);
        for (uint64_t w = a0 & ~(win - 1); w < a1 && o < sizeof line - 64; w += win) {
            uint64_t n = 0, p0 = (w > a0 ? w - a0 : 0) >> 12, p1 = ((w + win < a1 ? w + win : a1) - a0) >> 12;
            for (uint64_t p = p0; p < p1; p++) n += atomic_load(&g_alias_bits[p >> 6]) >> (p & 63) & 1;
            if (n) { o += snprintf(line + o, sizeof line - o, " +%#llx:%.1f", (unsigned long long)(w - (a0 & ~(win - 1))), n * 4096 / 1048576.0); tot += n; }
        }
        pthread_mutex_unlock(&g_map_lock);
        LOG("[mem] alias windows of %llu MB (window offset:MB mapped), %.1f MB in all:%s", (unsigned long long)(win >> 20), tot * 4096 / 1048576.0, line);
    }
    return NULL;
}

// ---- Address regions ----
// The SDK's VammManager hands out the alias region as address regions. The rewriter's observation gates in the sdk (patch_nso.py "Address regions", placed by its symbols in any
// build) show each one: its size, where the request returns to in the game (the first return address outside the sdk on the frame chain: the function that asked, or its caller
// when it tail-called the SDK API, whose frame is gone by then), the SDK API it came through (such as StandardAllocator) and its address. With the page bitmaps that gives how much of each region a game uses, which decides how its reservations can fit an iPhone's alias region.
// A region of 256 MB or more is logged when made; DM_REGIONS=<file> writes the live table every 2 s (the NSP script's heap check reads it, with a Switch-sized DM_ALIAS_SIZE)
typedef struct { uint64_t off, size, via; char who[40]; } region_t;   // via: the outermost sdk return address on the way (the SDK API the request came through)
static region_t *g_rgn; static int g_nrgn, g_rgn_cap; static pthread_mutex_t g_rgn_lock = PTHREAD_MUTEX_INITIALIZER;   // grows as needed: Unity 6 takes all its memory as regions
static _Thread_local uint64_t t_rgn_size, t_rgn_via; static _Thread_local char t_rgn_who[40];
// Where a request at an sdk function's entry gate returns to in the game: buf gets "main+0x…", *via the outermost sdk return address on the way (0 = none)
static void guest_caller(const uint64_t *r, char *buf, size_t n, uint64_t *via) {
    // at the function's entry the return address is the x30 the gate block pushed above the gate's register frame (regs + 800: x29, x30; entry.S), x30 itself holds the gate's own
    uint64_t lr = r[101], fp = r[29]; module_t *m = module_of((void *)(uintptr_t)lr);
    *via = 0;
    for (int i = 0; i < 32 && m && !strcmp(m->name, "sdk"); i++) {
        *via = lr - (uint64_t)(uintptr_t)m->base;   // AArch64 frame records: [fp] = the caller's fp, [fp + 8] = its return address
        uint64_t rec[2]; vm_size_t got = 0;   // read safely: static constructors run on the host's main thread, whose stack is outside the guest block
        if (!fp || (fp & 15) || vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof rec, (vm_address_t)rec, &got) || got != sizeof rec) break;
        lr = rec[1]; fp = rec[0]; m = module_of((void *)(uintptr_t)lr);
    }
    if (m) snprintf(buf, n, "%s+%#llx", m->name, (unsigned long long)(lr - (uint64_t)(uintptr_t)m->base));
    else snprintf(buf, n, "%#llx", (unsigned long long)lr);
}
static void region_ask(uint64_t *r, int param) {
    (void)param;
    t_rgn_size = r[2]; guest_caller(r, t_rgn_who, sizeof t_rgn_who, &t_rgn_via);
}
static void region_got(uint64_t *r, int param) {
    mem_init();
    uint64_t a = param >= 0 && param < 31 ? r[param] : 0, ab = (uint64_t)(uintptr_t)g_alias;
    if (a < ab || a - ab >= g_alias_cap) return;
    pthread_mutex_lock(&g_rgn_lock);
    if (g_nrgn == g_rgn_cap) {
        region_t *t = realloc(g_rgn, sizeof *t * (size_t)(g_rgn_cap ? 2 * g_rgn_cap : 256));
        if (t) { g_rgn = t; g_rgn_cap = g_rgn_cap ? 2 * g_rgn_cap : 256; }
    }
    int kept = g_nrgn < g_rgn_cap;
    if (kept) { g_rgn[g_nrgn].off = a - ab; g_rgn[g_nrgn].size = t_rgn_size; g_rgn[g_nrgn].via = t_rgn_via; memcpy(g_rgn[g_nrgn].who, t_rgn_who, sizeof t_rgn_who); g_nrgn++; }
    pthread_mutex_unlock(&g_rgn_lock);
    if (!kept) { static _Atomic int once; if (!atomic_fetch_add(&once, 1)) LOG("[RED] address region table: out of memory at %d regions, later ones are not in it", g_nrgn); }
    if (t_rgn_size >= 0x10000000) LOG("[mem] address region %.2f GB at alias +%.2f GB, returning to %s", t_rgn_size / 1073741824.0, (a - ab) / 1073741824.0, t_rgn_who);
}
static void region_free(uint64_t *r, int param) {
    (void)param;
    uint64_t off = r[1] - (uint64_t)(uintptr_t)g_alias;
    pthread_mutex_lock(&g_rgn_lock);
    for (int i = 0; i < g_nrgn; i++) if (g_rgn[i].off == off) { g_rgn[i] = g_rgn[--g_nrgn]; break; }
    pthread_mutex_unlock(&g_rgn_lock);
}
// ---- StandardAllocator's address space ----
// With virtual address memory, StandardAllocator::Initialize(nullptr, size) takes a region of size from the alias region and lays its heap out over all of it (CentralHeap), and
// games pass sizes made for the Switch's 64 GB alias region: Pokémon Legends: Z-A 12 GB, Pokémon Scarlet 12 GB, Paper Mario: The Thousand-Year Door 62 GB, Absolum 40 GB. Where the
// size is more than the alias region has room for, the request would fail (an nn::diag abort in nninitStartup), so the gate at the function's entry (patch_nso.py) gives it the room
// instead: the alias region less the regions already made (at least the SDK's 62 MB node area) and 256 MB for regions made later, in 32 MB steps; the profile key stdalloc_cap sets
// it instead (Absolum's .NET garbage collector takes 1.5 GB after it). The SDK lays the region out from that size, so it holds; the pool, not the region, bounds what the heap
// can map. A workaround for the iPhone's address space, not the game: with room for the request (a Switch-sized DM_ALIAS_SIZE) nothing changes. Found by symbol in every sdk
#define STDALLOC_LATER 0x10000000ull
static void stdalloc_init(uint64_t *r, int param) {
    (void)param;
    if (r[1]) return;   // a heap over memory the game passes in: no address region
    mem_init();
    uint64_t reach = 0x3e00000;   // the node area with the SDK-wide cut (sdk_patches.py); a build without it keeps 1 GB, and a 12 GB request fails there as before
    pthread_mutex_lock(&g_rgn_lock);
    for (int i = 0; i < g_nrgn; i++) if (g_rgn[i].off + g_rgn[i].size > reach) reach = g_rgn[i].off + g_rgn[i].size;
    pthread_mutex_unlock(&g_rgn_lock);
    uint64_t cap = DM_GAME_STDALLOC_CAP ? DM_GAME_STDALLOC_CAP : g_alias_cap > reach + STDALLOC_LATER ? (g_alias_cap - reach - STDALLOC_LATER) & ~0x1ffffffull : 0;
    if (!cap || r[2] <= cap) return;
    char who[40]; uint64_t via; guest_caller(r, who, sizeof who, &via);
    LOG("[mem] StandardAllocator::Initialize(nullptr, %.2f GB) returning to %s: %.2f GB of address space, %s", r[2] / 1073741824.0, who, cap / 1073741824.0,
        DM_GAME_STDALLOC_CAP ? "the profile's stdalloc_cap" : "what the alias region has room for (the profile key stdalloc_cap sets it)");
    r[2] = cap;
}
__attribute__((constructor)) static void regions_attach(void) {
    gate_obs_set(OBS_REGION_ASK, region_ask); gate_obs_set(OBS_REGION_GOT, region_got); gate_obs_set(OBS_REGION_FREE, region_free);
    gate_obs_set(OBS_STDALLOC_INIT, stdalloc_init);
}
// Pages set in a bitmap's bits [lo, hi), a word at a time: a 62 GB region is 16 million pages, counted under g_map_lock, which every MapPhysicalMemory takes
static uint64_t bits_set(_Atomic uint64_t *b, uint64_t lo, uint64_t hi) {
    uint64_t n = 0;
    for (; lo < hi && (lo & 63); lo++) n += atomic_load(&b[lo >> 6]) >> (lo & 63) & 1;
    for (; lo + 64 <= hi; lo += 64) n += (uint64_t)__builtin_popcountll(atomic_load(&b[lo >> 6]));
    for (; lo < hi; lo++) n += atomic_load(&b[lo >> 6]) >> (lo & 63) & 1;
    return n;
}
// The first and last bit set in [lo, hi) (-1 when none), skipping empty words
static void bits_span(const uint64_t *b, uint64_t lo, uint64_t hi, int64_t *first, int64_t *last) {
    *first = *last = -1;
    for (uint64_t i = lo; i < hi; ) {
        uint64_t w = b[i >> 6] >> (i & 63);
        if (!w) { i = (i | 63) + 1; continue; }
        i += (uint64_t)__builtin_ctzll(w);
        if (i >= hi) break;
        if (*first < 0) *first = (int64_t)i;
        *last = (int64_t)i; i++;
    }
}
// The table: alias offset, size, where the request returns to, bytes mapped now (4 KB pages), the part ever opened (host pages: from its first to its last, region-relative),
// the sdk offset it came through
static void *regions_thread(void *arg) {
    const char *path = arg; char tmp[1024]; snprintf(tmp, sizeof tmp, "%s.partial", path);
    uint64_t hpg = (uint64_t)getpagesize();
    for (;;) {
        FILE *f = fopen(tmp, "w");
        if (f) {
            fprintf(f, "# alias region %#llx; offset\tsize\treturns to\tmapped\tfirst opened\tend of the last opened (region-relative; - = none)\tvia (sdk offset)\n", (unsigned long long)g_alias_cap);
            pthread_mutex_lock(&g_rgn_lock); int n = g_nrgn; region_t *t = malloc(sizeof *t * (size_t)(n ? n : 1)); if (t && n) memcpy(t, g_rgn, sizeof *t * (size_t)n); pthread_mutex_unlock(&g_rgn_lock);
            pthread_mutex_lock(&g_map_lock);
            for (int i = 0; t && i < n; i++) {
                uint64_t lo = t[i].off, hi = t[i].off + t[i].size > g_alias_cap ? g_alias_cap : t[i].off + t[i].size, mapped = bits_set(g_alias_bits, lo >> 12, hi >> 12);
                int64_t first, last; bits_span(g_alias_exposed, lo / hpg, (hi + hpg - 1) / hpg, &first, &last);
                char fo[24] = "-", lo2[24] = "-";
                if (first >= 0) { snprintf(fo, sizeof fo, "%#llx", (unsigned long long)((uint64_t)first * hpg > lo ? (uint64_t)first * hpg - lo : 0));
                                  snprintf(lo2, sizeof lo2, "%#llx", (unsigned long long)((uint64_t)(last + 1) * hpg - lo)); }
                fprintf(f, "%#llx\t%#llx\t%s\t%#llx\t%s\t%s\t%#llx\n", (unsigned long long)t[i].off, (unsigned long long)t[i].size, t[i].who, (unsigned long long)mapped * 4096, fo, lo2,
                        (unsigned long long)t[i].via);
            }
            pthread_mutex_unlock(&g_map_lock); free(t);
            fclose(f); rename(tmp, path);
        }
        sleep(2);
    }
    return NULL;
}

int svc_thread(int num, uint64_t *regs);   // hle_thread.c: thread and synchronization kind

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr) {
    { void hlecount_svc(int); hlecount_svc(num); }   // DM_HLECOUNT: svc counts by number
    mem_init();
    if (svc_thread(num, regs)) return regs[0];
    switch (num) {
    case 0x01: {  // SetHeapSize(out_addr*, size)  → x1 = heap base
        size_t size = regs[1];
        // Larger than the heap region: ResultOutOfMemory (0xD001), as Horizon's KPageTable::SetHeapSize returns. nn::os::SetMemoryHeapSize turns only that, 103 and LimitReached into
        // os::ResultOutOfMemory and aborts on anything else; Donkey Kong Country: Tropical Freeze finds its heap size by a binary search between 3 and 6 GB, which needs the error back
        if (size > g_heap_cap) { LOG("SetHeapSize OOM: req %#zx > cap %#zx", size, g_heap_cap); regs[0] = 0xD001; return 0; }
        pthread_mutex_lock(&g_map_lock);
        size_t previous = g_heap_cur, image = image_bytes();
        if (size < previous && map_source_busy((uintptr_t)g_heap + size, previous - size)) {
            pthread_mutex_unlock(&g_map_lock); regs[0] = 0xD401; return 0;
        }
        if (image > APP_POOL || size > APP_POOL - image) {   // ResultLimitReached; games retry in 2 MB decrements
            // Logged once (Mario's retries would repeat it): a game that sizes its heap as pool − used memory and does not retry stops here when its modules are larger than the used memory reported
            // (profile mem_used; the NSP script's probe boot reads this line, and sets mem_used when no SetHeapSize succeeds afterwards)
            static int once; if (!once++) LOG("[mem] SetHeapSize %#zx + modules %#zx > app pool %#llx (used memory reported %#llx + bss %#zx)", size, image_bytes() - big_bss_bytes(),
                                              (unsigned long long)APP_POOL, (unsigned long long)DM_GAME_MEM_USED, big_bss_bytes());
            pthread_mutex_unlock(&g_map_lock); regs[0] = 0x10801; return 0;
        }
        uint64_t charged = (uint64_t)DM_GAME_MEM_USED + big_bss_bytes() + atomic_load(&g_alias_mapped);
        if (DM_GAME_MEM_MAP_LIMIT && (charged > APP_POOL || size > APP_POOL - charged)) {
            pthread_mutex_unlock(&g_map_lock); regs[0] = 0x10801; return 0;
        }
        int rc = 0;
        if (size > g_heap_cur) rc = mprotect(g_heap + g_heap_cur, size - g_heap_cur, PROT_READ | PROT_WRITE);
        else if (size < g_heap_cur) rc = mprotect(g_heap + size, g_heap_cur - size, PROT_NONE);
        if (rc) die("SetHeapSize mprotect failed: req=%#zx errno=%d", size, errno);
        if (g_flag.svc) LOG("SetHeapSize %#zx -> base %p mprotect=%d errno=%d", size, g_heap, rc, errno);
        else if (size > g_heap_cur) LOG("[mem] heap %.2f GB (heap region %.2f GB)", size / 1073741824.0, g_heap_cap / 1073741824.0);
        g_heap_cur = size;
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = 0; regs[1] = (uint64_t)g_heap; return 0;
    }
    case 0x02: case 0x03: { // Permission/attribute stubs must not modify a borrowed source.
        uint64_t addr = regs[0], size = regs[1];
        pthread_mutex_lock(&g_map_lock);
        int busy = size > UINT64_MAX - addr || (size && map_source_busy(addr, size));
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = busy ? 0xD401 : 0; return 0;
    }
    case 0x04: {  // MapMemory(dst, src, size): when creating a thread the SDK moves the stack on the heap into the stack region
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        uint32_t result = map_arguments(dst, src, size);
        if (result) { regs[0] = result; return 0; }
        pthread_mutex_lock(&g_map_lock);
        if (map_find(dst, size) || map_source_busy(src, size) || !map_source_backed(src, size)) result = 0xD401;
        else if (g_nmap == MAX_MAP) result = 0xCE01;
        if (result) { pthread_mutex_unlock(&g_map_lock); regs[0] = result; LOG("[RED] MapMemory refused (%#x): %#llx <- %#llx +%#llx", result, (unsigned long long)dst, (unsigned long long)src, (unsigned long long)size); return 0; }
        // Apple pages are 16K, Horizon pages 4K: the accessible region is extended to a 16K boundary (the extra pages opened are unused pages of the same region, semantics unaffected)
        uint64_t mask = (size_t)getpagesize() - 1;
        uint64_t lo = dst & ~mask, hi = (dst + size + mask) & ~mask;
        if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) die("MapMemory mprotect errno=%d", errno);
        dm_guest_write_prepare((void *)lo, hi - lo);
        memcpy((void *)dst, (void *)src, size);   // move semantics: the source region is not accessed while mapped (Horizon makes it inaccessible)
        map_add(dst, size, src, 0x0B /*MappedMemory*/);
        pthread_mutex_unlock(&g_map_lock);
        if (g_flag.svc) LOG("MapMemory %#llx <- %#llx +%#llx", dst, src, size);
        regs[0] = 0; return 0;
    }
    case 0x05: {  // UnmapMemory(dst, src, size): contents are moved back to the source region; a 16K page may be shared with an adjacent mapping, permissions are not revoked
        uint64_t dst = regs[0], src = regs[1], size = regs[2];
        uint32_t result = map_arguments(dst, src, size);
        if (result) { regs[0] = result; return 0; }
        pthread_mutex_lock(&g_map_lock);
        int k = map_find(dst, size);
        if (!k || g_map[k - 1].base != dst || g_map[k - 1].size != size || g_map[k - 1].src != src) {
            pthread_mutex_unlock(&g_map_lock); LOG("UnmapMemory does not match a mapping %#llx+%#llx", dst, size); regs[0] = 0xD401; return 0; }
        dm_guest_write_prepare((void *)src, size);
        memcpy((void *)src, (void *)dst, size);
        map_del(k - 1);
        pthread_mutex_unlock(&g_map_lock);
        regs[0] = 0; return 0;
    }
    case 0x06: {  // QueryMemory(MemoryInfo* out, PageInfo* pout, addr) → x1 = pageinfo
        uint64_t mi = regs[0], addr = regs[2];
        // MemoryInfo: base(0) size(8) state(0x10) attr(0x14) perm(0x18) ...
        uint8_t *a = (uint8_t *)addr;
        uint64_t base, size, state, perm;
        uint32_t attr = 0;
        int k;
        pthread_mutex_lock(&g_map_lock);   // the registry branches read g_map; the other branches are cheap, so the lock simply spans the whole chain
        uint64_t ab = (uintptr_t)g_alias, ae = ab + g_alias_cap, rb = 0, rn = 0;
        int mapped = 0, in_alias = addr >= ab && addr < ae;
        if (in_alias) alias_query(addr, &rb, &rn, &mapped);
        if (a >= g_heap && a < g_heap + g_heap_cur) { base=(uint64_t)g_heap; size=g_heap_cur; state=5/*Heap*/; perm=3; }
        else if ((k = map_find(addr, 1))) { base=g_map[k-1].base; size=g_map[k-1].size; state=g_map[k-1].state; perm=3; }
        else if (in_alias && mapped) { base=rb; size=rn; state=5; perm=3; }
        else if (a >= g_as && a < g_as + g_reserve) {   // free: up to the adjacent mapping/heap boundary
            uint64_t lo = (uint64_t)g_as, hi = (uint64_t)g_as + g_reserve;
            if (a >= g_heap) lo = (uint64_t)g_heap + g_heap_cur;
            else if (hi > (uint64_t)g_heap) hi = (uint64_t)g_heap;
            if (in_alias) {
                if (rb > ab && rb > lo) lo = rb;
                if (rb + rn < ae && rb + rn < hi) hi = rb + rn;
            } else if (g_alias_cap && addr < ab) {
                alias_query(ab, &rb, &rn, &mapped);
                uint64_t edge = mapped ? ab : rb + rn;
                if ((mapped || edge < ae) && edge < hi) hi = edge;
            } else if (g_alias_cap) {
                alias_query(ae - 1, &rb, &rn, &mapped);
                uint64_t edge = mapped ? ae : rb;
                if ((mapped || edge > ab) && edge > lo) lo = edge;
            }
            for (int i = 0; i < g_nmap; i++) {
                uint64_t e = g_map[i].base + g_map[i].size;
                if (e <= addr && e > lo) lo = e;
                if (g_map[i].base > addr && g_map[i].base < hi) hi = g_map[i].base;
            }
            base=lo; size=hi-lo; state=0; perm=0; }
        else if (module_of(a)) {
            // Loaded module: report the regions in Horizon's layout -- code [0, ro) Code+RX, read-only [ro, data) Code+R, data and bss [data, patch area) CodeData+RW.
            // The SDK's C++ exception unwinding (nn::ro::detail::GetExceptionInfo) walks back from the PC over "Code+RX" regions to find the module start, forward over Code then CodeData to find its end, then reads MOD0 at the module start for .eh_frame_hdr.
            // Previously "the 4 KiB page around it, Code+RX" was reported: the backward walk ran on into other memory, found no MOD0 and aborted (Engage's IL2CPP throws a C++ exception for every managed exception).
            // The rewriter's patch area is reported as CodeData+RX: modules sit back to back in the host, and an executable patch area reported as Code+RX would merge the previous module into the backward walk
            // Modules with the patch area before the base (patch_off < 0): the patch area is reported alone as CodeData+RX, and data and bss run to the end of the image
            module_t *mm = module_of(a); uint64_t off = (uint64_t)(a - mm->base), mb = (uint64_t)mm->base;
            uint64_t ro = mm->ro_off ? mm->ro_off : mm->text_end, da = mm->data_off ? mm->data_off : ro, pa = mm->patch_off > 0 ? (uint64_t)mm->patch_off : mm->size;
            if (a < mm->base)  { base = mb + mm->patch_off; size = mm->patch_len; state = 4/*CodeData*/; perm = 5; }
            else if (off < ro) { base = mb;      size = ro;            state = 3/*Code*/;     perm = 5; }
            else if (off < da) { base = mb + ro; size = da - ro;       state = 3/*Code*/;     perm = 1; }
            else if (off < pa) { base = mb + da; size = pa - da;       state = 4/*CodeData*/; perm = 3; }
            else               { base = mb + pa; size = mm->size - pa; state = 4/*CodeData*/; perm = 5; }
            if (state == 4 && perm == 3) {
                uint64_t lo = base - mb, hi = lo + size;
                module_data_query(mm, off, &lo, &hi, &perm);
                base = mb + lo; size = hi - lo;
            }
        }
        else {
            // The address lies in "the current host thread's stack": report the whole stack (state 0xB = stack). When the SDK initializes the main thread record it queries here with the sp at that moment and takes the reported range as the main thread stack;
            // later nn::os::GetCurrentStackInfo asserts that sp is still inside that range and aborts if not. Previously every address outside the guest address space was reported as "the 4 KiB page containing it",
            // so the main thread stack was recorded as one page. Whether the sp at init and a later sp share a page depends on the absolute position of the host stack top, which moves with the total length of environment variables / launch arguments (a different environment occasionally aborts at startup).
            uint64_t top = (uint64_t)pthread_get_stackaddr_np(pthread_self()), ssz = pthread_get_stacksize_np(pthread_self());
            if (addr < top && addr >= top - ssz) { base = top - ssz; size = ssz; state = 0xB/*Stack*/; perm = 3; }
            // Other host memory: report "free, no permissions" (the 4 KiB page around it) as Horizon does for unmapped addresses. It used to report Code+RX, and GetExceptionInfo walking back from main merged on through host memory down to 0
            else { base=addr & ~0xFFFull; size=0x1000; state=0/*Free*/; perm=0; } }
        map_source_query(addr, &base, &size, &attr, &perm);
        pthread_mutex_unlock(&g_map_lock);
        if (mi) {
            struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, reserved; }
                out = {base, size, (uint32_t)state, attr, (uint32_t)perm, 0, 0, 0};
            _Static_assert(sizeof out == 40, "MemoryInfo ABI size");
            memcpy((void *)(uintptr_t)mi, &out, sizeof out);
        }
        regs[0] = 0; regs[1] = 0; return 0;
    }
    case 0x2C: case 0x2D: {  // MapPhysicalMemory / UnmapPhysicalMemory(addr, size): only allowed inside the alias region (used by the SDK's VammManager)
        uintptr_t a = regs[0]; size_t size = regs[1];
        if (!size || ((a | size) & 0xfff) || !memory_contains((uintptr_t)g_alias, g_alias_cap, a, size)) {
            LOG("SVC %#x outside the alias region %p+%#zx", num, (void *)a, size); regs[0] = 0xD401; return 0;
        }
        pthread_mutex_lock(&g_map_lock);
        // The SDK extends committed windows across borrowed stack sources
        // (Raging Loop, StandardAllocator). Recommit preserves every live byte;
        // only decommit can invalidate backing required for eventual copyback.
        if (num == 0x2D && map_source_busy(a, size)) { pthread_mutex_unlock(&g_map_lock); regs[0] = 0xD401; return 0; }
        uint64_t off = a - (uintptr_t)g_alias;
        int64_t need = alias_unmapped(off, size) * 4096;
        // Profile mem_map_limit: a map that would take the process past the application pool fails with ResultLimitReached, as on Horizon (the pool less the used memory GetInfo reports).
        // Pokémon Legends: Z-A's nninitStartup allocates and frees everything allocatable (pool − 64 MB, mapped at the top of its StandardAllocator region: 12 GB, 4 GB after the cut in its patches.py); the SDK keeps the freed pages mapped
        // and maps new ones for later allocations, until a map fails: then it unmaps the cached block and retries (measured on the Mac: the refused 4 MB map at +0x43e2c000 was followed by
        // UnmapPhysicalMemory of the 3.15 GB block and the same map again). Without the limit the mapped total went past the pool in the first minute and the freed block stayed mapped.
        // Off by default: Ace Attorney Investigations Collection maps a little more than the 1 GB pool its profile reports
        if (DM_GAME_MEM_MAP_LIMIT && num == 0x2C) {
            int64_t room = (int64_t)APP_POOL - (int64_t)(g_heap_cur + DM_GAME_MEM_USED + big_bss_bytes()) - atomic_load(&g_alias_mapped);
            if (need && need > room) {
                pthread_mutex_unlock(&g_map_lock);
                static _Atomic int nref; if (atomic_fetch_add(&nref, 1) < 8) LOG("[mem] MapPhysicalMemory +%#zx refused: %.2f GB mapped, the application pool is full (ResultLimitReached, the SDK frees cached pages and retries)",
                                                                       size, atomic_load(&g_alias_mapped) / 1073741824.0);
                regs[0] = 0x10801; return 0;
            }
        }
        // Host pages are 16 KB, Horizon pages 4 KB: a map is rounded out to host pages (the extra neighboring 4K pages are inside the alias region and merely become writable early, harmless);
        // An edge host page is reclaimed only when no live logical neighbor remains.
        uintptr_t pg = (uintptr_t)getpagesize(), lo = (uintptr_t)a, hi = (uintptr_t)a + size;
        uintptr_t mlo = lo & ~(pg - 1), mhi = (hi + pg - 1) & ~(pg - 1);
        int commit = num == 0x2C;
        int64_t changed = commit ? need : (int64_t)size - need;
        if (!commit) {
            if ((lo - mlo) / 4096 != (uint64_t)alias_unmapped(mlo - (uintptr_t)g_alias, lo - mlo)) mlo += pg;
            if ((mhi - hi) / 4096 != (uint64_t)alias_unmapped(hi - (uintptr_t)g_alias, mhi - hi)) mhi -= pg;
        }
        if (changed && mhi > mlo) {
            if (mprotect((void *)mlo, mhi - mlo, commit ? PROT_READ | PROT_WRITE : PROT_NONE))
                die("SVC %#x mprotect %p+%#zx errno=%d", num, (void *)a, size, errno);
            if (commit) dm_guest_write_prepare((void *)mlo, mhi - mlo);
            else (void)madvise((void *)mlo, mhi - mlo, MADV_FREE);
        }
        if (commit && changed) alias_initialize(off, size);
        if (g_flag.svc) LOG("%sPhysicalMemory %p+%#zx", commit ? "Map" : "Unmap", (void *)a, size);
        // how deep the alias region is used (one line per 256 MB crossed): for sizing the reservation and alias region (iOS caps a single reservation at 6 GB)
        if (num == 0x2C) { static _Atomic uint64_t hw; uint64_t end = off + size, old = atomic_load(&hw);
            while (end > old && !atomic_compare_exchange_weak(&hw, &old, end)) {}
            if (end > old && end >> 28 != old >> 28) LOG("[mem] alias region mapped up to +%.2f GB (alias region %.2f GB)", end / 1073741824.0, g_alias_cap / 1073741824.0); }
        // Total actually mapped in the alias region (one line per 256 MB crossed, up or down): how much physical memory the game uses and whether it keeps growing. Counted with a page bitmap, only pages whose state really changed --
        // the SDK maps pages that are already mapped and unmaps ranges larger than what it mapped (Engage unmaps 0x1000000 and then 0x103f000 at the same address), so counting requested sizes went negative
        { int64_t d = alias_pages(off, size, commit) * 4096, before = atomic_fetch_add(&g_alias_mapped, d), after = before + d;
          pthread_mutex_unlock(&g_map_lock);
          if (before >> 28 != after >> 28) LOG("[mem] alias region mapped %.2f GB (application pool %.2f GB)", after / 1073741824.0, APP_POOL / 1073741824.0); }
        regs[0] = 0; return 0;
    }
    case 0x26: {  // Break(reason, ptr, size): SDK fatal abort
        uint32_t res = (regs[1] && regs[2] >= 4) ? *(uint32_t *)regs[1] : 0;
        module_t *mm = module_of((void *)regs[30]);
        static int nb;
        LOG("Break#%d reason=%#llx Result=%#x svc at %s+%#llx", ++nb, regs[0], res, mm ? mm->name : "?", orig_addr);
        dm_backtrace(regs[29]);
        if (regs[0] & 0x80000000ull) return 0;   // notification only, can continue
        _exit(42);
    }
    case 0x1E: guest_clock_read_wait(); regs[0] = 0; regs[1] = guest_ns() * 12 / 625; return 0;   // GetSystemTick
    case 0x27: {  // OutputDebugString(str, len)
        if (g_flag.dbg) fprintf(stderr, "[guest] %.*s\n", (int)regs[1], (char *)regs[0]);
        regs[0] = 0; return 0;
    }
    case 0x29: {  // GetInfo(out*, id, handle, subid) → x1 = value
        uint64_t id = regs[1], v = 0;
        switch (id) {
        case I_CoreMask: v = 7; break;
        case I_PriorityMask: v = 0; break;
        case I_AliasAddr: v = (uint64_t)g_alias; break;   case I_AliasSize: v = g_alias_cap; break;
        case I_HeapAddr:  v = (uint64_t)g_heap;  break;   case I_HeapSize:  v = g_heap_cap;  break;
        case I_AslrAddr:  v = (uint64_t)g_as;    break;   case I_AslrSize:  v = g_reserve;   break;
        case I_StackAddr: v = (uint64_t)g_stack; break;   case I_StackSize: v = g_stack_cap; break;
        case I_TotalMem: case I_TotalNonSys: v = APP_POOL; break;
        case I_UsedMem:  case I_UsedNonSys:  pthread_mutex_lock(&g_map_lock);
                                             v = g_heap_cur + DM_GAME_MEM_USED + big_bss_bytes();   // profile mem_used, 64 MB unless the game needs more (see mem_used in dm/games/README.md)
                                             // with mem_map_limit, the pages mapped in the alias region count too, as on Horizon (the limit in MapPhysicalMemory is the pool less this)
                                             if (DM_GAME_MEM_MAP_LIMIT) v += (uint64_t)atomic_load(&g_alias_mapped);
                                             pthread_mutex_unlock(&g_map_lock); break;
        // the system resource the NPDM declares (profile sys_resource; 32 MB without the key). The SDK's IsVirtualAddressMemoryEnabled is "total != 0": Big Brain Academy's nninitStartup
        // asks StandardAllocator for 60 GB of virtual address memory when it is, and sets a heap only when it is not (its NPDM declares none)
        case I_SysResTotal: v = DM_GAME_SYS_RESOURCE; break; case I_SysResUsed: v = DM_GAME_SYS_RESOURCE ? 0x400000 : 0; break;
        case I_DebuggerAttached: v = 0; break;
        case I_RandomEntropy: { uint64_t dm_entropy64(void); v = dm_entropy64(); break; }   // a deterministic stream when DM_DETCLOCK is on (hle_os.c)
        case I_ProgramId: { static uint64_t pid; if (!pid) { const char *e = getenv("DM_PROGRAM_ID"); pid = e ? strtoull(e, NULL, 16) : DM_GAME_PROGRAM_ID; } v = pid; break; }   // default from the game profile's program_id; DM_PROGRAM_ID=hex overrides
        case I_IsApplication: v = 1; break;
        // nn::os::GetFreeThreadCount (SDK 11+): the thread resource limit less the threads alive, as the kernel answers it (Atmosphère kern_svc_info.cpp). pm gives an application a limit of
        // 96 threads (pm_spec.cpp BaseApplicationThreads; a boost raises it to 256). Monster Train 2's Unity 2022.3 player creates a thread only when this is at least 1: the 0 it got here
        // left its main thread waiting at boot for a thread it never created (main+0x3e1ac4)
        case I_FreeThreadCount: { int thr_limit_used(void); int u = thr_limit_used(); v = u < 96 ? 96 - u : 0; break; }
        case I_UserExcAddr: { static _Alignas(0x1000) uint8_t ctx[0x1000]; v = (uint64_t)ctx; break; }   // user exception context area: newer SDKs' SetDyingMessageRegion writes to +0x1c0 (0 would write through a null pointer)
        default: v = 0; break;
        }
        regs[0] = 0; regs[1] = v; return 0;
    }
    default:
        if (!g_seen[num & 0xff]) { g_seen[num & 0xff] = 1; LOG("SVC %#x x0=%#llx x1=%#llx x2=%#llx (placeholder returns success)", num, regs[0], regs[1], regs[2]); }
        regs[0] = 0; return 0;
    }
}
