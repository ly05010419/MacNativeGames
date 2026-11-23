/* appletOE and its interfaces as libnx's appletInitialize and appletMainLoop reach them (switchbrew "Applet Manager
   services"; the order libnx follows is in its applet.c). The application is always in focus, docked, and never asked
   to suspend or exit: the message queue stays empty, its event unsignalled. It is handheld (1280x720): the game
   draws in software, and the window scales the picture. */
#include "hb.h"
#include <stdatomic.h>

static uint32_t message_event;                 /* writable end; the game holds the readable one */
static _Atomic int exit_requested;
void hb_applet_request_exit(void) { atomic_store(&exit_requested, 1); if (message_event) runtime_event_signal(message_event); }

static void common_state_getter(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: hb_copy(rs, hb_event(&message_event)); return;                 /* GetEventHandle */
    case 1:                                                                /* ReceiveMessage */
        if (atomic_exchange(&exit_requested, 0)) { HB_OUT_U32(rs, 4); return; }   /* ExitRequested */
        rs->result = 0x680; return;                                        /* no message */
    case 5: HB_OUT_U8(rs, 0); return;                                      /* GetOperationMode: handheld (a 1280x720 picture) */
    case 6: HB_OUT_U32(rs, 0); return;                                     /* GetPerformanceMode: normal */
    case 7: HB_OUT_U8(rs, 0); return;                                      /* GetCradleStatus */
    case 8: HB_OUT_U8(rs, 0); return;                                      /* GetBootMode */
    case 9: HB_OUT_U8(rs, 1); return;                                      /* GetCurrentFocusState: InFocus */
    case 10: case 11: return;                                              /* Request/ReleaseToAcquireSleepLock */
    case 13: hb_copy(rs, hb_event(NULL)); return;                          /* GetAcquiredSleepLockEvent */
    case 60: { int32_t wh[2] = {1280, 720}; IPC_OUT(rs, wh); return; }    /* GetDefaultDisplayResolution (handheld) */
    case 61: hb_copy(rs, hb_event(NULL)); return;                          /* GetDefaultDisplayResolutionChangeEvent */
    default: HB_UNKNOWN(s, rq);
    }
}
static void self_controller(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: HB_LOG("applet: Exit"); return;                                /* Exit */
    case 1: case 2: case 3: case 4: return;                                /* Lock/UnlockExit, Enter/LeaveFatalSection */
    case 10: case 11: case 12: case 13: case 14: case 16: case 17: case 18: case 19: return;   /* screenshot, notifications, focus handling */
    case 40: HB_OUT_U64(rs, 1); return;                                    /* CreateManagedDisplayLayer -> layer id */
    case 50: case 51: case 62: case 65: case 68: case 72: case 100: return;   /* handling and wake settings */
    case 91: hb_copy(rs, hb_event(NULL)); return;                          /* GetAccumulatedSuspendedTickChangedEvent */
    default: HB_UNKNOWN(s, rq);
    }
}
static void window_controller(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 1: HB_OUT_U64(rs, HB_ARUID); return;                              /* GetAppletResourceUserId */
    case 10: return;                                                       /* AcquireForegroundRights */
    default: HB_UNKNOWN(s, rq);
    }
}
static void application_functions(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 1: {                                                              /* PopLaunchParameter: none */
        rs->result = 0x2a2; return; }
    case 20: HB_OUT_U64(rs, 0); return;                                    /* EnsureSaveData: no user account needed */
    case 21: { uint64_t lang = 0; memcpy(&lang, "zh-Hant", 7); HB_OUT_U64(rs, lang); return; }   /* GetDesiredLanguage */
    case 22: return;                                                       /* SetTerminateResult */
    case 40: HB_OUT_U8(rs, 0); return;                                     /* NotifyRunning */
    case 65: HB_OUT_U8(rs, 0); return;                                     /* IsGamePlayRecordingSupported: no */
    case 66: case 67: case 100: case 101: case 121: case 122: return;     /* recording, copyright image, user channel */
    default: HB_UNKNOWN(s, rq);
    }
}
static void empty_interface(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {   /* audio, display, debug, applet creator */
    (void)rs;
    HB_LOG("applet %s: command %u answered with nothing", s->name, rq->cmd);
}
static void application_proxy(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: hb_session(rs, "ICommonStateGetter", common_state_getter, NULL, NULL); return;
    case 1: hb_session(rs, "ISelfController", self_controller, NULL, NULL); return;
    case 2: hb_session(rs, "IWindowController", window_controller, NULL, NULL); return;
    case 3: hb_session(rs, "IAudioController", empty_interface, NULL, NULL); return;
    case 4: hb_session(rs, "IDisplayController", empty_interface, NULL, NULL); return;
    case 10: hb_session(rs, "IProcessWindingController", empty_interface, NULL, NULL); return;
    case 11: hb_session(rs, "ILibraryAppletCreator", empty_interface, NULL, NULL); return;
    case 20: hb_session(rs, "IApplicationFunctions", application_functions, NULL, NULL); return;
    case 1000: hb_session(rs, "IDebugFunctions", empty_interface, NULL, NULL); return;
    default: HB_UNKNOWN(s, rq);
    }
}
static void applet_oe(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: hb_session(rs, "IApplicationProxy", application_proxy, NULL, NULL); return;   /* OpenApplicationProxy */
    default: HB_UNKNOWN(s, rq);
    }
}
/* apm: libnx opens a session for an application before appletOE (performance configuration). */
static void apm_session(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: return;                                                        /* SetPerformanceConfiguration */
    case 1: HB_OUT_U32(rs, 0x92220007); return;                            /* GetPerformanceConfiguration */
    default: HB_UNKNOWN(s, rq);
    }
}
static void apm(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: hb_session(rs, "apm:ISession", apm_session, NULL, NULL); return;   /* OpenSession */
    case 1: HB_OUT_U32(rs, 0); return;                                     /* GetPerformanceMode: normal */
    default: HB_UNKNOWN(s, rq);
    }
}
void hb_applet_register(void) {
    runtime_ipc_register("appletOE", applet_oe, NULL, NULL);
    runtime_ipc_register("apm", apm, NULL, NULL);
}
