"""One translation serves a texture slot whatever kind of 2D texture is bound there (Ocarina of Time, 2026-11-09).

The GPU side is games/oot/tests/depth_unified_gpu.m. Depth textures are read through the shader's plain texture2d declaration;
the runtime binds a point-sampling sampler for them, and only a slot the translator proved insensitive answers "*2d".
"""
import importlib.util
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / 'games/oot'


def seed_module():
    sys.path.insert(0, str(GAME / 'scripts'))
    spec = importlib.util.spec_from_file_location('oot_prepare_shader_seed', GAME / 'scripts/prepare_shader_seed.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@unittest.skipUnless((GAME / 'scripts/prepare_shader_seed.py').is_file(), 'a branch without this game')
class DepsMatch(unittest.TestCase):
    def setUp(self):
        self.match = seed_module().deps_match

    def serves(self, want, got):
        return self.match({'tex': {'7': want}}, {'tex': {'7': got}})

    def test_a_slot_the_code_fixes_serves_any_bound_kind(self):
        for got in ('2d', '2d_depth', '2darray', '2darray_depth', 'cube'):
            self.assertTrue(self.serves('*', got), got)

    def test_a_proven_slot_serves_2d_and_2d_arrays_only(self):
        for got in ('2d', '2darray', '2d_depth', '2darray_depth'):
            self.assertTrue(self.serves('*2d', got), got)
        for got in ('cube', '3d', 'buffer'):
            self.assertFalse(self.serves('*2d', got), got)

    def test_an_exact_kind_ignores_only_the_depth_format(self):
        self.assertTrue(self.serves('2d', '2d_depth'))
        self.assertTrue(self.serves('2darray', '2darray_depth'))
        self.assertFalse(self.serves('2d', '2darray'))

    def test_an_unanswered_slot_is_a_2d_texture(self):
        self.assertTrue(self.match({'tex': {'7': '2d'}}, {'tex': {}}))
        self.assertTrue(self.match({'tex': {'7': '*2d'}}, {'tex': {}}))
        self.assertFalse(self.match({'tex': {'7': '2darray'}}, {'tex': {}}))


@unittest.skipUnless((GAME / 'tools/totk_xlate/Service.cs').is_file(), 'a branch without this game')
class Sources(unittest.TestCase):
    def test_the_translator_no_longer_rewrites_depth_textures(self):
        service = (GAME / 'tools/totk_xlate/Service.cs').read_text()
        self.assertNotIn('RewriteDepthTextures', service)
        self.assertNotIn('totk_depth_fetch', service)
        self.assertIn('"*2d"', service)

    def test_the_runtime_answers_without_a_depth_suffix(self):
        source = (GAME / 'third_party/d2r_gfx/gfx/nvn/nvn_mtl.m').read_text(errors='replace')
        answers = re.search(r'static NSDictionary \*rt_answers\(int st\) \{.*?\n\}', source, re.S).group(0)
        self.assertNotIn('_depth', answers)

    def test_depth_formats_read_as_d001(self):
        table = (GAME / 'third_party/d2r_gfx/gfx/tex/nvn_fmt_mtl.c').read_text(errors='replace')
        self.assertIn('NVN_SWZ_ZERO', table)


if __name__ == '__main__':
    unittest.main()
