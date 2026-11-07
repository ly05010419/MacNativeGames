"""Retired Metal resources must not wait for a guest thread's outer pool to drain."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(platform.system() == "Darwin", "Metal resource lifetime probe")
class ResidencyRegistration(unittest.TestCase):
    def test_registration_does_not_extend_retired_resource_lifetime(self):
        source = (ROOT / "games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m").read_text()
        begin = source.index("static pthread_mutex_t g_res_mu")
        helpers = source[begin:source.index("/* Top registration sites", begin)]
        probe = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
static id<MTLResidencySet> g_res;
static int g_res_dirty;
#pragma clang attribute push(__attribute__((objc_externally_retained)), apply_to = function)
HELPERS
#pragma clang attribute pop
int main(void) { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device || ![device respondsToSelector:@selector(newResidencySetWithDescriptor:error:)])
        return 77;
    MTLResidencySetDescriptor *desc = [MTLResidencySetDescriptor new];
    desc.initialCapacity = 64;
    NSError *error = nil;
    g_res = [device newResidencySetWithDescriptor:desc error:&error];
    if (!g_res && !error) return 77; // Hosted virtual Metal devices expose the selector but no residency sets.
    if (!g_res) { fprintf(stderr, "residency set: %s\n", error.description.UTF8String); return 2; }
    @autoreleasepool {
        NSMutableArray *keep = [NSMutableArray new];
        for (int i = 0; i < 64; i++) {
            id<MTLBuffer> buffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
            if (!buffer) { fprintf(stderr, "buffer allocation failed\n"); return 2; }
            [keep addObject:buffer];
            res_add_at(buffer, 1);
        }
        [g_res commit];
        if (atomic_load(&g_res_site[1].alive) != 64 || g_res_site[1].adds != 64)
            return 3;
        @autoreleasepool {
            for (id<MTLBuffer> buffer in keep) res_remove(buffer);
            [keep removeAllObjects];
            [g_res commit];
        }
        unsigned long long alive = atomic_load(&g_res_site[1].alive);
        printf("retired buffers alive before outer pool drain: %llu\n", alive);
        if (alive || g_res_live_bytes || g_res_site[1].removes != 64) return 1;
    }
    return 0;
} }
'''.replace("HELPERS", helpers)
        with tempfile.TemporaryDirectory(prefix="mng-residency-registration-") as folder:
            path = Path(folder) / "probe.m"
            binary = Path(folder) / "probe"
            path.write_text(probe)
            build = subprocess.run([
                "xcrun", "--sdk", "macosx", "clang", "-std=gnu11", "-O2", "-fobjc-arc",
                "-Wall", "-Wextra", "-Werror", "-framework", "Foundation", "-framework", "Metal",
                str(path), "-o", str(binary),
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            if run.returncode == 77:
                self.skipTest("Metal device with residency sets unavailable")
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("retired buffers alive before outer pool drain: 0", run.stdout)


if __name__ == "__main__":
    unittest.main()
