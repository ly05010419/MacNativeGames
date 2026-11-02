/* nn::ro for Super Smash Bros. Ultimate 13.0.2: the game's fighter and item code lives in NRO modules inside data.arc (98,
   listed by .nrr/cross2_130001.nrr). The game imports six nn::ro functions; the host answers them in-process instead of the
   ldr:ro service. An NRO cannot run as the game hands it over, so every NRO is found and rewritten ahead of time like main
   (scripts/extract_nros.py, scripts/rewrite_nros.py), by the SHA-256 the NRR lists. LoadModule maps the rewritten copy of the
   image the game passes, relocates it (imports from main, the SDK and NROs loaded before; a JUMP_SLOT whose symbol no module
   defines yet is bound at its first call, loader.c), seals it like the other modules, fills the SDK's RoModule with the
   module's real tables and runs its constructors. Measured 23.10.: of the 98 NROs, 88 need no rewrite at all and 10 one x18
   site; none carries an svc or thread-register access, so an NRO with host gates is refused by name. */
#include "runtime.h"
#include <CommonCrypto/CommonDigest.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

extern const char *runtime_rewritten_dir;                     /* loader.c: the rewritten-module folder (argv[1]) */
void *runtime_lazy_defer(uint64_t slot, const char *name);    /* loader.c */
void runtime_lazy_track(uint64_t slot, const char *name);
void runtime_lazy_forget(uint64_t lo, uint64_t hi);
unsigned runtime_lazy_unbind(uint64_t lo, uint64_t hi);

typedef struct { uint32_t unused, mod0; uint64_t pad; char magic[4]; uint32_t version, size, flags;
                 uint32_t text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size, reserved; uint8_t build_id[32]; } NroHeader;
typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Sym;
typedef struct { uint64_t offset, info; int64_t addend; } Rela;

enum { MAX_MODS = 128 };
typedef struct {
    _Atomic uintptr_t base;            /* 0 = free slot; with text_end read by the preemption signal handler */
    _Atomic uint64_t text_end;
    size_t map_size; uint64_t file_size, data_off, key_off, patch_off;
    char digest[65];
    const Sym *sym; const char *str; uint32_t nsym;
    uint32_t *hash; uint32_t hash_mask;
    uint64_t fini;
    void *guest;                       /* the game's nn::ro::Module */
    _Alignas(16) uint8_t ro_object[0x100];   /* the SDK's RoModule, filled with this module's tables */
} RoMod;
static RoMod g_mods[MAX_MODS];
static pthread_rwlock_t g_lock = PTHREAD_RWLOCK_INITIALIZER;
static size_t page_up(size_t x) { return (x + 0x3fff) & ~(size_t)0x3fff; }
static unsigned hash_name(const char *s) { unsigned h = 2166136261u; for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u; return h; }

static int nro_header(const void *image, NroHeader *h) {
    if (!image) return 0;
    memcpy(h, image, sizeof *h);
    return !memcmp(h->magic, "NRO0", 4) && h->size >= sizeof *h && h->size < (512u << 20);
}
static void nro_digest(const void *image, uint32_t size, char hex[65]) {
    unsigned char d[32]; CC_SHA256(image, size, d);
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
}
static uint64_t layout_value(const char *text, const char *key) {
    char want[64]; snprintf(want, sizeof want, "# %s\t", key);
    const char *p = strstr(text, want); if (!p) { fprintf(stderr, "[ssbu] NRO layout: %s missing\n", key); runtime_fail("rewritten NRO without its layout table"); }
    return strtoull(p + strlen(want), NULL, 0);
}
static char *read_text(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)size + 1); if (!b || fread(b, 1, (size_t)size, f) != (size_t)size) runtime_fail("read rewritten NRO table");
    fclose(f); b[size] = 0; if (n) *n = (size_t)size; return b;
}

