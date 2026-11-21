// hle_audio.c unit test (silent sink): release events wake the reader, which drains the buffers in order.
// Expected values are computed directly from the sample rate and block length: 4800 frames / 48 kHz = 100 ms.
#include "../hle_audio.c"
#include <time.h>
#include <sys/mman.h>
__thread _Atomic int dm_host_work;   // defined by the graphics host in a full build
uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec; }
static struct { const void *k; void *v; } om[8];
void *objmap_get(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) return om[i].v; return NULL; }
void objmap_put(const void *k, void *v) { for (int i = 0; i < 8; i++) if (!om[i].k) { om[i].k = k; om[i].v = v; return; } }
void *objmap_del(const void *k) { for (int i = 0; i < 8; i++) if (om[i].k == k) { om[i].k = 0; return om[i].v; } return NULL; }
static _Atomic int g_sig;
void hle_signal_system_event(void *g) { (void)g; atomic_fetch_add_explicit(&g_sig, 1, memory_order_seq_cst); }
void hle_bind_system_event(void *g, int autoclear) { (void)g; (void)autoclear; }   // the real implementation needs a guest thread + SDK kernel event; the unit test only counts triggers
module_t *module_of(const void *p) { (void)p; return NULL; }   // call sites in the log: no guest modules in the unit test
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }   // hle_svc.c: no guest address space in the unit test, so the big-stack take-overs (Start / Stop / CloseAudioOut, hle_bigstack.c) run on the calling host stack
void dm_host_helper_qos(qos_class_t q) { (void)q; }   // hle_thread.c: the helper threads keep the default class in the unit test

// A callback can finish several buffers, and the notifier can combine pending wakes.
// Only inspect the released queue after an event, then take everything it announced.
static int drain_signaled(void *out, gbuf *buffers, int count, int *got, int *seen) {
    int signals = atomic_load_explicit(&g_sig, memory_order_acquire);
    if (signals == *seen) return 1;
    *seen = signals;
    gbuf *r;
    while ((r = h_au_Released(out))) {
        if (*got >= count || r != &buffers[*got]) {
            fprintf(stderr, "unexpected released buffer at index %d of %d\n", *got, count);
            return 0;
        }
        (*got)++;
    }
    return 1;
}

