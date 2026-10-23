"""Persistent library manifests are optional, validated content-addressed lookups."""
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INCLUDE = ROOT/'games/totk/third_party/d2r_gfx/gfx/nvn'
CHAIN = re.search(r'#define DMSH_CHAIN "([^"]+)"', (INCLUDE/'dmsh_chain.h').read_text())[1]
DIRECTORY = 'a'*64
SOURCE = 'b'*64
OTHER = 'c'*64


@unittest.skipUnless(shutil.which('xcrun'), 'native Foundation library loader is Mac-only')
class ShaderLibraryLoader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.tmp.name)
        source = cls.root/'probe.m'
        source.write_text('''#import "totk_shader_libraries.h"
int main(int argc, const char **argv) { @autoreleasepool {
 if (argc != 3) return 2;
 // The scope loader is independent: lookup does not require shader-keys.json.
 (void)totk_shader_scope_metadata; (void)totk_shader_scope_contains;
 NSDictionary *paths = totk_shader_libraries_read(@(argv[1]), @(argv[2]));
 NSDictionary *result = @{@"loaded": @(paths != nil), @"paths": paths ?: @{}};
 NSData *data = [NSJSONSerialization dataWithJSONObject:result options:0 error:nil];
 puts([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String); return 0;
} }
''')
        cls.binary = cls.root/'probe'
        subprocess.run(['xcrun', 'clang', '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                        '-Wno-deprecated-declarations', '-I'+str(INCLUDE), str(source),
                        '-framework', 'Foundation', '-o', str(cls.binary)], check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def manifest(self):
        # Match prepare_shader_libraries.compiler_settings: sys.platform is darwin.
        return dict(schema=1, chain=CHAIN,
                    compiler_identity=dict(executable='/toolchain/metal', version='Apple metal 1',
                                           platform='darwin', machine='arm64', platform_release='26.0.0'),
                    options=dict(math='safe', flags=['-fmetal-math-mode=safe'], target='compiler-default'),
                    directory=DIRECTORY, libraries={SOURCE: f'{DIRECTORY}/{SOURCE}.metallib'})

    def probe(self, root, chain=CHAIN):
        result = subprocess.run([str(self.binary), str(root), chain], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def load(self, manifest):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root/'manifest.json').write_text(json.dumps(manifest))
            result = self.probe(root)
            if result['loaded']:
                result['paths'] = {key: str(Path(path).relative_to(root)) for key, path in result['paths'].items()}
            return result

    def assert_rejected(self, manifest):
        self.assertEqual(self.load(manifest), dict(loaded=False, paths={}))

    def test_correct_manifest_resolves_exact_source_digests_without_shader_scope(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            manifest = self.manifest()
            manifest['libraries'][OTHER] = f'{DIRECTORY}/{OTHER}.metallib'
            (root/'manifest.json').write_text(json.dumps(manifest))
            self.assertFalse((root/'shader-keys.json').exists())
            self.assertEqual(self.probe(root), dict(loaded=True, paths={
                SOURCE: str(root/DIRECTORY/(SOURCE+'.metallib')),
                OTHER: str(root/DIRECTORY/(OTHER+'.metallib'))}))

    def test_wrong_chain_schema_math_flags_target_and_platform_are_rejected(self):
        for field, value in [('chain', 'other-chain'), ('schema', 2)]:
            with self.subTest(field=field):
                manifest = self.manifest(); manifest[field] = value
                self.assert_rejected(manifest)
        for field, value in [('math', 'fast'), ('flags', []), ('flags', ['-ffast-math']),
                             ('flags', ['-fmetal-math-mode=safe', '-ffast-math']),
                             ('target', 'applegpu_g13g')]:
            with self.subTest(field=field, value=value):
                manifest = self.manifest(); manifest['options'][field] = value
                self.assert_rejected(manifest)
        for value in ['linux', 'win32', 'Darwin', 'iOS']:
            with self.subTest(platform=value):
                manifest = self.manifest(); manifest['compiler_identity']['platform'] = value
                self.assert_rejected(manifest)
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); (root/'manifest.json').write_text(json.dumps(self.manifest()))
            self.assertEqual(self.probe(root, chain='other-chain'), dict(loaded=False, paths={}))

    def test_invalid_digest_directory_and_relative_path_are_rejected(self):
        for value in ['', '../escape', 'a'*63, 'a'*65, DIRECTORY.upper(), DIRECTORY+'\n', None, [], 42]:
            with self.subTest(directory=value):
                manifest = self.manifest(); manifest['directory'] = value
                self.assert_rejected(manifest)
        for value in ['', '../escape', 'b'*63, 'b'*65, SOURCE.upper(), SOURCE+'\n']:
            with self.subTest(digest=value):
                manifest = self.manifest()
                manifest['libraries'] = {value: f'{DIRECTORY}/{value}.metallib'}
                self.assert_rejected(manifest)
        for value in [f'../{SOURCE}.metallib', f'/{DIRECTORY}/{SOURCE}.metallib',
                      f'{DIRECTORY}/../{SOURCE}.metallib', f'{OTHER}/{SOURCE}.metallib',
                      f'{DIRECTORY}/{OTHER}.metallib', f'{DIRECTORY}/{SOURCE}.metal',
                      f'{DIRECTORY}/{SOURCE}.metallib\n', None, [], {}, 42]:
            with self.subTest(path=value):
                manifest = self.manifest(); manifest['libraries'][SOURCE] = value
                self.assert_rejected(manifest)

    def test_malformed_top_level_and_nested_types_are_rejected(self):
        for value in [None, [], ['manifest'], 'manifest', 42]:
            with self.subTest(root=value):
                self.assert_rejected(value)
        for field in ['schema', 'chain', 'options', 'compiler_identity', 'directory', 'libraries']:
            for value in [None, [], 'wrong type', 42]:
                with self.subTest(field=field, value=value):
                    manifest = self.manifest(); manifest[field] = value
                    self.assert_rejected(manifest)
        for field in ['math', 'flags', 'target']:
            for value in [None, [], {}, 42]:
                with self.subTest(option=field, value=value):
                    manifest = self.manifest(); manifest['options'][field] = value
                    self.assert_rejected(manifest)
        for value in [None, [], {}, 42]:
            with self.subTest(platform=value):
                manifest = self.manifest(); manifest['compiler_identity']['platform'] = value
                self.assert_rejected(manifest)

    def test_missing_identity_fields_are_rejected(self):
        for field in ['schema', 'chain', 'options', 'compiler_identity', 'directory', 'libraries']:
            with self.subTest(field=field):
                manifest = self.manifest(); del manifest[field]
                self.assert_rejected(manifest)
        for field in ['math', 'flags', 'target']:
            with self.subTest(option=field):
                manifest = self.manifest(); del manifest['options'][field]
                self.assert_rejected(manifest)
        manifest = self.manifest(); del manifest['compiler_identity']['platform']
        self.assert_rejected(manifest)

    def test_missing_empty_and_broken_manifest_fall_back(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.assertEqual(self.probe(root), dict(loaded=False, paths={}))
            for data in ['', '{', 'not JSON']:
                with self.subTest(data=data):
                    (root/'manifest.json').write_text(data)
                    self.assertEqual(self.probe(root), dict(loaded=False, paths={}))
            (root/'manifest.json').unlink()
            (root/'manifest.json').mkdir()
            self.assertEqual(self.probe(root), dict(loaded=False, paths={}))
        self.assertEqual(self.probe(''), dict(loaded=False, paths={}))

    def test_empty_library_map_is_valid_and_missing_binary_is_left_to_metal_fallback(self):
        # Manifest validation never compiles or opens a GPU library. An absent or
        # damaged binary is handled by the caller's existing Metal/source fallback.
        manifest = self.manifest(); manifest['libraries'] = {}
        self.assertEqual(self.load(manifest), dict(loaded=True, paths={}))
        self.assertEqual(self.load(self.manifest()), dict(loaded=True,
            paths={SOURCE: f'{DIRECTORY}/{SOURCE}.metallib'}))


if __name__ == '__main__':
    unittest.main()
