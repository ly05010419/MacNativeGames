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
#define RES_FAIL 0x2A2u
static void nop(void) {}
static uint32_t ok(void) { return 0; }
static uint32_t fail_res(void) { return RES_FAIL; }
static uint32_t fail_count(int *count, ...) { if (count) *count = 0; return RES_FAIL; }
static uint32_t ok_count(int *count, ...) { if (count) *count = 0; return 0; }
static uint32_t false_out(uint8_t *out, ...) { if (out) *out = 0; return 0; }
static const void *null_ptr(void) { return NULL; }
static uint8_t network_available(void) { return 0; }
static uint32_t nifm_unavailable(void) { return NIFM_UNAVAILABLE; }
static uint32_t ssl_unavailable(void) { return SSL_UNAVAILABLE; }
static uint32_t nsa_unavailable(void) { return NSA_UNAVAILABLE; }
static uint32_t empty_delivery_directories(int *count,void *names,int capacity){
    (void)names; (void)capacity;
    if(!count)runtime_fail("delivery-cache directory count pointer");
    *count=0; return 0;
}
static void nifm_request(void) { static int once; if (!once++) fprintf(stderr, "[totk-net] network request: offline\n"); }
/* Non-blocking network request (Odyssey 1.3.0 main calls these from one module at 0x6c9530-0x6cc984, research, 22 October).
   Without an override the SDK's own code ran: no IPC, but HandleNetworkRequestResult returned 0xca6e, which the game
   treats as an error. Per smo-recomp (runtime/os/smo_nn.c), 0 means connected, nifm 190 (0x17c6e: the error was shown
   and dealt with) means not connected, and anything else is an error. */
#define NIFM_HANDLED RESULT(110, 190)
static uint8_t request_on_hold(void) { return 0; }
static uint32_t nifm_request_handled(void) { return NIFM_HANDLED; }
static uint32_t nsa_available(uint8_t *available, const void *user) {
    (void)user; if (!available) runtime_fail("IsNetworkServiceAccountAvailable result pointer"); *available = 0; return 0;
}
static int64_t network_clock_now(void) { return (int64_t)time(NULL); }   /* time_point: seconds since the epoch */
static void environment_identifier(char *out) { if (!out) runtime_fail("nsd environment output"); memset(out, 0, 8); memcpy(out, "lp1", 3); }
static void execute_program(int index, const void *param, uint64_t size) {
    (void)param; fprintf(stderr, "[totk] nn::oe::ExecuteProgram(%d, %llu bytes)\n", index, (unsigned long long)size);
    runtime_fail("the game asked to start its program index 1 (title 01004A4010FEA001), which is not supported yet");
}
/* Open users kept across a program jump (PushOpenUsers before ExecuteProgram, PopOpenUsers in the next program). A
   fresh start has none; the game then opens its user the usual way (runtime/account.c). */
static void push_open_users(void) {}
/* Odyssey 1.3.0 main imports this SDK call when opening the movie-skip dialog.
   The original sdk+0x323a24 dereferences a Switch album service pointer that is
   null in this host. macOS captures are already oriented by the window system. */
