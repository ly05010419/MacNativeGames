"""XCX's depth prepass and EQUAL colour pass need invariant vertex positions."""
import os
from pathlib import Path
import sys
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'third_party/d2r_gfx/gfx/nvn/nvn_mtl.m'

HARNESS = r'''
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include <stdlib.h>
#define MLOG(...) ((void)0)
%s
int main(int argc, char **argv) {
 if(argc != 2) return 2;
 @autoreleasepool {
  const int want = atoi(argv[1]);
  struct { _Atomic int failures; _Atomic uintptr_t published; } state = {0};
  typeof(state) *shared = &state;
  /* Cold, concurrent first calls must all see the fully configured object. */
  dispatch_apply(1024, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) {
   (void)i;
   MTLCompileOptions *o = totk_precise_opts();
   uintptr_t address = (uintptr_t)(__bridge void *)o, zero = 0;
   atomic_compare_exchange_strong(&shared->published, &zero, address);
   if(atomic_load(&shared->published) != address || o.preserveInvariance != want ||
      o.mathMode != MTLMathModeSafe) atomic_fetch_add(&shared->failures, 1);
  });
  if(atomic_load(&shared->failures)) return 9;
 }
 return 0;
}
'''


@unittest.skipUnless(sys.platform == 'darwin', 'requires the macOS Metal SDK')
class ShaderCompileOptionsTest(unittest.TestCase):
    def test_invariant_default_and_explicit_diagnostic_controls(self):
        source = SOURCE.read_text()
        start = source.index('static MTLCompileOptions *totk_precise_opts(void) {')
        end = source.index('/* Compile translated MSL', start)
        with tempfile.TemporaryDirectory() as tmp:
            probe, binary = Path(tmp) / 'probe.m', Path(tmp) / 'probe'
            probe.write_text(HARNESS % source[start:end])
            subprocess.run(['xcrun', 'clang', '-std=gnu11', '-fobjc-arc',
                            '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', str(probe),
                            '-framework', 'Foundation', '-framework', 'Metal',
                            '-o', str(binary)], check=True)
            for override, expected in [(None, 1), ('0', 0), ('1', 1)]:
                with self.subTest(override=override):
                    env = os.environ.copy()
                    env.pop('DM_PRESERVE_INVARIANCE', None)
                    env.pop('TOTK_FAST_MATH', None)
                    if override is not None:
                        env['DM_PRESERVE_INVARIANCE'] = override
                    subprocess.run([str(binary), str(expected)], env=env, check=True)
