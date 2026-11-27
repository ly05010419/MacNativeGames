"""Exercise the small iOS map's capacity and host headroom in compiled C."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which('clang'), 'clang is required')
class IOSBotwMemory(unittest.TestCase):
    def test_retail_heap_fits_without_using_the_hosts_last_gib(self):
        code = r'''
#include "ios_botw_memory.h"
#include <assert.h>
int main(void) {
 const uint64_t gib = 1ULL << 30, window = 0x7000000000ULL;
 const uint64_t pool = 0xcd500000, actual_heap = 0xc3000000;
 uint64_t cap = ios_botw_heap_capacity(pool);
 assert(cap == 3286ULL * 1024 * 1024);
 assert(cap >= pool && cap >= actual_heap && cap % 0x200000 == 0);
 uint64_t alias = ios_botw190_alias_default_gib(window + 6*gib) * gib;
 assert(alias == gib);
 uint64_t reservation = 0x30000000ULL + cap + alias;
 assert(reservation < 6*gib && 6*gib - reservation >= gib);
 /* Unknown and larger maps retain the existing alias default. */
 assert(ios_botw190_alias_default_gib(0) == 16);
 assert(ios_botw190_alias_default_gib(window) == 16);
 assert(ios_botw190_alias_default_gib(window + 8*gib) == 16);
 assert(ios_botw190_alias_default_gib(1ULL << 39) == 16);
 assert(ios_botw190_alias_default_gib(window + 5*gib) == 1);
 /* Pool overrides are still covered, rounded upwards to a guest heap granule. */
 assert(ios_botw_heap_capacity(0x177d00001ULL) == 0x177e00000ULL);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / 'ios-botw-memory'
            built = subprocess.run(
                ['clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=undefined', '-fno-sanitize-recover=all',
                 '-I' + str(ROOT / 'games/totk/runtime'), '-x', 'c', '-', '-o', str(executable)],
                input=code, text=True, capture_output=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
