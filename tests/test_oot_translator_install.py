"""Ocarina of Time's App carries the shader translator its seed was translated with (2026-11-16, user: "极速更新 有bug？" /
"修 1234").

Only the full build installed this repository's translator into the work tree's build/shader-service, which package_app
copies into the App; every update made the seed again with the repository's translator (prepare_ns2_seed.py uses its own
copy). So the express updates from 11-15 shipped the seed of translator 9845718d with the in-game translator 3b26a94d of
11-11. These tests run games/oot/build.py's own functions: install_translator (every packaging path), check_translator
(package_app refuses an App whose translator is not its seed's), and the order in update_runtime / update_shaders.
"""
import importlib.util
import json
import os
import plistlib
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'


def load_build():
    spec = importlib.util.spec_from_file_location('oot_build_translator', GAME / 'build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class TranslatorInstall(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.base = Path(self.temp.name)
        self.build = load_build()
        self.game = self.base / 'game'   # a stand-in checkout: prebuilt files with their checksums, the real scripts
        prebuilt = self.game / 'prebuilt/macos-arm64'
        prebuilt.mkdir(parents=True)
        (self.game / 'scripts').symlink_to(GAME / 'scripts')
        self.translator = self.put(prebuilt, 'totk_xlate', b'translator of today\n')
        self.put(prebuilt, 'oot_write_proof.dylib', b'proof analyzer\n')
        (prebuilt / 'SPIRV-Tools.LICENSE').write_text('license\n')
        self.build.GAME = self.game
        self.work = self.base / 'work'
        self.tree = self.work / 'oot'
        self.service = self.tree / 'build/shader-service'

    def tearDown(self):
        self.temp.cleanup()

    def put(self, folder, name, data):
        (folder / name).write_bytes(data)
        (folder / (name + '.sha256')).write_text(self.build.sha(folder / name) + '  ' + name + '\n')
        return self.build.sha(folder / name)

    def seed(self, translator, mode='full'):
        seed = self.build.shader_dirs(self.tree, mode)[0]
        seed.mkdir(parents=True, exist_ok=True)
        record = {'chain': 'test'}
        if translator: record['translator_sha256'] = translator
        (seed / 'seed-manifest.json').write_text(json.dumps(record))
        return seed

    def stale(self):
        self.service.mkdir(parents=True, exist_ok=True)
        (self.service / 'totk_xlate').write_bytes(b'translator of 11-11\n')

    def test_install_puts_the_repository_translator_in_the_work_tree(self):
        self.build.install_translator(self.tree)
        self.assertEqual(self.build.sha(self.service / 'totk_xlate'), self.translator)
        self.assertTrue((self.service / 'oot_write_proof.dylib').is_file())
        self.assertTrue((self.service / 'SPIRV-Tools.LICENSE').is_file())

    def test_install_replaces_an_older_translator_with_a_new_file(self):
        self.stale()
        before = (self.service / 'totk_xlate').stat().st_ino
        self.build.install_translator(self.tree)
        self.assertEqual(self.build.sha(self.service / 'totk_xlate'), self.translator)
        self.assertNotEqual((self.service / 'totk_xlate').stat().st_ino, before)   # not written over a program macOS ran
        self.assertFalse((self.service / 'totk_xlate.new').exists())

    def test_install_keeps_an_unchanged_translator(self):
        self.build.install_translator(self.tree)
        inode = (self.service / 'totk_xlate').stat().st_ino
        self.build.install_translator(self.tree)
        self.assertEqual((self.service / 'totk_xlate').stat().st_ino, inode)

    def test_install_refuses_a_translator_that_is_not_its_checksum(self):
        (self.game / 'prebuilt/macos-arm64/totk_xlate').write_bytes(b'damaged\n')
        with self.assertRaisesRegex(ValueError, 'does not match'):
            self.build.install_translator(self.tree)

    def test_check_passes_the_seeds_own_translator(self):
        self.seed(self.translator)
        self.build.install_translator(self.tree)
        self.build.check_translator(self.tree, 'full')

    def test_check_refuses_the_old_translator_beside_a_new_seed(self):   # the 2026-11-15 express updates
        self.seed(self.translator)
        self.stale()
        with self.assertRaisesRegex(ValueError, 'not packaged'):
            self.build.check_translator(self.tree, 'full')

    def test_check_refuses_a_seed_that_does_not_name_its_translator(self):
        self.seed(None)
        self.build.install_translator(self.tree)
        with self.assertRaisesRegex(ValueError, 'not recorded'):
            self.build.check_translator(self.tree, 'full')

    def test_check_reads_a_quick_apps_own_seed(self):
        self.seed('0' * 64)   # the full build's seed, of another translator
        self.seed(self.translator, 'title')
        self.build.install_translator(self.tree)
        self.build.check_translator(self.tree, 'title')
        with self.assertRaises(ValueError):
            self.build.check_translator(self.tree, 'full')

    def test_package_app_checks_before_it_stages_anything(self):
        source = (GAME / 'build.py').read_text()
        body = source[source.index('def package_app('):]
        self.assertLess(body.index('check_translator(tree,mode)'), body.index("temporary=out.with_name("))

    def app(self):
        app = self.base / 'Ocarina of Time.app'
        (app / 'Contents').mkdir(parents=True)
        (app / 'Contents/Info.plist').write_bytes(plistlib.dumps({'MNGShaders': 'full'}))
        return app

    def ordered(self, calls):
        def steps(*_args, **_kwargs):
            calls.append(('shader_steps', self.build.sha(self.service / 'totk_xlate')))
        def package(*_args, **_kwargs):
            calls.append(('package_app', self.build.sha(self.service / 'totk_xlate')))
        return steps, package

    def test_express_update_installs_the_translator_before_the_seed(self):
        self.stale()
        calls = []
        steps, package = self.ordered(calls)
        with mock.patch.object(self.build, 'update_blocker', return_value=''), \
             mock.patch.object(self.build, 'app_running', return_value=False), \
             mock.patch.object(self.build, 'sync_tree'), \
             mock.patch.object(self.build.subprocess, 'run'), \
             mock.patch.object(self.build, 'shader_steps', side_effect=steps), \
             mock.patch.object(self.build, 'package_app', side_effect=package):
            self.build.update_runtime(self.app(), self.work)
        self.assertEqual(calls, [('shader_steps', self.translator), ('package_app', self.translator)])

    def test_shader_update_installs_the_translator_too(self):
        sys.path.insert(0, str(GAME / 'scripts'))
        from prepare_ns2_seed import translation_context
        self.stale()
        seed = self.seed(self.translator)
        record = json.loads((seed / 'seed-manifest.json').read_text())
        record['translation_context_sha256'] = translation_context(self.game / 'prebuilt/macos-arm64/totk_xlate', None)
        (seed / 'seed-manifest.json').write_text(json.dumps(record))
        (self.tree / 'extracted/oot/romfs').mkdir(parents=True)
        (self.tree / 'extracted/oot/romfs/index.json').write_text('{}')
        (self.work / 'build').mkdir(parents=True)
        (self.work / 'build/oot-native').write_bytes(b'runtime')
        calls = []
        steps, package = self.ordered(calls)
        with mock.patch.object(self.build, 'app_running', return_value=False), \
             mock.patch.object(self.build, 'shader_steps', side_effect=steps), \
             mock.patch.object(self.build, 'package_app', side_effect=package):
            self.build.update_shaders(self.app(), self.base / 'list.tsv', self.work)
        self.assertEqual(calls, [('shader_steps', self.translator), ('package_app', self.translator)])


if __name__ == '__main__':
    unittest.main()
