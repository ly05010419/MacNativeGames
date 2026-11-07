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

int totk_vpool_init(pool_t *o) {
    int phys = (o->flags & F_PHYSICAL) != 0, virt = (o->flags & F_VIRTUAL) != 0;
    o->game_mem = o->mem; o->host = NULL; o->span = 0;
    if (phys == virt) { LOG("【红】vpool: pool flags %#x must be exactly one of PHYSICAL/VIRTUAL", o->flags); return 0; }
    if (!o->size || o->size % TOTK_VPOOL_PAGE) { LOG("【红】vpool: pool size %#llx not a multiple of 64 KiB", o->size); return 0; }
    if (virt && o->mem) { LOG("【红】vpool: virtual pool given storage %p", (void *)o->mem); return 0; }
    if (phys && !o->mem) { LOG("【红】vpool: physical pool without storage"); return 0; }
    if (phys && !(o->flags & F_CPU_NO_ACCESS)) {
#ifdef GAME_NS2
        /* CPU-only physical storage (GPU_NO_ACCESS) the GPU reaches through a virtual pool only (Ocarina of Time: 305 MB, flags
           0x18c, 4 KiB aligned): kept as the caller's storage, alignment included (a copy would break CPU coherence); a whole
           contiguous mapping then makes the virtual pool that storage (totk_vpool_map). From games/mkw. */
        if ((uintptr_t)o->mem % host_page() && (o->flags & F_GPU_NO_ACCESS)) return 1;
#endif
        if ((uintptr_t)o->mem % host_page()) { LOG("【红】vpool: CPU-visible physical pool %p (size %#llx, flags %#x) not %#llx aligned; cannot alias", (void *)o->mem, (unsigned long long)o->size, o->flags, host_page()); return 0; }
        return 1;   // aliases the game storage directly
    }
    o->span = round_up(o->size, host_page());
    o->host = vm_fresh(o->span);
    if (!o->host) return 0;
    o->mem = o->host;
    return 1;
}

int totk_vpool_fini(pool_t *o) {
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
        kr = mach_vm_allocate(mach_task_self(), &dst, r->size, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE);
    } else {
        const pool_t *p = (const pool_t *)(const void *)r->physical_pool;
        vm_prot_t cur, max;
        kr = mach_vm_remap(mach_task_self(), &dst, r->size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                           (mach_vm_address_t)(uintptr_t)(p->mem + r->physical_offset), FALSE, &cur, &max, VM_INHERIT_NONE);
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

int totk_vpool_map(pool_t *v, int count, const totk_mapping_request *r) {
    if (!v || v->magic != POOL_MAGIC || !(v->flags & F_VIRTUAL) || !v->mem) { LOG("【红】vpool: MapVirtual target %p is not a virtual pool", (void *)v); return 0; }
    if (count < 0 || (count && !r)) { LOG("【红】vpool: invalid request count %d", count); return 0; }
    for (int i = 0; i < count; i++) if (!check_request(v, &r[i], i)) return 0;
#ifdef GAME_NS2
    /* A physical pool that could not be aliased page by page (16 KiB host pages, 4 KiB aligned storage): mapped whole into an
       unused virtual pool, the virtual pool becomes that storage (games/mkw's direct alias; before any GPU address of the
       virtual pool was handed out, so nothing holds the old one). Any other mapping of such storage is refused. */
    { const pool_t *physical = count == 1 ? (const pool_t *)r[0].physical_pool : NULL;
      int whole = physical && !r[0].virtual_offset && !r[0].physical_offset && r[0].size == v->size && physical->size >= v->size;
      if (v->direct_alias) { if (whole && physical->mem == v->mem) return 1; LOG("【红】vpool: cannot change a virtual pool that became its physical storage"); return 0; }
      if (whole && (uintptr_t)physical->mem % host_page() && (physical->flags & F_GPU_NO_ACCESS) && !v->address_used && !v->mapped) {
          pthread_mutex_lock(&g_map_mu);
          nvn_dirty_remove_range(v->mem); mtl_pool_del(v->mem);
          if (v->host) mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)(uintptr_t)v->host, v->span);
          v->host = NULL; v->span = 0; v->mem = physical->mem; v->direct_alias = v->mapped = 1;
          mtl_pool_add(v->mem, v->size); nvn_dirty_add_range(v->mem, v->size); mtl_pool_addressed(v->mem, v->size);
          vmap_note((uintptr_t)v->mem, v->size, (uintptr_t)physical->mem);
          pthread_mutex_unlock(&g_map_mu);
          LOG("[vpool] virtual pool %p (%#llx bytes) is its contiguous physical storage %p (not 16 KiB aligned)", (void *)v, (unsigned long long)v->size, (void *)physical->mem);
          return 1; }
      for (int i = 0; i < count; i++) if (r[i].physical_pool && (uintptr_t)((const pool_t *)r[i].physical_pool)->mem % host_page()) {
          LOG("【红】vpool: a physical pool not 16 KiB aligned mapped other than whole into an unused virtual pool (request %d)", i); return 0; } }
#endif
    pthread_mutex_lock(&g_map_mu);
    // GPU page tables of the old zero-copy buffer and dirty-page protections refer to the replaced pages.
    nvn_dirty_remove_range(v->mem);
    mtl_pool_del(v->mem);
    int ok = 1;
    for (int i = 0; ok && i < count; i++) ok = apply_request(v, &r[i]);
    mtl_pool_add(v->mem, v->size);
    nvn_dirty_add_range(v->mem, v->size);
    if (count) mtl_pool_addressed(v->mem, v->size);   // an address may have been given out before it had storage
    pthread_mutex_unlock(&g_map_mu);
    if (!ok) die("vpool: mapping failed after validation; virtual pool %p is inconsistent", (void *)v);
    if (count) v->mapped = 1;
    return 1;
}
