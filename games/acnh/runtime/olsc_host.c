/* Local session for the application's save-backup client. Command identities:
 * https://switchbrew.org/wiki/OLSC_services#olsc:u . No network client is created.
 * Unsupported backup operations remain explicit errors during bring-up. */
#include "runtime.h"
#include "ipc.h"
#include <stdlib.h>
#include <string.h>

typedef struct { int initialized; } OlscSession;

static void *olsc_open(void) {
    OlscSession *state = calloc(1, sizeof *state);
    if (!state) runtime_fail("olsc session allocation");
    return state;
}

static void olsc_handler(IpcSession *session, const IpcRequest *request, IpcResponse *response) {
    OlscSession *state = session->state;
    switch (request->cmd) {
    case 0: /* Initialize: establish the local application's service session. */
        state->initialized = 1;
        return;
    case 13: /* GetSaveDataBackupSetting(Uid) -> bool; never enabled offline. */
    case 14: { /* SetSaveDataBackupSetting(bool, Uid); the client disables backup when loading a local island. */
        if (!state->initialized) runtime_fail("olsc query before initialization");
        uint64_t uid[2]; runtime_account_uid(uid);
        unsigned uid_offset = request->cmd == 14 ? 8 : 0;
        if (memcmp(ipc_arg(request, uid_offset, sizeof uid), uid, sizeof uid)) {
            response->result = 0x7c67; /* The host has no such local account. */
            return;
        }
        if (request->cmd == 14) {
            /* main+0x5ba130 writes the bool before the aligned Uid at +8.
             * Disabling needs no remote operation; enabling is unavailable offline. */
            if (*(const uint8_t *)ipc_arg(request, 0, 1)) response->result = 0xd401;
            return;
        }
        /* The original 3.0.1 client reads one byte at main+0x5ba064;
         * main+0x2808cb4 compares it with 1 and disables backup if enabled. */
        uint8_t enabled = 0;
        IPC_OUT(response, enabled);
        return;
    }
    default:
        ipc_unimplemented(session, request);
    }
}

__attribute__((constructor)) static void register_olsc(void) {
    runtime_ipc_register("olsc:u", olsc_handler, olsc_open, free);
}
