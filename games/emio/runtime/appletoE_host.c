/* Minimal appletOE service for boot: log every command, answer with zeros until the
   game's needs are measured. Commands observed during bring-up get real
   implementations below; anything else fails loudly with its number. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>
static void applet_appfuncs(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
 (void)s;
 fprintf(stderr, "[emio-appletOE-appfuncs] cmd %u data_size %u nb %u\n", rq->cmd, rq->data_size, rq->nb);
 const uint32_t *w = (const uint32_t *)rq->data;
 for (unsigned i = 0; i + 4 <= rq->data_size && i < 64; i += 4)
  fprintf(stderr, "[emio-appletOE-appfuncs] arg+%u=%08x\n", i, w[i / 4]);
 switch (rq->cmd) {
 case 20: { /* display-resolution-ish setup; empty success until measured */
  return; }
 case 11: { /* follow-up setup; empty success until measured */
  return; }
 case 0: { /* bare query; empty success until measured */
  return; }
 case 1: { /* bare query; empty success until measured */
  return; }
 case 2: { /* bare query; empty success until measured */
  return; }
 default: ipc_unimplemented(s, rq);
 }
}
static void applet_oe(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
 (void)s;
 fprintf(stderr, "[emio-appletOE] cmd %u data_size %u nb %u\n", rq->cmd, rq->data_size, rq->nb);
 switch (rq->cmd) {
 case 0: { /* GetApplicationFunctions: hand out a session; refined once observed */
  rs->move[rs->nmove++] = runtime_ipc_new_session("appletOE IApplicationFunctions", applet_appfuncs, NULL, NULL);
  return; }
 default: ipc_unimplemented(s, rq);
 }
}
__attribute__((constructor)) static void register_applet_oe(void) {
 runtime_ipc_register("appletOE", applet_oe, NULL, NULL);
}
