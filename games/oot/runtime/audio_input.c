/* The microphone streams (audio_input.h): one capture fans out to a bounded queue per open stream, downmixed and resampled
   to 48 kHz mono PCM16, with a gain and a soft limiter. From seedds/nxnative c25d4011 (PR #1822, dm/host/audio_input.c,
   MIT, Copyright (c) 2026 seedds), unchanged but for the switches (OOT_MIC, OOT_MIC_GAIN) and the log tag. */
#include "audio_input.h"
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { INPUT_MAX = 8, INPUT_CLOSED = 0, INPUT_STOPPED = 1, INPUT_STARTED = 2, INPUT_MAX_FRAMES = 96000 };

struct dm_audio_input_stream {
    int16_t *samples;
    uint32_t capacity, sample_mask;
    _Atomic uint32_t head, tail;
    _Atomic uint32_t gain_bits, user_gain_bits;
    _Atomic uint64_t dropped, captured, read, read_requests, underruns, zero_fill;
    _Atomic uint64_t pre_squares, post_squares, level_samples;
    _Atomic uint32_t pre_peak, post_peak;
    _Atomic uint64_t last_report_ns;
    _Atomic int state;
};

static _Atomic(dm_audio_input_stream *) input_streams[INPUT_MAX];
static _Atomic uint32_t input_writers;
static _Atomic int input_ready;
static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned input_started_count;

// Resampler state belongs to the single AUHAL producer callback.
static double input_rate, input_next_output;
static uint64_t input_frame_index;
static float input_previous;
static int input_have_previous;

typedef struct {
    uint64_t samples, pre_squares, post_squares;
    uint32_t pre_peak, post_peak;
} input_level_window;

static uint32_t float_bits(float f) { uint32_t u; memcpy(&u, &f, sizeof u); return u; }
static float bits_float(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }

static float input_user_gain(void) {
    const char *text = getenv("OOT_MIC_GAIN");
    if (!text || !*text) return 4.0f;
    char *end = NULL;
    float gain = strtof(text, &end);
    return end != text && !*end && isfinite(gain) && gain >= 1.0f && gain <= 8.0f ? gain : 4.0f;
}

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void level_add(input_level_window *w, float before, float after) {
    double a = fabs((double)before), b = fabs((double)after);
    uint32_t ap = a >= 4294.0 ? UINT32_MAX : (uint32_t)(a * 1000000.0);
    uint32_t bp = b >= 4294.0 ? UINT32_MAX : (uint32_t)(b * 1000000.0);
    uint64_t asq = a >= 4294.0 ? UINT64_MAX / 4 : (uint64_t)(a * a * 1000000.0);
    uint64_t bsq = b >= 4294.0 ? UINT64_MAX / 4 : (uint64_t)(b * b * 1000000.0);
    w->samples++;
    w->pre_squares += asq;
    w->post_squares += bsq;
    if (ap > w->pre_peak) w->pre_peak = ap;
    if (bp > w->post_peak) w->post_peak = bp;
}

static void atomic_max_u32(_Atomic uint32_t *target, uint32_t value) {
    uint32_t old = atomic_load_explicit(target, memory_order_relaxed);
    while (old < value && !atomic_compare_exchange_weak_explicit(target, &old, value, memory_order_relaxed, memory_order_relaxed)) { }
}

static float soft_limit(float value) {
    if (!isfinite(value)) return 0.0f;
    float magnitude = fabsf(value);
    if (magnitude <= 0.98f) return value;
    float excess = magnitude - 0.98f;
    float limited = 0.98f + 0.02f * excess / (excess + 0.02f);
    return copysignf(limited, value);
}

static float dbfs(uint32_t peak, uint64_t squares, uint64_t samples, int rms) {
    double level = rms ? (samples ? sqrt((double)squares / 1000000.0 / (double)samples) : 0.0) : (double)peak / 1000000.0;
    return level > 0.0 ? (float)(20.0 * log10(level)) : -120.0f;
}

int dm_audio_input_available(void) {
    const char *enabled = getenv("OOT_MIC");
    return enabled && !strcmp(enabled, "1") && dm_audio_input_platform_device_available();
}

void dm_audio_input_device_name(char out[DM_AUDIO_INPUT_NAME_SIZE]) {
    memset(out, 0, DM_AUDIO_INPUT_NAME_SIZE);
    memcpy(out, DM_AUDIO_INPUT_DEVICE, sizeof DM_AUDIO_INPUT_DEVICE);
}

