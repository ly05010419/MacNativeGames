/* Horizon services Super Smash Bros. Ultimate (and Super Mario Odyssey, whose copy this began as) reach that TotK never
   did, answered in-process, as a Switch without a network
   connection. Adapted from games/by3/runtime/system_services.c (its set handler is not needed here: settings_service.c
   registers set). Command numbers and layouts follow the public switchbrew descriptions. Unknown commands still stop the
   runtime with their id. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

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

/* ---------- SDK functions answered by name (runtime_system_override) ----------
   Network-facing libraries answer as a Switch without a connection. The failure results are generic ones of each
   library's module (nifm 110, ssl 123, account 124), not a specific console error: the game only needs "unavailable". */
#define RESULT(module, description) ((uint32_t)(module) | ((uint32_t)(description) << 9))
#define NIFM_UNAVAILABLE RESULT(110, 300)
#define SSL_UNAVAILABLE RESULT(123, 300)
#define NSA_UNAVAILABLE RESULT(124, 280)
static void nop(void) {}
static uint32_t ok(void) { return 0; }
static uint8_t no(void) { return 0; }
static uint32_t nifm_unavailable(void) { return NIFM_UNAVAILABLE; }
static uint32_t ssl_unavailable(void) { return SSL_UNAVAILABLE; }
static uint32_t nsa_unavailable(void) { return NSA_UNAVAILABLE; }
static void nifm_request(void) { static int once; if (!once++) fprintf(stderr, "[totk-net] network request: offline\n"); }
static int64_t network_clock_now(void) { return (int64_t)time(NULL); }   /* time_point: seconds since the epoch */
static void environment_identifier(char *out) { if (!out) runtime_fail("nsd environment output"); memset(out, 0, 8); memcpy(out, "lp1", 3); }
static void friends_initialize(void) { fprintf(stderr, "[smo] friends: offline\n"); }
/* Super Smash Bros. Ultimate 13.0.2 reads the friend list and the blocked users at the title, registers play history and
   keeps a friend notification queue. A console that never went online has no friends, blocks no one and never gets a
   notification: lists are empty, the queue's event is never signaled and popping finds nothing (friends module 121). */
#define FRIENDS_QUEUE_EMPTY RESULT(121, 1)
static uint32_t friends_empty_list(int32_t *count) { if (!count) runtime_fail("friends list count pointer"); *count = 0; return 0; }
static uint32_t friends_registration_key(uint8_t *key) { if (key) memset(key, 0, 0x40); return 0; }
static uint8_t friends_queue_event[0x100] __attribute__((aligned(16)));
static void *friends_queue_system_event(void *queue) {
    (void)queue; static int bound;
    if (!bound) { hle_bind_system_event(friends_queue_event, 0); bound = 1; }
    return friends_queue_event;
}
static uint32_t friends_queue_pop(void *queue, void *info) { (void)queue; (void)info; return FRIENDS_QUEUE_EMPTY; }
/* Smash 13.0.2: no Nintendo Account is linked (the SDK's version reads the host's user handle as a service object); the
   screenshot permission, LCD backlight, auto-sleep sections and TV power matching have no Mac counterpart; parental
   controls allow everything (as Initialize, services.c). */
static uint32_t nsa_not_linked(uint8_t *available, const void *user) { (void)user; if (!available) runtime_fail("IsNetworkServiceAccountAvailable output"); *available = 0; return 0; }
static void tv_power_matching(int mode) { (void)mode; }
/* nn::pctl::Check*Permission return a bool, true = permitted (sdk+0x33a620 ends in `cset w0, eq` on the service's
   result). Answering 0 here read as "restricted" at every call (Q70, 2026-10-24: six in main read the free
   communication answer). Free communication is permitted, as on a console without parental controls; stereo vision
   is not, there are no VR goggles on the Mac (BeginVrMode answers with the SDK's own "not permitted", shared_font.c). */
static uint8_t free_communication_permitted(uint8_t show_error) { (void)show_error; return 1; }
static uint8_t stereo_vision_permitted(uint8_t show_error) { (void)show_error; return 0; }
/* The Mii database (mii:e) Super Smash Bros. Ultimate opens at the title for the Mii fighters: a console whose Mii Maker
   holds no Miis. Building default or random Miis still goes to the service and stops with its name until needed. */
