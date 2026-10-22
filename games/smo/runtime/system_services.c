/* Horizon services Bayonetta 3 reaches that TotK never did, answered in-process over IPC (ipc.c). Command numbers and
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

/* ---------- SDK functions answered by name (runtime_by3_override) ----------
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
    runtime_fail("the game asked to start its program index 1 (title 01004A4010FEA001), which is not supported yet");
}
/* Open users kept across a program jump (PushOpenUsers before ExecuteProgram, PopOpenUsers in the next program). A
   fresh start has none; the game then opens its user the usual way (runtime/account.c). */
static void push_open_users(void) {}
static uint32_t pop_open_users(int32_t *count, void *handles, int32_t max) {
    (void)handles; (void)max; if (!count) runtime_fail("PopOpenUsers count pointer"); *count = 0; return 0;
}
static void seven_init(void *buf, size_t sz) { (void)buf; (void)sz; }
static void seven_final(void) {}
static void seven_start(void) {}
static void seven_stop(void) {}
static void seven_reset(void) {}
static uint8_t seven_at_rest(void) { return 0; }
static void seven_set_strength(float f) { (void)f; }
static void seven_get_strength(float *f) { if (f) *f = 1.0f; }
static int seven_get_states(void *states, int count) {
    if (states && count > 0) memset(states, 0, count * 0x60);
    return 0;
}
static uint32_t seven_detail_init(void *buf, size_t sz) { (void)buf; (void)sz; return 0; }
static uint32_t seven_detail_final(void) { return 0; }
static uint32_t seven_detail_start(void) { return 0; }
static uint32_t seven_detail_stop(void) { return 0; }
static uint32_t seven_detail_reset(void) { return 0; }
static uint32_t seven_detail_at_rest(uint8_t *out) { if (out) *out = 0; return 0; }
static uint32_t seven_detail_set_strength(float f) { (void)f; return 0; }
static uint32_t seven_detail_get_strength(float *f) { if (f) *f = 1.0f; return 0; }
static uint32_t seven_detail_get_states(int *actual, void *states, int count) {
    if (actual) *actual = 0;
    if (states && count > 0) memset(states, 0, count * 0x60);
    return 0;
}
static void debug_pad_init(void) {}
static void debug_pad_get(void *out) { if (out) memset(out, 0, 0x40); }
static uint32_t debug_pad_detail_get(void *out) { if (out) memset(out, 0, 0x40); return 0; }
static uint32_t debug_pad_detail_gets(int *actual, void *out, int max) { (void)max; if (actual) *actual = 0; if (out) memset(out, 0, 0x40); return 0; }
static void oe_set_orientation(int o) { (void)o; }
static void oe_vol_balance_get(float *a, float *b) { if (a) *a = 1.0f; if (b) *b = 1.0f; }
static void oe_vol_balance_set(float a, float b) { (void)a; (void)b; }
static uint8_t oe_try_pop_notification(uint32_t *msg) { (void)msg; return 0; }
static uint8_t oe_is_inactivity_extended(void) { return 0; }
static void oe_set_inactivity_extended(uint8_t e) { (void)e; }
static uint8_t pctl_stereo_restriction(void) { return 0; }
static void pctl_set_stereo_restriction(uint8_t b) { (void)b; }
static uint8_t pctl_confirm_stereo(void) { return 1; }
static uint32_t pctl_req_stereo(void) { return 0; }
static uint32_t nfp_list_devices(void *handles, int *count, int max) { (void)handles; (void)max; if (count) *count = 0; return 0; }
static uint32_t nfp_get_npad_id(uint32_t *npad, const void *h) { (void)h; if (npad) *npad = 0; return 0; }
static uint32_t bcat_list_dir(int *count, void *entries, int max) { (void)entries; (void)max; if (count) *count = 0; return 0; }
static void hid_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    switch (rq->cmd) {
    case 0: { /* CreateAppletResource */
        rs->move[rs->nmove++] = runtime_ipc_new_session("hid:AppletResource", hid_handler, NULL, NULL);
        return;
    }
    default:
        fprintf(stderr, "[totk-hid] hid IPC cmd %u stubbed\n", rq->cmd);
        return;
    }
}
static void friend_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    switch (rq->cmd) {
    case 0: { /* CreateFriendService -> IFriendService */
        rs->move[rs->nmove++] = runtime_ipc_new_session("friend:IFriendService", friend_handler, NULL, NULL);
        return;
    }
    default:
        fprintf(stderr, "[totk-net] friend IPC cmd %u stubbed\n", rq->cmd);
        return;
    }
}
void *runtime_by3_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2oe13MuteRecordingEv", nop}, {"_ZN2nn2oe15UnmuteRecordingEv", nop},
        {"_ZN2nn2oe15EnableRecordingEv", nop}, {"_ZN2nn2oe16DisableRecordingEv", nop},
        {"_ZN2nn2oe29SetScreenShotImageOrientationENS_5album16ImageOrientationE", oe_set_orientation},
        {"_ZN2nn2oe24SetAlbumImageOrientationENS_5album16ImageOrientationE", oe_set_orientation},
        {"_ZN2nn2oe16EnableScreenShotEv", nop}, {"_ZN2nn2oe17DisableScreenShotEv", nop},
        {"_ZN2nn2oe23BeginBlockingHomeButtonEv", nop}, {"_ZN2nn2oe21EndBlockingHomeButtonEv", nop},
        {"_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", oe_vol_balance_get},
        {"_ZN2nn2oe24SetExpectedVolumeBalanceEff", oe_vol_balance_set},
        {"_ZN2nn2oe25TryPopNotificationMessageEPj", oe_try_pop_notification},
        {"_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv", oe_is_inactivity_extended},
        {"_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", oe_set_inactivity_extended},
        {"_ZN2nn4pctl26GetStereoVisionRestrictionEv", pctl_stereo_restriction},
        {"_ZN2nn4pctl26SetStereoVisionRestrictionEb", pctl_set_stereo_restriction},
        {"_ZN2nn4pctl42ConfirmStereoVisionRestrictionConfigurableEv", pctl_confirm_stereo},
        {"_ZN2nn4pctl53RequestStereoVisionRestrictionConfigurationPermissionEv", pctl_req_stereo},
        {"_ZN2nn3nfp10InitializeEv", ok}, {"_ZN2nn3nfp8FinalizeEv", nop},
        {"_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", nfp_list_devices},
        {"_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", ok},
        {"_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", ok},
        {"_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", ok},
        {"_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", nfp_get_npad_id},
        {"_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", ok},
        {"_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE", ok},
        {"_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", ok},
        {"_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", ok},
        {"_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", ok},
        {"_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", ok},
        {"_ZN2nn4bcat10InitializeEv", ok},
        {"_ZN2nn4bcat25MountDeliveryCacheStorageEv", ok},
        {"_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", ok},
        {"_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", ok},
        {"_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi", bcat_list_dir},
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
        {"_ZN2nn7friends10InitializeEv", ok},
        {"_ZN2nn7friends6detail3ipc10InitializeEv", ok},
        {"_ZN2nn7friends6detail17ShimLibraryGlobal10InitializeEv", ok},
        {"_ZN2nn7friends6detail3ipc8FinalizeEv", nop},
        {"_ZN2nn7friends12UserPresence10InitializeEv", ok},
        {"_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", ok},
        {"_ZN2nn7friends17NotificationQueue10InitializeEv", ok},
        {"_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE", ok},
        {"_ZN2nn7friends17NotificationQueue8FinalizeEv", nop},
        {"_ZN2nn7friends11UserSetting10InitializeERKNS_7account3UidE", ok},
        {"_ZN2nn7friends13FriendSetting10InitializeERKNS_7account3UidENS2_23NetworkServiceAccountIdE", ok},
        {"_ZN2nn4time26StandardNetworkSystemClock3nowEv", network_clock_now},
        {"_ZN2nn3err21ReportMultimediaErrorENS_6ResultERKN5movie25MultimediaTelemetryReportE", nop},
        {"_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", environment_identifier},
        {"_ZN2nn5prepo10InitializeEv", nop},
        {"_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", nop},
        {"_ZN2nn3hid28InitializeSevenSixAxisSensorEPvm", seven_init},
        {"_ZN2nn3hid26FinalizeSevenSixAxisSensorEv", seven_final},
        {"_ZN2nn3hid5debug26FinalizeSevenSixAxisSensorEv", seven_final},
        {"_ZN2nn3hid23StartSevenSixAxisSensorEv", seven_start},
        {"_ZN2nn3hid22StopSevenSixAxisSensorEv", seven_stop},
        {"_ZN2nn3hid27GetSevenSixAxisSensorStatesEPNS0_23SevenSixAxisSensorStateEi", seven_get_states},
        {"_ZN2nn3hid32ResetSevenSixAxisSensorTimestampEv", seven_reset},
        {"_ZN2nn3hid26IsSevenSixAxisSensorAtRestEv", seven_at_rest},
        {"_ZN2nn3hid35SetSevenSixAxisSensorFusionStrengthEf", seven_set_strength},
        {"_ZN2nn3hid35GetSevenSixAxisSensorFusionStrengthEPf", seven_get_strength},
        {"_ZN2nn3hid6detail28InitializeSevenSixAxisSensorEPvm", seven_detail_init},
        {"_ZN2nn3hid6detail26FinalizeSevenSixAxisSensorEv", seven_detail_final},
        {"_ZN2nn3hid6detail23StartSevenSixAxisSensorEv", seven_detail_start},
        {"_ZN2nn3hid6detail22StopSevenSixAxisSensorEv", seven_detail_stop},
        {"_ZN2nn3hid6detail27GetSevenSixAxisSensorStatesEPiPNS0_23SevenSixAxisSensorStateEi", seven_detail_get_states},
        {"_ZN2nn3hid6detail32ResetSevenSixAxisSensorTimestampEv", seven_detail_reset},
        {"_ZN2nn3hid6detail26IsSevenSixAxisSensorAtRestEPb", seven_detail_at_rest},
        {"_ZN2nn3hid6detail35SetSevenSixAxisSensorFusionStrengthEf", seven_detail_set_strength},
        {"_ZN2nn3hid6detail35GetSevenSixAxisSensorFusionStrengthEPf", seven_detail_get_strength},
        {"_ZN2nn3hid18InitializeDebugPadEv", debug_pad_init},
        {"_ZN2nn3hid16GetDebugPadStateEPNS0_13DebugPadStateE", debug_pad_get},
        {"_ZN2nn3hid17GetDebugPadStatesEPNS0_13DebugPadStateEi", debug_pad_detail_gets},
        {"_ZN2nn3hid6detail18InitializeDebugPadEv", debug_pad_init},
        {"_ZN2nn3hid6detail16FinalizeDebugPadEv", nop},
        {"_ZN2nn3hid6detail16GetDebugPadStateEPNS0_13DebugPadStateE", debug_pad_detail_get},
        {"_ZN2nn3hid6detail17GetDebugPadStatesEPiPNS0_13DebugPadStateEi", debug_pad_detail_gets},
        {"_ZN2nn7account13PushOpenUsersEv", push_open_users},
        {"_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi", pop_open_users},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

__attribute__((constructor)) static void register_system_services(void) {
    /* Bayonetta 3 makes one 1348 MiB NVN memory pool at start; the shared graphics layer refuses zero-copy GPU buffers
       above 1 GiB unless told otherwise (nvn_mtl.m pool_buffer_max). A value the user set wins. */
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
    runtime_ipc_register("hid", hid_handler, NULL, NULL);
    runtime_ipc_register("friend:u", friend_handler, NULL, NULL);
    runtime_ipc_register("friend:v", friend_handler, NULL, NULL);
    runtime_ipc_register("friend:m", friend_handler, NULL, NULL);
}
