/* hid IPC service (minimal): PLA's early init opens "hid" before the main loop. The input state itself is
   delivered by the host's by-name overrides (nn::hid::GetNpadState... in input.c); this service exists so the
   SDK's hid initialization succeeds. Unknown commands dump and stop loudly so real requirements surface. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>

static void *hid_open(void) { return calloc(1, 8); }
static void hid_close(void *state) { free(state); }

static void hid_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    fprintf(stderr, "[totk-ipc] hid cmd %u\n", rq->cmd);
    switch (rq->cmd) {
    case 0:  /* CreateAppletResource -> IAppletResource session */
    case 1:  /* ActivateAppletResource */
        rs->move[rs->nmove++] = runtime_ipc_new_session("hid:applet", hid_handler, calloc(1, 8), free);
        return;
    case 102: /* SetSupportedNpadStyleSet */
    case 103: /* GetSupportedNpadStyleSet */
    case 106: /* ActivateNpadWithRevision */
    case 110: /* SetSupportedNpadIdType */
    case 111: /* ActivateNpad */
        return;
    default:
        ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_hid(void) {
    runtime_ipc_register("hid", hid_handler, hid_open, hid_close);
}