dm_audio_input_stream *dm_audio_input_open(int format, int channels, int rate, int capacity_frames) {
    if (format != DM_AUDIO_INPUT_PCM16 || channels != DM_AUDIO_INPUT_CHANNELS || rate != DM_AUDIO_INPUT_RATE ||
        capacity_frames <= 0 || capacity_frames > INPUT_MAX_FRAMES || !dm_audio_input_available()) return NULL;

    pthread_mutex_lock(&input_lock);
    int free_slot = -1;
    for (int i = 0; i < INPUT_MAX; i++) if (!atomic_load_explicit(&input_streams[i], memory_order_relaxed)) { free_slot = i; break; }
    if (free_slot < 0) { pthread_mutex_unlock(&input_lock); return NULL; }

    // Cursor arithmetic wraps at 2^32. Power-of-two storage keeps the slot sequence continuous across that wrap.
    // The stream still exposes and enforces the exact capacity the game requested.
    uint32_t storage = 1;
    while (storage < (uint32_t)capacity_frames) storage <<= 1;
    dm_audio_input_stream *s = calloc(1, sizeof *s);
    if (s) s->samples = calloc(storage, sizeof *s->samples);
    if (!s || !s->samples) {
        free(s);
        pthread_mutex_unlock(&input_lock);
        return NULL;
    }
    s->capacity = (uint32_t)capacity_frames;
    s->sample_mask = storage - 1;
    atomic_init(&s->head, 0); atomic_init(&s->tail, 0);
    atomic_init(&s->gain_bits, float_bits(1.0f));
    atomic_init(&s->user_gain_bits, float_bits(input_user_gain()));
    atomic_init(&s->dropped, 0); atomic_init(&s->captured, 0); atomic_init(&s->read, 0); atomic_init(&s->read_requests, 0); atomic_init(&s->underruns, 0); atomic_init(&s->zero_fill, 0);
    atomic_init(&s->pre_squares, 0); atomic_init(&s->post_squares, 0); atomic_init(&s->level_samples, 0);
    atomic_init(&s->pre_peak, 0); atomic_init(&s->post_peak, 0); atomic_init(&s->last_report_ns, 0);
    atomic_init(&s->state, INPUT_STOPPED);
    atomic_store_explicit(&input_streams[free_slot], s, memory_order_seq_cst);
    pthread_mutex_unlock(&input_lock);
    return s;
}

static void input_close_capture_locked(void) {
    atomic_store_explicit(&input_ready, 0, memory_order_release);
    dm_audio_input_platform_close();  // joins the callback before resetting its resampler
    input_rate = 0;
    input_have_previous = 0;
}

static int input_start_capture_locked(void) {
    input_close_capture_locked();
    if (dm_audio_input_platform_prepare() || dm_audio_input_platform_start()) {
        dm_audio_input_platform_close();
        return -1;
    }
    atomic_store_explicit(&input_ready, 1, memory_order_release);
    return 0;
}

// The route listener runs on its own control queue. Preserve the streams and their start requests
// through a missing device or failed reopen, so a later device event or Start can bring them back.
void dm_audio_input_route_changed(void) {
    pthread_mutex_lock(&input_lock);
    if (input_started_count && dm_audio_input_platform_route_changed()) input_start_capture_locked();
    pthread_mutex_unlock(&input_lock);
}

static void input_stop_locked(dm_audio_input_stream *s) {
    if (atomic_load_explicit(&s->state, memory_order_relaxed) != INPUT_STARTED) return;
    atomic_store_explicit(&s->state, INPUT_STOPPED, memory_order_release);
    if (!--input_started_count) input_close_capture_locked();
}

void dm_audio_input_close(dm_audio_input_stream *s) {
    if (!s) return;
    pthread_mutex_lock(&input_lock);
    input_stop_locked(s);
    for (int i = 0; i < INPUT_MAX; i++) {
        if (atomic_load_explicit(&input_streams[i], memory_order_relaxed) == s) {
            atomic_store_explicit(&input_streams[i], NULL, memory_order_seq_cst);
            break;
        }
    }
    pthread_mutex_unlock(&input_lock);

    // A callback that has sampled the old slot must finish. A callback entering after the
    // count is observed at zero can only see the cleared slot (the writer-count and slot atomics are seq_cst).
    while (atomic_load_explicit(&input_writers, memory_order_seq_cst)) sched_yield();
    free(s->samples);
    free(s);
}

