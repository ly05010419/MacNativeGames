"""Ocarina of Time's first-person view math, compiled and run natively (zeldaVR/runtime/oot_vr_view.h)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class FirstPersonView(unittest.TestCase):
    def test_the_view_math(self):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'view')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_DARWIN_C_SOURCE',
                            str(ROOT / 'tests/test_oot_vr_view.c'), '-o', exe], check=True, capture_output=True, text=True)
            subprocess.run([exe], check=True, capture_output=True, text=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
