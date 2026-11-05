/* NGC's service exists, but this host does not mount the NG-word system data
   (0100000000000823). Report its documented data-access failure, not a clean
   check, a zero content version, or a fabricated masked string. XCX's original
   SDK propagates this Result and the game's caller retains the original text.
   No guest branch, filter result or integrity check is patched.

   ABI/error reference (MIT, Ryujinx Team and Contributors):
   https://github.com/NextendoNetwork/Ryujinx-Nextendo/blob/main/src/Ryujinx.Horizon/Ngc/Ipc/Service.cs
   https://github.com/NextendoNetwork/Ryujinx-Nextendo/blob/main/src/Ryujinx.Horizon/Sdk/Ngc/NgcResult.cs
   Commands 0..3: GetContentVersion, Check, Mask, Reload. Newer commands remain
   unsupported. Only an error Result is returned: no output data or buffers. */
#include "ipc.h"
#include <stdatomic.h>
#include <stdio.h>

#define NGC_DATA_ACCESS_ERROR ((102u << 9) | 146u)
static atomic_flag unavailable_logged = ATOMIC_FLAG_INIT;

static void ngc_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    if (rq->cmd > 3) ipc_unimplemented(s, rq);
    rs->result = NGC_DATA_ACCESS_ERROR;
    if (!atomic_flag_test_and_set(&unavailable_logged))
        fprintf(stderr, "[xcx-ngc] dictionary backend unavailable; cmd=%u returns DataAccessError %#x; no text checked or modified\n",
                rq->cmd, NGC_DATA_ACCESS_ERROR);
}

__attribute__((constructor)) static void register_ngc(void) {
    runtime_ipc_register("ngc:u", ngc_handler, NULL, NULL);
}
