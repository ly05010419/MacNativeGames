/* Host side of the services a game reaches only when it has a network or a console identity: the host is an offline
   console. Network requests are never accepted, so a game keeps its offline paths (no sockets, friends or local wireless
   follow). Super Mario Bros. Wonder imports them; the TOTK host this folder is forked from has none. */
#include "runtime.h"
#include "game_version.h"
#include "ipc.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CommonCrypto/CommonDigest.h>

/* Failures in each service's own module; games test them for success only. */
#define R_NIFM_UNAVAILABLE (110u | (300u << 9))
#define R_FRIENDS_UNAVAILABLE (121u | (6u << 9))
#define R_LDN_UNAVAILABLE (203u | (32u << 9))
#define R_SSL_UNAVAILABLE (123u | (204u << 9))
#define R_NSD_UNAVAILABLE (141u | (2u << 9))

/* ---------- nn::oe ---------- */
/* The host never asks the game to exit, so a section that defers exit requests has nothing to defer. */
static void enter_exit_section(void) { }
static void leave_exit_section(void) { }
/* Switch 2 (Mario Kart World): the system's graphics scale setting, an enum the game stores as an int; 0 (the default)
   and never a change to notify. A server-maintenance overlay has no system UI to show it on a Mac. */
static int graphics_scale(void) { return 0; }
static void graphics_scale_notification(unsigned char enabled) { (void)enabled; }
static void maintenance_overlay(int64_t from, int64_t to) { (void)from; (void)to; }
static void display_version(char *out) { memset(out, 0, 16); strncpy(out, TOTK_GAME_VERSION, 15); }
/* Stable for this Mac and this game, like the console's per-application pseudo device id. */
static void pseudo_device_id(uint8_t *uuid) {
    uuid_t host; struct timespec wait = {0, 0}; uint8_t digest[CC_SHA256_DIGEST_LENGTH]; uint64_t title = GAME_TITLE_ID;
    if (gethostuuid(host, &wait)) memset(host, 0, sizeof host);
    CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, host, sizeof host); CC_SHA256_Update(&c, &title, sizeof title); CC_SHA256_Final(digest, &c);
    memcpy(uuid, digest, 16);
}

/* ---------- nn::nifm ---------- */
static uint32_t nifm_initialize(void) { return 0; }
static unsigned char nifm_available(void) { return 0; }
static void nifm_local_mode(unsigned char on) { (void)on; }
static void nifm_submit_and_wait(void) { fprintf(stderr, "[totk] network request: refused (offline host)\n"); }
static void nifm_cancel(void) { }
static uint32_t nifm_unavailable(void) { return R_NIFM_UNAVAILABLE; }

/* ---------- nn::friends: no friends and no play history on an offline host ---------- */
static void friends_initialize(void) { }
static void friends_allocator(void *a, void *f) { (void)a; (void)f; }
static uint32_t friends_check(unsigned char *available, const void *uid) { (void)uid; if (available) *available = 0; return 0; }
static uint32_t friends_unavailable(void) { return R_FRIENDS_UNAVAILABLE; }
static uint32_t friends_list(int *count, void *out, const void *uid, int offset, int max, const void *filter) {
    (void)out; (void)uid; (void)offset; (void)max; (void)filter; if (count) *count = 0; return 0;
}
static uint32_t friends_add_history(void) { return 0; }

/* ---------- nn::ldn (local wireless): there is no radio, so no station or access point opens ---------- */
static uint32_t ldn_unavailable(void) { return R_LDN_UNAVAILABLE; }
static void ldn_finalize(void) { }
static int ldn_state(void) { return 0; }   /* State_None */
static void ldn_attach_event(void *event) { if (!event) runtime_fail("ldn state event"); hle_bind_system_event(event, 0); }

