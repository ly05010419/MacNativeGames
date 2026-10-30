// Import resolution (implemented ones go to the host, the rest go to named stubs) and host gate dispatch.
#include "dm.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <dlfcn.h>
#include <time.h>

typedef struct { const char *name; void *fn; } hle_ent;

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

// Per module, one cache of symbol number → resolved address
static uint64_t g_cache[MAX_MOD][8192];

uint64_t resolve_import(module_t *m, const char *name) {
    static const hle_ent *sec; static unsigned long secsz;
    if (!sec) sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &secsz);
    for (unsigned i = 0; i < secsz / sizeof(hle_ent); i++)
        if (!strcmp(sec[i].name, name)) {
            if (g_flag.imports) { Dl_info di; dladdr(sec[i].fn, &di); LOG("import %s -> %s", name, di.dli_sname); }
            void *hlecount_wrap(const char *, const char *, void *); return (uint64_t)hlecount_wrap("HLE", name, sec[i].fn);   // With DM_HLECOUNT it is a counting stub (including the libc passthrough table)
        }
    // Reuse the stub if one was already allocated for the same name
    void *hlecount_wrap(const char *, const char *, void *);
    for (unsigned k = 0; k < g_nstub; k++)
        if (!strcmp(g_stub_name[k], name)) return (uint64_t)hlecount_wrap("STUB", name, (void *)&unimpl_stubs[k]);
    if (g_nstub >= MAX_STUB) die("stub overflow");
    unsigned k = g_nstub++;
    g_stub_name[k] = name;
    (void)m;
    return (uint64_t)hlecount_wrap("STUB", name, (void *)&unimpl_stubs[k]);
}

// The host implements this import (the swhle table), without allocating a stub for it
int hle_implements(const char *name) {
    unsigned long secsz; const hle_ent *sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &secsz);
    for (unsigned i = 0; sec && i < secsz / sizeof(hle_ent); i++) if (!strcmp(sec[i].name, name)) return 1;
    return 0;
}

void stubs_report(void) { LOG("stubs=%u", g_nstub); }

// ---------- Host gate dispatch ----------
// The rewriter replaces every svc #n / mrs cntpct with a call to dm_gate; here we remember, by gate block return point (absolute address), what it originally was.
// Lookup: open-addressing hash, one hit (it used to do module_of + a linear compare over 110 entries on every gate call, keyed by in-module offset, which would collide across modules).
enum { G_SVC, G_CNTPCT, G_CNTVCT, G_OBS, G_SPIN };
// Observation gates (inserted offline from the rewriter's OBS table): number → read-only callback; with no callback attached the gate returns after one pass in and out
static void (*g_obs_fn[OBS_MAX])(const uint64_t *regs); static int g_obs_have[OBS_MAX];
void gate_obs_set(int id, void (*fn)(const uint64_t *regs)) { if (id <= 0 || id >= OBS_MAX) die("observation gate number out of range %d", id); g_obs_fn[id] = fn; }
int gate_obs_present(int id) { return id > 0 && id < OBS_MAX && g_obs_have[id]; }
typedef struct { uint64_t ret; int kind, arg; uint64_t orig; } gate_t;
#define GATE_BITS 15                     // 32768 slots, gate count ≤ 8192, load factor ≤ 1/4 (spin wait gates: Tears of the Kingdom's main has 843)
static gate_t g_gate[1u << GATE_BITS];
static int g_ngate;
static inline unsigned gate_h(uint64_t a) { return (unsigned)(((a >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - GATE_BITS)); }

void gate_register(uint64_t ret_abs, const char *kind, int arg, uint64_t orig) {
    int k = !strcmp(kind, "svc") ? G_SVC : !strcmp(kind, "cntpct") ? G_CNTPCT : !strcmp(kind, "cntvct") ? G_CNTVCT : !strcmp(kind, "obs") ? G_OBS : !strcmp(kind, "spin") ? G_SPIN : -1;
    if (k < 0) die("unknown gate kind %s", kind);
    if (k == G_OBS) { if (arg <= 0 || arg >= OBS_MAX) die("observation gate number out of range %d", arg); g_obs_have[arg] = 1; }
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
            case G_OBS: if (g_obs_fn[g->arg]) g_obs_fn[g->arg](regs); return 0;   // Registers are read-only; the x0–x8 that dm_gate writes back are the original values
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
