/* nn::ro: the SDK's run-time loader for NRO modules. On a real console it talks to the ldr:ro
   service; this host answers no such service, so the SDK's own Initialize dereferences a null
   session and dies.

   Measured on Engage without this file: signal 0xb, fault 0x0, esr 0x92000046 (data abort,
   translation fault level 2), pc = sdk+0x35b5b0. analysis/runtime-layout.json puts that offset
   inside _ZN2nn2ro22InitializeWithPortNameEPKc (starts at sdk+0x35b550), i.e. the SDK read a
   port/session field that was never filled because sm::GetService("ldr:ro") never succeeded.

   Engage is a Unity title that imports eight nn::ro interfaces but has no .nro / .nrr anywhere in
   its RomFS: at startup it only calls Initialize, and the symbol lookups it does go through
   LookupSymbol. So the behaviour to keep is small: Initialize succeeds without a service,
   LookupSymbol searches the modules the rewriter already loaded (runtime_symbol, the guest_sym
   equivalent), and everything that would need a real NRO fails honestly with a ro result code —
   nothing here pretends a module was loaded, because this port has no rewritten NRO set.

   The failure codes follow the upstream nxnative host (dist/work/eng_src/dm/host/hle_ro.c): the
   games that call these entries only test IsSuccess, so any non-zero value is a failure; 0x816 is
   the SDK's own "image without NRO0" result (sdk+0x3471e8 in SDK 8.2.1). */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

#define RO_FAIL ((uint32_t)(22 | (1 << 9)))   /* a failure under ro module 22 */
#define RO_NOT_NRO ((uint32_t)0x816)          /* the SDK's result for an image without NRO0 */

/* The NRO header: "NRO0" at +0x10, the bss size the game must supply at +0x38. Read with memcpy:
   the image is a guest pointer and its header alignment is not the host's. */
static int image_is_nro(const void *image) {
    char magic[4];
    if (!image) return 0;
    memcpy(magic, (const char *)image + 0x10, sizeof magic);
    return !memcmp(magic, "NRO0", 4);
}

static uint32_t ro_initialize(void) {
    static int said;
    if (!said++) fprintf(stderr, "[totk-ro] init takeover: nn::ro::Initialize (no ldr:ro service; symbols come from the loaded modules)\n");
    return 0;
}

/* The port-name form is what the SDK's Initialize calls internally (the frame that crashed). Any
   module that imports it gets the same answer: there is no service to connect to. */
static uint32_t ro_initialize_port(const char *port) {
    static int said;
    if (!said++) fprintf(stderr, "[totk-ro] init takeover: nn::ro::InitializeWithPortName(%s) (no ldr:ro service)\n", port ? port : "(null)");
    return 0;
}

static void ro_finalize(void) {}

/* Look up a definition by name in the loaded modules: main, subsdk0 and sdk are the guests'
   modules and the rewriter resolved their symbols, so a hit here is exactly what the game means
   by "the module that defines it". A miss is a real miss on a console too (ldr:ro answers
   not-found), and games probe for optional names, so it is a plain failure, not a stop. */
static uint32_t ro_lookup_symbol(uint64_t *out, const char *name) {
    void *p = name ? runtime_symbol(name) : NULL;
    static int n;
    if (n++ < 40) fprintf(stderr, "[totk-ro] LookupSymbol %s -> %p\n", name ? name : "(null)", p);
    if (!p) {
        static int first;
        if (!first++) fprintf(stderr, "[totk-ro] LookupSymbol cannot find %s (first miss; a miss is a failure, as on a console)\n", name ? name : "(null)");
        return RO_FAIL;
    }
    if (out) *out = (uint64_t)(uintptr_t)p;
    return 0;
}

/* nn::ro::Module is the SDK's own struct (module object, state, image, buffer) and only the SDK
   fills it, in LoadModule. This port never loads an NRO, so no Module handed in here can name a
   loaded image: fail rather than search the wrong thing. */
