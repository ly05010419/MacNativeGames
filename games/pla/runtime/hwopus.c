/* hwopus IPC service: nn::codec's hardware Opus decoder (PLA's Wwise threads use it for their streams),
   implemented with libopus on the host. Commands per the public IHardwareOpusDecoderManager /
   IHardwareOpusDecoder description; Opus frames carry Nintendo's 8-byte big-endian header
   (u32 data_size, u32 final_range) ahead of the packet. Unknown commands stop the runtime loudly. */
#include "runtime.h"
#include "ipc.h"
#include <opus.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

typedef struct { OpusDecoder *dec; int rate; int channels; } HwOpus;

static void decoder_close(void *state) {
    HwOpus *d = state;
    if (!d) return;
    if (d->dec) opus_decoder_destroy(d->dec);
    free(d);
}
static void *manager_open(void) { return calloc(1, 8); }
static void manager_close(void *state) { free(state); }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* IHardwareOpusDecoder: 0 = Decode (in A buffer, out C buffer) -> (u32 consumed, u32 samples). */
static void decoder_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    HwOpus *d = s->state;
    if (!d || !d->dec) ipc_unimplemented(s, rq);
    if (rq->cmd != 0) ipc_unimplemented(s, rq);
    if (!rq->na || !rq->a[0].size || !rq->nc || !rq->c[0].size) ipc_unimplemented(s, rq);
    const uint8_t *in = ipc_guest(rq->a[0].addr, rq->a[0].size);
    int16_t *out = ipc_guest(rq->c[0].addr, rq->c[0].size);
    if (!in || !out) ipc_unimplemented(s, rq);
    if (rq->a[0].size < 8) ipc_unimplemented(s, rq);
    uint32_t n = be32(in), range = be32(in + 4);
    if (n > rq->a[0].size - 8) ipc_unimplemented(s, rq);
    size_t capacity = rq->c[0].size / (sizeof(int16_t) * (size_t)d->channels);
    int got = opus_decode(d->dec, in + 8, (opus_int32)n, out, (int)(capacity > INT_MAX ? INT_MAX : capacity), 0);
    if (got < 0) { fprintf(stderr, "[totk] hwopus: opus_decode: %s\n", opus_strerror(got)); ipc_unimplemented(s, rq); }
    opus_uint32 final = 0; opus_decoder_ctl(d->dec, OPUS_GET_FINAL_RANGE(&final));
    if (n && range && final != range) { fprintf(stderr, "[totk] hwopus: final range %#x != stream %#x\n", final, range); }
    uint32_t consumed = 8 + n;
    IPC_OUT(rs, consumed);
    uint32_t samples = (uint32_t)got;
    IPC_OUT(rs, samples);
}

/* IHardwareOpusDecoderManager: 0 = OpenHardwareOpusDecoder(rate, channels, A work buffer) -> session;
   1 = GetWorkBufferSize(rate, channels) -> u32. */
static void manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    switch (rq->cmd) {
    case 0: {
        uint32_t rate = IPC_ARG(rq, uint32_t, 0), channels = IPC_ARG(rq, uint32_t, 4);
        HwOpus *d = calloc(1, sizeof *d);
        if (!d) runtime_fail("hwopus session allocation");
        d->rate = (int)rate; d->channels = (int)channels;
        int err;
        d->dec = opus_decoder_create((opus_int32)rate, (int)channels, &err);
        if (!d->dec) { fprintf(stderr, "[totk] hwopus: opus_decoder_create %u/%u: %s\n", rate, channels, opus_strerror(err)); free(d); runtime_fail("hwopus decoder create"); }
        if (d->dec) opus_decoder_ctl(d->dec, OPUS_SET_GAIN(0));
        rs->move[rs->nmove++] = runtime_ipc_new_session("hwopus:decoder", decoder_handler, d, decoder_close);
        fprintf(stderr, "[totk-ipc] hwopus decoder opened %u Hz x%u\n", rate, channels);
        return;
    }
    case 1: {
        uint32_t rate = IPC_ARG(rq, uint32_t, 0), channels = IPC_ARG(rq, uint32_t, 4);
        uint32_t size = (uint32_t)(opus_decoder_get_size((int)channels) + 0x1000);
        (void)rate;
        IPC_OUT(rs, size);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

__attribute__((constructor)) static void register_hwopus(void) {
    runtime_ipc_register("hwopus", manager_handler, manager_open, manager_close);
}
