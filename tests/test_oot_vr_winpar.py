"""The window view's parallax in Ocarina of Time (vr.md row 3), compiled and run natively (zeldaVR/runtime/oot_vr_winpar.h)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class WindowParallax(unittest.TestCase):
    def test_the_math(self):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'winpar')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_DARWIN_C_SOURCE',
                            str(ROOT / 'tests/test_oot_vr_winpar.c'), '-o', exe], check=True, capture_output=True, text=True)
            subprocess.run([exe], check=True, capture_output=True, text=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_off_by_default(self):
        """The window as committed stays unless the player chooses the parallax (the brief's rule 4)."""
        draw = (ROOT / 'zeldaVR/gfx/oot_vr_window_draw.h').read_text()
        self.assertIn('vr_option("win_parallax", 0) != 0', draw)
        rows = (ROOT / 'games/oot/runtime/settings_vr_window.h').read_text()
        self.assertIn('vr_oot_set_win_parallax(v)', rows)

    def test_wired(self):
        vr = (ROOT / 'zeldaVR/gfx/totk_vr.m').read_text()
        self.assertIn('#include "oot_vr_winpar_draw.h"', vr)
        self.assertIn('if (oot_win_par_head(s, head)) return;', vr)
        self.assertIn('if (g_oot_win_par_on) { d[0] = d[1] = d[2] = 0; return; }', vr)
        game = (ROOT / 'games/oot/runtime/oot_vr.h').read_text()
        self.assertIn('#include "oot_vr_winpar_cam.h"', game)
        self.assertIn('if (oot_win_par_camera(view, &control)) return;', game)
        # the parallax must not bump the hook's call count: the controllers would think the world camera runs on the big screen
        cam = (ROOT / 'games/oot/runtime/oot_vr_winpar_cam.h').read_text()
        self.assertNotIn('oot_vr_cam.calls', cam.split('#pragma once', 1)[1])
        # the hook's branch sits after the control is found on and before anything of the head view
        body = game.split('static void oot_vr_camera(', 1)[1]
        self.assertLess(body.index('if (!control.on)'), body.index('oot_win_par_camera'))
        self.assertLess(body.index('oot_win_par_camera'), body.index('if (control.level) vr_view_level(m);'))


if __name__ == '__main__':
    unittest.main()
