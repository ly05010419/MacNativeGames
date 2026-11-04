// TOTK: NVN physical / virtual memory pools.
// A virtual pool is a host VM range whose 64 KiB pages alias pages of physical pools via mach_vm_remap
// (copy=FALSE), so CPU and GPU writes through either view hit the same physical pages.
// Physical pools marked CPU_NO_ACCESS cannot be observed except through virtual mappings, so they get
// 16 KiB-aligned host storage (game storage may be only 4/8 KiB aligned, which Darwin cannot remap).
// CPU-visible physical pools must alias the game storage itself and therefore must be 16 KiB aligned.
#include "totk_vpool.h"
#include "nvn_mtl.h"
#include "nvn_dirty.h"
#include "../../host/hle.h"
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

#define F_CPU_NO_ACCESS NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS
#define F_GPU_NO_ACCESS NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS
#define F_PHYSICAL NVN_MEMORY_POOL_FLAGS_PHYSICAL
#define F_VIRTUAL NVN_MEMORY_POOL_FLAGS_VIRTUAL
#define POOL_MAGIC MAGIC('P', 'O', 'O', 'L')

static pthread_mutex_t g_map_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t host_page(void) { return (uint64_t)getpagesize(); }
static uint64_t round_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static uint8_t *vm_fresh(uint64_t span) {
    mach_vm_address_t a = 0;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &a, span, VM_FLAGS_ANYWHERE);
    if (kr != KERN_SUCCESS) { LOG("【红】vpool: mach_vm_allocate(%#llx) failed kr=%d", span, kr); return NULL; }
    return (uint8_t *)(uintptr_t)a;
}

#ifdef GAME_MKW
/* Unmapped virtual-pool pages. Metal residency wires every page of a zero-copy buffer, including zero-fill pages never
   touched (measured: a resident 2 GiB buffer with one touched page wired 2049 MB). MKW's virtual pools reach several GiB
   and are mostly unmapped, so fresh holes wired more than a 16 GB Mac has. Holes are instead views of one shared scratch
   region, wired once. Unmapped pages therefore read whatever was last written to any hole instead of zero; NVN gives
   accesses to unmapped virtual pages no contents the game may rely on. */
#define HOLE_CHUNK (64ull << 20)
static mach_vm_address_t g_hole;
static pthread_once_t g_hole_once = PTHREAD_ONCE_INIT;
static void hole_init(void) {
    if (mach_vm_allocate(mach_task_self(), &g_hole, HOLE_CHUNK, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) g_hole = 0;
}
static kern_return_t hole_fill(uint8_t *at, uint64_t size) {
    pthread_once(&g_hole_once, hole_init);
    if (!g_hole) return KERN_RESOURCE_SHORTAGE;
    for (uint64_t off = 0; off < size; off += HOLE_CHUNK) {
        mach_vm_address_t dst = (mach_vm_address_t)(uintptr_t)(at + off); vm_prot_t cur, max;
        uint64_t n = size - off < HOLE_CHUNK ? size - off : HOLE_CHUNK;
        kern_return_t kr = mach_vm_remap(mach_task_self(), &dst, n, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), g_hole,
                                         FALSE, &cur, &max, VM_INHERIT_NONE);
        if (kr != KERN_SUCCESS) return kr;
        if (dst != (mach_vm_address_t)(uintptr_t)(at + off)) return KERN_INVALID_ADDRESS;
        if ((cur & (VM_PROT_READ | VM_PROT_WRITE)) != (VM_PROT_READ | VM_PROT_WRITE)) return KERN_PROTECTION_FAILURE;
    }
    return KERN_SUCCESS;
}
#endif

