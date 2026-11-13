/* The Metal compiler service (MTLCompilerService) can go away in the middle of a compile and comes back on the next
   request: under memory pressure (iPad, 2026-10-18) or when one shader crashes it (#2070: Apple M5, a float add on device
   memory). Every compile running at that moment fails with "XPC_ERROR_CONNECTION_INTERRUPTED". totk_msl_library already
   tried a library again (nvn_mtl.m); an archive add (totk_pipeline_archive.h par_write) did not, so on a cold build of
   Ocarina of Time on an M5 the one crash failed four other threads' adds (write_failed 4) and packaging stopped. An attempt
   that fails so is tried again, up to COMPILER_RETRY_ATTEMPTS times, waiting 100, 200, 300 ms between them; any other
   failure is final at once. tests/compiler_retry_test.m. */
#pragma once
#import <Foundation/Foundation.h>
#include <unistd.h>

enum { COMPILER_RETRY_ATTEMPTS = 4 };

static inline BOOL compiler_interrupted(NSError *e) {
    return e && [e.localizedDescription containsString:@"XPC_ERROR_CONNECTION_INTERRUPTED"];
}

/* attempt: YES when it worked, else NO with *err set. Returns its last answer, *err its last error. wait_ms 0 (tests) does
   not sleep; the runtime passes 100. */
static inline BOOL compiler_retry(NSError **err, unsigned wait_ms, BOOL (^attempt)(NSError **)) {
    for (int n = 1;; n++) {
        NSError *e = nil;
        BOOL ok = attempt(&e);
        if (err) *err = e;
        if (ok || !compiler_interrupted(e) || n == COMPILER_RETRY_ATTEMPTS) return ok;
        if (wait_ms) usleep((useconds_t)(n * wait_ms) * 1000u);
    }
}
