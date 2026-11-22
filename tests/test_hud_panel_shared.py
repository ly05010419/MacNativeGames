"""The FPS / shader / load boxes and the corner panel are one copy for every game (user 2026-11-20: "fps pannel 也要统一，
测试保证"): shared/third_party/d2r_gfx/gfx/nvn/hud_panel.inc, Tears of the Kingdom's, included by each game's own
gfx/nvn/nvn_mtl.m, which keeps only the few numbers that differ (hud_fps_sizes, hud_pso_counts, hud_pso_total,
hud_rt_missed, hud_more_lines). A game whose graphics layer still has its own panel is listed in PENDING; the list may
only shrink, and a game that is in neither list fails here, so a new port starts on the shared panel."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PANEL = ROOT / 'shared/third_party/d2r_gfx/gfx/nvn/hud_panel.inc'
# The panel's own functions: defined in hud_panel.inc only
OWN = ('fps_overlay_prepare', 'shader_overlay_prepare', 'load_overlay_prepare', 'mtl_hud_safe_area', 'hud_line_publish',
       'hud_inset_right', 'dm_gfx_fps_now')
HOOKS = ('hud_fps_sizes', 'hud_pso_counts', 'hud_pso_total', 'hud_more_lines')
UNIFIED = {'totk', 'acnh', 'odyssey', 'eow', 'mk8dx', 'rdr', 'aocx'}   # oot shares totk's graphics folder (a link)
# Graphics layers that still carry their own (older) panel: to be moved onto hud_panel.inc. Only ever remove names.
PENDING = {'alc', 'by3', 'dread', 'kirby', 'lm3', 'mkw', 'mvdk', 'ori', 'ot0', 'pla', 'pokesv', 'ssbu', 'tmnt',
           'tomodachi', 'unicorn', 'wonder', 'xc3', 'xcx'}
PENDING_AT_MOST = 18


def graphics_layers():
    """{game: nvn_mtl.m} for every game with a graphics layer of its own (not reached through a linked folder)."""
    out = {}
    for f in sorted((ROOT / 'games').glob('*/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m')):
        game = f.relative_to(ROOT / 'games').parts[0]
        if (ROOT / 'games' / game / 'third_party/d2r_gfx').is_symlink():
            continue
        out[game] = f
    return out


def defines(text, name):
    """A definition (with its body), not a prototype."""
    return re.search(rf'^(?:static\s+)?[A-Za-z_][\w \*]*\b{name}\([^;{{]*\)\s*\{{', text, re.M) is not None


class SharedPanel(unittest.TestCase):
    def test_the_panel_lives_in_shared(self):
        text = PANEL.read_text()
        for name in OWN:
            self.assertTrue(defines(text, name), name)

    def test_every_game_is_on_it_or_pending(self):
        layers = graphics_layers()
        self.assertLessEqual(len(PENDING), PENDING_AT_MOST, 'PENDING only shrinks')
        self.assertEqual(set(layers) - UNIFIED - PENDING, set(), 'a new game copies the panel: include hud_panel.inc')
        self.assertEqual((UNIFIED | PENDING) - set(layers), set(), 'a listed game has no graphics layer of its own any more')

    def test_unified_games_include_it_and_keep_only_the_hooks(self):
        for game in sorted(UNIFIED):
            with self.subTest(game=game):
                folder = ROOT / 'games' / game / 'third_party/d2r_gfx/gfx/nvn'
                text = (folder / 'nvn_mtl.m').read_text()
                self.assertIn('#include "hud_panel.inc"', text)
                for name in OWN:
                    self.assertFalse(defines(text, name), f'{name}: its own copy of the shared panel')
                for name in HOOKS:
                    self.assertTrue(defines(text, name), f'{name}: the hook the shared panel needs')
                link = folder / 'hud_panel.inc'
                self.assertTrue(link.is_symlink() and link.resolve() == PANEL.resolve(), 'linked to shared/')

    def test_pending_games_still_need_it(self):
        layers = graphics_layers()
        for game in sorted(PENDING):
            with self.subTest(game=game):
                self.assertNotIn('#include "hud_panel.inc"', layers[game].read_text(), 'now unified: move it to UNIFIED')

    def test_phone_front_ends_report_the_safe_area(self):
        for game in ('totk', 'acnh'):
            front = (ROOT / 'games' / game / 'runtime/frontend_ios.m').read_text()
            self.assertIn('mtl_hud_safe_area(', front, game)


if __name__ == '__main__':
    unittest.main()
