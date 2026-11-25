"""Ocarina of Time's button map for the Quest's controllers, compiled and run natively (zeldaVR/runtime/oot_vr_pad.h)."""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class QuestPad(unittest.TestCase):
    def test_the_map(self):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'pad')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_DARWIN_C_SOURCE',
                            str(ROOT / 'tests/test_oot_vr_pad.c'), '-o', exe], check=True, capture_output=True, text=True)
            subprocess.run([exe], check=True, capture_output=True, text=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_the_button_numbers_are_the_links(self):
        link = (ROOT / 'zeldaVR/gfx/totk_vr_link.h').read_text()
        pad = (ROOT / 'zeldaVR/runtime/oot_vr_pad.h').read_text()
        for name, value in re.findall(r'VR_PAD_(A|B|X|Y|MENU|LSTICK|RSTICK) = (\d+)', link):
            self.assertIn('OOT_PAD_%s = %s' % (name, value), pad)


if __name__ == '__main__':
    unittest.main()
