/* The loader of a libnx homebrew NSO on the Zelda-line runtime (third_party/COPIES.md: the module mapping, sealing,
   permission, gate dispatch and thread environment follow games/mk8dx/runtime/loader.c). What differs from a Nintendo SDK
   game: there is one module, `main`, with no imports, and libnx relocates itself (crt0 calls __nx_dynamic, which adds
   the base to every RELR word), so this loader applies no relocation at all -- applying them here too would add the base
   twice. The module is entered at its crt0 (offset 0) as Horizon's loader enters an NSO: x0 = 0, x1 = the main thread's
   handle. */
#include "runtime.h"
#include "game_patches.h"
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include "mach_vm_compat.h"
#include "runtime_tables.h"

#define NM (sizeof(modules) / sizeof(modules[0]))
pthread_key_t runtime_keys[8];

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "[totk] FAIL: %s\n", message); fflush(stderr); exit(1); }
static void need(bool c, const char *m) { if (!c) runtime_fail(m); }
static size_t page_up(size_t x) { return (x + 0x3fff) & ~(size_t)0x3fff; }
void *runtime_symbol(const char *name) { (void)name; return NULL; }   /* a homebrew exports nothing the host calls */
uint64_t runtime_now_ns(void) { mach_timebase_info_data_t tb; mach_timebase_info(&tb); return (uint64_t)((__uint128_t)mach_absolute_time() * tb.numer / tb.denom); }
uint8_t *runtime_module_base(const char *name) { for (unsigned i = 0; i < NM; i++) if (!strcmp(modules[i].name, name)) return modules[i].base; return NULL; }
uint64_t runtime_image_bytes(void) { uint64_t total = 0; for (unsigned i = 0; i < NM; i++) total += modules[i].key_off; return page_up(total); }

/* ---------- module permissions (svcSetMemoryPermission / svcSetMemoryAttribute on the module's data) ----------
   libnx's __nx_dynamic makes its RELRO read-only and locks it. Guest pages are 4 KiB; a Darwin 16 KiB page gets the union
   of its four guest permissions, so protecting an edge never takes access from a writable neighbour. */
static pthread_mutex_t module_permission_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *module_permissions[NM];
static unsigned module_page_permission(unsigned i, uint64_t off) {
 Module *m = &modules[i];
 if (off < m->data_off) return off < page_up(m->text_end) ? 5 : 1;
 if (off >= m->key_off) return off >= m->patch_off ? 5 : 1;
 return module_permissions[i] ? module_permissions[i][(off - m->data_off) / 4096] : 3;
}
static unsigned char *permission_table(unsigned i) {
 Module *m = &modules[i];
 if (!module_permissions[i]) { size_t count = (m->key_off - m->data_off + 4095) / 4096; module_permissions[i] = malloc(count); need(module_permissions[i] != NULL, "module permission table"); memset(module_permissions[i], 3, count); }
 return module_permissions[i];
}
uint64_t runtime_module_permission(uint64_t address, uint64_t size, unsigned permission) {
 if (address & 4095) return 0xcc01;
 if (!size || (size & 4095)) return 0xca01;
 if (permission != 0 && permission != 1 && permission != 3) return 0xd801;
 for (unsigned i = 0; i < NM; i++) {
  Module *m = &modules[i]; uint64_t base = (uintptr_t)m->base;
  if (address < base || address - base < m->data_off || address - base >= m->key_off || size > m->key_off - (address - base)) continue;
  uint64_t off = address - base;
  pthread_mutex_lock(&module_permission_lock);
  unsigned char *table = permission_table(i);
  for (uint64_t p = 0; p < size / 4096; p++) if (table[(off - m->data_off) / 4096 + p] & 16) { pthread_mutex_unlock(&module_permission_lock); return 0xd401; }
  memset(table + (off - m->data_off) / 4096, (int)permission, size / 4096);
  for (uint64_t page = off & ~0x3fffULL; page < page_up(off + size); page += 0x4000) {
   unsigned combined = 0; for (unsigned p = 0; p < 4; p++) combined |= module_page_permission(i, page + p * 4096);
   need(!(combined & 4), "module permission overlaps executable page");
   need(mprotect(m->base + page, 0x4000, (combined & 1 ? PROT_READ : 0) | (combined & 2 ? PROT_WRITE : 0)) == 0, "module data permission");
  }
  pthread_mutex_unlock(&module_permission_lock);
  fprintf(stderr, "[totk] %s data permission +%#llx size=%#llx perm=%u\n", m->name, (unsigned long long)off, (unsigned long long)size, permission);
  return 0;
 }
 return 0xd401;
}
uint64_t runtime_module_lock_permission(uint64_t address, uint64_t size, unsigned mask, unsigned attribute) {
 if (address & 4095) return 0xcc01;
 if (!size || (size & 4095)) return 0xca01;
 if (mask != 16 || attribute != 16) return 0xd801;   /* MemAttr_IsPermissionLocked, the only attribute libnx sets */
 for (unsigned i = 0; i < NM; i++) {
  Module *m = &modules[i]; uint64_t start = (uintptr_t)m->base;
  if (address < start || address - start < m->data_off || address - start >= m->key_off || size > m->key_off - (address - start)) continue;
  pthread_mutex_lock(&module_permission_lock);
  unsigned char *table = permission_table(i); size_t first = (address - start - m->data_off) / 4096;
  for (uint64_t p = 0; p < size / 4096; p++) table[first + p] |= 16;
  pthread_mutex_unlock(&module_permission_lock); return 0;
 }
 return 0xd401;
}
int runtime_module_query(uint64_t address, uint64_t *base, uint64_t *size, unsigned *state, unsigned *permission, unsigned *attribute) {
 for (unsigned i = 0; i < NM; i++) {
  Module *m = &modules[i]; uint64_t start = (uintptr_t)m->base;
  if (!m->base || address < start || address - start >= m->key_off) continue;
  uint64_t off = (address - start) & ~4095ULL, lo = off, hi = off + 4096;
  pthread_mutex_lock(&module_permission_lock);
  unsigned perm = module_page_permission(i, off); int data = off >= m->data_off;
  uint64_t limit = data ? m->data_off : 0, end = data ? m->key_off : m->data_off;
  while (lo > limit && module_page_permission(i, lo - 4096) == perm) lo -= 4096;
  while (hi < end && module_page_permission(i, hi) == perm) hi += 4096;
  pthread_mutex_unlock(&module_permission_lock);
  *base = start + lo; *size = hi - lo; *state = data ? 4 : 3; *permission = perm & 7; *attribute = perm & 16; return 1;
 }
 return 0;
}

