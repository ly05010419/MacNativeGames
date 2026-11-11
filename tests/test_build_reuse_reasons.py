"""A long build step that is done again says why, and a build takes the verified game data the app it replaces already
carries instead of extracting it from the game file again (build_app.py source_mismatch, romfs_mismatch,
adopt_game_data, adoptable_bundle, data_bundle; user 2026-11-11: a work folder that had only seen express updates
extracted, merged and signed 17 GB again, 18 minutes, with nothing in the log saying it had nothing to reuse)."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import build_app

IMAGE = b'romfs image of the test version' * 1000
VERSION = 'vtest'
EXPECTED = dict(display_version='9.9.9', romfs=dict(size=len(IMAGE), sha256=hashlib.sha256(IMAGE).hexdigest()))


def app_with_data(app, image=IMAGE, info=None):
    bundle = app / f'Contents/Helpers/GameData-{VERSION}.bundle'
    (bundle / 'Contents/_CodeSignature').mkdir(parents=True)
    (bundle / 'Contents/_CodeSignature/CodeResources').write_bytes(b'seal')
    (bundle / 'Contents/Info.plist').write_bytes(info or build_app.data_bundle_info(build_app.BUNDLE_ID, VERSION))
    (bundle / 'Contents/Resources').mkdir()
    (bundle / 'Contents/Resources/romfs.bin').write_bytes(image)
    return bundle


def fake_index(cmd, log, cwd=None, env=None, quiet=True):
    """scripts/index_romfs.py as the build runs it: the report beside the image (no RomFS tables to read here)."""
    cmd = [str(c) for c in cmd]
    if cmd[1] != 'scripts/index_romfs.py':
        raise AssertionError(f'unexpected tool {cmd}')
    image, source = Path(cmd[2]), Path(cmd[cmd.index('--source') + 1])
    st = source.stat()
    (image.parent / 'index.json').write_text(json.dumps(dict(
        status='pass', romfs_size=image.stat().st_size, romfs_sha256=cmd[cmd.index('--sha256') + 1], files=[],
        adopted_from=str(source), adopted_size=st.st_size, adopted_mtime_ns=st.st_mtime_ns)))


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.tree, self.logs, self.app = self.root / 'work/totk', self.root / 'work/logs', self.root / 'Game.app'
        self.tree.mkdir(parents=True)
        self.said = []
        patches = [mock.patch.dict(build_app.VERSIONS, {VERSION: EXPECTED}),
                   mock.patch.object(build_app, 'say', self.said.append)]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        self.tmp.cleanup()


class Reasons(Base):
    def test_source_mismatch_names_what_changed(self):
        game, manifest = self.root / 'game.xci', self.root / 'extraction-manifest.json'
        game.write_bytes(b'game')
        self.assertEqual(build_app.source_mismatch(manifest, game), 'nothing was extracted in this work folder yet')
        st = game.stat()
        manifest.write_text(json.dumps(dict(source='/elsewhere/game.xci', source_size=st.st_size, source_mtime_ns=st.st_mtime_ns)))
        self.assertIn('/elsewhere/game.xci', build_app.source_mismatch(manifest, game))
        manifest.write_text(json.dumps(dict(source=str(game.resolve()), source_size=1, source_mtime_ns=st.st_mtime_ns)))
        self.assertIn('changed size', build_app.source_mismatch(manifest, game))
        manifest.write_text(json.dumps(dict(source=str(game.resolve()), source_size=st.st_size, source_mtime_ns=st.st_mtime_ns)))
        self.assertIsNone(build_app.source_mismatch(manifest, game))
        self.assertTrue(build_app.source_matches(manifest, game))

    def test_romfs_mismatch(self):
        folder = self.root / 'romfs'
        self.assertIn('no extracted image', build_app.romfs_mismatch(folder, EXPECTED))
        folder.mkdir()
        (folder / 'romfs.bin').write_bytes(IMAGE)
        (folder / 'index.json').write_text(json.dumps(dict(status='pass', romfs_sha256='0' * 64)))
        self.assertIn('another version', build_app.romfs_mismatch(folder, EXPECTED))
        (folder / 'index.json').write_text(json.dumps(dict(status='fail', romfs_sha256=EXPECTED['romfs']['sha256'])))
        self.assertIsNone(build_app.romfs_mismatch(folder, EXPECTED))
        self.assertIn('did not finish', build_app.romfs_mismatch(folder, EXPECTED, need_pass=True))

    def test_the_shader_bundle_says_why_it_is_made_again(self):
        with mock.patch.object(build_app, 'check_seed', return_value={}), \
             mock.patch.object(build_app, 'shader_libs_of', return_value=[]), \
             mock.patch.object(build_app, 'seal_shader_bundle') as seal:
            (self.tree / 'build/shader-seed/v143').mkdir(parents=True)
            (self.tree / 'build/shader-bundles').mkdir(parents=True)
            build_app.shader_bundle(self.tree, 'v143', self.logs)
        seal.assert_called_once()
        self.assertIn('there is none in this work folder', self.said[-1])


class AdoptGameData(Base):
    def folder(self):
        return self.tree / f'extracted/{VERSION}/romfs'

    def test_the_verified_image_of_the_app_is_taken(self):
        app_with_data(self.app)
        with mock.patch.object(build_app, 'run', side_effect=fake_index):
            self.assertTrue(build_app.adopt_game_data(self.tree, self.app, VERSION, self.folder(), self.logs))
        self.assertEqual((self.folder() / 'romfs.bin').read_bytes(), IMAGE)
        self.assertIsNone(build_app.romfs_mismatch(self.folder(), EXPECTED, need_pass=True))

    def test_another_image_or_no_app_is_not_taken(self):
        with mock.patch.object(build_app, 'run', side_effect=AssertionError('nothing to index')):
            self.assertFalse(build_app.adopt_game_data(self.tree, None, VERSION, self.folder(), self.logs))
            self.assertFalse(build_app.adopt_game_data(self.tree, self.app, VERSION, self.folder(), self.logs))
            app_with_data(self.app, image=IMAGE[:-1] + b'X')   # the same size, another SHA-256
            self.assertFalse(build_app.adopt_game_data(self.tree, self.app, VERSION, self.folder(), self.logs))
        self.assertFalse(self.folder().exists())
        self.assertIn('not the verified image', self.said[-1])

    def test_its_signed_bundle_is_taken_without_signing_again(self):
        app_with_data(self.app)
        with mock.patch.object(build_app, 'run', side_effect=fake_index):
            build_app.adopt_game_data(self.tree, self.app, VERSION, self.folder(), self.logs)
        with mock.patch.object(build_app, 'run', side_effect=AssertionError('codesign must not run')):
            bundle = build_app.data_bundle(self.tree, VERSION, self.logs, app=self.app)
        self.assertEqual((bundle / 'Contents/_CodeSignature/CodeResources').read_bytes(), b'seal')
        self.assertEqual((bundle / 'Contents/Resources/romfs.bin').read_bytes(), IMAGE)
        self.assertIn('not signed again', self.said[-1])

    def test_a_bundle_changed_since_or_of_another_build_is_signed(self):
        app_with_data(self.app)
        with mock.patch.object(build_app, 'run', side_effect=fake_index):
            build_app.adopt_game_data(self.tree, self.app, VERSION, self.folder(), self.logs)
        image = self.app / f'Contents/Helpers/GameData-{VERSION}.bundle/Contents/Resources/romfs.bin'
        os.utime(image, ns=(1, 1))   # touched since it was checked
        signed = []
        with mock.patch.object(build_app, 'run', side_effect=lambda cmd, *a, **k: signed.append(str(cmd[0]))):
            build_app.data_bundle(self.tree, VERSION, self.logs, app=self.app)
        self.assertEqual(signed, ['codesign', 'codesign'])
        self.assertIn('there is no signed game data bundle', self.said[-1])


class UpdateBuildNeedsNoBaseImage(Base):
    def test_step_data_of_an_update_build_extracts_no_base_romfs(self):
        fonts = self.tree / 'extracted/firmware/shared-fonts'
        ran = []
        with mock.patch.object(build_app, 'fonts_valid', return_value=True), \
             mock.patch.object(build_app, 'validate_fonts'), \
             mock.patch.object(build_app, 'run', side_effect=lambda cmd, *a, **k: ran.append([str(c) for c in cmd])):
            build_app.step_data(self.tree, self.root / 'game.xci', self.root / 'prod.keys', self.logs, update=True)
        self.assertEqual(ran, [])
        self.assertFalse((self.tree / 'extracted/v0').exists())
        self.assertFalse(fonts.exists())
        self.assertIn('not extracted', self.said[0])

    def test_the_merge_is_given_no_base_image(self):
        source = Path(build_app.__file__).read_text()
        body = source[source.index('def step_update_data('):source.index('\ndef build_spirv_cross(')]
        self.assertNotIn('--base-romfs', body)


if __name__ == '__main__':
    unittest.main()
