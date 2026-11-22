/* The guest address space and the memory SVCs of a libnx homebrew on the Zelda-line runtime.
   From games/mk8dx/runtime/services.c (third_party/COPIES.md): the reservation below 2^39 (HIPC buffer descriptors carry
   39 address bits), the heap (SetHeapSize), the stack mirrors (MapMemory as a scoped copy, 16 KiB host pages), GetInfo's
   regions. New here, for what libnx does that an SDK game does not: shared memory mapped where libnx chooses
   (MapSharedMemory: hid's input state), the JIT's code memory (CreateCodeMemory / ControlCodeMemory: one backing seen
   writable at one address and executable at another), and QueryMemory answering from a list of those mappings, which
   libnx's virtual memory manager reads to find free room. game_patches.c makes every place libnx picks 16 KiB aligned,
   so a mapping can alias host memory at Apple's page size. */
#include "runtime.h"
#include "ipc.h"
#include <TargetConditionals.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "runtime_tables.h"

#define GUEST_ADDRESS_LIMIT (1ULL << 39)
#define SEARCH_FROM 0x300000000ULL
#define STACK_OFF 0x10000000ULL
#define STACK_SIZE 0x10000000ULL
#define HEAP_OFF (STACK_OFF + STACK_SIZE + 0x10000000ULL)
#define HEAP_CAP (4ULL << 30)
#define ALIAS_OFF (HEAP_OFF + HEAP_CAP)
#define ALIAS_SIZE (1ULL << 30)                      /* reported, never mapped: libnx only keeps its picks out of it */
#define FREE_OFF (ALIAS_OFF + ALIAS_SIZE)
#if TARGET_OS_IPHONE
#define SPACE_SIZE (FREE_OFF + (2ULL << 30))
#else
#define SPACE_SIZE (FREE_OFF + (8ULL << 30))
#endif
#define APP_POOL 0xcd500000ULL                       /* the application pool a retail game sees (3.2 GiB) */
#define HOST_PAGE 0x4000ULL

static uint8_t *space, *heap;
static size_t heap_size;
static pthread_mutex_t memory_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t page_up16(uint64_t x) { return (x + HOST_PAGE - 1) & ~(HOST_PAGE - 1); }

/* Guest mappings other than the heap: what QueryMemory reports as used. */
typedef struct { uint64_t base, size; uint32_t state, perm; } Mapping;
#define MAX_MAPPINGS 4096
static Mapping maps[MAX_MAPPINGS]; static unsigned nmaps;
static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a < b + m && b < a + n; }

static void memory_init(void) {
 if (space) return;
 for (uint64_t at = SEARCH_FROM; at + SPACE_SIZE <= GUEST_ADDRESS_LIMIT && !space; at += 1ULL << 30) {
  mach_vm_address_t a = at;   /* FIXED without OVERWRITE: fails instead of replacing an existing mapping */
  if (mach_vm_map(mach_task_self(), &a, SPACE_SIZE, 0, VM_FLAGS_FIXED, MEMORY_OBJECT_NULL, 0, FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_DEFAULT) == KERN_SUCCESS) space = (uint8_t *)(uintptr_t)a;
 }
 if (!space) runtime_fail("reserve the guest address space below 2^39");
 heap = space + HEAP_OFF;
 fprintf(stderr, "[totk] guest layout: space %p+%#llx stack +%#llx heap %p (cap %#llx) alias +%#llx\n", space, SPACE_SIZE, STACK_OFF, heap, HEAP_CAP, ALIAS_OFF);
}
uint64_t runtime_heap_base(void) { return (uintptr_t)heap; }
static int in_space(uint64_t a, uint64_t n) { uint64_t s = (uintptr_t)space; return a >= s && n <= SPACE_SIZE && a - s <= SPACE_SIZE - n; }
static void back_to_reserved(uint64_t a, uint64_t n) {   /* hand a range back: no access, no memory behind it */
 if (mmap((void *)(uintptr_t)a, n, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) != (void *)(uintptr_t)a) runtime_fail("release guest range");
}
static int map_add(uint64_t base, uint64_t size, uint32_t state, uint32_t perm) {
 if (nmaps == MAX_MAPPINGS) return 0;
 for (unsigned i = 0; i < nmaps; i++) if (overlaps(base, size, maps[i].base, maps[i].size)) return 0;
 maps[nmaps++] = (Mapping){base, size, state, perm}; return 1;
}
static void map_remove(uint64_t base) { for (unsigned i = 0; i < nmaps; i++) if (maps[i].base == base) { maps[i] = maps[--nmaps]; return; } }

