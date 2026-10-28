// CPU-only probe of runtime/freeze_watchdog.c: fake frame counters advance, stop for 4 s, then advance again.
// Expect (MK8D_FREEZE_WATCHDOG=2): one "no frame" line, one thread dump, one GPU-state dump, a sample file, a recovery line.
// build: xcrun clang -std=c11 -O1 -Wall -Wextra -Werror tests/freeze_watchdog/probe.c runtime/freeze_watchdog.c -o <out>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
static _Atomic uint64_t frames;
uint64_t dm_nvn_frame_count(void) { return atomic_load(&frames); }
uint64_t mtl_present_count(void) { return atomic_load(&frames); }
void runtime_dump_threads(void) { fprintf(stderr, "probe: runtime_dump_threads called\n"); }
void mtl_wait_state_dump(void) { fprintf(stderr, "probe: mtl_wait_state_dump called\n"); }
int main(void) {
    for (int i = 0; i < 60; i++) { atomic_fetch_add(&frames, 1); usleep(16667); }   // 1 s of frames
    sleep(4);                                                                       // freeze
    for (int i = 0; i < 120; i++) { atomic_fetch_add(&frames, 1); usleep(16667); }  // 2 s of frames
    sleep(4);   // let the sampler finish
    return 0;
}
