/* Diagnostic probe table for CAllocator / SIGBUS. Restores or emulates displaced
   insns — never pc+=4 over a skipped original. No game nops. */
#include "runtime.h"
#include "game_patches.h"
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include "mach_vm_compat.h"
#include <dlfcn.h>
#include <unistd.h>

extern int nvn_dirty_tracked(const void *addr, size_t len);

#define BRK_IMM(id) (0xD4200000u | ((unsigned)(id) << 5))
enum { K_ONESHOT = 0, K_BLR = 1, K_STP = 2, K_ADRP = 3, K_KEEP_MADD = 4, K_KEEP_STP16 = 5, K_EMU = 6 };

static uint8_t *main_base;
static uint8_t *sdk_base;
static size_t main_size;
static uint64_t block_lo, block_hi;
static uint64_t first_pool;
static unsigned hits[64];
static unsigned fail_n;

static struct {
 uint64_t off;
 uint32_t orig;
 int kind;
 int mod; /* 0 main, 1 sdk */
 const char *name;
} sites[] = {
// DISABLED-FOR-CLEAN-BOOT  {0x10f1b0, 0, K_STP,   0, "entry-10f1b0"},
// DISABLED-FOR-CLEAN-BOOT  {0x10f26c, 0, K_ONESHOT, 0, "body-10f26c"},
// DISABLED-FOR-CLEAN-BOOT  {0x10f280, 0, K_ONESHOT, 0, "ldr-x8-10f280"},
// DISABLED-FOR-CLEAN-BOOT  {0x10f2a4, 0, K_BLR,   0, "blr-x8-10f2a4"},
// DISABLED-FOR-CLEAN-BOOT  {0x10f2a8, 0, K_ONESHOT, 0, "after-10f2a8"},
// DISABLED-FOR-CLEAN-BOOT  {0x94074,  0, K_ONESHOT, 0, "tbnz-94074"},
// DISABLED-FOR-CLEAN-BOOT  {0x94100,  0, K_ONESHOT, 0, "bl-94100"},
 {0x1100b8, 0, K_ADRP,  0, "allocfail-1100b8"},
 {0x110038, 0, K_EMU, 0, "alloc-entry"},
// DISABLED-FOR-CLEAN-BOOT  {0x10fc30, 0, K_ONESHOT, 0, "walk-10fc30"},
// DISABLED-FOR-CLEAN-BOOT  {0xfd80c,  0, K_KEEP_MADD, 0, "madd-fd80c"},
// DISABLED-FOR-CLEAN-BOOT  {0xfd810,  0, K_KEEP_STP16, 0, "zero-fd810"},
// DISABLED-FOR-CLEAN-BOOT  {0xea93c,  0, K_ONESHOT, 0, "createthread-ea93c"},
// DISABLED-FOR-CLEAN-BOOT  {0x95c5c,  0, K_ONESHOT, 1, "diagprint-95c5c"},
// DISABLED-FOR-CLEAN-BOOT  {0x95c30,  0, K_ONESHOT, 1, "diagprint-95c30"},
// DISABLED-FOR-CLEAN-BOOT  {0x5ccadc, 0, K_ONESHOT, 1, "plt-5ccadc"},
// DISABLED-FOR-CLEAN-BOOT  {0x5c4afc, 0, K_ONESHOT, 1, "plt-5c4afc"},
// DISABLED-FOR-CLEAN-BOOT  {0x5ccdcc, 0, K_ONESHOT, 1, "plt-mapalias"},
// DISABLED-FOR-CLEAN-BOOT  {0x1334c0, 0, K_ONESHOT, 1, "allocspace-ret"},
// DISABLED-FOR-CLEAN-BOOT  {0x1334e8, 0, K_ONESHOT, 1, "checkguard-call"},
// DISABLED-FOR-CLEAN-BOOT  {0x1b2d0c, 0, K_ONESHOT, 1, "factory-1b2d0c"},
// DISABLED-FOR-CLEAN-BOOT  {0xd9860,  0, K_ONESHOT, 1, "rommount-d9860"},
// DISABLED-FOR-CLEAN-BOOT  {0xd8978,  0, K_ONESHOT, 1, "rommount-ret"},
// DISABLED-FOR-CLEAN-BOOT  {0x93cd0,  0, K_EMU, 0, "romcache-alloc"},
// DISABLED-FOR-CLEAN-BOOT  {0x93cf4,  0, K_EMU, 0, "romcache-ret"},
// DISABLED-FOR-CLEAN-BOOT  {0x93cf0,  0, K_EMU, 0, "romcache-call"},
// DISABLED-FOR-CLEAN-BOOT  {0x99640,  0, K_EMU, 0, "abort-tbz"},
// DISABLED-FOR-CLEAN-BOOT  {0x1117f0, 0, K_ONESHOT, 0, "memcpy-loop"},
// DISABLED-FOR-CLEAN-BOOT  {0xdef38,  0, K_EMU, 0, "fmtswitch-call"},
 {0xdef18,  0, K_EMU, 0, "s-load"},
 {0xe3ca0,  0, K_ONESHOT, 0, "e3ca0-entry"},
 {0xe1bc0,  0, K_EMU, 0, "e1bc0-entry"},
 {0xe2740,  0, K_EMU, 0, "e2740-call"},
 {0x49cc0,  0, K_ONESHOT, 1, "virtcall-49cc0"},
 {0xe2744,  0, K_ONESHOT, 0, "e2744-ret"},
 {0x95c40,  0, K_ONESHOT, 1, "abortfn-95c40"},
 {0x3cf158, 0, K_ONESHOT, 1, "time-notinit"},
 {0x95c60,  0, K_ONESHOT, 1, "abortfn-95c60"},
// DISABLED-FOR-CLEAN-BOOT  {0xdeedc,  0, K_ONESHOT, 0, "fmtobj-entry"},
// DISABLED-FOR-CLEAN-BOOT  {0xdef40,  0, K_ONESHOT, 0, "vtable-ret"},
};

