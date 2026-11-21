// nn::ro (loading NRO modules at run time): on the real device it connects to the ldr:ro service. The host has no such service; running the SDK's Initialize as is reads a null session in InitializeWithPortName and crashes (Engage).
// Engage (Unity) imports 8 ro interfaces but its RomFS has no .nro / .nrr: at startup it only calls Initialize, and symbol lookups go through LookupSymbol.
// Super Smash Bros. Ultimate loads its fighters' code as NROs (98 in data.arc, registered by .nrr/cross2_*.nrr). The service's only part in the SDK's LoadModule is mapping the image
// (SDK 8.2.1 LoadModuleInternal, sdk+0x347460: ldr:ro LoadNro; then the SDK zeroes bss, relocates through rtld's module lists, calls the module's init and binds the earlier modules'
// variables). The host replaces rtld, so it takes over the whole call: the NROs were rewritten offline and mapped at start (loader.c "NRO modules"); LoadModule finds the one whose
// SHA-256 is the image's (what the NRR registers), relocates it against the modules loaded now and calls its DT_INIT, as the SDK does. The image and buffer the game passes stay unused
// (on Horizon they become the module's memory; the game keeps them until UnloadModule).
// Only success matters for the failures: the game checks IsSuccess only, so a failure code is a non-zero value under the ro module number 22.
#include "hle.h"
#include <CommonCrypto/CommonDigest.h>

#define RO_FAIL ((Result)(22 | (1 << 9)))   // a failure under ro module 22
#define RO_NOT_NRO ((Result)0x816)           // the SDK's result for an image without NRO0 (sdk+0x3471e8)
static pthread_mutex_t g_ro_mu = PTHREAD_MUTEX_INITIALIZER;   // the SDK holds g_RoLock over a load as well

HLE_FORCE("_ZN2nn2ro10InitializeEv", "connect to the ldr:ro service; the host has none, symbol lookups search the loaded modules directly", Result, h_ro_Initialize, void) { LOG("init takeover: ro::Initialize (no-op)"); return 0; }

HLE_FORCE("_ZN2nn2ro12LookupSymbolEPmPKc", "look up a definition by name in the loaded modules", Result, h_ro_LookupSymbol, uintptr_t *out, const char *name) {
    void *p = name ? guest_sym(name) : NULL;
    static int n; if (n++ < 40) LOG("ro::LookupSymbol %s → %p", name ? name : "(null)", p);
    if (!p) { static int red; if (!red++) LOG("[RED] ro::LookupSymbol cannot find %s (first case)", name ? name : "(null)"); return RO_FAIL; }
    if (out) *out = (uintptr_t)p;
    return 0;
}

// nn::ro::Module as the SDK fills it (LoadModuleInternal): +0 the module object (RoModule, inside the NRO on Horizon; the host's module_t here), +8 state (1 = loaded), +0x10 image, +0x18 buffer
typedef struct { void *module; uint32_t state, pad; const void *image; void *buffer; } ro_module;
static module_t *module_from(const ro_module *rm) {
    if (!rm || rm->state != 1) return NULL;
    for (int i = 0; i < g_nmod; i++) if (rm->module == (void *)&g_mod[i] && g_mod[i].nro && g_mod[i].loaded) return &g_mod[i];
    return NULL;
}

HLE_FORCE("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", "look up a definition by name in one loaded NRO", Result, h_ro_LookupModuleSymbol, uintptr_t *out, const void *mod, const char *name) {
    module_t *m = module_from(mod); void *p = m && name ? module_sym(m, name) : NULL;
    // A name the loaded module does not define fails on Horizon too (ldr:ro's not-found result), and games probe for one: Portal's Source engine asks each module for BuiltDebug, which
    // only its debug builds define. Only a module the host does not know is wrong
    if (!p && m) { static int n; if (n++ < 8) LOG("ro::LookupModuleSymbol %s: not defined in that module (failure, as on Horizon)", name ? name : "(null)"); return RO_FAIL; }
    if (!p) { static int red; if (!red++) LOG("[RED] ro::LookupModuleSymbol %s: not a loaded module", name ? name : "(null)"); return RO_FAIL; }
    if (out) *out = (uintptr_t)p;
    return 0;
}

// As the SDK computes it (sdk+0x3471c0, no service): the bss size from the NRO header
HLE_FORCE("_ZN2nn2ro13GetBufferSizeEPmPKv", "the bss size from the NRO header, as the SDK computes it", Result, h_ro_GetBufferSize, size_t *out, const void *image) {
    const uint8_t *h = image;
    if (!h || memcmp(h + 0x10, "NRO0", 4)) return RO_NOT_NRO;
    if (out) *out = *(const uint32_t *)(h + 0x38);
    return 0;
}

