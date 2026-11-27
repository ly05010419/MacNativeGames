/* Horizon csrng command 0 (GetRandomBytes): output map-alias buffer.
   Host arc4random_buf supplies OS cryptographic randomness, never a test seed. */
#include "ipc.h"
#include <stdlib.h>

static void csrng_handler(IpcSession *session, const IpcRequest *rq, IpcResponse *rs) {
    (void)session;
    if (rq->cmd != 0) { rs->result = IPC_RESULT_UNKNOWN_COMMAND; return; }
    /* Bounded before mapping. Invalid descriptors do not partially write output. */
    if (rq->nb != 1 || rq->b[0].size > 0x100000) {
        rs->result = 0xd401u; /* kernel InvalidSize */
        return;
    }
    if (rq->b[0].size) arc4random_buf(ipc_guest(rq->b[0].addr, rq->b[0].size), (size_t)rq->b[0].size);
}
__attribute__((constructor)) static void register_csrng(void) {
    runtime_ipc_register("csrng", csrng_handler, NULL, NULL);
}
