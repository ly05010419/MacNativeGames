"""Radial stick response, including per-axis multi-controller merge limits."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

TOTK = Path(__file__).resolve().parents[1] / 'games/totk'


@unittest.skipUnless(shutil.which('xcrun'), 'needs Xcode command line tools')
class StickResponseTests(unittest.TestCase):
    def test_response(self):
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'probe'
            subprocess.run(['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', f'-I{TOTK}/runtime',
                            str(TOTK / 'tests/stick_response/probe.c'), '-o', str(probe)], check=True)
            result = subprocess.run([str(probe)], check=True, capture_output=True, text=True)
            self.assertIn('radial stick response: pass', result.stdout)


if __name__ == '__main__':
    unittest.main()