int dm_audio_input_start(dm_audio_input_stream *s) {
    if (!s) return -1;
    pthread_mutex_lock(&input_lock);
    int was_started = atomic_load_explicit(&s->state, memory_order_relaxed) == INPUT_STARTED;
    if (!was_started) atomic_store_explicit(&s->state, INPUT_STARTED, memory_order_release);
    int result = 0;
    if (!atomic_load_explicit(&input_ready, memory_order_acquire) || dm_audio_input_platform_failed())
        result = input_start_capture_locked();
    if (!was_started) {
        if (result) atomic_store_explicit(&s->state, INPUT_STOPPED, memory_order_release);
        else input_started_count++;
    }
    pthread_mutex_unlock(&input_lock);
    return result;
}

void dm_audio_input_stop(dm_audio_input_stream *s) {
    if (!s) return;
    pthread_mutex_lock(&input_lock);
    input_stop_locked(s);
    pthread_mutex_unlock(&input_lock);
}

void dm_audio_input_purge(dm_audio_input_stream *s) {
    if (!s) return;
    uint32_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);
    atomic_store_explicit(&s->head, tail, memory_order_release);
}

int dm_audio_input_state(const dm_audio_input_stream *s) {
    if (!s) return INPUT_CLOSED;
    int state = atomic_load_explicit(&s->state, memory_order_acquire);
    if (state == INPUT_STARTED && (!atomic_load_explicit(&input_ready, memory_order_acquire) || dm_audio_input_platform_failed())) return INPUT_STOPPED;
    return state;
}

int dm_audio_input_capacity(const dm_audio_input_stream *s) { return s ? (int)s->capacity : 0; }

static void input_report_if_due(dm_audio_input_stream *s) {
    uint64_t now = monotonic_ns(), last = atomic_load_explicit(&s->last_report_ns, memory_order_relaxed);
    if (!now || now - last < 2000000000ull || !atomic_compare_exchange_strong_explicit(&s->last_report_ns, &last, now, memory_order_relaxed, memory_order_relaxed)) return;
    char name[0x100] = "unknown", uid[0x100] = "unknown";
    double rate = 0.0; int channels = 0;
    dm_audio_input_platform_info(name, uid, &rate, &channels);
    uint64_t captured = atomic_exchange_explicit(&s->captured, 0, memory_order_relaxed);
    uint64_t read = atomic_exchange_explicit(&s->read, 0, memory_order_relaxed);
    uint64_t requests = atomic_exchange_explicit(&s->read_requests, 0, memory_order_relaxed);
    uint64_t underruns = atomic_exchange_explicit(&s->underruns, 0, memory_order_relaxed);
    uint64_t zero = atomic_exchange_explicit(&s->zero_fill, 0, memory_order_relaxed);
    uint64_t dropped = atomic_exchange_explicit(&s->dropped, 0, memory_order_relaxed);
    uint64_t samples = atomic_exchange_explicit(&s->level_samples, 0, memory_order_relaxed);
    uint64_t pre_squares = atomic_exchange_explicit(&s->pre_squares, 0, memory_order_relaxed);
    uint64_t post_squares = atomic_exchange_explicit(&s->post_squares, 0, memory_order_relaxed);
    uint32_t pre_peak = atomic_exchange_explicit(&s->pre_peak, 0, memory_order_relaxed);
    uint32_t post_peak = atomic_exchange_explicit(&s->post_peak, 0, memory_order_relaxed);
    float user_gain = bits_float(atomic_load_explicit(&s->user_gain_bits, memory_order_relaxed));
    float game_gain = bits_float(atomic_load_explicit(&s->gain_bits, memory_order_relaxed));
    fprintf(stderr, "[ns2-audio] audio input: device=\"%s\" uid=\"%s\" rate=%.0f channels=%d captured=%llu read=%llu read_requests=%llu underruns=%llu zero_fill=%llu dropped=%llu gain=%.2fx game_gain=%.2fx pre_peak=%.1f dBFS pre_rms=%.1f dBFS post_peak=%.1f dBFS post_rms=%.1f dBFS resampler=%.6f\n",
            name, uid, rate, channels,
            (unsigned long long)captured, (unsigned long long)read, (unsigned long long)requests,
            (unsigned long long)underruns, (unsigned long long)zero, (unsigned long long)dropped,
            user_gain, game_gain, dbfs(pre_peak, pre_squares, samples, 0), dbfs(pre_peak, pre_squares, samples, 1),
            dbfs(post_peak, post_squares, samples, 0), dbfs(post_peak, post_squares, samples, 1),
            rate > 0.0 ? rate / DM_AUDIO_INPUT_RATE : 0.0);
}