int totk_vpool_init(pool_t *o) {
    int phys = (o->flags & F_PHYSICAL) != 0, virt = (o->flags & F_VIRTUAL) != 0;
    o->game_mem = o->mem; o->host = NULL; o->span = 0;
    if (phys == virt) { LOG("【红】vpool: pool flags %#x must be exactly one of PHYSICAL/VIRTUAL", o->flags); return 0; }
    if (!o->size || o->size % TOTK_VPOOL_PAGE) { LOG("【红】vpool: pool size %#llx not a multiple of 64 KiB", o->size); return 0; }
    if (virt && o->mem) { LOG("【红】vpool: virtual pool given storage %p", (void *)o->mem); return 0; }
    if (phys && !o->mem) { LOG("【红】vpool: physical pool without storage"); return 0; }
    if (phys && !(o->flags & F_CPU_NO_ACCESS)) {
#ifdef GAME_MKW
        /* CPU-only physical storage can back a whole virtual pool directly before its address is used.
           Keep the caller's storage, including its alignment; copying it would break CPU coherence. */
        if (o->flags & F_GPU_NO_ACCESS) return 1;
#endif
        if ((uintptr_t)o->mem % host_page()) { LOG("【红】vpool: CPU-visible physical pool %p not %#llx aligned; cannot alias", (void *)o->mem, host_page()); return 0; }
        return 1;   // aliases the game storage directly
    }
    o->span = round_up(o->size, host_page());
    o->host = vm_fresh(o->span);
    if (!o->host) return 0;
#ifdef GAME_MKW
    if (virt) {
        o->page_n = o->size / TOTK_VPOOL_PAGE; o->map_gen = 0;
        o->page_gen = calloc(o->page_n ? o->page_n : 1, sizeof *o->page_gen);
        o->page_src = calloc(o->page_n ? o->page_n : 1, sizeof *o->page_src);
        kern_return_t kr = hole_fill(o->host, o->span);
        if (kr != KERN_SUCCESS) { LOG("【红】vpool: unmapped pages for %#llx bytes failed kr=%d", o->span, kr); totk_vpool_fini(o); return 0; }
    }
#endif
    o->mem = o->host;
    return 1;
}

int totk_vpool_fini(pool_t *o) {
#ifdef GAME_MKW
    free(o->page_gen); o->page_gen = NULL; free(o->page_src); o->page_src = NULL; o->page_n = 0;
#endif
    if (!o->host) return 0;
    mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)o->host, o->span);
    o->host = NULL; o->mem = o->game_mem; o->span = 0;
    return 1;
}

static int check_request(const pool_t *v, const totk_mapping_request *r, int i) {
    uint64_t end = (uint64_t)r->virtual_offset + r->size;
    if (r->virtual_offset < 0 || r->physical_offset < 0 || !r->size) { LOG("【红】vpool: request %d negative/empty range", i); return 0; }
    if ((r->virtual_offset | r->physical_offset | r->size) % TOTK_VPOOL_PAGE) { LOG("【红】vpool: request %d not 64 KiB aligned (v=%#llx p=%#llx n=%#llx)", i, r->virtual_offset, r->physical_offset, r->size); return 0; }
    if (end < r->size || end > v->size) { LOG("【红】vpool: request %d exceeds virtual pool (%#llx > %#llx)", i, end, v->size); return 0; }
    if (!r->physical_pool) return 1;   // unmap
    const pool_t *p = (const pool_t *)(const void *)r->physical_pool;
    if (p->magic != POOL_MAGIC || !(p->flags & F_PHYSICAL)) { LOG("【红】vpool: request %d source %p is not a physical pool", i, (const void *)p); return 0; }
    uint64_t pend = (uint64_t)r->physical_offset + r->size;
    if (pend < r->size || pend > p->size) { LOG("【红】vpool: request %d exceeds physical pool (%#llx > %#llx)", i, pend, p->size); return 0; }
    return 1;
}

