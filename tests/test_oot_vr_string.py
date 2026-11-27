"""Ocarina of Time's bow and slingshot in the player's hands, compiled and run natively: the string, its straps, the nocked
arrow and its effects' camera (zeldaVR/runtime/oot_vr_string.h), and the grip that takes and holds the bow
(zeldaVR/runtime/oot_vr_bowgrip.h)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run_c(test):
    tmp = tempfile.mkdtemp()
    try:
        exe = str(Path(tmp) / 'test')
        subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_DARWIN_C_SOURCE',
                        str(ROOT / 'tests' / test), '-o', exe], check=True, capture_output=True, text=True)
        return subprocess.run([exe], check=True, capture_output=True, text=True).stdout
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


class BowString(unittest.TestCase):
    def test_the_string(self):
        self.assertIn('all passed', run_c('test_oot_vr_string.c'))

    def test_the_grip(self):
        self.assertIn('all passed', run_c('test_oot_vr_bowgrip.c'))

    def test_the_rig_script_takes_the_arrows(self):
        """The nocked arrow is known by its own model's vertex counts, made at packaging time from the player's files."""
        script = (ROOT / 'games/oot/scripts/make_vr_rig.py').read_text()
        for name in ('Item_Arrow_Normal', 'Item_Arrow_Fire', 'Item_Arrow_Ice', 'Item_Arrow_Light'):
            self.assertIn("'%s'" % name, script)


if __name__ == '__main__':
    unittest.main()
