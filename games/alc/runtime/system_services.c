/* Horizon services Astral Chain reaches that TotK never did, answered in-process over IPC (ipc.c). Command numbers and
   layouts follow the public switchbrew descriptions. Unknown commands still stop the runtime with their id. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

/* ---------- set: nn::settings ISettingsServer ---------- */
/* nn::settings::Language order -> language code (switchbrew "Settings services", LanguageCode). */
static const char *const language_codes[] = {
    "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419",
    "zh-Hans", "zh-Hant", "pt-BR",
};
enum { LANGUAGES = sizeof language_codes / sizeof *language_codes, REGION_USA = 1 };

static uint64_t code_of(unsigned language) {
    uint64_t v = 0;
    memcpy(&v, language_codes[language], strlen(language_codes[language]));
    return v;
}
/* Writes up to `room` codes into a guest out buffer; returns how many. */
static int32_t write_codes(uint64_t addr, uint64_t size) {
    uint64_t *out = ipc_guest(addr, size);
    int32_t n = 0;
    for (; n < LANGUAGES && (uint64_t)(n + 1) * 8 <= size; n++) out[n] = code_of((unsigned)n);
    return n;
}
static void settings_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { uint64_t v = runtime_desired_language(); IPC_OUT(rs, v); return; }   /* GetLanguageCode */
    case 1: {                                                                    /* GetAvailableLanguageCodes (C buffer) */
        if (!rq->nc) runtime_fail("set GetAvailableLanguageCodes: no output buffer");
        int32_t n = write_codes(rq->c[0].addr, rq->c[0].size);
        rs->xout[rs->nxout++] = (IpcBuffer){rq->c[0].addr, (uint64_t)n * 8, 0};
        IPC_OUT(rs, n); return;
    }
    case 2: {                                                                    /* MakeLanguageCode(Language) */
        uint32_t language = IPC_ARG(rq, uint32_t, 0);
        if (language >= LANGUAGES) { fprintf(stderr, "[totk-set] MakeLanguageCode(%u)\n", language); runtime_fail("set MakeLanguageCode: unknown language"); }
        uint64_t v = code_of(language); IPC_OUT(rs, v); return;
    }
    case 3: case 6: { int32_t n = LANGUAGES; IPC_OUT(rs, n); return; }          /* GetAvailableLanguageCodeCount(2) */
    case 4: { int32_t v = REGION_USA; IPC_OUT(rs, v); return; }                   /* GetRegionCode */
    case 5: {                                                                    /* GetAvailableLanguageCodes2 (B buffer) */
        if (!rq->nb) runtime_fail("set GetAvailableLanguageCodes2: no output buffer");
        int32_t n = write_codes(rq->b[0].addr, rq->b[0].size); IPC_OUT(rs, n); return;
    }
    case 8: { uint8_t v = 0; IPC_OUT(rs, v); return; }                            /* GetQuestFlag: not a kiosk unit */
    default: ipc_unimplemented(s, rq);
    }
}

/* ---------- bsd:u: nn::socket, offline ---------- */
/* The game is played offline, as a Switch without a connection: the client registers and its monitor starts, but every
   socket it asks for fails with ENETDOWN. Commands return {s32 ret, u32 errno} (switchbrew "Sockets services"). */
enum { BSD_ENETDOWN = 50 };
static void bsd_result(IpcResponse *rs, int32_t ret, uint32_t err) { IPC_OUT(rs, ret); IPC_OUT(rs, err); }
static void bsd_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: { uint64_t pid = 0; IPC_OUT(rs, pid); return; }                         /* RegisterClient(config, tmem) */
    case 1: return;                                                               /* StartMonitoring(pid) */
    case 2: case 3:                                                               /* Socket / SocketExempt */
        fprintf(stderr, "[totk-net] socket(%u, %u, %u) refused: offline\n", IPC_ARG(rq, uint32_t, 0), IPC_ARG(rq, uint32_t, 4), IPC_ARG(rq, uint32_t, 8));
        bsd_result(rs, -1, BSD_ENETDOWN); return;
    default: ipc_unimplemented(s, rq);
    }
}

/* ---------- SDK functions answered by name (runtime_alc_override) ----------
   Network-facing libraries answer as a Switch without a connection. The failure results are generic ones of each
   library's module (nifm 110, ssl 123, account 124), not a specific console error: the game only needs "unavailable". */