/* ---------- nn::ssl, nn::nsd, nn::ngc ---------- */
static uint32_t ssl_initialize(void) { return 0; }
static uint32_t ssl_unavailable(void) { return R_SSL_UNAVAILABLE; }
static uint32_t nsd_environment(char *out) { memset(out, 0, 8); memcpy(out, "lp1", 3); return 0; }
static uint32_t ngc_initialize(void *self, void *work, uint64_t size, unsigned char check) { (void)self; (void)work; (void)size; (void)check; return 0; }
/* Nothing is filtered: every word is reported clean. */
static uint32_t ngc_check(uint32_t *masks, uint32_t count, const void **words, uint64_t n) {
    (void)words; (void)n; if (masks) memset(masks, 0, count * sizeof *masks); return 0;
}

static uint32_t nsd_unavailable(void) { return R_NSD_UNAVAILABLE; }

/* ---------- nn::socket: the library starts, but there is no network, so every socket call fails with ENETDOWN ---------- */
#define SOCKET_ENETDOWN 50   /* Horizon uses the BSD errno values */
#define SOCKET_EAI_FAIL 4
static _Thread_local int socket_errno;
static uint32_t socket_initialize(void) { return 0; }
static int socket_down(void) { socket_errno = SOCKET_ENETDOWN; return -1; }
static int socket_last_error(void) { return socket_errno; }
static int socket_addr_info(const char *node, const char *service, const void *hints, void **out) {
    (void)node; (void)service; (void)hints; if (out) *out = NULL; return SOCKET_EAI_FAIL;
}
static void socket_free_addr_info(void *info) { (void)info; }
static uint32_t socket_htonl(uint32_t v) { return htonl(v); }
static uint16_t socket_htons(uint16_t v) { return htons(v); }
static const char *socket_ntop(int family, const void *in, char *out, uint32_t size) { return family == 2 ? inet_ntop(AF_INET, in, out, size) : NULL; }

/* ---------- nn::crypto ---------- */
static void random_bytes(void *out, uint64_t n) { arc4random_buf(out, (size_t)n); }

/* ---------- play reports: nothing is reported from an offline host ---------- */
static uint32_t ok_result(void) { return 0; }
static void nothing(void) { }

/* ---------- nn::settings ---------- */
/* The SDK builds the language's code through the settings service; the table is the system's fixed one. */
static unsigned char language_is(const int *language, const char *code) {
    static const char *const codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                                        "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    if (!language || !code || *language < 0 || *language >= (int)(sizeof codes / sizeof *codes)) return 0;
    return !strncmp(codes[*language], code, 8);
}

/* ---------- nn::chat (Switch 2 GameChat offline host) ---------- */
static pthread_mutex_t chat_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { void *mem; size_t size; uint32_t w, h; int used; } chat_images[128];
static struct { const void *storage; uint32_t base_handle; int count; int used; } chat_storages[32];

int runtime_chat_native_image_info(uint32_t handle, void **mem, size_t *size, uint32_t *w, uint32_t *h) {
    if (handle == 0 || handle > 128) return 0;
    pthread_mutex_lock(&chat_lock);
    uint32_t idx = handle - 1u;
    int ok = chat_images[idx].used;
    if (ok) {
        if (mem) *mem = chat_images[idx].mem;
        if (size) *size = chat_images[idx].size;
        if (w) *w = chat_images[idx].w;
        if (h) *h = chat_images[idx].h;
    }
    pthread_mutex_unlock(&chat_lock);
    return ok;
}