/* The module's exports, for runtime_symbol (loader.c) and the lazy binder. */
static void make_hash(RoMod *m) {
    unsigned cap = 16; while (cap < m->nsym * 2) cap <<= 1;
    m->hash = calloc(cap, sizeof *m->hash); if (!m->hash) runtime_fail("NRO symbol hash"); m->hash_mask = cap - 1;
    for (uint32_t i = 1; i < m->nsym; i++) {
        const Sym *s = &m->sym[i]; unsigned bind = s->info >> 4;
        if (!s->shndx || (bind != 1 && bind != 2) || !m->str[s->name]) continue;
        unsigned p = hash_name(m->str + s->name) & m->hash_mask;
        while (m->hash[p] && strcmp(m->str + s->name, m->str + m->sym[m->hash[p]].name)) p = (p + 1) & m->hash_mask;
        if (!m->hash[p]) m->hash[p] = i;
    }
}
static void *mod_symbol(const RoMod *m, const char *name) {
    unsigned p = hash_name(name) & m->hash_mask;
    while (m->hash[p]) {
        const Sym *s = &m->sym[m->hash[p]];
        if (!strcmp(name, m->str + s->name)) return s->shndx == 0xfff1 ? (void *)(uintptr_t)s->value : (void *)(atomic_load(&m->base) + s->value);
        p = (p + 1) & m->hash_mask;
    }
    return NULL;
}
void *runtime_ro_symbol(const char *name) {
    void *p = NULL;
    pthread_rwlock_rdlock(&g_lock);
    for (int i = 0; i < MAX_MODS && !p; i++) if (atomic_load(&g_mods[i].base) && g_mods[i].hash) p = mod_symbol(&g_mods[i], name);
    pthread_rwlock_unlock(&g_lock);
    return p;
}
/* threads.c (asynchronous preemption, from a signal handler): is pc in an NRO's original code? Reads two atomics per slot. */
int runtime_ro_code_pc(uint64_t pc) {
    for (int i = 0; i < MAX_MODS; i++) { uintptr_t b = atomic_load_explicit(&g_mods[i].base, memory_order_relaxed);
        if (b && pc >= b && pc - b < atomic_load_explicit(&g_mods[i].text_end, memory_order_relaxed)) return 1; }
    return 0;
}
/* QueryMemory for the NROs (services.c): what Horizon reports for a module ro loaded, ModuleCode (8) for text r-x and
   rodata r--, ModuleCodeData (9) for data and bss rw-, then the key page and the patch area. The SDK's
   nn::ro::detail::GetExceptionInfo finds the module of a throwing pc this way (ModuleCode back to its start, then
   ModuleCodeData right after it, then MOD0 for .eh_frame_hdr); an unknown address used to read as one page of code and
   it asserted (Super Smash Bros. Ultimate, 23.10., a C++ exception while a match loaded). */
int runtime_ro_query(uint64_t a, uint64_t *base, uint64_t *size, unsigned *state, unsigned *perm) {
    int found = 0;
    pthread_rwlock_rdlock(&g_lock);
    for (int i = 0; i < MAX_MODS && !found; i++) {
        RoMod *m = &g_mods[i]; uintptr_t b = atomic_load(&m->base);
        if (!b || a < b || a - b >= m->map_size) continue;
        uint64_t off = a - b, rx = page_up(atomic_load(&m->text_end)), ro_end = m->data_off & ~(uint64_t)0x3fff;
        const struct { uint64_t lo, hi; unsigned state, perm; } r[] = {{0, rx, 8, 5}, {rx, ro_end, 8, 1}, {ro_end, m->key_off, 9, 3},
                                                                         {m->key_off, m->key_off + 0x4000, 9, 1}, {m->key_off + 0x4000, m->map_size, 9, 5}};
        for (unsigned k = 0; k < sizeof r / sizeof *r; k++) if (off >= r[k].lo && off < r[k].hi) {
            *base = b + r[k].lo; *size = r[k].hi - r[k].lo; *state = r[k].state; *perm = r[k].perm; found = 1; break;
        }
    }
    pthread_rwlock_unlock(&g_lock);
    return found;
}
int runtime_ro_module_offset(uint64_t pc, const char **name, uint64_t *off) {
    for (int i = 0; i < MAX_MODS; i++) { uintptr_t b = atomic_load(&g_mods[i].base);
        if (b && pc >= b && pc - b < g_mods[i].map_size) { *name = g_mods[i].digest; *off = pc - b; return 1; } }
    return 0;
}

