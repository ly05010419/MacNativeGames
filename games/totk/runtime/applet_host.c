/* Host side of applet, telemetry, NFC and error-display services that main reaches through IPC.
   Client-side SDK code (PlayReport encoding, keyboard presets, diag logging) runs unchanged; only the
   calls that would talk to system services are taken here. */
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---------- nn::oe ---------- */
static float balance[2] = {1.0f, 1.0f};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t message_cv = PTHREAD_COND_INITIALIZER;
static uint32_t messages[32]; static unsigned head, tail;
static uint64_t start_ns;
__attribute__((constructor)) static void started(void) { start_ns = runtime_now_ns(); }

void runtime_oe_push_message(uint32_t message) {
    pthread_mutex_lock(&lock);
    if (tail - head < 32) { messages[tail++ % 32] = message; pthread_cond_broadcast(&message_cv); }
    pthread_mutex_unlock(&lock);
}
static void get_balance(float *l, float *r) { pthread_mutex_lock(&lock); if (l) *l = balance[0]; if (r) *r = balance[1]; pthread_mutex_unlock(&lock); }
static void set_balance(float l, float r) {
    if (l < 0 || l > 1 || r < 0 || r > 1) runtime_fail("volume balance out of range");
    pthread_mutex_lock(&lock); balance[0] = l; balance[1] = r; pthread_mutex_unlock(&lock);
}
static int64_t total_active_time(void) { return (int64_t)(runtime_now_ns() - start_ns); }   /* TimeSpan in ns */
/* The host never sleeps the console; the extended inactivity window has no further effect. */
static void inactivity_extended(unsigned char on) { (void)on; }
/* No system startup logo is drawn on the host, so there is nothing to dismiss. */
static void finish_startup_logo(void) { fprintf(stderr, "[totk] game finished startup logo\n"); }
/* The single native executable was launched directly; it has no predecessor
 * in an applet ExecuteProgram chain. -1 keeps the game's normal user-open path. */
static int previous_program_index(void) { return -1; }
/* A stable, nonzero resource id for this process's one application. Host input
 * and audio share that application; there is no separate system applet client. */
static uint64_t applet_resource_user_id(void) { return 1; }
static unsigned char try_pop_launch_parameter(uint64_t *size,void *buffer,uint64_t capacity) {
    (void)buffer;(void)capacity;if(size)*size=0;return 0;
}
static unsigned char try_pop(uint32_t *out) {
    pthread_mutex_lock(&lock); int ok = head != tail; if (ok && out) *out = messages[head++ % 32]; pthread_mutex_unlock(&lock); return (unsigned char)ok;
}
int runtime_core_away(void); void runtime_core_back(int *away);
static uint32_t pop(void) {
    __attribute__((cleanup(runtime_core_back))) int away = runtime_core_away();   /* blocks like a Horizon wait */
    pthread_mutex_lock(&lock); while (head == tail) pthread_cond_wait(&message_cv, &lock);
    uint32_t m = messages[head++ % 32]; pthread_mutex_unlock(&lock); return m;
}

/* ---------- nn::prepo: play reports are not transmitted (no Nintendo servers) ---------- */
static uint64_t reports;
static unsigned report_save(const void *report) { (void)report; if (!(reports++ % 100)) fprintf(stderr, "[totk] play report kept local (not transmitted), count %llu\n", (unsigned long long)reports); return 0; }
static unsigned report_save_uid(const void *report, const void *uid) { (void)uid; return report_save(report); }
static unsigned immediate_transmission(void) { return 0; }
static unsigned connection_status(void *report, const char *a, const char *b, const char *c) { (void)report; (void)a; (void)b; (void)c; return 0; }

