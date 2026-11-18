// MSL unit tests for the 4 compute kernels embedded in main: expected values are computed directly in C from the Maxwell disassembly semantics (header comment of nvn_builtin.h), without going through the MSL under test.
// Negative control: 4×4 cross matrix - every kernel is run against every expectation, and only the diagonal matches exactly (kernels are distinguishable, and a wrong hash→kernel mapping would be caught).
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
#include "../nvn/nvn_builtin.h"
#include "../nvn/host_msl.h"

enum { NREC = 40, N = 37, CAP = 64 };   // n = 37: spans two thread groups (32 + 5); elements 37..63 must keep the sentinel value

static void expect(int k, const uint64_t *s, uint64_t *e) {
    for (int i = 0; i < CAP; i++) e[i] = 0xAAAAAAAAAAAAAAAAull;
    for (int i = 0; i < N; i++)
        switch (k) {
            case 0: e[i] = s[2 * i]; break;                       // src + (id<<4)
            case 1: e[i] = s[2 * i + 1]; break;                   // src + (id<<4) + 8
            case 2: e[i] = s[4 * i + 2] - s[4 * i]; break;        // src + (id<<5) + 16 − src + (id<<5)
            case 3: e[i] = s[4 * i + 3] - s[4 * i + 1]; break;    // src + (id<<5) + 24 − src + (id<<5) + 8
        }
}

int main(void) {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    NSError *err = nil;
    id<MTLLibrary> lib = host_msl_lib(dev, nil, "builtin", &err);   // the library compiled when the host was built (dm/gfx/msl/builtin.metal)
    if (!lib) { printf("no builtin library: %s\n", err.localizedDescription.UTF8String); return 1; }
    id<MTLCommandQueue> q = [dev newCommandQueue];
    // Source data: NREC 32-byte records (= 2×NREC 16-byte records), with values that have high bits set and wraparound cases where "the later is smaller than the earlier"
    uint64_t src[4 * NREC];
    for (int i = 0; i < 4 * NREC; i++) src[i] = 0x0123456789ABCDEFull * (uint64_t)(i + 1) ^ ((uint64_t)i << 40);
    src[2] = 5; src[0] = 9;                                       // count difference of entry 0 = 5 − 9 → 64-bit wraparound
    uint32_t n = N;
    id<MTLBuffer> bn = [dev newBufferWithBytes:&n length:4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bs = [dev newBufferWithBytes:src length:sizeof src options:MTLResourceStorageModeShared];
    id<MTLBuffer> bd = [dev newBufferWithLength:CAP * 8 options:MTLResourceStorageModeShared];
    int fail = 0;
    for (int k = 0; k < 4; k++) {
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@(k_nvn_builtin_map[k].fn)] error:&err];
        int row[4];
        for (int j = 0; j < 4; j++) {
            memset(bd.contents, 0xAA, CAP * 8);
            id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:ps]; [ce setBuffer:bn offset:0 atIndex:0]; [ce setBuffer:bs offset:0 atIndex:1]; [ce setBuffer:bd offset:0 atIndex:2];
            [ce dispatchThreadgroups:MTLSizeMake((N + 31) / 32, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
            [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
            uint64_t e[CAP]; expect(j, src, e);
            row[j] = !memcmp(bd.contents, e, sizeof e);
        }
        int ok = row[k] && !row[(k + 1) % 4] && !row[(k + 2) % 4] && !row[(k + 3) % 4];
        printf("%s (%s): against its own expectation %s; matches against the other 3 expectations %d (expected 0) → %s\n", k_nvn_builtin_map[k].fn, k_nvn_builtin_map[k].ctl,
               row[k] ? "identical (wrap-around included, 27 sentinels untouched)" : "different", row[(k + 1) % 4] + row[(k + 2) % 4] + row[(k + 3) % 4], ok ? "✓" : "✗");
        fail += !ok;
    }
    printf("== 4 checks, %d failed ==\n", fail);
    return fail != 0;
}