static void poke_rx(uint8_t *p, uint32_t w) {
 uintptr_t page = (uintptr_t)p & ~(uintptr_t)0x3fff;
 mprotect((void *)page, 0x4000, PROT_READ | PROT_WRITE);
 memcpy(p, &w, 4);
 sys_icache_invalidate(p, 4);
 mprotect((void *)page, 0x4000, PROT_READ | PROT_EXEC);
}

static uint64_t g_wp2a_this = 0;
uint64_t wp2a_pool_this(void) { return g_wp2a_this; }
static int read_mem(uint64_t a, void *dst, size_t n) {
 mach_vm_size_t got = 0;
 return a && mach_vm_read_overwrite(mach_task_self(), a, n, (uintptr_t)dst, &got) == KERN_SUCCESS && got == n;
}

static void dump_words(const char *tag, uint64_t a, unsigned nbytes) {
 uint8_t buf[160];
 unsigned n = nbytes > sizeof buf ? (unsigned)sizeof buf : nbytes;
 fprintf(stderr, "[snap] %s @ 0x%llx\n", tag, (unsigned long long)a);
 if (!read_mem(a, buf, n)) { fprintf(stderr, "[snap]  unreadable\n"); return; }
 for (unsigned i = 0; i + 8 <= n; i += 8)
  fprintf(stderr, "[snap]  +%02x %016llx\n", i, (unsigned long long)*(uint64_t *)(buf + i));
}

static void dump_cstr(const char *tag, uint64_t a) {
 uint8_t buf[256];
 fprintf(stderr, "[snap] %s @ 0x%llx: ", tag, (unsigned long long)a);
 if (!a || !read_mem(a, buf, sizeof buf)) { fprintf(stderr, "unreadable\n"); return; }
 buf[sizeof buf - 1] = 0;
 size_t n = 0; while (n < sizeof buf && buf[n]) n++;
 if (n == sizeof buf) { fprintf(stderr, "(not NUL-terminated in 256B)\n"); return; }
 int printable = 1;
 for (size_t i = 0; i < n; i++) if (buf[i] < 0x20 && buf[i] != 0x0a || buf[i] >= 0x7f) printable = 0;
 if (printable) fprintf(stderr, "\"%.*s\"\n", (int)n, buf);
 else { fprintf(stderr, "non-text %zuB: ", n); for (size_t i = 0; i < n && i < 64; i++) fprintf(stderr, "%02x", buf[i]); fprintf(stderr, "\n"); }
}

static void symbolize(const char *tag, uint64_t a) {
 Dl_info inf; memset(&inf, 0, sizeof inf);
 int ok = dladdr((void *)(uintptr_t)a, &inf);
 fprintf(stderr, "[snap] %s %llx", tag, (unsigned long long)a);
 if (main_base && a >= (uint64_t)main_base && a < (uint64_t)main_base + main_size)
  fprintf(stderr, " main+%llx", (unsigned long long)(a - (uint64_t)main_base));
 if (ok && inf.dli_fname)
  fprintf(stderr, " %s+%llx %s", inf.dli_fname,
   (unsigned long long)(a - (uint64_t)inf.dli_fbase),
   inf.dli_sname ? inf.dli_sname : "?");
 fprintf(stderr, "\n");
}

static void region_of(const char *tag, uint64_t a) {
 mach_vm_address_t ra = a; mach_vm_size_t rs = 0;
 vm_region_basic_info_data_64_t ri; mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t ro = 0;
 fprintf(stderr, "[snap] %s %llx", tag, (unsigned long long)a);
 if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&ri, &rc, &ro) == KERN_SUCCESS)
  fprintf(stderr, " region=%llx size=%llx prot=%x/%x off=%llx\n",
   (unsigned long long)ra, (unsigned long long)rs, ri.protection, ri.max_protection,
   (unsigned long long)(a - ra));
 else fprintf(stderr, " unmapped\n");
}

static const char *far_domain(uint64_t far) {
 if (main_base && far >= (uint64_t)main_base && far < (uint64_t)main_base + main_size) return "guest-main";
 if (block_lo && far >= block_lo && far < block_hi) return "stdalloc-block";
 if (far >= 0x7000000000ull && far < 0x7100000000ull) return "os-heap-window";
 if (nvn_dirty_tracked((void *)(uintptr_t)far, 1)) return "nvn-dirty-tracked";
 return "other";
}

static void pool_one(uint64_t a, int i) {
 uint8_t b[0xc0];
 if (!read_mem(a, b, sizeof b)) { fprintf(stderr, "[snap] pool[%d] %llx unreadable\n", i, (unsigned long long)a); return; }
 char name[24]; memcpy(name, b + 0x48, 16); name[16] = 0;
 uint64_t en = *(uint64_t *)b, base = *(uint64_t *)(b + 0x20), sz = *(uint64_t *)(b + 0x28);
 uint32_t cnt = *(uint32_t *)(b + 0x38), blk = *(uint32_t *)(b + 0x40);
 uint64_t tab = *(uint64_t *)(b + 0x58), map = *(uint64_t *)(b + 0x68);
 uint64_t cb = *(uint64_t *)(b + 0x78), cb2 = *(uint64_t *)(b + 0x88);
 fprintf(stderr, "[snap] pool[%d] %llx en=%llx name='%s' base=%llx sz=%llx cnt=%x blk=%x tab=%llx map=%llx cb78=%llx cb88=%llx%s\n",
  i, (unsigned long long)a, (unsigned long long)en, name,
  (unsigned long long)base, (unsigned long long)sz, cnt, blk,
  (unsigned long long)tab, (unsigned long long)map,
  (unsigned long long)cb, (unsigned long long)cb2,
  (tab && !map) || (en && !tab) ? " HALF" : "");
}

