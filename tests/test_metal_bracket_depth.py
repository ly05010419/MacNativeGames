"""Deep generated NS2 MSL compiles offline without invalidating ordinary shader libraries.

Synthetic nested walk adapted from stan69b/nxnative, oot-clean 56b73f21 (seedds, MIT).
"""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import build_app

ROOT = Path(__file__).resolve().parents[1]


def nested_kernel(depth):
    body = ''.join(f'uint t{i} = min(left, a[{i}]); a[{i}] -= t{i}; left -= t{i};\nif (left != 0) {{\n'
                   for i in range(depth))
    return ('#include <metal_stdlib>\nusing namespace metal;\n'
            'kernel void k(device uint* a [[buffer(0)]], uint id [[thread_position_in_grid]]) {\n'
            'uint left = id;\n' + body + '}\n' * (depth + 1))


class MetalBracketDepth(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='mng-metal-depth-')
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.calls = {}

    def compiler(self, args, **kwargs):
        source = Path(args[-1])
        self.calls[source.name] = args
        Path(args[args.index('-o') + 1]).write_bytes(b'synthetic library')
        return subprocess.CompletedProcess(args, 0, stdout='', stderr='')

    def sources(self, folder):
        folder.mkdir(parents=True)
        for name, depth in [('deep_b.metal', 300), ('flat_b.metal', 3)]:
            (folder/name).write_text(nested_kernel(depth))

    def check_flags(self):
        self.assertIn('-fbracket-depth=1024', self.calls['deep_b.metal'])
        self.assertNotIn('-fbracket-depth=1024', self.calls['flat_b.metal'])

    def test_depth_boundary_and_bracket_kinds(self):
        source = self.root/'source.metal'
        for text, expected in [('', 0), ('f(a[b{c}]) { (x); }', 3), ('{' * 256 + '}' * 256, 256),
                               ('{' * 257 + '}' * 257, 257)]:
            with self.subTest(depth=expected):
                source.write_text(text)
                self.assertEqual(build_app.bracket_depth(source), expected)
                self.assertEqual(build_app.metal_flags(source), ['-fmetal-math-mode=safe'] +
                                 (['-fbracket-depth=1024'] if expected > 256 else []))

    def test_app_compiler_selects_only_deep_ns2_sources_and_reuses_cache(self):
        for version in ('pkp', 'dkb', 'v143', 'botw190'):
            with self.subTest(version=version):
                tree = self.root/version
                seed = tree/f'build/shader-seed/{version}'
                self.sources(seed)
                libs = tree/'build/shader-libs'
                libs.mkdir()
                (libs/'compiler.txt').write_text('fake 1 -fmetal-math-mode=safe')
                (libs/'existing.metallib').write_bytes(b'keep')
                self.calls.clear()
                with patch.object(build_app, 'metal_compiler', return_value=('metal', 'fake 1', None)), \
                        patch.object(build_app, 'variant_sources', return_value=[]), patch.object(build_app, 'say'), \
                        patch.object(build_app.subprocess, 'run', side_effect=self.compiler):
                    build_app.step_shader_libs(tree, tree/'logs', version=version)
                    if version in ('pkp', 'dkb'):
                        self.check_flags()
                    else:
                        for args in self.calls.values():
                            self.assertNotIn('-fbracket-depth=1024', args)
                    self.assertEqual(len(self.calls), 2)
                    self.calls.clear()
                    build_app.step_shader_libs(tree, tree/'logs', version=version)
                    self.assertEqual(self.calls, {})
                self.assertEqual((libs/'compiler.txt').read_text(), 'fake 1 -fmetal-math-mode=safe')
                self.assertEqual((libs/'existing.metallib').read_bytes(), b'keep')
                for source in seed.glob('*.metal'):
                    digest = hashlib.sha256(source.read_bytes()).hexdigest()
                    self.assertTrue((libs/f'{digest}.metallib').is_file())
                self.assertEqual((tree/'logs/shader-libs.log').read_text(), '')

    def test_work_folder_compiler_selects_deep_sources_and_reuses_cache(self):
        spec = importlib.util.spec_from_file_location('build_shader_libs', ROOT/'games/totk/scripts/build_shader_libs.py')
        tool = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(tool)
        cache, out = self.root/'cache', self.root/'out'
        self.sources(cache)
        with patch.object(tool, 'metal_compiler', return_value=('metal', 'fake 1', None)), \
                patch.object(tool.subprocess, 'run', side_effect=self.compiler), patch('builtins.print'), \
                patch.object(sys, 'argv', ['build_shader_libs.py', '--cache', str(cache), '--out', str(out)]):
            for run in range(2):
                with self.assertRaises(SystemExit) as done:
                    tool.main()
                self.assertEqual(done.exception.code, 0)
                if run == 0:
                    self.check_flags()
                else:
                    self.assertEqual(self.calls, {})
                self.calls.clear()
        self.assertEqual((out/'compiler.txt').read_text(), 'fake 1 -fmetal-math-mode=safe')
        self.assertEqual((out/'failed.log').read_text(), '')

    def test_real_metal_compiler_accepts_deep_walk_only_with_raised_limit(self):
        found = build_app.metal_compiler()
        if not found:
            self.skipTest('requires the macOS Metal toolchain')
        metal, _, env = found
        source = self.root/'deep.metal'
        source.write_text(nested_kernel(300))
        output = self.root/'deep.metallib'
        base = subprocess.run([metal, *build_app.METAL_FLAGS, '-o', output, source], env=env,
                              capture_output=True, text=True, timeout=60)
        self.assertNotEqual(base.returncode, 0)
        self.assertIn('bracket nesting level exceeded', base.stderr)
        fixed = subprocess.run([metal, *build_app.metal_flags(source), '-o', output, source], env=env,
                               capture_output=True, text=True, timeout=60)
        self.assertEqual(fixed.returncode, 0, fixed.stdout + fixed.stderr)
        self.assertGreater(output.stat().st_size, 0)


class IosMetalBracketDepth(unittest.TestCase):
    def test_ios_tools_raise_the_limit_only_for_deep_sources(self):
        spec = importlib.util.spec_from_file_location('ios_compile_seed', ROOT / 'games/totk/ios/tools/compile_seed.py')
        seed = importlib.util.module_from_spec(spec); spec.loader.exec_module(seed)
        with tempfile.TemporaryDirectory(prefix='mng-metal-depth-') as tmp:
            shallow, deep = Path(tmp) / 'shallow.metal', Path(tmp) / 'deep.metal'
            shallow.write_text(nested_kernel(10)); deep.write_text(nested_kernel(300))
            self.assertEqual(seed.metal_flags(shallow), seed.METAL_FLAGS)
            self.assertEqual(seed.metal_flags(deep), [*seed.METAL_FLAGS, '-fbracket-depth=1024'])


if __name__ == '__main__':
    unittest.main()
