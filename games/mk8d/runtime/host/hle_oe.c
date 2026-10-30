// nn::oe (application operating environment): on the real device the console state is queried through the am/oe services; the host gives fixed state directly.
// oe::Initialize is in hle_sdkinit.c, not here.
// Environment switches: DM_OPMODE=0 handheld / 1 console (default: the profile's op_mode, console unless set; console corresponds to the console-mode render profile); DM_LANG=language code (default en-US; the iOS app sets it from the IPA's DMLanguage, main.m).
#include "hle.h"

// The operation mode is runtime state (the front end can change it: iOS overheat downgrade switches to handheld): the single source is here, taken once from DM_OPMODE at startup, the environment variable is not read again afterwards
#include <stdatomic.h>
static _Atomic int g_opmode = -1;
int dm_opmode(void) {
    int m = g_opmode;
    if (m < 0) { const char *e = getenv("DM_OPMODE"); int v = e ? atoi(e) : DM_GAME_OPMODE; int exp = -1; atomic_compare_exchange_strong(&g_opmode, &exp, v); m = g_opmode; }
    return m;
}
// ---------- System notification queue (AppletMessage): front end state changes → fetched by the game every frame with TryPopNotificationMessage ----------
// Message numbers the host sends: 15 focus change, 30 operation mode change, 31 performance mode change
enum { MSG_FOCUS = 15, MSG_OPMODE = 30, MSG_PERFMODE = 31 };
#define MQ 32
static pthread_mutex_t g_mq_lock = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_mq_cond = PTHREAD_COND_INITIALIZER;   // cond: wakes the blocking PopNotificationMessage
static uint32_t g_mq[MQ]; static int g_mqh, g_mqn;
static _Atomic int g_ntf_opmode, g_ntf_perfmode;          // only pushed after the game turns it on with Set*NotificationEnabled
// Notification event (Hades main+0x3ed18c polls it every frame and pops one message each time it fires): the SDK's version returns the event oe::Initialize would have
// created, and the host's oe::Initialize is a no-op, so TryWaitSystemEvent asserted on it. A host-owned auto-clear SystemEvent instead, which follows the queue as the am service's
// does: signalled while a notification is queued, cleared when the last one is popped. Monster Hunter Rise's MessageSuperviseThread (main+0x6bc8fc4) waits on it with WaitAny,
// which does not clear it, then pops until the queue is empty; when the event was only ever signalled, the first focus change on iOS left it set and the thread spun on a core
// (995 ms/s, 5-6 W on an iPhone). The count is read and the event set or cleared under g_mq_lock, so a push racing a pop cannot leave it cleared over a queued notification
// (lock order g_mq_lock → G → g_se_lock; nothing takes g_mq_lock while holding G)
static uint8_t g_mq_ev[0x80] __attribute__((aligned(16)));
static _Atomic int g_mq_ev_on;
static void mq_ev_sync(void) {
    if (!atomic_load(&g_mq_ev_on)) return;
    pthread_mutex_lock(&g_mq_lock);
    if (g_mqn) hle_signal_system_event(g_mq_ev); else hle_clear_system_event(g_mq_ev);
    pthread_mutex_unlock(&g_mq_lock);
}
static void mq_push_(uint32_t m);
static void mq_push(uint32_t m) { mq_push_(m); mq_ev_sync(); }
static void mq_push_(uint32_t m) {
    pthread_mutex_lock(&g_mq_lock);
    // 15/30/31 are invalidation notifications meaning "query the current state", not events carrying historical values.
    // While one of the same type has not been taken yet, the first queue position is kept: a consumer loop only reads the latest state, repeated notifications write back the same thing, and with the same size there is no further resize.
    // Only these three state notifications are merged; future per-occurrence events (such as Resume/Exit) must not use this rule.
    // Legitimate producers only send these three, so at most three items are pending, and any number of focus losses/restores no longer hits the 32-slot limit; after a pop, new changes are enqueued as usual.
    if (m == MSG_FOCUS || m == MSG_OPMODE || m == MSG_PERFMODE)
        for (int i = 0; i < g_mqn; i++)
            if (g_mq[(g_mqh + i) % MQ] == m) { pthread_mutex_unlock(&g_mq_lock); return; }
    if (g_mqn == MQ) die("oe notification queue overflow (is the game not popping?)");
    g_mq[(g_mqh + g_mqn++) % MQ] = m;
    pthread_cond_broadcast(&g_mq_cond);
    pthread_mutex_unlock(&g_mq_lock);
}
void dm_set_opmode(int m) {   // called by the front end (overheat downgrade etc.): change the state and push 30 / 31 according to the switches the game has turned on
    int old = dm_opmode(); if (old == m) return;
    g_opmode = m;
    LOG("oe operation mode %d → %d", old, m);
    if (g_ntf_opmode) mq_push(MSG_OPMODE);
    if (g_ntf_perfmode) mq_push(MSG_PERFMODE);
}
// FocusState: 1 InFocus, 2 OutOfFocus, 3 Background. The front end calls dm_focus_set (iOS lifecycle / Mac window losing focus)
static _Atomic int g_focus = 1; static _Atomic uint64_t g_focus_in_ns;   // time of the most recent entry into InFocus
// Focus handling mode the game set (-1: none). The SDK's oe::SetFocusHandlingMode (SDK 18 sdk+0x3c5770) turns it into am's SetFocusHandlingMode(notify, ?, suspend) and
// SetOutOfFocusSuspendingEnabled: 0 → (0, 0, 1), 1 → (1, 1, 0), 2 → (1, 0, 1), 3 → (0, 0, 1) + out-of-focus suspending. Only 1 and 2 ask for focus notifications; under 0 and 3
// the system suspends the game out of focus and it never sees message 15 (the host's suspend is the guest clock freeze and the graphics gate). Super Mario Galaxy 2 sets 3, and
// its Wii layer's pause on focus 2 (main+0x663ba0) never finished: the main loop waits for the GX thread to pause (main+0x5ce870), which waits for work the paused CPU never sends
static _Atomic int g_focus_mode = -1;
static int focus_notifies(void) { int m = atomic_load(&g_focus_mode); return m != 0 && m != 3; }   // no mode set: notify, as before
int dm_focus_now(void) { return atomic_load(&g_focus); }
uint64_t dm_focus_in_since(void) { return atomic_load(&g_focus_in_ns); }   // graphics layer display link watchdog: stalls are only computed while active, counted from activation (nvn_mtl.m mtl_present_wait)
void dm_focus_set(int st) {
    if (st < 1 || st > 3) die("dm_focus_set: invalid state %d", st);
    if (st == 1) atomic_store(&g_focus_in_ns, now_ns());
    if (atomic_exchange(&g_focus, st) == st) return;
    LOG("oe focus state → %d", st);
    if (focus_notifies()) mq_push(MSG_FOCUS);
}
static int opmode(void) { return dm_opmode(); }

