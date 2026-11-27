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

/* ---------- csrng: IRandomInterface ---------- */
/* GenerateRandomBytes (command 0) has one map-alias output buffer (SfBufferAttr_Out).
   arc4random_buf uses the host CSPRNG, including reseeding across process forks. */
static void csrng_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)rs;
    if (rq->cmd != 0) ipc_unimplemented(s, rq);
    if (rq->nb != 1 || rq->na || rq->nx || rq->nw || rq->nc)
        runtime_fail("csrng GenerateRandomBytes: expected one output B buffer");
    const IpcBuffer *b = &rq->b[0];
    void *out = ipc_guest(b->addr, b->size);
    if (b->size) arc4random_buf(out, (size_t)b->size);
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
    case 24: bsd_result(rs, -1, BSD_ENETDOWN); return; /* Bind */
    case 31: bsd_result(rs, -1, BSD_ENETDOWN); return; /* SetSockOpt */
    case 6: bsd_result(rs, -1, BSD_ENETDOWN); return; /* Poll: networking is offline. */
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
/* Tegra NVDEC is not on this Mac. The movie library treats a null import as fatal
   (it asserts instead of returning), so answer the open with a failure the caller
   already checks. The first argument arrives as a small integer, not a pointer. */
static int nvdec_open(void) {
    static int once; if (!once++) fprintf(stderr, "[movie] NVDEC channel open refused: no Tegra decoder\n");
    return -1;
}
static int nvdec_close(void *channel) { (void)channel; return 0; }
/* The failure path waits on a semaphore the open never created. There is nothing to wait for. */
static int nvdec_sem_wait(void *sem) { (void)sem; return 0; }
static void nvdec_sleep(unsigned ms) { (void)ms; }
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
static int32_t zero_count(int32_t *out) { if (out) *out = 0; return 0; }
static int32_t list_audio_ins(void *out, int32_t max) { (void)out; (void)max; return 0; }
static uint32_t async_has_done(uint8_t *out) { if (out) *out = 1; return 0; }
static void aoc_list_changed_event(void *ev) {
    static uint32_t (*create)(void *, int, int);
    if (!create) create = runtime_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    if (create && ev) create(ev, 1, 0);
}
static uint32_t account_async_event(void *ctx, void *ev) {
    (void)ctx;
    aoc_list_changed_event(ev);
    return 0;
}
static uint32_t create_nsa_auth_req(void *ctx, const void *user, const void *param, void *buf, uint64_t size) {
    (void)ctx; (void)user; (void)param; (void)buf; (void)size;
    return NSA_UNAVAILABLE;
}
static uint32_t show_shop_aoc(uint64_t app_id, const void *user) {
    (void)app_id; (void)user;
    fprintf(stderr, "[totk-net] ShowShopAddOnContentList(%#llx): offline (ignored)\n", (unsigned long long)app_id);
    return 0;
}
static uint32_t show_web_page(void *ret, const void *arg) {
    (void)ret; (void)arg;
    fprintf(stderr, "[totk-net] ShowWebPage: offline (ignored)\n");
    return 0;
}
static uint8_t try_begin_free_comm(uint8_t show_ui) {
    (void)show_ui;
    return 0;
}
static uint32_t ssl_import_pki(void *ctx, uint64_t *out_id, const char *cert, uint32_t size, int fmt) {
    (void)ctx; (void)cert; (void)size; (void)fmt;
    if (out_id) *out_id = 1;
    return 0;
}
static uint32_t nsd_resolve_ex(void *out_fqdn, const void *in_fqdn) {
    if (out_fqdn && in_fqdn) memcpy(out_fqdn, in_fqdn, 0x100);
    return 0;
}
static void oe_get_display_version(char *out) {
    if (!out) return;
    memset(out, 0, 16);
    memcpy(out, "1.0.4", 5);
}
static void oe_get_pseudo_device_id(void *out) {
    if (!out) return;
    static const uint8_t uuid[16] = {0x49,0x54,0x54,0x2d,0x4d,0x41,0x43,0x2d,0x4e,0x41,0x54,0x49,0x56,0x45,0x30,0x31};
    memcpy(out, uuid, 16);
}
static uint32_t oe_get_perf_config(int mode) {
    return mode ? 0x92220008u : 0x20004u;
}
static uint32_t oe_get_server_env_type(void) {
    return 0;
}
static void *friend_invitation_event(void) {
    static uint8_t ev[0x30] __attribute__((aligned(8)));
    static int inited;
    if (!inited) {
        inited = 1;
        aoc_list_changed_event(ev);
    }
    return ev;
}
static uint8_t friend_invitation_try_pop(void *uid, uint64_t *size, void *buf, uint64_t buf_size) {
    (void)uid; (void)size; (void)buf; (void)buf_size;
    return 0;
}
static uint32_t shop_accessor_request(void) {
    return RESULT(164, 1);
}
static uint32_t ldn_unavailable(void) {
    return RESULT(203, 1);
}
static uint32_t ldn_make_ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}
static uint32_t account_get_empty_token(void *ctx, uint64_t *out_size, char *buf, uint64_t max_size) {
    (void)ctx;
    if (out_size) *out_size = 0;
    if (buf && max_size) buf[0] = 0;
    return NSA_UNAVAILABLE;
}
static uint32_t account_is_nsa_available(uint8_t *out, const void *user) {
    (void)user;
    if (out) *out = 0;
    return 0;
}
static uint32_t account_license_kind(void *ctx) {
    (void)ctx;
    return 0;
}
static uint32_t ngc_mask_profanity(void *this_, int32_t *out_num, void *text, uint32_t mask) {
    (void)this_; (void)text; (void)mask;
    if (out_num) *out_num = 0;
    return 0;
}
static uint32_t ngc_check_profanity(void *this_, uint32_t *out_flags, uint32_t mask, const void *words, uint64_t count) {
    (void)this_; (void)mask; (void)words;
    if (out_flags && count) memset(out_flags, 0, count * sizeof(uint32_t));
    return 0;
}
static uint32_t ngc_content_version(void *this_) {
    (void)this_;
    return 1;
}
static void show_error_code(uint64_t code) {
    fprintf(stderr, "[totk] nn::err::ShowError(%#llx): ignored\n", (unsigned long long)code);
}
static void show_app_error(const void *arg) {
    (void)arg;
    fprintf(stderr, "[totk] nn::err::ShowApplicationError: ignored\n");
}
static uint32_t audio_active_device_name(char *out) {
    if (out) { memset(out, 0, 0x100); memcpy(out, "AudioTvOutput", 13); }
    return 0;
}
static uint32_t audio_device_notification(void *ev) {
    aoc_list_changed_event(ev);
    return 0;
}
static int32_t audio_active_channel_count(void) {
    return 2;
}
void *runtime_by3_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"NvRmChannelOpen", nvdec_open}, {"NvRmChannelClose", nvdec_close},
        {"NvOsSemaphoreWait", nvdec_sem_wait}, {"NvOsSleepMS", nvdec_sleep},
        {"_ZN2nn2oe13MuteRecordingEv", nop}, {"_ZN2nn2oe15UnmuteRecordingEv", nop},
        {"_ZN2nn2oe15EnableRecordingEv", nop}, {"_ZN2nn2oe16DisableRecordingEv", nop},
        /* Same class as recording: the Mac has no capture button or album. The SDK
           bodies call GetSelfController and fault because g_Interfaces is unset. */
        {"_ZN2nn2oe16EnableScreenShotEv", nop}, {"_ZN2nn2oe17DisableScreenShotEv", nop},
        {"_ZN2nn2oe24SetAlbumImageOrientationENS_5album16ImageOrientationE", nop},
        {"_ZN2nn2oe29SetScreenShotImageOrientationENS_5album16ImageOrientationE", nop},
        {"_ZN2nn2oe33EnableAlbumImageTakenNotificationEv", nop},
        {"_ZN2nn2oe34DisableAlbumImageTakenNotificationEv", nop},
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", nop}, {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", nop},
        {"_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", oe_get_display_version},
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", oe_get_pseudo_device_id},
        {"_ZN2nn2oe27GetPerformanceConfigurationENS0_15PerformanceModeE", oe_get_perf_config},
        {"_ZN2nn2oe24GetServerEnvironmentTypeEv", oe_get_server_env_type},
        {"_ZN2nn2oe14ExecuteProgramEiPKvm", execute_program},
        {"_ZN2nn4nifm10InitializeEv", ok}, {"_ZN2nn4nifm18IsNetworkAvailableEv", network_available},
        {"_ZN2nn4nifm20SubmitNetworkRequestEv", nifm_request},
        {"_ZN2nn4nifm22IsNetworkRequestOnHoldEv", network_available},
        {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_request}, {"_ZN2nn4nifm20CancelNetworkRequestEv", nop},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_unavailable},
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", nifm_unavailable},
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", nifm_unavailable},
        {"_ZN2nn3ssl10InitializeEv", ok}, {"_ZN2nn3ssl10InitializeEj", ok}, {"_ZN2nn3ssl8FinalizeEv", ok},
        {"_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", ssl_unavailable},
        {"_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", ssl_unavailable},
        {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", account_is_nsa_available},
        {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", nsa_unavailable},
        {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", nsa_unavailable},
        {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", nsa_unavailable},
        {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", nsa_unavailable},
        {"_ZN2nn7account12AsyncContext7HasDoneEPb", async_has_done},
        {"_ZN2nn7account12AsyncContext9GetResultEv", nsa_unavailable},
        {"_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", account_async_event},
        {"_ZN2nn7account41CreateNintendoAccountAuthorizationRequestEPNS0_42NintendoAccountAuthorizationRequestContextERKNS0_10UserHandleERKNS0_45NintendoAccountAuthorizationRequestParametersEPvm", create_nsa_auth_req},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext10GetIdTokenEPmPcm", account_get_empty_token},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext20GetAuthorizationCodeEPmPcm", account_get_empty_token},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext8GetStateEPmPcm", account_get_empty_token},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext14GetSystemEventEPNS_2os11SystemEventE", account_async_event},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext34GetResultWithInteractionIfRequiredEv", nsa_unavailable},
        {"_ZN2nn7account42NintendoAccountAuthorizationRequestContext6CancelEv", ok},
        {"_ZN2nn7account34LoadNetworkServiceLicenseInfoAsyncEPNS0_37AsyncNetworkServiceLicenseInfoContextERKNS0_10UserHandleE", ok},
        {"_ZN2nn7account37AsyncNetworkServiceLicenseInfoContext28GetNetworkServiceLicenseKindEv", account_license_kind},
        {"_ZN2nn7friends10InitializeEv", nop},
        {"_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", ok},
        {"_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", ok},
        {"_ZN2nn7friends12UserPresence14SetDescriptionEPKc", ok},
        {"_ZN2nn7friends12UserPresence11SetAppValueEPKcS3_", ok},
        {"_ZN2nn7friends12UserPresence28DeclareOpenOnlinePlaySessionEv", nop},
        {"_ZN2nn7friends12UserPresence29DeclareCloseOnlinePlaySessionEv", nop},
        {"_ZN2nn7friends12UserPresence6CommitEv", ok},
        {"_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE", ok},
        {"_ZN2nn7friends17NotificationQueue8FinalizeEv", ok},
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", zero_count},
        {"_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii", zero_count},
        {"_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", ok},
        {"_ZN2nn7friends21StartFriendInvitationERKNS_7account3UidEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", ok},
        {"_ZN2nn7friends28StartSendingFriendInvitationERKNS_7account3UidEPKNS1_23NetworkServiceAccountIdEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", ok},
        {"_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE", ok},
        {"_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE", ok},
        {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", ok},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", ok},
        {"_ZN2nn7friends36GetFriendInvitationNotificationEventEv", friend_invitation_event},
        {"_ZN2nn7friends38TryPopFriendInvitationNotificationInfoEPNS_7account3UidEPmPvm", friend_invitation_try_pop},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", ok},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", ok},
        {"_ZN2nn3ngc15ProfanityFilter8FinalizeEv", nop},
        {"_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", nop},
        {"_ZN2nn3ngc15ProfanityFilter15SkipAtSignCheckENS1_8SkipModeE", nop},
        {"_ZN2nn3ngc15ProfanityFilter22SetNormalizationOptionEi", nop},
        {"_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", ngc_content_version},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", ngc_mask_profanity},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", ngc_mask_profanity},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", ngc_mask_profanity},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", ngc_check_profanity},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", ngc_check_profanity},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKtm", ngc_check_profanity},
        {"_ZN2nn3ldn10InitializeEv", ok}, {"_ZN2nn3ldn8FinalizeEv", ok},
        {"_ZN2nn3ldn8GetStateEv", ok}, {"_ZN2nn3ldn10DisconnectEv", ok},
        {"_ZN2nn3ldn11OpenStationEv", ldn_unavailable}, {"_ZN2nn3ldn12CloseStationEv", ok},
        {"_ZN2nn3ldn15OpenAccessPointEv", ldn_unavailable}, {"_ZN2nn3ldn16CloseAccessPointEv", ok},
        {"_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_unavailable},
        {"_ZN2nn3ldn14DestroyNetworkEv", ok},
        {"_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_unavailable},
        {"_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_unavailable},
        {"_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoEPNS0_16NodeLatestUpdateEi", ldn_unavailable},
        {"_ZN2nn3ldn15MakeIpv4AddressEhhhh", ldn_make_ipv4},
        {"_ZN2nn3ldn16SetAdvertiseDataEPKvm", ok},
        {"_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_unavailable},
        {"_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_unavailable},
        {"_ZN2nn4pctl32CheckFreeCommunicationPermissionEb", ok},
        {"_ZN2nn4pctl25TryBeginFreeCommunicationEb", try_begin_free_comm},
        {"_ZN2nn5audio12ListAudioInsEPNS0_11AudioInInfoEi", list_audio_ins},
        {"_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", audio_active_device_name},
        {"_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", audio_active_device_name},
        {"_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", audio_device_notification},
        {"_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", audio_active_channel_count},
        {"_ZN2nn3aoc31GetAddOnContentListChangedEventEPNS_2os11SystemEventE", aoc_list_changed_event},
        {"_ZN2nn2ec33InitializeForShopServiceAccessorsEPvm", ok},
        {"_ZN2nn2ec31FinalizeForShopServiceAccessorsEv", ok},
        {"_ZN2nn2ec19ShopServiceAccessor10InitializeERKNS0_11ShopServiceE", ok},
        {"_ZN2nn2ec19ShopServiceAccessor7RequestEPNS1_13AsyncResponseERKNS_7account3UidENS0_11ShopService6MethodEPKcRKNS1_8PostDataENS_8TimeSpanE", shop_accessor_request},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse3GetEPvm", shop_accessor_request},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse6CancelEv", nop},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7GetSizeEPm", shop_accessor_request},
        {"_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7TryWaitEv", network_available},
        {"_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", show_shop_aoc},
        {"_ZN2nn2ec24ShowShopSubscriptionListENS_13ApplicationIdERKNS_7account10UserHandleE", show_shop_aoc},
        {"_ZN2nn2ec26ShowShopConsumableItemListENS_13ApplicationIdERKNS_7account10UserHandleE", show_shop_aoc},
        {"_ZN2nn2ec27ShowShopSubscriptionDetailsERKNS0_8CourseIdENS0_5NsUidERKNS_7account10UserHandleE", show_shop_aoc},
        {"_ZN2nn2ec28ShowShopConsumableItemDetailENS_13ApplicationIdERKNS0_12ConsumableIdENS0_5NsUidERKNS_7account10UserHandleE", show_shop_aoc},
        {"_ZN2nn2ec30ShowShopApplicationInformationENS_13ApplicationIdERKNS_7account10UserHandleE", show_shop_aoc},
        {"_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", show_web_page},
        {"_ZN2nn3web18RequestExitWebPageEv", ok},
        {"_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", show_web_page},
        {"_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ok},
        {"_ZN2nn3ssl7Context7DestroyEv", ok},
        {"_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ssl_import_pki},
        {"_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", ssl_import_pki},
        {"_ZN2nn3ssl7Context9ImportCrlEPmPKcj", ssl_import_pki},
        {"_ZN2nn3ssl10Connection6CreateEPNS0_7ContextE", ssl_unavailable},
        {"_ZN2nn3ssl10Connection7DestroyEv", ok},
        {"_ZN2nn3ssl10Connection11DoHandshakeEv", ssl_unavailable},
        {"_ZN2nn3nsd7ResolveEPNS0_4FqdnERKS1_", nsd_resolve_ex},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", nsd_resolve_ex},
        {"_ZN2nn4time26StandardNetworkSystemClock3nowEv", network_clock_now},
        {"_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", show_error_code},
        {"_ZN2nn3err20ShowApplicationErrorERKNS0_19ApplicationErrorArgE", show_app_error},
        {"_ZN2nn3err21ReportMultimediaErrorENS_6ResultERKN5movie25MultimediaTelemetryReportE", nop},
        {"_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", environment_identifier},
        {"_ZN2nn5prepo10InitializeEv", nop},
        {"_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", nop},
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
    setenv("TOTK_USNAP_INDEX", "1", 0);
    setenv("TOTK_SHADOW_INDEX", "1", 0);
    setenv("TOTK_ARG_BIND_CACHE", "1", 0);
    setenv("TOTK_POOL_QUERY_CACHE", "1", 0);
    runtime_ipc_register("csrng", csrng_handler, NULL, NULL);
    runtime_ipc_register("set", settings_handler, NULL, NULL);
    runtime_ipc_register("bsd:u", bsd_handler, NULL, NULL);
    runtime_ipc_register("bsd:s", bsd_handler, NULL, NULL);
}
