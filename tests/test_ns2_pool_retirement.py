"""Exercise production pool retirement with real Metal buffers and GPU readback."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn'


@unittest.skipUnless(platform.system() == 'Darwin', 'Metal residency sets')
class PoolRetirement(unittest.TestCase):
    def test_pool_residency_lasts_until_submission_completion(self):
        text = (NVN/'nvn_mtl.m').read_text()
        def between(start, end):
            begin = text.index(start)
            return text[begin:text.index(end, begin)]
        helpers = between('static pthread_mutex_t g_res_mu', '/* Top registration sites')
        deferred = between('@interface NvDeferredRelease', 'static pthread_mutex_t g_bf_want_mu')
        remove = between('void mtl_pool_del(const void *mem)', 'static pidx_ent *pool_query')
        probe = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <sys/mman.h>
#include "nvn_pool_index.h"
static id<MTLResidencySet> g_res;
static int g_res_dirty;
static uint64_t g_fence, g_done;
static id g_cb;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pidx g_pools;
static pidx_query_cache g_pool_queries;
static _Atomic uint64_t g_reg_gen, g_pool_gen;
static uint64_t g_pool_bytes, g_pool_res_bytes;
static unsigned g_pool_res_n, dropped;
static NSMutableArray *g_keep;
static void stor_drop_range(uintptr_t lo, uintptr_t hi) { if (hi > lo) dropped++; }
static void wb_drop_range(uintptr_t lo, uintptr_t hi) { if (hi > lo) dropped++; }
#define BOTW_LATE_DROP(lo, hi)
#pragma clang attribute push(__attribute__((objc_externally_retained)), apply_to = function)
HELPERS
DEFERRED
REMOVE
#pragma clang attribute pop
#define CHECK(c, message) do { if (!(c)) { fprintf(stderr, "%s\n", message); return 1; } } while (0)
int main(void) { @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (!dev || ![dev respondsToSelector:@selector(newResidencySetWithDescriptor:error:)]) return 77;
    NSError *error = nil;
    MTLResidencySetDescriptor *desc = [MTLResidencySetDescriptor new]; desc.initialCapacity = 64;
    g_res = [dev newResidencySetWithDescriptor:desc error:&error];
    if (!g_res && !error) return 77;
    CHECK(g_res, "residency set unavailable");
    id<MTLCommandQueue> queue = [dev newCommandQueue]; [queue addResidencySet:g_res];
    g_keep = [NSMutableArray new];
    const size_t size = 16384;
    uint32_t *mem = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    CHECK(mem != MAP_FAILED, "backing allocation failed");
    id<MTLBuffer> output = [dev newBufferWithLength:size options:MTLResourceStorageModeShared];
    CHECK(output, "readback allocation failed");
    // Unused pools must retire without a future submission, even inside a long-lived caller pool.
    __weak id<MTLBuffer> unused;
    @autoreleasepool {
        id<MTLBuffer> buffer = [dev newBufferWithBytesNoCopy:mem length:size options:MTLResourceStorageModeShared deallocator:nil];
        CHECK(buffer, "unused pool allocation failed"); unused = buffer; [g_keep addObject:buffer];
        pidx_ent ent = {(uintptr_t)mem, (uintptr_t)mem+size, (uintptr_t)mem,
            (__bridge void *)buffer, buffer.gpuAddress, mem, 1};
        CHECK(pidx_insert(&g_pools, &ent), "unused pool registration failed");
        res_add_at(buffer, 1); [g_res commit];
        g_pool_bytes += size; g_pool_res_bytes += size; g_pool_res_n++;
    }
    @autoreleasepool { mtl_pool_del(mem); defer_drain(); [g_res commit]; }
    CHECK(!unused && !g_res_live_bytes, "unused pool waits for nonexistent submission");
    for (unsigned cycle = 0; cycle < 64; cycle++) {
        for (unsigned i = 0; i < size/4; i++) mem[i] = i*65537u ^ cycle*1234567u;
        __weak id<MTLBuffer> retired;
        @autoreleasepool {
            // Isolate registration temporaries; their separate lifetime bug is covered by PR #1841.
            id<MTLBuffer> buffer = [dev newBufferWithBytesNoCopy:mem length:size
                options:MTLResourceStorageModeShared deallocator:nil];
            CHECK(buffer, "zero-copy buffer allocation failed");
            retired = buffer; [g_keep addObject:buffer];
            pidx_ent ent = {(uintptr_t)mem, (uintptr_t)mem+size, (uintptr_t)mem,
                (__bridge void *)buffer, buffer.gpuAddress, mem, 1};
            CHECK(pidx_insert(&g_pools, &ent), "pool registration failed");
            res_add_at(buffer, 1); [g_res commit];
            g_pool_bytes += size; g_pool_res_bytes += size; g_pool_res_n++;
        }
        @autoreleasepool {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            g_cb = cb;
            id<MTLBlitCommandEncoder> encoder = [cb blitCommandEncoder];
            [encoder copyFromBuffer:retired sourceOffset:0 toBuffer:output destinationOffset:0 size:size];
            [encoder endEncoding]; encoder = nil;
            if (cycle % 3 == 1) { g_fence++; [cb commit]; g_cb = nil; }
#ifdef GAME_NS2
            if (cycle % 3 != 2) { @autoreleasepool { mtl_pool_del(mem); } }
            if (cycle % 3 != 2) {
            CHECK(!g_pools.n && !g_keep.count && !g_pool_bytes && !g_pool_res_bytes && !g_pool_res_n,
                "finalized pool still addressable");
            defer_drain(); [g_res commit];
            CHECK(retired && [g_res_objects containsObject:retired], "pool removed before pending submission completed");
            }
#endif
            if (cycle % 3 != 1) { g_fence++; [cb commit]; }
            [cb waitUntilCompleted];
            CHECK(cb.status == MTLCommandBufferStatusCompleted, "GPU submission failed");
            g_done = g_fence;
            g_cb = nil;
#ifdef GAME_NS2
            if (cycle % 3 == 2) { @autoreleasepool { mtl_pool_del(mem); } }
#endif
#ifndef GAME_NS2
            @autoreleasepool { mtl_pool_del(mem); }
#endif
            defer_drain(); [g_res commit];
            CHECK(![g_res_objects containsObject:retired], "completed pool remains resident");
            for (unsigned i = 0; i < size/4; i++)
                CHECK(((uint32_t *)output.contents)[i] == (i*65537u ^ cycle*1234567u), "GPU readback mismatch");
        }
        CHECK(!retired && !g_res_live_bytes, "completed pool wrapper retained");
        // The no-copy backing belongs to the caller: reuse only after the real GPU completion above.
        memset(mem, 0xa5, size);
    }
    CHECK(dropped == 130 && !g_pools.n && !g_keep.count && !g_pool_res_n, "pool teardown bookkeeping");
    CHECK(munmap(mem, size) == 0, "backing deallocation failed");
    printf("64 pool retirements, 262144 GPU words, backing reused only after completion\n");
    return 0;
} }
'''.replace('HELPERS', helpers).replace('DEFERRED', deferred).replace('REMOVE', remove)
        with tempfile.TemporaryDirectory(prefix='mng-pool-retirement-') as folder:
            path = Path(folder)/'probe.m'
            path.write_text(probe)
            for mode, flags in [('ns2', ['-DGAME_NS2=1']), ('switch1', [])]:
                with self.subTest(mode=mode):
                    exe = Path(folder)/mode
                    build = subprocess.run(['xcrun', 'clang', '-std=gnu11', '-O2', '-fobjc-arc',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-I'+str(NVN), *flags,
                        str(path), '-framework', 'Foundation', '-framework', 'Metal', '-o', str(exe)],
                        capture_output=True, text=True, timeout=60)
                    self.assertEqual(build.returncode, 0, build.stdout+build.stderr)
                    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
                    if run.returncode == 77:
                        self.skipTest('Metal device with residency sets unavailable')
                    self.assertEqual(run.returncode, 0, run.stdout+run.stderr)
                    self.assertIn('262144 GPU words', run.stdout)


if __name__ == '__main__':
    unittest.main()