#define RESULT(module, description) ((uint32_t)(module) | ((uint32_t)(description) << 9))
#define NIFM_UNAVAILABLE RESULT(110, 300)
#define SSL_UNAVAILABLE RESULT(123, 300)
#define NSA_UNAVAILABLE RESULT(124, 280)
static void nop(void) {}
static uint32_t ok(void) { return 0; }
static uint8_t network_available(void) { return 0; }
static uint32_t nifm_unavailable(void) { return NIFM_UNAVAILABLE; }
static uint32_t ssl_unavailable(void) { return SSL_UNAVAILABLE; }
static uint32_t nsa_unavailable(void) { return NSA_UNAVAILABLE; }
static void nifm_request(void) { static int once; if (!once++) fprintf(stderr, "[totk-net] network request: offline\n"); }
static int64_t network_clock_now(void) { return (int64_t)time(NULL); }   /* time_point: seconds since the epoch */
static void environment_identifier(char *out) { if (!out) runtime_fail("nsd environment output"); memset(out, 0, 8); memcpy(out, "lp1", 3); }
static void execute_program(int index, const void *param, uint64_t size) {
    (void)param; fprintf(stderr, "[totk] nn::oe::ExecuteProgram(%d, %llu bytes)\n", index, (unsigned long long)size);
    runtime_fail("the game asked to start its program index 1 (title 01007300020FA801), which is not supported yet");
}
/* Open users kept across a program jump (PushOpenUsers before ExecuteProgram, PopOpenUsers in the next program). A
   fresh start has none; the game then opens its user the usual way (runtime/account.c). */
/* nn::vi answered by name: there is no vi IPC service and presentation is the host's own window (the by3 model).
   The display and layer become distinct non-null markers so the game's bookkeeping holds; GetNativeWindow keeps
   by3's behavior of handing the NVN window no native handle (the graphics layer presents from its own window). */
static uint32_t vi_open_display(void **out) { if (!out) runtime_fail("vi OpenDefaultDisplay output"); *out = (void *)0x11; return 0; }
static uint32_t vi_create_layer(void **layer, void **display) { (void)display; if (!layer) runtime_fail("vi CreateLayer output"); *layer = (void *)0x21; return 0; }
static uint32_t vi_get_native_window(void **out, void *layer) { (void)layer; if (!out) runtime_fail("vi GetNativeWindow output"); *out = 0; return 0; }
static void push_open_users(void) {}
static uint32_t pop_open_users(int32_t *count, void *handles, int32_t max) {
    (void)handles; (void)max; if (!count) runtime_fail("PopOpenUsers count pointer"); *count = 0; return 0;
}
void *runtime_alc_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2oe13MuteRecordingEv", nop}, {"_ZN2nn2oe15UnmuteRecordingEv", nop},
        {"_ZN2nn2oe15EnableRecordingEv", nop}, {"_ZN2nn2oe16DisableRecordingEv", nop},
        {"_ZN2nn2oe14ExecuteProgramEiPKvm", execute_program},
        {"_ZN2nn4nifm10InitializeEv", ok}, {"_ZN2nn4nifm18IsNetworkAvailableEv", network_available},
        {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_request}, {"_ZN2nn4nifm20CancelNetworkRequestEv", nop},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_unavailable},
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", nifm_unavailable},
        {"_ZN2nn3ssl10InitializeEv", ok}, {"_ZN2nn3ssl8FinalizeEv", ok},
        {"_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", ssl_unavailable},
        {"_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", ssl_unavailable},
        {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", nsa_unavailable},
        {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", nsa_unavailable},
        {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", nsa_unavailable},
        {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", nsa_unavailable},
        {"_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", ok},
        {"_ZN2nn4time26StandardNetworkSystemClock3nowEv", network_clock_now},
        {"_ZN2nn3err21ReportMultimediaErrorENS_6ResultERKN5movie25MultimediaTelemetryReportE", nop},
        {"_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", environment_identifier},
        {"_ZN2nn5prepo10InitializeEv", nop},
        {"_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", nop},
        {"_ZN2nn2vi10InitializeEv", ok},
        {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_display},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer},
        {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window},
        {"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", ok},
        {"_ZN2nn7account13PushOpenUsersEv", push_open_users},
        {"_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi", pop_open_users},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

__attribute__((constructor)) static void register_system_services(void) {
    /* By3's game made one 1348 MiB NVN memory pool at start; Astral Chain's pool is measured at boot. The shared
       graphics layer refuses zero-copy GPU buffers above 1 GiB unless told otherwise (nvn_mtl.m pool_buffer_max).
       A value the user set wins. */
    setenv("DM_POOL_BUFFER_MAX_MIB", "2048", 0);
    /* Present pacing as on the Switch: a 60 Hz base, the game's present interval on top (1 = 60 fps, 2 = 30 fps). The
       TotK host follows the display (120 Hz on ProMotion) and runs uncapped, which relies on TotK's real-time frame step
       patch; without it a game that advances per frame would run twice as fast. */
    setenv("DM_PRESENT_HZ", "60", 0);
    setenv("TOTK_UNCAPPED", "0", 0);
    /* It draws a depth pre-pass, then the colour pass with a LEQUAL test: positions must match bit for bit between the two
       (nvn_mtl.m totk_precise_opts, preserveInvariance). */
    setenv("DM_PRESERVE_INVARIANCE", "1", 0);
    /* Its shader binaries mix GLSLC GPU code versions (1.15 and 1.16 seen first); the graphics layer's measured driver
       table stops at 1.14. The layer reads only three control-section fields (code length, stage, compute group size)
       and stops on implausible values, so a newer layout would show at once. */
    setenv("DM_NVN_GPU_CODE_MAX_MINOR", "31", 0);
    runtime_ipc_register("set", settings_handler, NULL, NULL);
    runtime_ipc_register("bsd:u", bsd_handler, NULL, NULL);
}
