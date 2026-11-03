/* Horizon services Bayonetta 3 reaches that TotK never did, answered in-process over IPC (ipc.c). Command numbers and
   layouts follow the public switchbrew descriptions. Unknown commands still stop the runtime with their id. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

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

/* ---------- SDK functions answered by name (runtime_game_override) ----------
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
    runtime_fail("the game asked to start another program index, which is not supported yet");
}
/* Open users kept across a program jump (PushOpenUsers before ExecuteProgram, PopOpenUsers in the next program). A
   fresh start has none; the game then opens its user the usual way (runtime/account.c). */
static void push_open_users(void) {}
static uint32_t pop_open_users(int32_t *count, void *handles, int32_t max) {
    (void)handles; (void)max; if (!count) runtime_fail("PopOpenUsers count pointer"); *count = 0; return 0;
}
#define LDN_UNAVAILABLE RESULT(203, 32)
#define NSD_UNAVAILABLE RESULT(141, 300)
uint64_t mtl_present_count(void);
static void pseudo_device_id(uint8_t *out) {
    if (!out) runtime_fail("GetPseudoDeviceId output");
    const char *dir = getenv("TOTK_PROFILE_DIR"); if (!dir) dir = "user-data";
    char path[1100]; snprintf(path, sizeof path, "%s/pseudo-device-id", dir);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) { ssize_t n = read(fd, out, 16); close(fd); if (n == 16) return; }
    arc4random_buf(out, 16); out[6] = (out[6] & 0x0f) | 0x40; out[8] = (out[8] & 0x3f) | 0x80;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, out, 16) != 16 || close(fd)) runtime_fail("store the pseudo device id in the profile folder");
}
static uint32_t latest_frame_number(uint64_t *out, const void *layer) { (void)layer; if (!out) runtime_fail("GetLatestFrameNumber output"); *out = mtl_present_count(); return 0; }
static int32_t list_frame_history(void *out, int32_t count, const void *layer) { (void)out; (void)count; (void)layer; return 0; }
static int32_t ldn_state(void) { return 0; }
static uint32_t ldn_unavailable(void) { return LDN_UNAVAILABLE; }
static uint32_t nsd_unavailable(void) { return NSD_UNAVAILABLE; }
/* nn::ec::ShowShopAddOnContentList: 2.2.1's DLC menu ("Purchase") opens the eShop library applet and blocks until the
   player closes it. There is no eShop here and the host runs no library applets (the SDK read a null applet proxy; seen in
   play, 2026-10-29), so the shop opens and closes at once and the game returns to its menu. The add-ons the player extracted are listed by
   nn::aoc all the same (2.2.1, XC3_AOC=0 hides them). */
static void shop_closed(uint64_t application_id, const void *user) { (void)user; fprintf(stderr, "[totk] eShop add-on list for %016llx: no shop on this Mac, closed at once\n", (unsigned long long)application_id); }
void *runtime_game_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2oe13MuteRecordingEv", nop}, {"_ZN2nn2oe15UnmuteRecordingEv", nop},
        {"_ZN2nn2oe15EnableRecordingEv", nop}, {"_ZN2nn2oe16DisableRecordingEv", nop},
        {"_ZN2nn2oe14ExecuteProgramEiPKvm", execute_program},
        {"_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", shop_closed},
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
        {"_ZN2nn7account13PushOpenUsersEv", push_open_users},
        {"_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi", pop_open_users},
        {"_ZN2nn2oe16EnableScreenShotEv", nop}, {"_ZN2nn2oe17DisableScreenShotEv", nop},
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", nop}, {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", nop},
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", pseudo_device_id},
        {"_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE", latest_frame_number},
        {"_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE", list_frame_history},
        {"_ZN2nn3ldn8GetStateEv", ldn_state},
        {"_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_unavailable},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", nsd_unavailable},
        {"_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ssl_unavailable},
        {"_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", ssl_unavailable},
        {"_ZN2nn3ssl7Context9ImportCrlEPmPKcj", ssl_unavailable},
        {"_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ssl_unavailable},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

__attribute__((constructor)) static void register_system_services(void) {
    /* The graphics settings below are inherited from games/by3 and games/ot0, where they were measured; Xenoblade Chronicles 3
       runs with them unchanged.
       Bayonetta 3 makes one 1348 MiB NVN memory pool at start; the shared graphics layer refuses zero-copy GPU buffers
       above 1 GiB unless told otherwise (nvn_mtl.m pool_buffer_max). A value the user set wins. */
    setenv("DM_POOL_BUFFER_MAX_MIB", "2048", 0);
    setenv("TOTK_UNCAPPED_INTERVAL", "1", 0);
    /* It draws a depth pre-pass, then the colour pass with a LEQUAL test: positions must match bit for bit between the two
       (nvn_mtl.m totk_precise_opts, preserveInvariance). */
    setenv("DM_PRESERVE_INVARIANCE", "1", 0);
    /* Its shader binaries mix GLSLC GPU code versions (1.15 and 1.16 seen first); the graphics layer's measured driver
       table stops at 1.14. The layer reads only three control-section fields (code length, stage, compute group size)
       and stops on implausible values, so a newer layout would show at once. */
    setenv("DM_NVN_GPU_CODE_MAX_MINOR", "31", 0);
    /* GPU paths the graphics layer keeps isolated until their standalone probe passes (nvn_mtl.m gpu_path_on); each was
       probed on this host before it was switched on here (an explicit value in the environment wins):
         TOTK_GPU_MIDCLEAR  probe-gpu-midclear 28.10. (Apple M5): 9,216 checks pass; Xenoblade Chronicles 3 clears with a
                            scissor or a partial mask inside a pass at its first event after the difficulty selection. */
    setenv("TOTK_GPU_MIDCLEAR", "1", 0);
    runtime_ipc_register("set", settings_handler, NULL, NULL);
    runtime_ipc_register("bsd:u", bsd_handler, NULL, NULL);
    runtime_ipc_register("bsd:s", bsd_handler, NULL, NULL);
}