// TOTK diagnostics: mapping table (virtual range -> CPU address of the physical backing) so a watch on a GPU
// address can protect the pages the CPU actually touches (the game reads/writes physical pools through their own view).
typedef struct { uintptr_t va, cpu; uint64_t size; } vmap_ent;
static vmap_ent g_vmap[512]; static int g_nvmap;
static void vmap_note(uintptr_t va, uint64_t size, uintptr_t cpu) {
    for (int i = 0; i < g_nvmap; i++) if (g_vmap[i].va == va) { g_vmap[i] = (vmap_ent){va, cpu, size}; return; }
    if (g_nvmap < 512) g_vmap[g_nvmap++] = (vmap_ent){va, cpu, size};
}
int totk_vpool_backing(uintptr_t va, uint64_t len, uintptr_t *cpu) {
    pthread_mutex_lock(&g_map_mu); int r = 0;
    for (int i = 0; i < g_nvmap; i++) { const vmap_ent *e = &g_vmap[i]; if (e->cpu && va >= e->va && va + len <= e->va + e->size) { *cpu = e->cpu + (va - e->va); r = 1; break; } }
    pthread_mutex_unlock(&g_map_mu); return r;
}
static int apply_request(pool_t *v, const totk_mapping_request *r) {
    mach_vm_address_t dst = (mach_vm_address_t)(uintptr_t)(v->mem + r->virtual_offset);
    kern_return_t kr;
    if (!r->physical_pool) {
#ifdef GAME_MKW
        kr = hole_fill(v->mem + r->virtual_offset, r->size);
#else
        kr = mach_vm_allocate(mach_task_self(), &dst, r->size, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE);
#endif
    } else {
        const pool_t *p = (const pool_t *)(const void *)r->physical_pool;
        vm_prot_t cur, max;
        kr = mach_vm_remap(mach_task_self(), &dst, r->size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                           (mach_vm_address_t)(uintptr_t)(p->mem + r->physical_offset), FALSE, &cur, &max, VM_INHERIT_NONE);
#ifdef GAME_MKW
        /* The physical page may be write-protected by the dirty tracker (sparse tiles watch CPU writes there); the remap copies
           that protection, but the virtual view itself is not tracked: make it read-write. */
        if (kr == KERN_SUCCESS && (cur & VM_PROT_WRITE) == 0 && (max & (VM_PROT_READ | VM_PROT_WRITE)) == (VM_PROT_READ | VM_PROT_WRITE) &&
            mach_vm_protect(mach_task_self(), dst, r->size, FALSE, VM_PROT_READ | VM_PROT_WRITE) == KERN_SUCCESS) cur = VM_PROT_READ | VM_PROT_WRITE;
#endif
        if (kr == KERN_SUCCESS && (cur & (VM_PROT_READ | VM_PROT_WRITE)) != (VM_PROT_READ | VM_PROT_WRITE)) { LOG("【红】vpool: alias lost RW (prot=%d)", cur); return 0; }
        if (kr == KERN_SUCCESS) vmap_note((uintptr_t)(v->mem + r->virtual_offset), r->size, (uintptr_t)(p->mem + r->physical_offset));
    }
    if (!r->physical_pool && kr == KERN_SUCCESS) vmap_note((uintptr_t)(v->mem + r->virtual_offset), r->size, 0);
    if (kr != KERN_SUCCESS || dst != (mach_vm_address_t)(uintptr_t)(v->mem + r->virtual_offset)) {
        LOG("【红】vpool: remap at %#llx size %#llx failed kr=%d", (unsigned long long)dst, r->size, kr);
        return 0;
    }
    return 1;
}

#ifdef GAME_MKW
/* Sparse textures in a virtual pool are uploaded tile by tile from the pages mapped into it (nvn_mtl.m sparse_sync):
   each 64 KiB page records the generation of the request that last mapped it, 0 when unmapped. */