// Screenshot / video recording switches: the host has no system screenshot service, no-op
HLE_FORCE("_ZN2nn2oe17FinishStartupLogoEv", "dismiss the system boot screen (through the am application proxy); the host has no boot screen", void, h_oe_FinishLogo, void) {}   // Metroid Dread calls it at main+0xadfed0
// am retains the requested inactivity extension even when auto-sleep is disabled.
// SDK 12.3.9 maps the normal setter to mode 1, Unsafe to mode 2, and false to 0.
// Xenoblade Chronicles 3 reads it after the opening movie (sdk+0x3391f0).
static _Atomic int g_inactivity_mode;
HLE_FORCE("_ZN2nn2oe44SetUserInactivityDetectionTimeExtendedUnsafeEb", "inactivity extension mode 2 or 0; the host never turns the screen off", void, h_oe_InactivityExt, unsigned on) { g_inactivity_mode = (on & 1) ? 2 : 0; }
HLE_FORCE("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", "inactivity extension mode 1 or 0; the host never turns the screen off", void, h_oe_InactivityExtSafe, unsigned on) { g_inactivity_mode = on & 1; }
HLE_FORCE("_ZN2nn2oe43GetUserInactivityDetectionTimeExtensionModeEv", "inactivity extension mode requested by the application", int, h_oe_InactivityMode, void) { return g_inactivity_mode; }
HLE_FORCE("_ZN2nn2oe15EnableRecordingEv", "video recording switch; the host has no system recording", void, h_oe_EnableRec, void) {}
// Applet resource user ID (ARUID, the number am assigns to this process): the host has no am, and the SDK's own getter returns 0. SDK 20's OpenAudioRenderer (the CPU rendering + manual execution path, sdk+0x221730)
//   aborts when it gets 0 (The Legend of Zelda: Breath of the Wild 1.9.0); SDK 16 did not check. Report a fixed non-zero ID, used only as an identifier
HLE_FORCE("_ZN2nn6applet23GetAppletResourceUserIdEv", "applet resource user ID; the host returns a fixed non-zero value", uint64_t, h_applet_Aruid, void) { return 0x80; }
// Launch parameter (present only when another program launched this one with one; returns bool): the host never passes one
HLE_FORCE("_ZN2nn2oe21TryPopLaunchParameterEPmPvm", "launch parameter: none", unsigned, h_oe_PopLaunchParam, size_t *out, void *buf, size_t n) { (void)buf; (void)n; if (out) *out = 0; return 0; }
// Tears of the Kingdom (SDK 20.5.22): the index of the program that launched this one in a multi-program application. The SDK asks the applet proxy, which the host never opens
// (null read in applet::GetPreviousProgramIndex right after the window setup); -1 = launched from the home menu
HLE_FORCE("_ZN2nn2oe23GetPreviousProgramIndexEv", "previous program index: launched from HOME, -1", int, h_oe_PrevProgram, void) { return -1; }
// Launch another program of a multi-program application (Octopath Traveler 0 imports it); on hardware it does not return. The host has only this program: logged, and the game carries on
HLE_FORCE("_ZN2nn2oe14ExecuteProgramEiPKvm", "launch another program of the application: the host runs only this one (logged)", void, h_oe_ExecProgram, int index, const void *param, size_t n) {
    (void)param; LOG("[RED] oe::ExecuteProgram(%d, %zu bytes of parameters): the host runs only this program; the game carries on", index, n);
}
HLE_FORCE("_ZN2nn2oe16DisableRecordingEv", "video recording switch; the host has no system recording", void, h_oe_DisableRec, void) {}
// Silence the game's sound in system video captures (through am; FINAL FANTASY RESONANCE imports both): the host records nothing
HLE_FORCE("_ZN2nn2oe13MuteRecordingEv", "mute the sound of system video recording; the host has no system recording", void, h_oe_MuteRec, void) {}
HLE_FORCE("_ZN2nn2oe15UnmuteRecordingEv", "unmute the sound of system video recording; the host has no system recording", void, h_oe_UnmuteRec, void) {}
HLE_FORCE("_ZN2nn2oe16EnableScreenShotEv", "screenshot switch; the host has no system screenshot", void, h_oe_EnableShot, void) {}
HLE_FORCE("_ZN2nn2oe17DisableScreenShotEv", "screenshot switch; the host has no system screenshot", void, h_oe_DisableShot, void) {}
// Super Mario Odyssey: HOME button blocking around its own menus, screenshot orientation, and the auto-sleep query (all through am)
HLE_FORCE("_ZN2nn2oe23BeginBlockingHomeButtonEv", "temporarily block the HOME button (through am); the host has no HOME button", void, h_oe_BlockHome, void) {}
HLE_FORCE("_ZN2nn2oe21EndBlockingHomeButtonEv", "restore the HOME button (through am)", void, h_oe_UnblockHome, void) {}
HLE_FORCE("_ZN2nn2oe29SetScreenShotImageOrientationENS_5album16ImageOrientationE", "screenshot orientation (through am); the host has no system screenshot", void, h_oe_ShotOrient, int o) { (void)o; }
// DRAGON QUEST XI S: auto-sleep disabled sections (through am). The host never sleeps the device; the query reports whether a section is open, as the applet does
static _Atomic int g_nosleep;
HLE_FORCE("_ZN2nn2oe29BeginAutoSleepDisabledSectionEv", "open an auto-sleep disabled section (through am); the host never auto-sleeps, counted for the query", void, h_oe_NoSleepBegin, void) { g_nosleep++; }
HLE_FORCE("_ZN2nn2oe27EndAutoSleepDisabledSectionEv", "close an auto-sleep disabled section (through am)", void, h_oe_NoSleepEnd, void) { if (g_nosleep > 0) g_nosleep--; }
HLE_FORCE("_ZN2nn2oe19IsAutoSleepDisabledEv", "whether an auto-sleep disabled section is open", unsigned, h_oe_NoSleepQ, void) { return g_nosleep > 0; }
HLE_FORCE("_ZN2nn2oe37IsUserInactivityDetectionTimeExtendedEv", "whether the application requested an inactivity extension", unsigned, h_oe_InactivityExtQ, void) { return g_inactivity_mode != 0; }
// Hyrule Warriors: Age of Calamity (SDK 9.6.0, main+0x8d1238): the copyright frame buffer, an overlay the system draws only into screenshots and recordings. The SDK hands the buffer to am as transfer memory
// (svcCreateTransferMemory, then a null applet proxy). The host has no system capture, so it keeps nothing
HLE_FORCE("_ZN2nn2oe30InitializeCopyrightFrameBufferEPvm", "copyright overlay buffer (for screenshots / recording, through am); the host has no system screenshot", void, h_oe_CopyrightInit, void *buf, size_t n) { (void)buf; (void)n; }
HLE_FORCE("_ZN2nn2oe17SetCopyrightImageEPKvmiiiiNS0_16WindowOriginModeE", "copyright overlay image; the host has no system screenshot", void, h_oe_CopyrightImage, const void *p, size_t n, int x, int y, int w, int h, int origin) { (void)p; (void)n; (void)x; (void)y; (void)w; (void)h; (void)origin; }
HLE_FORCE("_ZN2nn2oe22SetCopyrightVisibilityEb", "copyright overlay visibility; the host has no system screenshot", void, h_oe_CopyrightVisible, int on) { (void)on; }

