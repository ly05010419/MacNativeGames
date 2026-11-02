/* bsd:u (nn::socket), offline, from games/odyssey (system_services.c): the game is played as a Switch without a
   connection. The client registers and its monitor starts, but every socket it asks for fails with ENETDOWN. Commands
   return {s32 ret, u32 errno} (switchbrew "Sockets services"). Ori and the Blind Forest opens it at start. */
#include "ipc.h"
#include <stdio.h>

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

__attribute__((constructor)) static void register_bsd(void) { runtime_ipc_register("bsd:u", bsd_handler, NULL, NULL); }