/* ---------- MapMemory / UnmapMemory: a thread stack's mirror in the stack region (a scoped copy, as in Zelda) ---------- */
typedef struct { uint64_t dst, src, size; } StackMap;
static StackMap stack_maps[1024]; static unsigned nstack_maps;
static uint64_t stack_move(int unmap, uint64_t dst, uint64_t src, uint64_t size) {
 uint64_t stack_start = (uintptr_t)space + STACK_OFF;
 if (!size || ((dst | src | size) & 4095)) return 0xcc01;
 if (dst < stack_start || size > STACK_SIZE || dst - stack_start > STACK_SIZE - size) return 0xdc01;
 if (!(src >= (uintptr_t)heap && size <= heap_size && src - (uintptr_t)heap <= heap_size - size)) return 0xdc01;
 unsigned found = nstack_maps; for (unsigned i = 0; i < nstack_maps; i++) if (stack_maps[i].dst == dst && stack_maps[i].src == src && stack_maps[i].size == size) found = i;
 uint64_t lo = dst & ~(HOST_PAGE - 1), hi = page_up16(dst + size), slo = page_up16(src), shi = (src + size) & ~(HOST_PAGE - 1);
 if (unmap) {
  if (found == nstack_maps) return 0xd401;
  if (shi > slo && mprotect((void *)slo, shi - slo, PROT_READ | PROT_WRITE)) runtime_fail("restore moved stack source");
  memcpy((void *)src, (const void *)dst, size); stack_maps[found] = stack_maps[--nstack_maps]; map_remove(dst);
  for (uint64_t page = lo; page < hi; page += HOST_PAGE) {
   int used = 0; for (unsigned i = 0; i < nstack_maps; i++) if (overlaps(page, HOST_PAGE, stack_maps[i].dst, stack_maps[i].size)) used = 1;
   if (!used) back_to_reserved(page, HOST_PAGE);
  }
 } else {
  if (nstack_maps == sizeof stack_maps / sizeof *stack_maps) return 0xd401;
  for (unsigned i = 0; i < nstack_maps; i++) if (overlaps(dst, size, stack_maps[i].dst, stack_maps[i].size) || overlaps(src, size, stack_maps[i].src, stack_maps[i].size)) return 0xdc01;
  if (!map_add(dst, size, 0xb, 3)) return 0xdc01;
  if (mprotect((void *)lo, hi - lo, PROT_READ | PROT_WRITE)) runtime_fail("commit moved stack");
  memcpy((void *)dst, (const void *)src, size);
  if (shi > slo && mprotect((void *)slo, shi - slo, PROT_NONE)) runtime_fail("protect moved stack source");
  stack_maps[nstack_maps++] = (StackMap){dst, src, size};
 }
 return 0;
}

/* ---------- shared memory and code memory: one host backing mapped at the guest's address ---------- */
static uint64_t alias_at(uint64_t addr, uint64_t size, uint64_t host, int prot, uint32_t state, uint32_t perm) {
 if (addr & (HOST_PAGE - 1)) { fprintf(stderr, "[totk] mapping at %#llx is not 16 KiB aligned\n", (unsigned long long)addr); return 0xcc01; }
 uint64_t n = page_up16(size);
 if (!in_space(addr, n)) return 0xd401;
 if (!map_add(addr, size, state, perm)) return 0xdc01;
 mach_vm_address_t a = addr; vm_prot_t cur, max;
 kern_return_t kr = mach_vm_remap(mach_task_self(), &a, n, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), host, FALSE, &cur, &max, VM_INHERIT_NONE);
 if (kr != KERN_SUCCESS || a != addr) { fprintf(stderr, "[totk] remap %#llx failed %d\n", (unsigned long long)addr, kr); runtime_fail("alias guest mapping"); }
 if (mprotect((void *)(uintptr_t)addr, n, prot)) runtime_fail("guest mapping permission");
 return 0;
}
static uint64_t alias_remove(uint64_t addr, uint64_t size) {
 unsigned i = 0; while (i < nmaps && maps[i].base != addr) i++;
 if (i == nmaps || maps[i].size != size) return 0xd401;
 map_remove(addr); back_to_reserved(addr, page_up16(size)); return 0;
}
void *runtime_guest_map_shared(uint64_t addr, uint64_t size, void *host, int writable) {
 memory_init(); pthread_mutex_lock(&memory_lock);
 uint64_t r = alias_at(addr, size, (uintptr_t)host, PROT_READ | (writable ? PROT_WRITE : 0), 6, writable ? 3 : 1);
 pthread_mutex_unlock(&memory_lock); return r ? NULL : (void *)(uintptr_t)addr;
}
typedef struct { uint32_t magic; uint64_t size; void *host; } SharedMemory;   /* handle objects (O_TMEM handles in threads.c) */
#define SHMEM_MAGIC 0x4d454853u
#define CODEMEM_MAGIC 0x4d444f43u
uint32_t runtime_shared_memory_new(uint64_t size, void **host) {
 SharedMemory *s = calloc(1, sizeof *s); if (!s) runtime_fail("shared memory object");
 mach_vm_address_t a = 0; if (mach_vm_allocate(mach_task_self(), &a, page_up16(size), VM_FLAGS_ANYWHERE) != KERN_SUCCESS) runtime_fail("shared memory backing");
 *s = (SharedMemory){SHMEM_MAGIC, size, (void *)(uintptr_t)a}; *host = s->host;
 return runtime_handle_new(0, s);
}
typedef struct { uint32_t magic; uint64_t src, size; void *host; uint64_t owner, slave; } CodeMemory;