// OperationMode: 0 Handheld, 1 Console; PerformanceMode: 0 Normal, 1 Boost (one-to-one with the operation mode)
HLE_FORCE("_ZN2nn2oe16GetOperationModeEv", "operation mode query; a fixed value from DM_OPMODE", int, h_oe_GetOpMode, void) { return opmode(); }
// Profile perf_mode boost: Boost whatever the operation mode, a combination a Switch 1 never reports, so a profile choice and not a default. Rhythm Heaven Groove keeps one flag per mode
//   (main+0xa764 / main+0xa784): console means a TV, which gets a latency calibration (main+0x20d0, scene script main+0x4c0820), and Boost alone picks its 1920x1080 draw size
//   (table main+0xada6d0, else 1280x720). Handheld with Boost: no TV calibration on a phone, full-size drawing
HLE_FORCE("_ZN2nn2oe18GetPerformanceModeEv", "performance mode query; same as the operation mode (profile perf_mode boost: Boost)", int, h_oe_GetPerfMode, void) { return DM_GAME_PERF_BOOST ? 1 : opmode(); }
// SDK 11 calls of Monster Hunter Rise: the CPU overclock query reads an apm session the host never opened (null read in sdk+0x35258 at the start of nnMain, main+0x6bc8650);
// the default display resolution is the TV's (1080p docked) or the panel's (720p handheld) and only changes with the operation mode
// Taiko Risshiden V DX (SDK 12.3.3) turns it on in nnMain (main+0x2e1e4c): the setter goes through the same apm session (null read at sdk+0x362b8). The host does not scale clocks;
// the query reports what was set, as apm does
static unsigned char g_cpu_oc;
HLE_FORCE("_ZN2nn2oe21IsCpuOverclockEnabledEv", "CPU overclock state (through apm): what SetCpuOverclockEnabled set, off at start", unsigned, h_oe_CpuOc, void) { return g_cpu_oc; }
HLE_FORCE("_ZN2nn2oe22SetCpuOverclockEnabledEb", "CPU overclock switch (through apm); the host does not scale clocks", void, h_oe_SetCpuOc, unsigned on) { g_cpu_oc = on & 0xff; }
HLE_FORCE("_ZN2nn2oe27GetDefaultDisplayResolutionEPiS1_", "default display resolution: 1920x1080 docked, 1280x720 handheld", void, h_oe_DefRes, int *w, int *h) {
    int con = opmode() == 1; if (w) *w = con ? 1920 : 1280; if (h) *h = con ? 1080 : 720;
}
HLE_FORCE("_ZN2nn2oe38GetDefaultDisplayResolutionChangeEventEPNS_2os11SystemEventE", "default display resolution change event: bound, never signaled (the operation mode notification carries the change)",
          void, h_oe_DefResEv, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn2oe40SendServerMaintenanceOverlayNotificationENS_4time9PosixTimeES2_", "server maintenance overlay (through am); the host has no system overlay", void, h_oe_MaintOverlay, int64_t a, int64_t b) { (void)a; (void)b; }
// apm performance configurations: Normal 0x00010000 (CPU 1020 / GPU 384 / EMC 1600 MHz, the handheld default), Boost 0x00010001 (GPU 768 MHz, the docked default)
HLE_FORCE("_ZN2nn2oe27GetPerformanceConfigurationENS0_15PerformanceModeE", "query the CPU/GPU clock config (through apm); reports the default for the mode", int, h_oe_GetPerfCfg, int mode) { return mode == 1 ? 0x00010001 : 0x00010000; }
HLE_FORCE("_ZN2nn2oe24SetAlbumImageOrientationENS_5album16ImageOrientationE", "screenshot orientation (through am); the host has no system screenshot", void, h_oe_AlbumOrient, int o) { (void)o; }
// The Great Ace Attorney Chronicles (SDK 10), first thing in nnMain (main+0x6b188c): the SDK's version asks am through the applet proxy the host never made (null read at sdk+0x338070)
HLE_FORCE("_ZN2nn2oe34DisableAlbumImageTakenNotificationEv", "screenshot-taken notifications (through am); the host takes no system screenshots", void, h_oe_AlbumNotifyOff, void) {}
// Enable uses the same am application proxy (SDK 13.3.5 sdk+0x36ec50); the host produces no system-album notifications.
// Xenoblade Chronicles 3 (SDK 12.3.9) enables this at main+0x127fac4;
// the SDK otherwise dereferences the absent am SelfController at sdk+0x33b0f0.
// As with Disable, there is no producer: the host has no Horizon album captures.
HLE_FORCE("_ZN2nn2oe33EnableAlbumImageTakenNotificationEv", "screenshot-taken notifications (through am); the host takes no system screenshots", void, h_oe_AlbumNotifyOn, void) {}
HLE_FORCE("_ZN2nn2oe24IsInStoreDemoModeEnabledEv", "retail store demo mode (through am); never", unsigned, h_oe_StoreDemo, void) { return 0; }
HLE_FORCE("_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", "set the CPU/GPU clock config; the host does not scale clocks", void, h_oe_SetPerfCfg, int mode, int cfg) {
    LOG("oe::SetPerformanceConfiguration mode=%d cfg=%#x (ignored)", mode, cfg);
}

// Scripted state changes (verify lifecycle / overheat downgrade without a window): DM_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30" (second N); bg=1 / bg=0 run the whole background round trip (see script_bg)
// Time counts from the game's first notification fetch. Coexists with front end calls.
#include <unistd.h>
// bg=1 / bg=0: the whole "to background / back to foreground" sequence, in the same order as the scene lifecycle in front_ios.m (resign active -> enter background; enter foreground -> become active), minus the audio session reset iOS does on returning.
// Used to reproduce "after going to the background and back" problems on the Mac (audio take-over and resume, guest clock freeze, graphics suspend); to imitate the process being suspended as well, send dmhost SIGSTOP / SIGCONT between the two items
static void script_bg(int on) {
    if (on) { dm_focus_set(2); dm_focus_set(3); dm_audio_suspend(1); guest_clock_freeze_for(1, 1); dm_gfx_suspend(1); }
    else { dm_gfx_suspend(0); guest_clock_freeze_for(1, 0); dm_audio_suspend(0); dm_focus_set(2); dm_focus_set(1); }
}
static void *oe_script(void *arg) {
    char *buf = arg; uint64_t t0 = now_ns();
    for (char *save, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char key[16]; int v; double at;
        if (sscanf(tok, "%15[a-z]=%d@%lf", key, &v, &at) != 3) die("DM_OE_SCRIPT entry '%s' should be key=value@seconds", tok);
        uint64_t due = t0 + (uint64_t)(at * 1e9), now = now_ns();
        if (due > now) usleep((useconds_t)((due - now) / 1000));
        if (!strcmp(key, "focus")) dm_focus_set(v);
        else if (!strcmp(key, "opmode")) dm_set_opmode(v);
        else if (!strcmp(key, "bg")) script_bg(v);
        else die("DM_OE_SCRIPT unknown key '%s' (focus / opmode / bg)", key);
    }
    return NULL;
}
static void oe_script_start(void) {
    const char *e = getenv("DM_OE_SCRIPT"); if (!e) return;
    pthread_t th; if (pthread_create(&th, NULL, oe_script, strdup(e))) die("DM_OE_SCRIPT thread");
    pthread_detach(th);
}
static pthread_once_t g_oe_once = PTHREAD_ONCE_INIT;

HLE_FORCE("_ZN2nn2oe20GetCurrentFocusStateEv", "foreground / background state; driven by the front end's dm_focus_set", int, h_oe_GetFocus, void) { return g_focus; }
HLE_FORCE("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", "focus handling mode; 1 and 2 get focus notifications, 0 and 3 none (see g_focus_mode)", void, h_oe_SetFocusMode, int m) {
    atomic_store(&g_focus_mode, m); LOG("oe::SetFocusHandlingMode %d (%s)", m, focus_notifies() ? "focus notifications" : "suspended out of focus, no focus notifications");
}

// Notification: returns bool (w0), the host has no system notifications
HLE_FORCE("_ZN2nn2oe25TryPopNotificationMessageEPj", "system notification queue; front end state changes are queued", unsigned, h_oe_TryPopMsg, uint32_t *msg) {
    pthread_once(&g_oe_once, oe_script_start);
    pthread_mutex_lock(&g_mq_lock);
    int ok = g_mqn > 0;
    if (ok) { *msg = g_mq[g_mqh]; g_mqh = (g_mqh + 1) % MQ; g_mqn--; }
    pthread_mutex_unlock(&g_mq_lock);
    if (ok) LOG("oe notification popped by the game: %u", *msg);   // state changes are rare, printing each one does not flood the log
    mq_ev_sync();   // signalled again while notifications remain, cleared when the queue is empty (also a signal left behind by a pop that found it empty)
    return ok;
}
// The blocking form (Link's Awakening pops on a thread of its own, main+0xfa1ff8): the SDK's version waits on the system event oe::Initialize would have created,
// and the host's oe::Initialize is a no-op, so it aborted on an uninitialized event. Wait for the host queue instead, then take the message the Try path's way
HLE_FORCE("_ZN2nn2oe22PopNotificationMessageEv", "system notification queue (blocking); waits on the host queue", uint32_t, h_oe_PopMsg, void) {
    uint32_t m = 0;
    for (;;) {
        pthread_mutex_lock(&g_mq_lock);
        while (!g_mqn) {
            dm_thread_native_wait(1);
            pthread_cond_wait(&g_mq_cond, &g_mq_lock);
            dm_thread_native_wait(0);
        }
        pthread_mutex_unlock(&g_mq_lock);
        if (h_oe_TryPopMsg(&m)) return m;   // another thread may have taken it in between: wait again
    }
}
HLE_FORCE("_ZN2nn2oe27GetNotificationMessageEventEv", "notification event: the host's auto-clear SystemEvent, signalled while the queue holds notifications", void *, h_oe_GetMsgEv, void) {
    pthread_once(&g_oe_once, oe_script_start);   // DM_OE_SCRIPT also for games that wait on the event and pop only once it fires (Monster Hunter Rise)
    if (!atomic_load(&g_mq_ev_on)) { hle_bind_system_event(g_mq_ev, 1); atomic_store(&g_mq_ev_on, 1); mq_ev_sync(); }   // bound on the game's thread (it runs SDK code)
    return g_mq_ev;
}
// bool arguments: Switch callers do not extend them, declared as unsigned and only the low 8 bits are looked at
HLE_FORCE("_ZN2nn2oe28SetResumeNotificationEnabledEb", "resume notification switch; the host expresses resume as focus notification 15", void, h_oe_SetResumeNtf, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", "operation mode change notification switch", void, h_oe_SetOpModeNtf, unsigned on) { g_ntf_opmode = on & 0xff; }
HLE_FORCE("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", "performance mode change notification switch", void, h_oe_SetPerfModeNtf, unsigned on) { g_ntf_perfmode = on & 0xff; }

// nn::settings::LanguageCode = struct { char string[8]; }, 8 bytes returned by value in x0
HLE_FORCE("_ZN2nn2oe18GetDesiredLanguageEv", "system language query; value from DM_LANG", uint64_t, h_oe_GetLang, void) {
    const char *e = getenv("DM_LANG");
    char code[8] = {0};
    strncpy(code, e && *e ? e : "en-US", sizeof code - 1);
    uint64_t v; memcpy(&v, code, 8);
    return v;
}

// The console's region (Animal Crossing: New Horizons 3.0.3): the SDK asks settings::system::GetRegionCode, which reaches set:sys. A region that fits the language the game is given:
// 0 Japan, 1 the Americas, 2 Europe, 4 Hong Kong / Taiwan / Korea (nn::settings::system::RegionCode; 3 Australia and 5 China are left out, no language points at them alone)
HLE_FORCE("_ZN2nn2oe13GetRegionCodeEv", "console region (set:sys); from DM_LANG", int, h_oe_GetRegion, void) {
    const char *e = getenv("DM_LANG"); if (!e || !*e) e = "en-US";
    int r = !strncmp(e, "ja", 2) ? 0 : !strncmp(e, "ko", 2) || !strncmp(e, "zh", 2) ? 4 : !strcmp(e, "en-US") || !strcmp(e, "fr-CA") || !strcmp(e, "es-419") || !strcmp(e, "pt-BR") ? 1 : 2;
    static int once; if (!once++) LOG("oe::GetRegionCode: %d for language %s", r, e);
    return r;
}

// nn::util::Uuid 16 bytes: a fixed local value (only used to tell devices apart locally, networking is cut)
HLE_FORCE("_ZN2nn2oe17GetPseudoDeviceIdEPNS_4util4UuidE", "pseudo device ID; a fixed host value", void, h_oe_GetDevId, uint8_t *uuid) {
    static const uint8_t k[16] = {0x78, 0x62, 0x6c, 0x00, 0x53, 0x57, 0x4e, 0x41, 0x54, 0x49, 0x56, 0x45, 0x31, 0x30, 0x30, 0x31};
    memcpy(uuid, k, 16);
}
