// Import resolution (implemented ones go to the host, the rest go to named stubs) and host gate dispatch.
#include "hle.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <dlfcn.h>
#include <time.h>

// ---------- Unimplemented imports: named stubs (the statically assembled stub table in entry.S, the k-th = movz x9,#k; b unimpl_entry) ----------
#define MAX_STUB 4096                 // Matches the .rept count in entry.S
static unsigned g_nstub;
static const char *g_stub_name[MAX_STUB];
static uint8_t g_stub_seen[MAX_STUB];
extern uint64_t unimpl_stubs[MAX_STUB];   // 8 bytes per stub

uint64_t unimpl_c(unsigned idx, uint64_t *regs) {
    if (!g_stub_seen[idx]) {   // once per stub (a counter in the byte wrapped every 256 calls: Ace Attorney Investigations Collection calls nnProfilerRecordHeartbeat every frame)
        g_stub_seen[idx] = 1;
        LOG("UNIMPL %s (x0=%#llx x1=%#llx x2=%#llx)", g_stub_name[idx], regs[0], regs[1], regs[2]);
    }
    return 0;
}

// Name hash over the swhle section, built at the first lookup. Startup resolution is single-threaded;
// runtime NRO relocation holds the loader's module write lock, so later lookups and stub insertions are serialized too.
// resolve_import used to strcmp its way through the whole section for every import no module defines, and
// hle_implements for every weak one: 270 entries (the HLE hooks and the libc / Vulkan / GL passthrough tables of
// hle_table.c / hle_vk.m / hle_gl.m) per lookup. These were the last by-name linear scans of the loading phase
// whose module symbol tables module_sym's hash already fixed (loader.c build_symhash, whose comment measured
// that phase at 77-86 ms of the ① load before it)
static uint32_t name_h(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static unsigned *g_hle_idx, g_hle_mask; static const hle_ent *g_hle_sec;
static const hle_ent *hle_find(const char *name) {
    if (!g_hle_mask) {
        unsigned long secsz;
        g_hle_sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &secsz);
        unsigned n = g_hle_sec ? (unsigned)(secsz / sizeof *g_hle_sec) : 0;
        unsigned cap = 16; while (cap < n * 2) cap <<= 1;
        g_hle_idx = calloc(cap, sizeof *g_hle_idx); g_hle_mask = cap - 1;
        for (unsigned i = 0; i < n; i++)   // the first entry of a duplicated name wins, as the linear scan did
            for (unsigned j = name_h(g_hle_sec[i].name) & g_hle_mask;; j = (j + 1) & g_hle_mask) {
                if (!g_hle_idx[j]) { g_hle_idx[j] = i + 1; break; }
                if (!strcmp(g_hle_sec[g_hle_idx[j] - 1].name, g_hle_sec[i].name)) break;
            }
    }
    if (!g_hle_sec) return NULL;
    for (unsigned j = name_h(name) & g_hle_mask; g_hle_idx[j]; j = (j + 1) & g_hle_mask)
        if (!strcmp(g_hle_sec[g_hle_idx[j] - 1].name, name)) return &g_hle_sec[g_hle_idx[j] - 1];
    return NULL;
}

uint64_t resolve_import(module_t *m, const char *name) {
    const hle_ent *e = hle_find(name);
    if (e) {
        if (g_flag.imports) { Dl_info di; dladdr(e->fn, &di); LOG("import %s -> %s", name, di.dli_sname); }
        void *hlecount_wrap(const char *, const char *, void *); return (uint64_t)hlecount_wrap("HLE", name, e->fn);   // With DM_HLECOUNT it is a counting stub (including the libc passthrough table)
    }
    // Reuse the stub if one was already allocated for the same name (its own hash: the reuse scan over every stub was quadratic in the stub count)
    void *hlecount_wrap(const char *, const char *, void *);
    static unsigned *stub_idx, stub_mask;
    if (stub_idx)
        for (unsigned j = name_h(name) & stub_mask; stub_idx[j]; j = (j + 1) & stub_mask)
            if (!strcmp(g_stub_name[stub_idx[j] - 1], name)) return (uint64_t)hlecount_wrap("STUB", name, (void *)&unimpl_stubs[stub_idx[j] - 1]);
    if (g_nstub >= MAX_STUB) die("stub overflow");
    unsigned k = g_nstub++;
    g_stub_name[k] = name;
    if (!stub_idx || (unsigned)g_nstub * 2 > stub_mask + 1) {   // the first stub, or past half full: (re)build at twice the size
        unsigned cap = stub_idx ? (stub_mask + 1) * 2 : 512;
        free(stub_idx); stub_idx = calloc(cap, sizeof *stub_idx); stub_mask = cap - 1;
        for (unsigned i = 0; i < (unsigned)g_nstub; i++)
            for (unsigned j = name_h(g_stub_name[i]) & stub_mask;; j = (j + 1) & stub_mask) {
                if (!stub_idx[j]) { stub_idx[j] = i + 1; break; }
                if (!strcmp(g_stub_name[stub_idx[j] - 1], g_stub_name[i])) break;   // a duplicated name: keep the lower index
            }
    } else
        for (unsigned j = name_h(name) & stub_mask;; j = (j + 1) & stub_mask)
            if (!stub_idx[j]) { stub_idx[j] = k + 1; break; }
    (void)m;
    return (uint64_t)hlecount_wrap("STUB", name, (void *)&unimpl_stubs[k]);
}