static void pool_scan(uint64_t any) {
 if (!any) return;
 uint64_t start = any;
 for (int n = 0; n < 32; n++) {
  uint64_t p = any - (uint64_t)n * 0x1c8;
  uint64_t en = 0;
  if (!read_mem(p, &en, 8)) break;
  start = p;
 }
 fprintf(stderr, "[snap] pool-scan start %llx stride 0x1c8\n", (unsigned long long)start);
 for (int i = 0; i < 16; i++) pool_one(start + (uint64_t)i * 0x1c8, i);
}

static void dump_regs(ucontext_t *u, const char *why) {
 uint64_t *x = u->uc_mcontext->__ss.__x;
 fprintf(stderr, "[snap] ---- %s ----\n", why);
 symbolize("pc", u->uc_mcontext->__ss.__pc);
 symbolize("lr", u->uc_mcontext->__ss.__lr);
 fprintf(stderr, "[snap] sp=%llx\n", (unsigned long long)u->uc_mcontext->__ss.__sp);
 for (int i = 0; i <= 8; i++) fprintf(stderr, "[snap] x%d=%llx\n", i, (unsigned long long)x[i]);
 fprintf(stderr, "[snap] x16=%llx x17=%llx x19=%llx\n",
  (unsigned long long)x[16], (unsigned long long)x[17], (unsigned long long)x[19]);
}

