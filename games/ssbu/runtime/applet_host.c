/* Host side of applet, telemetry, NFC and error-display services that main reaches through IPC.
   Client-side SDK code (PlayReport encoding, keyboard presets, diag logging) runs unchanged; only the
   calls that would talk to system services are taken here. */
#include "runtime.h"
#include "game_version.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/* ---------- nn::oe ---------- */
static float balance[2] = {1.0f, 1.0f};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t message_cv = PTHREAD_COND_INITIALIZER;
static uint32_t messages[32]; static unsigned head, tail;
static uint64_t start_ns;
__attribute__((constructor)) static void started(void) { start_ns = runtime_now_ns(); }

/* nn::oe::GetNotificationMessageEvent (Super Smash Bros. Ultimate 13.0.2): a thread of the game (main+0x3754540) waits on it
   and then pops the messages. The SDK sets its own event up in nn::oe::Initialize, which the host answers (services.c), so
   the SDK's stayed uninitialized and WaitSystemEvent asserted. The host hands out an event of its own instead: bound on the
   first call (that is a guest thread, as binding needs), auto-clear, signaled for every queued message. */
static uint8_t message_event[0x100] __attribute__((aligned(16)));
static int message_event_ready;
void runtime_oe_push_message(uint32_t message) {
    pthread_mutex_lock(&lock);
    int queued = tail - head < 32;
    if (queued) { messages[tail++ % 32] = message; pthread_cond_broadcast(&message_cv); }
    int signal = queued && message_event_ready;
    pthread_mutex_unlock(&lock);
    if (signal) hle_signal_system_event(message_event);
}
static void *notification_event(void) {
    pthread_mutex_lock(&lock); int first = !message_event_ready; int pending = head != tail; pthread_mutex_unlock(&lock);
    if (first) {
        hle_bind_system_event(message_event, 1);
        pthread_mutex_lock(&lock); message_event_ready = 1; pending = head != tail; pthread_mutex_unlock(&lock);
        if (pending) hle_signal_system_event(message_event);
        fprintf(stderr, "[ssbu] oe notification message event bound (%s)\n", pending ? "a message is waiting" : "no message yet");
    }
    return message_event;
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

/* SDK 7 (Breath of the Wild 1.6.0, botw.md). Exit requests are handled by the window (the section brackets change nothing);
   video recording has no Mac counterpart; the DLC shop and the DLC-version error screen are console applets. */
static void exit_section(void) {}
static void enable_recording(void) {}
static void unacceptable_aoc_version(void) { fprintf(stderr, "[botw] the game reported a DLC version it cannot use\n"); }
static uint32_t show_aoc_shop(uint64_t application, const void *user) { (void)application; (void)user; fprintf(stderr, "[botw] eShop DLC page: not available\n"); return 0; }
/* A game asking to start itself again is a restart (relaunch.c, Q74). Smash calls nn::oe::RequestToRelaunchApplication()
   after its language changes; in the SDK that goes on to nn::applet::RequestToRelaunchApplication(storage), which
   calls nn::applet::RequestToLaunchApplication(0 = this program, storage) (sdk+0x80420), so both are answered here.
   The storage (launch parameters for the new instance) is not passed on; the new instance starts as from the menu.
   Another program cannot start on the Mac and stops with its id. A Result in the SDK, which a restart never returns. */
static void relaunch_application(void) {
    fprintf(stderr, "[ssbu] applet: the game asks to start itself again\n");
    runtime_relaunch("RequestToRelaunchApplication");
}
static uint32_t launch_application(uint64_t application, uint64_t storage) {
    if (application && application != GAME_TITLE_ID) {
        fprintf(stderr, "[ssbu] applet: asked to launch application %016llx\n", (unsigned long long)application);
        runtime_fail("nn::applet::RequestToLaunchApplication for another program");
    }
    fprintf(stderr, "[ssbu] applet: the game asks to start itself again (launch storage %#llx not passed on)\n",
            (unsigned long long)storage);
    runtime_relaunch("RequestToLaunchApplication");
}
void *runtime_applet_override(const char *name) {
    static const struct { const char *name; void *fn; } map[] = {
        {"_ZN2nn2oe28RequestToRelaunchApplicationEv", relaunch_application},
        {"_ZN2nn6applet26RequestToLaunchApplicationENS_3ncm13ApplicationIdENS0_13StorageHandleE", launch_application},
        {"_ZN2nn6applet23GetPreviousProgramIndexEv", previous_program_index},
        {"_ZN2nn6applet23GetAppletResourceUserIdEv", applet_resource_user_id},
        {"_ZN2nn2am23GetAppletResourceUserIdEv", applet_resource_user_id},
        {"_ZN2nn2oe21TryPopLaunchParameterEPmPvm", try_pop_launch_parameter},
        {"_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", get_balance}, {"_ZN2nn2oe24SetExpectedVolumeBalanceEff", set_balance},
        {"_ZN2nn2oe25GetProgramTotalActiveTimeEv", total_active_time}, {"_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", inactivity_extended},
        {"_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb", inactivity_extended},
        {"_ZN2nn2oe17FinishStartupLogoEv", finish_startup_logo}, {"_ZN2nn2oe25TryPopNotificationMessageEPj", try_pop},
        {"_ZN2nn2oe22PopNotificationMessageEv", pop}, {"_ZN2nn2oe27GetNotificationMessageEventEv", notification_event},
        {"_ZN2nn5prepo10PlayReport4SaveEv", report_save}, {"_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", report_save_uid},
        {"_ZN2nn5prepo28RequestImmediateTransmissionEv", immediate_transmission},
        {"_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", connection_status},
        {"_ZN2nn3nfp10InitializeEv", nfp_initialize}, {"_ZN2nn3nfp8FinalizeEv", nfp_finalize}, {"_ZN2nn3nfp8GetStateEv", nfp_get_state},
        {"_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", nfp_list},
        {"_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", nfp_attach_availability},
        {"_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", nfp_device_state},
        {"_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", nfp_no_device},
        {"_ZN2nn3nfp7UnmountERKNS0_12DeviceHandleE", nfp_no_device},   // Super Mario Odyssey imports it (main+0x6cfa84); no device ever mounts
        {"_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp19AttachActivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp21AttachDeactivateEventEPNS_2os15SystemEventTypeERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", nfp_no_device}, {"_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", nfp_no_device},
        {"_ZN2nn3err9ShowErrorENS_6ResultE", show_error},
        {"_ZN2nn2oe31EnterExitRequestHandlingSectionEv", exit_section}, {"_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", exit_section},
        {"_ZN2nn2oe15EnableRecordingEv", enable_recording},
        {"_ZN2nn3err40ShowUnacceptableAddOnContentVersionErrorEv", unacceptable_aoc_version},
        {"_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", show_aoc_shop},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(name, map[i].name)) return map[i].fn;
    return NULL;
}