// Exercise the actual device callback, not just a mixing helper. A stereo
// device must hear every channel of a 5.1 guest buffer and release the complete
// six-channel buffers at the same frame count, including a split inside a block.
static int test_surround_output(void) {
    enum { FRAMES = 273, SPLIT = 17 };
    const int16_t expected[8][2] = {
        {10000, 0}, {0, 10000}, {7070, 7070}, {2509, 2509},
        {7070, 0}, {0, 7070}, {32767, 32767}, {-32768, -32768}
    };
    for (int test = 0; test < 8; test++) {
        int16_t in[FRAMES * 6] = {0}, out[FRAMES * 2];
        for (int f = 0; f < FRAMES; f++) for (int ch = 0; ch < 6; ch++)
            in[f * 6 + ch] = test < 6 ? (test == ch ? 10000 : 0) : test == 6 ? 32767 : -32768;
        memset(out, 0xa5, sizeof out);
        gbuf b[2] = {{in, SPLIT * 12, SPLIT * 12}, {in + SPLIT * 6, (FRAMES - SPLIT) * 12, (FRAMES - SPLIT) * 12}};
        aout a = {.ch = 6};
        atomic_init(&a.state, STATE_STARTED); atomic_init(&a.vol, 0x3f800000u);
        a.q[0] = b; a.q[1] = b + 1; atomic_init(&a.q_tail, 2);
        AudioBufferList bl = {.mNumberBuffers = 1, .mBuffers = {{2, sizeof out, out}}};
        au_render(&a, NULL, NULL, 0, FRAMES, &bl);
        for (int f = 0; f < FRAMES; f++) for (int ch = 0; ch < 2; ch++) {
            if (out[f * 2 + ch] != expected[test][ch]) {
                printf("FAIL surround callback: case %d frame %d channel %d expected %d got %d\n", test, f, ch, expected[test][ch], out[f * 2 + ch]); return 1;
            }
        }
        if (atomic_load(&a.q_head) != 2 || atomic_load(&a.rel_tail) != 2 || a.rel[0] != b || a.rel[1] != b + 1 || a.pos || atomic_load(&a.under_n)) {
            puts("FAIL surround callback: guest buffer consumption / release order"); return 1;
        }
    }
    puts("surround callback: all six channels audible, saturation and split-buffer release verified");
    return 0;
}
// The guest AudioOut's front as the SDK fills it (au_info), and its lifetime under a guard like the middleware's: open, play, go to the background, close only if the first word says open,
// make the PCM inaccessible (a consumer left running would fault on it) and come back, 12 times (PR #109). Old pages stay mapped PROT_NONE, so no new buffer lands at a stale address
static int test_output_lifetime(void) {
    enum { CYCLES = 12 };
    uint32_t out[16] = {0}; uint8_t prm[16];
    size_t page = (size_t)getpagesize(), bytes = page * 4;
    void *pcm[CYCLES]; gbuf *buf[CYCLES];
    h_au_InitParam(prm);
    for (int i = 0; i < CYCLES; i++) {
        pcm[i] = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        buf[i] = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (pcm[i] == MAP_FAILED || buf[i] == MAP_FAILED) return 1;
        memset(out, 0, sizeof out);
        if (h_au_OpenDef0(out, prm)) return 1;
        if (out[0] != RATE || out[1] != 2 || out[2] != FMT_PCM16 || out[3] != STATE_STOPPED) {
            printf("FAIL AudioOut open: guest fields %u %u %u %u, expected %u 2 %u %u as the SDK writes them\n", out[0], out[1], out[2], out[3], RATE, FMT_PCM16, STATE_STOPPED); return 1;
        }
        h_au_SetBuf(buf[i], pcm[i], bytes, bytes);
        if (!h_au_Append(out, buf[i])) return 1;
        h_au_Start(out); usleep(1000); dm_audio_suspend(1);
        if (out[0]) { if (h_au_State(out) == STATE_STARTED) h_au_Stop(out); h_au_Close(out); }
        if (out[0] != RATE || out[3] != STATE_STOPPED || objmap_get(out)) { puts("FAIL AudioOut close: state not stopped or the host output still open"); return 1; }
        for (int k = 0; k < AOMAX; k++) if (g_aos[k]) { puts("FAIL AudioOut close: a consumer is still registered"); return 1; }
        if (mprotect(pcm[i], bytes, PROT_NONE) || mprotect(buf[i], page, PROT_NONE)) return 1;
        dm_audio_suspend(0); usleep(10000);
    }
    for (int i = 0; i < CYCLES; i++) { munmap(pcm[i], bytes); munmap(buf[i], page); }
    printf("AudioOut fields and lifetime (%s): %d guarded open / close / unmap / resume cycles, no consumer left\n", real_audio() ? "real output" : "silent sink", CYCLES);
    return 0;
}
int main(void) {
    if (test_surround_output() || test_output_lifetime()) return 1;
    char out[64], ev[64]; uint8_t prm[16]; gbuf b[3]; static int16_t pcm[3][4800 * 2];
    h_au_InitParam(prm);
    if (h_au_Open(out, ev, NULL, prm)) return 1;
    if (h_au_Ch(out) != 2 || h_au_Rate(out) != 48000 || h_au_State(out) != STATE_STOPPED) { puts("FAIL parameters"); return 1; }
    for (int i = 0; i < 3; i++) { h_au_SetBuf(&b[i], pcm[i], sizeof pcm[i], sizeof pcm[i]); if (!h_au_Append(out, &b[i])) return 1; }
    if (h_au_Released(out)) { puts("FAIL buffers released before start"); return 1; }
    uint64_t t0 = now_ns(); h_au_Start(out);
    int got = 0, seen = 0, order_ok = 1;
    while (got < 3 && now_ns() - t0 < 10000000000ull) {
        order_ok = drain_signaled(out, b, 3, &got, &seen);
        if (!order_ok) break;
        if (got < 3) usleep(1000);
    }
    h_au_Close(out);
    printf("released %d/3 in order=%d after %d signals\n", got, order_ok, seen);
    if (!order_ok || got != 3) return 1;

    // Scenario 2: the real audio output sink (PCM all 0, inaudible) plays 6 blocks of 100 ms each, pausing at 150 ms (app to background / incoming call) and resuming at 350 ms.
    // During the pause the silent sink takes over. Release events must let the reader reclaim all six blocks in order at real-time pace.
    setenv("DM_AUDIO", "1", 1);
    if (test_output_lifetime()) return 1;
    char out2[64]; gbuf b2[6]; static int16_t pcm2[6][4800 * 2]; g_sig = 0;
    if (h_au_Open(out2, ev, NULL, prm)) return 1;
    for (int i = 0; i < 6; i++) { h_au_SetBuf(&b2[i], pcm2[i], sizeof pcm2[i], sizeof pcm2[i]); h_au_Append(out2, &b2[i]); }
    t0 = now_ns(); h_au_Start(out2);
    int got2 = 0, seen2 = 0, susp = 0, resumed = 0;
    while (got2 < 6 && now_ns() - t0 < 10000000000ull) {
        uint64_t ms = (now_ns() - t0) / 1000000;
        if (!susp && ms >= 150) { dm_audio_suspend(1); susp = 1; }
        if (susp && !resumed && ms >= 350) { dm_audio_suspend(0); resumed = 1; }
        order_ok = drain_signaled(out2, b2, 6, &got2, &seen2);
        if (!order_ok) break;
        if (got2 < 6) usleep(1000);
    }
    double total = (now_ns() - t0) / 1e6;
    h_au_Close(out2);
    int ok2 = got2 == 6 && order_ok && susp && resumed && total > 520;
    printf("suspend / resume: released %d/6 in order=%d after %d signals, total duration lower bound 520 ms, got %.0f ms %s\n", got2, order_ok, seen2, total, ok2 ? "✓" : "✗");
    if (!ok2) return 1;

    // Scenario 3: the device stops calling back while the output plays (iOS does this when another audio session takes over, without telling the AudioUnit). 20 blocks of 100 ms; at 150 ms the AudioUnit is stopped behind the host's back.
    // Expected: the watchdog sees no callback for 200 ms, the silent sink takes over (all 20 blocks still released in order, about 2 s plus the 200-250 ms gap), and the AudioUnit is restarted 1 s later and calls back again
    // Allow 5 s for scheduling and AudioUnit stop/start delays on virtual CI Macs; buffer order, the playback lower bound and resumed callbacks still have to hold.
    // The virtual output of a CI Mac can also pause by itself for more than 200 ms after the restart: the watchdog then rightly stands the silent sink in again and retries 1 s later, which can fall after the
    // last release (CI: "AudioUnit restarted", a second "has not called back for 204 ms", then Close 1 s later, "calling back again=0"). So the backend is followed on every poll until a restarted AudioUnit
    // has called back, up to 20 s (the watchdog's back-off of 1, 2, 4 s fits), instead of being read once 100 ms after the last release; it still fails when no restarted AudioUnit ever calls back
    char out3[64]; gbuf b3[20]; static int16_t pcm3[20][4800 * 2]; g_sig = 0;
    if (h_au_Open(out3, ev, NULL, prm)) return 1;
    for (int i = 0; i < 20; i++) h_au_SetBuf(&b3[i], pcm3[i], sizeof pcm3[i], sizeof pcm3[i]);
    int app3 = 0; for (; app3 < 20 && h_au_Append(out3, &b3[app3]); app3++) ;
    aout *a3 = ao(out3); t0 = now_ns(); h_au_Start(out3);
    int got3 = 0, order3 = 1, cut = 0, standins = 0, sink = 0, restarted = 0, back = 0; uint64_t cb_restart = 0; double total3 = 0;
    while (!(got3 == 20 && back) && now_ns() - t0 < 20000000000ull) {
        uint64_t ms = (now_ns() - t0) / 1000000;
        if (!cut && ms >= 150) { AudioOutputUnitStop(a3->au); cut = 1; }
        // The watchdog changes the backend under g_aos_lock, and a restart comes at least 1 s after the silent sink stood in, so a 1 ms poll sees every stand-in and every restart
        pthread_mutex_lock(&g_aos_lock);
        if (cut && a3->backend == BK_NULL && a3->stalled) { standins += !sink; sink = 1; restarted = 0; }
        else if (standins && a3->backend == BK_AU && !a3->stalled) {
            uint64_t n = atomic_load(&a3->cb_n); sink = 0;
            if (!restarted) { restarted = 1; cb_restart = n; } else if (n > cb_restart) back = 1;   // callbacks counted after the restart was seen
        }
        pthread_mutex_unlock(&g_aos_lock);
        gbuf *r = got3 < 20 ? h_au_Released(out3) : NULL;
        if (!r) { usleep(1000); continue; }
        order3 &= r == &b3[got3];
        if (++got3 == 20) total3 = (now_ns() - t0) / 1e6;
        if (app3 < 20) h_au_Append(out3, &b3[app3++]);
    }
    double waited3 = (now_ns() - t0) / 1e6;
    h_au_Close(out3);
    int ok3 = got3 == 20 && order3 && total3 > 1900 && total3 < 5000 && back;
    printf("device stall: released %d/20 in order=%d in %.0f ms (expected 1900-5000), AudioUnit restarted and calling back again=%d (silent sink stood in %d times, %.0f ms) %s\n", got3, order3, total3, back, standins, waited3, ok3 ? "✓" : "✗");
    return !ok3;
}