static void relocate(RoMod *m, uint8_t *base, const uint64_t *tags) {
    void **resolved = calloc(m->nsym, sizeof *resolved); if (!resolved) runtime_fail("NRO relocation table");
    uint8_t *done = calloc(m->nsym, 1); if (!done) runtime_fail("NRO relocation table");
    unsigned relative = 0, symbolic = 0, lazy = 0, weak = 0, tracked = 0;
    const struct { uint64_t table, size; } lists[2] = {{tags[7], tags[8]}, {tags[23], tags[2]}};
    for (int l = 0; l < 2; l++) for (uint64_t at = lists[l].table; lists[l].table && at < lists[l].table + lists[l].size; at += sizeof(Rela)) {
        const Rela *r = (const Rela *)(base + at); unsigned kind = (unsigned)r->info; uint32_t si = (uint32_t)(r->info >> 32);
        if (r->offset < m->data_off || r->offset + 8 > m->key_off || si >= m->nsym) runtime_fail("NRO relocation outside its data");
        uint64_t *where = (uint64_t *)(base + r->offset);
        if (kind == 1027) { if (r->addend < 0 || (uint64_t)r->addend >= m->key_off) runtime_fail("NRO relative addend"); *where = (uintptr_t)base + (uint64_t)r->addend; relative++; continue; }
        if (kind != 257 && kind != 1025 && kind != 1026) { fprintf(stderr, "[ssbu] NRO %s relocation kind %u\n", m->digest, kind); runtime_fail("NRO relocation kind"); }
        const Sym *s = &m->sym[si]; const char *name = m->str + s->name;
        if (!done[si]) {
            done[si] = 1; void *p = NULL;
            if (s->shndx) p = s->shndx == 0xfff1 ? (void *)(uintptr_t)s->value : base + s->value;
            else { p = runtime_override(name); if (p) p = runtime_hle_wrap_named(p, name); else p = runtime_symbol(name); }
            resolved[si] = p;
        }
        void *p = resolved[si];
        if (!p && (s->info >> 4) == 2) { weak++; *where = 0; continue; }
        if (!p && kind == 1026) { *where = (uintptr_t)runtime_lazy_defer((uintptr_t)where, name); lazy++; continue; }
        if (!p) { fprintf(stderr, "[ssbu] NRO %s: %s is not defined by any loaded module\n", m->digest, name); runtime_fail("NRO import unresolved"); }
        *where = (uintptr_t)p + (kind == 1026 ? 0 : (uint64_t)r->addend); symbolic++;
        const char *owner; uint64_t off;   // bound to another NRO: reset when that one is unloaded (runtime_lazy_unbind)
        if (kind == 1026 && !s->shndx && runtime_ro_module_offset((uintptr_t)p, &owner, &off)) { runtime_lazy_track((uintptr_t)where, name); tracked++; }
    }
    /* DT_RELR (36/35): runs of relative relocations as address + bitmap entries. */
    for (uint64_t at = tags[36], next = 0; tags[36] && at < tags[36] + tags[35]; at += 8) {
        uint64_t e = *(const uint64_t *)(base + at);
        if (!(e & 1)) { if (e < m->data_off || e + 8 > m->key_off) runtime_fail("NRO RELR outside its data"); *(uint64_t *)(base + e) += (uintptr_t)base; next = e + 8; relative++; continue; }
        for (int bit = 1; bit < 64; bit++) if (e >> bit & 1) { uint64_t w = next + (uint64_t)(bit - 1) * 8; if (w < m->data_off || w + 8 > m->key_off) runtime_fail("NRO RELR outside its data"); *(uint64_t *)(base + w) += (uintptr_t)base; relative++; }
        next += 63 * 8;
    }
    free(resolved); free(done);
    fprintf(stderr, "[ssbu] NRO %.16s relocated: %u relative, %u symbolic (%u of them calls into another NRO), %u bound at first call, %u weak unresolved\n",
            m->digest, relative, symbolic, tracked, lazy, weak);
}

