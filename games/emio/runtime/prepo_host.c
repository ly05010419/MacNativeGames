/* Minimal prepo:u (play report) service for boot: log every command, answer with zeros until the game's
   needs are measured. Play reports are analytics; dropping them is safe for bring-up. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
static void prepo(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
 (void)s;
 fprintf(stderr, "[emio-prepo] cmd %u data_size %u nb %u\n", rq->cmd, rq->data_size, rq->nb);
 switch (rq->cmd) {
 case 0: /* open session? / initialize? empty success until measured */
 case 1:
 case 2:
 case 10400: /* play-report upload? acknowledge without storing (bring-up) */
  return;
 default: ipc_unimplemented(s, rq);
 }
}
__attribute__((constructor)) static void register_prepo(void) {
 runtime_ipc_register("prepo:u", prepo, NULL, NULL);
}