int dm_audio_input_filled(const dm_audio_input_stream *s) {
    if (!s || dm_audio_input_state(s) != INPUT_STARTED) return 0;
    uint32_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);
    uint32_t head = atomic_load_explicit(&s->head, memory_order_acquire);
    uint32_t n = tail - head;
    input_report_if_due((dm_audio_input_stream *)s);
    return n > s->capacity ? (int)s->capacity : (int)n;
}

int dm_audio_input_free(const dm_audio_input_stream *s) {
    if (!s) return 0;
    int filled = dm_audio_input_filled(s);
    return s->capacity - filled;
}

int dm_audio_input_read(dm_audio_input_stream *s, void *dst, int frames) {
    if (!s || !dst || frames <= 0) return 0;
    if (frames > (int)s->capacity) frames = (int)s->capacity;
    memset(dst, 0, (size_t)frames * sizeof(int16_t));
    if (dm_audio_input_state(s) != INPUT_STARTED) return 0;
    uint32_t head = atomic_load_explicit(&s->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);
    uint32_t available = tail - head;
    uint32_t n = available < (uint32_t)frames ? available : (uint32_t)frames;
    int16_t *out = dst;
    uint32_t slot = head & s->sample_mask;
    uint32_t first = s->sample_mask + 1 - slot;
    if (first > n) first = n;
    memcpy(out, s->samples + slot, first * sizeof *out);
    memcpy(out + first, s->samples, (n - first) * sizeof *out);
    atomic_store_explicit(&s->head, head + n, memory_order_release);
    atomic_fetch_add_explicit(&s->read_requests, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->read, n, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->zero_fill, (uint32_t)frames - n, memory_order_relaxed);
    if (n < (uint32_t)frames) atomic_fetch_add_explicit(&s->underruns, 1, memory_order_relaxed);
    input_report_if_due(s);
    return (int)n;
}

float dm_audio_input_gain(const dm_audio_input_stream *s) {
    return s ? bits_float(atomic_load_explicit(&s->gain_bits, memory_order_relaxed)) : 1.0f;
}

void dm_audio_input_set_gain(dm_audio_input_stream *s, float gain) {
    if (!s) return;
    if (!(gain >= 0.0f)) gain = 0.0f;
    if (gain > 8.0f) gain = 8.0f;
    atomic_store_explicit(&s->gain_bits, float_bits(gain), memory_order_relaxed);
}

static void input_push_sample(float value, dm_audio_input_stream *const *streams, input_level_window *levels) {
    if (!(value == value)) value = 0.0f;
    for (int i = 0; i < INPUT_MAX; i++) {
        dm_audio_input_stream *s = streams[i];
        if (!s || atomic_load_explicit(&s->state, memory_order_relaxed) != INPUT_STARTED) continue;
        float user_gain = bits_float(atomic_load_explicit(&s->user_gain_bits, memory_order_relaxed));
        float game_gain = bits_float(atomic_load_explicit(&s->gain_bits, memory_order_relaxed));
        float gain = user_gain * game_gain;
        if (gain > 8.0f) gain = 8.0f;
        float sample = soft_limit(value * gain);
        level_add(&levels[i], value, sample);
        uint32_t tail = atomic_load_explicit(&s->tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&s->head, memory_order_acquire);
        atomic_fetch_add_explicit(&s->captured, 1, memory_order_relaxed);
        if (tail - head >= s->capacity) { atomic_fetch_add_explicit(&s->dropped, 1, memory_order_relaxed); continue; }
        int16_t pcm = sample <= -1.0f ? INT16_MIN : sample >= 1.0f ? INT16_MAX : (int16_t)(sample * 32768.0f);
        s->samples[tail & s->sample_mask] = pcm;
        atomic_store_explicit(&s->tail, tail + 1, memory_order_release);
    }
}

/* A song played from the settings panel's 陶笛 page (ours, user 2026-11-20): while it plays it is the only producer, the
   capture's callback leaves the queues alone (dm_audio_input_process returns at once), so each queue keeps one writer. */