/* ---------- addresses for diagnostics ---------- */
int runtime_module_offset(uint64_t pc, const char **name, uint64_t *off) {
 for (unsigned i = 0; i < NM; i++) if (pc >= (uintptr_t)modules[i].base && pc < (uintptr_t)modules[i].base + modules[i].size) { *name = modules[i].name; *off = pc - (uintptr_t)modules[i].base; return 1; }
 return 0;
}
void runtime_describe_address(uint64_t a, char *out, size_t n) {
 const char *name; uint64_t off;
 if (runtime_module_offset(a, &name, &off)) { snprintf(out, n, "%s+0x%llx", name, (unsigned long long)off); return; }
 Dl_info di; if (dladdr((void *)(uintptr_t)a, &di) && di.dli_sname) { snprintf(out, n, "%s+0x%llx", di.dli_sname, (unsigned long long)(a - (uintptr_t)di.dli_saddr)); return; }
 snprintf(out, n, "0x%llx", (unsigned long long)a);
}
int runtime_guest_code_pc(uint64_t pc) { for (unsigned i = 0; i < NM; i++) { uint64_t b = (uintptr_t)modules[i].base; if (b && pc >= b && pc - b < modules[i].text_end) return 1; } return 0; }
void runtime_trace_guest(uint64_t fp, uint64_t lr) {
 for (unsigned n = 0; n < 16; n++) {
  const char *name; uint64_t off; if (runtime_module_offset(lr, &name, &off)) fprintf(stderr, "[totk] caller %s+0x%llx\n", name, (unsigned long long)off);
  if (!fp || fp % 8) break; uint64_t f[2]; mach_vm_size_t got = 0;
  if (mach_vm_read_overwrite(mach_task_self(), fp, 16, (uintptr_t)f, &got) != KERN_SUCCESS || got != 16) break;
  lr = f[1]; if (f[0] <= fp || f[0] - fp > (64u << 20)) break; fp = f[0];
 }
}