static void note_pages(pool_t *v, uint64_t voff, uint64_t size, uint8_t *src) {   // src: CPU address of the physical bytes behind voff (NULL: unmapped)
    if (!v->page_gen) return;
    uint64_t g = ++v->map_gen;
    for (uint64_t p = voff / TOTK_VPOOL_PAGE, e = (voff + size) / TOTK_VPOOL_PAGE, k = 0; p < e && p < v->page_n; p++, k++) {
        v->page_gen[p] = src ? (uint32_t)g : 0; if (v->page_src) v->page_src[p] = src ? src + k * TOTK_VPOOL_PAGE : NULL; }
}
#endif
int totk_vpool_map(pool_t *v, int count, const totk_mapping_request *r) {
    if (!v || v->magic != POOL_MAGIC || !(v->flags & F_VIRTUAL) || !v->mem) { LOG("【红】vpool: MapVirtual target %p is not a virtual pool", (void *)v); return 0; }
    if (count < 0 || (count && !r)) { LOG("【红】vpool: invalid request count %d", count); return 0; }
    if (!count) return 1;
    for (int i = 0; i < count; i++) if (!check_request(v, &r[i], i)) return 0;
#ifdef GAME_MKW
    { static int lg = -1; static unsigned n; if (lg < 0) { const char *e = getenv("MKW_VMAP_LOG"); lg = e && atoi(e); }
      if (lg && v->size >= (1ull << 30)) for (int i = 0; i < count && n < 4000; i++, n++)
          LOG("[vmap] pool %p size %#llx: %s voff %#llx size %#llx poff %#llx phys %p flags %#x size %#llx", (void *)v, (unsigned long long)v->size, r[i].physical_pool ? "map" : "unmap",
              (unsigned long long)r[i].virtual_offset, (unsigned long long)r[i].size, (unsigned long long)r[i].physical_offset, r[i].physical_pool,
              r[i].physical_pool ? ((const pool_t *)r[i].physical_pool)->flags : 0, r[i].physical_pool ? (unsigned long long)((const pool_t *)r[i].physical_pool)->size : 0ull); }
#endif
#ifdef GAME_MKW
    const pool_t *physical = count == 1 ? (const pool_t *)r[0].physical_pool : NULL;
    int whole = physical && !r[0].virtual_offset && !r[0].physical_offset && r[0].size == v->size && physical->size >= v->size;
    if (v->direct_alias) {
        if (whole && physical->mem == v->mem) return 1;
        LOG("[vpool] cannot change a borrowed contiguous mapping"); return 0;
    }
    if (whole && (uintptr_t)physical->mem % host_page() && (physical->flags & F_GPU_NO_ACCESS) && !v->address_used && !v->mapped) {
        pthread_mutex_lock(&g_map_mu);
        nvn_dirty_remove_range(v->mem); mtl_pool_del(v->mem);
        if (v->host) mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)v->host, v->span);
        v->host = NULL; v->span = 0; v->mem = physical->mem; v->direct_alias = v->mapped = 1;
        mtl_pool_add(v->mem, v->size); nvn_dirty_add_range(v->mem, v->size);
        vmap_note((uintptr_t)v->mem, v->size, (uintptr_t)physical->mem);
        pthread_mutex_unlock(&g_map_mu);
        LOG("[vpool] contiguous physical backing %p used before virtual address publication", (void *)physical->mem);
        note_pages(v, 0, v->size, physical->mem);
        return 1;
    }
    for (int i = 0; i < count; i++) if (r[i].physical_pool && (uintptr_t)((const pool_t *)r[i].physical_pool)->mem % host_page()) {
        LOG("[vpool] unaligned physical mapping requires an unused contiguous virtual pool"); return 0;
    }
#endif
    pthread_mutex_lock(&g_map_mu);
    // GPU page tables of the old zero-copy buffer and dirty-page protections refer to the replaced pages.
    nvn_dirty_remove_range(v->mem);
#ifdef GAME_MKW
    mtl_pool_del_keep_textures(v->mem);   // textures placed in the pool stay (nvn_mtl.m)
#else
    mtl_pool_del(v->mem);
#endif
    int ok = 1;
    for (int i = 0; ok && i < count; i++) ok = apply_request(v, &r[i]);
    if (ok && count) v->mapped = 1;
#ifdef GAME_MKW
    for (int i = 0; ok && i < count; i++) note_pages(v, (uint64_t)r[i].virtual_offset, r[i].size,
                                                     r[i].physical_pool ? ((const pool_t *)r[i].physical_pool)->mem + r[i].physical_offset : NULL);
#endif
    mtl_pool_add(v->mem, v->size);
    nvn_dirty_add_range(v->mem, v->size);
    pthread_mutex_unlock(&g_map_mu);
    if (!ok) die("vpool: mapping failed after validation; virtual pool %p is inconsistent", (void *)v);
    return 1;
}