HLE_FORCE("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", "load an NRO rewritten offline (loader.c): found by the image's SHA-256, relocated against the loaded modules, its init called",
          Result, h_ro_LoadModule, void *mod, const void *image, void *buf, size_t size, int flag) {
    const uint8_t *h = image;
    if (!h || memcmp(h + 0x10, "NRO0", 4)) return RO_NOT_NRO;
    uint32_t n = *(const uint32_t *)(h + 0x18);
    uint8_t sha[32]; CC_SHA256(h, n, sha);
    char hx[17]; for (int k = 0; k < 8; k++) snprintf(hx + 2 * k, 3, "%02x", sha[k]);
    // Online SDKs are never loaded, whatever the game (dm/tools/nro_collect.py ONLINE_SDKS: Epic Online Services, known by its export EOS_Platform_Create). Hades and Hades II load
    // rom:/nro/EOSSDK-Switch-Shipping.nro for Epic-account cross-save (Auth, Connect, PlayerDataStorage), which the host cannot serve; the real SDK would at best fail its login and at
    // worst wait on time-outs or contact Epic's servers. Refused, the game's EOS_Platform_Create stays a stub, it logs "[EOS] Failed to create platform" and plays offline with local
    // saves, as before the host loaded NROs. The pack leaves such an NRO out (a "refused" row in nro.tsv); an older nro.tsv that has it is caught by the export below
    if (nro_refused(sha)) { LOG("ro::LoadModule: refused %s… (%u bytes): an online SDK (Epic Online Services), not loaded; the game plays offline", hx, n); return RO_FAIL; }
    pthread_mutex_lock(&g_ro_mu);
    module_t *m = nro_find(sha);
    if (!m) { pthread_mutex_unlock(&g_ro_mu); LOG("[RED] ro::LoadModule: no rewritten NRO with SHA-256 %s… (%u bytes): it is not in the patched modules' nro.tsv", hx, n); return RO_FAIL; }
    if (module_sym(m, "EOS_Platform_Create")) { pthread_mutex_unlock(&g_ro_mu); LOG("ro::LoadModule: refused %s (%s…): an online SDK (Epic Online Services), not loaded; the game plays offline", m->name, hx); return RO_FAIL; }
    if (m->loaded) { pthread_mutex_unlock(&g_ro_mu); LOG("[RED] ro::LoadModule: %s (%s…) is loaded already", m->name, hx); return RO_FAIL; }
    if (m->nro_size != n) LOG("[RED] ro::LoadModule: %s is %u bytes, nro.tsv says %u", m->name, n, m->nro_size);
    nro_load(m);
    ro_module *rm = mod;
    if (rm) { rm->module = m; rm->state = 1; rm->image = image; rm->buffer = buf; }
    pthread_mutex_unlock(&g_ro_mu);
    LOG("ro::LoadModule %s (SHA-256 %s…, %u bytes, buffer %#zx, bind %s): init %s", m->name, hx, n, size, flag & 1 ? "now" : "lazy", m->init ? "called" : "none");
    if (m->init) ((void (*)(void))(m->base + m->init))();   // as RoModule::CallInit: the module's _init (registers it and runs its init_array)
    return 0;
}

// SDK 19 adds a bool after the bind flag (Hades II imports only this form, for its Epic Online Services NRO)
HLE_FORCE("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmib", "load an NRO rewritten offline (as the form without the bool)", Result, h_ro_LoadModule2, void *m, const void *image, void *buf, size_t size, int flag, int b) {
    (void)b; return h_ro_LoadModule(m, image, buf, size, flag);
}

HLE_FORCE("_ZN2nn2ro8FinalizeEv", "Initialize opened no ldr:ro session", void, h_ro_Finalize, void) { }

HLE_FORCE("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", "unload an NRO: its fini, then out of the symbol lookups", void, h_ro_UnloadModule, void *mod) {
    module_t *m = module_from(mod);
    if (!m || !m->loaded) { LOG("[RED] ro::UnloadModule: not a loaded module (%p)", mod); return; }
    if (m->fini) ((void (*)(void))(m->base + m->fini))();   // as RoModule::CallFini
    pthread_mutex_lock(&g_ro_mu);
    nro_unload(m);
    ((ro_module *)mod)->state = 0; ((ro_module *)mod)->module = NULL;
    pthread_mutex_unlock(&g_ro_mu);
}

// The NRRs only register hashes with the service, which checks a module's hash against them at load. The host's modules are the ones the NSP script found by those hashes
// (dm/tools/nro_collect.py), so there is nothing to check: success
HLE_FORCE("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", "register an NRR: nothing to check, the host's NROs were collected by its hashes", Result, h_ro_RegisterModuleInfo, void *info, const void *nrr) {
    const uint8_t *r = nrr;
    if (!r || memcmp(r, "NRR0", 4)) return RO_FAIL;
    uint32_t off = *(const uint32_t *)(r + 0x340), cnt = *(const uint32_t *)(r + 0x344), refused = 0;
    for (uint32_t i = 0; i < cnt; i++) refused += nro_refused(r + off + 32 * i);
    // An NRR that registers only online SDKs the host refuses (Hades and Hades II: game.nrr with the one EOS SDK hash) fails as every registration did before the host loaded NROs,
    // so such a game takes the path it took then (it never asked for the module)
    if (cnt && refused == cnt) { LOG("ro::RegisterModuleInfo: refused an NRR that registers only online SDKs (%u hashes), as before NROs were loaded; the game plays offline", cnt); return RO_FAIL; }
    static int n; if (n++ < 20) LOG("ro::RegisterModuleInfo: NRR with %u hashes", cnt);
    (void)info;
    return 0;
}

HLE_FORCE("_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", "NRRs are not registered", void, h_ro_UnregisterModuleInfo, void *info) { (void)info; }