static uint64_t query(uint64_t addr, uint64_t out) {
 uint64_t base, size; unsigned state = 0, perm = 0, attr = 0;
 uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(pthread_self()); size_t stack = pthread_get_stacksize_np(pthread_self());
 if (runtime_module_query(addr, &base, &size, &state, &perm, &attr)) {}
 else if (addr >= top - stack && addr < top) { base = top - stack; size = stack; state = 0xb; perm = 3; }
 else if (!in_space(addr, 1)) { base = addr & ~0xfffULL; size = 0x1000; state = 0x10; perm = 0; }   /* the host's: Inaccessible */
 else if (addr >= (uintptr_t)heap && addr < (uintptr_t)heap + heap_size) { base = (uintptr_t)heap; size = heap_size; state = 5; perm = 3; }
 else {
  pthread_mutex_lock(&memory_lock);
  unsigned hit = nmaps; for (unsigned i = 0; i < nmaps; i++) if (addr >= maps[i].base && addr - maps[i].base < maps[i].size) hit = i;
  if (hit < nmaps) { base = maps[hit].base; size = maps[hit].size; state = maps[hit].state; perm = maps[hit].perm; }
  else {   /* free: up to the nearest used range on either side */
   uint64_t lo = (uintptr_t)space, hi = (uintptr_t)space + SPACE_SIZE, hs = (uintptr_t)heap, he = hs + heap_size;
   if (heap_size) { if (he <= addr && he > lo) lo = he; if (hs > addr && hs < hi) hi = hs; }
   for (unsigned i = 0; i < nmaps; i++) { uint64_t e = page_up16(maps[i].base + maps[i].size);
    if (e <= addr && e > lo) lo = e; if (maps[i].base > addr && maps[i].base < hi) hi = maps[i].base; }
   base = lo; size = hi - lo;
  }
  pthread_mutex_unlock(&memory_lock);
 }
 struct { uint64_t base, size; uint32_t state, attr, perm, ipc, device, pad; } info = {base, size, state, attr, perm, 0, 0, 0};
 if (out) memcpy((void *)(uintptr_t)out, &info, sizeof info);
 return 0;
}

