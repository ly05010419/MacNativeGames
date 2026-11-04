/* Memory scan, watchpoints and breakpoints (Mac diagnostics; written for #285 to find which game code moves the map
   and Ultrahand cursors; docs/memory-scan-diagnostics.md). Included at the end of runtime/services.c, which knows the
   guest memory (the heap and the alias region's mapped pages). Nothing runs until a command arrives through the runtime
   command channel (input.txt, runtime/input.c); each command appends its result to memscan.txt next to controls.txt.
     mscan new [f32|i32]          snapshot every resident page the game has mapped; the values are 4-byte aligned words
     mscan changed | unchanged | inc | dec
                                  keep the candidates whose value did that since the step before (the first step
                                  compares with the snapshot and then frees it); f32 keeps only finite values under 1e6
     mscan show [n]               the candidates and their values now (n, default 40)
     mscan watch <hex address> <seconds> [rw]
                                  a hardware watchpoint on those 4 bytes in the guest threads: every instruction that
                                  writes there (rw: also reads), with its count, return address, callers (frame-pointer
                                  chain), integer and float registers (offsets from main). After a hit a breakpoint on the
                                  next instruction arms the watch again, so every access of a frame is seen. Guest stack
                                  addresses are refused (a slot reused by other code took the game down).
     mscan bp <hex main offset> <seconds>
                                  a hardware breakpoint on one instruction: how often it runs, with registers and callers
     mscan code <hex main offset> <words>
                                  words read from main + offset (any address: offset = address - main), for disassembly
     mscan poke <hex address> <value>
                                  writes a value of the scan type (to see a candidate move on screen)
     mscan base                   module bases, the heap and the alias region
   Reads go through mach_vm_read_overwrite, so a page the game unmaps during a scan is skipped instead of faulting, and
   only pages mincore reports as touched are read, so a scan does not make mapped but untouched memory resident. */
#if !TARGET_OS_IPHONE
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <mach/thread_act.h>

typedef struct { uint64_t start, len; uint8_t *data; } ScanChunk;
static ScanChunk *scan_chunks; static size_t scan_nchunks, scan_capchunks; static uint64_t scan_snap_bytes;
static uint64_t *scan_addr; static uint32_t *scan_val; static size_t scan_n, scan_cap;
static int scan_i32, scan_have;   /* scan_have: 0 nothing, 1 snapshot, 2 candidates */
enum { SCAN_MAX_CANDIDATES = 150000000 };

static void scan_out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void scan_out(const char *fmt, ...) {
    static char path[4096]; static int ready;
    if (!ready) {
        const char *c = getenv("TOTK_CONTROLS"), *s = c ? strrchr(c, '/') : NULL;
        if (s) snprintf(path, sizeof path, "%.*s/memscan.txt", (int)(s - c), c); else snprintf(path, sizeof path, "memscan.txt");
        ready = 1;
    }
    FILE *f = fopen(path, "a"); if (!f) return;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); struct tm tm; localtime_r(&ts.tv_sec, &tm);
    fprintf(f, "%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fputc('\n', f); fclose(f);
}
static double scan_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int scan_read(uint64_t at, void *to, uint64_t len) {
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), at, len, (mach_vm_address_t)(uintptr_t)to, &got) == KERN_SUCCESS && got == len;
}

/* The guest memory the game has mapped, as runs [start, end): the heap, then the alias region's mapped 4 KiB pages.
   Read without memory_lock: a page may change state during a scan, which scan_read tolerates. */
