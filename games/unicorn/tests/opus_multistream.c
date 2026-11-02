#include "runtime.h"
#include <opus_multistream.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr, "%s\n", message); abort(); }
void dm_guest_write_prepare(void *address, size_t size) { (void)address; (void)size; }
uint64_t runtime_now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }

int main(void) {
    const unsigned char mapping[6] = {0, 4, 1, 2, 3, 5};
    int error;
    OpusMSEncoder *encoder = opus_multistream_encoder_create(48000, 6, 4, 2, mapping, OPUS_APPLICATION_AUDIO, &error);
    assert(encoder && error == OPUS_OK);
    int16_t pcm[960 * 6], reference[960 * 6], output[960 * 6];
    for (int i = 0; i < 960; i++) for (int c = 0; c < 6; c++) pcm[i * 6 + c] = (int16_t)(((i * (c + 1) * 31) % 10000) - 5000);
    unsigned char packet[65536];
    int size = opus_multistream_encode(encoder, pcm, 960, packet + 8, sizeof packet - 8);
    assert(size > 0);
    opus_uint32 range;
    assert(opus_multistream_encoder_ctl(encoder, OPUS_GET_FINAL_RANGE(&range)) == OPUS_OK);
    for (int i = 0; i < 4; i++) { packet[i] = (unsigned)size >> (24 - 8 * i); packet[i + 4] = range >> (24 - 8 * i); }
    OpusMSDecoder *independent = opus_multistream_decoder_create(48000, 6, 4, 2, mapping, &error);
    assert(independent && error == OPUS_OK);
    assert(opus_multistream_decode(independent, packet + 8, size, reference, 960, 0) == 960);

    size_t (*work_size)(int,int,int,int,int) = runtime_multistream_override("_ZN2nn5codec47GetHardwareOpusMultiStreamDecoderWorkBufferSizeEiiiii");
    unsigned (*initialize)(void *,int,int,int,int,const unsigned char *,void *,size_t,int) = runtime_multistream_override("_ZN2nn5codec40InitializeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeEiiiiPKhPvmi");
    unsigned (*decode)(void *,size_t *,int *,int16_t *,size_t,const uint8_t *,size_t) = runtime_multistream_override("_ZN2nn5codec44DecodeOpusMultiStreamInterleavedWithHardwareEPNS0_34HardwareOpusMultiStreamDecoderTypeEPmPiPsmPKvm");
    unsigned (*reset)(void *) = runtime_multistream_override("_ZN2nn5codec35ResetHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeE");
    void (*finalize)(void *) = runtime_multistream_override("_ZN2nn5codec38FinalizeHardwareOpusMultiStreamDecoderEPNS0_34HardwareOpusMultiStreamDecoderTypeE");
    assert(work_size && initialize && decode && reset && finalize);
    assert(work_size(48000, 2, 2, 0, 0) % 4096 == 0);
    unsigned char guest[256]; size_t consumed = 0; int samples = 0;
    assert(initialize(guest, 48000, 6, 4, 2, mapping, NULL, work_size(48000, 6, 4, 2, 0), 0) == 0);
    assert(decode(guest, &consumed, &samples, output, sizeof output, packet, size + 8) == 0);
    assert(consumed == (size_t)size + 8 && samples == 960);
    assert(memcmp(reference, output, sizeof output) == 0);
    assert(reset(guest) == 0);
    assert(decode(guest, &consumed, &samples, output, sizeof output, packet, size + 8) == 0);
    assert(memcmp(reference, output, sizeof output) == 0);
    assert(decode(guest, &consumed, &samples, output, sizeof output, packet, size + 7) != 0);
    finalize(guest);
    opus_multistream_decoder_destroy(independent);
    opus_multistream_encoder_destroy(encoder);
    puts("PASS: six-channel Nintendo packet framing, decoded PCM, final range, reset and truncated packet rejection");
}