static uint32_t ro_lookup_module_symbol(uint64_t *out, const void *module, const char *name) {
    (void)out;
    static int first;
    if (!first++) fprintf(stderr, "[totk-ro] LookupModuleSymbol %s: no NRO module is loaded in this port (failure)\n", name ? name : "(null)");
    (void)module;
    return RO_FAIL;
}

/* Computed by the SDK itself on a console, no service involved: the bss size from the header. */
static uint32_t ro_buffer_size(uint64_t *out, const void *image) {
    if (!image_is_nro(image)) return RO_NOT_NRO;
    uint32_t bss;
    memcpy(&bss, (const char *)image + 0x38, sizeof bss);
    if (out) *out = bss;
    return 0;
}

/* No rewritten NRO set travels with this port (Engage has no .nro / .nrr), so a load request is
   refused: the image itself is checked first, as the SDK does, so a caller that passes garbage
   gets the SDK's own NRO0 error and not a fake success. */
static uint32_t ro_load_module_impl(const void *image) {
    if (!image_is_nro(image)) return RO_NOT_NRO;
    static int first;
    if (!first++) fprintf(stderr, "[totk-ro] LoadModule: an NRO image was offered but this port has no rewritten NRO set (failure)\n");
    return RO_FAIL;
}
static uint32_t ro_load_module(void *mod, const void *image, void *buffer, uint64_t size, int flag) {
    (void)mod; (void)buffer; (void)size; (void)flag;
    return ro_load_module_impl(image);
}
/* SDK 19 appends a bool after the bind flag; the extra argument only selects eager/lazy binding,
   which a module that never loads cannot reach. */
static uint32_t ro_load_module_bind(void *mod, const void *image, void *buffer, uint64_t size, int flag, int bind) {
    (void)bind;
    return ro_load_module(mod, image, buffer, size, flag);
}

/* Nothing was mapped under a Module, so there is no fini to run and no symbol to withdraw. */
static void ro_unload_module(void *mod) {
    (void)mod;
    static int first;
    if (!first++) fprintf(stderr, "[totk-ro] UnloadModule: no NRO module is loaded in this port (ignored)\n");
}

/* An NRR only registers hashes with the service, which checks them at load time. There is no
   load to check against here, so a well-formed NRR is accepted and the hashes are unused; a
   caller passing something that is not an NRR still fails, as the service would answer. */
static uint32_t ro_register_module_info(void *info, const void *nrr) {
    (void)info;
    char magic[4];
    if (!nrr) return RO_FAIL;
    memcpy(magic, nrr, sizeof magic);
    if (memcmp(magic, "NRR0", 4)) return RO_FAIL;
    static int n;
    if (n++ < 20) fprintf(stderr, "[totk-ro] RegisterModuleInfo: an NRR is registered but no NRO can be loaded here\n");
    return 0;
}
/* SDK 19 adds a trailing uint32 to the same call. */
static uint32_t ro_register_module_info_ex(void *info, const void *nrr, uint32_t extra) {
    (void)extra;
    return ro_register_module_info(info, nrr);
}
static void ro_unregister_module_info(void *info) { (void)info; }

void *runtime_ro_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2ro10InitializeEv", (void *)ro_initialize},
        {"_ZN2nn2ro22InitializeWithPortNameEPKc", (void *)ro_initialize_port},
        {"_ZN2nn2ro8FinalizeEv", (void *)ro_finalize},
        {"_ZN2nn2ro12LookupSymbolEPmPKc", (void *)ro_lookup_symbol},
        {"_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", (void *)ro_lookup_module_symbol},
        {"_ZN2nn2ro13GetBufferSizeEPmPKv", (void *)ro_buffer_size},
        {"_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", (void *)ro_load_module},
        {"_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmib", (void *)ro_load_module_bind},
        {"_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", (void *)ro_unload_module},
        {"_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", (void *)ro_register_module_info},
        {"_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKvj", (void *)ro_register_module_info_ex},
        {"_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", (void *)ro_unregister_module_info}};
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++)
        if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
