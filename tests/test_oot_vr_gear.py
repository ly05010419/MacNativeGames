"""Ocarina of Time in VR: the sword's tilt, the shield's poses, the untargeted guard (zeldaVR/runtime/oot_vr_gear.h), compiled and run natively."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Gear(unittest.TestCase):
    def test_the_gear(self):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'gear')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-D_DARWIN_C_SOURCE',
                            str(ROOT / 'tests/test_oot_vr_gear.c'), '-o', exe], check=True, capture_output=True, text=True)
            subprocess.run([exe], check=True, capture_output=True, text=True)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
