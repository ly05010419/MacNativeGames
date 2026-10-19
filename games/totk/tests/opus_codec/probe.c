/* Exercise the production wrapper with real Opus packets. No game or audio
 * device is opened. The same capacity reproducer fails on the original code. */
#include "../../runtime/opus_codec.c"
#include <assert.h>
#include <stdatomic.h>
#include <time.h>

static _Atomic unsigned long checks;
_Noreturn void runtime_fail(const char *s) { fprintf(stderr, "probe failure: %s\n", s); exit(1); }
static void check(int ok, const char *s) { if (!ok) runtime_fail(s); atomic_fetch_add(&checks, 1); }
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static void *blocked_output;
static int decode_entered, allow_decode;
void dm_guest_write_prepare(void *p, size_t n) {
    (void)n;
    pthread_mutex_lock(&gate_lock);
    if (p == blocked_output) {
        decode_entered = 1; pthread_cond_broadcast(&gate_cond);
        while (!allow_decode) pthread_cond_wait(&gate_cond, &gate_lock);
    }
    pthread_mutex_unlock(&gate_lock);
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
typedef struct {
    int rate, channels, frames;
    uint8_t bytes[4096];
    size_t size;
    int16_t expected[1920];
} Packet;
static Packet make_packet(int rate, int channels) {
    Packet p = {.rate=rate, .channels=channels, .frames=rate/50};
    int16_t input[1920];
    for (int i = 0; i < p.frames * channels; i++) input[i] = (int16_t)((i * 71) % 16000 - 8000);
    int err;
    OpusEncoder *e = opus_encoder_create(rate, channels, OPUS_APPLICATION_AUDIO, &err);
    check(e && err == OPUS_OK, "reference encoder");
    int n = opus_encode(e, input, p.frames, p.bytes + 8, sizeof p.bytes - 8);
    check(n > 0, "encode real packet");
    opus_uint32 range; check(opus_encoder_ctl(e, OPUS_GET_FINAL_RANGE(&range)) == OPUS_OK, "encoder range");
    put_be32(p.bytes, (uint32_t)n); put_be32(p.bytes + 4, range); p.size = (size_t)n + 8;
    opus_encoder_destroy(e);
    OpusDecoder *d = opus_decoder_create(rate, channels, &err);
    check(d && err == OPUS_OK, "reference decoder");
    check(opus_decode(d, p.bytes + 8, n, p.expected, p.frames, 0) == p.frames, "reference decode");
    opus_uint32 actual; check(opus_decoder_ctl(d, OPUS_GET_FINAL_RANGE(&actual)) == OPUS_OK && actual == range, "reference final range");
    opus_decoder_destroy(d);
    return p;
}
static void open_decoder(void *key, int rate, int channels) {
    check(initialize(key, rate, channels, NULL, work_size(rate, channels, 0), 0) == 0, "initialize");
    check(sample_rate(key) == rate && channel_count(key) == channels, "format lookup");
}
static void packet_decode(void *key, const Packet *p) {
    int16_t out[1920]; size_t consumed = 0; int frames = 0;
    check(!decode(key, &consumed, &frames, out, sizeof out, p->bytes, p->size), "wrapper decode");
    check(consumed == p->size && frames == p->frames, "decoded sizes");
    check(!memcmp(out, p->expected, (size_t)frames * (size_t)p->channels * sizeof *out), "PCM matches independent libopus state");
}
static void capacity_and_reuse(void) {
    uint8_t keys[512]; Packet p = make_packet(48000, 2);
    for (size_t i = 0; i < sizeof keys; i++) open_decoder(keys + i, 48000, 2);
    check(decoder_count == sizeof keys, "512 simultaneous decoders beyond old limit");
    for (size_t i = 0; i < sizeof keys; i++) packet_decode(keys + i, &p);
    for (size_t i = 0; i < sizeof keys; i++) finalize(keys + i);
    check(!decoders && !decoder_count, "all states reclaimed");
    for (int i = 0; i < 2000; i++) { open_decoder(keys, 24000, 1); finalize(keys); }
    check(!decoders && !decoder_count && decoder_created == decoder_finalized, "2000 same-address lifecycles reclaimed");
    open_decoder(keys, 24000, 1);
    for (int i = 0; i < 1000; i++) {
        open_decoder(keys, 48000, 2); packet_decode(keys, &p);
        check(decoder_count == 1, "reinitialization keeps a single registry entry");
    }
    finalize(keys);
}
static void formats_and_errors(void) {
    const int rates[] = {8000,12000,16000,24000,48000}; uint8_t key;
    for (size_t i = 0; i < sizeof rates / sizeof *rates; i++) for (int ch = 1; ch <= 2; ch++) {
        Packet p = make_packet(rates[i], ch); open_decoder(&key, rates[i], ch);
        packet_decode(&key, &p);
        check(initialize(&key, rates[i], ch, NULL, work_size(rates[i], ch, 0), 0) == 0, "same guest reinitialization accepted");
        check(decoder_count == 1, "same guest reused without another registry entry");
        packet_decode(&key, &p); check(!reset(&key), "reset decoder"); packet_decode(&key, &p);
        int16_t out[1920]; size_t used = 0; int frames = 0;
        check(decode(&key, &used, &frames, out, sizeof out, p.bytes, p.size-1) == R_INVALID, "truncated packet rejected");
        check(decode(&key, &used, &frames, out, 0, p.bytes, p.size) == R_INVALID, "short output rejected without stuck lock");
        check(!reset(&key), "reset after decode failure"); packet_decode(&key, &p); finalize(&key);
    }
    check(initialize(&key, 44100, 2, NULL, 100000, 0) == R_INVALID, "invalid rate");
    check(initialize(&key, 48000, 3, NULL, 100000, 0) == R_INVALID, "invalid channels");
    check(initialize(&key, 48000, 2, NULL, 1, 0) == R_INVALID, "short work buffer");
    check(!decoder_count, "invalid input leaves registry empty");
}
static Packet concurrent_packet;
static pthread_mutex_t ready_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_cond = PTHREAD_COND_INITIALIZER;
static unsigned ready;
static void *worker(void *unused) {
    (void)unused; uint8_t keys[24];
    for (size_t i = 0; i < sizeof keys; i++) open_decoder(keys + i, 48000, 2);
    pthread_mutex_lock(&ready_lock); ready++; pthread_cond_broadcast(&ready_cond);
    while (ready < 8) pthread_cond_wait(&ready_cond, &ready_lock);
    pthread_mutex_unlock(&ready_lock);
    for (int round = 0; round < 40; round++) for (size_t i = 0; i < sizeof keys; i++) {
        open_decoder(keys+i, 48000, 2); packet_decode(keys+i, &concurrent_packet);
    }
    for (size_t i = 0; i < sizeof keys; i++) finalize(keys+i);
    for (int i = 0; i < 100; i++) { open_decoder(keys, 48000, 2); packet_decode(keys, &concurrent_packet); finalize(keys); }
    return NULL;
}
static void concurrent_streams(void) {
    pthread_t threads[8]; concurrent_packet = make_packet(48000, 2);
    for (size_t i = 0; i < 8; i++) check(!pthread_create(threads+i, NULL, worker, NULL), "create worker");
    for (size_t i = 0; i < 8; i++) check(!pthread_join(threads[i], NULL), "join worker");
    check(!decoders && !decoder_count && decoder_created == decoder_finalized, "parallel lifecycle balance");
}
static uint8_t finalizing_key;
static int16_t finalizing_output[1920];
static _Atomic int finalize_started, finalize_done;
static void *held_decode(void *unused) {
    (void)unused; size_t used; int frames;
    check(!decode(&finalizing_key, &used, &frames, finalizing_output, sizeof finalizing_output,
                  concurrent_packet.bytes, concurrent_packet.size), "held decode finishes");
    check(!memcmp(finalizing_output, concurrent_packet.expected, sizeof finalizing_output), "held decode PCM intact");
    return NULL;
}
static void *waiting_transition(void *reinitialize) {
    atomic_store(&finalize_started, 1);
    if (reinitialize) open_decoder(&finalizing_key, 48000, 2);
    else finalize(&finalizing_key);
    atomic_store(&finalize_done, 1); return NULL;
}
static void transition_waits_for_decode(int reinitialize) {
    open_decoder(&finalizing_key, 48000, 2);
    pthread_mutex_lock(&gate_lock);
    blocked_output = finalizing_output; decode_entered = allow_decode = 0;
    pthread_mutex_unlock(&gate_lock);
    atomic_store(&finalize_started, 0); atomic_store(&finalize_done, 0);
    pthread_t decoding, closing;
    check(!pthread_create(&decoding, NULL, held_decode, NULL), "create held decode");
    pthread_mutex_lock(&gate_lock);
    while (!decode_entered) pthread_cond_wait(&gate_cond, &gate_lock);
    pthread_mutex_unlock(&gate_lock);
    check(!pthread_create(&closing, NULL, waiting_transition, reinitialize ? &finalizing_key : NULL), "create lifecycle transition");
    struct timespec tick = {.tv_nsec=1000000};
    while (!atomic_load(&finalize_started)) nanosleep(&tick, NULL);
    struct timespec wait = {.tv_nsec=20000000}; nanosleep(&wait, NULL);
    check(!atomic_load(&finalize_done), "lifecycle transition waits for in-flight use");
    pthread_mutex_lock(&gate_lock); allow_decode = 1; pthread_cond_broadcast(&gate_cond); pthread_mutex_unlock(&gate_lock);
    check(!pthread_join(decoding, NULL) && !pthread_join(closing, NULL), "join lifetime race");
    check(atomic_load(&finalize_done), "lifecycle transition completed after decode");
    if (reinitialize) { packet_decode(&finalizing_key, &concurrent_packet); finalize(&finalizing_key); }
    check(!decoder_count && !decoders, "lifecycle reclaimed after decode");
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--bad-range")) {
        Packet p = make_packet(48000, 2); uint8_t key; open_decoder(&key, 48000, 2);
        p.bytes[7] ^= 1; packet_decode(&key, &p); runtime_fail("corrupt range accepted");
    }
    {   /* a header that records no final range (0) is decoded without the check (a40bc87); any other value is checked */
        Packet p = make_packet(48000, 2); uint8_t key; open_decoder(&key, 48000, 2);
        put_be32(p.bytes + 4, 0); packet_decode(&key, &p); finalize(&key);
    }
    capacity_and_reuse(); formats_and_errors(); concurrent_streams(); transition_waits_for_decode(0); transition_waits_for_decode(1);
    printf("{\"status\":\"pass\",\"checks\":%lu,\"simultaneous_decoders\":512,\"created\":%zu,\"finalized\":%zu,\"active\":%zu,\"reinitialized\":%zu,\"worker_threads\":8}\n",
           atomic_load(&checks), decoder_created, decoder_finalized, decoder_count, decoder_reinitialized);
    return 0;
}