static void scan_ranges(void (*each)(uint64_t start, uint64_t end)) {
    if (heap && heap_size) each((uintptr_t)heap, (uintptr_t)heap + heap_size);
    if (!alias_space) return;
    uint64_t n = ALIAS_SIZE / 4096;
    for (uint64_t i = 0; i < n;) {
        if (!alias_pages[i]) { i++; continue; }
        uint64_t j = i; while (j < n && alias_pages[j]) j++;
        each(ALIAS_START + i * 4096, ALIAS_START + j * 4096); i = j;
    }
}
static void scan_add_chunk(uint64_t start, uint64_t end) {
    while (start < end) {   /* a page that cannot be read ends the chunk; the rest is tried page by page */
        uint64_t len = end - start; uint8_t *d = malloc(len); if (!d) { scan_out("error: out of memory for %llu bytes", (unsigned long long)len); return; }
        if (scan_read(start, d, len)) {
            if (scan_nchunks == scan_capchunks) { scan_capchunks = scan_capchunks ? scan_capchunks * 2 : 1024; scan_chunks = realloc(scan_chunks, scan_capchunks * sizeof *scan_chunks); }
            scan_chunks[scan_nchunks++] = (ScanChunk){start, len, d}; scan_snap_bytes += len; return;
        }
        free(d);
        if (len <= 4096) return;
        uint64_t mid = start + ((len / 2 + 4095) & ~4095ULL); scan_add_chunk(start, mid); start = mid;
    }
}
static void scan_snap_range(uint64_t start, uint64_t end) {   /* only the host pages mincore reports as touched */
    uint64_t pg = vm_page_size, a = start & ~(pg - 1), run = 0; int in = 0;
    static char vec[4096];
    while (a < end) {
        uint64_t span = end - a; if (span > sizeof vec * pg) span = sizeof vec * pg;
        if (mincore((void *)(uintptr_t)a, span, vec)) { if (in) { scan_add_chunk(run, a); in = 0; } a += span; continue; }
        for (uint64_t k = 0; k * pg < span; k++) {
            uint64_t p = a + k * pg, lo = p < start ? start : p;
            if (vec[k] & (MINCORE_INCORE | MINCORE_PAGED_OUT)) { if (!in) { run = lo; in = 1; } }   /* not MINCORE_ANONYMOUS alone: never touched */
            else if (in) { scan_add_chunk(run, p); in = 0; }
        }
        a += span;
    }
    if (in) scan_add_chunk(run, end);
}
static void scan_free_snapshot(void) {
    for (size_t i = 0; i < scan_nchunks; i++) free(scan_chunks[i].data);
    free(scan_chunks); scan_chunks = NULL; scan_nchunks = scan_capchunks = 0; scan_snap_bytes = 0;
}
static void scan_free_candidates(void) { free(scan_addr); free(scan_val); scan_addr = NULL; scan_val = NULL; scan_n = scan_cap = 0; }

