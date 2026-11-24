/* Dark Souls Remastered's online-only SDK calls that shared/runtime/offline_host.c does not list, answered as an offline
   console would (no friends, nobody blocked, no play history). Without them the SDK's own code opened the friends
   service over IPC, which this host does not serve ("IPC service not implemented" from
   nn::friends::GetBlockedUserList, main+0x92f1a8, 23 s into the first boot, as the title movie started).
   The result code is offline_host.c's R_FRIENDS_UNAVAILABLE. */
#include "runtime.h"
#include <stdint.h>
#include <string.h>
#include <time.h>

#define R_FRIENDS_UNAVAILABLE (121u | (6u << 9))

/* GetBlockedUserList(int *count, NetworkServiceAccountId *out, int offset, int max): nobody is blocked. */
static uint32_t blocked_users(int *count, void *out, int offset, int max) {
    (void)out; (void)offset; (void)max;
    if (count) *count = 0;
    return 0;
}
/* GetPlayHistoryRegistrationKey(key *, const Uid &, bool): no network account, so no key. */
static uint32_t play_history_key(void *key, const void *uid, unsigned char local) {
    (void)key; (void)uid; (void)local;
    return R_FRIENDS_UNAVAILABLE;
}
/* AddPlayHistory(...): nothing is recorded offline. */
static uint32_t add_play_history(const void *uid, const void *key, const void *name, const void *their_name) {
    (void)uid; (void)key; (void)name; (void)their_name;
    return R_FRIENDS_UNAVAILABLE;
}
/* ShowUserDetailInfo(...): the user-page applet does not exist here. */
static uint32_t user_detail(const void *uid, const void *account, const void *name, const void *their_name) {
    (void)uid; (void)account; (void)name; (void)their_name;
    return R_FRIENDS_UNAVAILABLE;
}

/* nn::socket::Poll(pollfd *, nfds, timeout ms). offline_host.c answers -1/ENETDOWN at once, and the game's network thread
   (h=0x1014 in the first boots) called it 805 million times in 75 s, holding virtual core 0 the whole time while another
   thread (h=0x1012, mask 0x1) queued for that core and the picture stayed black. An offline console's poll that sees no
   event times out: wait the timeout (at least 1 ms, at most POLL_WAIT_CAP_MS) with the virtual core given up, then
   answer 0, "no descriptor ready". */
enum { POLL_WAIT_CAP_MS = 50 };
static int socket_poll(void *fds, unsigned long count, int timeout_ms) {
    (void)fds; (void)count;
    int ms = timeout_ms < 1 ? 1 : timeout_ms > POLL_WAIT_CAP_MS ? POLL_WAIT_CAP_MS : timeout_ms;
    if (timeout_ms < 0) ms = POLL_WAIT_CAP_MS;   /* "wait forever": sleep the cap, the caller polls again */
    int away = runtime_core_away();
    struct timespec ts = {0, (long)ms * 1000000L};
    nanosleep(&ts, NULL);
    runtime_core_back(&away);
    return 0;
}

/* nn::ngc::ProfanityFilter::SetMaskMode / MaskProfanityWordsInText (offline_host.c stubs C1Ev / Initialize / Finalize
   so the filter's internal WorkBufAllocator is uninitialized; DSR calls MaskProfanityWordsInText on the character name
   when confirming in Character Creation). Offline: nothing is masked, 0 profanity words found. */
static void ngc_set_mask_mode(void *self, int mode) { (void)self; (void)mode; }
static uint32_t ngc_mask_profanity_u16(void *self, int *count, uint16_t *text, uint32_t patterns) {
    (void)self; (void)text; (void)patterns;
    if (count) *count = 0;
    return 0;
}

void *runtime_dsr_offline_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn6socket4PollEP6pollfdmi", socket_poll},
        {"_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdEii", blocked_users},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", play_history_key},
        {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", add_play_history},
        {"_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", user_detail},
        {"_ZN2nn3ngc15ProfanityFilter11SetMaskModeENS1_8MaskModeE", ngc_set_mask_mode},
        {"_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", ngc_mask_profanity_u16},
    };
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++)
        if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
