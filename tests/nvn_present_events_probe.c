#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include "nvn_frame_telemetry.h"

static void queue(uint64_t frame, uint64_t ticket, uint64_t serial, double observed) {
    nvn_present_event("queued", frame, ticket, serial, 0, 0, observed, "enqueue");
}
static void *writer(void *argument) {
    uint64_t thread = (uint64_t)(uintptr_t)argument;
    for (uint64_t i = 0; i < 32; i++) {
        uint64_t identity = thread * 32 + i;
        queue(20000 + identity, 10000 + identity, 30000 + identity, 5000 + identity / 1000.0);
        nvn_present_event("presented", 20000 + identity, 10000 + identity, 30000 + identity,
                          0, 4999 + identity / 1000.0, 5001 + identity / 1000.0, "drawable_callback");
    }
    return NULL;
}
static void *reader(void *unused) {
    (void)unused;
    for (uint64_t i = 0; i < 10000; i++) (void)nvn_present_time_get(10000 + i % 256);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(nvn_gpu_timestamps_valid(10, 10.002, 0));
    assert(!nvn_gpu_timestamps_valid(0, 55000, 0));  // observed uptime-sized bug
    assert(!nvn_gpu_timestamps_valid(-1, 1, 0));
    assert(!nvn_gpu_timestamps_valid(1, 0, 0));
    assert(nvn_gpu_timestamps_valid(1, 1, 0));  // valid positive zero-duration feedback
    assert(!nvn_gpu_timestamps_valid(0, 0, 0));  // no valid host timestamp remains unknown
    assert(!nvn_gpu_timestamps_valid(NAN, 1, 0));
    assert(!nvn_gpu_timestamps_valid(1, NAN, 0));
    assert(!nvn_gpu_timestamps_valid(INFINITY, INFINITY, 0));
    assert(!nvn_gpu_timestamps_valid(1, INFINITY, 0));
    assert(!nvn_gpu_timestamps_valid(1, 1e20, 0));
    assert(!nvn_gpu_timestamps_valid(1, 2, 1));
    if (!strcmp(argv[1], "disabled")) {
        assert(!getenv("DM_NVN_TSV"));
        queue(10, 1, 7001, 1000);
        nvn_present_event("presented", 10, 1, 7001, 0, 1000.01, 1000.02, "drawable_callback");
        assert(nvn_present_time_get(1) == 1000.01);
        assert(!g_present_metrics_file);
        return 0;
    }
    queue(10, 1, 7001, 1000);
    queue(11, 2, 7002, 1000.01);
    queue(1034, 1025, 8025, 2000);
    nvn_present_event("presented", 1034, 1025, 8025, 0, 2000.05, 2000.06, "drawable_callback");
    // Callback arrives much later than eight frames and after its legacy ring slot was reused.
    nvn_present_event("presented", 10, 1, 7001, 0, 1000.01, 2000.07, "drawable_callback");
    assert(nvn_present_time_get(1025) == 2000.05);
    assert(nvn_present_time_get(1) == 0);
    nvn_present_event("presented", 11, 2, 7002, 0, 1000.02, 2000.08, "drawable_callback");
    queue(12, 3, 7003, 2000.09);
    nvn_present_event("dropped", 12, 3, 7003, 0, 0, 2000.1, "merged");
    nvn_present_event("presented", 12, 3, 7003, 1, 2000.11, 2000.12, "drawable_callback");
    assert(nvn_present_time_get(3) == -1);  // generated drawable cannot overwrite game terminal state
    queue(13, 4, 7004, 2000.13);
    nvn_present_event("presented", 13, 4, 7004, 0, 0, 2000.14, "drawable_callback");
    queue(14, 5, 7005, 2000.15);
    nvn_present_event("presented", 14, 5, 7005, 0, NAN, 2000.16, "drawable_callback");
    queue(15, 6, 7006, 2000.17);
    nvn_present_event("presented", 15, 6, 7006, 0, -1, 2000.18, "drawable_callback");
    queue(16, 7, 7007, 2000.19);  // deliberately unresolved, must remain observable
    nvn_present_event("dropped", 17, 0, 7008, 0, 0, 2000.2, "discard_before_enqueue");
    for (uint64_t ticket = 8; ticket <= 9; ticket++) {
        queue(10 + ticket, ticket, 7000 + ticket, 2000.21);
        nvn_present_event("presented", 10 + ticket, ticket, 7000 + ticket, 0, 2000.22, 2000.23, "drawable_callback");
    }
    pthread_t threads[8], observer;
    assert(!pthread_create(&observer, NULL, reader, NULL));
    for (uintptr_t i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, writer, (void *)i));
    for (int i = 0; i < 8; i++) assert(!pthread_join(threads[i], NULL));
    assert(!pthread_join(observer, NULL));
    assert(!g_present_metrics_error);
    return 0;
}
