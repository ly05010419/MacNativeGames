"""The release's prebuilt programs (tools/prebuilt.py) in the builds that compiled their own copies until 2026-11-22
(user: "做 1. 智慧的再现、奥德赛改用编好的 SPIRV-Cross，动物森友会改用编好的 hactool 2. 时之笛的运行时改成发布时编好"):
Echoes of Wisdom and Super Mario Odyssey take spirv-cross, Animal Crossing hactool, Ocarina of Time its runtime and pool
sources reader. Each is the same commit, sources, hooks and flags as the build's own rule, so nothing it makes changes."""
import os
from pathlib import Path
import re
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tests'))
from tools import prebuilt  # noqa: E402
from test_prebuilt import source_with  # noqa: E402

import build_app  # noqa: E402


def made_dir(path):
    path.mkdir(parents=True)
    return path


class KeepPlaced(unittest.TestCase):
    def test_placed_once_then_left_with_its_time(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = source_with(made_dir(Path(tmp) / 'src'), {'spirv-cross': b'made'})
            dest = Path(tmp) / 'work/build/spirv-cross/spirv-cross'
            self.assertTrue(prebuilt.keep_placed(src, 'spirv-cross', dest))
            self.assertEqual(dest.read_bytes(), b'made')
            old = time.time() - 3600
            os.utime(dest, (old, old))
            self.assertTrue(prebuilt.keep_placed(src, 'spirv-cross', dest))
            self.assertEqual(dest.stat().st_mtime, old)   # what make builds from it is not made again
            dest.write_bytes(b'compiled here before')
            self.assertTrue(prebuilt.keep_placed(src, 'spirv-cross', dest))
            self.assertEqual(dest.read_bytes(), b'made')
            self.assertFalse(prebuilt.keep_placed(Path(tmp) / 'nothing', 'spirv-cross', dest))


class Recipes(unittest.TestCase):
    """The prebuilt programs stand in for these builds' own only because they are made the same way."""
    def test_spirv_cross_is_the_same_in_every_rule(self):
        commit, sources, flags = build_app.SPIRV_CROSS[1], ' '.join(build_app.SPIRV_CROSS_SOURCES), ' '.join(build_app.SPIRV_CROSS_FLAGS)
        for game in ('eow', 'odyssey'):
            make = (ROOT / f'games/{game}/Makefile').read_text()
            self.assertIn(f'SPIRV_CROSS_COMMIT := {commit}', make, game)
            listed = re.search(r'SPIRV_CROSS_SOURCES := (.*?)\n(?!  )', make, re.S)[1].replace('\\\n', ' ')
            self.assertEqual(listed.split(), sources.split(), game)
            self.assertIn(f'xcrun clang++ {flags}', make, game)

    def test_hactool_is_the_same_recipe(self):
        app = build_app.__file__
        text = Path(app).read_text()
        ours = text[text.index('def build_hactool('):text.index('\ndef remove_large_output_overlays(')]
        acnh = (ROOT / 'games/acnh/tools/build_hactool.py').read_text()
        theirs = acnh[acnh.index('def build_hactool('):acnh.index("\nif __name__ == '__main__':")]
        body = lambda s: s[s.index("    git_fetch("):].strip()
        self.assertEqual(body(ours), body(theirs))
        self.assertIn(repr(build_app.HACTOOL[1])[1:-1], acnh)


class Builds(unittest.TestCase):
    def test_animal_crossing_takes_the_prebuilt_hactool(self):
        sys.path.insert(0, str(ROOT / 'games/acnh'))
        import importlib.util
        spec = importlib.util.spec_from_file_location('acnh_build', ROOT / 'games/acnh/build.py')
        acnh = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(acnh)
        with tempfile.TemporaryDirectory() as tmp:
            src = source_with(made_dir(Path(tmp) / 'src'), {'hactool': b'hactool'})
            with mock.patch.object(acnh, 'REPO', src), mock.patch.object(acnh, 'run', side_effect=AssertionError('built')):
                tool = acnh.hactool_for(Path(tmp) / 'work', 'python3')
            self.assertEqual(tool.read_bytes(), b'hactool')

    def test_ocarina_takes_its_prebuilt_runtime(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('oot_build', ROOT / 'games/oot/build.py')
        oot = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(oot)
        with tempfile.TemporaryDirectory() as tmp:
            src = source_with(made_dir(Path(tmp) / 'src'), {'oot-native': b'runtime', 'oot-pool-sources-list': b'reader'})
            game = src / 'games/oot'
            game.mkdir(parents=True)
            work = Path(tmp) / 'work'
            with mock.patch.object(oot, 'GAME', game), mock.patch.object(oot.subprocess, 'run', side_effect=AssertionError('make')):
                oot.build_runtime(work, Path(tmp) / 'log', 2, {})
            self.assertEqual((work / 'build/oot-native').read_bytes(), b'runtime')
            self.assertEqual((work / 'build/pool-sources-list').read_bytes(), b'reader')
            self.assertTrue((work / 'build' / oot.RUNTIME_STAMP).is_file())

    def test_echoes_and_odyssey_place_spirv_cross_before_make(self):
        eow = (ROOT / 'games/eow/build.py').read_text()
        self.assertIn("prebuilt.keep_placed(REPO, 'spirv-cross', work / 'build/spirv-cross/spirv-cross')", eow)
        self.assertLess(eow.index('keep_placed'), eow.index("'prepare', 'runtime', 'shader-tools'"))
        ody = (ROOT / 'games/odyssey/scripts/build_odyssey.py').read_text()
        self.assertIn('"spirv-cross", work / "build/spirv-cross/spirv-cross"', ody)
        self.assertIn('PREBUILT_SPIRV_CROSS=1', ody)
        make = (ROOT / 'games/odyssey/Makefile').read_text()
        branch = make[make.index('ifeq ($(PREBUILT_SPIRV_CROSS),1)'):make.index('else', make.index('ifeq ($(PREBUILT_SPIRV_CROSS),1)'))]
        self.assertNotIn('clang', branch)

    def test_the_release_makes_them(self):
        self.assertIn('spirv-cross', prebuilt.NAMES)
        self.assertIn('hactool', prebuilt.NAMES)
        self.assertEqual(set(prebuilt.OOT_PROGRAMS), {'oot-native', 'oot-pool-sources-list'})
        self.assertTrue(set(prebuilt.OOT_PROGRAMS) <= set(prebuilt.NAMES))


if __name__ == '__main__':
    unittest.main()
