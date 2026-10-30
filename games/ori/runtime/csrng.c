/* csrng (spl::IRandomInterface): the system's cryptographic random numbers, from macOS's. Newer SDKs open it in their
   constructors (seen with Paper Mario: The Thousand-Year Door through scripts/try_game.py). */
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>

static void csrng(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {   /* GenerateRandomBytes(out buffer): a map-alias (B) buffer, or a pointer (C) one with auto-select */
        const IpcBuffer *b = rq->nb && rq->b[0].size ? &rq->b[0] : rq->nc && rq->c[0].size ? &rq->c[0] : NULL;
        if (!b) return;
        arc4random_buf(ipc_guest(b->addr, b->size), b->size);
        if (b == &rq->c[0]) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, b->size, 0};   /* a pointer reply says what it wrote */
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_csrng(void) { runtime_ipc_register("csrng", csrng, NULL, NULL); }
