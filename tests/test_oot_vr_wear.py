"""What Link wears and the game's own grip in first person, compiled and run natively (zeldaVR/runtime/oot_vr_wear.h)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class Wear(unittest.TestCase):
    def test_the_rules(self):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'wear')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', str(ROOT / 'tests/test_oot_vr_wear.c'), '-o', exe], check=True, capture_output=True, text=True)
            run = subprocess.run([exe], capture_output=True, text=True, timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('checks passed', run.stdout)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
