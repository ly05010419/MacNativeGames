"""Ocarina of Time VR: the wrist calibration and the six wrist fine-tunes (vr.md row 31).

The math (zeldaVR/runtime/oot_vr_wristcal.h) is compiled and run natively (tests/test_oot_vr_wristcal.c). The wiring is checked
from the source: every reader of a hand's orientation goes through vr_oot_hands (zeldaVR/gfx/oot_vr_wristcal_opts.h), so the arms,
the held items, the wrist panel, the boomerang's mark, the ocarina and the gestures' sword tip agree; only the debug marks at the
controllers read them raw. The VR page has the button and the six sliders, and Menu held + A calibrates in the game.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GFX = ROOT / 'zeldaVR/gfx'
ARMS = ROOT / 'games/totk/third_party/d2r_gfx/gfx/nvn/oot_vr_arms.inc'
NAMES = ['wrist_%s_%s' % (side, axis) for side in 'lr' for axis in ('pitch', 'yaw', 'roll')]


def function_body(text, signature):
    start = text.index(signature)
    open_brace = text.index('{', start)
    depth = 0
    for at in range(open_brace, len(text)):
        depth += {'{': 1, '}': -1}.get(text[at], 0)
        if depth == 0:
            return text[open_brace:at + 1]
    raise AssertionError('unbalanced braces after ' + signature)


class WristCalibrationMath(unittest.TestCase):
    def test_known_turns_and_round_trip(self):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'wristcal')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                            str(ROOT / 'tests/test_oot_vr_wristcal.c'), '-o', exe], check=True, capture_output=True, text=True)
            subprocess.run([exe], check=True, capture_output=True, text=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


class OneHandReader(unittest.TestCase):
    def test_the_oot_pictures_read_the_calibrated_hands(self):
        for path in sorted(GFX.glob('oot_vr_*.h')):
            text = path.read_text(encoding='utf-8')
            if path.name == 'oot_vr_wristcal_opts.h':
                continue
            raw = [m.start() for m in re.finditer(r'vr_link_hands\(', text)]
            if path.name == 'oot_vr_wrist_draw.h':
                marks = function_body(text, 'static void oot_hand_marks_draw(')
                self.assertEqual(len(raw), 1, 'only the debug marks read the controllers raw')
                self.assertIn('vr_link_hands(', marks)
            else:
                self.assertEqual(raw, [], path.name)

    def test_the_arms_and_the_gestures(self):
        self.assertNotIn('vr_link_hands(', ARMS.read_text(encoding='utf-8'))
        self.assertNotIn('vr_oot_hand_tilt', ARMS.read_text(encoding='utf-8'), 'the tilt is in the hands now, not added again')
        vr = (GFX / 'totk_vr.m').read_text(encoding='utf-8')
        self.assertIn('vr_oot_hands(h)', function_body(vr, 'static int oot_hands('))
        self.assertIn('#include "oot_vr_wristcal_opts.h"', vr)
        self.assertRegex(vr, r'if \(o\.calibrate\) vr_oot_wrist_calibrate\("Menu \+ A"\)')

    def test_the_hands_apply_both_the_angles_and_the_tilt(self):
        opts = (GFX / 'oot_vr_wristcal_opts.h').read_text(encoding='utf-8')
        body = function_body(opts, 'void vr_oot_hands(vr_hand out[2])')
        self.assertIn('vr_link_hands(out)', body)
        self.assertIn('vr_oot_hand_tilt()', body)
        self.assertIn('oot_wc_apply(', body)
        calibrate = function_body(opts, 'int vr_oot_wrist_calibrate(const char *how)')
        self.assertIn('vr_link_hands(h)', calibrate, 'the calibration reads the controllers as they are')
        self.assertIn('[vr-wristcal] calibrated', calibrate)
        for name in NAMES:
            self.assertIn('vr_option("%s", 0)' % name, opts)
            self.assertIn('vr_option_set("%s", deg)' % name, opts)


class VrPage(unittest.TestCase):
    def test_the_rows(self):
        page = (ROOT / 'games/oot/runtime/settings_game.h').read_text(encoding='utf-8')
        rows = function_body(page, 'static void settings_vr_rows(')
        self.assertIn('vr_oot_wrist_calibrate("the VR page")', rows)
        self.assertIn('vr_oot_set_wrist_deg(side,axis,v)', rows)
        self.assertIn('r.sliderMin=-180;r.sliderMax=180;', rows)
        for decl in ('int vr_oot_wrist_deg(int side, int axis);', 'void vr_oot_set_wrist_deg(int side, int axis, int deg);',
                     'int vr_oot_wrist_calibrate(const char *how);'):
            self.assertIn(decl, page)


if __name__ == '__main__':
    unittest.main()
