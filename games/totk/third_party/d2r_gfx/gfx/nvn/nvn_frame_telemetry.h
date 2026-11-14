/* DM_NVN_TSV also writes <path>.presented.tsv. Actual/observed timestamps use mach_absolute_time /
   CACurrentMediaTime seconds (Python time.monotonic on macOS), never an assumed callback-frame delay. */
#ifndef NVN_FRAME_TELEMETRY_H
#define NVN_FRAME_TELEMETRY_H
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int nvn_gpu_timestamps_valid(double start, double end, int failed) {
    return !failed && isfinite(start) && isfinite(end) && start > 0 && end >= start &&
           end < (double)UINT64_MAX / 1e9;
}

#define NVN_PRESENT_TIME_RING 1024
typedef struct { uint64_t ticket; double time; } nvn_present_time;
static nvn_present_time g_present_times[NVN_PRESENT_TIME_RING];
static pthread_mutex_t g_present_metrics_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_present_metrics_once = PTHREAD_ONCE_INIT;
static FILE *g_present_metrics_file;
static int g_present_metrics_error;

/* Called inside the metrics mutex, once. Only an enabled diagnostic creates a file. */
static void nvn_present_metrics_init(void) {
    const char *path = getenv("DM_NVN_TSV");
    if (!path || !*path) return;
    const char suffix[] = ".presented.tsv";
    size_t length = strlen(path);
    if (length > SIZE_MAX - sizeof suffix) return;
    char *output = malloc(length + sizeof suffix);
    if (!output) return;
    memcpy(output, path, length); memcpy(output + length, suffix, sizeof suffix);
    g_present_metrics_file = fopen(output, "w");
    if (!g_present_metrics_file) fprintf(stderr, "[present-metrics] cannot open %s\n", output);
    free(output);
    if (g_present_metrics_file) fprintf(g_present_metrics_file,
        "# nvn-present-events-v1 clock=mach_absolute_time_seconds; rows are in publication order, not display order\n"
        "event\tframe\tticket\tserial\tgenerated\tpresented_s\tobserved_s\treason\n");
}

/* Identity is captured by the caller, not reconstructed from the current frame. Queued rows expose missing/late
   callbacks; terminal rows are exported on completion/drop even after ring wrap. Reasons are constant literals.
   generated=1 is an extra interpolated drawable, not the game frame's terminal state. */
static void nvn_present_event(const char *event, uint64_t frame, uint64_t ticket, uint64_t serial,
                              int generated, double presented, double observed, const char *reason) {
    pthread_mutex_lock(&g_present_metrics_mu);
    if (!strcmp(event, "presented")) {
        if (presented == 0) { event = "dropped"; reason = "drawable_not_presented"; }
        else if (!isfinite(presented) || presented < 0) { event = "invalid"; reason = "invalid_presented_time"; }
    }
    if (!generated && ticket && strcmp(event, "queued")) {
        double legacy = !strcmp(event, "presented") ? presented : -1;
        nvn_present_time *slot = &g_present_times[ticket % NVN_PRESENT_TIME_RING];
        /* A very late result is exported but cannot evict a newer legacy-ring generation. */
        if (slot->ticket <= ticket) *slot = (nvn_present_time){ticket, legacy};
    }
    pthread_once(&g_present_metrics_once, nvn_present_metrics_init);
    if (g_present_metrics_file && !g_present_metrics_error) {
        if (!isfinite(presented)) presented = 0;
        if (!isfinite(observed) || observed <= 0) { event = "invalid"; observed = 0; reason = "invalid_observed_time"; }
        int failed = fprintf(g_present_metrics_file, "%s\t%llu\t%llu\t%llu\t%d\t%.9f\t%.9f\t%s\n", event,
            (unsigned long long)frame, (unsigned long long)ticket, (unsigned long long)serial,
            generated != 0, presented, observed, reason) < 0;
        failed |= fflush(g_present_metrics_file) != 0;  // live evidence; no fsync and no callback-latency deadline
        if (failed) { g_present_metrics_error = 1; fprintf(stderr, "[present-metrics] write failed; sidecar incomplete\n"); }
    }
    pthread_mutex_unlock(&g_present_metrics_mu);
}

static double nvn_present_time_get(uint64_t ticket) {
    pthread_mutex_lock(&g_present_metrics_mu);
    nvn_present_time entry = g_present_times[ticket % NVN_PRESENT_TIME_RING];
    pthread_mutex_unlock(&g_present_metrics_mu);
    return entry.ticket == ticket ? entry.time : 0;
}
#endif