static _Atomic int input_injecting;
void dm_audio_input_inject_begin(void) {
    atomic_store_explicit(&input_injecting, 1, memory_order_seq_cst);
    while (atomic_load_explicit(&input_writers, memory_order_seq_cst)) sched_yield();   // a callback under way finishes first
}
void dm_audio_input_inject_end(void) { atomic_store_explicit(&input_injecting, 0, memory_order_seq_cst); }
void dm_audio_input_inject(const float *mono, uint32_t frames) {
    if (!mono || !frames) return;
    atomic_fetch_add_explicit(&input_writers, 1, memory_order_seq_cst);
    dm_audio_input_stream *streams[INPUT_MAX];
    input_level_window levels[INPUT_MAX] = {0};
    for (int i = 0; i < INPUT_MAX; i++) streams[i] = atomic_load_explicit(&input_streams[i], memory_order_seq_cst);
    for (uint32_t i = 0; i < frames; i++) input_push_sample(mono[i], streams, levels);
    for (int i = 0; i < INPUT_MAX; i++) {
        dm_audio_input_stream *s = streams[i];
        if (!s || !levels[i].samples) continue;
        atomic_fetch_add_explicit(&s->level_samples, levels[i].samples, memory_order_relaxed);
        atomic_fetch_add_explicit(&s->pre_squares, levels[i].pre_squares, memory_order_relaxed);
        atomic_fetch_add_explicit(&s->post_squares, levels[i].post_squares, memory_order_relaxed);
        atomic_max_u32(&s->pre_peak, levels[i].pre_peak);
        atomic_max_u32(&s->post_peak, levels[i].post_peak);
    }
    atomic_fetch_sub_explicit(&input_writers, 1, memory_order_seq_cst);
}
int dm_audio_input_listening(void) {   // a stream the game started: it is listening now (the ocarina is out)
    for (int i = 0; i < INPUT_MAX; i++) {
        dm_audio_input_stream *s = atomic_load_explicit(&input_streams[i], memory_order_seq_cst);
        if (s && atomic_load_explicit(&s->state, memory_order_acquire) == INPUT_STARTED) return 1;
    }
    return 0;
}

void dm_audio_input_process(const float *const *channels, int channel_count, uint32_t frames, double sample_rate) {
    if (!frames || !channels || channel_count < 1 || channel_count > 8 || !(sample_rate >= 8000.0 && sample_rate <= 192000.0)) return;
    for (int c = 0; c < channel_count; c++) if (!channels[c]) return;
    atomic_fetch_add_explicit(&input_writers, 1, memory_order_seq_cst);
    if (atomic_load_explicit(&input_injecting, memory_order_seq_cst)) {   // a song from the panel is playing instead
        atomic_fetch_sub_explicit(&input_writers, 1, memory_order_seq_cst);
        return;
    }
    dm_audio_input_stream *streams[INPUT_MAX];
    input_level_window levels[INPUT_MAX] = {0};
    for (int i = 0; i < INPUT_MAX; i++) streams[i] = atomic_load_explicit(&input_streams[i], memory_order_seq_cst);
    if (input_rate != sample_rate) {
        input_rate = sample_rate; input_next_output = 0; input_frame_index = 0; input_previous = 0; input_have_previous = 0;
    }
    double step = sample_rate / DM_AUDIO_INPUT_RATE;
    for (uint32_t i = 0; i < frames; i++) {
        float mono = 0.0f;
        for (int c = 0; c < channel_count; c++) mono += channels[c][i];
        mono /= (float)channel_count;
        if (!input_have_previous) {
            input_push_sample(mono, streams, levels);
            input_previous = mono; input_have_previous = 1; input_frame_index = 1; input_next_output = step;
            continue;
        }
        uint64_t t = input_frame_index++;
        while (input_next_output <= (double)t) {
            double fraction = input_next_output - (double)(t - 1);
            if (fraction >= 0.0) input_push_sample(input_previous + (mono - input_previous) * (float)fraction, streams, levels);
            input_next_output += step;
        }
        input_previous = mono;
    }
    for (int i = 0; i < INPUT_MAX; i++) {
        dm_audio_input_stream *s = streams[i];
        if (!s || !levels[i].samples) continue;
        atomic_fetch_add_explicit(&s->level_samples, levels[i].samples, memory_order_relaxed);
        atomic_fetch_add_explicit(&s->pre_squares, levels[i].pre_squares, memory_order_relaxed);
        atomic_fetch_add_explicit(&s->post_squares, levels[i].post_squares, memory_order_relaxed);
        atomic_max_u32(&s->pre_peak, levels[i].pre_peak);
        atomic_max_u32(&s->post_peak, levels[i].post_peak);
    }
    atomic_fetch_sub_explicit(&input_writers, 1, memory_order_seq_cst);
}
