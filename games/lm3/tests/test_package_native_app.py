"""Standalone packaging preserves the old app on failure and ships no personal state."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
SPEC = importlib.util.spec_from_file_location('lm3_package_native_app', ROOT / 'scripts/package_native_app.py')
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


def sha(data):
    return hashlib.sha256(data).hexdigest()


class NativeAppPackaging(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='lm3-package-')
        self.base = Path(self.temporary.name)
        self.root = self.base / 'source'
        self.seed = self.base / 'prepared-seed'
        self.app = self.base / '输出 含空格' / 'Luigi portable.app'
        self.runtime_identities = []

        def write(relative, data):
            path = self.root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            return path

        rows = []
        for name in PACKAGE.MODULES:
            data = (name + ' patched module').encode()
            write(f'rewritten/v140/{name}.patched.bin', data)
            self.runtime_identities.append(sha(data))
            rows.append(f'{{.name="{name}",.digest="{sha(data)}",.file_size={len(data)}ULL}},')
            write(f'extracted/v140/exefs/{name}', (name + ' original').encode())
        write('build/runtime_tables.h', '\n'.join(rows).encode())
        write('build/game_version_config.h', b'#define TOTK_GAME_DIR "v140"\n#define TOTK_GAME_VERSION "1.4.0"\n')
        image, index = b'local romfs image', b'local romfs index'
        self.romfs_hash = sha(image)
        write('extracted/v140/romfs/romfs.bin', image)
        write('extracted/v140/romfs/runtime-index.bin', index)
        write('extracted/v140/romfs/index.json', json.dumps({
            'status': 'pass', 'romfs_sha256': sha(image), 'romfs_size': len(image),
        }).encode())
        write('build/romfs_config.h', (
            f'#define ROMFS_IMAGE_SHA256 "{sha(image)}"\n'
            f'#define ROMFS_INDEX_SHA256 "{sha(index)}"\n').encode())
        self.runtime_identities += [sha(image), sha(index)]
        fonts = []
        for i in range(6):
            data = f'font {i}'.encode()
            write(f'extracted/firmware/shared-fonts/{i}-font{i}.bfttf', data)
            fonts.append({'type': i, 'name': f'font{i}', 'bfttf_bytes': len(data), 'sha256': sha(data)})
            self.runtime_identities.append(sha(data))
        write('extracted/firmware/shared-fonts/manifest.json', json.dumps({'status': 'pass', 'fonts': fonts}).encode())
        write('third_party/d2r_gfx/gfx/nvn/dmsh_chain.h', b'#define DMSH_CHAIN "fixture-chain"\n')
        self.runtime = write('build/lm3-native', '\n'.join([*self.runtime_identities, 'fixture-chain']).encode())
        write('build/shader-service/totk_xlate', b'translator')
        write('build/spirv-cross/spirv-cross', b'cross compiler')
        write('build/lm3-app-launcher', b'launcher')
        write('build/AppIcon.icns', b'icon')
        # These nearby files must never be swept into the allowlisted package.
        write('prod.keys', b'fixture secret, never bundled')
        write('work/profile/private.save', b'private progress')
        write('build/cheats.txt', b'not part of the approved resource list')
        write('rewritten/v140/developer.log', b'not shipped')
        self.seed.mkdir()
        key = 'a' * 64 + '-' + 'b' * 12
        contents = {key + '_b.metal': b'fragment source', key + '_b.json': b'{}',
                    'pso_manifest.tsv': b'# fixture warmup list\n'}
        for name, data in contents.items():
            (self.seed / name).write_bytes(data)
        self.seed_manifest = {'schema': 1, 'chain': 'fixture-chain', 'shaders': 1,
                              'files': {name: sha(data) for name, data in contents.items()}}
        self.write_seed_manifest()

    def tearDown(self):
        self.temporary.cleanup()

    def write_seed_manifest(self):
        (self.seed / 'seed-manifest.json').write_text(json.dumps(self.seed_manifest))

    def metadata(self, path):
        # Check the maximum requirement is used, including the shader compiler.
        return {'minimum_macos': '27.2' if path.name == 'spirv-cross' else '26.0',
                'architectures': ['arm64'], 'dependencies': ['/usr/lib/libSystem.B.dylib']}

    def package(self, sign=None):
        with mock.patch.object(PACKAGE, 'verify_exefs', return_value={
            'display': '1.4.0', 'romfs_sha256': self.romfs_hash,
        }), mock.patch.object(PACKAGE, 'inspect_executable', side_effect=self.metadata), \
                mock.patch.object(PACKAGE, 'run', side_effect=sign or (lambda command: '')):
            return PACKAGE.package(self.app, 'v140', self.seed, root=self.root)

    def test_allowlist_relocation_and_outer_signature_resource_seal(self):
        signing = []

        def sign(arguments):
            signing.append(arguments)
            target = Path(arguments[-1])
            if '--force' in arguments and target.is_dir():
                launcher = target / 'Contents/MacOS/LM3'
                launcher.write_bytes(launcher.read_bytes() + b' outer resource seal')
            return ''

        report = self.package(sign)
        manifest = PACKAGE.verify_resources(self.app)
        self.assertEqual(report['minimum_macos'], '27.2')
        info = plistlib.loads((self.app / 'Contents/Info.plist').read_bytes())
        self.assertEqual(info['LSMinimumSystemVersion'], '27.2')
        self.assertEqual(info['CFBundleExecutable'], 'LM3')
        self.assertNotIn(str(self.root), json.dumps(manifest))
        names = set(manifest['files'])
        self.assertIn('Contents/Resources/game/extracted/v140/exefs/main', names)
        self.assertIn('Contents/Resources/game/build/pso_manifest.tsv', names)
        self.assertEqual((self.app / PACKAGE.GAME_PREFIX / 'build/shader-map.tsv').read_text(),
                         '# TOTK control-code-sha256-v1\n')
        self.assertNotIn('Contents/MacOS/LM3', names)  # covered by codesign, not a circular resource hash
        self.assertFalse(any('prod.keys' in name or '/profile/' in name or 'developer.log' in name or 'cheats.txt' in name for name in names))
        self.assertEqual(sum('--force' in command for command in signing), 5)  # 4 executables, then the app
        self.assertTrue(any('--deep' in command and '--verify' in command for command in signing))

    def test_resource_tampering_is_detected(self):
        self.package()
        (self.app / PACKAGE.GAME_PREFIX / 'extracted/v140/romfs/romfs.bin').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'resource manifest'):
            PACKAGE.verify_resources(self.app)

    def test_modified_module_is_rejected_before_app_replacement(self):
        self.app.mkdir(parents=True)
        (self.app / 'previous').write_text('keep this app')
        (self.root / 'rewritten/v140/main.patched.bin').write_bytes(b'wrong rewrite')
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            self.package()
        self.assertEqual((self.app / 'previous').read_text(), 'keep this app')
        self.assertFalse(list(self.app.parent.glob('.*.stage-*.app')))

    def test_signature_failure_preserves_existing_app_and_removes_only_staging(self):
        self.app.mkdir(parents=True)
        (self.app / 'previous').write_text('keep this app')

        def failed_sign(command):
            raise RuntimeError('synthetic codesign failure')

        with self.assertRaisesRegex(RuntimeError, 'synthetic codesign'):
            self.package(failed_sign)
        self.assertEqual((self.app / 'previous').read_text(), 'keep this app')
        self.assertFalse(list(self.app.parent.glob('.*.stage-*.app')))

    def test_invalid_seed_identity_hash_or_extra_files_is_rejected(self):
        self.seed_manifest['chain'] = 'old-chain'
        self.write_seed_manifest()
        with self.assertRaisesRegex(ValueError, 'translation chain'):
            PACKAGE.validated_seed(self.seed, self.root)
        self.seed_manifest['chain'] = 'fixture-chain'
        self.write_seed_manifest()
        pso = self.seed / 'pso_manifest.tsv'
        pso.write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            PACKAGE.validated_seed(self.seed, self.root)
        pso.write_bytes(b'# fixture warmup list\n')
        (self.seed / ('a' * 64 + '-' + 'b' * 12 + '_b.metallib')).write_bytes(b'device binary')
        with self.assertRaisesRegex(ValueError, 'unlisted files'):
            PACKAGE.validated_seed(self.seed, self.root)

    def test_seed_symlink_cannot_reach_outside_prepared_directory(self):
        pso = self.seed / 'pso_manifest.tsv'
        pso.unlink()
        pso.symlink_to(self.root / 'prod.keys')
        with self.assertRaisesRegex(ValueError, 'regular file'):
            PACKAGE.validated_seed(self.seed, self.root)

    def test_wrong_selected_game_version_rejected(self):
        (self.root / 'build/game_version_config.h').write_text(
            '#define TOTK_GAME_DIR "v120"\n#define TOTK_GAME_VERSION "1.2.0"\n')
        with self.assertRaisesRegex(ValueError, 'build configuration'):
            self.package()

    def test_output_cannot_contain_its_own_input_checkout(self):
        self.app.mkdir(parents=True)
        destination = self.app / 'source'
        self.root.rename(destination)
        self.root = destination
        with self.assertRaisesRegex(ValueError, 'overlaps a package input'):
            self.package()
        self.assertTrue((destination / 'build/lm3-native').is_file())

    def test_external_dependency_is_rejected(self):
        def output(command):
            if command[1] == 'lipo':
                return 'arm64\n'
            if command[1] == 'vtool':
                return ' platform MACOS\n minos 27.2\n sdk 27.2\n'
            return 'fixture:\n\t/opt/homebrew/lib/private.dylib (compatibility version 1.0.0, current version 1.0.0)\n'

        with mock.patch.object(PACKAGE, 'run', side_effect=output):
            with self.assertRaisesRegex(ValueError, 'External runtime dependency'):
                PACKAGE.inspect_executable(self.runtime)

    def test_install_failure_keeps_previous_directory_unchanged(self):
        self.app.mkdir(parents=True)
        (self.app / 'previous').write_text('old')
        stage = self.app.parent / 'stage.app'
        stage.mkdir()
        (stage / 'new').write_text('new')

        def failed_exchange(first, second):
            raise OSError('unsupported atomic directory exchange')

        with self.assertRaisesRegex(OSError, 'atomic directory exchange'):
            PACKAGE.install_bundle(stage, self.app, failed_exchange)
        self.assertEqual((self.app / 'previous').read_text(), 'old')
        self.assertEqual((stage / 'new').read_text(), 'new')

    @unittest.skipUnless(sys.platform == 'darwin', 'macOS atomic directory exchange')
    def test_real_atomic_swap_retains_previous_app(self):
        self.app.mkdir(parents=True)
        (self.app / 'previous').write_text('old')
        stage = self.app.parent / 'stage.app'
        stage.mkdir()
        (stage / 'new').write_text('new')
        backup = PACKAGE.install_bundle(stage, self.app)
        self.assertEqual((self.app / 'new').read_text(), 'new')
        self.assertEqual((backup / 'previous').read_text(), 'old')
        self.assertFalse(stage.exists())


if __name__ == '__main__':
    unittest.main()
