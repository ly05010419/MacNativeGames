/* Minimal HID service for boot: log every command, answer with zeros until the game's
   needs are measured. Commands observed during bring-up get real implementations
   below; anything else fails loudly with its number. */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>
static void hid(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
 (void)s;
 fprintf(stderr, "[emio-hid] cmd %u data_size %u nb %u\n", rq->cmd, rq->data_size, rq->nb);
 const uint32_t *w = (const uint32_t *)rq->data;
 for (unsigned i = 0; i + 4 <= rq->data_size && i < 32; i += 4)
  fprintf(stderr, "[emio-hid] arg+%u=%08x\n", i, w[i / 4]);
 switch (rq->cmd) {
 case 0: { /* ActivateDebugger? / GetSharedMemoryHandle? empty success until measured */
  return; }
 case 128: { /* follow-up setup; empty success until measured */
  return; }
 default: ipc_unimplemented(s, rq);
 }
}
__attribute__((constructor)) static void register_hid(void) {
 runtime_ipc_register("hid", hid, NULL, NULL);
}
