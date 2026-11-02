/* The hwopus service (the Switch's hardware Opus decode). Astral Chain's SDK (1.2.11) reaches it
   through nn::codec::HardwareOpusDecoderManager: connect (sdk+0x1168b0), then a manager command that
   opens a decoder over a transfer-memory work buffer (the client template packs rate|channels as one
   8-byte raw argument and reads a 4-byte size back). The host decodes with the linked libopus; the
   guest's work buffer is accepted but unused, like the nn::codec overrides in opus_codec.c. Commands
   that arrive before they are implemented are dumped and stop the runtime with the guest caller chain. */
#include "runtime.h"
#include "ipc.h"
#include <opus.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct HwDecoder {
    OpusDecoder *dec;
    int rate, channels;
    pthread_mutex_t lock;
} HwDecoder;

static void decoder_close(void *p) {
    HwDecoder *d = p;
    if (d->dec) opus_decoder_destroy(d->dec);
    pthread_mutex_destroy(&d->lock);
    free(d);
}

static HwDecoder *decoder_new(int rate, int channels) {
    if (!((rate == 48000 || rate == 24000 || rate == 16000 || rate == 12000 || rate == 8000) &&
          (channels == 1 || channels == 2))) {
        fprintf(stderr, "[totk-ipc] hwopus: unsupported format %u Hz x%u\n", rate, channels);
        runtime_fail("hwopus format not supported");
    }
    HwDecoder *d = calloc(1, sizeof *d);
    if (!d) runtime_fail("hwopus decoder allocation");
    int err = 0;
    d->dec = opus_decoder_create(rate, channels, &err);
    if (!d->dec || err != OPUS_OK) runtime_fail("hwopus libopus decoder creation");
    pthread_mutex_init(&d->lock, NULL);
    d->rate = rate; d->channels = channels;
    return d;
}

/* The decoder object session: its commands (decode/reset/...) arrive once the manager open succeeds.
   Each one is dumped in full and stops the runtime, so the next round implements the exact protocol. */
static void decoder_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)rs;
    fprintf(stderr, "[totk-ipc] hwopus decoder: cmd %u type %u data %u raw:", rq->cmd, rq->type, rq->data_size);
    for (unsigned i = 0; i + 4 <= rq->data_size && i < 48; i += 4) {
        uint32_t v; memcpy(&v, rq->data + i, 4);
        fprintf(stderr, " %08x", v);
    }
    fprintf(stderr, " | nx %u na %u nb %u move %u copy %u\n", rq->nx, rq->na, rq->nb, rq->nmove, rq->ncopy);
    ipc_unimplemented(s, rq);
}

/* The manager session. Command 1 is the work-buffer size query: raw {rate u32, channels u32} and a
   4-byte size back (the client rounds it up to 0x1000 and allocates the work buffer from it). The
   open/decode commands arrive on later rounds and are dumped until implemented. */
static void hwopus_service(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    fprintf(stderr, "[totk-ipc] hwopus: cmd %u type %u data %u raw:", rq->cmd, rq->type, rq->data_size);
    for (unsigned i = 0; i + 4 <= rq->data_size && i < 32; i += 4) {
        uint32_t v; memcpy(&v, rq->data + i, 4);
        fprintf(stderr, " %08x", v);
    }
    fprintf(stderr, " | nx %u na %u nb %u move %u copy %u\n", rq->nx, rq->na, rq->nb, rq->nmove, rq->ncopy);
    if (rq->cmd == 1 && rq->data_size >= 8) {
        uint32_t rate = 0, channels = 0;
        memcpy(&rate, ipc_arg(rq, 0, 4), 4);
        memcpy(&channels, ipc_arg(rq, 4, 4), 4);
        fprintf(stderr, "[totk-ipc] hwopus: work size query %u Hz x%u\n", rate, channels);
        uint32_t size = (uint32_t)opus_decoder_get_size((int)channels) + 0x1000;
        ipc_out(rs, &size, sizeof size);
        return;
    }
    if (rq->cmd == 0 && rq->data_size >= 8) {
        /* OpenHardwareOpusDecoder: raw {rate u32, channels u32, ...}, the work buffer arrives as a
           move handle (transfer memory); the host keeps its own state and leaves the buffer alone. */
        uint32_t rate = 0, channels = 0;
        memcpy(&rate, ipc_arg(rq, 0, 4), 4);
        memcpy(&channels, ipc_arg(rq, 4, 4), 4);
        fprintf(stderr, "[totk-ipc] hwopus: open %u Hz x%u (move handles %u)\n", rate, channels, rq->nmove);
        HwDecoder *d = decoder_new((int)rate, (int)channels);
        rs->move[rs->nmove++] = runtime_ipc_new_session("hwopus decoder", decoder_handler, d, decoder_close);
        return;
    }
    ipc_unimplemented(s, rq);
}

__attribute__((constructor)) static void register_hwopus(void) { runtime_ipc_register("hwopus", hwopus_service, NULL, NULL); }
