/* #2070: an attempt that fails because the Metal compiler service went away (it restarts on the next request) is tried
   again, as totk_msl_library already did for libraries; any other failure is final at once. No GPU. */
#import <Foundation/Foundation.h>
#include <stdio.h>
#include "../third_party/d2r_gfx/gfx/nvn/nvn_compiler_retry.h"

static NSError *xpc(void) {
    return [NSError errorWithDomain:@"MTLLibraryErrorDomain" code:1 userInfo:@{NSLocalizedDescriptionKey:
        @"Compilation failed due to an interrupted connection: XPC_ERROR_CONNECTION_INTERRUPTED. This error occurred after multiple retries."}];
}
static int fail(const char *what) { fprintf(stderr, "FAIL: %s\n", what); return 1; }

int main(void) { @autoreleasepool {
    __block int calls = 0; NSError *e = nil;
    BOOL ok = compiler_retry(&e, 0, ^BOOL(NSError **err) { if (++calls < 3) { *err = xpc(); return NO; } return YES; });
    if (!ok || calls != 3) return fail("an interrupted attempt is tried again until it works");
    calls = 0; e = nil;
    ok = compiler_retry(&e, 0, ^BOOL(NSError **err) { calls++; *err = [NSError errorWithDomain:@"x" code:2 userInfo:@{NSLocalizedDescriptionKey: @"bad descriptor"}]; return NO; });
    if (ok || calls != 1 || ![e.localizedDescription isEqualToString:@"bad descriptor"]) return fail("another error is final at once and kept");
    calls = 0; e = nil;
    ok = compiler_retry(&e, 0, ^BOOL(NSError **err) { calls++; *err = xpc(); return NO; });
    if (ok || calls != COMPILER_RETRY_ATTEMPTS || ![e.localizedDescription containsString:@"XPC_ERROR_CONNECTION_INTERRUPTED"])
        return fail("an interruption that lasts stops after the attempts and keeps the error");
    if (!compiler_interrupted(xpc()) || compiler_interrupted(nil)) return fail("the interruption is told by its description");
    puts("compiler retry: all checks pass");
    return 0;
}}
