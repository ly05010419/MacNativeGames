/* Minimal Application Performance Manager for boot: log every command, answer with
   zeros until the game's needs are measured. Commands observed during bring-up
   get real implementations below; anything else fails loudly with its number. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>
static void apm(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
 (void)s;
 fprintf(stderr, "[emio-apm] cmd %u data_size %u nb %u\n", rq->cmd, rq->data_size, rq->nb);
 switch (rq->cmd) {
 case 0: { /* OpenSession? answer empty success; refined once observed */
  return; }
 default: ipc_unimplemented(s, rq);
 }
}
__attribute__((constructor)) static void register_apm(void) {
 runtime_ipc_register("apm", apm, NULL, NULL);
}