static void screenshot_image_orientation(int orientation) {
    static int reported;
    if (!reported++) fprintf(stderr, "[odyssey-oe] screenshot image orientation request %d handled by macOS window\n", orientation);
}
static uint32_t pop_open_users(int32_t *count, void *handles, int32_t max) {
    (void)handles; (void)max; if (!count) runtime_fail("PopOpenUsers count pointer"); *count = 0; return 0;
}
void *runtime_by3_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2oe13MuteRecordingEv", nop}, {"_ZN2nn2oe15UnmuteRecordingEv", nop},
        {"_ZN2nn2oe15EnableRecordingEv", nop}, {"_ZN2nn2oe16DisableRecordingEv", nop},
        {"_ZN2nn2oe29SetScreenShotImageOrientationENS_5album16ImageOrientationE", screenshot_image_orientation},
        {"_ZN2nn2oe14ExecuteProgramEiPKvm", execute_program},
        {"_ZN2nn4nifm10InitializeEv", ok}, {"_ZN2nn4nifm18IsNetworkAvailableEv", network_available},
        {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_request}, {"_ZN2nn4nifm20CancelNetworkRequestEv", nop},
        {"_ZN2nn4nifm20SubmitNetworkRequestEv", nifm_request}, {"_ZN2nn4nifm22IsNetworkRequestOnHoldEv", request_on_hold},
        {"_ZN2nn4nifm26HandleNetworkRequestResultEv", nifm_request_handled},
        {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", nsa_available},
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
        {"_ZN2nn7friends10InitializeEv", nop},
        {"_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", nop},
        {"_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE", fail_res},
        {"_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE", fail_res},
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", fail_count},
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE", fail_count},
        {"_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi", fail_res},
        {"_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendEPKNS_7account23NetworkServiceAccountIdEi", fail_res},
        {"_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii", ok_count},
        {"_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdEii", ok_count},
        {"_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", fail_res},
        {"_ZN2nn7friends25StartSendingFriendRequestERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", fail_res},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", fail_res},
        {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", fail_res},
        {"_ZN2nn7friends14AddPlayHistoryERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameES6_", fail_res},
        {"_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", fail_res},
        {"_ZN2nn7friends12UserPresence14SetDescriptionEPKc", fail_res},
        {"_ZN2nn7friends12UserPresence6CommitEv", fail_res},
        {"_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", false_out},
        {"_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", fail_res},
        {"_ZN2nn7friends14ShowFriendListERKNS_7account3UidE", fail_res},
        {"_ZN2nn7friends19ShowBlockedUserListERKNS_7account3UidE", fail_res},
        {"_ZN2nn7friends29ShowReceivedFriendRequestListERKNS_7account3UidE", fail_res},
        {"_ZN2nn7friends23StartFacedFriendRequestERKNS_7account3UidE", fail_res},
        {"_ZN2nn7friends33ShowMethodsOfSendingFriendRequestERKNS_7account3UidE", fail_res},
        {"_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi", fail_res},
        {"_ZN2nn4bcat10InitializeEv", nop},
        {"_ZN2nn4bcat25MountDeliveryCacheStorageEv", fail_res},
        {"_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", nop},
        {"_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi", empty_delivery_directories},
        {"_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE", fail_res},
        {"_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm", fail_res},
        {"_ZN2nn4bcat17DeliveryCacheFile9GetDigestEv", null_ptr},
        {"_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv", fail_res},
        {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", fail_res},
        {"_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi", fail_count},
        {"_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", fail_res},
        {"_ZN2nn3ldn10InitializeEv", ok},
        {"_ZN2nn3ldn8FinalizeEv", ok},
        {"_ZN2nn3ldn8GetStateEv", ok},
        {"_ZN2nn3ldn11OpenStationEv", fail_res},
        {"_ZN2nn3ldn12CloseStationEv", ok},
        {"_ZN2nn3ldn15OpenAccessPointEv", fail_res},
        {"_ZN2nn3ldn16CloseAccessPointEv", ok},
        {"_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ok},
        {"_ZN2nn3nfp10InitializeEv", ok},
        {"_ZN2nn3nfp8FinalizeEv", ok},
        {"_ZN2nn3nfp8GetStateEv", ok},
        {"_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", ok_count},
        {"_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", ok},
        {"_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", ok},
        {"_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", ok},
        {"_ZN2nn3nfp28AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", ok},
        {"_ZN2nn4time26StandardNetworkSystemClock3nowEv", network_clock_now},
        {"_ZN2nn3err21ReportMultimediaErrorENS_6ResultERKN5movie25MultimediaTelemetryReportE", nop},
        {"_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", environment_identifier},
        {"_ZN2nn5prepo10InitializeEv", nop},
        {"_ZN2nn5prepo28RequestImmediateTransmissionEv", ok},
        {"_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", ok},
        {"_ZN2nn5prepo10PlayReport4SaveEv", ok},
        {"_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", nop},
        {"_ZN2nn7account13PushOpenUsersEv", push_open_users},
        {"_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi", pop_open_users},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

static void dummy_service_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s; (void)rq;
    uint32_t zero = 0;
    IPC_OUT(rs, zero);
}

static void generic_service_creator(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    if (rq->cmd <= 5) {
        rs->move[rs->nmove++] = runtime_ipc_new_session("service:child", dummy_service_handler, NULL, NULL);
        return;
    }
    uint32_t zero = 0;
    IPC_OUT(rs, zero);
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
    setenv("TOTK_GPU_MIDCLEAR", "1", 0);
    setenv("TOTK_GPU_ALLOW_SUBMIT", "1", 0);
    setenv("TOTK_GPU_WIDEN", "1", 0);
    setenv("TOTK_GPU_BLOCKRT", "1", 0);
    runtime_ipc_register("set", settings_handler, NULL, NULL);
    runtime_ipc_register("bsd:u", bsd_handler, NULL, NULL);
    runtime_ipc_register("friend:u", generic_service_creator, NULL, NULL);
    runtime_ipc_register("bcat:u", generic_service_creator, NULL, NULL);
    runtime_ipc_register("bcat:a", generic_service_creator, NULL, NULL);
    runtime_ipc_register("bcat:m", generic_service_creator, NULL, NULL);
    runtime_ipc_register("ldn:u", generic_service_creator, NULL, NULL);
    runtime_ipc_register("nfp:user", generic_service_creator, NULL, NULL);
}
