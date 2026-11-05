/* csrng IPC service: the SDK asks the CSPRNG for random bytes (command 0, GetRandomBytes into an X buffer).
   Served with arc4random_buf. Unknown commands dump and stop loudly. */
#include "runtime.h"
#include "ipc.h"
#include <stdlib.h>
#include <stdio.h>

static void *csrng_open(void) { return calloc(1, 8); }
static void csrng_close(void *state) { free(state); }

static void csrng_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    {static unsigned n; if (n++ < 6) { uint32_t d0 = rq->data_size >= 4 ? *(const uint32_t *)ipc_arg(rq, 0, 4) : 0;
        fprintf(stderr, "[totk-ipc] csrng cmd %u type %u data %u nx %u x0.size=%#llx raw=%#x\n",
        rq->cmd, rq->type, rq->data_size, rq->nx, (unsigned long long)rq->x[0].size, d0); }}
    switch (rq->cmd) {
    case 0: {  /* GetRandomBytes: TIPC carries a u32 byte count in-raw; CMIF uses the X buffer sizes */
        uint64_t want = rq->x[0].size;
        if (rq->type == 6 && rq->data_size >= 4) want = *(const uint32_t *)ipc_arg(rq, 0, 4);
        if (want > 0x10000) ipc_unimplemented(s, rq);
        if (!want) { uint32_t zero = 0; ipc_out(rs, &zero, 4); return; }   /* zero-length probe: answer OK */
        if (rq->type == 6) {  /* TIPC: the random bytes ride back in the raw response data */
            uint8_t tmp[0x1000];
            if (want > sizeof tmp) ipc_unimplemented(s, rq);
            arc4random_buf(tmp, want);
            ipc_out(rs, tmp, (uint32_t)want);
            return;
        }
        if (!rq->nx) ipc_unimplemented(s, rq);
        uint8_t *dst = ipc_guest(rq->x[0].addr, rq->x[0].size);
        if (!dst) ipc_unimplemented(s, rq);
        arc4random_buf(dst, want);
        rs->xout[rs->nxout++] = (IpcBuffer){rq->x[0].addr, (uint32_t)want, rq->x[0].index};
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_csrng(void) {
    runtime_ipc_register("csrng", csrng_handler, csrng_open, csrng_close);
}
