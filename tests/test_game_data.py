"""The game data folder the runtime reads at start (games/totk/runtime/game_data.h) and update_runtime.py, which puts a
new runtime into an app without its game files. Synthetic files only; no game files or keys."""
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import build_app        # noqa: E402
import update_runtime   # noqa: E402

SHA = 'ab' * 32


def tables(version, digests):
    """A runtime-tables.bin header and module records as games/totk/scripts/prepare_runtime.py writes them."""
    records = b''.join(struct.pack('<16s72s13Q4I', name.encode(), digest.encode(), *([0] * 13), 0, 0, 0, 0)
                       for name, digest in digests.items())
    size = 64 + len(records) + 8
    return struct.pack('<8s8sIIQQQQQ', b'TOTKTAB1', version.encode(), len(digests), 1, 0, 0, size - 8, 8, size) + records + bytes(8)


class GameDataReader(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('cc') or shutil.which('clang')
        if not compiler:
            raise unittest.SkipTest('a C compiler is required for the game data reader')
        cls.dir = Path(tempfile.mkdtemp())
        cls.probe = cls.dir / 'game-data-probe'
        build = subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Werror', '-DTOTK_GAME_V143=1',
                                f'-I{ROOT / "games/totk/runtime"}', str(ROOT / 'tests/game_data_probe.c'),
                                str(ROOT / 'games/totk/runtime/game_data.c'), '-o', str(cls.probe)], capture_output=True, text=True)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.dir)

    def read(self, text, key, kind='text', folder=None):
        data = self.dir / 'data'
        shutil.rmtree(data, ignore_errors=True)
        data.mkdir()
        if text is not None:
            (data / 'romfs-config.txt').write_text(text)
        return subprocess.run([str(self.probe), 'romfs-config.txt', key, kind], capture_output=True, text=True,
                              env={'TOTK_GAME_DATA': str(folder or data)})

    def test_values(self):
        text = f'# made by prepare_romfs_backend.py\nROMFS_IMAGE_SIZE 17052625232\nROMFS_IMAGE_SHA256 {SHA}\n\nNAME a b\n'
        self.assertEqual(self.read(text, 'ROMFS_IMAGE_SIZE', 'number').stdout, '17052625232\n')
        self.assertEqual(self.read(text, 'ROMFS_IMAGE_SHA256', 'sha256').stdout, SHA + '\n')
        self.assertEqual(self.read(text, 'NAME').stdout, 'a b\n')
        self.assertEqual(self.read('ROMFS_IMAGE_SIZE 0x10', 'ROMFS_IMAGE_SIZE', 'number').stdout, '16\n')   # no last newline

    def test_refused(self):
        for text, key, kind, why in (
                (None, 'ROMFS_IMAGE_SIZE', 'number', 'game data missing'),
                ('OTHER 1\n', 'ROMFS_IMAGE_SIZE', 'number', 'game data value missing'),
                ('ROMFS_IMAGE_SIZE 12x\n', 'ROMFS_IMAGE_SIZE', 'number', 'game data number'),
                ('ROMFS_IMAGE_SIZE -1\n', 'ROMFS_IMAGE_SIZE', 'number', 'game data number'),
                (f'ROMFS_IMAGE_SHA256 {SHA.upper()}\n', 'ROMFS_IMAGE_SHA256', 'sha256', 'game data SHA-256'),
                (f'ROMFS_IMAGE_SHA256 {SHA}0\n', 'ROMFS_IMAGE_SHA256', 'sha256', 'game data SHA-256'),
                ('ROMFS_IMAGE_SIZE\n', 'ROMFS_IMAGE_SIZE', 'number', 'not KEY VALUE')):
            with self.subTest(text=text):
                r = self.read(text, key, kind)
                self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
                self.assertIn(why, r.stderr)

    def test_default_folder_is_the_versions_build_folder(self):
        data = self.dir / 'cwd/build/game-data/v143'
        data.mkdir(parents=True, exist_ok=True)
        (data / 'romfs-config.txt').write_text('ROMFS_IMAGE_SIZE 7\n')
        r = subprocess.run([str(self.probe), 'romfs-config.txt', 'ROMFS_IMAGE_SIZE', 'number'], capture_output=True, text=True,
                           cwd=self.dir / 'cwd', env={})
        self.assertEqual(r.stdout, '7\n', r.stderr)