/* ---------- nn::nfp ---------- */
#define NFP_DEVICE_NOT_FOUND 0x8073u   /* nfp module 115, description 64 */
static int nfp_state;
static unsigned nfp_initialize(void) { nfp_state = 1; return 0; }
static unsigned nfp_attach_availability(void *event) { if (!event) runtime_fail("nfp availability event"); hle_bind_system_event(event, 0); return 0; }
static int nfp_get_state(void) { return nfp_state; }
#ifndef RUNTIME_AMIIBO
/* The host has no NFC reader. */
static unsigned nfp_finalize(void) { nfp_state = 0; return 0; }
static unsigned nfp_list(void *handles, int *count, int max) { (void)handles; (void)max; if (count) *count = 0; return NFP_DEVICE_NOT_FOUND; }
static unsigned nfp_no_device(void) { return NFP_DEVICE_NOT_FOUND; }
static int nfp_device_state(const void *h) { (void)h; return 0; }
#else
/* Breath of the Wild and Tears of the Kingdom on the Mac (RUNTIME_AMIIBO): one virtual reader. The Amiibo menu (frontend.m) puts a figure on it, known only by its 8-byte amiibo id
   (head and tail as AmiiboAPI lists them), as Ryujinx's virtual amiibo does: no dump and no amiibo keys. The game only reads
   figures (BotW 1.9.0 imports no nfp write call; TotK's calls were not checked against a running game), so the tag, model,
   common and register info are all it gets. No figure carries an
   application area, so Wolf Link reads as one never used with Twilight Princess HD (3 hearts). A figure stays on the reader
   until the game stops detection after it was seen, so each choice in the menu is one scan; the menu can also take it off.
   On the Mac a scan that starts with no figure waiting also shows the amiibo picker in the game window (frontend.m
   runtime_front_amiibo_picker; user 2026-11-10: "a pop up to choose the amiibo/cancel as it already does with the
   keyboard"), and the end of the scan closes it. */
