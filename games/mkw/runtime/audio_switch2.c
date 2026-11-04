/* Mario Kart World (Switch 2): the SDK's audio library opens "aud:u" in place of the Switch's "audren:u". Its interface
   is mapped from the commands the game sends (each unknown one is logged with its arguments before the start stops). */
#include "runtime.h"
#include "ipc.h"
#include <stdio.h>
#include <string.h>

static void dump_args(const IpcRequest *rq) {
    fprintf(stderr, "[mkw-audio] data:");
    for (uint32_t i = 0; i < rq->data_size && i < 0x80; i += 4) { uint32_t w; memcpy(&w, rq->data + i, 4); fprintf(stderr, " %08x", w); }
    fprintf(stderr, "\n");
    for (unsigned i = 0; i < rq->na; i++) fprintf(stderr, "[mkw-audio] a%u %#llx+%#llx\n", i, (unsigned long long)rq->a[i].addr, (unsigned long long)rq->a[i].size);
    for (unsigned i = 0; i < rq->nb; i++) fprintf(stderr, "[mkw-audio] b%u %#llx+%#llx\n", i, (unsigned long long)rq->b[i].addr, (unsigned long long)rq->b[i].size);
    for (unsigned i = 0; i < rq->nx; i++) fprintf(stderr, "[mkw-audio] x%u %#llx+%#llx\n", i, (unsigned long long)rq->x[i].addr, (unsigned long long)rq->x[i].size);
    for (unsigned i = 0; i < rq->nc; i++) fprintf(stderr, "[mkw-audio] c%u %#llx+%#llx\n", i, (unsigned long long)rq->c[i].addr, (unsigned long long)rq->c[i].size);
    if (rq->ncopy || rq->nmove || rq->has_pid) fprintf(stderr, "[mkw-audio] copy %u move %u pid %d\n", rq->ncopy, rq->nmove, rq->has_pid);
}

void runtime_audio_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);   /* audio_renderer.c */

static void root_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 2:   /* (u64, 0 from the game) -> the Switch's IAudioRendererManager, same commands and the same REV15
                 AudioRendererParameter (sdk+0x1f7370: the interface the audio renderer functions use) */
        rs->move[rs->nmove++] = runtime_ipc_new_session("IAudioRendererManager", runtime_audio_manager_handler, NULL, NULL);
        return;
    default: dump_args(rq); ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_aud(void) { runtime_ipc_register("aud:u", root_handler, NULL, NULL); }