// The host implements this import (the swhle table), without allocating a stub for it
int hle_implements(const char *name) { return hle_find(name) != NULL; }

void stubs_report(void) { LOG("stubs=%u", g_nstub); }

// ---------- Host gate dispatch ----------
// The rewriter replaces every svc #n / mrs cntpct with a call to dm_gate; here we remember, by gate block return point (absolute address), what it originally was.
// Lookup: open-addressing hash, one hit (it used to do module_of + a linear compare over 110 entries on every gate call, keyed by in-module offset, which would collide across modules).
enum { G_SVC, G_CNTPCT, G_CNTVCT, G_OBS, G_SPIN };
// Observation gates (inserted offline from the rewriter's OBS table): number → read-only callback; with no callback attached the gate returns after one pass in and out
static void (*g_obs_fn[OBS_MAX])(uint64_t *regs, int param); static int g_obs_have[OBS_MAX];
void gate_obs_set(int id, void (*fn)(uint64_t *regs, int param)) { if (id <= 0 || id >= OBS_MAX) die("observation gate number out of range %d", id); g_obs_fn[id] = fn; }
int gate_obs_present(int id) { return id > 0 && id < OBS_MAX && g_obs_have[id]; }
typedef struct { uint64_t ret; int kind, arg; uint64_t orig; } gate_t;
#define GATE_BITS 15                     // 32768 slots, gate count ≤ 8192, load factor ≤ 1/4 (spin wait gates: Tears of the Kingdom's main has 843)
static gate_t g_gate[1u << GATE_BITS];
static int g_ngate;
static inline unsigned gate_h(uint64_t a) { return (unsigned)(((a >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - GATE_BITS)); }

void gate_register(uint64_t ret_abs, const char *kind, int arg, uint64_t orig) {
    int k = !strcmp(kind, "svc") ? G_SVC : !strcmp(kind, "cntpct") ? G_CNTPCT : !strcmp(kind, "cntvct") ? G_CNTVCT : !strcmp(kind, "obs") ? G_OBS : !strcmp(kind, "spin") ? G_SPIN : -1;
    if (k < 0) die("unknown gate kind %s", kind);
    if (k == G_OBS) { if ((arg & 0xff) <= 0 || (arg & 0xff) >= OBS_MAX) die("observation gate number out of range %d", arg & 0xff); g_obs_have[arg & 0xff] = 1; }
    if (++g_ngate > (1 << GATE_BITS) / 4) die("gate overflow");
    unsigned i = gate_h(ret_abs);
    while (g_gate[i].ret) { if (g_gate[i].ret == ret_abs) die("gate registered twice %#llx", ret_abs); i = (i + 1) & ((1u << GATE_BITS) - 1); }
    g_gate[i] = (gate_t){ret_abs, k, arg, orig};
}

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr);   // hle_svc.c

uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs) {
    for (unsigned i = gate_h(ret_addr);; i = (i + 1) & ((1u << GATE_BITS) - 1)) {
        const gate_t *g = &g_gate[i];
        if (g->ret == ret_addr) switch (g->kind) {
            case G_CNTPCT: case G_CNTVCT: {
                guest_clock_read_wait();
                uint64_t t = guest_ns() * 12 / 625;    // 19.2 MHz system counter; original instruction mrs xN, cntpct_el0, arg = N
                regs[g->arg] = t;
                return t;
            }
            case G_OBS: if (g_obs_fn[g->arg & 0xff]) g_obs_fn[g->arg & 0xff](regs, g->arg >> 8); return 0;   // dm_gate writes x0–x29 back: the original values, unless the callback changed one (OBS_STDALLOC_INIT)
            case G_SPIN: {   // a pass of a spin wait loop (patch_nso.py "Spin waits"), where the thread may sleep (hle_thread.c dm_spin_wait): inside an SVC frame, so that a thread paused there has its context
                void thr_svc_enter(uint64_t *, uint64_t), thr_svc_leave(void), dm_spin_wait(uint64_t, uint64_t);
                thr_svc_enter(regs, g->orig); dm_spin_wait(ret_addr, g->orig); thr_svc_leave(); return 0; }
            default: {   // register the gate frame on SVC entry / exit: it is the context of a paused thread (hle_thread.c)
                void thr_svc_enter(uint64_t *, uint64_t), thr_svc_leave(void);
                thr_svc_enter(regs, g->orig); uint64_t r = svc_dispatch(g->arg, regs, g->orig); thr_svc_leave(); return r; }
        }
        if (!g->ret) die("unknown gate at %#llx", ret_addr);
    }
}