int runtime_emio_snap_trap(int num, void *context) {
 if (num != SIGTRAP || !main_base) return 0;
 ucontext_t *u = context;
 uint64_t pc = u->uc_mcontext->__ss.__pc;
 uint64_t *x = u->uc_mcontext->__ss.__x;
 int id = -1;
 for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) {
  uint8_t *base = sites[i].mod ? sdk_base : main_base;
  if (base && pc == (uint64_t)(base + sites[i].off)) { id = (int)i; break; }
 }
 if (id < 0) return 0;
 hits[id]++;
 if (sites[id].kind == K_ADRP && hits[id] > 12) {
  poke_rx(main_base + sites[id].off, sites[id].orig);
  u->uc_mcontext->__ss.__pc = (uint64_t)(main_base + sites[id].off);
  return 1;
 }
 if (sites[id].kind == K_KEEP_MADD || sites[id].kind == K_KEEP_STP16) goto keep_only;
 dump_regs(u, sites[id].name);
 if (sites[id].off == 0x94074) {
  fprintf(stderr, "[snap] tbnz w8=#0 taken-if-nz  w8=%llx  target=main+0x94348\n", (unsigned long long)x[8]);
  uint64_t slot = 0;
  if (main_base && read_mem((uint64_t)(main_base + 0x610938), &slot, 8))
   fprintf(stderr, "[snap] relr-slot-610938=%llx\n", (unsigned long long)slot);
 }
 if (sites[id].off == 0x94100)
  fprintf(stderr, "[snap] bl 10f1b0 x0=%llx x1=%llx x2=%llx w3=%llx w4=%llx w5=%llx w6=%llx\n",
   (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2],
   (unsigned long long)x[3], (unsigned long long)x[4], (unsigned long long)x[5], (unsigned long long)x[6]);
 if (sites[id].off == 0x110038) {
  g_wp2a_this = x[0];
  { static _Atomic unsigned c=0; unsigned k=++c;
   if (x[1] >= 0x10000 || (k%500)==1) fprintf(stderr, "[snap] AllocAlign-entry#%u this=%llx size=%llx\n", k, (unsigned long long)x[0], (unsigned long long)x[1]); }
 }
 if (sites[id].off == 0x1100b8) {
  fail_n++;
  fprintf(stderr, "[snap] AllocAlign-fail #%u this=%llx w1=%llx w2=%llx\n",
   fail_n, (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2]);
  symbolize("AllocAlign-caller-lr", u->uc_mcontext->__ss.__lr);
  /* WP2a: dump pool struct + map for free/contiguity. this is NULL here (return value);
     recover pool via first-seen instance (title uses one allocator). */
  extern uint64_t wp2a_pool_this(void);
  uint64_t pt = wp2a_pool_this();
  if (pt) {
   uint64_t base=0,sz=0,cnt=0,bsz=0,map=0;
   read_mem(pt+0x20,&base,8); read_mem(pt+0x28,&sz,8);
   {uint32_t v=0; read_mem(pt+0x38,&v,4); cnt=v;}
   {uint32_t v=0; read_mem(pt+0x40,&v,4); bsz=v;}
   read_mem(pt+0x68,&map,8);
   fprintf(stderr, "[snap] pool this=%llx base=%llx size=%llx count=%llu blksz=%llu map=%llx\n",
    (unsigned long long)pt,(unsigned long long)base,(unsigned long long)sz,(unsigned long long)cnt,(unsigned long long)bsz,(unsigned long long)map);
   if (map && cnt && cnt < 100000 && bsz) {
    /* count free + longest run. Map format unknown: try byte-per-block then bit-per-block. */
    uint8_t mbuf[1024]; uint64_t got=0;
    if (read_mem(map, mbuf, cnt<1024?cnt:1024)) {
     unsigned free1=0, run1=0, best1=0;
     for (uint64_t i=0;i<cnt&&i<1024;i++) { if(mbuf[i]==0){free1++;run1++;if(run1>best1)best1=run1;} else run1=0; }
     fprintf(stderr, "[snap] map-as-bytes free=%u best-run=%u of %llu\n", free1, best1, (unsigned long long)cnt);
     { unsigned fb=0, rb=0, bb=0; for (uint64_t i=0;i<96&&i<1024;i++) for(int b=0;b<8;b++){ int z=((mbuf[i]>>b)&1)==0; if(z){fb++;rb++;if(rb>bb)bb=rb;} else rb=0; }
      fprintf(stderr, "[snap] map-as-bits free=%u best-run=%u (96 bytes)\n", fb, bb); }
     { char b[128];int n=0;const char *h="[snap] map-raw ";while(*h)b[n++]=*h++;
      for(unsigned i=0;i<32&&n<120;i++){ unsigned v=mbuf[i]; b[n++]="0123456789abcdef"[(v>>4)&15]; b[n++]="0123456789abcdef"[v&15]; }
      b[n++]='\n';write(2,b,n); }
    }
   }
  }
 }
 if (sites[id].off == 0xfd80c) {
  fprintf(stderr, "[snap] madd x2=x13*x17+x12 x0=%llx x12=%llx x13=%llx x15=%llx x16=%llx x17=%llx\n",
   (unsigned long long)x[0], (unsigned long long)x[12], (unsigned long long)x[13],
   (unsigned long long)x[15], (unsigned long long)x[16], (unsigned long long)x[17]);
  uint32_t live[8];
  if (read_mem((uint64_t)main_base + 0xfd810, live, sizeof live)) {
   fprintf(stderr, "[snap] live fd810: %08x %08x %08x %08x %08x %08x %08x %08x\n",
    live[0], live[1], live[2], live[3], live[4], live[5], live[6], live[7]);
   fprintf(stderr, "[snap] orig fd810: a97c003f b94042c3 f90c003f b9002000 b9002800 b9010003 a97c033f b94042c3\n");
  }
 }
 if (sites[id].off == 0xfd810 && hits[id] <= 3) {
  uint64_t x1 = x[1], x2 = x[2];
  uint64_t span = x2 > x1 ? x2 - x1 : 0;
  fprintf(stderr, "[snap] zero-loop x1=%llx x2=%llx span=%llx span%%c0=%llx x12=%llx x15=%llx x16=%llx\n",
   (unsigned long long)x1, (unsigned long long)x2, (unsigned long long)span,
   (unsigned long long)(span % 0xc0), (unsigned long long)x[12],
   (unsigned long long)x[15], (unsigned long long)x[16]);
  symbolize("zero-lr", u->uc_mcontext->__ss.__lr);
 }
 if (sites[id].off == 0xea93c) {
  fprintf(stderr, "[snap] CreateThread x0=%llx x1=%llx x2=%llx x3=%llx x4=%llx x5=%llx\n",
   (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2],
   (unsigned long long)x[3], (unsigned long long)x[4], (unsigned long long)x[5]);
  dump_words("thread-obj x0", x[0], 0x80);
  symbolize("thread-entry x1", x[1]);
  symbolize("thread-stack x3", x[3]);
 }
 if (sites[id].off == 0x95c30) {
  fprintf(stderr, "[snap] diagprint30 x0=%llx x1=%llx x2=%llx x3=%llx x4=%llx\n",
   (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2],
   (unsigned long long)x[3], (unsigned long long)x[4]);
  dump_cstr("diag30-x0", x[0]);
  dump_cstr("diag30-x1", x[1]);
  dump_cstr("diag30-x2", x[2]);
  dump_cstr("diag30-x3", x[3]);
  dump_cstr("diag30-x4", x[4]);
 }
 if (sites[id].off == 0x5ccadc || sites[id].off == 0x5c4afc || sites[id].off == 0x5ccdcc) {
  fprintf(stderr, "[snap] plt x17=%llx x0=%llx x1=%llx x2=%llx x3=%llx x4=%llx x5=%llx\n",
   (unsigned long long)x[17], (unsigned long long)x[0], (unsigned long long)x[1],
   (unsigned long long)x[2], (unsigned long long)x[3], (unsigned long long)x[4], (unsigned long long)x[5]);
  symbolize("plt-target", x[17]);
  region_of("plt-target-region", x[17]);
  if (sites[id].off == 0x5ccdcc) {
   fprintf(stderr, "[snap] MapAliasStack out=%llx stack=%llx size=%llx\n",
    (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2]);
   region_of("alias-out", x[0]);
   region_of("alias-stack", x[1]);
   dump_words("alias-stack-head", x[1], 0x20);
  }
 }
 if (sites[id].off == 0x1334c0) {
  fprintf(stderr, "[snap] AllocateSpace ret x0=%llx size=%llx stack=%llx\n",
   (unsigned long long)x[0], (unsigned long long)x[19], (unsigned long long)x[20]);
  symbolize("allocspace-lr", u->uc_mcontext->__ss.__lr);
 }
 if (sites[id].off == 0x1334e8) {
  fprintf(stderr, "[snap] CheckGuard x0=%llx cand=%llx size=%llx w3=%llx\n",
   (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3]);
 }
 if (sites[id].off == 0x1b2d0c) {
  fprintf(stderr, "[snap] factory x0=%llx x1=%llx vt=%llx\n",
   (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[19]);
  dump_words("factory-obj x19", x[19], 0x40);
  uint64_t vt = 0;
  if (read_mem(x[19], &vt, 8)) {
   fprintf(stderr, "[snap] factory-vtable=%llx\n", (unsigned long long)vt);
   dump_words("vtable head", vt, 0x40);
   uint64_t meth = 0;
   if (read_mem(vt + 0x30, &meth, 8)) symbolize("factory-method+0x30", meth);
  }
  dump_words("factory-sp-out", u->uc_mcontext->__ss.__sp, 0x20);
 }
 if (sites[id].off == 0xd9860) {
  fprintf(stderr, "[snap] rommount x0=%llx x1=%llx x2=%llx x3=%llx w5=%llx\n",
   (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2],
   (unsigned long long)x[3], (unsigned long long)x[4]);
  dump_cstr("rommount-name", x[0]);
 }
 if (sites[id].off == 0x93cd0 && hits[id] <= 20) {
  fprintf(stderr, "[snap] romcache-alloc#%u size=%llx\n", hits[id], (unsigned long long)x[0]);
  uint64_t flag = 0;
  if (main_base && read_mem((uint64_t)(main_base + 0x1a6dc08), &flag, 8))
   fprintf(stderr, "[snap] romcache-heapflag=%llx\n", (unsigned long long)flag);
 }
 if (sites[id].off == 0x93cf0 && hits[id] <= 60) {
  uint64_t lr = u->uc_mcontext->__ss.__lr;
  fprintf(stderr, "[snap] romcache-call#%u size=%llx alloctr=%llx lr=%llx\n", hits[id], (unsigned long long)x[1], (unsigned long long)x[0], (unsigned long long)lr);
  if (x[0] > 0x1000000) symbolize("badsize-caller", lr);
 }
 if (sites[id].off == 0x93cf4) {
  if (!x[0]) fprintf(stderr, "[snap] romcache-ret NULL size-unknown call#%u\n", hits[id]);
  else if (hits[id] <= 20) fprintf(stderr, "[snap] romcache-ret#%u w0=%llx\n", hits[id], (unsigned long long)x[0]);
 }
 if (sites[id].off == 0x99640 && hits[id] <= 20) {
  fprintf(stderr, "[snap] abort-tbz#%u w0=%llx x19=%llx\n", hits[id], (unsigned long long)x[0], (unsigned long long)x[19]);
 }
 if (sites[id].off == 0x1117f0) {
  fprintf(stderr, "[snap] memcpy x0=%llx x3=%llx x13=%llx x14=%llx x15=%llx\n",
   (unsigned long long)x[0], (unsigned long long)x[3], (unsigned long long)x[13],
   (unsigned long long)x[14], (unsigned long long)x[15]);
  symbolize("memcpy-lr", u->uc_mcontext->__ss.__lr);
 }
 if (sites[id].off == 0xdef38 && hits[id] <= 10) {
  uint64_t f58 = 0;
  read_mem(x[0] + 0x58, &f58, 8);
  fprintf(stderr, "[snap] fmtswitch x0=%llx field58=%llx lr=%llx\n",
   (unsigned long long)x[0], (unsigned long long)f58, (unsigned long long)u->uc_mcontext->__ss.__lr);
  symbolize("fmtswitch-lr", u->uc_mcontext->__ss.__lr);
 }
 if (sites[id].off == 0xdeedc) {
  fprintf(stderr, "[snap] fmtobj x0=%llx\n", (unsigned long long)x[0]);
  symbolize("fmtobj-lr", u->uc_mcontext->__ss.__lr);
  dump_words("fmtobj-S", x[0], 0x70);
 }
 if (sites[id].off == 0xdef40) {
  fprintf(stderr, "[snap] vtable-ret w0=%llx sp=%llx\n", (unsigned long long)x[0], (unsigned long long)u->uc_mcontext->__ss.__sp);
  dump_words("vtable-out-sp", u->uc_mcontext->__ss.__sp, 0x20);
 }
 if (sites[id].off == 0xded4c && hits[id] <= 5) {
  fprintf(stderr, "[snap] SKIP memcpy dst=%llx src=%llx (test if fatal)\n", (unsigned long long)x[0], (unsigned long long)x[3]);
  u->uc_mcontext->__ss.__pc = (uint64_t)((sites[id].mod ? sdk_base : main_base) + sites[id].off + 4);
  return 1;
 }
 if (sites[id].off == 0xdef18 && hits[id] <= 40) {
  uint64_t v = 0; read_mem(x[0] + 0x58, &v, 8);
  if (!v) fprintf(stderr, "[snap] s-load#%u SUBSTITUTE scratch for null S=%llx\n", hits[id], (unsigned long long)x[0]);
  else fprintf(stderr, "[snap] s-load#%u S=%llx S58=%llx\n", hits[id], (unsigned long long)x[0], (unsigned long long)v);
 }
 if (sites[id].off == 0xe1bc0) {
  uint64_t w25 = 0, w20 = 0;
  read_mem(x[0] + 0x4e4, &w25, 4); read_mem(x[0] + 0x4dc, &w20, 4);
  fprintf(stderr, "[snap] e1bc0 x0=%llx w25=%llx w20=%llx\n", (unsigned long long)x[0], (unsigned long long)w25, (unsigned long long)w20);
  dump_words("e1bc0-obj", x[0], 0x40);
 }
 if (sites[id].off == 0xe2740 && hits[id] <= 20) {
  uint64_t a = 0, b = 0; read_mem(x[0] + 0x4dc, &a, 4); read_mem(x[0] + 0x4e4, &b, 4);
  fprintf(stderr, "[snap] e2740-call#%u x0=%llx w20=%llx w25=%llx\n", hits[id], (unsigned long long)x[0], (unsigned long long)a, (unsigned long long)b);
 }
 if ((sites[id].off == 0x95c40 || sites[id].off == 0x95c60) && hits[id] <= 3) {
  fprintf(stderr, "[snap] abortfn+%llx called x0=%llx x1=%llx x2=%llx x3=%llx x4=%llx x5=%llx lr=%llx sp=%llx\n",
   (unsigned long long)sites[id].off, (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2],
   (unsigned long long)x[3], (unsigned long long)x[4], (unsigned long long)x[5], (unsigned long long)u->uc_mcontext->__ss.__lr, (unsigned long long)u->uc_mcontext->__ss.__sp);
  dump_cstr("abort-x0", x[0]); dump_cstr("abort-x1", x[1]); dump_cstr("abort-x2", x[2]);
  dump_words("abort-sp", u->uc_mcontext->__ss.__sp, 0x60);
 }
 if (sites[id].off == 0xe2744) {
  fprintf(stderr, "[snap] e2744-ret w0=%llx\n", (unsigned long long)x[0]);
 }
 if (sites[id].off == 0xe1bc0) {
  uint64_t a = 0, b = 0; read_mem(x[0] + 0x4dc, &a, 4); read_mem(x[0] + 0x4e4, &b, 4);
  uint64_t m = 0, c = 0; read_mem(x[0] + 0x4d8, &m, 4); read_mem(x[0] + 0x3a1, &c, 1);
  uint64_t vt = 0; read_mem(x[0], &vt, 8);
  fprintf(stderr, "[snap] e1bc0-fixup x0=%llx +4dc=%llx +4e4=%llx mask=%llx cnt=%llx vtable=%llx\n", (unsigned long long)x[0], (unsigned long long)a, (unsigned long long)b, (unsigned long long)m, (unsigned long long)c, (unsigned long long)vt);
  symbolize("e1bc0-vtable", vt);
  /* Fixup DISABLED (diagnostic): let w25 stay -1 (natural) so the game takes its default display-mode
     path. If that path aborts on a GetNpadStyleSet check, the style bits (now precise, not forced 0x3F)
     will reveal the exact requirement. */
  if (0 && b == 0xffffffffu) {
   uint32_t one = 1;
   memcpy((void *)(uintptr_t)(x[0] + 0x4e4), &one, 4);
   fprintf(stderr, "[snap] e1bc0-fixup applied (case 1: full-key)\n");
  }
  symbolize("e1bc0-lr", u->uc_mcontext->__ss.__lr);
 }
 if (sites[id].off == 0xe2740 && hits[id] <= 10) {
  uint64_t a = 0, b = 0; read_mem(x[0] + 0x4dc, &a, 4); read_mem(x[0] + 0x4e4, &b, 4);
  fprintf(stderr, "[snap] e2740-call#%u x0=%llx +4dc=%llx +4e4=%llx\n", hits[id], (unsigned long long)x[0], (unsigned long long)a, (unsigned long long)b);
 }
 if (sites[id].off == 0x49cc0) {
  uint64_t vt = 0, meth = 0;
  if (read_mem(x[0], &vt, 8)) read_mem(vt + 0x18, &meth, 8);
  fprintf(stderr, "[snap] virtcall x0=%llx vt=%llx meth18=%llx\n", (unsigned long long)x[0], (unsigned long long)vt, (unsigned long long)meth);
  symbolize("virtcall-target", meth);
 }
 if (sites[id].off == 0xe3ca0) {
  uint64_t v = 0; read_mem(x[0] + 0x90, &v, 4);
  fprintf(stderr, "[snap] e3ca0 x0=%llx +90=%llx\n", (unsigned long long)x[0], (unsigned long long)v);
  region_of("e3ca0-obj", x[0]);
 }
 /* SUPPRESS DISABLED: let aborts fire so the first failure is visible (bypasses above should prevent them). */
 if (sites[id].off == 0x3cf158) {
  fprintf(stderr, "[snap] time-notinit BYPASS (fake time service ready)\n");
  u->uc_mcontext->__ss.__x[0] = 0;
  u->uc_mcontext->__ss.__pc = (uint64_t)(sdk_base + sites[id].off + 4);
  return 1;
 }
 if (sites[id].off == 0xd8978) {
  fprintf(stderr, "[snap] rommount-ret w0=%llx\n", (unsigned long long)x[0]);
 }
 if (sites[id].off == 0x95c5c) {
  fprintf(stderr, "[snap] diagprint x0=%llx x1=%llx x2=%llx w3=%llx\n",
   (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3]);
  dump_cstr("diag-fmt x0", x[0]);
  dump_cstr("diag-a1 x1", x[1]);
  dump_cstr("diag-a2 x2", x[2]);
 }

 if (x[19] > 0x100000000ull) {
  dump_words("obj x19", x[19], 0x98);
  uint64_t cb = 0;
  if (read_mem(x[19] + 0x78, &cb, 8))
   fprintf(stderr, "[snap] [x19+0x78]=%llx x8=%llx %s\n",
    (unsigned long long)cb, (unsigned long long)x[8], cb == x[8] ? "match" : "MISMATCH");
 }
 if (main_base) {
  dump_words("StdAlloc 1a6dc10", (uint64_t)(main_base + 0x1a6dc10), 0x40);
  uint64_t sa[8];
  if (read_mem((uint64_t)(main_base + 0x1a6dc10), sa, sizeof sa)) {
   if (sa[3] && sa[6]) { block_lo = sa[3]; block_hi = sa[6]; }
   else if (sa[5] && sa[6]) { block_lo = sa[5]; block_hi = sa[6]; }
  }
 }

 if (sites[id].kind == K_BLR) {
  symbolize("callee-x8", x[8]);
  region_of("callee-region", x[8]);
  fprintf(stderr, "[snap] emulate blr  w0=%llx w1=%llx\n", (unsigned long long)x[0], (unsigned long long)x[1]);
  poke_rx(main_base + sites[id].off, sites[id].orig);
  u->uc_mcontext->__ss.__lr = (uint64_t)(main_base + 0x10f2a8);
  u->uc_mcontext->__ss.__pc = x[8];
  return 1;
 }
 if (sites[id].kind == K_STP) {
  /* stp x29, x30, [sp, #-0x20]! */
  uint64_t sp = u->uc_mcontext->__ss.__sp - 0x20;
  uint64_t pair[2] = { u->uc_mcontext->__ss.__fp, u->uc_mcontext->__ss.__lr };
  memcpy((void *)(uintptr_t)sp, pair, 16);
  u->uc_mcontext->__ss.__sp = sp;
  u->uc_mcontext->__ss.__pc = pc + 4;
  if (hits[id] == 1) first_pool = x[0];
  if (hits[id] <= 4)
   fprintf(stderr, "[snap] persist-stp caller lr; x0=%llx x1=%llx x2=%llx w4=%llx w5=%llx w6=%llx\n",
    (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2],
    (unsigned long long)x[4], (unsigned long long)x[5], (unsigned long long)x[6]);
  return 1;
 }
 if (sites[id].kind == K_ADRP) {
  uint32_t insn = sites[id].orig;
  unsigned rd = insn & 31;
  uint64_t imm21 = ((insn >> 5) & 0x7ffff) << 2 | ((insn >> 29) & 3);
  int64_t off = ((int64_t)(imm21 << 43)) >> 31; /* sign-extend 21, then <<12 */
  x[rd] = (pc & ~0xfffull) + (uint64_t)off;
  u->uc_mcontext->__ss.__pc = pc + 4;
  return 1;
 }
 if (sites[id].kind == K_EMU) {
  /* persistent log + emulate: bl / cbz / cbnz / tbz / tbnz. Never restores. */
  uint32_t o = sites[id].orig;
  uint64_t npc = pc + 4;
  if ((o & 0xFC000000u) == 0x94000000u) { /* bl */
   int32_t d = ((int32_t)((o & 0x3FFFFFFu) << 6) >> 6) * 4;
   u->uc_mcontext->__ss.__lr = pc + 4;
   npc = pc + (int64_t)d;
  } else if ((o & 0x7F000000u) == 0x34000000u || (o & 0x7F000000u) == 0x35000000u) { /* cbz/cbnz */
   unsigned rt = o & 31;
   int64_t d = ((int64_t)((uint64_t)((o >> 5) & 0x7FFFFu) << 45) >> 45) * 4;
   int is_cbnz = ((o >> 30) & 1) != 0;
   int zero = (rt == 31) ? 1 : (x[rt] == 0);
   if (zero != is_cbnz) npc = pc + d;
  } else if (o == 0xD10343FFu && sites[id].off == 0xe1bc0) { /* sub sp, sp, #0xd0 (keep armed) */
   u->uc_mcontext->__ss.__sp -= 0xd0;
   u->uc_mcontext->__ss.__pc = pc + 4;
   return 1;
  } else if (o == 0xF9402C00u && sites[id].off == 0xdef18) { /* ldr x0, [x0, #0x58] */
   static uint8_t escratch[0x1000]; static int eseed;
   if (!eseed) { memset(escratch, 0, sizeof escratch); eseed = 1; }
   uint64_t v = 0;
   if (!read_mem(x[0] + 0x58, &v, 8)) { fprintf(stderr, "[snap] s-load unreadable\n"); }
   x[0] = v ? v : (uint64_t)(uintptr_t)escratch;
   u->uc_mcontext->__ss.__pc = pc + 4;
   return 1;
  } else if ((o & 0x7F000000u) == 0x36000000u || (o & 0x7F000000u) == 0x37000000u) { /* tbz/tbnz */
   unsigned rt = o & 31;
   unsigned bit = (o >> 19) & 31;
   int64_t d = ((int64_t)((uint64_t)((o >> 5) & 0x3FFFu) << 50) >> 50) * 4;
   int is_tbnz = ((o >> 30) & 1) != 0;
   uint64_t rv = (rt == 31) ? 0 : x[rt];
   int set = ((rv >> bit) & 1) != 0;
   if ((!set) != is_tbnz) npc = pc + d;
  } else if (o == 0xFD7BBEA9u && sites[id].off == 0x110038) { /* stp x29,x30,[sp,#-0x20]! (keep armed) */
   uint64_t sp = u->uc_mcontext->__ss.__sp - 0x20;
   uint64_t fp = u->uc_mcontext->__ss.__fp, lr = u->uc_mcontext->__ss.__lr;
   memcpy((void *)(uintptr_t)sp, &fp, 8);
   memcpy((void *)(uintptr_t)(sp+8), &lr, 8);
   u->uc_mcontext->__ss.__sp = sp;
   npc = pc + 4;
  } else {
   fprintf(stderr, "[snap] K_EMU unknown orig %08x at %s\n", o, sites[id].name);
   poke_rx((sites[id].mod ? sdk_base : main_base) + sites[id].off, sites[id].orig);
   npc = (uint64_t)((sites[id].mod ? sdk_base : main_base) + sites[id].off);
  }
  u->uc_mcontext->__ss.__pc = npc;
  return 1;
 }
keep_only:
 { static uint64_t last_s58 = 0; static int logged_s58; static unsigned npoll;
  if (main_base) { uint64_t cur = 0xdeadbeef; int ok = read_mem((uint64_t)(main_base + 0xb46cda0 + 0x58), &cur, 8);
   if (++npoll <= 3) fprintf(stderr, "[snap] poll#%u ok=%d cur=%llx\n", npoll, ok, (unsigned long long)cur);
   if (ok && cur != last_s58) {
   if (!logged_s58 || cur != 0) { fprintf(stderr, "[snap] S+58 transition %llx -> %llx\n", (unsigned long long)last_s58, (unsigned long long)cur); if (cur) logged_s58 = 1; }
   last_s58 = cur; } } }
 if (sites[id].kind == K_KEEP_MADD) {
  /* madd x2, x13, x17, x12 — keep trap armed, no restore */
  x[2] = x[13] * x[17] + x[12];
  u->uc_mcontext->__ss.__pc = pc + 4;
  if (hits[id] <= 30) {
   uint64_t span = x[2] > x[12] ? x[2] - x[12] : 0;
   fprintf(stderr, "[snap] madd#%u x0=%llx x12=%llx x13=%llx x17=%llx x2=%llx span=%llx mod=%llx x15=%llx x16=%llx\n",
    hits[id], (unsigned long long)x[0], (unsigned long long)x[12], (unsigned long long)x[13],
    (unsigned long long)x[17], (unsigned long long)x[2], (unsigned long long)span,
    (unsigned long long)(span % 0xc0), (unsigned long long)x[15], (unsigned long long)x[16]);
   symbolize("madd-lr", u->uc_mcontext->__ss.__lr);
  }
  return 1;
 }
 if (sites[id].kind == K_KEEP_STP16) {
  /* stp xzr, xzr, [x1] — keep trap armed, no restore */
  if (x[1]) memset((void *)(uintptr_t)x[1], 0, 16);
  u->uc_mcontext->__ss.__pc = pc + 4;
  if (hits[id] <= 40) {
   uint64_t x1 = x[1], x2 = x[2];
   uint64_t span = x2 > x1 ? x2 - x1 : 0;
   fprintf(stderr, "[snap] zero#%u x1=%llx x2=%llx span=%llx mod=%llx x15=%llx x16=%llx\n",
    hits[id], (unsigned long long)x1, (unsigned long long)x2, (unsigned long long)span,
    (unsigned long long)(span % 0xc0), (unsigned long long)x[15], (unsigned long long)x[16]);
  }
  return 1;
 }
 /* oneshot: restore and re-execute */
 { uint8_t *base = sites[id].mod ? sdk_base : main_base;
  poke_rx(base + sites[id].off, sites[id].orig);
  u->uc_mcontext->__ss.__pc = (uint64_t)(base + sites[id].off); }
 if (sites[id].off == 0x10f2a8 && first_pool) pool_scan(first_pool);
 if (sites[id].off == 0x10fc30 && x[0]) pool_scan(x[0]);
 return 1;
}