static size_t chat_cam_per_image(int w, int h) {
    size_t wa = ((size_t)(w > 0 ? w : 384) + 63u) & ~63u;
    size_t ha = ((size_t)(h > 0 ? h : 216) + 127u) & ~127u;
    return ((wa * ha * 4u) + 0xffffu) & ~0xffffu;
}
static size_t chat_cam_buf_size(int w, int h, int count) {
    return chat_cam_per_image(w, h) * (size_t)(count > 0 ? count : 1);
}
static uint32_t chat_cam_init(void *storage, int w, int h, int count, void *buf, size_t size) {
    int n = count > 0 ? count : 1;
    size_t per = chat_cam_per_image(w, h);
    if (size && per > size / (size_t)n) per = (size / (size_t)n) & ~0xfffu;
    if (buf && size) memset(buf, 0, size);
    pthread_mutex_lock(&chat_lock);
    uint32_t base = 0;
    for (int start = 0; start <= 128 - n; start++) {
        int free_run = 1;
        for (int k = 0; k < n; k++) {
            if (chat_images[start + k].used) { free_run = 0; break; }
        }
        if (free_run) {
            base = (uint32_t)(start + 1);
            for (int k = 0; k < n; k++) {
                chat_images[start + k].mem = buf ? (uint8_t *)buf + (size_t)k * per : NULL;
                chat_images[start + k].size = per;
                chat_images[start + k].w = (uint32_t)(w > 0 ? w : 384);
                chat_images[start + k].h = (uint32_t)(h > 0 ? h : 216);
                chat_images[start + k].used = 1;
            }
            break;
        }
    }
    if (base) {
        int slot = -1;
        for (int i = 0; i < 32; i++) {
            if (chat_storages[i].used && chat_storages[i].storage == storage) { slot = i; break; }
            if (slot < 0 && !chat_storages[i].used) slot = i;
        }
        if (slot >= 0) {
            chat_storages[slot].storage = storage;
            chat_storages[slot].base_handle = base;
            chat_storages[slot].count = n;
            chat_storages[slot].used = 1;
        }
    }
    pthread_mutex_unlock(&chat_lock);
    return 0;
}
static void chat_cam_fini(void *storage) {
    pthread_mutex_lock(&chat_lock);
    for (int i = 0; i < 32; i++) {
        if (chat_storages[i].used && chat_storages[i].storage == storage) {
            uint32_t base = chat_storages[i].base_handle;
            int n = chat_storages[i].count;
            for (int k = 0; k < n; k++) {
                uint32_t idx = base - 1u + (uint32_t)k;
                if (idx < 128) memset(&chat_images[idx], 0, sizeof chat_images[idx]);
            }
            memset(&chat_storages[i], 0, sizeof chat_storages[i]);
            break;
        }
    }
    pthread_mutex_unlock(&chat_lock);
}
static uint32_t chat_cam_handle(const void *storage, int index) {
    uint32_t out = 0;
    pthread_mutex_lock(&chat_lock);
    for (int i = 0; i < 32; i++) {
        if (chat_storages[i].used && chat_storages[i].storage == storage) {
            if (index >= 0 && index < chat_storages[i].count)
                out = chat_storages[i].base_handle + (uint32_t)index;
            break;
        }
    }
    pthread_mutex_unlock(&chat_lock);
    return out;
}
static void chat_init(void) {}
static int chat_is_chatting(void) { return 0; }
static uint32_t chat_fail(void) { return 1u; }
static uint32_t chat_ok(void) { return 0u; }
static uint32_t chat_attach_room_event(void *ev) { if (ev) hle_bind_system_event(ev, 0); return 0u; }
static void chat_noop(void) {}

static void camera_ctx_ipc(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    switch (rq->cmd) {
    case 3:
        rs->result = 0xdc4u;   /* OpenCamera("Default"): no USB camera connected (main+0xcded84 accepts 0xdc4) */
        return;
    case 21: {                 /* GetCameraAvailabilityEvent (main+0xcdc5b0): never signaled */
        static uint32_t ev;
        if (!ev) ev = runtime_event_new();
        rs->copy[rs->ncopy++] = runtime_event_readable(ev);
        rs->result = 0u;
        return;
    }
    default: {
        static uint64_t seen;
        if (rq->cmd >= 64 || !(seen >> rq->cmd & 1u)) {
            if (rq->cmd < 64) seen |= 1ull << rq->cmd;
            fprintf(stderr, "[totk-ipc] camera:ctx cmd %u -> 0xdc4\n", rq->cmd);
        }
        rs->result = 0xdc4u;
        return;
    }
    }
}
static uint32_t camera_create_context(uint32_t *out_handle) {
    if (!out_handle) return 1u;
    *out_handle = runtime_ipc_new_session("camera:ctx", camera_ctx_ipc, NULL, NULL);
    return 0u;
}