static uint64_t get_info(unsigned id, uint64_t *value) {
 switch (id) {
 case 0: *value = 7; return 0;                                    /* CoreMask: cores 0-2, an application's */
 case 1: *value = 0xfffffffff0000000ULL; return 0;               /* PriorityMask */
 case 2: *value = (uintptr_t)space + ALIAS_OFF; return 0; case 3: *value = ALIAS_SIZE; return 0;
 case 4: *value = (uintptr_t)heap; return 0; case 5: *value = HEAP_CAP; return 0;
 case 6: *value = APP_POOL; return 0;                            /* TotalMemorySize */
 case 7: *value = runtime_image_bytes() + heap_size; return 0;  /* UsedMemorySize */
 case 8: case 10: *value = 0; return 0;
 case 11: arc4random_buf(value, 8); return 0;                    /* RandomEntropy */
 case 12: *value = (uintptr_t)space; return 0; case 13: *value = SPACE_SIZE; return 0;   /* ASLR region: the whole space */
 case 14: *value = (uintptr_t)space + STACK_OFF; return 0; case 15: *value = STACK_SIZE; return 0;
 case 16: *value = 0; return 0;                                  /* SystemResourceSizeTotal */
 case 17: *value = 0; return 0;
 case 18: *value = GAME_TITLE_ID; return 0;                      /* ProgramId */
 case 20: *value = 0; return 0;                                  /* UserExceptionContextAddress: none */
 case 21: *value = APP_POOL; return 0; case 22: *value = runtime_image_bytes() + heap_size; return 0;   /* ...WithoutSystemResource */
 case 23: *value = 1; return 0;                                  /* IsApplication */
 case 28: *value = 0; return 0;                                  /* AliasRegionExtraSize */
 default: fprintf(stderr, "[totk] GetInfo id %u not implemented\n", id); return 0xf001;
 }
}

/* Returns 1 when the SVC is one of these. */
int svc_memory(unsigned num, uint64_t *r) {
 memory_init();
 switch (num) {
 case 0x01: {   /* SetHeapSize(x1 size) -> x1 address */
  size_t size = r[1];
  if (size > HEAP_CAP || size % 0x200000 || size + runtime_image_bytes() > APP_POOL) { r[0] = 0xd401; return 1; }
  pthread_mutex_lock(&memory_lock);
  if (size > heap_size && mprotect(heap + heap_size, size - heap_size, PROT_READ | PROT_WRITE)) runtime_fail("commit heap");
  if (size < heap_size) back_to_reserved((uintptr_t)heap + size, heap_size - size);
  heap_size = size; pthread_mutex_unlock(&memory_lock);
  fprintf(stderr, "[totk] SetHeapSize %#zx at %p\n", size, heap);
  r[0] = 0; r[1] = (uintptr_t)heap; return 1;
 }
 case 0x02: r[0] = runtime_module_permission(r[0], r[1], (unsigned)r[2]); return 1;
 case 0x03: r[0] = runtime_module_lock_permission(r[0], r[1], (unsigned)r[2], (unsigned)r[3]); return 1;
 case 0x04: case 0x05: pthread_mutex_lock(&memory_lock); r[0] = stack_move(num == 5, r[0], r[1], r[2]); pthread_mutex_unlock(&memory_lock); return 1;
 case 0x06: r[0] = query(r[2], r[0]); r[1] = 0; return 1;   /* QueryMemory(x0 info out, x2 address) -> w1 page info */
 case 0x13: {   /* MapSharedMemory(w0 handle, x1 address, x2 size, w3 permission) */
  SharedMemory *s = runtime_handle_get((uint32_t)r[0], 0);
  if (!s || s->magic != SHMEM_MAGIC) { r[0] = 0xe401; return 1; }
  if (r[2] != s->size) { r[0] = 0xca01; return 1; }
  pthread_mutex_lock(&memory_lock); r[0] = alias_at(r[1], r[2], (uintptr_t)s->host, PROT_READ | ((r[3] & 2) ? PROT_WRITE : 0), 6, (uint32_t)r[3]); pthread_mutex_unlock(&memory_lock);
  return 1;
 }
 case 0x14: {   /* UnmapSharedMemory(w0 handle, x1 address, x2 size) */
  pthread_mutex_lock(&memory_lock); r[0] = alias_remove(r[1], r[2]); pthread_mutex_unlock(&memory_lock); return 1;
 }
 case 0x4b: {   /* CreateCodeMemory(x1 address, x2 size) -> w1 handle. The source's contents are not used (libnx clears it). */
  if (!r[2] || ((r[1] | r[2]) & 4095)) { r[0] = 0xcc01; return 1; }
  CodeMemory *c = calloc(1, sizeof *c); if (!c) runtime_fail("code memory object");
  mach_vm_address_t a = 0; if (mach_vm_allocate(mach_task_self(), &a, page_up16(r[2]), VM_FLAGS_ANYWHERE) != KERN_SUCCESS) runtime_fail("code memory backing");
  *c = (CodeMemory){CODEMEM_MAGIC, r[1], r[2], (void *)(uintptr_t)a, 0, 0};
  r[0] = 0; r[1] = runtime_handle_new(0, c);
  fprintf(stderr, "[totk] CreateCodeMemory %#llx bytes (source %#llx)\n", (unsigned long long)c->size, (unsigned long long)c->src);
  return 1;
 }
 case 0x4c: {   /* ControlCodeMemory(w0 handle, w1 operation, x2 address, x3 size, w4 permission) */
  CodeMemory *c = runtime_handle_get((uint32_t)r[0], 0);
  if (!c || c->magic != CODEMEM_MAGIC) { r[0] = 0xe401; return 1; }
  unsigned op = (unsigned)r[1]; uint64_t addr = r[2], size = r[3], perm = r[4];
  if (size != c->size) { r[0] = 0xca01; return 1; }
  pthread_mutex_lock(&memory_lock);
  if (op == 0 && perm == 3 && !c->owner) { r[0] = alias_at(addr, size, (uintptr_t)c->host, PROT_READ | PROT_WRITE, 0x13, 3); if (!r[0]) c->owner = addr; }        /* MapOwner: CodeMutable */
  else if (op == 1 && perm == 5 && !c->slave) {   /* MapSlave: Code */
   r[0] = alias_at(addr, size, (uintptr_t)c->host, PROT_READ | PROT_EXEC, 0x12, 5);
   if (!r[0]) { c->slave = addr;
    if (c->owner) {   /* the two views are one memory: a word written through the writable one reads back through the other */
     volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)c->owner, *x = (volatile uint32_t *)(uintptr_t)addr; uint32_t keep = *w;
     *w = 0x5a5aa5a5u; if (*x != 0x5a5aa5a5u) runtime_fail("code memory views do not share memory"); *w = keep; } } }
  else if (op == 2 && c->owner == addr) { r[0] = alias_remove(addr, size); c->owner = 0; }
  else if (op == 3 && c->slave == addr) { r[0] = alias_remove(addr, size); c->slave = 0; }
  else r[0] = 0xe601;
  pthread_mutex_unlock(&memory_lock);
  fprintf(stderr, "[totk] ControlCodeMemory op=%u %#llx+%#llx perm=%llu -> %#llx\n", op, (unsigned long long)addr, (unsigned long long)size, (unsigned long long)perm, (unsigned long long)r[0]);
  return 1;
 }
 case 0x29: {   /* GetInfo(w1 id, w2 handle, x3 sub id) -> x1 */
  uint64_t value = 0; r[0] = get_info((unsigned)r[1], &value); r[1] = value; return 1;
 }
 default: return 0;
 }
}