#define NFP_WRONG_DEVICE_STATE 0x9273u   /* description 73 */
#define NFP_TAG_NOT_FOUND 0xc273u        /* description 97 */
#define NFP_NO_APPLICATION_AREA 0x10073u /* description 128 */
static const struct { uint64_t id; const char *name; } figures[] = {
    {0x0100000003530902ull, "Link - Archer"}, {0x0100000003540902ull, "Link - Rider"}, {0x0101000003560902ull, "Zelda"},
    {0x0140000003550902ull, "Guardian"}, {0x01410000035c0902ull, "Bokoblin"},
    {0x0105000003580902ull, "Daruk"}, {0x0106000003590902ull, "Urbosa"}, {0x01070000035a0902ull, "Mipha"}, {0x01080000035b0902ull, "Revali"},
    {0x01030000024f0902ull, "Midna & Wolf Link"},
    {0x01000000034b0902ull, "Link - Ocarina of Time"}, {0x01000000034c0902ull, "Link - Majora's Mask"},
    {0x01000000034d0902ull, "Link - Twilight Princess"}, {0x01000000034e0902ull, "Link - Skyward Sword"},
    {0x01000000034f0902ull, "8-Bit Link"}, {0x0100010003500902ull, "Toon Link - The Wind Waker"}, {0x0101000003520902ull, "Toon Zelda - The Wind Waker"},
    {0x0100000003990902ull, "Link - Link's Awakening"}, {0x0101030004140902ull, "Zelda & Loftwing"},
    {0x0100000004180902ull, "Link - Tears of the Kingdom"}, {0x0101000004190902ull, "Zelda - Tears of the Kingdom"},
    {0x01020100041a0902ull, "Ganondorf - Tears of the Kingdom"},
    {0x0100000000040002ull, "Link (Smash Bros.)"}, {0x01000000037c0002ull, "Young Link (Smash Bros.)"},
    {0x0100010000160002ull, "Toon Link (Smash Bros.)"}, {0x01010000000e0002ull, "Zelda (Smash Bros.)"},
    {0x0101010000170002ull, "Sheik (Smash Bros.)"}, {0x01020100001b0002ull, "Ganondorf (Smash Bros.)"},
};
enum { DEVICE_INIT, DEVICE_SEARCH, DEVICE_ACTIVE, DEVICE_DEACTIVE, DEVICE_MOUNT };   /* nn::nfp::DeviceState */
typedef struct { uint8_t uuid[10], uuid_length, reserved1[21]; uint32_t protocol, tag_type; uint8_t reserved2[48]; } TagInfo;
typedef struct { uint8_t character_id[2], character_variant, type; uint16_t model_number; uint8_t series, reserved[57]; } ModelInfo;
typedef struct { uint16_t year; uint8_t month, day; uint16_t write_counter, version; uint32_t application_area_size; uint8_t reserved[52]; } CommonInfo;
typedef struct { uint8_t mii_char_info[0x58]; uint16_t year; uint8_t month, day; char nickname[41]; uint8_t font_region, reserved[122]; } RegisterInfo;
_Static_assert(sizeof(TagInfo) == 0x58 && sizeof(ModelInfo) == 0x40 && sizeof(CommonInfo) == 0x40 && sizeof(RegisterInfo) == 0x100, "nn::nfp info layouts");
static pthread_mutex_t nfp_lock = PTHREAD_MUTEX_INITIALIZER;
static int nfp_device = DEVICE_INIT, nfp_figure = -1;   /* the figure on the reader, or queued for the next detection */
static void *activate_event, *deactivate_event;
int runtime_amiibo_count(void) { return (int)(sizeof figures / sizeof *figures); }
const char *runtime_amiibo_name(int i) { return i >= 0 && i < runtime_amiibo_count() ? figures[i].name : NULL; }
int runtime_amiibo_current(void) { pthread_mutex_lock(&nfp_lock); int f = nfp_figure; pthread_mutex_unlock(&nfp_lock); return f; }
/* Main thread (the menu). A figure already seen by the game is lifted first; the new one is seen at once if the game is searching. */
void runtime_amiibo_place(int i) {
    if (i >= runtime_amiibo_count()) i = -1;
    void *lift = NULL, *seen = NULL;
    pthread_mutex_lock(&nfp_lock);
    if (nfp_device == DEVICE_ACTIVE || nfp_device == DEVICE_MOUNT) { nfp_device = DEVICE_DEACTIVE; lift = deactivate_event; }
    nfp_figure = i;
    if (i >= 0 && nfp_device == DEVICE_SEARCH) { nfp_device = DEVICE_ACTIVE; seen = activate_event; }
    pthread_mutex_unlock(&nfp_lock);
    fprintf(stderr, i >= 0 ? "[amiibo] %s on the reader%s\n" : "[amiibo] reader empty\n", i >= 0 ? figures[i].name : "", seen ? "" : " (seen at the next scan)");
    if (lift) hle_signal_system_event(lift);
    if (seen) hle_signal_system_event(seen);
}
static unsigned nfp_finalize(void) {
    pthread_mutex_lock(&nfp_lock); nfp_state = 0; nfp_device = DEVICE_INIT; activate_event = deactivate_event = NULL; pthread_mutex_unlock(&nfp_lock); return 0;
}
static unsigned nfp_list(uint64_t *handles, int *count, int max) {
    if (!handles || !count) runtime_fail("nfp device list");
    if (max < 1) { *count = 0; return NFP_DEVICE_NOT_FOUND; }
    handles[0] = 1; *count = 1; return 0;
}
static unsigned nfp_npad_id(uint32_t *out, const void *h) { (void)h; if (!out) runtime_fail("nfp npad id"); *out = runtime_handheld_mode() ? 0x20u : 0u; return 0; }
static int nfp_device_state(const void *h) { (void)h; pthread_mutex_lock(&nfp_lock); int s = nfp_device; pthread_mutex_unlock(&nfp_lock); return s; }
/* Auto-clear, as the console's nfp events are. */
static unsigned nfp_attach_activate(void *event, const void *h) {
    (void)h; if (!event) runtime_fail("nfp activate event");
    hle_bind_system_event(event, 1); pthread_mutex_lock(&nfp_lock); activate_event = event; pthread_mutex_unlock(&nfp_lock); return 0;
}
static unsigned nfp_attach_deactivate(void *event, const void *h) {
    (void)h; if (!event) runtime_fail("nfp deactivate event");
    hle_bind_system_event(event, 1); pthread_mutex_lock(&nfp_lock); deactivate_event = event; pthread_mutex_unlock(&nfp_lock); return 0;
}
static unsigned nfp_start_detection(const void *h) {
    (void)h; void *seen = NULL;
    pthread_mutex_lock(&nfp_lock);
    nfp_device = nfp_figure >= 0 ? DEVICE_ACTIVE : DEVICE_SEARCH;
    if (nfp_figure >= 0) seen = activate_event;
    pthread_mutex_unlock(&nfp_lock);
    fprintf(stderr, "[amiibo] the game starts a scan%s\n", seen ? "; the waiting figure is seen" : "");
    if (seen) hle_signal_system_event(seen);
    else runtime_front_amiibo_picker(1);
    return 0;
}
static unsigned nfp_stop_detection(const void *h) {
    (void)h; pthread_mutex_lock(&nfp_lock);
    int scanned = nfp_device == DEVICE_ACTIVE || nfp_device == DEVICE_MOUNT;
    if (scanned) nfp_figure = -1;   /* this choice's one scan is done */
    nfp_device = DEVICE_INIT; pthread_mutex_unlock(&nfp_lock);
    fprintf(stderr, "[amiibo] the game stops the scan%s\n", scanned ? " (a figure was seen)" : "");
    runtime_front_amiibo_picker(0);
    return 0;
}
/* The figure's state under nfp_lock: the table index, or -1 with *result set when the device is not in a state that allows the call. */
static int nfp_figure_in(int need_mount, unsigned *result) {
    pthread_mutex_lock(&nfp_lock);
    int ok = need_mount ? nfp_device == DEVICE_MOUNT : nfp_device == DEVICE_ACTIVE || nfp_device == DEVICE_MOUNT;
    int f = ok ? nfp_figure : -1;
    pthread_mutex_unlock(&nfp_lock);
    *result = f >= 0 ? 0 : nfp_device == DEVICE_DEACTIVE ? NFP_TAG_NOT_FOUND : NFP_WRONG_DEVICE_STATE;
    return f;
}
static unsigned nfp_mount(const void *h, int model_type) {
    (void)h; (void)model_type; unsigned r = NFP_WRONG_DEVICE_STATE;
    pthread_mutex_lock(&nfp_lock);
    if (nfp_device == DEVICE_ACTIVE && nfp_figure >= 0) { nfp_device = DEVICE_MOUNT; r = 0; }
    int f = nfp_figure; pthread_mutex_unlock(&nfp_lock);
    if (!r) fprintf(stderr, "[amiibo] the game reads %s\n", figures[f].name);
    return r;
}
static unsigned nfp_mount_target(const void *h, int model_type, int target) { (void)target; return nfp_mount(h, model_type); }
static unsigned nfp_unmount(const void *h) {
    (void)h; pthread_mutex_lock(&nfp_lock);
    unsigned r = nfp_device == DEVICE_MOUNT ? 0 : NFP_WRONG_DEVICE_STATE;
    if (!r) nfp_device = DEVICE_ACTIVE;
    pthread_mutex_unlock(&nfp_lock); return r;
}
/* A stable 7-byte NTAG215 serial per figure (NXP's 0x04 first): the game tells figures apart by it, as by a real figure's. */
static unsigned nfp_tag_info(TagInfo *out, const void *h) {
    (void)h; unsigned r; int f = nfp_figure_in(0, &r); if (f < 0) return r;
    if (!out) runtime_fail("nfp tag info");
    memset(out, 0, sizeof *out);
    uint64_t x = figures[f].id ^ 0xcbf29ce484222325ull;
    for (int i = 0; i < 4; i++) x = (x ^ (x >> 29)) * 0x100000001b3ull;
    out->uuid[0] = 0x04; for (int i = 1; i < 7; i++) out->uuid[i] = (uint8_t)(x >> (8 * i));
    out->uuid_length = 7; out->protocol = 1; out->tag_type = 2;   /* NFC-A, type 2 tag */
    return 0;
}
/* The id's bytes as on the tag: character id (big-endian), variant, figure type, model number, series. */
static unsigned nfp_model_info(ModelInfo *out, const void *h) {
    (void)h; unsigned r; int f = nfp_figure_in(1, &r); if (f < 0) return r;
    if (!out) runtime_fail("nfp model info");
    uint64_t id = figures[f].id; memset(out, 0, sizeof *out);
    out->character_id[0] = (uint8_t)(id >> 56); out->character_id[1] = (uint8_t)(id >> 48);
    out->character_variant = (uint8_t)(id >> 40); out->type = (uint8_t)(id >> 32);
    out->model_number = (uint16_t)(id >> 16); out->series = (uint8_t)(id >> 8);
    return 0;
}
static void today(uint16_t *year, uint8_t *month, uint8_t *day) {
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    *year = (uint16_t)(tm.tm_year + 1900); *month = (uint8_t)(tm.tm_mon + 1); *day = (uint8_t)tm.tm_mday;
}
static unsigned nfp_common_info(CommonInfo *out, const void *h) {
    (void)h; unsigned r; int f = nfp_figure_in(1, &r); if (f < 0) return r;
    if (!out) runtime_fail("nfp common info");
    memset(out, 0, sizeof *out); today(&out->year, &out->month, &out->day); out->application_area_size = 0xd8;
    return 0;
}
/* Registered to no Mii (BotW draws none); the nickname is the figure's name, cut to the console's 10 characters. */
static unsigned nfp_register_info(RegisterInfo *out, const void *h) {
    (void)h; unsigned r; int f = nfp_figure_in(1, &r); if (f < 0) return r;
    if (!out) runtime_fail("nfp register info");
    memset(out, 0, sizeof *out); today(&out->year, &out->month, &out->day);
    snprintf(out->nickname, 11, "%s", figures[f].name);
    return 0;
}
static unsigned nfp_open_application_area(const void *h, uint32_t access_id) {
    unsigned r; int f = nfp_figure_in(1, &r); if (f < 0) return r;
    fprintf(stderr, "[amiibo] %s has no application area for %#x\n", figures[f].name, access_id); (void)h;
    return NFP_NO_APPLICATION_AREA;
}
static unsigned nfp_application_area(void *buffer, size_t *size, const void *h, size_t capacity) {
    (void)buffer; (void)h; (void)capacity; if (size) *size = 0;
    unsigned r; return nfp_figure_in(1, &r) < 0 ? r : NFP_NO_APPLICATION_AREA;
}
#endif