static uint32_t ro_initialize(void) { fprintf(stderr, "[ssbu] nn::ro::Initialize\n"); return 0; }
static uint32_t ro_register(void *info, const void *nrr) {
    uint32_t count = 0; uint64_t program = 0;
    if (nrr && !memcmp(nrr, "NRR0", 4)) { memcpy(&program, (const uint8_t *)nrr + 0x330, 8); memcpy(&count, (const uint8_t *)nrr + 0x344, 4); }
    fprintf(stderr, "[ssbu] nn::ro::RegisterModuleInfo(%p): NRR of %016llx with %u module digests\n", info, (unsigned long long)program, count);
    return 0;
}
static uint32_t ro_unregister(void *info) { fprintf(stderr, "[ssbu] nn::ro::UnregisterModuleInfo(%p)\n", info); return 0; }
static uint32_t ro_buffer_size(uint64_t *out, const void *image) {
    NroHeader h;
    if (!nro_header(image, &h)) runtime_fail("nn::ro::GetBufferSize: not an NRO image");
    *out = ((uint64_t)h.bss_size + 0xfff) & ~0xfffULL;
    return 0;
}
static uint32_t ro_load(void *module, const void *image, void *buffer, uint64_t buffer_size, int flag) {
    NroHeader h;
    if (!module || !nro_header(image, &h)) runtime_fail("nn::ro::LoadModule: not an NRO image");
    char hex[65]; nro_digest(image, h.size, hex);
    char path[4096], *layout; size_t n;
    snprintf(path, sizeof path, "%s/nro/%s.fixups.tsv", runtime_rewritten_dir, hex);
    if (!(layout = read_text(path, &n))) {
        fprintf(stderr, "[ssbu] nn::ro::LoadModule: NRO %s (%u bytes) has no rewritten copy in %s/nro\n", hex, h.size, runtime_rewritten_dir);
        runtime_fail("NRO not rewritten: run scripts/extract_nros.py and scripts/rewrite_nros.py");
    }
    snprintf(path, sizeof path, "%s/nro/%s.gates.tsv", runtime_rewritten_dir, hex);
    size_t ngate_bytes = 0; char *gates = read_text(path, &ngate_bytes);
    if (!gates || ngate_bytes) { fprintf(stderr, "[ssbu] NRO %s carries host gates\n", hex); runtime_fail("an NRO with svc / timer gates is not wired yet"); }
    free(gates);
    pthread_rwlock_wrlock(&g_lock);
    RoMod *m = NULL; for (int i = 0; i < MAX_MODS && !m; i++) if (!atomic_load(&g_mods[i].base)) m = &g_mods[i];
    pthread_rwlock_unlock(&g_lock);
    if (!m) runtime_fail("more than 128 NRO modules loaded");
    memset(m->ro_object, 0, sizeof m->ro_object); m->guest = module; snprintf(m->digest, sizeof m->digest, "%s", hex);
    uint64_t patch_off = layout_value(layout, "PATCH_OFF"), key_off = layout_value(layout, "KEY_PAGE_OFF");
    uint64_t text_end = layout_value(layout, "TEXT_END"), data_off = layout_value(layout, "DATA_OFF");
    free(layout);
    /* The rewritten image: the NRO's memory image with its bss, the key page and the patch area (patch_nso.py). */
    snprintf(path, sizeof path, "%s/nro/%s.patched.bin", runtime_rewritten_dir, hex);
    FILE *f = fopen(path, "rb"); struct stat st;
    if (!f || fstat(fileno(f), &st)) runtime_fail("open rewritten NRO image");
    m->file_size = (uint64_t)st.st_size; m->map_size = page_up(m->file_size); m->data_off = data_off; m->key_off = key_off; m->patch_off = patch_off;
    if (key_off % 0x4000 || key_off + 0x4000 > patch_off || patch_off > m->file_size || text_end > data_off || h.size > key_off) runtime_fail("rewritten NRO layout");
    uint8_t *base = mmap(NULL, m->map_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED || fread(base, 1, m->file_size, f) != m->file_size) runtime_fail("map rewritten NRO");
    fclose(f);
    /* The rewrite changes only text words and appends after the image: everything else equals what the game read. */
    if (memcmp(base + text_end, (const uint8_t *)image + text_end, h.size - text_end)) runtime_fail("rewritten NRO data differs from the game's NRO");
    uint64_t *key = (uint64_t *)(base + key_off); for (int i = 0; i < 8; i++) key[i] = runtime_keys[i] * 8;
    /* Dynamic section through MOD0 (its offset is the image's second word). */
    uint32_t mod0; memcpy(&mod0, base + 4, 4);
    if (memcmp(base + mod0, "MOD0", 4)) runtime_fail("NRO without MOD0");
    int32_t dyn_rel; memcpy(&dyn_rel, base + mod0 + 4, 4);
    uint64_t tags[40] = {0}, dyn = mod0 + (int64_t)dyn_rel;
    for (int i = 0; i < 512; i++) { int64_t tag; uint64_t v; memcpy(&tag, base + dyn + 16 * i, 8); memcpy(&v, base + dyn + 16 * i + 8, 8); if (!tag) break; if (tag >= 0 && tag < 40) tags[tag] = v; }
    if (!tags[6] || !tags[5] || (tags[7] && tags[9] != 24)) runtime_fail("NRO dynamic section");
    m->sym = (const Sym *)(base + tags[6]); m->str = (const char *)(base + tags[5]);
    if (tags[4]) memcpy(&m->nsym, base + tags[4] + 4, 4);   /* DT_HASH: nchain = symbol count */
    else { uint32_t dynsym_size; memcpy(&dynsym_size, base + 0x7c, 4); m->nsym = dynsym_size / sizeof(Sym); }   /* the NRO header's dynsym size */
    if (!m->nsym || (uint64_t)m->nsym * sizeof(Sym) > key_off) runtime_fail("NRO symbol count");
    relocate(m, base, tags);
    /* Seal as loader.c seals main: text r-x, rodata r--, data and bss rw-, key page r--, patch area r-x. */
    size_t rx = page_up(text_end), ro_end = data_off & ~(size_t)0x3fff;
    if (mprotect(base, rx, PROT_READ | PROT_EXEC) || (ro_end > rx && mprotect(base + rx, ro_end - rx, PROT_READ)) ||
        mprotect(base + key_off, 0x4000, PROT_READ) ||
        (m->map_size > patch_off && mprotect(base + patch_off, m->map_size - patch_off, PROT_READ | PROT_EXEC))) runtime_fail("seal NRO");
    sys_icache_invalidate(base, rx); if (m->map_size > patch_off) sys_icache_invalidate(base + patch_off, m->map_size - patch_off);
    /* The SDK's RoModule (layout as Smash's own modders read it): list links, relocation tables, base, dynamic, init/fini,
       hash, string and symbol tables, GOT. */
    uint64_t *o = (uint64_t *)m->ro_object;
    o[0] = o[1] = (uintptr_t)m->ro_object; o[2] = tags[7] ? (uintptr_t)base + tags[7] : 0; o[3] = tags[23] ? (uintptr_t)base + tags[23] : 0;
    o[4] = (uintptr_t)base; o[5] = (uintptr_t)base + dyn; o[6] = 1; o[7] = tags[2];
    o[8] = tags[12] ? (uintptr_t)base + tags[12] : 0; o[9] = tags[13] ? (uintptr_t)base + tags[13] : 0;
    uint32_t nbucket = 0; if (tags[4]) memcpy(&nbucket, base + tags[4], 4);
    o[10] = tags[4] ? (uintptr_t)base + tags[4] + 8 : 0; o[11] = tags[4] ? (uintptr_t)base + tags[4] + 8 + 4ull * nbucket : 0;
    o[12] = (uintptr_t)base + tags[5]; o[13] = (uintptr_t)base + tags[6]; o[14] = tags[10]; o[15] = tags[3] ? (uintptr_t)base + tags[3] : 0;
    o[16] = tags[8]; o[19] = tags[8] / 24; o[20] = m->nsym; o[21] = nbucket;
    m->fini = tags[13];
    make_hash(m);
    atomic_store(&m->text_end, text_end);
    pthread_rwlock_wrlock(&g_lock); atomic_store(&m->base, (uintptr_t)base); pthread_rwlock_unlock(&g_lock);
    uint64_t *g = module; g[0] = (uintptr_t)m->ro_object; ((uint32_t *)module)[2] = 1; g[2] = (uintptr_t)base; g[3] = (uintptr_t)buffer;
    fprintf(stderr, "[ssbu] nn::ro::LoadModule: NRO %.16s (%u bytes, bss %u, buffer %llu, flag %d) at %p, %u symbols\n",
            hex, h.size, h.bss_size, (unsigned long long)buffer_size, flag, (void *)base, m->nsym);
    /* Constructors: DT_INIT runs the module's init array (the SDK toolchain's startup), as for main. */
    if (tags[12]) ((void (*)(void))(base + tags[12]))();
    else for (uint64_t i = 0; tags[25] && i < tags[27] / 8; i++) { void (*fn)(void); memcpy(&fn, base + tags[25] + 8 * i, 8); if (fn) fn(); }
    return 0;
}
static uint32_t ro_unload(void *module) {
    RoMod *m = NULL;
    pthread_rwlock_rdlock(&g_lock); for (int i = 0; i < MAX_MODS && !m; i++) if (atomic_load(&g_mods[i].base) && g_mods[i].guest == module) m = &g_mods[i]; pthread_rwlock_unlock(&g_lock);
    if (!m) runtime_fail("nn::ro::UnloadModule of a module that is not loaded");
    uint8_t *base = (uint8_t *)atomic_load(&m->base);
    if (m->fini) ((void (*)(void))(base + m->fini))();
    pthread_rwlock_wrlock(&g_lock); atomic_store(&m->base, 0); atomic_store(&m->text_end, 0); free(m->hash); m->hash = NULL; pthread_rwlock_unlock(&g_lock);
    runtime_lazy_forget((uintptr_t)base, (uintptr_t)base + m->map_size);
    unsigned unbound = runtime_lazy_unbind((uintptr_t)base, (uintptr_t)base + m->map_size);   // calls into it bind again
    munmap(base, m->map_size);
    fprintf(stderr, "[ssbu] nn::ro::UnloadModule: NRO %.16s, %u import slots of other modules bind again at their next call\n", m->digest, unbound);
    memset(module, 0, 0x20);
    return 0;
}

void *runtime_ro_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2ro10InitializeEv", ro_initialize},
        {"_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", ro_register},
        {"_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", ro_unregister},
        {"_ZN2nn2ro13GetBufferSizeEPmPKv", ro_buffer_size},
        {"_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", ro_load},
        {"_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", ro_unload},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
