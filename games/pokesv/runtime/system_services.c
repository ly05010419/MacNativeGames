/* Horizon services Bayonetta 3 reaches that the earlier port never did, answered in-process over IPC (ipc.c). Command numbers and
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
        if (language >= LANGUAGES) { fprintf(stderr, "[psv-set] MakeLanguageCode(%u)\n", language); runtime_fail("set MakeLanguageCode: unknown language"); }
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
        fprintf(stderr, "[psv-net] socket(%u, %u, %u) refused: offline\n", IPC_ARG(rq, uint32_t, 0), IPC_ARG(rq, uint32_t, 4), IPC_ARG(rq, uint32_t, 8));
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
/* BCAT delivery cache (news and Mystery Gift, Pokémon Violet): there is no delivery data offline, so the storage does not
   mount and a sync request fails at once, as on a console that never went online (after games/acnh, third_party/COPIES.md). */
#define RES_FAIL 0x2A2u
static uint32_t fail_res(void) { return RES_FAIL; }
static uint32_t fail_count(int *count, ...) { if (count) *count = 0; return RES_FAIL; }
static int64_t zero_size(void) { return 0; }
static uint32_t empty_delivery_directories(int *count,void *names,int capacity){
    (void)names; (void)capacity;
    if(!count)runtime_fail("delivery-cache directory count pointer");
    *count=0; return 0;
}
/* nn::crypto::GenerateCryptographicallyRandomBytes (Pokémon Violet) opens the csrng service; the host's entropy answers it. */
static void random_bytes(void *out, size_t size) { if (size && !out) runtime_fail("random bytes output"); arc4random_buf(out, size); }
/* Bluetooth LE (Pokémon Violet: Poké Ball Plus and GO Plus +): the Mac has no such accessory, so a scan finds nothing,
   its events never fire and every connection attempt fails. Objects (GattService and the like) stay the SDK's own code. */
static uint32_t ble_event(void *event) { if (!event) runtime_fail("bluetooth event pointer"); hle_bind_system_event(event, 0); return 0; }
static int no_ble_entries(void *out, int max) { (void)out; (void)max; return 0; }
static uint32_t ble_scan_parameter(uint8_t *uuid, int which) { (void)which; if (!uuid) runtime_fail("bluetooth scan parameter"); memset(uuid, 0, 0x14); uuid[0] = 0x10; return 0; }
static void nifm_request(void) { static int once; if (!once++) fprintf(stderr, "[psv-net] network request: offline\n"); }
static int64_t network_clock_now(void) { return (int64_t)time(NULL); }   /* time_point: seconds since the epoch */
static void environment_identifier(char *out) { if (!out) runtime_fail("nsd environment output"); memset(out, 0, 8); memcpy(out, "lp1", 3); }
static void execute_program(int index, const void *param, uint64_t size) {
    (void)param; fprintf(stderr, "[psv] nn::oe::ExecuteProgram(%d, %llu bytes)\n", index, (unsigned long long)size);
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
    const char *dir = getenv("PSV_PROFILE_DIR"); if (!dir) dir = "user-data";
    char path[1100]; snprintf(path, sizeof path, "%s/pseudo-device-id", dir);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) { ssize_t n = read(fd, out, 16); close(fd); if (n == 16) return; }
    arc4random_buf(out, 16); out[6] = (out[6] & 0x0f) | 0x40; out[8] = (out[8] & 0x3f) | 0x80;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, out, 16) != 16 || close(fd)) runtime_fail("store the pseudo device id in the profile folder");
}
static uint32_t latest_frame_number(uint64_t *out, const void *layer) { (void)layer; if (!out) runtime_fail("GetLatestFrameNumber output"); *out = mtl_present_count(); return 0; }
static int32_t list_frame_history(void *out, int32_t count, const void *layer) { (void)out; (void)count; (void)layer; return 0; }
/* nn::ldn, local wireless (2026-11-03): Violet opened ldn:u from the overworld (the Poké Portal's local play) and the host,
   which has no ldn:u, stopped the game ("IPC service not implemented"). As in the Smash port, it answers as a Switch whose
   wireless communication is off, so the SDK client never opens the service: Initialize fails with 2203-0022 (module 203 Ldn,
   description 22), the state stays None, Finalize has nothing to close, and the state change event never fires. The other
   calls answer "unavailable" in case the game makes them anyway. */
