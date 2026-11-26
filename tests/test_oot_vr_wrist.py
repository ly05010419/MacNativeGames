"""The wrist panel of Ocarina of Time's VR and the saved flags it shows, compiled and run natively (zeldaVR/runtime/oot_vr_wrist.h, oot_gamedata.h)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Wrist(unittest.TestCase):
    def build_and_run(self, source):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'test')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_DARWIN_C_SOURCE',
                            str(ROOT / 'tests' / source), '-o', exe], check=True, capture_output=True, text=True)
            subprocess.run([exe], check=True, capture_output=True, text=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_the_saved_flags(self):
        self.build_and_run('test_oot_gamedata.c')

    def test_the_panel(self):
        self.build_and_run('test_oot_vr_wrist.c')


if __name__ == '__main__':
    unittest.main()
