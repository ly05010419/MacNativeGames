"""Persona 5 Royal on the Zelda-line runtime: the pieces this port added that run without the game.

- its identity (runtime/game_version.h) and the pins it is checked against (assets/expected-1.0.2.json);
- the Mac app: the packager checks what it carries and lays it out where the launcher looks, on the shared bundle steps
  and launcher (tools/port_app/bundle.py, shared/app/app_launcher.h);
- the front end names this game, not Super Mario Odyssey;
- the safeguards and fixes taken from Age of Calamity and Tears of the Kingdom are wired in."""
import importlib.util
import json
from pathlib import Path
import re
import tempfile
import unittest

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
NVN = HERE / 'third_party/d2r_gfx/gfx/nvn'


def load(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Identity(unittest.TestCase):
    def test_game_version_and_pins(self):
        version = (HERE / 'runtime/game_version.h').read_text()
        for line in ('#define GAME_P5R 1', '#define GAME_FOLDER "P5R"', '#define GAME_NAME "Persona 5 Royal"',
                     '#define TOTK_GAME_VERSION "1.0.2"', '#define GAME_TITLE_ID 0x01004B10157F2000ULL', '#define GAME_INBOX_ID "p5r"'):
            self.assertIn(line, version)
        expected = json.loads((HERE / 'assets/expected-1.0.2.json').read_text())
        self.assertEqual(expected['version'], '1.0.2')
        self.assertEqual(expected['title_id'], '01004B10157F2000')
        self.assertEqual(sorted(expected['rewritten_sha256']), ['main.patched.bin', 'sdk.patched.bin', 'subsdk0.patched.bin'])
        for digest in expected['rewritten_sha256'].values():
            self.assertRegex(digest, r'^[0-9a-f]{64}$')


class MacApp(unittest.TestCase):
    def setUp(self):
        self.pkg = load(HERE / 'scripts/package_app.py', 'p5r_package_app')

    def test_the_identity_the_launcher_and_the_data_folder_agree(self):
        self.assertEqual(self.pkg.BUNDLE_ID, 'org.macnativegames.p5r')
        self.assertIn('org.macnativegames.p5r', (HERE / 'runtime/game_info.plist').read_text())
        self.assertEqual(self.pkg.EXECUTABLE, 'P5R', 'the data folder name (GAME_FOLDER)')
        launcher = (HERE / 'app/launcher.c').read_text()
        self.assertIn('launcher_data_folder(folder, &b, NAME, "P5R_DATA_DIR", NULL, "P5R")', launcher)
        self.assertTrue((HERE / 'app/app_launcher.h').is_symlink())
        self.assertEqual((HERE / 'app/app_launcher.h').resolve(), (ROOT / 'shared/app/app_launcher.h').resolve())

    def test_the_layout_is_where_the_launcher_looks(self):
        launcher = (HERE / 'app/launcher.c').read_text()
        assemble = (HERE / 'scripts/package_app.py').read_text()
        for path in ('build/p5r-native', 'rewritten/local', 'romfs', 'fonts'):
            self.assertIn(f'"{path}"', launcher)
        for path in ("game / 'build/p5r-native'", "game / 'build/shader-service/totk_xlate'", "game / 'build/spirv-cross/spirv-cross'",
                     "game / 'rewritten/local'", "game / 'romfs' / name", "(game / 'fonts').mkdir"):
            self.assertIn(path, assemble)
        self.assertEqual(self.pkg.ROMFS_FILES, ('index.json', 'runtime-index.bin', 'romfs.bin'))

    def test_an_unprepared_work_folder_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(ValueError, 'no runtime'):
                self.pkg.verify(Path(d))

    def test_the_app_and_work_folder_stay_apart(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(ValueError, 'separate'):
                self.pkg.package(Path(d), Path(d) / 'Persona 5 Royal.app')
            with self.assertRaisesRegex(ValueError, r'\.app'):
                self.pkg.package(Path(d), Path(d).parent / 'Persona 5 Royal')


class FrontEnd(unittest.TestCase):
    def test_it_names_this_game(self):
        front = (HERE / 'runtime/frontend.m').read_text()
        self.assertNotIn('Super Mario Odyssey', front)
        self.assertIn('GAME_NAME " Mac — Ver. %s"', front)
        self.assertIn('mng_active_start(@"p5r", mng_active_version());', front)
        self.assertIn('if(getenv("TOTK_HEADLESS"))', front, 'the graceful headless close')


class Safeguards(unittest.TestCase):
    def test_the_gpu_hang_guard_loop_cap_and_shared_memory(self):
        mtl = (NVN / 'nvn_mtl.m').read_text()
        self.assertIn('static int gpu_hang_s(void)', mtl)
        self.assertIn('#include "msl_loop_cap.h"', mtl)
        self.assertIn('#include "compute_shared.h"', (NVN / 'totk_shader_rt.m').read_text())
        for name in ('msl_loop_cap.h', 'compute_shared.h'):
            self.assertEqual((NVN / name).resolve(), (ROOT / 'shared/third_party/d2r_gfx/gfx/nvn' / name).resolve())

    def test_write_tracking_splits_overlapping_pools(self):
        for name in ('nvn_dirty.c', 'nvn_dirty.h'):
            self.assertEqual((NVN / name).resolve(), (ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn' / name).resolve())
        self.assertRegex((NVN / 'nvn_dirty.c').read_text(), r'#if defined\(GAME_BOTW\) \|\| defined\(GAME_AOCX\) \|\| defined\(GAME_P5R\)\n#define NVN_DIRTY_SPLIT 1')


if __name__ == '__main__':
    unittest.main()
