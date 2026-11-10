"""Ocarina of Time packaging: the Mac's Metal libraries are those of the shaders the shader list uses (user 2026-11-09:
"只要 我一直用的 我收集的"), picked by games/oot/build.py itself, and the App carries its shaders by default."""
import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'


def load_build():
    spec = importlib.util.spec_from_file_location('oot_build_under_test', GAME / 'build.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def variant(n):
    return f'{n:064x}-{n:012x}_b'


def render_key(vs, fs):
    """A 544-byte pipeline key in hex, as the resolved lists write it: two NUL-padded 80-byte names first."""
    name = lambda v: v.encode().ljust(80, b'\0')
    return (name(vs) + name(fs) + bytes(544 - 160)).hex()


class ListedShaderKeys(unittest.TestCase):
    def setUp(self):
        self.build = load_build()
        self.tmp = tempfile.TemporaryDirectory()
        self.seed = Path(self.tmp.name) / 'seed'
        self.seed.mkdir()
        self.shared = Path(self.tmp.name) / 'shader_list.tsv'

    def tearDown(self):
        self.tmp.cleanup()

    def test_every_shader_the_lists_use(self):
        (self.seed / 'pso_manifest.tsv').write_text(
            '# generated\n'
            f'R\t{render_key(variant(1), variant(2))}\tm\n'   # a render pipeline: both stages, from the hex key
            f'S\t{variant(3)}\t0\tm\n'
            f'M\t{variant(4)}\t{render_key(variant(5), variant(6))}\tm\n')
        (self.seed / 'pso_title.tsv').write_text(f'T\t{render_key(variant(7), variant(8))}\t0:0;\tt\n')
        self.shared.write_text(f'# list\nV\t{9:064x}\t{{}}\nR\t{render_key(variant(99), variant(98))}\tm\n')
        compute = [f'C\t{variant(10)}']
        keys = self.build.listed_shader_keys(self.seed, self.shared, compute)
        self.assertEqual(keys, {f'{n:064x}' for n in (1, 2, 3, 4, 5, 6, 7, 8, 9, 10)})

    def test_lists_missing(self):
        self.assertEqual(self.build.listed_shader_keys(self.seed, self.shared, []), set())


class BundledByDefault(unittest.TestCase):
    def test_default_bundles(self):
        build = load_build()
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('OOT_BUNDLE_SHADERS', None)
            self.assertTrue(build.bundle_shaders())
            os.environ['OOT_BUNDLE_SHADERS'] = '0'
            self.assertFalse(build.bundle_shaders())
            os.environ['OOT_BUNDLE_SHADERS'] = '1'
            self.assertTrue(build.bundle_shaders())


class ShaderSubset(unittest.TestCase):
    """User 2026-11-10: "用户要编译 预热全部着色器。测试只预热我的": a player's build compiles every shader."""
    def test_choice(self):
        build = load_build()
        self.assertIsNone(build.shader_subset(Path('t'), Path('l'), {}))
        self.assertIsNone(build.shader_subset(Path('t'), Path('l'), {'OOT_SHADER_SUBSET': 'all'}))
        self.assertEqual(build.shader_subset(Path('t'), Path('l'), {'OOT_SHADER_SUBSET': '/keys.txt'}), '/keys.txt')
        with mock.patch.object(build, 'listed_shader_file', return_value='/listed.txt') as listed:
            self.assertEqual(build.shader_subset(Path('t'), Path('l'), {'OOT_SHADER_SUBSET': 'listed'}), '/listed.txt')
            listed.assert_called_once()


class TestRunsSkipTheOpening(unittest.TestCase):
    """User 2026-11-10: the opening is skipped in test runs only (GameSlot.env); a player's start shows it."""
    def test_slot_env(self):
        spec = importlib.util.spec_from_file_location('oot_game_slot', GAME / 'scripts/dev/game_slot.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        slot = module.GameSlot.__new__(module.GameSlot)
        for name, value in dict(fd=3, dir=Path('/tmp'), owner='test', purpose='p', token='t', since=0, expect=1).items():
            setattr(slot, name, value)
        self.assertEqual(slot.env({})['OOT_AUTO_SKIP_OPENING'], '1')
        self.assertEqual(slot.env({'OOT_AUTO_SKIP_OPENING': '0'})['OOT_AUTO_SKIP_OPENING'], '0')


if __name__ == '__main__':
    unittest.main()