/* Diagnostics (TOTK_INPUT_FILE "hbfind <text>"): where a text is in the game's heap, with the printable text around it --
   the game's own error messages (its emulator keeps them in heap buffers) are readable this way. */
void hb_heap_find(const char *text) {
 size_t n = strlen(text), hits = 0; if (!n || !heap) return;
 for (uint8_t *p = heap; p + n <= heap + heap_size && hits < 16; p++) {
  if (*p != (uint8_t)text[0] || memcmp(p, text, n)) continue;
  uint8_t *a = p, *b = p + n; while (a > heap && a > p - 200 && a[-1] >= 0x20 && a[-1] < 0x7f) a--; while (b < heap + heap_size && b < p + 400 && *b >= 0x20 && *b < 0x7f) b++;
  fprintf(stderr, "[hbfind] heap+%#llx: %.*s\n", (unsigned long long)(p - heap), (int)(b - a), a); hits++; p = b;
 }
 fprintf(stderr, "[hbfind] \"%s\": %zu hit(s)\n", text, hits);
}
/* Diagnostics (TOTK_INPUT_FILE "hbdword <hex>"): where a 32-bit value is in the heap (4-byte aligned), with the 24 words
   after it -- an x86 return address finds the emulated game's stack. */
void hb_heap_dword(uint32_t value) {
 unsigned hits = 0; if (!heap) return;
 for (uint32_t *p = (uint32_t *)heap; (uint8_t *)(p + 24) <= heap + heap_size && hits < 8; p++) {
  if (*p != value) continue;
  fprintf(stderr, "[hbdword] heap+%#llx:", (unsigned long long)((uint8_t *)p - heap));
  for (unsigned i = 0; i < 24; i++) fprintf(stderr, " %08x", p[i]);
  fputc('\n', stderr); hits++;
 }
 fprintf(stderr, "[hbdword] %08x: %u hit(s)\n", value, hits);
}