/* ---------- nn::err (nn::swkbd is in runtime/swkbd_host.c) ---------- */
static void show_error(uint32_t result) {
    fprintf(stderr, "[totk] game error dialog: %04u-%04u (result %#x)\n", 2000 + (result & 0x1ff), (result >> 9) & 0x1fff, result);
}

#ifdef GAME_BOTW
/* SDK 7 (Breath of the Wild 1.6.0, botw.md). Exit requests are handled by the window (the section brackets change nothing);
   video recording has no Mac counterpart; the DLC shop and the DLC-version error screen are console applets. */
static void exit_section(void) {}
static void enable_recording(void) {}
static void unacceptable_aoc_version(void) { fprintf(stderr, "[botw] the game reported a DLC version it cannot use\n"); }
static uint32_t show_aoc_shop(uint64_t application, const void *user) { (void)application; (void)user; fprintf(stderr, "[botw] eShop DLC page: not available\n"); return 0; }
#endif
void *runtime_applet_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn6applet23GetPreviousProgramIndexEv", previous_program_index},
        {"_ZN2nn6applet23GetAppletResourceUserIdEv", applet_resource_user_id},
        {"_ZN2nn2am23GetAppletResourceUserIdEv", applet_resource_user_id},
        {"_ZN2nn2oe21TryPopLaunchParameterEPmPvm", try_pop_launch_parameter},
        {"_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", get_balance}, {"_ZN2nn2oe24SetExpectedVolumeBalanceEff", set_balance},
        {"_ZN2nn2oe25GetProgramTotalActiveTimeEv", total_active_time}, {"_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", inactivity_extended},
        {"_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb", inactivity_extended},
        {"_ZN2nn2oe17FinishStartupLogoEv", finish_startup_logo}, {"_ZN2nn2oe25TryPopNotificationMessageEPj", try_pop},
        {"_ZN2nn2oe22PopNotificationMessageEv", pop},
        {"_ZN2nn5prepo10PlayReport4SaveEv", report_save}, {"_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", report_save_uid},
        {"_ZN2nn5prepo28RequestImmediateTransmissionEv", immediate_transmission},
        {"_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", connection_status},
        {"_ZN2nn3nfp10InitializeEv", nfp_initialize}, {"_ZN2nn3nfp8FinalizeEv", nfp_finalize}, {"_ZN2nn3nfp8GetStateEv", nfp_get_state},
        {"_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", nfp_list},
        {"_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", nfp_attach_availability},
        {"_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", nfp_device_state},
#ifdef RUNTIME_AMIIBO
        {"_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", nfp_npad_id},
        {"_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_attach_activate},
        {"_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_attach_deactivate},
        {"_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", nfp_start_detection}, {"_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", nfp_stop_detection},
        {"_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeE", nfp_mount},
        {"_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", nfp_mount_target},
        {"_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE", nfp_unmount},
        {"_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", nfp_tag_info},
        {"_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", nfp_model_info},
        {"_ZN2nn3nfp13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE", nfp_common_info},
        {"_ZN2nn3nfp15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE", nfp_register_info},
        {"_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj", nfp_open_application_area},
        {"_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm", nfp_application_area},
#else
        {"_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", nfp_no_device},
        {"_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", nfp_no_device}, {"_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", nfp_no_device},
#endif
        {"_ZN2nn3err9ShowErrorENS_6ResultE", show_error},
#ifdef GAME_BOTW
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", exit_section}, {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", exit_section},
        {"_ZN2nn2oe15EnableRecordingEv", enable_recording},
        {"_ZN2nn3err40ShowUnacceptableAddOnContentVersionErrorEv", unacceptable_aoc_version},
        {"_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", show_aoc_shop},
#endif
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
