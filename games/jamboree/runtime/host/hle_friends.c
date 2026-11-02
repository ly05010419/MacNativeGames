// nn::friends (friends / presence / invitations, out of scope): every interface that connects to the friends service is cut to "unavailable".
// Not cut: the getters of Friend / Profile / FriendPresence and the constructors of Friend / Profile / UserPresence (they only read and write the game's own objects and do not go through the service).
// Return convention: Result returns RES_FAIL; bool returns false; event pointers get a SystemEvent that is bound to a kernel event and never fires.
#include "hle.h"

// Notification event that never fires (GetSystemEvent-style calls return a pointer; bound on the guest thread on first use)
static _Alignas(16) uint8_t g_ev_notify[0x100], g_ev_invite[0x100];
static void *quiet_event(uint8_t *ev) { hle_bind_system_event(ev, 0); return ev; }

HLE_FORCE("_ZN2nn7friends10InitializeEv", "connects to the friends service (cut)", void, h_fr_Initialize, void) {}
// AsyncContext: no request is sent; the completion event fires immediately and the result is failure (same as hle_account.c)
HLE_FORCE("_ZN2nn7friends12AsyncContextC1Ev", "async context creates no session (cut)", void, h_fr_AcCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7friends12AsyncContextD1Ev", "async context has no session to close (cut)", void, h_fr_AcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", "completion event fires immediately (cut)", Result, h_fr_AcGetEv, void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
HLE_FORCE("_ZNK2nn7friends12AsyncContext9GetResultEv", "async result always fails (cut)", Result, h_fr_AcResult, const void *self) { (void)self; return RES_FAIL; }
// Lists / profiles / play history
HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE", "friend list is empty (cut)", Result, h_fr_GetFriendList, int *count, void *out, int off, int n, const void *filt) {
    (void)out; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL;
}
// Blocked users, SDK 4's overload without a Uid: an empty list and success. Dark Souls Remastered asks after the opening movie and turns a failure into an error for the error applet
// (main+0x8ff9f0). The Uid overload below reports a failure, which DRAGON QUEST XI S takes as it is
HLE_FORCE("_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdEii", "blocked user list is empty (cut)", Result, h_fr_GetBlocked4, int *count, void *out, int off, int n) {
    (void)out; (void)off; (void)n; if (count) *count = 0; return RES_OK;
}
HLE_FORCE("_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi", "profile query fails (cut)",
          Result, h_fr_GetProfileList, void *ctx, void *out, const void *uid, const void *ids, int n) { (void)ctx; (void)out; (void)uid; (void)ids; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi", "friend info update fails (cut)",
          Result, h_fr_UpdateFriendInfo, void *f, const void *uid, const void *ids, int n) { (void)f; (void)uid; (void)ids; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends14AddPlayHistoryERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameES6_", "play history is not registered (cut)",
          Result, h_fr_AddPlayHistory, const void *k, const void *a, const void *b) { (void)k; (void)a; (void)b; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", "play history key unavailable (cut)",
          Result, h_fr_GetPHKey, void *k, const void *uid, int local) { (void)k; (void)uid; (void)local; return RES_FAIL; }