static uint32_t mii_initialize(void *db) { (void)db; fprintf(stderr, "[ssbu] mii: database with no Miis\n"); return 0; }
static int32_t mii_count(const void *db, int32_t source) { (void)db; (void)source; return 0; }
static uint32_t mii_get(void *db, int32_t *count, void *out, int32_t max, int32_t source) { (void)db; (void)out; (void)max; (void)source; if (count) *count = 0; return 0; }
static uint8_t mii_updated(void *db, int32_t source) { (void)db; (void)source; return 0; }
/* Building Miis (the six defaults for the Mii fighters, random ones, Mii data in the 3DS format): the SDK carries the Mii
   library the mii:e service runs (nn::mii::detail, with the default table and the random generator), so the host runs that
   code on the game's thread as the service would in its own process: a StoreData from the table, at random or from the
   Ver3 data, then the CharInfo from it. Nothing is made up here. */
/* The console's Mii author id (set:sys), stamped into every Mii built here: one fixed version-4 UUID for this host. */
static void mii_author_id(uint8_t *uuid) {
    static const uint8_t id[16] = {0x4d, 0x61, 0x63, 0x4e, 0x61, 0x74, 0x69, 0x76, 0x65, 0x47, 0x61, 0x6d, 0x65, 0x73, 0x53, 0x42};
    if (!uuid) runtime_fail("GetMiiAuthorId output pointer");
    memcpy(uuid, id, 16); uuid[6] = (uint8_t)((uuid[6] & 0x0f) | 0x40); uuid[8] = (uint8_t)((uuid[8] & 0x3f) | 0x80);
}
static void *mii_fn(const char *name) { void *p = runtime_symbol(name); if (!p) { fprintf(stderr, "[ssbu] mii: %s missing\n", name); runtime_fail("Mii library function missing from the SDK"); } return p; }
typedef struct { _Alignas(16) uint8_t b[0x200]; } MiiScratch;   /* UtilityImpl, StoreDataRaw (0x44 bytes) */
static void mii_util(MiiScratch *u) { memset(u, 0, sizeof *u); ((void (*)(void *))mii_fn("_ZN2nn3mii6detail11UtilityImplC1Ev"))(u->b); }
static void mii_util_done(MiiScratch *u) { ((void (*)(void *))mii_fn("_ZN2nn3mii6detail11UtilityImplD1Ev"))(u->b); }
static void mii_char_from_store(void *out, const MiiScratch *store) {
    ((void (*)(void *, const void *))mii_fn("_ZN2nn3mii6detail11CharInfoRaw19SetFromStoreDataRawERKNS1_12StoreDataRawE"))(out, store->b);
}
static void mii_build_default(const void *db, void *out, int32_t index) {
    (void)db; if (!out || index < 0 || index > 5) runtime_fail("nn::mii::Database::BuildDefault: index outside the six default Miis");
    MiiScratch u, s; mii_util(&u); memset(&s, 0, sizeof s);
    ((void (*)(void *, void *, int32_t))mii_fn("_ZN2nn3mii6detail12StoreDataRaw12BuildDefaultEPNS1_11UtilityImplEi"))(s.b, u.b, index);
    mii_char_from_store(out, &s); mii_util_done(&u);
    static int said; if (!said++) fprintf(stderr, "[ssbu] mii: default Miis built by the SDK's own Mii library\n");
}
static uint32_t mii_build_random(void *db, void *out, const void *desc) {
    (void)db; if (!out || !desc) runtime_fail("nn::mii::Database::BuildRandom arguments");
    MiiScratch u, s; mii_util(&u); memset(&s, 0, sizeof s);
    ((void (*)(void *, void *, const void *))mii_fn("_ZN2nn3mii6detail12StoreDataRaw11BuildRandomEPNS1_11UtilityImplERKNS0_10RandomDescE"))(s.b, u.b, desc);
    mii_char_from_store(out, &s); mii_util_done(&u); return 0;
}
static uint32_t mii_convert(void *db, void *out, const void *ver3) {
    (void)db; if (!out || !ver3) runtime_fail("nn::mii::Database::Convert arguments");
    MiiScratch u, s; mii_util(&u); memset(&s, 0, sizeof s);
    ((void (*)(const void *, void *, void *))mii_fn("_ZNK2nn3mii6detail16Ver3StoreDataRaw16BuildToStoreDataEPNS1_12StoreDataRawEPNS1_11UtilityImplE"))(ver3, s.b, u.b);
    mii_char_from_store(out, &s); mii_util_done(&u); return 0;
}
/* Applet functions without a Mac counterpart (the SDK's own versions read an applet object the host never creates): the
   screenshot orientation (the Capture button, after the opening scene), the HOME button blocking around scenes, and the
   extended inactivity time (never extended). */