class UpdateRuntime(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.root)

    def app(self, version='v143', executable='TOTK'):
        app = self.root / 'Game.app'
        (app / 'Contents').mkdir(parents=True)
        import plistlib
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable': executable}))
        res = app / 'Contents/Resources/totk'
        digests = {}
        for name in ('main', 'subsdk0', 'sdk'):
            image = res / f'rewritten/{version}/{name}.patched.bin'
            image.parent.mkdir(parents=True, exist_ok=True)
            image.write_bytes(name.encode() * 100)
            digests[name] = hashlib.sha256(image.read_bytes()).hexdigest()
        index = res / f'extracted/{version}/romfs/runtime-index.bin'
        index.parent.mkdir(parents=True)
        index.write_bytes(b'TOTKRFS1 index')
        fonts = res / 'extracted/firmware/shared-fonts'
        fonts.mkdir(parents=True)
        lines = []
        for i in range(6):
            (fonts / f'{i}-font.bfttf').write_bytes(bytes([i]) * 16)
            lines.append(f'FONT{i}_FILE {i}-font.bfttf\nFONT{i}_SHA256 {hashlib.sha256(bytes([i]) * 16).hexdigest()}\n')
        data = self.root / 'game-data'
        data.mkdir(exist_ok=True)
        (data / 'runtime-tables.bin').write_bytes(tables(version, digests))
        (data / 'romfs-config.txt').write_text(f'ROMFS_IMAGE_SIZE 6\nROMFS_IMAGE_SHA256 {SHA}\n'
                                               f'ROMFS_INDEX_SHA256 {hashlib.sha256(index.read_bytes()).hexdigest()}\n')
        (data / 'shared-fonts.txt').write_text(''.join(lines))
        return app, res, data

    def test_identity(self):
        app, _, _ = self.app()
        self.assertEqual(update_runtime.app_identity(app), ('totk', 'v143'))

    def test_identity_refuses_other_apps(self):
        app, res, _ = self.app()
        (res / 'rewritten/v112').mkdir()
        with self.assertRaisesRegex(build_app.BuildError, 'not an app build_app.py made'):
            update_runtime.app_identity(app)
        ios = self.root / 'TOTK.app'
        ios.mkdir()
        (ios / 'Info.plist').write_bytes(b'')
        with self.assertRaisesRegex(build_app.BuildError, 'iPhone / iPad'):
            update_runtime.app_identity(ios)

    def test_game_data_of_the_apps_files(self):
        _, res, data = self.app()
        update_runtime.check_game_data(data, res, 'v143')

    def test_game_data_of_other_files_is_refused(self):
        _, res, data = self.app()
        (res / 'rewritten/v143/sdk.patched.bin').write_bytes(b'a modded sdk')
        with self.assertRaisesRegex(build_app.BuildError, r'other code modules .*\(sdk\)'):
            update_runtime.check_game_data(data, res, 'v143')
        (data / 'runtime-tables.bin').write_bytes(tables('v121', {}))
        with self.assertRaisesRegex(build_app.BuildError, 'version v121, the app is v143'):
            update_runtime.check_game_data(data, res, 'v143')

    def test_game_data_of_another_index_or_fonts_is_refused(self):
        _, res, data = self.app()
        (res / 'extracted/v143/romfs/runtime-index.bin').write_bytes(b'another index')
        with self.assertRaisesRegex(build_app.BuildError, 'another RomFS index'):
            update_runtime.check_game_data(data, res, 'v143')
        shutil.rmtree(self.root / 'Game.app')
        _, res, data = self.app()
        (res / 'extracted/firmware/shared-fonts/3-font.bfttf').write_bytes(b'x')
        with self.assertRaisesRegex(build_app.BuildError, r'other shared fonts .*3-font'):
            update_runtime.check_game_data(data, res, 'v143')

    def test_refusals_ask_for_a_full_rebuild(self):
        """The refusals that no flag of update_runtime.py can fix say that a full rebuild is required (issue #1182)."""
        _, res, data = self.app()
        (res / 'rewritten/v143/sdk.patched.bin').write_bytes(b'a modded sdk')
        with self.assertRaisesRegex(build_app.BuildError, r'\(sdk\); a full rebuild with build_app.py is required'):
            update_runtime.check_game_data(data, res, 'v143')
        shutil.rmtree(self.root / 'Game.app')
        _, res, data = self.app()
        (res / 'extracted/v143/romfs/runtime-index.bin').write_bytes(b'another index')
        with self.assertRaisesRegex(build_app.BuildError, 'another RomFS index.*a full rebuild with build_app.py is required'):
            update_runtime.check_game_data(data, res, 'v143')
        shutil.rmtree(self.root / 'Game.app')
        _, res, data = self.app()
        (res / 'extracted/firmware/shared-fonts/3-font.bfttf').write_bytes(b'x')
        with self.assertRaisesRegex(build_app.BuildError, r'other shared fonts .*3-font.*a full rebuild'):
            update_runtime.check_game_data(data, res, 'v143')
        want = update_runtime.rewriter_sha256()
        with self.assertRaisesRegex(build_app.BuildError, 'no record of the rewriter.*a full rebuild with build_app.py is required'):
            update_runtime.check_rewriter(data, res, 'v143', want)
        (data / 'rewriter.txt').write_text(f'REWRITER_SHA256 {"0" * 64}\n')
        with self.assertRaisesRegex(build_app.BuildError, 'another patch_nso.py.*A full rebuild with build_app.py is required'):
            update_runtime.check_rewriter(data, res, 'v143', want)
        shutil.rmtree(self.root / 'Game.app')
        _, res, _ = self.app()
        (res / 'build/shader-seed').mkdir(parents=True)
        (res / 'build/shader-seed/seed-manifest.json').write_text('{"chain": "totk-native-v1-unverified"}')
        with self.assertRaisesRegex(build_app.BuildError, 'or do a full rebuild with build_app.py'):
            update_runtime.check_seed_chain(res, update_runtime.runtime_chain())

    def test_seed_of_another_chain_is_refused(self):
        _, res, _ = self.app()
        chain = update_runtime.runtime_chain()
        self.assertRegex(chain, r'^totk-native-v\d+-')
        with self.assertRaisesRegex(build_app.BuildError, 'no readable startup shader seed'):
            update_runtime.check_seed_chain(res, chain)
        (res / 'build/shader-seed').mkdir(parents=True)
        (res / 'build/shader-seed/seed-manifest.json').write_text('{"chain": "totk-native-v1-unverified"}')
        with self.assertRaisesRegex(build_app.BuildError, 'chain totk-native-v1-unverified.*needs a new seed'):
            update_runtime.check_seed_chain(res, chain)
        (res / 'build/shader-seed/seed-manifest.json').write_text(json.dumps({'chain': chain}))
        update_runtime.check_seed_chain(res, chain)

    def test_seed_of_another_chain_with_seed_option(self):
        _, res, _ = self.app()
        chain = update_runtime.runtime_chain()
        self.assertTrue(update_runtime.check_seed_chain(res, chain, remake=True), 'no seed: one is made')
        (res / 'build/shader-seed').mkdir(parents=True)
        (res / 'build/shader-seed/seed-manifest.json').write_text('{"chain": "totk-native-v1-unverified"}')
        with self.assertRaisesRegex(build_app.BuildError, 'Pass --seed'):
            update_runtime.check_seed_chain(res, chain)
        self.assertTrue(update_runtime.check_seed_chain(res, chain, remake=True))
        (res / 'build/shader-seed/seed-manifest.json').write_text(json.dumps({'chain': chain}))
        self.assertFalse(update_runtime.check_seed_chain(res, chain, remake=True))

    def work_tree(self, res):
        """A work folder holding the app's modules and an extracted RomFS that its game data names."""
        tree = self.root / 'work/totk'
        for image in (res / 'rewritten/v143').iterdir():
            (tree / 'rewritten/v143').mkdir(parents=True, exist_ok=True)
            (tree / 'rewritten/v143' / image.name).write_bytes(image.read_bytes())
        romfs = tree / 'extracted/v143/romfs'
        romfs.mkdir(parents=True)
        (romfs / 'index.json').write_text(json.dumps({'status': 'pass', 'romfs_sha256': SHA}))
        (romfs / 'romfs.bin').write_bytes(b'romfs!')
        return tree

    def test_work_files_of_the_app(self):
        _, res, data = self.app()
        update_runtime.check_work_files(self.work_tree(res), data, 'v143')

    def test_work_files_of_another_build_are_refused(self):
        _, res, data = self.app()
        tree = self.work_tree(res)
        (tree / 'rewritten/v143/main.patched.bin').write_bytes(b'another main')
        with self.assertRaisesRegex(build_app.BuildError, "not the app's main module"):
            update_runtime.check_work_files(tree, data, 'v143')
        shutil.rmtree(self.root / 'work')
        tree = self.work_tree(res)
        (tree / 'extracted/v143/romfs/romfs.bin').write_bytes(b'longer romfs')
        with self.assertRaisesRegex(build_app.BuildError, 'not the RomFS of the app'):
            update_runtime.check_work_files(tree, data, 'v143')
        (tree / 'extracted/v143/romfs/index.json').unlink()
        with self.assertRaisesRegex(build_app.BuildError, 'not the RomFS of the app'):
            update_runtime.check_work_files(tree, data, 'v143')

    def test_replace_seed(self):
        app, res, _ = self.app()
        for rel in ('build/shader-seed/old.metal', 'build/shader-libs/old.metallib', 'build/game-data/v143/shader-seed.aliases.tsv'):
            (res / rel).parent.mkdir(parents=True, exist_ok=True)
            (res / rel).write_text('old')
        tree = self.root / 'tree'
        seed = tree / 'build/shader-seed/v143'
        seed.mkdir(parents=True)
        (seed / 'new_b.metal').write_text('new')
        (seed / 'seed-manifest.json').write_text(json.dumps(dict(
            version='v143', chain=update_runtime.runtime_chain(), shaders=8000,
            files={'new_b.metal': hashlib.sha256(b'new').hexdigest()})))
        (tree / 'build/shader-libs').mkdir(parents=True)
        (tree / f'build/shader-libs/{hashlib.sha256(b"new").hexdigest()}.metallib').write_text('lib')
        removed = []
        with patch.object(build_app, 'remove_in_background', side_effect=removed.append):
            update_runtime.replace_seed(tree, app, 'v143', 'totk')
        self.assertEqual(sorted(p.name for p in (res / 'build/shader-seed').iterdir()), ['new_b.metal', 'seed-manifest.json'])
        self.assertEqual([p.name for p in (res / 'build/shader-libs').iterdir()], [f'{hashlib.sha256(b"new").hexdigest()}.metallib'])
        self.assertEqual([p.name for p in removed], ['Game.app.shader-seed.old', 'Game.app.shader-libs.old'])
        self.assertTrue(all(p.parent == app.parent for p in removed), 'moved out of the app, not deleted inside it')
        self.assertFalse((res / 'build/game-data/v143/shader-seed.aliases.tsv').exists(), 'the old seed\'s aliases go with it')

    def test_rewriter_of_the_repository(self):
        _, res, data = self.app()   # modules that are not the pinned rewrites (as a --mod build's): judged by rewriter.txt
        want = update_runtime.rewriter_sha256()
        with self.assertRaisesRegex(build_app.BuildError, 'no record of the rewriter'):
            update_runtime.check_rewriter(data, res, 'v143', want)
        (data / 'rewriter.txt').write_text(f'# made by prepare_runtime.py\nREWRITER_SHA256 {"0" * 64}\n')
        with self.assertRaisesRegex(build_app.BuildError, 'not the rewrites this repository pins for 1.4.3.*another '
                                                          'patch_nso.py .*the app was not changed'):
            update_runtime.check_rewriter(data, res, 'v143', want)
        (data / 'rewriter.txt').write_text(f'REWRITER_SHA256 {want}\n')
        update_runtime.check_rewriter(data, res, 'v143', want)
        self.assertEqual(want, hashlib.sha256((ROOT / 'games/totk/third_party/d2r/patch_nso.py').read_bytes()).hexdigest())

    def test_pinned_rewrites_pass_with_an_older_rewriter_record(self):
        """build_app.py keeps modules that match expected-<version>.json, so rewriter.txt can name an older patch_nso.py
        whose output for this version is the same (the maintainer's 1.4.3 build, 2026-10-25)."""
        _, res, data = self.app()
        (data / 'rewriter.txt').write_text(f'REWRITER_SHA256 {"3d" * 32}\n')   # an older rewriter
        pins = {f'{n}.patched.bin': hashlib.sha256((res / f'rewritten/v143/{n}.patched.bin').read_bytes()).hexdigest()
                for n in ('main', 'subsdk0', 'sdk')}
        with patch.dict(build_app.VERSIONS, {'v143': dict(build_app.VERSIONS['v143'], rewritten_sha256=pins)}):
            update_runtime.check_rewriter(data, res, 'v143', update_runtime.rewriter_sha256())
            (res / 'rewritten/v143/sdk.patched.bin').write_bytes(b'one module not the pinned rewrite')
            with self.assertRaisesRegex(build_app.BuildError, 'another patch_nso.py'):
                update_runtime.check_rewriter(data, res, 'v143', update_runtime.rewriter_sha256())

    def test_probe_before_install(self):
        app, res, _ = self.app()
        logs = self.root / 'logs'
        binary = res / 'build/totk-native'
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_text('#!/bin/sh\ntest "$1" = rewritten/v143 && test "$2" = probe && test -z "$TOTK_GAME_DATA" && '
                          'echo \'{"status":"pass"}\'\n')
        binary.chmod(0o755)
        with patch.dict('os.environ', {'TOTK_GAME_DATA': '/elsewhere'}):
            update_runtime.probe_runtime(app, 'v143', logs)   # the app's own game data, not one the environment names
        binary.write_text('#!/bin/sh\necho "[totk] FAIL: game tables of another game version" >&2\nexit 1\n')
        with self.assertRaisesRegex(build_app.BuildError, 'does not load .*another game version.*the app was not changed'):
            update_runtime.probe_runtime(app, 'v143', logs)
        self.assertIn('another game version', (logs / 'runtime-probe.log').read_text())

    def test_record_commit(self):
        app, _, _ = self.app()
        import plistlib
        with patch.object(build_app, 'source_commit', return_value='abc1234'), \
             patch.object(build_app, 'source_revision', return_value='a' * 40):
            update_runtime.record_commit(app)
        info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
        self.assertEqual((info['MNGSourceCommit'], info['CFBundleExecutable']), ('abc1234', 'TOTK'))
        self.assertEqual(info['MNGSourceRevision'], 'a' * 40)
        with patch.object(build_app, 'source_commit', return_value=''), \
             patch.object(build_app, 'source_revision', return_value=''):   # not a git checkout: no stale commit, no stale update base
            update_runtime.record_commit(app)
        info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
        self.assertNotIn('MNGSourceCommit', info)
        self.assertNotIn('MNGSourceRevision', info)

    def test_restore_replaced_app(self):
        app = self.root / 'Game.app'
        replaced = self.root / 'Game.app.replaced-1'
        for folder, label in ((app, 'new'), (replaced, 'old')):
            (folder / 'Contents/MacOS').mkdir(parents=True)
            (folder / 'Contents/MacOS/TOTK').write_text(label)
        (app / 'Contents/Helpers/GameData-v143.bundle').mkdir(parents=True)   # moved in from the old app (keep_game_data)
        (app / 'Contents/Helpers/GameData-v143.bundle/romfs.bin').write_text('16 GB')
        failed = update_runtime.restore_replaced(app, replaced)
        self.assertEqual((app / 'Contents/MacOS/TOTK').read_text(), 'old')
        self.assertEqual((app / 'Contents/Helpers/GameData-v143.bundle/romfs.bin').read_text(), '16 GB')
        self.assertEqual((failed / 'Contents/MacOS/TOTK').read_text(), 'new')
        self.assertFalse(replaced.exists())
        self.assertEqual(failed.parent, app.parent)

    def test_seed_aliases(self):
        _, res, data = self.app()
        tree = self.root / 'tree'
        seed = tree / 'build/shader-seed/v143'
        seed.mkdir(parents=True)
        (seed / 'seed-manifest.json').write_text('{"seed": 1}')
        (tree / 'build/shader-seed/v143.aliases.tsv').write_text('a\tb\n')
        (res / 'build/shader-seed').mkdir(parents=True)
        (res / 'build/shader-seed/seed-manifest.json').write_text('{"seed": 2}')
        new_data = self.root / 'new-data'
        self.assertFalse(update_runtime.seed_aliases(tree, res, 'v143', new_data), 'another seed: the lists are kept')
        (res / 'build/shader-seed/seed-manifest.json').write_text('{"seed": 1}')
        self.assertTrue(update_runtime.seed_aliases(tree, res, 'v143', new_data))
        self.assertEqual((new_data / 'shader-seed.aliases.tsv').read_text(), 'a\tb\n')

    def test_place_runtime_replaces_only_the_runtime(self):
        app, res, _ = self.app()
        tree = self.root / 'tree'
        for rel in ('build/totk-native-v143', 'build/spirv-cross/spirv-cross', 'prebuilt/macos-arm64/totk_xlate',
                    'build/pso_manifest.tsv', 'build/pso_title.tsv', 'build/app-launcher'):
            (tree / rel).parent.mkdir(parents=True, exist_ok=True)
            (tree / rel).write_bytes(b'new ' + rel.encode())
        before = {p: p.read_bytes() for p in res.rglob('*') if p.is_file()}
        with patch.object(build_app, 'run') as run:
            build_app.place_runtime(tree, app, 'v143', self.root / 'logs', lists=False)
        run.assert_not_called()   # no aliases: nothing renamed
        self.assertEqual((res / 'build/totk-native').read_bytes(), b'new build/totk-native-v143')
        self.assertEqual((app / 'Contents/MacOS/TOTK').read_bytes(), b'new build/app-launcher')
        self.assertFalse((res / 'build/pso_title.tsv').exists(), 'lists=False keeps the app\'s lists')
        for path, content in before.items():
            self.assertEqual(path.read_bytes(), content, f'{path} is a game file')


if __name__ == '__main__':
    unittest.main()
