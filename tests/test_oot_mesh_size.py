"""Exercise source-specific options, cache invalidation, and the real runtime option selector.
No game shader or metallib is a test fixture.
"""
import contextlib
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'
spec = importlib.util.spec_from_file_location('oot_size_libraries', ROOT/'games/oot/scripts/build_shader_libs.py')
libs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(libs)


class OfflineOptions(unittest.TestCase):
    def test_only_exact_digest_uses_size(self):
        self.assertEqual(libs.optimization_flags(libs.SIZE_SOURCE), ['-Os'])
        self.assertEqual(libs.optimization_flags(libs.SIZE_SOURCE[:-1]), [])
        self.assertEqual(libs.optimization_flags('0'*64), [])

    def test_changed_policy_rebuilds_one_library_and_then_reuses_it(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); cache = root/'seed'; cache.mkdir(); out = root/'libs'
            source = cache/'fixture_b.metal'; source.write_text('kernel void main0() {}')
            unrelated = cache/'other_b.metal'; unrelated.write_text('kernel void other() {}')
            sha = libs.digest(source)
            calls = []

            def compile_call(argv, **kwargs):
                calls.append(argv)
                Path(argv[argv.index('-o')+1]).write_bytes((' '.join(map(str, argv[:argv.index('-o')]))).encode())
                return subprocess.CompletedProcess(argv, 0, '', '')

            def run(policy):
                with mock.patch.object(libs, 'SIZE_SOURCE', policy), \
                     mock.patch.object(sys, 'argv', ['build_shader_libs.py', '--cache', str(cache), '--out', str(out)]), \
                     mock.patch.object(libs, 'metal_compiler', return_value=('metal-fixture', 'fixture', {})), \
                     mock.patch.object(libs, 'GameSlot', return_value=contextlib.nullcontext()), \
                     mock.patch.object(libs.subprocess, 'run', side_effect=compile_call):
                    with self.assertRaises(SystemExit) as done:
                        libs.main()
                    self.assertEqual(done.exception.code, 0)

            run('0'*64); self.assertEqual(len(calls), 2)
            calls.clear(); run(sha)
            self.assertEqual(len(calls), 1)
            self.assertIn('-Os', calls[0]); self.assertEqual(calls[0][-1], source)
            self.assertIn('-fmetal-math-mode=safe', calls[0]); self.assertIn('-fpreserve-invariance', calls[0])
            record = json.loads((out/'libraries-manifest.json').read_text())
            self.assertEqual(record['optimization_flags'], {sha+'.metallib': ['-Os']})
            calls.clear(); run(sha); self.assertEqual(calls, [])
            # Switching the policy back must not adopt the size binary as a default one.
            run('0'*64); self.assertEqual(len(calls), 1); self.assertNotIn('-Os', calls[0])


@unittest.skipUnless(sys.platform == 'darwin', 'Metal compile options require macOS')
class RuntimeOptions(unittest.TestCase):
    def test_production_selector_preserves_math_and_shared_options(self):
        text = (NVN/'nvn_mtl.m').read_text()
        precise = text[text.index('static MTLCompileOptions *totk_precise_opts(void) {'):]
        precise = precise[:precise.index('\n}\n')+3]
        selector = text[text.index('static MTLCompileOptions *totk_source_opts(NSString *source) {'):]
        selector = selector[:selector.index('\n}\n')+3]
        fixture = 'synthetic mesh source'
        sha = hashlib.sha256(fixture.encode()).hexdigest()
        header = (NVN/'oot_mesh_size.h').read_text().replace(libs.SIZE_SOURCE, sha)
        for oot, ios in [(1, 0), (0, 0), (1, 1)]:
            with self.subTest(oot=oot, ios=ios), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp); (root/'oot_mesh_size.h').write_text(header)
                source = root/'probe.m'; binary = root/'probe'
                source.write_text('''#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <stdio.h>
#define MLOG(...) ((void)0)
''' + ('#define GAME_OOT 1\n' if oot else '') + f'#define TARGET_OS_IPHONE {ios}\n' +
                    '#include "oot_mesh_size.h"\n' + precise + selector + f'''
int main(void) {{ @autoreleasepool {{
    MTLCompileOptions *base = totk_precise_opts();
    MTLCompileOptions *chosen = totk_source_opts(@"{fixture}");
    assert(base.optimizationLevel == MTLLibraryOptimizationLevelDefault);
    assert(chosen.optimizationLevel == {('MTLLibraryOptimizationLevelSize' if oot and not ios else 'MTLLibraryOptimizationLevelDefault')});
    assert(chosen.preserveInvariance && chosen.mathMode == MTLMathModeSafe);
    assert(totk_source_opts(@"another source") == base);
    assert(base.optimizationLevel == MTLLibraryOptimizationLevelDefault);
}} return 0; }}
''')
                built = subprocess.run(['xcrun', 'clang', '-fobjc-arc', str(source), '-framework', 'Foundation',
                                        '-framework', 'Metal', '-o', str(binary)], capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stderr)
                self.assertEqual(subprocess.run([str(binary)]).returncode, 0)


if __name__ == '__main__':
    unittest.main()
