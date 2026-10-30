/* XCX's SDK AddSessionId reaches prepo:u even though report saving is already
   handled offline by applet_host.c. Keep its original report encoding: provide
   only GetSystemSessionId (10400, u64 out), as documented at
   https://switchbrew.org/wiki/BCAT_services#prepo:a,_prepo:a2,_prepo:m,_prepo:u,_prepo:s
   A native process is one system session. Its random local id lasts until exit;
   it contains no device identity and is neither persisted nor transmitted.
   Other commands remain named failures, never a blanket success. */
#include "ipc.h"
#include <pthread.h>
#include <stdlib.h>

static pthread_once_t session_once = PTHREAD_ONCE_INIT;
static uint64_t session_id;
static void make_session_id(void) {
    do { arc4random_buf(&session_id, sizeof session_id); } while (!session_id);
}
static void prepo_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    if (rq->cmd != 10400) ipc_unimplemented(s, rq);
    pthread_once(&session_once, make_session_id);
    IPC_OUT(rs, session_id);
}
__attribute__((constructor)) static void register_prepo(void) {
    runtime_ipc_register("prepo:u", prepo_handler, NULL, NULL);
}