// Notification queue
HLE_FORCE("_ZN2nn7friends17NotificationQueueC1Ev", "notification queue creates no session (cut)", void, h_fr_NqCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7friends17NotificationQueueD1Ev", "notification queue has no session to close (cut)", void, h_fr_NqDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE", "notification queue init fails (cut)", Result, h_fr_NqInit, void *self, const void *uid) { (void)self; (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends17NotificationQueue14GetSystemEventEv", "notification event never fires (cut)", void *, h_fr_NqGetEv, void *self) { (void)self; return quiet_event(g_ev_notify); }
HLE_FORCE("_ZN2nn7friends17NotificationQueue3PopEPNS0_16NotificationInfoE", "notification queue is always empty (cut)", Result, h_fr_NqPop, void *self, void *info) { (void)self; (void)info; return RES_FAIL; }
// Invitations
HLE_FORCE("_ZN2nn7friends36GetFriendInvitationNotificationEventEv", "invitation notification event never fires (cut)", void *, h_fr_InvEv, void) { return quiet_event(g_ev_invite); }
HLE_FORCE("_ZN2nn7friends38TryPopFriendInvitationNotificationInfoEPNS_7account3UidEPmPvm", "no invitations (cut)", int, h_fr_InvPop, void *uid, uint64_t *sz, void *buf, size_t n) {
    (void)uid; (void)sz; (void)buf; (void)n; return 0;
}
HLE_FORCE("_ZN2nn7friends21StartFriendInvitationERKNS_7account3UidEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", "invitations unavailable (cut)",
          Result, h_fr_StartInv, const void *uid, int n, const void *d, const void *p, size_t sz) { (void)uid; (void)n; (void)d; (void)p; (void)sz; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends28StartSendingFriendInvitationERKNS_7account3UidEPKNS1_23NetworkServiceAccountIdEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", "invitations unavailable (cut)",
          Result, h_fr_SendInv, const void *uid, const void *ids, int n, const void *d, const void *p, size_t sz) { (void)uid; (void)ids; (void)n; (void)d; (void)p; (void)sz; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", "friend detail applet (cut)",
          Result, h_fr_ShowDetail, const void *uid, const void *id, const void *a, const void *b) { (void)uid; (void)id; (void)a; (void)b; return RES_FAIL; }
// Friend list applets (Engage's friends menu): the original starts an applet, the host has no applet proxy
HLE_FORCE("_ZN2nn7friends14ShowFriendListERKNS_7account3UidE", "friend list applet (cut)", Result, h_fr_ShowList, const void *uid) { (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends19ShowBlockedUserListERKNS_7account3UidE", "blocked user list applet (cut)", Result, h_fr_ShowBlocked, const void *uid) { (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends29ShowReceivedFriendRequestListERKNS_7account3UidE", "received friend request applet (cut)", Result, h_fr_ShowRecv, const void *uid) { (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends23StartFacedFriendRequestERKNS_7account3UidE", "in-person friend request applet (cut)", Result, h_fr_StartFaced, const void *uid) { (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends25StartSendingFriendRequestERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", "send friend request applet (cut)",
          Result, h_fr_StartSend, const void *uid, const void *nsa, const void *mine, const void *theirs) { (void)uid; (void)nsa; (void)mine; (void)theirs; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends33ShowMethodsOfSendingFriendRequestERKNS_7account3UidE", "send friend request applet (cut)", Result, h_fr_ShowSend, const void *uid) { (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii", "blocked user list is empty (cut)",
          Result, h_fr_GetBlocked, int *count, void *out, const void *uid, int off, int n) { (void)out; (void)uid; (void)off; (void)n; if (count) *count = 0; return RES_FAIL; }
// Online play sessions (DRAGON QUEST XI S)
HLE_FORCE("_ZN2nn7friends28DeclareOpenOnlinePlaySessionERKNS_7account3UidE", "online play session unavailable (cut)", Result, h_fr_OpenSession, const void *uid) { (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends29DeclareCloseOnlinePlaySessionERKNS_7account3UidE", "online play session unavailable (cut)", Result, h_fr_CloseSession, const void *uid) { (void)uid; return RES_FAIL; }
// Presence
HLE_FORCE("_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", "presence unavailable (cut)", Result, h_fr_UpInit, void *self, const void *uid) { (void)self; (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends12UserPresence14SetDescriptionEPKc", "presence unavailable (cut)", Result, h_fr_UpDesc, void *self, const char *s) { (void)self; (void)s; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends12UserPresence6CommitEv", "presence unavailable (cut)", Result, h_fr_UpCommit, void *self) { (void)self; return RES_FAIL; }