void runtime_emio_snap_crash(void *context) {
 if (!main_base) return;
 ucontext_t *u = context;
 uint64_t *x = u->uc_mcontext->__ss.__x;
 uint64_t pc = u->uc_mcontext->__ss.__pc;
 uint64_t far = 0; /* filled by caller via si_addr — we re-read from ESR path below */
 siginfo_t dummy; (void)dummy;
 fprintf(stderr, "[snap] ==== CRASH CONSISTENCY ====\n");
 symbolize("pc", pc);
 symbolize("lr", u->uc_mcontext->__ss.__lr);
 uint32_t insn = 0;
 if (read_mem(pc, &insn, 4))
  fprintf(stderr, "[snap] insn@pc %08x\n", insn);
 else fprintf(stderr, "[snap] insn@pc unreadable\n");
 for (int i = 0; i < 29; i++)
  fprintf(stderr, "[snap] x%d=%llx\n", i, (unsigned long long)x[i]);
 fprintf(stderr, "[snap] sp=%llx fp=%llx\n",
  (unsigned long long)u->uc_mcontext->__ss.__sp,
  (unsigned long long)u->uc_mcontext->__ss.__fp);
 uint64_t map = 0;
 if (x[0] && read_mem(x[0] + 0x68, &map, 8))
  fprintf(stderr, "[snap] [x0+0x68]=%llx\n", (unsigned long long)map);
 dump_words("crash-x0", x[0], 0x98);
 if (first_pool) pool_scan(first_pool);
 fprintf(stderr, "[snap] block %llx..%llx  nvn_dirty(x16)=%d nvn_dirty(x0)=%d\n",
  (unsigned long long)block_lo, (unsigned long long)block_hi,
  nvn_dirty_tracked((void *)(uintptr_t)x[16], 1),
  nvn_dirty_tracked((void *)(uintptr_t)x[0], 1));
}