#define LDN_WIRELESS_OFF RESULT(203, 22)
static uint32_t ldn_initialize(void) {
    static int once; if (!once++) fprintf(stderr, "[psv] local wireless: off (ldn Initialize answered 2203-0022)\n");
    return LDN_WIRELESS_OFF;
}
static void ldn_attach_state_event(void *event) { if (!event) runtime_fail("ldn state change event"); hle_bind_system_event(event, 0); }
static int32_t ldn_state(void) { return 0; }
static uint32_t ldn_unavailable(void) { return LDN_UNAVAILABLE; }
static uint32_t nsd_unavailable(void) { return NSD_UNAVAILABLE; }
/* nn::ec::ShowShopAddOnContentList: the game's add-on content entry opens the eShop library applet and blocks until the
   player closes it. Violet calls the overload with only the user (main+0x820210). There is no eShop here and the host runs
   no library applets (the SDK read a null self controller at sdk+0x3a300, owner 2026-10-30), so the shop opens and closes
   at once: the game returns to its menu, and aoc still reports no add-on. */
static void shop_closed(uint64_t application_id, const void *user) { (void)user; fprintf(stderr, "[psv] eShop add-on list for %016llx: no shop on this Mac, closed at once\n", (unsigned long long)application_id); }
static void shop_closed_for_user(const void *user) { (void)user; fprintf(stderr, "[psv] eShop add-on list: no shop on this Mac, closed at once\n"); }
void *runtime_game_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2oe13MuteRecordingEv", nop}, {"_ZN2nn2oe15UnmuteRecordingEv", nop},
        {"_ZN2nn2oe15EnableRecordingEv", nop}, {"_ZN2nn2oe16DisableRecordingEv", nop},
        {"_ZN2nn2oe14ExecuteProgramEiPKvm", execute_program},
        {"_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", shop_closed},
        {"_ZN2nn2ec24ShowShopAddOnContentListERKNS_7account10UserHandleE", shop_closed_for_user},
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
        {"_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", random_bytes},
        {"_ZN2nn9bluetooth13InitializeBleEv", nop},
        {"_ZN2nn9bluetooth19AcquireBleScanEventEPNS_2os15SystemEventTypeE", ble_event},
        {"_ZN2nn9bluetooth24AcquireBleMtuConfigEventEPNS_2os15SystemEventTypeE", ble_event},
        {"_ZN2nn9bluetooth28AcquireBleGattOperationEventEPNS_2os15SystemEventTypeE", ble_event},
        {"_ZN2nn9bluetooth31AcquireBleServiceDiscoveryEventEPNS_2os15SystemEventTypeE", ble_event},
        {"_ZN2nn9bluetooth37AcquireBleConnectionStateChangedEventEPNS_2os15SystemEventTypeE", ble_event},
        {"_ZN2nn9bluetooth23StartBleScanSmartDeviceERKNS0_17GattAttributeUuidE", ok}, {"_ZN2nn9bluetooth22StopBleScanSmartDeviceEv", ok},
        {"_ZN2nn9bluetooth16GetBleScanResultEPNS0_13BleScanResultEi", no_ble_entries},
        {"_ZN2nn9bluetooth24GetBleConnectionInfoListEPNS0_17BleConnectionInfoEi", no_ble_entries},
        {"_ZN2nn9bluetooth19GetBleScanParameterEPNS0_17GattAttributeUuidEi", ble_scan_parameter},
        {"_ZN2nn9bluetooth19ConnectToGattServerERKNS0_7AddressE", fail_res}, {"_ZN2nn9bluetooth24DisconnectFromGattServerEj", fail_res},
        {"_ZN2nn9bluetooth15ConfigureBleMtuEjt", fail_res}, {"_ZN2nn9bluetooth14GetGattServiceEPNS0_11GattServiceERKNS0_17GattAttributeUuidEj", ok},
        {"_ZN2nn9bluetooth22GetGattOperationResultEPNS0_26BleClientGattOperationInfoE", fail_res},
        {"_ZN2nn9bluetooth22ReadGattCharacteristicERKNS0_18GattCharacteristicE", fail_res},
        {"_ZN2nn9bluetooth23WriteGattCharacteristicERKNS0_18GattCharacteristicE", fail_res},
        {"_ZN2nn9bluetooth19WriteGattDescriptorERKNS0_14GattDescriptorE", fail_res},
        {"_ZN2nn9bluetooth36EnableGattCharacteristicNotificationERKNS0_18GattCharacteristicEb", fail_res},
        {"_ZN2nn9bluetooth33RegisterGattOperationNotificationERKNS0_17GattAttributeUuidE", fail_res},
        {"_ZN2nn9bluetooth35UnregisterGattOperationNotificationERKNS0_17GattAttributeUuidE", fail_res},
        {"_ZN2nn4bcat10InitializeEv", nop}, {"_ZN2nn4bcat25MountDeliveryCacheStorageEv", fail_res},
        {"_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", nop},
        {"_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi", empty_delivery_directories},
        {"_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc", fail_res}, {"_ZN2nn4bcat17DeliveryCacheFile4ReadEPmlPvm", fail_res},
        {"_ZN2nn4bcat17DeliveryCacheFile5CloseEv", nop}, {"_ZN2nn4bcat17DeliveryCacheFile7GetSizeEv", zero_size},
        {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenEPKc", fail_res},
        {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", fail_res},
        {"_ZN2nn4bcat22DeliveryCacheDirectory4ReadEPiPNS0_27DeliveryCacheDirectoryEntryEi", fail_count},
        {"_ZN2nn4bcat22DeliveryCacheDirectory5CloseEv", nop},
        {"_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", fail_res},
        {"_ZN2nn4bcat21DeliveryCacheProgress6UpdateEv", nop},
        {"_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", nop},
        {"_ZN2nn7account13PushOpenUsersEv", push_open_users},
        {"_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi", pop_open_users},
        {"_ZN2nn2oe16EnableScreenShotEv", nop}, {"_ZN2nn2oe17DisableScreenShotEv", nop},
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", nop}, {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", nop},
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", pseudo_device_id},
        {"_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE", latest_frame_number},
        {"_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE", list_frame_history},
        {"_ZN2nn3ldn10InitializeEv", ldn_initialize}, {"_ZN2nn3ldn8FinalizeEv", ok},
        {"_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_attach_state_event},
        {"_ZN2nn3ldn11OpenStationEv", ldn_unavailable},
        {"_ZN2nn3ldn15OpenAccessPointEv", ldn_unavailable},
        {"_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_unavailable},
        {"_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_unavailable},
        {"_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_unavailable},
        {"_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", ldn_unavailable},
        {"_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_unavailable},
        {"_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_unavailable},
        {"_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_unavailable},
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
    setenv("PSV_UNCAPPED_INTERVAL", "1", 0);
    /* It draws a depth pre-pass, then the colour pass with a LEQUAL test: positions must match bit for bit between the two
       (nvn_mtl.m psv_precise_opts, preserveInvariance). */
    setenv("DM_PRESERVE_INVARIANCE", "1", 0);
    /* Its shader binaries mix GLSLC GPU code versions (1.15 and 1.16 seen first); the graphics layer's measured driver
       table stops at 1.14. The layer reads only three control-section fields (code length, stage, compute group size)
       and stops on implausible values, so a newer layout would show at once. */
    setenv("DM_NVN_GPU_CODE_MAX_MINOR", "31", 0);
    /* GPU paths the graphics layer keeps isolated until their standalone probe passes (nvn_mtl.m gpu_path_on); each was
       probed on this host before it was switched on here (an explicit value in the environment wins):
         PSV_GPU_MIDCLEAR  probe-gpu-midclear 28.10. (Apple M5): 9,216 checks pass; Xenoblade Chronicles 3 clears with a
                            scissor or a partial mask inside a pass at its first event after the difficulty selection. */
    setenv("PSV_GPU_MIDCLEAR", "1", 0);
    runtime_ipc_register("set", settings_handler, NULL, NULL);
    runtime_ipc_register("bsd:u", bsd_handler, NULL, NULL);
    runtime_ipc_register("bsd:s", bsd_handler, NULL, NULL);
}
