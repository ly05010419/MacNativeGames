"""The comfort ring, the crawl's eye height, the kinds of scene and the item wheel in Ocarina of Time's VR, compiled and run natively
(zeldaVR/runtime/oot_vr_comfort.h, oot_vr_wheel.h)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Comfort(unittest.TestCase):
    def build_and_run(self, source):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'test')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_DARWIN_C_SOURCE',
                            str(ROOT / 'tests' / source), '-o', exe], check=True, capture_output=True, text=True)
            subprocess.run([exe], check=True, capture_output=True, text=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_comfort(self):
        self.build_and_run('test_oot_vr_comfort.c')

    def test_the_item_wheel(self):
        self.build_and_run('test_oot_vr_wheel.c')


if __name__ == '__main__':
    unittest.main()