/* ---------- mapping ---------- */
#if TARGET_OS_IPHONE
/* iOS: code written at run time cannot execute, so the shared tools compile the rewritten image (text with the game
   patches already applied on the Mac, export-ios) into the executable; it is remapped back to its relative offsets. */
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include "ios_embed.h"
static void remap_at(Module *m, uintptr_t to, const void *from, size_t size, boolean_t copy, vm_prot_t need_prot) {
 mach_vm_address_t a = to; vm_prot_t cur = 0, max = 0;
 kern_return_t kr = mach_vm_remap(mach_task_self(), &a, size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (uintptr_t)from, copy, &cur, &max, VM_INHERIT_NONE);
 if (kr != KERN_SUCCESS || a != to) { fprintf(stderr, "[totk] %s vm_remap failed: %d\n", m->name, kr); runtime_fail("remap embedded module"); }
 need((cur & need_prot) == need_prot, "embedded module permissions after remap");
}
static void map_module(Module *m, const char *dir) {
 (void)dir; const IosEmbed *e = NULL;
 for (unsigned i = 0; i < sizeof(ios_embed) / sizeof(*ios_embed); i++) if (!strcmp(ios_embed[i].name, m->name)) e = &ios_embed[i];
 need(e != NULL, "module missing from ios_embed.h");
 char sx0[17], sx1[17], sw[17]; snprintf(sx0, sizeof sx0, "__%sx0", m->name); snprintf(sx1, sizeof sx1, "__%sx1", m->name); snprintf(sw, sizeof sw, "__TOTK%sW", m->name);
 unsigned long z0 = 0, zw = 0, z1 = 0;
 const uint8_t *x0 = getsectiondata(&_mh_execute_header, "__TOTKX", sx0, &z0), *w = getsectiondata(&_mh_execute_header, sw, "__img", &zw), *x1 = getsectiondata(&_mh_execute_header, "__TOTKX", sx1, &z1);
 need(x0 && w && x1, "embedded module sections missing");
 need(z0 == page_up(m->text_end) && z0 + zw == m->patch_off && m->patch_off + z1 == page_up(m->file_size), "embedded module layout");
 m->size = page_up(m->file_size);
 mach_vm_address_t base = 0; need(mach_vm_allocate(mach_task_self(), &base, m->size, VM_FLAGS_ANYWHERE) == KERN_SUCCESS, "reserve module range");
 remap_at(m, base, x0, z0, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
 remap_at(m, base + z0, w, zw, TRUE, VM_PROT_READ | VM_PROT_WRITE);
 remap_at(m, base + m->patch_off, x1, z1, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
 m->base = (uint8_t *)(uintptr_t)base;
 uint64_t *key = (void *)(m->base + m->key_off); for (unsigned i = 0; i < 8; i++) key[i] = runtime_keys[i] * 8;
}
#else
static void map_module(Module *m, const char *dir) {
 char path[4096]; need(snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name) < (int)sizeof path, "image path");
 FILE *f = fopen(path, "rb"); need(f != NULL, "open patched module (run make prepare)");
 struct stat st; need(fstat(fileno(f), &st) == 0 && (size_t)st.st_size == m->file_size, "patched module size");
 m->size = page_up(m->file_size);
 m->base = mmap(NULL, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
 need(m->base != MAP_FAILED, "map module"); need(fread(m->base, 1, m->file_size, f) == m->file_size, "read image"); fclose(f);
 unsigned char digest[32]; CC_SHA256(m->base, (CC_LONG)m->file_size, digest); char hex[65];
 for (unsigned i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
 need(!strcmp(hex, m->digest), "image SHA256 differs from verified rewrite");
 need(m->key_off % 0x4000 == 0 && m->key_off + 0x4000 <= m->patch_off && m->patch_off < m->size, "key and patch layout");
 uint64_t *key = (void *)(m->base + m->key_off); for (unsigned i = 0; i < 8; i++) key[i] = runtime_keys[i] * 8;
 fprintf(stderr, "[totk] mapped %s at %p size=%#zx\n", m->name, m->base, m->size);
}
/* TOTK_EXPORT_IOS=<dir>: the text after the game patches, for the shared iOS embedding tools. */
static void export_ios_text(const char *rewritten, const char *out) {
 for (unsigned i = 0; i < NM; i++) {
  Module *m = &modules[i]; char path[4096]; size_t n = page_up(m->text_end);
  need(snprintf(path, sizeof path, "%s/%s.patched.bin", rewritten, m->name) < (int)sizeof path, "image path");
  FILE *f = fopen(path, "rb"); need(f != NULL, "open patched module"); uint8_t *orig = malloc(n); need(orig && fread(orig, 1, n, f) == n, "read original text"); fclose(f);
  unsigned diff = 0; for (size_t k = 0; k < n; k += 4) if (memcmp(orig + k, m->base + k, 4)) diff++; free(orig);
  need(snprintf(path, sizeof path, "%s/%s.x0.bin", out, m->name) < (int)sizeof path, "export path");
  f = fopen(path, "wb"); need(f && fwrite(m->base, 1, n, f) == n && fclose(f) == 0, "write exported text");
  printf("%s text_bytes=%zu patched_words=%u\n", m->name, n, diff);
 }
}
#endif
static void seal(Module *m) {
 size_t rx = page_up(m->text_end), ro_end = m->data_off & ~(size_t)0x3fff;
 need(rx <= ro_end, "text/data overlap");
#if TARGET_OS_IPHONE
 if (ro_end > rx) need(mprotect(m->base + rx, ro_end - rx, PROT_READ) == 0, "rodata R");
 need(mprotect(m->base + m->key_off, 0x4000, PROT_READ) == 0, "key page R");
 return;
#endif
 need(mprotect(m->base, rx, PROT_READ | PROT_EXEC) == 0, "text RX");
 if (ro_end > rx) need(mprotect(m->base + rx, ro_end - rx, PROT_READ) == 0, "rodata R");
 need(mprotect(m->base + m->key_off, 0x4000, PROT_READ) == 0, "key page R");
 size_t patch_end = page_up(m->file_size);
 need(mprotect(m->base + m->patch_off, patch_end - m->patch_off, PROT_READ | PROT_EXEC) == 0, "patch RX");
 sys_icache_invalidate(m->base, rx); sys_icache_invalidate(m->base + m->patch_off, patch_end - m->patch_off);
}

/* ---------- guest threads' environment ---------- */
typedef struct { void *tls, *tlr, *dummy, *signal_stack; } ThreadEnvironment;
static pthread_key_t environment_key;
#define SIGNAL_STACK_SIZE (256u << 10)
static void environment_destroy(void *p) {
 ThreadEnvironment *e = p; if (!e) return;
 if (e->signal_stack) { stack_t off = {.ss_flags = SS_DISABLE}; sigaltstack(&off, NULL); free(e->signal_stack); }
 free(e->dummy); free(e->tlr); free(e->tls); free(e);
}
/* libnx keeps its IPC message buffer at the thread-local region's start, the user TLS slots at +0x108 and its ThreadVars
   at +0x1e0 (tls_tp at +0x1f8, which its __aarch64_read_tp reads): the 0x200 bytes Horizon gives a thread. */
void runtime_thread_init(void) {
 need(!pthread_getspecific(environment_key), "duplicate guest thread environment");
 ThreadEnvironment *e = calloc(1, sizeof *e); need(e != NULL, "thread environment");
 e->tls = calloc(1, 8192); e->tlr = calloc(1, 0x200); e->dummy = calloc(1, 0x400);
 need(e->tls && e->tlr && e->dummy, "thread environment allocation");
 e->signal_stack = malloc(SIGNAL_STACK_SIZE); need(e->signal_stack != NULL, "signal stack allocation");
 stack_t ss = {.ss_sp = e->signal_stack, .ss_size = SIGNAL_STACK_SIZE, .ss_flags = 0}; need(sigaltstack(&ss, NULL) == 0, "install signal stack");
 memcpy((uint8_t *)e->tlr + 0x1f8, &e->dummy, 8);
 need(pthread_setspecific(environment_key, e) == 0 && pthread_setspecific(runtime_keys[0], NULL) == 0, "thread ownership");
 need(pthread_setspecific(runtime_keys[1], e->tls) == 0 && pthread_setspecific(runtime_keys[2], e->tlr) == 0 && pthread_setspecific(runtime_keys[3], (void *)runtime_gate) == 0, "thread slots");
 uintptr_t tp; __asm__ volatile("mrs %0,tpidrro_el0" : "=r"(tp));
 need((tp & 7) == 0, "unaligned raw Darwin TSD incompatible with patches");
 for (unsigned i = 0; i < 4; i++) { void *raw; memcpy(&raw, (void *)(tp + runtime_keys[i] * 8), 8); need(raw == pthread_getspecific(runtime_keys[i]), "Darwin TSD layout"); }
}

/* ---------- the host gate: svc and the system counter ---------- */
void runtime_dispatch(uint64_t ret, uint64_t *regs) {
 for (unsigned i = 0; i < NM; i++) for (unsigned j = 0; j < modules[i].ngate; j++) {
  const Gate *g = &modules[i].gates[j]; if ((uintptr_t)modules[i].base + g->ret != ret) continue;
  if (g->timer) { need(g->arg < 31 && g->arg != 18, "timer target register"); regs[g->arg] = (uint64_t)((__uint128_t)runtime_now_ns() * 12 / 625); return; }   /* 19.2 MHz */   /* 19.2 MHz */
  runtime_svc(g->arg, regs, modules[i].name, g->original); return;
 }
 runtime_fail("unknown gate return address");
}

/* ---------- faults ---------- */
static void signal_hex(const char *label, uint64_t value) {
 char b[128]; size_t n = 0; while (*label) b[n++] = *label++; b[n++] = '0'; b[n++] = 'x';
 for (int i = 15; i >= 0; i--) b[n++] = "0123456789abcdef"[(value >> (i * 4)) & 15]; b[n++] = '\n'; write(2, b, n);
}
/* CTR_EL0 as the guest reads it (Darwin traps the read). The line sizes are what cache maintenance steps by, so they must
   not exceed the real ones: 32 bytes (IminLine = DminLine = 3). The JIT's flush (libnx armDCacheFlush + armICacheInvalidate
   on the executable view) stepping by 128 bytes left stale instructions on an M4 (tests/jit_cache_probe.c: 128-byte steps
   fail, 64 and below hold); ERG = CWG = 5, PIPT I-cache. */
#define HOST_CTR_EL0 0x8553c003ULL
static void crash(int num, siginfo_t *info, void *context) {
 ucontext_t *u = context;
 if (num == SIGILL) {   /* EL0 cache maintenance Darwin traps but Horizon allows (the JIT's cache flush) */
  uint32_t insn = *(const uint32_t *)(uintptr_t)u->uc_mcontext->__ss.__pc; unsigned rt = insn & 31;
  if ((insn & ~31u) == 0xd53b0020u) { if (rt != 31) u->uc_mcontext->__ss.__x[rt] = HOST_CTR_EL0; u->uc_mcontext->__ss.__pc += 4; return; }   /* mrs Xt, CTR_EL0 */
  if ((insn & ~31u) == 0xd50b7e20u || (insn & ~31u) == 0xd50b7b20u) {   /* dc civac / dc cvau, Xt */
   uintptr_t a = rt == 31 ? 0 : (uintptr_t)u->uc_mcontext->__ss.__x[rt]; sys_dcache_flush((void *)(a & ~(uintptr_t)127), 128); u->uc_mcontext->__ss.__pc += 4; return; }
  if ((insn & ~31u) == 0xd50b7520u) {   /* ic ivau, Xt */
   uintptr_t a = rt == 31 ? 0 : (uintptr_t)u->uc_mcontext->__ss.__x[rt]; sys_icache_invalidate((void *)(a & ~(uintptr_t)127), 128); u->uc_mcontext->__ss.__pc += 4; return; }
 }
 uint64_t pc = u->uc_mcontext->__ss.__pc, fp = u->uc_mcontext->__ss.__fp;
 signal_hex("[totk] signal=", num); signal_hex("fault=", (uintptr_t)info->si_addr); signal_hex("esr=", u->uc_mcontext->__es.__esr);
 signal_hex("pc=", pc); signal_hex("lr=", u->uc_mcontext->__ss.__lr); signal_hex("fp=", fp);
 { uint32_t w[8]; mach_vm_size_t got = 0; if (mach_vm_read_overwrite(mach_task_self(), (pc & ~3ULL) - 16, 32, (uintptr_t)w, &got) == KERN_SUCCESS && got == 32) for (unsigned i = 0; i < 8; i++) signal_hex(i == 4 ? "insn at pc=" : "insn=", w[i]); }
 for (unsigned i = 0; i < NM; i++) if (pc >= (uintptr_t)modules[i].base && pc < (uintptr_t)modules[i].base + modules[i].size) { write(2, modules[i].name, strlen(modules[i].name)); signal_hex(" offset=", pc - (uintptr_t)modules[i].base); }
 for (unsigned i = 0; i < 29; i++) signal_hex("x=", u->uc_mcontext->__ss.__x[i]); signal_hex("sp=", u->uc_mcontext->__ss.__sp);
 for (unsigned depth = 0; depth < 24 && fp && !(fp & 15); depth++) {
  uint64_t frame[2]; mach_vm_size_t got = 0;
  if (mach_vm_read_overwrite(mach_task_self(), fp, 16, (uintptr_t)frame, &got) != KERN_SUCCESS || got != 16) break;
  signal_hex("frame lr=", frame[1]);
  for (unsigned i = 0; i < NM; i++) if (frame[1] >= (uintptr_t)modules[i].base && frame[1] < (uintptr_t)modules[i].base + modules[i].size) { write(2, modules[i].name, strlen(modules[i].name)); signal_hex(" offset=", frame[1] - (uintptr_t)modules[i].base); }
  if (frame[0] == fp) break; fp = frame[0];
 }
 _exit(128 + num);
}
static void abort_report(int num) {
 (void)num; void *frames[64]; int n = backtrace(frames, 64); write(2, "[totk] host abort, call stack:\n", 31); backtrace_symbols_fd(frames, n, 2);
 signal(SIGABRT, SIG_DFL); raise(SIGABRT);
}

/* ---------- what the game prints ----------
   libnx sends stdout and stderr to its "stdnull" device unless the game opens a console. Before the game starts, the
   device's write_r (an unrelocated offset that libnx's __nx_dynamic turns into an address by adding the base) is set so
   that it lands on a host function: the text reaches this log line by line, "[guest] " in front. HB_GUEST_STDOUT=0 keeps
   it silent. */
static pthread_mutex_t guest_out_lock = PTHREAD_MUTEX_INITIALIZER;
static char guest_line[1024]; static size_t guest_used;
static int64_t guest_write(void *reent, void *fd, const char *p, size_t n) {
 (void)reent; (void)fd; pthread_mutex_lock(&guest_out_lock);
 for (size_t i = 0; i < n; i++) {
  char c = p[i];
  if (c == '\n' || guest_used == sizeof guest_line - 1) { fprintf(stderr, "[guest] %.*s\n", (int)guest_used, guest_line); guest_used = 0; if (c == '\n') continue; }
  guest_line[guest_used++] = c;
 }
 pthread_mutex_unlock(&guest_out_lock); return (int64_t)n;
}
static void guest_stdout(Module *m) {
 const char *e = getenv("HB_GUEST_STDOUT"); if ((e && !strcmp(e, "0")) || !GUEST_STDNULL_DEVOPTAB) return;
 uint64_t *dev = (uint64_t *)(m->base + GUEST_STDNULL_DEVOPTAB);
 need(dev[0] < m->data_off && !memcmp(m->base + dev[0], "stdnull", 8) && dev[4], "libnx stdnull device not where prepared");
 dev[4] = (uint64_t)(uintptr_t)runtime_hle_wrap_named((void *)guest_write, "guest stdout") - (uintptr_t)m->base;
}

/* ---------- start ---------- */
/* Diagnostics (TOTK_INPUT_FILE "hbregs"): the guest main thread's registers where it is now (SIGUSR1). */
static pthread_t guest_main_thread; static int guest_main_started;
static void regs_signal(int num, siginfo_t *info, void *context) {
 (void)num; (void)info; ucontext_t *u = context; uint64_t pc = u->uc_mcontext->__ss.__pc;
 signal_hex("[hbregs] pc=", pc);
 for (unsigned i = 0; i < NM; i++) if (pc >= (uintptr_t)modules[i].base && pc < (uintptr_t)modules[i].base + modules[i].size) signal_hex("[hbregs] main+", pc - (uintptr_t)modules[i].base);
 signal_hex("[hbregs] lr=", u->uc_mcontext->__ss.__lr); signal_hex("[hbregs] sp=", u->uc_mcontext->__ss.__sp);
 for (unsigned i = 0; i < 29; i++) signal_hex("[hbregs] x=", u->uc_mcontext->__ss.__x[i]);
 { uint64_t w[256]; mach_vm_size_t got = 0; if (mach_vm_read_overwrite(mach_task_self(), u->uc_mcontext->__ss.__sp, sizeof w, (uintptr_t)w, &got) == KERN_SUCCESS)
    for (unsigned i = 0; i < got / 8; i++) if (w[i] >= 0x400000 && w[i] < 0x20000000) { signal_hex("[hbregs] stack slot=", i); signal_hex("[hbregs]   x86-like=", w[i]); } }
 uint64_t fp = u->uc_mcontext->__ss.__fp;
 for (unsigned depth = 0; depth < 12 && fp && !(fp & 15); depth++) {
  uint64_t frame[2]; mach_vm_size_t got = 0;
  if (mach_vm_read_overwrite(mach_task_self(), fp, 16, (uintptr_t)frame, &got) != KERN_SUCCESS || got != 16) break;
  for (unsigned i = 0; i < NM; i++) if (frame[1] >= (uintptr_t)modules[i].base && frame[1] < (uintptr_t)modules[i].base + modules[i].size) signal_hex("[hbregs] caller main+", frame[1] - (uintptr_t)modules[i].base);
  if (frame[0] <= fp) break; fp = frame[0];
 }
}
void hb_guest_regs(void) {
 static int installed; if (!installed) { struct sigaction a = {0}; a.sa_sigaction = regs_signal; a.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&a.sa_mask); sigaction(SIGUSR1, &a, NULL); installed = 1; }
 if (guest_main_started) pthread_kill(guest_main_thread, SIGUSR1);
}
static void *guest_main(void *unused) {
 (void)unused; guest_main_thread = pthread_self(); guest_main_started = 1;
 runtime_thread_init(); uint32_t main_handle = svc_thread_main_init();
 fprintf(stderr, "[totk] ENTER crt0 main+0x0 (NSO entry: x0=0, x1=main thread %#x)\n", main_handle);
 ((void (*)(uint64_t, uint64_t))(void *)modules[0].base)(0, main_handle);
 runtime_fail("crt0 returned");
}
static void boot(void) {
 runtime_boot_status("启动游戏", "Starting the game");
 /* The guest main thread runs on a host thread with the stack its NPDM asks for (at least 8 MiB: the gate keeps its
    800-byte frame and the host services on this same stack). */
 pthread_attr_t a; pthread_attr_init(&a);
 size_t stack = GAME_MAIN_STACK_SIZE < (8u << 20) ? (8u << 20) : page_up(GAME_MAIN_STACK_SIZE);
 pthread_attr_setstacksize(&a, stack);
 pthread_t t; need(pthread_create(&t, &a, guest_main, NULL) == 0, "guest main thread"); pthread_attr_destroy(&a);
 pthread_detach(t);
}
int main(int argc, char **argv) {
 need(argc == 3, "usage: mj2013-native <rewritten dir> boot|probe|export-ios");
 need(sysconf(_SC_PAGESIZE) == 0x4000, "expected 16KB Apple pages");
 struct sigaction a = {0}; a.sa_sigaction = crash; a.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&a.sa_mask);
 sigaction(SIGSEGV, &a, NULL); sigaction(SIGBUS, &a, NULL); sigaction(SIGILL, &a, NULL); sigaction(SIGTRAP, &a, NULL);
 { struct sigaction ab = {0}; ab.sa_handler = abort_report; ab.sa_flags = SA_ONSTACK; sigemptyset(&ab.sa_mask); sigaction(SIGABRT, &ab, NULL); }
 for (unsigned i = 0; i < 8; i++) need(pthread_key_create(&runtime_keys[i], NULL) == 0 && runtime_keys[i] != 0, "allocate Darwin key");
 runtime_hle_stack_init((uint64_t)runtime_keys[4] * 8);
 need(pthread_key_create(&environment_key, environment_destroy) == 0, "thread environment key");
 for (unsigned i = 0; i < NM; i++) map_module(&modules[i], argv[1]);
 runtime_game_patches(modules[0].base, modules[0].text_end);
 guest_stdout(&modules[0]);
#if !TARGET_OS_IPHONE
 if (!strcmp(argv[2], "export-ios")) { const char *out = getenv("TOTK_EXPORT_IOS"); need(out != NULL, "export-ios needs TOTK_EXPORT_IOS=<dir>"); export_ios_text(argv[1], out); return 0; }
#endif
 for (unsigned i = 0; i < NM; i++) seal(&modules[i]);
 hb_services_init();
 if (!strcmp(argv[2], "probe")) { printf("{\"status\":\"pass\",\"module\":\"main\",\"gates\":%u,\"scope\":\"mapped, patched and sealed; not entered\"}\n", modules[0].ngate); return 0; }
 need(!strcmp(argv[2], "boot"), "unknown mode");
 if (getenv("TOTK_HEADLESS")) { boot(); for (;;) pause(); }
 runtime_front_run(boot);
 return 0;
}
