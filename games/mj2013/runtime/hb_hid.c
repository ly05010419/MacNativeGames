/* hid: the service libnx's hidInitialize opens, and the 256 KiB shared memory where Horizon's hid sysmodule publishes
   input as LIFOs (layout: third_party/libnx/hid_shmem.h). A host thread samples the player's input (input.c) every
   5 ms and pushes one entry into each LIFO the game may read: touch screen, mouse, keyboard, and the controller as
   player 1 (full key, a Pro Controller) and as the handheld console. */
#include "hb.h"
#include "keyboard_layout.h"
#include "hid_publish.h"
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

uint32_t runtime_shared_memory_new(uint64_t size, void **host);   /* svc_memory.c */
static HidSharedMemory *shm;
static uint32_t shm_handle;
static uint64_t sampling;

static void *sampler(void *unused) {
    (void)unused;
    for (;;) {
        HbInput in; hb_input_snapshot(&in);
        hb_hid_publish(shm, &in, ++sampling);
        struct timespec t = {0, 5000000}; nanosleep(&t, NULL);
    }
    return NULL;
}
static void hid_shared_memory(void) {
    if (shm) return;
    void *host; shm_handle = runtime_shared_memory_new(sizeof(HidSharedMemory), &host); shm = host;
    HbInput in; memset(&in, 0, sizeof in); hb_hid_publish(shm, &in, ++sampling);   /* connected before the first read */
    pthread_t t; if (pthread_create(&t, NULL, sampler, NULL)) runtime_fail("hid sampler thread"); pthread_detach(t);
}
static void applet_resource(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: hid_shared_memory(); hb_copy(rs, shm_handle); return;   /* GetSharedMemoryHandle */
    default: HB_UNKNOWN(s, rq);
    }
}
static void hid(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: hb_session(rs, "IAppletResource", applet_resource, NULL, NULL); return;   /* CreateAppletResource */
    case 1: case 11: case 21: case 31: case 32: return;          /* ActivateDebugPad / TouchScreen / Mouse / Keyboard / SendKeyboardLockKeyEvent */
    case 40: case 41: case 51: case 55: case 56: case 58: case 59: case 60: case 61: return;   /* Xpad, home / sleep / capture buttons, gesture */
    case 66: case 67: case 69: case 70: case 72: case 73: case 75: case 77: case 78: case 80: return;   /* six-axis sensor start / stop / settings (no output) */
    case 100: case 102: case 103: case 104: case 120: case 122: case 123: case 124: case 125: case 126: case 128: case 130: case 132: case 134: return;   /* npad configuration and activation (no output) */
    case 101: HB_OUT_U32(rs, 3); return;                         /* GetSupportedNpadStyleSet */
    case 106: hb_copy(rs, hb_event(NULL)); return;               /* AcquireNpadStyleSetUpdateEventHandle */
    case 107: case 109: return;                                  /* DisconnectNpad, ActivateNpadWithRevision */
    case 121: HB_OUT_U64(rs, 0); return;                         /* GetNpadJoyHoldType: vertical */
    case 200: { uint32_t info[4] = {1, 0, 0, 0}; IPC_OUT(rs, info); return; }   /* GetVibrationDeviceInfo */
    case 201: case 203: case 206: return;                        /* SendVibrationValue, CreateActiveVibrationDeviceList, SendVibrationValues */
    default: HB_UNKNOWN(s, rq);
    }
}
void hb_hid_register(void) { runtime_ipc_register("hid", hid, NULL, NULL); }
