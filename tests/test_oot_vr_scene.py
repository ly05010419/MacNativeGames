"""Ocarina of Time in VR: the game's scenes (zeldaVR/runtime/oot_vr_scene.h), the ocarina in the hands (oot_vr_ocarina.h) and the
wrist panel's dungeon row (oot_vr_dungeon.h), compiled and run natively; and the game side's hook and settings wiring."""
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
        return subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


class Scenes(unittest.TestCase):
    def test_scene_rules(self):
        self.assertIn('scenes:', run_c('scene'))

    def test_ocarina(self):
        self.assertIn('the ocarina:', run_c('ocarina'))

    def test_dungeon_row(self):
        self.assertIn('52 hashes', run_c('dungeon'))

    def test_the_ocarina_hook_is_the_verified_word(self):
        """The note wait's update, main+0x2889280, is hooked at its pacibsp (read on update 131072's image)."""
        vr = (ROOT / 'games/oot/runtime/oot_vr.h').read_text()
        self.assertRegex(vr, r'\{ 0x2889280, 0xd503237fu, 0, 0, oot_ocarina_tick,')

    def test_the_scene_rows_are_on_the_vr_page(self):
        game = (ROOT / 'games/oot/runtime/settings_game.h').read_text()
        rows = (ROOT / 'games/oot/runtime/settings_vr_scene.h').read_text()
        self.assertIn('settings_vr_scene_rows(rows);', game)
        self.assertIn('#include "settings_vr_scene.h"', game)
        draw = (ROOT / 'zeldaVR/gfx/oot_vr_scene_draw.h').read_text()
        for name in re.findall(r'vr_oot_set_(\w+)\(v\)', rows):
            self.assertIn('void vr_oot_set_%s(int' % name, draw)


if __name__ == '__main__':
    unittest.main()