enum { KEEP_CHANGED, KEEP_UNCHANGED, KEEP_INC, KEEP_DEC };
static int scan_plausible(uint32_t w) { float f; memcpy(&f, &w, 4); return isfinite(f) && fabsf(f) < 1e6f; }
static int scan_keep(int mode, uint32_t old, uint32_t now) {
    if (!scan_i32 && (!scan_plausible(now) || !scan_plausible(old))) return 0;
    if (mode == KEEP_CHANGED) return now != old;
    if (mode == KEEP_UNCHANGED) return now == old;
    if (scan_i32) { int32_t a = (int32_t)old, b = (int32_t)now; return mode == KEEP_INC ? b > a : b < a; }
    float a, b; memcpy(&a, &old, 4); memcpy(&b, &now, 4); return mode == KEEP_INC ? b > a : b < a;
}
static int scan_push(uint64_t addr, uint32_t v) {
    if (scan_n == scan_cap) {
        if (scan_cap >= SCAN_MAX_CANDIDATES) return 0;
        size_t c = scan_cap ? scan_cap * 2 : 1 << 20; if (c > SCAN_MAX_CANDIDATES) c = SCAN_MAX_CANDIDATES;
        uint64_t *na = realloc(scan_addr, c * sizeof *na); uint32_t *nv = realloc(scan_val, c * sizeof *nv);
        if (!na || !nv) { if (na) scan_addr = na; if (nv) scan_val = nv; return 0; }
        scan_addr = na; scan_val = nv; scan_cap = c;
    }
    scan_addr[scan_n] = addr; scan_val[scan_n] = v; scan_n++; return 1;
}
static void scan_filter(int mode, const char *name) {
    double t0 = scan_now(); static uint32_t buf[(1 << 20) / 4]; int full = 0;
    if (scan_have == 1) {   /* first step: the snapshot against memory now */
        for (size_t c = 0; c < scan_nchunks && !full; c++) {
            const ScanChunk *k = &scan_chunks[c];
            for (uint64_t off = 0; off < k->len && !full; off += sizeof buf) {
                uint64_t len = k->len - off; if (len > sizeof buf) len = sizeof buf;
                if (!scan_read(k->start + off, buf, len)) continue;   /* unmapped since: its candidates are gone */
                const uint32_t *old = (const uint32_t *)(k->data + off);
                for (uint64_t i = 0; i < len / 4; i++)
                    if (scan_keep(mode, old[i], buf[i]) && !scan_push(k->start + off + i * 4, buf[i])) { full = 1; break; }
            }
        }
        scan_free_snapshot(); scan_have = 2;
        if (full) scan_out("warning: stopped at %d candidates; the rest of memory was not compared (narrow with another step, or start again)", SCAN_MAX_CANDIDATES);
    } else {   /* later steps: each candidate's word now, read 64 KiB at a time (candidates are in address order) */
        size_t kept = 0; static uint8_t win[1 << 16]; uint64_t wbase = 1; int wok = 0;
        for (size_t i = 0; i < scan_n; i++) {
            uint64_t a = scan_addr[i], b = a & ~0xffffULL;
            if (b != wbase) { wbase = b; wok = scan_read(b, win, sizeof win); }
            uint32_t now;
            if (wok) memcpy(&now, win + (a - b), 4); else if (!scan_read(a, &now, 4)) continue;
            if (scan_keep(mode, scan_val[i], now)) { scan_addr[kept] = a; scan_val[kept] = now; kept++; }
        }
        scan_n = kept;
    }
    scan_out("%s: %zu candidates (%.1f s)", name, scan_n, scan_now() - t0);
}
static void scan_print_word(char *s, size_t n, uint32_t w) {
    if (scan_i32) snprintf(s, n, "%d", (int32_t)w); else { float f; memcpy(&f, &w, 4); snprintf(s, n, "%.6g", f); }
}
static void scan_show(size_t limit) {
    if (scan_have != 2) { scan_out("show: no candidates yet (mscan new, then a step)"); return; }
    scan_out("show: %zu candidates, first %zu (address, value at the last step, value now, where)", scan_n, scan_n < limit ? scan_n : limit);
    for (size_t i = 0; i < scan_n && i < limit; i++) {
        uint32_t now = 0; int ok = scan_read(scan_addr[i], &now, 4); char a[32], b[32];
        scan_print_word(a, sizeof a, scan_val[i]); if (ok) scan_print_word(b, sizeof b, now); else snprintf(b, sizeof b, "unmapped");
        uint64_t x = scan_addr[i];
        if (heap && x >= (uintptr_t)heap && x < (uintptr_t)heap + heap_size) scan_out("  %#llx  %s  %s  heap+%#llx", (unsigned long long)x, a, b, (unsigned long long)(x - (uintptr_t)heap));
        else scan_out("  %#llx  %s  %s  alias+%#llx", (unsigned long long)x, a, b, (unsigned long long)(x - ALIAS_START));
    }
}

/* The watchpoint: the same mechanism as 1.4.3's frmwatch (game_patches_v143.c), for any address, write or read/write. */
static _Atomic uint64_t scan_watch_addr; static int scan_watch_rw; static _Atomic int scan_stepping; static _Atomic uint64_t scan_bp_addr;
enum { SCAN_SLOTS = 256 };
enum { SCAN_CHAIN = 10 };
static struct { _Atomic uint64_t pc; _Atomic uint64_t lr; _Atomic uint32_t n; _Atomic int filled; uint64_t x[31], sp, chain[SCAN_CHAIN]; float v[16]; } scan_hits[SCAN_SLOTS];
static void scan_watch_set(thread_act_t t, uint64_t addr) {
    arm_debug_state64_t d; mach_msg_type_number_t c = ARM_DEBUG_STATE64_COUNT;
    if (thread_get_state(t, ARM_DEBUG_STATE64, (thread_state_t)&d, &c) != KERN_SUCCESS) return;
    /* WCR: enable, EL0 (PAC 2), load/store control (1 load, 2 store, 3 both), byte select for 4 bytes at addr */
    int watch = addr > 1;   /* addr 1: the code breakpoint alone (mscan bp) */
    d.__wvr[1] = watch ? (addr & ~7ULL) : 0;
    d.__wcr[1] = watch ? ((0xfULL << ((addr & 7) + 5)) | ((scan_watch_rw ? 3u : 2u) << 3) | (2u << 1) | 1u) : 0;
    if (!addr) { d.__bvr[1] = 0; d.__bcr[1] = 0; }   /* the step breakpoint (scan_step) goes with the watch */
    uint64_t bp = atomic_load(&scan_bp_addr);
    d.__bvr[2] = bp ? (bp & ~3ULL) : 0; d.__bcr[2] = bp ? ((0xfu << 5) | (2u << 1) | 1u) : 0;
    thread_set_state(t, ARM_DEBUG_STATE64, (thread_state_t)&d, ARM_DEBUG_STATE64_COUNT);
}
/* After a hit the watch is off on that thread and a breakpoint sits on the next instruction (the access itself is no
   branch); when it fires the watch is armed again, so every access of the frame is seen, not only the first one. */
