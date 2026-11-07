/* A recorder can become ready arbitrarily late. Its submit-boundary trigger
   must still capture the requested window, including delayed GPU feedback. */
#include "../third_party/d2r_gfx/gfx/nvn/totk_timeline.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Atomic int ready, release_writers;
static int checked;
#define CHECK(c, why) do { if (!(c)) { fprintf(stderr, "%s\n", why); return 1; } checked++; } while (0)
static void *initialize(void *unused) {
    (void)unused;
    totk_tl_at_frame(TL_SUB_IN, 1, 1, 0, 0);
    return NULL;
}
static void *feedback(void *unused) {
    (void)unused;
    totk_tl_at_frame(TL_GPU, 200, 100000, 7, 300);
    return NULL;
}
static void *write_rows(void *unused) {
    (void)unused;
    totk_tl_at_frame(TL_PASS_BEGIN, 400, 100001, 19, 29);
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&release_writers)) sched_yield();
    for (int i = 0; i < 25000; i++) totk_tl_at_frame(TL_PASS_BEGIN, 400, 100001, 19, 29);
    return NULL;
}
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char config[4096];
    if (snprintf(config, sizeof config, "%s:%s", argv[1],
                 getenv("OOT_TIMELINE_FIXED_NEGATIVE") ? "1:1" : "trigger") >= (int)sizeof config) return 2;
    if (setenv("TOTK_TIMELINE", config, 1)) return 2;
    unlink(argv[1]);
    pthread_t workers[4];
    for (int i = 0; i < 4; i++) if (pthread_create(&workers[i], NULL, initialize, NULL)) return 2;
    for (int i = 0; i < 4; i++) if (pthread_join(workers[i], NULL)) return 2;
    totk_tl_frame(1);
    totk_tl_frame(99999); /* Deliberately beyond every former startup window. */
    CHECK(!totk_tl_on(), "timeline started before the trigger");
    CHECK(!totk_tl_trigger(100000, 0), "zero count accepted");
    CHECK(!totk_tl_trigger(UINT64_MAX, 3), "overflowing window accepted");
    CHECK(totk_tl_trigger(100000, 3), "delayed trigger failed: fixed startup window already expired");
    CHECK(access(argv[1], F_OK) != 0, "waiting mode prematurely wrote output");
    CHECK(!totk_tl_trigger(200000, 3), "duplicate trigger replaced the window");
    totk_tl_frame(100000);
    totk_tl_at_frame(TL_COMMIT, 100, 100000, 7, 1);
    totk_tl_at_frame(TL_FENCE, 101, 100000, 7, 77);
    totk_tl_frame(100020);
    if (pthread_create(&workers[0], NULL, feedback, NULL) || pthread_join(workers[0], NULL)) return 2;
    totk_tl_frame(100032);
    CHECK(totk_tl_on(), "window closed before feedback drain ended");
    for (int i = 0; i < 4; i++) if (pthread_create(&workers[i], NULL, write_rows, NULL)) return 2;
    while (atomic_load(&ready) != 4) sched_yield();
    atomic_store(&release_writers, 1);
    totk_tl_frame(100033); /* Serialize while other threads are logging. */
    for (int i = 0; i < 4; i++) if (pthread_join(workers[i], NULL)) return 2;
    CHECK(!totk_tl_on(), "window did not close");
    CHECK(!totk_tl_trigger(200000, 3), "one-shot capture restarted after close");
    FILE *f = fopen(argv[1], "r"); if (!f) return 2;
    char line[512], kind[32]; unsigned long long t, frame, a, b; unsigned tid;
    int owner = 0, parallel = 0, metadata = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') { metadata += !strcmp(line, "# first=100000 count=3 dropped=0\n"); continue; }
        if (sscanf(line, "%llu %31s %u %llu %llu %llu", &t, kind, &tid, &frame, &a, &b) != 6) return 2;
        if (!strcmp(kind, "pass_begin")) {
            CHECK(t == 400 && frame == 100001 && a == 19 && b == 29 && tid, "partially published concurrent row");
            parallel++;
        } else {
            CHECK(frame == 100000 && a == 7 && tid, "delayed event lost its submission owner");
            CHECK((!strcmp(kind, "commit") && t == 100 && b == 1) ||
                  (!strcmp(kind, "fence") && t == 101 && b == 77) ||
                  (!strcmp(kind, "gpu") && t == 200 && b == 300), "unexpected event in trigger window");
            owner++;
        }
    }
    fclose(f);
    CHECK(metadata == 1 && owner == 3 && parallel >= 4, "missing range, submission or concurrent events");
    printf("{\"status\":\"pass\",\"probe\":\"timeline-trigger\",\"checked\":%d,\"concurrent_rows\":%d}\n", checked, parallel);
    return 0;
}