void *runtime_offline_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", enter_exit_section},
        {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", leave_exit_section},
        {"_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", display_version},
        {"_ZN2nn2oe23GetCurrentGraphicsScaleEv", graphics_scale},
        {"_ZN2nn2oe42SetGraphicsScaleChangedNotificationEnabledEb", graphics_scale_notification},
        {"_ZN2nn2oe40SendServerMaintenanceOverlayNotificationENS_4time9PosixTimeES2_", maintenance_overlay},
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", pseudo_device_id}, {"_ZN2nn4nifm10InitializeEv", nifm_initialize},
        {"_ZN2nn4nifm18IsNetworkAvailableEv", nifm_available}, {"_ZN2nn4nifm19SetLocalNetworkModeEb", nifm_local_mode},
        {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_submit_and_wait},
        {"_ZN2nn4nifm20CancelNetworkRequestEv", nifm_cancel},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_unavailable},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", nsd_unavailable}, {"_ZN2nn6socket10InitializeEPvmmi", socket_initialize},
        {"_ZN2nn6socket12GetLastErrorEv", socket_last_error},
        {"_ZN2nn6socket11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_", socket_addr_info},
        {"_ZN2nn6socket12FreeAddrInfoEPNS0_8AddrInfoE", socket_free_addr_info}, {"_ZN2nn6socket9InetHtonlEj", socket_htonl},
        {"_ZN2nn6socket9InetHtonsEt", socket_htons},
        {"_ZN2nn6socket6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE", socket_down},
        {"_ZN2nn6socket4BindEiPKNS0_8SockAddrEj", socket_down},
        {"_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", socket_down},
        {"_ZN2nn6socket8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj", socket_down},
        {"_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", socket_down}, {"_ZN2nn6socket5CloseEi", socket_down},
        {"_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", socket_down}, {"_ZN2nn6socket11EventFdReadEiPm", socket_down},
        {"_ZN2nn6socket12EventFdWriteEim", socket_down},
        {"_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", random_bytes},
        {"_ZN2nn5prepo10PlayReportC1Ev", nothing}, {"_ZN2nn5prepo10PlayReport10SetEventIdEPKc", ok_result},
        {"_ZN2nn5prepo10PlayReport9SetBufferEPvm", ok_result}, {"_ZN2nn5prepo10PlayReport3AddEPKcl", ok_result},
        {"_ZN2nn5prepo10PlayReport3AddEPKcS3_", ok_result}, {"_ZN2nn6socket6SocketEiii", socket_down},
        {"_ZN2nn6socket4BindEiPK8sockaddrj", socket_down},
        {"_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", socket_addr_info},
        {"_ZN2nn6socket12FreeAddrInfoEP8addrinfo", socket_free_addr_info}, {"_ZN2nn6socket8InetNtopEiPKvPcj", socket_ntop},
        {"_ZN2nn6socket9InetNtohsEt", socket_htons}, {"_ZN2nn7friends10InitializeEv", friends_initialize},
        {"_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", friends_allocator},
        {"_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", friends_check},
        {"_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", friends_unavailable},
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", friends_list},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", friends_unavailable},
        {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", friends_add_history},
        {"_ZN2nn3ldn10InitializeEv", ldn_unavailable}, {"_ZN2nn3ldn8FinalizeEv", ldn_finalize},
        {"_ZN2nn3ldn8GetStateEv", ldn_state},
        {"_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ldn_attach_event},
        {"_ZN2nn3ldn15OpenAccessPointEv", ldn_unavailable}, {"_ZN2nn3ldn11OpenStationEv", ldn_unavailable},
        {"_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ldn_unavailable},
        {"_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ldn_unavailable},
        {"_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_unavailable},
        {"_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ldn_unavailable},
        {"_ZN2nn3ssl10InitializeEv", ssl_initialize}, {"_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ssl_unavailable},
        {"_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", nsd_environment},
        {"_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", language_is},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", ngc_initialize},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", ngc_check},
        {"_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", ngc_check},
        {"_ZN2nn4chat10InitializeEv", chat_init},
        {"_ZN2nn4chat10IsChattingEv", chat_is_chatting},
        {"_ZN2nn4chat11GetRoomInfoEPNS0_8RoomInfoE", chat_fail},
        {"_ZN2nn4chat13SetSharingKeyERKNS0_10SharingKeyE", chat_ok},
        {"_ZN2nn4chat13ShowChatStartERKNS_7account3UidE", chat_fail},
        {"_ZN2nn4chat15ClearSharingKeyEv", chat_ok},
        {"_ZN2nn4chat20SetScreenSharingModeENS0_17ScreenSharingModeE", chat_ok},
        {"_ZN2nn4chat22ShowCameraSharingGuideEv", chat_fail},
        {"_ZN2nn4chat24StopLocalCameraStreamingEv", chat_ok},
        {"_ZN2nn4chat25GetLocalCameraImageHandleEPKNS0_20LocalCameraVideoTypeEi", chat_cam_handle},
        {"_ZN2nn4chat25StartLocalCameraStreamingEv", chat_fail},
        {"_ZN2nn4chat25StopRemoteCameraStreamingERKNS_7account23NetworkServiceAccountIdE", chat_ok},
        {"_ZN2nn4chat26GetRemoteCameraImageHandleEPKNS0_21RemoteCameraVideoTypeEi", chat_cam_handle},
        {"_ZN2nn4chat26StartRemoteCameraStreamingERKNS_7account23NetworkServiceAccountIdE", chat_fail},
        {"_ZN2nn4chat27RequestNextLocalCameraImageEPNS_2gs16NativeSyncHandleEPNS0_20LocalCameraVideoTypeEiS2_", chat_fail},
        {"_ZN2nn4chat28AttachToRoomInfoChangedEventEPNS_2os11SystemEventE", chat_attach_room_event},
        {"_ZN2nn4chat28RequestNextRemoteCameraImageEPNS_2gs16NativeSyncHandleEPNS0_21RemoteCameraVideoTypeEiS2_RKNS_7account23NetworkServiceAccountIdE", chat_fail},
        {"_ZN2nn4chat31FinalizeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeE", chat_cam_fini},
        {"_ZN2nn4chat32FinalizeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeE", chat_cam_fini},
        {"_ZN2nn4chat33InitializeLocalCameraVideoStorageEPNS0_20LocalCameraVideoTypeEiiiPvm", chat_cam_init},
        {"_ZN2nn4chat34InitializeRemoteCameraVideoStorageEPNS0_21RemoteCameraVideoTypeEiiiPvm", chat_cam_init},
        {"_ZN2nn4chat35CalculateLocalCameraVideoBufferSizeEiii", chat_cam_buf_size},
        {"_ZN2nn4chat36CalculateRemoteCameraVideoBufferSizeEiii", chat_cam_buf_size},
        {"_ZN2nn4chat36ShowLocalCameraConfigurationVersion0ENS0_25CameraImageProcessingModeE", chat_fail},
        {"_ZN2nn4chat44EndFullScreenWithLocalCameraAvailableSectionEv", chat_noop},
        {"_ZN2nn4chat46BeginFullScreenWithLocalCameraAvailableSectionEb", chat_noop},
        {"_ZN2nn6camera3sdk19CreateCameraContextEPNS_5gsdrv18UniqueNativeHandleE", camera_create_context},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