static void scan_step(uint64_t next_pc, uint64_t watch) {
    thread_act_t self = mach_thread_self();
    arm_debug_state64_t d; mach_msg_type_number_t c = ARM_DEBUG_STATE64_COUNT;
    if (thread_get_state(self, ARM_DEBUG_STATE64, (thread_state_t)&d, &c) == KERN_SUCCESS) {
        if (next_pc) { d.__wcr[1] = 0; d.__bvr[1] = next_pc & ~3ULL; d.__bcr[1] = (0xfu << 5) | (2u << 1) | 1u; }
        else {
            d.__bvr[1] = 0; d.__bcr[1] = 0;
            if (watch) { d.__wvr[1] = watch & ~7ULL; d.__wcr[1] = (0xfULL << ((watch & 7) + 5)) | ((scan_watch_rw ? 3u : 2u) << 3) | (2u << 1) | 1u; }
            uint64_t bp = atomic_load(&scan_bp_addr);
            if (bp) { d.__bvr[2] = bp & ~3ULL; d.__bcr[2] = (0xfu << 5) | (2u << 1) | 1u; }
        }
        thread_set_state(self, ARM_DEBUG_STATE64, (thread_state_t)&d, ARM_DEBUG_STATE64_COUNT);
    }
    mach_port_deallocate(mach_task_self(), self);
}
int runtime_guest_thread_ports(unsigned *out, int max);   /* runtime/threads.c */
static void scan_watch_all(uint64_t addr) {   /* only the game's threads: a host thread may not take the trap */
    unsigned ports[512]; int n = runtime_guest_thread_ports(ports, 512); thread_act_t self = mach_thread_self();
    for (int i = 0; i < n; i++)
        if (ports[i] != self && thread_suspend(ports[i]) == KERN_SUCCESS) { scan_watch_set(ports[i], addr); thread_resume(ports[i]); }
    mach_port_deallocate(mach_task_self(), self);
}
static void scan_record(ucontext_t *u, uint64_t pc) {
    for (unsigned i = (unsigned)(pc >> 2) % SCAN_SLOTS, k = 0; k < SCAN_SLOTS; k++, i = (i + 1) % SCAN_SLOTS) {
        uint64_t z = 0;
        if (atomic_compare_exchange_strong(&scan_hits[i].pc, &z, pc) || z == pc) {
            atomic_store(&scan_hits[i].lr, u->uc_mcontext->__ss.__lr); atomic_fetch_add(&scan_hits[i].n, 1);
            int f = 0;
            if (atomic_compare_exchange_strong(&scan_hits[i].filled, &f, 1)) {
                memcpy(scan_hits[i].x, u->uc_mcontext->__ss.__x, 29 * 8); scan_hits[i].x[29] = u->uc_mcontext->__ss.__fp;
                scan_hits[i].x[30] = u->uc_mcontext->__ss.__lr; scan_hits[i].sp = u->uc_mcontext->__ss.__sp;
                for (int r = 0; r < 16; r++) { __uint128_t q = u->uc_mcontext->__ns.__v[r]; uint32_t w32 = (uint32_t)q; memcpy(&scan_hits[i].v[r], &w32, 4); }
                /* the callers: the frame-pointer chain, followed only while it stays 8-byte aligned and inside the 1 MiB
                   above sp (this thread's stack), so a bad link ends the walk instead of faulting */
                uint64_t sp = u->uc_mcontext->__ss.__sp, fp = u->uc_mcontext->__ss.__fp;
                for (int c = 0; c < SCAN_CHAIN; c++) {
                    scan_hits[i].chain[c] = 0;
                    if (fp & 7 || fp < sp || fp >= sp + (1 << 20)) continue;   /* game frames are 8-byte aligned (fp = sp + 0x48 seen) */
                    uint64_t next = ((const uint64_t *)(uintptr_t)fp)[0], ret = ((const uint64_t *)(uintptr_t)fp)[1];
                    scan_hits[i].chain[c] = ret; if (next <= fp) fp = 0; else fp = next;
                }
                atomic_store(&scan_hits[i].filled, 2);
            }
            break;
        }
    }
}
int runtime_mem_scan_trap(void *context) {   /* loader.c crash(): 1 when this SIGTRAP was the scan watchpoint */
    ucontext_t *u = context; uint64_t w = atomic_load(&scan_watch_addr);
    unsigned ec = (unsigned)(u->uc_mcontext->__es.__esr >> 26); uint64_t far = u->uc_mcontext->__es.__far;
    /* any watchpoint exception while the scan's watch is armed is ours: a wide access (a double, a register pair, a vector)
       reports its own start in FAR, which can lie below the watched word (2026-10-27: the game closed on such a store) */
    (void)far;
    if (ec == 0x30 || ec == 0x31) {
        uint64_t bp = atomic_load(&scan_bp_addr), pc0 = u->uc_mcontext->__ss.__pc;
        if (bp && pc0 == bp) {   /* the code breakpoint: record it like a watch hit, step over it with slot 1 */
            scan_record(u, pc0);
            thread_act_t self = mach_thread_self(); arm_debug_state64_t d; mach_msg_type_number_t c = ARM_DEBUG_STATE64_COUNT;
            if (thread_get_state(self, ARM_DEBUG_STATE64, (thread_state_t)&d, &c) == KERN_SUCCESS) {
                d.__bcr[2] = 0; d.__bvr[1] = (pc0 + 4) & ~3ULL; d.__bcr[1] = (0xfu << 5) | (2u << 1) | 1u;
                thread_set_state(self, ARM_DEBUG_STATE64, (thread_state_t)&d, ARM_DEBUG_STATE64_COUNT);
            }
            mach_port_deallocate(mach_task_self(), self); return 1;
        }
        /* the step breakpoint: the instruction ran, arm the watch and the code breakpoint again (also late ones) */
        if (!scan_stepping) return 0;
        scan_step(0, w); return 1;
    }
    if (!w || (ec != 0x34 && ec != 0x35)) return 0;
    uint64_t pc = u->uc_mcontext->__ss.__pc;
    scan_record(u, pc);
    scan_step(pc + 4, 0);   /* the access runs unwatched, then the breakpoint after it arms the watch again */
    return 1;
}
static void scan_where(char *s, size_t n, uint64_t a) {   /* the closest module base below the address names it */
    static const char *names[] = {"rtld", "main", "subsdk0", "subsdk1", "sdk"};
    uint64_t best = 0; const char *which = NULL;
    for (unsigned i = 0; i < 5; i++) { uint8_t *b = runtime_module_base(names[i]); if (b && (uintptr_t)b <= a && (uintptr_t)b > best) { best = (uintptr_t)b; which = names[i]; } }
    if (which && a - best < 0x8000000ULL) snprintf(s, n, "%s+%#llx", which, (unsigned long long)(a - best));
    else snprintf(s, n, "%#llx (host)", (unsigned long long)a);
}
static void scan_report(void) {
    unsigned total = 0, sites = 0;
    for (unsigned i = 0; i < SCAN_SLOTS; i++) {
        uint64_t pc = atomic_load(&scan_hits[i].pc); if (!pc) continue;
        char p[64], l[64]; scan_where(p, sizeof p, pc); scan_where(l, sizeof l, atomic_load(&scan_hits[i].lr));
        uint32_t n = atomic_load(&scan_hits[i].n); total += n; sites++;
        scan_out("  hit %s x%u, return %s", p, n, l);
        if (atomic_load(&scan_hits[i].filled) == 2) {
            char line[1400]; int o = 0;
            for (int r = 0; r < 31; r++) o += snprintf(line + o, sizeof line - o, "%sx%d=%llx", r ? " " : "", r, (unsigned long long)scan_hits[i].x[r]);
            snprintf(line + o, sizeof line - o, " sp=%llx", (unsigned long long)scan_hits[i].sp);
            scan_out("    %s", line);
            o = 0;
            for (int c = 0; c < SCAN_CHAIN && scan_hits[i].chain[c]; c++) { char w[64]; scan_where(w, sizeof w, scan_hits[i].chain[c]); o += snprintf(line + o, sizeof line - o, "%s%s", c ? " <- " : "", w); }
            if (o) scan_out("    callers: %s", line);
            o = 0;
            for (int r = 0; r < 16; r++) o += snprintf(line + o, sizeof line - o, "%ss%d=%g", r ? " " : "", r, scan_hits[i].v[r]);
            scan_out("    %s", line);
        }
    }
    scan_out("done: %u hits at %u instructions", total, sites);
}
static void scan_watch(uint64_t addr, double secs, int rw) {
    if (addr & 3) { scan_out("watch: the address must be a multiple of 4"); return; }
    uint32_t probe; if (!scan_read(addr, &probe, 4)) { scan_out("watch: %#llx is not mapped", (unsigned long long)addr); return; }
    if (low_space && addr >= (uintptr_t)low_space + STACK_OFF && addr < (uintptr_t)low_space + STACK_OFF + STACK_SIZE) {
        /* a stack slot is written by every function that reuses it, some in contexts where the trap cannot be taken
           (2026-10-27: the game closed); watch the heap copy instead */
        scan_out("watch: %#llx is on a guest thread's stack; not watched", (unsigned long long)addr); return;
    }
    for (unsigned i = 0; i < SCAN_SLOTS; i++) { atomic_store(&scan_hits[i].pc, 0); atomic_store(&scan_hits[i].n, 0); atomic_store(&scan_hits[i].filled, 0); }
    scan_watch_rw = rw; atomic_store(&scan_stepping, 1); atomic_store(&scan_watch_addr, addr);
    scan_out("watch: %#llx for %.0f s (%s)", (unsigned long long)addr, secs, rw ? "reads and writes" : "writes");
    for (double t = 0; t < secs; t += 0.01) { scan_watch_all(addr); usleep(10000); }
    scan_watch_all(0); atomic_store(&scan_watch_addr, 0); usleep(20000);
    scan_report();
}
static void scan_base(void) {
    static const char *names[] = {"rtld", "main", "subsdk0", "subsdk1", "sdk"};
    for (unsigned i = 0; i < 5; i++) { uint8_t *b = runtime_module_base(names[i]); if (b) scan_out("base %s %#llx", names[i], (unsigned long long)(uintptr_t)b); }
    scan_out("base heap %#llx size %#llx, alias %#llx mapped %llu MiB", (unsigned long long)(uintptr_t)heap, (unsigned long long)heap_size,
             (unsigned long long)ALIAS_START, (unsigned long long)(alias_bytes >> 20));
}
static pthread_mutex_t scan_lock = PTHREAD_MUTEX_INITIALIZER;
int runtime_mem_scan_command(const char *line) {   /* runtime/input.c: the text after "mscan "; always 1 (errors go to memscan.txt) */
    char cmd[32] = {0}, arg[64] = {0}; double secs; unsigned long long a; float fv; long long iv; unsigned words;
    if (sscanf(line, "%31s %63[^\n]", cmd, arg) < 1) { scan_out("error: empty mscan command"); return 1; }
    pthread_mutex_lock(&scan_lock);
    if (!strcmp(cmd, "new")) {
        scan_free_snapshot(); scan_free_candidates(); scan_i32 = !strncmp(arg, "i32", 3);
        double t0 = scan_now(); scan_ranges(scan_snap_range); scan_have = 1;
        scan_out("new (%s): snapshot of %llu MiB in %zu chunks (%.1f s)", scan_i32 ? "i32" : "f32", (unsigned long long)(scan_snap_bytes >> 20), scan_nchunks, scan_now() - t0);
    } else if (!strcmp(cmd, "changed") || !strcmp(cmd, "unchanged") || !strcmp(cmd, "inc") || !strcmp(cmd, "dec")) {
        if (!scan_have) scan_out("%s: no snapshot (mscan new first)", cmd);
        else scan_filter(cmd[0] == 'c' ? KEEP_CHANGED : cmd[0] == 'u' ? KEEP_UNCHANGED : cmd[0] == 'i' ? KEEP_INC : KEEP_DEC, cmd);
    } else if (!strcmp(cmd, "show")) {
        unsigned n = 40; if (*arg) sscanf(arg, "%u", &n); scan_show(n);
    } else if (!strcmp(cmd, "watch")) {
        char mode[8] = {0};
        if (sscanf(arg, "%llx %lf %7s", &a, &secs, mode) < 2 || secs <= 0 || secs > 120) scan_out("error: mscan watch <hex address> <seconds 1-120> [rw]");
        else scan_watch(a, secs, !strcmp(mode, "rw"));
    } else if (!strcmp(cmd, "bp")) {   /* "bp <hex main offset> <seconds>": count executions of one instruction, with registers */
        uint8_t *m = runtime_module_base("main");
        if (!m || sscanf(arg, "%llx %lf", &a, &secs) != 2 || secs <= 0 || secs > 120) scan_out("error: mscan bp <hex main offset> <seconds>");
        else {
            for (unsigned i = 0; i < SCAN_SLOTS; i++) { atomic_store(&scan_hits[i].pc, 0); atomic_store(&scan_hits[i].n, 0); atomic_store(&scan_hits[i].filled, 0); }
            atomic_store(&scan_stepping, 1); atomic_store(&scan_bp_addr, (uintptr_t)m + a);
            scan_out("bp: main+%#llx for %.1f s", a, secs);
            uint64_t none = 0;
            for (double t = 0; t < secs; t += 0.01) { scan_watch_all(1); usleep(10000); }   /* addr 1: no watch (wcr off below) */
            atomic_store(&scan_bp_addr, 0); scan_watch_all(0); (void)none; usleep(20000);
            scan_report();
        }
    } else if (!strcmp(cmd, "code")) {
        uint8_t *m = runtime_module_base("main");
        if (!m || sscanf(arg, "%llx %u", &a, &words) != 2 || !words || words > 4096) scan_out("error: mscan code <hex main offset> <words 1-4096>");
        else for (unsigned i = 0; i < words; i++) {
            uint32_t w; if (!scan_read((uintptr_t)m + a + i * 4, &w, 4)) { scan_out("code: main+%#llx not readable", a + i * 4); break; }
            scan_out("code main+%#llx %08x", a + i * 4, w);
        }
    } else if (!strcmp(cmd, "poke")) {
        uint32_t w; int ok = 0;
        if (scan_i32 ? sscanf(arg, "%llx %lld", &a, &iv) == 2 : sscanf(arg, "%llx %f", &a, &fv) == 2) {
            if (scan_i32) { int32_t v = (int32_t)iv; memcpy(&w, &v, 4); } else memcpy(&w, &fv, 4);
            uint32_t old = 0; ok = !(a & 3) && scan_read(a, &old, 4) &&
                mach_vm_write(mach_task_self(), a, (vm_offset_t)(uintptr_t)&w, 4) == KERN_SUCCESS;
            char o[32], n[32]; scan_print_word(o, sizeof o, old); scan_print_word(n, sizeof n, w);
            scan_out(ok ? "poke %#llx: %s -> %s" : "poke %#llx failed (%s -> %s)", a, o, n);
        } else scan_out("error: mscan poke <hex address> <value>");
    } else if (!strcmp(cmd, "base")) scan_base();
    else scan_out("error: unknown mscan command '%s' (new, changed, unchanged, inc, dec, show, watch, code, poke, base)", cmd);
    pthread_mutex_unlock(&scan_lock);
    return 1;
}
#endif
