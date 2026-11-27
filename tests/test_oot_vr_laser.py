"""The VR laser, the big screen's drag (zeldaVR/runtime/oot_vr_laser.h) and the VR menu's layout, open button and mask
(zeldaVR/runtime/oot_vr_menu.h), compiled and run natively; and the menu's options fit, its rows are the VR page's own."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run_c(name):
    tmp = tempfile.mkdtemp()
    try:
        exe = str(Path(tmp) / name)
        subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_DARWIN_C_SOURCE',
                        str(ROOT / ('tests/test_oot_vr_%s.c' % name)), '-o', exe], check=True, capture_output=True, text=True)
        subprocess.run([exe], check=True, capture_output=True, text=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


class Laser(unittest.TestCase):
    def test_the_laser_and_the_screen_drag(self):
        run_c('laser')

    def test_the_menu(self):
        run_c('menu')

    def test_the_menu_buttons_are_the_links(self):
        link = (ROOT / 'zeldaVR/gfx/totk_vr_link.h').read_text()
        menu = (ROOT / 'zeldaVR/runtime/oot_vr_menu.h').read_text()
        for name, value in re.findall(r'VR_PAD_(A|B|X|Y|MENU|LSTICK|RSTICK) = (\d+)', link):
            self.assertIn('OOT_MENU_PAD_%s = %s' % (name, value), menu)

    def test_one_list_of_rows(self):
        """The headset's menu draws the VR page's rows (settings_vr_rows), not a list of its own."""
        rows = (ROOT / 'games/oot/runtime/settings_vr_menu.h').read_text()
        self.assertIn('settings_vr_rows(rows)', rows)
        draw = (ROOT / 'zeldaVR/gfx/oot_vr_menu_draw.h').read_text()
        self.assertNotIn('settings_text', draw)
        self.assertIn('runtime_vr_menu_rows', draw)
        game = (ROOT / 'games/oot/runtime/settings_game.h').read_text()
        self.assertIn('settings_vr_menu_rows(rows)', game)

    def test_off_by_default(self):
        """Nothing new happens until the player opens the menu: the screen drag is off, the menu fixed, the laser straight."""
        draw = (ROOT / 'zeldaVR/gfx/oot_vr_menu_draw.h').read_text()
        for name, default in (('screen_drag', 0), ('menu_follow', 0), ('laser_deg', 0), ('screen_yaw', 0), ('screen_up', 0)):
            self.assertRegex(draw, r'vr_option\("%s", %d\)' % (name, default))
        self.assertRegex(draw, r'vr_option\("menu_button", OOT_MENU_BUTTON_L3\)')


if __name__ == '__main__':
    unittest.main()
