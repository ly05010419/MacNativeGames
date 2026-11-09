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


if __name__ == '__main__':
    unittest.main()
