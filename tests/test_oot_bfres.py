"""The FRES model reader for the VR body, on a synthetic file, compiled and run natively (zeldaVR/runtime/oot_bfres.h)."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class ModelReader(unittest.TestCase):
    def test_the_reader_under_sanitizers(self):
        tmp = tempfile.mkdtemp()
        try:
            exe = str(Path(tmp) / 'bfres')
            subprocess.run(['xcrun', 'clang', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            str(ROOT / 'tests/test_oot_bfres.c'), '-o', exe], check=True, capture_output=True, text=True)
            run = subprocess.run([exe], capture_output=True, text=True, timeout=240)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('checks passed', run.stdout)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
