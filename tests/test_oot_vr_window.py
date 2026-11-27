"""The window view of Ocarina of Time (vr.md row 3), compiled and run natively (zeldaVR/runtime/oot_vr_window.h)."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class WindowView(unittest.TestCase):
    def test_the_window(self):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'window')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_DARWIN_C_SOURCE',
                            str(ROOT / 'tests/test_oot_vr_window.c'), '-o', exe], check=True, capture_output=True, text=True)
            subprocess.run([exe], check=True, capture_output=True, text=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_the_rows_are_on_the_vr_page(self):
        game = (ROOT / 'games/oot/runtime/settings_game.h').read_text()
        rows = (ROOT / 'games/oot/runtime/settings_vr_window.h').read_text()
        draw = (ROOT / 'zeldaVR/gfx/oot_vr_window_draw.h').read_text() + (ROOT / 'zeldaVR/gfx/oot_vr_passthrough_draw.h').read_text()
        self.assertIn('settings_vr_window_rows(rows);', game)
        self.assertIn('#include "settings_vr_window.h"', game)
        for name in re.findall(r'vr_oot_set_(\w+)\(v\)', rows):
            self.assertIn('void vr_oot_set_%s(int' % name, draw)
        self.assertIn('void vr_oot_window_place(void)', draw)

    def test_the_curved_screen_stays_the_default(self):
        """Nothing changes for players unless they choose the window (the brief's rule 4)."""
        draw = (ROOT / 'zeldaVR/gfx/oot_vr_window_draw.h').read_text()
        self.assertIn('vr_option("screen_shape", OOT_WIN_CURVED)', draw)
        vr = (ROOT / 'zeldaVR/gfx/totk_vr.m').read_text()
        self.assertIn('#include "oot_vr_window_draw.h"', vr)
        # a picture with no head pose: the window, else the curved screen (the window's parallax passes its pictures' sequences too)
        self.assertIn('!oot_window_posed(e, u.m, pics[eye], x->pose.p, g_oot_hud_tex != nil, useds[eye], peye[eye], half) && (useds[eye] || !oot_arc_draw(e, u.m, pics[eye]))', vr)
        posed = (ROOT / 'zeldaVR/gfx/oot_vr_winpar_draw.h').read_text()
        self.assertIn('if (!used) return oot_window_draw(e, m, pic, eye, world);', posed)
        self.assertIn('oot_window_tick(&head, place_now);', vr)


if __name__ == '__main__':
    unittest.main()