static void screenshot_orientation(int orientation) { (void)orientation; }
static void home_button_blocking(void) {}
/* BCAT (news and event data the console downloads in the background): a console that never connected has an empty
   delivery cache. It mounts, and its directory list is empty, so the game never opens a file in it. */
static uint32_t bcat_initialize(void) { fprintf(stderr, "[smo] bcat: offline, empty delivery cache\n"); return 0; }
static uint32_t bcat_enumerate(int32_t *count, void *names, int32_t max) {
    (void)names; (void)max; if (!count) runtime_fail("bcat EnumerateDeliveryCacheDirectory count pointer"); *count = 0; return 0;
}
/* The offline web page (the game's own HTML pages, htmlcontents.htdocs/html/<region><language>/index.html): on the console
   a library applet (id 0x17) shows it; the host has none, and the SDK's CreateLibraryApplet read a null applet object
   (2026-10-22, 5 minutes into a session). It is answered as a page the player closed at once. The return value is the
   SDK's 0x1010-byte OfflineHtmlPageReturnValue: exit reason at +0 (its constructor writes 0x14, GetOfflineExitReason reads
   the word), the last URL at +8, its size at +0x1008. The game copies the last URL only for exit reason 3 (a callback URL,
   main+0x6cb5e0), so reason 0 with an empty URL returns it to its menu. */
/* Parental controls for the VR mode (stereo vision): Initialize is answered locally (services.c), so the SDK's versions
   would talk to a pctl session that does not exist. A console without restrictions: the setting can be changed (the
   game calls Confirm...Configurable and Request...Permission as nn::Result, main+0x568ff0 / +0x56901c) and keeps what
   the game sets (GetStereoVisionRestriction is read as a bool). */
