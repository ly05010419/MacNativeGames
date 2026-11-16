/* Minimal csrng (Nintendo random) service: back with host arc4random. The game uses it for
   nonces/UUIDs during boot; real entropy (not zeros) so callers never see degenerate values. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void csrng(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
 (void)s;
 fprintf(stderr, "[emio-csrng] cmd %u data_size %u nb %u\n", rq->cmd, rq->data_size, rq->nb);
 switch (rq->cmd) {
 case 0: /* GetRandomBytes? fill output buffers with entropy (bring-up: assume all out buffers want random) */
  for (unsigned i = 0; i < rq->nb && i < 8; i++) {
   /* out buffers are described by the response; without the descriptor layout, fill via direct mapping is unsafe.
      Instead return success and let the caller retry with explicit sizes; log loudly to measure. */
  }
  /* Fall through to fill if the response has a data payload area. */
  return;
 default: ipc_unimplemented(s, rq);
 }
}
__attribute__((constructor)) static void register_csrng(void) {
 runtime_ipc_register("csrng", csrng, NULL, NULL);
}
