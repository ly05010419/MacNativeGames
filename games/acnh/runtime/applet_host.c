/* Host side of applet, telemetry, NFC and error-display services that main reaches through IPC.
   Client-side SDK code (PlayReport encoding, keyboard presets, diag logging) runs unchanged; only the
   calls that would talk to system services are taken here. */
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---------- nn::nfp: the host has no NFC reader ---------- */
#define NFP_DEVICE_NOT_FOUND 0x8073u   /* nfp module 115, description 64 */
static int nfp_state;
static unsigned nfp_initialize(void) { nfp_state = 1; return 0; }
static unsigned nfp_finalize(void) { nfp_state = 0; return 0; }
static int nfp_get_state(void) { return nfp_state; }
static unsigned nfp_list(void *handles, int *count, int max) { (void)handles; (void)max; if (count) *count = 0; return NFP_DEVICE_NOT_FOUND; }
static unsigned nfp_attach_availability(void *event) { if (!event) runtime_fail("nfp availability event"); hle_bind_system_event(event, 0); return 0; }
static unsigned nfp_no_device(void) { return NFP_DEVICE_NOT_FOUND; }
static int nfp_device_state(const void *h) { (void)h; return 0; }

/* ---------- nn::err (nn::swkbd is in runtime/swkbd_host.c) ---------- */
static void show_error(uint32_t result) {
    fprintf(stderr, "[totk] game error dialog: %04u-%04u (result %#x)\n", 2000 + (result & 0x1ff), (result >> 9) & 0x1fff, result);
}

static void exit_section(void) {}
static void enable_recording(void) {}
static void oe_noop(void) {}
static void get_pseudo_device_id(uint8_t *uuid) {
    static const uint8_t k[16] = {0x78, 0x62, 0x6c, 0x00, 0x53, 0x57, 0x4e, 0x41, 0x54, 0x49, 0x56, 0x45, 0x31, 0x30, 0x30, 0x31};
    if (uuid) memcpy(uuid, k, 16);
}
static int get_region_code(void) {
    const char *e = getenv("DM_LANG"); if (!e || !*e) e = "en-US";
    return !strncmp(e, "ja", 2) ? 0 : !strncmp(e, "ko", 2) || !strncmp(e, "zh", 2) ? 4 : !strcmp(e, "en-US") || !strcmp(e, "fr-CA") || !strcmp(e, "es-419") || !strcmp(e, "pt-BR") ? 1 : 2;
}
static uint64_t desired_language(void) { return runtime_desired_language(); }
static int get_op_mode(void) { return 1; }
static int get_perf_mode(void) { return 1; }
static int get_focus_state(void) { return 1; }

/* ---------- nn::web: the host has no browser applet ----------
   Layouts from the 1.3.0 sdk: ShowOfflineHtmlPageArg starts with a 0x2000-byte list (u16 count at +0, entries from +8,
   each u16 kind, u16 size, u32 0, then size bytes; the document path is kind 6, SetDocumentPath at sdk+0x3b5f10).
   OfflineHtmlPageReturnValue is 0x1010 bytes: exit reason at +0 (constructor stores 20, GetOfflineExitReason reads a
   32-bit value), last URL at +8 and its size at +0x1008. main's one caller (main+0x6cb5b4) treats a nonzero Result as
   failure and reads the last URL only for exit reason 3, so the page reports success, closed by the player (1). */
#define WEB_ARG_SIZE 0x2000u
#define WEB_DOCUMENT_PATH 6
#define WEB_EXIT_BACK_BUTTON 1
static unsigned show_offline_html_page(void *out, const void *arg) {
    const uint8_t *a = arg; const char *path = NULL; int path_len = 0;
    unsigned count = a ? (unsigned)(a[0] | a[1] << 8) : 0;
    for (size_t at = 8; count-- && at + 8 <= WEB_ARG_SIZE;) {
        unsigned kind = a[at] | a[at + 1] << 8, size = a[at + 2] | a[at + 3] << 8;
        if (at + 8 + size > WEB_ARG_SIZE) break;
        if (kind == WEB_DOCUMENT_PATH) { path = (const char *)a + at + 8; path_len = (int)strnlen(path, size); break; }
        at += 8 + size;
    }
    fprintf(stderr, "[totk] offline web page \"%.*s\" requested; no browser applet on the host, reported closed\n", path_len, path ? path : "");
    if (!out) runtime_fail("offline web page without a return value");
    uint8_t *r = out; memset(r, 0, 0x1010); uint64_t reason = WEB_EXIT_BACK_BUTTON; memcpy(r, &reason, sizeof reason);
    return 0;
}

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
        {"_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", nfp_no_device},
        {"_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", nfp_no_device}, {"_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3err9ShowErrorENS_6ResultE", show_error},
        {"_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE", show_offline_html_page},
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", exit_section},
        {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", exit_section},
        {"_ZN2nn2oe15EnableRecordingEv", enable_recording},
        {"_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", get_pseudo_device_id},
        {"_ZN2nn2oe13GetRegionCodeEv", get_region_code},
        {"_ZN2nn2oe18GetDesiredLanguageEv", desired_language},
        {"_ZN2nn2oe16GetOperationModeEv", get_op_mode},
        {"_ZN2nn2oe18GetPerformanceModeEv", get_perf_mode},
        {"_ZN2nn2oe20GetCurrentFocusStateEv", get_focus_state},
        {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", oe_noop},
        {"_ZN2nn2oe28SetResumeNotificationEnabledEb", oe_noop},
        {"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", oe_noop},
        {"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", oe_noop},
        {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", oe_noop},
        {"_ZN2nn2oe10InitializeEv", oe_noop},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