void runtime_emio_snap_crash_far(void *addr, void *context) {
 uint64_t far = (uint64_t)(uintptr_t)addr;
 ucontext_t *u = context;
 uint64_t *x = u->uc_mcontext->__ss.__x;
 uint64_t pc = u->uc_mcontext->__ss.__pc;
 uint32_t insn = 0;
 read_mem(pc, &insn, 4);
 uint64_t expect = 0;
 int known = 0;
 /* strh wt, [xn, #imm12*2]  0x79000000 */
 if ((insn & 0xffc00000u) == 0x79000000u) {
  unsigned rn = (insn >> 5) & 31;
  unsigned imm = ((insn >> 10) & 0xfff) * 2;
  uint64_t base = rn == 31 ? u->uc_mcontext->__ss.__sp : x[rn];
  expect = base + imm;
  known = 1;
  fprintf(stderr, "[snap] FAR-decode strh [x%u,#%u] base=%llx expect=%llx far=%llx %s domain=%s\n",
   rn, imm, (unsigned long long)base, (unsigned long long)expect, (unsigned long long)far,
   expect == far ? "CONSISTENT" : "MISMATCH", far_domain(far));
 }
 /* str wt, [xn, #imm] */
 else if ((insn & 0xffc00000u) == 0xb9000000u) {
  unsigned rn = (insn >> 5) & 31;
  unsigned imm = ((insn >> 10) & 0xfff) * 4;
  uint64_t base = rn == 31 ? u->uc_mcontext->__ss.__sp : x[rn];
  expect = base + imm;
  known = 1;
  fprintf(stderr, "[snap] FAR-decode str [x%u,#%u] base=%llx expect=%llx far=%llx %s domain=%s\n",
   rn, imm, (unsigned long long)base, (unsigned long long)expect, (unsigned long long)far,
   expect == far ? "CONSISTENT" : "MISMATCH", far_domain(far));
 }
 else {
  fprintf(stderr, "[snap] FAR=%llx insn=%08x domain=%s (no str decode)\n",
   (unsigned long long)far, insn, far_domain(far));
 }
 (void)known; (void)expect;
 region_of("FAR", far);
 int tracked = nvn_dirty_tracked((void *)(uintptr_t)far, 1);
 fprintf(stderr, "[snap] nvn_dirty_tracked(FAR)=%d\n", tracked);
}

void runtime_emio_snap_install(Module *m) {
 if (!m || !m->base || !m->name) return;
 int is_sdk = !strcmp(m->name, "sdk");
 if (is_sdk) sdk_base = m->base;
 else { main_base = m->base; main_size = m->size ? m->size : 0xc000000; }
 for (unsigned i = 0; i < sizeof sites / sizeof *sites; i++) {
  if ((sites[i].mod != 0) != is_sdk) continue;
  uint32_t orig;
  memcpy(&orig, m->base + sites[i].off, 4);
  sites[i].orig = orig;
  poke_rx(m->base + sites[i].off, BRK_IMM(i + 1));
  fprintf(stderr, "[snap] trap %s %s+%#llx orig=%08x kind=%d\n",
   sites[i].name, m->name, (unsigned long long)sites[i].off, orig, sites[i].kind);
 }
 if (!is_sdk && main_base) {
  /* (Removed: static pre-fill targeted the wrong object; S is dynamic. See s-load substitution.) */
  (void)main_base;
 }
}