static _Atomic uint8_t stereo_vision_restricted;
static uint32_t stereo_vision_ok(void) { return 0; }
static uint8_t stereo_vision_get(void) { return stereo_vision_restricted; }
static void stereo_vision_set(uint8_t on) { stereo_vision_restricted = on & 1; fprintf(stderr, "[smo] pctl: VR mode restriction %s\n", on & 1 ? "on" : "off"); }
static uint32_t show_offline_html_page(uint8_t *ret, const void *arg) {
    (void)arg; if (!ret) runtime_fail("nn::web::ShowOfflineHtmlPage return value pointer");
    memset(ret, 0, 0x1010);
    fprintf(stderr, "[smo] web: the game's offline page is not shown on the Mac; answered as closed\n");
    return 0;
}
void *runtime_system_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2oe13MuteRecordingEv", nop}, {"_ZN2nn2oe15UnmuteRecordingEv", nop}, {"_ZN2nn2oe16DisableRecordingEv", nop},
        {"_ZN2nn2oe29SetScreenShotImageOrientationENS_5album16ImageOrientationE", screenshot_orientation},
        {"_ZN2nn2oe23BeginBlockingHomeButtonEv", home_button_blocking}, {"_ZN2nn2oe21EndBlockingHomeButtonEv", home_button_blocking},
        {"_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv", no},
        {"_ZN2nn4nifm10InitializeEv", ok}, {"_ZN2nn4nifm18IsNetworkAvailableEv", no},
        {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", nifm_request}, {"_ZN2nn4nifm20CancelNetworkRequestEv", nop},
        /* A bool in the SDK, true = the error was handled and the request may be submitted again (sdk+0x322d90 ends in
           `and w0, w0, #1`). The Result answered before only read as false because its low bit is 0 (Q72). */
        {"_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", no},
        {"_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", nifm_unavailable},
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", nifm_unavailable},
        /* SDK 7.3.2 spellings Super Mario Odyssey imports: a request that is submitted, never on hold, and fails. */
        {"_ZN2nn4nifm20SubmitNetworkRequestEv", nifm_request}, {"_ZN2nn4nifm22IsNetworkRequestOnHoldEv", no},
        {"_ZN2nn4nifm26HandleNetworkRequestResultEv", nifm_unavailable},
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", nifm_unavailable},
        {"_ZN2nn3ssl10InitializeEv", ok}, {"_ZN2nn3ssl8FinalizeEv", ok},
        {"_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", ssl_unavailable},
        {"_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", ssl_unavailable},
        {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", nsa_unavailable},
        {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", nsa_unavailable},
        {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", nsa_unavailable},
        {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", nsa_unavailable},
        {"_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", show_offline_html_page},
        {"_ZN2nn4pctl42ConfirmStereoVisionRestrictionConfigurableEv", stereo_vision_ok},
        {"_ZN2nn4pctl53RequestStereoVisionRestrictionConfigurationPermissionEv", stereo_vision_ok},
        {"_ZN2nn4pctl26GetStereoVisionRestrictionEv", stereo_vision_get},
        {"_ZN2nn4pctl26SetStereoVisionRestrictionEb", stereo_vision_set},
        {"_ZN2nn7friends10InitializeEv", friends_initialize},
        {"_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", ok},
        {"_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", friends_empty_list},
        {"_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii", friends_empty_list},
        {"_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi", ok},
        {"_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", ok},
        {"_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", friends_registration_key},
        {"_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE", ok},
        {"_ZN2nn7friends17NotificationQueue8FinalizeEv", nop},
        {"_ZN2nn7friends17NotificationQueue14GetSystemEventEv", friends_queue_system_event},
        {"_ZN2nn7friends17NotificationQueue3PopEPNS0_16NotificationInfoE", friends_queue_pop},
        {"_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", ok},
        {"_ZN2nn3mii8Database10InitializeEv", mii_initialize}, {"_ZNK2nn3mii8Database8GetCountEi", mii_count},
        {"_ZN2nn3mii8Database3GetEPiPNS0_8CharInfoEii", mii_get}, {"_ZN2nn3mii8Database9IsUpdatedEi", mii_updated},
        {"_ZNK2nn3mii8Database12BuildDefaultEPNS0_8CharInfoEi", mii_build_default},
        {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", nsa_not_linked},
        {"_ZN2nn2oe16EnableScreenShotEv", nop}, {"_ZN2nn2oe17DisableScreenShotEv", nop},
        {"_ZN2nn2oe20SwitchLcdBacklightOnEv", nop}, {"_ZN2nn2oe21SwitchLcdBacklightOffEv", nop},
        {"_ZN2nn2oe29BeginAutoSleepDisabledSectionEv", nop}, {"_ZN2nn2oe27EndAutoSleepDisabledSectionEv", nop},
        {"_ZN2nn2oe27SetTvPowerStateMatchingModeENS0_24TvPowerStateMatchingModeE", tv_power_matching},
        {"_ZN2nn4pctl32CheckFreeCommunicationPermissionEb", free_communication_permitted},
        {"_ZN2nn4pctl27CheckStereoVisionPermissionEb", stereo_vision_permitted},
        {"_ZN2nn8settings6system14GetMiiAuthorIdEPNS_4util4UuidE", mii_author_id},
        {"_ZN2nn3mii8Database11BuildRandomEPNS0_8CharInfoERKNS0_10RandomDescE", mii_build_random},
        {"_ZN2nn3mii8Database7ConvertEPNS0_8CharInfoERKNS0_13Ver3StoreDataE", mii_convert},
        {"_ZN2nn4time26StandardNetworkSystemClock3nowEv", network_clock_now},
        {"_ZN2nn3err21ReportMultimediaErrorENS_6ResultERKN5movie25MultimediaTelemetryReportE", nop},
        {"_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", environment_identifier},
        {"_ZN2nn5prepo10InitializeEv", nop},
        {"_ZN2nn4bcat10InitializeEv", bcat_initialize}, {"_ZN2nn4bcat25MountDeliveryCacheStorageEv", ok},
        {"_ZN2nn4bcat27UnmountDeliveryCacheStorageEv", nop},
        {"_ZN2nn4bcat31EnumerateDeliveryCacheDirectoryEPiPNS0_13DirectoryNameEi", bcat_enumerate},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

/* SMO_TRACE_FONT=1 (diagnostics): the game's calls into the SDK's scalable font engine, with arguments and results, through
   the loader's observer hook (the original runs; only the first calls of each are printed). */
#define FONT_TRACE(fn, symbol) \
    static uint64_t fn(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) { \
        static _Atomic unsigned calls; uint64_t (*real)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = runtime_symbol(symbol); \
        uint64_t r = real(a, b, c, d, e); \
        if (calls++ < 12) fprintf(stderr, "[smo-font] %s(%#llx, %#llx, %#llx, %#llx, %#llx) -> %#llx\n", #fn, (unsigned long long)a, \
                                  (unsigned long long)b, (unsigned long long)c, (unsigned long long)d, (unsigned long long)e, (unsigned long long)r); \
        return r; }
FONT_TRACE(Initialize, "_ZN2nn6fontll18ScalableFontEngine10InitializeEPvj")
static uint64_t LoadFont_raw(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e);
static uint64_t LoadFont(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e) {
    static _Atomic unsigned shown;
    if (c && shown++ < 4) { const uint8_t *p = (const uint8_t *)(uintptr_t)c;
        fprintf(stderr, "[smo-font] LoadFont data %#llx: before %02x%02x%02x%02x %02x%02x%02x%02x | at %02x%02x%02x%02x %02x%02x%02x%02x\n",
                (unsigned long long)c, p[-8], p[-7], p[-6], p[-5], p[-4], p[-3], p[-2], p[-1], p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]); }
    return LoadFont_raw(a, b, c, d, e);
}
FONT_TRACE(LoadFont_raw, "_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj")
FONT_TRACE(SetFont, "_ZN2nn6fontll18ScalableFontEngine7SetFontEPc")
FONT_TRACE(Decode, "_ZN2nn6fontll24ScalableFontEngineHelper6DecodeEPKvj")
FONT_TRACE(GetError, "_ZN2nn6fontll18ScalableFontEngine8GetErrorEv")
FONT_TRACE(CheckGlyphExist, "_ZN2nn6fontll18ScalableFontEngine15CheckGlyphExistEj")
FONT_TRACE(AcquireGlyphmap, "_ZN2nn6fontll18ScalableFontEngine15AcquireGlyphmapEjt")
FONT_TRACE(SetScale, "_ZN2nn6fontll18ScalableFontEngine8SetScaleEiiii")
void *runtime_font_observer(const char *name) {
    static int on = -1; if (on < 0) on = getenv("SMO_TRACE_FONT") != NULL;
    if (!on) return NULL;
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn6fontll18ScalableFontEngine10InitializeEPvj", Initialize}, {"_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj", LoadFont},
        {"_ZN2nn6fontll18ScalableFontEngine7SetFontEPc", SetFont}, {"_ZN2nn6fontll24ScalableFontEngineHelper6DecodeEPKvj", Decode},
        {"_ZN2nn6fontll18ScalableFontEngine8GetErrorEv", GetError}, {"_ZN2nn6fontll18ScalableFontEngine15CheckGlyphExistEj", CheckGlyphExist},
        {"_ZN2nn6fontll18ScalableFontEngine15AcquireGlyphmapEjt", AcquireGlyphmap}, {"_ZN2nn6fontll18ScalableFontEngine8SetScaleEiiii", SetScale},
    };
    for (size_t i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}

__attribute__((constructor)) static void register_system_services(void) {
    /* Present pacing as on the Switch: a 60 Hz base, the game's present interval on top (1 = 60 fps, 2 = 30 fps). The
       TotK host follows the display (120 Hz on ProMotion) and runs uncapped, which relies on TotK's real-time frame step
       patch; without it a game that advances per frame would run twice as fast (as games/by3). A value the user set wins. */
    setenv("DM_PRESENT_HZ", "60", 0);
    setenv("TOTK_UNCAPPED", "0", 0);
    runtime_ipc_register("bsd:u", bsd_handler, NULL, NULL);
}
