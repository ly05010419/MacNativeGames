/* Host side of the services a game reaches only when it has a network or a console identity: the host is an offline
   console. Network requests are never accepted, so a game keeps its offline paths (no sockets, friends or local wireless
   follow). From games/wonder (Super Mario Bros. Wonder imports them); here for the Switch 2 games (GAME_NS2: Donkey Kong
   Bananza initializes nifm at boot). TOTK and BotW import none of them and do not build this file. */
#include "runtime.h"
void hle_signal_system_event(void *event);
#include "game_version.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef GAME_NS2
#include <errno.h>
#include <limits.h>
#include <time.h>
#endif
#include <CommonCrypto/CommonDigest.h>
#include <TargetConditionals.h>

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
static void display_version(char *out) { memset(out, 0, 16); strncpy(out, TOTK_GAME_VERSION, 15); }
/* Stable for this installation and this game, like the console's per-application pseudo device id. */
static void pseudo_device_id(uint8_t *uuid) {
    uuid_t host; uint8_t digest[CC_SHA256_DIGEST_LENGTH]; uint64_t title = GAME_TITLE_ID;
#if TARGET_OS_IPHONE
    void runtime_ios_device_uuid(uint8_t out[16]);
    runtime_ios_device_uuid(host);
#else
    struct timespec wait = {0, 0};
    if (gethostuuid(host, &wait)) memset(host, 0, sizeof host);
#endif
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

/* ---------- nn::socket: no network; GAME_NS2 also supports local event counter fds ---------- */
#define SOCKET_ENETDOWN 50   /* Horizon uses the BSD errno values */
#define SOCKET_EAI_FAIL 4
static _Thread_local int socket_errno;
static uint32_t socket_initialize(void) { return 0; }
static int socket_down(void) { socket_errno = SOCKET_ENETDOWN; return -1; }
static int socket_last_error(void) { return socket_errno; }
#ifdef GAME_NS2
/* SDK 22 ABI: EventFdFlags semaphore=1, cloexec=2 (no exec here), nonblock=4;
   F_GETFL=3, F_SETFL=4, O_NONBLOCK=0x800. pollfd has an eight-byte stride.
   This SDK uses libc EAGAIN=11, not the older BSD errno 35. */
enum { LOCAL_FD_BASE = 0x4000, LOCAL_FD_COUNT = 64, SOCKET_EBADF = 9,
       SOCKET_EINVAL = 22, SOCKET_EMFILE = 24, SOCKET_EAGAIN = 11,
       SOCKET_NONBLOCK = 0x800, SOCKET_POLLIN = 1, SOCKET_POLLOUT = 4, SOCKET_POLLNVAL = 0x20 };
typedef struct { int fd; int16_t events, revents; } SocketPollFd;
_Static_assert(sizeof(SocketPollFd) == 8, "guest pollfd ABI");
typedef struct { int fd, flags, semaphore; uint64_t counter; } LocalEventFd;
static LocalEventFd local_event_fds[LOCAL_FD_COUNT];
static int local_next_fd = LOCAL_FD_BASE;
static pthread_mutex_t local_fd_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t local_fd_changed = PTHREAD_COND_INITIALIZER;
static uint64_t local_fd_version;
static void local_fd_notify(void) { ++local_fd_version; pthread_cond_broadcast(&local_fd_changed); }
static int socket_set_errno(int error) {
    socket_errno = error;
#ifdef GAME_OOT
    /* gRPC reads libc errno directly. Keep the existing offline BSD error 50
       in GetLastErrno, translating it for SDK 22 libc (ENETDOWN=100).
       The original accessor uses the calling guest's rewritten TLS. */
    int *(*guest_errno)(void) = runtime_symbol("__errno_location");
    if (guest_errno) *guest_errno() = error == SOCKET_ENETDOWN ? 100 : error;
#endif
    return 0;
}
static int socket_error(int error) { socket_set_errno(error); return -1; }
static LocalEventFd *local_fd_find(int fd) {
    for (unsigned i = 0; i < LOCAL_FD_COUNT; ++i)
        if (local_event_fds[i].fd == fd && fd >= LOCAL_FD_BASE) return &local_event_fds[i];
    return NULL;
}
static int local_fd_error(int fd) {
    return socket_error(fd >= LOCAL_FD_BASE && fd < local_next_fd ? SOCKET_EBADF : SOCKET_ENETDOWN);
}
/* Give up the virtual core without a host lock held; the version check closes
   the notification race during that handoff. Recheck the predicate on return. */
static int local_fd_wait(const struct timespec *deadline) {
    uint64_t version = local_fd_version;
    pthread_mutex_unlock(&local_fd_lock);
    int away = runtime_core_away();
    pthread_mutex_lock(&local_fd_lock);
    int result = 0;
    if (deadline) {
        struct timespec remaining;
        clock_gettime(CLOCK_MONOTONIC, &remaining);
        remaining.tv_sec = deadline->tv_sec - remaining.tv_sec;
        remaining.tv_nsec = deadline->tv_nsec - remaining.tv_nsec;
        if (remaining.tv_nsec < 0) { --remaining.tv_sec; remaining.tv_nsec += 1000000000; }
        if (remaining.tv_sec < 0) result = ETIMEDOUT;
        else if (version == local_fd_version)
            result = pthread_cond_timedwait_relative_np(&local_fd_changed, &local_fd_lock, &remaining);
    } else if (version == local_fd_version) result = pthread_cond_wait(&local_fd_changed, &local_fd_lock);
    pthread_mutex_unlock(&local_fd_lock);
    runtime_core_back(&away);
    pthread_mutex_lock(&local_fd_lock);
    return result;
}
static int socket_event_fd(uint64_t initial, int flags) {
    if ((flags & ~7) || initial == UINT64_MAX) return socket_error(SOCKET_EINVAL);
    pthread_mutex_lock(&local_fd_lock);
    int fd = -1;
    for (unsigned i = 0; i < LOCAL_FD_COUNT && local_next_fd < INT_MAX; ++i) {
        if (local_event_fds[i].fd) continue;
        fd = local_next_fd++;
        local_event_fds[i] = (LocalEventFd){fd, flags & 4 ? SOCKET_NONBLOCK : 0, flags & 1, initial};
        break;
    }
    pthread_mutex_unlock(&local_fd_lock);
    if (fd < 0) return socket_error(SOCKET_EMFILE);
    fprintf(stderr, "[totk] offline EventFd: fd=%d initial=%llu flags=%#x (local only)\n",
            fd, (unsigned long long)initial, flags);
    return fd;
}
static int socket_event_read(int fd, uint64_t *out) {
    if (!out) return socket_error(SOCKET_EINVAL);
    pthread_mutex_lock(&local_fd_lock);
    int result;
    for (;;) {
        LocalEventFd *event = local_fd_find(fd);
        if (!event) { result = local_fd_error(fd); break; }
        if (event->counter) {
            *out = event->semaphore ? 1 : event->counter;
            event->counter -= *out;
            local_fd_notify();
            result = 0; break;
        }
        if (event->flags & SOCKET_NONBLOCK) { result = socket_error(SOCKET_EAGAIN); break; }
        local_fd_wait(NULL);
    }
    pthread_mutex_unlock(&local_fd_lock);
    return result;
}
static int socket_event_write(int fd, uint64_t value) {
    if (value == UINT64_MAX) return socket_error(SOCKET_EINVAL);
    pthread_mutex_lock(&local_fd_lock);
    int result;
    for (;;) {
        LocalEventFd *event = local_fd_find(fd);
        if (!event) { result = local_fd_error(fd); break; }
        if (value <= UINT64_MAX - 1 - event->counter) {
            event->counter += value;
            local_fd_notify();
            result = 0; break;
        }
        if (event->flags & SOCKET_NONBLOCK) { result = socket_error(SOCKET_EAGAIN); break; }
        local_fd_wait(NULL);
    }
    pthread_mutex_unlock(&local_fd_lock);
    return result;
}
static int socket_close(int fd) {
    pthread_mutex_lock(&local_fd_lock);
    LocalEventFd *event = local_fd_find(fd);
    int result = event ? 0 : local_fd_error(fd);
    if (event) { memset(event, 0, sizeof *event); local_fd_notify(); }
    pthread_mutex_unlock(&local_fd_lock);
    return result;
}
/* Fixed x2 argument matches guest AAPCS64 varargs; Darwin varargs use the stack. */
static int socket_fcntl(int fd, int command, int flags) {
    pthread_mutex_lock(&local_fd_lock);
    LocalEventFd *event = local_fd_find(fd);
    int result;
    if (!event) result = local_fd_error(fd);
    else if (command == 3) result = event->flags;
    else if (command == 4) {
        event->flags = flags & SOCKET_NONBLOCK; result = 0;
        local_fd_notify();
    } else result = socket_error(SOCKET_EINVAL);
    pthread_mutex_unlock(&local_fd_lock);
    return result;
}
static int socket_poll(SocketPollFd *fds, uint64_t count, int timeout) {
    if ((!fds && count) || count > 0x1fffffff) return socket_error(SOCKET_EINVAL);
    struct timespec deadline;
    if (timeout > 0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout / 1000;
        deadline.tv_nsec += (long)(timeout % 1000) * 1000000;
        if (deadline.tv_nsec >= 1000000000) { ++deadline.tv_sec; deadline.tv_nsec -= 1000000000; }
    }
    pthread_mutex_lock(&local_fd_lock);
    int result, expired = 0;
    for (;;) {
        result = 0;
        for (uint64_t i = 0; i < count; ++i) {
            fds[i].revents = 0;
            if (fds[i].fd < 0) continue;
            LocalEventFd *event = local_fd_find(fds[i].fd);
            if (!event) {
                if (fds[i].fd < LOCAL_FD_BASE || fds[i].fd >= local_next_fd) {
                    result = socket_error(SOCKET_ENETDOWN); break;
                }
                fds[i].revents = SOCKET_POLLNVAL;
            } else {
                if (event->counter) fds[i].revents |= fds[i].events & SOCKET_POLLIN;
                if (event->counter < UINT64_MAX - 1) fds[i].revents |= fds[i].events & SOCKET_POLLOUT;
            }
            if (fds[i].revents) ++result;
        }
        if (result || !timeout || expired) break;
        int error = local_fd_wait(timeout > 0 ? &deadline : NULL);
        if (error == ETIMEDOUT) expired = 1;
        else if (error) { result = socket_error(SOCKET_EINVAL); break; }
    }
    pthread_mutex_unlock(&local_fd_lock);
    return result;
}
static int socket_network_down(void) { return socket_error(SOCKET_ENETDOWN); }
static int socket_option_down(int fd, int level, int option, const void *value, uint32_t size) {
    (void)value;
    fprintf(stderr, "[totk] offline SetSockOpt: fd=%d level=%#x option=%#x size=%u refused\n",
            fd, level, option, size);
    return socket_network_down();
}
#endif
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
static int socket_ready(void) { return 1; }
static uint32_t false_flag(void) { return 0; }
static void friends_event(void *self, void *event) { (void)self; if (event) { hle_bind_system_event(event, 0); hle_signal_system_event(event); } }
static uint8_t invitation_event[0x80] __attribute__((aligned(16)));   /* friend invitations: bound, never signalled */
static void *friends_invitation_event(void) { static int bound; if (!bound) { bound = 1; hle_bind_system_event(invitation_event, 0); } return invitation_event; }
static uint32_t friends_done(void *self, unsigned char *done) { (void)self; if (done) *done = 1; return 0; }

/* ---------- nn::settings ---------- */
/* The SDK builds the language's code through the settings service; the table is the system's fixed one. */
static unsigned char language_is(const int *language, const char *code) {
    static const char *const codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW",
                                        "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
    if (!language || !code || *language < 0 || *language >= (int)(sizeof codes / sizeof *codes)) return 0;
    return !strncmp(codes[*language], code, 8);
}

#ifdef GAME_NS2
/* SDK 22's PlayReport::Send (Ocarina of Time saves one as the first scene starts) returns an AsyncContext through x8, holding a pointer to the service's object. Null is what the SDK's own
   default context has: it completes at once with success. Naked because x8 is a hidden result pointer no C return type can name; nothing is sent (seedds/nxnative hle_offline.c, MIT). */
__attribute__((naked)) static void prepo_send(void) { __asm__("str xzr, [x8]\n    ret"); }
#endif
void *runtime_offline_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
#ifdef GAME_NS2
        {"_ZN2nn5prepo10PlayReport4SendERKNS_7account3UidE", prepo_send},
        /* The gRPC wakeup probe needs no socket options: only local event fds. */
        {"_ZN2nn6socket7EventFdEmNS0_12EventFdFlagsE", socket_event_fd},
        {"nnsocketEventFd", socket_event_fd},
        {"_ZN2nn6socket11EventFdReadEiPm", socket_event_read},
        {"nnsocketEventFdRead", socket_event_read},
        {"_ZN2nn6socket12EventFdWriteEim", socket_event_write},
        {"nnsocketEventFdWrite", socket_event_write},
        {"_ZN2nn6socket5CloseEi", socket_close},
        {"nnsocketClose", socket_close},
        {"_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", socket_fcntl},
        {"_ZN2nn6socket5FcntlEiiz", socket_fcntl},
        {"nnsocketFcntl", socket_fcntl},
        {"_ZN2nn6socket4PollEP6pollfdmi", socket_poll},
        {"_ZN2nn6socket4PollEPNS0_6PollFdEmi", socket_poll},
        {"nnsocketPoll", socket_poll},
        {"nnsocketGetLastErrno", socket_last_error},
        {"nnsocketSetLastErrno", socket_set_errno},
        {"_ZN2nn6socket12SetLastErrnoEi", socket_set_errno},
        {"nnsocketSocket", socket_network_down},
        {"nnsocketConnect", socket_network_down},
        {"nnsocketBind", socket_network_down},
        {"nnsocketListen", socket_network_down},
        {"nnsocketAccept", socket_network_down},
        {"nnsocketShutdown", socket_network_down},
        {"nnsocketSend", socket_network_down},
        {"nnsocketRecv", socket_network_down},
        {"nnsocketSendMsg", socket_network_down},
        {"nnsocketRecvMsg", socket_network_down},
        {"nnsocketSetSockOpt", socket_option_down},
        {"_ZN2nn6socket10SetSockOptEiNS0_5LevelENS0_6OptionEPKvj", socket_option_down},
        {"_ZN2nn6socket10SetSockOptEiiiPKvj", socket_option_down},
        {"nnsocketGetSockOpt", socket_network_down},
        {"nnsocketGetSockName", socket_network_down},
        {"nnsocketGetPeerName", socket_network_down},
        {"nnsocketGetAddrInfo", socket_addr_info}, {"nnsocketFreeAddrInfo", socket_free_addr_info},
        {"nnsocketInetHtonl", socket_htonl}, {"nnsocketInetNtohl", socket_htonl},
        {"nnsocketInetHtons", socket_htons}, {"nnsocketInetNtohs", socket_htons},
#endif
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", enter_exit_section},
        {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", leave_exit_section},
        {"_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", display_version},
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", pseudo_device_id}, {"_ZN2nn4nifm10InitializeEv", nifm_initialize},
        {"_ZN2nn4nifm18IsNetworkAvailableEv", nifm_available}, {"_ZN2nn4nifm19SetLocalNetworkModeEb", nifm_local_mode},
        {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_submit_and_wait},
        {"_ZN2nn4nifm20CancelNetworkRequestEv", nifm_cancel},
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", nifm_unavailable},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_unavailable},
        {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", nsd_unavailable}, {"_ZN2nn6socket10InitializeEPvmmi", socket_initialize},
        {"_ZN2nn6socket12GetLastErrorEv", socket_last_error},
        /* SDK 20's socket forms (Pokemon Pokopia): initialization succeeds, every socket call finds the network down. */
        {"_ZN2nn6socket10InitializeERKNS0_6ConfigE", socket_initialize}, {"_ZN2nn6socket8FinalizeEv", socket_initialize},
        {"_ZN2nn6socket12GetLastErrnoEv", socket_last_error},
        /* More of SDK 20's offline surface (Pokemon Pokopia): sockets stay down; crash reports, play reports and the profanity
           filter's setup are accepted; SSL, friends, local wireless, data delivery and the network request report the
           network unavailable, so the game keeps to its offline paths. */
        {"_ZN2nn6socket13IsInitializedEv", socket_ready}, {"_ZN2nn6socket4ReadEiPvm", socket_down}, {"_ZN2nn6socket5WriteEiPKvm", socket_down},
        {"_ZN2nn6socket11GetPeerNameEiP8sockaddrPj", socket_down},
        {"_ZN2nn2oe17EnableCrashReportEv", nothing}, {"_ZN2nn2oe18DisableCrashReportEv", nothing},
        {"_ZN2nn2oe40SendServerMaintenanceOverlayNotificationENS_4time9PosixTimeES2_", nothing},
        {"_ZN2nn4nifm20SubmitNetworkRequestEv", nothing}, {"_ZN2nn4nifm22IsNetworkRequestOnHoldEv", false_flag},
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", nifm_unavailable},
        {"_ZN2nn3ssl7ContextC1Ev", nothing}, {"_ZN2nn3ssl7ContextD1Ev", nothing}, {"_ZN2nn3ssl7Context7DestroyEv", ok_result},
        {"_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", ssl_unavailable}, {"_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ssl_unavailable},
        {"_ZN2nn3ssl7Context9ImportCrlEPmPKcj", ssl_unavailable}, {"_ZN2nn3ssl8FinalizeEv", ok_result},
        {"_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", ssl_unavailable},
        {"_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", ssl_unavailable},
        {"_ZN2nn7friends12AsyncContextC1Ev", nothing}, {"_ZN2nn7friends12AsyncContextD1Ev", nothing},
        {"_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", friends_event}, {"_ZN2nn7friends12AsyncContext7HasDoneEPb", friends_done},
        {"_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileEPKNS_7account23NetworkServiceAccountIdEi", friends_unavailable},
        {"_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi", friends_unavailable},
        {"_ZN2nn7friends21StartFriendInvitationERKNS_7account3UidEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", friends_unavailable},
        {"_ZN2nn7friends28StartSendingFriendInvitationERKNS_7account3UidEPKNS1_23NetworkServiceAccountIdEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", friends_unavailable},
        {"_ZN2nn7friends38TryPopFriendInvitationNotificationInfoEPNS_7account3UidEPmPvm", false_flag},
        {"_ZN2nn7friends7ProfileC1Ev", nothing},
        {"_ZN2nn7friends36GetFriendInvitationNotificationEventEv", friends_invitation_event},
        {"_ZN2nn3ldn10DisconnectEv", ldn_unavailable}, {"_ZN2nn3ldn12CloseStationEv", ldn_unavailable}, {"_ZN2nn3ldn16CloseAccessPointEv", ldn_unavailable},
        {"_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ldn_unavailable},
        {"_ZN2nn3ldn16SetAdvertiseDataEPKvm", ldn_unavailable}, {"_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ldn_unavailable},
        {"_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", ldn_unavailable},
        {"_ZN2nn3ngc15ProfanityFilterC1Ev", nothing}, {"_ZN2nn3ngc15ProfanityFilterD1Ev", nothing}, {"_ZN2nn3ngc15ProfanityFilter8FinalizeEv", ok_result},
        {"_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", ok_result}, {"_ZN2nn3ngc15ProfanityFilter22SetNormalizationOptionEi", nothing},
        {"_ZN2nn5prepo10PlayReportC1EPKc", nothing}, {"_ZN2nn5prepo10PlayReportD1Ev", nothing},
        {"_ZN2nn5prepo10PlayReport3AddEPKcRKNS0_5ArrayE", ok_result}, {"_ZN2nn5prepo10PlayReport3AddEPKcb", ok_result}, {"_ZN2nn5prepo10PlayReport3AddEPKcf", ok_result},
        {"_ZN2nn5prepo5ArrayC1Ev", nothing}, {"_ZN2nn5prepo5Array9SetBufferEPvm", nothing}, {"_ZN2nn5prepo5Array3AddEf", ok_result}, {"_ZN2nn5prepo5Array3AddEl", ok_result},
        {"_ZN2nn4bcat17DeliveryCacheFileC1Ev", nothing}, {"_ZN2nn4bcat17DeliveryCacheFileD1Ev", nothing},
        {"_ZN2nn4bcat17DeliveryCacheFile4OpenEPKc", nifm_unavailable}, {"_ZN2nn4bcat17DeliveryCacheFile4OpenERKNS0_13DirectoryNameERKNS0_8FileNameE", nifm_unavailable},
        {"_ZN2nn4bcat22DeliveryCacheDirectoryC1Ev", nothing}, {"_ZN2nn4bcat22DeliveryCacheDirectoryD1Ev", nothing},
        {"_ZN2nn4bcat22DeliveryCacheDirectory4OpenERKNS0_13DirectoryNameE", nifm_unavailable},
        {"_ZN2nn4bcat21DeliveryCacheProgressC1Ev", nothing}, {"_ZN2nn4bcat21DeliveryCacheProgressD1Ev", nothing},
        {"_ZN2nn4bcat24RequestSyncDeliveryCacheEPNS0_21DeliveryCacheProgressE", nifm_unavailable},
        {"_ZN2nn6socket10GetSockOptEiiiPvPj", socket_down}, {"_ZN2nn6socket10SetSockOptEiiiPKvj", socket_down},
        {"_ZN2nn6socket11GetSockNameEiP8sockaddrPj", socket_down}, {"_ZN2nn6socket4PollEP6pollfdmi", socket_down},
        {"_ZN2nn6socket4RecvEiPvmi", socket_down}, {"_ZN2nn6socket4SendEiPKvmi", socket_down}, {"_ZN2nn6socket5FcntlEiNS0_12FcntlCommandEz", socket_down},
        {"_ZN2nn6socket6AcceptEiP8sockaddrPj", socket_down}, {"_ZN2nn6socket6ListenEii", socket_down},
        {"_ZN2nn6socket6SelectEiPNS0_5FdSetES2_S2_PNS0_7TimeValE", socket_down}, {"_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", socket_down},
        {"_ZN2nn6socket7ConnectEiPK8sockaddrj", socket_down}, {"_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", socket_down},
        {"_ZN2nn6socket8ShutdownEii", socket_down},
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
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
